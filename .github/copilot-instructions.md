# Copilot Instructions

## Project Overview

Linux kernel driver and userspace utilities for Intel RealSense D4XX series 3D depth cameras operating over GMSL (Gigabit Multimedia Serial Link) MIPI CSI-2 interface on NVIDIA Jetson platforms. Licensed under GPL-2.0.

- **Supported platforms:** Jetson AGX Xavier (JetPack 4.6.1, 5.0.2, 5.1.2) and AGX Orin (JetPack 6.0, 6.1, 6.2, 6.2.1)
- **Supported cameras:** D457 (primary), D401, D40x, D41x, D43x, D45x, D46x series

## Coding Conventions

### Kernel Driver (C — `kernel/realsense/d4xx.c`)

- **Follow Linux kernel coding style**: tabs for indentation (8-space width), `/* */` block comments, max ~80–100 char lines.
- **Keep changes minimal**: do not inflate code. Prefer single-line expressions over multi-line blocks, reuse existing helpers/paths instead of adding new ones, and avoid verbose comments that restate what the code already says. Every added line must earn its place — if a change can be expressed more concisely without losing clarity, use the shorter form.
- **Before adding new code, check callers and existing paths**: when introducing a check, loop, or helper, first verify whether the same logic already exists elsewhere in the call chain. If a function has only one caller, consider placing the logic in the caller instead of duplicating it. Never add a second copy of logic that can be combined with an existing one — consolidate first, don't layer.
- **After removing code, clean up stale references**: when deleting a function, code block, or feature, immediately search for defines, variables, struct fields, forward declarations, and comments that were only used by the removed code. Remove all of them in the same patch. Do not leave dead code behind.
- **Function naming**: prefix all functions with `ds5_`. Mux-related functions use `ds5_mux_`. Examples: `ds5_read()`, `ds5_write()`, `ds5_probe()`, `ds5_mux_s_stream()`.
- **Struct naming**: prefix with `ds5_`. Examples: `struct ds5`, `struct ds5_sensor`, `struct ds5_ctrls`, `struct ds5_format`.
- **Macro naming**: prefix with `DS5_`. Register addresses: `DS5_FW_VERSION`, `DS5_START_STOP_STREAM`, `DS5_DEPTH_STREAM_DT`.
- **Driver name**: `DS5_DRIVER_NAME` = `"d4xx"`, with variants `-awg`, `-asr`, `-class`, `-dfu`.
- **I2C access**: use `ds5_read()` / `ds5_write()` wrappers around `regmap_raw_read()` / `regmap_raw_write()` with built-in retry logic (`DS5_I2C_RETRY_COUNT=5`, `DS5_I2C_RETRY_DELAY_US=5000`).
- **Polling vs. normal I2C semantics**: for transient-failure-expected polling loops (HWMC status checks, reset readiness polls, SERDES recovery probes, DFU timeout checks), use `ds5_read_poll()` instead of `ds5_read()`. `ds5_read_poll()` performs a single direct `regmap_raw_read()` call without retry or logging, preventing false warnings and excessive log spam on expected transients. Reserve `ds5_read()` for normal I2C operations where retry logic and verbose failure logging are desired.
- **Helper macros**: `ds5_read_with_check()`, `ds5_write_with_check()`, `ds5_raw_read_with_check()`, `ds5_raw_write_with_check()` — these return on error.
- **Logging**: use `dev_err()`, `dev_warn()`, `dev_info()`, `dev_dbg()` with `&state->client->dev` as the device. Always include `__func__` in log messages.
- **Locking**: `mutex_lock()` / `mutex_unlock()` for state synchronization.
- **SERDES topology locking**: protect global topology scans/updates of `ds5_inited[]` and `dser_inited[]` with `serdes_lock__`. Use `struct ds5_dev::lock` for per-camera mutable fields (`ds5_primary`, `*_streaming`) and `struct dser_control::lock` for per-deserializer slot fields (`dser_dev`). For sibling checks, snapshot under lock and do I2C probing after unlocking.
- **Module registration**: `module_i2c_driver()` pattern.
- **Conditional compilation**:
  - `CONFIG_VIDEO_D4XX_SERDES` — SerDes (GMSL) support vs non-SerDes path.
  - `CONFIG_TEGRA_CAMERA_PLATFORM` — Tegra-specific camera platform integration.
  - `LINUX_VERSION_CODE` checks for API differences between kernel versions (4.9, 5.10, 5.15+).
- **Prefer lazy invalidation over explicit loops**: when state must be invalidated across multiple instances (e.g. after a deserializer reset), increment an atomic generation counter (`atomic_inc()`) and let each instance detect the bump lazily (e.g. in `ds5_configure()`). Avoid O(N) loops that iterate `ds5_inited[]` to poke siblings. Combine lazy checks when possible — if an existing function already detects a generation mismatch, add new invalidation logic there rather than adding a separate check elsewhere.
- **HW reset readiness source of truth**: do not use register `0x5020` (`DS5_DFU_MAGIC_REG`) as a non-DFU reset-ready signal. Before HW reset, scratch `DS5_*_CONTROL_STATUS` with a non-zero sentinel (for example `0x00AD`), then after reset poll for the default `0x0000` restore to declare readiness. Keep `0x5020` only for DFU-magic detection (`0x04030201`).
- **Post-reset operational readiness source of truth**: after reset completion, treat `DS5_DEVICE_TYPE` becoming valid as the firmware-ready gate for format/config-dependent code paths. `DS5_FW_VERSION` may become readable earlier and is acceptable for basic liveness checks, but do not use it as the only post-reset readiness signal.
- **Reset-time cache invalidation for readiness gates**: whenever HW reset invalidates firmware-populated registers, clear any cached equivalents in the same reset path before readiness polling (for example reset `cached_device_type` before waiting on `DS5_DEVICE_TYPE`). Never allow a stale cache value to satisfy post-reset readiness checks.
- **`ds5_mux_s_stream()` pre-toggle no-op rule**: for start (`on=1`), if reset-generation invalidation was detected and FW still reports streaming, do not return no-op; force a stop, clear cached stream state, and continue through normal start/configure flow.

### V4L2 Subdev Architecture

Each camera registers four sensor subdevices: Depth, RGB, IR (Y8/Y8I/Y12I), and IMU. Each has separate V4L2 subdev ops structs:
- `ds5_depth_subdev_ops`, `ds5_ir_subdev_ops`, `ds5_rgb_subdev_ops`, `ds5_imu_subdev_ops`
- Mux ops: `ds5_mux_subdev_ops`, `ds5_mux_pad_ops`, `ds5_mux_core_ops`, `ds5_mux_video_ops`

A deserializer abstraction layer (`struct dser_interface`) provides function pointer tables for MAX9296 vs MAX96712 variants.

- **SerDes pipe configuration**: the **driver** configures all four SerDes pipes (Depth, RGB, IR, IMU) at stream start via `ds5_configure()`. The D457 firmware does **not** configure any pipes. Do not add probe-time pipe setup or special-case individual pipes (e.g. IMU) in `ds5_probe()`.
- **Device tree assumptions**: all supported device trees include all four sensor instances (Depth, RGB, IR, IMU). Do not add DT-scanning logic to check for the presence of individual sensor types.

### Video Device Layout (per camera)

| Device  | Stream           | Format                 |
|---------|-----------------|------------------------|
| video0  | Depth           | Z16                    |
| video1  | Depth metadata  | D4XX custom format     |
| video2  | Color RGB       | RGB888/YUV422          |
| video3  | Color metadata  | D4XX custom format     |
| video4  | IR              | GREY, Y8I, Y12I        |
| video5  | IMU             | Custom                 |

### Tests (Python — `test/v4l2_test/`)

- **Framework**: pytest with marker `@pytest.mark.d457` on all test classes.
- **Class-based tests**: `class TestCameraDiscovery`, `class TestLaserControl`, `class TestFirmwareVersion`, etc.
- **Test naming**: `test_at_least_one_camera`, `test_driver_name`, `test_six_devices_exist`, `test_fw_version_format`.
- **Fixtures**: session-scoped `all_cameras`, `camera`; per-test `depth_device`, `fw_version`.
- **Constants**: `test/v4l2_test/d4xx/constants.py` mirrors driver CIDs from `d4xx.c`.
- **Categories**: Discovery, Streaming, Controls, Metadata, Error Handling.
- **Streaming validation**: FPS tolerance 5%, frame count 60, min 90% frame arrival, consecutive drop limit 2.
- **Test timeout**: 200 seconds (configured in `test/pytest.ini`).

### Shell Scripts

- Use `#!/bin/bash` with `set -e` (fail-on-error).
- Source `scripts/setup-common` for JetPack version normalization.
- Use `DEVDIR=$(cd \`dirname $0\` && pwd)` to resolve repo root.
- Branch on JetPack major version (4.x / 5.x / 6.x) for platform-specific logic.

### Device Tree

- **Xavier (Tegra194)**: `.dtsi` includes, pattern `tegra194-camera-d4xx-{variant}.dtsi`.
- **Orin (Tegra234)**: DT overlays (`.dts`), pattern `tegra234-camera-d4xx-overlay-{variant}.dts`, uses `/dts-v1/; /plugin/;`.
- Variants: `single`, `dual`, `single.calib`, `dual.calib`, `fg12-16ch`, `max96712-EVB`.

## Build System

### Prerequisites

```bash
sudo apt install -y build-essential bc wget flex bison curl libssl-dev xxd
```

### Full Build Flow

```bash
./setup_workspace.sh <version>    # Clone NVIDIA sources, install toolchain
./apply_patches.sh <version>      # Apply D4XX patches to kernel + NVIDIA OOT modules
./build_all.sh <version>          # Build kernel, DTBs, and driver modules
```

Build outputs go to `images/<version>/`.

### Version Mapping

| JetPack | L4T Revision | Kernel Dir                    | Normalized |
|---------|-------------|-------------------------------|------------|
| 4.6.1   | 32.7.1      | `kernel/kernel-4.9`           | 4.6.1      |
| 5.0.2   | 35.1        | `kernel/kernel-5.10`          | 5.x        |
| 5.1.2   | 35.4.1      | `kernel/kernel-5.10`          | 5.x        |
| 6.0     | 36.3        | `kernel/kernel-jammy-src`     | 6.x        |
| 6.1     | 36.4        | `kernel/kernel-jammy-src`     | 6.x        |
| 6.2     | 36.4.3      | `kernel/kernel-jammy-src`     | 6.x        |
| 6.2.1   | 36.4.4      | `kernel/kernel-jammy-src`     | 6.x        |

### Cross-Compilation Toolchains

- JP 4.6.1: Linaro GCC 7.3
- JP 5.x: Bootlin GCC 9.3
- JP 6.x: Bootlin GCC 11.3 (`aarch64-buildroot-linux-gnu`)

### Patch Application

`apply_patches.sh` copies `kernel/realsense/d4xx.c`, device tree files, and `nvidia-oot/max96712.h` into the NVIDIA source tree, applies git patches, and commits with `"RS patched"`. The `reset` action uses `git reset --hard` to a stored base commit.

Camera variant flags: `--one-cam`, `--dual-cam`, `--max96712-EVB`, `--fg12-16ch`, `--fg12-16ch-dual` (only some apply to specific JetPack versions).

## Key Directories

| Path | Description |
|------|-------------|
| `kernel/realsense/d4xx.c` | Main V4L2 I2C subdevice driver (~6900 lines) |
| `kernel/kernel-4.9/` | Kernel patches for JetPack 4.6.1 |
| `kernel/kernel-5.10/` | Kernel patches for JetPack 5.x |
| `kernel/kernel-jammy-src/` | Kernel patches for JetPack 6.x |
| `kernel/nvidia/` | NVIDIA driver patches (MAX9295/9296 SerDes, VI capture) |
| `nvidia-oot/` | Out-of-tree NVIDIA module patches for JetPack 6.x |
| `hardware/realsense/` | Device tree source files |
| `hardware/nvidia/` | Platform-level DT patches |
| `scripts/` | Build orchestration and SerDes configuration scripts |
| `test/v4l2_test/` | V4L2 pytest test suite |
| `utilities/streamApp/` | C++ V4L2 streaming application |
| `utilities/JsonToBin/` | Python JSON-to-binary preset converter |

## CI

CI workflows (`.github/workflows/build-jp*.yml`) build for each JetPack version on pushes to `master`/`dev` and all PRs. The V4L2 test workflow runs on a self-hosted Jetson runner when `kernel/realsense/**` or `test/v4l2_test/**` paths change.

CI requires `git config user.email/name` to be set before `apply_patches.sh`.

## Branching

- `master` — primary/release branch
- `dev` — active development branch
-
## Refactoring notes (recent commits)

Recent multi-phase refactoring of `kernel/realsense/d4xx.c`:

**Phase 1–3 (Reset Readiness + Per-Instance State Init + Polling Decoupling):**
- **Reset readiness migration**: replaced `DS5_DFU_MAGIC_REG` (0x5020) non-DFU readiness detection with CONTROL_STATUS scratch-and-poll handshake (scratch 0x00AD before reset, poll for 0x0000 restore after). Eliminated ~800 lines of post-ready compensating logic (device-type waits, HWMC probe loops, natural-recovery gates) that hinged on 0x5020 assumption.
- **Per-instance control base init**: added `control_base` and `control_status_reg` fields to `struct ds5`, initialized once at probe based on camera type (Depth/Y8/IMU → DEPTH values 0x4100/0x401E; RGB → RGB values 0x4200/0x402E). Eliminates dynamic ternary recalculation across `ds5_s_ctrl()` and `ds5_g_volatile_ctrl()`, reducing code duplication and improving readability.
- **Polling semantics separation**: introduced `ds5_read_poll()` helper for expected-transient-failure loops (HWMC status, reset readiness, SERDES recovery, DFU timeout, probe communication retries). Migrated 13 polling contexts to use single-shot direct regmap reads, eliminating false warnings and excessive log spam without changing I/O behavior. Preserves `ds5_read()` retry/logging semantics for normal I2C operations.
- **HWMC helper consolidation**: refactored calibration set/get, GVD, LOG command send, and DFU switch to use `ds5_hwmc_send()` + `ds5_hwmc_wait()` consistently instead of ad-hoc raw register writes.

Earlier phases (below) documented for reference:

**Earlier Phases (Encapsulation + Lazy Invalidation):**
- **Encapsulated shared state**: introduced `struct ds5_dev` (per-camera) and
  `struct dser_control` (per-deserializer) to centralize reset generation,
  cached device type and last-reset timestamps instead of scattered globals.
- **Separated reset generation**: split camera-level and deserializer-level
  reset counters so camera resets and deserializer resets are independent;
  callers check both generation counters and invalidate cached state on mismatch.
- **Atomic, lazy invalidation**: replaced many explicit peer-invalidation loops
  with `atomic_inc()` of the appropriate reset-gen; peers detect the bump in
  `ds5_configure()` and invalidate themselves lazily, reducing O(N) churn.
- **Single registration/link path**: factored duplicate SERDES/board registration
  logic into `ds5_setup_and_link()` to avoid repeated code paths and make
  primary/peer linking explicit.
- **Keep streaming visibility consistent**: driver now maintains per-camera
  streaming flags (kept in sync from `ds5_mux_s_stream()`) so recovery and
  sibling checks can reliably detect active streams.
- **Non-SERDES compatibility**: added small fallbacks so callers can use the
  same reset-gen accessors in non-SERDES builds (where applicable).

Overall advantages: clearer ownership of shared state, fewer global arrays and linear
searches, more deterministic recovery behavior, reduced duplicate code, improved logging
clarity, and architectural separation of restart readiness detection from compensation logic.

## Workflow Rules

### Post-patch configuration review (mandatory)

After every confirmed code patch, review and update this file and `CLAUDE.md`:

1. **Stale facts**: correct or remove architectural claims, assumptions, or descriptions that the patch invalidated.
2. **New conventions**: if the patch exposed a coding practice gap (e.g. a cleanup step that was missed), add a general coding convention so it is enforced going forward — do not just fix the specific instance.

Do not leave stale or incorrect claims in configuration files after changing the code they describe.

This review is not complete until the final response explicitly reports the outcome of both checks.

- If a new convention was exposed, document it in the same patch series.
- If no new convention was exposed, say that explicitly and give a short reason why the change was purely mechanical or already covered by existing rules.
- If the code patch was refined by follow-up edits, re-run the convention review against the final net change, not just the first patch.
- Treat a missed convention update as an incomplete task, even if the code change itself is correct.
