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
 * @brief Session data I/O structure - OPAQUE WRAPPER
 * 
 * In the real library, this is a union of ni_logan_frame_t and ni_logan_packet_t,
 * which are both HUGE structs (~400 bytes each).
 * 
 * CRITICAL: We MUST match the library's size EXACTLY or input_data_fifo will be
 * at the wrong offset in ni_logan_enc_context_t!
 * 
 * Library measurements (from runtime logs):
 *   sizeof(ni_logan_session_data_io_t) = 416 bytes
 *   This is embedded in ni_logan_enc_context_t as "output_pkt" field
 * 
 * We use opaque padding to avoid importing the massive frame/packet definitions.
 * 
 * ACCESSING FRAME DATA:
 * The library's actual structure has:
 *   union { ni_logan_frame_t frame; ni_logan_packet_t packet; } data;
 * And ni_logan_frame_t starts with: void *p_data[...];
 * 
 * To access p_data safely, cast to _ni_logan_session_data_io_accessor.
 */
typedef struct _ni_logan_session_data_io {
    uint8_t _opaque_union[416];  /**< Opaque padding matching library union size */
} ni_logan_session_data_io_t;

/**
 * @brief Note on accessing frame data from ni_logan_session_data_io_t
 * 
 * The library's actual structure is:
 *   typedef struct { union { ni_logan_frame_t frame; ni_logan_packet_t packet; } data; } ni_logan_session_data_io_t;
 * 
 * Library functions expect: ni_logan_frame_t * (pointer to frame)
 * The frame is the first member of the union, so it starts at offset 0.
 * 
 * To get ni_logan_frame_t * from ni_logan_session_data_io_t *p:
 *   Just cast: (ni_logan_frame_t *)p
 * 
 * This works because:
 *   - Union starts at offset 0 of the struct
 *   - Frame is first member of union (offset 0 from union start)
 *   - Therefore, &p->data.frame == p (same address)
 */

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
/**
 * @brief Forward declaration for FIFO buffer type
 */
typedef struct _ni_logan_fifo_buffer ni_logan_fifo_buffer_t;

/**
 * @brief Main encoder context - EXACT copy from ni_enc_api_logan.h lines 42-114
 * 
 * IMPORTANT: This struct definition MUST match the library exactly!
 * Any deviation causes struct layout corruption and crashes.
 */
typedef struct _ni_logan_enc_context {
    char *dev_xcoder;         /* from the user command, which device allocation method we use */
    int  dev_enc_idx;         /* index of the decoder on the xcoder card */
    char *dev_enc_name;       /* dev name of the xcoder card to use */
    int  keep_alive_timeout;  /* keep alive timeout setting */
    int  set_high_priority;   /*set_high_priority*/

    int timebase_num;
    int timebase_den;
    int ticks_per_frame;
    int64_t bit_rate;
    int width;
    int height;
    int ff_log_level; /* ffmpeg log level */
    int codec_format;
    int pix_fmt;

    // color metrics
    int color_primaries;
    int color_trc;
    int color_space;
    int color_range;

    // sample_aspect_ratio
    int sar_num;
    int sar_den;

    void *p_session_ctx;  /* ni_logan_session_context_t */
    void *p_encoder_params; /* ni_logan_encoder_params_t */
    ni_logan_session_data_io_t *p_input_fme; /* used for sending raw data to xcoder */
    ni_logan_session_data_io_t  output_pkt; /* used for receiving bitstream from xcoder */
    ni_logan_fifo_buffer_t *input_data_fifo;

    /* Variables that need to be initialized */
    int started;
    uint8_t *p_spsPpsHdr;
    int spsPpsHdrLen;
    int spsPpsAttach; /* Attach SPS&PPS to Packet */
    int spsPpsArrived;
    int firstPktArrived; /* specially treat first pkt of the encoded bitstream */
    int dts_offset;
    int reconfigCount; /* count of reconfig */
    uint64_t total_frames_received;
    int64_t first_frame_pts;
    int64_t latest_dts;

    // original ConformanceWindowOffsets
    int orig_conf_win_top;   /*!*< A conformance window size of TOP */
    int orig_conf_win_bottom;   /*!*< A conformance window size of BOTTOM */
    int orig_conf_win_left;  /*!*< A conformance window size of LEFT */
    int orig_conf_win_right; /*!*< A conformance window size of RIGHT */

    // to generate encoded bitstream headers
    uint8_t *extradata;
    int extradata_size;

    // low delay mode flags
    int gotPacket; /* used to stop receiving packets when a packet is already received */
    int sentFrame; /* used to continue receiving packets when a frame is sent and a packet is not yet received */

    // sync framerate
    int fps_number;
    int fps_denominator;

    /* actual device index/name of encoder after opened */
    int  actual_dev_enc_idx;
    char *actual_dev_name;
    
    int eos_fme_received; // received the eos frame from the ffmpeg interface
    int encoder_flushing; // NI hardware encoder start flushing
    int encoder_eof; // recieved eof from NI hardware encoder
} ni_logan_enc_context_t;

/*******************************************************************************
 * COMPILE-TIME STRUCT SIZE VERIFICATION
 * 
 * These checks ensure our struct definitions match the library EXACTLY.
 * Measurements from libxcoder_logan v3.5.1 runtime logs:
 *   - sizeof(ni_logan_session_data_io_t) = 416 bytes
 *   - sizeof(ni_logan_enc_context_t) = 688 bytes  
 *   - offsetof(input_data_fifo) = 544 bytes
 * 
 * If compilation fails here, the struct layout is WRONG and must be fixed!
 ******************************************************************************/
#ifndef _MSC_VER
  /* GCC/Clang: Use _Static_assert */
  _Static_assert(sizeof(ni_logan_session_data_io_t) == 416,
                 "ni_logan_session_data_io_t size mismatch!");
  _Static_assert(sizeof(ni_logan_enc_context_t) == 688,
                 "ni_logan_enc_context_t size mismatch!");
#else
  /* MSVC: Use static_assert (C11) or compile-time array check */
  #if _MSC_VER >= 1600  /* VS2010+ */
    static_assert(sizeof(ni_logan_session_data_io_t) == 416,
                  "ni_logan_session_data_io_t size mismatch!");
    static_assert(sizeof(ni_logan_enc_context_t) == 688,
                  "ni_logan_enc_context_t size mismatch!");
  #else
    /* Fallback for old MSVC: compile-time array trick */
    typedef char _verify_session_data_io[(sizeof(ni_logan_session_data_io_t)==416)?1:-1];
    typedef char _verify_enc_context[(sizeof(ni_logan_enc_context_t)==688)?1:-1];
  #endif
#endif


