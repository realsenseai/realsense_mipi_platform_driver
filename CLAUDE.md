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

`scripts/deploy_kernel.sh` is the deploy path for **all** JetPack versions (there is no `deploy_kernel_6.2.sh`). It packages the modules, copies them to the target, updates `/boot/<REMOTE_BOOT_FOLDER>` and reboots:

```bash
./scripts/deploy_kernel.sh <JP_VERSION> [TARGET] [USERNAME] [REMOTE_PATH] [REMOTE_BOOT_FOLDER]
./scripts/deploy_kernel.sh 6.2 fw-orin-3 nvidia drgb_poc dev   # → /home/nvidia/drgb_poc, /boot/dev
```

Deploy the **whole** module set this script produces — never hand-copy individual `.ko` files. `tegra-camera.ko` exports `camera_common_*` to `d4xx.ko` **and** `tegra_csi_*`/`csi5_fops` to `nvhost-nvcsi*`, so replacing it alone strands the other consumers ("disagrees about version of symbol") and NVCSI fails to load, leaving no `/dev/video*` at all. Two further traps on a live rig: `rmmod` of the SerDes/camera modules can wedge in **D-state** (unkillable, needs a power cycle), and a `modprobe` issued *after* boot binds the I2C devices but creates no `/dev/video*` — the VI nodes are only built during boot-time setup, so reboot instead of reloading by hand. `d4xx` has no `modules-load.d`/initramfs entry; it autoloads from the DT modalias, and that enumeration is intermittent — a boot with 0 nodes is usually the known GMSL cold-boot flakiness, so reboot (or power-cycle) rather than assuming the build is bad.

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

Python ctypes mirrors of V4L2 UAPI structs (`test/v4l2_test/v4l2/structs.py`) must reproduce the kernel's exact layout: `v4l2_ext_control` is `__attribute__((packed))` in UAPI (20 bytes, ptr union at offset 12) and needs `_pack_ = 1`, while `v4l2_ext_controls` is naturally aligned (32 bytes) and must not be packed. A misaligned mirror makes compound (`has-payload`) `G/S_EXT_CTRLS` fail as `EFAULT` — or, when wrappers swallow `OSError`, as silent all-zero payloads that masquerade as a driver bug (RSDSO-20711). When python-driven compound controls return zeros but `v4l2-ctl` works on the same node, suspect caller marshalling before the driver.

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
- **`hardware/realsense/`** — Device tree source files. Xavier uses `.dtsi` includes (`tegra194-camera-d4xx-*.dtsi`), Orin uses DT overlays (`tegra234-camera-d4xx-overlay*.dts`). Single-camera and dual-camera variants exist. (Separate `.calib.` overlay variants were removed once the driver stopped corrupting calibration-format streams on the standard overlay — calibration formats now stream on the regular DTB; metadata is simply not populated in calibration mode.) **JP7/noble Orin overlays** (e.g. `tegra234-camera-d4xx-overlay-fg12-16ch.dtso`) must use the `.dtso` extension — the noble Makefile overlay rule only builds `.dtso` — and must **not** use `JETSON_COMPATIBLE`, since the `tegra234-p3737-0000+p3701-0000.h` platform dt-bindings header is absent from the noble tree; hardcode the `compatible` string instead. Headers like `tegra234-gpio.h` / `pinctrl-tegra.h` (and thus the TSC/`MAX1_CSI_SYNC` ext-sync fragments) are present, so keep the external-sync GPIOs. `apply_patches.sh` links these `.dtso` files into the tree and the noble `0004` patch lists their `.dtbo` under `CONFIG_ARCH_TEGRA_234_SOC`. **DT describes only the static board/link topology and is parsed before the camera is probed — it does not know which camera model (D401/D457/D430/D45x…) is on a link (that is detected at runtime via `DS5_DEVICE_TYPE`).** Only put genuinely static board-/link-level facts in DT (lane count, `vc-id`, deserializer address, external-sync GPIOs); any behavior that depends on the *connected camera model* must be decided in-driver from the detected device type, never via a DT property (RSDEV-12608 dropped a `maxim,oneshot-on-alloc` DT flag for exactly this reason).
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
- **A control base is a routing decision, not just an address, and AE gates the manual pair.** The FW maps each `REG_BASE_CAMn_CONTROLS` offset to a UVC unit (`I2cSlaveHostIf.c` `s_hostCtrlTable`): the depth base reaches `vfID_UnitExtDepth`/`vfID_UnitProcDepth` (the imager), the RGB base reaches the ISP units. Under D401 CSI-PT the ISP is off (`IspDataSelect = 0`), so RGB-base writes are inert and exposure/gain must target the depth base — **and so must `V4L2_CID_EXPOSURE_AUTO`**, because `uvcControlDepthAECheck()` rejects manual exposure/gain with `CRC_WRONG_STATE_ERR` while depth AE is on. Redirecting only the manual pair leaves it permanently refused. Each base also has its own unit convention: depth AE encodes off/on as `0`/`1` while RGB follows the UVC menu (`8` = aperture priority), and depth exposure counts 1 us against the RGB register's 100 us — so a redirect must convert encoding *and* units, not just swap the base (RSDSO-21895).
- Redirecting a control per-mode belongs in the `ds5_s_ctrl()`/`ds5_g_volatile_ctrl()` handlers gated on the active format (e.g. `ds5_rgb_shares_imager()` keys on `data_type == DS5_FW_CSI_PT`), never in `ds5_probe()`'s `state->control_base`: that field backs *every* control on the node, and white balance/saturation/sharpness legitimately belong to the ISP whenever CSI-PT is not active.

## MIPI lane configuration (hybrid 2-lane camera / 4-lane deserializer)

Each MIPI segment's lane count comes from a **different** DT property, so the camera→serializer and deserializer→Jetson widths are set independently:

| Segment | DT property | Consumer |
|---|---|---|
| DS5 camera TX lanes | ds5 node's own endpoint `bus-width` | `camera_common_parse_ports()` → `ds5_hw_init()` writes `DS5_MIPI_LANE_NUMS` |
| Serializer RX from DS5 | `gmsl-link/csi-mode` (`"1x4"` → 4-lane RX cfg) | `max9295_setup_streaming()` |
| Deserializer TX → Jetson | `lane_cnt` on the max96712 node (default 2) | `max96712_init_settings()`: `MIPI_TX10` lane count + `BACKTOP25` rate (4 lanes → 1300 Mbps, 2 lanes → 2500 Mbps) |
| NVCSI brick/CIL lanes | nvcsi channel endpoint `bus-width` | `csi5_stream_set_config()` `cil_config.num_lanes` |
| VI channel lanes | `tegra-capture-vi` endpoint `bus-width` | `tegra_vi_get_port_info()` |
| VI clock/bandwidth calc | `modeN/num_lanes` | `tegra_channel_get_num_lanes()` |

**All max96712 D4xx overlays use the hybrid config** (HW-validated on fg12-16ch, `…-cams-0-1-d5xx-d4xx.dts`): camera→serializer stays **2-lane**, deserializer→Jetson runs **4-lane**. Concretely: ds5 endpoint `bus-width = <2>` and `gmsl-link/num-lanes = <2>` stay as-is; `lane_cnt = <4>` on the max96712; `bus-width = <4>` on every VI and nvcsi endpoint; `num_lanes = "4"` in every `modeN`. No d4xx.c change is involved. Any **new** max96712 D4xx overlay must be authored this way. Pure-D5xx overlays (`…-d5xx.dts`, max96717 serializer) are 4-lane end to end; max9296 and max96724 overlays are out of scope (no `lane_cnt` property).

- **`serdes_pix_clk_hz` is required in every `modeN`** whenever the deser link rate differs from what `pix_clk_hz` implies. `sensor_common.c` computes per-lane rate = `(serdes_pix_clk_hz ?: pix_clk_hz) * bit_depth / num_lanes` → mode `mipi_clock` → `cil_config.mipi_clock_rate`. Omitting it at 4-lane/1300 Mbps under-clocks NVCSI ~4x and yields per-frame `DATA_LANE_ERR*_RXFIFO_FULL`, `PD_CRC_ERR`, `CHANSEL_FAULT`, and VI request timeouts. Value for 4 × 1300 Mbps at 16 bpp: `325000000`. Keep this in lockstep with `MAX96712_BACKTOP25_*` in `nvidia-oot/6.0/0003-…patch` — a `BACKTOP25` rate bump must be mirrored into **every** max96712 4-lane overlay (`.dts`/`.dtso`, tegra234 **and** tegra264) in the same change, or NVCSI runs at the wrong CIL clock.
- `gmsl-link/num-lanes` (`g_ctx.num_csi_lanes`) is consumed **only** by max9296 — it is dead on the max96712 path and kept documentary.
- The 4-lane deser output occupies a whole NVCSI brick (`1x4`, port A/C/E/G). Multi-deserializer overlays must therefore keep nvcsi `port-index` on even brick boundaries (0/2/4/6) with matching `tegra_sinterface` (`serial_a/c/e/g`). Do not change `port-index` as part of a lane-width edit.
- 4-lane also requires the carrier to actually route 4 CSI lanes from the deserializer to the Jetson connector; the DT change alone cannot create them.

## Kernel ABI / module compatibility notes

- Never add a member to a public kernel struct referenced by exported symbols (e.g. `struct i2c_adapter` in `include/linux/i2c.h`). genksyms recomputes the CRC of every exported symbol referencing that struct, so prebuilt out-of-tree modules built against the unpatched headers — notably the BSP NVIDIA display stack (`nvidia.ko`/`nvidia-modeset.ko`/`nvidia-drm.ko`) — fail to load with "disagrees about version of symbol". Add only new exported functions; keep private state out of public headers. After any kernel-header patch, diff the rebuilt `vmlinux.symvers`/`Module.symvers` against what the BSP modules require (`modprobe --dump-modversions <module>.ko`).
- The i2c bus-clk-rate feature (kernel patch `0005`) is functional only on 5.x/6.x, where `tegra_i2c_xfer()` reprograms the clock from `adap->bus_clk_rate`. On 7.x the noble i2c-tegra rewrite ignores it, so `0005` is removed for 7.x and the d4xx DFU call sites are version-guarded (`#if ... LINUX_VERSION_CODE < KERNEL_VERSION(6, 8, 0)`). Do not re-add or stub `0005` on 5.x/6.x.
- JP7/Thor: do not rebuild/replace the BSP NVIDIA display stack — the bundled `nvdisplay` is a non-matching pre-release that breaks GPU/display init (black screen / blinking cursor; `/proc/driver/nvidia/version` shows `TempVersion`). `build_all.sh` passes `SKIP_NVIDIA_DISPLAY=1` for `>= 7.0`, and `install_to_kernel.sh` overlays modules on JP7 (no `rm -rf /lib/modules`).
- `kernel/realsense/d4xx.c` is symlinked into **every** JetPack version's build tree, but the SerDes sources/headers are per-generation (`nvidia-oot/max96712.{c,h}` for 6.x/7.x via symlinked patch `0003`/`0006`; independent `kernel/nvidia/` patches + `kernel/nvidia/max96712.h` for 5.x). When d4xx wires a deserializer op that references a **new** exported SerDes symbol, that symbol must be provided by **every** JP's SerDes header/driver, since one d4xx.c builds against all of them (CI builds 5.0.2→7.2). Two ways to keep that consistent: (a) add the symbol to **all** generations' SerDes drivers+headers and wire it unconditionally (what `max96712_reset_oneshot_link` does — provided by both `nvidia-oot/max96712.h` and `kernel/nvidia/max96712.h`, so no ifdef); or (b) if a generation genuinely won't provide it, either guard the wiring with a feature macro defined only where the symbol exists, **or** — preferred when the op is a no-op there — don't wire it at all and rely on d4xx's `if (ops->op)` NULL check (what max9296 does for `reset_oneshot_link`: absent from `max9296_interface`, never wired). Do **not** leave a per-generation feature-macro ifdef once the symbol is present in all headers — it becomes dead. New module-to-module exported symbols are ABI-safe (both `.ko` rebuild+deploy together); this is distinct from the kernel-public-struct CRC hazard above.
- **max96717 and max96724 are single-source, unlike max96712.** `nvidia-oot/max96717.{c,h}` and `nvidia-oot/max96724.{c,h}` are the *only* copies that build: `apply_patches.sh` symlinks them into the JP6/JP7 `nvidia-oot/` tree **and** into the JP5 `kernel/nvidia/drivers/media/i2c/` tree ("MAX96717/MAX96724 use the same implementation on JP5 and JP6"). There is no per-generation patch carrier and no JP5 fork to port to — edit the `nvidia-oot/` file once and every JetPack gets it.

## Concurrency notes

- A stored kthread pointer whose worker may return before teardown must own an
  extra task reference. Acquire it before waking the new task, release it after
  `kthread_stop()`, and clear the stored pointer on creation failure and after stop.
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
- GMSL one-shot reset semantics (per Maxim/ADI): the deserializer one-shot reset does **not** reset or re-train anything — its only effect is flushing the deserializer pixel line buffer. Separately, a camera HW reset does **not** reset the serializer (the ser keeps its config and link lock). So the one-shot's only legitimate job is discarding a stale partial frame left in the line buffer (e.g. camera reset mid-frame); fired while sibling streams are live it truncates their in-flight frames (PIXEL_INCOMPLETE). Do not describe it as "link re-training" in code or docs.
- The deserializer GMSL one-shot link reset (max96712 `CTRL1` + `msleep(100)`) flushes **one** GMSL link's pixel line buffer (`link = vc_id / MAX9295_MAX_STREAMS`). It fires from exactly **two** sanctioned, d4xx-side call sites — never from `set_pipe`: (a) `ds5_hw_reset_with_recovery()`, post-HW-reset recovery (RSDEV-12608); (b) `ds5_mux_s_stream()`, on GMSL link cold bring-up, via `ds5_flush_link_on_cold_bringup()` — called after `ds5_configure()` succeeds and before the `DS5_START_STOP_STREAM` write (RSDSO-21786). Both gate on the same invariant: fire only when all four of the camera's `ds5_dev` streaming flags (`depth/rgb/ir/imu_streaming`) are clear, i.e. the link is idle. `__max96712_set_pipe` / `__max96712_set_multi_vc_pipe` must never flush, and a plain pipe (re)alloc must never trigger one: all of a camera's streams share one physical link, so a flush against a live link truncates a sibling's in-flight frames (PIXEL_INCOMPLETE) — RSDEV-12608's original bug. The recovery-path flush is needed after a **camera HW reset**: the camera resets mid-frame but the serializer is not reset (keeps config + link lock), so a stale partial frame is left in the deser line buffer with nothing else to clear it. `ds5_hw_reset_with_recovery()` clears it by calling the additive per-link op `dser_interface.reset_oneshot_link(dev, vc_id)` (guarded `if (ops->reset_oneshot_link)`) after the device is ready; max96712 flushes `1 << (vc_id/MAX9295_MAX_STREAMS)`. The op is a **single unlocked `CTRL1` write** — `regmap` serializes the I2C and it does no shared-state RMW, so it needs no `priv->lock` (matching the sibling `max96712_reset_oneshot`); do **not** add a lock around it (holding `priv->lock` across its `msleep(100)` would needlessly stall a concurrent sibling `set_pipe`). max9296 wires **no** op (NULL) — its `set_pipe` re-establishes the pipe without leaving a stale line buffer, so it needs no flush (asymmetric design: max96712 flush, max9296 none, both HW-confirmed).
  **Why the flush must exist and must live in the recovery path (HW-proven, fw-orin-nano-1, D457+D401 on one max96712):** a NOFLUSH build (no flush anywhere but `setup_link`'s probe-time one-shot) **kernel-panicked** during recovery — the post-reset restart came up on an un-flushed link, the stale frame made the depth stream never complete, VI `uncorr_err: request timed out after 2500 ms` cascaded unbounded until tegra_camera err_rec hit `vi_capture_setup: memoryinfo ringbuffer alloc failed` → NULL deref in `tegra_channel_kthread_capture_enqueue`/`__memset`. So it can't move to probe-time `init_settings`/`setup_link` only (camera HW reset is post-probe and re-runs neither). The adopted per-link recovery flush validated 4/4 recoveries D457-DUT+D401-load **and** D401-DUT, 0 panic, dmesg `RSDEV-12608: link N line-buffer flush (post-HW-reset)` firing once per reset on the correct link. Transient VI 2500ms still occur ([[RSDEV-13047]] residual) but err_rec self-recovers because the flush lets the stream produce valid frames.
  **History:** the first shipped form gated the flush inside `set_pipe` via an `arm_link_reset(dev,bool)` op + `pending_link_reset` + a d4xx `link_cold_bringup` decision, consumed inside `__max96712_set_pipe` on every pipe (re)alloc regardless of sibling state. That machinery was **removed** (RSDEV-12608). Do not reintroduce a serdes-side arm op, and do not put a flush back inside the max96712 `set_pipe` bodies. The sanctioned mechanism for cold bring-up is d4xx-side and idle-link-gated instead: `ds5_dev.link_flush_pending` + `ds5_flush_link_on_cold_bringup()` (RSDSO-21786) — armed only while all four streaming flags are clear, consumed once, outside `set_pipe`.
  **RSDSO-21786 evidence (fw-orin-nano-1, 2 cameras on one max96712):** after camera A streams, camera B's *first* stream wedges — FW starts, frames reach VI and are all discarded (`corr_err` `err_data` 512/131072, then endless `uncorr_err: request timed out after 2500 ms`). A single manual per-link `CTRL1` one-shot un-wedges it (0 → 300 frames). Probe-time all-links `0x0F` does **not** prevent it. The same camera restarting 3x consecutively needs no flush (3/3 pass). A wedge coexisting with a live stream on another link is kernel-fatal (NULL deref in `vi_capture_release`/`tegra_channel_error_recover`) — confirming the flush must never fire against a live sibling. Regression traced to `9a93638`, which removed the flush from `__max96712_set_pipe`.
  The coarse `reset_oneshot(0x0F)` (all-links) on the Y12I/`is_calib` path is a **separate**, ungated reset, out of scope here (all-links → cross-glitches other cameras, [[RSDEV-13028]]). JP5's independent `kernel/nvidia` max96712 driver carries the **same** `reset_oneshot_link` (patches `5.1.2/0007` + `5.0.2/0017`); JP7 shares JP6's via the `0006→0003` symlink. Keep all generations in sync.

## SerDes pipe allocation (MAX96712)

Two allocation schemes coexist on one MAX96712 (up to 4 serializers):
- **MAX9295 serializer** (default): dynamic — `ds5_configure()` allocates a fresh deserializer pipe per stream via `get_available_pipe_id()`, configures it per-stream in `set_pipe()`, and frees it in `release_pipe()` on stop.
- **MAX96717 serializer**: it funnels all of a camera's streams through one serializer pipe (`MAX96717_PIPE_ID = 2`) as distinct VCs, so the deserializer must dedicate **one sticky "multi-VC" pipe** to that camera's link and reuse it for the driver's lifetime. `ds5_configure()` detects `ser_ops == &max96717_interface` and allocates via the optional `dser_interface::get_multi_vc_pipe_id()` hook (NULL for MAX9296).

MAX96712 keys the sticky pipe by link (`link = vc_id / MAX9295_MAX_STREAMS`): `link_multi_vc_pipe[]` (-1 = none) + `multi_vc_configured[]`. Contract, when adding/using these helpers:
- `max96712_release_pipe()` **no-ops** for a multi-VC pipe (never frees or disables it) — a stream stop must not tear it down. `max96712_pipe_is_multi_vc()` detects them and **requires `priv->lock` held**.
- `max96712_set_pipe()` lays down the multi-VC mapping **exactly once** (`__max96712_set_multi_vc_pipe()`, maps all 4 local VCs), guarded by `multi_vc_configured`. `max96712_init_settings()` clears `multi_vc_configured` (it zeroes `PIPE_EN`, voiding the mapping) so a dser re-init forces re-apply, while pipe ownership persists.
- Mappings whose data type changes at runtime are **re-applied on every** `max96712_set_pipe()` by `__max96712_update_multi_vc_pipe()`, after the configure-once block. Today that is only `vc_id == 1` (RGB `MIPI_TX21/TX22`), so RGB can switch between the regular and calibration pixel format on a sticky pipe. Anything a stream start can change must go here, not in the configure-once table.
- The multi-VC register table is validated on **link 0** only; link>0 extended-VC-msb (`MIPI_TX_EXT0..3 = link_id << 2`) is generalized to match the dynamic path but unproven — validate on hardware.
- `max96712.c` has **no repo copy** — it is carried entirely by patch files. JP6/JP7 come from `nvidia-oot/6.0/0003-…patch` (JP6.1/6.2 symlink to 6.0; JP7's `7.0/0006-…patch` and `7.1/0006-…patch` are themselves symlinks back to `6.0/0003`, verified on disk). Edit the `sources_<v>/nvidia-oot/.../max96712.c` working copy, then regenerate 0003 via `git diff HEAD^ -- drivers/media/i2c/max96712.c` (0003 touches only this file). Regenerating 0003 auto-ships JP7 too — there is no separate JP7 copy to port.
- **JP5 carries the whole max96712 driver in two *independent* patch files** — `kernel/nvidia/5.1.2/0007-…patch` (JP5.1.6 symlinks to it) and `kernel/nvidia/5.0.2/0017-…patch` — neither of which points at `6.0/0003`. Every max96712 change must be ported into all three carriers, so the function bodies stay byte-identical across generations (`reset_oneshot_link`, the multi-VC scheme and `__max96712_update_multi_vc_pipe` are all mirrored this way). When hand-editing a JP5 patch instead of regenerating it, fix up the containing hunk's new-side line count, every later hunk's new-side start offset, the diffstat, and the `index <pre>..<post>` post-image hash; verify with `git apply --check` against `git show HEAD~1:drivers/media/i2c/max96712.c` from `sources_<v>/kernel/nvidia`.

## Subagent and workflow economics

Every subagent runs its own requests, so a fan-out multiplies cost. Be deliberate.

- **Pin `model` down, never up.** An unset `model` inherits the session model, so leaving it unset is how the operator's `/model` choice reaches the fan-out — including a deliberate Fable session. Pin explicitly only to a *cheaper* tier for mechanical stages: refuting a stated claim against code, running a staged script, collecting logs, prose edits. Leave the judgement stages unset (final judge, synthesis, root-cause hunt) so they track the session. Never hardcode `model: 'opus'` — it silently overrides a downgrade the operator made on purpose, and the same argument applies to `effort`. One review workflow here ran 13 agents at the session model where the ~10 refuters should have been pinned to Sonnet.
- **Batch per-item fan-out.** One verifier per *dimension* handling all that dimension's findings beats one verifier per finding. Gate verification by severity — do not spend a high-effort agent disproving a `low` nit.
- **Don't re-send context you could point at.** A long facts preamble repeated to N agents is paid N times, and the agents usually re-read the same files anyway. Name the file paths and the specific functions instead.
- **Right-size to the diff.** A small single-file change does not need a large adversarial fan-out; a few reviewers find the same defects. Scale the fleet to the blast radius, not to the ambition.
- **Never delegate waiting.** An agent told to run a long rig job and wait burns tokens and may return nothing. Use a backgrounded Bash watcher, or the [`soak-watch`](.claude/skills/soak-watch/SKILL.md) skill, which monitored a 5.5 h soak for zero subagents and zero conversation turns.
- The repo's own agents in `.claude/agents/` are already Sonnet except `debug-agent` (Opus, justified for root-cause work). Keep new agents Sonnet unless there is a stated reason.

## Model/effort recommendation (MANDATORY)

**Every advised action must carry a model + reasoning-effort recommendation.** This applies to anything you propose as a next step: "want me to X?", a plan step, a validation leg, a build/deploy run, a follow-up ticket task, a suggested cleanup. No advised action ships without it.

Format — one line per action: `<action> — <model>, effort <tier>` plus a short clause of *why*. Effort tiers: `low | medium | high | xhigh | max`.

- **Decompose compound actions** when the parts differ. "Implement the fix and validate on HW" is at least three recommendations: the concurrency-sensitive code edit, the harness scripting, and the pass/fail log triage. Recommending one tier for the whole thing hides the part that actually needs the headroom.
- **Justify against the failure mode, not the line count.** A 10-line edit to locking or a shared-SerDes path deserves a high tier because a wrong call panics a rig or silently returns a P1; a 400-line mechanical rename does not. Cite the specific risk (race, cross-generation ABI, patch-carrier fan-out, panic-on-wedge) rather than saying "this is complex".
- **Name the tier explicitly even when it is the cheap one.** If a task genuinely doesn't care, write `any tier / inherit session` — do not omit the field. Silence reads as "not considered".
- **Flag delegation separately.** If the action is better run as a subagent or Workflow, say so and give the model/effort for the subagent, noting that both require explicit user opt-in in this repo.
- **Re-recommend when scope changes.** If new evidence makes an action riskier (e.g. HW work turns out to be panic-prone), restate the recommendation instead of letting the old one stand.

## SerDes format configuration notes

- `DS5_FW_CSI_PT` (`0x2E`) is the D40x FW CSI-PT mode selector for the OV9782 sensor — not a MIPI wire DT. Written to `DS5_RGB_STREAM_DT` to activate FW CSI passthrough mode. The sole active use is the D401 RAW8 CSI passthrough format (`mbus_code = MEDIA_BUS_FMT_SBGGR8_1X8`):
  - Write `0x2E` to `dt_addr` — FW activates CSI passthrough mode.
  - Write `0x2A` (`MIPI_CSI2_TYPE_RAW8`) to `override_addr` — the D401 FW remaps **all** wire DTs (pixel long packets from OV9782 and FW-generated short packets) to RAW8.
  - Set SerDes PIPE_X_DT = `0x2A` — aligns MAX9296 routing with the actual RAW8 wire DT. In `ds5_configure()`, guard with `mbus_code == MEDIA_BUS_FMT_SBGGR8_1X8` after the standard `data_type1` init, setting `data_type1 = MIPI_CSI2_TYPE_RAW8`.
  - Before writing `DS5_RGB_RES_WIDTH`, clamp the V4L2 format width (1612) to the OV9782 physical pixel count (1288). The FW expects the sensor pixel count; the V4L2 width stays at 1612 so `bytesperline` accommodates the actual 1612 B/line VDF output.
- Do **not** call `max9295_set_pipe_bpp()` during stream start. Both the new-pipe branch and unconditional BPP write paths were removed after experiments proved they cause `CAPTURE_CHANNEL_ERROR_COLLISION` in the Tegra VI descriptor engine (~56 extra COLLISION events per run). The default BPP register (`0x30` = enable+16bpp) over-allocates GMSL2 link bandwidth but streaming is functionally correct.

## NVCSI / VI capture notes

- **D401 OV9782 RAW10 dword alignment — RAW8 passthrough is the fix**: The D401 VDF dword-aligns each RAW10 line: 1288px × 10/8 = 1610 B → `ALIGN(1610, 4)` = 1612 B transmitted. A pure RAW10 NVCSI capture path is not viable for this sensor:
  - The NVCSI RAW10 depacketizer requires `frame_x` to be **8-pixel aligned**. No 8-aligned value gives an expected WC of exactly 1612 B (`frame_x=1288` → 1610 B → `PIXEL_LONG_LINE` → ~20% SOF; `frame_x=1296` → 1620 B → `PIXEL_SHORT_LINE` → unvalidated). Non-8-aligned values (1290, 1292) cause `CAPTURE_CHANNEL_ERROR_COLLISION` and 0% SOF regardless of WC accuracy.
  - **Correct fix**: use the D401 RAW8 CSI passthrough approach (see SerDes notes above). FW `override_addr=0x2A` remaps all wire DTs to RAW8; NVCSI is configured with `match.datatype=0x2A`, `frame_x=1612` (1 B/pixel → expected WC = 1612 B = actual VDF output). RAW8 has no depacketizer and no pixel-alignment constraint.
- **VI `dt_enable`/`dt_override` semantics (HW-mapped on JP6.2/D585, fw-orin-5, RW16 calib):** with `dt_enable=1`, DT **matching must be disabled** (`match.datatype=0`, `datatype_mask=0`) or the channel never matches a frame — even an identity override (`0x2E→0x2E`) with matching kept silently times out. Line **pixel accounting always follows the wire DT** (RAW16 = 2 B/px); doubling `frame_x` to bytes gives `err_data 512` = `PIXEL_SHORT_LINE`. Overriding to a **user-defined DT (0x30) completes frames with all-zero payload** — "no processing rule" means no data, not passthrough. Overriding to RAW8 with native geometry writes mangled widened values. The **working passthrough**: `dt_override=YUV422-8 (0x1E)` + `pixfmt T_Y8_U8__Y8_V8` (enum 16) + native geometry — byte-identical payload, 30/30 clean frames, coexists with concurrent metadata streaming. Writer 19 (`T_U8_Y8__V8_Y8`, the YUYV row's) pair-swaps (BE); writer 17 swaps the two pixel MSBs — only 16 is identity. Shipped as `nvidia-oot/6.2/0013` (symlinked 6.0/6.1/7.0/7.1/7.2) as a **per-format-row mechanism**: `struct tegra_video_format` gained trailing `dt_override`/`pixfmt_override` fields (0 = stock; existing rows untouched), set via `TEGRA_VIDEO_FORMAT_OVERRIDE(...)`, applied row-driven in `vi5_setup_surface()` — never a fourcc if-statement in vi5_fops. The old byte-swapped rows (`RW16`, and the plain `Y16I` row) were **removed from every generation's `0001` format patch** (incl. JP5) — the RGB calib surface is the **`V4L2_PIX_FMT_SGRBG16` ('GR16', upstream fourcc) override row** on the `SGRBG16_1X16` mbus, and the IR calib surface is a `Y16I` **override row** (same fourcc/32bpp geometry, now LE). Override rows also carry **transport-geometry fractions** (`frame_x_scale`/`frame_y_scale`, zero-numerator = 1/1): Y16I declares `frame_x_scale = 2/1` (32bpp V4L2 pixel on a 16bpp RAW16 wire — `frame_x` counts wire pixels, which no override can re-segment), replacing the fourcc-gated `width *= 2` formerly carried by the `0001` patches. `frame_y_scale` also scales the surface size (for flat transports like NV12-over-UD, rows = H×3/2). One structural nuance: **removing added lines from a patch can leave a context-only hunk, which `git apply` rejects as corrupt** — drop the whole no-op hunk. **Flat NV12 over UD32** (converged from PR #585, mode-agnostic): d4xx's `ds5_format` gained `override_data_type` (nonzero = the format's wire DT when it differs from the source `data_type`). **Unified contract: the FW override register always carries the wire DT** — `ds5_wire_data_type()` (`override_data_type ?: data_type`) feeds both the override register write and the serdes routing, with no camera-model branches in the value path; the D401 CSI-PT RAW8 forcing is now the SBGGR8 entries' `override_data_type` rather than inline special cases. RGB programs the register unconditionally, like depth/IR always have — every stream start (re-)asserts the format's wire DT, which displaces any stale carrier across format switches, driver reloads, and camera models alike, at the cost of one cached-suppressed register write, and the NV12 row (`RS_NV12_FLAT_1X8` mbus, a `TEGRA_VIDEO_FORMAT_GEOMETRY` row: native RAW8 match, no DT override, `frame_y_scale=3/2`) replaces #585's side-table and `sizeimage` special cases. **The NV12 wire carrier is RAW8 (0x2A), not UD32**: the MAX96717 pixel pipe (forced BPP16 + 8-bit doubling) forwards standard 8-bit DTs but silently drops user-defined ones — UD32 gave FW-streaming/VI-silence in pixel mode; UD32 remains valid for tunnel mode only. HW-validated on fw-orin-5: NV12 640x360 and 1280x720 exact 1.5·bpl·H frames, and the unified override contract's displacement semantics confirmed (YUYV/GR16 after an NV12 run re-assert their natural wire DT — HKR treats override-to-natural-DT as clear) — `sizeimage` now honors `frame_y_scale` generically at both channel.c sites. NV12 under our match-disabled block pends rig validation (#585's HW-proven config kept matching on). The JP7/noble `vi5_setup_surface` context differs from jammy, so `0013` has a JP7 variant: real file `nvidia-oot/7.0/0013` (7.1/7.2 symlink it); 6.x uses `nvidia-oot/6.2/0013` (the `V4L2_PIX_FMT_RW16` define in the kernel `0002`/format patches must **stay**: it predates the MIPI use — PR #119's librealsense UVC format set — and the same patch maps `UVC_GUID_FORMAT_RW16` to it for USB devices). Duplicate-mbus rows are fine (the fmts bitmap enumerates all of them) but duplicate *fourccs* are not: `tegra_core_get_format_by_fourcc()` is first-match and not subdev-filtered, which is why `SGRBG10` was rejected — the RAW10 row already owns it. Repeated aborted starts can wedge the whole GMSL link (all streams time out, no i2c errors); `rmmod d4xx && rmmod max96717 && modprobe d4xx` recovered it on fw-orin-5.
- The MIPI CSI-2 spec transmits RAW16 **MSB-byte-first**; the NVCSI RAW16 unpack writes little-endian `T_R16`, so a camera sending LSB-first RAW16 (D585 HKR calib streams) captures byte-swapped ("big-endian") in memory. D4xx FW is spec-conformant (depth Z16 lands correctly), so the RW16 override must stay format-gated, never applied to all RAW16 channels. Both D58x calib streams (GR16 and Y16I) are captured via the override, little-endian.
- `frame_x` in `struct vi_frame_config` is in **pixels**, not bytes. NVCSI derives expected WC as `(frame_x × bpp_for_DT) / 8` using integer truncation.

## Post-patch instruction hygiene

After every confirmed code patch, review `CLAUDE.md` against the final net diff, including any follow-up tuning edits.

- Check for stale architectural claims and remove or correct them immediately.
- Check whether the patch exposed a reusable convention; if it did, write it down as a general rule instead of leaving it implicit in code.
- If the patch changed the locking, usage, or API contract of a helper or utility function (e.g. moved lock acquisition inside/outside, changed required caller context, or altered error handling), immediately update all documentation and instructions to reflect the new contract. Always check for this class of change after any helper edit.
- If no new convention was exposed, state that explicitly in the final report and include a short justification.
- Do not treat the task as complete until that review outcome has been reported.

## Comment style

Keep comments concise and high-signal — both in-code and external (Jira/PR/status updates). Lead with the finding or the "why", in the fewest lines that stay correct; cut restated context and prose. Jira/PR comments especially: a few skimmable lines or bullets, not walls of text.

**In-code comments: 2–3 lines maximum.** State the *why* — never narrate what the code plainly does. If one comment cannot cover it, do **not** grow the block: split it into two short comments, each placed at the spot it explains (e.g. one at the constant/`#define`, one at the use site) rather than one paragraph parked above the function. Long rationale, HW evidence and cross-site invariants belong in the commit message, the PR description, or this file — not in a wall of `*` lines.

**No comments that describe an absence or a removal.** An in-code comment states what the code *does* (or a non-obvious *why*) — never what it no longer does, what was deleted, or what it deliberately omits. Delete comments like `no flush here`, `we no longer do X`, `removed Y`, `intentionally not calling Z`. If the absence carries a real invariant worth preserving (e.g. "don't re-add the flush at `set_pipe` — it truncates siblings"), record it **once** as a forward-looking rule in the commit message / PR description / this file, not as a per-site breadcrumb. This applies to the RSDEV-12608/RSDSO-21786 flush sites specifically: the "flush fires only from its two sanctioned call sites, never from `set_pipe`" invariant is documented in the Concurrency notes above and the PR — the `set_pipe`/multi-VC bodies carry no such comment.
