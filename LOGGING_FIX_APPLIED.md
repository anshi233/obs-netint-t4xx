# 🎯 Libxcoder Logging Now Redirected to OBS

## ✅ **What I Just Fixed**

### **Problem:**
- libxcoder uses `ni_log()` which outputs to **stderr**
- OBS doesn't capture stderr in its log files
- All my debug messages in libxcoder were invisible!

### **Solution:**
- Added `netint_log_callback()` function in plugin
- Resolves `ni_log_set_callback` from libxcoder DLL
- Redirects all libxcoder logs to OBS's `blog()` system
- Logs appear with `[libxcoder]` prefix

---

## 🔨 **Rebuild OBS Plugin**

```powershell
cd E:\src\t408\obs-studio\build_x64
cmake --build . --config Debug --target obs-netint-t4xx
```

Or in Visual Studio (OBS solution):
- Right-click `obs-netint-t4xx` project
- Click "Build"

---

## 📊 **What You'll See After Rebuild**

### **When OBS Starts:**
```
[obs-netint-t4xx] DLL LOADED SUCCESSFULLY:
[obs-netint-t4xx]   E:\...\x64\DebugDLL\libxcoder_logan.dll
[obs-netint-t4xx] Setting up log callback to capture libxcoder logs...
[obs-netint-t4xx] ╔════════════════════════════════════════════════════╗
[obs-netint-t4xx] ║ LIBXCODER LOGGING REDIRECTED TO OBS              ║
[obs-netint-t4xx] ║ All libxcoder ni_log() output will now appear    ║
[obs-netint-t4xx] ║ in OBS log with [libxcoder] prefix                ║
[obs-netint-t4xx] ╚════════════════════════════════════════════════════╝
```

### **When Encoder is Created:**

#### **If libxcoder was rebuilt with LRETURN fix:**
```
[libxcoder] ╔════════════════════════════════════════════════════════════╗
[libxcoder] ║ LIBXCODER FIX APPLIED: LRETURN macro now defined         ║
[libxcoder] ║ This fixes silent error handling failures                ║
[libxcoder] ╚════════════════════════════════════════════════════════════╝
[libxcoder] >>> ATTEMPTING FIFO ALLOCATION: 30 buffers × 152 bytes...
```

Then either SUCCESS or FAILED boxes will appear!

#### **If libxcoder was NOT rebuilt:**
You won't see the boxes above - just normal libxcoder TRACE/DEBUG messages.

---

## 🎯 **Next Steps**

1. **Rebuild OBS plugin** (see above)

2. **Run OBS** (F5 in Visual Studio)

3. **Try to create NETINT encoder**

4. **Check the log** - you should now see:
   - ✅ `[libxcoder]` prefixed messages
   - ✅ Box messages if libxcoder was rebuilt
   - ✅ FIFO allocation attempt/result
   - ✅ LRETURN jump confirmation

5. **Send me the log** showing the `[libxcoder]` messages

---

## 🔍 **Troubleshooting**

### **If you see:**
```
[obs-netint-t4xx] ni_log_set_callback not found - libxcoder logs will not appear
```

**Cause:** Old DLL version doesn't export `ni_log_set_callback`

**Solution:** Rebuild libxcoder DLL from source (see REBUILD_WITH_FIX.md)

### **If you see [libxcoder] messages but NO boxes:**

**Cause:** libxcoder DLL hasn't been rebuilt with my logging additions

**Solution:** Rebuild libxcoder DLL:
1. Open `libxcoder_logan.sln`
2. Select **DebugDLL | x64**
3. Build → Rebuild Solution
4. Test OBS again

---

## ✨ **The Magic**

```c
// In netint-libxcoder.c:
static void netint_log_callback(int level, const char *fmt, va_list vl)
{
    /* Map libxcoder levels to OBS levels */
    int obs_level = (level <= 2) ? LOG_ERROR : LOG_INFO;
    
    /* Format and forward to OBS */
    char buffer[4096];
    vsnprintf(buffer, sizeof(buffer), fmt, vl);
    blog(obs_level, "[libxcoder] %s", buffer);  // ← Appears in OBS log!
}

// After loading DLL:
p_ni_log_set_callback(netint_log_callback);  // ← Redirect stderr to OBS!
```

Now **every** `ni_log()` call in libxcoder appears in OBS log! 🎉

---

**Rebuild the OBS plugin and test!** You'll finally see all the debug messages! 🚀

