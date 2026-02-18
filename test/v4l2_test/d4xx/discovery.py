"""Scan /dev/video*, identify D4XX cameras by QUERYCAP or symlinks, group devices."""

import glob
import os
import re
from dataclasses import dataclass, field
from typing import List, Optional

from ..v4l2.device import V4L2Device
from ..v4l2 import ioctls
from . import constants as C

# Symlink patterns created by the D4XX driver udev rules on Tegra platforms.
# Format: /dev/video-rs-{role}-{camera_index}
_SYMLINK_ROLES = {
    "depth":    C.STREAM_DEPTH,
    "depth-md": C.STREAM_DEPTH_MD,
    "color":    C.STREAM_RGB,
    "color-md": C.STREAM_RGB_MD,
    "ir":       C.STREAM_IR,
    "imu":      C.STREAM_IMU,
}


@dataclass
class D4xxCamera:
    """Represents a discovered D4XX camera with its video device nodes."""
    base_index: int
    devices: List[str]
    driver: str = ""
    card: str = ""
    bus_info: str = ""
    fw_version: Optional[str] = None
    fw_version_raw: Optional[bytes] = None

    @property
    def depth_path(self):
        return self.devices[C.STREAM_DEPTH]

    @property
    def depth_md_path(self):
        return self.devices[C.STREAM_DEPTH_MD]

    @property
    def rgb_path(self):
        return self.devices[C.STREAM_RGB]

    @property
    def rgb_md_path(self):
        return self.devices[C.STREAM_RGB_MD]

    @property
    def ir_path(self):
        return self.devices[C.STREAM_IR]

    @property
    def imu_path(self):
        return self.devices[C.STREAM_IMU]


def _video_index(path):
    """Extract numeric index from /dev/videoN."""
    m = re.search(r"video(\d+)$", path)
    return int(m.group(1)) if m else -1


def _read_fw_version(device_path):
    """Read firmware version from the depth device node."""
    try:
        with V4L2Device(device_path) as dev:
            ctrl = dev.get_ctrl(C.DS5_CAMERA_CID_FW_VERSION)
            raw = ctrl.value
            # FW version is packed as 4 bytes in a 32-bit int
            major = (raw >> 24) & 0xFF
            minor = (raw >> 16) & 0xFF
            patch = (raw >> 8) & 0xFF
            build = raw & 0xFF
            return f"{major}.{minor}.{patch}.{build}", raw.to_bytes(4, "big")
    except (OSError, Exception):
        return None, None


def _discover_via_symlinks():
    """Discover cameras using /dev/video-rs-* symlinks (Tegra platforms).

    Returns a list of D4xxCamera or empty list if symlinks not found.
    """
    symlinks = sorted(glob.glob("/dev/video-rs-*"))
    if not symlinks:
        return []

    # Group symlinks by camera index: {cam_idx: {stream_idx: path}}
    cam_map = {}
    for link in symlinks:
        basename = os.path.basename(link)  # e.g. "video-rs-depth-0"
        m = re.match(r"video-rs-(.+)-(\d+)$", basename)
        if not m:
            continue
        role, cam_idx = m.group(1), int(m.group(2))
        if role not in _SYMLINK_ROLES:
            continue
        stream_idx = _SYMLINK_ROLES[role]
        cam_map.setdefault(cam_idx, {})[stream_idx] = os.path.realpath(link)

    cameras = []
    for cam_idx in sorted(cam_map):
        streams = cam_map[cam_idx]
        # Need at least depth to be useful
        if C.STREAM_DEPTH not in streams:
            continue

        # Build ordered device list; use empty string for missing streams
        devices = [streams.get(i, "") for i in range(C.DEVICES_PER_CAMERA)]

        depth_path = devices[C.STREAM_DEPTH]
        driver = card = bus_info = ""
        try:
            with V4L2Device(depth_path) as dev:
                cap = dev.query_cap()
                driver = cap.driver.split(b"\x00")[0].decode("ascii", errors="replace")
                card = cap.card.split(b"\x00")[0].decode("ascii", errors="replace")
                bus_info = cap.bus_info.split(b"\x00")[0].decode("ascii", errors="replace")
        except (OSError, Exception):
            pass

        cam = D4xxCamera(
            base_index=_video_index(depth_path),
            devices=devices,
            driver=driver,
            card=card,
            bus_info=bus_info,
        )
        fw_str, fw_raw = _read_fw_version(depth_path)
        cam.fw_version = fw_str
        cam.fw_version_raw = fw_raw
        cameras.append(cam)

    return cameras


def _discover_via_querycap():
    """Discover cameras by scanning /dev/video* and matching D4XX driver name."""
    video_paths = sorted(glob.glob("/dev/video*"), key=_video_index)
    if not video_paths:
        return []

    d4xx_paths = []
    for path in video_paths:
        if not re.search(r"video\d+$", path):
            continue
        try:
            with V4L2Device(path) as dev:
                cap = dev.query_cap()
                driver = cap.driver.split(b"\x00")[0]
                if driver == C.D4XX_DRIVER_NAME:
                    d4xx_paths.append((path, cap))
        except (OSError, Exception):
            continue

    if not d4xx_paths:
        return []

    cameras = []
    for i in range(0, len(d4xx_paths), C.DEVICES_PER_CAMERA):
        group = d4xx_paths[i:i + C.DEVICES_PER_CAMERA]
        if len(group) < C.DEVICES_PER_CAMERA:
            break

        paths = [p for p, _ in group]
        cap = group[0][1]
        base_idx = _video_index(paths[0])

        cam = D4xxCamera(
            base_index=base_idx,
            devices=paths,
            driver=cap.driver.split(b"\x00")[0].decode("ascii", errors="replace"),
            card=cap.card.split(b"\x00")[0].decode("ascii", errors="replace"),
            bus_info=cap.bus_info.split(b"\x00")[0].decode("ascii", errors="replace"),
        )

        fw_str, fw_raw = _read_fw_version(paths[0])
        cam.fw_version = fw_str
        cam.fw_version_raw = fw_raw
        cameras.append(cam)

    return cameras


def discover_cameras():
    """Discover all connected D4XX cameras.

    First tries /dev/video-rs-* symlinks (Tegra platforms with udev rules).
    Falls back to scanning /dev/video* with QUERYCAP driver name matching.
    """
    cameras = _discover_via_symlinks()
    if cameras:
        return cameras
    return _discover_via_querycap()
