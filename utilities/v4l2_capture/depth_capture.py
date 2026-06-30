#!/usr/bin/env python3
"""Pure-Python V4L2 MMAP capture for a D4XX CSI depth (Z16) video node.

Drives the full V4L2 MMAP pipeline directly via ctypes + fcntl.ioctl (no
compiled C, no external V4L2 bindings). Dumps a captured frame to a raw file
*even when the V4L2 error flag is set* (v4l2-ctl --stream-to silently drops
error-flagged buffers; this does not), and can render the raw Z16 frame to a
viewable PNG. Capture-only needs no deps; --png/--display need numpy + Pillow.
--raw, --png and --display are all optional and independent.

The shared V4L2 capture core lives in v4l2_capture.py (same directory); this
script only adds the Z16 pixel format and the depth render path.

Examples
--------
  ./depth_capture.py --dev /dev/video-rs-depth-0 -W 1280 -H 720 \
                     --raw frame.raw --png frame.png

  # capture and pop up a window (needs a display; over SSH use 'ssh -X').
  # Run WITHOUT sudo so --display works (the video node is group-accessible).
  ./depth_capture.py -W 1280 -H 720 --display

  # just re-render an existing raw (no capture):
  ./depth_capture.py --render-only --raw frame.raw -W 1280 -H 720 --png frame.png
"""
import argparse, sys

from v4l2_capture import capture, show, fourcc

PIX_FMT_Z16 = fourcc(b"Z16 ")


def render(W, H, pct, data=None, raw=None, png=None, display=False):
    """Render a Z16 frame to a grayscale image. Source is either in-memory
    `data` (bytes) or a `raw` file path. Optionally save to `png` and/or pop
    up a viewer window (`display`)."""
    import numpy as np
    from PIL import Image
    if data is not None:
        d = np.frombuffer(data, dtype=np.uint16)[:W * H].reshape(H, W)
    else:
        d = np.fromfile(raw, dtype=np.uint16)[:W * H].reshape(H, W)
    nz = d[d > 0]
    hi = int(np.percentile(nz, pct)) if nz.size else 1
    g = np.clip(d.astype(np.float32) / max(1, hi) * 255, 0, 255).astype(np.uint8)
    img = Image.fromarray(g, "L")
    rows = [r for r in range(H) if d[r].any()]
    print("z16 %dx%d min=%d max=%d mean=%.0f data_rows=%d (%s..%s) scaled@p%g=%d%s"
          % (W, H, d.min(), d.max(), d.mean(), len(rows),
             rows[0] if rows else "-", rows[-1] if rows else "-", pct, hi,
             (" -> " + png) if png else ""))
    if png:
        img.save(png)
    if display:
        show(g, img, W, H, "depth %dx%d" % (W, H))


def main():
    ap = argparse.ArgumentParser(description="Pure-Python V4L2 capture + Z16 render")
    ap.add_argument("--dev", default="/dev/video-rs-depth-0")
    ap.add_argument("-W", "--width", type=int, default=1280)
    ap.add_argument("-H", "--height", type=int, default=720)
    ap.add_argument("--frames", type=int, default=90, help="max frames to dequeue while waiting for data (~3s @30fps)")
    ap.add_argument("--save-index", type=int, default=2, help="don't save before this frame (skips initial sync frames)")
    ap.add_argument("--raw", help="save the captured frame to this raw file (optional)")
    ap.add_argument("--png", help="render the captured frame to this PNG (optional)")
    ap.add_argument("--pct", type=float, default=95.0, help="contrast clip percentile (nonzero px)")
    ap.add_argument("--display", action="store_true", help="open a window showing the rendered image")
    ap.add_argument("--render-only", action="store_true", help="skip capture, just render --raw")
    a = ap.parse_args()
    W, H = a.width, a.height
    if a.render_only:
        if not a.raw or not (a.png or a.display):
            sys.exit("--render-only needs --raw (input) and at least one of --png / --display")
        render(W, H, a.pct, raw=a.raw, png=a.png, display=a.display)
        return
    W, H, bpl, size, frame = capture(a.dev, W, H, PIX_FMT_Z16, a.frames, a.save_index, a.raw)
    if a.png or a.display:
        render(W, H, a.pct, data=frame, png=a.png, display=a.display)
    if not a.raw and not a.png and not a.display:
        print("note: none of --raw / --png / --display given; capture ran but output nothing")


if __name__ == "__main__":
    main()
