# 🔨 Rebuild Instructions - Struct Size Fix Applied

## ✅ **What Was Fixed**

Changed `ni_logan_session_data_io_t` from **8 bytes** → **416 bytes** to match library!

**Before:**
```c
typedef struct _ni_logan_session_data_io {
    struct {
        struct {
            void *p_data;  // 8 bytes
        } frame;
    } data;
} ni_logan_session_data_io_t;
```

**After:**
```c
typedef struct _ni_logan_session_data_io {
    uint8_t _opaque_union[416];  // Matches library!
} ni_logan_session_data_io_t;
```

---

## 🔨 **Rebuild OBS Plugin**

### **In Visual Studio:**
1. Right-click `obs-netint-t4xx` project in Solution Explorer
2. Click **"Rebuild"**

### **Or in PowerShell:**
```powershell
cd E:\src\t408\obs-studio\build_x64
cmake --build . --config Debug --target obs-netint-t4xx --clean-first
```

---

## ✅ **Expected Compilation Result**

### **Static Assertions Should PASS:**
```
Compiling netint-encoder.c...
Compiling netint-libxcoder.c...
static_assert checks: PASSED ✓
  sizeof(ni_logan_session_data_io_t) = 416 ✓
  sizeof(ni_logan_enc_context_t) = 688 ✓
Build succeeded.
```

If static assertions **FAIL**, the struct sizes still don't match!

---

## 📊 **Expected Runtime Result**

After rebuild and testing:

```
[libxcoder] >>> STRUCT LAYOUT (LIBRARY SIDE):
[libxcoder] >>>   p_enc_ctx = 0x...
[libxcoder] >>>   &p_enc_ctx->input_data_fifo = 0x... (offset = 544 bytes)
[libxcoder] >>>   sizeof(ni_logan_enc_context_t) = 688 bytes

[obs-netint-t4xx] STRUCT LAYOUT DEBUG:
[obs-netint-t4xx]   &ctx->enc = 0x...
[obs-netint-t4xx]   &ctx->enc.input_data_fifo = 0x... (offset = 544 bytes)  ← MATCHES!
[obs-netint-t4xx]   sizeof(ni_logan_enc_context_t) = 688 bytes  ← MATCHES!

[libxcoder] ╔═══════════════════════════════════════════════════════════════╗
[libxcoder] ║ FIFO ALLOCATION SUCCESSFUL!                                  ║
[libxcoder] ║ input_data_fifo = 0x00000184XXXXXXXX                         ║
[libxcoder] ╚═══════════════════════════════════════════════════════════════╝

[obs-netint-t4xx]   input_data_fifo = 0x00000184XXXXXXXX (should NOT be NULL)  ← NOT NULL! ✅
```

**SUCCESS! Encoder creation should work!** 🎉

---

## 🐛 **If Compilation Fails**

### **Error: static_assert failed**
```
error: static assertion failed: ni_logan_session_data_io_t size mismatch!
```

**Solution:** The 416-byte size is wrong. Check library logs for actual size and update the constant.

### **Error: 'data' is not a member**
```
error: 'data' is not a member of '_ni_logan_session_data_io'
```

**Solution:** Code is trying to access fields directly. Use `ni_logan_session_data_io_accessor_t` cast instead.

---

## 🚀 **Test Encoding**

After successful rebuild:
1. Launch OBS (F5 in Visual Studio)
2. Create NETINT encoder in Settings → Output
3. Try to start streaming/recording
4. Check log for successful encoding

**Expected:**
- ✅ Encoder creates successfully
- ✅ Frames are sent and received
- ✅ Encoded output is generated

---

**Rebuild now!** This should be the final fix! 🎯

