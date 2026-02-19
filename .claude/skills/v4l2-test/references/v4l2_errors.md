# V4L2 Error Reference

Common V4L2 and media framework errors encountered during D4XX camera testing.

## Table of Contents
- [Stream Start/Stop Errors](#stream-startstop-errors)
- [Format Negotiation Errors](#format-negotiation-errors)
- [Control Errors](#control-errors)
- [Device Detection Errors](#device-detection-errors)
- [Dmesg Error Patterns](#dmesg-error-patterns)

---

## Stream Start/Stop Errors

### VIDIOC_STREAMON failures

**Error**: `VIDIOC_STREAMON failed: errno 22 (Invalid argument)`
- **Cause**: Format not set or incompatible format/buffer type
- **Check**: Verify format is set with VIDIOC_S_FMT before streaming
- **Check**: Ensure buffer type matches (VIDEO_CAPTURE vs META_CAPTURE)

**Error**: `VIDIOC_STREAMON failed: errno 5 (I/O error)`
- **Cause**: Hardware/driver issue, likely MIPI CSI-2 or SerDes problem
- **Check dmesg for**: `tegra-camrtc`, `nvcsi`, `d4xx`, `max929*` errors
- **Action**: Check camera power, I2C communication, SerDes link

**Error**: `VIDIOC_STREAMON failed: errno 16 (Device or resource busy)`
- **Cause**: Another process streaming from same device
- **Action**: Check for other applications accessing /dev/video*, close them

### VIDIOC_STREAMOFF failures

**Error**: `VIDIOC_STREAMOFF timeout`
- **Cause**: Driver hung, pending DMA transactions
- **Check dmesg for**: VI capture engine errors, timeout messages
- **Action**: May require device reset or reboot

### VIDIOC_DQBUF (dequeue buffer) failures

**Error**: `VIDIOC_DQBUF failed: errno 11 (Resource temporarily unavailable)`
- **Cause**: No frames available (non-blocking mode)
- **Normal**: Expected in non-blocking mode when no frame ready

**Error**: `VIDIOC_DQBUF failed: errno 5 (I/O error)`
- **Cause**: Frame capture error, corrupted frame, or stream interrupted
- **Check dmesg for**: Frame errors, CSI errors, overflow messages
- **Action**: May indicate unreliable GMSL link or SerDes issue

**Error**: `VIDIOC_DQBUF failed: errno 19 (No such device)`
- **Cause**: Device unplugged or driver crashed
- **Check dmesg for**: Driver oops, kernel panic, device removal
- **Action**: Check dmesg, verify camera still detected, may need reboot

---

## Format Negotiation Errors

### VIDIOC_S_FMT failures

**Error**: `VIDIOC_S_FMT failed: errno 22 (Invalid argument)`
- **Cause**: Unsupported resolution, pixelformat, or framerate
- **Action**: Query supported formats with VIDIOC_ENUM_FMT, VIDIOC_ENUM_FRAMESIZES
- **D4XX specific**: Check camera model, not all models support all resolutions

**Error**: `VIDIOC_S_FMT succeeded but returned different format`
- **Behavior**: Driver adjusted to nearest supported format
- **Action**: Check returned format, validate it matches expectations

### VIDIOC_ENUM_FMT failures

**Error**: `VIDIOC_ENUM_FMT failed: errno 22 (Invalid argument)`
- **Cause**: Index out of range (normal, indicates end of format list)
- **Normal**: Expected behavior when enumerating all formats

---

## Control Errors

### VIDIOC_S_CTRL / VIDIOC_S_EXT_CTRLS failures

**Error**: `VIDIOC_S_CTRL failed: errno 22 (Invalid argument)`
- **Cause**: Invalid control ID or value out of range
- **Action**: Query control with VIDIOC_QUERY_CTRL to get valid range
- **D4XX specific**: Some controls are camera-model specific (e.g., laser power on D457 but not D401)

**Error**: `VIDIOC_S_CTRL failed: errno 16 (Device or resource busy)`
- **Cause**: Cannot change control while streaming (for some controls)
- **Action**: Stop stream, set control, restart stream

**Error**: `VIDIOC_S_CTRL failed: errno 5 (I/O error)`
- **Cause**: I2C communication failure with camera
- **Check dmesg for**: I2C errors, D4XX driver errors
- **Action**: Check camera connection, I2C bus health

### VIDIOC_G_CTRL / VIDIOC_G_EXT_CTRLS failures

**Error**: `VIDIOC_G_CTRL failed: errno 22 (Invalid argument)`
- **Cause**: Invalid control ID
- **Action**: Verify control is supported by this camera model

---

## Device Detection Errors

### No video devices

**Symptom**: `/dev/video*` devices missing
- **Check**: `ls /dev/video*`
- **Check dmesg for**: D4XX driver probe failures
- **Causes**:
  - Driver not loaded: `lsmod | grep d4xx`
  - Device tree not loaded: Check DTB/DTBO
  - Camera not detected on I2C: I2C communication failure
  - SerDes link down: MAX9295/MAX9296 not configured

### Wrong number of video devices

**Expected**: 6 video devices per D4XX camera (Depth, Depth-meta, RGB, RGB-meta, IR, IMU)
- **Check**: `v4l2-ctl --list-devices`
- **If fewer devices**: Partial driver initialization, check dmesg

### Device permissions

**Error**: `open /dev/video0: Permission denied`
- **Cause**: User not in video group or incorrect permissions
- **Action**: `sudo chmod 666 /dev/video*` or `sudo usermod -aG video $USER`

---

## Dmesg Error Patterns

### D4XX driver errors

```
d4xx: probe failed: -19
```
- No device on I2C bus, camera not detected

```
d4xx: I2C read failed
d4xx: I2C write failed
```
- I2C communication failure, check SerDes, cables, power

```
d4xx: firmware version mismatch
```
- Unsupported firmware version, may need driver update

```
d4xx: streaming start failed
```
- Generic streaming failure, check MIPI CSI-2 configuration

### NVCSI (Tegra CSI) errors

```
nvcsi: csi port is not streaming
```
- No data on MIPI CSI-2 lanes, check SerDes link

```
nvcsi: uncorrectable error detected
```
- MIPI CSI-2 protocol error, corrupted data

```
tegra-camrtc: timeout waiting for response
```
- Camera real-time controller timeout, serious issue

### VI (Video Input) errors

```
vi5: timeout after 2500ms
vi5: frame capture timeout
```
- No frames arriving from sensor, check entire pipeline

```
vi5: buffer overflow
```
- CPU not dequeuing buffers fast enough

### SerDes (MAX9295/MAX9296) errors

```
max9295: link not locked
max9296: link not locked
```
- GMSL link not established, check cables, power, SerDes configuration

```
max9296: I2C transaction failed
```
- Cannot communicate with deserializer, critical issue

### General patterns

**Frequency of errors**:
- Single error: May be transient, retry might succeed
- Repeated errors: Persistent issue, investigate root cause
- Errors increasing over time: Hardware degradation, overheating, or resource leak

**Error correlation**:
- D4XX + NVCSI errors: MIPI CSI-2 level issue
- D4XX + SerDes errors: GMSL link issue
- VI + NVCSI errors: Tegra capture pipeline issue
- Multiple subsystem errors: Systemic issue, power, clocking, or device tree

---

## Diagnostic Workflow

When analyzing V4L2 test failures:

1. **Identify the failing ioctl**: STREAMON, DQBUF, S_FMT, etc.
2. **Check errno**: Maps to specific error condition
3. **Correlate with dmesg**: Look for driver/hardware errors at same time
4. **Identify subsystem**: D4XX driver, V4L2 core, NVCSI, VI, SerDes
5. **Pattern recognition**: Single failure vs. repeated, specific tests vs. all tests
6. **Hypothesize root cause**: Based on error patterns and affected subsystems
7. **Recommend action**: Fix, workaround, or further investigation needed
