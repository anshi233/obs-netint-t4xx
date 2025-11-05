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

/** Maximum auxiliary data per frame */
#define NI_MAX_NUM_AUX_DATA_PER_FRAME 16

/** Maximum transmit/receive buffer size for packets */
#define NI_LOGAN_MAX_TX_SZ (8 * 1024 * 1024)  /* 8 MB */

/** Return codes */
#define NI_LOGAN_RETCODE_SUCCESS 0
#define NI_LOGAN_RETCODE_FAILURE -1
#define NI_LOGAN_RETCODE_INVALID_PARAM -2
#define NI_LOGAN_RETCODE_ERROR_MEM_ALOC -3

/** Device types for session open/read/write */
typedef enum {
    NI_LOGAN_DEVICE_TYPE_DECODER = 0,
    NI_LOGAN_DEVICE_TYPE_ENCODER = 1,
    NI_LOGAN_DEVICE_TYPE_SCALER = 2,
    NI_LOGAN_DEVICE_TYPE_AI = 3
} ni_logan_device_type_t;
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
 * @brief Forward declarations for types used in frame/packet structures
 */
typedef enum { NI_LOGAN_CODEC_FORMAT_H264 = 0, NI_LOGAN_CODEC_FORMAT_H265 = 1 } ni_logan_codec_format_t;
typedef enum { NI_LOGAN_PIC_TYPE_I = 0, NI_LOGAN_PIC_TYPE_P = 1, LOGAN_PIC_TYPE_IDR = 2 } ni_logan_pic_type_t;

/* Frame type constants for packet metadata */
typedef enum {
    NI_LOGAN_FRAME_TYPE_I = 0,
    NI_LOGAN_FRAME_TYPE_P = 1,
    NI_LOGAN_FRAME_TYPE_B = 2
} ni_logan_frame_type_t;

/* Color space enums for VUI */
typedef enum {
    NI_COL_PRI_RESERVED0   = 0,
    NI_COL_PRI_BT709       = 1,
    NI_COL_PRI_UNSPECIFIED = 2,
    NI_COL_PRI_RESERVED    = 3,
    NI_COL_PRI_BT470M      = 4,
    NI_COL_PRI_BT470BG     = 5,
    NI_COL_PRI_SMPTE170M   = 6,
    NI_COL_PRI_SMPTE240M   = 7,
    NI_COL_PRI_FILM        = 8,
    NI_COL_PRI_BT2020      = 9,
    NI_COL_PRI_SMPTE428    = 10,
    NI_COL_PRI_SMPTE431    = 11,
    NI_COL_PRI_SMPTE432    = 12,
    NI_COL_PRI_JEDEC_P22   = 22,
} ni_color_primaries_t;

typedef enum {
    NI_COL_TRC_RESERVED0    = 0,
    NI_COL_TRC_BT709        = 1,
    NI_COL_TRC_UNSPECIFIED  = 2,
    NI_COL_TRC_RESERVED     = 3,
    NI_COL_TRC_GAMMA22      = 4,
    NI_COL_TRC_GAMMA28      = 5,
    NI_COL_TRC_SMPTE170M    = 6,
    NI_COL_TRC_SMPTE240M    = 7,
    NI_COL_TRC_LINEAR       = 8,
    NI_COL_TRC_LOG          = 9,
    NI_COL_TRC_LOG_SQRT     = 10,
    NI_COL_TRC_IEC61966_2_4 = 11,
    NI_COL_TRC_BT1361_ECG   = 12,
    NI_COL_TRC_IEC61966_2_1 = 13,
    NI_COL_TRC_BT2020_10    = 14,
    NI_COL_TRC_BT2020_12    = 15,
    NI_COL_TRC_SMPTE2084    = 16,
    NI_COL_TRC_SMPTE428     = 17,
    NI_COL_TRC_ARIB_STD_B67 = 18,
} ni_color_transfer_characteristic_t;

typedef enum {
    NI_COL_SPC_RGB         = 0,
    NI_COL_SPC_BT709       = 1,
    NI_COL_SPC_UNSPECIFIED = 2,
    NI_COL_SPC_RESERVED    = 3,
    NI_COL_SPC_FCC         = 4,
    NI_COL_SPC_BT470BG     = 5,
    NI_COL_SPC_SMPTE170M   = 6,
    NI_COL_SPC_SMPTE240M   = 7,
    NI_COL_SPC_YCGCO       = 8,
    NI_COL_SPC_BT2020_NCL  = 9,
    NI_COL_SPC_BT2020_CL   = 10,
    NI_COL_SPC_SMPTE2085   = 11,
    NI_COL_SPC_CHROMA_DERIVED_NCL = 12,
    NI_COL_SPC_CHROMA_DERIVED_CL  = 13,
    NI_COL_SPC_ICTCP       = 14,
} ni_color_space_t;
typedef struct _ni_logan_buf ni_logan_buf_t;
typedef struct _ni_aux_data ni_aux_data_t;
typedef struct _ni_logan_all_custom_sei ni_logan_all_custom_sei_t;

/**
 * @brief Frame structure - EXACT copy from ni_device_api_logan.h lines 1387-1480
 * 
 * This is the complete structure, not a forward declaration, to ensure correct size.
 */
typedef struct _ni_logan_frame
{
  ni_logan_codec_format_t src_codec;
  long long dts;
  long long pts;
  uint32_t end_of_stream;
  uint32_t start_of_stream;
  uint32_t video_width;
  uint32_t video_height;
  uint32_t video_orig_width;
  uint32_t video_orig_height;

  uint32_t crop_top;
  uint32_t crop_bottom;
  uint32_t crop_left;
  uint32_t crop_right;

  uint16_t force_headers;
  uint8_t use_cur_src_as_long_term_pic;
  uint8_t use_long_term_ref;

  int force_key_frame;
  ni_logan_pic_type_t ni_logan_pict_type;
  unsigned int sei_total_len;

  unsigned int sei_cc_offset;
  unsigned int sei_cc_len;
  unsigned int sei_hdr_mastering_display_color_vol_offset;
  unsigned int sei_hdr_mastering_display_color_vol_len;
  unsigned int sei_hdr_content_light_level_info_offset;
  unsigned int sei_hdr_content_light_level_info_len;
  unsigned int sei_hdr_plus_offset;
  unsigned int sei_hdr_plus_len;
  unsigned int sei_user_data_unreg_offset;
  unsigned int sei_user_data_unreg_len;
  unsigned int sei_alt_transfer_characteristics_offset;
  unsigned int sei_alt_transfer_characteristics_len;
  unsigned int vui_offset;
  unsigned int vui_len;

  unsigned int roi_len;
  unsigned int reconf_len;
  unsigned int extra_data_len;
  uint16_t force_pic_qp;
  uint32_t frame_chunk_idx;

  void * p_data[NI_LOGAN_MAX_NUM_DATA_POINTERS];
  uint32_t data_len[NI_LOGAN_MAX_NUM_DATA_POINTERS];

  void* p_buffer;
  uint32_t buffer_size;

  ni_logan_buf_t *dec_buf;
  uint8_t preferred_characteristics_data_len;

  uint8_t *p_custom_sei;
  uint16_t bit_depth;
  int flags;

  ni_aux_data_t *aux_data[NI_MAX_NUM_AUX_DATA_PER_FRAME];
  int            nb_aux_data;

  uint8_t  color_primaries;
  uint8_t  color_trc;
  uint8_t  color_space;
  int      video_full_range_flag;
  uint8_t  aspect_ratio_idc;
  uint16_t sar_width;
  uint16_t sar_height;
  uint32_t vui_num_units_in_tick;
  uint32_t vui_time_scale;
  uint8_t separate_metadata;
} ni_logan_frame_t;

/**
 * @brief Packet structure - EXACT copy from ni_device_api_logan.h lines 1613-1639
 */
typedef struct _ni_logan_packet
{
  long long dts;
  long long pts;
  long long pos;
  uint32_t end_of_stream;
  uint32_t start_of_stream;
  uint32_t video_width;
  uint32_t video_height;
  uint32_t frame_type;
  int recycle_index;

  void* p_data;
  uint32_t data_len;
  int sent_size;

  void* p_buffer;
  uint32_t buffer_size;
  uint32_t avg_frame_qp;

  ni_logan_all_custom_sei_t *p_all_custom_sei;
  int len_of_sei_after_vcl;
  int flags;
} ni_logan_packet_t;

/**
 * @brief Session data I/O structure - EXACT copy from ni_device_api_logan.h lines 1661-1669
 * 
 * This is the REAL structure with the actual union, not padding!
 * Contains union of frame and packet for bidirectional data transfer.
 */
typedef struct _ni_logan_session_data_io
{
  union
  {
    ni_logan_frame_t  frame;
    ni_logan_packet_t packet;
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


