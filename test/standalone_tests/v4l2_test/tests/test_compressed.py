"""D58x compressed colour over GMSL: every buffer arrives cleared.

The capture hardware writes only the rows a frame needs and never the rest of
the buffer, so the driver clears each compressed buffer before its capture.
These tests check what userspace receives: a well-formed frame at the start of
the buffer, only zeros after the frame's end across the whole buffer, and a
length that is the frame's end for MMAP (cut at dequeue) and the buffer size
for USERPTR (no kernel mapping to cut it with).
"""

import pytest

from ..d4xx import constants as C
from ..v4l2 import ioctls
from ..v4l2.device import V4L2Device
from ..v4l2.stream import StreamContext

COMPRESSED_FPS = 30
# Enough frames for every buffer to hold a key frame, then smaller frames.
COMPRESSED_FRAMES = 150
BUFFER_COUNT = 4

JPEG_SOI = b"\xff\xd8"
JPEG_EOI = b"\xff\xd9"
JPEG_SOS = 0xDA
# Markers with no length field: TEM and RST0..7.
JPEG_STANDALONE = {0x01} | set(range(0xD0, 0xD8))
H264_START_CODES = (b"\x00\x00\x00\x01", b"\x00\x00\x01")
# Emulation prevention keeps three zero bytes out of a NAL unit, and a start
# code holds at most three, so four zeros only follow the last NAL unit. A
# leftover starting within three bytes of the end would read as frame data;
# JPEG, which ends at its EOI marker, has no such gap.
H264_END_RUN = b"\x00" * 4


def _jpeg_end(data):
    """Offset just past the EOI marker, walking segments and entropy data."""
    if not data.startswith(JPEG_SOI):
        return None
    off = len(JPEG_SOI)
    while off + 4 <= len(data):
        if data[off] != 0xFF:
            return None
        marker = data[off + 1]
        if marker == JPEG_SOS:
            off += 2 + int.from_bytes(data[off + 2:off + 4], "big")
            break
        off += 2 + int.from_bytes(data[off + 2:off + 4], "big")
    # Entropy data: 0xFF is followed by a stuffed 0x00 or an RST marker.
    while True:
        off = data.find(b"\xff", off)
        if off < 0 or off + 1 >= len(data):
            return None
        nxt = data[off + 1]
        if nxt == JPEG_EOI[1]:
            return off + 2
        if nxt != 0x00 and nxt not in JPEG_STANDALONE:
            return None
        off += 2


def _h264_end(data):
    """Offset where the first run of four zero bytes starts; None without one,
    since a frame reaching the buffer end leaves no tail to check."""
    if not data.startswith(H264_START_CODES):
        return None
    end = data.find(H264_END_RUN)
    return None if end < 0 else end


FRAME_END = {
    ioctls.V4L2_PIX_FMT_H264: _h264_end,
    ioctls.V4L2_PIX_FMT_MJPEG: _jpeg_end,
}


def _sizes(path, pixfmt):
    with V4L2Device(path) as dev:
        return sorted(((fs.discrete.width, fs.discrete.height)
                       for fs in dev.enum_framesizes(pixfmt)
                       if fs.type == ioctls.V4L2_FRMSIZE_TYPE_DISCRETE),
                      key=lambda wh: wh[0] * wh[1])


@pytest.mark.d585
class TestCompressedZeroTail:
    """After the frame's end every byte of the buffer is zero; the length is
    that end for MMAP and the buffer size for USERPTR."""

    @pytest.mark.parametrize("memory", [ioctls.V4L2_MEMORY_MMAP,
                                        ioctls.V4L2_MEMORY_USERPTR],
                             ids=["mmap", "userptr"])
    @pytest.mark.parametrize("pick", ["smallest", "largest"])
    @pytest.mark.parametrize("pixfmt", [ioctls.V4L2_PIX_FMT_H264,
                                        ioctls.V4L2_PIX_FMT_MJPEG],
                             ids=lambda f: ioctls.FOURCC_TO_NAME[f])
    def test_only_zeros_after_frame(self, d58x_encoder_camera, pixfmt, pick,
                                    memory):
        name = ioctls.FOURCC_TO_NAME[pixfmt]
        sizes = _sizes(d58x_encoder_camera.rgb_path, pixfmt)
        assert sizes, f"RGB device lists no {name} frame sizes"
        width, height = sizes[0] if pick == "smallest" else sizes[-1]

        with V4L2Device(d58x_encoder_camera.rgb_path) as dev:
            fmt = dev.set_format(width, height, pixfmt)
            assert fmt.fmt.pix.pixelformat == pixfmt, \
                "S_FMT changed the pixel format"
            assert fmt.fmt.pix.bytesperline == 0, \
                "compressed format reports a bytesperline"
            sizeimage = fmt.fmt.pix.sizeimage
            assert sizeimage % C.COMPRESSED_CARRIER_ROW_BYTES == 0, \
                f"sizeimage {sizeimage} is not whole carrier rows"
            dev.set_parm(COMPRESSED_FPS)

            frame_end = FRAME_END[pixfmt]
            last_end = {}
            shrank = 0
            with StreamContext(dev, buf_count=BUFFER_COUNT, memory=memory,
                               buf_size=sizeimage) as stream:
                for _ in range(COMPRESSED_FRAMES):
                    buf, _ = stream.dequeue()
                    try:
                        seq = buf.sequence
                        assert not buf.flags & ioctls.V4L2_BUF_FLAG_ERROR, \
                            f"{name} frame {seq} errored"
                        # The whole buffer, not just bytesused: a broken clear
                        # leaves leftovers past a cut length too.
                        _, mm = stream.buffers[buf.index]
                        mm.seek(0)
                        data = mm.read(sizeimage)
                        end = frame_end(data)
                        assert end is not None, \
                            f"{name} frame {seq} is not a well-formed frame"
                        tail = data[end:]
                        assert tail.count(0) == len(tail), \
                            f"{name} frame {seq}: non-zero byte after its end {end}"
                        expected = (end if memory == ioctls.V4L2_MEMORY_MMAP
                                    else sizeimage)
                        assert buf.bytesused == expected, \
                            f"{name} frame {seq}: bytesused {buf.bytesused} != {expected}"
                        if end < last_end.get(buf.index, 0):
                            shrank += 1
                        last_end[buf.index] = end
                    finally:
                        stream.requeue(buf)

        # Precondition: a buffer held a larger frame before a smaller one,
        # the case where leftovers would show.
        assert shrank > 0, f"no {name} frame followed a larger one in its buffer"
