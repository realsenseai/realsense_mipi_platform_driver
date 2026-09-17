#!/usr/bin/env python3
"""Pure-Python V4L2 MMAP capture for a D4XX CSI IR video node.

Sibling of depth_capture.py / color_capture.py for the IR stream
(/dev/video-rs-ir-0). Three modes:
  --mode y8   GREY  (8 bpp, single 8-bit greyscale image)
  --mode y8i  'Y8I' (16 bpp, two 8-bit IR images byte-interleaved L,R,L,R...)
  --mode y16i 'Y16I' (32 bpp, two 16-bit IR images pixel-interleaved
              L0,R0,L1,R1...; calibration mode, RSDSO-21787). Slots are
              LITTLE-endian on the host (the per-format VI datatype override,
              nvidia-oot 0013, captures the wire bytes as transmitted) and
              only the low 12 bits carry Y12I data.

Y8 renders as plain greyscale; Y8I/Y16I are deinterleaved into the left and
right images and shown side-by-side. Drives V4L2 directly via ctypes+fcntl.ioctl
(no compiled C, no V4L2 bindings); dumps a frame even when the error flag is
set; waits for a non-empty frame. Capture-only needs no deps; --png/--display
need numpy + Pillow.

The shared V4L2 capture core lives in v4l2_capture.py (same directory); this
script only adds the Y8/Y8I pixel formats and the IR render path.

Examples
--------
  ./ir_capture.py --mode y8  -W 1280 -H 720 --display
  ./ir_capture.py --mode y8i -W 1280 -H 720 --raw ir.raw --png ir.png
  ./ir_capture.py --mode y16i -W 1600 -H 1300 --png calib.png   # D58x calib
  # Run WITHOUT sudo so --display works (the video node is group-accessible).
"""
import argparse, sys

from v4l2_capture import capture, show, fourcc

PIX_FMT_GREY = fourcc(b"GREY")   # Y8  : 8 bpp greyscale
PIX_FMT_Y8I  = fourcc(b"Y8I ")   # Y8I : 16 bpp, two 8-bit images interleaved
PIX_FMT_Y16I = fourcc(b"Y16I")   # Y16I: 32 bpp, two 16-bit images interleaved
MODES = {"y8": PIX_FMT_GREY, "y8i": PIX_FMT_Y8I, "y16i": PIX_FMT_Y16I}
BPP = {"y8": 1, "y8i": 2, "y16i": 4}         # bytes per V4L2 pixel


def render(mode, W, H, bpl, data=None, raw=None, png=None, display=False, pct=None):
    """Render an IR frame. y8 -> greyscale (HxW). y8i/y16i -> deinterleave the
    two images (even/odd pixels) and show them side-by-side (left | right).
    IR scenes are often dark; pass --pct (e.g. 99.5) to contrast-stretch by
    clipping at that percentile of nonzero pixels. Default renders 8-bit raw;
    y16i without --pct is scaled by its max value to fit 8 bits."""
    import numpy as np
    from PIL import Image
    b = np.frombuffer(data, dtype=np.uint8) if data is not None else np.fromfile(raw, dtype=np.uint8)
    rows = (len(b) // bpl) if bpl else H
    b = b[:rows * bpl].reshape(rows, bpl)
    if mode == "y16i":
        # LITTLE-endian: the per-format VI datatype override (nvidia-oot
        # 0013) captures the RAW16 payload byte-for-byte as the camera
        # transmits it (LSB first), same as the RGB GR16 calib surface.
        # (Pre-0013 drivers byte-swapped via the stock RAW16 rule -> '>u2'.)
        p = b.view("<u2")                           # rows x (bpl/2) LE 16-bit pixels
        left, right = p[:, 0::2], p[:, 1::2]        # two HxW 16-bit images
        g = np.concatenate([left, right], axis=1)   # side-by-side: Hx(2W)
        desc = "y16i %dx%d 16bpp (left|right deinterleaved, each %dx%d)" % (g.shape[1], rows, left.shape[1], rows)
    elif mode == "y8i":
        left, right = b[:, 0::2], b[:, 1::2]        # two HxW images
        g = np.concatenate([left, right], axis=1)   # side-by-side: Hx(2W)
        desc = "y8i %dx%d (left|right deinterleaved, each %dx%d)" % (g.shape[1], rows, left.shape[1], rows)
    else:
        g = b                                        # GREY: bpl == width
        desc = "y8 %dx%d" % (g.shape[1], rows)
    lo, hi_raw, mean = int(g.min()), int(g.max()), float(g.mean())
    scaled = ""
    if pct is not None:
        nz = g[g > 0]
        hi = int(np.percentile(nz, pct)) if nz.size else 1
        g = np.clip(g.astype(np.float32) / max(1, hi) * 255, 0, 255).astype(np.uint8)
        scaled = " scaled@p%g=%d" % (pct, hi)
    elif g.dtype == np.uint16:
        hi = max(1, int(g.max()))
        g = (g.astype(np.float32) / hi * 255).astype(np.uint8)
        scaled = " scaled@max=%d" % hi
    img = Image.fromarray(g, "L")
    drows = [r for r in range(rows) if b[r].any()]
    print("%s min=%d max=%d mean=%.0f data_rows=%d (%s..%s)%s%s"
          % (desc, lo, hi_raw, mean, len(drows),
             drows[0] if drows else "-", drows[-1] if drows else "-",
             scaled, (" -> " + png) if png else ""))
    if png:
        img.save(png)
    if display:
        show(g, img, g.shape[1], rows, "ir %dx%d" % (g.shape[1], rows))


def main():
    ap = argparse.ArgumentParser(description="Pure-Python V4L2 capture + render (IR node: Y8 / Y8I / Y16I)")
    ap.add_argument("--dev", default="/dev/video-rs-ir-0")
    ap.add_argument("--mode", choices=("y8", "y8i", "y16i"), default="y8",
                    help="y8=GREY 8bpp, y8i=Y8I 16bpp interleaved, y16i=Y16I 32bpp interleaved calib (default y8)")
    ap.add_argument("-W", "--width", type=int, default=1280)
    ap.add_argument("-H", "--height", type=int, default=720)
    ap.add_argument("--frames", type=int, default=90, help="max frames to dequeue while waiting for data (~3s @30fps)")
    ap.add_argument("--save-index", type=int, default=2, help="don't save before this frame (skips initial sync frames)")
    ap.add_argument("--raw", help="save the captured frame to this raw file (optional)")
    ap.add_argument("--png", help="render the captured frame to this PNG (optional)")
    ap.add_argument("--display", action="store_true", help="open a window showing the rendered image")
    ap.add_argument("--pct", type=float, default=None,
                    help="contrast-stretch: clip at this percentile of nonzero px (e.g. 99.5 for dark IR)")
    ap.add_argument("--render-only", action="store_true", help="skip capture, just render --raw")
    a = ap.parse_args()
    if a.render_only:
        if not a.raw or not (a.png or a.display):
            sys.exit("--render-only needs --raw (input) and at least one of --png / --display")
        bpl = a.width * BPP[a.mode]
        render(a.mode, a.width, a.height, bpl, raw=a.raw, png=a.png, display=a.display, pct=a.pct)
        return
    W, H, bpl, size, frame = capture(a.dev, a.width, a.height, MODES[a.mode], a.frames, a.save_index, a.raw)
    if a.png or a.display:
        render(a.mode, W, H, bpl, data=frame, png=a.png, display=a.display, pct=a.pct)
    if not a.raw and not a.png and not a.display:
        print("note: none of --raw / --png / --display given; capture ran but output nothing")


if __name__ == "__main__":
    main()
