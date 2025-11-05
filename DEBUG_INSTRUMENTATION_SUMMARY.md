# Debug Instrumentation Summary

## 🎉 What Has Been Added

I've added comprehensive debugging instrumentation to the **obs-netint-t4xx** plugin to help identify bugs with Visual Studio debugger.

## 📁 New/Modified Files

### New Files Created:
1. **`netint-debug.h`** - Debug instrumentation API
   - Memory sentinels for corruption detection
   - SEH exception handlers for crash catching  
   - State validators
   - Memory dump utilities
   - Auto-breakpoint macros

2. **`DEBUGGING_GUIDE.md`** - Complete debugging manual
   - How to use each debug feature
   - Common bug scenarios with solutions
   - Log message reference
   - Example debug sessions

3. **`DEBUG_QUICK_START.md`** - Quick reference
   - 3-step setup guide
   - What to send when bug occurs
   - Common scenarios
   - Pro tips

4. **`DEBUG_INSTRUMENTATION_SUMMARY.md`** - This file
   - Overview of changes
   - Next steps

### Modified Files:
1. **`netint-encoder.c`** - Core encoder with debug guards
   - Memory sentinels at start/end of `netint_ctx` struct
   - SEH guards around **ALL** libxcoder API calls
   - State validation before critical operations
   - Detailed logging before/after API calls
   - Automatic debugger breaks on errors

## 🛡️ What's Protected Now

### All Critical API Calls:
- ✅ `ni_logan_encode_init()` - With SEH guard + memory dump
- ✅ `ni_logan_encode_params_parse()` - With SEH guard + validation
- ✅ `ni_logan_encode_open()` - With SEH guard + state dump
- ✅ `ni_logan_encode_get_frame()` - With SEH guard
- ✅ `ni_logan_encode_reconfig_vfr()` - With SEH guard
- ✅ `ni_logan_encode_copy_frame_data()` - With SEH guard
- ✅ `ni_logan_encode_send()` - With SEH guard
- ✅ `ni_logan_encode_receive()` - With SEH guard (receive thread)
- ✅ `ni_logan_encode_copy_packet_data()` - With SEH guard (receive thread)

### Memory Safety:
- ✅ Buffer overflow detection (sentinels)
- ✅ Use-after-free detection (freed marker)
- ✅ NULL pointer checks
- ✅ Invalid context type detection
- ✅ Range validation (width, height, codec_format)

### State Validation:
- ✅ Encoder context structure integrity
- ✅ String pointer validity
- ✅ Width/height range (0-8192)
- ✅ Codec format validity

## 🎯 How It Works

### When Normal Operation:
```c
// Call API
int ret = p_ni_logan_encode_init(&ctx->enc);
// Returns 0 - Success!
```

**Plugin logs:**
```
[DEBUG API] Calling: p_ni_logan_encode_init(&ctx->enc)
[DEBUG STATE] BEFORE ...: (full state dump)
[DEBUG API] Returned: 0 (0x00000000)
```

**Result:** Normal execution, detailed log for debugging later

---

### When Crash Occurs:
```c
// Call API
NETINT_SEH_GUARDED_CALL(ret = p_ni_logan_encode_init(&ctx->enc), NULL);
// CRASH! Access violation in libxcoder
```

**Plugin logs:**
```
[DEBUG SEH] EXCEPTION caught during p_ni_logan_encode_init
[DEBUG SEH] Exception code: 0xC0000005 (ACCESS_VIOLATION)
[DEBUG SEH] This indicates a crash in libxcoder or invalid memory access
```

**Result:** 
- 🎯 Visual Studio debugger **breaks at exact crash location**
- 📋 Full state dump in log
- 🔍 Can examine all local variables
- ✨ No more mystery crashes!

---

### When Memory Corruption:
```c
// Some code corrupts memory
memcpy(past_end_of_struct, data, 1000);  // Oops!

// Later...
netint_encode() {
    // Sentinel check
    if (ctx->sentinel_begin.sentinel != 0xDEADBEEF) {
        // CORRUPTION DETECTED!
    }
}
```

**Plugin logs:**
```
[DEBUG] MEMORY CORRUPTION at netint_encode entry: 
    sentinel=0x41414141 (expected 0xDEADBEEF) for netint_ctx
```

**Result:**
- 🎯 Debugger breaks **before** crash happens
- 📋 Shows what overwrote the sentinel
- 🔍 Can trace back to find the culprit
- ✨ Catches corruption early!

## 📊 Debug Features Summary

| Feature | What It Does | When It Triggers |
|---------|-------------|------------------|
| **Memory Sentinels** | Guards at start/end of context | On corruption or use-after-free |
| **SEH Guards** | Catches crashes in DLL | When libxcoder crashes |
| **State Validators** | Checks context validity | Before each API call |
| **Auto Breakpoints** | Breaks into debugger | On any error/crash |
| **Memory Dumps** | Hex dump of structures | Before/after init/parse |
| **State Logging** | Full context state | Before/after API calls |

## 🚀 Next Steps for You

### Step 1: Build the Plugin
```bash
cd obs-studio/build
cmake -DCMAKE_BUILD_TYPE=Debug ..
cmake --build . --config Debug
```

### Step 2: Run OBS in Visual Studio
1. Open `obs-studio.sln`
2. Set `obs-studio` or `obs64` as startup project
3. Press **F5** (Start Debugging)

### Step 3: Trigger the Bug
- Go to Settings → Output
- Select "NETINT T4XX" as encoder
- Configure settings (bitrate, resolution, etc.)
- Start streaming/recording
- **Wait for bug to occur**

### Step 4: When Debugger Breaks
**It will show something like:**
```
Exception thrown at 0x00007FF8... in libxcoder_logan.dll: 
0xC0000005: Access violation reading location 0x0000000000000000.
```

**OR**

```
Break instruction at line 948 in netint-encoder.c
[DEBUG] MEMORY CORRUPTION detected
```

### Step 5: Gather Information
1. **Copy error message** from Output window (View → Output)
2. **Copy call stack** (Debug → Windows → Call Stack)
3. **Expand `ctx` variable** in Locals window (Debug → Windows → Locals)
4. **Screenshot the Locals window** showing `ctx->enc` structure
5. **Copy full log file** from OBS (Help → Log Files → Show Log Files)

### Step 6: Send Me the Information
Send me:
- ✅ Error message
- ✅ Call stack
- ✅ Screenshot of Locals (ctx structure)
- ✅ Full log file (last 500 lines minimum)
- ✅ What you were doing when it crashed

I'll analyze and tell you **exactly** what's wrong and how to fix it!

## 🐛 Expected Scenarios

Based on common libxcoder integration issues, here's what might happen:

### Scenario A: Crash in `ni_logan_encode_init`
**Why:** Usually NULL or invalid `dev_xcoder` pointer  
**Fix:** Initialize with empty string, not NULL  
**You'll see:**
```
[DEBUG SEH] EXCEPTION in p_ni_logan_encode_init
[DEBUG STATE] dev_xcoder='(null)' ← Problem!
```

### Scenario B: Crash in `ni_logan_encode_open`
**Why:** Resource manager not initialized, or device not available  
**Fix:** Run `init_rsrc_logan.exe` as admin  
**You'll see:**
```
[obs-netint-t4xx] Failed to open encoder device (ret=-5)
Check: Is init_rsrc_logan.exe running?
```

### Scenario C: Memory Corruption
**Why:** Struct size mismatch between plugin and DLL  
**Fix:** Update `ni_logan_enc_context_t` in shim header  
**You'll see:**
```
[DEBUG] MEMORY CORRUPTION: sentinel=0x...
[DEBUG MEM] enc context AFTER init: (shows corruption)
```

### Scenario D: Hang (No Packets)
**Why:** Encoder not started, or device busy  
**Fix:** Check `enc.started` flag and device availability  
**You'll see:**
```
[obs-netint-t4xx] Encoder HUNG: no packets for 10 seconds
[DEBUG STATE] started=0 ← Not started!
```

## 🔧 Troubleshooting the Debugger

### Issue: Debugger Doesn't Break
**Cause:** Release build instead of Debug  
**Fix:**
```bash
cmake -DCMAKE_BUILD_TYPE=Debug ..
cmake --build . --config Debug
```

### Issue: Can't See Variable Values
**Cause:** Optimizations enabled  
**Fix:** Check in `netint-encoder.c` properties:
- C/C++ → Optimization → Disabled (/Od)
- C/C++ → Debug Info → Full (/Zi)

### Issue: DLL Not Loaded
**Cause:** libxcoder_logan.dll not in PATH  
**Fix:** Copy DLL to OBS build directory:
```bash
copy t408\V3.5.1\release\libxcoder_logan\NI_MSVS2022_XCODER\build\libxcoder_logan.dll ^
     obs-studio\build\rundir\Debug\bin\64bit\
```

## 📚 Documentation Files

- **`DEBUG_QUICK_START.md`** ← Start here! Quick 3-step guide
- **`DEBUGGING_GUIDE.md`** ← Full manual with examples
- **`netint-debug.h`** ← API reference (inline docs)
- **`DEBUG_INSTRUMENTATION_SUMMARY.md`** ← This file

## 🎓 Learning Resources

### Understanding SEH (Structured Exception Handling)
Windows mechanism to catch crashes before they kill the process.
Our use: Catch crashes in libxcoder DLL and report them cleanly.

### Understanding Memory Sentinels
"Guard values" placed before/after data structures.
If corrupted → Something wrote past buffer bounds.

### Understanding Call Stack
Shows the chain of function calls leading to current point.
Most recent call at top, oldest at bottom.

## ✨ Benefits of This Instrumentation

| Before | After |
|--------|-------|
| 💥 Crash → OBS dies | 🎯 Crash → Debugger breaks at exact location |
| ❓ No idea what happened | 📋 Full context in log |
| 😢 Mystery bugs | 🔍 Clear error messages |
| ⏰ Hours of debugging | ⚡ Minutes to identify |
| 🔮 Guess and check | 🎓 Know exactly what's wrong |

## 🎬 What's Next?

1. **Build the plugin** in Debug mode
2. **Run in Visual Studio debugger** (F5)
3. **Trigger the bug** (start streaming with NETINT encoder)
4. **Send me the debug output** when debugger breaks
5. **I'll tell you exactly what's wrong** and how to fix it!

---

**Ready to catch those bugs? Let's do this! 🐛🎯**

Questions? Run OBS in the debugger and send me what you see!

