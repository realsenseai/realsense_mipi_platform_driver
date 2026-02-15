# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Linux kernel driver and userspace utilities for Intel RealSense D4XX series 3D depth cameras operating over GMSL (Gigabit Multimedia Serial Link) MIPI CSI-2 interface on NVIDIA Jetson platforms. Licensed under GPL-2.0.

**Supported platforms:** Jetson AGX Xavier (JetPack 4.6.1, 5.0.2, 5.1.2) and AGX Orin (JetPack 6.0, 6.1, 6.2, 6.2.1)
**Supported cameras:** D457 (primary), D401, D40x, D41x, D43x, D45x, D46x series

## Build Commands

Build dependencies (on Ubuntu):
```bash
sudo apt install -y build-essential bc wget flex bison curl libssl-dev xxd
```

Full build flow for a given JetPack version (e.g., 6.2):
```bash
./setup_workspace.sh 6.2          # Clone NVIDIA sources, install toolchain
./apply_patches.sh 6.2            # Apply D4XX patches to kernel + NVIDIA OOT modules
./build_all.sh 6.2                # Build kernel, DTBs, and driver modules
```

Build outputs go to `images/<version>/` (e.g., `images/6.x/`).

CI runs these three steps for each JetPack version (see `.github/workflows/build-jp*.yml`). CI requires `git config user.email/name` to be set before `apply_patches.sh`.

### Patch application

`apply_patches.sh` applies patches and resets them:
```bash
./apply_patches.sh [--one-cam | --dual-cam] apply <version>  # Apply patches
./apply_patches.sh reset <version>                            # Reset all patches
```
The `--one-cam`/`--dual-cam` options only apply to JetPack 5.0.2.

### Deploy to Jetson

```bash
./scripts/deploy_kernel.sh        # For JP 4.x/5.x
./scripts/deploy_kernel_6.2.sh    # For JP 6.2+
```

## Testing

Tests run on-device using pytest (Python 3). Located in `test/`.

```bash
cd test
python3 run_ci.py                          # Run all D457 tests
python3 run_ci.py -r test_fw_version       # Run specific test by regex
pytest -vs -m d457 test/                   # Direct pytest invocation
```

Pytest marker: `d457` (defined in `test/pytest.ini`). Test timeout: 200 seconds.

## Architecture

### Driver stack (top to bottom)

```
V4L2 userspace (v4l2-ctl, gstreamer, etc.)
    ↓
Kernel V4L2 / media framework
    ↓
D4XX kernel driver (kernel/realsense/d4xx.c, ~6200 lines)
    ↓ I2C
SerDes (MAX9295 serializer / MAX9296 deserializer)
    ↓ GMSL link
RealSense D4XX camera module
```

### Key directories

- **`kernel/realsense/d4xx.c`** — The main driver. Single-file V4L2 subdevice driver handling I2C communication, MIPI CSI-2 stream config, firmware control (DFU), calibration data, metadata capture, and V4L2 controls (exposure, laser power, AE ROI, etc.). Registers four sensor subdevices per camera: Depth, RGB, IR (Y8/Y8I/Y12I), and IMU.
- **`kernel/kernel-4.9/`, `kernel/kernel-5.10/`, `kernel/kernel-jammy-src/`** — Kernel patches organized by JetPack generation: 4.6.1 uses kernel 4.9, 5.x uses kernel 5.10, 6.x uses kernel-jammy-src.
- **`kernel/nvidia/`** — NVIDIA driver patches (max9295/max9296 SerDes, VI capture engine) organized by JetPack version.
- **`nvidia-oot/`** — Out-of-tree NVIDIA module patches for JetPack 6.x (subdirs `6.0/`, `6.1/`, `6.2/`, `6.2.1/`). Has its own Makefile for building conftest, hwpm, nvidia-oot, nvgpu, nvidia-display modules.
- **`hardware/realsense/`** — Device tree source files. Xavier uses `.dtsi` includes (`tegra194-camera-d4xx-*.dtsi`), Orin uses DT overlays (`tegra234-camera-d4xx-overlay*.dts`). Single-camera and dual-camera variants exist, plus `.calib.` variants for calibration.
- **`hardware/nvidia/`** — Platform-level DT patches (`t19x/galen/` for Xavier, `t23x/` for Orin T234).
- **`scripts/`** — Build orchestration. `setup-common` defines version-to-revision mappings and kernel directory selection. `source_sync_*.sh` scripts clone NVIDIA kernel repos. `SerDes_D457_*.sh` scripts configure serializer/deserializer registers.
- **`utilities/streamApp/`** — C++ streaming application with V4L2 interface (`v4l2_ds5_mipi.cpp`), camera capabilities enumeration, GUI, and firmware logging.
- **`utilities/JsonToBin/`** — Python tool to convert JSON camera presets to binary register configs.

### Video device layout (per camera)

Each camera creates 6 V4L2 video devices:
- video0: Depth (Z16)
- video1: Depth metadata (D4XX custom format)
- video2: Color RGB (RGB888/YUV422)
- video3: Color RGB metadata
- video4: IR (GREY, Y8I, Y12I)
- video5: IMU

### Cross-compilation

The build system cross-compiles for ARM64. Toolchains vary by JetPack:
- JP 4.6.1: Linaro GCC 7.3
- JP 5.x: Bootlin GCC 9.3
- JP 6.x: Bootlin GCC 11.3 (`aarch64-buildroot-linux-gnu`)

`setup_workspace.sh` automatically downloads the appropriate toolchain.

### Version mapping (in `scripts/setup-common`)

| JetPack | L4T Revision | Kernel Dir |
|---------|-------------|------------|
| 4.6.1   | 32.7.1      | kernel/kernel-4.9 |
| 5.0.2   | 35.1        | kernel/kernel-5.10 |
| 5.1.2   | 35.4.1      | kernel/kernel-5.10 |
| 6.0     | 36.3        | kernel/kernel-jammy-src |
| 6.1     | 36.4        | kernel/kernel-jammy-src |
| 6.2     | 36.4.3      | kernel/kernel-jammy-src |
| 6.2.1   | 36.4.4      | kernel/kernel-jammy-src |

## Branching

- `master` — primary/release branch
- `dev` — active development branch
- CI builds run on pushes to `master` and `dev`, and on all PRs
