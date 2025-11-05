/**
 * @file netint-libxcoder.c
 * @brief Dynamic library loader for NETINT libxcoder_logan.so
 * 
 * This module handles runtime loading of the NETINT libxcoder library (libxcoder_logan.so on Linux,
 * libxcoder_logan.dll on Windows) using OBS platform abstractions (os_dlopen/os_dlsym/os_dlclose).
 * This approach allows the plugin to be built without requiring NETINT SDK headers or libraries
 * at compile time.
 * 
 * Key Features:
 * - Lazy loading: Library is only loaded when first encoder is created
 * - Symbol resolution: Function pointers are resolved at runtime via os_dlsym
 * - Optional APIs: Some functions (device discovery, parameter setting) are optional
 * - Path override: Can specify library path via NETINT_LIBXCODER_PATH environment variable
 * 
 * Architecture:
 * - Function pointers are stored as global variables (prefixed with p_)
 * - All function pointers are checked before use (NULL checks)
 * - Library handle is cached in static variable to avoid reloading
 * 
 * Error Handling:
 * - If library fails to load, function pointers remain NULL
 * - Encoder creation code checks for NULL pointers before using functions
 * - Detailed error logging helps diagnose missing library or version mismatches
 */

#include "netint-libxcoder.h"

#include <obs-module.h>
#include <util/platform.h>
#include <stdlib.h>

/**
 * @brief Cached handle to the dynamically loaded libxcoder library
 * 
 * This static variable stores the os_dlopen() handle. It is NULL if:
 * - Library hasn't been loaded yet
 * - Library failed to load
 * - Library was closed
 * 
 * The handle is used by os_dlclose() to unload the library when plugin unloads.
 */
static void *s_lib_handle = NULL;

/**
 * @name Required Encoder API Function Pointers
 * @brief These functions must be present in libxcoder for basic encoding to work
 * 
 * All of these are resolved at library load time via os_dlsym(). If any required
 * function is missing, library loading fails.
 */
/*@{*/
/** Initialize encoder context with basic parameters (width, height, bitrate, etc.) */
int (*p_ni_logan_encode_init)(ni_logan_enc_context_t *) = NULL;

/** Parse encoder parameters and validate configuration */
int (*p_ni_logan_encode_params_parse)(ni_logan_enc_context_t *) = NULL;

/** Open encoder device and establish connection to hardware */
int (*p_ni_logan_encode_open)(ni_logan_enc_context_t *) = NULL;

/** Close encoder device and release hardware resources */
int (*p_ni_logan_encode_close)(ni_logan_enc_context_t *) = NULL;

/** Generate encoder headers (SPS/PPS for H.264, VPS/SPS/PPS for H.265) */
int (*p_ni_logan_encode_header)(ni_logan_enc_context_t *) = NULL;

/** Get a frame buffer from encoder for input data */
int (*p_ni_logan_encode_get_frame)(ni_logan_enc_context_t *) = NULL;

/** Reconfigure encoder for variable frame rate (VFR) scenarios */
void (*p_ni_logan_encode_reconfig_vfr)(ni_logan_enc_context_t *, ni_logan_frame_t *, int64_t) = NULL;

/** Copy video frame data from OBS format into encoder's frame buffer */
int (*p_ni_logan_encode_copy_frame_data)(ni_logan_enc_context_t *, ni_logan_frame_t *, uint8_t *[NI_LOGAN_MAX_NUM_DATA_POINTERS], int[NI_LOGAN_MAX_NUM_DATA_POINTERS]) = NULL;

/** Send frame to encoder for encoding (triggers hardware encoding) */
int (*p_ni_logan_encode_send)(ni_logan_enc_context_t *) = NULL;

/** Copy encoded packet data from encoder output buffer to our buffer */
int (*p_ni_logan_encode_copy_packet_data)(ni_logan_enc_context_t *, uint8_t *, int, int) = NULL;

/** Receive encoded packet from encoder (non-blocking, returns size if available) */
int (*p_ni_logan_encode_receive)(ni_logan_enc_context_t *) = NULL;
/*@}*/

/**
 * @name Optional Resource Management API Function Pointers
 * @brief These functions are optional - used for device discovery
 * 
 * If these are NULL, device discovery in UI won't work, but manual device
 * specification will still function.
 */
/*@{*/
/** Initialize resource management system for device discovery */
int (*p_ni_logan_rsrc_init)(int, int) = NULL;

/** Get list of available NETINT devices on the system */
int (*p_ni_logan_rsrc_get_local_device_list)(char devices[][NI_LOGAN_MAX_DEVICE_NAME_LEN], int max_handles) = NULL;
/*@}*/

/**
 * @name Optional Encoder Parameters API Function Pointers
 * @brief Used for setting advanced encoder parameters (CBR/VBR, profile, etc.)
 * 
 * These are forward-declared types because we don't have full definitions.
 * The function allows runtime parameter setting after encoder initialization.
 */
/*@{*/
struct _ni_logan_session_context;
typedef struct _ni_logan_session_context ni_logan_session_context_t;
typedef struct _ni_logan_encoder_params ni_logan_encoder_params_t;

/** Set encoder parameter value by name (e.g., "cbr", "profile") */
int (*p_ni_logan_encoder_params_set_value)(ni_logan_encoder_params_t *, const char *, const char *, ni_logan_session_context_t *) = NULL;
/*@}*/

/**
 * @brief Dynamically load libxcoder_logan.so and resolve all required function symbols
 * 
 * This function performs the following operations:
 * 1. Checks if library is already loaded (idempotent)
 * 2. Determines library path (environment variable override or default)
 * 3. Uses os_dlopen() to load the shared library (cross-platform)
 * 4. Resolves all required function symbols via os_dlsym()
 * 5. Resolves optional function symbols (gracefully handles missing ones)
 * 
 * Library Path Resolution:
 * - First checks NETINT_LIBXCODER_PATH environment variable for custom path
 * - Falls back to platform-specific default: "libxcoder_logan.so" (Linux) or "libxcoder_logan.dll" (Windows)
 * - os_dlopen() searches standard paths: LD_LIBRARY_PATH/PATH, /usr/lib, /usr/local/lib, etc.
 * 
 * Symbol Resolution:
 * - Required symbols: Must be present or loading fails
 * - Optional symbols: Set to NULL if missing, checked before use in encoder code
 * 
 * Thread Safety:
 * - This function is not thread-safe (should only be called from main thread)
 * - Multiple calls are safe (idempotent) but should be avoided
 * 
 * @return true if library loaded and all required symbols resolved
 * @return false if library not found, missing symbols, or other loading error
 */
bool ni_libxcoder_open(void)
{
    /* If library is already loaded, return success immediately */
    /* This makes the function idempotent and safe to call multiple times */
    if (s_lib_handle) {
        return true;
    }

    /* Check for environment variable override of library path */
    /* This is useful for development/testing or non-standard installations */
    const char *override_path = getenv("NETINT_LIBXCODER_PATH");
    const char *libname;
    
    if (override_path && *override_path) {
        libname = override_path;
    } else {
        /* Use platform-specific default library name */
#ifdef _WIN32
        libname = "libxcoder_logan.dll";
#else
        libname = "libxcoder_logan.so";
#endif
    }

    /* Load the shared library using OBS platform abstraction */
    /* os_dlopen() handles platform-specific loading (LoadLibrary on Windows, dlopen on Linux) */
    s_lib_handle = os_dlopen(libname);
    if (!s_lib_handle) {
        blog(LOG_WARNING, "[obs-netint-t4xx] Failed to load %s", libname);
        return false;
    }

    /* Resolve required symbols - these MUST be present for encoding to work */
    /* The RESOLVE macro simplifies the repetitive os_dlsym() + error checking pattern */
#define RESOLVE(name)                                                                      \
    do {                                                                                   \
        p_##name = (void *)os_dlsym(s_lib_handle, #name);                                    \
        if (!p_##name) {                                                                   \
            blog(LOG_ERROR, "[obs-netint-t4xx] Failed to resolve symbol %s", #name);      \
            ni_libxcoder_close();                                                          \
            return false;                                                                  \
        }                                                                                  \
    } while (0)

    /* Resolve all required encoder API functions */
    /* If any of these fail, the library is unusable and we abort loading */
    RESOLVE(ni_logan_encode_init);
    RESOLVE(ni_logan_encode_params_parse);
    RESOLVE(ni_logan_encode_open);
    RESOLVE(ni_logan_encode_close);
    RESOLVE(ni_logan_encode_header);
    RESOLVE(ni_logan_encode_get_frame);
    RESOLVE(ni_logan_encode_reconfig_vfr);
    RESOLVE(ni_logan_encode_copy_frame_data);
    RESOLVE(ni_logan_encode_send);
    RESOLVE(ni_logan_encode_copy_packet_data);
    RESOLVE(ni_logan_encode_receive);
    
    /* Optional discovery APIs - these are nice-to-have for device auto-detection */
    /* If missing, we continue - device discovery just won't work in UI */
    p_ni_logan_rsrc_init = (void *)os_dlsym(s_lib_handle, "ni_logan_rsrc_init");
    p_ni_logan_rsrc_get_local_device_list = (void *)os_dlsym(s_lib_handle, "ni_logan_rsrc_get_local_device_list");
    
    /* Optional encoder params API - allows runtime parameter adjustment */
    /* If missing, we can't set advanced parameters like CBR/VBR mode dynamically */
    p_ni_logan_encoder_params_set_value = (void *)os_dlsym(s_lib_handle, "ni_logan_encoder_params_set_value");

#undef RESOLVE
    blog(LOG_INFO, "[obs-netint-t4xx] Successfully loaded %s", libname);
    return true;
}

/**
 * @brief Unload the libxcoder library and reset all function pointers
 * 
 * This function is called during plugin unload to clean up resources. It:
 * 1. Closes the os_dlopen() handle (reduces reference count)
 * 2. Resets the handle to NULL
 * 3. Function pointers remain as-is (but should not be used after this call)
 * 
 * Note: This function does NOT reset function pointers to NULL because:
 * - They may still be in use by existing encoder instances
 * - OBS should destroy all encoders before calling module unload
 * - Setting them to NULL could cause crashes if cleanup is still in progress
 * 
 * Thread Safety:
 * - Should only be called from main thread during plugin unload
 * - Should not be called while encoders are active (OBS handles this)
 */
void ni_libxcoder_close(void)
{
    if (s_lib_handle) {
        /* Close the library handle - this decrements the reference count */
        /* The library will actually unload when reference count reaches 0 */
        os_dlclose(s_lib_handle);
        s_lib_handle = NULL;
    }
}


