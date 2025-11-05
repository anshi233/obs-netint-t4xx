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
#include <stdarg.h>

/**
 * @brief Log callback to redirect libxcoder logs to OBS
 * 
 * libxcoder uses ni_log() which outputs to stderr by default.
 * OBS doesn't capture stderr, so we need to redirect to blog().
 */
static void netint_log_callback(int level, const char *fmt, va_list vl)
{
    /* Map libxcoder log levels to OBS log levels */
    int obs_level;
    switch (level) {
        case 1: /* NI_LOG_FATAL */
        case 2: /* NI_LOG_ERROR */
            obs_level = LOG_ERROR;
            break;
        case 3: /* NI_LOG_INFO */
            obs_level = LOG_INFO;
            break;
        case 4: /* NI_LOG_DEBUG */
            obs_level = LOG_DEBUG;
            break;
        case 5: /* NI_LOG_TRACE */
            obs_level = LOG_DEBUG;
            break;
        default:
            obs_level = LOG_INFO;
            break;
    }
    
    /* Forward to OBS's logging system with [libxcoder] prefix */
    char buffer[4096];
    vsnprintf(buffer, sizeof(buffer), fmt, vl);
    
    /* Remove trailing newline if present (blog adds its own) */
    size_t len = strlen(buffer);
    if (len > 0 && buffer[len-1] == '\n') {
        buffer[len-1] = '\0';
    }
    
    blog(obs_level, "[libxcoder] %s", buffer);
}

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

/** Allocate encoder frame buffer (for device session API) */
int (*p_ni_logan_encoder_frame_buffer_alloc)(ni_logan_frame_t *, int, int, int[NI_LOGAN_MAX_NUM_DATA_POINTERS], int, int) = NULL;

/** Free frame buffer */
int (*p_ni_logan_frame_buffer_free)(ni_logan_frame_t *) = NULL;

/** Copy YUV data to HW format */
void (*p_ni_logan_copy_hw_yuv420p)(uint8_t *[NI_LOGAN_MAX_NUM_DATA_POINTERS], uint8_t *[NI_LOGAN_MAX_NUM_DATA_POINTERS], int, int, int, int[NI_LOGAN_MAX_NUM_DATA_POINTERS], int[NI_LOGAN_MAX_NUM_DATA_POINTERS], int[NI_LOGAN_MAX_NUM_DATA_POINTERS], int[NI_LOGAN_MAX_NUM_DATA_POINTERS]) = NULL;

/** Device session write (low-level API) */
int (*p_ni_logan_device_session_write)(ni_logan_session_context_t *, ni_logan_session_data_io_t *, int) = NULL;

/** Device session read (low-level API) */
int (*p_ni_logan_device_session_read)(ni_logan_session_context_t *, ni_logan_session_data_io_t *, int) = NULL;

/** Open device session */
int (*p_ni_logan_device_session_open)(ni_logan_session_context_t *, int) = NULL;

/** Close device session */
int (*p_ni_logan_device_session_close)(ni_logan_session_context_t *, int, int) = NULL;

/** Initialize session context */
void (*p_ni_logan_device_session_context_init)(ni_logan_session_context_t *) = NULL;

/** Get HW YUV420p dimensions */
void (*p_ni_logan_get_hw_yuv420p_dim)(int, int, int, int, int[NI_LOGAN_MAX_NUM_DATA_POINTERS], int[NI_LOGAN_MAX_NUM_DATA_POINTERS]) = NULL;

/** Allocate packet buffer */
int (*p_ni_logan_packet_buffer_alloc)(ni_logan_packet_t *, int) = NULL;

/** Free packet buffer */
int (*p_ni_logan_packet_buffer_free)(ni_logan_packet_t *) = NULL;

/** Initialize default encoder parameters */
int (*p_ni_logan_encoder_init_default_params)(ni_logan_encoder_params_t *, int, int, long, int, int) = NULL;
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

/** Set encoder GOP parameter value by name */
int (*p_ni_logan_encoder_gop_params_set_value)(ni_logan_encoder_params_t *, const char *, const char *, void *) = NULL;

/** Set VUI parameters */
void (*p_ni_logan_set_vui)(ni_logan_encoder_params_t *, ni_logan_session_context_t *, ni_color_primaries_t, ni_color_transfer_characteristic_t, ni_color_space_t, int, int, int, ni_logan_codec_format_t) = NULL;
/*@}*/

/**
 * @name Optional Logging API Function Pointers
 * @brief Used to redirect libxcoder logs to OBS logging system
 */
/*@{*/
#include <stdarg.h>
/** Set custom log callback to capture libxcoder logs */
void (*p_ni_log_set_callback)(void (*log_callback)(int, const char*, va_list)) = NULL;
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

    /* ============================================================================
     * TEMPORARY DEBUG: Use specific DLL path for debugging
     * 
     * TODO: REMOVE THIS AFTER DEBUGGING IS COMPLETE!
     * 
     * This hardcoded path ensures we're using the exact DLL version from the
     * libxcoder source directory, not some other version in PATH.
     * 
     * Currently using: x64/DebugDLL/libxcoder_logan.dll (has debug symbols and error messages)
     * 
     * Other options to try if DebugDLL doesn't exist:
     * - build/libxcoder_logan.dll (last built version)
     * - x64/ReleaseDLL/libxcoder_logan.dll (optimized, less error info)
     * 
     * For production, we should use the normal library search paths.
     * ============================================================================ */
#ifdef _WIN32
    /* Try DebugDLL first (best for debugging with error messages) */
    const char *debug_dll_path = "E:\\src\\t408\\t408\\V3.5.1\\release\\libxcoder_logan\\NI_MSVS2022_XCODER\\x64\\DebugDLL\\libxcoder_logan.dll";
    
    /* Fallback to build directory if DebugDLL doesn't exist */
    const char *fallback_dll_path = "E:\\src\\t408\\t408\\V3.5.1\\release\\libxcoder_logan\\NI_MSVS2022_XCODER\\build\\libxcoder_logan.dll";
    
    /* Log which DLL we're trying to load */
    blog(LOG_INFO, "[obs-netint-t4xx] ========================================");
    blog(LOG_INFO, "[obs-netint-t4xx] DEBUG MODE: Using hardcoded DLL path");
    blog(LOG_INFO, "[obs-netint-t4xx] TODO: REMOVE THIS AFTER DEBUGGING!");
    blog(LOG_INFO, "[obs-netint-t4xx] ========================================");
    blog(LOG_INFO, "[obs-netint-t4xx] Primary:  %s", debug_dll_path);
    blog(LOG_INFO, "[obs-netint-t4xx] Fallback: %s", fallback_dll_path);
#endif
    
    /* Check for environment variable override of library path */
    /* This is useful for development/testing or non-standard installations */
    const char *override_path = getenv("NETINT_LIBXCODER_PATH");
    const char *libname;
    
    if (override_path && *override_path) {
        libname = override_path;
        blog(LOG_INFO, "[obs-netint-t4xx] Using DLL from NETINT_LIBXCODER_PATH: %s", libname);
    } else {
        /* Use platform-specific default library name */
#ifdef _WIN32
        /* DEBUG: Try DebugDLL first, fallback to build directory */
        libname = debug_dll_path;
        blog(LOG_INFO, "[obs-netint-t4xx] Trying primary path (DebugDLL): %s", libname);
#else
        libname = "libxcoder_logan.so";
#endif
    }

    /* Load the shared library using OBS platform abstraction */
    /* os_dlopen() handles platform-specific loading (LoadLibrary on Windows, dlopen on Linux) */
    s_lib_handle = os_dlopen(libname);
    
#ifdef _WIN32
    /* If DebugDLL failed, try fallback to build directory */
    if (!s_lib_handle && !override_path) {
        blog(LOG_WARNING, "[obs-netint-t4xx] DebugDLL not found, trying fallback path...");
        libname = fallback_dll_path;
        blog(LOG_INFO, "[obs-netint-t4xx] Trying fallback path (build): %s", libname);
        s_lib_handle = os_dlopen(libname);
    }
#endif
    
    if (!s_lib_handle) {
        blog(LOG_ERROR, "[obs-netint-t4xx] ========================================");
        blog(LOG_ERROR, "[obs-netint-t4xx] FAILED TO LOAD libxcoder_logan.dll");
        blog(LOG_ERROR, "[obs-netint-t4xx] ========================================");
        blog(LOG_ERROR, "[obs-netint-t4xx] All attempts failed:");
#ifdef _WIN32
        blog(LOG_ERROR, "[obs-netint-t4xx]   1. NETINT_LIBXCODER_PATH: %s", 
             override_path ? override_path : "(not set)");
        blog(LOG_ERROR, "[obs-netint-t4xx]   2. DebugDLL: %s", debug_dll_path);
        blog(LOG_ERROR, "[obs-netint-t4xx]   3. Build directory: %s", fallback_dll_path);
#else
        blog(LOG_ERROR, "[obs-netint-t4xx]   %s", libname);
#endif
        blog(LOG_ERROR, "[obs-netint-t4xx] ========================================");
        blog(LOG_ERROR, "[obs-netint-t4xx] Troubleshooting:");
        blog(LOG_ERROR, "[obs-netint-t4xx]   1. Rebuild libxcoder from source (NI_MSVS2022_XCODER project)");
        blog(LOG_ERROR, "[obs-netint-t4xx]   2. Use DebugDLL|x64 configuration");
        blog(LOG_ERROR, "[obs-netint-t4xx]   3. Check DLL dependencies with Dependency Walker");
        blog(LOG_ERROR, "[obs-netint-t4xx]   4. Unset NETINT_LIBXCODER_PATH environment variable");
        blog(LOG_ERROR, "[obs-netint-t4xx] ========================================");
        return false;
    }
    
    blog(LOG_INFO, "[obs-netint-t4xx] ========================================");
    blog(LOG_INFO, "[obs-netint-t4xx] DLL LOADED SUCCESSFULLY:");
    blog(LOG_INFO, "[obs-netint-t4xx]   %s", libname);
    blog(LOG_INFO, "[obs-netint-t4xx] ========================================");

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
    
    /* Device session API (lower-level, direct hardware access) */
    RESOLVE(ni_logan_device_session_context_init);
    RESOLVE(ni_logan_device_session_open);
    RESOLVE(ni_logan_device_session_close);
    RESOLVE(ni_logan_device_session_write);
    RESOLVE(ni_logan_device_session_read);
    
    /* Frame buffer management API */
    RESOLVE(ni_logan_encoder_frame_buffer_alloc);
    RESOLVE(ni_logan_frame_buffer_free);
    RESOLVE(ni_logan_get_hw_yuv420p_dim);
    RESOLVE(ni_logan_copy_hw_yuv420p);
    RESOLVE(ni_logan_packet_buffer_alloc);
    RESOLVE(ni_logan_packet_buffer_free);
    RESOLVE(ni_logan_encoder_init_default_params);
    
    /* High-level encode API (keeping for compatibility, but switching to device session API) */
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
    RESOLVE(ni_logan_encoder_gop_params_set_value);
    RESOLVE(ni_logan_set_vui);

    /* Optional logging API - redirect libxcoder logs to OBS logging */
    p_ni_log_set_callback = (void *)os_dlsym(s_lib_handle, "ni_log_set_callback");
    if (p_ni_log_set_callback) {
        blog(LOG_INFO, "[obs-netint-t4xx] Setting up log callback to capture libxcoder logs...");
        p_ni_log_set_callback(netint_log_callback);
        blog(LOG_INFO, "[obs-netint-t4xx] ╔════════════════════════════════════════════════════╗");
        blog(LOG_INFO, "[obs-netint-t4xx] ║ LIBXCODER LOGGING REDIRECTED TO OBS              ║");
        blog(LOG_INFO, "[obs-netint-t4xx] ║ All libxcoder ni_log() output will now appear    ║");
        blog(LOG_INFO, "[obs-netint-t4xx] ║ in OBS log with [libxcoder] prefix                ║");
        blog(LOG_INFO, "[obs-netint-t4xx] ╚════════════════════════════════════════════════════╝");
    } else {
        blog(LOG_WARNING, "[obs-netint-t4xx] ni_log_set_callback not found - libxcoder logs will not appear in OBS log");
    }

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


