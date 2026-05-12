#!/usr/bin/env python3
"""
Multi-camera Depth+RGB sync stability test for mixed D457/D401 setups.

Streams Depth (Z16, 640×480 @ 30 FPS) and, for cameras with a discovered
RGB node, also RGB concurrently from all discovered D4XX cameras, then validates
per-stream stability and cross-camera depth sync.

Camera model detection is automatic via GVD active-projector field:
    - D457 (D45X, has projector): sync_mode=3 (Full Slave)
    - D401 (D40X, no projector):  sync_mode=2 (Slave)

RGB validation is enabled per camera whenever the driver exposes a usable
color video node, regardless of model.

Pass criteria per camera:
  - Depth arrival rate >= 90 % of requested frames
  - No more than 2 consecutive frame drops (sequence gap <= 3)
    - [RGB-capable cameras] RGB arrival rate >= 90 %
    - [RGB-capable cameras] Depth-RGB timestamp drift: median <= 2 ms AND >= 95 % within 2 ms

Cross-camera depth sync (PASS/FAIL, skips first 30 frames of startup transient):
  - Nearest-timestamp matching across all cameras per frame slot
  - PASS  : median spread <= 2 ms AND >= 95 % frames within 2 ms
  - WARN  : some pairs valid but below threshold (partial alignment)
  - No-sync: < 20 % valid pairs (cameras free-running, no ext trigger)

Usage:
    python3 test_quad_depth_rgb_sync.py [--frames N] [--num-cameras N]
                                        [--no-reset]
                                        [--cross-cam-threshold MS]

Exit code 0 if all per-camera checks pass, 1 otherwise.
(Cross-camera sync failure raises exit code to 2.)
"""

import argparse
import ctypes
import fcntl
import mmap
import os
import re
import select
import statistics
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

# ─────────────────────────────────────────────────────────────────────────────
# V4L2 ioctl infrastructure
# ─────────────────────────────────────────────────────────────────────────────
_IOC_NRBITS   = 8
_IOC_TYPEBITS = 8
_IOC_SIZEBITS = 14
_IOC_DIRBITS  = 2
_IOC_NRSHIFT   = 0
_IOC_TYPESHIFT = _IOC_NRSHIFT   + _IOC_NRBITS
_IOC_SIZESHIFT = _IOC_TYPESHIFT + _IOC_TYPEBITS
_IOC_DIRSHIFT  = _IOC_SIZESHIFT + _IOC_SIZEBITS
_IOC_NONE  = 0
_IOC_WRITE = 1
_IOC_READ  = 2


def _IOC(direction, type_, nr, size):
    return ((direction << _IOC_DIRSHIFT) | (ord(type_) << _IOC_TYPESHIFT)
            | (nr << _IOC_NRSHIFT) | (size << _IOC_SIZESHIFT))


def _IOR(t, n, s):  return _IOC(_IOC_READ,           t, n, s)
def _IOW(t, n, s):  return _IOC(_IOC_WRITE,          t, n, s)
def _IOWR(t, n, s): return _IOC(_IOC_READ|_IOC_WRITE, t, n, s)


def _fourcc(s: str) -> int:
    s = (s + '    ')[:4]
    return ord(s[0]) | (ord(s[1]) << 8) | (ord(s[2]) << 16) | (ord(s[3]) << 24)


# ─────────────────────────────────────────────────────────────────────────────
# ctypes structs (matching kernel ABI; adapted from test/v4l2_test/v4l2/structs.py)
# ─────────────────────────────────────────────────────────────────────────────
class v4l2_capability(ctypes.Structure):
    _fields_ = [
        ("driver",       ctypes.c_char * 16),
        ("card",         ctypes.c_char * 32),
        ("bus_info",     ctypes.c_char * 32),
        ("version",      ctypes.c_uint32),
        ("capabilities", ctypes.c_uint32),
        ("device_caps",  ctypes.c_uint32),
        ("reserved",     ctypes.c_uint32 * 3),
    ]


class v4l2_fmtdesc(ctypes.Structure):
    _fields_ = [
        ("index",       ctypes.c_uint32),
        ("type",        ctypes.c_uint32),
        ("flags",       ctypes.c_uint32),
        ("description", ctypes.c_char * 32),
        ("pixelformat", ctypes.c_uint32),
        ("mbus_code",   ctypes.c_uint32),
        ("reserved",    ctypes.c_uint32 * 3),
    ]


class v4l2_pix_format(ctypes.Structure):
    _fields_ = [
        ("width",           ctypes.c_uint32),
        ("height",          ctypes.c_uint32),
        ("pixelformat",     ctypes.c_uint32),
        ("field",           ctypes.c_uint32),
        ("bytesperline",    ctypes.c_uint32),
        ("sizeimage",       ctypes.c_uint32),
        ("colorspace",      ctypes.c_uint32),
        ("priv",            ctypes.c_uint32),
        ("flags",           ctypes.c_uint32),
        ("ycbcr_enc_or_hsv_enc", ctypes.c_uint32),
        ("quantization",    ctypes.c_uint32),
        ("xfer_func",       ctypes.c_uint32),
    ]


class _v4l2_format_fmt(ctypes.Union):
    _fields_ = [
        ("pix",      v4l2_pix_format),
        ("raw_data", ctypes.c_uint8 * 200),
        # v4l2_window (not exposed here) contains pointer members on aarch64 →
        # forces the union to 8-byte alignment, giving v4l2_format total = 208. */
        ("_align",   ctypes.c_uint64),
    ]


class v4l2_format(ctypes.Structure):
    _fields_ = [
        ("type", ctypes.c_uint32),
        ("fmt",  _v4l2_format_fmt),
    ]


class v4l2_requestbuffers(ctypes.Structure):
    _fields_ = [
        ("count",        ctypes.c_uint32),
        ("type",         ctypes.c_uint32),
        ("memory",       ctypes.c_uint32),
        ("capabilities", ctypes.c_uint32),
        ("flags",        ctypes.c_uint8),
        ("reserved",     ctypes.c_uint8 * 3),
    ]


class v4l2_timeval(ctypes.Structure):
    _fields_ = [
        ("tv_sec",  ctypes.c_long),
        ("tv_usec", ctypes.c_long),
    ]


class v4l2_timecode(ctypes.Structure):
    _fields_ = [
        ("type",    ctypes.c_uint32),
        ("flags",   ctypes.c_uint32),
        ("frames",  ctypes.c_uint8),
        ("seconds", ctypes.c_uint8),
        ("minutes", ctypes.c_uint8),
        ("hours",   ctypes.c_uint8),
        ("userbits", ctypes.c_uint8 * 4),
    ]


class _v4l2_buffer_m(ctypes.Union):
    _fields_ = [
        ("offset",  ctypes.c_uint32),
        ("userptr", ctypes.c_ulong),
        ("planes",  ctypes.c_void_p),
        ("fd",      ctypes.c_int32),
    ]


class v4l2_buffer(ctypes.Structure):
    _fields_ = [
        ("index",               ctypes.c_uint32),
        ("type",                ctypes.c_uint32),
        ("bytesused",           ctypes.c_uint32),
        ("flags",               ctypes.c_uint32),
        ("field",               ctypes.c_uint32),
        ("timestamp",           v4l2_timeval),
        ("timecode",            v4l2_timecode),
        ("sequence",            ctypes.c_uint32),
        ("memory",              ctypes.c_uint32),
        ("m",                   _v4l2_buffer_m),
        ("length",              ctypes.c_uint32),
        ("reserved2",           ctypes.c_uint32),
        ("request_fd_or_reserved", ctypes.c_int32),
    ]


# Compute ioctl numbers from actual struct sizes (matches kernel exactly)
_sz = ctypes.sizeof
VIDIOC_QUERYCAP  = _IOR  ("V",  0, _sz(v4l2_capability))
VIDIOC_ENUM_FMT  = _IOWR ("V",  2, _sz(v4l2_fmtdesc))
VIDIOC_S_FMT     = _IOWR ("V",  5, _sz(v4l2_format))
VIDIOC_REQBUFS   = _IOWR ("V",  8, _sz(v4l2_requestbuffers))
VIDIOC_QUERYBUF  = _IOWR ("V",  9, _sz(v4l2_buffer))
VIDIOC_QBUF      = _IOWR ("V", 15, _sz(v4l2_buffer))
VIDIOC_DQBUF     = _IOWR ("V", 17, _sz(v4l2_buffer))
VIDIOC_STREAMON  = _IOW  ("V", 18, _sz(ctypes.c_int))
VIDIOC_STREAMOFF = _IOW  ("V", 19, _sz(ctypes.c_int))

V4L2_BUF_TYPE_VIDEO_CAPTURE = 1
V4L2_MEMORY_MMAP            = 1
V4L2_FIELD_ANY              = 0

# FourCC constants
PIX_FMT_Z16  = _fourcc("Z16 ")
PIX_FMT_YUYV = _fourcc("YUYV")
PIX_FMT_UYVY = _fourcc("UYVY")
PIX_FMT_BGR3 = _fourcc("BGR3")
PIX_FMT_RGB3 = _fourcc("RGB3")

# D4XX camera sync mode and HW reset CIDs
_V4L2_CTRL_CLASS_CAMERA  = 0x009a0000
_DS5_DEPTH_STREAM_DT     = 0x4000
_DS5_CAMERA_CID_BASE     = _V4L2_CTRL_CLASS_CAMERA | _DS5_DEPTH_STREAM_DT
DS5_CAMERA_CID_GVD       = _DS5_CAMERA_CID_BASE | 8
DS5_CAMERA_CID_SYNC_MODE = _DS5_CAMERA_CID_BASE | 16
DS5_CAMERA_CID_HW_RESET  = _DS5_CAMERA_CID_BASE | 33

# GVD layout offsets (from librealsense d400-private.h)
# All GVD-data offsets are +4 to account for HWMC response header.
_GVD_MODULE_SERIAL_OFFSET     = 52   # 48 + 4-byte header
_GVD_SERIAL_SIZE              = 6    # 6 bytes, each printed as 2-digit hex
_GVD_ACTIVE_PROJECTOR_OFFSET  = 174  # 170 + 4-byte header (1 byte: 0=no, 1=yes)
_GVD_SIZE                     = 239

# Per-model sync modes (set automatically based on detected camera model)
SYNC_MODE_D457 = 3   # Full Slave
SYNC_MODE_D401 = 2   # Slave

# V4L2 extended controls structures (for reading GVD blob)
class _v4l2_ext_control_u(ctypes.Union):
    _pack_ = 4
    _fields_ = [
        ("value",   ctypes.c_int32),
        ("value64", ctypes.c_int64),
        ("string",  ctypes.c_char_p),
        ("p_u8",    ctypes.c_void_p),
        ("p_u16",   ctypes.c_void_p),
        ("p_u32",   ctypes.c_void_p),
        ("ptr",     ctypes.c_void_p),
    ]

class _v4l2_ext_control(ctypes.Structure):
    _pack_ = 4
    _fields_ = [
        ("id",        ctypes.c_uint32),
        ("size",      ctypes.c_uint32),
        ("reserved2", ctypes.c_uint32 * 1),
        ("_u",        _v4l2_ext_control_u),
    ]
    _anonymous_ = ("_u",)

class _v4l2_ext_controls_u(ctypes.Union):
    _fields_ = [
        ("ctrl_class", ctypes.c_uint32),
        ("which",      ctypes.c_uint32),
    ]

class _v4l2_ext_controls(ctypes.Structure):
    _fields_ = [
        ("_u",         _v4l2_ext_controls_u),
        ("count",      ctypes.c_uint32),
        ("error_idx",  ctypes.c_uint32),
        ("request_fd", ctypes.c_int32),
        ("reserved",   ctypes.c_uint32 * 1),
        ("controls",   ctypes.POINTER(_v4l2_ext_control)),
    ]
    _anonymous_ = ("_u",)

VIDIOC_G_EXT_CTRLS = _IOWR("V", 71, _sz(_v4l2_ext_controls))

DEVICES_PER_CAMERA = 6
STREAM_DEPTH = 0
STREAM_RGB   = 2

D4XX_DRIVER = b"d4xx"

# ─────────────────────────────────────────────────────────────────────────────
# Camera discovery
# ─────────────────────────────────────────────────────────────────────────────
@dataclass
class D4xxCamera:
    index: int          # camera index (0-based)
    depth_path: str
    rgb_path: str
    rgb_pixfmt: int = PIX_FMT_YUYV   # negotiated at discovery time
    card: str = ""
    serial: str = ""
    model: str = "D457"   # "D457" (D45X, has projector) or "D401" (D40X, no projector)

    @property
    def has_rgb(self) -> bool:
        """RGB validation follows the discovered color node, not camera model."""
        return bool(self.rgb_path)

    @property
    def sync_mode(self) -> int:
        return SYNC_MODE_D457 if self.model != "D401" else SYNC_MODE_D401


def _read_gvd(depth_path: str) -> Optional[bytes]:
    """Read full GVD blob from the depth device. Returns None on failure."""
    try:
        fd = os.open(depth_path, os.O_RDWR | os.O_NONBLOCK)
    except OSError:
        return None
    try:
        buf = (ctypes.c_uint8 * _GVD_SIZE)()
        ext = _v4l2_ext_control()
        ext.id = DS5_CAMERA_CID_GVD
        ext.size = _GVD_SIZE
        ext.p_u8 = ctypes.addressof(buf)
        ctrls = _v4l2_ext_controls()
        ctrls.ctrl_class = _V4L2_CTRL_CLASS_CAMERA
        ctrls.count = 1
        ctrls.controls = ctypes.pointer(ext)
        fcntl.ioctl(fd, VIDIOC_G_EXT_CTRLS, ctrls)
        return bytes(buf)
    except OSError:
        return None
    finally:
        os.close(fd)


def _get_serial(depth_path: str, card: str = "", gvd: Optional[bytes] = None) -> str:
    """Extract camera serial from GVD ASIC serial field."""
    if gvd is None:
        gvd = _read_gvd(depth_path)
    if gvd and len(gvd) > _GVD_MODULE_SERIAL_OFFSET + _GVD_SERIAL_SIZE:
        raw = gvd[_GVD_MODULE_SERIAL_OFFSET:
                  _GVD_MODULE_SERIAL_OFFSET + _GVD_SERIAL_SIZE]
        if any(b != 0xff and b != 0x00 for b in raw):
            return "".join(f"{b:02x}" for b in raw)
    return _serial_from_card(card)


def _get_model(gvd: Optional[bytes]) -> str:
    """Detect camera model from GVD active_projector field.

    D457 (D45X) has an active laser projector (value 1).
    D401 (D40X) has no projector (value 0).
    Falls back to "D457" if GVD is unavailable.
    """
    if gvd and len(gvd) > _GVD_ACTIVE_PROJECTOR_OFFSET:
        return "D457" if gvd[_GVD_ACTIVE_PROJECTOR_OFFSET] else "D401"
    return "D457"


def _serial_from_card(card: str) -> str:
    """Extract I2C bus-addr from card name as fallback camera identifier."""
    if card:
        m = re.search(r"(\d+-[0-9a-fA-F]{4})", card)
        if m:
            return m.group(1)
    return "unknown"


def _video_index(path: str) -> int:
    m = re.search(r"video(\d+)$", path)
    return int(m.group(1)) if m else -1


def _querycap(path: str) -> Optional[v4l2_capability]:
    try:
        fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)
    except OSError:
        return None
    try:
        cap = v4l2_capability()
        fcntl.ioctl(fd, VIDIOC_QUERYCAP, cap)
        return cap
    except OSError:
        return None
    finally:
        os.close(fd)


def _enum_formats(path: str) -> List[int]:
    """Return list of pixelformat FourCCs supported by the device."""
    try:
        fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)
    except OSError:
        return []
    try:
        fmts = []
        idx = 0
        while True:
            desc = v4l2_fmtdesc()
            desc.index = idx
            desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE
            try:
                fcntl.ioctl(fd, VIDIOC_ENUM_FMT, desc)
            except OSError:
                break
            fmts.append(desc.pixelformat)
            idx += 1
        return fmts
    finally:
        os.close(fd)


def discover_cameras() -> List[D4xxCamera]:
    """Discover D4XX cameras.

    Strategy 1 (preferred): use /dev/video-rs-{role}-{N} symlinks created by
    the D4XX driver's udev rules on Tegra platforms.
    Strategy 2 (fallback): scan /dev/video*, group consecutive d4xx/tegra-video
    nodes that report 'DS5' in their card name into sets of DEVICES_PER_CAMERA.
    """
    cameras = _discover_via_symlinks()
    if not cameras:
        cameras = _discover_via_querycap()
    return cameras


def _discover_via_symlinks() -> List[D4xxCamera]:
    """Group /dev/video-rs-depth-N and /dev/video-rs-color-N into cameras."""
    import glob
    depth_links = sorted(glob.glob("/dev/video-rs-depth-[0-9]*"))
    if not depth_links:
        return []

    cameras: List[D4xxCamera] = []
    for link in depth_links:
        m = re.search(r"video-rs-depth-(\d+)$", link)
        if not m:
            continue
        cam_idx = int(m.group(1))
        depth_path = os.path.realpath(link)

        color_link = f"/dev/video-rs-color-{cam_idx}"
        rgb_path = ""
        rgb_pixfmt = PIX_FMT_YUYV
        if os.path.exists(color_link):
            rgb_path = os.path.realpath(color_link)
            rgb_fmts = _enum_formats(rgb_path)
            if rgb_fmts:
                preferred = [PIX_FMT_YUYV, PIX_FMT_UYVY, PIX_FMT_BGR3, PIX_FMT_RGB3]
                rgb_pixfmt = next((f for f in preferred if f in rgb_fmts), rgb_fmts[0])
            else:
                rgb_path = ""

        cap = _querycap(depth_path)
        card = cap.card.rstrip(b"\x00").decode("ascii", errors="replace") if cap else ""

        gvd = _read_gvd(depth_path)
        serial = _get_serial(depth_path, card, gvd)
        model = _get_model(gvd)

        cameras.append(D4xxCamera(
            index=cam_idx,
            depth_path=depth_path,
            rgb_path=rgb_path,
            rgb_pixfmt=rgb_pixfmt,
            card=card,
            serial=serial,
            model=model,
        ))

    return sorted(cameras, key=lambda c: c.index)


# DS5 card name substring used when falling back to QUERYCAP scan
_DS5_CARD_MARKER = b"DS5"
_TEGRA_DRIVER    = b"tegra-video"


def _discover_via_querycap() -> List[D4xxCamera]:
    """Fallback: scan /dev/video*, group 6 consecutive DS5 tegra-video nodes."""
    all_paths = sorted(
        (p for p in (os.path.join("/dev", e) for e in os.listdir("/dev"))
         if re.search(r"^video\d+$", os.path.basename(p))),
        key=_video_index,
    )

    ds5_nodes: List[str] = []
    for path in all_paths:
        cap = _querycap(path)
        if cap is None:
            continue
        driver = cap.driver.rstrip(b"\x00")
        card   = cap.card
        if (driver in (D4XX_DRIVER, _TEGRA_DRIVER)
                and _DS5_CARD_MARKER in card):
            ds5_nodes.append(path)

    cameras: List[D4xxCamera] = []
    i = 0
    while i + DEVICES_PER_CAMERA <= len(ds5_nodes):
        group      = ds5_nodes[i:i + DEVICES_PER_CAMERA]
        depth_path = group[STREAM_DEPTH]
        rgb_path   = group[STREAM_RGB]
        rgb_pixfmt = PIX_FMT_YUYV
        rgb_fmts   = _enum_formats(rgb_path)
        if rgb_fmts:
            preferred  = [PIX_FMT_YUYV, PIX_FMT_UYVY, PIX_FMT_BGR3, PIX_FMT_RGB3]
            rgb_pixfmt = next((f for f in preferred if f in rgb_fmts), rgb_fmts[0])
        else:
            rgb_path = ""

        cap  = _querycap(depth_path)
        card = cap.card.rstrip(b"\x00").decode("ascii", errors="replace") if cap else ""
        gvd = _read_gvd(depth_path)
        serial = _get_serial(depth_path, card, gvd)
        model = _get_model(gvd)
        cameras.append(D4xxCamera(
            index=len(cameras),
            depth_path=depth_path,
            rgb_path=rgb_path,
            rgb_pixfmt=rgb_pixfmt,
            card=card,
            serial=serial,
            model=model,
        ))
        i += DEVICES_PER_CAMERA

    return cameras


# ─────────────────────────────────────────────────────────────────────────────
# Sync mode setup
# ─────────────────────────────────────────────────────────────────────────────
def set_sync_mode(camera: D4xxCamera, mode: int) -> bool:
    """Set camera sync mode via v4l2-ctl. Returns True on success."""
    result = subprocess.run(
        ["v4l2-ctl", "-d", camera.depth_path,
         "-c", f"camera_sync_mode={mode}"],
        capture_output=True,
    )
    if result.returncode != 0:
        print(f"  [WARN] cam{camera.index}: failed to set sync_mode={mode}: "
              f"{result.stderr.decode().strip()}")
        return False
    return True


def hw_reset_camera(camera: D4xxCamera) -> bool:
    """Send HW reset to a camera via v4l2-ctl. Returns True on success."""
    result = subprocess.run(
        ["v4l2-ctl", "-d", camera.depth_path, "-c", "hw_reset=0"],
        capture_output=True,
    )
    return result.returncode == 0


def hw_reset_all(cameras: List[D4xxCamera], wait_s: float = 4.0) -> None:
    """Reset all cameras and wait for FW to come back up."""
    print(f"  Sending HW reset to {len(cameras)} cameras...")
    for cam in cameras:
        ok = hw_reset_camera(cam)
        print(f"    cam{cam.index}: {'OK' if ok else 'WARN (reset may have failed)'}")
    print(f"  Waiting {wait_s:.0f}s for FW restart...", end="", flush=True)
    time.sleep(wait_s)
    print(" done")


# ─────────────────────────────────────────────────────────────────────────────
# Low-level streaming
# ─────────────────────────────────────────────────────────────────────────────
FrameInfo = Tuple[int, int]   # (sequence, timestamp_us)

BUF_COUNT = 4


def _open_stream(path: str, width: int, height: int, pixfmt: int
                 ) -> Tuple[int, List[Tuple[v4l2_buffer, mmap.mmap]]]:
    """Open device, set format, allocate mmap buffers. Returns (fd, buffers)."""
    fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)

    fmt = v4l2_format()
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE
    fmt.fmt.pix.width = width
    fmt.fmt.pix.height = height
    fmt.fmt.pix.pixelformat = pixfmt
    fmt.fmt.pix.field = V4L2_FIELD_ANY
    fcntl.ioctl(fd, VIDIOC_S_FMT, fmt)

    req = v4l2_requestbuffers()
    req.count  = BUF_COUNT
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE
    req.memory = V4L2_MEMORY_MMAP
    fcntl.ioctl(fd, VIDIOC_REQBUFS, req)

    buffers: List[Tuple[v4l2_buffer, mmap.mmap]] = []
    for i in range(req.count):
        buf = v4l2_buffer()
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE
        buf.memory = V4L2_MEMORY_MMAP
        buf.index  = i
        fcntl.ioctl(fd, VIDIOC_QUERYBUF, buf)

        mm = mmap.mmap(fd, buf.length, mmap.MAP_SHARED,
                       mmap.PROT_READ | mmap.PROT_WRITE,
                       offset=buf.m.offset)
        buffers.append((buf, mm))

        qbuf = v4l2_buffer()
        qbuf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE
        qbuf.memory = V4L2_MEMORY_MMAP
        qbuf.index  = i
        fcntl.ioctl(fd, VIDIOC_QBUF, qbuf)

    return fd, buffers


def _close_stream(fd: int, buffers: List[Tuple[v4l2_buffer, mmap.mmap]]) -> None:
    try:
        buf_type = ctypes.c_int(V4L2_BUF_TYPE_VIDEO_CAPTURE)
        fcntl.ioctl(fd, VIDIOC_STREAMOFF, buf_type)
    except OSError:
        pass
    for _, mm in buffers:
        mm.close()
    # Release kernel buffers
    req = v4l2_requestbuffers()
    req.count  = 0
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE
    req.memory = V4L2_MEMORY_MMAP
    try:
        fcntl.ioctl(fd, VIDIOC_REQBUFS, req)
    except OSError:
        pass
    os.close(fd)


def _capture_frames(fd: int, buffers: List[Tuple[v4l2_buffer, mmap.mmap]],
                    n_frames: int, timeout_s: float = 5.0,
                    stop_event: Optional[threading.Event] = None) -> List[FrameInfo]:
    """Capture n_frames from an already-started stream. Returns (seq, ts_us) list."""
    frames: List[FrameInfo] = []
    for _ in range(n_frames):
        deadline = time.monotonic() + timeout_s
        while True:
            if stop_event and stop_event.is_set():
                return frames
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return frames  # timeout — stop collecting, report partial result
            ready, _, _ = select.select([fd], [], [], min(0.25, remaining))
            if ready:
                break

        dqbuf = v4l2_buffer()
        dqbuf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE
        dqbuf.memory = V4L2_MEMORY_MMAP
        fcntl.ioctl(fd, VIDIOC_DQBUF, dqbuf)

        ts_us = (dqbuf.timestamp.tv_sec * 1_000_000
                 + dqbuf.timestamp.tv_usec)
        frames.append((dqbuf.sequence, ts_us))

        # Re-queue
        qbuf = v4l2_buffer()
        qbuf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE
        qbuf.memory = V4L2_MEMORY_MMAP
        qbuf.index  = dqbuf.index
        fcntl.ioctl(fd, VIDIOC_QBUF, qbuf)

    return frames


# ─────────────────────────────────────────────────────────────────────────────
# Per-stream thread  (two-phase: setup → wait → capture)
# ─────────────────────────────────────────────────────────────────────────────
def _stream_thread(
    path: str,
    width: int,
    height: int,
    pixfmt: int,
    n_frames: int,
    out: dict,
    setup_done: threading.Event,
    capture_start: threading.Event,
    stop_event: threading.Event,
) -> None:
    """
    Phase 1 (parallel): open device, set format, allocate mmap buffers.
                        Store fd in out['fd'] and signal setup_done.
    Phase 2 (after main thread calls STREAMON): capture frames.

    Separating setup from STREAMON lets the main thread call STREAMON on all
    8 devices sequentially, avoiding Tegra VI resource-allocation races.
    """
    try:
        fd, buffers = _open_stream(path, width, height, pixfmt)
        out["fd"]      = fd
        out["buffers"] = buffers
    except Exception as exc:
        out["error"] = f"setup failed: {exc}"
        setup_done.set()
        return

    setup_done.set()      # signal: mmap ready, fd open
    capture_start.wait()  # wait until main thread has called STREAMON

    fd      = out.get("fd")
    buffers = out.get("buffers")
    if fd is None:
        out["error"] = "fd lost before capture"
        return
    try:
        out["frames"] = _capture_frames(fd, buffers, n_frames, timeout_s=5.0,
                                        stop_event=stop_event)
    except Exception as exc:
        out["error"] = f"capture failed: {exc}"


# ─────────────────────────────────────────────────────────────────────────────
# Validation helpers
# ─────────────────────────────────────────────────────────────────────────────
@dataclass
class StreamStats:
    label: str
    arrived: int
    requested: int
    max_gap: int        # max sequence gap (1 = no drop, 3 = 2 consecutive drops)
    arrival_pct: float
    error: str = ""

    @property
    def ok(self) -> bool:
        return (not self.error
                and self.arrival_pct >= 0.90
                and self.max_gap <= 3)


@dataclass
class SyncStats:
    cam_index: int
    n_pairs: int          # total timestamp-matched pairs
    n_valid: int          # pairs within MAX_PAIRING_GAP_MS (excludes startup skew)
    drift_median_ms: float
    drift_max_ms: float
    pct_within_2ms: float

    @property
    def ok(self) -> bool:
        return (self.n_valid >= 30               # at least 1s of valid data
                and self.drift_median_ms <= 2.0
                and self.pct_within_2ms >= 0.95)


# Pairs with drift > this are start-up skew artifacts (depth and RGB started
# at different V-sync boundaries) and are excluded from sync statistics.
MAX_PAIRING_GAP_MS = 50.0   # ~1.5 frame periods at 30 fps


def validate_stream(label: str, frames: List[FrameInfo], requested: int) -> StreamStats:
    arrived = len(frames)
    arrival_pct = arrived / requested if requested > 0 else 0.0

    max_gap = 1
    if len(frames) >= 2:
        seqs = [f[0] for f in frames]
        for i in range(1, len(seqs)):
            gap = seqs[i] - seqs[i - 1]
            if gap > max_gap:
                max_gap = gap

    return StreamStats(
        label=label,
        arrived=arrived,
        requested=requested,
        max_gap=max_gap,
        arrival_pct=arrival_pct,
    )


def analyse_intra_sync(cam_index: int,
                       depth_frames: List[FrameInfo],
                       rgb_frames: List[FrameInfo]) -> SyncStats:
    """Match depth and RGB frames by closest timestamp, then compute drift.

    Pairing by index is incorrect when the two streams start at different
    V-sync boundaries.  We sort both lists by timestamp and use a two-pointer
    walk to pair each depth frame with the nearest-in-time RGB frame.
    """
    if not depth_frames or not rgb_frames:
        return SyncStats(cam_index, 0, 0, float("inf"), float("inf"), 0.0)

    depth_ts = sorted(f[1] for f in depth_frames)
    rgb_ts   = sorted(f[1] for f in rgb_frames)

    drifts_us: List[float] = []
    j = 0
    for d_ts in depth_ts:
        # Advance j while the next RGB timestamp is closer to d_ts
        while j < len(rgb_ts) - 1 and abs(rgb_ts[j + 1] - d_ts) < abs(rgb_ts[j] - d_ts):
            j += 1
        drifts_us.append(abs(rgb_ts[j] - d_ts))

    drifts_ms   = [d / 1000.0 for d in drifts_us]
    # Exclude start-up skew pairs (depth/RGB start at different V-sync boundaries)
    valid       = [d for d in drifts_ms if d <= MAX_PAIRING_GAP_MS]
    n_total     = len(drifts_ms)
    n_valid     = len(valid)

    if not valid:
        return SyncStats(cam_index, n_total, 0, float("inf"), float("inf"), 0.0)
    within_2ms  = sum(1 for d in valid if d <= 2.0)

    return SyncStats(
        cam_index=cam_index,
        n_pairs=n_total,
        n_valid=n_valid,
        drift_median_ms=statistics.median(valid),
        drift_max_ms=max(valid),
        pct_within_2ms=within_2ms / n_valid,
    )


@dataclass
class CrossCamStats:
    n_cams: int
    n_ref_frames: int         # total depth frames from reference camera (after skip)
    n_valid: int              # frames where spread <= one-frame threshold
    spread_median_ms: float
    spread_max_ms: float
    pct_within_threshold: float   # fraction of frames within 1-frame window
    threshold_ms: float

    @property
    def ok(self) -> bool:
        """PASS: median spread <= threshold AND >= 95 % pairs are valid."""
        return (self.n_valid > 0
                and self.spread_median_ms <= self.threshold_ms
                and self.pct_within_threshold >= 0.95)

    @property
    def has_sync(self) -> bool:
        """Any meaningful alignment detected (>= 20 % valid pairs)."""
        return self.n_ref_frames > 0 and self.pct_within_threshold >= 0.20


# Skip first N depth frames to ignore start-up phase differences between cameras.
_CROSS_CAM_SKIP_FRAMES = 30  # ~1 s at 30 fps


def analyse_cross_camera_sync(
    all_depth_frames: Dict[int, List[FrameInfo]],
    threshold_ms: float = 2.0,
) -> CrossCamStats:
    """
    For each depth frame of a reference camera (after startup skip), find the
    nearest-in-time depth frame from every other camera, then compute the
    per-slot spread (max_ts - min_ts across all cameras).

    Only slots where ALL cameras contribute a frame within `threshold_ms` of
    the reference are counted as 'valid' (i.e. cameras are actually aligned).
    """
    non_empty = {cam: sorted(f[1] for f in frames)
                 for cam, frames in all_depth_frames.items() if frames}
    n_cams = len(non_empty)

    if n_cams < 2:
        return CrossCamStats(n_cams, 0, 0, float("inf"), float("inf"), 0.0, threshold_ms)

    # Use the camera with the most frames as reference
    ref_cam = max(non_empty, key=lambda c: len(non_empty[c]))
    ref_ts  = non_empty[ref_cam][_CROSS_CAM_SKIP_FRAMES:]

    if not ref_ts:
        return CrossCamStats(n_cams, 0, 0, float("inf"), float("inf"), 0.0, threshold_ms)

    # Build sorted timestamp lists for every peer camera
    peers = {cam: ts for cam, ts in non_empty.items() if cam != ref_cam}

    spreads_all: List[float] = []
    spreads_valid: List[float] = []

    for r_ts in ref_ts:
        # For each peer camera find nearest timestamp
        slot_ts = [r_ts]
        skip_slot = False
        for ts_list in peers.values():
            # binary-search nearest
            import bisect
            idx = bisect.bisect_left(ts_list, r_ts)
            candidates = []
            if idx < len(ts_list):
                candidates.append(ts_list[idx])
            if idx > 0:
                candidates.append(ts_list[idx - 1])
            if not candidates:
                skip_slot = True
                break
            nearest = min(candidates, key=lambda t: abs(t - r_ts))
            slot_ts.append(nearest)

        if skip_slot:
            continue

        spread_ms = (max(slot_ts) - min(slot_ts)) / 1000.0
        spreads_all.append(spread_ms)
        if spread_ms <= threshold_ms:
            spreads_valid.append(spread_ms)

    n_total = len(spreads_all)
    n_valid = len(spreads_valid)
    pct     = n_valid / n_total if n_total else 0.0

    median_ms = statistics.median(spreads_valid) if spreads_valid else float("inf")
    max_ms    = max(spreads_valid)               if spreads_valid else float("inf")

    return CrossCamStats(
        n_cams=n_cams,
        n_ref_frames=n_total,
        n_valid=n_valid,
        spread_median_ms=median_ms,
        spread_max_ms=max_ms,
        pct_within_threshold=pct,
        threshold_ms=threshold_ms,
    )


# ─────────────────────────────────────────────────────────────────────────────
# Summary report
# ─────────────────────────────────────────────────────────────────────────────
def _pass_fail(ok: bool) -> str:
    return "PASS" if ok else "FAIL"


def print_summary(cameras: List[D4xxCamera],
                  stream_stats: Dict[Tuple[int, str], StreamStats],
                  sync_stats: Dict[int, SyncStats],
                  cross_cam: CrossCamStats,
                  check_sync: bool = True,
                  check_cross_cam: bool = True) -> Tuple[bool, bool]:
    """
    Print per-camera and cross-camera summary.
    Returns (per_camera_pass, cross_camera_pass).
    """
    per_cam_ok = True

    print("\n" + "=" * 103)
    print("MULTI-CAMERA DEPTH+RGB SYNC STABILITY REPORT")
    print("=" * 103)
    print(f"  {'Cam':<5} {'Model':<6} {'Depth Arr':>10} {'RGB Arr':>9} {'MaxGap':>8} "
          f"{'Pairs(v/t)':>11} {'Drift med':>11} {'Drift max':>10} {'≤2ms%':>7} {'Result':>8}")
    print("  " + "-" * 99)

    for cam in cameras:
        ci = cam.index
        ds = stream_stats.get((ci, "depth"))
        rs = stream_stats.get((ci, "rgb"))  # None for D401
        ss = sync_stats.get(ci)             # None for D401

        if ds is None:
            print(f"  {ci:<5} {cam.model:<6} {'N/A':>10} {'N/A':>9} {'N/A':>8} "
                  f"{'N/A':>11} {'N/A':>11} {'N/A':>10} {'N/A':>7} {'ERROR':>8}")
            print(f"  {'':5}   ↳ serial={cam.serial}  depth={cam.depth_path}")
            per_cam_ok = False
            continue

        if cam.has_rgb:
            # RGB-capable camera: check depth + RGB (+ intra-sync when enabled)
            if rs is None or ss is None:
                print(f"  {ci:<5} {cam.model:<6} "
                      f"{'N/A':>10} {'N/A':>9} {'N/A':>8} "
                      f"{'N/A':>11} {'N/A':>11} {'N/A':>10} {'N/A':>7} {'ERROR':>8}")
                per_cam_ok = False
                continue
            cam_pass = (ds.ok and rs.ok and ss.ok) if check_sync else (ds.ok and rs.ok)
            rgb_arr   = f"{100*rs.arrival_pct:.0f}% ({rs.arrived}/{rs.requested})"
            gap_str   = f"{ds.max_gap}/{rs.max_gap}"
            pairs_str = f"{ss.n_valid}/{ss.n_pairs}"
            drift_med = f"{ss.drift_median_ms:>9.2f}ms"
            drift_max = f"{ss.drift_max_ms:>9.2f}ms"
            pct_2ms   = f"{100*ss.pct_within_2ms:>6.0f}%"
        else:
            # Depth-only camera: no RGB or intra-sync
            cam_pass  = ds.ok
            rgb_arr   = "---"
            gap_str   = f"{ds.max_gap}"
            pairs_str = "---"
            drift_med = "      ---"
            drift_max = "      ---"
            pct_2ms   = "   ---"

        if not cam_pass:
            per_cam_ok = False

        depth_arr = f"{100*ds.arrival_pct:.0f}% ({ds.arrived}/{ds.requested})"
        result    = _pass_fail(cam_pass)

        print(f"  {ci:<5} {cam.model:<6} {depth_arr:>10} {rgb_arr:>9} {gap_str:>8} "
              f"{pairs_str:>8} "
              f"{drift_med} {drift_max} "
              f"{pct_2ms} {result:>8}")

        fail_notes = []
        if not cam_pass:
            fail_notes.append(f"serial={cam.serial}  depth={cam.depth_path}")
        if ds.error:
            fail_notes.append(f"depth error: {ds.error}")
        elif not ds.ok:
            fail_notes.append(f"depth: arr={100*ds.arrival_pct:.0f}%, gap={ds.max_gap}")
        if cam.has_rgb:
            if rs and rs.error:
                fail_notes.append(f"rgb error: {rs.error}")
            elif rs and not rs.ok:
                fail_notes.append(f"rgb: arr={100*rs.arrival_pct:.0f}%, gap={rs.max_gap}")
            if check_sync and ss and not ss.ok:
                fail_notes.append(f"sync: median={ss.drift_median_ms:.2f}ms, "
                                  f"≤2ms={100*ss.pct_within_2ms:.0f}%")
        for note in fail_notes:
            print(f"  {'':5}   ↳ {note}")

    # ── Cross-camera section ─────────────────────────────────────────────────
    print("  " + "-" * 99)
    cc_thr    = f"{cross_cam.threshold_ms:.0f}ms"
    cc_valid  = f"{cross_cam.n_valid}/{cross_cam.n_ref_frames}"
    cc_pct    = f"{100*cross_cam.pct_within_threshold:.0f}%"
    cc_med    = (f"{cross_cam.spread_median_ms:.2f}ms"
                 if cross_cam.spread_median_ms != float("inf") else "N/A")
    cc_max    = (f"{cross_cam.spread_max_ms:.2f}ms"
                 if cross_cam.spread_max_ms != float("inf") else "N/A")

    if not check_cross_cam:
        cc_result = "SKIPPED"
        cc_note = "cross-camera sync requires at least 2 active cameras"
    elif not check_sync:
        cc_result = "SKIPPED"
        cc_note = "sync checks disabled for forced mode 0 (stream-presence run)"
    else:
        if not cross_cam.has_sync:
            cc_result = "NO-SYNC"
            cc_note   = f"only {cc_pct} frames within {cc_thr} threshold — no ext trigger?"
        elif cross_cam.ok:
            cc_result = "PASS"
            cc_note   = ""
        else:
            cc_result = "FAIL"
            cc_note   = (f"median={cc_med} > {cc_thr} or "
                         f"valid={cc_pct} < 95%")

    print(f"  {'X-cam':<5} {cross_cam.n_cams} cameras, "
          f"valid pairs {cc_valid} ({cc_pct}), "
          f"spread med={cc_med} max={cc_max}   → {cc_result}")
    if cc_note:
        print(f"  {'':5}   ↳ {cc_note}")

    print("=" * 103)
    print(f"  Per-camera:   {_pass_fail(per_cam_ok)}")
    print(f"  Cross-camera: {cc_result}")
    print("=" * 103)
    return per_cam_ok, (True if (not check_sync or not check_cross_cam) else cross_cam.ok)


# ─────────────────────────────────────────────────────────────────────────────
# Main
# ─────────────────────────────────────────────────────────────────────────────
WIDTH  = 640
HEIGHT = 480
FPS    = 30
PRE_MODE_APPLY_DELAY_S = 2.0
PRE_STREAM_START_DELAY_S = 2.0


def _run_once(args: argparse.Namespace, run_idx: int = 1,
              total_runs: int = 1) -> Tuple[int, bool]:
    n_frames = args.frames
    check_sync = args.sync_mode_override != 0
    check_cross_cam = True

    run_title = (f"D4XX Multi-Camera Depth+RGB Sync Test (run {run_idx}/{total_runs})"
                 if total_runs > 1 else
                 "D4XX Multi-Camera Depth+RGB Sync Test")
    print(f"\n{run_title}")
    print(f"  Resolution       : {WIDTH}x{HEIGHT} @ {FPS} fps")
    print(f"  Frames           : {n_frames} (~{n_frames/FPS:.0f}s)")
    if args.sync_mode_override is None:
        print(f"  Sync modes       : D457=3 (Full Slave), D401=2 (Slave)")
    else:
        print(f"  Sync modes       : override={args.sync_mode_override} (all cameras)")
    print(f"  Sync checks      : {'enabled' if check_sync else 'disabled (mode 0 stream-presence run)'}")
    print(f"  HW reset         : {'skip (--no-reset)' if args.no_reset else 'yes'}")
    print(f"  Thresholds       : arrival >=90%, drops <=2, depth-RGB median <=2ms, >=95% within 2ms")
    print(f"  X-cam threshold  : {args.cross_cam_threshold:.1f}ms")

    # ── 1. Discover cameras ──────────────────────────────────────────────────
    print("\n[1] Discovering D4XX cameras...")
    discovered = discover_cameras()
    if len(discovered) < args.num_cameras:
        print(f"  ERROR: found {len(discovered)} camera(s), need {args.num_cameras}. "
              f"Check driver: lsmod | grep d4xx")
        return 1, False
    cameras = discovered[:args.num_cameras]
    check_cross_cam = len(cameras) >= 2
    if len(discovered) > len(cameras):
        print(f"  Found {len(discovered)} cameras, using first {len(cameras)}:")
    else:
        print(f"  Found {len(cameras)} cameras:")
    for cam in cameras:
        mode_to_set = (args.sync_mode_override
                       if args.sync_mode_override is not None else cam.sync_mode)
        print(f"    cam{cam.index}: model={cam.model}  depth={cam.depth_path}  "
              f"rgb={cam.rgb_path if cam.has_rgb else 'N/A'}  "
              f"serial={cam.serial}  sync_mode={mode_to_set}")

    # ── 2. HW reset (optional) ───────────────────────────────────────────────
    if not args.no_reset:
        print("\n[2] HW reset (clearing any stale stream state)...")
        hw_reset_all(cameras, wait_s=4.0)
    else:
        print("\n[2] HW reset skipped.")

    print(f"\n[3] Waiting {PRE_MODE_APPLY_DELAY_S:.0f}s before applying sync mode...")
    time.sleep(PRE_MODE_APPLY_DELAY_S)

    # ── 4. Set sync mode (per camera model or override) ─────────────────────
    print(f"\n[4] Setting sync modes...")
    for cam in cameras:
        mode_to_set = (args.sync_mode_override
                       if args.sync_mode_override is not None else cam.sync_mode)
        ok = set_sync_mode(cam, mode_to_set)
        print(f"  cam{cam.index} ({cam.model}): sync_mode={mode_to_set} {'OK' if ok else 'WARN'}")
    time.sleep(0.3)

    print(f"\n[5] Waiting {PRE_STREAM_START_DELAY_S:.0f}s before stream setup/start...")
    time.sleep(PRE_STREAM_START_DELAY_S)

    # ── 6. Two-phase stream launch ────────────────────────────────────────────
    # Phase A (parallel)  : each thread opens device + sets format + allocates mmap
    # Phase B (sequential): main thread calls STREAMON on all fds in order
    # Phase C (parallel)  : threads capture frames then close
    #
    # Keeping STREAMON sequential in the main thread avoids Tegra VI resource
    # allocation races that occur when threads race to STREAMON simultaneously.
    n_streams = sum(2 if cam.has_rgb else 1 for cam in cameras)
    print(f"\n[4] Starting {n_streams} streams (setup parallel, STREAMON sequential)...")

    setup_events: Dict[Tuple[int, str], threading.Event] = {}
    stop_events: Dict[Tuple[int, str], threading.Event] = {}
    capture_start = threading.Event()
    results: Dict[Tuple[int, str], dict] = {}
    threads: List[threading.Thread] = []
    thread_key_map: Dict[str, Tuple[int, str]] = {}
    stream_order: List[Tuple[int, str]] = []  # ordered for STREAMON

    for cam in cameras:
        stream_list = []
        stream_list.append(("depth", cam.depth_path, PIX_FMT_Z16))
        if cam.has_rgb:
            stream_list.append(("rgb", cam.rgb_path, cam.rgb_pixfmt))
        for stream, path, pixfmt in stream_list:
            key = (cam.index, stream)
            out = {}
            results[key] = out
            ev = threading.Event()
            stop_ev = threading.Event()
            setup_events[key] = ev
            stop_events[key] = stop_ev
            stream_order.append(key)
            t = threading.Thread(
                target=_stream_thread,
                args=(path, WIDTH, HEIGHT, pixfmt, n_frames, out, ev, capture_start, stop_ev),
                daemon=True,
                name=f"cam{cam.index}-{stream}",
            )
            threads.append(t)
            thread_key_map[t.name] = key
            t.start()

    # Wait for all setup phases to complete (timeout per stream: 10s)
    setup_deadline = time.monotonic() + 10.0
    for key, ev in setup_events.items():
        remaining = max(0.0, setup_deadline - time.monotonic())
        if not ev.wait(timeout=remaining):
            results[key]["error"] = "setup timed out"

    # STREAMON all fds sequentially — one per camera-stream, depth before rgb
    n_started = 0
    for key in stream_order:
        out = results[key]
        fd = out.get("fd")
        if fd is None:
            continue
        try:
            buf_type = ctypes.c_int(V4L2_BUF_TYPE_VIDEO_CAPTURE)
            fcntl.ioctl(fd, VIDIOC_STREAMON, buf_type)
            n_started += 1
        except Exception as exc:
            out["error"] = f"STREAMON failed: {exc}"
    print(f"  STREAMON issued for {n_started}/{len(stream_order)} streams")

    # Release all capture threads
    start = time.monotonic()
    capture_start.set()

    deadline = start + n_frames / FPS + 20.0
    timed_out_threads = 0
    live_thread_keys = set()
    for t in threads:
        remaining = max(0.0, deadline - time.monotonic())
        t.join(timeout=remaining)
        if t.is_alive():
            timed_out_threads += 1
            print(f"  [WARN] Thread {t.name} did not finish in time")
            key = thread_key_map.get(t.name)
            if key is not None:
                stop_events[key].set()
                t.join(timeout=6.0)
                if t.is_alive():
                    live_thread_keys.add(key)
                    if not results[key].get("error"):
                        results[key]["error"] = "thread timeout (possible stuck stream)"
    elapsed = time.monotonic() - start
    print(f"  Capture complete in {elapsed:.1f}s")

    # STREAMOFF/CLOSE all streams in reverse STREAMON order from main thread.
    n_stopped = 0
    for key in reversed(stream_order):
        if key in live_thread_keys:
            continue
        out = results.get(key, {})
        fd = out.pop("fd", None)
        buffers = out.pop("buffers", None)
        if fd is None or buffers is None:
            continue
        try:
            _close_stream(fd, buffers)
            n_stopped += 1
        except Exception as exc:
            if not out.get("error"):
                out["error"] = f"close failed: {exc}"
    print(f"  STREAMOFF/CLOSE issued for {n_stopped}/{len(stream_order)} streams (reverse order)")

    # ── 6. Collect results and validate ──────────────────────────────────────
    print("\n[7] Validating results...")

    stream_stats: Dict[Tuple[int, str], StreamStats] = {}
    sync_stats: Dict[int, SyncStats] = {}
    all_depth_frames: Dict[int, List[FrameInfo]] = {}

    # Compute first-frame reference (earliest depth timestamp across all cameras)
    depth_starts_us = {
        ci: results.get((ci, "depth"), {}).get("frames", [[0, 0]])[0][1]
        for ci in (cam.index for cam in cameras)
    }
    ref_ts = min(t for t in depth_starts_us.values() if t > 0) if any(
        t > 0 for t in depth_starts_us.values()) else 0

    for cam in cameras:
        ci = cam.index

        streams_to_check = ["depth"]
        if cam.has_rgb:
            streams_to_check.append("rgb")

        for stream in streams_to_check:
            key = (ci, stream)
            out = results.get(key, {})
            frames: List[FrameInfo] = out.get("frames", [])
            error = out.get("error", "")
            label = f"cam{ci}/{stream}"

            if error:
                ss = StreamStats(label=label, arrived=0, requested=n_frames,
                                 max_gap=999, arrival_pct=0.0, error=error)
            else:
                ss = validate_stream(label, frames, n_frames)

            stream_stats[key] = ss
            print(f"  {label}: {ss.arrived}/{ss.requested} frames, "
                  f"max_gap={ss.max_gap}, "
                  f"arrival={100*ss.arrival_pct:.0f}%"
                  + (f" ERROR: {error}" if error else ""))

        depth_frames = results.get((ci, "depth"), {}).get("frames", [])
        rgb_frames = results.get((ci, "rgb"), {}).get("frames", []) if cam.has_rgb else []
        all_depth_frames[ci] = depth_frames

        # Show stream start offset relative to earliest depth frame
        def _start_offset(frames):
            return ((frames[0][1] - ref_ts) / 1000.0) if frames else float("nan")

        d_offset = _start_offset(depth_frames)
        if cam.has_rgb:
            r_offset = _start_offset(rgb_frames)
            print(f"  cam{ci}/start_offset: depth={d_offset:.1f}ms  rgb={r_offset:.1f}ms")
        else:
            print(f"  cam{ci}/start_offset: depth={d_offset:.1f}ms  (no RGB)")

        if cam.has_rgb:
            ss_sync = analyse_intra_sync(ci, depth_frames, rgb_frames)
            sync_stats[ci] = ss_sync
            print(f"  cam{ci}/sync: pairs={ss_sync.n_pairs} valid={ss_sync.n_valid}, "
                  f"median={ss_sync.drift_median_ms:.2f}ms, "
                  f"max={ss_sync.drift_max_ms:.2f}ms, "
                  f"within_2ms={100*ss_sync.pct_within_2ms:.0f}%")
        else:
            print(f"  cam{ci}/sync: skipped (no RGB node)")

    # ── 7. Cross-camera sync ─────────────────────────────────────────────────
    cross_cam = analyse_cross_camera_sync(all_depth_frames,
                                          threshold_ms=args.cross_cam_threshold)

    # ── 8. Summary ───────────────────────────────────────────────────────────
    per_cam_ok, cross_ok = print_summary(
        cameras, stream_stats, sync_stats, cross_cam,
        check_sync=check_sync, check_cross_cam=check_cross_cam)

    no_frame_streams = [
        key for key, ss in stream_stats.items() if ss.arrived == 0
    ]
    if no_frame_streams:
        labels = ", ".join(f"cam{ci}/{stream}" for ci, stream in no_frame_streams)
        print(f"  [RUN ALERT] Streams with zero received frames: {labels}")

    if per_cam_ok and cross_ok and timed_out_threads == 0:
        return (0 if not no_frame_streams else 1), bool(no_frame_streams)
    if not per_cam_ok and not cross_ok:
        return 2, bool(no_frame_streams)   # both failed
    return 1, bool(no_frame_streams)   # one of the checks failed, stream timed out, or zero-frame stream


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--frames",      type=int,   default=300,
                        help="Frames to capture per stream (default: 300)")
    parser.add_argument("--num-cameras", type=int, default=4,
                        help="Number of discovered cameras to use (1-4, default: 4)")
    parser.add_argument("--no-reset",    action="store_true",
                        help="Skip HW reset preamble")
    parser.add_argument("--cross-cam-threshold", type=float, default=2.0,
                        help="Cross-camera spread threshold in ms (default: 2.0)")
    parser.add_argument("--runs", type=int, default=1,
                        help="Number of repeated test iterations (default: 1)")
    parser.add_argument("--sync-mode-override", type=int, default=None,
                        help="Force one sync mode for all cameras (e.g. 2)")
    parser.add_argument("--inter-run-delay", type=float, default=0.5,
                        help="Delay between runs in seconds (default: 0.5)")
    args = parser.parse_args()

    if args.runs < 1:
        print("ERROR: --runs must be >= 1")
        return 1
    if not 1 <= args.num_cameras <= 4:
        print("ERROR: --num-cameras must be between 1 and 4")
        return 1

    aggregate = {0: 0, 1: 0, 2: 0}
    zero_frame_runs: List[int] = []
    for run_idx in range(1, args.runs + 1):
        rc, had_zero_frames = _run_once(args, run_idx=run_idx, total_runs=args.runs)
        aggregate[rc] = aggregate.get(rc, 0) + 1
        if had_zero_frames:
            zero_frame_runs.append(run_idx)

        if run_idx < args.runs and args.inter_run_delay > 0:
            print(f"\n[run {run_idx}/{args.runs}] sleeping {args.inter_run_delay:.1f}s before next run...")
            time.sleep(args.inter_run_delay)

    if args.runs > 1:
        print("\n" + "=" * 72)
        print("AGGREGATE RUN SUMMARY")
        print("=" * 72)
        print(f"  Runs total : {args.runs}")
        print(f"  Runs PASS  : {aggregate.get(0, 0)}")
        print(f"  Runs WARN  : {aggregate.get(1, 0)}")
        print(f"  Runs FAIL  : {aggregate.get(2, 0)}")
        if zero_frame_runs:
            runs_txt = ", ".join(str(i) for i in zero_frame_runs)
            print(f"  Zero-frame runs : {len(zero_frame_runs)} ({runs_txt})")
        else:
            print("  Zero-frame runs : 0")
        print("=" * 72)

    if aggregate.get(2, 0) > 0:
        return 2
    if aggregate.get(1, 0) > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

