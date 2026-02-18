"""V4L2Device class: open/close/ioctl wrappers for V4L2 devices."""

import ctypes
import fcntl
import os

from . import ioctls
from . import structs as S


class V4L2Device:
    """Wrapper around a V4L2 video device node."""

    def __init__(self, path):
        self.path = path
        self.fd = -1

    def open(self):
        self.fd = os.open(self.path, os.O_RDWR | os.O_NONBLOCK)
        return self

    def close(self):
        if self.fd >= 0:
            os.close(self.fd)
            self.fd = -1

    def fileno(self):
        return self.fd

    def __enter__(self):
        self.open()
        return self

    def __exit__(self, *exc):
        self.close()
        return False

    def ioctl(self, request, arg):
        return fcntl.ioctl(self.fd, request, arg)

    def query_cap(self):
        cap = S.v4l2_capability()
        self.ioctl(ioctls.VIDIOC_QUERYCAP, cap)
        return cap

    def enum_formats(self, buf_type=ioctls.V4L2_BUF_TYPE_VIDEO_CAPTURE):
        formats = []
        idx = 0
        while True:
            fmt = S.v4l2_fmtdesc()
            fmt.index = idx
            fmt.type = buf_type
            try:
                self.ioctl(ioctls.VIDIOC_ENUM_FMT, fmt)
            except OSError:
                break
            formats.append(fmt)
            idx += 1
        return formats

    def get_format(self, buf_type=ioctls.V4L2_BUF_TYPE_VIDEO_CAPTURE):
        fmt = S.v4l2_format()
        fmt.type = buf_type
        self.ioctl(ioctls.VIDIOC_G_FMT, fmt)
        return fmt

    def set_format(self, width, height, pixelformat,
                   buf_type=ioctls.V4L2_BUF_TYPE_VIDEO_CAPTURE):
        fmt = S.v4l2_format()
        fmt.type = buf_type
        fmt.fmt.pix.width = width
        fmt.fmt.pix.height = height
        fmt.fmt.pix.pixelformat = pixelformat
        fmt.fmt.pix.field = ioctls.V4L2_FIELD_ANY
        self.ioctl(ioctls.VIDIOC_S_FMT, fmt)
        return fmt

    def set_meta_format(self, dataformat,
                        buf_type=ioctls.V4L2_BUF_TYPE_META_CAPTURE):
        fmt = S.v4l2_format()
        fmt.type = buf_type
        fmt.fmt.meta.dataformat = dataformat
        self.ioctl(ioctls.VIDIOC_S_FMT, fmt)
        return fmt

    def get_parm(self, buf_type=ioctls.V4L2_BUF_TYPE_VIDEO_CAPTURE):
        parm = S.v4l2_streamparm()
        parm.type = buf_type
        self.ioctl(ioctls.VIDIOC_G_PARM, parm)
        return parm

    def set_parm(self, fps, buf_type=ioctls.V4L2_BUF_TYPE_VIDEO_CAPTURE):
        parm = S.v4l2_streamparm()
        parm.type = buf_type
        parm.parm.capture.timeperframe.numerator = 1
        parm.parm.capture.timeperframe.denominator = fps
        try:
            self.ioctl(ioctls.VIDIOC_S_PARM, parm)
        except OSError as e:
            import errno
            if e.errno in (errno.EBUSY, errno.ENOTTY, errno.EINVAL):
                pass  # tegra-video may not support VIDIOC_S_PARM
            else:
                raise
        return parm

    def enum_framesizes(self, pixelformat):
        sizes = []
        idx = 0
        while True:
            fs = S.v4l2_frmsizeenum()
            fs.index = idx
            fs.pixel_format = pixelformat
            try:
                self.ioctl(ioctls.VIDIOC_ENUM_FRAMESIZES, fs)
            except OSError:
                break
            sizes.append(fs)
            idx += 1
        return sizes

    def enum_frameintervals(self, pixelformat, width, height):
        intervals = []
        idx = 0
        while True:
            fi = S.v4l2_frmivalenum()
            fi.index = idx
            fi.pixel_format = pixelformat
            fi.width = width
            fi.height = height
            try:
                self.ioctl(ioctls.VIDIOC_ENUM_FRAMEINTERVALS, fi)
            except OSError:
                break
            intervals.append(fi)
            idx += 1
        return intervals

    def query_ctrl(self, ctrl_id):
        qc = S.v4l2_queryctrl()
        qc.id = ctrl_id
        self.ioctl(ioctls.VIDIOC_QUERYCTRL, qc)
        return qc

    def get_ctrl(self, ctrl_id):
        ctrl = S.v4l2_control()
        ctrl.id = ctrl_id
        self.ioctl(ioctls.VIDIOC_G_CTRL, ctrl)
        return ctrl

    def set_ctrl(self, ctrl_id, value):
        ctrl = S.v4l2_control()
        ctrl.id = ctrl_id
        ctrl.value = value
        self.ioctl(ioctls.VIDIOC_S_CTRL, ctrl)
        return ctrl

    def get_ext_ctrls(self, ctrl_id, size, data_ptr):
        ext = S.v4l2_ext_control()
        ext.id = ctrl_id
        ext.size = size
        ext.ptr = data_ptr

        ctrls = S.v4l2_ext_controls()
        ctrls.ctrl_class = 0
        ctrls.count = 1
        ctrls.controls = ctypes.pointer(ext)
        self.ioctl(ioctls.VIDIOC_G_EXT_CTRLS, ctrls)
        return ext

    def set_ext_ctrls(self, ctrl_id, size, data_ptr):
        ext = S.v4l2_ext_control()
        ext.id = ctrl_id
        ext.size = size
        ext.ptr = data_ptr

        ctrls = S.v4l2_ext_controls()
        ctrls.ctrl_class = 0
        ctrls.count = 1
        ctrls.controls = ctypes.pointer(ext)
        self.ioctl(ioctls.VIDIOC_S_EXT_CTRLS, ctrls)
        return ext
