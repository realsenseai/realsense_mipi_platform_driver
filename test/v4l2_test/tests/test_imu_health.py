"""GMSL health stream on the IMU node: the 38 B default and the 256 B health width.

The host selects the health width by setting the IMU node to 256 x 1, a size only
the camera's format descriptor offers; the FW sizes its IMU-VC packets from that
width. Health reports and IMU records share the node and differ by their first byte.
Each channel is switched on, paced and (logs) filtered by V4L2 controls on the IMU node.
"""

import errno
import re
import struct
import time
from collections import defaultdict

import pytest

from ..d4xx import constants as C
from ..v4l2.device import V4L2Device
from ..v4l2.stream import StreamContext

IMU_RECORD_BYTES = 38
HEALTH_REPORT_BYTES = 256
IMU_REPORT_IDS = range(1, 4)          # accel, gyro, custom
IMU_SAMPLE_IDS = (1, 2)               # the 38 B records with a 64-bit timestamp
HEALTH_REPORT_IDS = range(4, 8)       # errors, logs, sensors/HW, metrics
# channelId, version, sequence, payloadSize, lostCount, status, sessionId, reportTime (us)
HEALTH_HEADER = struct.Struct("<BBBBBBHI")
HEALTH_VERSION = 2
HEALTH_PAYLOAD_MAX = HEALTH_REPORT_BYTES - HEALTH_HEADER.size
HEALTH_STATUS_NO_DATA = 0x01
# Payload item size per channel (design 4.3/4.5); 0 = keep-alive only.
HEALTH_ITEM_BYTES = {4: 6, 5: 20, 6: 28, 7: 0}
HEALTH_TIME_WRAP = 1 << 32
US_PER_S = 1_000_000
IMU_TIMESTAMP = struct.Struct("<Q")
IMU_TIMESTAMP_OFFSET = 2

CAPTURE_SECONDS = 10
KEEPALIVE_SECONDS = 20
KEEPALIVE_PERIOD_S = 1.0               # interim default rate, every channel
SENDER_TICK_S = 0.1
# a due report can slip a tick when the link is busy (design T-4)
KEEPALIVE_MEAN_SLIP_TICKS = 2
KEEPALIVE_GAP_SLIP_TICKS = 3
# reports a channel may miss across the window: the capture edges cut the first and last period
KEEPALIVE_EDGE_PERIODS = 2
DEQUEUE_TIMEOUT_S = 2.0
# IMU rate at 256 B must stay within this fraction of the 38 B rate.
IMU_RATE_TOLERANCE = 0.1

# Health controls: channel n (errors, logs, sensors, metrics) sends report id 4 + n.
HEALTH_CHANNELS = range(len(C.DS5_CAMERA_CID_HEALTH_ENABLE))
HEALTH_LOGS = 1
HEALTH_METRICS = 3
PERIOD_MIN_MS = 100
PERIOD_MAX_MS = 3_600_000
PERIOD_DEF_MS = 1000
SEVERITY_WARNING = 2
SEVERITY_DISABLE = 5
MS_PER_S = 1000
CHANNEL_PERIOD_MS = 300
CHANNEL_SECONDS = 4
OFF_SECONDS = 2
# lines already queued in VI when a channel goes off still dequeue after it
OFF_HOST_SLACK_S = 0.5
RESET_TIMEOUT_S = 30
RESET_POLL_S = 0.5

_IMU_RES_RE = re.compile(
    r"static const struct ds5_resolution (\w*imu\w*)\[\] = \{(.*?)\n\};", re.S)


def _builtin_imu_widths(src=C._D4XX_C):
    """Every width the D4xx built-in IMU tables offer, by table name. D58x
    pixel mode runs 256 B for sync spacing, not health, so it is left out."""
    text = src.read_text()
    return {name: [int(w) for w in re.findall(r"\.width\s*=\s*(\d+)", body)]
            for name, body in _IMU_RES_RE.findall(text) if "d58x" not in name}


def _imu_sizes(dev):
    """Enumerated IMU sizes in driver order (the first is the default)."""
    pixfmt = dev.enum_formats()[0].pixelformat
    return pixfmt, [(s.discrete.width, s.discrete.height)
                    for s in dev.enum_framesizes(pixfmt)]


def _set_imu_width(dev, pixfmt, width):
    """S_FMT then read back: set_fmt picks the closest size, never fails.
    Returns the line pitch: VI pads each line, so a 38 B record fills 64 B."""
    dev.set_format(width, 1, pixfmt)
    pix = dev.get_format().fmt.pix
    assert pix.width == width, f"IMU width read back {pix.width}, asked {width}"
    return pix.bytesperline


def _capture_lines(dev, seconds):
    """[(host_time_s, bytes)] for every IMU-node line in the window."""
    lines = []
    with StreamContext(dev) as stream:
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            buf, data = stream.dequeue(timeout=DEQUEUE_TIMEOUT_S)
            lines.append((time.monotonic(), data))
            stream.requeue(buf)
    return lines


def _split(lines):
    assert all(d for _, d in lines), "empty IMU-node line"
    imu = [(t, d) for t, d in lines if d[0] in IMU_REPORT_IDS]
    health = [(t, d) for t, d in lines if d[0] in HEALTH_REPORT_IDS]
    other = [d[0] for _, d in lines if d[0] not in IMU_REPORT_IDS
             and d[0] not in HEALTH_REPORT_IDS]
    return imu, health, other


def _check_imu(imu, pitch):
    """Size, zero padding, per-sensor timestamps never go backwards."""
    last = {}
    for _, d in imu:
        assert len(d) == pitch, f"IMU line {len(d)} B, pitch {pitch}"
        assert not any(d[IMU_RECORD_BYTES:]), "IMU line padding not zero"
        if d[0] not in IMU_SAMPLE_IDS:
            continue
        ts, = IMU_TIMESTAMP.unpack_from(d, IMU_TIMESTAMP_OFFSET)
        assert ts >= last.get(d[0], 0), f"IMU id {d[0]} timestamp went back"
        last[d[0]] = ts


def _imu_rate(imu, seconds):
    return len(imu) / seconds


def _check_report(d, seqs, state):
    """One health report against the 12 B header + payload layout (T-3).
    One capture is one enable: a single session id, report times moving forward per channel."""
    assert len(d) == HEALTH_REPORT_BYTES, f"health report {len(d)} B"
    ch, ver, seq, size, _lost, status, session, rtime = HEALTH_HEADER.unpack_from(d)
    assert ver == HEALTH_VERSION, f"channel {ch} version {ver}"
    assert session != 0, f"channel {ch} session id 0"
    assert state.setdefault("session", session) == session, \
        f"channel {ch} session id 0x{session:04x}, capture started in 0x{state['session']:04x}"
    if ch in state.setdefault("time", {}):
        step = (rtime - state["time"][ch]) % HEALTH_TIME_WRAP
        assert 0 < step < HEALTH_TIME_WRAP // 2, f"channel {ch} report time went back"
    state["time"][ch] = rtime
    assert size <= HEALTH_PAYLOAD_MAX, f"channel {ch} payload {size}"
    item = HEALTH_ITEM_BYTES[ch]
    assert (size % item == 0) if item else size == 0, \
        f"channel {ch} payload {size} not whole {item} B items"
    assert not any(d[HEALTH_HEADER.size + size:]), \
        f"channel {ch} bytes after payload not zero"
    assert size or status & HEALTH_STATUS_NO_DATA, \
        f"channel {ch} empty payload without no-data status"
    if ch in seqs:
        assert seq == (seqs[ch] + 1) & 0xFF, \
            f"channel {ch} sequence {seqs[ch]} -> {seq}"
    seqs[ch] = seq


@pytest.fixture
def imu_device(camera, request):
    """The D4xx IMU node; its width goes back to the default after the test.
    --imu-node pins it where the node layout is not six per camera."""
    if camera.fw_version and int(camera.fw_version.split(".")[0]) in C.D58X_FW_MAJORS:
        pytest.skip(f"D58x (FW {camera.fw_version}): IMU widths differ")
    dev = V4L2Device(request.config.getoption("--imu-node") or camera.imu_path)
    dev.open()
    pixfmt, sizes = _imu_sizes(dev)
    if not sizes:
        dev.close()
        pytest.skip("IMU node lists no frame sizes")
    yield dev
    try:
        dev.set_format(sizes[0][0], sizes[0][1], pixfmt)
    finally:
        dev.close()


@pytest.fixture
def imu_wide(imu_device, request):
    """The IMU node, only if the camera offers the 256 B health width.
    A health-on image must offer it, so its absence then fails."""
    _, sizes = _imu_sizes(imu_device)
    if (HEALTH_REPORT_BYTES, 1) not in sizes:
        msg = f"IMU sizes {sizes}: no {HEALTH_REPORT_BYTES} x 1"
        if request.config.getoption("--imu-health"):
            pytest.fail(f"{msg}, but --imu-health expects the health width")
        pytest.skip(f"{msg}, the camera publishes no health width")
    return imu_device


def _health_ctrl_ids():
    return (*C.DS5_CAMERA_CID_HEALTH_ENABLE, *C.DS5_CAMERA_CID_HEALTH_PERIOD,
            C.DS5_CAMERA_CID_HEALTH_LOG_MIN_SEVERITY)


def _report_id(channel):
    return HEALTH_REPORT_IDS[channel]


def _health_supported(dev):
    """The camera answers health controls; EOPNOTSUPP = it can't send health."""
    try:
        dev.get_ctrl(C.DS5_CAMERA_CID_HEALTH_ENABLE[0])
    except OSError as e:
        if e.errno == errno.EOPNOTSUPP:
            return False
        raise
    return True


def _health_reset(dev):
    """Every channel off at the default period; logs filter at its default."""
    for ch in HEALTH_CHANNELS:
        dev.set_ctrl(C.DS5_CAMERA_CID_HEALTH_ENABLE[ch], 0)
        dev.set_ctrl(C.DS5_CAMERA_CID_HEALTH_PERIOD[ch], PERIOD_DEF_MS)
    dev.set_ctrl(C.DS5_CAMERA_CID_HEALTH_LOG_MIN_SEVERITY, SEVERITY_WARNING)


def _health_enable(dev, channels, period_ms=PERIOD_DEF_MS):
    for ch in channels:
        dev.set_ctrl(C.DS5_CAMERA_CID_HEALTH_PERIOD[ch], period_ms)
        dev.set_ctrl(C.DS5_CAMERA_CID_HEALTH_ENABLE[ch], 1)


def _report_times(health):
    """{report id: [FW report time in s]} in arrival order."""
    times = defaultdict(list)
    for _, d in health:
        times[d[0]].append(HEALTH_HEADER.unpack_from(d)[-1] / US_PER_S)
    return times


def _gaps(t):
    return [(b - a) % (HEALTH_TIME_WRAP / US_PER_S) for a, b in zip(t, t[1:])]


@pytest.fixture
def health_ctrl(imu_device, request):
    """The IMU node of a camera that sends health, every channel off before and after.
    --imu-health makes a camera without health a failure instead of a skip."""
    if not _health_supported(imu_device):
        msg = "health controls return EOPNOTSUPP"
        if request.config.getoption("--imu-health"):
            pytest.fail(f"{msg}, but --imu-health expects a camera that sends health")
        pytest.skip(f"{msg}: this camera can't send health")
    _health_reset(imu_device)
    yield imu_device
    _health_reset(imu_device)


@pytest.fixture
def health_wide(health_ctrl, imu_wide):
    """health_ctrl, for tests that stream health at the 256 B width."""
    return health_ctrl


@pytest.fixture
def health_on(health_wide):
    """Every channel on at the default period."""
    _health_enable(health_wide, HEALTH_CHANNELS)
    return health_wide


@pytest.mark.d457
class TestImuHealthTables:
    """The 256 B width comes only from the camera, never the built-in tables."""

    def test_builtin_tables_never_offer_health_width(self):
        if not C._D4XX_C.is_file():
            pytest.skip(f"driver source {C._D4XX_C} not on this machine")
        widths = _builtin_imu_widths()
        assert widths, "no IMU resolution table found in d4xx_main.c"
        bad = {n: w for n, w in widths.items() if HEALTH_REPORT_BYTES in w}
        assert not bad, f"built-in IMU tables offer {HEALTH_REPORT_BYTES}: {bad}"


@pytest.mark.d457
class TestImuHealthSizes:

    def test_default_is_38(self, imu_device):
        _, sizes = _imu_sizes(imu_device)
        assert sizes[0] == (IMU_RECORD_BYTES, 1), f"IMU default {sizes[0]}"

    def test_size_list(self, imu_device):
        _, sizes = _imu_sizes(imu_device)
        allowed = [(IMU_RECORD_BYTES, 1), (HEALTH_REPORT_BYTES, 1)]
        assert sizes in (allowed[:1], allowed), f"IMU sizes {sizes}"


@pytest.mark.d457
class TestImuHealthStream:

    def test_default_width_imu_only(self, imu_device):
        """T-2 at 38 B: IMU records only, never a health report."""
        pixfmt, _ = _imu_sizes(imu_device)
        pitch = _set_imu_width(imu_device, pixfmt, IMU_RECORD_BYTES)
        imu, health, other = _split(_capture_lines(imu_device, CAPTURE_SECONDS))
        assert imu, "no IMU records at 38 B"
        assert not health, f"{len(health)} health reports at 38 B"
        assert not other, f"unknown first bytes {sorted(set(other))}"
        _check_imu(imu, pitch)

    def test_wide_width_round_trip(self, imu_wide, request):
        """256 B: IMU unchanged, padded; health only from a health-on image."""
        pixfmt, _ = _imu_sizes(imu_wide)
        _set_imu_width(imu_wide, pixfmt, IMU_RECORD_BYTES)
        base, _, _ = _split(_capture_lines(imu_wide, CAPTURE_SECONDS))
        pitch = _set_imu_width(imu_wide, pixfmt, HEALTH_REPORT_BYTES)
        imu, health, other = _split(_capture_lines(imu_wide, CAPTURE_SECONDS))
        assert not other, f"unknown first bytes {sorted(set(other))}"
        _check_imu(imu, pitch)
        want, got = _imu_rate(base, CAPTURE_SECONDS), _imu_rate(imu, CAPTURE_SECONDS)
        assert abs(got - want) <= IMU_RATE_TOLERANCE * want, \
            f"IMU rate {got:.1f}/s at 256 B vs {want:.1f}/s at 38 B"
        if not request.config.getoption("--imu-health"):
            assert not health, f"{len(health)} health reports, stream is off"

    def test_wide_then_default_restart(self, imu_wide):
        """A 38 B restart after a 256 B run carries no health and clean IMU lines."""
        pixfmt, _ = _imu_sizes(imu_wide)
        _set_imu_width(imu_wide, pixfmt, HEALTH_REPORT_BYTES)
        _capture_lines(imu_wide, CAPTURE_SECONDS)
        pitch = _set_imu_width(imu_wide, pixfmt, IMU_RECORD_BYTES)
        imu, health, other = _split(_capture_lines(imu_wide, CAPTURE_SECONDS))
        assert imu, "no IMU records at 38 B after a 256 B run"
        assert not health, f"{len(health)} health reports at 38 B after a 256 B run"
        assert not other, f"unknown first bytes {sorted(set(other))}"
        _check_imu(imu, pitch)

    def test_health_report_format(self, health_on):
        """T-3: every report follows the header and payload layout; IMU lines stay clean."""
        pixfmt, _ = _imu_sizes(health_on)
        pitch = _set_imu_width(health_on, pixfmt, HEALTH_REPORT_BYTES)
        imu, health, _ = _split(_capture_lines(health_on, CAPTURE_SECONDS))
        assert health, "no health reports with every channel on"
        _check_imu(imu, pitch)
        seqs, state = {}, {}
        for _, d in health:
            _check_report(d, seqs, state)
        assert set(seqs) == set(HEALTH_REPORT_IDS), \
            f"channels seen {sorted(seqs)}"

    def test_health_keepalive(self, health_on):
        """T-4: every channel reports at its rate, never silent past a period plus 3 ticks.
        Timed on the reports' own FW time, so host dequeue bursts do not count."""
        pixfmt, _ = _imu_sizes(health_on)
        _set_imu_width(health_on, pixfmt, HEALTH_REPORT_BYTES)
        _, health, _ = _split(_capture_lines(health_on, KEEPALIVE_SECONDS))
        times = defaultdict(list)
        for _, d in health:
            times[d[0]].append(HEALTH_HEADER.unpack_from(d)[-1])
        for ch in HEALTH_REPORT_IDS:
            t = times.get(ch, [])
            want = int(KEEPALIVE_SECONDS / KEEPALIVE_PERIOD_S) - KEEPALIVE_EDGE_PERIODS
            assert len(t) >= want, f"channel {ch}: {len(t)} reports in {KEEPALIVE_SECONDS} s, want {want}"
            gaps = [((b - a) % HEALTH_TIME_WRAP) / US_PER_S for a, b in zip(t, t[1:])]
            worst = max(gaps)
            assert worst <= KEEPALIVE_PERIOD_S + KEEPALIVE_GAP_SLIP_TICKS * SENDER_TICK_S, \
                f"channel {ch} silent {worst:.2f} s"
            mean = sum(gaps) / len(gaps)
            assert abs(mean - KEEPALIVE_PERIOD_S) <= KEEPALIVE_MEAN_SLIP_TICKS * SENDER_TICK_S, \
                f"channel {ch} mean interval {mean:.2f} s"


@pytest.mark.d457
class TestImuHealthControl:
    """Per-channel enable, period and log filter through V4L2 controls on the IMU node."""

    def test_controls_listed(self, imu_device):
        """All nine controls are registered with the USB report's ranges."""
        for cid in C.DS5_CAMERA_CID_HEALTH_ENABLE:
            qc = imu_device.query_ctrl(cid)
            assert (qc.minimum, qc.maximum, qc.default_value) == (0, 1, 0), \
                f"enable 0x{cid:x} range {qc.minimum}..{qc.maximum} def {qc.default_value}"
        for cid in C.DS5_CAMERA_CID_HEALTH_PERIOD:
            qc = imu_device.query_ctrl(cid)
            got = (qc.minimum, qc.maximum, qc.step, qc.default_value)
            assert got == (PERIOD_MIN_MS, PERIOD_MAX_MS, PERIOD_MIN_MS, PERIOD_DEF_MS), \
                f"period 0x{cid:x} min/max/step/def {got}"
        qc = imu_device.query_ctrl(C.DS5_CAMERA_CID_HEALTH_LOG_MIN_SEVERITY)
        assert (qc.minimum, qc.maximum, qc.default_value) == \
            (0, SEVERITY_DISABLE, SEVERITY_WARNING), "log severity range"

    def test_unsupported_refused(self, imu_device):
        """A camera that can't send health refuses every health get and set."""
        if _health_supported(imu_device):
            pytest.skip("this camera sends health")
        for cid in _health_ctrl_ids():
            with pytest.raises(OSError) as e:
                imu_device.get_ctrl(cid)
            assert e.value.errno == errno.EOPNOTSUPP, f"get 0x{cid:x}: {e.value}"
        for cid in C.DS5_CAMERA_CID_HEALTH_ENABLE:
            with pytest.raises(OSError) as e:
                imu_device.set_ctrl(cid, 0)
            assert e.value.errno == errno.EOPNOTSUPP, f"set 0x{cid:x}: {e.value}"

    def test_set_get(self, health_ctrl):
        """Every value set reads back from the camera."""
        for ch in HEALTH_CHANNELS:
            for value in (1, 0):
                health_ctrl.set_ctrl(C.DS5_CAMERA_CID_HEALTH_ENABLE[ch], value)
                got = health_ctrl.get_ctrl(C.DS5_CAMERA_CID_HEALTH_ENABLE[ch]).value
                assert got == value, f"channel {ch} enable {got}, set {value}"
            for value in (PERIOD_MIN_MS, CHANNEL_PERIOD_MS, PERIOD_MAX_MS):
                health_ctrl.set_ctrl(C.DS5_CAMERA_CID_HEALTH_PERIOD[ch], value)
                got = health_ctrl.get_ctrl(C.DS5_CAMERA_CID_HEALTH_PERIOD[ch]).value
                assert got == value, f"channel {ch} period {got}, set {value}"
        for value in range(SEVERITY_DISABLE + 1):
            health_ctrl.set_ctrl(C.DS5_CAMERA_CID_HEALTH_LOG_MIN_SEVERITY, value)
            got = health_ctrl.get_ctrl(C.DS5_CAMERA_CID_HEALTH_LOG_MIN_SEVERITY).value
            assert got == value, f"log severity {got}, set {value}"

    def test_bad_severity_refused(self, health_ctrl):
        cid = C.DS5_CAMERA_CID_HEALTH_LOG_MIN_SEVERITY
        with pytest.raises(OSError) as e:
            health_ctrl.set_ctrl(cid, SEVERITY_DISABLE + 1)
        assert e.value.errno in (errno.EINVAL, errno.ERANGE), str(e.value)
        assert health_ctrl.get_ctrl(cid).value == SEVERITY_WARNING

    def test_all_off_no_health(self, health_wide):
        """256 B with every channel off: IMU only."""
        pixfmt, _ = _imu_sizes(health_wide)
        _set_imu_width(health_wide, pixfmt, HEALTH_REPORT_BYTES)
        imu, health, _ = _split(_capture_lines(health_wide, CAPTURE_SECONDS))
        assert imu, "no IMU records"
        assert not health, f"{len(health)} health reports with every channel off"

    @pytest.mark.parametrize("channel", HEALTH_CHANNELS)
    def test_single_channel(self, health_wide, channel):
        """One channel on: only its reports arrive, never silent past its period plus 3 ticks."""
        _health_enable(health_wide, [channel], CHANNEL_PERIOD_MS)
        pixfmt, _ = _imu_sizes(health_wide)
        _set_imu_width(health_wide, pixfmt, HEALTH_REPORT_BYTES)
        _, health, _ = _split(_capture_lines(health_wide, CHANNEL_SECONDS))
        times = _report_times(health)
        assert set(times) == {_report_id(channel)}, f"report ids {sorted(times)}"
        period = CHANNEL_PERIOD_MS / MS_PER_S
        t = times[_report_id(channel)]
        want = int(CHANNEL_SECONDS / period) - KEEPALIVE_EDGE_PERIODS
        assert len(t) >= want, f"{len(t)} reports in {CHANNEL_SECONDS} s, want {want}"
        worst = max(_gaps(t))
        assert worst <= period + KEEPALIVE_GAP_SLIP_TICKS * SENDER_TICK_S, \
            f"silent {worst:.2f} s at a {period} s period"

    def test_off_stops_channel(self, health_wide):
        """Switching a channel off mid-stream stops its reports within one period."""
        _health_enable(health_wide, [HEALTH_METRICS], CHANNEL_PERIOD_MS)
        pixfmt, _ = _imu_sizes(health_wide)
        _set_imu_width(health_wide, pixfmt, HEALTH_REPORT_BYTES)
        cid = C.DS5_CAMERA_CID_HEALTH_ENABLE[HEALTH_METRICS]
        off_at = None
        last = None
        with StreamContext(health_wide) as stream:
            start = time.monotonic()
            while time.monotonic() < start + 2 * OFF_SECONDS:
                buf, data = stream.dequeue(timeout=DEQUEUE_TIMEOUT_S)
                now = time.monotonic()
                if data[0] == _report_id(HEALTH_METRICS):
                    last = now
                stream.requeue(buf)
                if off_at is None and now >= start + OFF_SECONDS:
                    health_wide.set_ctrl(cid, 0)
                    off_at = time.monotonic()
        assert last is not None, "no metrics reports before switching off"
        limit = off_at + CHANNEL_PERIOD_MS / MS_PER_S + OFF_HOST_SLACK_S
        assert last <= limit, f"metrics report {last - off_at:.2f} s after off"

    def test_log_filter_disable(self, health_wide):
        """Logs at minimum severity Disable: keep-alives only after the first report,
        which may still flush logs queued before the filter changed."""
        health_wide.set_ctrl(C.DS5_CAMERA_CID_HEALTH_LOG_MIN_SEVERITY, SEVERITY_DISABLE)
        _health_enable(health_wide, [HEALTH_LOGS], CHANNEL_PERIOD_MS)
        pixfmt, _ = _imu_sizes(health_wide)
        _set_imu_width(health_wide, pixfmt, HEALTH_REPORT_BYTES)
        _, health, _ = _split(_capture_lines(health_wide, CHANNEL_SECONDS))
        logs = [d for _, d in health if d[0] == _report_id(HEALTH_LOGS)]
        assert len(logs) > 1, f"{len(logs)} log reports"
        sizes = [HEALTH_HEADER.unpack_from(d)[3] for d in logs[1:]]
        assert not any(sizes), f"log payload sizes {sizes} with the filter at Disable"

    def test_hw_reset_turns_off(self, health_ctrl, camera):
        """A camera HW reset returns every channel to off."""
        _health_enable(health_ctrl, HEALTH_CHANNELS)
        with V4L2Device(camera.depth_path) as dev:
            dev.set_ctrl(C.DS5_CAMERA_CID_HW_RESET, 1)
        end = time.monotonic() + RESET_TIMEOUT_S
        while True:
            try:
                got = [health_ctrl.get_ctrl(cid).value
                       for cid in C.DS5_CAMERA_CID_HEALTH_ENABLE]
                break
            except OSError as e:
                # EBUSY while the camera model is unknown, I2C errors while it reboots
                if time.monotonic() > end:
                    pytest.fail(f"health controls still fail {RESET_TIMEOUT_S} s after reset: {e}")
                time.sleep(RESET_POLL_S)
        assert got == [0] * len(got), f"enables after reset {got}"
