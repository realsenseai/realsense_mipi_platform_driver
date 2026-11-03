"""Higher-level V4L2 control read/write helpers with typed access."""

import ctypes

from . import ioctls
from . import structs as S


def read_int_control(device, ctrl_id):
    """Read a standard integer V4L2 control, return its value."""
    ctrl = device.get_ctrl(ctrl_id)
    return ctrl.value


def write_int_control(device, ctrl_id, value):
    """Write a standard integer V4L2 control."""
    device.set_ctrl(ctrl_id, value)


def read_u8_array_control(device, ctrl_id, size):
    """Read a U8 array extended control, return bytes."""
    buf = (ctypes.c_uint8 * size)()
    device.get_ext_ctrls(ctrl_id, size, ctypes.addressof(buf))
    return bytes(buf)


def read_u16_array_control(device, ctrl_id, count):
    """Read a U16 array extended control, return list of ints."""
    size = count * ctypes.sizeof(ctypes.c_uint16)
    buf = (ctypes.c_uint16 * count)()
    device.get_ext_ctrls(ctrl_id, size, ctypes.addressof(buf))
    return list(buf)


def read_u32_control(device, ctrl_id, count=1):
    """Read a U32 extended control, return list of ints."""
    size = count * ctypes.sizeof(ctypes.c_uint32)
    buf = (ctypes.c_uint32 * count)()
    device.get_ext_ctrls(ctrl_id, size, ctypes.addressof(buf))
    return list(buf)


def write_u8_array_control(device, ctrl_id, data):
    """Write a U8 array extended control from bytes."""
    size = len(data)
    buf = (ctypes.c_uint8 * size)(*data)
    device.set_ext_ctrls(ctrl_id, size, ctypes.addressof(buf))


def enumerate_controls(device):
    """Enumerate all available controls, return list of v4l2_queryctrl."""
    controls = []
    ctrl_id = ioctls.V4L2_CTRL_FLAG_NEXT_CTRL
    while True:
        try:
            qc = device.query_ctrl(ctrl_id)
        except OSError:
            break
        if qc.flags & ioctls.V4L2_CTRL_FLAG_DISABLED:
            ctrl_id = qc.id | ioctls.V4L2_CTRL_FLAG_NEXT_CTRL
            continue
        controls.append(qc)
        ctrl_id = qc.id | ioctls.V4L2_CTRL_FLAG_NEXT_CTRL
    return controls
