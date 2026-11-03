# RealSense D4XX on Qualcomm (QCS6490 / RB3 Gen 2 Vision Kit)

Build infrastructure for the Qualcomm port (RSDEV-15171). This is the
Qualcomm counterpart of `setup_workspace.sh` / `apply_patches.sh` /
`build_all.sh`, which serve NVIDIA Jetson.

**Status: builds only.** These scripts produce a kernel and a loadable
`d4xx.ko` + SerDes module set for QCS6490. They do *not* yet stream — the
device tree, the `qcom-camss` changes and the d4xx stream-model work are
tracked separately (see *Known gaps* below).

## Quick start

```bash
sudo apt install -y build-essential bc wget flex bison curl libssl-dev xxd \
                    python3 rsync kmod cpio

./setup_workspace_qcom.sh 6.18     # clone kernel + SerDes donor, fetch toolchain
./apply_patches_qcom.sh 6.18       # patch both trees, stage the module sources
./build_all_qcom.sh 6.18           # kernel + DTB + modules
```

Output lands in `images/qcom-6.18/`, with `rootfs/boot/Image`,
`rootfs/boot/dtb/qcs6490-rb3gen2.dtb` and the modules under
`rootfs/lib/modules/<ver>/extra/`.

Iterating on driver source only:

```bash
./build_all_qcom.sh --modules-only 6.18
```

Reset both source trees to their pinned bases:

```bash
./apply_patches_qcom.sh reset 6.18   # `reset` goes BEFORE the version
```

## What gets fetched

| Tree | Source | Purpose |
|---|---|---|
| `sources_qcom_6.18/kernel` | `qualcomm-linux/kernel`, branch `qcom-6.18.y`, pinned commit | The kernel we build and patch |
| `sources_qcom_6.18/nv-oot` | `nv-tegra/linux-nv-oot`, tag `jetson_36.4.3` | MAX9295/MAX9296 source donor — see below |
| `qcom-gcc/` or `l4t-gcc/7.x` | ARM GNU `aarch64-none-linux-gnu` 13.3.rel1 | Cross toolchain |

The kernel is cloned shallow at a pinned commit, so the tree is reproducible
even though `qcom-6.18.y` moves daily. Bump `QCOM_KERNEL_PIN` in
`scripts/setup-common-qcom` to advance it.

### Why a NVIDIA tree is cloned for a Qualcomm build

`max9295.c` and `max9296.c` have **no standalone copy in this repo** — they are
NVIDIA's sources plus our `nvidia-oot/6.0/0002` patch carrier. Rather than keep
a second, Qualcomm-specific copy that would silently drift from the Jetson one,
`apply_patches_qcom.sh` replays that same carrier onto a donor checkout and
symlinks the result into the module build. A MAX9296 fix therefore reaches
Jetson and Qualcomm from one edit, exactly as CLAUDE.md requires.

If the NVIDIA clone is unreachable and you already have a JetPack workspace,
point the donor at it:

```bash
ln -s $PWD/sources_6.2/nvidia-oot sources_qcom_6.18/nv-oot
```

### Only MAX9295 + MAX9296 are built

The RB3 Gen 2 vision mezzanine carries a MAX9296A deserializer and MAX9295
serializers, and nothing else can be fitted to it, so this build defines
`CONFIG_VIDEO_D4XX_SERDES_MAX9296_ONLY` and ships two SerDes modules.

d4xx names all five SerDes drivers in its `dser_interface` / `ser_interface`
tables, so without that macro `d4xx.ko` would not link unless `max96712`,
`max96717` and `max96724` were built too. The macro drops those three
interface tables and their device-tree branches; the comparison sites use the
`ds5_{dser,ser}_is_*()` predicates, which compile to `false` for an absent
part, so no call site needs an `#ifdef`. Every other platform leaves the macro
undefined and is completely unaffected — verified by building both shapes.

This is also why the in-tree name collisions below do not bite us: we no
longer build a `max96712.ko` or `max96717.ko` at all.

## The in-tree Maxim drivers, if this platform ever grows past MAX9296

Unlike the NVIDIA trees, the upstream kernel ships its own Maxim GMSL drivers,
and two of them share a name with ours. Nothing collides today because we
build neither — but anyone enabling the full SerDes set here must handle it.

`drivers/staging/media/max96712` is an unrelated 487-line Renesas-contributed
V4L2 subdev driver (ours is a 1560-line NVIDIA-derived pipe-management library
exporting 18 symbols; the two have no API in common and are not
interchangeable). It would collide twice over:

- **Module name.** It builds as `max96712.ko`, exactly ours. `depmod` resolves
  the name to the in-tree module, leaving d4xx's `max96712_*` symbols
  unresolved — `modprobe d4xx` then fails with no useful hint.
- **DT compatibles.** It claims both `maxim,max96712` and `maxim,max96724`, so
  it would race us for the device even if the module names differed.

`drivers/media/i2c/max96717.c` is the same name clash, minus the compatible
one (it claims only `maxim,max96717f`).

So: dropping `CONFIG_VIDEO_D4XX_SERDES_MAX9296_ONLY` means also disabling
`CONFIG_VIDEO_MAX96712` and `CONFIG_VIDEO_MAX96717` in
`qcom/<ver>/realsense.config`. Upstream owns these compatible strings, so our
DT nodes share a namespace with drivers we do not control.

There are no in-tree `max9295`/`max9296` drivers, so the pair we do build is
unaffected.

## Porting shims

`qcom/compat-include/` holds two headers so the NVIDIA SerDes sources build
**unmodified** off-Tegra:

- `nvidia/conftest.h` — NVIDIA generates this by probing the kernel; we decide
  the two macros the SerDes sources use from `LINUX_VERSION_CODE`.
- `media/camera_common.h` — empty. All five SerDes drivers include it and
  reference nothing from it.

The only driver-source change this port needed is the `msleep_range()` guard in
`d4xx_main.c`: it is an NVIDIA `camera_common.h` helper, previously defined
only for a narrow 5.15.x window, now defined whenever that header is absent.

## Known gaps (not addressed by this infrastructure)

These are the functional blockers from the RSDEV-15171 assessment. The build
flow here is a prerequisite for all of them, not a solution to any:

1. **Qualcomm CCI caps I2C at 10 B write / 12 B read.** DFU (1024 B blocks),
   GVD (276–606 B), HWMC blobs and the camera format descriptor all exceed
   this. If the mezzanine's GMSL I2C is on CCI rather than a GENI/QUP bus, an
   `i2c-qcom-cci` burst-length patch is required. Chunking is not viable.
2. **`camss` hardcodes virtual channel 0.** `csid_configure_stream()` passes
   `vc = 0` for every RDI port, so our four streams cannot be separated — the
   wire data types collide (depth+RGB both `0x1E`, IR+IMU both `0x2A`).
3. **`camss` format tables lack our formats.** CSID `formats_gen2` has no
   `RGB888_1X24` (Y12I), no RAW16 and no DT `0x12` (embedded metadata); the VFE
   RDI tables have no Z16/Y8I/Y12I/RW16 fourccs.
4. **One sensor subdev per CSIPHY.** `camss` still uses legacy `s_stream` with
   no stream id, while d4xx registers four I2C instances per camera — a Tegra
   VI-channel-shaped design.
5. **Concurrency budget.** A CSID exposes 4 source streams and a VFE 3 lines
   (full) or 4 (lite). Full D457 parity is 7 nodes per camera.
6. **No device tree.** `qcs6490-rb3gen2-vision-mezzanine.dtso` upstream
   describes only an IMX577; the GMSL nodes do not exist yet.

The build also emits one `-Wunused-function` warning for
`ds5_ctrl_driver_owned()`. That is not noise: the whole descriptor-driven
control-sync block in `d4xx_main.c` sits under `CONFIG_TEGRA_CAMERA_PLATFORM`,
so off-Tegra its only caller disappears. Resolving that belongs to the d4xx
stream/control rework (gap 4), not here — silencing the warning would hide a
real gap.

## Layout

```
setup_workspace_qcom.sh        fetch sources + toolchain
apply_patches_qcom.sh          patch both trees, stage module sources
build_all_qcom.sh              kernel, DTB, modules
scripts/setup-common-qcom      version table, paths, toolchain selection
qcom/6.18/*.patch              kernel patches (RealSense UAPI formats)
qcom/6.18/realsense.config     config fragment merged over the arm64 defconfig
qcom/compat-include/           off-Tegra shims for the NVIDIA SerDes sources
qcom/modules/Kbuild            out-of-tree module wiring
```
