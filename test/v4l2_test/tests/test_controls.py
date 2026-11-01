"""V4L2 control get/set tests: laser, exposure, gain, AE ROI, calibration."""

import errno
import time

import pytest

from ..d4xx import constants as C
from ..v4l2 import ioctls
from ..v4l2.device import V4L2Device
from ..v4l2.stream import StreamContext
from ..v4l2.controls import (
    read_int_control,
    write_int_control,
    read_u8_array_control,
    enumerate_controls,
)


@pytest.mark.d457
@pytest.mark.d401
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
@pytest.mark.d401
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
@pytest.mark.d401
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
@pytest.mark.d401
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
@pytest.mark.d401
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
@pytest.mark.d401
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
@pytest.mark.d401
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
@pytest.mark.d401
class TestAutoExposure:
    """Auto-exposure mode switching and manual exposure control."""

    def test_auto_exposure_mode_switch(self, depth_device):
        """Switch between auto and manual exposure, verify readback."""
        try:
            qc = depth_device.query_ctrl(ioctls.V4L2_CID_EXPOSURE_AUTO)
        except OSError:
            pytest.skip("auto_exposure control not available")

        original = read_int_control(
            depth_device, ioctls.V4L2_CID_EXPOSURE_AUTO
        )

        try:
            # Switch to manual
            write_int_control(
                depth_device,
                ioctls.V4L2_CID_EXPOSURE_AUTO,
                ioctls.V4L2_EXPOSURE_MANUAL,
            )
            val = read_int_control(
                depth_device, ioctls.V4L2_CID_EXPOSURE_AUTO
            )
            assert val == ioctls.V4L2_EXPOSURE_MANUAL, \
                f"Expected manual ({ioctls.V4L2_EXPOSURE_MANUAL}), got {val}"

            # Switch to aperture priority (auto)
            write_int_control(
                depth_device,
                ioctls.V4L2_CID_EXPOSURE_AUTO,
                ioctls.V4L2_EXPOSURE_APERTURE_PRIORITY,
            )
            val = read_int_control(
                depth_device, ioctls.V4L2_CID_EXPOSURE_AUTO
            )
            assert val == ioctls.V4L2_EXPOSURE_APERTURE_PRIORITY, \
                f"Expected aperture priority ({ioctls.V4L2_EXPOSURE_APERTURE_PRIORITY}), got {val}"
        finally:
            try:
                write_int_control(
                    depth_device, ioctls.V4L2_CID_EXPOSURE_AUTO, original
                )
            except OSError:
                pass

    def test_manual_exposure_set_get(self, depth_device):
        """In manual mode, set exposure_time_absolute and read back."""
        try:
            depth_device.query_ctrl(ioctls.V4L2_CID_EXPOSURE_AUTO)
        except OSError:
            pytest.skip("auto_exposure control not available")

        original_mode = read_int_control(
            depth_device, ioctls.V4L2_CID_EXPOSURE_AUTO
        )

        try:
            # Switch to manual mode
            write_int_control(
                depth_device,
                ioctls.V4L2_CID_EXPOSURE_AUTO,
                ioctls.V4L2_EXPOSURE_MANUAL,
            )

            # Read current exposure value
            try:
                original_exp = read_int_control(
                    depth_device, ioctls.V4L2_CID_EXPOSURE_ABSOLUTE
                )
            except OSError:
                pytest.skip("exposure_time_absolute not readable")

            # Set two different known-safe values and verify readback.
            # exposure_time_absolute is a u32 control (range 1-200000 typical).
            test_values = [1000, 5000]
            for target in test_values:
                write_int_control(
                    depth_device, ioctls.V4L2_CID_EXPOSURE_ABSOLUTE, target
                )
                val = read_int_control(
                    depth_device, ioctls.V4L2_CID_EXPOSURE_ABSOLUTE
                )
                assert val == target, \
                    f"Exposure mismatch: set {target}, got {val}"
        finally:
            try:
                write_int_control(
                    depth_device, ioctls.V4L2_CID_EXPOSURE_AUTO, original_mode
                )
            except OSError:
                pass


@pytest.mark.d457
@pytest.mark.d401
class TestHWReset:
    """Hardware reset via CID_HW_RESET button control.

    Triggers a full camera module reset and verifies the device recovers:
    1. Read a control to confirm the device is alive
    2. Trigger hw_reset (button write)
    3. Close the device (it becomes invalid during reset)
    4. Poll until the device is accessible again
    5. Verify the camera is functional: read a control + short stream
    """

    RESET_POLL_INTERVAL = 0.5  # seconds between recovery polls
    RESET_TIMEOUT = 15.0       # max seconds to wait for recovery

    def test_hw_reset_recovery(self, camera):
        """Reset camera hardware and verify it comes back functional."""
        # 1. Verify device is alive before reset
        with V4L2Device(camera.depth_path) as dev:
            try:
                dev.query_ctrl(C.DS5_CAMERA_CID_HW_RESET)
            except OSError:
                pytest.skip("hw_reset control not available")

            cap = dev.query_cap()
            assert cap.capabilities != 0, "Device not responding before reset"

            # 2. Trigger reset
            write_int_control(dev, C.DS5_CAMERA_CID_HW_RESET, 1)

        # 3. Device is resetting — wait for it to come back
        start = time.monotonic()
        recovered = False

        while time.monotonic() - start < self.RESET_TIMEOUT:
            time.sleep(self.RESET_POLL_INTERVAL)
            try:
                with V4L2Device(camera.depth_path) as dev:
                    cap = dev.query_cap()
                    if cap.capabilities != 0:
                        recovered = True
                        break
            except OSError:
                continue

        assert recovered, \
            f"Camera did not recover within {self.RESET_TIMEOUT}s after hw_reset"

        # 4. Verify functional: read laser power control
        with V4L2Device(camera.depth_path) as dev:
            val = read_int_control(dev, C.DS5_CAMERA_CID_LASER_POWER)
            assert val in (0, 1), f"Unexpected laser power after reset: {val}"

        # 5. Verify functional: short depth stream
        with V4L2Device(camera.depth_path) as dev:
            dev.set_format(848, 480, ioctls.V4L2_PIX_FMT_Z16)
            dev.set_parm(30)
            with StreamContext(dev) as stream:
                frames = stream.capture_frames(10, timeout=5.0)
                assert len(frames) > 0, "No frames after hw_reset recovery"


@pytest.mark.d457
@pytest.mark.d401
class TestReadoutShaping:
    """Readout shaping control (Depth register DS5_READOUT_SHAPING=0x0030), range 0-100."""

    MIN = 0
    MAX = 100
    MID = 50
    DEFAULT = 0

    def test_readout_shaping_enumerated(self, depth_device):
        controls = enumerate_controls(depth_device)
        names = [c.name.decode("ascii", errors="replace").lower() for c in controls]
        assert any("readout shaping" in n for n in names), \
            "readout shaping not found in enumerated controls"

    def test_readout_shaping_read(self, depth_device):
        val = read_int_control(depth_device, C.DS5_CAMERA_CID_READOUT_SHAPING)
        assert self.MIN <= val <= self.MAX, f"readout_shaping out of range: {val}"

    def test_readout_shaping_set_legal(self, depth_device):
        original = read_int_control(depth_device, C.DS5_CAMERA_CID_READOUT_SHAPING)
        try:
            for v in (self.MIN, self.MID, self.MAX):
                write_int_control(depth_device, C.DS5_CAMERA_CID_READOUT_SHAPING, v)
                val = read_int_control(depth_device, C.DS5_CAMERA_CID_READOUT_SHAPING)
                assert val == v, f"set {v}: got {val}"
        finally:
            write_int_control(depth_device, C.DS5_CAMERA_CID_READOUT_SHAPING, original)

    def test_readout_shaping_clamping(self, depth_device):
        # V4L2 (VIDIOC_S_CTRL) clamps out-of-range writes to [min, max]
        original = read_int_control(depth_device, C.DS5_CAMERA_CID_READOUT_SHAPING)
        try:
            write_int_control(depth_device, C.DS5_CAMERA_CID_READOUT_SHAPING, self.MAX + 1)
            val = read_int_control(depth_device, C.DS5_CAMERA_CID_READOUT_SHAPING)
            assert val == self.MAX, f"{self.MAX + 1} should clamp to {self.MAX}, got {val}"

            write_int_control(depth_device, C.DS5_CAMERA_CID_READOUT_SHAPING, self.MIN - 1)
            val = read_int_control(depth_device, C.DS5_CAMERA_CID_READOUT_SHAPING)
            assert val == self.MIN, f"{self.MIN - 1} should clamp to {self.MIN}, got {val}"
        finally:
            write_int_control(depth_device, C.DS5_CAMERA_CID_READOUT_SHAPING, original)


@pytest.mark.d457
@pytest.mark.d401
class TestAEType:
    """Depth auto-exposure mode (HWMC SETAETYPE 0x87 / GETAETYPE 0x88).

    Single read/write control 'depth ae mode' (DS5_CAMERA_CID_AE_MODE): reads
    route to GETAETYPE, writes to SETAETYPE. Mirrors USB depth XU selector 0x11.
    Values: 0=Legacy, 1=V2. FW rejects writes while streaming, so these tests
    run on an idle device.
    """

    MIN = 0       # DS5_AE_TYPE_LEGACY
    MAX = 1       # DS5_AE_TYPE_V2
    DEFAULT = 0   # DS5_AE_TYPE_LEGACY

    def test_ae_type_enumerated(self, depth_device):
        controls = enumerate_controls(depth_device)
        names = [c.name.decode("ascii", errors="replace").lower() for c in controls]
        assert any("depth ae mode" in n for n in names), \
            "depth ae mode not found in enumerated controls"

    def test_ae_type_read(self, depth_device):
        val = read_int_control(depth_device, C.DS5_CAMERA_CID_AE_MODE)
        assert self.MIN <= val <= self.MAX, f"ae mode out of range: {val}"

    def test_ae_type_write_legal(self, depth_device):
        original = read_int_control(depth_device, C.DS5_CAMERA_CID_AE_MODE)
        try:
            for v in (self.MIN, self.MAX):
                write_int_control(depth_device, C.DS5_CAMERA_CID_AE_MODE, v)
                val = read_int_control(depth_device, C.DS5_CAMERA_CID_AE_MODE)
                assert val == v, f"set {v}: got {val}"
        finally:
            write_int_control(depth_device, C.DS5_CAMERA_CID_AE_MODE, original)

    def test_ae_type_set_illegal(self, depth_device):
        # V4L2 (VIDIOC_S_CTRL) clamps out-of-range writes to [min, max]
        original = read_int_control(depth_device, C.DS5_CAMERA_CID_AE_MODE)
        try:
            write_int_control(depth_device, C.DS5_CAMERA_CID_AE_MODE, self.MAX + 1)
            val = read_int_control(depth_device, C.DS5_CAMERA_CID_AE_MODE)
            assert val == self.MAX, f"{self.MAX + 1} should clamp to {self.MAX}, got {val}"

            write_int_control(depth_device, C.DS5_CAMERA_CID_AE_MODE, self.MIN - 1)
            val = read_int_control(depth_device, C.DS5_CAMERA_CID_AE_MODE)
            assert val == self.MIN, f"{self.MIN - 1} should clamp to {self.MIN}, got {val}"
        finally:
            write_int_control(depth_device, C.DS5_CAMERA_CID_AE_MODE, original)

    def test_ae_type_default(self, depth_device):
        original = read_int_control(depth_device, C.DS5_CAMERA_CID_AE_MODE)
        try:
            write_int_control(depth_device, C.DS5_CAMERA_CID_AE_MODE, self.DEFAULT)
            val = read_int_control(depth_device, C.DS5_CAMERA_CID_AE_MODE)
            assert val == self.DEFAULT, f"default {self.DEFAULT}: got {val}"
        finally:
            write_int_control(depth_device, C.DS5_CAMERA_CID_AE_MODE, original)

    def test_ae_type_roundtrip(self, depth_device):
        original = read_int_control(depth_device, C.DS5_CAMERA_CID_AE_MODE)
        try:
            for v in (self.MIN, self.MAX):
                write_int_control(depth_device, C.DS5_CAMERA_CID_AE_MODE, v)
                val = read_int_control(depth_device, C.DS5_CAMERA_CID_AE_MODE)
                assert val == v, f"roundtrip {v}: got {val}"
        finally:
            write_int_control(depth_device, C.DS5_CAMERA_CID_AE_MODE, original)


@pytest.mark.d457
@pytest.mark.d401
class TestSyncMode:
    """Sync mode control (RSDEV-6449): simplified 3-value public API.

    Public values: 0=Default, 1=Master, 2=External Sync.
    FW maps External Sync to Slave (D401) or SlaveFull (D457) internally.
    D58x has no master role, so the driver skip-masks Master there
    (RSDEV-14614); the accepted values are chosen per SKU below.
    """

    SYNC_MODE_DEFAULT  = 0
    SYNC_MODE_MASTER   = 1
    SYNC_MODE_EXTERNAL = 2

    @staticmethod
    def _is_d58x(dev):
        """Aligned depth is registered on the D58x depth node only."""
        try:
            dev.query_ctrl(C.D500_CAMERA_CID_ALIGNED_DEPTH)
            return True
        except OSError:
            return False

    @classmethod
    def _valid_modes(cls, dev):
        if cls._is_d58x(dev):
            return (cls.SYNC_MODE_DEFAULT, cls.SYNC_MODE_EXTERNAL)
        return (cls.SYNC_MODE_DEFAULT, cls.SYNC_MODE_MASTER,
                cls.SYNC_MODE_EXTERNAL)

    @classmethod
    def _non_default_mode(cls, dev):
        """A mode that is valid on this SKU and is not DEFAULT."""
        return (cls.SYNC_MODE_EXTERNAL if cls._is_d58x(dev)
                else cls.SYNC_MODE_MASTER)

    def test_sync_mode_range(self, depth_device):
        """Control must advertise min=0, max=2."""
        qc = depth_device.query_ctrl(C.DS5_CAMERA_CID_SYNC_MODE)
        assert qc.minimum == 0, f"Expected min=0, got {qc.minimum}"
        assert qc.maximum == self.SYNC_MODE_EXTERNAL, \
            f"Expected max={self.SYNC_MODE_EXTERNAL}, got {qc.maximum}"

    def test_sync_mode_set_get_roundtrip(self, depth_device):
        """SET/GET roundtrip for each valid public value."""
        original = read_int_control(depth_device, C.DS5_CAMERA_CID_SYNC_MODE)
        try:
            for mode in self._valid_modes(depth_device):
                write_int_control(depth_device, C.DS5_CAMERA_CID_SYNC_MODE, mode)
                val = read_int_control(depth_device, C.DS5_CAMERA_CID_SYNC_MODE)
                assert val == mode, f"SET {mode} → GET returned {val}"
        finally:
            write_int_control(depth_device, C.DS5_CAMERA_CID_SYNC_MODE,
                              self.SYNC_MODE_DEFAULT)

    def test_sync_mode_default_after_reset(self, depth_device):
        """After writing DEFAULT, readback must be DEFAULT."""
        write_int_control(depth_device, C.DS5_CAMERA_CID_SYNC_MODE,
                          self._non_default_mode(depth_device))
        write_int_control(depth_device, C.DS5_CAMERA_CID_SYNC_MODE,
                          self.SYNC_MODE_DEFAULT)
        val = read_int_control(depth_device, C.DS5_CAMERA_CID_SYNC_MODE)
        assert val == self.SYNC_MODE_DEFAULT, \
            f"Expected DEFAULT(0) after reset, got {val}"

    def test_sync_mode_master_rejected_on_d58x(self, depth_device):
        """D58x must reject Master; the menu entry is skip-masked."""
        if not self._is_d58x(depth_device):
            pytest.skip("Master is a valid mode on D4xx")
        with pytest.raises(OSError) as exc:
            write_int_control(depth_device, C.DS5_CAMERA_CID_SYNC_MODE,
                              self.SYNC_MODE_MASTER)
        assert exc.value.errno == errno.EINVAL, \
            f"Expected EINVAL setting Master on D58x, got {exc.value.errno}"


@pytest.mark.d457
@pytest.mark.d401
class TestControlEnumeration:
    """Verify controls can be enumerated."""

    def test_enumerate_controls(self, depth_device):
        controls = enumerate_controls(depth_device)
        assert len(controls) > 0, "No controls found"
        names = [qc.name.decode("ascii", errors="replace") for qc in controls]
        assert len(names) > 0


@pytest.mark.d457
class TestAlignedDepth:
    """D58x 'Enable Aligned Depth' (D500_CAMERA_CID_ALIGNED_DEPTH, USB depth XU 0x10).

    Single R/W boolean on the D58x depth node. SET is refused with EBUSY
    unless the whole camera is idle; GET/SET report EOPNOTSUPP when the FW
    lacks the register. FW support is detected by a first GET, so old FW is
    never written to. Aligned depth keeps the depth WxH and Z16, so geometry
    and frame rate must not depend on the control.
    """

    MIN = 0       # native depth
    MAX = 1       # depth aligned to the colour viewport
    STEP = 1
    DEFAULT = 0
    NAME = "enable aligned depth"
    FLAGS = (ioctls.V4L2_CTRL_FLAG_VOLATILE |
             ioctls.V4L2_CTRL_FLAG_EXECUTE_ON_WRITE)

    BYTES_PER_PIXEL = 2   # Z16
    FPS = 30
    FPS_TOLERANCE = 0.05
    FRAME_COUNT = 60
    MIN_FRAME_ARRIVAL = 0.90
    MAX_CONSECUTIVE_DROPS = 2
    STREAM_SIZES = [(848, 480), (1280, 720), (640, 360)]

    @staticmethod
    def _require_control(dev):
        """Return the queryctrl, or skip when this node does not register it."""
        try:
            return dev.query_ctrl(C.D500_CAMERA_CID_ALIGNED_DEPTH)
        except OSError:
            pytest.skip("aligned depth control not registered "
                        "(not a D58x depth node)")

    @staticmethod
    def _fw_value(dev):
        """GET the control; None when the FW lacks support (EOPNOTSUPP).

        Any other error is a real failure and propagates.
        """
        try:
            return read_int_control(dev, C.D500_CAMERA_CID_ALIGNED_DEPTH)
        except OSError as e:
            if e.errno == errno.EOPNOTSUPP:
                return None
            raise

    def _require_fw_value(self, dev):
        """Return the current value, or skip on FW without support."""
        self._require_control(dev)
        value = self._fw_value(dev)
        if value is None:
            pytest.skip("FW does not support aligned depth "
                        "(GET returned EOPNOTSUPP)")
        return value

    @staticmethod
    def _require_mode(dev, width, height, fps):
        pixfmt = ioctls.V4L2_PIX_FMT_Z16
        sizes = {(s.discrete.width, s.discrete.height)
                 for s in dev.enum_framesizes(pixfmt)
                 if s.type == ioctls.V4L2_FRMSIZE_TYPE_DISCRETE}
        if (width, height) not in sizes:
            pytest.skip(f"Z16 {width}x{height} not supported on this device")
        rates = [fi.discrete.denominator / fi.discrete.numerator
                 for fi in dev.enum_frameintervals(pixfmt, width, height)
                 if fi.type == ioctls.V4L2_FRMIVAL_TYPE_DISCRETE
                 and fi.discrete.numerator > 0]
        if fps not in rates:
            pytest.skip(f"{fps} FPS not supported at Z16 {width}x{height}")

    def test_aligned_depth_enumerated(self, depth_device):
        qc = self._require_control(depth_device)
        ids = {c.id for c in enumerate_controls(depth_device)}
        assert C.D500_CAMERA_CID_ALIGNED_DEPTH in ids, \
            "aligned depth not found in enumerated controls"
        name = qc.name.decode("ascii", errors="replace").lower()
        assert name == self.NAME, f"unexpected name: {name!r}"
        assert qc.type == ioctls.V4L2_CTRL_TYPE_BOOLEAN, \
            f"expected BOOLEAN ({ioctls.V4L2_CTRL_TYPE_BOOLEAN}), got {qc.type}"
        assert (qc.minimum, qc.maximum, qc.step) == \
            (self.MIN, self.MAX, self.STEP), \
            f"range {qc.minimum}..{qc.maximum} step {qc.step}"
        assert qc.flags & self.FLAGS == self.FLAGS, \
            f"flags 0x{qc.flags:x} lack VOLATILE|EXECUTE_ON_WRITE"
        assert self.MIN <= qc.default_value <= self.MAX, \
            f"default {qc.default_value} outside [{self.MIN}, {self.MAX}]"

    def test_aligned_depth_read(self, depth_device):
        """GET is in range on supporting FW and EOPNOTSUPP on old FW."""
        self._require_control(depth_device)
        value = self._fw_value(depth_device)
        if value is not None:
            assert self.MIN <= value <= self.MAX, \
                f"aligned depth out of range: {value}"

    def test_aligned_depth_write_legal(self, depth_device):
        original = self._require_fw_value(depth_device)
        try:
            for v in (self.MIN, self.MAX):
                write_int_control(
                    depth_device, C.D500_CAMERA_CID_ALIGNED_DEPTH, v)
                val = read_int_control(
                    depth_device, C.D500_CAMERA_CID_ALIGNED_DEPTH)
                assert val == v, f"set {v}: got {val}"
        finally:
            write_int_control(
                depth_device, C.D500_CAMERA_CID_ALIGNED_DEPTH, original)

    def test_aligned_depth_set_illegal(self, depth_device):
        # The V4L2 core normalises BOOLEAN writes (any nonzero is true), so
        # both out-of-range values must read back as MAX.
        original = self._require_fw_value(depth_device)
        try:
            for v in (self.MAX + 1, self.MIN - 1):
                write_int_control(
                    depth_device, C.D500_CAMERA_CID_ALIGNED_DEPTH, v)
                val = read_int_control(
                    depth_device, C.D500_CAMERA_CID_ALIGNED_DEPTH)
                assert val == self.MAX, \
                    f"{v} should normalise to {self.MAX}, got {val}"
        finally:
            write_int_control(
                depth_device, C.D500_CAMERA_CID_ALIGNED_DEPTH, original)

    def test_aligned_depth_default(self, depth_device):
        original = self._require_fw_value(depth_device)
        try:
            write_int_control(
                depth_device, C.D500_CAMERA_CID_ALIGNED_DEPTH, self.DEFAULT)
            val = read_int_control(
                depth_device, C.D500_CAMERA_CID_ALIGNED_DEPTH)
            assert val == self.DEFAULT, f"default {self.DEFAULT}: got {val}"
        finally:
            write_int_control(
                depth_device, C.D500_CAMERA_CID_ALIGNED_DEPTH, original)

    def test_aligned_depth_roundtrip(self, depth_device):
        original = self._require_fw_value(depth_device)
        try:
            for v in (self.MAX, self.MIN, self.MAX):
                write_int_control(
                    depth_device, C.D500_CAMERA_CID_ALIGNED_DEPTH, v)
                val = read_int_control(
                    depth_device, C.D500_CAMERA_CID_ALIGNED_DEPTH)
                assert val == v, f"roundtrip {v}: got {val}"
        finally:
            write_int_control(
                depth_device, C.D500_CAMERA_CID_ALIGNED_DEPTH, original)

    def test_aligned_depth_set_while_streaming_ebusy(self, camera):
        """SET while depth streams is refused with EBUSY.

        The driver checks idleness before any FW access, so this also runs
        on old FW. It writes the current value (DEFAULT when FW cannot
        report one) so a wrongly accepted write changes nothing.
        """
        width, height = self.STREAM_SIZES[0]
        with V4L2Device(camera.depth_path) as dev:
            self._require_control(dev)
            self._require_mode(dev, width, height, self.FPS)
            value = self._fw_value(dev)
            if value is None:
                value = self.DEFAULT
            dev.set_format(width, height, ioctls.V4L2_PIX_FMT_Z16)
            dev.set_parm(self.FPS)
            with StreamContext(dev) as stream:
                stream.capture_frames(2, timeout=5.0)
                with pytest.raises(OSError) as exc:
                    write_int_control(
                        dev, C.D500_CAMERA_CID_ALIGNED_DEPTH, value)
            got = exc.value.errno
            assert got == errno.EBUSY, \
                f"expected EBUSY, got {errno.errorcode.get(got, got)}"

    @pytest.mark.parametrize("enabled", [False, True], ids=["off", "on"])
    @pytest.mark.parametrize("size", STREAM_SIZES,
                             ids=[f"{w}x{h}" for w, h in STREAM_SIZES])
    def test_aligned_depth_capture_geometry(self, camera, size, enabled):
        """Z16 geometry and frame rate are identical with the control on/off.

        Tegra VI pads the stride to TEGRA_STRIDE_ALIGNMENT, so sizeimage is
        bytesperline*H, and equals W*H*2 whenever the Z16 line is unpadded.
        """
        width, height = size
        packed_line = width * self.BYTES_PER_PIXEL
        with V4L2Device(camera.depth_path) as dev:
            self._require_control(dev)
            self._require_mode(dev, width, height, self.FPS)
            original = self._fw_value(dev)
            if enabled and original is None:
                pytest.skip("FW does not support aligned depth "
                            "(GET returned EOPNOTSUPP)")

            ref = dev.set_format(
                width, height, ioctls.V4L2_PIX_FMT_Z16).fmt.pix
            ref = (ref.bytesperline, ref.sizeimage)
            try:
                if original is not None:
                    target = self.MAX if enabled else self.MIN
                    write_int_control(
                        dev, C.D500_CAMERA_CID_ALIGNED_DEPTH, target)
                    val = read_int_control(
                        dev, C.D500_CAMERA_CID_ALIGNED_DEPTH)
                    assert val == target, f"set {target}: got {val}"

                pix = dev.set_format(
                    width, height, ioctls.V4L2_PIX_FMT_Z16).fmt.pix
                assert (pix.width, pix.height, pix.pixelformat) == \
                    (width, height, ioctls.V4L2_PIX_FMT_Z16), \
                    f"format became {pix.width}x{pix.height} 0x{pix.pixelformat:x}"
                assert (pix.bytesperline, pix.sizeimage) == ref, \
                    f"geometry {(pix.bytesperline, pix.sizeimage)} != {ref}"
                assert pix.bytesperline >= packed_line, \
                    f"bytesperline {pix.bytesperline} < W*2 {packed_line}"
                assert pix.sizeimage == pix.bytesperline * height, \
                    f"sizeimage {pix.sizeimage} != {pix.bytesperline}*{height}"
                if pix.bytesperline == packed_line:
                    assert pix.sizeimage == packed_line * height, \
                        f"sizeimage {pix.sizeimage} != W*H*2"

                dev.set_parm(self.FPS)
                with StreamContext(dev) as stream:
                    frames = stream.capture_frames(
                        self.FRAME_COUNT,
                        timeout=max(5.0, 4.0 * self.FRAME_COUNT / self.FPS),
                    )
            finally:
                if original is not None:
                    write_int_control(
                        dev, C.D500_CAMERA_CID_ALIGNED_DEPTH, original)

        assert len(frames) >= int(self.FRAME_COUNT * self.MIN_FRAME_ARRIVAL), \
            f"Only {len(frames)}/{self.FRAME_COUNT} frames arrived"
        for buf, _ in frames:
            assert buf.bytesused == pix.sizeimage, \
                f"bytesused {buf.bytesused} != sizeimage {pix.sizeimage}"

        sequences = [buf.sequence for buf, _ in frames]
        for prev, cur in zip(sequences, sequences[1:]):
            assert cur > prev, f"Non-monotonic sequence: {prev} -> {cur}"
            assert cur - prev <= self.MAX_CONSECUTIVE_DROPS + 1, \
                f"Frame drop: gap={cur - prev} between seq {prev} and {cur}"

        # Mean rate over the capture; the first frame is warm-up.
        stamps = [buf.timestamp.tv_sec + buf.timestamp.tv_usec / 1e6
                  for buf, _ in frames[1:]]
        span = stamps[-1] - stamps[0]
        assert span > 0, "frame timestamps do not advance"
        measured = (len(stamps) - 1) / span
        assert abs(measured - self.FPS) <= self.FPS * self.FPS_TOLERANCE, \
            f"FPS {measured:.2f} outside {self.FPS} +/- {self.FPS_TOLERANCE:.0%}"


@pytest.mark.d457
@pytest.mark.d401
class TestErrorCode:
    """Depth FW error code (librealsense XU 0x07): read-to-clear, FW-gated on D4xx."""

    def test_error_code_read_clears(self, depth_device, fw_version):
        raw, version_str = fw_version
        if raw < C.DS5_FW_ERROR_CODE_MIN:
            with pytest.raises(OSError) as exc:
                read_int_control(depth_device, C.DS5_CAMERA_CID_ERROR_CODE)
            assert exc.value.errno == errno.EOPNOTSUPP, \
                f"FW {version_str}: expected EOPNOTSUPP, got {exc.value}"
            return
        first = read_int_control(depth_device, C.DS5_CAMERA_CID_ERROR_CODE)
        assert 0 <= first <= 255
        assert read_int_control(depth_device, C.DS5_CAMERA_CID_ERROR_CODE) == 0, \
            "error code did not clear on read"
