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
The `--one-cam`/`--dual-cam` options only apply to JetPack 5.0.2. The target version comes from the `jetpack_version` file (written by `setup_workspace.sh`), not an argv. **Non-interactive:** `apply_patches.sh` prompts via `read` when a sub-repo has local changes and, under `set -e`, aborts (rc=1) on EOF — pipe confirmations with `yes '' | ./apply_patches.sh [reset]` when running headless.

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
- **`hardware/realsense/`** — Device tree source files. Xavier uses `.dtsi` includes (`tegra194-camera-d4xx-*.dtsi`), Orin uses DT overlays (`tegra234-camera-d4xx-overlay*.dts`). Single-camera and dual-camera variants exist. (Separate `.calib.` overlay variants were removed once the driver stopped corrupting calibration-format streams on the standard overlay — calibration formats now stream on the regular DTB; metadata is simply not populated in calibration mode.) **JP7/noble Orin overlays** (e.g. `tegra234-camera-d4xx-overlay-fg12-16ch-cams-0.dtso`) must use the `.dtso` extension — the noble Makefile overlay rule only builds `.dtso` — and must **not** use `JETSON_COMPATIBLE`, since the `tegra234-p3737-0000+p3701-0000.h` platform dt-bindings header is absent from the noble tree; hardcode the `compatible` string instead. Headers like `tegra234-gpio.h` / `pinctrl-tegra.h` (and thus the TSC/`MAX1_CSI_SYNC` ext-sync fragments) are present, so keep the external-sync GPIOs. `apply_patches.sh` links these `.dtso` files into the tree and the noble `0004` patch lists their `.dtbo` under `CONFIG_ARCH_TEGRA_234_SOC`. **DT describes only the static board/link topology and is parsed before the camera is probed — it does not know which camera model (D401/D457/D430/D45x…) is on a link (that is detected at runtime via `DS5_DEVICE_TYPE`).** Only put genuinely static board-/link-level facts in DT (lane count, `vc-id`, deserializer address, external-sync GPIOs); any behavior that depends on the *connected camera model* must be decided in-driver from the detected device type, never via a DT property (RSDEV-12608 dropped a `maxim,oneshot-on-alloc` DT flag for exactly this reason).
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

## V4L2 control conventions

- A control that librealsense reaches through a **USB depth XU selector** must be a **single read/write V4L2 control**, not a split get/set pair. The MIPI backend's `xu_to_cid()` maps one selector to one CID, and both `get_xu()` and `set_xu()` use that same CID — so a read-only "get" CID plus a separate "set" CID cannot be reached by a single selector. Expose one control with flags `V4L2_CTRL_FLAG_VOLATILE | V4L2_CTRL_FLAG_EXECUTE_ON_WRITE` (do **not** set `READ_ONLY`): `VOLATILE` routes each read to `ds5_g_volatile_ctrl`, `EXECUTE_ON_WRITE` routes each write to `ds5_s_ctrl`. `DS5_CAMERA_CID_AE_MODE` (depth AE regular/accelerated, HWMC SETAETYPE 0x87 / GETAETYPE 0x88, XU selector 0x11) is the reference example.
- Split get/set CID pairs (e.g. `ae_roi_get/set`, `ae_setpoint_get/set`) are only appropriate for controls librealsense drives via the HWMC blob passthrough (`RS_HWMONITOR → RS_CAMERA_CID_HWMC`), never via a direct XU selector. Adding a matching `case` in librealsense's `xu_to_cid()` is still required for the selector to resolve.

## Kernel ABI / module compatibility notes

- Never add a member to a public kernel struct referenced by exported symbols (e.g. `struct i2c_adapter` in `include/linux/i2c.h`). genksyms recomputes the CRC of every exported symbol referencing that struct, so prebuilt out-of-tree modules built against the unpatched headers — notably the BSP NVIDIA display stack (`nvidia.ko`/`nvidia-modeset.ko`/`nvidia-drm.ko`) — fail to load with "disagrees about version of symbol". Add only new exported functions; keep private state out of public headers. After any kernel-header patch, diff the rebuilt `vmlinux.symvers`/`Module.symvers` against what the BSP modules require (`modprobe --dump-modversions <module>.ko`).
- The i2c bus-clk-rate feature (kernel patch `0005`) is functional only on 5.x/6.x, where `tegra_i2c_xfer()` reprograms the clock from `adap->bus_clk_rate`. On 7.x the noble i2c-tegra rewrite ignores it, so `0005` is removed for 7.x and the d4xx DFU call sites are version-guarded (`#if ... LINUX_VERSION_CODE < KERNEL_VERSION(6, 8, 0)`). Do not re-add or stub `0005` on 5.x/6.x.
- JP7/Thor: do not rebuild/replace the BSP NVIDIA display stack — the bundled `nvdisplay` is a non-matching pre-release that breaks GPU/display init (black screen / blinking cursor; `/proc/driver/nvidia/version` shows `TempVersion`). `build_all.sh` passes `SKIP_NVIDIA_DISPLAY=1` for `>= 7.0`, and `install_to_kernel.sh` overlays modules on JP7 (no `rm -rf /lib/modules`).
- `kernel/realsense/d4xx.c` is symlinked into **every** JetPack version's build tree, but the SerDes sources/headers are per-generation (`nvidia-oot/max96712.{c,h}` for 6.x/7.x via symlinked patch `0003`/`0006`; independent `kernel/nvidia/` patches + `kernel/nvidia/max96712.h` for 5.x). When d4xx wires a deserializer op that references a **new** exported SerDes symbol, that symbol must be provided by **every** JP's SerDes header/driver, since one d4xx.c builds against all of them (CI builds 5.0.2→7.2). Two ways to keep that consistent: (a) add the symbol to **all** generations' SerDes drivers+headers and wire it unconditionally (what `max96712_arm_link_reset` does — provided by both `nvidia-oot/max96712.h` and `kernel/nvidia/max96712.h`, so no ifdef); or (b) if a generation genuinely won't provide it, either guard the wiring with a feature macro defined only where the symbol exists, **or** — preferred when the op is a no-op there — don't wire it at all and rely on d4xx's `if (ops->op)` NULL check (what `max9296_arm_link_reset` does: absent everywhere, never wired). Do **not** leave a per-generation feature-macro ifdef once the symbol is present in all headers — it becomes dead. New module-to-module exported symbols are ABI-safe (both `.ko` rebuild+deploy together); this is distinct from the kernel-public-struct CRC hazard above.

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
- The deserializer GMSL one-shot link reset (max96712 `CTRL1` + `msleep(100)`, issued in **both** `__max96712_set_pipe` (per-VC / max9295 path) **and** `__max96712_set_multi_vc_pipe` (multi-VC / max96717 path) — gate **both** sites) must fire **only on cold link bring-up**, never on every pipe (re)alloc — all of a camera's streams share one physical link (`vc_id_msb = vc_id >> 2`), so re-arming one stream on a plain toggle re-trains the whole link and truncates the sibling streams' in-flight frames (RSDEV-12608). d4xx owns the **decision**: `ds5_mux_s_stream()` sets `state->link_cold_bringup = !(any ds5_dev->*_streaming)` under `ds5_dev->lock` **before** writing its own streaming flag ("first active stream on this link"), and `ds5_setup_pipeline()` passes it to the deserializer via the additive `dser_interface.arm_link_reset(dev, bool)` op, called under `serdes_lock__` immediately before `set_pipe` (guarded `if (ops->arm_link_reset)`; max9296 wires no op on any JP — its `set_pipe` issues no reset, so the NULL check simply skips it). Gate the decision on the **reset-cleared** `ds5_dev` streaming flags, **not** a deser-side pipe refcount: `ds5_hw_reset_with_recovery()` clears the flags but deliberately keeps the SERDES pipes, so a refcount would stay ≥2 and skip the re-lock the recovery path needs (both streams then come up on an un-retrained link = 0 frames). max96712 stores the bool in `pending_link_reset` and re-arms it `true` after consuming it (fail-safe default = reset). The coarse `reset_oneshot(0x0F)` (all-links) used on the Y12I/`is_calib` path is a **separate**, ungated reset and is out of scope of this gate. JP5's independent `kernel/nvidia` max96712 driver carries the **same two per-alloc reset sites** and the same bug (patches `5.1.2/0007` + `5.0.2/0017`); keep it in sync with the `nvidia-oot` (6.x/7.x) version.

## SerDes pipe allocation (MAX96712)

Two allocation schemes coexist on one MAX96712 (up to 4 serializers):
- **MAX9295 serializer** (default): dynamic — `ds5_configure()` allocates a fresh deserializer pipe per stream via `get_available_pipe_id()`, configures it per-stream in `set_pipe()`, and frees it in `release_pipe()` on stop.
- **MAX96717 serializer**: it funnels all of a camera's streams through one serializer pipe (`MAX96717_PIPE_ID = 2`) as distinct VCs, so the deserializer must dedicate **one sticky "multi-VC" pipe** to that camera's link and reuse it for the driver's lifetime. `ds5_configure()` detects `ser_ops == &max96717_interface` and allocates via the optional `dser_interface::get_multi_vc_pipe_id()` hook (NULL for MAX9296).

MAX96712 keys the sticky pipe by link (`link = vc_id / MAX9295_MAX_STREAMS`): `link_multi_vc_pipe[]` (-1 = none) + `multi_vc_configured[]`. Contract, when adding/using these helpers:
- `max96712_release_pipe()` **no-ops** for a multi-VC pipe (never frees or disables it) — a stream stop must not tear it down. `max96712_pipe_is_multi_vc()` detects them and **requires `priv->lock` held**.
- `max96712_set_pipe()` configures a multi-VC pipe **exactly once** (`__max96712_set_multi_vc_pipe()`, maps all 4 local VCs), guarded by `multi_vc_configured`; later starts no-op. `max96712_init_settings()` clears `multi_vc_configured` (it zeroes `PIPE_EN`, voiding the mapping) so a dser re-init forces re-apply, while pipe ownership persists.
- The multi-VC register table is validated on **link 0** only; link>0 extended-VC-msb (`MIPI_TX_EXT0..3 = link_id << 2`) is generalized to match the dynamic path but unproven — validate on hardware.
- `max96712.c` has **no repo copy** — it is carried entirely by `nvidia-oot/6.0/0003-…patch` (JP6.1/6.2 symlink to 6.0; JP7 = `7.x/0006-…patch`). Edit the `sources_<v>/nvidia-oot/.../max96712.c` working copy, then regenerate 0003 via `git diff HEAD^ -- drivers/media/i2c/max96712.c` (0003 touches only this file). JP7 patches are **not** updated by JP6 work — port separately after validation.

## Post-patch instruction hygiene

After every confirmed code patch, review both `.github/copilot-instructions.md` and `CLAUDE.md` against the final net diff, including any follow-up tuning edits.

- Check for stale architectural claims and remove or correct them immediately.
- Check whether the patch exposed a reusable convention; if it did, write it down as a general rule instead of leaving it implicit in code.
- If the patch changed the locking, usage, or API contract of a helper or utility function (e.g. moved lock acquisition inside/outside, changed required caller context, or altered error handling), immediately update all documentation and instructions to reflect the new contract. Always check for this class of change after any helper edit.
- If no new convention was exposed, state that explicitly in the final report and include a short justification.
- Do not treat the task as complete until that review outcome has been reported.

## Comment style

Keep comments concise and high-signal — both in-code and external (Jira/PR/status updates). Lead with the finding or the "why", in the fewest lines that stay correct; cut restated context and prose. Jira/PR comments especially: a few skimmable lines or bullets, not walls of text.
