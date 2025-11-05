/**
 * @file netint-debug.h
 * @brief Debugging and validation helpers for NETINT T4XX plugin
 * 
 * This header provides debugging instrumentation to help identify bugs:
 * - Memory validation sentinels
 * - API call guards with state dumps
 * - Crash detection with debugger breaks
 * - Detailed state logging
 * 
 * Usage:
 * 1. Enable DEBUG_NETINT_PLUGIN in build or define here
 * 2. Run OBS under Visual Studio debugger
 * 3. When validation fails, debugger will break at exact location
 * 4. Examine state dump in log and local variables
 */

#pragma once

#include <obs-module.h>
#include <stdint.h>
#include <stdio.h>

/* Enable debugging - comment out for release builds */
/* DISABLED: Encoder is working now, debug breaks interfere with encoding */
/* #define DEBUG_NETINT_PLUGIN 1 */

/* Memory sentinel values for corruption detection */
#define NETINT_SENTINEL_BEGIN 0xDEADBEEF
#define NETINT_SENTINEL_END   0xCAFEBABE
#define NETINT_SENTINEL_FREED 0xFEEDFACE

#ifdef DEBUG_NETINT_PLUGIN

/* Visual Studio debugger break - only available in debug mode */
#ifdef _WIN32
#include <intrin.h>
#define NETINT_DEBUGBREAK() __debugbreak()
#else
#include <signal.h>
#define NETINT_DEBUGBREAK() raise(SIGTRAP)
#endif

/**
 * @brief Structure for tracking context validity
 * 
 * Place at beginning and end of context structures to detect:
 * - Buffer overflows
 * - Use-after-free
 * - Memory corruption
 */
typedef struct {
    uint32_t sentinel;      /* Should always be NETINT_SENTINEL_BEGIN or _END */
    uint32_t magic;         /* Context-specific magic number */
    const char *type_name;  /* Human-readable type name */
    void *self_ptr;         /* Pointer to containing structure (for validation) */
} netint_debug_sentinel_t;

/**
 * @brief Initialize a debug sentinel
 * 
 * @param s Pointer to sentinel
 * @param value Sentinel value (BEGIN or END)
 * @param magic Magic number for this context type
 * @param type_name Human-readable type name
 * @param self Pointer to containing structure
 */
static inline void netint_debug_sentinel_init(netint_debug_sentinel_t *s, uint32_t value, 
                                               uint32_t magic, const char *type_name, void *self)
{
    s->sentinel = value;
    s->magic = magic;
    s->type_name = type_name;
    s->self_ptr = self;
}

/**
 * @brief Validate a debug sentinel
 * 
 * @param s Pointer to sentinel
 * @param expected_value Expected sentinel value (BEGIN or END)
 * @param expected_magic Expected magic number
 * @param location Source location string for error reporting
 * @return true if valid, false if corrupted (triggers debugger break)
 */
static inline bool netint_debug_sentinel_check(const netint_debug_sentinel_t *s, 
                                                uint32_t expected_value,
                                                uint32_t expected_magic,
                                                const char *location)
{
    if (!s) {
        blog(LOG_ERROR, "[DEBUG] NULL sentinel at %s", location);
        NETINT_DEBUGBREAK();
        return false;
    }
    
    if (s->sentinel == NETINT_SENTINEL_FREED) {
        blog(LOG_ERROR, "[DEBUG] USE-AFTER-FREE detected at %s for %s (ptr=%p)", 
             location, s->type_name ? s->type_name : "unknown", s->self_ptr);
        NETINT_DEBUGBREAK();
        return false;
    }
    
    if (s->sentinel != expected_value) {
        blog(LOG_ERROR, "[DEBUG] MEMORY CORRUPTION at %s: sentinel=0x%08X (expected 0x%08X) for %s",
             location, s->sentinel, expected_value, s->type_name ? s->type_name : "unknown");
        NETINT_DEBUGBREAK();
        return false;
    }
    
    if (s->magic != expected_magic) {
        blog(LOG_ERROR, "[DEBUG] WRONG CONTEXT TYPE at %s: magic=0x%08X (expected 0x%08X) for %s",
             location, s->magic, expected_magic, s->type_name ? s->type_name : "unknown");
        NETINT_DEBUGBREAK();
        return false;
    }
    
    return true;
}

/**
 * @brief Mark sentinel as freed (for use-after-free detection)
 */
static inline void netint_debug_sentinel_mark_freed(netint_debug_sentinel_t *s)
{
    if (s) {
        s->sentinel = NETINT_SENTINEL_FREED;
    }
}

/**
 * @brief Log detailed encoder context state
 * 
 * @param ctx Encoder context
 * @param location Source location
 */
#define NETINT_LOG_ENCODER_STATE(ctx, location) \
    do { \
        if (ctx) { \
            blog(LOG_INFO, "[DEBUG STATE] %s:", location); \
            blog(LOG_INFO, "  encoder=%p started=%d flushing=%d", \
                 (ctx)->encoder, (ctx)->enc.started, (ctx)->flushing); \
            blog(LOG_INFO, "  dev_xcoder='%s' dev_enc_name='%s' dev_enc_idx=%d", \
                 (ctx)->enc.dev_xcoder ? (ctx)->enc.dev_xcoder : "(null)", \
                 (ctx)->enc.dev_enc_name ? (ctx)->enc.dev_enc_name : "(null)", \
                 (ctx)->enc.dev_enc_idx); \
            blog(LOG_INFO, "  p_session_ctx=%p p_encoder_params=%p p_input_fme=%p", \
                 (ctx)->enc.p_session_ctx, (ctx)->enc.p_encoder_params, (ctx)->enc.p_input_fme); \
            blog(LOG_INFO, "  width=%d height=%d codec_format=%d pix_fmt=%d", \
                 (ctx)->enc.width, (ctx)->enc.height, (ctx)->enc.codec_format, (ctx)->enc.pix_fmt); \
            blog(LOG_INFO, "  bit_rate=%lld timebase=%d/%d", \
                 (long long)(ctx)->enc.bit_rate, (ctx)->enc.timebase_num, (ctx)->enc.timebase_den); \
            blog(LOG_INFO, "  got_headers=%d extra=%p extra_size=%zu", \
                 (ctx)->got_headers, (ctx)->extra, (ctx)->extra_size); \
            blog(LOG_INFO, "  consecutive_errors=%d total_errors=%d", \
                 (ctx)->consecutive_errors, (ctx)->total_errors); \
        } else { \
            blog(LOG_ERROR, "[DEBUG STATE] %s: ctx is NULL!", location); \
        } \
    } while (0)

/**
 * @brief Guard macro for API calls with detailed logging
 * 
 * Logs before/after API call and validates return value.
 * On error, logs full state and breaks into debugger.
 */
#define NETINT_API_CALL_GUARD(ctx, api_call, expected_success) \
    do { \
        blog(LOG_INFO, "[DEBUG API] Calling: " #api_call); \
        NETINT_LOG_ENCODER_STATE(ctx, "BEFORE " #api_call); \
        int _ret = (api_call); \
        blog(LOG_INFO, "[DEBUG API] " #api_call " returned: %d (0x%08X)", _ret, _ret); \
        NETINT_LOG_ENCODER_STATE(ctx, "AFTER " #api_call); \
        if (expected_success && _ret < 0) { \
            blog(LOG_ERROR, "[DEBUG API] " #api_call " FAILED with ret=%d", _ret); \
            blog(LOG_ERROR, "[DEBUG API] Breaking into debugger..."); \
            NETINT_DEBUGBREAK(); \
        } \
    } while (0)

/**
 * @brief Validate pointer is not NULL
 */
#define NETINT_CHECK_NULL(ptr, name, location) \
    do { \
        if (!(ptr)) { \
            blog(LOG_ERROR, "[DEBUG CHECK] NULL pointer: %s at %s", name, location); \
            NETINT_DEBUGBREAK(); \
        } \
    } while (0)

/**
 * @brief Validate encoder context structure fields
 */
#define NETINT_VALIDATE_ENC_CONTEXT(ctx, location) \
    do { \
        if (!(ctx)) { \
            blog(LOG_ERROR, "[DEBUG VALIDATE] NULL encoder context at %s", location); \
            NETINT_DEBUGBREAK(); \
        } else { \
            /* Check for obviously corrupt values */ \
            if ((ctx)->enc.width <= 0 || (ctx)->enc.width > 8192) { \
                blog(LOG_ERROR, "[DEBUG VALIDATE] Invalid width=%d at %s", (ctx)->enc.width, location); \
                NETINT_DEBUGBREAK(); \
            } \
            if ((ctx)->enc.height <= 0 || (ctx)->enc.height > 8192) { \
                blog(LOG_ERROR, "[DEBUG VALIDATE] Invalid height=%d at %s", (ctx)->enc.height, location); \
                NETINT_DEBUGBREAK(); \
            } \
            if ((ctx)->enc.codec_format < 0 || (ctx)->enc.codec_format > 10) { \
                blog(LOG_ERROR, "[DEBUG VALIDATE] Invalid codec_format=%d at %s", (ctx)->enc.codec_format, location); \
                NETINT_DEBUGBREAK(); \
            } \
            /* Validate string pointers are not wild */ \
            if ((ctx)->enc.dev_xcoder && ((uintptr_t)(ctx)->enc.dev_xcoder < 0x10000)) { \
                blog(LOG_ERROR, "[DEBUG VALIDATE] Corrupt dev_xcoder=%p at %s", (ctx)->enc.dev_xcoder, location); \
                NETINT_DEBUGBREAK(); \
            } \
            if ((ctx)->enc.dev_enc_name && ((uintptr_t)(ctx)->enc.dev_enc_name < 0x10000)) { \
                blog(LOG_ERROR, "[DEBUG VALIDATE] Corrupt dev_enc_name=%p at %s", (ctx)->enc.dev_enc_name, location); \
                NETINT_DEBUGBREAK(); \
            } \
        } \
    } while (0)

/**
 * @brief Dump memory region as hex for debugging
 */
static inline void netint_debug_dump_memory(const void *ptr, size_t size, const char *label)
{
    if (!ptr) {
        blog(LOG_INFO, "[DEBUG MEM] %s: NULL pointer", label);
        return;
    }
    
    blog(LOG_INFO, "[DEBUG MEM] %s at %p, size=%zu:", label, ptr, size);
    
    const uint8_t *bytes = (const uint8_t *)ptr;
    size_t dump_size = size < 256 ? size : 256; /* Limit to 256 bytes */
    
    for (size_t i = 0; i < dump_size; i += 16) {
        char hex_buf[64] = {0};
        char ascii_buf[20] = {0};
        
        for (size_t j = 0; j < 16 && (i + j) < dump_size; j++) {
            uint8_t byte = bytes[i + j];
            sprintf(hex_buf + j * 3, "%02X ", byte);
            ascii_buf[j] = (byte >= 32 && byte < 127) ? byte : '.';
        }
        
        blog(LOG_INFO, "  %04zX: %-48s %s", i, hex_buf, ascii_buf);
    }
    
    if (size > 256) {
        blog(LOG_INFO, "  ... (truncated, total size=%zu)", size);
    }
}

/**
 * @brief Windows SEH exception filter for crash catching
 */
#ifdef _WIN32
#include <windows.h>

static inline const char* netint_exception_code_to_string(DWORD code)
{
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_INVALID_HANDLE: return "INVALID_HANDLE";
        case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
        default: return "UNKNOWN";
    }
}

#define NETINT_SEH_GUARDED_CALL(api_call, error_return_value) \
    do { \
        __try { \
            api_call; \
        } __except (EXCEPTION_EXECUTE_HANDLER) { \
            DWORD _code = GetExceptionCode(); \
            blog(LOG_ERROR, "[DEBUG SEH] EXCEPTION caught during " #api_call); \
            blog(LOG_ERROR, "[DEBUG SEH] Exception code: 0x%08X (%s)", _code, netint_exception_code_to_string(_code)); \
            blog(LOG_ERROR, "[DEBUG SEH] This indicates a crash in libxcoder or invalid memory access"); \
            NETINT_DEBUGBREAK(); \
            return error_return_value; \
        } \
    } while (0)

#else
/* Non-Windows: No SEH, just call directly */
#define NETINT_SEH_GUARDED_CALL(api_call, error_return_value) \
    do { \
        api_call; \
    } while (0)
#endif

#else /* !DEBUG_NETINT_PLUGIN */

/* Release mode - all debug macros become no-ops */
#define NETINT_DEBUGBREAK() ((void)0)
#define NETINT_LOG_ENCODER_STATE(ctx, location) ((void)0)
#define NETINT_API_CALL_GUARD(ctx, api_call, expected_success) (api_call)
#define NETINT_CHECK_NULL(ptr, name, location) ((void)0)
#define NETINT_VALIDATE_ENC_CONTEXT(ctx, location) ((void)0)
#define netint_debug_dump_memory(ptr, size, label) ((void)0)
#define NETINT_SEH_GUARDED_CALL(api_call, error_return_value) (api_call)

#endif /* DEBUG_NETINT_PLUGIN */

/**
 * @brief Magic number for encoder context validation
 */
#define NETINT_ENC_CONTEXT_MAGIC 0x4E455449  /* "NETI" in hex */

