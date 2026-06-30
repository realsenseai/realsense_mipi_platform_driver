#!/usr/bin/env python3
"""Pure-Python V4L2 MMAP capture for a D4XX CSI color/NV12 (16-bit) node.

Sibling of depth_capture.py for the VC1 color stream produced by, e.g.:
    csitest -f nv12 -o yuyv -W 1280 -H 720 --fps 30 --vc 1 -n 60
The camera's source pattern is NV12 but it is emitted as YUYV on the CSI bus,
so the V4L2 color node carries YUYV 4:2:2 (16-bit/pixel, 2560 B/line at 1280
wide). This tool requests the format, reports what the driver negotiates, and
decodes the YUYV frame to a viewable RGB image (BT.601) using the negotiated
geometry.

Like depth_capture.py: drives V4L2 directly via ctypes+fcntl.ioctl (no compiled
C, no V4L2 bindings), dumps a frame even when the error flag is set, and waits
for a non-empty frame. Capture-only needs no deps; --png/--display need numpy + Pillow.

The shared V4L2 capture core lives in v4l2_capture.py (same directory); this
script only adds the NV12 pixel format and the YUYV->RGB render path.

Examples
--------
  ./nv12_capture.py --dev /dev/video-rs-color-0 -W 1280 -H 720 --display
  ./nv12_capture.py --raw c.raw --png c.png
  # Run WITHOUT sudo so --display works (the video node is group-accessible).
"""
import argparse, sys

from v4l2_capture import capture, show, fourcc

PIX_FMT_NV12 = fourcc(b"NV12")


def render(cols, rows, data=None, raw=None, png=None, display=False):
    """Decode the YUYV (4:2:2) frame to a viewable RGB image.

    `cols` is the pixel width (= bytesperline/2), `rows` the height. YUYV packs
    each 4 bytes as [Y0 U Y1 V] -> 2 pixels; we lift the two luma samples and
    upsample the shared chroma, then YCbCr(BT.601)->RGB."""
    import numpy as np
    from PIL import Image
    b = np.frombuffer(data, dtype=np.uint8) if data is not None else np.fromfile(raw, dtype=np.uint8)
    bpl = cols * 2                                   # YUYV: 2 bytes per pixel
    b = b[:rows * bpl].reshape(rows, bpl)
    Y = b[:, 0::2].astype(np.float32)                # (rows, cols)  both luma samples
    U = np.repeat(b[:, 1::4], 2, axis=1).astype(np.float32)   # chroma, upsampled x2
    V = np.repeat(b[:, 3::4], 2, axis=1).astype(np.float32)
    w = min(Y.shape[1], U.shape[1], V.shape[1])
    Y, U, V = Y[:, :w], U[:, :w] - 128, V[:, :w] - 128
    R = Y + 1.402 * V
    G = Y - 0.344136 * U - 0.714136 * V
    B = Y + 1.772 * U
    rgb = np.clip(np.stack([R, G, B], axis=-1), 0, 255).astype(np.uint8)
    img = Image.fromarray(rgb, "RGB")
    drows = [r for r in range(rows) if b[r].any()]
    print("yuyv->rgb %dx%d data_rows=%d (%s..%s)%s"
          % (w, rows, len(drows), drows[0] if drows else "-", drows[-1] if drows else "-",
             (" -> " + png) if png else ""))
    if png:
        img.save(png)
    if display:
        show(rgb, img, w, rows, "nv12/color %dx%d" % (w, rows))


def main():
    ap = argparse.ArgumentParser(description="Pure-Python V4L2 capture + 16-bit render (color/NV12 node)")
    ap.add_argument("--dev", default="/dev/video-rs-color-0")
    ap.add_argument("-W", "--width", type=int, default=1280)
    ap.add_argument("-H", "--height", type=int, default=720)
    ap.add_argument("--frames", type=int, default=90, help="max frames to dequeue while waiting for data (~3s @30fps)")
    ap.add_argument("--save-index", type=int, default=2, help="don't save before this frame (skips initial sync frames)")
    ap.add_argument("--raw", help="save the captured frame to this raw file (optional)")
    ap.add_argument("--png", help="render the captured frame to this PNG (optional)")
    ap.add_argument("--display", action="store_true", help="open a window showing the rendered image")
    ap.add_argument("--render-only", action="store_true", help="skip capture, just render --raw")
    a = ap.parse_args()
    if a.render_only:
        if not a.raw or not (a.png or a.display):
            sys.exit("--render-only needs --raw (input) and at least one of --png / --display")
        # YUYV geometry: pixel width == --width, bytesperline == width*2
        render(a.width, a.height, raw=a.raw, png=a.png, display=a.display)
        return
    W, H, bpl, size, frame = capture(a.dev, a.width, a.height, PIX_FMT_NV12, a.frames, a.save_index, a.raw)
    # render as 16-bit using the negotiated geometry
    cols, rows = bpl // 2, (size // bpl if bpl else H)
    if a.png or a.display:
        render(cols, rows, data=frame, png=a.png, display=a.display)
    if not a.raw and not a.png and not a.display:
        print("note: none of --raw / --png / --display given; capture ran but output nothing")


if __name__ == "__main__":
    main()
