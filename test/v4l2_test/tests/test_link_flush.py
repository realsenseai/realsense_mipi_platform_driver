"""GMSL cold-start link flush (RSDEV-16879).

A cold stream start flushes the camera's GMSL link only when the link may
hold stale data: another camera used the deserializer, the camera reset, or
its link state is unknown. A repeated start on a clean link skips the flush,
which would cost the new stream its first frames. Flushes are counted from
the deserializer's kernel log line, so these tests need dmesg (directly or
through passwordless sudo) and skip without it.
"""

import ctypes
import glob
import os
import re
import struct
import subprocess
import time

import pytest

from ..d4xx import constants as C
from ..v4l2 import ioctls
from ..v4l2.controls import read_u32_control, write_int_control
from ..v4l2.device import V4L2Device
from ..v4l2.stream import StreamContext

FLUSH_LOG = "line-buffer flush"
FLUSH_PARAM = "/sys/module/d4xx/parameters/flush_every_cold_start"
START_FPS = 30
START_FRAMES = 10
RESET_POLL_INTERVAL = 0.5  # seconds between recovery polls
RESET_TIMEOUT = 15.0       # max seconds for the camera to come back
RESET_SETTLE = 2.0         # seconds after recovery before the next start

HWMC_HEADER = 0x14
HWMC_MAGIC = 0xCDAB
HWMC_OPCODE_HW_RESET = 0x20
HWMC_CONTROL_SIZE = 1028   # DS5_HWMC_BUFFER_SIZE plus the length word
HWMC_CMD_LEN = 24          # header word + 4 bytes, as the driver sends it

# Raw camera and deserializer registers, written behind the driver's back to
# put the stream in a state the driver did not cause.
DS5_HWMC_DATA = 0x4900
DS5_HWMC_EXEC = 0x490C
DS5_HWMC_EXEC_GO = 0x0001
DS5_START_STOP_STREAM = 0x1000
DS5_STREAM_START = 0x0200
DS5_STREAM_DEPTH = 0x0
DS5_STREAM_RGB = 0x1
MAX96712_DRIVER_DIR = "/sys/bus/i2c/drivers/max96712"
MAX96712_LINK_EN = 0x0006
RAW_RESET_SETTLE = 1.5     # seconds for a raw reset to take the camera down
RAW_START_SETTLE = 0.5     # seconds for a raw start to reach the camera
LINK_OUTAGE = 3.5          # seconds, longer than the driver's start window

LOG_STOP_IDLE = "in 0 state already"
LOG_START_LIVE = "in 1 state already"
LOG_START_TIMEOUT = "toggle to 1 timeout"
LOG_STOP_FAILED = ("status i2c read failed", "toggle to 0 timeout")
LOG_FORCED_STOP = "forcing stop"

H264_FPS = (30, 60, 90)
H264_FRAMES = 3
H264_WIDTH, H264_HEIGHT = 640, 480
NAL_IDR, NAL_SPS, NAL_PPS = 5, 7, 8
NAL_TYPE_MASK = 0x1F


def _dmesg():
    for cmd in (["dmesg"], ["sudo", "-n", "dmesg"]):
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode == 0:
            return res.stdout
    pytest.skip("kernel log not readable (needs root or passwordless sudo)")


LOG_STAMP = re.compile(r"^\[\s*(\d+\.\d+)\]")


def _stamped_lines():
    for line in _dmesg().splitlines():
        m = LOG_STAMP.match(line)
        if m:
            yield float(m.group(1)), line


def _log_mark():
    """Time of the newest kernel log line; the ring buffer may have wrapped,
    so new lines are found by time, not by count."""
    return max((t for t, _ in _stamped_lines()), default=0.0)


def _new_log(mark):
    return "\n".join(line for t, line in _stamped_lines() if t > mark)


def _set_flush_param(value):
    res = subprocess.run(["sudo", "-n", "tee", FLUSH_PARAM],
                         input=f"{value}\n", capture_output=True, text=True)
    if res.returncode != 0:
        pytest.skip(f"cannot write {FLUSH_PARAM}: {res.stderr.strip()}")


def _depth_mode(dev):
    for fs in dev.enum_framesizes(ioctls.V4L2_PIX_FMT_Z16):
        if fs.type == ioctls.V4L2_FRMSIZE_TYPE_DISCRETE:
            return fs.discrete.width, fs.discrete.height
    pytest.skip("no discrete Z16 mode")


def _cold_start(camera):
    """One cold depth start and stop; returns the flushes it caused."""
    mark = _log_mark()
    with V4L2Device(camera.depth_path) as dev:
        dev.set_format(*_depth_mode(dev), ioctls.V4L2_PIX_FMT_Z16)
        dev.set_parm(START_FPS)
        with StreamContext(dev) as stream:
            assert stream.capture_frames(START_FRAMES), "no depth frames"
    return _new_log(mark).count(FLUSH_LOG)


def _is_d58x(camera):
    """Aligned depth is registered on the D58x depth node only."""
    with V4L2Device(camera.depth_path) as dev:
        try:
            dev.query_ctrl(C.D500_CAMERA_CID_ALIGNED_DEPTH)
            return True
        except OSError:
            return False


def _wait_recovered(camera):
    start = time.monotonic()
    while time.monotonic() - start < RESET_TIMEOUT:
        time.sleep(RESET_POLL_INTERVAL)
        try:
            with V4L2Device(camera.depth_path) as dev:
                read_u32_control(dev, C.DS5_CAMERA_CID_FW_VERSION)
            time.sleep(RESET_SETTLE)
            return
        except OSError:
            continue
    pytest.fail(f"camera did not recover within {RESET_TIMEOUT}s")


def _skip_d58x(camera):
    # The D58x prototype link recovery self-deadlocks on reset (separate bug).
    if _is_d58x(camera):
        pytest.skip("D58x HW reset hangs in deserializer link recovery")


def _other_d4xx(camera, all_cameras):
    """Another D4xx on the same deserializer, or skip."""
    bus, _ = _i2c_target(camera)
    others = [c for c in all_cameras if c is not camera
              and _i2c_target(c)[0] == bus and not _is_d58x(c)]
    if not others:
        pytest.skip("no other D4xx on this camera's deserializer")
    return others[0]


@pytest.fixture
def flush_link(camera):
    """A camera whose link has a per-link flush; leaves the clean-link rule on."""
    try:
        open(FLUSH_PARAM).close()
    except OSError:
        pytest.skip(f"{FLUSH_PARAM} missing (driver without RSDEV-16879)")
    _set_flush_param(1)
    try:
        if _cold_start(camera) < 1:
            pytest.skip("no per-link flush on this camera's deserializer")
        links = re.findall(r"link (\d+) " + FLUSH_LOG, _dmesg())
        camera.gmsl_link = int(links[-1]) if links else None
    finally:
        _set_flush_param(0)
    _cold_start(camera)  # this camera is now the deserializer's last start
    return camera


@pytest.mark.d457
@pytest.mark.d401
@pytest.mark.d585
class TestColdStartFlush:

    def test_repeated_cold_start_skips_flush(self, flush_link):
        assert _cold_start(flush_link) == 0
        assert _cold_start(flush_link) == 0

    def test_start_while_other_camera_streams(self, flush_link, all_cameras):
        """A start while another camera streams leaves the history unknown."""
        _skip_d58x(flush_link)
        other = _other_d4xx(flush_link, all_cameras)
        with V4L2Device(other.depth_path) as dev:
            _open_depth(dev)
            with StreamContext(dev) as stream:
                stream.capture_frames(START_FRAMES)
                assert _cold_start(flush_link) == 1, "start after the other camera's did not flush"
                assert _cold_start(flush_link) == 1, "restart while the other camera streams did not flush"
        # This camera started last and the other one is idle: only the
        # streaming the start recorded explains the flush.
        _assert_flush_then_clean(flush_link)

    def test_flush_every_cold_start_param(self, flush_link):
        _set_flush_param(1)
        try:
            assert _cold_start(flush_link) == 1
            assert _cold_start(flush_link) == 1
        finally:
            _set_flush_param(0)
        assert _cold_start(flush_link) == 0


@pytest.mark.d457
@pytest.mark.d401
class TestResetForcesFlush:
    """A reset leaves a partial frame behind: the next cold start flushes once."""

    def test_hw_reset_flushes_next_start(self, flush_link):
        _skip_d58x(flush_link)
        with V4L2Device(flush_link.depth_path) as dev:
            write_int_control(dev, C.DS5_CAMERA_CID_HW_RESET, 1)
        _wait_recovered(flush_link)
        assert _cold_start(flush_link) == 1
        assert _cold_start(flush_link) == 0

    def test_hw_reset_flushes_other_camera(self, flush_link, all_cameras):
        """A reset's link recovery touches the whole deserializer."""
        _skip_d58x(flush_link)
        other = _other_d4xx(flush_link, all_cameras)
        _cold_start(other)
        assert _cold_start(other) == 0, "other camera's link not clean"
        with V4L2Device(flush_link.depth_path) as dev:
            write_int_control(dev, C.DS5_CAMERA_CID_HW_RESET, 1)
        _wait_recovered(flush_link)
        assert _cold_start(other) == 1, "other camera did not flush after the reset"
        # Until the reset camera's own start flushes, it may still send frames.
        assert _cold_start(flush_link) == 1, "reset camera did not flush"
        _assert_flush_then_clean(other)

    def test_legacy_hwmc_reset_flushes_next_start(self, flush_link):
        _skip_d58x(flush_link)
        cmd = struct.pack("<HHI4I", HWMC_HEADER, HWMC_MAGIC,
                          HWMC_OPCODE_HW_RESET, 0, 0, 0, 0)
        buf = (ctypes.c_uint8 * HWMC_CONTROL_SIZE)(*cmd)
        with V4L2Device(flush_link.depth_path) as dev:
            try:
                dev.set_ext_ctrls(C.DS5_CAMERA_CID_HWMC, HWMC_CONTROL_SIZE,
                                  ctypes.addressof(buf))
            except OSError:
                pass  # the camera reboots before it can answer
        _wait_recovered(flush_link)
        assert _cold_start(flush_link) == 1
        assert _cold_start(flush_link) == 0


def _i2c_target(camera):
    """(bus, address) from the node name, e.g. "DS5 mux 9-001a"."""
    m = re.search(r"(\d+)-([0-9a-f]{4})$", camera.card.strip())
    if not m:
        pytest.skip(f"no I2C address in card name {camera.card!r}")
    return int(m.group(1)), int(m.group(2), 16)


def _i2c(bus, addr, out, read=0):
    cmd = ["sudo", "-n", "i2ctransfer", "-y", "-f", str(bus),
           f"w{len(out)}@{addr:#x}"] + [hex(b) for b in out]
    if read:
        cmd.append(f"r{read}")
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0 and "not found" in res.stderr + res.stdout:
        pytest.skip("i2ctransfer not available")
    return res


def _cam_write(camera, reg, data):
    """Camera register write; the address goes little-endian on the wire."""
    bus, addr = _i2c_target(camera)
    return _i2c(bus, addr, [reg & 0xFF, reg >> 8] + list(data))


def _raw_reset(camera):
    cmd = struct.pack("<HHI4I", HWMC_HEADER, HWMC_MAGIC,
                      HWMC_OPCODE_HW_RESET, 0, 0, 0, 0)[:HWMC_CMD_LEN]
    _cam_write(camera, DS5_HWMC_DATA, cmd)
    _cam_write(camera, DS5_HWMC_EXEC, struct.pack("<H", DS5_HWMC_EXEC_GO))


def _raw_start(camera, stream_id):
    cmd = struct.pack("<H", DS5_STREAM_START | stream_id)
    assert _cam_write(camera, DS5_START_STOP_STREAM, cmd).returncode == 0


def _deser(camera):
    """(bus, address) of the camera's max96712; the register goes big-endian."""
    bus, _ = _i2c_target(camera)
    nodes = glob.glob(os.path.join(MAX96712_DRIVER_DIR, f"{bus}-00*"))
    if not nodes or camera.gmsl_link is None:
        pytest.skip("camera is not behind a max96712 with a known link")
    return bus, int(os.path.basename(nodes[0]).split("-")[1], 16)


def _link_outage(camera):
    """Drop the camera's deserializer link; returns the call that restores it."""
    bus, addr = _deser(camera)
    reg = [MAX96712_LINK_EN >> 8, MAX96712_LINK_EN & 0xFF]
    link_en = int(_i2c(bus, addr, reg, read=1).stdout, 16)
    began = time.monotonic()
    _i2c(bus, addr, reg + [link_en & ~(1 << camera.gmsl_link)])

    def restore():
        time.sleep(max(0.0, LINK_OUTAGE - (time.monotonic() - began)))
        _i2c(bus, addr, reg + [link_en])
    return restore


def _open_depth(dev):
    dev.set_format(*_depth_mode(dev), ioctls.V4L2_PIX_FMT_Z16)
    dev.set_parm(START_FPS)


def _assert_flush_then_clean(camera):
    assert _cold_start(camera) == 1, "next cold start did not flush"
    assert _cold_start(camera) == 0, "the start after it flushed again"


@pytest.mark.d457
@pytest.mark.d401
class TestLinkDirtyTriggers:
    """Each stream path that leaves the link state unknown forces one flush.

    The camera is driven into that state with raw I2C writes the driver does
    not see, so these tests need passwordless sudo and i2ctransfer.
    """

    def test_stop_finding_stream_idle(self, flush_link):
        _skip_d58x(flush_link)
        mark = _log_mark()
        with V4L2Device(flush_link.depth_path) as dev:
            _open_depth(dev)
            with StreamContext(dev) as stream:
                stream.capture_frames(START_FRAMES)
                _raw_reset(flush_link)
                time.sleep(RAW_RESET_SETTLE)
        assert LOG_STOP_IDLE in _new_log(mark), "stop did not find the stream idle"
        _wait_recovered(flush_link)
        _assert_flush_then_clean(flush_link)

    def test_start_camera_reports_live(self, flush_link):
        _skip_d58x(flush_link)
        mark = _log_mark()
        _raw_start(flush_link, DS5_STREAM_DEPTH)
        time.sleep(RAW_START_SETTLE)
        assert _cold_start(flush_link) == 0, "a start on a live stream flushed it"
        assert LOG_START_LIVE in _new_log(mark), "start did not find the stream live"
        _assert_flush_then_clean(flush_link)

    def test_failed_start(self, flush_link):
        _skip_d58x(flush_link)
        mark = _log_mark()
        with V4L2Device(flush_link.depth_path) as dev:
            _open_depth(dev)
            restore = _link_outage(flush_link)
            try:
                with pytest.raises(OSError):
                    with StreamContext(dev) as stream:
                        stream.capture_frames(START_FRAMES)
            finally:
                restore()
        assert LOG_START_TIMEOUT in _new_log(mark), "start did not time out"
        _wait_recovered(flush_link)
        _assert_flush_then_clean(flush_link)

    def test_failed_stop(self, flush_link):
        _skip_d58x(flush_link)
        mark = _log_mark()
        restore = None
        with V4L2Device(flush_link.depth_path) as dev:
            _open_depth(dev)
            try:
                with StreamContext(dev) as stream:
                    stream.capture_frames(START_FRAMES)
                    restore = _link_outage(flush_link)
            except OSError:
                pass  # the stop may report the failure
            finally:
                if restore:
                    restore()
        log = _new_log(mark)
        assert any(m in log for m in LOG_STOP_FAILED), "stop did not fail"
        _wait_recovered(flush_link)
        # The driver still counts the stream as running: this start is a
        # no-op and its stop is the one that reaches the camera.
        assert _cold_start(flush_link) == 0, "start after a failed stop flushed"
        _assert_flush_then_clean(flush_link)

    def test_forced_stop(self, flush_link):
        """A colour start forced to stop, while depth keeps the link busy.

        Depth's cold start after the reset already flushed for the reset, so
        only the forced stop can make the next cold start flush.
        """
        _skip_d58x(flush_link)
        with V4L2Device(flush_link.depth_path) as dev:
            write_int_control(dev, C.DS5_CAMERA_CID_HW_RESET, 1)
        _wait_recovered(flush_link)
        mark = _log_mark()
        with V4L2Device(flush_link.depth_path) as depth:
            _open_depth(depth)
            with StreamContext(depth) as stream:
                stream.capture_frames(START_FRAMES)
                assert _new_log(mark).count(FLUSH_LOG) == 1, \
                    "depth start after the reset did not flush"
                _raw_start(flush_link, DS5_STREAM_RGB)
                time.sleep(RAW_START_SETTLE)
                with V4L2Device(flush_link.rgb_path) as rgb:
                    rgb.set_parm(START_FPS)
                    with StreamContext(rgb) as colour:
                        colour.capture_frames(START_FRAMES)
        assert LOG_FORCED_STOP in _new_log(mark), "colour start did not force a stop"
        _assert_flush_then_clean(flush_link)


def _nal_types(data):
    types, off = [], 0
    while True:
        off = data.find(b"\x00\x00\x01", off)
        if off < 0 or off + 3 >= len(data):
            return types
        types.append(data[off + 3] & NAL_TYPE_MASK)
        off += 3


def _h264_rates(path):
    with V4L2Device(path) as dev:
        if not any(f.pixelformat == ioctls.V4L2_PIX_FMT_H264
                   for f in dev.enum_formats()):
            pytest.skip("no H.264 on the colour node")
        ivals = dev.enum_frameintervals(ioctls.V4L2_PIX_FMT_H264,
                                        H264_WIDTH, H264_HEIGHT)
        return {iv.discrete.denominator // iv.discrete.numerator
                for iv in ivals
                if iv.type == ioctls.V4L2_FRMIVAL_TYPE_DISCRETE}


@pytest.mark.d585
@pytest.mark.parametrize("fps", H264_FPS)
def test_h264_first_buffer_is_key_frame(flush_link, fps):
    """The first buffer of a repeated cold start holds SPS, PPS and IDR."""
    path = flush_link.rgb_path
    if fps not in _h264_rates(path):
        pytest.skip(f"H.264 {H264_WIDTH}x{H264_HEIGHT} has no {fps} fps")
    for _ in range(2):  # the first start may flush for another camera
        with V4L2Device(path) as dev:
            dev.set_format(H264_WIDTH, H264_HEIGHT, ioctls.V4L2_PIX_FMT_H264)
            dev.set_parm(fps)
            with StreamContext(dev) as stream:
                _, first = stream.capture_frames(H264_FRAMES)[0]
    types = _nal_types(first)
    assert {NAL_SPS, NAL_PPS, NAL_IDR} <= set(types), \
        f"first buffer NAL types {types} at {fps} fps"
