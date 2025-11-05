# libxcoder_logan.dll Debug Information

## 🎯 Current Configuration

The plugin is **temporarily hardcoded** to load:
```
E:\src\t408\t408\V3.5.1\release\libxcoder_logan\NI_MSVS2022_XCODER\build\libxcoder_logan.dll
```

**⚠️ TODO: Remove this hardcoded path after debugging!**

Location: `netint-libxcoder.c` lines 163-183

---

## 📁 Available DLL Builds

The libxcoder project has multiple build configurations:

### 1. **build/** - Main build output
```
E:\src\t408\t408\V3.5.1\release\libxcoder_logan\NI_MSVS2022_XCODER\build\libxcoder_logan.dll
```
- Currently used by plugin (hardcoded)
- Might be Debug or Release depending on last build

### 2. **x64/Debug/** - Debug build (static linking)
```
E:\src\t408\t408\V3.5.1\release\libxcoder_logan\NI_MSVS2022_XCODER\x64\Debug\libxcoder_logan.lib
```
- Static library only (no DLL)
- Full debug symbols

### 3. **x64/DebugDLL/** - Debug build (dynamic linking)
```
E:\src\t408\t408\V3.5.1\release\libxcoder_logan\NI_MSVS2022_XCODER\x64\DebugDLL\libxcoder_logan.dll
```
- ✅ **RECOMMENDED FOR DEBUGGING**
- Debug symbols
- Error checking enabled
- Assertions active
- Better error messages

### 4. **x64/ReleaseDLL/** - Release build (dynamic linking)
```
E:\src\t408\t408\V3.5.1\release\libxcoder_logan\NI_MSVS2022_XCODER\x64\ReleaseDLL\libxcoder_logan.dll
```
- Optimized
- Error checking might be stripped
- Used for production

---

## 🔍 Current Issue: FIFO Allocation Failure

The current DLL is returning success from `ni_logan_encode_init()` but **NOT allocating** `input_data_fifo`.

### Symptoms:
```
[obs-netint-t4xx] ni_logan_encode_init returned: 0  ← Success
[obs-netint-t4xx]   input_data_fifo = 0000000000000000  ← But NULL!
```

### Possible Causes:

1. **Release build stripped error handling**
   - Release DLL might have optimized out the error check
   - FIFO allocation failed but error was ignored

2. **DLL compiled without proper flags**
   - Missing dependencies
   - Wrong preprocessor defines

3. **Memory allocation failure in ni_logan_fifo_initialize()**
   - System resource issue
   - Shared memory not available

---

## 🧪 Testing Different DLL Versions

### Test 1: Use DebugDLL (Recommended)

**Update hardcoded path in `netint-libxcoder.c` line 163:**
```c
const char *debug_dll_path = "E:\\src\\t408\\t408\\V3.5.1\\release\\libxcoder_logan\\NI_MSVS2022_XCODER\\x64\\DebugDLL\\libxcoder_logan.dll";
```

**Expected Result:**
- Better error messages
- FIFO allocation failure logged properly
- More detailed trace output

### Test 2: Use ReleaseDLL

**Update to:**
```c
const char *debug_dll_path = "E:\\src\\t408\\t408\\V3.5.1\\release\\libxcoder_logan\\NI_MSVS2022_XCODER\\x64\\ReleaseDLL\\libxcoder_logan.dll";
```

**Expected Result:**
- Optimized code
- Might have same issue as current DLL

### Test 3: Rebuild libxcoder from Source

**If both above fail, build the DLL yourself:**
```
1. Open: E:\src\t408\t408\V3.5.1\release\libxcoder_logan\NI_MSVS2022_XCODER\libxcoder_logan.sln
2. Select configuration: DebugDLL | x64
3. Build → Rebuild Solution
4. DLL will be in: x64\DebugDLL\libxcoder_logan.dll
5. Update hardcoded path to point to newly built DLL
```

---

## 📊 Comparing DLL Versions

To check which DLL you have:

```powershell
# Check file size and date
Get-Item "E:\src\t408\t408\V3.5.1\release\libxcoder_logan\NI_MSVS2022_XCODER\build\libxcoder_logan.dll" | 
    Select-Object Name,Length,LastWriteTime

# Compare with other versions
Get-Item "E:\src\t408\t408\V3.5.1\release\libxcoder_logan\NI_MSVS2022_XCODER\x64\*DLL\libxcoder_logan.dll" | 
    Select-Object Directory,Name,Length,LastWriteTime
```

**Typically:**
- **DebugDLL:** ~5-10 MB (larger due to debug symbols)
- **ReleaseDLL:** ~2-5 MB (optimized, smaller)

---

## 🔧 Recommended Action

1. **Try DebugDLL version first** (best chance of seeing error messages)

2. **Check log output** - DebugDLL should show why FIFO allocation failed:
   ```
   ni_log(NI_LOG_ERROR, "...initialize enc frame buffer pool failed")
   ```

3. **If DebugDLL also has NULL FIFO:**
   - Check if init_rsrc_logan.exe is running
   - Check system resources (Task Manager → Performance)
   - Try rebooting the T408 card

4. **Send me the log** with DebugDLL version loaded

---

## 💡 Why Hardcoded Path Helps

**Benefits:**
- ✅ Know exactly which DLL version we're using
- ✅ No PATH conflicts with other libxcoder versions
- ✅ Can quickly test different builds
- ✅ Easier to debug DLL version issues

**After debugging:**
- ❌ Remove hardcoded path
- ✅ Use normal library search (PATH, LD_LIBRARY_PATH)
- ✅ Document which DLL version is required

---

## 📝 What to Send Me

After testing with DebugDLL version:

1. **Log output showing:**
   ```
   [obs-netint-t4xx] DEBUG MODE: Attempting to load DLL from hardcoded path:
   [obs-netint-t4xx]   E:\...\x64\DebugDLL\libxcoder_logan.dll
   [obs-netint-t4xx] DLL loaded successfully
   ```

2. **Initialization results:**
   ```
   [obs-netint-t4xx] ni_logan_encode_init returned: X
   [obs-netint-t4xx]   input_data_fifo = 0x... (or NULL)
   ```

3. **Any new error messages** from the library itself

This will tell us if the problem is the DLL version or something else!

