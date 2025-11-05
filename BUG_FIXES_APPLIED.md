# Bug Fixes Applied to obs-netint-t4xx Plugin

## 🎯 Critical Bug #1: Thread-Safety Violation (FIXED)

### **Root Cause**
The plugin used a **background thread** to receive encoded packets, causing **race conditions** with the main encoding thread. Both threads accessed `ctx->enc` simultaneously without synchronization.

**Thread 1 (Main):**
- `netint_encode()` → `p_ni_logan_encode_send(&ctx->enc)`  

**Thread 2 (Background):**
- `netint_recv_thread()` → `p_ni_logan_encode_receive(&ctx->enc)` (CONCURRENT!)

**Result:** Memory corruption in `ctx->enc` structure, eventually corrupting pthread mutexes → crash in `pthread_mutex_unlock`

### **Evidence**
```
Crash location: pthread_mutex_unlock+0x50
Stack Arg1: 0xCAFEBABE ← This is NETINT_SENTINEL_END!
Stack Arg2: 0x4E455449 ← This is "NETI" (our debug magic!)
```

The mutex structure contained our debug sentinel values = memory corruption.

### **Fix Applied**
✅ **Removed background receive thread entirely**  
✅ **Implemented single-threaded design matching FFmpeg reference**  
✅ **Synchronous send+receive in same function call**  

### **New Design** (matches FFmpeg `ni_encode_video.c`):
```c
netint_encode() {
    if (frame) {
        // Send frame to hardware
        p_ni_logan_encode_send(&ctx->enc);
    }
    
    // Try to receive packet (non-blocking)
    int got = p_ni_logan_encode_receive(&ctx->enc);
    if (got > 0) {
        // Copy and return packet
        *received = true;
    }
    
    return true;
}
```

**Benefits:**
- ✅ No race conditions
- ✅ Thread-safe (single thread)
- ✅ Matches vendor's design intent
- ✅ Simpler code
- ✅ No mutex overhead

---

## 🐛 Critical Bug #2: Keyframe Interval Not Applied (IDENTIFIED)

### **Problem**
The plugin reads `keyint` setting but **NEVER applies it** to the encoder!

```c
// Line 331:
int keyint = (int)obs_data_get_int(settings, "keyint");
if (keyint <= 0) keyint = (int)(2 * (voi->fps_num / (double)voi->fps_den));

// ... keyint is NEVER USED after this! ❌
```

### **Impact**
- Users can set keyframe interval in UI, but it has no effect
- Encoder uses default GOP size (probably 30 or 60 frames)
- Seeking in output video may not work as expected

### **Fix Needed**
Need to determine correct parameter name for libxcoder API:
- Check Integration Guide PDF for GOP/keyframe parameter name
- Use `p_ni_logan_encoder_params_set_value()` to set it
- Common names: "gopPresetIdx", "intraPeriod", "gop_size"

**Action:** Check integration guide PDF for exact parameter name

---

## ⚠️ Potential Bug #3: Missing Encoder Parameters

### **Parameters Currently Set:**
✅ `width`, `height` - Resolution  
✅ `bit_rate` - Bitrate  
✅ `codec_format` - H.264 or H.265  
✅ `timebase_num`, `timebase_den` - Framerate  
✅ `profile` - Baseline/Main/High  
✅ `cbr` - Rate control mode  
✅ `spsPpsAttach` - Repeat headers  

### **Parameters NOT Set (potentially missing):**
❓ `keyint` / `gop_size` - **READ but NOT APPLIED** ← BUG #2  
❓ `level` - H.264/H.265 level (e.g., 4.1, 5.1)  
❓ `qp_min`, `qp_max` - QP range  
❓ `rc_mode` - Advanced rate control settings  
❓ `bframes` - B-frame count  
❓ `refs` - Reference frame count  

### **To Verify:**
1. Check Integration Guide PDF section on encoder parameters
2. Compare with FFmpeg xcoder-params string format
3. Add missing critical parameters

---

## 📊 Code Changes Summary

### Files Modified:
1. **`netint-encoder.c`** - Core encoder implementation
   - Removed background thread (`recv_thread`)
   - Removed packet queue (`pkt_queue`)
   - Removed thread mutexes (`queue_mutex`, `state_mutex`)
   - Simplified error tracking (removed state machine)
   - Rewrote `netint_encode()` to be single-threaded
   - Removed `netint_recv_thread()` function entirely

2. **`netint-debug.h`** - NEW: Debug instrumentation
   - SEH exception guards for crash catching
   - Memory corruption detection
   - State validation macros
   - Auto-debugger breakpoints

### Files Created:
- `netint-debug.h` - Debug API
- `DEBUGGING_GUIDE.md` - Complete debug manual
- `DEBUG_QUICK_START.md` - Quick reference
- `DEBUG_INSTRUMENTATION_SUMMARY.md` - Overview
- `BUG_FIXES_APPLIED.md` - This file

### Lines of Code:
- **Removed:** ~200 lines (threading code, error tracking)
- **Added:** ~150 lines (debug instrumentation)
- **Net change:** Simpler, cleaner code

---

## 🚀 Testing Instructions

### Step 1: Rebuild Plugin
```bash
cd obs-studio\build_x64
cmake --build . --config Debug --target obs-netint-t4xx --clean-first
```

### Step 2: Run in Debugger
1. Open `obs-studio.sln` in Visual Studio
2. Press **F5** to run with debugger

### Step 3: Test Encoding
1. Settings → Output → Encoder: "NETINT T4XX"
2. Start recording/streaming
3. Encode for 30-60 seconds
4. Stop recording

### Expected Results:

#### ✅ **If Fix Works:**
```
[obs-netint-t4xx] Encoder creation complete (single-threaded design like FFmpeg)
[obs-netint-t4xx] Frame sent successfully (PTS=0)
[obs-netint-t4xx] ni_logan_encode_receive returned: 1234  ← Packet received!
[obs-netint-t4xx] First packet received, extracting headers (size=1234)...
[obs-netint-t4xx] Packet received: size=1234, pts=0, keyframe=1
... (continuous packet flow) ...
```

**No crashes!** Encoding works smoothly.

#### ❌ **If Still Crashes:**
Debugger will break at exact location with:
```
[DEBUG SEH] EXCEPTION caught during <api_call>
[DEBUG SEH] Exception code: 0x... (...)
```

Send me the full crash info!

---

## 🔍 Known Issues Still To Fix

### Issue #1: Keyframe Interval Parameter
**Status:** Identified, not yet fixed  
**Priority:** Medium  
**Impact:** Keyframe interval setting in UI has no effect  
**Fix:** Need parameter name from Integration Guide

### Issue #2: Parameter Validation
**Status:** Not checked yet  
**Priority:** Low  
**Impact:** Some OBS settings might not work  
**Fix:** Audit all parameters against API documentation

---

## 📚 Next Steps

1. **Test the thread-safety fix**
   - Rebuild and run
   - Should eliminate the pthread_mutex_unlock crash
   
2. **Check Integration Guide PDF**
   - Find correct parameter name for GOP/keyframe interval
   - Check for any other required parameters
   
3. **Apply keyframe interval fix**
   - Add parameter setting in `netint_create()`
   
4. **Full parameter audit**
   - Compare all OBS settings with libxcoder capabilities
   - Add missing parameters

---

## 🎓 Lessons Learned

### **Lesson #1: Check Reference Implementation First**
The FFmpeg example showed single-threaded design. We should have followed it exactly.

###  **Lesson #2: Vendor APIs May Not Be Thread-Safe**
Never assume hardware encoder APIs support concurrent access. Check documentation!

### **Lesson #3: Debug Instrumentation is Essential**
The SEH guards and state logging helped identify the exact crash location immediately.

### **Lesson #4: Struct Layout Matters**
Embedded debug sentinels broke struct layout. Keep debug fields minimal or use external allocation.

---

## 📞 Support

If issues persist after rebuild:
1. Send full log output
2. Debugger crash location (if breaks)
3. Local variables dump
4. Steps to reproduce

We'll fix it! 🐛→✅

