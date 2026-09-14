#!/usr/bin/env python3
"""Pure-Python V4L2 MMAP capture for a D4XX CSI color video node.

Sibling of depth_capture.py / ir_capture.py for the VC1 color stream. Three modes:
  --mode nv12  'NV12' (16 bpp on the wire) produced by, e.g.:
                   csitest -f nv12 -o yuyv -W 1280 -H 720 --fps 30 --vc 1 -n 60
               The camera's source pattern is NV12 but it is emitted as YUYV on
               the CSI bus, so the node carries YUYV 4:2:2 (16 bpp, 2560 B/line
               at 1280 wide) and is decoded as YUYV->RGB (BT.601). NOTE the
               D58x *product* color node does not advertise NV12 - use --mode
               yuyv there.
  --mode yuyv  'YUYV' 4:2:2 as advertised by the product color node; same
               decode path as nv12.
  --mode rw16  'RW16' (16 bpp) RGB *calibration* surface, RSDSO-21787. HKR ISYS
               emits unpacked GRBG10: one 10-bit Bayer sample per 16-bit
               container, carried over CSI-2 RAW16 (DT 0x2E, WC = 2*W). Native
               geometry is 1600x1300 @15/25. NOTE the host buffer is BIG-endian
               (the Tegra RAW16 path byte-swaps 16-bit samples) - see
               _render_rw16.

RW16 is demosaiced by 2x2 GRBG binning, so the rendered PNG is (W/2)x(H/2) RGB;
--raw always holds the untouched full-resolution 16-bit Bayer for calibration
consumers. Drives V4L2 directly via ctypes+fcntl.ioctl (no compiled C, no V4L2
bindings); dumps a frame even when the error flag is set; waits for a non-empty
frame. Capture-only needs no deps; --png/--display need numpy + Pillow.

The shared V4L2 capture core lives in v4l2_capture.py (same directory); this
script only adds the color pixel formats and their render paths.

Examples
--------
  ./color_capture.py --mode nv12 --dev /dev/video-rs-color-0 -W 1280 -H 720 --display
  ./color_capture.py --mode yuyv -W 1280 -H 720 --png c.png       # D58x product color

  ./color_capture.py --mode rw16 -W 1600 -H 1300 --png calib.png   # D58x RGB calib
  ./color_capture.py --mode rw16 -W 1600 -H 1300 --raw calib.raw --pct 99.5
  # Run WITHOUT sudo so --display works (the video node is group-accessible).
"""
import argparse, sys

from v4l2_capture import capture, show, fourcc

PIX_FMT_NV12 = fourcc(b"NV12")   # csitest color node: 16 bpp, YUYV bytes on the wire
PIX_FMT_YUYV = fourcc(b"YUYV")   # product color node: YUYV 4:2:2, 16 bpp
PIX_FMT_RW16 = fourcc(b"RW16")   # RGB calib: 16 bpp, GRBG10 in BE 16-bit words
MODES = {"nv12": PIX_FMT_NV12, "yuyv": PIX_FMT_YUYV, "rw16": PIX_FMT_RW16}
BPP = {"nv12": 2, "yuyv": 2, "rw16": 2}          # bytes per V4L2 pixel


def _render_yuyv(rows, b, png):
    """Decode the YUYV (4:2:2) frame to a viewable RGB image.

    YUYV packs each 4 bytes as [Y0 U Y1 V] -> 2 pixels; we lift the two luma
    samples and upsample the shared chroma, then YCbCr(BT.601)->RGB."""
    import numpy as np
    from PIL import Image
    Y = b[:, 0::2].astype(np.float32)                         # (rows, cols) both luma samples
    U = np.repeat(b[:, 1::4], 2, axis=1).astype(np.float32)   # chroma, upsampled x2
    V = np.repeat(b[:, 3::4], 2, axis=1).astype(np.float32)
    w = min(Y.shape[1], U.shape[1], V.shape[1])
    Y, U, V = Y[:, :w], U[:, :w] - 128, V[:, :w] - 128
    R = Y + 1.402 * V
    G = Y - 0.344136 * U - 0.714136 * V
    B = Y + 1.772 * U
    rgb = np.clip(np.stack([R, G, B], axis=-1), 0, 255).astype(np.uint8)
    drows = [r for r in range(rows) if b[r].any()]
    print("yuyv->rgb %dx%d data_rows=%d (%s..%s)%s"
          % (w, rows, len(drows), drows[0] if drows else "-", drows[-1] if drows else "-",
             (" -> " + png) if png else ""))
    return rgb, Image.fromarray(rgb, "RGB"), w, rows


def _render_rw16(rows, b, png, pct):
    """Demosaic the RW16 (GRBG10-in-16-bit) frame to a viewable RGB image.

    Each 16-bit word holds one 10-bit Bayer sample, stored MSB-FIRST: the
    Tegra VI T_R16 store byte-swaps 16-bit samples arriving on the CSI-2 RAW16
    (DT 0x2E) carrier, so the buffer is big-endian and must be read as '>u2'.
    Measured on fw-orin-5: '<u2' yields 98% of words above 0x3FF and a G-plane
    mean |horizontal delta| of 3554 (noise); '>u2' yields 0% above 0x3FF and a
    delta of 8.8 (a real image). The shipped Y16I IR calib stream behaves the
    same way. The GRBG 2x2 block is binned into a single RGB pixel:
        row0: G0 R      R = block[0,1]
        row1: B  G1     B = block[1,0]
                        G = (G0 + G1) / 2
    so the rendered image is (cols/2)x(rows/2). 10-bit samples are scaled to
    8 bits by >>2 unless --pct asks for a percentile contrast stretch."""
    import numpy as np
    from PIL import Image
    p = b.view(">u2") & 0x03FF                    # rows x cols, 10-bit BE samples
    h, w = (rows // 2) * 2, (p.shape[1] // 2) * 2
    if h < 2 or w < 2:
        sys.exit("rw16: frame too small to demosaic (%dx%d)" % (p.shape[1], rows))
    q = p[:h, :w]
    G0, R = q[0::2, 0::2], q[0::2, 1::2]
    B, G1 = q[1::2, 0::2], q[1::2, 1::2]
    G = ((G0.astype(np.uint32) + G1) // 2).astype(np.uint16)
    raw10 = np.stack([R, G, B], axis=-1)          # (h/2, w/2, 3) 10-bit
    lo, hi_raw, mean = int(p.min()), int(p.max()), float(p.mean())
    if pct is not None:
        nz = raw10[raw10 > 0]
        hi = int(np.percentile(nz, pct)) if nz.size else 1
        rgb = np.clip(raw10.astype(np.float32) / max(1, hi) * 255, 0, 255).astype(np.uint8)
        scaled = " scaled@p%g=%d" % (pct, hi)
    else:
        rgb = (raw10 >> 2).astype(np.uint8)       # 10-bit -> 8-bit
        scaled = " scaled@>>2"
    drows = [r for r in range(rows) if b[r].any()]
    print("rw16 grbg10 %dx%d -> rgb %dx%d (2x2 binned) min=%d max=%d mean=%.0f "
          "data_rows=%d (%s..%s)%s%s"
          % (p.shape[1], rows, w // 2, h // 2, lo, hi_raw, mean, len(drows),
             drows[0] if drows else "-", drows[-1] if drows else "-",
             scaled, (" -> " + png) if png else ""))
    return rgb, Image.fromarray(rgb, "RGB"), w // 2, h // 2


def render(mode, rows, bpl, data=None, raw=None, png=None, display=False, pct=None):
    """Render a color frame according to `mode` (see the module docstring)."""
    import numpy as np
    b = np.frombuffer(data, dtype=np.uint8) if data is not None else np.fromfile(raw, dtype=np.uint8)
    b = b[:rows * bpl].reshape(rows, bpl)
    if mode == "rw16":
        rgb, img, w, h = _render_rw16(rows, b, png, pct)
    else:
        rgb, img, w, h = _render_yuyv(rows, b, png)
    if png:
        img.save(png)
    if display:
        show(rgb, img, w, h, "%s/color %dx%d" % (mode, w, h))


def main():
    ap = argparse.ArgumentParser(
        description="Pure-Python V4L2 capture + render (color node: NV12/YUYV or RW16 RGB calib)")
    ap.add_argument("--dev", default="/dev/video-rs-color-0")
    ap.add_argument("--mode", choices=("nv12", "yuyv", "rw16"), default="nv12",
                    help="nv12=csitest NV12 node carrying YUYV 16bpp, yuyv=product color "
                         "node YUYV 4:2:2, rw16=RW16 GRBG10-in-16-bit RGB calibration "
                         "surface (default nv12)")
    ap.add_argument("-W", "--width", type=int, default=1280)
    ap.add_argument("-H", "--height", type=int, default=720)
    ap.add_argument("--frames", type=int, default=90, help="max frames to dequeue while waiting for data (~3s @30fps)")
    ap.add_argument("--save-index", type=int, default=2, help="don't save before this frame (skips initial sync frames)")
    ap.add_argument("--raw", help="save the captured frame to this raw file (optional)")
    ap.add_argument("--png", help="render the captured frame to this PNG (optional)")
    ap.add_argument("--display", action="store_true", help="open a window showing the rendered image")
    ap.add_argument("--pct", type=float, default=None,
                    help="rw16 only: contrast-stretch by clipping at this percentile of nonzero samples (e.g. 99.5)")
    ap.add_argument("--render-only", action="store_true", help="skip capture, just render --raw")
    a = ap.parse_args()
    if a.pct is not None and a.mode != "rw16":
        sys.exit("--pct only applies to --mode rw16")
    if a.render_only:
        if not a.raw or not (a.png or a.display):
            sys.exit("--render-only needs --raw (input) and at least one of --png / --display")
        # both modes are 16 bpp: pixel width == --width, bytesperline == width*2
        render(a.mode, a.height, a.width * BPP[a.mode], raw=a.raw, png=a.png,
               display=a.display, pct=a.pct)
        return
    W, H, bpl, size, frame = capture(a.dev, a.width, a.height, MODES[a.mode], a.frames, a.save_index, a.raw)
    # render using the negotiated geometry
    rows = (size // bpl) if bpl else H
    if a.png or a.display:
        render(a.mode, rows, bpl, data=frame, png=a.png, display=a.display, pct=a.pct)
    if not a.raw and not a.png and not a.display:
        print("note: none of --raw / --png / --display given; capture ran but output nothing")


if __name__ == "__main__":
    main()
