/**
 * @file netint-libxcoder.h
 * @brief Header file for dynamic libxcoder library loader (cross-platform)
 * 
 * This header file declares the interface for loading and using the NETINT
 * libxcoder library (libxcoder_logan.so on Linux, libxcoder_logan.dll on Windows)
 * at runtime. It provides:
 * - Functions to open/close the library
 * - Function pointers for all libxcoder API functions we use
 * - Forward declarations for opaque types used by the API
 * 
 * The function pointers are resolved at runtime via os_dlsym() when the library
 * is loaded. This allows the plugin to be built without requiring NETINT
 * SDK headers or libraries at compile time.
 * 
 * Function pointers are NULL if:
 * - Library hasn't been loaded yet
 * - Library failed to load
 * - Function doesn't exist in the library (for optional functions)
 * 
 * Code should check function pointers for NULL before using them.
 */

#pragma once

#ifndef NETINT_LIBXCODER_H
#define NETINT_LIBXCODER_H

#include <stdbool.h>
#include <stdint.h>

#include "netint-libxcoder-shim.h"

/* Forward declarations for new device session API types */
typedef struct _ni_logan_session_context ni_logan_session_context_t;
typedef struct _ni_logan_encoder_params ni_logan_encoder_params_t;
typedef struct _ni_logan_packet ni_logan_packet_t;

/**
 * @brief Open and load libxcoder library, resolving all function symbols
 * 
 * This function uses os_dlopen() to load the library and os_dlsym() to resolve function
 * pointers. It can be called multiple times safely (idempotent).
 * 
 * Library path resolution:
 * - Checks NETINT_LIBXCODER_PATH environment variable first
 * - Falls back to platform-specific default: "libxcoder_logan.so" (Linux) or "libxcoder_logan.dll" (Windows)
 * 
 * @return true if library loaded and all required symbols resolved, false otherwise
 */
bool ni_libxcoder_open(void);

/**
 * @brief Close libxcoder library and release resources
 * 
 * This function closes the library handle opened by ni_libxcoder_open().
 * Function pointers are not reset (they may still be in use).
 */
void ni_libxcoder_close(void);

/**
 * @name Required Encoder API Function Pointers
 * @brief These functions must be present in libxcoder for encoding to work
 * 
 * All of these are resolved at library load time. If any required function
 * is missing, library loading fails.
 * 
 * These pointers are NULL until ni_libxcoder_open() is called successfully.
 * Code should check for NULL before using (though encoder creation already
 * checks this).
 */
/*@{*/
/** Initialize encoder context with parameters */
extern int (*p_ni_logan_encode_init)(ni_logan_enc_context_t *);

/** Parse and validate encoder parameters */
extern int (*p_ni_logan_encode_params_parse)(ni_logan_enc_context_t *);

/** Open connection to hardware encoder device */
extern int (*p_ni_logan_encode_open)(ni_logan_enc_context_t *);

/** Close connection to hardware encoder device */
extern int (*p_ni_logan_encode_close)(ni_logan_enc_context_t *);

/** Generate encoder headers (SPS/PPS for H.264, VPS/SPS/PPS for H.265) */
extern int (*p_ni_logan_encode_header)(ni_logan_enc_context_t *);

/** Get frame buffer from encoder for input data */
extern int (*p_ni_logan_encode_get_frame)(ni_logan_enc_context_t *);

/** Reconfigure encoder for variable frame rate (VFR) */
extern void (*p_ni_logan_encode_reconfig_vfr)(ni_logan_enc_context_t *, ni_logan_frame_t *, int64_t);

/** Copy video frame data from OBS format to encoder buffer */
extern int (*p_ni_logan_encode_copy_frame_data)(ni_logan_enc_context_t *, ni_logan_frame_t *, uint8_t *[NI_LOGAN_MAX_NUM_DATA_POINTERS], int[NI_LOGAN_MAX_NUM_DATA_POINTERS]);

/** Send frame to encoder for encoding */
extern int (*p_ni_logan_encode_send)(ni_logan_enc_context_t *);

/** Copy encoded packet data from encoder buffer */
extern int (*p_ni_logan_encode_copy_packet_data)(ni_logan_enc_context_t *, uint8_t *, int, int);

/** Receive encoded packet from encoder (non-blocking) */
extern int (*p_ni_logan_encode_receive)(ni_logan_enc_context_t *);

/** Allocate encoder frame buffer (for device session API) */
extern int (*p_ni_logan_encoder_frame_buffer_alloc)(ni_logan_frame_t *, int, int, int[NI_LOGAN_MAX_NUM_DATA_POINTERS], int, int, int);

/** Free frame buffer */
extern int (*p_ni_logan_frame_buffer_free)(ni_logan_frame_t *);

/** Copy YUV data to HW format */
extern void (*p_ni_logan_copy_hw_yuv420p)(uint8_t *[NI_LOGAN_MAX_NUM_DATA_POINTERS], uint8_t *[NI_LOGAN_MAX_NUM_DATA_POINTERS], int, int, int, int[NI_LOGAN_MAX_NUM_DATA_POINTERS], int[NI_LOGAN_MAX_NUM_DATA_POINTERS], int[NI_LOGAN_MAX_NUM_DATA_POINTERS], int[NI_LOGAN_MAX_NUM_DATA_POINTERS]);

/** Device session write (low-level API) */
extern int (*p_ni_logan_device_session_write)(ni_logan_session_context_t *, ni_logan_session_data_io_t *, int);

/** Device session read (low-level API) */
extern int (*p_ni_logan_device_session_read)(ni_logan_session_context_t *, ni_logan_session_data_io_t *, int);

/** Open device session */
extern int (*p_ni_logan_device_session_open)(ni_logan_session_context_t *, int);

/** Close device session */
extern int (*p_ni_logan_device_session_close)(ni_logan_session_context_t *, int, int);

/** Initialize session context */
extern void (*p_ni_logan_device_session_context_init)(ni_logan_session_context_t *);

/** Get HW YUV420p dimensions */
extern void (*p_ni_logan_get_hw_yuv420p_dim)(int, int, int, int, int[NI_LOGAN_MAX_NUM_DATA_POINTERS], int[NI_LOGAN_MAX_NUM_DATA_POINTERS]);

/** Allocate packet buffer */
extern int (*p_ni_logan_packet_buffer_alloc)(ni_logan_packet_t *, int);

/** Free packet buffer */
extern int (*p_ni_logan_packet_buffer_free)(ni_logan_packet_t *);

/** Initialize default encoder parameters */
extern int (*p_ni_logan_encoder_init_default_params)(ni_logan_encoder_params_t *, int, int, long, int, int);
/*@}*/

/**
 * @name Optional Resource Management API Function Pointers
 * @brief These functions are optional - used for device discovery
 * 
 * If these are NULL, device discovery won't work, but manual device
 * specification will still function. These are set to NULL if the
 * library doesn't export them (older versions).
 */
/*@{*/
/** Initialize resource management system for device discovery */
extern int (*p_ni_logan_rsrc_init)(int should_match_rev, int timeout_seconds);

/** Get list of available NETINT devices on the system */
extern int (*p_ni_logan_rsrc_get_local_device_list)(char devices[][NI_LOGAN_MAX_DEVICE_NAME_LEN], int max_handles);
/*@}*/

/**
 * @name Optional Encoder Parameters API Function Pointers
 * @brief Used for setting advanced encoder parameters at runtime
 * 
 * These are forward-declared types because we don't have full definitions
 * (they're opaque types from libxcoder). The function allows runtime
 * parameter setting after encoder initialization.
 * 
 * If this function is NULL, we can't set advanced parameters like CBR/VBR
 * mode dynamically, but basic encoding will still work.
 */
/*@{*/
/* Forward declaration for opaque types */
struct _ni_logan_session_context;
typedef struct _ni_logan_session_context ni_logan_session_context_t;
typedef struct _ni_logan_encoder_params ni_logan_encoder_params_t;

/** Set encoder parameter value by name (e.g., "cbr", "profile") */
extern int (*p_ni_logan_encoder_params_set_value)(ni_logan_encoder_params_t *, const char *, const char *, ni_logan_session_context_t *);

/** Set encoder GOP parameter value by name */
extern int (*p_ni_logan_encoder_gop_params_set_value)(ni_logan_encoder_params_t *, const char *, const char *, void *);

/** Set VUI parameters */
extern void (*p_ni_logan_set_vui)(ni_logan_encoder_params_t *, ni_logan_session_context_t *, ni_color_primaries_t, ni_color_transfer_characteristic_t, ni_color_space_t, int, int, int, ni_logan_codec_format_t);
/*@}*/

/**
 * @name Encoder Parameter Name Constants
 * @brief String constants for encoder parameter names (from ni_device_api_logan.h)
 */
/*@{*/
#define NI_LOGAN_ENC_PARAM_GOP_PRESET_IDX                 "gopPresetIdx"
#define NI_LOGAN_ENC_PARAM_INTRA_PERIOD                   "intraPeriod"
/*@}*/

#endif /* NETINT_LIBXCODER_H */