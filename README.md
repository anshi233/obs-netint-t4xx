# NETINT T408 Encoder Plugin (obs-netint-t4xx)

This OBS encoder plugin integrates the NETINT T408 hardware encoder via libxcoder.

Runtime requirements
- libxcoder_logan.so present in the system library path (e.g., /usr/lib, /usr/local/lib)
- A NETINT T408 (or compatible) card with drivers installed

Notes
- The plugin dynamically loads libxcoder_logan at runtime; no vendor headers are required to build this plugin.
- To override the lib path, set NETINT_LIBXCODER_PATH to the absolute path of libxcoder_logan.so before launching OBS.

Build
- Built as part of the OBS build when PLATFORMS includes Linux.

Distribution
- Ship only the resulting obs-netint-t4xx module with OBS. Do not bundle libxcoder.
- Document that the target system must have NETINT drivers and libxcoder installed.

