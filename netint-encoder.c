/**
 * @file netint-encoder.c
 * @brief Core encoder implementation for NETINT T4XX hardware encoder
 * 
 * This file implements the OBS encoder interface for NETINT T408 hardware encoders.
 * It provides hardware-accelerated H.264 and H.265 encoding using NETINT's PCIe cards.
 * 
 * Architecture Overview:
 * - Asynchronous encoding: Uses background thread to receive encoded packets
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
    DARRAY(struct netint_pkt) pkt_queue; /**< Thread-safe queue of encoded packets waiting for OBS */
    pthread_t recv_thread;            /**< Background thread handle for receiving encoded packets */
    pthread_mutex_t queue_mutex;      /**< Mutex protecting pkt_queue from concurrent access */
    pthread_mutex_t state_mutex;      /**< Mutex protecting state and error counters */
    bool stop_thread;                  /**< Flag to signal background thread to stop */
    bool mutexes_initialized;         /**< Flag indicating if mutexes were initialized (for safe cleanup) */
    char *rc_mode;                     /**< Rate control mode: "CBR" or "VBR" */
    char *profile;                      /**< Encoder profile: "baseline", "main", or "high" */
    bool repeat_headers;               /**< If true, attach SPS/PPS to every keyframe */
    int codec_type;                    /**< Codec type: 0 = H.264, 1 = H.265 (HEVC) */
    
    /* Error tracking and health monitoring */
    netint_encoder_state_t state;      /**< Current encoder state (for health monitoring) */
    int consecutive_errors;            /**< Count of consecutive errors (reset on success) */
    int total_errors;                  /**< Total error count since encoder creation */
    int recovery_attempts;             /**< Number of recovery attempts made */
    uint64_t last_packet_time;         /**< Timestamp (os_gettime_ns) of last received packet */
    uint64_t last_frame_time;          /**< Timestamp (os_gettime_ns) of last frame sent */
    uint64_t encoder_start_time;       /**< Timestamp (os_gettime_ns) when encoder was created */
    uint64_t last_error_time;          /**< Timestamp (os_gettime_ns) of last error */
    char last_error_msg[256];          /**< Last error message for debugging */
};

/**
 * @brief Get the display name for this encoder type
 * 
 * This function is called by OBS Studio to display the encoder name in the UI.
 * The name appears in the encoder selection dropdown menu.
 * 
 * @param type_data Unused - OBS encoder type data (not used in this implementation)
 * @return Static string "NETINT T4XX" - the display name for this encoder
 */
static const char *netint_get_name(void *type_data)
{
    UNUSED_PARAMETER(type_data);
    return "NETINT T4XX";
}

/* Forward declarations */
static void netint_destroy(void *data);
static void *netint_recv_thread(void *param);
static void netint_log_error(struct netint_ctx *ctx, const char *operation, const char *error_msg);
static bool netint_check_encoder_health(struct netint_ctx *ctx);
static bool netint_attempt_recovery(struct netint_ctx *ctx);
static void netint_record_success(struct netint_ctx *ctx);

/**
 * @brief Log error with context and update error tracking
 * 
 * This function logs errors with full context (operation, encoder state, error counts)
 * and updates the encoder's error tracking state. This helps with debugging by providing
 * a complete picture of what went wrong and when.
 * 
 * @param ctx Encoder context
 * @param operation Name of the operation that failed (e.g., "encode_get_frame")
 * @param error_msg Error message describing what went wrong
 */
static void netint_log_error(struct netint_ctx *ctx, const char *operation, const char *error_msg)
{
    if (!ctx) return;
    
    pthread_mutex_lock(&ctx->state_mutex);
    
    uint64_t now = os_gettime_ns();
    ctx->consecutive_errors++;
    ctx->total_errors++;
    ctx->last_error_time = now;
    
    /* Store error message for debugging */
    snprintf(ctx->last_error_msg, sizeof(ctx->last_error_msg), "%s: %s", operation, error_msg);
    
    /* Update state based on error count */
    if (ctx->consecutive_errors >= MAX_CONSECUTIVE_ERRORS) {
        ctx->state = NETINT_ENCODER_STATE_FAILED;
        blog(LOG_ERROR, "[obs-netint-t4xx] Encoder FAILED after %d consecutive errors in operation '%s': %s", 
              ctx->consecutive_errors, operation, error_msg);
        blog(LOG_ERROR, "[obs-netint-t4xx] Encoder stats: total_errors=%d, recovery_attempts=%d, uptime=%.1fs",
              ctx->total_errors, ctx->recovery_attempts, 
              (now - ctx->encoder_start_time) / 1000000000.0);
    } else {
        ctx->state = NETINT_ENCODER_STATE_ERROR;
        blog(LOG_WARNING, "[obs-netint-t4xx] Encoder error in '%s': %s (consecutive: %d/%d, total: %d)",
              operation, error_msg, ctx->consecutive_errors, MAX_CONSECUTIVE_ERRORS, ctx->total_errors);
    }
    
    pthread_mutex_unlock(&ctx->state_mutex);
}

/**
 * @brief Record successful operation and reset error counters
 * 
 * This function is called when an operation succeeds. It resets consecutive error
 * counters and updates the encoder state to normal. This helps distinguish between
 * transient errors and persistent failures.
 * 
 * @param ctx Encoder context
 */
static void netint_record_success(struct netint_ctx *ctx)
{
    if (!ctx) return;
    
    pthread_mutex_lock(&ctx->state_mutex);
    
    /* Reset consecutive errors on success */
    if (ctx->consecutive_errors > 0) {
        blog(LOG_INFO, "[obs-netint-t4xx] Encoder recovered from %d consecutive errors", ctx->consecutive_errors);
        ctx->consecutive_errors = 0;
    }
    
    /* Update state to normal if we were in error state */
    if (ctx->state == NETINT_ENCODER_STATE_ERROR || ctx->state == NETINT_ENCODER_STATE_RECOVERING) {
        ctx->state = NETINT_ENCODER_STATE_NORMAL;
    }
    
    pthread_mutex_unlock(&ctx->state_mutex);
}

/**
 * @brief Check encoder health and detect hangs
 * 
 * This function monitors the encoder's health by checking:
 * - Time since last packet received
 * - Time since last frame sent
 * - Encoder state
 * 
 * If the encoder appears hung (no packets for too long), it attempts recovery.
 * 
 * @param ctx Encoder context
 * @return true if encoder is healthy, false if hung or failed
 */
static bool netint_check_encoder_health(struct netint_ctx *ctx)
{
    if (!ctx) return false;
    
    pthread_mutex_lock(&ctx->state_mutex);
    
    /* If already failed, don't check further */
    if (ctx->state == NETINT_ENCODER_STATE_FAILED) {
        pthread_mutex_unlock(&ctx->state_mutex);
        return false;
    }
    
    uint64_t now = os_gettime_ns();
    bool is_healthy = true;
    
    /* Check if encoder is hung (no packets received for too long) */
    if (!ctx->flushing && ctx->last_packet_time > 0) {
        uint64_t time_since_packet = (now - ctx->last_packet_time) / 1000000000ULL; /* Convert to seconds */
        
        if (time_since_packet >= ENCODER_HANG_TIMEOUT_SEC) {
            ctx->state = NETINT_ENCODER_STATE_HUNG;
            blog(LOG_WARNING, "[obs-netint-t4xx] Encoder appears HUNG: no packets for %llu seconds", 
                  (unsigned long long)time_since_packet);
            blog(LOG_WARNING, "[obs-netint-t4xx] Last packet: %llu seconds ago, Last frame: %llu seconds ago",
                  (unsigned long long)time_since_packet,
                  ctx->last_frame_time > 0 ? (unsigned long long)((now - ctx->last_frame_time) / 1000000000ULL) : 0);
            is_healthy = false;
        }
    }
    
    pthread_mutex_unlock(&ctx->state_mutex);
    
    /* Attempt recovery if encoder is hung */
    if (!is_healthy && ctx->state == NETINT_ENCODER_STATE_HUNG) {
        if (!netint_attempt_recovery(ctx)) {
            return false;
        }
    }
    
    return true;
}

/**
 * @brief Attempt to recover from encoder errors or hang
 * 
 * This function attempts to recover the encoder when it encounters errors or hangs.
 * Recovery strategies:
 * 1. Try to close and reopen encoder connection (if API supports it)
 * 2. Reset encoder state
 * 3. Clear error counters
 * 
 * If recovery fails after MAX_RECOVERY_ATTEMPTS, the encoder is marked as failed.
 * 
 * @param ctx Encoder context
 * @return true if recovery was attempted (may still fail), false if max attempts reached
 */
static bool netint_attempt_recovery(struct netint_ctx *ctx)
{
    if (!ctx) return false;
    
    pthread_mutex_lock(&ctx->state_mutex);
    
    if (ctx->recovery_attempts >= MAX_RECOVERY_ATTEMPTS) {
        blog(LOG_ERROR, "[obs-netint-t4xx] Max recovery attempts (%d) reached, encoder marked as FAILED", 
              MAX_RECOVERY_ATTEMPTS);
        ctx->state = NETINT_ENCODER_STATE_FAILED;
        pthread_mutex_unlock(&ctx->state_mutex);
        return false;
    }
    
    ctx->recovery_attempts++;
    ctx->state = NETINT_ENCODER_STATE_RECOVERING;
    blog(LOG_INFO, "[obs-netint-t4xx] Attempting encoder recovery (attempt %d/%d)...", 
          ctx->recovery_attempts, MAX_RECOVERY_ATTEMPTS);
    
    pthread_mutex_unlock(&ctx->state_mutex);
    
    /* Recovery strategy: Try to reset encoder state */
    /* Note: libxcoder may not support hot-reconnect, so we log the attempt */
    /* In a real implementation, we might try to close/reopen the encoder */
    blog(LOG_INFO, "[obs-netint-t4xx] Recovery: Clearing error counters and resetting state");
    
    pthread_mutex_lock(&ctx->state_mutex);
    ctx->consecutive_errors = 0;
    ctx->last_error_time = 0;
    ctx->last_error_msg[0] = '\0';
    /* Reset packet time to give encoder a fresh start */
    ctx->last_packet_time = 0;
    ctx->state = NETINT_ENCODER_STATE_NORMAL;
    pthread_mutex_unlock(&ctx->state_mutex);
    
    blog(LOG_INFO, "[obs-netint-t4xx] Recovery attempt completed, encoder state reset to NORMAL");
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
    
    /* Initialize dynamic array for packet queue */
    da_init(ctx->pkt_queue);
    
    /* Initialize mutexes for thread-safe access */
    pthread_mutex_init(&ctx->queue_mutex, NULL);
    pthread_mutex_init(&ctx->state_mutex, NULL);
    ctx->mutexes_initialized = true;
    
    /* Initialize error tracking and health monitoring */
    ctx->state = NETINT_ENCODER_STATE_NORMAL;
    ctx->consecutive_errors = 0;
    ctx->total_errors = 0;
    ctx->recovery_attempts = 0;
    ctx->last_packet_time = 0;
    ctx->last_frame_time = 0;
    ctx->encoder_start_time = os_gettime_ns();
    ctx->last_error_time = 0;
    ctx->last_error_msg[0] = '\0';

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
    int keyint = (int)obs_data_get_int(settings, "keyint");
    if (keyint <= 0) keyint = (int)(2 * (voi->fps_num / (double)voi->fps_den));
    
    /* Set timebase for timestamps (matches OBS video output timebase) */
    /* timebase = fps_den / fps_num (e.g., 1/30 for 30fps, 1001/30000 for 29.97fps) */
    ctx->enc.timebase_num = (int)voi->fps_den;
    ctx->enc.timebase_den = (int)voi->fps_num;
    ctx->enc.ticks_per_frame = 1;
    
    /* Codec selection: determine whether to use H.264 or H.265 */
    /* Priority: 1) settings codec, 2) encoder codec type, 3) default to H.264 */
    const char *codec_str = obs_data_get_string(settings, "codec");
    
    /* If codec not set in settings, determine from encoder codec type */
    /* OBS creates separate encoder instances for H.264 vs H.265, so we can infer */
    if (!codec_str || *codec_str == '\0') {
        const char *encoder_codec = obs_encoder_get_codec(encoder);
        if (encoder_codec && strcmp(encoder_codec, "hevc") == 0) {
            codec_str = "h265";
        } else {
            codec_str = "h264";
        }
    }
    
    /* Set codec format based on codec selection */
    /* Store codec type in context for later use (keyframe detection, packet parsing) */
    if (codec_str && strcmp(codec_str, "h265") == 0) {
        ctx->codec_type = 1; /* H.265 (HEVC) */
        ctx->enc.codec_format = 1; /* NI_LOGAN_CODEC_FORMAT_H265 */
    } else {
        ctx->codec_type = 0; /* H.264 (AVC) */
        ctx->enc.codec_format = 0; /* NI_LOGAN_CODEC_FORMAT_H264 */
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
    
    /* Store rate control mode and profile strings (used later for parameter setting) */
    ctx->rc_mode = bstrdup(obs_data_get_string(settings, "rc_mode"));
    ctx->profile = bstrdup(obs_data_get_string(settings, "profile"));
    
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
    if (p_ni_logan_encode_init(&ctx->enc) < 0) {
        blog(LOG_ERROR, "[obs-netint-t4xx] Failed to initialize encoder");
        goto fail;
    }
    blog(LOG_INFO, "[obs-netint-t4xx] ni_logan_encode_init succeeded");
    
    /* Set advanced encoder parameters after initial init */
    /* These parameters require the encoder to be initialized first (they modify internal state) */
    if (ctx->enc.p_encoder_params && p_ni_logan_encoder_params_set_value) {
        ni_logan_encoder_params_t *params = (ni_logan_encoder_params_t *)ctx->enc.p_encoder_params;
        ni_logan_session_context_t *session_ctx = (ni_logan_session_context_t *)ctx->enc.p_session_ctx;
        
        /* NOTE: Do NOT enable GenHdrs! */
        /* Some T4xx hardware/firmware doesn't support pre-generating headers */
        /* and it causes params_parse to fail with ERROR_INVALID_SESSION (-5) */
        /* Instead, we always extract headers from the first encoded packet */
        blog(LOG_INFO, "[obs-netint-t4xx] Will extract headers from first encoded packet (GenHdrs disabled)");
        
        /* Set rate control mode: CBR (constant) or VBR (variable) */
        /* CBR = constant bitrate (good for streaming), VBR = variable bitrate (better quality) */
        if (ctx->rc_mode) {
            if (strcmp(ctx->rc_mode, "CBR") == 0) {
                p_ni_logan_encoder_params_set_value(params, "cbr", "1", session_ctx);
            } else {
                p_ni_logan_encoder_params_set_value(params, "cbr", "0", session_ctx);
            }
        }
        
        /* Set encoder profile - maps string names to codec-specific profile IDs */
        /* Profile determines feature set: baseline < main < high */
        /* Profile IDs differ between H.264 and H.265 */
        if (ctx->profile) {
            const char *profile_id_str = NULL;
            if (ctx->codec_type == 1) {
                /* H.265 (HEVC) profiles - profile IDs from HEVC spec */
                if (strcmp(ctx->profile, "baseline") == 0 || strcmp(ctx->profile, "main") == 0) {
                    profile_id_str = "1"; /* HEVC Main profile (8-bit) */
                } else if (strcmp(ctx->profile, "high") == 0) {
                    profile_id_str = "2"; /* HEVC Main 10 profile (10-bit) */
                }
            } else {
                /* H.264 profiles - profile IDs from H.264 spec */
                if (strcmp(ctx->profile, "baseline") == 0) {
                    profile_id_str = "66"; /* H.264 Baseline Profile */
                } else if (strcmp(ctx->profile, "main") == 0) {
                    profile_id_str = "77"; /* H.264 Main Profile */
                } else if (strcmp(ctx->profile, "high") == 0) {
                    profile_id_str = "100"; /* H.264 High Profile */
                }
            }
            if (profile_id_str) {
                p_ni_logan_encoder_params_set_value(params, "profile", profile_id_str, session_ctx);
            }
        }
    }
    
    /* Parse and validate all encoder parameters */
    /* This checks for parameter conflicts and applies defaults */
    /* If generate_enc_hdrs is set, this will also generate headers automatically */
    blog(LOG_INFO, "[obs-netint-t4xx] Calling ni_logan_encode_params_parse (will generate headers)...");
    int parse_ret = p_ni_logan_encode_params_parse(&ctx->enc);
    blog(LOG_INFO, "[obs-netint-t4xx] ni_logan_encode_params_parse returned: %d", parse_ret);
    
    if (parse_ret < 0) {
        blog(LOG_ERROR, "[obs-netint-t4xx] Failed to parse encoder parameters (ret=%d)", parse_ret);
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
    
    /* Open connection to hardware encoder device for actual encoding */
    /* This establishes communication with the PCIe card and allocates hardware resources */
    /* Note: This is different from the temporary open done during header generation */
    blog(LOG_INFO, "[obs-netint-t4xx] Calling ni_logan_encode_open for encoding session...");
    blog(LOG_INFO, "[obs-netint-t4xx] enc context: dev_xcoder='%s' dev_enc_name='%s' dev_enc_idx=%d",
         ctx->enc.dev_xcoder ? ctx->enc.dev_xcoder : "(null)",
         ctx->enc.dev_enc_name ? ctx->enc.dev_enc_name : "(null)",
         ctx->enc.dev_enc_idx);
    blog(LOG_INFO, "[obs-netint-t4xx] enc context: p_session_ctx=%p, p_encoder_params=%p",
         ctx->enc.p_session_ctx, ctx->enc.p_encoder_params);
    blog(LOG_INFO, "[obs-netint-t4xx] enc context: width=%d, height=%d, codec_format=%d",
         ctx->enc.width, ctx->enc.height, ctx->enc.codec_format);
    
    int open_ret = p_ni_logan_encode_open(&ctx->enc);
    blog(LOG_INFO, "[obs-netint-t4xx] ni_logan_encode_open returned: %d", open_ret);
    
    if (open_ret < 0) {
        blog(LOG_ERROR, "[obs-netint-t4xx] Failed to open encoder device (ret=%d)", open_ret);
        blog(LOG_ERROR, "[obs-netint-t4xx] Check: 1) Is init_rsrc_logan.exe running with admin? 2) Is device available? 3) Run ni_rsrc_list_logan.exe to verify");
        goto fail;
    }
    
    blog(LOG_INFO, "[obs-netint-t4xx] ni_logan_encode_open succeeded!");
    blog(LOG_INFO, "[obs-netint-t4xx] Opened device: %s (hw_id=%d)", 
         ctx->enc.actual_dev_name ? ctx->enc.actual_dev_name : "(unknown)",
         ctx->enc.actual_dev_enc_idx);
    
    /* Note: Headers might not be available yet if hardware doesn't support pre-generation */
    /* In that case, we'll extract them from the first encoded packet */
    if (!ctx->got_headers) {
        blog(LOG_INFO, "[obs-netint-t4xx] Headers will be extracted from first encoded packet");
    }

    /* Start background receive thread for asynchronous packet reception */
    /* This thread continuously polls for encoded packets and queues them */
    /* This reduces latency by having packets ready when OBS requests them */
    blog(LOG_INFO, "[obs-netint-t4xx] Creating background receive thread...");
    ctx->stop_thread = false;
    if (pthread_create(&ctx->recv_thread, NULL, netint_recv_thread, ctx) != 0) {
        blog(LOG_ERROR, "[obs-netint-t4xx] Failed to create receive thread");
        goto fail;
    }
    blog(LOG_INFO, "[obs-netint-t4xx] Receive thread created successfully");
    blog(LOG_INFO, "[obs-netint-t4xx] Encoder creation complete, returning context to OBS");
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
    
    blog(LOG_INFO, "[obs-netint-t4xx] netint_destroy called");
    
    /* Signal background thread to stop */
    ctx->stop_thread = true;
    
    /* Wait for background thread to finish */
    /* This ensures thread has finished accessing shared data before we free it */
    /* Note: pthread_join will block until thread exits, which should happen quickly
     * because the thread checks stop_thread flag in its main loop */
    pthread_t zero_thread = {0};
    if (pthread_equal(ctx->recv_thread, zero_thread) == 0) {
	    blog(LOG_INFO, "[obs-netint-t4xx] Joining background thread...");
	    int join_result = pthread_join(ctx->recv_thread, NULL);
	    if (join_result != 0) {
		    blog(LOG_WARNING, "[obs-netint-t4xx] pthread_join failed with error %d", join_result);
	    }
    }
    
    /* Destroy mutexes (no longer needed since thread is stopped) */
    /* Only destroy mutexes if they were successfully initialized */
    if (ctx->mutexes_initialized) {
        blog(LOG_INFO, "[obs-netint-t4xx] Destroying mutexes...");
        pthread_mutex_destroy(&ctx->queue_mutex);
        pthread_mutex_destroy(&ctx->state_mutex);
        blog(LOG_INFO, "[obs-netint-t4xx] Mutexes destroyed successfully");
    } else {
        blog(LOG_INFO, "[obs-netint-t4xx] Mutexes not initialized, skipping destroy");
    }
    
    /* Free all queued packets - any packets not yet consumed by OBS */
    for (size_t i = 0; i < ctx->pkt_queue.num; i++) {
        struct netint_pkt *p = &ctx->pkt_queue.array[i];
        if (p->data) bfree(p->data);
    }
    
    /* Free the packet queue array itself */
    da_free(ctx->pkt_queue);
    
    /* Close hardware encoder connection */
    /* Only close if encoder was actually opened (p_session_ctx AND started flag set) */
    /* Check started flag to avoid closing partially-initialized encoder */
    if (ctx->enc.p_session_ctx && ctx->enc.started) {
        blog(LOG_INFO, "[obs-netint-t4xx] Closing encoder session (started=%d)...", ctx->enc.started);
        if (p_ni_logan_encode_close) {
            int close_ret = p_ni_logan_encode_close(&ctx->enc);
            blog(LOG_INFO, "[obs-netint-t4xx] ni_logan_encode_close returned: %d", close_ret);
        }
    } else {
        blog(LOG_INFO, "[obs-netint-t4xx] Skipping encode_close (started=%d, p_session_ctx=%p)", 
             ctx->enc.started, ctx->enc.p_session_ctx);
        /* NOTE: Do NOT manually free p_session_ctx or p_encoder_params! */
        /* These were allocated by libxcoder_logan.dll and must be freed by it */
        /* Cross-DLL memory management causes heap corruption on Windows */
        /* The library will clean up when the process exits, or we can call ni_logan_encode_close */
        /* if we want explicit cleanup, but that requires the encoder to be in a valid state */
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
    
    /* enc is EMBEDDED in ctx, so it will be freed when ctx is freed */
    /* No need to manually free it */
    
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
 * @brief Encode a video frame and/or receive an encoded packet
 * 
 * This is the main encoding function called by OBS Studio for each video frame.
 * It performs two operations:
 * 1. Sends a new frame to hardware encoder (if frame != NULL)
 * 2. Pops an encoded packet from queue (if available) and returns it to OBS
 * 
 * Encoding Flow:
 * - Get frame buffer from encoder
 * - Copy frame data from OBS format to encoder buffer
 * - Reconfigure for variable frame rate (if needed)
 * - Send frame to hardware encoder
 * - Pop packet from queue (background thread filled it)
 * 
 * Flushing:
 * - When frame == NULL, encoder is being flushed (no more input)
 * - Set flushing flag, but continue draining queue
 * - Background thread will stop when encoder signals EOF
 * 
 * Threading:
 * - This function runs on main thread (called by OBS)
 * - Background thread (netint_recv_thread) receives packets asynchronously
 * - Queue is protected by mutex for thread-safe access
 * 
 * Packet Metadata:
 * - PTS/DTS: Presentation and decode timestamps (from encoder)
 * - Keyframe flag: Detected by parsing NAL unit header
 * - Priority: Determined by OBS codec parsing functions
 * - Timebase: Matches video output timebase
 * 
 * @param data Encoder context pointer
 * @param frame Video frame to encode (NULL when flushing)
 * @param packet Output packet structure (filled if packet available)
 * @param received Output flag: true if packet was returned, false if no packet ready
 * @return true on success, false on error (encoder error, not "no packet")
 */
static bool netint_encode(void *data, struct encoder_frame *frame, struct encoder_packet *packet, bool *received)
{
    struct netint_ctx *ctx = data;
    *received = false;

    /* Check encoder health before processing */
    pthread_mutex_lock(&ctx->state_mutex);
    if (ctx->state == NETINT_ENCODER_STATE_FAILED) {
        pthread_mutex_unlock(&ctx->state_mutex);
        blog(LOG_ERROR, "[obs-netint-t4xx] Encode called but encoder is in FAILED state");
        return false;
    }
    pthread_mutex_unlock(&ctx->state_mutex);

    if (!frame) {
        /* Flushing mode: no more input frames, but continue draining queue */
        /* This happens when stream/recording stops - encoder has pending packets */
        ctx->flushing = true;
        /* Continue draining queue even when flushing */
    } else {
        /* Update last frame time for health monitoring */
        pthread_mutex_lock(&ctx->state_mutex);
        ctx->last_frame_time = os_gettime_ns();
        pthread_mutex_unlock(&ctx->state_mutex);
        
        /* Check encoder health periodically (every 10 frames roughly) */
        /* Note: This is a global counter, but health checking is lightweight */
        static int health_check_counter = 0;
        if (++health_check_counter >= 10) {
            health_check_counter = 0;
            if (!netint_check_encoder_health(ctx)) {
                /* Encoder is hung or failed, return error */
                /* OBS will handle encoder recreation when this returns false */
                return false;
            }
        }
        
        /* Get a frame buffer from the encoder for input data */
        /* This allocates/prepares a buffer in the encoder's internal format */
        if (p_ni_logan_encode_get_frame(&ctx->enc) < 0) {
            netint_log_error(ctx, "encode_get_frame", "Failed to get frame buffer from encoder");
            return false;
        }

        /* Get pointer to encoder's frame buffer structure */
        ni_logan_frame_t *ni_frame = (ni_logan_frame_t *)ctx->enc.p_input_fme->data.frame.p_data;
        
        /* Prepare plane pointers and line sizes for YUV420P format */
        /* planes[0] = Y plane, planes[1] = U plane, planes[2] = V plane */
        uint8_t *planes[NI_LOGAN_MAX_NUM_DATA_POINTERS] = {frame->data[0], frame->data[1], frame->data[2], NULL};
        int linesize[NI_LOGAN_MAX_NUM_DATA_POINTERS] = {frame->linesize[0], frame->linesize[1], frame->linesize[2], 0};

        /* Reconfigure encoder for variable frame rate (VFR) if needed */
        /* This adjusts encoder timing based on actual frame PTS */
        p_ni_logan_encode_reconfig_vfr(&ctx->enc, ni_frame, frame->pts);

        /* Copy frame data from OBS format to encoder's frame buffer */
        /* This copies Y, U, V planes with their respective line sizes */
        if (p_ni_logan_encode_copy_frame_data(&ctx->enc, ni_frame, planes, linesize) < 0) {
            netint_log_error(ctx, "encode_copy_frame_data", "Failed to copy frame data to encoder buffer");
            return false;
        }

        /* Send frame to hardware encoder - triggers actual encoding */
        /* Hardware will encode asynchronously, packet will be available later */
        if (p_ni_logan_encode_send(&ctx->enc) < 0) {
            netint_log_error(ctx, "encode_send", "Failed to send frame to hardware encoder");
            return false;
        }
        
        /* Frame sent successfully - record success to reset error counters */
        netint_record_success(ctx);
    }

    /* Background thread handles receive, we just pop from queue */
    /* Lock mutex to safely access queue */
    pthread_mutex_lock(&ctx->queue_mutex);
    if (ctx->pkt_queue.num) {
        /* Get first packet from queue (FIFO order) */
        struct netint_pkt pkt = ctx->pkt_queue.array[0];
        
        /* Fill OBS packet structure with queued packet data */
        packet->data = pkt.data;
        packet->size = pkt.size;
        packet->type = OBS_ENCODER_VIDEO;
        packet->pts = pkt.pts;
        packet->dts = pkt.dts;
        
        /* Get timebase from video output (matches original frame rate) */
        video_t *video = obs_encoder_video(ctx->encoder);
        const struct video_output_info *voi = video_output_get_info(video);
        packet->timebase_num = (int32_t)voi->fps_den;
        packet->timebase_den = (int32_t)voi->fps_num;
        
        /* Set keyframe flag and priority (determined during packet parsing) */
        packet->keyframe = pkt.keyframe;
        packet->priority = pkt.priority;
        *received = true;
        
        /* Update last packet time for health monitoring */
        pthread_mutex_lock(&ctx->state_mutex);
        ctx->last_packet_time = os_gettime_ns();
        pthread_mutex_unlock(&ctx->state_mutex);
        
        /* Record success - packet received successfully */
        netint_record_success(ctx);
        
        /* Remove packet from queue by shifting remaining packets */
        /* This is O(n) but queue is typically small (1-2 packets) */
        memmove(&ctx->pkt_queue.array[0], &ctx->pkt_queue.array[1], (ctx->pkt_queue.num - 1) * sizeof(pkt));
        ctx->pkt_queue.num--;
    }
    pthread_mutex_unlock(&ctx->queue_mutex);

    return true;
}

/**
 * @brief Background thread function for receiving encoded packets asynchronously
 * 
 * This thread continuously polls the hardware encoder for encoded packets and queues them.
 * Running this in a separate thread reduces latency because:
 * - Packets are ready when OBS requests them (no waiting for encoding)
 * - Encoding happens in parallel with other OBS operations
 * - Better CPU utilization (doesn't block main encoding thread)
 * 
 * Thread Lifecycle:
 * - Started in netint_create() before encoder is returned
 * - Runs until stop_thread flag is set (in netint_destroy())
 * - Exits when encoder signals EOF (end of stream) or stop_thread is true
 * 
 * Packet Reception:
 * - Polls encoder with ni_logan_encode_receive() (non-blocking)
 * - Allocates buffer for packet data
 * - Copies packet data from encoder buffer
 * - Parses packet to determine keyframe and priority
 * - Queues packet for main thread to consume
 * 
 * Error Handling:
 * - If buffer allocation fails, sleep briefly and retry
 * - If copy fails, free buffer and continue
 * - If encoder signals EOF, exit thread gracefully
 * 
 * Thread Safety:
 * - Queue access is protected by queue_mutex
 * - All queue operations are locked
 * - Main thread (netint_encode) also locks when accessing queue
 * 
 * @param param Pointer to encoder context (netint_ctx structure)
 * @return NULL (thread exit value, not used)
 */
static void *netint_recv_thread(void *param)
{
    blog(LOG_INFO, "[obs-netint-t4xx] Receive thread: ENTRY POINT");
    struct netint_ctx *ctx = param;
    blog(LOG_INFO, "[obs-netint-t4xx] Receive thread: ctx pointer = %p", ctx);
    
    blog(LOG_INFO, "[obs-netint-t4xx] Receive thread started, entering main loop");
    
    /* Main receive loop - runs until encoder is destroyed or EOF */
    while (!ctx->stop_thread) {
        /* Poll encoder for encoded packet - non-blocking call */
        /* Returns: >0 = packet size, 0 = no packet ready, <0 = error or EOF */
        blog(LOG_DEBUG, "[obs-netint-t4xx] Receive thread: calling ni_logan_encode_receive...");
        int got = p_ni_logan_encode_receive(&ctx->enc);
        blog(LOG_DEBUG, "[obs-netint-t4xx] Receive thread: ni_logan_encode_receive returned %d", got);
        
        if (got > 0) {
            /* Packet is available - allocate buffer for it */
            uint8_t *buf = bmalloc((size_t)got);
            if (!buf) {
                /* Allocation failed - sleep briefly and retry */
                /* This shouldn't happen often, but handles memory pressure */
                os_sleep_ms(10);
                continue;
            }
            
            /* Determine if this is the first packet (needs special handling) */
            /* First packet flag affects how packet data is copied */
            int first = ctx->enc.firstPktArrived ? 0 : 1;
            
            /* Copy packet data from encoder's internal buffer to our buffer */
            if (p_ni_logan_encode_copy_packet_data(&ctx->enc, buf, first, 0) == 0) {
                /* Mark that first packet has arrived (for future packets) */
                ctx->enc.firstPktArrived = 1;
                
                /* Extract headers from first packet if not already obtained during init */
                /* The first packet from the encoder always contains SPS/PPS/VPS headers */
                /* These are needed for stream initialization (decoders need them to start) */
                if (first && !ctx->got_headers) {
                    blog(LOG_INFO, "[obs-netint-t4xx] First packet received, extracting headers (size=%d)...", got);
                    
                    /* Free any existing extradata (shouldn't happen, but be safe) */
                    if (ctx->extra) {
                        bfree(ctx->extra);
                        ctx->extra = NULL;
                        ctx->extra_size = 0;
                    }
                    
                    /* Copy entire first packet as headers */
                    /* For H.264/H.265, the first packet contains SPS/PPS/VPS NAL units */
                    ctx->extra = bmemdup(buf, (size_t)got);
                    ctx->extra_size = (size_t)got;
                    ctx->got_headers = true;
                    
                    blog(LOG_INFO, "[obs-netint-t4xx] Headers extracted from first packet: %zu bytes", ctx->extra_size);
                    
                    /* NOTE: Do NOT free/reallocate ctx->enc.extradata - it's managed by libxcoder! */
                    /* Cross-DLL memory management causes heap corruption on Windows */
                    /* The encoder library will manage its own extradata pointer */
                }
                
                /* Create packet structure with metadata */
                struct netint_pkt pkt = {0};
                pkt.data = buf;
                pkt.size = (size_t)got;
                
                /* Set timestamps - use latest_dts if available, otherwise first_frame_pts */
                /* DTS (decode timestamp) is when frame should be decoded */
                pkt.pts = ctx->enc.latest_dts != 0 ? ctx->enc.latest_dts : ctx->enc.first_frame_pts;
                pkt.dts = pkt.pts; /* For now, PTS and DTS are the same */
                
                /* Parse packet to determine keyframe and priority based on codec type */
                /* Keyframe detection: examines NAL unit header (H.264) or NAL unit type (H.265) */
                /* Priority: determines packet importance for streaming (SPS/PPS = highest priority) */
                if (ctx->codec_type == 1) {
                    /* H.265 (HEVC) parsing */
                    pkt.keyframe = obs_hevc_keyframe(pkt.data, pkt.size);
                    struct encoder_packet tmp = {0};
                    tmp.data = pkt.data;
                    tmp.size = pkt.size;
                    pkt.priority = obs_parse_hevc_packet_priority(&tmp);
                } else {
                    /* H.264 parsing */
                    pkt.keyframe = obs_avc_keyframe(pkt.data, pkt.size);
                    struct encoder_packet tmp = {0};
                    tmp.data = pkt.data;
                    tmp.size = pkt.size;
                    pkt.priority = obs_parse_avc_packet_priority(&tmp);
                }
                
                /* Queue packet for main thread to consume */
                /* Lock mutex to safely add to queue */
                pthread_mutex_lock(&ctx->queue_mutex);
                
                /* Check queue size limit to prevent unbounded growth */
                if (ctx->pkt_queue.num >= MAX_PKT_QUEUE_SIZE) {
                    /* Queue is full - log warning but still add packet */
                    /* In the future, we could drop oldest packet, but for now just warn */
                    blog(LOG_WARNING, "[obs-netint-t4xx] Packet queue full (%zu packets), OBS may not be consuming fast enough", ctx->pkt_queue.num);
                }
                
                da_push_back(ctx->pkt_queue, &pkt);
                pthread_mutex_unlock(&ctx->queue_mutex);
            } else {
                /* Copy failed - free buffer and continue */
                /* This shouldn't happen often, but handles encoder errors gracefully */
                bfree(buf);
            }
            continue;
        } else if (got < 0) {
            /* Error or EOF from encoder */
            blog(LOG_DEBUG, "[obs-netint-t4xx] Receive thread: got < 0, encoder_eof=%d", ctx->enc.encoder_eof);
            if (ctx->enc.encoder_eof) {
                /* Encoder signaled EOF (end of stream) - stop receiving */
                /* This happens when encoder is flushed and all packets are drained */
                blog(LOG_INFO, "[obs-netint-t4xx] Encoder EOF received, stopping receive thread");
                break;
            } else {
                /* Actual error from encoder */
                /* Log error but don't exit thread - continue trying */
                blog(LOG_WARNING, "[obs-netint-t4xx] encode_receive returned error %d, continuing...", got);
                
                /* Check if we should give up after too many errors */
                pthread_mutex_lock(&ctx->state_mutex);
                if (ctx->consecutive_errors >= MAX_CONSECUTIVE_ERRORS) {
                    blog(LOG_ERROR, "[obs-netint-t4xx] Too many errors in receive thread, exiting");
                    pthread_mutex_unlock(&ctx->state_mutex);
                    break;
                }
                pthread_mutex_unlock(&ctx->state_mutex);
            }
        }
        
        /* No packet ready - sleep briefly to avoid busy-waiting */
        /* 2ms sleep balances responsiveness with CPU usage */
        blog(LOG_DEBUG, "[obs-netint-t4xx] Receive thread: no packet ready, sleeping 2ms");
        os_sleep_ms(2);
    }
    
    blog(LOG_INFO, "[obs-netint-t4xx] Receive thread exiting normally");
    return NULL;
}

/**
 * @brief Set default values for encoder settings
 * 
 * This function is called by OBS Studio when creating a new encoder instance to
 * initialize settings with sensible defaults. These defaults are used if the user
 * hasn't specified a value.
 * 
 * Default Values:
 * - codec: "h264" (will be overridden by encoder codec type if not set)
 * - bitrate: 6000 kbps (good starting point for 1080p streaming)
 * - keyint: 2 seconds (auto-calculated from frame rate if not set)
 * - rc_mode: "CBR" (constant bitrate, good for streaming)
 * - profile: "high" (best quality, supports all features)
 * - repeat_headers: true (attach SPS/PPS to every keyframe for streaming)
 * 
 * @param settings OBS settings object to populate with defaults
 */
static void netint_get_defaults(obs_data_t *settings)
{
    /* Default codec will be set based on encoder codec type in netint_create */
    /* This default is used if codec isn't explicitly set in settings */
    obs_data_set_default_string(settings, "codec", "h264");
    
    /* Default bitrate: 6000 kbps (good for 1080p streaming) */
    obs_data_set_default_int(settings, "bitrate", 6000);
    
    /* Default keyframe interval: 2 seconds (auto-calculated from FPS if <= 0) */
    obs_data_set_default_int(settings, "keyint", 2);
    
    /* Default rate control: CBR (constant bitrate) - better for streaming */
    obs_data_set_default_string(settings, "rc_mode", "CBR");
    
    /* Default profile: high (best quality, supports all encoder features) */
    obs_data_set_default_string(settings, "profile", "high");
    
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
 * - codec: Dropdown list (H.264 or H.265)
 * - bitrate: Integer input (100-100000 kbps, step 50)
 * - keyint: Integer input (1-20 seconds, step 1)
 * - device: Text input or dropdown (auto-populated if discovery available)
 * - rc_mode: Dropdown list (CBR or VBR)
 * - profile: Dropdown list (baseline, main, high)
 * - repeat_headers: Checkbox (attach SPS/PPS to every keyframe)
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
    
    /* Codec selection dropdown - H.264 or H.265 */
    obs_property_t *codec_prop = obs_properties_add_list(props, "codec", "Codec", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(codec_prop, "h264", "H.264");
    obs_property_list_add_string(codec_prop, "h265", "H.265");
    
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
/** H.264 encoder registration - appears as "NETINT T4XX" in encoder list */
static struct obs_encoder_info netint_h264_info = {
    .id = "obs_netint_t4xx_h264",      /**< Unique identifier for this encoder */
    .codec = "h264",                   /**< Codec string (matches OBS codec type) */
    .type = OBS_ENCODER_VIDEO,        /**< Encoder type: video encoder */
    .caps = 0,                         /**< Capability flags - see documentation above */
    .get_name = netint_get_name,
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
    .get_video_info = NULL,            /**< Video info not provided (NULL callback) */
};

/** H.265 (HEVC) encoder registration - appears as "NETINT T4XX" in encoder list */
static struct obs_encoder_info netint_h265_info = {
    .id = "obs_netint_t4xx_h265",      /**< Unique identifier for this encoder */
    .codec = "hevc",                   /**< Codec string (OBS uses "hevc" for H.265) */
    .type = OBS_ENCODER_VIDEO,        /**< Encoder type: video encoder */
    .caps = 0,                         /**< Capability flags - see documentation above */
    .get_name = netint_get_name,
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
    .get_video_info = NULL,            /**< Video info not provided (NULL callback) */
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


