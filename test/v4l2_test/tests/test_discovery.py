"""Device enumeration, format listing, and firmware version tests."""

import errno
import os

import pytest

from ..d4xx import constants as C
from ..d4xx.discovery import discover_cameras
from ..v4l2 import ioctls
from ..v4l2.device import V4L2Device
from ..v4l2.stream import StreamContext

REFUSAL_FPS = 30


@pytest.mark.d457
class TestCameraDiscovery:
    """Verify D4XX cameras are detected and properly enumerated."""

    def test_at_least_one_camera(self, all_cameras):
        assert len(all_cameras) >= 1, "Expected at least one D4XX camera"

    def test_driver_name(self, camera):
        known = {n.decode() for n in C.KNOWN_DRIVER_NAMES}
        assert camera.driver in known, f"Unexpected driver: {camera.driver}"

    def test_six_devices_exist(self, camera):
        for path in camera.devices:
            if path:  # symlink discovery may leave missing streams as ""
                assert os.path.exists(path), f"Device node missing: {path}"
        assert len(camera.devices) == C.DEVICES_PER_CAMERA

    def test_fw_version_format(self, camera):
        if camera.fw_version is None:
            pytest.skip("FW version not available (tegra-video driver)")
        parts = camera.fw_version.split(".")
        assert len(parts) == 4, f"FW version not 4-part: {camera.fw_version}"
        major = int(parts[0])
        assert major == 5, f"Expected FW major version 5, got {major}"


@pytest.mark.d457
class TestDeviceCapabilities:
    """Verify QUERYCAP reports correct capabilities."""

    def test_depth_has_video_capture(self, depth_device):
        cap = depth_device.query_cap()
        assert cap.device_caps & ioctls.V4L2_CAP_VIDEO_CAPTURE, \
            "Depth device missing VIDEO_CAPTURE capability"

    def test_depth_has_streaming(self, depth_device):
        cap = depth_device.query_cap()
        assert cap.device_caps & ioctls.V4L2_CAP_STREAMING, \
            "Depth device missing STREAMING capability"

    def test_depth_md_has_meta_capture(self, depth_md_device):
        cap = depth_md_device.query_cap()
        assert cap.device_caps & ioctls.V4L2_CAP_META_CAPTURE, \
            "Depth metadata device missing META_CAPTURE capability"


@pytest.mark.d457
class TestFormatEnumeration:
    """Verify format and frame size enumeration on each stream."""

    def test_depth_has_z16(self, depth_device):
        formats = depth_device.enum_formats()
        pixfmts = {f.pixelformat for f in formats}
        assert ioctls.V4L2_PIX_FMT_Z16 in pixfmts, \
            "Depth device missing Z16 format"

    def test_depth_framesizes(self, depth_device):
        sizes = depth_device.enum_framesizes(ioctls.V4L2_PIX_FMT_Z16)
        assert len(sizes) > 0, "No frame sizes for Z16"
        # Verify at least 848x480 is present (common across all D4XX models)
        dims = {(s.discrete.width, s.discrete.height) for s in sizes
                if s.type == ioctls.V4L2_FRMSIZE_TYPE_DISCRETE}
        assert (848, 480) in dims, f"848x480 not in depth sizes: {dims}"

    def test_depth_frameintervals(self, depth_device):
        intervals = depth_device.enum_frameintervals(
            ioctls.V4L2_PIX_FMT_Z16, 848, 480
        )
        assert len(intervals) > 0, "No frame intervals for Z16 848x480"
        fps_values = []
        for fi in intervals:
            if fi.type == ioctls.V4L2_FRMIVAL_TYPE_DISCRETE:
                if fi.discrete.numerator > 0:
                    fps_values.append(
                        fi.discrete.denominator / fi.discrete.numerator
                    )
        assert len(fps_values) > 0, "No discrete frame intervals found"
        assert 30.0 in fps_values, f"30 FPS not supported: {fps_values}"

    def test_rgb_has_uyvy(self, rgb_device):
        formats = rgb_device.enum_formats()
        pixfmts = {f.pixelformat for f in formats}
        assert (ioctls.V4L2_PIX_FMT_UYVY in pixfmts
                or ioctls.V4L2_PIX_FMT_YUYV in pixfmts), \
            f"RGB device missing UYVY/YUYV: {pixfmts}"

    def test_ir_has_grey_or_y8i(self, ir_device):
        formats = ir_device.enum_formats()
        pixfmts = {f.pixelformat for f in formats}
        ir_fmts = {ioctls.V4L2_PIX_FMT_GREY, ioctls.V4L2_PIX_FMT_Y8I,
                    ioctls.V4L2_PIX_FMT_Y12I}
        assert pixfmts & ir_fmts, f"IR device missing GREY/Y8I/Y12I: {pixfmts}"


def _stream_refused(path, pixfmt, width, height):
    """True if STREAMON fails with EAGAIN, d4xx's code for a camera refusal.
    Host-side steps and any other STREAMON error raise."""
    with V4L2Device(path) as dev:
        dev.set_format(width, height, pixfmt)
        dev.set_parm(REFUSAL_FPS)
        stream = StreamContext(dev)
        try:
            stream._reqbufs()
            stream._mmap_buffers()
            stream._queue_all()
            try:
                stream._streamon()
            except OSError as err:
                if err.errno != errno.EAGAIN:
                    raise
                return True
            return False
        finally:
            stream.__exit__(None, None, None)


def _discrete_fps(intervals):
    """Frame rates of the discrete intervals, in fps."""
    return [fi.discrete.denominator / fi.discrete.numerator for fi in intervals
            if fi.type == ioctls.V4L2_FRMIVAL_TYPE_DISCRETE
            and fi.discrete.numerator > 0]


def _macroblocks(pixels):
    """H.264 macroblocks spanning a picture edge of this many pixels."""
    return -(-pixels // C.H264_MACROBLOCK_EDGE_PX)


@pytest.mark.d585
class TestCompressedColourFormats:
    """3C lists H.264/MJPEG with frame sizes; on 2C (no colour encoder) a listed
    format must be refused at STREAMON with EAGAIN, and any other result fails."""

    @pytest.mark.parametrize("pixfmt", [ioctls.V4L2_PIX_FMT_H264,
                                        ioctls.V4L2_PIX_FMT_MJPEG])
    def test_rgb_lists_compressed(self, d58x_camera, d58x_is_2c, pixfmt):
        with V4L2Device(d58x_camera.rgb_path) as dev:
            pixfmts = {f.pixelformat for f in dev.enum_formats()}
            sizes = dev.enum_framesizes(pixfmt) if pixfmt in pixfmts else []
        name = ioctls.FOURCC_TO_NAME[pixfmt]
        dims = sorted(((fs.discrete.width, fs.discrete.height) for fs in sizes
                       if fs.type == ioctls.V4L2_FRMSIZE_TYPE_DISCRETE),
                      key=lambda wh: wh[0] * wh[1])
        if not d58x_is_2c:
            assert pixfmt in pixfmts, f"RGB device missing {name}: {pixfmts}"
            assert len(sizes) > 0, f"No frame sizes for {name}"
            return
        if pixfmt not in pixfmts:
            return
        assert dims, f"2C D585 lists {name} with no frame sizes"
        width, height = dims[0]
        assert _stream_refused(d58x_camera.rgb_path, pixfmt, width, height), \
            f"2C D585 accepted a {name} {width}x{height} stream"

    def test_h264_rates_within_level40(self, d58x_encoder_camera):
        """Every offered H.264 size and rate fits level 4.0's MB rate."""
        pixfmt = ioctls.V4L2_PIX_FMT_H264
        with V4L2Device(d58x_encoder_camera.rgb_path) as dev:
            listed = [f.pixelformat for f in dev.enum_formats()]
            sizes = [(fs.discrete.width, fs.discrete.height)
                     for fs in dev.enum_framesizes(pixfmt)
                     if fs.type == ioctls.V4L2_FRMSIZE_TYPE_DISCRETE]
            rates = {wh: _discrete_fps(dev.enum_frameintervals(pixfmt, *wh))
                     for wh in sizes}
        assert pixfmt in listed, "H.264 is not listed on the colour node"
        assert sizes, "No discrete H.264 frame sizes"
        over = []
        for (width, height), fps_list in rates.items():
            assert fps_list, f"No H.264 frame rates at {width}x{height}"
            mbs = _macroblocks(width) * _macroblocks(height)
            over += [f"{width}x{height}@{fps:g}" for fps in fps_list
                     if mbs * fps > C.H264_LEVEL40_MAX_MBPS]
        assert not over, (f"H.264 profiles over level 4.0 "
                          f"({C.H264_LEVEL40_MAX_MBPS} MB/s): {over}")


@pytest.mark.d457
class TestDFUDevice:
    """Verify the DFU character device exists."""

    def test_dfu_device_exists(self, camera):
        # DFU device is typically /dev/d4xx-dfu-*
        import glob
        dfu_devs = glob.glob("/dev/d4xx-dfu-*")
        assert len(dfu_devs) >= 1, "No DFU device found (/dev/d4xx-dfu-*)"
