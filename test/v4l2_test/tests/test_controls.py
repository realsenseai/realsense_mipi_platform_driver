"""V4L2 control get/set tests: laser, exposure, gain, AE ROI, calibration."""

import pytest

from ..d4xx import constants as C
from ..v4l2.controls import (
    read_int_control,
    write_int_control,
    read_u8_array_control,
    enumerate_controls,
)


@pytest.mark.d457
class TestFirmwareVersion:
    """Read firmware version via control interface."""

    def test_fw_version_readable(self, depth_device, fw_version):
        raw, version_str = fw_version
        assert raw != 0, "FW version is zero"
        assert version_str.startswith("5."), \
            f"Expected FW 5.x.x.x, got {version_str}"

    def test_fw_version_matches_discovery(self, camera, fw_version):
        _, version_str = fw_version
        assert camera.fw_version == version_str


@pytest.mark.d457
class TestLaserControl:
    """Laser on/off toggle and manual laser power."""

    def test_laser_power_on_off(self, depth_device):
        # Turn laser on
        write_int_control(depth_device, C.DS5_CAMERA_CID_LASER_POWER, 1)
        val = read_int_control(depth_device, C.DS5_CAMERA_CID_LASER_POWER)
        assert val == 1, f"Laser not on: {val}"

        # Turn laser off
        write_int_control(depth_device, C.DS5_CAMERA_CID_LASER_POWER, 0)
        val = read_int_control(depth_device, C.DS5_CAMERA_CID_LASER_POWER)
        assert val == 0, f"Laser not off: {val}"

        # Restore laser on
        write_int_control(depth_device, C.DS5_CAMERA_CID_LASER_POWER, 1)

    def test_manual_laser_power_range(self, depth_device):
        qc = depth_device.query_ctrl(C.DS5_CAMERA_CID_MANUAL_LASER_POWER)
        assert qc.minimum >= 0
        assert qc.maximum > qc.minimum, \
            f"Invalid laser power range: {qc.minimum}-{qc.maximum}"

        # Set to minimum
        write_int_control(
            depth_device, C.DS5_CAMERA_CID_MANUAL_LASER_POWER, qc.minimum
        )
        val = read_int_control(
            depth_device, C.DS5_CAMERA_CID_MANUAL_LASER_POWER
        )
        assert val == qc.minimum

        # Set to maximum
        write_int_control(
            depth_device, C.DS5_CAMERA_CID_MANUAL_LASER_POWER, qc.maximum
        )
        val = read_int_control(
            depth_device, C.DS5_CAMERA_CID_MANUAL_LASER_POWER
        )
        assert val == qc.maximum


@pytest.mark.d457
class TestExposureControl:
    """Manual exposure set/get."""

    def test_exposure_set_get(self, depth_device):
        # Query exposure control range
        try:
            qc = depth_device.query_ctrl(C.DS5_CAMERA_CID_AE_SETPOINT_GET)
        except OSError:
            pytest.skip("AE setpoint control not available")

        # Read current value
        original = read_int_control(
            depth_device, C.DS5_CAMERA_CID_AE_SETPOINT_GET
        )

        # Try writing a mid-range value
        try:
            mid = (qc.minimum + qc.maximum) // 2
            write_int_control(
                depth_device, C.DS5_CAMERA_CID_AE_SETPOINT_SET, mid
            )
            readback = read_int_control(
                depth_device, C.DS5_CAMERA_CID_AE_SETPOINT_GET
            )
            assert readback == mid, f"Exposure mismatch: set {mid}, got {readback}"
        finally:
            # Restore original
            try:
                write_int_control(
                    depth_device, C.DS5_CAMERA_CID_AE_SETPOINT_SET, original
                )
            except OSError:
                pass


@pytest.mark.d457
class TestAEROI:
    """Auto-exposure ROI roundtrip."""

    def test_ae_roi_roundtrip(self, depth_device):
        try:
            original = read_int_control(
                depth_device, C.DS5_CAMERA_CID_AE_ROI_GET
            )
        except OSError:
            pytest.skip("AE ROI control not available")

        # Write a test value and read back
        test_val = original
        write_int_control(depth_device, C.DS5_CAMERA_CID_AE_ROI_SET, test_val)
        readback = read_int_control(
            depth_device, C.DS5_CAMERA_CID_AE_ROI_GET
        )
        assert readback == test_val, \
            f"AE ROI mismatch: set {test_val}, got {readback}"


@pytest.mark.d457
class TestGVD:
    """GVD (General Version Data) readable."""

    def test_gvd_readable(self, depth_device):
        try:
            data = read_u8_array_control(
                depth_device, C.DS5_CAMERA_CID_GVD, 256
            )
            assert len(data) == 256, f"GVD size: {len(data)}"
            assert any(b != 0 for b in data), "GVD is all zeros"
        except OSError:
            pytest.skip("GVD control not available")


@pytest.mark.d457
class TestCalibration:
    """Calibration table readable."""

    def test_depth_calibration_readable(self, depth_device):
        try:
            data = read_u8_array_control(
                depth_device, C.DS5_CAMERA_DEPTH_CALIBRATION_TABLE_GET, 512
            )
            assert len(data) == 512
            assert any(b != 0 for b in data), "Calibration is all zeros"
        except OSError:
            pytest.skip("Depth calibration control not available")

    def test_coeff_calibration_readable(self, depth_device):
        try:
            data = read_u8_array_control(
                depth_device, C.DS5_CAMERA_COEFF_CALIBRATION_TABLE_GET, 512
            )
            assert len(data) == 512
            assert any(b != 0 for b in data), "Coeff calibration is all zeros"
        except OSError:
            pytest.skip("Coeff calibration control not available")


@pytest.mark.d457
class TestPWM:
    """PWM control range."""

    def test_pwm_range(self, depth_device):
        try:
            qc = depth_device.query_ctrl(C.DS5_CAMERA_CID_PWM)
        except OSError:
            pytest.skip("PWM control not available")

        assert qc.minimum >= 0
        assert qc.maximum > 0, f"PWM max={qc.maximum}"

        val = read_int_control(depth_device, C.DS5_CAMERA_CID_PWM)
        assert qc.minimum <= val <= qc.maximum, \
            f"PWM {val} outside [{qc.minimum}, {qc.maximum}]"


@pytest.mark.d457
class TestControlEnumeration:
    """Verify controls can be enumerated."""

    def test_enumerate_controls(self, depth_device):
        controls = enumerate_controls(depth_device)
        assert len(controls) > 0, "No controls found"
        names = [qc.name.decode("ascii", errors="replace") for qc in controls]
        assert len(names) > 0
