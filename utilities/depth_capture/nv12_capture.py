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

Examples
--------
  ./nv12_capture.py --dev /dev/video-rs-color-0 -W 1280 -H 720 --display
  ./nv12_capture.py --raw c.raw --png c.png
  # Run WITHOUT sudo so --display works (the video node is group-accessible).
"""
import argparse, ctypes as C, fcntl, mmap, os, sys

# ---- ioctl encoding (asm-generic) ----
def _IOC(d, t, nr, size): return (d << 30) | (size << 16) | (ord(t) << 8) | nr
def _IOWR(t, nr, sz): return _IOC(3, t, nr, sz)
def _IOW(t, nr, sz):  return _IOC(1, t, nr, sz)

# ---- V4L2 structs (64-bit ABI) ----
class v4l2_pix_format(C.Structure):
    _fields_ = [(n, C.c_uint32) for n in (
        "width", "height", "pixelformat", "field", "bytesperline", "sizeimage",
        "colorspace", "priv", "flags", "ycbcr_enc", "quantization", "xfer_func")]

class v4l2_format(C.Structure):
    class _fmt(C.Union):
        # _align forces 8-byte alignment so the union matches the kernel
        # (its real members contain pointers); pads `type` -> sizeof == 208,
        # so VIDIOC_S_FMT == 0xc0d05605.
        _fields_ = [("pix", v4l2_pix_format), ("raw_data", C.c_uint8 * 200),
                    ("_align", C.c_uint64)]
    _fields_ = [("type", C.c_uint32), ("fmt", _fmt)]

class v4l2_requestbuffers(C.Structure):
    _fields_ = [("count", C.c_uint32), ("type", C.c_uint32), ("memory", C.c_uint32),
                ("capabilities", C.c_uint32), ("flags", C.c_uint8), ("reserved", C.c_uint8 * 3)]

class _timeval(C.Structure):
    _fields_ = [("tv_sec", C.c_long), ("tv_usec", C.c_long)]

class _timecode(C.Structure):
    _fields_ = [("type", C.c_uint32), ("flags", C.c_uint32), ("frames", C.c_uint8),
                ("seconds", C.c_uint8), ("minutes", C.c_uint8), ("hours", C.c_uint8),
                ("userbits", C.c_uint8 * 4)]

class v4l2_buffer(C.Structure):
    class _m(C.Union):
        _fields_ = [("offset", C.c_uint32), ("userptr", C.c_ulong),
                    ("planes", C.c_void_p), ("fd", C.c_int32)]
    _fields_ = [("index", C.c_uint32), ("type", C.c_uint32), ("bytesused", C.c_uint32),
                ("flags", C.c_uint32), ("field", C.c_uint32), ("timestamp", _timeval),
                ("timecode", _timecode), ("sequence", C.c_uint32), ("memory", C.c_uint32),
                ("m", _m), ("length", C.c_uint32), ("reserved2", C.c_uint32),
                ("request_fd", C.c_int32)]

VIDIOC_S_FMT     = _IOWR('V', 5,  C.sizeof(v4l2_format))
VIDIOC_REQBUFS   = _IOWR('V', 8,  C.sizeof(v4l2_requestbuffers))
VIDIOC_QUERYBUF  = _IOWR('V', 9,  C.sizeof(v4l2_buffer))
VIDIOC_QBUF      = _IOWR('V', 15, C.sizeof(v4l2_buffer))
VIDIOC_DQBUF     = _IOWR('V', 17, C.sizeof(v4l2_buffer))
VIDIOC_STREAMON  = _IOW('V', 18,  C.sizeof(C.c_int))
VIDIOC_STREAMOFF = _IOW('V', 19,  C.sizeof(C.c_int))

BUF_TYPE_VIDEO_CAPTURE = 1
MEMORY_MMAP            = 1
FIELD_NONE             = 1
BUF_FLAG_ERROR         = 0x0040
def fourcc(s): return s[0] | (s[1] << 8) | (s[2] << 16) | (s[3] << 24)
def fourcc_str(v): return "".join(chr((v >> (8 * i)) & 0xff) for i in range(4))
PIX_FMT_NV12 = fourcc(b"NV12")


def _has_data(buf, ln):
    """Cheap all-zero test: sample ~4K bytes spread across the frame.
    Distinguishes a real frame from a zero-filled (no-stream) error buffer."""
    step = max(1, ln // 4096)
    return any(buf[i] for i in range(0, ln, step))


def capture(dev, W, H, nframes, save_idx, raw_out):
    fd = os.open(dev, os.O_RDWR)
    try:
        f = v4l2_format(); f.type = BUF_TYPE_VIDEO_CAPTURE
        f.fmt.pix.width = W; f.fmt.pix.height = H
        f.fmt.pix.pixelformat = PIX_FMT_NV12; f.fmt.pix.field = FIELD_NONE
        fcntl.ioctl(fd, VIDIOC_S_FMT, f)
        W, H = f.fmt.pix.width, f.fmt.pix.height
        bpl, size = f.fmt.pix.bytesperline, f.fmt.pix.sizeimage
        # render as 16-bit using the negotiated geometry
        cols, rows = bpl // 2, (size // bpl if bpl else H)
        print("fmt %ux%u fourcc=%s bpl=%u size=%u -> 16-bit grid %ux%u"
              % (W, H, fourcc_str(f.fmt.pix.pixelformat), bpl, size, cols, rows))

        rb = v4l2_requestbuffers(); rb.count = 4
        rb.type = BUF_TYPE_VIDEO_CAPTURE; rb.memory = MEMORY_MMAP
        fcntl.ioctl(fd, VIDIOC_REQBUFS, rb)

        maps = []
        for i in range(rb.count):
            b = v4l2_buffer(); b.type = BUF_TYPE_VIDEO_CAPTURE; b.memory = MEMORY_MMAP; b.index = i
            fcntl.ioctl(fd, VIDIOC_QUERYBUF, b)
            mm = mmap.mmap(fd, b.length, mmap.MAP_SHARED, mmap.PROT_READ, offset=b.m.offset)
            maps.append(mm)
            fcntl.ioctl(fd, VIDIOC_QBUF, b)

        fcntl.ioctl(fd, VIDIOC_STREAMON, C.c_int(BUF_TYPE_VIDEO_CAPTURE))
        frame = None             # bytes of the kept frame (first with data, else last)
        got_data = False
        for n in range(nframes):
            b = v4l2_buffer(); b.type = BUF_TYPE_VIDEO_CAPTURE; b.memory = MEMORY_MMAP
            fcntl.ioctl(fd, VIDIOC_DQBUF, b)
            err = 1 if (b.flags & BUF_FLAG_ERROR) else 0
            ln = b.bytesused or maps[b.index].size()
            data = _has_data(maps[b.index], ln)
            print("dq idx=%d used=%u err=%d seq=%u data=%d" % (b.index, b.bytesused, err, b.sequence, data))
            if (not got_data) and n >= save_idx and data:
                frame = bytes(maps[b.index][:ln]); got_data = True
                print("captured frame seq=%u err=%d (%u bytes)" % (b.sequence, err, ln))
            elif frame is None:
                frame = bytes(maps[b.index][:ln])
            fcntl.ioctl(fd, VIDIOC_QBUF, b)
            if got_data:
                break
        fcntl.ioctl(fd, VIDIOC_STREAMOFF, C.c_int(BUF_TYPE_VIDEO_CAPTURE))
        if not got_data:
            print("WARNING: no frame contained data in %d frames -- is csitest streaming during the "
                  "capture? Keeping last (empty) frame." % nframes)
        if raw_out and frame is not None:
            with open(raw_out, "wb") as o:
                o.write(frame)
            print("wrote raw -> %s" % raw_out)
        return cols, rows, frame
    finally:
        os.close(fd)


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
        _show(rgb, img, w, rows)


def _show(g, img, W, H):
    """Pop up a window showing the rendered frame, drawn with pure tkinter
    (Tk talks straight to X11, no D-Bus -> fast over ssh -X / MobaXterm as a
    normal user; GNOME viewers stall on the user D-Bus session bus). Falls
    back to eog, then matplotlib."""
    title = "nv12/color %dx%d" % (W, H)
    if not os.environ.get("DISPLAY") and sys.platform.startswith("linux"):
        print("note: --display but $DISPLAY is unset; for a remote window use 'ssh -X' "
              "(or MobaXterm with X11 forwarding on)")
    try:
        import tkinter as tk
        from PIL import ImageTk
        root = tk.Tk(); root.title(title)
        photo = ImageTk.PhotoImage(img)
        tk.Label(root, image=photo).pack()
        root.bind("<Escape>", lambda e: root.destroy())
        root.bind("q", lambda e: root.destroy())
        print("showing window (Esc/q or close it to exit) ...")
        root.mainloop()
        return
    except Exception as e:
        print("tk display failed (%s); trying eog" % e)
    import shutil
    if shutil.which("eog"):
        try:
            import subprocess, tempfile
            fd, p = tempfile.mkstemp(suffix=".png"); os.close(fd); img.save(p)
            env = dict(os.environ, NO_AT_BRIDGE="1", GTK_A11Y="none")
            subprocess.Popen(["eog", p], env=env)
            print("opened in eog: %s (close the window when done)" % p)
            return
        except Exception as e:
            print("eog display failed (%s); trying matplotlib" % e)
    try:
        import matplotlib
        matplotlib.use("TkAgg", force=True)
        import matplotlib.pyplot as plt
        plt.figure(title)
        if getattr(g, "ndim", 2) == 3:
            plt.imshow(g)
        else:
            plt.imshow(g, cmap="gray", vmin=0, vmax=255)
        plt.title(title); plt.tight_layout()
        print("showing matplotlib window (close it to exit) ...")
        plt.show()
        return
    except Exception as e:
        print("could not open a display window: %s\n"
              "  -> save with --png and copy it off the device to view." % e)


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
    cols, rows, frame = capture(a.dev, a.width, a.height, a.frames, a.save_index, a.raw)
    if a.png or a.display:
        render(cols, rows, data=frame, png=a.png, display=a.display)
    if not a.raw and not a.png and not a.display:
        print("note: none of --raw / --png / --display given; capture ran but output nothing")


if __name__ == "__main__":
    main()
