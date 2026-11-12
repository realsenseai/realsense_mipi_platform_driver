# RealSense D4XX on Qualcomm (QCS6490 / RB3 Gen 2 Vision Kit)

Build infrastructure for the Qualcomm port (RSDEV-15171), targeting **Ubuntu
24.04 LTS**. This is the Qualcomm counterpart of `setup_workspace.sh` /
`apply_patches.sh` / `build_all.sh`, which serve NVIDIA Jetson.

**Status: builds only, nothing has run on a board.** These scripts produce
loadable `d4xx.ko` + SerDes modules and a CAMSS fork, packaged as DKMS `.deb`s.
The fork already carries the multi-stream capture path (see *The CAMSS fork*),
written against the source and compile-tested; the device tree and the d4xx
side of the stream contract are still to do (see *Known gaps*).

**Nothing here patches the kernel.** Every consumer is out-of-tree, so the
board keeps Canonical's signed, updated `linux-qcom`.

## Target

| | |
|---|---|
| Distro | Ubuntu 24.04 LTS (noble), image `x15` (2026-09-25) |
| Kernel | `linux-qcom` 6.8 (`6.8.0-1089-qcom` on the x15 image) |
| Board | RB3 Gen 2 Vision Kit, QCS6490 |

24.04 is the **newest** Ubuntu Canonical publishes for this board — the 25.10
and 26.04 Qualcomm images are IQ-X only. That is fortunate rather than
limiting; see *Why 24.04 is the right target* below.

## Quick start

```bash
sudo apt install -y build-essential bc wget flex bison curl libssl-dev xxd \
                    python3 rsync kmod cpio dpkg-dev

./setup_workspace_qcom.sh noble    # Ubuntu kernel source + SerDes donor + toolchain
./apply_patches_qcom.sh noble      # stage the out-of-tree module trees
./build_all_qcom.sh noble          # compile gate + DKMS .debs
```

Output lands in `images/qcom-noble/dkms/`:

```
realsense-d4xx-dkms_<ver>_all.deb     d4xx + max9295 + max9296
realsense-camss-dkms_<ver>_all.deb    forked qcom-camss
```

Install on the board (which needs `linux-headers-qcom`, already in the stock
image):

```bash
sudo apt install ./realsense-d4xx-dkms_<ver>_all.deb
sudo apt install ./realsense-camss-dkms_<ver>_all.deb
```

Reset the source trees:

```bash
./apply_patches_qcom.sh reset noble   # `reset` goes BEFORE the series
```

## How the build splits

- **The host cross-build is a compile gate only.** It configures the Ubuntu
  kernel source with the plain arm64 `defconfig` and runs `modules_prepare`,
  purely to catch compile errors in CI. Ubuntu generates its real config from
  `debian.qcom/config/annotations` at package build time, so reproducing it
  here would buy nothing.
- **DKMS on the board is the deliverable.** The `.deb`s carry source; the board
  rebuilds against its own `linux-headers-qcom`. That is what makes the modules
  ABI-correct for whatever kernel Canonical ships that week.

## What gets fetched

| Tree | Source | Purpose |
|---|---|---|
| `sources_qcom_noble/kernel` | Ubuntu `linux-qcom` source package (orig + `.diff.gz`), pinned version | Compile gate, and the CAMSS fork's base |
| `sources_qcom_noble/nv-oot` | `nv-tegra/linux-nv-oot`, tag `jetson_36.4.3` | MAX9295/MAX9296 source donor |
| `qcom-gcc/` or `l4t-gcc/7.x` | ARM GNU `aarch64-none-linux-gnu` 13.3.rel1 | Cross toolchain |

The kernel comes from the **Debian source package, not git**: Canonical's
kernel git is behind an auth wall and its cgit times out, while the archive is
public and versioned. The `.diff.gz` carries Canonical's delta — which is where
the sc7280 CAMSS backport lives.

### Why a NVIDIA tree is cloned for a Qualcomm build

`max9295.c` and `max9296.c` have **no standalone copy in this repo** — they are
NVIDIA's sources plus our `nvidia-oot/6.0/0002` patch carrier.
`apply_patches_qcom.sh` replays that same carrier onto a donor checkout and
symlinks the result, so a MAX9296 fix reaches Jetson and Qualcomm from one
edit. Never materialise a second copy under `qcom/`.

If the NVIDIA clone is unreachable and you have a JetPack workspace:

```bash
ln -s $PWD/sources_6.2/nvidia-oot sources_qcom_noble/nv-oot
```

### Why only MAX9295 + MAX9296

The vision mezzanine carries a MAX9296A deserializer and MAX9295 serializers,
and nothing else can be fitted, so the build defines
`CONFIG_VIDEO_D4XX_SERDES_MAX9296_ONLY`. d4xx names all five SerDes drivers in
its interface tables, so without that macro `d4xx.ko` would not link unless
`max96712`, `max96717` and `max96724` were built too. The macro drops those
three tables and their device-tree branches; comparison sites use the
`ds5_{dser,ser}_is_*()` predicates, which compile to `false` for an absent
part, so no call site needs an `#ifdef`. Every other platform leaves the macro
undefined and is unaffected.

It also sidesteps the in-tree `max96712.ko` / `max96717.ko` name collisions
entirely, since we build neither.

## Why 24.04 is the right target

Canonical's 6.8 `linux-qcom` carries the full sc7280 CAMSS backport — driver
(`sc7280_resources`, `qcom,sc7280-camss`) and DTS. Mainline only got sc7280
CAMSS in **v6.18**.

More importantly, **6.8 maps virtual channels the way we need and newer kernels
do not**:

```c
/* Ubuntu 6.8 */
__csid_configure_rdi_stream(csid, enable, i);       /* i, not 0 */
    input_format = &csid->fmt[MSM_CSID_PAD_FIRST_SRC + vc];
    val |= format->data_type << RDI_CFG0_DATA_TYPE;
    val |= vc            << RDI_CFG0_VIRTUAL_CHANNEL;
```

Source pad N → RDI N → VC N, with pad N's own format supplying the data type —
exactly what our four streams need. Mainline later *decoupled* port index from
VC (correct for a general sensor, which should not have to occupy port 3 to use
VC 3) and passes a hardcoded `0`; `qcom-6.18.y` and even `qcom-7.3` still do.

So moving to a newer kernel would **re-introduce** VC work, not remove it.

## Why metadata needs no kernel patch here

On Jetson, embedded metadata forced kernel patching: the Tegra VI has one
channel per sensor stream, so capturing the metadata DT meant teaching the VI
*core* to create an extra channel — `channel.c`, `graph.c`, `mc_common.c`,
`vi5_fops.c` plus two core headers.

CAMSS already provides what Tegra had to be taught. Each `struct vfe_line` owns
a `struct camss_video video_out`, registered per line by `msm_video_register()`
— **every RDI is already its own `/dev/videoN`**. Metadata is therefore not a
special mechanism, just another `(VC, DT)` → RDI route: a CSID format entry
mapping an mbus code to DT `0x12`, a VFE RDI row, and the per-RDI VC selection
6.8 already does.

All of that lives inside the CAMSS module, which is why the whole port stays
out-of-tree.

## The CAMSS fork

`CONFIG_VIDEO_QCOM_CAMSS` is `tristate`, every object is local to its own
directory, and it includes only public kernel headers — so it builds
out-of-tree unmodified. `apply_patches_qcom.sh` copies the driver out of the
kernel tree into `sources_qcom_noble/modules/camss` and applies
`qcom/<series>/camss/*.patch` on top.

DKMS installs to `/lib/modules/<ver>/updates/dkms/`, which `depmod` prefers
over `kernel/`, so our fork replaces the stock `qcom-camss.ko` **without**
touching the kernel package or blacklisting anything.

`qcom/modules/camss.Kbuild` hardcodes the object list; `apply_patches_qcom.sh`
diffs it against the kernel's own Makefile and warns on drift (it already
caught 6.8 and 6.18 differing).

### What the fork changes

Five patches in `qcom/noble/camss/`, applied in order. None has run on
hardware.

| Patch | What it does |
|---|---|
| `0001` format rows | The media-bus codes d4xx emits, in the gen2 CSID, CSIPHY and VFE RDI tables, with their fourccs: Z16, Y8I, Y12I, Y16I, GR16, flat NV12, BGGR10P, RSVL and the embedded-metadata row (DT `0x12`, fourcc `D4XX`). |
| `0002` per-pad CSID | Each CSID source pad keeps its own code and size. Its RDI takes VC and DT from the sensor's frame descriptor, for the stream chosen by the CSID controls `RDI0 Stream` … `RDI3 Stream` (default: RDI N captures stream N). |
| `0003` independent streams | A video node starts and stops only its own CSID pad and its own sensor stream. The CSIPHY lanes stay up from the first stream to the last. |
| `0004` controls | A video node exposes the controls of the sensor stream it captures, as the Tegra VI does. Rebuilt when the node is opened with no other handle on it. |
| `0005` bring-up aids | Link frequency from the CAMSS endpoint's `link-frequencies` when the sensor reports none; `dev_dbg` lines for lanes, link frequency, per-RDI stream/VC/DT and raw IRQ status at RDI stop. |

**Why not the userspace streams API.** Ubuntu's 6.8 compiles it off:
`v4l2_subdev_enable_streams_api` is a `static bool` left false in
`v4l2-subdev.c`, so `VIDIOC_SUBDEV_S_ROUTING` returns `ENOIOCTLCMD` and the
core zeroes the `stream` field of the format ioctls. Turning it on means
patching `videodev`, which this port does not do. Inside the kernel the API
works, so the fork uses it there and never needs userspace to name a stream:
every format userspace sets sits on a single-stream pad, the sensor's routing
is fixed by the sensor driver, and the stream → RDI choice is a CSID control.

### What the sensor has to provide

| For | The bound subdev provides | Without it |
|---|---|---|
| Being found | `MEDIA_ENT_F_CAM_SENSOR` and one source pad | Not linked |
| VC and DT per stream | `get_frame_desc` on the source pad: one CSI-2 entry per stream with `stream`, `bus.csi2.vc`, `bus.csi2.dt` | RDI N captures VC N in the DT of its pad format |
| Independent start/stop | `V4L2_SUBDEV_FL_STREAMS`, an active state with routing, `enable_streams` / `disable_streams` | `s_stream` once, for the first stream and until the last |
| Controls per node | The subdev linked to the sink pad a stream is routed from has a control handler | The bound subdev's own handler on every node |
| Link frequency | `V4L2_CID_LINK_FREQ` | `link-frequencies` on the CAMSS endpoint |

d4xx's mux satisfies the first row today. Its `get_frame_desc` is a
placeholder with no bus type, which the fork treats as absent — and the
fallback already fits d4xx's streams when their `vc-id`s are 0..3, since the
pad-format rows carry the same wire data types d4xx uses. The rest is the
single-instance work that builds on the role parameter (RSDEV-15178).

### Configuring the graph

CAMSS does not propagate formats from the sensor, so userspace sets the whole
graph before the first stream starts — links cannot change while anything
streams. A sketch for one camera on a lite path (four RDIs); entity numbers
are placeholders until the device tree exists:

```bash
M=/dev/media0
media-ctl -d $M -l '"msm_csiphy4":1->"msm_csid3":0[1]'
for n in 0 1 2 3; do
    media-ctl -d $M -l "\"msm_csid3\":$((n + 1))->\"msm_vfe3_rdi$n\":0[1]"
done

# The multiplexed link is validated on one nominal format: the sensor
# source pad's, which is the depth stream's.
media-ctl -d $M -V '"msm_csiphy4":0[fmt:UYVY8_1X16/848x480]'
media-ctl -d $M -V '"msm_csid3":0[fmt:UYVY8_1X16/848x480]'

# One format per stream, on the CSID source pad and the VFE line behind it.
media-ctl -d $M -V '"msm_csid3":1[fmt:UYVY8_1X16/848x480]'      # depth
media-ctl -d $M -V '"msm_vfe3_rdi0":0[fmt:UYVY8_1X16/848x480]'
media-ctl -d $M -V '"msm_csid3":2[fmt:YUYV8_1X16/1280x720]'     # colour
media-ctl -d $M -V '"msm_vfe3_rdi1":0[fmt:YUYV8_1X16/1280x720]'
media-ctl -d $M -V '"msm_csid3":3[fmt:Y8_1X8/848x480]'          # infrared
media-ctl -d $M -V '"msm_vfe3_rdi2":0[fmt:Y8_1X8/848x480]'
media-ctl -d $M -V '"msm_csid3":4[fmt:Y8_1X8/32x1]'             # IMU
media-ctl -d $M -V '"msm_vfe3_rdi3":0[fmt:Y8_1X8/32x1]'

# The node picks the fourcc among the rows of its media-bus code.
v4l2-ctl -d /dev/video0 --set-fmt-video=width=848,height=480,pixelformat='Z16 '

# Capture another stream on an RDI, e.g. stream 4 on RDI3. The control is on
# the msm_csid3 subdev node and is read when the RDI next starts.
v4l2-ctl -d /dev/v4l-subdevN -c rdi3_stream=4
```

Open a video node only after its links are set: that is when it takes its
controls.

### Limits of the fork

- **One CSID per CSIPHY**, so a camera gets four RDIs on a lite path
  (CSID 3/4 → VFE 3/4) and three on a full one. Four image streams fit a lite
  path; their three metadata streams do not fit beside them.
- **Metadata is a capture node, not a metadata node.** An RDI set to the
  `0x12` stream delivers the embedded lines as a `D4XX`-fourcc image.
  librealsense's MIPI backend expects `V4L2_BUF_TYPE_META_CAPTURE`.
- **`bytesused` is the full buffer**, as on Jetson, for variable-length frames.
- **BGGR10P stride.** The D401 passthrough line is 1612 bytes for 1288 pixels;
  the node reports 1616 (CAMSS aligns lines to 16), while the RDI writes the
  bytes as they arrive.
- **No compressed carriers.** The H.264 and JPEG rows need carrier geometry
  the video node does not have.
- **Only gen2 CSIDs** (sc7280 among them) configure one RDI at a time. Other
  revisions keep the stock behaviour of configuring all linked RDIs together.

## Porting shims

`qcom/compat-include/` holds three headers:

- `nvidia/conftest.h` — NVIDIA generates this by probing the kernel; we decide
  the two macros the SerDes sources use from `LINUX_VERSION_CODE`.
- `media/camera_common.h` — the SerDes sources reference no symbol *declared*
  by NVIDIA's version; what they use is its transitive includes, so this shim
  is just that include subset. Widen it only for a symbol genuinely declared
  there.
- `media/rs-formats.h` — the RealSense private media-bus codes and the two
  fourccs 6.8 lacks (Y16I, RW16, RSVL), force-included into both module trees.
  **This replaces the kernel UAPI patch**: Z16, Y8I, Y12I, GREY and SGRBG16
  are in 6.8, and librealsense takes its fourccs from its own headers.

The only driver-source change this port needed is the `msleep_range()` guard in
`d4xx_main.c`: an NVIDIA `camera_common.h` helper, now defined whenever that
header is absent rather than only in the 5.15.136–147 window.

## Known gaps

1. **Qualcomm CCI caps I2C at 11 B write / 12 B read.** Proven from the shipped
   DTB: the mezzanine camera sits on `cci1_i2c1`, and the node is
   `qcom,sc7280-cci` + `qcom,msm8996-cci` → `cci_v2_data` (the driver stages
   one `CCI_I2C_WRITE` per transfer in a 12-byte buffer, 1 command byte + 11
   data; the queue itself is 64 words deep, so this reads as a driver limit
   until Qualcomm says otherwise). DFU (1024 B
   blocks), GVD (276–606 B), HWMC and the format descriptor all exceed it.
   Chunking is not viable — the camera's register file returns from the
   register address on every read. Needs either a GENI/QUP bus or an
   `i2c-qcom-cci` burst-length patch. **Top blocker.**
2. **The CAMSS fork is untested.** It compiles and its logic was read against
   the 6.8 source; no register it writes has been seen on hardware.
3. **d4xx is still four instances per camera.** CAMSS binds one subdev per
   CSIPHY, and the fork now gives that subdev what it needs to carry four
   streams (*What the sensor has to provide*). d4xx's side — one instance, the
   streams routing, `enable_streams`, a CSI-2 frame descriptor — is not
   written.
4. **Concurrency budget.** A CSID exposes 4 source streams, a VFE 3 lines
   (full) or 4 (lite), and a CSIPHY feeds one CSID. Full D457 parity is 7
   nodes per camera, so metadata is cut for capacity unless two CSIDs can
   share a CSIPHY — a hardware question for bring-up.
5. **No device tree.** The shipped RB3 vision-mezz DTB has `camss` and both
   `cci` nodes `status = "disabled"` and only CSIPHY3 → IMX577 wired; no GMSL
   nodes at all. (Qualcomm *does* have a GMSL DT model —
   `qcom,cam-gmsl-deserializer0..3`, `cam-gmsl-sensor0..15` — but only on the
   sa8775p/QCS9100 ADAS boards, under CAMX rather than CAMSS.)
6. The build emits one `-Wunused-function` for `ds5_ctrl_driver_owned()`: the
   descriptor control-sync block is under `CONFIG_TEGRA_CAMERA_PLATFORM`, so
   off-Tegra its only caller vanishes. That belongs to gap 3, not here.

## Layout

```
setup_workspace_qcom.sh        fetch Ubuntu kernel source + donor + toolchain
apply_patches_qcom.sh          stage the out-of-tree module trees
build_all_qcom.sh              compile gate + DKMS .debs
scripts/setup-common-qcom      series table, pinned kernel version, toolchain
qcom/modules/Kbuild            d4xx + SerDes out-of-tree wiring
qcom/modules/camss.Kbuild      CAMSS fork out-of-tree wiring
qcom/compat-include/           off-Tegra shims + RealSense private formats
qcom/noble/camss/*.patch       CAMSS fork patches, applied in order
```
