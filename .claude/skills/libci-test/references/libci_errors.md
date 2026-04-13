# libCI Error Catalog

Error patterns found in libCI test output and correlated dmesg entries, organized by category.

## Test Output Patterns

### I2C / Communication Errors

| Pattern | Meaning | Severity |
|---------|---------|----------|
| `errno=121 Last Error: Remote I/O error` | I2C transfer failed — camera unreachable | High |
| `xioctl(VIDIOC_S_EXT_CTRLS) failed` | V4L2 extended control write failed | High |
| `xioctl(VIDIOC_STREAMON) failed` | Could not start stream | High |
| `xioctl(VIDIOC_DQBUF) failed` | Dequeue buffer failed — no frames | High |

### Streaming / FPS Errors

| Pattern | Meaning | Severity |
|---------|---------|----------|
| `Expected N FPS, got M FPS (deviation: X%)` | FPS out of tolerance (>5%) | Medium |
| `No frames received` | Stream started but produced no data | High |
| `Timeout waiting for frames` | Frame delivery stalled | High |
| `PASSED` with deviation | Normal — deviation within tolerance | Info |

### Firmware Errors

| Pattern | Meaning | Severity |
|---------|---------|----------|
| `Firmware Update failed - wrong path or permissions` | FW update path issue (common on MIPI/GMSL) | Medium |
| `left : X.Y.Z.W` / `right : A.B.C.D` | FW version mismatch after update attempt | Medium |
| `DFU` errors | Device Firmware Update failures | Medium |

### Calibration Errors

| Pattern | Meaning | Severity |
|---------|---------|----------|
| `Not enough depth pixels! - low fill factor` | OCC calibration needs better scene/lighting | Low |
| `Calibration failed` | On-chip calibration error | Low |

### Framework / Pipeline Errors

| Pattern | Meaning | Severity |
|---------|---------|----------|
| `Pipeline start failed` | librealsense pipeline could not start | High |
| `Record/playback` errors | File I/O or format issues | Medium |
| `enumerate-devices` failures | Device enumeration problem | High |

## dmesg Patterns

### D4XX Driver

| Pattern | Meaning | Action |
|---------|---------|--------|
| `ds5_write(): i2c write retry N` | I2C write needed retries | Check if transient or persistent |
| `ds5_read(): i2c read failed` | I2C read failure after all retries | Check SerDes link |
| `ds5_hw_reset_with_recovery()` | HW reset triggered by libRS | Normal during tests that use hw_reset |
| `Device ready after N ms` | Post-reset recovery timing | Normal, note duration |
| `GMSL link recovered naturally` | Link came back without SerDes reset | Good — fast recovery |
| `SERDES intervention needed` | Had to reset SerDes to recover link | Slower recovery path |
| `configured DEPTH/RGB/IR/IMU stream` | Stream configuration log | Normal |

### SerDes (MAX9295/MAX9296)

| Pattern | Meaning | Action |
|---------|---------|--------|
| `max9296` with `error` or `fail` | Deserializer communication error | Check GMSL cable/connection |
| `max9295` with `error` or `fail` | Serializer communication error | Check camera module |
| `ERRB` or `ERR` in serdes context | GMSL error flag asserted | Check link integrity |

### Tegra Capture VI

| Pattern | Meaning | Action |
|---------|---------|--------|
| `corr_err: discarding frame 0` | First frame discard (normal) | Expected on stream start |
| `tegra-capture-vi` with `error` or `timeout` | VI capture engine error | Check CSI/stream config |
| `uncorr_err` | Uncorrectable capture error | Investigate CSI timing |

### System-Level

| Pattern | Meaning | Action |
|---------|---------|--------|
| `BUG:` or `Oops:` | Kernel bug/crash | Critical — save full dmesg |
| `Call trace:` | Kernel stack trace | Critical if in d4xx/vi/serdes |
| `OOM` or `out of memory` | Memory exhaustion | Check for buffer leaks |

## Known Benign Patterns

These patterns appear in libCI output but are NOT failures:

- `no device matches configuration "D555"` — D555 not connected, skip expected
- `no device matches configuration "D455"` — D455 not connected, skip expected
- `This test is dedicated to run with D405 only` — D405-specific test, skip on D457
- `stepping over test` — Test self-skipped for this device type
- `corr_err: discarding frame 0, flags: 0, err_data 131072` — Normal first-frame discard
- `Firmware Update failed - wrong path or permissions` — Expected on GMSL/MIPI (no USB DFU path)
