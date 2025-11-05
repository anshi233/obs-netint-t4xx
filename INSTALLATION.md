# Plugin Installation Guide

## Installation Summary

The NETINT T4XX encoder plugin has been successfully compiled and installed.

### Installation Locations

1. **Build Runtime Directory** (for development/testing):
   ```
   /home/ubuntu/src/t408/obs-studio/build/rundir/Release/lib/obs-plugins/obs-netint-t4xx.so
   ```
   - Use this when running OBS from the build directory
   - Run OBS with: `/home/ubuntu/src/t408/obs-studio/build/rundir/Release/bin/obs`

2. **Install Directory** (custom install path):
   ```
   /home/ubuntu/obs-studio-install/lib/obs-plugins/obs-netint-t4xx.so
   ```
   - Use this if OBS is installed to `/home/ubuntu/obs-studio-install`
   - Set OBS_PLUGIN_PATH environment variable if needed

### Plugin Information

- **Plugin Name**: obs-netint-t4xx
- **Version**: 1.0.0
- **File Size**: ~28 KB
- **Architecture**: x86-64 (64-bit)
- **Type**: Shared library (.so)

### How to Use

1. **Start OBS Studio** from the build directory:
   ```bash
   /home/ubuntu/src/t408/obs-studio/build/rundir/Release/bin/obs
   ```

2. **Or if OBS is installed system-wide**, ensure the plugin is in the plugin directory:
   ```bash
   # Plugin should be in one of these locations:
   # - /usr/lib/obs-plugins/
   # - /usr/local/lib/obs-plugins/
   # - ~/.config/obs-studio/plugins/obs-netint-t4xx/
   ```

3. **In OBS Studio**:
   - Go to Settings → Output → Streaming (or Recording)
   - Select "Encoder" dropdown
   - Choose "NETINT T4XX" for H.264 or H.265 encoding

### Requirements

- **libxcoder_logan.so** must be installed and accessible
  - Default location: `/usr/lib/` or `/usr/local/lib/`
  - Or set `NETINT_LIBXCODER_PATH` environment variable to point to library
- **NETINT T408 hardware** with drivers installed
- **OBS Studio** must be running

### Verification

To verify the plugin is loaded:

1. **Check OBS logs** for plugin loading messages:
   ```
   [obs-netint-t4xx] Plugin version 1.0.0 loading on OBS...
   [obs-netint-t4xx] Successfully loaded libxcoder_logan.so
   ```

2. **Check OBS plugin list**:
   - Settings → Plugins
   - Look for "NETINT T408 Hardware Encoder (libxcoder)"

3. **Test encoder creation**:
   - Try to create an encoder instance
   - Check logs for: "Encoder initialized: 1920x1080 @ 6000 kbps"

### Troubleshooting

If plugin doesn't load:

1. **Check library path**:
   ```bash
   ldd /home/ubuntu/src/t408/obs-studio/build/rundir/Release/lib/obs-plugins/obs-netint-t4xx.so
   ```

2. **Check for libxcoder_logan.so**:
   ```bash
   find /usr -name "libxcoder_logan.so" 2>/dev/null
   ```

3. **Set library path**:
   ```bash
   export NETINT_LIBXCODER_PATH=/path/to/libxcoder_logan.so
   ```

4. **Check OBS logs**:
   - Look in OBS log viewer or console output
   - Search for "[obs-netint-t4xx]" messages

### Recompilation

To recompile the plugin:

```bash
cd /home/ubuntu/src/t408/obs-studio/build
cmake --build . --target obs-netint-t4xx -j$(nproc)
```

### Reinstallation

To reinstall the plugin:

```bash
cd /home/ubuntu/src/t408/obs-studio/build
cmake --install . --component obs-netint-t4xx
```

### System-Wide Installation (Optional)

If you want to install to system paths:

```bash
cd /home/ubuntu/src/t408/obs-studio/build
sudo cmake --install . --component obs-netint-t4xx --prefix /usr/local
```

This will install to:
- `/usr/local/lib/obs-plugins/obs-netint-t4xx.so`
- `/usr/local/share/obs/obs-plugins/obs-netint-t4xx/` (data files)

**Note**: Make sure OBS Studio is installed system-wide for this to work.

