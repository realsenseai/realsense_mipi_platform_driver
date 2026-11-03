"""Pytest fixtures for D4XX V4L2 native tests.

Provides camera discovery, device open/close, and report plugin registration.
"""

import platform
import re

import pytest

from .d4xx.discovery import discover_cameras
from .d4xx import constants as C
from .v4l2.device import V4L2Device
from .v4l2 import ioctls
from .v4l2.controls import read_u8_array_control
from .report import D4xxReportPlugin


# ---- Plugin registration ----

_report_plugin = D4xxReportPlugin()


def pytest_configure(config):
    config.pluginmanager.register(_report_plugin, "d4xx_report")


# ---- CLI options ----

def pytest_addoption(parser):
    parser.addoption(
        "--device-index",
        type=int,
        default=0,
        help="Index of the D4XX camera to test (default: 0, the first found)",
    )


# ---- Session-scoped fixtures ----

@pytest.fixture(scope="session")
def all_cameras():
    """Discover all connected D4XX cameras. Skip all if none found."""
    cameras = discover_cameras()
    if not cameras:
        pytest.skip("No D4XX cameras detected")
    return cameras


@pytest.fixture(scope="session")
def camera(all_cameras, request):
    """The primary camera under test, selected by --device-index."""
    idx = request.config.getoption("--device-index")
    if idx >= len(all_cameras):
        pytest.skip(f"Camera index {idx} not available (found {len(all_cameras)})")
    cam = all_cameras[idx]

    # Feed info to report plugin
    _report_plugin.set_camera_info(
        card=cam.card,
        fw_version=cam.fw_version or "unknown",
        device_path=cam.depth_path,
    )
    return cam


@pytest.fixture(scope="session")
def fw_version(camera):
    """Cached firmware version as (raw_int, version_string) tuple."""
    with V4L2Device(camera.depth_path) as dev:
        try:
            ctrl = dev.get_ctrl(C.DS5_CAMERA_CID_FW_VERSION)
            raw = ctrl.value
            major = (raw >> 24) & 0xFF
            minor = (raw >> 16) & 0xFF
            patch = (raw >> 8) & 0xFF
            build = raw & 0xFF
            return raw, f"{major}.{minor}.{patch}.{build}"
        except OSError:
            pytest.skip("FW version control not available (tegra-video driver)")


def _kernel_version():
    """(major, minor) of the running kernel, or (0, 0) if unparseable."""
    m = re.match(r"(\d+)\.(\d+)", platform.release())
    return (int(m.group(1)), int(m.group(2))) if m else (0, 0)


@pytest.fixture(scope="session")
def d58x_camera(camera):
    """The camera under test, only if it is a D58x on a JetPack 6+ kernel."""
    if camera.fw_version is None:
        pytest.skip("FW version not available: cannot tell a D58x")
    if int(camera.fw_version.split(".")[0]) not in C.D58X_FW_MAJORS:
        pytest.skip(f"Not a D58x (FW {camera.fw_version})")
    if _kernel_version() < C.COMPRESSED_MIN_KERNEL:
        pytest.skip(f"Kernel {platform.release()} predates compressed colour")
    return camera


def _d58x_usb_pid(camera):
    """USB PID from the camera's GVD, or None if GVD cannot be read."""
    try:
        with V4L2Device(camera.depth_path) as dev:
            gvd = read_u8_array_control(dev, C.DS5_CAMERA_CID_GVD,
                                        C.D58X_GVD_LEN)
    except OSError:
        return None
    off = C.D58X_GVD_PID_OFFSET
    return int.from_bytes(gvd[off:off + C.D58X_GVD_PID_LEN], "little")


@pytest.fixture(scope="session")
def d58x_is_2c(d58x_camera):
    """True on a 2C D585 (GVD PID), which has no colour encoder. An
    unreadable GVD counts as 3C, so a format loss is never hidden."""
    return _d58x_usb_pid(d58x_camera) == C.D585_2C_PROTO_PID


@pytest.fixture(scope="session")
def d58x_encoder_camera(d58x_camera, d58x_is_2c):
    """A D58x with a colour encoder; a 2C D585 skips."""
    if d58x_is_2c:
        pytest.skip(f"D585 2C (GVD PID 0x{C.D585_2C_PROTO_PID:04X}): "
                    "no colour encoder")
    return d58x_camera


# ---- Function-scoped device fixtures ----

@pytest.fixture
def depth_device(camera):
    """Open and yield the depth video device, close after test."""
    dev = V4L2Device(camera.depth_path)
    dev.open()
    yield dev
    dev.close()


@pytest.fixture
def rgb_device(camera):
    """Open and yield the RGB video device, close after test."""
    dev = V4L2Device(camera.rgb_path)
    dev.open()
    yield dev
    dev.close()


@pytest.fixture
def ir_device(camera):
    """Open and yield the IR video device, close after test."""
    dev = V4L2Device(camera.ir_path)
    dev.open()
    yield dev
    dev.close()


@pytest.fixture
def depth_md_device(camera):
    """Open and yield the depth metadata device, close after test."""
    dev = V4L2Device(camera.depth_md_path)
    dev.open()
    yield dev
    dev.close()


def _discrete_sizes(dev, pixfmt):
    """Return set of (w, h) for discrete frame sizes."""
    return {
        (s.discrete.width, s.discrete.height)
        for s in dev.enum_framesizes(pixfmt)
        if s.type == ioctls.V4L2_FRMSIZE_TYPE_DISCRETE
    }


# ---- Cached common resolution discovery (used by pytest_generate_tests) ----

_common_res_cache = None


def _discover_common_resolutions():
    """Discover resolutions shared by depth (Z16) and RGB.

    Returns ([(w,h), ...], rgb_pixfmt) or ([], None) if unavailable.
    Cached after first call.
    """
    global _common_res_cache
    if _common_res_cache is not None:
        return _common_res_cache

    cameras = discover_cameras()
    if not cameras:
        _common_res_cache = ([], None)
        return _common_res_cache

    cam = cameras[0]
    try:
        with V4L2Device(cam.depth_path) as ddev:
            depth_sizes = _discrete_sizes(ddev, ioctls.V4L2_PIX_FMT_Z16)

        with V4L2Device(cam.rgb_path) as rdev:
            rgb_formats = rdev.enum_formats()
            if not rgb_formats:
                _common_res_cache = ([], None)
                return _common_res_cache
            rgb_pixfmt = rgb_formats[0].pixelformat
            rgb_sizes = _discrete_sizes(rdev, rgb_pixfmt)

        common = sorted(depth_sizes & rgb_sizes, key=lambda wh: wh[0] * wh[1])
        _common_res_cache = (common, rgb_pixfmt)
    except (OSError, Exception):
        _common_res_cache = ([], None)

    return _common_res_cache


@pytest.fixture(scope="session")
def common_depth_rgb_resolutions(camera):
    """Resolutions supported by both depth (Z16) and RGB on this camera."""
    resolutions, rgb_pixfmt = _discover_common_resolutions()
    if not resolutions:
        pytest.skip("No common resolutions between depth and RGB")
    return resolutions, rgb_pixfmt


# ---- Cached common depth+RGB+IR resolution discovery ----

_common_all_res_cache = None


def _discover_common_all_resolutions():
    """Discover resolutions shared by depth (Z16), RGB, and IR (GREY).

    Returns ([(w,h), ...], rgb_pixfmt) or ([], None) if unavailable.
    Cached after first call.
    """
    global _common_all_res_cache
    if _common_all_res_cache is not None:
        return _common_all_res_cache

    cameras = discover_cameras()
    if not cameras:
        _common_all_res_cache = ([], None)
        return _common_all_res_cache

    cam = cameras[0]
    try:
        with V4L2Device(cam.depth_path) as ddev:
            depth_sizes = _discrete_sizes(ddev, ioctls.V4L2_PIX_FMT_Z16)

        with V4L2Device(cam.rgb_path) as rdev:
            rgb_formats = rdev.enum_formats()
            if not rgb_formats:
                _common_all_res_cache = ([], None)
                return _common_all_res_cache
            rgb_pixfmt = rgb_formats[0].pixelformat
            rgb_sizes = _discrete_sizes(rdev, rgb_pixfmt)

        with V4L2Device(cam.ir_path) as idev:
            ir_sizes = _discrete_sizes(idev, ioctls.V4L2_PIX_FMT_GREY)

        common = sorted(
            depth_sizes & rgb_sizes & ir_sizes,
            key=lambda wh: wh[0] * wh[1],
        )
        _common_all_res_cache = (common, rgb_pixfmt)
    except (OSError, Exception):
        _common_all_res_cache = ([], None)

    return _common_all_res_cache


@pytest.fixture(scope="session")
def common_depth_rgb_ir_resolutions(camera):
    """Resolutions supported by depth (Z16), RGB, and IR (GREY)."""
    resolutions, rgb_pixfmt = _discover_common_all_resolutions()
    if not resolutions:
        pytest.skip("No common resolutions between depth, RGB, and IR")
    return resolutions, rgb_pixfmt


def pytest_generate_tests(metafunc):
    """Dynamically parametrize resolution fixtures from hardware enumeration."""
    if "resolution" in metafunc.fixturenames:
        resolutions, _ = _discover_common_resolutions()
        ids = [f"{w}x{h}" for w, h in resolutions]
        metafunc.parametrize("resolution", resolutions, ids=ids)
    if "tri_resolution" in metafunc.fixturenames:
        resolutions, _ = _discover_common_all_resolutions()
        ids = [f"{w}x{h}" for w, h in resolutions]
        metafunc.parametrize("tri_resolution", resolutions, ids=ids)
