"""Metadata capture, frame counter, CRC32 validation tests."""

import ctypes

import pytest

from ..d4xx import constants as C
from ..d4xx.metadata import (
    MD_COMPRESSED_BLOCK_BYTES,
    MD_COMPRESSED_VERSION,
    MD_LINE_BYTES,
    STMetaDataExtMipiDepthIR,
    parse_compressed_frame,
    parse_metadata,
    validate_crc32,
)
from ..v4l2 import ioctls
from ..v4l2.device import V4L2Device
from ..v4l2.stream import StreamContext


METADATA_FRAMES = 30
COMPRESSED_FPS = 30


def _capture_depth_with_metadata(camera, width=848, height=480, fps=30):
    """Capture depth frames and corresponding metadata simultaneously."""
    return _capture_with_metadata(camera.depth_path, camera.depth_md_path,
                                  ioctls.V4L2_PIX_FMT_Z16, width, height, fps)


def _read_to_row_end(stream, buf, row_bytes):
    """Mapped buffer from its start to the end of the row holding bytesused."""
    _, mm = stream.buffers[buf.index]
    end = -(-buf.bytesused // row_bytes) * row_bytes
    return mm[:min(end, len(mm))]


def _capture_with_metadata(video_path, md_path, pixfmt, width, height, fps,
                           read_last_row=False):
    """Capture video frames and corresponding metadata simultaneously.

    Opens both the video device and metadata device, streams both,
    and returns paired (video_frames, metadata_frames). With read_last_row,
    each frame is read up to the end of its last carrier row, not bytesused.
    """
    video_dev = V4L2Device(video_path)
    md_dev = V4L2Device(md_path)

    video_dev.open()
    md_dev.open()

    try:
        # Configure the video stream
        fmt = video_dev.set_format(width, height, pixfmt)
        assert fmt.fmt.pix.pixelformat == pixfmt, "S_FMT changed the pixel format"
        # Compressed: V4L2 wants bytesperline 0, the rows are the carrier's.
        assert not read_last_row or fmt.fmt.pix.bytesperline == 0, \
            "compressed format reports a bytesperline"
        row_bytes = C.COMPRESSED_CARRIER_ROW_BYTES if read_last_row else 0
        video_dev.set_parm(fps)

        # Configure metadata — try D4XX format; tegra-embedded has a fixed
        # format and rejects S_FMT/G_FMT, so just skip format configuration
        try:
            md_dev.set_meta_format(
                ioctls.V4L2_META_FMT_D4XX,
                ioctls.V4L2_BUF_TYPE_META_CAPTURE,
            )
        except OSError:
            pass  # tegra-embedded: format is fixed, proceed without setting

        timeout = max(5.0, 4.0 * METADATA_FRAMES / fps)

        # Start metadata stream first, then video (matching test_metadata.c order)
        md_stream = StreamContext(
            md_dev,
            buf_type=ioctls.V4L2_BUF_TYPE_META_CAPTURE,
            buf_count=4,
        )
        video_stream = StreamContext(video_dev, buf_count=4)

        md_stream.__enter__()
        video_stream.__enter__()

        try:
            video_frames = []
            md_frames = []

            for _ in range(METADATA_FRAMES):
                # Dequeue video frame
                vbuf, vdata = video_stream.dequeue(timeout=timeout)
                if row_bytes:
                    vdata = _read_to_row_end(video_stream, vbuf, row_bytes)
                video_frames.append((vbuf, vdata))
                video_stream.requeue(vbuf)

                # Dequeue metadata frame
                try:
                    mbuf, mdata = md_stream.dequeue(timeout=timeout)
                    md_frames.append((mbuf, mdata))
                    md_stream.requeue(mbuf)
                except (TimeoutError, OSError):
                    md_frames.append((None, None))

            return video_frames, md_frames
        finally:
            video_stream.__exit__(None, None, None)
            md_stream.__exit__(None, None, None)
    finally:
        video_dev.close()
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


@pytest.mark.d585
class TestCompressedByteCount:
    """D58x compressed colour: bytesused is the byte count in the metadata
    block, and the rest of the last carrier row reads zero."""

    @pytest.mark.parametrize("pixfmt", [ioctls.V4L2_PIX_FMT_H264, ioctls.V4L2_PIX_FMT_MJPEG],
                             ids=lambda f: ioctls.FOURCC_TO_NAME[f])
    def test_bytesused_matches_block(self, d58x_encoder_camera, pixfmt):
        name = ioctls.FOURCC_TO_NAME[pixfmt]
        with V4L2Device(d58x_encoder_camera.rgb_path) as dev:
            sizes = sorted(((fs.discrete.width, fs.discrete.height)
                            for fs in dev.enum_framesizes(pixfmt)
                            if fs.type == ioctls.V4L2_FRMSIZE_TYPE_DISCRETE),
                           key=lambda wh: wh[0] * wh[1])
        assert sizes, f"RGB device lists no {name} frame sizes"
        width, height = sizes[0]
        frames, md_frames = _capture_with_metadata(
            d58x_encoder_camera.rgb_path, d58x_encoder_camera.rgb_md_path, pixfmt,
            width, height, COMPRESSED_FPS, read_last_row=True)

        md_by_seq = {buf.sequence: data for buf, data in md_frames
                     if data is not None}
        if md_by_seq and max(map(len, md_by_seq.values())) < MD_LINE_BYTES:
            pytest.skip("Metadata node publishes a short line; block not visible")

        checked = 0
        for buf, data in frames:
            if buf.flags & ioctls.V4L2_BUF_FLAG_ERROR or buf.sequence not in md_by_seq:
                continue
            block = parse_compressed_frame(md_by_seq[buf.sequence])
            assert block is not None, f"No byte-count block, frame {buf.sequence}"
            assert block.version == MD_COMPRESSED_VERSION
            assert block.size >= MD_COMPRESSED_BLOCK_BYTES
            assert buf.bytesused == block.encoded_bytes, \
                f"bytesused {buf.bytesused} != block {block.encoded_bytes}"
            tail = data[buf.bytesused:]
            nonzero = next((i for i, b in enumerate(tail) if b), None)
            assert nonzero is None, \
                f"Last-row byte {buf.bytesused + nonzero} non-zero, frame {buf.sequence}"
            checked += 1
        assert checked > 0, f"No good {name} frame paired with its metadata"
