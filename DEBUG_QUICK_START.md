# Quick Start: Debugging OBS NETINT Plugin

## 🎯 What's Been Added

I've added comprehensive debugging instrumentation to help you identify bugs:

✅ **Memory Guards** - Detect buffer overflows and use-after-free  
✅ **Crash Catchers** - SEH guards catch crashes in libxcoder DLL  
✅ **State Validators** - Check context validity before API calls  
✅ **Auto Breakpoints** - Debugger breaks at **exact** crash location  
✅ **Detailed Logs** - State dumps before/after each API call  

## 🚀 How to Use (3 Steps)

### Step 1: Build Plugin
```bash
cd obs-studio/build
cmake -DCMAKE_BUILD_TYPE=Debug ..
cmake --build . --config Debug
```

### Step 2: Run in Visual Studio
1. Open `obs-studio.sln` in Visual Studio
2. Set `obs-studio` as startup project
3. Press **F5** to run with debugger attached

### Step 3: Trigger the Bug
- Enable NETINT encoder in OBS settings
- Start streaming/recording
- When bug occurs → **Debugger will break automatically!**

## 🔍 What Happens When Bug Occurs

### Before (without debugging):
```
OBS crashes silently
No idea where or why
😢
```

### After (with debugging):
```
[DEBUG SEH] EXCEPTION caught during p_ni_logan_encode_init
[DEBUG SEH] Exception code: 0xC0000005 (ACCESS_VIOLATION)
[DEBUG STATE] encoder context:
  dev_xcoder='(null)' ← Problem found!
  dev_enc_name='blkio1'
  
🎯 Debugger breaks at EXACT line
📋 Full context in log
✨ Easy to fix
```

## 📋 What to Send Me

When the debugger breaks, send me:

### 1. Error Message (from Output window)
```
[DEBUG SEH] EXCEPTION caught during p_ni_logan_encode_init
Exception code: 0xC0000005 (ACCESS_VIOLATION)
```

### 2. Call Stack (Debug → Windows → Call Stack)
```
obs-netint-t4xx.dll!netint_create() Line 601
obs64.exe!obs_encoder_create()
...
```

### 3. Local Variables (Debug → Windows → Locals)
Expand `ctx` and copy:
```
ctx = 0x...
  enc = {...}
    dev_xcoder = 0x0000000000000000  ← NULL
    dev_enc_name = "blkio1"
    width = 1920
    height = 1080
```

### 4. Log File
Copy everything from OBS log (Help → Log Files → Show Log Files)

## 🎯 Common Scenarios

### Scenario 1: Crash During Init
**You'll see:**
```
[DEBUG SEH] EXCEPTION caught during p_ni_logan_encode_init
```

**Debugger breaks at:** `netint_create()` line ~601  
**Check:** `ctx->enc.dev_xcoder`, `ctx->enc.dev_enc_name`, struct layout

### Scenario 2: Memory Corruption
**You'll see:**
```
[DEBUG] MEMORY CORRUPTION: sentinel=0x41414141 (expected 0xDEADBEEF)
```

**Debugger breaks at:** Entry to function detecting corruption  
**Check:** What wrote to memory before this? (use memory dumps)

### Scenario 3: Use-After-Free
**You'll see:**
```
[DEBUG] USE-AFTER-FREE detected for netint_ctx (ptr=0x...)
```

**Debugger breaks at:** Function trying to use freed memory  
**Check:** Call stack to see who destroyed the encoder

### Scenario 4: Encoder Hangs (No Crash)
**You'll see:**
```
[obs-netint-t4xx] Encoder appears HUNG: no packets for 10 seconds
```

**Debugger doesn't break** (not a crash)  
**Check:** Log for repeated `ni_logan_encode_receive returned: 0`

## 💡 Pro Tips

### Tip 1: Let Debugger Catch It
**Don't** set manual breakpoints initially. The debug guards will break at the **exact** problem location automatically.

### Tip 2: Read the Log First
Before debugging, scan the log for:
- `[DEBUG SEH]` - Crashes
- `[DEBUG VALIDATE]` - Invalid state
- `[DEBUG] MEMORY CORRUPTION` - Buffer overflow

### Tip 3: Compare Memory Dumps
Look for the "BEFORE" and "AFTER" dumps:
```
[DEBUG MEM] enc context BEFORE init: ...
[DEBUG MEM] enc context AFTER init: ...
```

Compare them to see what changed.

### Tip 4: Check FFmpeg Reference
If confused about API usage:
```
t408\V3.5.1\release\FFmpeg-n4.3.1\NI_MSVS2022-n4.3.1\libavcodec\nienc_logan.c
```

## 🐛 Example Bug Fix Workflow

### 1. Run OBS in Debugger
```
F5 in Visual Studio
```

### 2. Reproduce Bug
- Select NETINT encoder
- Start streaming

### 3. Debugger Breaks
```
[DEBUG SEH] EXCEPTION in p_ni_logan_encode_init
Access Violation at 0x00007FF...
```

### 4. Check Locals
```
ctx->enc.dev_xcoder = NULL  ← Found it!
```

### 5. Fix in Code
```c
// Was: ctx->enc.dev_xcoder = NULL;
// Fix: 
ctx->enc.dev_xcoder = (char *)bstrdup("");
```

### 6. Rebuild and Test
```
Ctrl+Shift+B → F5
```

### 7. Works! ✅

## 🔧 Troubleshooting

### Debugger Doesn't Break
**Problem:** Debug build not used  
**Fix:** Check CMAKE_BUILD_TYPE=Debug and rebuild

### Too Much Logging
**Problem:** LOG_DEBUG fills up logs  
**Fix:** In OBS, set log level to INFO instead of DEBUG

### Can't See Variables
**Problem:** Optimizations enabled  
**Fix:** Ensure Debug build (not RelWithDebInfo)

### DLL Not Found
**Problem:** libxcoder_logan.dll not in PATH  
**Fix:** Copy DLL to OBS build directory or add to PATH

## 📚 Full Documentation

For complete details, see:
- `DEBUGGING_GUIDE.md` - Comprehensive debugging manual
- `netint-debug.h` - Debug API reference

## ❓ Questions?

When you have the debugger output, send me:
1. The error message
2. Call stack
3. Local variables (especially `ctx->enc` structure)
4. Full log

I'll analyze it and tell you exactly what's wrong and how to fix it!

---

**Ready to debug? Press F5 and let's catch those bugs! 🐛→🎯**

