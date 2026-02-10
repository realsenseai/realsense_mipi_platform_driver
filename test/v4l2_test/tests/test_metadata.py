"""Metadata capture, frame counter, CRC32 validation tests."""

import ctypes

import pytest

from ..d4xx import constants as C
from ..d4xx.metadata import (
    STMetaDataExtMipiDepthIR,
    parse_metadata,
    validate_crc32,
)
from ..v4l2 import ioctls
from ..v4l2.device import V4L2Device
from ..v4l2.stream import StreamContext


METADATA_FRAMES = 30


def _capture_depth_with_metadata(camera, width=848, height=480, fps=30):
    """Capture depth frames and corresponding metadata simultaneously.

    Opens both the depth device and metadata device, streams both,
    and returns paired (depth_frames, metadata_frames).
    """
    depth_dev = V4L2Device(camera.depth_path)
    md_dev = V4L2Device(camera.depth_md_path)

    depth_dev.open()
    md_dev.open()

    try:
        # Configure depth
        depth_dev.set_format(width, height, ioctls.V4L2_PIX_FMT_Z16)
        depth_dev.set_parm(fps)

        # Configure metadata
        md_dev.set_meta_format(
            ioctls.V4L2_META_FMT_D4XX,
            ioctls.V4L2_BUF_TYPE_META_CAPTURE,
        )

        timeout = max(5.0, 4.0 * METADATA_FRAMES / fps)

        # Start metadata stream first, then depth (matching test_metadata.c order)
        md_stream = StreamContext(
            md_dev,
            buf_type=ioctls.V4L2_BUF_TYPE_META_CAPTURE,
            buf_count=4,
        )
        depth_stream = StreamContext(depth_dev, buf_count=4)

        md_stream.__enter__()
        depth_stream.__enter__()

        try:
            depth_frames = []
            md_frames = []

            for _ in range(METADATA_FRAMES):
                # Dequeue depth frame
                dbuf, ddata = depth_stream.dequeue(timeout=timeout)
                depth_frames.append((dbuf, ddata))
                depth_stream.requeue(dbuf)

                # Dequeue metadata frame
                try:
                    mbuf, mdata = md_stream.dequeue(timeout=timeout)
                    md_frames.append((mbuf, mdata))
                    md_stream.requeue(mbuf)
                except (TimeoutError, OSError):
                    md_frames.append((None, None))

            return depth_frames, md_frames
        finally:
            depth_stream.__exit__(None, None, None)
            md_stream.__exit__(None, None, None)
    finally:
        depth_dev.close()
        md_dev.close()


@pytest.mark.d457
class TestMetadataCapture:
    """Verify metadata can be captured alongside depth frames."""

    def test_metadata_arrives(self, camera):
        _, md_frames = _capture_depth_with_metadata(camera)
        valid = [(buf, data) for buf, data in md_frames if data is not None]
        assert len(valid) > 0, "No metadata frames captured"

    def test_metadata_parseable(self, camera):
        _, md_frames = _capture_depth_with_metadata(camera)
        parsed_count = 0
        for _, data in md_frames:
            if data is None:
                continue
            md, md_type = parse_metadata(data)
            if md is not None:
                parsed_count += 1
        assert parsed_count > 0, "No metadata frames parseable"


@pytest.mark.d457
class TestMetadataFrameCounter:
    """Verify frame counter increments in metadata."""

    def test_frame_counter_monotonic(self, camera):
        _, md_frames = _capture_depth_with_metadata(camera)

        counters = []
        for _, data in md_frames:
            if data is None:
                continue
            md, md_type = parse_metadata(data)
            if md is None:
                continue
            if md_type == "ExtMipiDepthIR":
                counters.append(md.Frame_counter)
            elif md_type == "DepthYNormalMode":
                counters.append(md.intelCaptureTiming.frameCounter)

        assert len(counters) >= 2, f"Too few metadata frames: {len(counters)}"

        for i in range(1, len(counters)):
            assert counters[i] > counters[i - 1], \
                f"Frame counter not monotonic: {counters[i-1]} -> {counters[i]}"


@pytest.mark.d457
class TestMetadataTimestamp:
    """Verify HW timestamps increment in metadata."""

    def test_hw_timestamp_increments(self, camera):
        _, md_frames = _capture_depth_with_metadata(camera)

        timestamps = []
        for _, data in md_frames:
            if data is None:
                continue
            md, md_type = parse_metadata(data)
            if md is None:
                continue
            if md_type == "ExtMipiDepthIR":
                timestamps.append(md.hwTimestamp)
            elif md_type == "DepthYNormalMode":
                timestamps.append(md.captureStats.hwTimestamp)

        assert len(timestamps) >= 2, f"Too few timestamps: {len(timestamps)}"

        for i in range(1, len(timestamps)):
            assert timestamps[i] >= timestamps[i - 1], \
                f"HW timestamp not monotonic: {timestamps[i-1]} -> {timestamps[i]}"


@pytest.mark.d457
class TestMetadataCRC:
    """Verify CRC32 validation on metadata."""

    def test_crc32_valid(self, camera):
        _, md_frames = _capture_depth_with_metadata(camera)

        checked = 0
        passed = 0
        for _, data in md_frames:
            if data is None:
                continue
            md, md_type = parse_metadata(data)
            if md is None:
                continue
            checked += 1
            if validate_crc32(data, md, md_type):
                passed += 1

        assert checked > 0, "No metadata frames to check CRC"
        # Allow some CRC failures (transient), but majority should pass
        assert passed >= checked * 0.8, \
            f"CRC failures: {checked - passed}/{checked}"
