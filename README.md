NETINT T4XX OBS Encoder Plugin
================================

This project provides an OBS Studio output encoder plugin that integrates NETINT T4XX Smart VPU hardware for low-power, high-density video encoding. It targets the NETINT Logan T408/T432 family and has been developed and validated on Windows using the NETINT SDK v3.5.1; Linux builds are expected to function as long as the SDK is available.

Completely created by Vibe coding powered by Cursor Composer 1

Resources
---------
- NETINT T4XX SDK and documentation: [docs.netint.com/vpu/logan](https://docs.netint.com/vpu/logan/)

Prerequisites
-------------
- OBS Studio source tree
- NETINT SDK installed
- `libxcoder_logan.dll` on Windows or `libxcoder_logan.so` on Linux available in the system `PATH` or copied next to the OBS executable

Build Instructions
------------------
1. Clone this repository inside the OBS Studio plugins source directory (e.g., `obs-studio/plugins/`).
2. Build OBS Studio as you would for any other plugin.

Post-Install Steps
------------------
- Run `init_rsrc_logan` with administrative privileges so the VPU resources are initialized correctly.
- Launch OBS Studio with administrative privileges to allow the plugin to access the hardware encoder.

Usage
-----
1. Start OBS Studio (as administrator if required by your environment).
2. Open `Settings` → `Output`, then switch to the `Advanced` output mode.
3. In the `Streaming` or `Recording` tab, select the NETINT T4XX encoder from the encoder dropdown.

Support
-------
No support - Why not DIY with vibe coding?
