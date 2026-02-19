# V4L2 Test Structure

Overview of the v4l2_test directory organization and test categories.

## Directory Structure

```
test/v4l2_test/
├── conftest.py              # Pytest fixtures (camera discovery, device fixtures)
├── pytest.ini               # Pytest configuration
├── report.py                # Custom test report plugin
├── d4xx/                    # D4XX camera-specific utilities
│   ├── constants.py         # Device paths, pixel formats, control IDs
│   ├── discovery.py         # Camera detection logic
│   └── controls.py          # D4XX control helpers
├── v4l2/                    # V4L2 API wrappers
│   ├── device.py            # V4L2Device class (open, ioctl wrappers)
│   ├── ioctls.py            # V4L2 ioctl definitions and constants
│   ├── stream.py            # StreamContext for capture
│   └── buffer.py            # Buffer management
└── tests/                   # Test modules
    ├── test_discovery.py    # Camera detection tests
    ├── test_streaming.py    # Streaming tests (FPS, resolutions)
    ├── test_controls.py     # V4L2 control tests (get/set)
    ├── test_metadata.py     # Metadata capture tests
    └── test_error_handling.py  # Error condition tests
```

## Test Categories

### test_discovery.py
**Purpose**: Verify camera detection and device enumeration

**Tests**:
- `test_camera_detected`: At least one D4XX camera found
- `test_video_device_count`: Correct number of video devices (6 per camera)
- `test_device_capabilities`: Each device reports expected capabilities
- `test_media_controller`: Media controller topology correct

**Typical failures**:
- Driver not loaded
- Device tree not applied
- Camera not powered or not on I2C bus

### test_streaming.py
**Purpose**: Validate streaming at various resolutions/formats/framerates

**Tests**:
- `test_depth_streaming[config]`: Parametrized for multiple depth resolutions
- `test_ir_streaming[config]`: IR stream tests
- `test_rgb_streaming[config]`: RGB stream tests
- `test_multiple_stream`: Stream depth + IR simultaneously
- `test_frame_sequence`: Validate sequence numbers increment correctly
- `test_fps_accuracy`: Actual FPS matches requested FPS (within tolerance)

**Validates**:
- Frame arrival rate (must achieve >= 90% of requested FPS)
- Sequence number continuity (detect dropped frames)
- Frame data integrity (non-zero data, expected size)

**Typical failures**:
- STREAMON fails: Format/hardware issue
- Low FPS: CPU overload, GMSL bandwidth, driver issue
- Dropped frames: Buffer starvation, MIPI errors
- Sequence gaps: Hardware frame drops

### test_controls.py
**Purpose**: Test V4L2 control interface (get/set camera parameters)

**Tests**:
- `test_exposure_control`: Get/set exposure time
- `test_gain_control`: Get/set analog/digital gain
- `test_laser_power_control`: Laser power control (D457 only)
- `test_ae_roi_control`: Auto-exposure ROI
- `test_control_persistence`: Controls persist across stream start/stop
- `test_control_ranges`: Values clamped to min/max
- `test_invalid_control`: Setting invalid control ID fails gracefully

**Typical failures**:
- ENOTTY: Control not supported by this camera model
- I/O error: I2C communication failure
- Control has no effect: Firmware bug

### test_metadata.py
**Purpose**: Validate metadata capture (frame timestamps, sensor parameters)

**Tests**:
- `test_metadata_device_exists`: Metadata video device present
- `test_metadata_format`: Metadata format correct (D4XX_META_DATA)
- `test_metadata_streaming`: Can stream metadata
- `test_metadata_sync`: Metadata sequence matches video frame sequence
- `test_metadata_content`: Metadata contains expected fields (timestamp, exposure, etc.)

**Typical failures**:
- No metadata device: Driver config issue
- Metadata out of sync: Timing bug in driver
- Malformed metadata: Firmware/driver mismatch

### test_error_handling.py
**Purpose**: Verify graceful error handling in fault conditions

**Tests**:
- `test_stream_without_format`: STREAMON without prior S_FMT should fail
- `test_invalid_format`: S_FMT with unsupported format fails cleanly
- `test_double_streamon`: Second STREAMON should fail or be idempotent
- `test_dqbuf_before_qbuf`: DQBUF without QBUF should fail
- `test_close_while_streaming`: Clean shutdown if device closed during stream

**Typical failures**:
- Driver crash: Kernel oops in dmesg
- Hang: Driver doesn't handle edge case, becomes unresponsive
- Resource leak: Stream not properly cleaned up

## Pytest Configuration

### pytest.ini
```ini
[pytest]
markers =
    d457: Tests for D457 camera
    streaming: Streaming tests (may take longer)
    controls: Control interface tests
```

### Fixtures (conftest.py)

**Session-scoped**:
- `all_cameras`: Discover all D4XX cameras (skip all if none found)
- `camera`: Primary camera under test (selected by `--device-index`)

**Function-scoped**:
- `depth_device`: Opens depth video device, closes after test
- `ir_device`: Opens IR video device, closes after test
- `rgb_device`: Opens RGB video device, closes after test

### CLI Options
```bash
pytest --device-index=0    # Test first camera (default)
pytest --device-index=1    # Test second camera
pytest -vs                 # Verbose output, no capture
pytest -k "streaming"      # Run only streaming tests
pytest -m "d457"           # Run only tests marked for D457
```

## Running Tests

### Run all tests
```bash
cd /path/to/v4l2_test
pytest -vs
```

### Run specific test file
```bash
pytest -vs tests/test_streaming.py
```

### Run specific test
```bash
pytest -vs tests/test_streaming.py::test_depth_streaming
```

### Run specific parametrized test
```bash
pytest -vs tests/test_streaming.py::test_depth_streaming[848x480@30]
```

### Filter by pattern
```bash
pytest -vs -k "depth"      # All tests with "depth" in name
pytest -vs -k "control"    # All control tests
```

## Expected Test Duration

- **Discovery tests**: < 5 seconds
- **Streaming tests**: 30-60 seconds (depends on frame count)
- **Control tests**: < 10 seconds
- **Metadata tests**: 20-30 seconds
- **Error handling**: < 10 seconds

**Full test suite**: Approximately 5-10 minutes depending on system performance.

## Common Test Failures

### All tests fail with "No D4XX cameras detected"
- Driver not loaded: `lsmod | grep d4xx`
- No video devices: `ls /dev/video*`
- Camera not detected: Check dmesg, I2C, power

### Streaming tests fail with STREAMON error
- Format negotiation issue
- Hardware not ready (MIPI CSI-2 not configured)
- Check dmesg for NVCSI, VI errors

### Control tests fail with ENOTTY
- Control not supported by camera model
- Expected for model-specific controls (e.g., laser power on D457)

### Intermittent failures
- Timing issue (CPU load, test system performance)
- Hardware flakiness (cables, SerDes link)
- Re-run to confirm persistence

### Metadata tests fail
- Metadata device not created: Driver configuration issue
- Metadata format incorrect: Firmware/driver version mismatch
