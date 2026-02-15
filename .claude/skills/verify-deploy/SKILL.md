---
name: verify-deploy
description: Verify a deployed RealSense MIPI platform driver on a Jetson device. Use when the user wants to check if deployment succeeded, verify the driver is working, or test camera functionality. Triggers on requests mentioning verify deployment, check deployment, test driver, verify camera, or deployment status.
---

# Verify Deploy Skill

## Overview

This skill verifies that the D4XX driver was successfully deployed to a Jetson device by checking:
1. Kernel version and boot status
2. Driver probe messages in dmesg
3. Video device creation
4. V4L2 device enumeration
5. Supported formats
6. Streaming capability

## Prerequisites

- A Jetson device with deployed D4XX driver
- SSH access to the Jetson
- The device should have completed reboot after deployment

## Verification Workflow

### Step 1: Check Connectivity and Kernel Version

First, verify the Jetson is reachable and check the kernel version:

```bash
ssh <USERNAME>@<TARGET> "uname -a"
```

Expected output should show:
- `5.15.x-tegra` for JetPack 6.x
- `5.10.x-tegra` for JetPack 5.x
- `4.9.x-tegra` for JetPack 4.6.1

The build date should match your deployment date.

### Step 2: Check Driver Probe Status

Verify the d4xx driver loaded successfully:

```bash
ssh <USERNAME>@<TARGET> "sudo dmesg | grep d4xx"
```

Expected output:
- `d4xx X-001a: Probing driver for D4xx` messages for each sensor
- `D4XX Sensor: DEPTH/RGB/Y8/IMU` messages showing sensor types
- Firmware version (e.g., `firmware build: 5.17.2.2`)
- Sync mode configuration messages
- **No error messages** (errors indicate probe failure)

For a single D4XX camera, expect 4 probe messages (DEPTH, RGB, IR, IMU subdevices).

### Step 3: Verify Video Devices

Check that video devices were created:

```bash
ssh <USERNAME>@<TARGET> "ls -la /dev/video*"
```

Expected devices per camera:
- video0: Depth (Z16)
- video1: Depth metadata
- video2: RGB (RGB888/YUV422)
- video3: RGB metadata
- video4: IR (GREY, Y8I, Y12I)
- video5: IMU
- (Optional) video6: Additional metadata device

**6-7 video devices** should be present for a single camera.

### Step 4: List V4L2 Devices

Verify V4L2 device enumeration:

```bash
ssh <USERNAME>@<TARGET> "v4l2-ctl --list-devices"
```

Expected output:
- `NVIDIA Tegra Video Input Device` with `/dev/media0`
- `vi-output, DS5 mux X-001a` with all video devices listed
- All video devices should be associated with the D4XX camera

### Step 5: Check Supported Formats

Query supported formats for the depth sensor:

```bash
ssh <USERNAME>@<TARGET> "v4l2-ctl -d /dev/video0 --list-formats-ext"
```

Expected output:
- Format: `Z16` (16-bit Depth)
- Multiple resolutions: 1280x720, 848x480, 640x480, 640x360, etc.
- Multiple frame rates: 5, 15, 30, 60, 90 fps (depending on resolution)

### Step 6: Test Streaming

Test that the camera can actually capture frames:

Streaming depth frames from video0:
```bash
ssh <USERNAME>@<TARGET> "timeout 15 v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=30"
```

wait for 3 seconds between each stream test to avoid overwhelming the device.

Streaming color frames from video2:
```bash
ssh <USERNAME>@<TARGET> "timeout 15 v4l2-ctl -d /dev/video2 --stream-mmap --stream-count=30"
```

Streaming IR frames from video4:
```bash
ssh <USERNAME>@<TARGET> "timeout 15 v4l2-ctl -d /dev/video4 --stream-mmap --stream-count=30"
```

Expected output:
- 30 `<` characters (one per captured frame)
- No timeout or error messages

Success indicates:
- Driver is functional
- Camera is responding
- MIPI CSI-2 interface is working
- Frame capture pipeline is operational

## Verification Summary

A successful deployment should show:

| Component | Status Check | Expected Result |
|-----------|--------------|-----------------|
| Kernel | `uname -a` | Correct version with recent build date |
| Driver probe | `dmesg \| grep d4xx` | 4 probe messages, no errors |
| Video devices | `ls /dev/video*` | 6-7 devices present |
| V4L2 enumeration | `v4l2-ctl --list-devices` | All devices associated with D4XX |
| Format support | `v4l2-ctl --list-formats-ext` | Z16 format with multiple resolutions |
| Streaming | `v4l2-ctl --stream-mmap` | Successfully captures frames |

## Common Issues

### Driver Not Loading
**Symptoms**: No dmesg messages, no video devices
**Causes**:
- Device tree overlay not applied
- Kernel/driver version mismatch
- I2C bus issues

**Debug**:
```bash
sudo dmesg | grep -i "d4xx\|camera\|i2c"
ls /boot/tegra234-camera-d4xx-overlay*.dtbo  # Check overlay exists
```

### Video Devices Missing
**Symptoms**: Some or all video devices not created
**Causes**:
- Partial driver initialization
- Sensor subdevice probe failure

**Debug**:
```bash
sudo dmesg | grep -i error
media-ctl -p  # Check media controller topology
```

### Streaming Fails
**Symptoms**: v4l2-ctl timeout or VIDIOC errors
**Causes**:
- MIPI CSI-2 configuration issues
- Sensor not responding
- SerDes link problems

**Debug**:
```bash
sudo dmesg | tail -50  # Check recent kernel messages
v4l2-ctl -d /dev/video0 --all  # Check device controls and capabilities
```

### Wrong Kernel Version
**Symptoms**: Old kernel version in `uname -a`
**Causes**:
- Deploy script didn't update kernel
- Wrong boot partition selected
- Reboot didn't complete properly

**Fix**:
```bash
sudo reboot  # Try rebooting again
# Check extlinux.conf boot configuration
cat /boot/extlinux/extlinux.conf
```

## Quick Verification Script

For automated verification, all checks can be combined:

```bash
TARGET=<hostname>
USER=<username>

echo "=== Kernel Version ==="
ssh $USER@$TARGET "uname -a"

echo -e "\n=== Driver Probe ==="
ssh $USER@$TARGET "sudo dmesg | grep d4xx | tail -20"

echo -e "\n=== Video Devices ==="
ssh $USER@$TARGET "ls -l /dev/video*"

echo -e "\n=== V4L2 Devices ==="
ssh $USER@$TARGET "v4l2-ctl --list-devices | head -20"

echo -e "\n=== Streaming Test ==="
ssh $USER@$TARGET "timeout 10 v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=5"
```

## Integration with Deploy Workflow

This skill complements the `/deploy` skill:
1. User runs `/deploy` to deploy the driver
2. Wait 2-5 minutes for reboot
3. User runs `/verify-deploy` to confirm deployment succeeded

The verification can be automated immediately after deploy completes, or run manually at any time to check driver status.
