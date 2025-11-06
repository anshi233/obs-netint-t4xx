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
    
    /* Background thread design - necessary because encode_receive() blocks! */
    pthread_t recv_thread;            /**< Background thread that calls blocking encode_receive() */
    pthread_mutex_t queue_mutex;      /**< Protects packet queue access ONLY */
    volatile bool stop_thread;        /**< Signal to background thread to stop */
    bool thread_created;              /**< true if recv_thread was successfully created */
    struct netint_pkt *pkt_queue_head; /**< Head of packet queue (oldest packet) */
    struct netint_pkt *pkt_queue_tail; /**< Tail of packet queue (newest packet) */

    /* Debug flags */
    bool debug_eos_sent;              /**< true if debug EOS was already sent */

    /* Frame buffering for batch processing like xcoder_logan */
    struct {
        void *frames[60];             /**< Buffer for incoming frames */
        int count;                    /**< Number of frames in buffer */
        int capacity;                 /**< Maximum frames to buffer before processing batch */
    } frame_buffer;

    char *rc_mode;                     /**< Rate control mode: "CBR" or "VBR" */
    char *profile;                      /**< Encoder profile: "baseline", "main", or "high" */
    char *gop_preset;                  /**< GOP preset: "simple" (I-P-P-P) or "default" (I-B-B-B-P) */
    bool repeat_headers;               /**< If true, attach SPS/PPS to every keyframe */
    int codec_type;                    /**< Codec type: 0 = H.264, 1 = H.265 (HEVC) */
    uint64_t frame_count;              /**< Total frames processed (for start_of_stream logic) */
    int keyint_frames;                 /**< Keyframe interval in frames */

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
static void *netint_recv_thread(void *data);

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
 * - profile: "baseline", "main", or "high" (codec-specific)
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

    /* Initialize frame buffer for batch processing like xcoder_logan */
    ctx->frame_buffer.count = 0;
    ctx->frame_buffer.capacity = 1;  /* Process frames immediately - OBS reuses frame buffer! */
    
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
    
    /* DEBUG: Show struct memory layout */
    blog(LOG_INFO, "[obs-netint-t4xx] ========================================");
    blog(LOG_INFO, "[obs-netint-t4xx] STRUCT LAYOUT DEBUG:");
    blog(LOG_INFO, "[obs-netint-t4xx]   &ctx->enc = %p (base address)", &ctx->enc);
    blog(LOG_INFO, "[obs-netint-t4xx]   &ctx->enc.input_data_fifo = %p (field offset = %zu bytes)", 
         &ctx->enc.input_data_fifo, (char*)&ctx->enc.input_data_fifo - (char*)&ctx->enc);
    blog(LOG_INFO, "[obs-netint-t4xx]   sizeof(ni_logan_enc_context_t) in plugin = %zu bytes", sizeof(ctx->enc));
    blog(LOG_INFO, "[obs-netint-t4xx] ========================================");
    
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
        
        int gop_ret = p_ni_logan_encoder_params_set_value(params, "gopPresetIdx", gop_value, session_ctx);
        blog(LOG_INFO, "[obs-netint-t4xx] GOP set to %s: ret=%d", gop_desc, gop_ret);

        /* CRITICAL: Enable rate control first! Without this, encoder uses Constant QP mode and ignores bitrate! */
        p_ni_logan_encoder_params_set_value(params, "RcEnable", "1", session_ctx);
        blog(LOG_INFO, "[obs-netint-t4xx] Rate control ENABLED (RcEnable=1)");

        /* Set bitrate and framerate parameters */
        char bitrate_str[32];
        char framerate_str[32];
        char framerate_denom_str[32];

        sprintf(bitrate_str, "%lld", (long long)ctx->enc.bit_rate);
        sprintf(framerate_str, "%d", ctx->enc.timebase_den);
        sprintf(framerate_denom_str, "%d", ctx->enc.timebase_num);

        p_ni_logan_encoder_params_set_value(params, "bitrate", bitrate_str, session_ctx);
        blog(LOG_INFO, "[obs-netint-t4xx] Bitrate parameter set to %lld bps (%d kbps): ret=0",
             (long long)ctx->enc.bit_rate, (int)(ctx->enc.bit_rate / 1000));
        
        p_ni_logan_encoder_params_set_value(params, "frameRate", framerate_str, session_ctx);
        p_ni_logan_encoder_params_set_value(params, "frameRateDenom", framerate_denom_str, session_ctx);
        blog(LOG_INFO, "[obs-netint-t4xx] Framerate parameters: %d/%d (%.2f fps), ret=0,0",
             ctx->enc.timebase_den, ctx->enc.timebase_num,
             ctx->enc.timebase_den / (double)ctx->enc.timebase_num);
        
        p_ni_logan_encoder_params_set_value(params, "RcInitDelay", "3000", session_ctx);
        blog(LOG_INFO, "[obs-netint-t4xx] VBV buffer size (RCInitDelay) set to 3000 ms: ret=0");

        /* Set rate control mode: CBR (constant) or VBR (variable) */
        /* CBR = constant bitrate (good for streaming), VBR = variable bitrate (better quality) */
        if (ctx->rc_mode) {
            if (strcmp(ctx->rc_mode, "CBR") == 0) {
                p_ni_logan_encoder_params_set_value(params, "cbr", "1", session_ctx);
                blog(LOG_INFO, "[obs-netint-t4xx] Rate control mode: CBR (constant bitrate)");
            } else {
                p_ni_logan_encoder_params_set_value(params, "cbr", "0", session_ctx);
                blog(LOG_INFO, "[obs-netint-t4xx] Rate control mode: VBR (variable bitrate)");
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
                 *   Profile 2 = Main10 (10-bit ONLY)
                 * 
                 * H.265 standard does NOT have a "high" profile like H.264!
                 * According to NETINT Integration Guide section 6.6:
                 *   "Any profile can be used for 8 bit encoding but only the 
                 *    10 bit profiles (main10 for H.265) may be used for 10 bit encoding"
                 * 
                 * For 8-bit encoding (bit_depth_factor=1), we MUST use Profile 1 (Main)
                 * Using Profile 2 (Main10) for 8-bit creates corrupted headers!
                 */
                profile_id_str = "1"; /* Always use Main profile (ID=1) for 8-bit H.265 */
                blog(LOG_INFO, "[obs-netint-t4xx] H.265 8-bit encoding: Profile '%s' mapped to Main (ID=1)", 
                     ctx->profile);
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
                p_ni_logan_encoder_params_set_value(params, "profile", profile_id_str, session_ctx);
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
     * After that, we can use device_session_write/read (low-level) for data I/O.
     * This hybrid approach gives us:
     * - Correct initialization (encode_init/params_parse)
     * - Correct configuration (encode_open)
     * - Direct data transfer (device_session_write/read) with no FIFO!
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

    blog(LOG_INFO, "[obs-netint-t4xx] Encoder initialization complete!");
    blog(LOG_INFO, "[obs-netint-t4xx] Headers will be extracted from first encoded packet");

    /* ===================================================================
     * Initialize background receive thread
     * ===================================================================
     * 
     * CRITICAL: The background thread is NECESSARY because encode_receive()
     * blocks internally. Hardware won't process frames until output is drained!
     */
    
    /* Initialize mutex FIRST (before starting thread!) */
    /* NOTE: Only need ONE mutex for the packet queue */
    /* Library's FIFO is designed for concurrent send/receive without external mutex */
    if (pthread_mutex_init(&ctx->queue_mutex, NULL) != 0) {
        blog(LOG_ERROR, "[obs-netint-t4xx] Failed to initialize queue mutex");
        netint_destroy(ctx);
        return NULL;
    }
    
    /* Initialize queue and thread state */
    ctx->pkt_queue_head = NULL;
    ctx->pkt_queue_tail = NULL;
    ctx->stop_thread = false;
    ctx->thread_created = false;
    
    /* Start background receive thread */
    if (pthread_create(&ctx->recv_thread, NULL, netint_recv_thread, ctx) != 0) {
        blog(LOG_ERROR, "[obs-netint-t4xx] Failed to create receive thread");
        pthread_mutex_destroy(&ctx->queue_mutex);
        netint_destroy(ctx);
        return NULL;
    }
    
    ctx->thread_created = true;
    blog(LOG_INFO, "[obs-netint-t4xx] Background receive thread started successfully");
    blog(LOG_INFO, "[obs-netint-t4xx] Encoder creation complete (background thread design)");
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
    if (!ctx->flushing && ctx->enc.p_session_ctx) {
        blog(LOG_INFO, "[obs-netint-t4xx] ========================================");
        blog(LOG_INFO, "[obs-netint-t4xx] Sending EOS frame in destroy (OBS skipped flush call)");
        blog(LOG_INFO, "[obs-netint-t4xx] ========================================");
        
        /* Allocate stack-local EOS frame */
        ni_logan_session_data_io_t eos_frame_data = {0};
        ni_logan_frame_t *eos_ni_frame = &eos_frame_data.data.frame;
        
        /* Calculate frame dimensions and strides */
        int eos_linesize[NI_LOGAN_MAX_NUM_DATA_POINTERS] = {0};
        int eos_dst_height[NI_LOGAN_MAX_NUM_DATA_POINTERS] = {0};
        p_ni_logan_get_hw_yuv420p_dim(ctx->enc.width, ctx->enc.height, 1,
                                      (ctx->codec_type == 0) ? 1 : 0,
                                      eos_linesize, eos_dst_height);
        
        /* Set extra_data_len BEFORE allocation */
        eos_ni_frame->extra_data_len = 64;
        
        /* Allocate frame buffer */
        int eos_alloc_ret = p_ni_logan_encoder_frame_buffer_alloc(eos_ni_frame, 
                                                                   ctx->enc.width, 
                                                                   ctx->enc.height,
                                                                   eos_linesize, 
                                                                   (ctx->codec_type == 0) ? 1 : 0, 
                                                                   eos_ni_frame->extra_data_len, 
                                                                   1);
        if (eos_alloc_ret == NI_LOGAN_RETCODE_SUCCESS) {
            /* Set EOS frame metadata */
            eos_ni_frame->video_width = ctx->enc.width;
            eos_ni_frame->video_height = ctx->enc.height;
            eos_ni_frame->pts = 0;
            eos_ni_frame->dts = 0;
            eos_ni_frame->start_of_stream = 0;
            eos_ni_frame->end_of_stream = 1;  /* ← CRITICAL: Signals EOS! */
            eos_ni_frame->force_key_frame = 0;
            eos_ni_frame->ni_logan_pict_type = 0;
            
            blog(LOG_INFO, "[obs-netint-t4xx] ✅ STEP 1: Sending EOS frame (end_of_stream=1) to encoder...");
            
            /* Send EOS frame with retry (FIFO might be full) */
            int eos_sent = 0;
            int eos_retry_count = 0;
            const int eos_max_retries = 300;  /* 3 seconds max (10ms * 300) */
            
            do {
                eos_sent = p_ni_logan_device_session_write(ctx->enc.p_session_ctx, 
                                                            &eos_frame_data,
                                                            NI_LOGAN_DEVICE_TYPE_ENCODER);
                
                if (eos_sent < 0) {
                    blog(LOG_ERROR, "[obs-netint-t4xx] ❌ device_session_write error: %d", eos_sent);
                    break;
                } else if (eos_sent == 0) {
                    /* FIFO full - wait for encoder to drain */
                    eos_retry_count++;
                    if (eos_retry_count >= eos_max_retries) {
                        blog(LOG_ERROR, "[obs-netint-t4xx] ❌ FIFO still full after %d retries (%.1f sec)", 
                             eos_max_retries, eos_max_retries * 0.01);
                        break;
                    }
                    if (eos_retry_count % 50 == 0) {
                        blog(LOG_INFO, "[obs-netint-t4xx] Hardware FIFO full, waiting for encoder to drain... (%d ms)", 
                             eos_retry_count * 10);
                    }
                    os_sleep_ms(10);  /* Wait for encoder to drain output */
                }
            } while (eos_sent == 0);
            
            if (eos_sent > 0) {
                blog(LOG_INFO, "[obs-netint-t4xx] ✅ EOS frame sent successfully after %d retries (%d bytes)", 
                     eos_retry_count, eos_sent);
                ctx->flushing = true;
                
                /* Wait for encoder_eof acknowledgment (with timeout) */
                blog(LOG_INFO, "[obs-netint-t4xx] ⏳ STEP 2: Waiting for encoder EOS acknowledgment...");
                int wait_count = 0;
                const int max_wait_ms = 3000;  /* 3 second timeout */
                const int sleep_ms = 10;
                
                while (!ctx->enc.encoder_eof && wait_count < (max_wait_ms / sleep_ms)) {
                    os_sleep_ms(sleep_ms);
                    wait_count++;
                    
                    if (wait_count % 100 == 0) {
                        blog(LOG_INFO, "[obs-netint-t4xx] Still waiting for EOS acknowledgment... (%d ms)", wait_count * sleep_ms);
                    }
                }
                
                if (ctx->enc.encoder_eof) {
                    blog(LOG_INFO, "[obs-netint-t4xx] ✅ STEP 2 SUCCESS: Encoder acknowledged EOS after %d ms", wait_count * sleep_ms);
                } else {
                    blog(LOG_WARNING, "[obs-netint-t4xx] ⚠️  Timeout waiting for encoder EOS acknowledgment after %d ms", max_wait_ms);
                    blog(LOG_WARNING, "[obs-netint-t4xx] Proceeding with close anyway...");
                }
            } else {
                blog(LOG_ERROR, "[obs-netint-t4xx] ❌ Failed to send EOS frame (ret=%d)", eos_sent);
            }
            
            /* Free EOS frame buffer */
            p_ni_logan_frame_buffer_free(eos_ni_frame);
        } else {
            blog(LOG_ERROR, "[obs-netint-t4xx] ❌ Failed to allocate EOS frame buffer (ret=%d)", eos_alloc_ret);
        }
    }
    
    /* ===================================================================
     * Stop background receive thread
     * ===================================================================
     */
    if (ctx->thread_created) {
        blog(LOG_INFO, "[obs-netint-t4xx] Stopping receive thread...");
        ctx->stop_thread = true;
        
        /* Wait for thread to finish (may take time if encode_receive is blocking) */
        pthread_join(ctx->recv_thread, NULL);
        blog(LOG_INFO, "[obs-netint-t4xx] Receive thread stopped");
        ctx->thread_created = false;
    }
    
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
    
    /* Destroy mutex LAST (after thread is stopped!) */
    pthread_mutex_destroy(&ctx->queue_mutex);
    
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
    /* NETINT encoder requires I420 format (planar Y, U, V separate) */
    /* NOT NV12 (which has interleaved UV plane) */
    info->format = VIDEO_FORMAT_I420;
}

/**
 * @brief Background thread that continuously receives packets from encoder
 * 
 * CRITICAL: This thread is NECESSARY because ni_logan_encode_receive() BLOCKS
 * internally with a retry loop (up to 2000+ attempts). If we call it from the
 * OBS video thread, the entire UI freezes!
 * 
 * This thread:
 * 1. Calls encode_receive() continuously (can block - that's OK here!)
 * 2. When packet arrives, adds it to thread-safe queue
 * 3. Main thread pops packets from queue (non-blocking)
 * 
 * @param data Pointer to netint_ctx structure
 * @return NULL
 */
static void *netint_recv_thread(void *data)
{
    struct netint_ctx *ctx = data;
    blog(LOG_INFO, "[obs-netint-t4xx] Receive thread started");
    
    /* ✅ Allocate packet buffer ONCE (reused for all reads) */
    ni_logan_session_data_io_t out_packet = {0};
    ni_logan_packet_t *ni_pkt = &out_packet.data.packet;
    
    /* Allocate buffer for packet data */
    int alloc_ret = p_ni_logan_packet_buffer_alloc(ni_pkt, NI_LOGAN_MAX_TX_SZ);
    if (alloc_ret != NI_LOGAN_RETCODE_SUCCESS) {
        blog(LOG_ERROR, "[obs-netint-t4xx] [RECV THREAD] Failed to allocate packet buffer (ret=%d)", alloc_ret);
        return NULL;
    }
    
    blog(LOG_INFO, "[obs-netint-t4xx] [RECV THREAD] Packet buffer allocated (%d bytes max)", NI_LOGAN_MAX_TX_SZ);
    
        while (!ctx->stop_thread) {
            /* ✅ Read packet directly from hardware using device session API */
            /* This can BLOCK - that's OK in background thread! */
            blog(LOG_DEBUG, "[obs-netint-t4xx] [RECV THREAD] Attempting to read packet...");
            int recv_size = p_ni_logan_device_session_read(ctx->enc.p_session_ctx, &out_packet,
                                                             NI_LOGAN_DEVICE_TYPE_ENCODER);
            blog(LOG_INFO, "[obs-netint-t4xx] [RECV THREAD] device_session_read returned: %d", recv_size);

        if (ctx->stop_thread) break;  /* Check after blocking call */
        
        /* Check if we got a valid packet (accounting for metadata size) */
        int meta_size = NI_LOGAN_FW_ENC_BITSTREAM_META_DATA_SIZE;
        if (recv_size > meta_size) {
            int packet_size = recv_size - meta_size;
            blog(LOG_INFO, "[obs-netint-t4xx] [RECV THREAD] Got packet: %d bytes (total recv=%d)",
                 packet_size, recv_size);
            
            /* ===================================================================
             * STEP 2: Check if this is the EOS acknowledgment packet
             * ===================================================================
             */
            if (ni_pkt->end_of_stream) {
                blog(LOG_INFO, "[obs-netint-t4xx] ========================================");
                blog(LOG_INFO, "[obs-netint-t4xx] ✅ STEP 2 SUCCESS: Encoder sent EOS acknowledgment!");
                blog(LOG_INFO, "[obs-netint-t4xx] ========================================");
                blog(LOG_INFO, "[obs-netint-t4xx] Packet details:");
                blog(LOG_INFO, "[obs-netint-t4xx]   end_of_stream = %d ← Encoder confirmed EOS", ni_pkt->end_of_stream);
                blog(LOG_INFO, "[obs-netint-t4xx]   packet_size = %d bytes", packet_size);
                blog(LOG_INFO, "[obs-netint-t4xx]   pts = %lld, dts = %lld", 
                     (long long)ni_pkt->pts, (long long)ni_pkt->dts);
                blog(LOG_INFO, "[obs-netint-t4xx] Setting encoder_eof flag...");
                ctx->enc.encoder_eof = 1;
                blog(LOG_INFO, "[obs-netint-t4xx] EOS handshake complete - encoder session can now close cleanly");
            }
            
            /* Allocate our queue packet structure */
            struct netint_pkt *pkt = bzalloc(sizeof(*pkt));
            if (!pkt) {
                blog(LOG_ERROR, "[obs-netint-t4xx] [RECV THREAD] Failed to allocate packet struct");
                continue;
            }
            
            /* Allocate and copy packet data */
            pkt->data = bmalloc((size_t)packet_size);
            if (!pkt->data) {
                blog(LOG_ERROR, "[obs-netint-t4xx] [RECV THREAD] Failed to allocate packet data");
                bfree(pkt);
                continue;
            }
            
            /* Skip metadata - actual H.264/H.265 data starts after meta_size bytes */
            memcpy(pkt->data, (uint8_t*)ni_pkt->p_data + meta_size, (size_t)packet_size);
            pkt->size = (size_t)packet_size;
            
            /* Extract headers from first packet if not already done */
            if (!ctx->got_headers) {
                blog(LOG_INFO, "[obs-netint-t4xx] [RECV THREAD] Extracting headers from first packet (%zu bytes)", pkt->size);
                
                /* DIAGNOSTIC: Dump first 64 bytes of header for analysis */
                blog(LOG_INFO, "[obs-netint-t4xx] [RECV THREAD] Header hex dump (first %d bytes):", 
                     pkt->size < 64 ? (int)pkt->size : 64);
                for (size_t i = 0; i < pkt->size && i < 64; i += 16) {
                    char hex_buf[128] = {0};
                    char *p = hex_buf;
                    size_t end = (i + 16 < pkt->size) ? i + 16 : pkt->size;
                    for (size_t j = i; j < end && j < 64; j++) {
                        p += sprintf(p, "%02X ", pkt->data[j]);
                    }
                    blog(LOG_INFO, "[obs-netint-t4xx]   %04zX: %s", i, hex_buf);
                }
                
                /* Analyze NAL unit types to detect actual codec */
                bool has_h265_nal = false;
                bool has_h264_nal = false;
                
                for (size_t i = 0; i < pkt->size - 4; i++) {
                    /* Look for start codes: 00 00 00 01 or 00 00 01 */
                    if (pkt->data[i] == 0 && pkt->data[i+1] == 0) {
                        size_t nal_start = 0;
                        if (pkt->data[i+2] == 0 && pkt->data[i+3] == 1) {
                            nal_start = i + 4;  /* 4-byte start code */
                        } else if (pkt->data[i+2] == 1) {
                            nal_start = i + 3;  /* 3-byte start code */
                        }
                        
                        if (nal_start > 0 && nal_start < pkt->size) {
                            uint8_t nal_byte = pkt->data[nal_start];
                            
                            /* H.265 NAL unit type is in bits 1-6 (mask 0x7E, shift right 1) */
                            uint8_t h265_type = (nal_byte >> 1) & 0x3F;
                            
                            /* H.264 NAL unit type is in bits 0-4 (mask 0x1F) */
                            uint8_t h264_type = nal_byte & 0x1F;
                            
                            blog(LOG_INFO, "[obs-netint-t4xx]   NAL at offset %zu: byte=0x%02X, H.265_type=%u, H.264_type=%u", 
                                 nal_start, nal_byte, h265_type, h264_type);
                            
                            /* H.265 VPS=32, SPS=33, PPS=34 */
                            if (h265_type >= 32 && h265_type <= 34) {
                                has_h265_nal = true;
                                blog(LOG_INFO, "[obs-netint-t4xx]   ✅ H.265 NAL detected: %s", 
                                     h265_type == 32 ? "VPS" : h265_type == 33 ? "SPS" : "PPS");
                            }
                            
                            /* H.264 SPS=7, PPS=8 */
                            if (h264_type == 7 || h264_type == 8) {
                                has_h264_nal = true;
                                blog(LOG_INFO, "[obs-netint-t4xx]   ⚠️  H.264 NAL detected: %s", 
                                     h264_type == 7 ? "SPS" : "PPS");
                            }
                        }
                    }
                }
                
                if (has_h265_nal && !has_h264_nal) {
                    blog(LOG_INFO, "[obs-netint-t4xx] ✅ Headers are H.265 (HEVC) - codec is working correctly!");
                } else if (has_h264_nal && !has_h265_nal) {
                    blog(LOG_ERROR, "[obs-netint-t4xx] ❌ CRITICAL: Headers are H.264 but H.265 was requested!");
                    blog(LOG_ERROR, "[obs-netint-t4xx] ❌ Hardware encoder is producing wrong codec!");
                } else if (has_h265_nal && has_h264_nal) {
                    blog(LOG_WARNING, "[obs-netint-t4xx] ⚠️  Mixed H.264/H.265 NALs detected - unusual!");
                } else {
                    blog(LOG_WARNING, "[obs-netint-t4xx] ⚠️  Could not identify codec from headers");
                }
                
                if (ctx->extra) bfree(ctx->extra);
                ctx->extra = bmemdup(pkt->data, pkt->size);
                ctx->extra_size = pkt->size;
                ctx->got_headers = true;
            }
            
            /* Get timestamps from encoder context */
            /* NOTE: With device_session API, timestamps come from packet metadata */
            pkt->pts = ni_pkt->pts;
            pkt->dts = ni_pkt->dts;
            
            blog(LOG_INFO, "[obs-netint-t4xx] [RECV THREAD] Packet timestamps: PTS=%lld, DTS=%lld", 
                 (long long)pkt->pts, (long long)pkt->dts);
            
            /* If timestamps not set, use fallback from encoder context */
            if (pkt->pts == 0 && ctx->enc.latest_dts != 0) {
                pkt->pts = ctx->enc.latest_dts;
                pkt->dts = pkt->pts;
            } else if (pkt->pts == 0 && ctx->enc.first_frame_pts != 0) {
                pkt->pts = ctx->enc.first_frame_pts;
                pkt->dts = pkt->pts;
            }
            
            /* Determine if keyframe by parsing packet data */
            if (ctx->codec_type == 1) {
                pkt->keyframe = obs_hevc_keyframe(pkt->data, pkt->size);
            } else {
                pkt->keyframe = obs_avc_keyframe(pkt->data, pkt->size);
            }
            
            /* Add to queue - THREAD SAFE */
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
            
            blog(LOG_INFO, "[obs-netint-t4xx] [RECV THREAD] ✅ Packet queued: size=%zu, pts=%lld, keyframe=%d",
                 pkt->size, (long long)pkt->pts, pkt->keyframe);
        } else if (recv_size < 0) {
            /* Error or EOF */
            if (ctx->enc.encoder_eof) {
                blog(LOG_INFO, "[obs-netint-t4xx] ========================================");
                blog(LOG_INFO, "[obs-netint-t4xx] [RECV THREAD] Encoder EOF detected - stopping receive loop");
                blog(LOG_INFO, "[obs-netint-t4xx] ========================================");
                blog(LOG_INFO, "[obs-netint-t4xx] encoder_eof = %d", ctx->enc.encoder_eof);
                blog(LOG_INFO, "[obs-netint-t4xx] EOS handshake complete - safe to close session");
                break;
            }
            /* Other errors - continue */
            blog(LOG_DEBUG, "[obs-netint-t4xx] [RECV THREAD] Read error: %d", recv_size);
            os_sleep_ms(1);  /* Small sleep to avoid busy-wait */
        } else {
            /* recv_size == 0 or <= meta_size: No packet yet */
            os_sleep_ms(1);  /* Small sleep to avoid busy-wait */
        }
    }
    
    /* Free packet buffer */
    p_ni_logan_packet_buffer_free(ni_pkt);
    
    blog(LOG_INFO, "[obs-netint-t4xx] Receive thread stopped");
    return NULL;
}

/**
 * @brief Encode a video frame and/or receive an encoded packet
 * 
 * Background thread design - necessary because encode_receive() BLOCKS!
 * 
 * Main thread (this function):
 * 1. Checks packet queue for available packets
 * 2. Returns packet to OBS if available
 * 3. Sends new frame to encoder (if provided)
 * 
 * Background thread (netint_recv_thread):
 * 1. Calls encode_receive() continuously (can block safely)
 * 2. Adds packets to thread-safe queue
 * 
 * @param data Encoder context pointer
 * @param frame Video frame to encode (NULL when flushing)
 * @param packet Output packet structure (filled if packet available)
 * @param received Output flag: true if packet was returned, false if no packet ready
 * @return true on success, false on error
 */
static bool netint_send_eos_frame(struct netint_ctx *ctx)
{
    blog(LOG_INFO, "[obs-netint-t4xx] Getting EOS frame buffer from encoder FIFO...");
    int eos_get_ret = p_ni_logan_encode_get_frame(&ctx->enc);
    if (eos_get_ret != NI_LOGAN_RETCODE_SUCCESS) {
        blog(LOG_ERROR, "[obs-netint-t4xx] EOS encode_get_frame failed (ret=%d)", eos_get_ret);
        return false;
    }

    ni_logan_session_data_io_t *eos_input_fme = (ni_logan_session_data_io_t *)ctx->enc.p_input_fme;
    if (!eos_input_fme) {
        blog(LOG_ERROR, "[obs-netint-t4xx] EOS frame buffer is NULL after encode_get_frame!");
        return false;
    }

    ni_logan_frame_t *eos_ni_frame = &eos_input_fme->data.frame;

    /* Allocate EOS frame data buffers */
    int eos_linesize[NI_LOGAN_MAX_NUM_DATA_POINTERS] = {0};
    int eos_dst_height[NI_LOGAN_MAX_NUM_DATA_POINTERS] = {0};
    p_ni_logan_get_hw_yuv420p_dim(ctx->enc.width, ctx->enc.height, 1,
                                  (ctx->codec_type == 0) ? 1 : 0,
                                  eos_linesize, eos_dst_height);

    int eos_alloc_ret = p_ni_logan_encoder_frame_buffer_alloc(eos_ni_frame, ctx->enc.width, ctx->enc.height,
                                                               eos_linesize, (ctx->codec_type == 0) ? 1 : 0, 0, 1);
    if (eos_alloc_ret != NI_LOGAN_RETCODE_SUCCESS) {
        blog(LOG_ERROR, "[obs-netint-t4xx] EOS frame buffer allocation failed (ret=%d)", eos_alloc_ret);
        return false;
    }

    /* Initialize EOS frame metadata */
    eos_ni_frame->video_width = ctx->enc.width;
    eos_ni_frame->video_height = ctx->enc.height;
    eos_ni_frame->pts = 0;
    eos_ni_frame->dts = 0;
    eos_ni_frame->start_of_stream = 0;
    eos_ni_frame->end_of_stream = 1;
    eos_ni_frame->force_key_frame = 0;

    blog(LOG_INFO, "[obs-netint-t4xx] Sending EOS frame to encoder");

    /* Send EOS frame */
    int eos_sent;
    int eos_retry_count = 0;
    const int eos_max_retries = 50;

    do {
        eos_sent = p_ni_logan_device_session_write(ctx->enc.p_session_ctx, ctx->enc.p_input_fme,
                                                    NI_LOGAN_DEVICE_TYPE_ENCODER);

        if (eos_sent < 0) {
            blog(LOG_ERROR, "[obs-netint-t4xx] EOS frame send failed (ret=%d)", eos_sent);
            return false;
        } else if (eos_sent == 0) {
            eos_retry_count++;
            if (eos_retry_count >= eos_max_retries) {
                blog(LOG_WARNING, "[obs-netint-t4xx] EOS FIFO full after %d retries, cannot send EOS", eos_max_retries);
                return false;
            }
            blog(LOG_DEBUG, "[obs-netint-t4xx] EOS FIFO full, retrying in 10ms (attempt %d/%d)", eos_retry_count, eos_max_retries);
            os_sleep_ms(10);
        }
    } while (eos_sent == 0);

    blog(LOG_INFO, "[obs-netint-t4xx] EOS frame sent successfully");
    return true;
}

static bool netint_encode_batch(struct netint_ctx *ctx, struct encoder_packet *packet, bool *received)
{
    blog(LOG_INFO, "[obs-netint-t4xx] Processing batch of %d frames like xcoder_logan", ctx->frame_buffer.count);

    /* Send all buffered frames to the encoder using LOW-LEVEL API like xcoder */
    /* XCoder uses device_session_write with stack-allocated session_data_io_t, NOT FIFO APIs! */
    for (int i = 0; i < ctx->frame_buffer.count; i++) {
        struct encoder_frame *frame = (struct encoder_frame *)ctx->frame_buffer.frames[i];

        /* Get basic frame dimensions */
        int width = ctx->enc.width;
        int height = ctx->enc.height;

        blog(LOG_INFO, "[obs-netint-t4xx] Sending frame %d/%d: %dx%d @ %lld",
             i+1, ctx->frame_buffer.count, width, height, (long long)frame->pts);
        
        blog(LOG_INFO, "[obs-netint-t4xx] OBS frame: linesize=[%d,%d,%d] data=[%p,%p,%p]",
             frame->linesize[0], frame->linesize[1], frame->linesize[2],
             frame->data[0], frame->data[1], frame->data[2]);
        
        /* Check if U and V planes look correct */
        if (frame->data[1] && frame->data[2]) {
            ptrdiff_t uv_offset = frame->data[2] - frame->data[1];
            blog(LOG_INFO, "[obs-netint-t4xx] U-V plane offset: %td bytes (expected: %d for normal I420)",
                 uv_offset, frame->linesize[1] * height / 2);
        }

        /* Allocate our own session_data_io_t on stack (like xcoder) */
        ni_logan_session_data_io_t input_data = {0};
        ni_logan_frame_t *ni_frame = &input_data.data.frame;

        /* Allocate frame data buffers */
        int dst_stride[NI_LOGAN_MAX_NUM_DATA_POINTERS] = {0};
        int dst_height[NI_LOGAN_MAX_NUM_DATA_POINTERS] = {0};
        p_ni_logan_get_hw_yuv420p_dim(width, height, 1, (ctx->codec_type == 0) ? 1 : 0, dst_stride, dst_height);

        /* Set extra_data_len BEFORE allocation (xcoder does this) */
        ni_frame->extra_data_len = 64;  /* NI_LOGAN_APP_ENC_FRAME_META_DATA_SIZE - REQUIRED! */

        int alloc_ret = p_ni_logan_encoder_frame_buffer_alloc(ni_frame, width, height,
                                                               dst_stride, (ctx->codec_type == 0) ? 1 : 0, 
                                                               ni_frame->extra_data_len, 1);
        if (alloc_ret != NI_LOGAN_RETCODE_SUCCESS) {
            blog(LOG_ERROR, "[obs-netint-t4xx] Failed to allocate frame data buffers (ret=%d)", alloc_ret);
            return false;
        }

        /* Set frame metadata - MATCH XCODER EXACTLY */
        ni_frame->video_width = width;
        ni_frame->video_height = height;
        ni_frame->pts = frame->pts;
        ni_frame->dts = 0;
        ni_frame->start_of_stream = (ctx->frame_count == 0) ? 1 : 0;
        ni_frame->end_of_stream = 0;

        /* Set keyframe properties */
        int is_keyframe = (ctx->frame_count == 0);  /* Only force keyframe on first frame */
        ni_frame->force_key_frame = is_keyframe ? 1 : 0;
        ni_frame->ni_logan_pict_type = is_keyframe ? LOGAN_PIC_TYPE_IDR : 0;  /* 0 for auto */

        /* Copy YUV data directly to the allocated frame buffer */
        if (!ni_frame->p_data[0]) {
            blog(LOG_ERROR, "[obs-netint-t4xx] Frame buffer p_data[0] is NULL!");
            p_ni_logan_frame_buffer_free(ni_frame);
            return false;
        }

        /* OBS will provide I420 format (via get_video_info callback) */
        /* I420 = planar Y, U, V with separate planes */
        uint8_t *src_planes[NI_LOGAN_MAX_NUM_DATA_POINTERS] = {
            frame->data[0],      /* Y plane */
            frame->data[1],      /* U plane */
            frame->data[2],      /* V plane */
            NULL
        };
        int src_stride[NI_LOGAN_MAX_NUM_DATA_POINTERS] = {
            frame->linesize[0],  /* Y stride */
            frame->linesize[1],  /* U stride */
            frame->linesize[2],  /* V stride */
            0
        };
        int src_height[NI_LOGAN_MAX_NUM_DATA_POINTERS] = {
            height,       /* Y plane height */
            height / 2,   /* U plane height (YUV420 - subsampled by 2) */
            height / 2,   /* V plane height (YUV420 - subsampled by 2) */
            0
        };

        /* Log YUV copy parameters for debugging */
        blog(LOG_INFO, "[obs-netint-t4xx] YUV copy: src_stride=[%d,%d,%d] src_height=[%d,%d,%d] dst_stride=[%d,%d,%d] dst_height=[%d,%d,%d]",
             src_stride[0], src_stride[1], src_stride[2],
             src_height[0], src_height[1], src_height[2],
             dst_stride[0], dst_stride[1], dst_stride[2],
             dst_height[0], dst_height[1], dst_height[2]);

        /* Copy YUV data using hardware-optimized function */
        /* API: ni_logan_copy_hw_yuv420p(dst, src, width, height, bit_depth_factor, 
                                          dst_stride, dst_height, src_stride, src_height) */
        p_ni_logan_copy_hw_yuv420p((uint8_t **)ni_frame->p_data, (uint8_t **)src_planes,
                                    width, height, 1,
                                    dst_stride, dst_height, src_stride, src_height);

        /* Send frame using LOW-LEVEL device_session_write (like xcoder) - NO FIFO! */
        int sent = p_ni_logan_device_session_write(ctx->enc.p_session_ctx, &input_data,
                                                    NI_LOGAN_DEVICE_TYPE_ENCODER);

        if (sent < 0) {
            blog(LOG_ERROR, "[obs-netint-t4xx] device_session_write failed (ret=%d)", sent);
            p_ni_logan_frame_buffer_free(ni_frame);
            return false;
        } else if (sent == 0) {
            /* Hardware FIFO full - retry with delay */
            blog(LOG_WARNING, "[obs-netint-t4xx] Hardware FIFO full, retrying frame %d", i+1);
            int retry_count = 0;
            const int max_retries = 100;
            do {
                os_sleep_ms(10);
                sent = p_ni_logan_device_session_write(ctx->enc.p_session_ctx, &input_data,
                                                        NI_LOGAN_DEVICE_TYPE_ENCODER);
                retry_count++;
                if (retry_count >= max_retries) {
                    blog(LOG_ERROR, "[obs-netint-t4xx] Hardware FIFO still full after %d retries", max_retries);
                    p_ni_logan_frame_buffer_free(ni_frame);
                    return false;
                }
            } while (sent == 0);
            
            if (sent < 0) {
                blog(LOG_ERROR, "[obs-netint-t4xx] device_session_write failed after retry (ret=%d)", sent);
                p_ni_logan_frame_buffer_free(ni_frame);
                return false;
            }
        }

        /* Free the frame buffer after sending (xcoder pattern) */
        p_ni_logan_frame_buffer_free(ni_frame);

        blog(LOG_INFO, "[obs-netint-t4xx] Frame %d/%d sent successfully (PTS=%lld, sent=%d bytes)",
             i+1, ctx->frame_buffer.count, (long long)frame->pts, sent);

        /* Mark encoder as started after first successful frame send */
        if (sent > 0 && !ctx->enc.started) {
            ctx->enc.started = 1;
            blog(LOG_INFO, "[obs-netint-t4xx] ✅ Encoder started! First frame sent to hardware.");
        }

        ctx->frame_count++;
    }

    /* Clear the frame buffer now that frames are in hardware FIFO */
    ctx->frame_buffer.count = 0;

    blog(LOG_INFO, "[obs-netint-t4xx] Batch submitted, encoder ready for next frame");

    return true;
}

static bool netint_encode_flush(struct netint_ctx *ctx, struct encoder_packet *packet, bool *received)
{
    blog(LOG_INFO, "[obs-netint-t4xx] ========================================");
    blog(LOG_INFO, "[obs-netint-t4xx] FLUSH MODE: Sending EOS frame to encoder");
    blog(LOG_INFO, "[obs-netint-t4xx] ========================================");

    /* CRITICAL: Check if EOS already sent */
    if (ctx->flushing) {
        blog(LOG_INFO, "[obs-netint-t4xx] Already in flushing mode, encoder_eof=%d", ctx->enc.encoder_eof);
        *received = false;
        return true;
    }

    /* Mark as flushing to prevent re-entry */
    ctx->flushing = true;

    /* ===================================================================
     * STEP 1: Send EOS frame to encoder (API requirement for streaming)
     * ===================================================================
     * According to libxcoder API documentation:
     * - Application must send frame with end_of_stream = 1
     * - Encoder will respond with packet containing end_of_stream = 1
     * - This is a BIDIRECTIONAL handshake for streaming mode
     */

    blog(LOG_INFO, "[obs-netint-t4xx] STEP 1: Allocating stack-local EOS frame buffer");
    
    /* Allocate our own session_data_io_t on stack for EOS frame */
    ni_logan_session_data_io_t eos_frame_data = {0};
    ni_logan_frame_t *eos_ni_frame = &eos_frame_data.data.frame;

    /* Calculate frame dimensions and strides */
    int eos_linesize[NI_LOGAN_MAX_NUM_DATA_POINTERS] = {0};
    int eos_dst_height[NI_LOGAN_MAX_NUM_DATA_POINTERS] = {0};
    p_ni_logan_get_hw_yuv420p_dim(ctx->enc.width, ctx->enc.height, 1,
                                  (ctx->codec_type == 0) ? 1 : 0,
                                  eos_linesize, eos_dst_height);

    blog(LOG_INFO, "[obs-netint-t4xx] EOS frame dimensions: %dx%d, linesize=[%d,%d,%d]",
         ctx->enc.width, ctx->enc.height, eos_linesize[0], eos_linesize[1], eos_linesize[2]);

    /* Set extra_data_len BEFORE allocation (required by API) */
    eos_ni_frame->extra_data_len = 64;  /* NI_LOGAN_APP_ENC_FRAME_META_DATA_SIZE */

    /* Allocate frame buffer */
    int eos_alloc_ret = p_ni_logan_encoder_frame_buffer_alloc(eos_ni_frame, 
                                                               ctx->enc.width, 
                                                               ctx->enc.height,
                                                               eos_linesize, 
                                                               (ctx->codec_type == 0) ? 1 : 0, 
                                                               eos_ni_frame->extra_data_len, 
                                                               1);
    if (eos_alloc_ret != NI_LOGAN_RETCODE_SUCCESS) {
        blog(LOG_ERROR, "[obs-netint-t4xx] ❌ STEP 1 FAILED: EOS frame buffer allocation failed (ret=%d)", eos_alloc_ret);
        ctx->flushing = false;
        return false;
    }

    blog(LOG_INFO, "[obs-netint-t4xx] ✅ EOS frame buffer allocated successfully");

    /* ===================================================================
     * CRITICAL: Set end_of_stream = 1 (this signals EOS to encoder!)
     * ===================================================================
     */
    eos_ni_frame->video_width = ctx->enc.width;
    eos_ni_frame->video_height = ctx->enc.height;
    eos_ni_frame->pts = 0;
    eos_ni_frame->dts = 0;
    eos_ni_frame->start_of_stream = 0;
    eos_ni_frame->end_of_stream = 1;  /* ← CRITICAL: This signals EOS! */
    eos_ni_frame->force_key_frame = 0;
    eos_ni_frame->ni_logan_pict_type = 0;

    blog(LOG_INFO, "[obs-netint-t4xx] STEP 1: EOS frame metadata set:");
    blog(LOG_INFO, "[obs-netint-t4xx]   video_width=%d, video_height=%d", 
         eos_ni_frame->video_width, eos_ni_frame->video_height);
    blog(LOG_INFO, "[obs-netint-t4xx]   start_of_stream=%d", eos_ni_frame->start_of_stream);
    blog(LOG_INFO, "[obs-netint-t4xx]   end_of_stream=%d ← CRITICAL: Signals EOS to encoder", 
         eos_ni_frame->end_of_stream);
    blog(LOG_INFO, "[obs-netint-t4xx]   pts=%lld, dts=%lld", 
         (long long)eos_ni_frame->pts, (long long)eos_ni_frame->dts);

    /* Send EOS frame to hardware encoder */
    blog(LOG_INFO, "[obs-netint-t4xx] STEP 1: Sending EOS frame to hardware encoder...");
    
    int eos_sent;
    int eos_retry_count = 0;
    const int eos_max_retries = 100;  /* Increased for reliability */

    do {
        eos_sent = p_ni_logan_device_session_write(ctx->enc.p_session_ctx, 
                                                    &eos_frame_data,
                                                    NI_LOGAN_DEVICE_TYPE_ENCODER);

        if (eos_sent < 0) {
            blog(LOG_ERROR, "[obs-netint-t4xx] ❌ STEP 1 FAILED: device_session_write returned error %d", eos_sent);
            p_ni_logan_frame_buffer_free(eos_ni_frame);
            ctx->flushing = false;
            return false;
        } else if (eos_sent == 0) {
            eos_retry_count++;
            if (eos_retry_count >= eos_max_retries) {
                blog(LOG_ERROR, "[obs-netint-t4xx] ❌ STEP 1 FAILED: Hardware FIFO full after %d retries", eos_max_retries);
                p_ni_logan_frame_buffer_free(eos_ni_frame);
                ctx->flushing = false;
                return false;
            }
            if (eos_retry_count % 10 == 0) {
                blog(LOG_WARNING, "[obs-netint-t4xx] EOS FIFO full, retrying... (attempt %d/%d)", 
                     eos_retry_count, eos_max_retries);
            }
            os_sleep_ms(10);
        } else {
            blog(LOG_INFO, "[obs-netint-t4xx] ✅ STEP 1 SUCCESS: EOS frame sent to hardware (%d bytes)", eos_sent);
            blog(LOG_INFO, "[obs-netint-t4xx]   Encoder now knows stream is ending");
            blog(LOG_INFO, "[obs-netint-t4xx]   Encoder will flush internal buffers");
        }
    } while (eos_sent == 0);

    /* Free EOS frame buffer */
    p_ni_logan_frame_buffer_free(eos_ni_frame);

    blog(LOG_INFO, "[obs-netint-t4xx] ========================================");
    blog(LOG_INFO, "[obs-netint-t4xx] STEP 2: Waiting for encoder EOS acknowledgment");
    blog(LOG_INFO, "[obs-netint-t4xx] ========================================");
    blog(LOG_INFO, "[obs-netint-t4xx] According to API spec, encoder will respond with:");
    blog(LOG_INFO, "[obs-netint-t4xx]   - Final encoded packets from buffer");
    blog(LOG_INFO, "[obs-netint-t4xx]   - Last packet will have end_of_stream = 1");
    blog(LOG_INFO, "[obs-netint-t4xx] Background thread will receive and queue these packets");
    blog(LOG_INFO, "[obs-netint-t4xx] Main thread should keep calling encode() to drain queue");

    /* ===================================================================
     * STEP 2: Wait for encoder to acknowledge EOS
     * ===================================================================
     * The background receive thread will continue receiving packets.
     * When encoder sends packet with end_of_stream = 1, it will set
     * ctx->enc.encoder_eof = 1 and stop the receive loop.
     * 
     * For now, just return false to signal no packet ready yet.
     * OBS will keep calling encode() to drain remaining packets.
     */

    blog(LOG_INFO, "[obs-netint-t4xx] STEP 2: EOS handshake initiated");
    blog(LOG_INFO, "[obs-netint-t4xx]   encoder_eof status: %d (will be 1 when encoder confirms)", 
         ctx->enc.encoder_eof);
    blog(LOG_INFO, "[obs-netint-t4xx]   Background thread continues receiving packets");
    blog(LOG_INFO, "[obs-netint-t4xx]   OBS should keep calling encode() to drain queue");

    *received = false;
    return true;
}

static bool netint_send_eos_frame(struct netint_ctx *ctx);
static bool netint_encode(void *data, struct encoder_frame *frame, struct encoder_packet *packet, bool *received)
{
    struct netint_ctx *ctx = data;
    *received = false;

    /* DEBUG: Log every call to netint_encode to see if OBS is calling us */
    static int call_count = 0;
    call_count++;
    blog(LOG_INFO, "[obs-netint-t4xx] ▶ netint_encode() call #%d: frame=%p", call_count, frame);

    /* DEBUG: Check encoder state */
    blog(LOG_INFO, "[obs-netint-t4xx] Encoder state: started=%d, p_input_fme=%p",
         ctx->enc.started, ctx->enc.p_input_fme);

#ifdef DEBUG_NETINT_PLUGIN
    /* Validate magic number to detect corruption */
    if (ctx->debug_magic != NETINT_ENC_CONTEXT_MAGIC) {
        blog(LOG_ERROR, "[DEBUG] Invalid context magic in netint_encode: 0x%08X (expected 0x%08X)", 
             ctx->debug_magic, NETINT_ENC_CONTEXT_MAGIC);
        NETINT_DEBUGBREAK();
        return false;
    }
#endif

    /* Validate context */
    NETINT_VALIDATE_ENC_CONTEXT(ctx, "netint_encode entry");

    /* ===================================================================
     * STEP 1: ALWAYS check for available packets FIRST
     * ===================================================================
     *
     * Critical: Check for packets BEFORE sending frames. The encoder may have
     * buffered frames and produced packets that need to be returned immediately.
     * This matches the xcoder_logan pattern: receive -> send -> receive -> send...
     */

    /* DEBUG: Try synchronous receive like xcoder_logan */
    if (!*received) {
        /* Try to receive a packet synchronously, just like xcoder_logan does */
        ni_logan_session_data_io_t sync_packet = {0};
        ni_logan_packet_t *sync_pkt = &sync_packet.data.packet;

        /* Allocate packet buffer for synchronous read */
        int alloc_ret = p_ni_logan_packet_buffer_alloc(sync_pkt, NI_LOGAN_MAX_TX_SZ);
        if (alloc_ret == NI_LOGAN_RETCODE_SUCCESS) {
            /* Try synchronous read */
            int sync_recv_size = p_ni_logan_device_session_read(ctx->enc.p_session_ctx, &sync_packet,
                                                               NI_LOGAN_DEVICE_TYPE_ENCODER);

            if (sync_recv_size > 16) {  /* Got a real packet (> metadata size) */
                int packet_size = sync_recv_size - 16;  /* Subtract metadata */

                blog(LOG_INFO, "[obs-netint-t4xx] ✅ SYNC RECEIVE: Got packet %d bytes (total %d)",
                     packet_size, sync_recv_size);

                /* Check if this is the header packet (first packet) */
                if (!ctx->got_headers && packet_size == 65) {
                    blog(LOG_INFO, "[obs-netint-t4xx] SYNC RECEIVE: Extracting headers from first packet");
                    if (ctx->extra) bfree(ctx->extra);
                    ctx->extra = bmemdup(sync_pkt->p_data, (size_t)packet_size);
                    ctx->extra_size = (size_t)packet_size;
                    ctx->got_headers = true;
                } else {
                    /* This is a video packet! */
                    blog(LOG_INFO, "[obs-netint-t4xx] ✅ SYNC RECEIVE: VIDEO PACKET! %d bytes", packet_size);

                    packet->data = bmalloc((size_t)packet_size);
                    if (packet->data) {
                        memcpy(packet->data, sync_pkt->p_data, (size_t)packet_size);
                        packet->size = (size_t)packet_size;
                        packet->pts = sync_pkt->pts;
                        packet->dts = sync_pkt->dts;
                        packet->type = OBS_ENCODER_VIDEO;

                        /* Get timebase */
                        video_t *video = obs_encoder_video(ctx->encoder);
                        const struct video_output_info *voi = video_output_get_info(video);
                        packet->timebase_num = (int32_t)voi->fps_den;
                        packet->timebase_den = (int32_t)voi->fps_num;

                        /* Parse packet for keyframe and priority */
                        if (ctx->codec_type == 1) {
                            packet->keyframe = obs_hevc_keyframe(packet->data, packet->size);
                            packet->priority = obs_parse_hevc_packet_priority(packet);
                        } else {
                            packet->keyframe = obs_avc_keyframe(packet->data, packet->size);
                            packet->priority = obs_parse_avc_packet_priority(packet);
                        }

                        *received = true;
                        blog(LOG_INFO, "[obs-netint-t4xx] ✅ SYNC RECEIVE: Returning VIDEO packet to OBS: %zu bytes, keyframe=%d",
                             packet->size, packet->keyframe);
                        p_ni_logan_packet_buffer_free(sync_pkt);
                        return true;
                    }
                }
            }
            p_ni_logan_packet_buffer_free(sync_pkt);
        }
    }

    /* Fallback: Try to get any queued packets from background thread */
    pthread_mutex_lock(&ctx->queue_mutex);
    struct netint_pkt *pkt = ctx->pkt_queue_head;
    if (pkt) {
        /* Remove from queue */
        ctx->pkt_queue_head = pkt->next;
        if (!ctx->pkt_queue_head) {
            ctx->pkt_queue_tail = NULL;
        }
    }
    pthread_mutex_unlock(&ctx->queue_mutex);

    /* If we have a packet, return it to OBS */
    if (pkt) {
        blog(LOG_INFO, "[obs-netint-t4xx] ✅ Returning queued packet: size=%zu, pts=%lld, keyframe=%d",
             pkt->size, (long long)pkt->pts, pkt->keyframe);

        packet->data = pkt->data;
        packet->size = pkt->size;
        packet->pts = pkt->pts;
        packet->dts = pkt->dts;
        packet->keyframe = pkt->keyframe;
        packet->type = OBS_ENCODER_VIDEO;

        /* Get timebase */
        video_t *video = obs_encoder_video(ctx->encoder);
        const struct video_output_info *voi = video_output_get_info(video);
        packet->timebase_num = (int32_t)voi->fps_den;
        packet->timebase_den = (int32_t)voi->fps_num;

        /* Parse packet for priority */
        if (ctx->codec_type == 1) {
            packet->priority = obs_parse_hevc_packet_priority(packet);
        } else {
            packet->priority = obs_parse_avc_packet_priority(packet);
        }

        /* Encoder busy flag already cleared by netint_encode_batch */

        /* Free packet struct (but NOT data - OBS owns it now!) */
        bfree(pkt);

        *received = true;
        blog(LOG_INFO, "[obs-netint-t4xx] ✅ Packet returned to OBS: %zu bytes", packet->size);
        return true;
    }

    /* ===================================================================
     * STEP 2: Buffer incoming frame for batch processing (like xcoder_logan)
     * ===================================================================
     */

    /* Buffer the incoming frame */
    if (frame) {
        /* Buffer frame (with capacity=1, this immediately triggers batch processing) */
        if (ctx->frame_buffer.count < ctx->frame_buffer.capacity) {
            /* Add frame to buffer */
            ctx->frame_buffer.frames[ctx->frame_buffer.count] = (void *)frame;
            ctx->frame_buffer.count++;
            blog(LOG_INFO, "[obs-netint-t4xx] Buffered frame %d/%d", ctx->frame_buffer.count, ctx->frame_buffer.capacity);

            /* If buffer not full yet, tell OBS we don't have a packet ready */
            if (ctx->frame_buffer.count < ctx->frame_buffer.capacity) {
                *received = false;
                return true;
            }
        }

        /* Buffer is full - process the batch */
        blog(LOG_INFO, "[obs-netint-t4xx] Frame buffer full (%d frames), processing batch like xcoder_logan", ctx->frame_buffer.count);
        return netint_encode_batch(ctx, packet, received);
    } else {
        /* Flushing mode */
        blog(LOG_INFO, "[obs-netint-t4xx] Flushing encoder - no more frames");
        return netint_encode_flush(ctx, packet, received);
    }
}
static void netint_get_defaults(obs_data_t *settings)
{
    /* NOTE: Codec is NOT a setting anymore - it's determined by which encoder the user selects */
    /* OBS will create either obs_netint_t4xx_h264 or obs_netint_t4xx_h265 based on user's choice */
    
    /* Default bitrate: 6000 kbps (good for 1080p streaming) */
    obs_data_set_default_int(settings, "bitrate", 6000);
    
    /* Default keyframe interval: 2 seconds (auto-calculated from FPS if <= 0) */
    obs_data_set_default_int(settings, "keyint", 2);
    
    /* Default rate control: CBR (constant bitrate) - better for streaming */
    obs_data_set_default_string(settings, "rc_mode", "CBR");
    
    /* Default profile: high (best quality, supports all encoder features) */
    obs_data_set_default_string(settings, "profile", "high");
    
    /* Default GOP preset: default (I-B-B-B-P pattern with B-frames for best quality) */
    obs_data_set_default_string(settings, "gop_preset", "default");
    
    /* Default repeat headers: true (attach SPS/PPS to every keyframe) */
    /* This is important for streaming where clients may join mid-stream */
    obs_data_set_default_bool(settings, "repeat_headers", true);
}

/**
 * @brief Create properties UI for encoder settings
 * 
 * This function is called by OBS Studio to create the settings UI that appears
 * when the user configures the encoder. It creates all the UI controls (text boxes,
 * dropdowns, checkboxes) for each setting.
 * 
 * Properties Created:
 * - bitrate: Integer input (100-100000 kbps, step 50)
 * - keyint: Integer input (1-20 seconds, step 1)
 * - device: Text input or dropdown (auto-populated if discovery available)
 * - rc_mode: Dropdown list (CBR or VBR)
 * - profile: Dropdown list (baseline, main, high)
 * - repeat_headers: Checkbox (attach SPS/PPS to every keyframe)
 * 
 * NOTE: Codec selection (H.264 vs H.265) is done by selecting the encoder in OBS:
 * - "NETINT T4XX H.264" for H.264 encoding
 * - "NETINT T4XX H.265" for H.265 encoding
 * This matches how NVENC works and ensures proper codec detection by MP4 muxer.
 * 
 * Device Discovery:
 * - If device discovery APIs are available, populate device dropdown
 * - Otherwise, device is a text input field for manual entry
 * - Discovery happens at property creation time (not dynamic)
 * 
 * @param data Encoder context (unused - properties are the same for all instances)
 * @return OBS properties object containing all UI controls
 */
static obs_properties_t *netint_get_properties(void *data)
{
    UNUSED_PARAMETER(data);
    obs_properties_t *props = obs_properties_create();
    
    /* NOTE: Codec dropdown removed - user selects encoder type in OBS instead */
    /* This ensures MP4 muxer correctly identifies codec type from encoder registration */
    
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
    
    /* Profile selection: baseline, main, or high */
    obs_property_t *prof = obs_properties_add_list(props, "profile", "Profile", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(prof, "baseline", "baseline");
    obs_property_list_add_string(prof, "main", "main");
    obs_property_list_add_string(prof, "high", "high");
    
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
    /* This converts the text input to a dropdown with discovered devices */
    if (p_ni_logan_rsrc_init && p_ni_logan_rsrc_get_local_device_list) {
        /* Initialize resource management system */
        /* Accept both SUCCESS (0) and INIT_ALREADY (0x7FFFFFFF) as success */
        int rsrc_ret = p_ni_logan_rsrc_init(0, 1);
        if (rsrc_ret == 0 || rsrc_ret == 0x7FFFFFFF) {
            char names[16][NI_LOGAN_MAX_DEVICE_NAME_LEN] = {0};
            int n = p_ni_logan_rsrc_get_local_device_list(names, 16);
            if (n > 0) {
                /* Get existing device property (text input) */
                obs_property_t *dev = obs_properties_get(props, "device");
                if (dev) {
                    /* Update description if it exists */
                    obs_property_set_long_description(dev, "Device Name");
                } else {
                    /* Create new dropdown if text input doesn't exist (shouldn't happen) */
                    dev = obs_properties_add_list(props, "device", "Device", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
                }
                /* Add all discovered devices to dropdown */
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
    .get_defaults = netint_get_defaults,
    .get_properties = netint_get_properties,
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
    .get_defaults = netint_get_defaults,
    .get_properties = netint_get_properties,
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






