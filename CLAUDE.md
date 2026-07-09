# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Linux kernel driver and userspace utilities for Intel RealSense D4XX series 3D depth cameras operating over GMSL (Gigabit Multimedia Serial Link) MIPI CSI-2 interface on NVIDIA Jetson platforms. Licensed under GPL-2.0.

**Supported platforms:** Jetson AGX Xavier (JetPack 5.0.2, 5.1.2) and AGX Orin (JetPack 6.0, 6.1, 6.2, 6.2.1) and AGX Thor (JetPack 7.0, 7.1)
**Supported cameras:** D457 (primary), D401, D40x, D41x, D43x, D45x series

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

On JetPack 5.x, `install_to_kernel.sh` refuses to install a kernel `Image` whose embedded `Linux version` differs from the running `uname -r` (JP5 installs only a few `.ko`, so a mismatched-JetPack Image boots with no modules and can latch the bootloader into recovery); `SKIP_KERNEL_CHECK=1` overrides. Keep this guard when editing the deploy scripts.

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
- **`kernel/kernel-5.10/`, `kernel/kernel-jammy-src/`, `kernel/kernel-noble-src/`** — Kernel patches organized by JetPack generation: 5.x uses kernel 5.10, 6.x uses kernel-jammy-src, 7.x uses kernel-noble-src.
- **`kernel/nvidia/`** — NVIDIA driver patches (max9295/max9296 SerDes, VI capture engine) organized by JetPack version.
- **`nvidia-oot/`** — Out-of-tree NVIDIA module patches for JetPack 6.x/7.x (subdirs `6.0/`, `6.1/`, `6.2/`, plus `6.2.1/` and `6.2.2/` which are symlinks to `6.2/`; JP7 lives in `7.0/`, `7.1/`). Has its own Makefile for building conftest, hwpm, nvidia-oot, nvgpu, and (except on JP7) nvidia-display modules. The JP6.x patches are **subject-grouped**: `0001-embedded-metadata` (Tegra VI/camera-core + `d4xx.o` build wiring), `0002-max9295-max9296-SerDes` (GMSL serializer+deserializer pair), `0003-Adding-max96712`, `0004-Fix-y12i-calibration-stream`, `0005-Runtime-tsc-rate-config`. `6.0/` holds the real files; `6.1/`/`6.2/` symlink the byte-identical ones back to `6.0/` and only keep a real file where content differs. (The JP7 `7.0/`/`7.1/` dirs still use the older un-split numbering.) **JP7/Thor:** the build passes `SKIP_NVIDIA_DISPLAY=1` so it does **not** produce `nvidia.ko`/`nvidia-modeset.ko`/`nvidia-drm.ko` — the bundled `nvdisplay` source is a pre-release that doesn't match the board's BSP userspace driver and breaks GPU/display init. The board keeps its matched BSP display modules, and `scripts/install_to_kernel.sh` overlays modules on JP7 (no `rm -rf /lib/modules`) so those BSP modules survive.
- **`hardware/realsense/`** — Device tree source files. Xavier uses `.dtsi` includes (`tegra194-camera-d4xx-*.dtsi`), Orin uses DT overlays (`tegra234-camera-d4xx-overlay*.dts`). Single-camera and dual-camera variants exist. (Separate `.calib.` overlay variants were removed once the driver stopped corrupting calibration-format streams on the standard overlay — calibration formats now stream on the regular DTB; metadata is simply not populated in calibration mode.) **JP7/noble Orin overlays** (e.g. `tegra234-camera-d4xx-overlay-fg12-16ch-cams-0.dtso`) must use the `.dtso` extension — the noble Makefile overlay rule only builds `.dtso` — and must **not** use `JETSON_COMPATIBLE`, since the `tegra234-p3737-0000+p3701-0000.h` platform dt-bindings header is absent from the noble tree; hardcode the `compatible` string instead. Headers like `tegra234-gpio.h` / `pinctrl-tegra.h` (and thus the TSC/`MAX1_CSI_SYNC` ext-sync fragments) are present, so keep the external-sync GPIOs. `apply_patches.sh` links these `.dtso` files into the tree and the noble `0004` patch lists their `.dtbo` under `CONFIG_ARCH_TEGRA_234_SOC`.
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
- JP 5.x: Bootlin GCC 9.3
- JP 6.x: Bootlin GCC 11.3 (`aarch64-buildroot-linux-gnu`)
- JP 7.x: ARM GNU toolchain (`aarch64-none-linux-gnu`)

`setup_workspace.sh` automatically downloads the appropriate toolchain.

### Version mapping (in `scripts/setup-common`)

| JetPack | L4T Revision | Kernel Dir |
|---------|-------------|------------|
| 5.0.2   | 35.1        | kernel/kernel-5.10 |
| 5.1.2   | 35.4.1      | kernel/kernel-5.10 |
| 6.0     | 36.3        | kernel/kernel-jammy-src |
| 6.1     | 36.4        | kernel/kernel-jammy-src |
| 6.2     | 36.4.3      | kernel/kernel-jammy-src |
| 6.2.1   | 36.4.4      | kernel/kernel-jammy-src |
| 7.0     | 38.2        | kernel/kernel-noble-src |
| 7.1     | 38.4        | kernel/kernel-noble-src |

## Branching

- `master` — primary/release branch
- `dev` — active development branch; **default target for all PRs**
- CI builds run on pushes to `master` and `dev`, and on all PRs

## Kernel ABI / module compatibility notes

- Never add a member to a public kernel struct referenced by exported symbols (e.g. `struct i2c_adapter` in `include/linux/i2c.h`). genksyms recomputes the CRC of every exported symbol referencing that struct, so prebuilt out-of-tree modules built against the unpatched headers — notably the BSP NVIDIA display stack (`nvidia.ko`/`nvidia-modeset.ko`/`nvidia-drm.ko`) — fail to load with "disagrees about version of symbol". Add only new exported functions; keep private state out of public headers. After any kernel-header patch, diff the rebuilt `vmlinux.symvers`/`Module.symvers` against what the BSP modules require (`modprobe --dump-modversions <module>.ko`).
- The i2c bus-clk-rate feature (kernel patch `0005`) is functional only on 5.x/6.x, where `tegra_i2c_xfer()` reprograms the clock from `adap->bus_clk_rate`. On 7.x the noble i2c-tegra rewrite ignores it, so `0005` is removed for 7.x and the d4xx DFU call sites are version-guarded (`#if ... LINUX_VERSION_CODE < KERNEL_VERSION(6, 8, 0)`). Do not re-add or stub `0005` on 5.x/6.x.
- JP7/Thor: do not rebuild/replace the BSP NVIDIA display stack — the bundled `nvdisplay` is a non-matching pre-release that breaks GPU/display init (black screen / blinking cursor; `/proc/driver/nvidia/version` shows `TempVersion`). `build_all.sh` passes `SKIP_NVIDIA_DISPLAY=1` for `>= 7.0`, and `install_to_kernel.sh` overlays modules on JP7 (no `rm -rf /lib/modules`).

## Concurrency notes

- In SERDES builds, hold `serdes_lock__` while scanning or assigning global topology slots (`ds5_inited[]`, `dser_inited[]`).
- Protect per-camera mutable slot state (`ds5_primary`, `depth/rgb/ir/imu_streaming`) with `struct ds5_dev::lock`.
- Protect per-deserializer slot assignment (`dser_dev`) with `struct dser_control::lock`.
- Any path that releases a primary camera slot must clear both camera-slot ownership and deserializer-slot ownership together; do not clear only `ds5_primary`. Use a shared helper so teardown/error paths stay symmetric. The `ds5_release_slot()` helper always acquires and releases `serdes_lock__` internally; callers must not hold the lock when calling it.
- For sibling-health checks, snapshot pointers/flags under lock and perform I2C probing after unlocking.
- Do not use `0x5020` as non-DFU reset-ready status; in non-DFU mode it is not a readiness source of truth. For HW reset readiness, scratch `DS5_*_CONTROL_STATUS` with a non-zero sentinel before reset and wait for FW to restore default `0x0000` after reset. Use `0x5020` only for DFU magic detection.
- After reset completion, use `DS5_DEVICE_TYPE` validity as the operational-readiness gate for code that depends on firmware-populated stream/config state. `DS5_FW_VERSION` can come back earlier and should only be treated as basic liveness, not full post-reset readiness.
- On each HW reset, clear cached values for firmware-populated readiness registers before polling readiness (for example clear `cached_device_type` before waiting for `DS5_DEVICE_TYPE`). Do not let pre-reset cache values short-circuit post-reset readiness checks.
- For polling loops expecting transient I2C failures (HWMC status checks, reset readiness polls, DFU timeout checks), use `ds5_read_poll()` which performs a single-shot regmap read without retry or logging. This prevents false warnings and excessive log spam. Reserve `ds5_read()` for normal I2C operations where retry semantics are desired.
- In `ds5_mux_s_stream()`, treat pre-toggle "already streaming" as no-op only when state is coherent; after reset-generation invalidation on start path, force stop + state clear and proceed with normal reconfiguration flow.
- In `ds5_probe()`, the DFU-magic recovery check (`DS5_DFU_MAGIC_REG` 0x5020 → `DS5_DFU_MAGIC_LSW` 0x0201) must run **before** `ds5_wait_device_type()`. A device sitting in the bootloader after an interrupted FW upgrade never serves `DS5_DEVICE_TYPE` (0x0310) — placing the device-type wait first causes a ~10 s timeout followed by `goto e_chardev` which tears down the `/dev/d4xx-dfu*` chardev, leaving the device unrecoverable over MIPI. Early-return `DS5_DFU_RECOVERY` on magic match; only then proceed to the device-type wait for operational devices.

## Post-patch instruction hygiene

After every confirmed code patch, review both `.github/copilot-instructions.md` and `CLAUDE.md` against the final net diff, including any follow-up tuning edits.

- Check for stale architectural claims and remove or correct them immediately.
- Check whether the patch exposed a reusable convention; if it did, write it down as a general rule instead of leaving it implicit in code.
- If the patch changed the locking, usage, or API contract of a helper or utility function (e.g. moved lock acquisition inside/outside, changed required caller context, or altered error handling), immediately update all documentation and instructions to reflect the new contract. Always check for this class of change after any helper edit.
- If no new convention was exposed, state that explicitly in the final report and include a short justification.
- Do not treat the task as complete until that review outcome has been reported.
