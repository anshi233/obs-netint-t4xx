# NETINT T4XX Plugin Debugging Guide

This guide explains how to use the debugging instrumentation added to the obs-netint-t4xx plugin to identify and fix bugs.

## Overview

The plugin now includes comprehensive debugging features:
- **Memory corruption detection** - Sentinels detect buffer overflows and use-after-free
- **Crash guards** - SEH exception handling catches crashes in libxcoder API calls
- **State validation** - Validates encoder context before critical operations
- **Detailed logging** - Logs state before/after each API call
- **Debugger breaks** - Automatically breaks into Visual Studio debugger when errors occur

## Prerequisites

1. **Visual Studio** with debugger (2019 or later)
2. **OBS Studio** source code built in Debug configuration
3. **libxcoder_logan.dll** (v3.5.1 or later) in system PATH
4. **NETINT T408 hardware** with drivers installed

## Setup Instructions

### 1. Build Plugin in Debug Mode

The debugging features are enabled by default when `DEBUG_NETINT_PLUGIN` is defined in `netint-debug.h`.

```cmake
# In your CMakeLists.txt, ensure you're building in Debug:
cmake -DCMAKE_BUILD_TYPE=Debug ..
```

### 2. Configure Visual Studio Debugger

1. Open the OBS solution in Visual Studio
2. Set `obs-studio` or `obs64` as the startup project
3. Configure debugging:
   - **Debug → Properties → Debugging**
   - Set **Working Directory** to your OBS build directory
   - Ensure **Debugger Type** is set to "Auto" or "Native Only"

### 3. Enable Detailed Logging

In OBS, enable verbose logging:
- Go to **Help → Log Files → Show Log Files**
- Edit OBS config to set log level to INFO or DEBUG

## Using the Debugger

### Automatic Breakpoints

When a bug is detected, the debugger will automatically break at the **exact location**. This happens for:

1. **Memory corruption detected**
   - Sentinel values don't match (buffer overflow)
   - Sentinel shows use-after-free
   - Wrong context type (magic number mismatch)

2. **Invalid state detected**
   - Width/height out of range (0 or > 8192)
   - Invalid codec format
   - Corrupt string pointers (address < 0x10000)

3. **Crashes in libxcoder**
   - Access violation (bad pointer)
   - Division by zero
   - Illegal instruction
   - Stack overflow

### When Debugger Breaks

When the debugger breaks, you'll see:

```
[DEBUG] MEMORY CORRUPTION at <location>: sentinel=0xFEEDFACE (expected 0xDEADBEEF) for netint_ctx
```

**What to do:**
1. **Don't panic!** The debugger caught the bug before it crashed
2. **Read the error message** in the Output window (View → Output)
3. **Examine the call stack** (Debug → Windows → Call Stack)
4. **Check local variables** (Debug → Windows → Locals)
5. **Copy the full error context** from the log

### Inspecting State

When the debugger breaks, you can:

1. **View encoder context:**
   ```
   Locals window → ctx → expand
   ```
   Check:
   - `enc.width`, `enc.height`, `enc.codec_format`
   - `enc.p_session_ctx`, `enc.p_encoder_params`
   - `enc.dev_xcoder`, `enc.dev_enc_name`

2. **View sentinels:**
   ```
   ctx.sentinel_begin.sentinel  // Should be 0xDEADBEEF
   ctx.sentinel_end.sentinel    // Should be 0xCAFEBABE
   ```

3. **Check for corruption:**
   - If sentinel != expected value → Memory corruption
   - If sentinel == 0xFEEDFACE → Use-after-free

## Log Messages Reference

### Normal Operation

```
[DEBUG] Encoder context allocated at 0x000001234567890, size=1024
[DEBUG API] Calling: p_ni_logan_encode_init(&ctx->enc)
[DEBUG STATE] BEFORE ni_logan_encode_init:
  encoder=0x... started=0 flushing=0
  dev_xcoder='blkio1' dev_enc_name='blkio1' dev_enc_idx=1
  ...
[DEBUG API] ni_logan_encode_init returned: 0 (0x00000000)
```

### Memory Corruption

```
[DEBUG] MEMORY CORRUPTION at netint_encode entry: sentinel=0x41414141 (expected 0xDEADBEEF) for netint_ctx
```
**Cause:** Buffer overflow wrote past end of structure  
**Action:** Check what wrote to memory before this point

### Use-After-Free

```
[DEBUG] USE-AFTER-FREE detected at netint_encode entry for netint_ctx (ptr=0x...)
```
**Cause:** Encoder was destroyed but still being used  
**Action:** Check if OBS called destroy too early or encode after destroy

### API Crash

```
[DEBUG SEH] EXCEPTION caught during p_ni_logan_encode_init(&ctx->enc)
[DEBUG SEH] Exception code: 0xC0000005 (ACCESS_VIOLATION)
```
**Cause:** libxcoder crashed (likely bad pointer or invalid state)  
**Action:** Check encoder context fields, especially string pointers

### Invalid State

```
[DEBUG VALIDATE] Invalid width=-1 at netint_encode entry
```
**Cause:** Encoder context was corrupted or not initialized properly  
**Action:** Check initialization code

## Common Bugs and How to Debug Them

### Bug: Crash in ni_logan_encode_init

**Symptoms:**
```
[DEBUG SEH] EXCEPTION caught during p_ni_logan_encode_init
Exception code: 0xC0000005 (ACCESS_VIOLATION)
```

**Debug Steps:**
1. Check if `ctx->enc.dev_xcoder` is NULL or invalid
2. Verify `ctx->enc` structure is zero-initialized
3. Compare `ctx->enc` layout with `ni_enc_api_logan.h`
4. Check if DLL version matches headers

**Common Cause:** `dev_xcoder` not initialized (should be empty string, not NULL)

### Bug: Memory Corruption After params_parse

**Symptoms:**
```
[DEBUG] MEMORY CORRUPTION at netint_encode entry: sentinel=0x00000000
```

**Debug Steps:**
1. Check log for "enc context AFTER init" memory dump
2. Compare with "BEFORE init" dump to see what changed
3. Look for buffer overflow in `p_session_ctx` or `p_encoder_params`
4. Check if libxcoder wrote past allocated buffer

**Common Cause:** Struct size mismatch between plugin and DLL

### Bug: Use-After-Free

**Symptoms:**
```
[DEBUG] USE-AFTER-FREE detected at netint_encode entry
```

**Debug Steps:**
1. Check call stack - was destroy() called?
2. Check if OBS is calling encode() after encoder failed
3. Verify thread synchronization in receive thread

**Common Cause:** Race condition between destroy and encode

### Bug: Hang (No Packets Received)

**Symptoms:**
- No crash, but encoding doesn't work
- No packets in queue
- State shows `NETINT_ENCODER_STATE_HUNG`

**Debug Steps:**
1. Check if `p_ni_logan_encode_receive` returns 0 (no packet)
2. Verify `enc.started` flag is set
3. Check if resource manager is initialized
4. Run `ni_rsrc_list_logan.exe` to verify device

**Common Cause:** Device not opened, init_rsrc_logan not running

## Memory Dumps

When debugging memory corruption, the plugin dumps memory regions. Example:

```
[DEBUG MEM] enc context BEFORE init at 0x..., size=512:
  0000: 00 00 00 00 62 6C 6B 69 6F 31 00 00 00 00 00 00  ....blkio1......
  0010: 00 00 00 00 01 00 00 00 62 6C 6B 69 6F 31 00 00  ........blkio1..
  ...
```

**How to read:**
- First column: Offset in hex
- Middle: Hex bytes (16 per line)
- Right: ASCII representation (`.` = non-printable)

**What to look for:**
- String pointers (e.g., "blkio1")
- NULL pointers (all zeros)
- Suspicious values (0xDEADBEEF, 0xFEEDFACE = freed memory)

## Advanced: Disabling Debugging

To disable debugging for release builds:

1. Open `netint-debug.h`
2. Comment out:
   ```c
   // #define DEBUG_NETINT_PLUGIN 1
   ```
3. Rebuild plugin

All debug code becomes no-ops (zero performance impact).

## Reporting Bugs

When you find a bug, include:

1. **Full log file** with debug output
2. **Call stack** when debugger breaks
3. **Local variables** dump (ctx structure)
4. **Memory dumps** (if memory corruption)
5. **OBS version** and plugin version
6. **libxcoder version** (DLL file properties)
7. **Steps to reproduce**

## Example Debug Session

Here's a complete example of debugging a crash:

### 1. Start OBS in Debugger

```
F5 in Visual Studio
```

### 2. Enable NETINT Encoder

- Settings → Output → Encoder: "NETINT T4XX"
- Start streaming

### 3. Debugger Breaks

```
[DEBUG SEH] EXCEPTION caught during p_ni_logan_encode_open
Exception code: 0xC0000005 (ACCESS_VIOLATION)
```

Visual Studio breaks at line with NETINT_SEH_GUARDED_CALL.

### 4. Examine State

**Locals window:**
```
ctx = 0x00000123456789AB
  enc = {...}
    dev_xcoder = 0x0000000000000000  ← NULL!
    dev_enc_name = 0x00000123456789CD "blkio1"
```

**Found it!** `dev_xcoder` is NULL, but API expects string.

### 5. Check Initialization

Look at log:
```
[DEBUG STATE] BEFORE ni_logan_encode_init:
  dev_xcoder='(null)' ← Should be empty string!
```

### 6. Fix

In `netint_create()`, change:
```c
ctx->enc.dev_xcoder = (char *)bstrdup("");  // Was NULL, now empty string
```

### 7. Rebuild and Test

```
Ctrl+Shift+B to rebuild
F5 to run
```

Works! Encoder initializes successfully.

## Tips and Tricks

1. **Use breakpoints sparingly** - Let the debug guards catch issues automatically
2. **Check log first** - Often shows the problem without debugging
3. **Compare with FFmpeg** - Look at FFmpeg integration for reference
4. **Memory dumps are gold** - Show exact state before/after API calls
5. **Don't ignore warnings** - "Invalid width" might not crash, but indicates bug

## Getting Help

If you're stuck:

1. Check the log for patterns
2. Compare memory dumps before/after
3. Look at FFmpeg reference implementation
4. Check API integration guide PDF
5. Ask for help with full debug output

Good luck debugging! 🐛🔍

