"""Stream at various resolutions/FPS, validate frame rate and sequence."""

import time

import pytest

from ..d4xx import constants as C
from ..v4l2 import ioctls
from ..v4l2.device import V4L2Device
from ..v4l2.stream import StreamContext


# Common depth resolutions to test (subset that all D4XX models support)
DEPTH_CONFIGS = [
    (848, 480, ioctls.V4L2_PIX_FMT_Z16, 30),
    (640, 480, ioctls.V4L2_PIX_FMT_Z16, 30),
    (640, 360, ioctls.V4L2_PIX_FMT_Z16, 30),
    (480, 270, ioctls.V4L2_PIX_FMT_Z16, 30),
    (424, 240, ioctls.V4L2_PIX_FMT_Z16, 30),
    (848, 480, ioctls.V4L2_PIX_FMT_Z16, 60),
    (640, 480, ioctls.V4L2_PIX_FMT_Z16, 60),
    (424, 240, ioctls.V4L2_PIX_FMT_Z16, 90),
]

# IR configs
IR_CONFIGS = [
    (848, 480, ioctls.V4L2_PIX_FMT_GREY, 30),
    (640, 480, ioctls.V4L2_PIX_FMT_GREY, 30),
    (424, 240, ioctls.V4L2_PIX_FMT_GREY, 30),
]

FPS_TOLERANCE = 0.05  # 5%
FRAME_COUNT = 60
MAX_CONSECUTIVE_DROPS = 2
MIN_FRAME_ARRIVAL = 0.90  # 90% of requested frames must arrive


def _stream_and_validate(device_path, width, height, pixelformat, fps,
                         buf_type=ioctls.V4L2_BUF_TYPE_VIDEO_CAPTURE):
    """Configure format, start stream, capture frames, validate FPS."""
    with V4L2Device(device_path) as dev:
        if buf_type == ioctls.V4L2_BUF_TYPE_META_CAPTURE:
            dev.set_meta_format(pixelformat, buf_type)
        else:
            dev.set_format(width, height, pixelformat, buf_type)
        dev.set_parm(fps, buf_type)

        with StreamContext(dev, buf_type=buf_type) as stream:
            frames = stream.capture_frames(
                FRAME_COUNT,
                timeout=max(5.0, 4.0 * FRAME_COUNT / fps),
            )

    assert len(frames) >= int(FRAME_COUNT * MIN_FRAME_ARRIVAL), \
        f"Only {len(frames)}/{FRAME_COUNT} frames arrived"

    # Validate sequence numbers
    sequences = [buf.sequence for buf, _ in frames]
    for i in range(1, len(sequences)):
        assert sequences[i] > sequences[i - 1], \
            f"Non-monotonic sequence: {sequences[i-1]} -> {sequences[i]}"
        gap = sequences[i] - sequences[i - 1]
        assert gap <= MAX_CONSECUTIVE_DROPS + 1, \
            f"Frame drop: gap={gap} between seq {sequences[i-1]} and {sequences[i]}"

    # Validate FPS from timestamps (skip first frame for warm-up)
    timestamps = []
    for buf, _ in frames:
        ts = buf.timestamp.tv_sec + buf.timestamp.tv_usec / 1e6
        timestamps.append(ts)

    if len(timestamps) >= 3:
        for i in range(2, len(timestamps)):
            dt = timestamps[i] - timestamps[i - 1]
            if dt > 0:
                measured_fps = 1.0 / dt
                assert measured_fps > fps * (1 - FPS_TOLERANCE), \
                    f"FPS too low: {measured_fps:.2f} < {fps * (1 - FPS_TOLERANCE):.2f}"
                assert measured_fps < fps * (1 + FPS_TOLERANCE), \
                    f"FPS too high: {measured_fps:.2f} > {fps * (1 + FPS_TOLERANCE):.2f}"

    return frames


def _config_id(val):
    """Generate test ID from config tuple."""
    w, h, _, fps = val
    return f"{w}x{h}-{fps}"


@pytest.mark.d457
class TestDepthStreaming:
    """Depth stream at various resolutions and FPS."""

    @pytest.mark.parametrize("config", DEPTH_CONFIGS, ids=_config_id)
    def test_depth_stream_fps(self, camera, config):
        width, height, pixfmt, fps = config
        # Verify resolution is supported before testing
        with V4L2Device(camera.depth_path) as dev:
            sizes = dev.enum_framesizes(pixfmt)
            supported = {(s.discrete.width, s.discrete.height)
                         for s in sizes
                         if s.type == ioctls.V4L2_FRMSIZE_TYPE_DISCRETE}
            if (width, height) not in supported:
                pytest.skip(f"{width}x{height} not supported on this device")

            intervals = dev.enum_frameintervals(pixfmt, width, height)
            fps_list = []
            for fi in intervals:
                if fi.type == ioctls.V4L2_FRMIVAL_TYPE_DISCRETE and fi.discrete.numerator > 0:
                    fps_list.append(fi.discrete.denominator / fi.discrete.numerator)
            if fps not in fps_list:
                pytest.skip(f"{fps} FPS not supported at {width}x{height}")

        _stream_and_validate(camera.depth_path, width, height, pixfmt, fps)


@pytest.mark.d457
class TestIRStreaming:
    """IR stream at various resolutions and FPS."""

    @pytest.mark.parametrize("config", IR_CONFIGS, ids=_config_id)
    def test_ir_stream_fps(self, camera, config):
        width, height, pixfmt, fps = config
        with V4L2Device(camera.ir_path) as dev:
            sizes = dev.enum_framesizes(pixfmt)
            supported = {(s.discrete.width, s.discrete.height)
                         for s in sizes
                         if s.type == ioctls.V4L2_FRMSIZE_TYPE_DISCRETE}
            if (width, height) not in supported:
                pytest.skip(f"{width}x{height} not supported for IR")

            intervals = dev.enum_frameintervals(pixfmt, width, height)
            fps_list = []
            for fi in intervals:
                if fi.type == ioctls.V4L2_FRMIVAL_TYPE_DISCRETE and fi.discrete.numerator > 0:
                    fps_list.append(fi.discrete.denominator / fi.discrete.numerator)
            if fps not in fps_list:
                pytest.skip(f"{fps} FPS not supported for IR at {width}x{height}")

        _stream_and_validate(camera.ir_path, width, height, pixfmt, fps)


@pytest.mark.d457
class TestRGBStreaming:
    """RGB stream basic test."""

    def test_rgb_stream_30fps(self, camera):
        with V4L2Device(camera.rgb_path) as dev:
            formats = dev.enum_formats()
            if not formats:
                pytest.skip("No RGB formats available")
            pixfmt = formats[0].pixelformat

            sizes = dev.enum_framesizes(pixfmt)
            if not sizes:
                pytest.skip("No RGB frame sizes available")

            # Pick first available size
            s = sizes[0]
            if s.type != ioctls.V4L2_FRMSIZE_TYPE_DISCRETE:
                pytest.skip("Only discrete frame sizes supported")
            w, h = s.discrete.width, s.discrete.height

            intervals = dev.enum_frameintervals(pixfmt, w, h)
            fps_list = []
            for fi in intervals:
                if fi.type == ioctls.V4L2_FRMIVAL_TYPE_DISCRETE and fi.discrete.numerator > 0:
                    fps_list.append(fi.discrete.denominator / fi.discrete.numerator)

            fps = 30 if 30 in fps_list else fps_list[0] if fps_list else 30

        _stream_and_validate(camera.rgb_path, w, h, pixfmt, int(fps))


CONCURRENT_MIN_DURATION = 5.0  # seconds


def _stream_depth_rgb(camera, width, height, rgb_pixfmt, duration):
    """Stream depth Z16 + RGB at (width, height) for *duration* seconds.

    Returns (depth_frames, rgb_frames) lists of (v4l2_buffer, data).
    """
    depth_dev = V4L2Device(camera.depth_path)
    rgb_dev = V4L2Device(camera.rgb_path)

    depth_dev.open()
    rgb_dev.open()

    try:
        depth_dev.set_format(width, height, ioctls.V4L2_PIX_FMT_Z16)
        depth_dev.set_parm(30)

        rgb_dev.set_format(width, height, rgb_pixfmt)
        rgb_dev.set_parm(30)

        depth_stream = StreamContext(depth_dev, buf_count=4)
        rgb_stream = StreamContext(rgb_dev, buf_count=4)

        depth_stream.__enter__()
        rgb_stream.__enter__()

        try:
            depth_frames = []
            rgb_frames = []
            per_frame_timeout = 2.0
            start = time.monotonic()

            while time.monotonic() - start < duration:
                dbuf, ddata = depth_stream.dequeue(timeout=per_frame_timeout)
                depth_frames.append((dbuf, ddata))
                depth_stream.requeue(dbuf)

                rbuf, rdata = rgb_stream.dequeue(timeout=per_frame_timeout)
                rgb_frames.append((rbuf, rdata))
                rgb_stream.requeue(rbuf)

            return depth_frames, rgb_frames

        finally:
            rgb_stream.__exit__(None, None, None)
            depth_stream.__exit__(None, None, None)
    finally:
        rgb_dev.close()
        depth_dev.close()


@pytest.mark.d457
class TestDepthRGBConcurrent:
    """Concurrent depth + RGB streaming at every common resolution."""

    def test_depth_rgb_concurrent(self, camera, common_depth_rgb_resolutions,
                                  resolution):
        """Stream depth+RGB concurrently at a common resolution for 5+ s."""
        _, rgb_pixfmt = common_depth_rgb_resolutions
        width, height = resolution

        depth_frames, rgb_frames = _stream_depth_rgb(
            camera, width, height, rgb_pixfmt, CONCURRENT_MIN_DURATION,
        )

        assert len(depth_frames) > 0, "No depth frames"
        assert len(rgb_frames) > 0, "No RGB frames"

        # Non-empty data
        nonzero_depth = sum(1 for _, d in depth_frames if len(d) > 0)
        assert nonzero_depth == len(depth_frames), \
            f"{len(depth_frames) - nonzero_depth} empty depth frames"

        nonzero_rgb = sum(1 for _, d in rgb_frames if len(d) > 0)
        assert nonzero_rgb == len(rgb_frames), \
            f"{len(rgb_frames) - nonzero_rgb} empty RGB frames"

        # Depth sequence monotonic
        depth_seqs = [buf.sequence for buf, _ in depth_frames]
        for i in range(1, len(depth_seqs)):
            assert depth_seqs[i] > depth_seqs[i - 1], \
                f"Depth seq not monotonic: {depth_seqs[i-1]} -> {depth_seqs[i]}"

        # RGB sequence monotonic
        rgb_seqs = [buf.sequence for buf, _ in rgb_frames]
        for i in range(1, len(rgb_seqs)):
            assert rgb_seqs[i] > rgb_seqs[i - 1], \
                f"RGB seq not monotonic: {rgb_seqs[i-1]} -> {rgb_seqs[i]}"


def _stream_depth_rgb_ir(camera, width, height, rgb_pixfmt, duration):
    """Stream depth Z16 + RGB + IR GREY at (width, height) for *duration* seconds.

    Returns (depth_frames, rgb_frames, ir_frames) lists of (v4l2_buffer, data).
    """
    depth_dev = V4L2Device(camera.depth_path)
    rgb_dev = V4L2Device(camera.rgb_path)
    ir_dev = V4L2Device(camera.ir_path)

    depth_dev.open()
    rgb_dev.open()
    ir_dev.open()

    try:
        depth_dev.set_format(width, height, ioctls.V4L2_PIX_FMT_Z16)
        depth_dev.set_parm(30)

        rgb_dev.set_format(width, height, rgb_pixfmt)
        rgb_dev.set_parm(30)

        ir_dev.set_format(width, height, ioctls.V4L2_PIX_FMT_GREY)
        ir_dev.set_parm(30)

        depth_stream = StreamContext(depth_dev, buf_count=4)
        rgb_stream = StreamContext(rgb_dev, buf_count=4)
        ir_stream = StreamContext(ir_dev, buf_count=4)

        depth_stream.__enter__()
        rgb_stream.__enter__()
        ir_stream.__enter__()

        try:
            depth_frames = []
            rgb_frames = []
            ir_frames = []
            per_frame_timeout = 2.0
            start = time.monotonic()

            while time.monotonic() - start < duration:
                dbuf, ddata = depth_stream.dequeue(timeout=per_frame_timeout)
                depth_frames.append((dbuf, ddata))
                depth_stream.requeue(dbuf)

                rbuf, rdata = rgb_stream.dequeue(timeout=per_frame_timeout)
                rgb_frames.append((rbuf, rdata))
                rgb_stream.requeue(rbuf)

                ibuf, idata = ir_stream.dequeue(timeout=per_frame_timeout)
                ir_frames.append((ibuf, idata))
                ir_stream.requeue(ibuf)

            return depth_frames, rgb_frames, ir_frames

        finally:
            ir_stream.__exit__(None, None, None)
            rgb_stream.__exit__(None, None, None)
            depth_stream.__exit__(None, None, None)
    finally:
        ir_dev.close()
        rgb_dev.close()
        depth_dev.close()


@pytest.mark.d457
class TestDepthRGBIRConcurrent:
    """Concurrent depth + RGB + IR streaming at every common resolution."""

    def test_depth_rgb_ir_concurrent(self, camera,
                                      common_depth_rgb_ir_resolutions,
                                      tri_resolution):
        """Stream depth+RGB+IR concurrently at a common resolution for 5+ s."""
        _, rgb_pixfmt = common_depth_rgb_ir_resolutions
        width, height = tri_resolution

        depth_frames, rgb_frames, ir_frames = _stream_depth_rgb_ir(
            camera, width, height, rgb_pixfmt, CONCURRENT_MIN_DURATION,
        )

        assert len(depth_frames) > 0, "No depth frames"
        assert len(rgb_frames) > 0, "No RGB frames"
        assert len(ir_frames) > 0, "No IR frames"

        # Non-empty data
        nonzero_depth = sum(1 for _, d in depth_frames if len(d) > 0)
        assert nonzero_depth == len(depth_frames), \
            f"{len(depth_frames) - nonzero_depth} empty depth frames"

        nonzero_rgb = sum(1 for _, d in rgb_frames if len(d) > 0)
        assert nonzero_rgb == len(rgb_frames), \
            f"{len(rgb_frames) - nonzero_rgb} empty RGB frames"

        nonzero_ir = sum(1 for _, d in ir_frames if len(d) > 0)
        assert nonzero_ir == len(ir_frames), \
            f"{len(ir_frames) - nonzero_ir} empty IR frames"

        # Depth sequence monotonic
        depth_seqs = [buf.sequence for buf, _ in depth_frames]
        for i in range(1, len(depth_seqs)):
            assert depth_seqs[i] > depth_seqs[i - 1], \
                f"Depth seq not monotonic: {depth_seqs[i-1]} -> {depth_seqs[i]}"

        # RGB sequence monotonic
        rgb_seqs = [buf.sequence for buf, _ in rgb_frames]
        for i in range(1, len(rgb_seqs)):
            assert rgb_seqs[i] > rgb_seqs[i - 1], \
                f"RGB seq not monotonic: {rgb_seqs[i-1]} -> {rgb_seqs[i]}"

        # IR sequence monotonic
        ir_seqs = [buf.sequence for buf, _ in ir_frames]
        for i in range(1, len(ir_seqs)):
            assert ir_seqs[i] > ir_seqs[i - 1], \
                f"IR seq not monotonic: {ir_seqs[i-1]} -> {ir_seqs[i]}"


@pytest.mark.d457
class TestStreamStartStop:
    """Stream start/stop cycling."""

    def test_start_stop_cycle(self, camera):
        """Start and stop the stream 3 times to verify recovery."""
        for cycle in range(3):
            with V4L2Device(camera.depth_path) as dev:
                dev.set_format(848, 480, ioctls.V4L2_PIX_FMT_Z16)
                dev.set_parm(30)
                with StreamContext(dev) as stream:
                    frames = stream.capture_frames(10, timeout=5.0)
                    assert len(frames) > 0, f"No frames in cycle {cycle}"
