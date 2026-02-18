"""V4L2 ioctl numbers and fourcc pixel format constants."""

import ctypes
import struct

# ioctl direction bits
_IOC_NONE = 0
_IOC_WRITE = 1
_IOC_READ = 2

_IOC_NRBITS = 8
_IOC_TYPEBITS = 8
_IOC_SIZEBITS = 14
_IOC_DIRBITS = 2

_IOC_NRSHIFT = 0
_IOC_TYPESHIFT = _IOC_NRSHIFT + _IOC_NRBITS
_IOC_SIZESHIFT = _IOC_TYPESHIFT + _IOC_TYPEBITS
_IOC_DIRSHIFT = _IOC_SIZESHIFT + _IOC_SIZEBITS


def _IOC(direction, type_, nr, size):
    return (
        (direction << _IOC_DIRSHIFT)
        | (ord(type_) << _IOC_TYPESHIFT)
        | (nr << _IOC_NRSHIFT)
        | (size << _IOC_SIZESHIFT)
    )


def _IO(type_, nr):
    return _IOC(_IOC_NONE, type_, nr, 0)


def _IOR(type_, nr, size):
    return _IOC(_IOC_READ, type_, nr, size)


def _IOW(type_, nr, size):
    return _IOC(_IOC_WRITE, type_, nr, size)


def _IOWR(type_, nr, size):
    return _IOC(_IOC_READ | _IOC_WRITE, type_, nr, size)


def v4l2_fourcc(a, b, c, d):
    return ord(a) | (ord(b) << 8) | (ord(c) << 16) | (ord(d) << 24)


# --- VIDIOC ioctl numbers ---
# Sizes are imported from structs at module level; we use ctypes.sizeof
# We defer the actual computation to after structs are importable.
# For now, define a helper to compute at import time.

def _v4l2_ioctl_sizes():
    """Compute ioctl numbers using actual struct sizes."""
    from . import structs as S

    sz = ctypes.sizeof
    return {
        "VIDIOC_QUERYCAP": _IOR("V", 0, sz(S.v4l2_capability)),
        "VIDIOC_ENUM_FMT": _IOWR("V", 2, sz(S.v4l2_fmtdesc)),
        "VIDIOC_G_FMT": _IOWR("V", 4, sz(S.v4l2_format)),
        "VIDIOC_S_FMT": _IOWR("V", 5, sz(S.v4l2_format)),
        "VIDIOC_REQBUFS": _IOWR("V", 8, sz(S.v4l2_requestbuffers)),
        "VIDIOC_QUERYBUF": _IOWR("V", 9, sz(S.v4l2_buffer)),
        "VIDIOC_G_PARM": _IOWR("V", 21, sz(S.v4l2_streamparm)),
        "VIDIOC_S_PARM": _IOWR("V", 22, sz(S.v4l2_streamparm)),
        "VIDIOC_QBUF": _IOWR("V", 15, sz(S.v4l2_buffer)),
        "VIDIOC_DQBUF": _IOWR("V", 17, sz(S.v4l2_buffer)),
        "VIDIOC_STREAMON": _IOW("V", 18, ctypes.sizeof(ctypes.c_int)),
        "VIDIOC_STREAMOFF": _IOW("V", 19, ctypes.sizeof(ctypes.c_int)),
        "VIDIOC_G_CTRL": _IOWR("V", 27, sz(S.v4l2_control)),
        "VIDIOC_S_CTRL": _IOWR("V", 28, sz(S.v4l2_control)),
        "VIDIOC_QUERYCTRL": _IOWR("V", 36, sz(S.v4l2_queryctrl)),
        "VIDIOC_G_EXT_CTRLS": _IOWR("V", 71, sz(S.v4l2_ext_controls)),
        "VIDIOC_S_EXT_CTRLS": _IOWR("V", 72, sz(S.v4l2_ext_controls)),
        "VIDIOC_ENUM_FRAMESIZES": _IOWR("V", 74, sz(S.v4l2_frmsizeenum)),
        "VIDIOC_ENUM_FRAMEINTERVALS": _IOWR("V", 75, sz(S.v4l2_frmivalenum)),
    }


# Lazy-loaded ioctl numbers — populated on first access
_ioctls = None


def __getattr__(name):
    global _ioctls
    if name.startswith("VIDIOC_"):
        if _ioctls is None:
            _ioctls = _v4l2_ioctl_sizes()
        if name in _ioctls:
            return _ioctls[name]
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")


# --- Pixel formats (fourcc) ---
V4L2_PIX_FMT_Z16 = v4l2_fourcc("Z", "1", "6", " ")
V4L2_PIX_FMT_UYVY = v4l2_fourcc("U", "Y", "V", "Y")
V4L2_PIX_FMT_YUYV = v4l2_fourcc("Y", "U", "Y", "V")
V4L2_PIX_FMT_GREY = v4l2_fourcc("G", "R", "E", "Y")
V4L2_PIX_FMT_Y8I = v4l2_fourcc("Y", "8", "I", " ")
V4L2_PIX_FMT_Y12I = v4l2_fourcc("Y", "1", "2", "I")
V4L2_PIX_FMT_RGB24 = v4l2_fourcc("R", "G", "B", "3")

# Metadata format used by D4XX
V4L2_META_FMT_D4XX = v4l2_fourcc("D", "4", "X", "X")

# --- Buffer types ---
V4L2_BUF_TYPE_VIDEO_CAPTURE = 1
V4L2_BUF_TYPE_META_CAPTURE = 13

# --- Memory types ---
V4L2_MEMORY_MMAP = 1
V4L2_MEMORY_USERPTR = 2

# --- Field types ---
V4L2_FIELD_NONE = 1
V4L2_FIELD_ANY = 0

# --- Frame size types ---
V4L2_FRMSIZE_TYPE_DISCRETE = 1
V4L2_FRMSIZE_TYPE_CONTINUOUS = 2
V4L2_FRMSIZE_TYPE_STEPWISE = 3

# --- Frame interval types ---
V4L2_FRMIVAL_TYPE_DISCRETE = 1

# --- Control flags ---
V4L2_CTRL_FLAG_DISABLED = 0x0001
V4L2_CTRL_FLAG_NEXT_CTRL = 0x80000000

# --- Control types ---
V4L2_CTRL_TYPE_INTEGER = 1
V4L2_CTRL_TYPE_BOOLEAN = 2
V4L2_CTRL_TYPE_MENU = 3
V4L2_CTRL_TYPE_INTEGER64 = 5
V4L2_CTRL_TYPE_U8 = 0x0100
V4L2_CTRL_TYPE_U16 = 0x0101
V4L2_CTRL_TYPE_U32 = 0x0102

# --- Standard V4L2 control IDs ---
V4L2_CID_BASE = 0x00980000
V4L2_CID_CAMERA_CLASS_BASE = 0x009A0900
V4L2_CID_EXPOSURE_AUTO = V4L2_CID_CAMERA_CLASS_BASE + 1
V4L2_CID_EXPOSURE_ABSOLUTE = V4L2_CID_CAMERA_CLASS_BASE + 2

# auto_exposure menu values
V4L2_EXPOSURE_MANUAL = 1
V4L2_EXPOSURE_APERTURE_PRIORITY = 3

# --- Control classes ---
V4L2_CTRL_CLASS_CAMERA = 0x009A0000

# --- Capabilities ---
V4L2_CAP_VIDEO_CAPTURE = 0x00000001
V4L2_CAP_META_CAPTURE = 0x00800000
V4L2_CAP_STREAMING = 0x04000000

FOURCC_TO_NAME = {
    V4L2_PIX_FMT_Z16: "Z16",
    V4L2_PIX_FMT_UYVY: "UYVY",
    V4L2_PIX_FMT_YUYV: "YUYV",
    V4L2_PIX_FMT_GREY: "GREY",
    V4L2_PIX_FMT_Y8I: "Y8I",
    V4L2_PIX_FMT_Y12I: "Y12I",
    V4L2_PIX_FMT_RGB24: "RGB3",
    V4L2_META_FMT_D4XX: "D4XX",
}
