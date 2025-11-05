/**
 * @file netint-encoder.h
 * @brief Public interface for NETINT T4XX encoder plugin
 * 
 * This header file defines the public API that the main plugin module (obs-netint.c)
 * uses to interact with the encoder implementation. It provides functions for:
 * - Initializing and deinitializing the libxcoder library loader
 * - Registering encoder implementations with OBS Studio
 * 
 * The actual encoder implementation is in netint-encoder.c, which implements
 * the OBS encoder interface (obs_encoder_info callbacks).
 */

#pragma once

#include <obs-module.h>

/**
 * @brief Initialize the libxcoder library loader
 * 
 * This function attempts to dynamically load libxcoder_logan.so and resolve
 * all required function symbols. It can be called multiple times safely (idempotent).
 * 
 * If the library is not available, this function returns false but does not
 * prevent the plugin from loading. Encoders will appear in the UI but will
 * fail to create if the library is missing.
 * 
 * @return true if library loaded successfully, false otherwise
 */
bool netint_loader_init(void);

/**
 * @brief Deinitialize the libxcoder library loader
 * 
 * This function closes the dynamically loaded library and releases resources.
 * It should be called during plugin unload to clean up.
 * 
 * Note: This function does not reset function pointers, as they may still be
 * in use by existing encoder instances. OBS should destroy all encoders
 * before calling module unload.
 */
void netint_loader_deinit(void);

/**
 * @brief Register encoder implementations with OBS Studio
 * 
 * This function registers both H.264 and H.265 encoder implementations with
 * OBS Studio. After calling this, the encoders will appear in OBS Studio's
 * encoder selection UI.
 * 
 * Registration happens even if the library isn't loaded - the encoders will
 * appear in the UI but will fail to create if the library is missing.
 * 
 * This function should be called during plugin initialization (obs_module_load).
 */
void netint_register_encoders(void);


