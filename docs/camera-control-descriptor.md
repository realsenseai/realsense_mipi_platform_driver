<!-- Source of truth for this design note. Edit it here.

     Companion to camera-format-descriptor.md. Same wire scheme, same aperture, a
     different set of tables. This file is step 1 of RSDEV-14835: the inventory and
     taxonomy that the wire format has to be designed against. -->

# Camera-Resident Control Descriptor — Inventory and Taxonomy

The format descriptor moved *what a camera can stream* onto the camera. RSDEV-14835 asks
for the same treatment for *what a camera can be told*. Before a record layout can be
designed, two questions have to be answered with evidence: **what kinds of control exist**,
and **which camera exposes which**.

DESIGN NOTE **control descriptor** · STATUS **inventory — design pending** · DRIVER **kernel/realsense/d4xx.c** · TICKET **RSDEV-14835** · COMPANION **[camera-format-descriptor.md](camera-format-descriptor.md)**

> Sources: `kernel/realsense/d4xx.c` at `dev` (`7cbf944`, module version 1.0.7.9), and a live
> enumeration on **fw-orin-5** — D585 at `9-001a`, D457 at `9-002a`, JP6.2, same module
> build. Every per-SKU difference claimed below was read off those two cameras, not inferred
> from the source alone.

## 1 · Where control knowledge lives today

This is the first way controls differ from formats. The format tables were in **one** place —
a `switch (dev_type)` repeated per sensor in `ds5_fixed_configuration()`. Control knowledge
is spread across **five** places in the driver, and a sixth outside the repo:

| # | Place | What it decides | Movable? |
|---|---|---|---|
| 1 | 37 `v4l2_ctrl_config` structs | name, V4L2 type, `dims`/`elem_size`, flags, min/max/step/def, menu strings | **Yes** |
| 2 | `ds5_ctrl_init()` — four `sid` branches × `is_d58x` | which node gets which control | **Yes** |
| 3 | `ds5_adjust_sync_mode_control()`, `ds5_adjust_rgb_controls()`, `d500_adjust_device_mode_controls()` | post-probe per-SKU range / menu-mask / disable fixups | **Yes** |
| 4 | the register map (`DS5_*`, `D58X_*`, `D500_*_XU_BASE`) + `ds5_rgb_ctrl_offset()` | which address or opcode the value is written to | **Partly** |
| 5 | `ds5_s_ctrl()` / `ds5_g_volatile_ctrl()` — ~40 `case` arms each | transport, encoding, locking, state gating, errno contract | **No** |
| 6 | librealsense `RS_CAMERA_CID_*` + `xu_to_cid()` (`src/linux/backend-v4l2.cpp`) | the SDK's independent, hand-maintained copy of the CID numbering | **No** (separate repo) |

A descriptor can realistically absorb **#1, #2 and #3** — which is where every per-SKU
divergence listed in §5 actually lives. It can carry the *data* of #4. It cannot touch #5,
and that bounds the payoff exactly as the format descriptor's §10 did.

## 2 · Taxonomy: five orthogonal axes

A control is not one thing. Each of the five axes below varies independently, and the
descriptor has to encode each one it intends to move.

### 2.1 Transport — how the value reaches the camera

| Tag | Transport | Shape | Examples |
|---|---|---|---|
| **T1** | Single FW register, fixed offset from the node's `control_base` (`0x4100` depth / `0x4200` RGB) | one 16-bit word | exposure, gain, laser power, sync mode, PWM, visual preset, readout shaping, temperatures, error code |
| **T2** | Same, but the **offset differs per SKU** via `ds5_rgb_ctrl_offset()` | one 16-bit word | every RGB ISP control: `0x42{08,10,14,18,1C,20}` on D4xx vs `0x42{30,32,34,36,38,3A,3C}` on D58x — note the stride also changes, 4 B → 2 B |
| **T3** | HWMC command, fixed opcode, params in the request header | mailbox round-trip | GVD `0x10`, LOG `0x0F`, ERB `0x17`, EWB `0x18`, calib get/set `0x15`/`0x62`, AE ROI `0x44`/`0x45`, AE setpoint `0x2B`/`0x2C`, AE type `0x87`/`0x88` |
| **T4** | HWMC **passthrough** — the driver does not interpret the payload | opaque blob | `hwmc`, `hwmc_rw` |
| **T5** | D5xx XU register window — a packed struct at a `0x45xx` base, written and read back | 1 B … 38 B | gyro `0x4538`, accel `0x4539`, device mode `0x4528`, 2C AE policy `0x4530`, aligned depth `0x4599`, detection distance `0x4598`, MinZ `0x4500`, decimation `0x4540`, temporal `0x4568` |
| **T6** | Driver-orchestrated — the control triggers a host-side sequence, not a single write | action | `hw_reset` → `ds5_hw_reset_with_recovery()`: HWMC `0x20`, readiness poll, `ds5_hw_init()`, SerDes line-buffer flush, descriptor reload |

> [!NOTE]
> **T5 is the one that grew fastest and is the least uniform.** Nine of the eighteen
> SKU-conditional controls are XU windows, and all nine are D58x — each with its own
> packing, its own readback
> rule and its own errno table (`D500_CAMERA_CID_ACCEL_SENSITIVITY` is the reference; see
> CLAUDE.md). This is where descriptor-driven registration would pay off first — and also
> where it is hardest, because the *semantics* (readback-verify, `0xFF` means absent) live in
> `ds5_s_ctrl()`, which cannot move.

### 2.2 Data shape

| Shape | V4L2 type | Count | Notes |
|---|---|---|---|
| Boolean | `BOOLEAN` | 5 | laser on/off, AWB, AE priority, aligned depth, detection distance |
| Scalar integer | `INTEGER` / `U32` | 19 | exposure is forced to `U32` after `v4l2_ctrl_new_std()` to match the UVC/USB width |
| Menu | `MENU` | 8 | AE auto, power-line frequency, sync mode, visual preset, device mode, 2C AE policy, gyro, accel |
| Action | `BUTTON` | 1 | `hw_reset` |
| Byte array | `U8[]` + `has-payload` | 12 | 8 B (AE ROI) … 1028 B (legacy HWMC) |
| Scalar in an array | `U32[1]` | 1 | `fw_version` |
| **Heterogeneous** `u32` array | `U32[]` + custom `v4l2_ctrl_type_ops` | 3 | MinZ/decimation/temporal: each slot has its own min/max/step/def, which the scalar `v4l2_ctrl_config` fields cannot express — `.init` supplies per-slot defaults, `.validate` per-slot clamping |

### 2.3 Access semantics

| Semantics | Flags | Examples |
|---|---|---|
| R/W, single control | `VOLATILE \| EXECUTE_ON_WRITE` | exposure, gain, sync mode, AE mode, aligned depth — **required** for anything librealsense reaches through a USB XU selector, because `xu_to_cid()` maps one selector to one CID |
| Read-only telemetry | `VOLATILE \| READ_ONLY` | temperatures, GVD, fw_version, logger |
| **Split get/set pair** | two CIDs | AE ROI, AE setpoint, calib tables — legal **only** for controls the SDK drives through the HWMC blob, never through a selector |
| Write-only in practice | no `VOLATILE` | `set_depth_calib`, `set_coeff_calib`, `ewb` |
| Action | `EXECUTE_ON_WRITE`, `BUTTON` | `hw_reset` |
| **Read-to-clear** | `VOLATILE \| READ_ONLY`, side-effecting | `error_code` (`0x413C`) — every `G_CTRL` consumes the latched error, so nothing in the driver may read it implicitly |

### 2.4 Scope — which node owns it, and whether that is correct

| Scope | Hosted on | Examples |
|---|---|---|
| Genuinely per-sensor | that sensor's node | exposure, gain, AE — the same CID resolves to a different register base per node |
| Shared depth/IR pair | depth **and** IR | laser power, manual laser power — one physical emitter, two handles |
| Camera-wide, **parked on depth** | depth only | sync mode, device mode, visual preset, PWM, error code, temperatures, readout shaping, and all nine D5xx depth XU windows |
| Camera-wide, parked on IMU | IMU only | gyro / accel sensitivity |
| Camera-wide, replicated | depth + IR + RGB | HWMC, HWMC_RW, hw_reset, ERB/EWB, calib tables, GVD, logger, fw_version, AE ROI/setpoint |

> The third and fifth rows are the subject of **RSDEV-14562** (per-stream instances coalesced
> to one). A control descriptor that encodes "which node" would bake today's layout into the
> wire format. **It should encode scope instead** — `camera` / `sensor:<stream_id>` — and let
> the driver decide where to hang it. That is the one place these two tickets must agree.

### 2.5 Availability gating — why a control may be absent or different

| Gate | Decided by | Example |
|---|---|---|
| SKU presence | device type (`0x0310`) | readout shaping D4xx-only; MinZ/temporal/decimation D58x-only |
| SKU range/default | device type | manual laser power `0..360/30 def 150` (D457) vs `0..540/45 def 225` (D585) |
| SKU menu composition | device type | sync mode: D58x skip-masks **Master**, keeping External's wire value at 2 |
| FW version | `fw_version`, checked at **read** time | `error_code` returns `-EOPNOTSUPP` below 5.17.6 on D4xx; gyro `0x4538` only exists in HKR FW from 2026-09-03 |
| Runtime device state | another control's value | `2c_ae_policy` is `DISABLED` until `device_mode` reads 2C |
| Stream state | streaming flags | XU writes gated `-EBUSY` while the relevant stream (or the whole camera) is live |

## 3 · The inventory: 49 control IDs

Grouped by where `ds5_ctrl_init()` registers them. "Transport" uses the §2.1 tags.
Addresses are absolute for a depth node (`control_base = 0x4100`).

### 3.1 Registered on depth, IR **and** RGB (17 custom + 3 standard)

| Control | CID | Type | Transport | Detail |
|---|---|---|---|---|
| `auto_exposure` | `V4L2_CID_EXPOSURE_AUTO` | menu | T1 | `+0x0C`; menu restricted to Manual + Aperture-priority |
| `exposure_time_absolute` | `V4L2_CID_EXPOSURE_ABSOLUTE` | u32 | T1 | `+0x00`/`+0x02` LSB/MSB; depth 1..200000, RGB 1..10000 |
| `analogue_gain` | `V4L2_CID_ANALOGUE_GAIN` | int | T1 | `+0x04` |
| `logger` | `BASE+0` | u8[1024] RO | T3 | opcode `0x0F`, then a raw read of the HWMC data window |
| `fw_version` | `BASE+7` | u32[1] RO | T1 | `0x030C` |
| `gvd` | `BASE+8` | u8[] RO | T3 | opcode `0x10`; **dims 276 (D4xx) / 606 (D58x)** |
| `get_depth_calib` / `set_depth_calib` | `BASE+3` / `+4` | u8[256] | T3 | `0x15` / `0x62`, table id `0x1F` |
| `get_coeff_calib` / `set_coeff_calib` | `BASE+5` / `+6` | u8[512] | T3 | `0x15` / `0x62`, table id `0x19` |
| `ae_roi_get` / `ae_roi_set` | `BASE+9` / `+10` | u8[8] (u16 elems) | T3 | `0x45` / `0x44` |
| `ae_setpoint_get` / `ae_setpoint_set` | `BASE+11` / `+12` | int 0..4095 | T3 | `0x2C` / `0x2B`, param1 `0x0A` |
| `depth_ae_mode` | `BASE+35` | int 0..1 | T3 | `0x88` / `0x87`; single R/W control — XU selector `0x11` |
| `erb_eeprom_read` | `BASE+13` | u8[1020] | T3 | `0x17` |
| `ewb_eeprom_write` | `BASE+14` | u8[1020] | T3 | `0x18` |
| `hwmc` | `BASE+15` | u8[1028] | T4 | legacy tools |
| `hwmc_rw` | `BASE+32` | u8[1024] | T4 | UVC-compatible SET/GET pair |
| `hw_reset` | `BASE+33` | button | T6 | full recovery sequence, not a bare `0x20` |

### 3.2 Depth **and** IR only (2)

| Control | CID | Type | Transport | Detail |
|---|---|---|---|---|
| `laser_power_on_off` | `BASE+1` | bool | T1 | `0x4108` |
| `manual_laser_power` | `BASE+2` | int | T1 | `0x4124`; **range differs per SKU** |

### 3.3 RGB only (6 + 4 D58x-only)

| Control | CID | D4xx addr | D58x addr | Notes |
|---|---|---|---|---|
| `saturation` | `V4L2_CID_SATURATION` | `0x4210` | `0x4230` | |
| `sharpness` | `V4L2_CID_SHARPNESS` | `0x4214` | `0x4232` | |
| `white_balance_temperature` | `V4L2_CID_WHITE_BALANCE_TEMPERATURE` | `0x4218` | `0x4234` | |
| `white_balance_automatic` | `V4L2_CID_AUTO_WHITE_BALANCE` | `0x421C` | `0x4238` | |
| `power_line_frequency` | `V4L2_CID_POWER_LINE_FREQUENCY` | `0x4220` | `0x423A` | **menu max 3 (Auto) on D4xx, 2 on D58x** |
| `exposure_dynamic_framerate` | `V4L2_CID_EXPOSURE_AUTO_PRIORITY` | `0x4208` | `0x423C` | **`DISABLED` on D40x** |
| `brightness` | `V4L2_CID_BRIGHTNESS` | — | `0x4224` | D58x only |
| `contrast` | `V4L2_CID_CONTRAST` | — | `0x4228` | D58x only |
| `gamma` | `V4L2_CID_GAMMA` | — | `0x422C` | D58x only |
| `hue` | `V4L2_CID_HUE` | — | `0x4236` | D58x only |

### 3.4 Depth only (3 common + 1 D4xx + 11 D58x)

| Control | CID | Type | Transport | SKU |
|---|---|---|---|---|
| `camera_sync_mode` | `BASE+16` | menu | T1 `0x412C` | both — **menu differs** |
| `pwm_frequency_selector` | `BASE+22` | int | T1 `0x4128` | both |
| `error_code` | `BASE+27` | int RO, read-to-clear | T1 `0x413C` | both — **FW-gated on D4xx** |
| `readout_shaping` | `BASE+34` | int 0..100 | T1 `0x4130` | D4xx only |
| `visual_preset` | `BASE+21` | menu 0..5 | T1 `0x4120` | **D58x only today** (see §7) |
| `soc_pvt_temperature` | `BASE+24` | int RO | T1 `0x4130` | D58x only |
| `ohm_temperature` | `BASE+26` | int RO | T1 `0x4134` | D58x only |
| `projector_temperature` | `BASE+25` | int RO | T1 `0x4138` | D58x only |
| `device_mode` | `BASE+36` | menu 3C/2C | T5 `0x4528` | D58x only |
| `2c_ae_policy` | `BASE+37` | menu | T5 `0x4530` | D58x only — **`DISABLED` unless 2C** |
| `enable_aligned_depth` | `BASE+55` | bool | T5 `0x4599` | D58x only — camera must be idle |
| `detection_distance` | `BASE+40` | bool | T5 `0x4598` | D58x only |
| `minz_configuration` | `BASE+44` | u32[7] | T5 `0x4500` | D58x only — heterogeneous slots |
| `d500_decimation_configuration` | `BASE+49` | u32[2] | T5 `0x4540` | D58x only — heterogeneous slots |
| `d500_temporal_configuration` | `BASE+51` | u32[4] | T5 `0x4568` | D58x only — heterogeneous slots, 38 B window |

> `readout_shaping` and `soc_pvt_temperature` share offset `0x0030`. The two meanings are
> mutually exclusive by SKU and `ds5_ctrl_init()` registers exactly one of them — a hazard a
> descriptor would remove by construction, since the camera would name only its own.

### 3.5 IMU only (1 + 2 D58x)

| Control | CID | Type | Transport | SKU |
|---|---|---|---|---|
| `fw_version` | `BASE+7` | u32[1] RO | T1 | both |
| `gyro_sensitivity` | `BASE+38` | menu 0..4 | T5 `0x4538` | D58x only |
| `accel_sensitivity` | `BASE+39` | menu 0..3 | T5 `0x4539` | D58x only |

### 3.6 Not ours

The mux subdev registers **zero** controls. Every node additionally carries ~15 controls from
the Tegra VI channel (`bypass_mode`, `override_enable`, `height_align`, `size_align`,
`write_isp_format`, `sensor_*_properties`, `sensor_modes`, `low_latency_mode`,
`preferred_stride`, `override_capture_timeout_ms`, and three `tegracam` entries that always
fail with `EINVAL`). None of these are camera-owned and none are in scope.

## 4 · Per-camera matrix

Counts are d4xx-registered controls only, excluding the VI/tegracam block. Verified by
enumeration on fw-orin-5.

| Node | D4xx (D457) | D58x (D585) | Δ |
|---|---|---|---|
| Depth | **26** | **36** | D58x: +11 (device mode, 2C policy, preset, 3 temps, MinZ, decimation, temporal, aligned depth, detection distance) / −1 (readout shaping) |
| IR | **22** | **22** | identical set; `manual_laser_power` range differs |
| RGB | **26** | **30** | D58x: +brightness, contrast, gamma, hue; all six shared ISP controls at different addresses |
| IMU | **1** | **3** | D58x: +gyro, +accel |
| Mux | 0 | 0 | — |

Differences that are **not** presence, read live from the two cameras:

| Control | D457 | D585 |
|---|---|---|
| `manual_laser_power` | `min=0 max=360 step=30 default=150` | `min=0 max=540 step=45 default=225` |
| `analogue_gain` (RGB) | `min=0 max=128 default=64` | `min=16 max=248 default=16` |
| `gvd` | `dims=[276]` | `dims=[606]` |
| `camera_sync_mode` menu | `0: Default · 1: Master · 2: External Sync` | `0: Default · 2: External Sync` |
| `power_line_frequency` menu | `0..3`, incl. Auto | `0..2` |
| `visual_preset` | not registered | `0..5` |
| `readout_shaping` | `0..100` | not registered |

The remaining SKUs (D40x, D41x, D43x) are not on this rig. From the source, the only
per-model divergence below D45x/D58x granularity today is `ae_priority` being `DISABLED` on
D40x. **That is the whole of the per-model control variation the driver currently models** —
which is itself a finding: the driver distinguishes six device types for *formats* but only
three (`D40X`, other D4xx, `D58X`) for *controls*.

## 5 · What actually varies — the descriptor's real job

Six kinds of variation, in rough order of how much code each one costs today:

1. **Presence.** 18 of 49 controls are SKU-conditional. Today: `if (is_d58x)` branches in
   `ds5_ctrl_init()`.
2. **Range / step / default.** Same CID, different numbers. Today: duplicate
   `v4l2_ctrl_config` structs (`ds5_ctrl_manual_laser_power` vs `…_d58x`) or duplicate
   `v4l2_ctrl_new_std()` call sites.
3. **Menu composition.** Same enum values — they are FW wire values and must never be
   renumbered — with entries removed per SKU. Today: `menu_skip_mask`, applied through the
   `step` argument of `__v4l2_ctrl_modify_range()`.
4. **Register address.** Same control, different offset *and different stride* between D4xx
   and D58x. Today: `ds5_rgb_ctrl_offset()` plus two parallel `#define` blocks.
5. **Payload size.** `gvd` is 276 or 606 bytes. Today: a mutable copy of the
   `v4l2_ctrl_config` patched before registration.
6. **Availability at runtime.** FW-version gates and device-state gates, both of which can
   change *after* probe (a DFU refreshes `fw_version` without re-running `ds5_ctrl_init()`).
   Today: checked inside `ds5_s_ctrl()`/`ds5_g_volatile_ctrl()` and reported as an errno.

Items 1–5 are static per camera-and-FW and are exactly what a descriptor describes. **Item 6
is not** — and the existing convention is deliberate: a control stays registered and reports
`-EOPNOTSUPP`/`-EBUSY`, because hiding it at probe would go stale after a DFU or HW reset.
A control descriptor must preserve that, which means **a descriptor reload after reset is
mandatory, not optional** — the same `ds5_desc_reload_after_reset()` path the format tables
already use.

## 6 · What cannot move

Mirroring §10 of the format note, because it bounds the payoff the same way.

- **The CID number.** librealsense hardcodes `RS_CAMERA_CID_* = RS_CAMERA_CID_BASE + N`. A
  camera cannot invent a CID; the driver needs a `control_id → CID` registry exactly parallel
  to the format descriptor's `pixfmt_id → mbus_code` map, growing only when a genuinely new
  control appears. Same for `V4L2_CID_*` standard controls, which are Linux constants.
- **Transport semantics.** Readback-verify, `0xFF`-means-absent, read-to-clear, lock
  ordering, the `-EBUSY`/`-EOPNOTSUPP`/`-EIO`/`-EBADMSG` tables — all of this is `ds5_s_ctrl()`
  logic. A descriptor can say *which* transport, never *how* it behaves.
- **Heterogeneous array validation.** The per-slot min/max/step tables for MinZ/decimation/
  temporal could be descriptor data, but the `v4l2_ctrl_type_ops` that consume them cannot.
- **Host-owned facts.** Which node a control is hung on is a host decision (and one
  RSDEV-14562 is about to change). So is anything derived from the SerDes or link mode.
- **The SDK's `xu_to_cid()` map.** A separate repo and a separate change.

## 7 · Gaps the inventory exposes

Worth logging regardless of the descriptor, because they are defects the enumeration found:

- **`visual_preset` is D58x-only on MIPI.** librealsense maps `RS_HARDWARE_PRESET →
  RS_CAMERA_CID_PRESET` (`BASE+21`) unconditionally, so the selector resolves to a CID a
  D457 GMSL node does not register. The register (`0x4120`) is a D4xx register.
- **Registered-but-unserviceable reads.** On the D585, `logger` fails `ENOBUFS(105)`,
  `get_depth_calib`/`get_coeff_calib` fail `ENOENT(2)`, and `gvd` intermittently fails
  `EIO(5)` on the depth node while succeeding on IR and RGB. On the D457, `error_code` fails
  `EOPNOTSUPP(95)` — that one is by design (FW < 5.17.6). `hwmc_rw` fails `ENODATA(61)` on
  both, which is the expected answer to a read with no pending response. **These are exactly
  the cases a camera-declared control list would eliminate**, and they are the strongest
  single argument for the feature.
- **The SDK reaches 9 of 49.** `xu_to_cid()` covers nine selectors and only
  `xu.subdevice == 0`, with four `TBD` comments still in the source. Everything D5xx-specific
  is unreachable through a selector today.
- **Split get/set pairs are a legacy shape.** Four settings (AE ROI, AE setpoint, depth
  calib, coeff calib) burn eight CIDs and cannot be reached by a selector at all.
- **CID space is being consumed unevenly.** `BASE+17..20`, `+23`, `+28..31` are claimed by
  librealsense but unused by the driver; `+41..43` are reserved; `+45..48`, `+50`, `+52..54`
  are skipped. A descriptor does not fix this, but a design should not pretend the space is
  dense.

## 8 · Open design questions

1. **Scope encoding.** `camera` vs `sensor:<stream_id>`, not "node" — see §2.4. Needs to be
   agreed with RSDEV-14562 before the record layout is fixed.
2. **How much of §2.1 does a record carry?** Encoding only *presence + range + menu mask*
   (items 1–3 and 5 of §5) is a small record and removes most of the `is_d58x` branching.
   Adding the register address (item 4) removes the parallel `#define` blocks but puts an
   address on the wire, which the format descriptor deliberately avoided for data types.
3. **Does the control table go in the existing blob or a second aperture?** The format blob
   is 334 B against a 2 KB aperture; a 49-entry control table at ~8 B/record is ~400 B plus
   menu strings. Reusing `RS_DESC_BASE` with new `RS_DESC_T_*` table types is the cheap
   answer and keeps one CRC, one version, one reload path.
4. **Menu strings: wire or driver?** Carrying them costs the most bytes of anything here.
   Carrying only the *mask* against a driver-side string table costs ~2 B and keeps the
   strings in one reviewable place — at the price of a driver change for a new menu entry.
5. **Legacy FW.** Every shipping D4xx predates this, so the hardcoded tables stay behind the
   fallback indefinitely. As with formats, the honest short-term accounting is that this adds
   code before it removes any.
6. **Does the SDK consume it?** If the camera declares its controls, the SDK's hardcoded
   `xu_to_cid()` is the next hardcoded table down the chain. Out of scope for this ticket,
   but the wire format should not make it impossible.

---

*Design note · control set and per-SKU differences enumerated from `kernel/realsense/d4xx.c`
at `dev` 7cbf944 and verified live on fw-orin-5 (D585 9-001a, D457 9-002a, d4xx 1.0.7.9)*
