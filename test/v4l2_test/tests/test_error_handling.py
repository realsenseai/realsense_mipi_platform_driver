"""Boundary conditions, invalid parameters, and recovery tests."""

import ctypes

import pytest

from ..d4xx import constants as C
from ..v4l2 import ioctls
from ..v4l2 import structs as S
from ..v4l2.device import V4L2Device
from ..v4l2.stream import StreamContext
from ..v4l2.controls import write_int_control, read_int_control


@pytest.mark.d457
class TestInvalidFormat:
    """Verify graceful handling of unsupported resolutions."""

    def test_unsupported_resolution(self, depth_device):
        """Setting an unsupported resolution should fail or clamp."""
        try:
            fmt = depth_device.set_format(
                9999, 9999, ioctls.V4L2_PIX_FMT_Z16
            )
            # Driver may clamp to nearest supported; verify it didn't accept 9999
            assert fmt.fmt.pix.width != 9999 or fmt.fmt.pix.height != 9999, \
                "Driver accepted invalid 9999x9999 resolution"
        except OSError:
            pass  # Expected: driver rejects invalid format

    def test_zero_fps(self, depth_device):
        """Setting zero FPS should fail or be handled gracefully."""
        try:
            parm = depth_device.set_parm(0)
            # Driver may clamp to minimum; verify it didn't accept 0
            denom = parm.parm.capture.timeperframe.denominator
            assert denom > 0, "Driver accepted zero FPS"
        except OSError:
            pass  # Expected


@pytest.mark.d457
class TestDoubleStreamon:
    """Verify STREAMON twice doesn't crash."""

    def test_double_streamon(self, camera):
        with V4L2Device(camera.depth_path) as dev:
            dev.set_format(848, 480, ioctls.V4L2_PIX_FMT_Z16)
            dev.set_parm(30)

            # Allocate buffers
            req = S.v4l2_requestbuffers()
            req.count = 4
            req.type = ioctls.V4L2_BUF_TYPE_VIDEO_CAPTURE
            req.memory = ioctls.V4L2_MEMORY_MMAP
            dev.ioctl(ioctls.VIDIOC_REQBUFS, req)

            # Queue at least one buffer (needed before STREAMON)
            buf = S.v4l2_buffer()
            buf.type = ioctls.V4L2_BUF_TYPE_VIDEO_CAPTURE
            buf.memory = ioctls.V4L2_MEMORY_MMAP
            buf.index = 0
            dev.ioctl(ioctls.VIDIOC_QUERYBUF, buf)
            dev.ioctl(ioctls.VIDIOC_QBUF, buf)

            # First STREAMON
            buf_type = ctypes.c_int(ioctls.V4L2_BUF_TYPE_VIDEO_CAPTURE)
            dev.ioctl(ioctls.VIDIOC_STREAMON, buf_type)

            # Second STREAMON — should not crash
            try:
                dev.ioctl(ioctls.VIDIOC_STREAMON, buf_type)
            except OSError:
                pass  # Some drivers reject, that's fine

            # Cleanup
            buf_type = ctypes.c_int(ioctls.V4L2_BUF_TYPE_VIDEO_CAPTURE)
            dev.ioctl(ioctls.VIDIOC_STREAMOFF, buf_type)

            # Release buffers
            req2 = S.v4l2_requestbuffers()
            req2.count = 0
            req2.type = ioctls.V4L2_BUF_TYPE_VIDEO_CAPTURE
            req2.memory = ioctls.V4L2_MEMORY_MMAP
            try:
                dev.ioctl(ioctls.VIDIOC_REQBUFS, req2)
            except OSError:
                pass


@pytest.mark.d457
class TestDQBUFWithoutStreamon:
    """DQBUF without STREAMON should fail gracefully."""

    def test_dqbuf_without_streamon(self, camera):
        with V4L2Device(camera.depth_path) as dev:
            dev.set_format(848, 480, ioctls.V4L2_PIX_FMT_Z16)

            buf = S.v4l2_buffer()
            buf.type = ioctls.V4L2_BUF_TYPE_VIDEO_CAPTURE
            buf.memory = ioctls.V4L2_MEMORY_MMAP

            with pytest.raises(OSError):
                dev.ioctl(ioctls.VIDIOC_DQBUF, buf)


@pytest.mark.d457
class TestOutOfRangeControl:
    """Setting out-of-range control values should fail or clamp."""

    def test_laser_power_out_of_range(self, depth_device):
        qc = depth_device.query_ctrl(C.DS5_CAMERA_CID_MANUAL_LASER_POWER)

        # Try value above maximum
        try:
            write_int_control(
                depth_device,
                C.DS5_CAMERA_CID_MANUAL_LASER_POWER,
                qc.maximum + 100,
            )
            # If it accepted, verify it clamped
            val = read_int_control(
                depth_device, C.DS5_CAMERA_CID_MANUAL_LASER_POWER
            )
            assert val <= qc.maximum, \
                f"Accepted out-of-range value: {val} > {qc.maximum}"
        except OSError:
            pass  # Expected: driver rejects out-of-range


@pytest.mark.d457
class TestOpenCloseCycle:
    """Rapid open/close cycling should not leak or crash."""

    def test_open_close_10_times(self, camera):
        for i in range(10):
            dev = V4L2Device(camera.depth_path)
            dev.open()
            cap = dev.query_cap()
            assert cap.driver.split(b"\x00")[0] == C.D4XX_DRIVER_NAME
            dev.close()


@pytest.mark.d457
class TestStreamAfterFormatChange:
    """Stream works after changing format."""

    def test_format_change_then_stream(self, camera):
        with V4L2Device(camera.depth_path) as dev:
            # Set one format
            dev.set_format(848, 480, ioctls.V4L2_PIX_FMT_Z16)
            dev.set_parm(30)

            # Change to another format
            sizes = dev.enum_framesizes(ioctls.V4L2_PIX_FMT_Z16)
            supported = [(s.discrete.width, s.discrete.height)
                         for s in sizes
                         if s.type == ioctls.V4L2_FRMSIZE_TYPE_DISCRETE]

            # Pick a different resolution if available
            alt = None
            for w, h in supported:
                if (w, h) != (848, 480):
                    alt = (w, h)
                    break

            if alt is None:
                pytest.skip("Only one resolution available")

            dev.set_format(alt[0], alt[1], ioctls.V4L2_PIX_FMT_Z16)
            dev.set_parm(30)

            with StreamContext(dev) as stream:
                frames = stream.capture_frames(10, timeout=5.0)
                assert len(frames) > 0, "No frames after format change"
