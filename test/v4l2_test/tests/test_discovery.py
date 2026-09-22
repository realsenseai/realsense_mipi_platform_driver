"""Device enumeration, format listing, and firmware version tests."""

import os

import pytest

from ..d4xx import constants as C
from ..d4xx.discovery import discover_cameras
from ..v4l2 import ioctls
from ..v4l2.device import V4L2Device


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


D401_DUAL_RGB_RAW_FOURCCS = {
    ioctls.V4L2_PIX_FMT_SBGGR8,    # BA81, pre-1.0.6.10
    ioctls.V4L2_PIX_FMT_SBGGR10P,  # pBAA, 1.0.6.10+
}


@pytest.mark.d401
class TestD401DualRGB:
    """D401 GMSL dual-RGB: EP4 (Color) and EP3 (Color1) both carry a raw Bayer
    CSI-PT passthrough row alongside their normal format. RSDEV-14662 regressed
    when the driver-side fourcc for this row changed (BA81 -> pBAA in 1.0.6.10)
    without a matching librealsense update, silently dropping Color1. These
    tests check driver-side symmetry only -- they cannot see the librealsense
    fourcc map gap, which is exactly the point: a driver-only CI leg that
    passes here still isolates the bug to the SDK side, per RSDEV-14662.
    """

    def _raw_bayer_fourcc(self, device):
        formats = device.enum_formats()
        pixfmts = {f.pixelformat for f in formats}
        raw = pixfmts & D401_DUAL_RGB_RAW_FOURCCS
        if not raw:
            pytest.skip("No raw Bayer dual-RGB row on this node "
                         "(not a D401 dual-RGB build)")
        return raw

    def test_rgb_node_has_raw_bayer_row(self, rgb_device):
        self._raw_bayer_fourcc(rgb_device)

    def test_ir_node_has_raw_bayer_row(self, ir_device):
        self._raw_bayer_fourcc(ir_device)

    def test_dual_rgb_fourcc_matches_across_nodes(self, rgb_device, ir_device):
        """EP4 and EP3 must advertise the SAME raw Bayer fourcc.

        A future format change landed on only one of the two rows (e.g. a
        patch that edits ds5_40x_rgb_formats but misses ds5_y_formats_40x, or
        vice versa) would desync Color vs Color1 the same way RSDEV-14662 did,
        just from the opposite direction -- one node moves format, the other
        does not, and whichever loses librealsense recognition silently drops.
        """
        rgb_raw = self._raw_bayer_fourcc(rgb_device)
        ir_raw = self._raw_bayer_fourcc(ir_device)
        assert rgb_raw == ir_raw, (
            f"Color (EP4) advertises raw fourcc(s) {rgb_raw} but "
            f"Color1 (EP3) advertises {ir_raw} -- dual-RGB rows have "
            f"desynced formats"
        )


@pytest.mark.d457
class TestDFUDevice:
    """Verify the DFU character device exists."""

    def test_dfu_device_exists(self, camera):
        # DFU device is typically /dev/d4xx-dfu-*
        import glob
        dfu_devs = glob.glob("/dev/d4xx-dfu-*")
        assert len(dfu_devs) >= 1, "No DFU device found (/dev/d4xx-dfu-*)"
