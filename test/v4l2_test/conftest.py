"""Pytest fixtures for D4XX V4L2 native tests.

Provides camera discovery, device open/close, and report plugin registration.
"""

import pytest

from .d4xx.discovery import discover_cameras
from .d4xx import constants as C
from .v4l2.device import V4L2Device
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
        ctrl = dev.get_ctrl(C.DS5_CAMERA_CID_FW_VERSION)
        raw = ctrl.value
        major = (raw >> 24) & 0xFF
        minor = (raw >> 16) & 0xFF
        patch = (raw >> 8) & 0xFF
        build = raw & 0xFF
        return raw, f"{major}.{minor}.{patch}.{build}"


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
