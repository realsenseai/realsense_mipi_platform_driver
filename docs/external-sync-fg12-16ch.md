# External Sync for fg12-16ch Board

## Overview

External sync replaces the D4XX camera's internal frame clock with a shared external trigger signal, synchronizing frame capture across multiple cameras. There are two options for providing the sync signal on the fg12-16ch board:

1. **Option A — Orin TSC via MFP2** (per-deserializer): Use the Tegra234 TSC signal generators to produce a hardware FSYNC pulse routed to each MAX96712's MFP2 pin individually.
2. **Option B — External signal generator via MFP6** (shared): Connect an external signal generator to the `CAM_SYNCALL` pin (PCC.00 / `spi2_sck_pcc0`), which is wired to all MAX96712 MFP6 inputs simultaneously.

The MAX96712 driver configures ESYNC tunneling on **both** MFP2 and MFP6 at probe time, so both paths are always active. The cameras will sync to whichever signal is present.

## Signal Paths

### Option A: TSC → MFP2 (per-deserializer)

```
Orin TSC signal generator (hardwired to specific pin)
    ↓ pinmux routes pin to "tsc" function
MAX96712 MFP2 input pin (one per deserializer)
    ↓ ESYNC register tunneling (configured by driver at probe)
MAX9295 serializer → D4XX camera FSYNC input
```

Each MAX96712 deserializer on the fg12-16ch board has its own MFP2 input, connected to a dedicated Orin GPIO pin. The TSC generators are hardwired in silicon to specific output pins — you cannot remap them.

### Option B: External signal generator → MFP6 (shared)

```
External signal generator
    ↓ wire to CAM_SYNCALL (PCC.00 / spi2_sck_pcc0)
MAX96712 MFP6 input pin (shared across all deserializers)
    ↓ ESYNC register tunneling (configured by driver at probe)
MAX9295 serializer → D4XX camera FSYNC input
```

The `CAM_SYNCALL` pin does not have TSC routing in the Orin SoC, so it cannot be driven by a TSC generator. It is configured as a high-Z input with pull-down and must be driven by an external source.

## Hardware Pin Mapping

| Deserializer | MFP2 Pin | Orin GPIO | Orin Pad Name | TSC Generator Offset |
|---|---|---|---|---|
| MAX96712_1 | MFP2 | PBB.2 | `soc_gpio50_pbb2` | `generator@380` |
| MAX96712_2 | MFP2 | PAA.7 | `can0_err_paa7` | `generator@480` |
| MAX96712_3 | MFP2 | PCC.2 | `spi2_mosi_pcc2` | **No TSC routing** (see note) |
| MAX96712_4 | MFP2 | PBB.3 | `can1_err_pbb3` | `generator@500` |

**Note:** MAX96712_3's MFP2 pin (PCC.2) does not have TSC routing in the Orin SoC. To sync cameras on deserializer 3, use a jumper cable from one of the other TSC-driven pins to PCC.2.

## Supported Overlay Variants

| Overlay DTS | Deserializers with TSC (MFP2) | Generators | MFP6 input |
|---|---|---|---|
| `tegra234-camera-d4xx-overlay-fg12-16ch-cams-0-1.dts` | 1 | `@380` | Yes |
| `tegra234-camera-d4xx-overlay-fg12-16ch-cams-0-1-2-3.dts` | 1 | `@380` | Yes |
| `tegra234-camera-d4xx-overlay-fg12-16ch-cams-0-4.dts` | 1, 2 | `@380`, `@480` | Yes |
| `tegra234-camera-d4xx-overlay-fg12-16ch-cams-0-4.calib.dts` | 1, 2 | `@380`, `@480` | Yes |
| `tegra234-camera-d4xx-overlay-fg12-16ch-cams-0-4-8-12.dts` | 1, 2, 4 | `@380`, `@480`, `@500` | Yes |
| `tegra234-camera-d4xx-overlay-fg12-16ch-cams-0-4-8-12.calib.dts` | 1, 2, 4 | `@380`, `@480`, `@500` | Yes |
| `tegra234-camera-d4xx-overlay-fg12-16ch.calib.dts` | 1 | `@380` | Yes |
| `tegra234-camera-d4xx-overlay-fg12-16ch-PWR-only.dts` | 1, 2, 4 | `@380`, `@480`, `@500` | Yes |

All overlays configure:
- Pinmux to route TSC-capable GPIO pads to the `tsc` function
- `CAM_SYNCALL` (PCC.00) as high-Z input with pull-down for MFP6 external signal
- MAX96712_3's MFP2 pin (PCC.02) as high-Z input with pull-down (no TSC routing available)

## Prerequisites

1. **Kernel patches applied:** The `0010-Runtime-tsc-rate-config.patch` must be applied for your JetPack version (6.0, 6.1, 6.2, 7.0, or 7.1). This patch adds the `CDI_TSC_SET_RATE` ioctl to the `cam_cdi_tsc` driver.

2. **Device tree overlay loaded:** One of the fg12-16ch overlays listed above must be active. The overlay configures:
   - TSC signal generators (frequency, duty cycle, GPIO pinmux references)
   - Pin multiplexing to route pads to TSC function
   - Power regulators and PWDN GPIOs

3. **MAX96712 MFP2 ESYNC tunneling:** Configured automatically by the max96712 driver at probe time (registers `0x306`–`0x3AA`). No user action needed.

## Using ext_sync_gen.py

The script is deployed to the Jetson at `kernel_mod/<version>/ext_sync_gen.py` or can be found in the repo at `scripts/ext_sync_gen.py`.

### Start sync at default rate (30 Hz, 25% duty cycle)

```bash
python3 ext_sync_gen.py --enable
```

### Start sync at custom rate

```bash
python3 ext_sync_gen.py --enable --fps 60 --duty 50
```

### Stop sync

```bash
python3 ext_sync_gen.py --disable
```

### Options

| Flag | Description | Range | Default |
|---|---|---|---|
| `--enable` | Start all TSC generators | — | — |
| `--disable` | Stop all TSC generators | — | — |
| `--fps` | Signal frequency in Hz | 1–120 | 30 |
| `--duty` | Duty cycle in percent | 1–99 | 25 |

When `--fps` or `--duty` is provided with `--enable`, the script first sends a `CDI_TSC_SET_RATE` ioctl to update all generators, then starts them. The rate change takes effect on the next start.

## Workflow

1. Boot the Jetson with the appropriate DT overlay loaded.
2. Enable external sync - External generator to MFP6 or TSC to MFP2 using:
   ```bash
   python3 ext_sync_gen.py --enable --fps 30
   ```
3. Set cameras to inter cam sync mode (mode 3 for D457 cameras and mode 2 for D401) camera streaming .
4. Start streaming
5. Cameras are now frame-synced to the external signal.
6. To stop external sync and revert to internal timing return cameras to inter cam sync mode 0
7. Stop TSC generator using:
   ```bash
   python3 ext_sync_gen.py --disable
   ```

## Troubleshooting

### `/dev/cdi_tsc` not found
- The `cam_cdi_tsc` module is not loaded or the device tree does not enable the `tsc_sig_gen@c6a0000` node.
- Check: `ls /dev/cdi_tsc` and `dmesg | grep cdi_tsc` for errors

### No PWM signal on a specific pin
- TSC generator offsets are **hardwired to pins** in silicon. You cannot drive an arbitrary pin from an arbitrary generator. Verify the correct generator offset is used for your target deserializer (see Hardware Pin Mapping table above).
- Check `dmesg | grep cdi_tsc` to see whether generators are being started.

## JetPack Version Support

| JetPack | Patch File | Status |
|---|---|---|
| 6.0 | `nvidia-oot/6.0/0010-Runtime-tsc-rate-config.patch` | Supported |
| 6.1 | `nvidia-oot/6.1/0010-Runtime-tsc-rate-config.patch` | Supported |
| 6.2 | `nvidia-oot/6.2/0010-Runtime-tsc-rate-config.patch` | Supported |
| 7.0 | `nvidia-oot/7.0/0010-Runtime-tsc-rate-config.patch` | Supported |
| 7.1 | `nvidia-oot/7.1/0010-Runtime-tsc-rate-config.patch` | Supported |
