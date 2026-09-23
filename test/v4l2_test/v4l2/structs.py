"""V4L2 ctypes Structure definitions for ioctl operations."""

import ctypes


class v4l2_capability(ctypes.Structure):
    _fields_ = [
        ("driver", ctypes.c_char * 16),
        ("card", ctypes.c_char * 32),
        ("bus_info", ctypes.c_char * 32),
        ("version", ctypes.c_uint32),
        ("capabilities", ctypes.c_uint32),
        ("device_caps", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32 * 3),
    ]


class v4l2_fmtdesc(ctypes.Structure):
    _fields_ = [
        ("index", ctypes.c_uint32),
        ("type", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("description", ctypes.c_char * 32),
        ("pixelformat", ctypes.c_uint32),
        ("mbus_code", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32 * 3),
    ]


class v4l2_pix_format(ctypes.Structure):
    _fields_ = [
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
        ("pixelformat", ctypes.c_uint32),
        ("field", ctypes.c_uint32),
        ("bytesperline", ctypes.c_uint32),
        ("sizeimage", ctypes.c_uint32),
        ("colorspace", ctypes.c_uint32),
        ("priv", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("ycbcr_enc_or_hsv_enc", ctypes.c_uint32),
        ("quantization", ctypes.c_uint32),
        ("xfer_func", ctypes.c_uint32),
    ]


class v4l2_meta_format(ctypes.Structure):
    _fields_ = [
        ("dataformat", ctypes.c_uint32),
        ("buffersize", ctypes.c_uint32),
    ]


class v4l2_rect(ctypes.Structure):
    _fields_ = [
        ("left", ctypes.c_int32),
        ("top", ctypes.c_int32),
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
    ]


class v4l2_clip(ctypes.Structure):
    pass


v4l2_clip._fields_ = [
    ("c", v4l2_rect),
    ("next", ctypes.POINTER(v4l2_clip)),
]


class v4l2_window(ctypes.Structure):
    """Unused directly, but its pointer fields (clips, bitmap) are the reason
    the kernel's real struct v4l2_format is 8-byte aligned/208 bytes on LP64
    -- omitting it here silently shrank our v4l2_format to 204 bytes, which
    corrupts the _IOWR-encoded VIDIOC_S_FMT/G_FMT ioctl numbers (the size is
    baked into the request code) and makes the kernel return ENOTTY for a
    request it doesn't recognize.
    """
    _fields_ = [
        ("w", v4l2_rect),
        ("field", ctypes.c_uint32),
        ("chromakey", ctypes.c_uint32),
        ("clips", ctypes.POINTER(v4l2_clip)),
        ("clipcount", ctypes.c_uint32),
        ("bitmap", ctypes.c_void_p),
        ("global_alpha", ctypes.c_uint8),
    ]


class _v4l2_format_fmt(ctypes.Union):
    _fields_ = [
        ("pix", v4l2_pix_format),
        ("win", v4l2_window),
        ("meta", v4l2_meta_format),
        ("raw_data", ctypes.c_uint8 * 200),
    ]


class v4l2_format(ctypes.Structure):
    _fields_ = [
        ("type", ctypes.c_uint32),
        ("fmt", _v4l2_format_fmt),
    ]


class v4l2_requestbuffers(ctypes.Structure):
    _fields_ = [
        ("count", ctypes.c_uint32),
        ("type", ctypes.c_uint32),
        ("memory", ctypes.c_uint32),
        ("capabilities", ctypes.c_uint32),
        ("flags", ctypes.c_uint8),
        ("reserved", ctypes.c_uint8 * 3),
    ]


class v4l2_timeval(ctypes.Structure):
    _fields_ = [
        ("tv_sec", ctypes.c_long),
        ("tv_usec", ctypes.c_long),
    ]


class v4l2_timecode(ctypes.Structure):
    _fields_ = [
        ("type", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("frames", ctypes.c_uint8),
        ("seconds", ctypes.c_uint8),
        ("minutes", ctypes.c_uint8),
        ("hours", ctypes.c_uint8),
        ("userbits", ctypes.c_uint8 * 4),
    ]


class _v4l2_buffer_m(ctypes.Union):
    _fields_ = [
        ("offset", ctypes.c_uint32),
        ("userptr", ctypes.c_ulong),
        ("planes", ctypes.c_void_p),
        ("fd", ctypes.c_int32),
    ]


class v4l2_buffer(ctypes.Structure):
    _fields_ = [
        ("index", ctypes.c_uint32),
        ("type", ctypes.c_uint32),
        ("bytesused", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("field", ctypes.c_uint32),
        ("timestamp", v4l2_timeval),
        ("timecode", v4l2_timecode),
        ("sequence", ctypes.c_uint32),
        ("memory", ctypes.c_uint32),
        ("m", _v4l2_buffer_m),
        ("length", ctypes.c_uint32),
        ("reserved2", ctypes.c_uint32),
        ("request_fd_or_reserved", ctypes.c_int32),
    ]


class v4l2_fract(ctypes.Structure):
    _fields_ = [
        ("numerator", ctypes.c_uint32),
        ("denominator", ctypes.c_uint32),
    ]


class v4l2_captureparm(ctypes.Structure):
    _fields_ = [
        ("capability", ctypes.c_uint32),
        ("capturemode", ctypes.c_uint32),
        ("timeperframe", v4l2_fract),
        ("extendedmode", ctypes.c_uint32),
        ("readbuffers", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32 * 4),
    ]


class _v4l2_streamparm_parm(ctypes.Union):
    _fields_ = [
        ("capture", v4l2_captureparm),
        ("raw_data", ctypes.c_uint8 * 200),
    ]


class v4l2_streamparm(ctypes.Structure):
    _fields_ = [
        ("type", ctypes.c_uint32),
        ("parm", _v4l2_streamparm_parm),
    ]


class v4l2_control(ctypes.Structure):
    _fields_ = [
        ("id", ctypes.c_uint32),
        ("value", ctypes.c_int32),
    ]


class v4l2_queryctrl(ctypes.Structure):
    _fields_ = [
        ("id", ctypes.c_uint32),
        ("type", ctypes.c_uint32),
        ("name", ctypes.c_char * 32),
        ("minimum", ctypes.c_int32),
        ("maximum", ctypes.c_int32),
        ("step", ctypes.c_int32),
        ("default_value", ctypes.c_int32),
        ("flags", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32 * 2),
    ]


class v4l2_ext_control(ctypes.Structure):
    class _u(ctypes.Union):
        _fields_ = [
            ("value", ctypes.c_int32),
            ("value64", ctypes.c_int64),
            ("string", ctypes.c_char_p),
            ("p_u8", ctypes.c_void_p),
            ("p_u16", ctypes.c_void_p),
            ("p_u32", ctypes.c_void_p),
            ("ptr", ctypes.c_void_p),
        ]

    _fields_ = [
        ("id", ctypes.c_uint32),
        ("size", ctypes.c_uint32),
        ("reserved2", ctypes.c_uint32 * 1),
        ("_u", _u),
    ]
    _anonymous_ = ("_u",)


class v4l2_ext_controls(ctypes.Structure):
    class _u(ctypes.Union):
        _fields_ = [
            ("ctrl_class", ctypes.c_uint32),
            ("which", ctypes.c_uint32),
        ]

    _fields_ = [
        ("_u", _u),
        ("count", ctypes.c_uint32),
        ("error_idx", ctypes.c_uint32),
        ("request_fd", ctypes.c_int32),
        ("reserved", ctypes.c_uint32 * 1),
        ("controls", ctypes.POINTER(v4l2_ext_control)),
    ]
    _anonymous_ = ("_u",)


class v4l2_frmsize_discrete(ctypes.Structure):
    _fields_ = [
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
    ]


class v4l2_frmsize_stepwise(ctypes.Structure):
    _fields_ = [
        ("min_width", ctypes.c_uint32),
        ("max_width", ctypes.c_uint32),
        ("step_width", ctypes.c_uint32),
        ("min_height", ctypes.c_uint32),
        ("max_height", ctypes.c_uint32),
        ("step_height", ctypes.c_uint32),
    ]


class _v4l2_frmsizeenum_u(ctypes.Union):
    _fields_ = [
        ("discrete", v4l2_frmsize_discrete),
        ("stepwise", v4l2_frmsize_stepwise),
    ]


class v4l2_frmsizeenum(ctypes.Structure):
    _fields_ = [
        ("index", ctypes.c_uint32),
        ("pixel_format", ctypes.c_uint32),
        ("type", ctypes.c_uint32),
        ("_u", _v4l2_frmsizeenum_u),
        ("reserved", ctypes.c_uint32 * 2),
    ]
    _anonymous_ = ("_u",)


class v4l2_frmival_discrete(ctypes.Structure):
    _fields_ = [
        ("numerator", ctypes.c_uint32),
        ("denominator", ctypes.c_uint32),
    ]


class v4l2_frmival_stepwise(ctypes.Structure):
    _fields_ = [
        ("min", v4l2_fract),
        ("max", v4l2_fract),
        ("step", v4l2_fract),
    ]


class _v4l2_frmivalenum_u(ctypes.Union):
    _fields_ = [
        ("discrete", v4l2_frmival_discrete),
        ("stepwise", v4l2_frmival_stepwise),
    ]


class v4l2_frmivalenum(ctypes.Structure):
    _fields_ = [
        ("index", ctypes.c_uint32),
        ("pixel_format", ctypes.c_uint32),
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
        ("type", ctypes.c_uint32),
        ("_u", _v4l2_frmivalenum_u),
        ("reserved", ctypes.c_uint32 * 2),
    ]
    _anonymous_ = ("_u",)
