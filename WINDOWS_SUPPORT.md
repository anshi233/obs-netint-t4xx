# Windows Support

The NETINT T4XX encoder plugin now supports both **Linux** and **Windows** platforms.

## Changes Made

### 1. Platform Abstraction for Dynamic Library Loading

**Before (Linux-only):**
- Used POSIX `dlopen()`, `dlsym()`, `dlclose()` directly
- Required linking against `libdl` on Linux

**After (Cross-platform):**
- Uses OBS platform abstraction functions: `os_dlopen()`, `os_dlsym()`, `os_dlclose()`
- These automatically use `LoadLibrary`/`GetProcAddress`/`FreeLibrary` on Windows
- No platform-specific linking required

**Files Modified:**
- `netint-libxcoder.c`: Replaced `dlopen`/`dlsym`/`dlclose` with `os_dlopen`/`os_dlsym`/`os_dlclose`
- `netint-libxcoder.h`: Updated documentation to reflect cross-platform support

### 2. Platform-Specific Library Names

The plugin now automatically selects the correct library name based on platform:

- **Linux**: `libxcoder_logan.so`
- **Windows**: `libxcoder_logan.dll`

The library name can still be overridden via the `NETINT_LIBXCODER_PATH` environment variable.

### 3. Threading Support

**Before:**
- Used direct `#include <pthread.h>`

**After:**
- Uses `#include <util/threading.h>` from OBS
- OBS's threading.h provides cross-platform pthread support (works on Windows via emulation)

**Files Modified:**
- `netint-encoder.c`: Changed from `pthread.h` to `util/threading.h`

### 4. Build System Updates

**CMakeLists.txt Changes:**
- Added Windows platform support: `PLATFORMS WINDOWS LINUX`
- Conditionally link `libdl` only on Linux: `$<$<PLATFORM_ID:Linux>:dl>`
- Added Windows resource file (`.rc`) generation for plugin metadata

**plugins/CMakeLists.txt:**
- Updated plugin registration to include Windows: `PLATFORMS WINDOWS LINUX`

### 5. Windows Resource File

Created `cmake/windows/obs-module.rc.in` for Windows DLL metadata:
- File version information
- Product description
- Copyright information
- Embedded in plugin DLL for Windows

## Building for Windows

### Prerequisites

1. **Windows Build Environment:**
   - Visual Studio 2019 or later (or MinGW-w64)
   - CMake 3.28 or later
   - OBS Studio build environment

2. **NETINT SDK:**
   - Install NETINT libxcoder SDK for Windows
   - Ensure `libxcoder_logan.dll` is in PATH or set `NETINT_LIBXCODER_PATH`

### Build Steps

```bash
# Configure build (from OBS Studio root)
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release

# Build plugin
cmake --build build --target obs-netint-t4xx --config Release
```

The plugin will be built as `obs-netint-t4xx.dll` in the Windows build output directory.

## Runtime Behavior

### Library Loading

On Windows, the plugin will:
1. Check `NETINT_LIBXCODER_PATH` environment variable
2. If not set, look for `libxcoder_logan.dll` in:
   - Current directory
   - Directory containing OBS executable
   - System PATH directories
   - Windows system directories (System32, etc.)

### Threading

All threading operations use OBS's cross-platform pthread implementation:
- `pthread_create()` - Works on Windows
- `pthread_mutex_*()` - Works on Windows
- `pthread_join()` - Works on Windows

No Windows-specific threading code is required.

## Testing

To test on Windows:

1. **Verify Library Loading:**
   - Check OBS logs for: `[obs-netint-t4xx] Successfully loaded libxcoder_logan.dll`

2. **Test Encoder Creation:**
   - Create a new encoder in OBS
   - Select "NETINT T4XX" encoder
   - Verify encoder initializes without errors

3. **Test Encoding:**
   - Start a stream or recording
   - Verify frames are encoded successfully
   - Check logs for encoding errors

## Known Limitations

1. **Device Discovery:**
   - Device discovery may behave differently on Windows
   - Manual device specification via UI should work

2. **Path Handling:**
   - Windows uses backslashes for paths, but OBS platform functions handle this

3. **Library Dependencies:**
   - Ensure `libxcoder_logan.dll` and its dependencies are accessible
   - May need to install Visual C++ redistributables if required by NETINT SDK

## Compatibility

- **OBS Studio**: Compatible with OBS versions that support Windows plugins
- **NETINT SDK**: Requires NETINT libxcoder SDK for Windows (compatible with Linux version)
- **Windows Versions**: Should work on Windows 10 and later (tested versions may vary)

## Troubleshooting

### Plugin Not Loading

1. **Check OBS Logs:**
   - Look for `[obs-netint-t4xx]` messages
   - Check for library loading errors

2. **Verify DLL Location:**
   ```powershell
   # Check if library is found
   Get-ChildItem -Path . -Recurse -Filter "libxcoder_logan.dll"
   ```

3. **Check Dependencies:**
   ```powershell
   # Use Dependency Walker or similar tool to check DLL dependencies
   ```

### Encoding Errors

1. **Verify Hardware:**
   - Ensure NETINT T408 hardware is installed and detected
   - Check Windows Device Manager for NETINT devices

2. **Check Permissions:**
   - Ensure OBS has permissions to access hardware
   - May need to run as Administrator if hardware requires it

## Future Enhancements

Potential improvements for Windows:
- Windows-specific device discovery optimizations
- DirectShow integration (if applicable)
- Windows-specific performance optimizations
- Better error messages for Windows-specific issues

