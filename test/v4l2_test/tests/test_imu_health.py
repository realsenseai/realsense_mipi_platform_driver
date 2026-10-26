"""GMSL health stream on the IMU node: the 38 B default and the 256 B health width.

The host selects the health width by setting the IMU node to 256 x 1, a size only
the camera's format descriptor offers; the FW sizes its IMU-VC packets from that
width. Health reports and IMU records share the node and differ by their first byte.
"""

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


@pytest.fixture
def health_on(request):
    if not request.config.getoption("--imu-health"):
        pytest.skip("needs a health-on FW image: pass --imu-health")


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

    def test_health_report_format(self, imu_wide, health_on):
        """T-3: every report follows the header and payload layout."""
        pixfmt, _ = _imu_sizes(imu_wide)
        _set_imu_width(imu_wide, pixfmt, HEALTH_REPORT_BYTES)
        _, health, _ = _split(_capture_lines(imu_wide, CAPTURE_SECONDS))
        assert health, "health-on image sent no health reports"
        seqs, state = {}, {}
        for _, d in health:
            _check_report(d, seqs, state)
        assert set(seqs) == set(HEALTH_REPORT_IDS), \
            f"channels seen {sorted(seqs)}"

    def test_health_keepalive(self, imu_wide, health_on):
        """T-4: every channel reports at its rate, never silent past a period plus 3 ticks.
        Timed on the reports' own FW time, so host dequeue bursts do not count."""
        pixfmt, _ = _imu_sizes(imu_wide)
        _set_imu_width(imu_wide, pixfmt, HEALTH_REPORT_BYTES)
        _, health, _ = _split(_capture_lines(imu_wide, KEEPALIVE_SECONDS))
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
