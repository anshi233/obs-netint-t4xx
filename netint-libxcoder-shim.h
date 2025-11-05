/**
 * @file netint-libxcoder-shim.h
 * @brief Type definitions and structures for NETINT libxcoder API
 * 
 * This header file contains minimal type definitions needed to interface with
 * the NETINT libxcoder_logan.so library. These definitions are based on the
 * actual libxcoder API but are redefined here so we don't need vendor headers
 * at compile time.
 * 
 * Compatibility:
 * - Tested with: NETINT T408 v3.5.0, v3.5.1
 * - Should work with: v3.4.x, v3.3.x (API appears stable)
 * - Dynamic loading ensures compatibility with multiple versions
 * 
 * Important: These types must match the actual libxcoder API. If the vendor
 * updates their API, these definitions may need to be updated accordingly.
 * 
 * Note on input_data_fifo field:
 * - v3.5.1 uses: ni_logan_fifo_buffer_t *input_data_fifo
 * - We use: void *input_data_fifo (more flexible, same size)
 * - This is safe because: we don't access this field directly, it's only
 *   used internally by libxcoder, and both are pointers (same size)
 * 
 * This is a "shim" header because it provides a thin compatibility layer
 * between our code and the vendor library without requiring their headers.
 */

#pragma once

#include <stdint.h>

/**
 * @name Library Constants
 * @brief Constants used by libxcoder API
 */
/*@{*/
/** Maximum number of data pointers (planes) for video frames */
#define NI_LOGAN_MAX_NUM_DATA_POINTERS 4

/** Maximum length of device name string */
#define NI_LOGAN_MAX_DEVICE_NAME_LEN 32
/*@}*/

/**
 * @brief Pixel format enumeration
 * 
 * Currently only YUV420P (planar YUV 4:2:0) is supported by the encoder.
 * This format has separate Y, U, and V planes.
 */
typedef enum {
    NI_LOGAN_PIX_FMT_YUV420P = 0,  /**< Planar YUV 4:2:0 format */
} ni_logan_pix_fmt_t;

/**
 * @brief Forward declaration for frame structure
 * 
 * This is an opaque type - the actual structure is defined in libxcoder.
 * We only need pointers to it, so a forward declaration is sufficient.
 */
typedef struct _ni_logan_frame ni_logan_frame_t;

/**
 * @brief Session data I/O structure
 * 
 * This structure is used to pass frame data to and from the encoder.
 * It contains nested structures for accessing the actual frame buffer.
 * 
 * The structure layout matches libxcoder's internal organization.
 */
typedef struct _ni_logan_session_data_io {
    struct {
        struct {
            void *p_data;  /**< Pointer to frame data (cast to ni_logan_frame_t *) */
        } frame;
    } data;
} ni_logan_session_data_io_t;

/**
 * @brief Main encoder context structure
 * 
 * This is the primary structure used to configure and control the hardware encoder.
 * It contains all encoder parameters, state, and handles.
 * 
 * The structure is initialized with encoding parameters, then passed to various
 * libxcoder functions to control encoding.
 * 
 * Note: Some fields are set by libxcoder internally and should not be modified
 * directly. Only fields documented as "input" should be set by our code.
 */
typedef struct _ni_logan_enc_context {
    /* Device selection fields */
    char *dev_xcoder;        /**< Device identifier (legacy, may be unused) */
    int  dev_enc_idx;        /**< Encoder device index (if using index instead of name) */
    char *dev_enc_name;      /**< Encoder device name (e.g., "/dev/ni_encoder0") - INPUT */
    int  keep_alive_timeout; /**< Keep-alive timeout in seconds */
    int  set_high_priority;  /**< Set high priority for encoder process */

    /* Timing and frame rate */
    int timebase_num;        /**< Timebase numerator (fps denominator) - INPUT */
    int timebase_den;        /**< Timebase denominator (fps numerator) - INPUT */
    int ticks_per_frame;    /**< Ticks per frame (usually 1) - INPUT */
    int64_t bit_rate;       /**< Target bitrate in bits per second - INPUT */

    /* Video parameters */
    int width;              /**< Video width in pixels - INPUT */
    int height;             /**< Video height in pixels - INPUT */
    int ff_log_level;       /**< FFmpeg log level (for debugging) */
    int codec_format;       /**< Codec format: 0=H.264, 1=H.265 - INPUT */
    int pix_fmt;            /**< Pixel format (NI_LOGAN_PIX_FMT_YUV420P) - INPUT */

    /* Color space parameters */
    int color_primaries;    /**< Color primaries (BT.709, BT.2020, etc.) */
    int color_trc;          /**< Color transfer characteristics */
    int color_space;        /**< Color space (YUV, RGB, etc.) */
    int color_range;        /**< Color range (limited/full) */

    /* Aspect ratio */
    int sar_num;            /**< Sample aspect ratio numerator */
    int sar_den;            /**< Sample aspect ratio denominator */

    /* Internal state (set by libxcoder) */
    void *p_session_ctx;                    /**< Session context (opaque, set by libxcoder) */
    void *p_encoder_params;                 /**< Encoder parameters (opaque, set by libxcoder) */
    ni_logan_session_data_io_t *p_input_fme; /**< Input frame buffer (set by libxcoder) */
    ni_logan_session_data_io_t  output_pkt;  /**< Output packet buffer (set by libxcoder) */
    void *input_data_fifo;                   /**< Input data FIFO (internal) - Compatible with ni_logan_fifo_buffer_t * in v3.5.1 */

    /* Encoding state flags */
    int started;                    /**< Encoder has started (set by libxcoder) */
    uint8_t *p_spsPpsHdr;          /**< SPS/PPS header data (set by libxcoder) */
    int spsPpsHdrLen;              /**< SPS/PPS header length (set by libxcoder) */
    int spsPpsAttach;              /**< Attach SPS/PPS to keyframes - INPUT */
    int spsPpsArrived;             /**< SPS/PPS headers have arrived (set by libxcoder) */
    int firstPktArrived;            /**< First packet has arrived (set by libxcoder) */
    int dts_offset;                /**< DTS offset (set by libxcoder) */
    int reconfigCount;              /**< Reconfiguration count (set by libxcoder) */
    uint64_t total_frames_received; /**< Total frames received (set by libxcoder) */
    int64_t first_frame_pts;        /**< PTS of first frame (set by libxcoder) */
    int64_t latest_dts;             /**< Latest DTS (set by libxcoder) */

    /* Configuration window (cropping) */
    int orig_conf_win_top;    /**< Original configuration window top */
    int orig_conf_win_bottom; /**< Original configuration window bottom */
    int orig_conf_win_left;   /**< Original configuration window left */
    int orig_conf_win_right;  /**< Original configuration window right */

    /* Extradata (SPS/PPS headers) */
    uint8_t *extradata;       /**< Extradata buffer (set by libxcoder after encode_header) */
    int extradata_size;       /**< Extradata size in bytes (set by libxcoder) */

    /* Status flags */
    int gotPacket;            /**< Got a packet (set by libxcoder) */
    int sentFrame;            /**< Sent a frame (set by libxcoder) */

    /* Frame rate (may be set by libxcoder) */
    int fps_number;           /**< FPS numerator */
    int fps_denominator;       /**< FPS denominator */

    /* Actual device info (set by libxcoder after opening) */
    int  actual_dev_enc_idx;   /**< Actual device index used (set by libxcoder) */
    char *actual_dev_name;     /**< Actual device name used (set by libxcoder) */

    /* End-of-stream flags */
    int eos_fme_received;      /**< End-of-stream frame received (set by libxcoder) */
    int encoder_flushing;      /**< Encoder is flushing (set by libxcoder) */
    int encoder_eof;           /**< Encoder has reached end-of-file (set by libxcoder) */
} ni_logan_enc_context_t;


