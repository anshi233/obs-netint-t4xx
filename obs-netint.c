/**
 * @file obs-netint.c
 * @brief Main plugin entry point for OBS Studio NETINT T4XX hardware encoder plugin
 * 
 * This file implements the OBS module interface for the NETINT T408 hardware encoder plugin.
 * The plugin provides hardware-accelerated H.264 and H.265 encoding using NETINT's T4XX series
 * PCIe cards via the libxcoder_logan library.
 * 
 * Architecture:
 * - This plugin dynamically loads libxcoder_logan.so at runtime (no compile-time dependency)
 * - Supports both H.264 and H.265 encoding through separate encoder registrations
 * - Uses a background thread to receive encoded packets asynchronously for lower latency
 * - Gracefully handles missing library (encoders appear in UI but won't work without library)
 * 
 * Key Design Decisions:
 * - Dynamic loading allows the plugin to be built without vendor SDK headers
 * - Plugin remains loadable even if libxcoder is missing (useful for debugging/distribution)
 * - Separate encoder registrations for H.264 and H.265 allow OBS to distinguish codec types
 */

#include <obs-module.h>
#include "netint-encoder.h"
#include "netint-libxcoder.h"

/**
 * @brief OBS module declaration macro
 * 
 * This macro tells OBS Studio that this is a valid plugin module. It must be called
 * before any other OBS module functions.
 */
OBS_DECLARE_MODULE()

/**
 * @brief Set default locale for plugin strings
 * 
 * Configures the plugin to use English (US) locale by default for any user-facing strings.
 * The first parameter is the module name, second is the locale identifier.
 */
OBS_MODULE_USE_DEFAULT_LOCALE("obs-netint-t4xx", "en-US")

/**
 * @brief Get human-readable description of this plugin module
 * 
 * This function is called by OBS Studio to display information about the plugin in the UI.
 * It should return a short, descriptive string explaining what the plugin does.
 * 
 * @return Pointer to a static string describing the plugin
 */
MODULE_EXPORT const char *obs_module_description(void)
{
    return "NETINT T408 Hardware Encoder (libxcoder)";
}

/**
 * @brief Get plugin version string
 * 
 * This function is called by OBS Studio to display the plugin version.
 * It should return a version string in semantic versioning format (e.g., "1.0.0").
 * 
 * @return Pointer to a static string containing the plugin version
 */
MODULE_EXPORT const char *obs_module_version(void)
{
    return "1.0.0";
}

/**
 * @brief Get plugin author/developer name
 * 
 * This function is called by OBS Studio to display the plugin author/developer.
 * 
 * @return Pointer to a static string containing the author name
 */
MODULE_EXPORT const char *obs_module_author(void)
{
    return "NETINT Technologies / OBS Plugin Contributors";
}

/**
 * @brief Plugin initialization function called by OBS Studio when the plugin is loaded
 * 
 * This is the main entry point for plugin initialization. It performs the following tasks:
 * 1. Checks OBS API version for compatibility
 * 2. Attempts to dynamically load libxcoder_logan.so (NETINT's encoder library)
 * 3. Registers H.264 and H.265 encoder implementations with OBS Studio
 * 
 * Important: The plugin does NOT fail to load if libxcoder is unavailable. This allows:
 * - The plugin to be distributed without requiring the library on build systems
 * - Users to see the encoder options even if they haven't installed NETINT drivers yet
 * - Better debugging experience (can check plugin loading without hardware)
 * 
 * If the library is missing, encoders will appear in OBS UI but will fail to create
 * when actually selected (with appropriate error messages).
 * 
 * Version Compatibility:
 * - Minimum supported: OBS Studio 27.0 (API version 27)
 * - Tested with: OBS Studio 28.x, 29.x
 * - Future versions should be compatible if API remains stable
 * 
 * @return true if plugin loaded successfully (always returns true, even if library missing)
 */
bool obs_module_load(void)
{
    /* Check OBS API version for compatibility */
    /* This helps identify potential compatibility issues early */
    /* Note: OBS_VERSION is a compile-time define, not a runtime function */
    #ifdef OBS_VERSION
        blog(LOG_INFO, "[obs-netint-t4xx] Plugin version 1.0.0 loading on OBS %s", OBS_VERSION);
    #else
        blog(LOG_INFO, "[obs-netint-t4xx] Plugin version 1.0.0 loading (OBS version unknown)");
    #endif

    /* Try to initialize library, but don't fail if it's not available */
    /* This allows the plugin to load even on systems without NETINT hardware/drivers */
    if (!netint_loader_init()) {
        blog(LOG_INFO, "[obs-netint-t4xx] libxcoder not available; encoder will be selectable but non-functional (for debugging)");
    } else {
        /* Initialize NETINT resource manager to connect to shared memory */
        /* This is required for device discovery and auto-selection ("bestload", etc.) */
        blog(LOG_INFO, "[obs-netint-t4xx] p_ni_logan_rsrc_init function pointer: %p", p_ni_logan_rsrc_init);
        
        if (p_ni_logan_rsrc_init) {
            blog(LOG_INFO, "[obs-netint-t4xx] Calling ni_logan_rsrc_init(should_match_rev=0, timeout=1)...");
            int rsrc_ret = p_ni_logan_rsrc_init(0, 1); /* should_match_rev=0, timeout=1s */
            blog(LOG_INFO, "[obs-netint-t4xx] ni_logan_rsrc_init returned: %d (0x%X)", rsrc_ret, rsrc_ret);
            
            if (rsrc_ret == 0) {
                blog(LOG_INFO, "[obs-netint-t4xx] Resource manager initialized successfully!");
            } else {
                blog(LOG_WARNING, "[obs-netint-t4xx] Resource manager initialization failed (ret=%d). Device auto-selection may not work.", rsrc_ret);
                blog(LOG_WARNING, "[obs-netint-t4xx] Check: 1) Is init_rsrc_logan.exe running with admin? 2) Try running it from: %s", 
                     "E:\\src\\t408\\t408\\V3.5.1\\release\\libxcoder_logan\\NI_MSVS2022_XCODER\\x64\\ReleaseDLL\\init_rsrc_logan.exe");
            }
        } else {
            blog(LOG_WARNING, "[obs-netint-t4xx] ni_logan_rsrc_init function not found in library - device auto-selection disabled");
        }
    }

    /* Always register encoders so they can be selected even if library is missing */
    /* This ensures the encoder options appear in OBS Studio's encoder selection UI */
    /* Actual encoder creation will fail gracefully with error messages if library is missing */
    netint_register_encoders();
    return true;
}

/**
 * @brief Plugin cleanup function called by OBS Studio when the plugin is unloaded
 * 
 * This function is called when OBS Studio shuts down or the plugin is unloaded.
 * It performs cleanup of dynamically loaded library resources.
 * 
 * Note: OBS Studio should have already destroyed all encoder instances before
 * calling this function, so we don't need to clean up encoder contexts here.
 */
void obs_module_unload(void)
{
    /* Close the dynamically loaded libxcoder library if it was opened */
    /* This releases the os_dlopen() handle and any associated resources */
    netint_loader_deinit();
}


