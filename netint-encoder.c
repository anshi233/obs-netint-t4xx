/**
 * @file netint-encoder.c
 * @brief Core encoder implementation for NETINT T4XX hardware encoder
 * 
 * This file implements the OBS encoder interface for NETINT T408 hardware encoders.
 * It provides hardware-accelerated H.264 and H.265 encoding using NETINT's PCIe cards.
 * 
 * Architecture Overview:
 * - Asynchronous encoding: Uses background thread to receive encoded packetsgi
 * - Thread-safe queue: Encoded packets are queued and consumed by main thread
 * - Dual codec support: Separate encoder registrations for H.264 and H.265
 * - Dynamic library: Uses function pointers resolved at runtime (no compile-time deps)
 * 
 * Encoding Flow:
 * 1. OBS calls netint_encode() with video frame
 * 2. Frame is copied into encoder's buffer via ni_logan_encode_copy_frame_data()
 * 3. Frame is sent to hardware via ni_logan_encode_send()
 * 4. Background thread continuously calls ni_logan_encode_receive()
 * 5. Received packets are queued with metadata (PTS, DTS, keyframe flag)
 * 6. Main thread pops packets from queue and returns to OBS
 * 
 * Threading Model:
 * - Main thread: Calls encode() from OBS, sends frames, receives packets from queue
 * - Background thread: Continuously receives encoded packets, queues them
 * - Queue mutex: Protects packet queue from concurrent access
 * 
 * Key Features:
 * - Variable frame rate (VFR) support via reconfig_vfr()
 * - SPS/PPS header extraction and storage for stream initialization
 * - Automatic device discovery if no device specified
 * - CBR/VBR rate control mode selection
 * - Profile selection (baseline/main/high for H.264, main/main10 for H.265)
 * - Keyframe interval control
 * 
 * Error Handling:
 * - All libxcoder calls check return values
 * - Failed operations return false to OBS
 * - Error messages logged with [obs-netint-t4xx] prefix
 * - Cleanup on failure in netint_create() via goto fail pattern
 */

#include "netint-encoder.h"
#include "netint-libxcoder.h"
#include "netint-debug.h"

#include <obs-avc.h>
#include <obs-hevc.h>
#include <util/dstr.h>
#include <util/darray.h>
#include <util/platform.h>
#include <util/threading.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include "netint-libxcoder-shim.h"

/**
 * @brief Encoded packet structure for queue management
 * 
 * This structure represents a single encoded video packet (NAL unit) that has been
 * received from the hardware encoder and is waiting to be returned to OBS.
 * 
 * The queue allows asynchronous packet reception: the background thread receives packets
 * and queues them, while the main thread pops them when OBS requests them.
 */
struct netint_pkt {
    uint8_t *data;        /**< Encoded packet data (allocated, must be freed) */
    size_t size;          /**< Size of encoded packet in bytes */
    int64_t pts;          /**< Presentation timestamp (when frame should be displayed) */
    int64_t dts;          /**< Decode timestamp (when frame should be decoded) */
    bool keyframe;        /**< true if this is a keyframe (I-frame), false for P/B frames */
    int priority;         /**< Packet priority (higher = more important for streaming) */
    struct netint_pkt *next; /**< Next packet in queue (linked list) */
};

struct netint_frame_job {
    uint8_t *buffer;                  /**< Host copy of frame data laid out in HW format */
    size_t buffer_size;               /**< Size of the buffer in bytes */
    size_t buffer_capacity;           /**< Total capacity of allocated buffer */
    int64_t pts;                      /**< Presentation/Decode timestamp */
    bool start_of_stream;             /**< Marks first frame to hardware */
    bool end_of_stream;               /**< Signals EOS to hardware */
    bool from_pool;                   /**< Indicates job originated from reusable pool */
    struct netint_frame_job *next;    /**< Next job in queue */
};

/**
 * @brief Maximum packet queue size to prevent unbounded growth
 * 
 * This limit prevents the queue from growing indefinitely if OBS doesn't consume
 * packets fast enough. If the queue reaches this size, we log a warning.
 * In extreme cases, we could drop oldest packets, but for now we just warn.
 */
#define MAX_PKT_QUEUE_SIZE 10

/**
 * @brief Maximum consecutive errors before encoder is considered failed
 * 
 * If the encoder encounters this many consecutive errors, it will be marked
 * as failed and will return errors to OBS. OBS will then handle recreation.
 */
#define MAX_CONSECUTIVE_ERRORS 5

/**
 * @brief Maximum time (in seconds) without receiving a packet before considering encoder hung
 * 
 * If no packets are received for this duration (and we're not flushing), the encoder
 * may be hung. We log a warning and attempt recovery.
 */
#define ENCODER_HANG_TIMEOUT_SEC 10

/**
 * @brief Maximum number of recovery attempts before giving up
 * 
 * After this many recovery attempts, we stop trying to recover and mark encoder as failed.
 */
#define MAX_RECOVERY_ATTEMPTS 3

/**
 * @brief Minimum number of reusable frame jobs to keep in the pool
 */
#define NETINT_JOB_POOL_MIN_CAPACITY 6

/**
 * @brief Encoder context structure - stores all state for a single encoder instance
 * 
 * This structure contains all the state needed for one encoder instance:
 * - Hardware encoder context (libxcoder structures)
 * - Threading state (background receive thread)
 * - Packet queue (thread-safe queue for encoded packets)
 * - Configuration (codec, bitrate, profile, etc.)
 * - Stream metadata (SPS/PPS headers for H.264/H.265)
 * 
 * Each encoder instance (H.264 or H.265) has its own context.
 * Multiple contexts can exist simultaneously (e.g., one for streaming, one for recording).
 */
/**
 * @brief Encoder state enumeration for health monitoring
 */
typedef enum {
    NETINT_ENCODER_STATE_NORMAL,      /**< Encoder is operating normally */
    NETINT_ENCODER_STATE_ERROR,       /**< Encoder encountered errors but still trying */
    NETINT_ENCODER_STATE_HUNG,        /**< Encoder appears to be hung (no packets) */
    NETINT_ENCODER_STATE_FAILED,      /**< Encoder has failed and should be recreated */
    NETINT_ENCODER_STATE_RECOVERING   /**< Encoder is attempting recovery */
} netint_encoder_state_t;

struct netint_ctx {
    obs_encoder_t *encoder;           /**< OBS encoder handle (for accessing video info, etc.) */
    ni_logan_enc_context_t enc;       /**< NETINT libxcoder encoder context (hardware state) - EMBEDDED like FFmpeg does */
    uint8_t *extra;                   /**< SPS/PPS header data (extradata) for stream initialization */
    size_t extra_size;                /**< Size of extradata in bytes */
    bool got_headers;                 /**< true if headers were obtained (either during init or from first packet) */
    bool flushing;                    /**< true when encoder is being flushed (no more input frames) */
    
    /* Background worker that manages both send and receive with pipelining */
    pthread_t io_thread;              /**< Background thread that handles encode_send/receive */
    pthread_mutex_t queue_mutex;      /**< Protects packet queue access ONLY */
    pthread_mutex_t io_mutex;         /**< Serializes access to libxcoder context */
    pthread_mutex_t frame_queue_mutex; /**< Protects pending frame job queue */
    pthread_cond_t frame_queue_cond;  /**< Signals availability of frame jobs */
    bool queue_mutex_initialized;
    bool io_mutex_initialized;
    bool frame_queue_mutex_initialized;
    bool frame_queue_cond_initialized;
    volatile bool stop_thread;        /**< Signal to background thread to stop */
    bool thread_created;              /**< true if io_thread was successfully created */
    struct netint_pkt *pkt_queue_head; /**< Head of packet queue (oldest packet) */
    struct netint_pkt *pkt_queue_tail; /**< Tail of packet queue (newest packet) */

    struct netint_frame_job *frame_queue_head; /**< Pending frame jobs (host buffers ready for send) */
    struct netint_frame_job *frame_queue_tail;
    int pending_jobs;                 /**< Number of queued frame jobs */
    int inflight_frames;              /**< Count of frames submitted to HW but not yet drained */
    int max_inflight;                 /**< Maximum frames to keep in-flight before draining */
    uint64_t frames_submitted;        /**< Total frames enqueued (for start-of-stream decisions) */

    /* Reusable frame job pool */
    pthread_mutex_t job_pool_mutex;   /**< Protects reusable frame job pool */
    bool job_pool_mutex_initialized;
    struct netint_frame_job *job_pool_head; /**< Singly-linked list of available jobs */
    int job_pool_size;                /**< Current number of jobs in pool */
    int job_pool_capacity;            /**< Maximum number of jobs preallocated */

    /* Precomputed hardware frame layout */
    int hw_stride[NI_LOGAN_MAX_NUM_DATA_POINTERS];
    int hw_height[NI_LOGAN_MAX_NUM_DATA_POINTERS];
    size_t hw_plane_size[NI_LOGAN_MAX_NUM_DATA_POINTERS];
    size_t hw_plane_offset[NI_LOGAN_MAX_NUM_DATA_POINTERS];
    size_t hw_frame_size;             /**< Total bytes for one HW-formatted frame */

    char *rc_mode;                     /**< Rate control mode: "CBR" or "VBR" */
    char *profile;                      /**< Encoder profile: H.264="baseline"/"main"/"high", H.265="main"/"main10" */
    char *gop_preset;                  /**< GOP preset: "simple" (I-P-P-P) or "default" (I-B-B-B-P) */
    bool repeat_headers;               /**< If true, attach SPS/PPS to every keyframe */
    int codec_type;                    /**< Codec type: 0 = H.264, 1 = H.265 (HEVC) */
    uint64_t frame_count;              /**< Total frames processed (for start_of_stream logic) */
    int keyint_frames;                 /**< Keyframe interval in frames */
    int qp_value;                      /**< Constant QP value used when rate control is disabled */
    bool lossless;                     /**< Lossless encoding enabled (HEVC only, RC disabled) */

    /* Error tracking and health monitoring */
    int consecutive_errors;            /**< Count of consecutive errors (reset on success) */
    int total_errors;                  /**< Total error count since encoder creation */
    uint64_t encoder_start_time;       /**< Timestamp (os_gettime_ns) when encoder was created */
    
#ifdef DEBUG_NETINT_PLUGIN
    uint32_t debug_magic;             /**< Magic number for validation */
#endif
};

/**
 * @brief Get the display name for H.264 encoder
 * 
 * This function is called by OBS Studio to display the encoder name in the UI.
 * The name appears in the encoder selection dropdown menu.
 * 
 * @param type_data Unused - OBS encoder type data (not used in this implementation)
 * @return Static string "NETINT T4XX H.264" - the display name for this encoder
 */
static const char *netint_h264_get_name(void *type_data)
{
    UNUSED_PARAMETER(type_data);
    return "NETINT T4XX H.264";
}

/**
 * @brief Get the display name for H.265 encoder
 * 
 * This function is called by OBS Studio to display the encoder name in the UI.
 * The name appears in the encoder selection dropdown menu.
 * 
 * @param type_data Unused - OBS encoder type data (not used in this implementation)
 * @return Static string "NETINT T4XX H.265" - the display name for this encoder
 */
static const char *netint_h265_get_name(void *type_data)
{
    UNUSED_PARAMETER(type_data);
    return "NETINT T4XX H.265";
}

/* Forward declarations */
static void netint_destroy(void *data);
static void *netint_io_thread(void *data);
static void netint_enqueue_job(struct netint_ctx *ctx, struct netint_frame_job *job, bool count_frame);
static struct netint_frame_job *netint_dequeue_job(struct netint_ctx *ctx, bool wait_for_job);
static void netint_destroy_job_pool(struct netint_ctx *ctx);
static bool netint_init_job_pool(struct netint_ctx *ctx);
static struct netint_frame_job *netint_acquire_job(struct netint_ctx *ctx, bool require_buffer);
static void netint_release_job(struct netint_ctx *ctx, struct netint_frame_job *job);
static bool netint_queue_frame(struct netint_ctx *ctx, struct encoder_frame *frame);
static bool netint_queue_eos(struct netint_ctx *ctx);
static bool netint_hw_send_job(struct netint_ctx *ctx, struct netint_frame_job *job);
static bool netint_hw_receive_once(struct netint_ctx *ctx);
static void netint_hw_drain(struct netint_ctx *ctx, bool drain_all);
static bool netint_set_encoder_param(struct netint_ctx *ctx, ni_logan_encoder_params_t *params,
                                     ni_logan_session_context_t *session_ctx,
                                     const char *name, const char *value);

/**
 * @brief Simple error logging helper
 * 
 * @param ctx Encoder context
 * @param operation Name of the operation that failed
 * @param ret_code Return code from API
 */
static void netint_log_error(struct netint_ctx *ctx, const char *operation, int ret_code)
{
    if (!ctx) return;
    
    ctx->consecutive_errors++;
    ctx->total_errors++;
    
    blog(LOG_ERROR, "[obs-netint-t4xx] %s failed with ret=%d (consecutive: %d, total: %d)",
          operation, ret_code, ctx->consecutive_errors, ctx->total_errors);
    
    /* If too many consecutive errors, warn user */
    if (ctx->consecutive_errors >= MAX_CONSECUTIVE_ERRORS) {
        blog(LOG_ERROR, "[obs-netint-t4xx] Too many consecutive errors (%d), encoder may need recreation",
              ctx->consecutive_errors);
    }
}

static bool netint_set_encoder_param(struct netint_ctx *ctx, ni_logan_encoder_params_t *params,
                                     ni_logan_session_context_t *session_ctx,
                                     const char *name, const char *value)
{
    if (!params || !session_ctx) {
        blog(LOG_ERROR, "[obs-netint-t4xx] Tried to set encoder param %s=%s but encoder params/session_ctx are NULL",
             name ? name : "(null)", value ? value : "(null)");
        return false;
    }

    if (!p_ni_logan_encoder_params_set_value) {
        blog(LOG_ERROR, "[obs-netint-t4xx] ni_logan_encoder_params_set_value symbol missing while setting %s=%s",
             name ? name : "(null)", value ? value : "(null)");
        return false;
    }

    int ret = p_ni_logan_encoder_params_set_value(params, name, value, session_ctx);
    if (ret != NI_LOGAN_RETCODE_SUCCESS) {
        blog(LOG_ERROR, "[obs-netint-t4xx] Failed to set encoder param %s=%s (ret=%d)",
             name ? name : "(null)", value ? value : "(null)", ret);
        return false;
    }

    blog(LOG_DEBUG, "[obs-netint-t4xx] Encoder param %s=%s applied successfully", name, value);
    return true;
}

/**
 * @brief Create and initialize a new encoder instance
 * 
 * This function is called by OBS Studio when a user selects this encoder and configures it.
 * It performs the following operations:
 * 1. Ensures libxcoder library is loaded
 * 2. Allocates and initializes encoder context structure
 * 3. Extracts configuration from OBS settings
 * 4. Configures hardware encoder with parameters
 * 5. Opens connection to hardware device
 * 6. Extracts SPS/PPS headers for stream initialization
 * 7. Starts background thread for packet reception
 * 
 * Configuration Parameters (from settings):
 * - bitrate: Target bitrate in kbps (converted to bps for hardware)
 * - keyint: Keyframe interval in seconds (auto-calculated if not set)
 * - device: Optional device name (auto-discovered if not specified)
 * - codec: "h264" or "h265" (auto-detected from encoder codec if not set)
 * - rc_mode: "CBR" (constant bitrate) or "VBR" (variable bitrate)
 * - profile: H.264="baseline"/"main"/"high", H.265="main"/"main10"
 * - repeat_headers: If true, attach SPS/PPS to every keyframe
 * 
 * Device Selection:
 * - If device name is provided in settings, use it
 * - Otherwise, use device discovery API to find first available device
 * - If discovery fails, encoder will use default device selection
 * 
 * Error Handling:
 * - Returns NULL on any failure (library not loaded, init failed, etc.)
 * - All allocated resources are cleaned up via netint_destroy() on failure
 * - Error messages logged to OBS log with [obs-netint-t4xx] prefix
 * 
 * Threading:
 * - Creates background thread for packet reception (reduces latency)
 * - Thread is started before returning (encoder ready to use immediately)
 * 
 * @param settings OBS settings object containing encoder configuration
 * @param encoder OBS encoder handle (used to get video info, codec type, etc.)
 * @return Pointer to encoder context on success, NULL on failure
 */
static void *netint_create(obs_data_t *settings, obs_encoder_t *encoder)
{
    /* Check if library is loaded - if not, try to load it now */
    /* This handles the case where plugin loaded but library wasn't available at load time */
    if (!p_ni_logan_encode_init) {
        if (!netint_loader_init()) {
#ifdef _WIN32
            blog(LOG_ERROR, "[obs-netint-t4xx] libxcoder_logan.dll not available. Cannot create encoder.");
#else
            blog(LOG_ERROR, "[obs-netint-t4xx] libxcoder_logan.so not available. Cannot create encoder.");
#endif
            return NULL;
        }
    }

    /* Allocate encoder context structure - zero-initialized for safety */
    struct netint_ctx *ctx = bzalloc(sizeof(*ctx));
    ctx->encoder = encoder;
    
    /* Initialize error tracking */
    ctx->consecutive_errors = 0;
    ctx->total_errors = 0;
    ctx->encoder_start_time = os_gettime_ns();
    ctx->frame_count = 0;
    ctx->frames_submitted = 0;
    ctx->pending_jobs = 0;
    ctx->inflight_frames = 0;
    ctx->pkt_queue_head = NULL;
    ctx->pkt_queue_tail = NULL;
    ctx->frame_queue_head = NULL;
    ctx->frame_queue_tail = NULL;
    
#ifdef DEBUG_NETINT_PLUGIN
    /* Initialize debug magic for validation - ALWAYS set this! */
    ctx->debug_magic = NETINT_ENC_CONTEXT_MAGIC;
    blog(LOG_INFO, "[DEBUG] Encoder context allocated at %p, size=%zu", ctx, sizeof(*ctx));
    blog(LOG_INFO, "[DEBUG] Debug magic initialized to 0x%08X", ctx->debug_magic);
#endif

    /* Get video output information to determine frame rate and format */
    video_t *video = obs_encoder_video(encoder);
    const struct video_output_info *voi = video_output_get_info(video);

    /* Zero-initialize encoder context structure (EMBEDDED, not allocated) */
    memset(&ctx->enc, 0, sizeof(ctx->enc));

    /* Set basic encoder parameters */
    ctx->enc.dev_enc_idx = 1;  /* H/W ID 1 = encoder (H/W ID 0 = decoder) */
    ctx->enc.keep_alive_timeout = 3;  /* Default timeout in seconds */
    ctx->enc.set_high_priority = 0;   /* Don't set high priority by default */
    
    /* IMPORTANT: dev_xcoder MUST be set before calling ni_logan_encode_init! */
    /* The init function calls strcmp() on dev_xcoder, which crashes if NULL */
    ctx->enc.dev_xcoder = (char *)bstrdup("");  /* Empty string initially */
    
    /* Set basic video parameters from OBS encoder */
    ctx->enc.width = (int)obs_encoder_get_width(encoder);
    ctx->enc.height = (int)obs_encoder_get_height(encoder);
    
    /* Get bitrate from settings and convert from kbps to bps (hardware expects bps) */
    ctx->enc.bit_rate = (int64_t)obs_data_get_int(settings, "bitrate") * 1000;
    
    /* Device selection: try user-specified device first, then auto-discovery */
    const char *dev_name = obs_data_get_string(settings, "device");
    if (dev_name && *dev_name) {
        /* User specified a device name - use it directly */
        blog(LOG_INFO, "[obs-netint-t4xx] Using device from USER SETTINGS: '%s'", dev_name);
        bfree(ctx->enc.dev_enc_name);
        bfree(ctx->enc.dev_xcoder);
        ctx->enc.dev_enc_name = (char *)bstrdup(dev_name);
        ctx->enc.dev_xcoder = (char *)bstrdup(dev_name);
    } else if (p_ni_logan_rsrc_init && p_ni_logan_rsrc_get_local_device_list) {
        /* No device specified - try to discover available devices */
        blog(LOG_INFO, "[obs-netint-t4xx] No device in settings, attempting AUTO-DISCOVERY...");
        /* Initialize resource management system (should_match_rev=0, timeout=1s) */
        /* Accept both SUCCESS (0) and INIT_ALREADY (0x7FFFFFFF) as success */
        int rsrc_ret = p_ni_logan_rsrc_init(0, 1);
        blog(LOG_INFO, "[obs-netint-t4xx] ni_logan_rsrc_init returned: %d (0x%X)", rsrc_ret, rsrc_ret);
        if (rsrc_ret == 0 || rsrc_ret == 0x7FFFFFFF) {
            char names[16][NI_LOGAN_MAX_DEVICE_NAME_LEN] = {0};
            int n = p_ni_logan_rsrc_get_local_device_list(names, 16);
            blog(LOG_INFO, "[obs-netint-t4xx] Found %d device(s) via auto-discovery", n);
            if (n > 0) {
                /* Use the first available device */
                blog(LOG_INFO, "[obs-netint-t4xx] AUTO-DETECTED device: '%s'", names[0]);
                bfree(ctx->enc.dev_enc_name);
                bfree(ctx->enc.dev_xcoder);
                ctx->enc.dev_enc_name = (char *)bstrdup(names[0]);
                ctx->enc.dev_xcoder = (char *)bstrdup(names[0]);
            } else {
                blog(LOG_WARNING, "[obs-netint-t4xx] Auto-discovery found 0 devices, encoder will use default device");
            }
        } else {
            blog(LOG_WARNING, "[obs-netint-t4xx] Resource init failed (ret=%d), cannot auto-discover devices", rsrc_ret);
        }
    } else {
        blog(LOG_INFO, "[obs-netint-t4xx] Device discovery APIs not available, encoder will use default device");
    }
    
    /* Keyframe interval: get from settings, or auto-calculate based on frame rate */
    /* Default is 2 seconds worth of frames (ensures regular keyframes for seeking) */
    int keyint_seconds = (int)obs_data_get_int(settings, "keyint");
    if (keyint_seconds <= 0) keyint_seconds = 2; /* Default 2 seconds */
    
    /* Convert seconds to frames based on framerate */
    ctx->keyint_frames = (int)(keyint_seconds * (voi->fps_num / (double)voi->fps_den));
    blog(LOG_INFO, "[obs-netint-t4xx] Keyframe interval: %d seconds = %d frames @ %.2f fps",
         keyint_seconds, ctx->keyint_frames, voi->fps_num / (double)voi->fps_den);
    
    /* Set timebase for timestamps (matches OBS video output timebase) */
    /* timebase = fps_den / fps_num (e.g., 1/30 for 30fps, 1001/30000 for 29.97fps) */
    ctx->enc.timebase_num = (int)voi->fps_den;
    ctx->enc.timebase_den = (int)voi->fps_num;
    ctx->enc.ticks_per_frame = 1;
    ctx->enc.fps_number = (int)voi->fps_num;
    ctx->enc.fps_denominator = (int)voi->fps_den;
    blog(LOG_INFO, "[obs-netint-t4xx] Encoder timebase=%d/%d fps=%d/%d",
         ctx->enc.timebase_num, ctx->enc.timebase_den,
         ctx->enc.fps_number, ctx->enc.fps_denominator);
    
    /* Codec selection: Determined by which encoder registration OBS used */
    /* OBS will call the appropriate create function based on encoder ID */
    const char *codec_str = obs_encoder_get_codec(encoder);
    
    blog(LOG_INFO, "[obs-netint-t4xx] Codec from OBS encoder registration: '%s'", 
         codec_str ? codec_str : "(null)");
    
    /* Set codec format based on OBS encoder registration */
    /* Store codec type in context for later use (keyframe detection, packet parsing) */
    if (codec_str && strcmp(codec_str, "hevc") == 0) {
        ctx->codec_type = 1; /* H.265 (HEVC) */
        ctx->enc.codec_format = 1; /* NI_LOGAN_CODEC_FORMAT_H265 */
        blog(LOG_INFO, "[obs-netint-t4xx] Codec selected: H.265 (HEVC) - codec_type=1, codec_format=1");
    } else {
        ctx->codec_type = 0; /* H.264 (AVC) */
        ctx->enc.codec_format = 0; /* NI_LOGAN_CODEC_FORMAT_H264 */
        blog(LOG_INFO, "[obs-netint-t4xx] Codec selected: H.264 (AVC) - codec_type=0, codec_format=0");
    }
    
    /* Set pixel format - currently only YUV420P is supported */
    /* YUV420P = Planar YUV 4:2:0 (separate Y, U, V planes) */
    ctx->enc.pix_fmt = NI_LOGAN_PIX_FMT_YUV420P;

    memset(ctx->hw_stride, 0, sizeof(ctx->hw_stride));
    memset(ctx->hw_height, 0, sizeof(ctx->hw_height));
    memset(ctx->hw_plane_size, 0, sizeof(ctx->hw_plane_size));
    memset(ctx->hw_plane_offset, 0, sizeof(ctx->hw_plane_offset));

    int bit_depth_factor = 1; /* Only 8-bit supported by OBS today */
    int is_h264 = (ctx->codec_type == 0) ? 1 : 0;
    p_ni_logan_get_hw_yuv420p_dim(ctx->enc.width, ctx->enc.height, bit_depth_factor, is_h264,
                                  ctx->hw_stride, ctx->hw_height);

    ctx->hw_frame_size = 0;
    for (int i = 0; i < NI_LOGAN_MAX_NUM_DATA_POINTERS; i++) {
        ctx->hw_plane_offset[i] = ctx->hw_frame_size;
        if (ctx->hw_stride[i] > 0 && ctx->hw_height[i] > 0) {
            ctx->hw_plane_size[i] = (size_t)ctx->hw_stride[i] * (size_t)ctx->hw_height[i];
            ctx->hw_frame_size += ctx->hw_plane_size[i];
        } else {
            ctx->hw_plane_size[i] = 0;
        }
    }

    ctx->max_inflight = 4; /* Allow up to 4 frames queued in hardware before draining */

    if (!netint_init_job_pool(ctx)) {
        blog(LOG_ERROR, "[obs-netint-t4xx] Failed to initialize frame job pool");
        goto fail;
    }
    
    /* Initialize color space parameters (required by library) */
    ctx->enc.color_primaries = 2;  /* NI_COL_PRI_UNSPECIFIED */
    ctx->enc.color_trc = 2;        /* NI_COL_TRC_UNSPECIFIED */
    ctx->enc.color_space = 2;      /* NI_COL_SPC_UNSPECIFIED */
    ctx->enc.color_range = 0;      /* NI_COL_RANGE_UNSPECIFIED */
    
    /* Initialize sample aspect ratio (1:1 = square pixels) */
    ctx->enc.sar_num = 1;
    ctx->enc.sar_den = 1;
    
    /* Store rate control mode, profile, and GOP preset strings (used later for parameter setting) */
    ctx->rc_mode = bstrdup(obs_data_get_string(settings, "rc_mode"));
    ctx->profile = bstrdup(obs_data_get_string(settings, "profile"));
    ctx->gop_preset = bstrdup(obs_data_get_string(settings, "gop_preset"));
    
    /* Repeat headers setting: if true, attach SPS/PPS to every keyframe */
    /* This is useful for streaming where clients may join mid-stream */
    ctx->repeat_headers = obs_data_get_bool(settings, "repeat_headers");
    ctx->qp_value = (int)obs_data_get_int(settings, "qp");
    if (ctx->qp_value < 0)
        ctx->qp_value = 0;
    else if (ctx->qp_value > 51)
        ctx->qp_value = 51;
    if (ctx->codec_type == 1) {
        ctx->lossless = obs_data_get_bool(settings, "lossless");
        if (ctx->lossless && (!ctx->rc_mode || strcmp(ctx->rc_mode, "DISABLED") != 0)) {
            blog(LOG_WARNING, "[obs-netint-t4xx] Lossless requested but rate control not disabled; lossless will be ignored");
        }
    } else {
        ctx->lossless = false;
    }
    if (ctx->repeat_headers) {
        ctx->enc.spsPpsAttach = 1;
    }

    /* Disable libxcoder verbose logging to avoid crashes in logging callback */
    /* The logging callback in libxcoder v3.5.1 has issues with some format strings */
    ctx->enc.ff_log_level = 24; /* AV_LOG_ERROR = 16, AV_LOG_WARNING = 24 */
    
    /* Initialize encoder context with all parameters we've set */
    /* This validates parameters and prepares the encoder structure */
    blog(LOG_INFO, "[obs-netint-t4xx] Calling ni_logan_encode_init with dev_xcoder='%s' dev_enc_name='%s' dev_enc_idx=%d",
         ctx->enc.dev_xcoder ? ctx->enc.dev_xcoder : "(null)",
         ctx->enc.dev_enc_name ? ctx->enc.dev_enc_name : "(null)",
         ctx->enc.dev_enc_idx);
    
    /* Validate context before API call */
    NETINT_VALIDATE_ENC_CONTEXT(ctx, "before ni_logan_encode_init");
    
#ifdef DEBUG_NETINT_PLUGIN
    /* Dump enc context memory before init */
    netint_debug_dump_memory(&ctx->enc, sizeof(ctx->enc), "enc context BEFORE init");
#endif
    
    /* Call with SEH guard to catch crashes */
    int init_ret = -1;
    NETINT_SEH_GUARDED_CALL(init_ret = p_ni_logan_encode_init(&ctx->enc), NULL);
    
    blog(LOG_INFO, "[obs-netint-t4xx] ni_logan_encode_init returned: %d", init_ret);
    
    if (init_ret < 0) {
        blog(LOG_ERROR, "[obs-netint-t4xx] Failed to initialize encoder (ret=%d)", init_ret);
        NETINT_LOG_ENCODER_STATE(ctx, "AFTER failed ni_logan_encode_init");
        goto fail;
    }
    
    /* CRITICAL CHECK: Verify that init actually allocated internal structures */
    blog(LOG_INFO, "[obs-netint-t4xx] Verifying encoder initialization...");
    blog(LOG_INFO, "[obs-netint-t4xx]   p_session_ctx = %p (should NOT be NULL)", ctx->enc.p_session_ctx);
    blog(LOG_INFO, "[obs-netint-t4xx]   p_encoder_params = %p (should NOT be NULL)", ctx->enc.p_encoder_params);
    blog(LOG_INFO, "[obs-netint-t4xx]   input_data_fifo = %p (should NOT be NULL)", ctx->enc.input_data_fifo);
    
#ifdef DEBUG_NETINT_PLUGIN
    /* DEBUG: Show struct memory layout */
    blog(LOG_DEBUG, "[obs-netint-t4xx] ========================================");
    blog(LOG_DEBUG, "[obs-netint-t4xx] STRUCT LAYOUT DEBUG:");
    blog(LOG_DEBUG, "[obs-netint-t4xx]   &ctx->enc = %p (base address)", &ctx->enc);
    blog(LOG_DEBUG, "[obs-netint-t4xx]   &ctx->enc.input_data_fifo = %p (field offset = %zu bytes)", 
         &ctx->enc.input_data_fifo, (char*)&ctx->enc.input_data_fifo - (char*)&ctx->enc);
    blog(LOG_DEBUG, "[obs-netint-t4xx]   sizeof(ni_logan_enc_context_t) in plugin = %zu bytes", sizeof(ctx->enc));
    blog(LOG_DEBUG, "[obs-netint-t4xx] ========================================");
#endif
    
    if (!ctx->enc.p_session_ctx || !ctx->enc.p_encoder_params || !ctx->enc.input_data_fifo) {
        blog(LOG_ERROR, "[obs-netint-t4xx] CRITICAL: ni_logan_encode_init returned success but didn't allocate internal structures!");
        blog(LOG_ERROR, "[obs-netint-t4xx] This indicates a library initialization failure.");
        blog(LOG_ERROR, "[obs-netint-t4xx] Check: 1) Is libxcoder_logan.dll the correct version? 2) Are all dependencies present?");
        NETINT_LOG_ENCODER_STATE(ctx, "AFTER ni_logan_encode_init with NULL internals");
        goto fail;
    }
    
#ifdef DEBUG_NETINT_PLUGIN
    /* Dump enc context memory after init */
    netint_debug_dump_memory(&ctx->enc, sizeof(ctx->enc), "enc context AFTER init");
#endif
    
    blog(LOG_INFO, "[obs-netint-t4xx] ni_logan_encode_init succeeded and allocated internal structures");

    /* ===================================================================
     * Set VUI (Video Usability Information) parameters
     * ===================================================================
     * Critical: VUI parameters must be set after encode_init but before params_parse,
     * just like xcoder_logan does. This configures color space, primaries,
     * transfer characteristics, and SAR.
     */

    blog(LOG_INFO, "[obs-netint-t4xx] Setting VUI parameters after encode_init...");
    ni_logan_encoder_params_t *vui_params = (ni_logan_encoder_params_t *)ctx->enc.p_encoder_params;
    ni_logan_session_context_t *vui_ctx = (ni_logan_session_context_t *)ctx->enc.p_session_ctx;
    if (vui_params && vui_ctx) {
        p_ni_logan_set_vui(vui_params, vui_ctx,
                           (ni_color_primaries_t)ctx->enc.color_primaries,
                           (ni_color_transfer_characteristic_t)ctx->enc.color_trc,
                           (ni_color_space_t)ctx->enc.color_space,
                           0, /* video_full_range_flag */
                           ctx->enc.sar_num, ctx->enc.sar_den,
                           (ni_logan_codec_format_t)ctx->enc.codec_format);
        blog(LOG_INFO, "[obs-netint-t4xx] VUI parameters set successfully");
    } else {
        blog(LOG_ERROR, "[obs-netint-t4xx] Cannot set VUI parameters - p_encoder_params or p_session_ctx is NULL!");
    }

    /* Set advanced encoder parameters after initial init and VUI setup */
    /* These parameters require the encoder to be initialized first (they modify internal state) */
    if (ctx->enc.p_encoder_params && p_ni_logan_encoder_params_set_value) {
        ni_logan_encoder_params_t *params = (ni_logan_encoder_params_t *)ctx->enc.p_encoder_params;
        ni_logan_session_context_t *session_ctx = (ni_logan_session_context_t *)ctx->enc.p_session_ctx;
        
        /* NOTE: Do NOT enable GenHdrs! */
        /* Some T4xx hardware/firmware doesn't support pre-generating headers */
        /* and it causes params_parse to fail with ERROR_INVALID_SESSION (-5) */
        /* Instead, we always extract headers from the first encoded packet */
        blog(LOG_INFO, "[obs-netint-t4xx] Will extract headers from first encoded packet (GenHdrs disabled)");

        /* ===================================================================
         * GOP Parameter Setting - DISABLED
         * ===================================================================
         * All GOP parameter setting APIs return "Unknown option" (-7).
         * This suggests GOP parameters cannot be set through parameter APIs
         * in this version of libxcoder_logan. The encoder will use defaults.
         *
         * If GOP parameters are needed, they may need to be set through:
         * - Direct structure field modification
         * - Low-level session context configuration
         * - Firmware-specific configuration
         *
         * For now, rely on encoder defaults and focus on getting video output.
         */

        /* Set GOP preset based on user preference */
        const char *gop_value = "5";  /* Default: GOP 5 (I-B-B-B-P, best quality) */
        const char *gop_desc = "default (I-B-B-B-P)";
        
        if (ctx->gop_preset && strcmp(ctx->gop_preset, "simple") == 0) {
            gop_value = "2";  /* GOP 2 (I-P-P-P, no B-frames, lower latency) */
            gop_desc = "simple (I-P-P-P, no B-frames)";
        }
        
        if (!netint_set_encoder_param(ctx, params, session_ctx, "gopPresetIdx", gop_value)) {
            blog(LOG_ERROR, "[obs-netint-t4xx] Failed to set GOP preset to %s", gop_desc);
            goto fail;
        }
        blog(LOG_INFO, "[obs-netint-t4xx] GOP set to %s", gop_desc);

        const bool rc_disabled = (ctx->rc_mode && strcmp(ctx->rc_mode, "DISABLED") == 0);

        if (rc_disabled) {
            if (!netint_set_encoder_param(ctx, params, session_ctx, "RcEnable", "0")) {
                blog(LOG_ERROR, "[obs-netint-t4xx] Failed to disable rate control");
                goto fail;
            }
            blog(LOG_INFO, "[obs-netint-t4xx] Rate control DISABLED (RcEnable=0)");

            char qp_str[16];
            snprintf(qp_str, sizeof(qp_str), "%d", ctx->qp_value);
            if (!netint_set_encoder_param(ctx, params, session_ctx, "intraQP", qp_str) ||
                !netint_set_encoder_param(ctx, params, session_ctx, "minQp", qp_str) ||
                !netint_set_encoder_param(ctx, params, session_ctx, "maxQp", qp_str)) {
                blog(LOG_ERROR, "[obs-netint-t4xx] Failed to set constant QP parameters");
                goto fail;
            }
            blog(LOG_INFO, "[obs-netint-t4xx] Constant QP mode: intraQP/minQp/maxQp set to %d", ctx->qp_value);

            /* Ensure RC-specific flags are cleared */
            if (!netint_set_encoder_param(ctx, params, session_ctx, "cbr", "0")) {
                blog(LOG_ERROR, "[obs-netint-t4xx] Failed to clear CBR flag while RC disabled");
                goto fail;
            }

            if (ctx->codec_type == 1 && ctx->lossless) {
                if (!netint_set_encoder_param(ctx, params, session_ctx, "losslessEnable", "1")) {
                    blog(LOG_ERROR, "[obs-netint-t4xx] Failed to enable lossless mode");
                    goto fail;
                }
                blog(LOG_INFO, "[obs-netint-t4xx] Lossless HEVC encoding enabled");
            } else {
                if (!netint_set_encoder_param(ctx, params, session_ctx, "losslessEnable", "0")) {
                    blog(LOG_ERROR, "[obs-netint-t4xx] Failed to disable lossless flag");
                    goto fail;
                }
            }
        } else {
            /* CRITICAL: Enable rate control first! Without this, encoder uses Constant QP mode and ignores bitrate! */
            if (!netint_set_encoder_param(ctx, params, session_ctx, "RcEnable", "1")) {
                blog(LOG_ERROR, "[obs-netint-t4xx] Failed to enable rate control");
                goto fail;
            }
            blog(LOG_INFO, "[obs-netint-t4xx] Rate control ENABLED (RcEnable=1)");

            /* Set bitrate and framerate parameters */
            char bitrate_str[32];
            char framerate_str[32];
            char framerate_denom_str[32];

            sprintf(bitrate_str, "%lld", (long long)ctx->enc.bit_rate);
            sprintf(framerate_str, "%d", ctx->enc.timebase_den);
            sprintf(framerate_denom_str, "%d", ctx->enc.timebase_num);

            if (!netint_set_encoder_param(ctx, params, session_ctx, "bitrate", bitrate_str)) {
                blog(LOG_ERROR, "[obs-netint-t4xx] Failed to apply target bitrate %lld bps",
                     (long long)ctx->enc.bit_rate);
                goto fail;
            }
            blog(LOG_INFO, "[obs-netint-t4xx] Bitrate parameter set to %lld bps (%d kbps)",
                 (long long)ctx->enc.bit_rate, (int)(ctx->enc.bit_rate / 1000));
            
            if (!netint_set_encoder_param(ctx, params, session_ctx, "frameRate", framerate_str) ||
                !netint_set_encoder_param(ctx, params, session_ctx, "frameRateDenom", framerate_denom_str)) {
                blog(LOG_ERROR, "[obs-netint-t4xx] Failed to set encoder framerate parameters (%s/%s)",
                     framerate_str, framerate_denom_str);
                goto fail;
            }
            blog(LOG_INFO, "[obs-netint-t4xx] Framerate parameters: %d/%d (%.2f fps)",
                 ctx->enc.timebase_den, ctx->enc.timebase_num,
                 ctx->enc.timebase_den / (double)ctx->enc.timebase_num);
            
            if (!netint_set_encoder_param(ctx, params, session_ctx, "RcInitDelay", "3000")) {
                blog(LOG_ERROR, "[obs-netint-t4xx] Failed to set VBV buffer size (RcInitDelay)");
                goto fail;
            }
            blog(LOG_INFO, "[obs-netint-t4xx] VBV buffer size (RCInitDelay) set to 3000 ms");

            /* Set rate control mode: CBR (constant) or VBR (variable) */
            /* CBR = constant bitrate (good for streaming), VBR = variable bitrate (better quality) */
            if (ctx->rc_mode && strcmp(ctx->rc_mode, "CBR") == 0) {
                if (!netint_set_encoder_param(ctx, params, session_ctx, "cbr", "1")) {
                    blog(LOG_ERROR, "[obs-netint-t4xx] Failed to enable CBR mode");
                    goto fail;
                }
                blog(LOG_INFO, "[obs-netint-t4xx] Rate control mode: CBR (constant bitrate)");
            } else {
                if (!netint_set_encoder_param(ctx, params, session_ctx, "cbr", "0")) {
                    blog(LOG_ERROR, "[obs-netint-t4xx] Failed to enable VBR mode");
                    goto fail;
                }
                blog(LOG_INFO, "[obs-netint-t4xx] Rate control mode: VBR (variable bitrate)");
            }

            /* Lossless is incompatible with rate control */
            if (!netint_set_encoder_param(ctx, params, session_ctx, "losslessEnable", "0")) {
                blog(LOG_ERROR, "[obs-netint-t4xx] Failed to ensure lossless disabled while RC enabled");
                goto fail;
            }
        }
        
        /* Set encoder profile - maps string names to codec-specific profile IDs */
        /* Profile determines feature set: baseline < main < high */
        /* Profile IDs differ between H.264 and H.265 */
        if (ctx->profile) {
            const char *profile_id_str = NULL;
            if (ctx->codec_type == 1) {
                /* H.265 (HEVC) - NETINT-specific profile IDs
                 * CRITICAL: NETINT H.265 encoder ONLY supports 2 profiles:
                 *   Profile 1 = Main (8-bit)
                 *   Profile 2 = Main10 (10-bit)
                 * 
                 * H.265 standard does NOT have a "high" profile like H.264!
                 * According to NETINT Integration Guide section 6.6:
                 *   "Any profile can be used for 8 bit encoding but only the 
                 *    10 bit profiles (main10 for H.265) may be used for 10 bit encoding"
                 */
                if (strcmp(ctx->profile, "main10") == 0) {
                    profile_id_str = "2"; /* Main10 profile (10-bit) */
                    blog(LOG_INFO, "[obs-netint-t4xx] H.265 profile: Main10 (ID=2) - 10-bit encoding");
                } else {
                    profile_id_str = "1"; /* Main profile (8-bit) - default */
                    blog(LOG_INFO, "[obs-netint-t4xx] H.265 profile: Main (ID=1) - 8-bit encoding");
                }
            } else {
                /* H.264 profiles - libxcoder expects enum values, NOT H.264 spec profile_idc! */
                /* From NETINT Integration Guide section 6.6:
                 *   1=baseline, 2=main, 3=extended, 4=high, 5=high10 */
                if (strcmp(ctx->profile, "baseline") == 0) {
                    profile_id_str = "1"; /* Baseline profile (enum value) */
                } else if (strcmp(ctx->profile, "main") == 0) {
                    profile_id_str = "2"; /* Main profile (enum value) */
                } else if (strcmp(ctx->profile, "high") == 0) {
                    profile_id_str = "4"; /* High profile (enum value) */
                }
            }
            if (profile_id_str) {
                if (!netint_set_encoder_param(ctx, params, session_ctx, "profile", profile_id_str)) {
                    blog(LOG_ERROR, "[obs-netint-t4xx] Failed to set encoder profile to %s", ctx->profile);
                    goto fail;
                }
                blog(LOG_INFO, "[obs-netint-t4xx] Profile set to: %s (ID=%s)", ctx->profile, profile_id_str);
            }
        }
    }
    
    /* Parse and validate all encoder parameters */
    /* This checks for parameter conflicts and applies defaults */
    /* If generate_enc_hdrs is set, this will also generate headers automatically */
    blog(LOG_INFO, "[obs-netint-t4xx] Calling ni_logan_encode_params_parse (will generate headers)...");
    
    /* Validate context before API call */
    NETINT_VALIDATE_ENC_CONTEXT(ctx, "before ni_logan_encode_params_parse");
    
    /* Call with SEH guard to catch crashes */
    int parse_ret = -1;
    NETINT_SEH_GUARDED_CALL(parse_ret = p_ni_logan_encode_params_parse(&ctx->enc), NULL);
    
    blog(LOG_INFO, "[obs-netint-t4xx] ni_logan_encode_params_parse returned: %d", parse_ret);
    
    if (parse_ret < 0) {
        blog(LOG_ERROR, "[obs-netint-t4xx] Failed to parse encoder parameters (ret=%d)", parse_ret);
        NETINT_LOG_ENCODER_STATE(ctx, "AFTER failed ni_logan_encode_params_parse");
        goto fail;
    }
    
    /* Check if headers were generated during params_parse */
    blog(LOG_INFO, "[obs-netint-t4xx] After params_parse: extradata=%p, extradata_size=%d", 
         ctx->enc.extradata, ctx->enc.extradata_size);
    
    if (ctx->enc.extradata && ctx->enc.extradata_size > 0) {
        /* Headers were successfully generated - use them */
        ctx->extra = bmemdup(ctx->enc.extradata, (size_t)ctx->enc.extradata_size);
        ctx->extra_size = (size_t)ctx->enc.extradata_size;
        blog(LOG_INFO, "[obs-netint-t4xx] Headers generated during init, size: %zu bytes", ctx->extra_size);
        ctx->got_headers = true;
    } else {
        /* Headers not generated - this is normal for some T4xx hardware/firmware versions */
        /* We'll extract them from the first encoded packet instead */
        blog(LOG_INFO, "[obs-netint-t4xx] Headers not available during init. Will extract from first encoded packet.");
        ctx->got_headers = false;
    }
    
    /* ===================================================================
     * Open and configure encoder session
     * ===================================================================
     * 
     * CRITICAL: We use encode_init/params_parse (high-level) for initialization,
     * so we MUST call encode_open() (high-level) to configure the hardware!
     * 
     * encode_open() does important things:
     * - Sends encoder configuration to hardware
     * - Configures GOP, bitrate, resolution settings
     * - Initializes hardware encoder instance
     * 
     * After that, we use the high-level encode_get_frame/encode_send and
     * encode_receive APIs for data movement. This keeps us on the supported
     * FIFO path provided by libxcoder while still benefiting from the
     * simplified configuration helpers.
     */
    
    blog(LOG_INFO, "[obs-netint-t4xx] Opening and configuring encoder session...");
    int open_ret = p_ni_logan_encode_open(&ctx->enc);
    
    if (open_ret != NI_LOGAN_RETCODE_SUCCESS) {
        blog(LOG_ERROR, "[obs-netint-t4xx] Failed to open encoder session (ret=%d)", open_ret);
        blog(LOG_ERROR, "[obs-netint-t4xx] Check: 1) Is the hardware device accessible? 2) Is another process using it?");
        goto fail;
    }
    
    blog(LOG_INFO, "[obs-netint-t4xx] ✅ Encoder session opened and configured!");
    blog(LOG_INFO, "[obs-netint-t4xx] Hardware is now ready to accept frames");

    /* encode_send() expects started=1 if session already opened */
    ctx->enc.started = 1;

    blog(LOG_INFO, "[obs-netint-t4xx] Encoder initialization complete!");
    blog(LOG_INFO, "[obs-netint-t4xx] Headers will be extracted from first encoded packet");

    /* ===================================================================
     * Initialize background receive thread
     * ===================================================================
     * 
     * CRITICAL: The background thread is NECESSARY because encode_receive()
     * blocks internally. Hardware won't process frames until output is drained!
     */
    
    if (pthread_mutex_init(&ctx->queue_mutex, NULL) != 0) {
        blog(LOG_ERROR, "[obs-netint-t4xx] Failed to initialize packet queue mutex");
        netint_destroy(ctx);
        return NULL;
    }
    ctx->queue_mutex_initialized = true;

    if (pthread_mutex_init(&ctx->io_mutex, NULL) != 0) {
        blog(LOG_ERROR, "[obs-netint-t4xx] Failed to initialize IO mutex");
        pthread_mutex_destroy(&ctx->queue_mutex);
        ctx->queue_mutex_initialized = false;
        netint_destroy(ctx);
        return NULL;
    }
    ctx->io_mutex_initialized = true;

    if (pthread_mutex_init(&ctx->frame_queue_mutex, NULL) != 0) {
        blog(LOG_ERROR, "[obs-netint-t4xx] Failed to initialize frame queue mutex");
        pthread_mutex_destroy(&ctx->io_mutex);
        ctx->io_mutex_initialized = false;
        pthread_mutex_destroy(&ctx->queue_mutex);
        ctx->queue_mutex_initialized = false;
        netint_destroy(ctx);
        return NULL;
    }
    ctx->frame_queue_mutex_initialized = true;

    if (pthread_cond_init(&ctx->frame_queue_cond, NULL) != 0) {
        blog(LOG_ERROR, "[obs-netint-t4xx] Failed to initialize frame queue condition variable");
        pthread_mutex_destroy(&ctx->frame_queue_mutex);
        ctx->frame_queue_mutex_initialized = false;
        pthread_mutex_destroy(&ctx->io_mutex);
        ctx->io_mutex_initialized = false;
        pthread_mutex_destroy(&ctx->queue_mutex);
        ctx->queue_mutex_initialized = false;
        netint_destroy(ctx);
        return NULL;
    }
    ctx->frame_queue_cond_initialized = true;

    ctx->stop_thread = false;
    ctx->thread_created = false;
    ctx->flushing = false;

    if (pthread_create(&ctx->io_thread, NULL, netint_io_thread, ctx) != 0) {
        blog(LOG_ERROR, "[obs-netint-t4xx] Failed to create IO thread");
        pthread_cond_destroy(&ctx->frame_queue_cond);
        ctx->frame_queue_cond_initialized = false;
        pthread_mutex_destroy(&ctx->frame_queue_mutex);
        ctx->frame_queue_mutex_initialized = false;
        pthread_mutex_destroy(&ctx->io_mutex);
        ctx->io_mutex_initialized = false;
        pthread_mutex_destroy(&ctx->queue_mutex);
        ctx->queue_mutex_initialized = false;
        netint_destroy(ctx);
        return NULL;
    }

    ctx->thread_created = true;
    blog(LOG_INFO, "[obs-netint-t4xx] Background IO thread started successfully");
    blog(LOG_INFO, "[obs-netint-t4xx] Encoder creation complete (pipelined design)");
    return ctx;
fail:
    /* Cleanup all allocated resources on any failure */
    netint_destroy(ctx);
    return NULL;
}

/**
 * @brief Destroy encoder instance and free all resources
 * 
 * This function is called by OBS Studio when the encoder is no longer needed
 * (e.g., user changes encoder, stops streaming/recording, OBS shuts down).
 * 
 * Cleanup order is important:
 * 1. Signal background thread to stop
 * 2. Wait for thread to finish (pthread_join)
 * 3. Free all queued packets
 * 4. Destroy queue and mutex
 * 5. Close hardware encoder connection
 * 6. Free all allocated memory
 * 
 * Thread Safety:
 * - This function should be called from the same thread that created the encoder
 * - Background thread is stopped before cleanup, so no concurrent access issues
 * 
 * @param data Pointer to encoder context (netint_ctx structure)
 */
static void netint_destroy(void *data)
{
    struct netint_ctx *ctx = data;
    if (!ctx) return;
    
    blog(LOG_INFO, "[obs-netint-t4xx] ========================================");
    blog(LOG_INFO, "[obs-netint-t4xx] netint_destroy called - closing encoder");
    blog(LOG_INFO, "[obs-netint-t4xx] ========================================");
    
    /* Check EOS handshake status */
    blog(LOG_INFO, "[obs-netint-t4xx] EOS handshake status:");
    blog(LOG_INFO, "[obs-netint-t4xx]   flushing = %d (should be 1 if stop was requested)", ctx->flushing);
    blog(LOG_INFO, "[obs-netint-t4xx]   encoder_eof = %d (should be 1 if EOS acknowledged)", ctx->enc.encoder_eof);
    
    if (ctx->flushing && ctx->enc.encoder_eof) {
        blog(LOG_INFO, "[obs-netint-t4xx] ✅ PROPER SHUTDOWN: EOS handshake completed successfully");
    } else if (ctx->flushing && !ctx->enc.encoder_eof) {
        blog(LOG_WARNING, "[obs-netint-t4xx] ⚠️  INCOMPLETE SHUTDOWN: EOS sent but not acknowledged by encoder");
        blog(LOG_WARNING, "[obs-netint-t4xx] This may indicate encoder is still processing or thread stopped early");
    } else {
        blog(LOG_WARNING, "[obs-netint-t4xx] ⚠️  ABRUPT SHUTDOWN: No EOS handshake performed");
        blog(LOG_WARNING, "[obs-netint-t4xx] OBS skipped flush - we'll send EOS frame now in destroy");
    }

#ifdef DEBUG_NETINT_PLUGIN
    /* Validate magic before destroy */
    if (ctx->debug_magic != NETINT_ENC_CONTEXT_MAGIC) {
        blog(LOG_WARNING, "[DEBUG] Invalid context magic in netint_destroy: 0x%08X", ctx->debug_magic);
    }
#endif
    
    /* ===================================================================
     * Send EOS frame if not already done (OBS doesn't call flush for this encoder)
     * ===================================================================
     */
    if (ctx->thread_created) {
        if (!ctx->flushing && ctx->enc.p_session_ctx) {
            blog(LOG_INFO, "[obs-netint-t4xx] Queueing EOS job during destroy");
            if (netint_queue_eos(ctx)) {
                ctx->flushing = true;
            } else {
                blog(LOG_ERROR, "[obs-netint-t4xx] Failed to queue EOS job during destroy");
            }
        }

        blog(LOG_INFO, "[obs-netint-t4xx] Stopping IO thread...");
        if (ctx->frame_queue_mutex_initialized) {
            pthread_mutex_lock(&ctx->frame_queue_mutex);
            ctx->stop_thread = true;
            if (ctx->frame_queue_cond_initialized) {
                pthread_cond_broadcast(&ctx->frame_queue_cond);
            }
            pthread_mutex_unlock(&ctx->frame_queue_mutex);
        } else {
            ctx->stop_thread = true;
            if (ctx->frame_queue_cond_initialized) {
                pthread_cond_broadcast(&ctx->frame_queue_cond);
            }
        }

        pthread_join(ctx->io_thread, NULL);
        blog(LOG_INFO, "[obs-netint-t4xx] IO thread stopped");
        ctx->thread_created = false;
    }

    /* Free any remaining frame jobs (should be none) */
    if (ctx->frame_queue_mutex_initialized) {
        struct netint_frame_job *pending_job = NULL;
        while ((pending_job = netint_dequeue_job(ctx, false)) != NULL) {
            netint_release_job(ctx, pending_job);
        }
    }

    netint_destroy_job_pool(ctx);
    
    /* Free all queued packets */
    struct netint_pkt *pkt = ctx->pkt_queue_head;
    while (pkt) {
        struct netint_pkt *next = pkt->next;
        if (pkt->data) bfree(pkt->data);
        bfree(pkt);
        pkt = next;
    }
    ctx->pkt_queue_head = NULL;
    ctx->pkt_queue_tail = NULL;
    
    /* Destroy mutexes LAST (after thread is stopped!) */
    if (ctx->frame_queue_cond_initialized) {
        pthread_cond_destroy(&ctx->frame_queue_cond);
        ctx->frame_queue_cond_initialized = false;
    }

    if (ctx->frame_queue_mutex_initialized) {
        pthread_mutex_destroy(&ctx->frame_queue_mutex);
        ctx->frame_queue_mutex_initialized = false;
    }

    if (ctx->io_mutex_initialized) {
        pthread_mutex_destroy(&ctx->io_mutex);
        ctx->io_mutex_initialized = false;
    }

    if (ctx->queue_mutex_initialized) {
        pthread_mutex_destroy(&ctx->queue_mutex);
        ctx->queue_mutex_initialized = false;
    }
    
    /* Close hardware encoder connection */
    /* We use encode_open() for initialization, so use encode_close() for cleanup */
    if (ctx->enc.p_session_ctx) {
        blog(LOG_INFO, "[obs-netint-t4xx] Closing encoder session...");
        if (p_ni_logan_encode_close) {
            int close_ret = p_ni_logan_encode_close(&ctx->enc);
            blog(LOG_INFO, "[obs-netint-t4xx] ni_logan_encode_close returned: %d", close_ret);
        }
    } else {
        blog(LOG_INFO, "[obs-netint-t4xx] Skipping encode_close (p_session_ctx=%p)", 
             ctx->enc.p_session_ctx);
        /* NOTE: Do NOT manually free p_session_ctx or p_encoder_params! */
        /* These were allocated by libxcoder_logan.dll and must be freed by it */
        /* Cross-DLL memory management causes heap corruption on Windows */
    }
    
    /* Free device name strings (allocated by us with bstrdup) */
    /* These are safe to free - we allocated them before calling library functions */
    if (ctx->enc.dev_enc_name) {
        bfree(ctx->enc.dev_enc_name);
        ctx->enc.dev_enc_name = NULL;
    }
    if (ctx->enc.dev_xcoder) {
        bfree(ctx->enc.dev_xcoder);
        ctx->enc.dev_xcoder = NULL;
    }
    
    /* Free extradata (SPS/PPS headers) - allocated by us */
    if (ctx->extra) {
        bfree(ctx->extra);
        ctx->extra = NULL;
        ctx->extra_size = 0;
    }
    
    /* Free configuration strings - allocated by us */
    if (ctx->rc_mode) bfree(ctx->rc_mode);
    if (ctx->profile) bfree(ctx->profile);
    if (ctx->gop_preset) bfree(ctx->gop_preset);
    
    /* enc is EMBEDDED in ctx, so it will be freed when ctx is freed */
    /* No need to manually free it */
    
#ifdef DEBUG_NETINT_PLUGIN
    /* Clear magic for use-after-free detection */
    ctx->debug_magic = 0xFEEDFACE;  /* Freed marker */
    blog(LOG_INFO, "[obs-netint-t4xx] Context %p marked as freed", ctx);
#endif
    
    /* Free context structure itself */
    bfree(ctx);
}

/**
 * @brief Update encoder settings (called when user changes configuration)
 * 
 * Currently, this function returns false because the NETINT encoder doesn't support
 * dynamic parameter changes after initialization. Returning false signals to OBS
 * that it should destroy and recreate the encoder instance with new settings.
 * 
 * This is a limitation of the hardware encoder - parameters must be set at initialization.
 * To change settings, the encoder must be destroyed and recreated with new parameters.
 * 
 * Returning false is the correct approach: it tells OBS that the encoder cannot
 * be updated in place, and OBS will handle the recreation automatically.
 * 
 * Future enhancement: Could implement parameter change support if libxcoder API supports it.
 * 
 * @param data Encoder context (unused - encoder doesn't support updates)
 * @param settings New settings (unused - encoder doesn't support updates)
 * @return false (indicates encoder cannot be updated, OBS will recreate it)
 */
static bool netint_update(void *data, obs_data_t *settings)
{
    UNUSED_PARAMETER(data);
    UNUSED_PARAMETER(settings);
    /* Return false to indicate encoder doesn't support dynamic updates */
    /* OBS will destroy and recreate the encoder with new settings */
    blog(LOG_INFO, "[obs-netint-t4xx] Encoder settings changed - encoder will be recreated");
    return false;
}

/**
 * @brief Specify preferred video format for encoder input
 * 
 * This function tells OBS what pixel format we want frames in.
 * NETINT encoder requires I420 (planar YUV420) format, not NV12.
 * 
 * @param data Encoder context (unused)
 * @param info Video scale info - we set info->format to our preferred format
 */
static void netint_get_video_info(void *data, struct video_scale_info *info)
{
    UNUSED_PARAMETER(data);
    info->format = VIDEO_FORMAT_I420;
}
static void netint_destroy_job_pool(struct netint_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    struct netint_frame_job *head = NULL;

    if (ctx->job_pool_mutex_initialized) {
        pthread_mutex_lock(&ctx->job_pool_mutex);
        head = ctx->job_pool_head;
        ctx->job_pool_head = NULL;
        ctx->job_pool_size = 0;
        pthread_mutex_unlock(&ctx->job_pool_mutex);

        pthread_mutex_destroy(&ctx->job_pool_mutex);
        ctx->job_pool_mutex_initialized = false;
    } else {
        head = ctx->job_pool_head;
        ctx->job_pool_head = NULL;
        ctx->job_pool_size = 0;
    }

    while (head) {
        struct netint_frame_job *next = head->next;
        if (head->buffer) {
            bfree(head->buffer);
        }
        bfree(head);
        head = next;
    }

    ctx->job_pool_capacity = 0;
}

static bool netint_init_job_pool(struct netint_ctx *ctx)
{
    if (!ctx) {
        return false;
    }

    ctx->job_pool_capacity = ctx->max_inflight * 2;
    if (ctx->job_pool_capacity < NETINT_JOB_POOL_MIN_CAPACITY) {
        ctx->job_pool_capacity = NETINT_JOB_POOL_MIN_CAPACITY;
    }

    if (pthread_mutex_init(&ctx->job_pool_mutex, NULL) != 0) {
        blog(LOG_ERROR, "[obs-netint-t4xx] Failed to initialize job pool mutex");
        ctx->job_pool_mutex_initialized = false;
        return false;
    }

    ctx->job_pool_mutex_initialized = true;
    ctx->job_pool_head = NULL;
    ctx->job_pool_size = 0;

    for (int i = 0; i < ctx->job_pool_capacity; i++) {
        struct netint_frame_job *job = bzalloc(sizeof(*job));
        if (!job) {
            blog(LOG_ERROR, "[obs-netint-t4xx] Failed to allocate frame job for pool");
            netint_destroy_job_pool(ctx);
            return false;
        }

        job->from_pool = true;
        job->buffer_capacity = ctx->hw_frame_size;
        job->buffer_size = ctx->hw_frame_size;

        if (ctx->hw_frame_size > 0) {
            job->buffer = bmalloc(ctx->hw_frame_size);
            if (!job->buffer) {
                blog(LOG_ERROR, "[obs-netint-t4xx] Failed to allocate %zu-byte buffer for job pool entry",
                     ctx->hw_frame_size);
                bfree(job);
                netint_destroy_job_pool(ctx);
                return false;
            }
        } else {
            job->buffer = NULL;
        }

        job->next = ctx->job_pool_head;
        ctx->job_pool_head = job;
        ctx->job_pool_size++;
    }

    blog(LOG_INFO, "[obs-netint-t4xx] Initialized frame job pool (capacity=%d, frame_size=%zu)",
         ctx->job_pool_capacity, ctx->hw_frame_size);
    return true;
}

static struct netint_frame_job *netint_acquire_job(struct netint_ctx *ctx, bool require_buffer)
{
    struct netint_frame_job *job = NULL;

    if (ctx->job_pool_mutex_initialized) {
        pthread_mutex_lock(&ctx->job_pool_mutex);
        if (ctx->job_pool_head) {
            job = ctx->job_pool_head;
            ctx->job_pool_head = job->next;
            ctx->job_pool_size--;
        }
        pthread_mutex_unlock(&ctx->job_pool_mutex);
    }

    if (job) {
        job->next = NULL;
        job->buffer_size = ctx->hw_frame_size;
        job->from_pool = true;

        if (require_buffer && ctx->hw_frame_size > 0) {
            if (job->buffer_capacity < ctx->hw_frame_size) {
                uint8_t *new_buffer = (uint8_t *)brealloc(job->buffer, ctx->hw_frame_size);
                if (!new_buffer) {
                    blog(LOG_ERROR, "[obs-netint-t4xx] Failed to grow pooled frame buffer to %zu bytes",
                         ctx->hw_frame_size);
                    job->next = NULL;
                    job->buffer_size = job->buffer_capacity;
                    netint_release_job(ctx, job);
                    return NULL;
                }
                job->buffer = new_buffer;
                job->buffer_capacity = ctx->hw_frame_size;
            }
            if (!job->buffer) {
                job->buffer = bmalloc(ctx->hw_frame_size);
                if (!job->buffer) {
                    blog(LOG_ERROR, "[obs-netint-t4xx] Failed to allocate pooled frame buffer (%zu bytes)",
                         ctx->hw_frame_size);
                    netint_release_job(ctx, job);
                    return NULL;
                }
                job->buffer_capacity = ctx->hw_frame_size;
            }
        }

        return job;
    }

    job = bzalloc(sizeof(*job));
    if (!job) {
        blog(LOG_ERROR, "[obs-netint-t4xx] Failed to allocate frame job (pool empty)");
        return NULL;
    }

    job->from_pool = false;
    job->buffer_capacity = ctx->hw_frame_size;
    job->buffer_size = ctx->hw_frame_size;

    if (require_buffer && ctx->hw_frame_size > 0) {
        job->buffer = bmalloc(ctx->hw_frame_size);
        if (!job->buffer) {
            blog(LOG_ERROR, "[obs-netint-t4xx] Failed to allocate %zu-byte frame buffer (pool empty)",
                 ctx->hw_frame_size);
            bfree(job);
            return NULL;
        }
    }

    return job;
}

static void netint_release_job(struct netint_ctx *ctx, struct netint_frame_job *job)
{
    if (!job) {
        return;
    }

    if (job->from_pool && ctx->job_pool_mutex_initialized) {
        job->pts = 0;
        job->start_of_stream = false;
        job->end_of_stream = false;
        job->buffer_size = ctx->hw_frame_size;
        job->next = NULL;

        pthread_mutex_lock(&ctx->job_pool_mutex);
        if (ctx->job_pool_size < ctx->job_pool_capacity) {
            job->next = ctx->job_pool_head;
            ctx->job_pool_head = job;
            ctx->job_pool_size++;
            pthread_mutex_unlock(&ctx->job_pool_mutex);
            return;
        }
        pthread_mutex_unlock(&ctx->job_pool_mutex);
        /* Pool full - fall through to free */
    }

    if (job->buffer) {
        bfree(job->buffer);
        job->buffer = NULL;
    }

    bfree(job);
}

static void netint_enqueue_job(struct netint_ctx *ctx, struct netint_frame_job *job, bool count_frame)
{
    pthread_mutex_lock(&ctx->frame_queue_mutex);

    if (count_frame && ctx->frames_submitted == 0) {
        job->start_of_stream = true;
    }

    if (count_frame) {
        ctx->frames_submitted++;
    }

    job->next = NULL;

    if (!ctx->frame_queue_head) {
        ctx->frame_queue_head = job;
        ctx->frame_queue_tail = job;
    } else {
        ctx->frame_queue_tail->next = job;
        ctx->frame_queue_tail = job;
    }

    ctx->pending_jobs++;
    pthread_cond_signal(&ctx->frame_queue_cond);
    pthread_mutex_unlock(&ctx->frame_queue_mutex);
}

static struct netint_frame_job *netint_dequeue_job(struct netint_ctx *ctx, bool wait_for_job)
{
    struct netint_frame_job *job = NULL;

    pthread_mutex_lock(&ctx->frame_queue_mutex);
    while (wait_for_job && !ctx->stop_thread && !ctx->frame_queue_head) {
        pthread_cond_wait(&ctx->frame_queue_cond, &ctx->frame_queue_mutex);
    }

    if (ctx->frame_queue_head) {
        job = ctx->frame_queue_head;
        ctx->frame_queue_head = job->next;
        if (!ctx->frame_queue_head) {
            ctx->frame_queue_tail = NULL;
        }
        job->next = NULL;
        ctx->pending_jobs--;
    }
    pthread_mutex_unlock(&ctx->frame_queue_mutex);

    return job;
}

static bool netint_queue_frame(struct netint_ctx *ctx, struct encoder_frame *frame)
{
    struct netint_frame_job *job = netint_acquire_job(ctx, true);
    if (!job) {
        blog(LOG_ERROR, "[obs-netint-t4xx] Failed to acquire frame job");
        return false;
    }

    job->buffer_size = ctx->hw_frame_size;
    job->pts = frame->pts;
    job->end_of_stream = false;
    job->start_of_stream = false;

    if (ctx->hw_frame_size > 0) {
        if (!job->buffer) {
            blog(LOG_ERROR, "[obs-netint-t4xx] Frame job missing buffer (%zu bytes expected)", ctx->hw_frame_size);
            netint_release_job(ctx, job);
            return false;
        }
        uint8_t *dest_planes[NI_LOGAN_MAX_NUM_DATA_POINTERS] = {0};
        for (int i = 0; i < NI_LOGAN_MAX_NUM_DATA_POINTERS; i++) {
            if (ctx->hw_plane_size[i] > 0) {
                dest_planes[i] = job->buffer + ctx->hw_plane_offset[i];
            }
        }

        uint8_t *src_planes[NI_LOGAN_MAX_NUM_DATA_POINTERS] = {
            frame->data[0],
            frame->data[1],
            frame->data[2],
            NULL};

        int src_stride[NI_LOGAN_MAX_NUM_DATA_POINTERS] = {
            frame->linesize[0],
            frame->linesize[1],
            frame->linesize[2],
            0};

        int src_height[NI_LOGAN_MAX_NUM_DATA_POINTERS] = {
            ctx->enc.height,
            ctx->enc.height / 2,
            ctx->enc.height / 2,
            0};

        p_ni_logan_copy_hw_yuv420p(dest_planes,
                                   src_planes,
                                   ctx->enc.width,
                                   ctx->enc.height,
                                   1,
                                   ctx->hw_stride,
                                   ctx->hw_height,
                                   src_stride,
                                   src_height);
    }

    netint_enqueue_job(ctx, job, true);
    return true;
}

static bool netint_queue_eos(struct netint_ctx *ctx)
{
    struct netint_frame_job *job = netint_acquire_job(ctx, false);
    if (!job) {
        job = bzalloc(sizeof(*job));
        if (!job) {
            blog(LOG_ERROR, "[obs-netint-t4xx] Failed to allocate EOS job");
            return false;
        }
    }

    job->buffer_size = 0;
    job->pts = 0;
    job->end_of_stream = true;
    job->start_of_stream = false;

    netint_enqueue_job(ctx, job, false);
    return true;
}

static bool netint_hw_send_job(struct netint_ctx *ctx, struct netint_frame_job *job)
{
    bool success = false;
    bool allocated_buffer = false;

    pthread_mutex_lock(&ctx->io_mutex);

    int get_ret = p_ni_logan_encode_get_frame(&ctx->enc);
    if (get_ret < 0) {
        blog(LOG_ERROR, "[obs-netint-t4xx] ni_logan_encode_get_frame failed (ret=%d)", get_ret);
        netint_log_error(ctx, "ni_logan_encode_get_frame", get_ret);
        goto done;
    }

    ni_logan_session_data_io_t *input_fme = ctx->enc.p_input_fme;
    if (!input_fme) {
        blog(LOG_ERROR, "[obs-netint-t4xx] p_input_fme is NULL after encode_get_frame");
        goto done;
    }

    ni_logan_frame_t *ni_frame = &input_fme->data.frame;
    ni_frame->extra_data_len = 64;

    int alloc_ret = p_ni_logan_encoder_frame_buffer_alloc(
        ni_frame,
        ctx->enc.width,
        ctx->enc.height,
        ctx->hw_stride,
        (ctx->codec_type == 0) ? 1 : 0,
        ni_frame->extra_data_len,
        1);

    if (alloc_ret != NI_LOGAN_RETCODE_SUCCESS) {
        blog(LOG_ERROR, "[obs-netint-t4xx] Failed to allocate frame buffer (ret=%d)", alloc_ret);
        netint_log_error(ctx, "ni_logan_encoder_frame_buffer_alloc", alloc_ret);
        goto done;
    }
    allocated_buffer = true;

    if (!job->end_of_stream && job->buffer && ctx->hw_frame_size > 0) {
        size_t offset = 0;
        for (int i = 0; i < NI_LOGAN_MAX_NUM_DATA_POINTERS; i++) {
            if (ctx->hw_plane_size[i] > 0 && ni_frame->p_data[i]) {
                memcpy(ni_frame->p_data[i], job->buffer + offset, ctx->hw_plane_size[i]);
                offset += ctx->hw_plane_size[i];
            } else if (ni_frame->p_data[i] && ctx->hw_plane_size[i] == 0) {
                /* Plane exists but empty - zero it */
                size_t plane_stride = (size_t)ctx->hw_stride[i];
                size_t plane_height = (size_t)ctx->hw_height[i];
                if (plane_stride > 0 && plane_height > 0) {
                    memset(ni_frame->p_data[i], 0, plane_stride * plane_height);
                }
            }
        }
    } else if (allocated_buffer) {
        for (int i = 0; i < NI_LOGAN_MAX_NUM_DATA_POINTERS; i++) {
            if (ni_frame->p_data[i] && ctx->hw_plane_size[i] > 0) {
                memset(ni_frame->p_data[i], 0, ctx->hw_plane_size[i]);
            }
        }
    }

    ni_frame->video_width = ctx->enc.width;
    ni_frame->video_height = ctx->enc.height;
    ni_frame->video_orig_width = ctx->enc.width;
    ni_frame->video_orig_height = ctx->enc.height;
    ni_frame->pts = job->end_of_stream ? 0 : job->pts;
    ni_frame->dts = ni_frame->pts;
    ni_frame->start_of_stream = job->start_of_stream ? 1 : 0;
    ni_frame->end_of_stream = job->end_of_stream ? 1 : 0;
    ni_frame->force_key_frame = job->start_of_stream ? 1 : 0;
    ni_frame->ni_logan_pict_type = job->start_of_stream ? LOGAN_PIC_TYPE_IDR : 0;
    ni_frame->bit_depth = 8;
    ni_frame->color_primaries = (uint8_t)ctx->enc.color_primaries;
    ni_frame->color_trc = (uint8_t)ctx->enc.color_trc;
    ni_frame->color_space = (uint8_t)ctx->enc.color_space;
    ni_frame->video_full_range_flag = ctx->enc.color_range;

    int send_ret = p_ni_logan_encode_send(&ctx->enc);
    if (send_ret < 0) {
        blog(LOG_ERROR, "[obs-netint-t4xx] ni_logan_encode_send failed (ret=%d)", send_ret);
        netint_log_error(ctx, "ni_logan_encode_send", send_ret);
        goto done;
    }

    if (!ctx->enc.started) {
        ctx->enc.started = 1;
        blog(LOG_INFO, "[obs-netint-t4xx] Encoder marked as started (ni_logan_encode_send success)");
    }

    if (!job->end_of_stream) {
        ctx->frame_count++;

    }

    ctx->consecutive_errors = 0;
    success = true;

done:
    if (allocated_buffer) {
        p_ni_logan_frame_buffer_free(&ctx->enc.p_input_fme->data.frame);
    }
    pthread_mutex_unlock(&ctx->io_mutex);
    return success;
}

static bool netint_hw_receive_once(struct netint_ctx *ctx)
{
    uint8_t *copied_data = NULL;
    size_t copied_size = 0;
    int64_t pkt_pts = 0;
    int64_t pkt_dts = 0;
    bool pkt_keyframe = false;
    bool got_packet = false;

    pthread_mutex_lock(&ctx->io_mutex);
    int recv_size = p_ni_logan_encode_receive(&ctx->enc);

    if (recv_size > 0) {
        ni_logan_packet_t *ni_pkt = &ctx->enc.output_pkt.data.packet;
        int packet_size = recv_size;
        if (ctx->enc.spsPpsAttach && ctx->enc.p_spsPpsHdr && ctx->enc.spsPpsHdrLen > 0) {
            packet_size += ctx->enc.spsPpsHdrLen;
        }

        copied_data = bmalloc((size_t)packet_size);
        if (!copied_data) {
            blog(LOG_ERROR, "[obs-netint-t4xx] [IO THREAD] Failed to allocate packet buffer (%d bytes)", packet_size);
            netint_log_error(ctx, "packet_buffer_alloc", -ENOMEM);
        } else {
            int first_packet_flag = ctx->enc.firstPktArrived ? 0 : 1;
            int copy_ret = p_ni_logan_encode_copy_packet_data(&ctx->enc,
                                                              copied_data,
                                                              first_packet_flag,
                                                              ctx->enc.spsPpsAttach);
            if (copy_ret < 0) {
                blog(LOG_ERROR, "[obs-netint-t4xx] [IO THREAD] encode_copy_packet_data failed (ret=%d)", copy_ret);
                netint_log_error(ctx, "ni_logan_encode_copy_packet_data", copy_ret);
                bfree(copied_data);
                copied_data = NULL;
            } else {
                copied_size = (size_t)packet_size;

                if (!ctx->got_headers && ctx->enc.p_spsPpsHdr && ctx->enc.spsPpsHdrLen > 0) {
                    if (ctx->extra) bfree(ctx->extra);
                    ctx->extra = bmemdup(ctx->enc.p_spsPpsHdr, (size_t)ctx->enc.spsPpsHdrLen);
                    ctx->extra_size = (size_t)ctx->enc.spsPpsHdrLen;
                    ctx->got_headers = true;
                    blog(LOG_INFO, "[obs-netint-t4xx] [IO THREAD] Stored SPS/PPS extradata (%zu bytes)", ctx->extra_size);
                }

                pkt_pts = ni_pkt->pts;
                pkt_dts = ni_pkt->dts;
                if (pkt_pts == 0 && ctx->enc.latest_dts != 0) {
                    pkt_pts = ctx->enc.latest_dts;
                    pkt_dts = pkt_pts;
                }

                if (ctx->codec_type == 1) {
                    pkt_keyframe = obs_hevc_keyframe(copied_data, copied_size);
                } else {
                    pkt_keyframe = obs_avc_keyframe(copied_data, copied_size);
                }

                ctx->enc.encoder_eof = ni_pkt->end_of_stream;
                ctx->enc.firstPktArrived = 1;
                got_packet = true;
            }
        }
    } else if (recv_size < 0) {
        if (ctx->enc.encoder_eof) {
            pthread_mutex_unlock(&ctx->io_mutex);
            return false;
        }
    }

    pthread_mutex_unlock(&ctx->io_mutex);

    if (!got_packet || !copied_data) {
        if (copied_data) bfree(copied_data);
        return false;
    }

    struct netint_pkt *pkt = bzalloc(sizeof(*pkt));
    if (!pkt) {
        blog(LOG_ERROR, "[obs-netint-t4xx] [IO THREAD] Failed to allocate queue packet");
        bfree(copied_data);
        return false;
    }

    pkt->data = copied_data;
    pkt->size = copied_size;
    pkt->pts = pkt_pts;
    pkt->dts = pkt_dts;
    pkt->keyframe = pkt_keyframe;
    pkt->priority = 0;

    pthread_mutex_lock(&ctx->queue_mutex);
    pkt->next = NULL;
    if (ctx->pkt_queue_tail) {
        ctx->pkt_queue_tail->next = pkt;
        ctx->pkt_queue_tail = pkt;
    } else {
        ctx->pkt_queue_head = pkt;
        ctx->pkt_queue_tail = pkt;
    }
    pthread_mutex_unlock(&ctx->queue_mutex);

    pthread_mutex_lock(&ctx->frame_queue_mutex);
    if (ctx->inflight_frames > 0) {
        ctx->inflight_frames--;
    }
    pthread_mutex_unlock(&ctx->frame_queue_mutex);

    ctx->consecutive_errors = 0;
    return true;
}

static void netint_hw_drain(struct netint_ctx *ctx, bool drain_all)
{
    int drained = 0;
    while (netint_hw_receive_once(ctx)) {
        drained++;
        if (!drain_all && drained >= 1) {
            break;
        }
    }
}

static void *netint_io_thread(void *data)
{
    struct netint_ctx *ctx = data;
    blog(LOG_INFO, "[obs-netint-t4xx] IO thread started (pipelined send/receive)");

    while (true) {
        struct netint_frame_job *job = netint_dequeue_job(ctx, true);

        if (!job) {
            if (ctx->stop_thread) {
                break;
            }

            netint_hw_drain(ctx, false);
            continue;
        }

        if (!netint_hw_send_job(ctx, job)) {
            netint_release_job(ctx, job);
            continue;
        }

        if (!job->end_of_stream) {
            pthread_mutex_lock(&ctx->frame_queue_mutex);
            ctx->inflight_frames++;
            pthread_mutex_unlock(&ctx->frame_queue_mutex);
        }

        netint_release_job(ctx, job);

        pthread_mutex_lock(&ctx->frame_queue_mutex);
        int inflight = ctx->inflight_frames;
        pthread_mutex_unlock(&ctx->frame_queue_mutex);

        if (ctx->max_inflight > 0 && inflight >= ctx->max_inflight) {
            netint_hw_drain(ctx, true);
        } else {
            netint_hw_drain(ctx, false);
        }
    }

    /* Final drain to ensure packets (including EOS) are delivered */
    netint_hw_drain(ctx, true);

    blog(LOG_INFO, "[obs-netint-t4xx] IO thread exiting");
    return NULL;
}

static bool netint_encode(void *data, struct encoder_frame *frame, struct encoder_packet *packet, bool *received)
{
    struct netint_ctx *ctx = data;
    *received = false;

#ifdef DEBUG_NETINT_PLUGIN
    if (ctx->debug_magic != NETINT_ENC_CONTEXT_MAGIC) {
        blog(LOG_ERROR, "[DEBUG] Invalid context magic in netint_encode: 0x%08X (expected 0x%08X)",
             ctx->debug_magic, NETINT_ENC_CONTEXT_MAGIC);
        NETINT_DEBUGBREAK();
        return false;
    }
#endif

    NETINT_VALIDATE_ENC_CONTEXT(ctx, "netint_encode entry");

    /* Check for packets produced by IO thread */
    pthread_mutex_lock(&ctx->queue_mutex);
    struct netint_pkt *pkt = ctx->pkt_queue_head;
    if (pkt) {
        ctx->pkt_queue_head = pkt->next;
        if (!ctx->pkt_queue_head) {
            ctx->pkt_queue_tail = NULL;
        }
    }
    pthread_mutex_unlock(&ctx->queue_mutex);

    bool delivered_packet = false;
    if (pkt) {
        packet->data = pkt->data;
        packet->size = pkt->size;
        packet->pts = pkt->pts;
        packet->dts = pkt->dts;
        packet->keyframe = pkt->keyframe;
        packet->type = OBS_ENCODER_VIDEO;

        video_t *video = obs_encoder_video(ctx->encoder);
        const struct video_output_info *voi = video_output_get_info(video);
        packet->timebase_num = (int32_t)voi->fps_den;
        packet->timebase_den = (int32_t)voi->fps_num;

        if (ctx->codec_type == 1) {
            packet->priority = obs_parse_hevc_packet_priority(packet);
        } else {
            packet->priority = obs_parse_avc_packet_priority(packet);
        }

        bfree(pkt);
        *received = true;
        delivered_packet = true;
    }

    if (!frame) {
        if (!ctx->flushing) {
            blog(LOG_INFO, "[obs-netint-t4xx] Queueing EOS frame");
            if (!netint_queue_eos(ctx)) {
                return false;
            }
            ctx->flushing = true;
        }

        return true;
    }

    if (!netint_queue_frame(ctx, frame)) {
        return false;
    }

    if (!delivered_packet) {
        *received = false;
    }
    return true;
}
/**
 * @brief Set default settings for H.264 encoder
 */
static void netint_h264_get_defaults(obs_data_t *settings)
{
    /* Default bitrate: 6000 kbps (good for 1080p streaming) */
    obs_data_set_default_int(settings, "bitrate", 6000);
    
    /* Default keyframe interval: 2 seconds (auto-calculated from FPS if <= 0) */
    obs_data_set_default_int(settings, "keyint", 2);
    
    /* Default rate control: CBR (constant bitrate) - better for streaming */
    obs_data_set_default_string(settings, "rc_mode", "CBR");

    /* Default QP when rate control is disabled */
    obs_data_set_default_int(settings, "qp", 22);

    /* Lossless disabled by default */
    obs_data_set_default_bool(settings, "lossless", false);
    
    /* Default profile: high (best quality for H.264) */
    obs_data_set_default_string(settings, "profile", "high");
    
    /* Default GOP preset: default (I-B-B-B-P pattern with B-frames for best quality) */
    obs_data_set_default_string(settings, "gop_preset", "default");
    
    /* Default repeat headers: true (attach SPS/PPS to every keyframe) */
    /* This is important for streaming where clients may join mid-stream */
    obs_data_set_default_bool(settings, "repeat_headers", true);
}

/**
 * @brief Set default settings for H.265 encoder
 */
static void netint_h265_get_defaults(obs_data_t *settings)
{
    /* Default bitrate: 6000 kbps (good for 1080p streaming) */
    obs_data_set_default_int(settings, "bitrate", 6000);
    
    /* Default keyframe interval: 2 seconds (auto-calculated from FPS if <= 0) */
    obs_data_set_default_int(settings, "keyint", 2);
    
    /* Default rate control: CBR (constant bitrate) - better for streaming */
    obs_data_set_default_string(settings, "rc_mode", "CBR");

    /* Default QP when rate control is disabled */
    obs_data_set_default_int(settings, "qp", 22);

    /* Lossless disabled by default */
    obs_data_set_default_bool(settings, "lossless", false);
    
    /* Default profile: main (H.265 only supports main and main10) */
    obs_data_set_default_string(settings, "profile", "main");
    
    /* Default GOP preset: default (I-B-B-B-P pattern with B-frames for best quality) */
    obs_data_set_default_string(settings, "gop_preset", "default");
    
    /* Default repeat headers: true (attach SPS/PPS to every keyframe) */
    /* This is important for streaming where clients may join mid-stream */
    obs_data_set_default_bool(settings, "repeat_headers", true);
}

/**
 * @brief Create properties UI for H.264 encoder settings
 * 
 * H.264-specific profiles: baseline, main, high
 */
static obs_properties_t *netint_h264_get_properties(void *data)
{
    UNUSED_PARAMETER(data);
    obs_properties_t *props = obs_properties_create();
    
    /* Bitrate input: 100-100000 kbps, step size 50 kbps */
    obs_properties_add_int(props, "bitrate", "Bitrate (kbps)", 100, 100000, 50);
    
    /* Keyframe interval: 1-20 seconds, step size 1 second */
    obs_properties_add_int(props, "keyint", "Keyframe Interval (s)", 1, 20, 1);
    
    /* Device name: text input (will be converted to dropdown if discovery works) */
    obs_properties_add_text(props, "device", "Device Name (optional)", OBS_TEXT_DEFAULT);
    
    /* Rate control mode: CBR (constant) or VBR (variable) */
    obs_property_t *rc = obs_properties_add_list(props, "rc_mode", "Rate Control", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(rc, "CBR", "CBR");
    obs_property_list_add_string(rc, "VBR", "VBR");
    obs_property_list_add_string(rc, "Disabled (Constant QP)", "DISABLED");

    obs_properties_add_int(props, "qp", "QP (RC Disabled)", 0, 51, 1);
    
    /* H.264 Profile selection: baseline, main, or high */
    obs_property_t *prof = obs_properties_add_list(props, "profile", "Profile", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(prof, "baseline", "Baseline");
    obs_property_list_add_string(prof, "main", "Main");
    obs_property_list_add_string(prof, "high", "High");
    
    /* GOP preset selection: controls compression vs latency tradeoff */
    obs_property_t *gop = obs_properties_add_list(props, "gop_preset", "GOP Preset", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(gop, "Default (I-B-B-B-P) - Best Quality", "default");
    obs_property_list_add_string(gop, "Simple (I-P-P-P) - Lower Latency", "simple");
    obs_property_set_long_description(gop, 
        "GOP structure controls compression efficiency:\n"
        "• Default: Uses B-frames for best quality and compression\n"
        "• Simple: No B-frames, lower latency but larger file size");
    
    /* Repeat headers checkbox: attach SPS/PPS to every keyframe */
    obs_properties_add_bool(props, "repeat_headers", "Repeat SPS/PPS on Keyframes");

    /* Populate device list if discovery APIs are available */
    if (p_ni_logan_rsrc_init && p_ni_logan_rsrc_get_local_device_list) {
        int rsrc_ret = p_ni_logan_rsrc_init(0, 1);
        if (rsrc_ret == 0 || rsrc_ret == 0x7FFFFFFF) {
            char names[16][NI_LOGAN_MAX_DEVICE_NAME_LEN] = {0};
            int n = p_ni_logan_rsrc_get_local_device_list(names, 16);
            if (n > 0) {
                obs_property_t *dev = obs_properties_get(props, "device");
                if (dev) {
                    obs_property_set_long_description(dev, "Device Name");
                } else {
                    dev = obs_properties_add_list(props, "device", "Device", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
                }
                for (int i = 0; i < n; i++) {
                    obs_property_list_add_string(dev, names[i], names[i]);
                }
            }
        }
    }
    return props;
}

/**
 * @brief Create properties UI for H.265 encoder settings
 * 
 * H.265-specific profiles: main (8-bit), main10 (10-bit)
 */
static obs_properties_t *netint_h265_get_properties(void *data)
{
    UNUSED_PARAMETER(data);
    obs_properties_t *props = obs_properties_create();
    
    /* Bitrate input: 100-100000 kbps, step size 50 kbps */
    obs_properties_add_int(props, "bitrate", "Bitrate (kbps)", 100, 100000, 50);
    
    /* Keyframe interval: 1-20 seconds, step size 1 second */
    obs_properties_add_int(props, "keyint", "Keyframe Interval (s)", 1, 20, 1);
    
    /* Device name: text input (will be converted to dropdown if discovery works) */
    obs_properties_add_text(props, "device", "Device Name (optional)", OBS_TEXT_DEFAULT);
    
    /* Rate control mode: CBR (constant) or VBR (variable) */
    obs_property_t *rc = obs_properties_add_list(props, "rc_mode", "Rate Control", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(rc, "CBR", "CBR");
    obs_property_list_add_string(rc, "VBR", "VBR");
    obs_property_list_add_string(rc, "Disabled (Constant QP)", "DISABLED");

    obs_properties_add_int(props, "qp", "QP (RC Disabled)", 0, 51, 1);
    obs_properties_add_bool(props, "lossless", "Lossless (HEVC only, requires RC Disabled)");
    
    /* H.265 Profile selection: main or main10 ONLY */
    obs_property_t *prof = obs_properties_add_list(props, "profile", "Profile", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(prof, "main", "Main (8-bit)");
    obs_property_list_add_string(prof, "main10", "Main10 (10-bit)");
    obs_property_set_long_description(prof,
        "H.265 profiles supported by NetInt T408:\n"
        "• Main: 8-bit encoding (recommended for most uses)\n"
        "• Main10: 10-bit encoding (higher quality, larger files)");
    
    /* GOP preset selection: controls compression vs latency tradeoff */
    obs_property_t *gop = obs_properties_add_list(props, "gop_preset", "GOP Preset", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(gop, "Default (I-B-B-B-P) - Best Quality", "default");
    obs_property_list_add_string(gop, "Simple (I-P-P-P) - Lower Latency", "simple");
    obs_property_set_long_description(gop, 
        "GOP structure controls compression efficiency:\n"
        "• Default: Uses B-frames for best quality and compression\n"
        "• Simple: No B-frames, lower latency but larger file size");
    
    /* Repeat headers checkbox: attach SPS/PPS to every keyframe */
    obs_properties_add_bool(props, "repeat_headers", "Repeat VPS/SPS/PPS on Keyframes");

    /* Populate device list if discovery APIs are available */
    if (p_ni_logan_rsrc_init && p_ni_logan_rsrc_get_local_device_list) {
        int rsrc_ret = p_ni_logan_rsrc_init(0, 1);
        if (rsrc_ret == 0 || rsrc_ret == 0x7FFFFFFF) {
            char names[16][NI_LOGAN_MAX_DEVICE_NAME_LEN] = {0};
            int n = p_ni_logan_rsrc_get_local_device_list(names, 16);
            if (n > 0) {
                obs_property_t *dev = obs_properties_get(props, "device");
                if (dev) {
                    obs_property_set_long_description(dev, "Device Name");
                } else {
                    dev = obs_properties_add_list(props, "device", "Device", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
                }
                for (int i = 0; i < n; i++) {
                    obs_property_list_add_string(dev, names[i], names[i]);
                }
            }
        }
    }
    return props;
}

/**
 * @brief Get encoder extradata (SPS/PPS headers) for stream initialization
 * 
 * This function is called by OBS Studio when initializing a stream or recording.
 * The extradata contains codec-specific configuration headers:
 * - H.264: SPS (Sequence Parameter Set) and PPS (Picture Parameter Set)
 * - H.265: VPS (Video Parameter Set), SPS, and PPS
 * 
 * These headers are required by decoders to initialize and start decoding the stream.
 * They contain information about:
 * - Video resolution
 * - Frame rate
 * - Color space
 * - Profile/level
 * - Other codec-specific parameters
 * 
 * The headers are generated by the encoder during initialization (netint_create)
 * and stored in ctx->extra. They are returned to OBS without copying (OBS manages
 * the lifetime).
 * 
 * @param data Encoder context pointer
 * @param extra_data Output: pointer to extradata buffer
 * @param size Output: size of extradata in bytes
 * @return true if extradata is available, false otherwise
 */
static bool netint_get_extra_data(void *data, uint8_t **extra_data, size_t *size)
{
    struct netint_ctx *ctx = data;
    
    blog(LOG_INFO, "[obs-netint-t4xx] ▶ get_extra_data() called, got_headers=%d", ctx->got_headers);
    
    /* If headers not available yet, wait for them (up to 5 seconds) */
    /* This handles the case where headers are extracted from the first packet */
    if (!ctx->got_headers) {
        blog(LOG_INFO, "[obs-netint-t4xx] Headers not yet available, waiting for first packet...");
        
        /* Wait up to 5 seconds (50 x 100ms) for headers to be extracted */
        for (int i = 0; i < 50; i++) {
            os_sleep_ms(100);
            
            if (ctx->got_headers) {
                blog(LOG_INFO, "[obs-netint-t4xx] Headers became available after %d ms", i * 100);
                break;
            }
        }
        
        if (!ctx->got_headers) {
            blog(LOG_ERROR, "[obs-netint-t4xx] Timeout waiting for encoder headers");
            return false;
        }

    }
    
    /* Double-check extradata is valid */
    if (!ctx->extra || !ctx->extra_size) {
        blog(LOG_ERROR, "[obs-netint-t4xx] Headers flag set but extradata is NULL");
        return false;
    }
    
    blog(LOG_INFO, "[obs-netint-t4xx] ✅ get_extra_data() returning headers: %zu bytes", ctx->extra_size);
    
    /* Return extradata pointer and size to OBS */
    /* OBS will copy this data if needed, so we don't need to allocate */
    *extra_data = ctx->extra;
    *size = ctx->extra_size;
    
    blog(LOG_DEBUG, "[obs-netint-t4xx] Returning %zu bytes of header data", *size);
    return true;
}
/**
 * @name Encoder Registration Structures
 * @brief OBS encoder info structures that define the encoder interface
 * 
 * These structures tell OBS Studio how to use this encoder. Each codec (H.264 and H.265)
 * has its own registration so OBS can distinguish between them.
 * 
 * Function Pointers:
 * - get_name: Returns display name shown in UI
 * - create: Called when encoder is created (user selects it)
 * - destroy: Called when encoder is destroyed
 * - update: Called when settings change (returns false - encoder doesn't support updates)
 * - encode: Called for each video frame to encode
 * - get_defaults: Sets default values for settings
 * - get_properties: Creates settings UI
 * - get_extra_data: Returns SPS/PPS headers for stream initialization
 * 
 * Capability Flags (caps):
 * - Currently set to 0 because:
 *   - Not deprecated (OBS_ENCODER_CAP_DEPRECATED)
 *   - Doesn't pass textures directly (OBS_ENCODER_CAP_PASS_TEXTURE)
 *   - Doesn't support dynamic bitrate changes (OBS_ENCODER_CAP_DYN_BITRATE)
 *   - Not an internal OBS encoder (OBS_ENCODER_CAP_INTERNAL)
 *   - Region of Interest not implemented (OBS_ENCODER_CAP_ROI)
 *   - Scaling not implemented (OBS_ENCODER_CAP_SCALING)
 * 
 * Optional Callbacks (explicitly NULL):
 * - get_sei_data: Not implemented (returns NULL)
 * - get_video_info: Not implemented (returns NULL)
 * 
 * These are set to NULL explicitly to ensure forward compatibility if OBS adds
 * new optional callbacks to the structure in future versions.
 */
/*@{*/
/** H.264 encoder registration - appears as "NETINT T4XX H.264" in encoder list */
static struct obs_encoder_info netint_h264_info = {
    .id = "obs_netint_t4xx_h264",      /**< Unique identifier for this encoder */
    .codec = "h264",                   /**< Codec string (matches OBS codec type) */
    .type = OBS_ENCODER_VIDEO,        /**< Encoder type: video encoder */
    .caps = 0,                         /**< Capability flags - see documentation above */
    .get_name = netint_h264_get_name,  /**< Returns "NETINT T4XX H.264" */
    .create = netint_create,
    .destroy = netint_destroy,
    .update = netint_update,
    .encode = netint_encode,
    .get_defaults = netint_h264_get_defaults,
    .get_properties = netint_h264_get_properties,
    .get_extra_data = netint_get_extra_data,
    /* Optional callbacks - explicitly NULL for forward compatibility */
    .get_sei_data = NULL,              /**< SEI data not provided (NULL callback) */
    .get_audio_info = NULL,            /**< Audio info not applicable (video encoder only) */
    .get_video_info = netint_get_video_info,  /**< Request I420 format from OBS */
};

/** H.265 (HEVC) encoder registration - appears as "NETINT T4XX H.265" in encoder list */
static struct obs_encoder_info netint_h265_info = {
    .id = "obs_netint_t4xx_h265",      /**< Unique identifier for this encoder */
    .codec = "hevc",                   /**< Codec string (OBS uses "hevc" for H.265) */
    .type = OBS_ENCODER_VIDEO,        /**< Encoder type: video encoder */
    .caps = 0,                         /**< Capability flags - see documentation above */
    .get_name = netint_h265_get_name,  /**< Returns "NETINT T4XX H.265" */
    .create = netint_create,
    .destroy = netint_destroy,
    .update = netint_update,
    .encode = netint_encode,
    .get_defaults = netint_h265_get_defaults,
    .get_properties = netint_h265_get_properties,
    .get_extra_data = netint_get_extra_data,
    /* Optional callbacks - explicitly NULL for forward compatibility */
    .get_sei_data = NULL,              /**< SEI data not provided (NULL callback) */
    .get_audio_info = NULL,            /**< Audio info not applicable (video encoder only) */
    .get_video_info = netint_get_video_info,  /**< Request I420 format from OBS */
};
/*@}*/

/**
 * @brief Initialize the libxcoder library loader
 * 
 * This is a wrapper function that calls the library loader. It's called from
 * the main plugin module (obs-netint.c) during plugin initialization.
 * 
 * @return true if library loaded successfully, false otherwise
 */
bool netint_loader_init(void)
{
    return ni_libxcoder_open();
}

/**
 * @brief Deinitialize the libxcoder library loader
 * 
 * This is a wrapper function that closes the library. It's called from
 * the main plugin module (obs-netint.c) during plugin unload.
 */
void netint_loader_deinit(void)
{
    ni_libxcoder_close();
}

/**
 * @brief Register both H.264 and H.265 encoders with OBS Studio
 * 
 * This function is called during plugin initialization to make the encoders
 * available in OBS Studio's encoder selection UI. Both encoders are registered
 * so users can choose either H.264 or H.265 encoding.
 * 
 * Registration happens even if the library isn't loaded - the encoders will
 * appear in the UI but will fail to create if the library is missing.
 */
void netint_register_encoders(void)
{
    obs_register_encoder(&netint_h264_info);
    obs_register_encoder(&netint_h265_info);
}






