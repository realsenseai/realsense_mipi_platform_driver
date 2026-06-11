# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Linux kernel driver and userspace utilities for Intel RealSense D4XX series 3D depth cameras operating over GMSL (Gigabit Multimedia Serial Link) MIPI CSI-2 interface on NVIDIA Jetson platforms. Licensed under GPL-2.0.

**Supported platforms:** Jetson AGX Xavier (JetPack 5.0.2, 5.1.2) and AGX Orin (JetPack 6.0, 6.1, 6.2, 6.2.1)
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

CI also runs a V4L2 test workflow on a self-hosted Jetson runner when `kernel/realsense/**` or `test/v4l2_test/**` paths change.

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
- JP 5.x: Bootlin GCC 9.3
- JP 6.x: Bootlin GCC 11.3 (`aarch64-buildroot-linux-gnu`)

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

## Coding conventions

### Kernel driver (C — `kernel/realsense/d4xx.c`)

- **Keep changes minimal**: no code inflation. Prefer concise single-line forms; reuse existing helpers and paths instead of adding new ones.
- **Before adding new code, check callers**: verify equivalent logic doesn't already exist in the call chain. If a function has one caller, put the logic there. Never duplicate logic — consolidate first.
- **After removing code, clean up stale references**: immediately remove defines, variables, struct fields, forward declarations, and comments that were only used by the removed code. Do not leave dead code behind.
- **Naming**: functions prefixed `ds5_` (mux functions `ds5_mux_`), structs prefixed `ds5_`, macros prefixed `DS5_`. Driver name `DS5_DRIVER_NAME = "d4xx"` with variants `-awg`, `-asr`, `-class`, `-dfu`.
- **I2C helpers**: `ds5_read()` / `ds5_write()` — retry wrappers (`DS5_I2C_RETRY_COUNT=5`, `DS5_I2C_RETRY_DELAY_US=5000`). `ds5_read_with_check()` / `ds5_write_with_check()` / `ds5_raw_read_with_check()` / `ds5_raw_write_with_check()` — return on error. `ds5_read_poll()` — single-shot no-retry for polling loops (see Concurrency notes).
- **Logging**: `dev_err()`, `dev_warn()`, `dev_info()`, `dev_dbg()` with `&state->client->dev`. Always include `__func__` in log messages.
- **Conditional compilation**: `CONFIG_VIDEO_D4XX_SERDES` (GMSL vs. non-SerDes path), `CONFIG_TEGRA_CAMERA_PLATFORM` (Tegra integration), `LINUX_VERSION_CODE` checks for kernel API differences.
- **Lazy invalidation over explicit loops**: when state must be invalidated across instances (e.g. after a deserializer reset), increment an atomic generation counter (`atomic_inc()`) and let instances detect the bump lazily in `ds5_configure()`. Avoid O(N) loops over `ds5_inited[]`. Add new invalidation logic to an existing mismatch-detection site rather than introducing a separate check.

### V4L2 subdev architecture

- **SerDes pipe configuration**: the **driver** configures all four SerDes pipes (Depth, RGB, IR, IMU) at stream start via `ds5_configure()`. The D457 firmware does **not** configure any pipes. Do not add probe-time pipe setup or special-case individual pipes in `ds5_probe()`.
- **Device tree assumptions**: all supported device trees include all four sensor instances (Depth, RGB, IR, IMU). Do not add DT-scanning logic to check for the presence of individual sensor types.
- **Unsupported device types are DFU-only, not probe failures**: `ds5_probe()` must not tear down the DFU chardev just because the device reports a type the driver does not operationally support (e.g. legacy/old firmware such as D430 GS reporting type 3, which reports type 5 once updated). The probe device-type wait calls `ds5_wait_device_type(state, &type, /*require_supported=*/false)` — it returns as soon as the type register is **populated** (non-zero), accepting unknown types. Probe then checks `ds5_is_valid_device_type(type)`: supported → full `ds5_v4l_init()`; unsupported → early `return 0` (DFU-only, before `ds5_v4l_init()`). Mirror the recovery early-return: `i2c_set_clientdata(c, state)` so `ds5_remove()` can recover `state`, but leave `dfu_state_flag` at `IDLE` (do **not** set `DS5_DFU_RECOVERY` — the device is in app mode, so opening the chardev must drive `DS5_DFU_OPEN` → `ds5_dfu_switch_to_dfu()`). Keep `ds5_is_valid_device_type()` as the list of *operationally-supported* types only — do not add legacy/DFU-only types to it, and do not add per-type cases to the format/sync `switch(dev_type)` blocks; for such types the goal is FW update only.
- **`ds5_wait_device_type()` contract**: takes `require_supported`. Pass `true` from the reset/recovery path (`ds5_hw_reset_with_recovery()`) — only an operationally-supported type confirms GMSL link recovery for a known device. Pass `false` from `ds5_probe()` — any FW-populated (non-zero) type satisfies the wait so unknown/legacy types reach the DFU-only routing above. Non-zero is treated as "FW populated the type"; this assumes the register transitions `0 → final value` without transient non-zero garbage (by probe time the device has settled via prior FW_VERSION/SerDes/DFU-magic reads).
- **DFU download finalizes once, at `close()`**: the firmware-image chardev download follows standard size-agnostic USB DFU — block transfers carry data, and a single zero-length `DFU_DNLOAD` (`ds5_write(0x4a04, 0)`) is the *sole* end-of-transfer marker that drives `DOWNLOAD_IDLE → MANIFEST → flash-commit → reboot`. `ds5_dfu_device_write()` is **transfer-only**: it streams blocks (with per-block `dfuDNLOAD_IDLE` sync) and stays in `DS5_DFU_IN_PROGRESS` — it must **not** emit `0x4a04=0`/manifest. The finalize lives only in `ds5_dfu_device_release()` (gated on `DS5_DFU_IN_PROGRESS`), because `close()` is the only reliable end-of-image signal — a `write()` length is not (userspace buffering, e.g. `std::ofstream`, splits the image across syscalls in arbitrary, possibly non-`DFU_BLOCK_SIZE`-aligned chunks; finalizing on a partial block both stalls 1024-aligned images and prematurely commits truncated ones). Do not reintroduce finalization into the write path.

### Patches

- **Separate patches per kernel module**: when changes span different kernel modules (e.g. max9295, max9296, max96712, d4xx), each module must get its own patch file. Do not combine changes to different modules in a single patch.
- **Signed-off-by in every patch**: every `.patch` file must include a `Signed-off-by:` trailer. When modifying a patch, add your own using the current `git config user.name` / `user.email`.

## Branching

- `master` — primary/release branch
- `dev` — active development branch; **default target for all PRs**
- CI builds run on pushes to `master` and `dev`, and on all PRs

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
- In `ds5_probe()`, the DFU-magic recovery check (`DS5_DFU_MAGIC_REG` 0x5020 → `DS5_DFU_MAGIC_LSW` 0x0201) must run **before** `ds5_wait_device_type()`. A device sitting in the bootloader after an interrupted FW upgrade never serves `DS5_DEVICE_TYPE` (0x0310) — placing the device-type wait first causes a ~10 s timeout followed by `goto e_chardev` which tears down the `/dev/d4xx-dfu*` chardev, leaving the device unrecoverable over MIPI. Early-return `DS5_DFU_RECOVERY` on magic match; only then proceed to the device-type wait for operational devices. Additionally, the initial `DS5_FW_VERSION` communication check (before the cam-type DT parsing) must also probe `DS5_DFU_MAGIC_REG` on failure before issuing `goto e_regulator`: the DFU bootloader does not respond to app registers (0x030c NAKs with -EREMOTEIO), so an unconditional abort on FW_VERSION failure prevents probe from ever reaching the DFU magic check.

## Workflow rules

- When the user establishes requirements during a conversation, treat them as fixed. Do not modify, reinterpret, or drop requirements on your own initiative. Only change requirements when the user explicitly requests it. If a requirement seems wrong or conflicting, raise it as a question — do not silently adjust.

## Post-patch instruction hygiene

After every confirmed code patch, review `CLAUDE.md` against the final net diff, including any follow-up tuning edits.

- Check for stale architectural claims and remove or correct them immediately.
- Check whether the patch exposed a reusable convention; if it did, write it down as a general rule instead of leaving it implicit in code.
- If the patch changed the locking, usage, or API contract of a helper or utility function (e.g. moved lock acquisition inside/outside, changed required caller context, or altered error handling), immediately update all documentation and instructions to reflect the new contract. Always check for this class of change after any helper edit.
- If no new convention was exposed, state that explicitly in the final report and include a short justification.
- Do not treat the task as complete until that review outcome has been reported.
