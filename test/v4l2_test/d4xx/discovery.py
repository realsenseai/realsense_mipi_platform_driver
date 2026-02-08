"""Scan /dev/video*, identify D4XX cameras by QUERYCAP, group by 6."""

import glob
import os
import re
from dataclasses import dataclass, field
from typing import List, Optional

from ..v4l2.device import V4L2Device
from ..v4l2 import ioctls
from . import constants as C


@dataclass
class D4xxCamera:
    """Represents a discovered D4XX camera with its 6 video device nodes."""
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


def discover_cameras():
    """Discover all connected D4XX cameras.

    Scans /dev/video* devices, uses QUERYCAP to identify D4XX driver,
    and groups consecutive devices into cameras (6 devices per camera).
    """
    video_paths = sorted(glob.glob("/dev/video*"), key=_video_index)
    if not video_paths:
        return []

    # Find all D4XX device nodes
    d4xx_paths = []
    for path in video_paths:
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

    # Group into cameras by consecutive groups of DEVICES_PER_CAMERA
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

        # Read FW version from depth device
        fw_str, fw_raw = _read_fw_version(paths[0])
        cam.fw_version = fw_str
        cam.fw_version_raw = fw_raw

        cameras.append(cam)

    return cameras
