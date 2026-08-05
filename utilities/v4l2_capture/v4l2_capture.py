#!/usr/bin/env python3
"""Shared pure-Python V4L2 MMAP capture core for the D4XX CSI capture tools.

depth_capture.py, ir_capture.py and color_capture.py all drive the V4L2 MMAP
pipeline identically -- the only per-stream differences are the pixel format
requested and how the captured frame is decoded for display. That common core
(the ioctl encoding, the V4L2 ctypes structs, the VIDIOC constants, the
capture loop and the display fallback chain) lives here so an ABI/ioctl fix
only has to be made once.

This module is imported by the three sibling scripts; it is not a standalone
tool. Keep it in the same directory as them.
"""
import ctypes as C, fcntl, mmap, os, sys

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
        # (its real members contain pointers); this pads `type` and makes
        # sizeof == 208, so VIDIOC_S_FMT == 0xc0d05605.
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


def _has_data(buf, ln):
    """Cheap all-zero test: sample ~4K bytes spread across the frame.
    Distinguishes a real frame from a zero-filled (no-stream) error buffer."""
    step = max(1, ln // 4096)
    return any(buf[i] for i in range(0, ln, step))


def capture(dev, W, H, pixfmt, nframes, save_idx, raw_out):
    """Run the full V4L2 MMAP pipeline on `dev`, requesting `pixfmt` at WxH.

    Dumps the captured frame *even when the V4L2 error flag is set* (v4l2-ctl
    --stream-to silently drops error-flagged buffers; this does not). Keeps the
    first frame WITH data at/after `save_idx` (skips initial sync/zero frames);
    otherwise keeps the most recent as a fallback, optionally writing it to
    `raw_out`. Returns (W, H, bpl, size, frame) using the geometry the driver
    negotiated; `frame` is the kept bytes (or None if none were captured)."""
    fd = os.open(dev, os.O_RDWR)
    maps = []
    try:
        f = v4l2_format(); f.type = BUF_TYPE_VIDEO_CAPTURE
        f.fmt.pix.width = W; f.fmt.pix.height = H
        f.fmt.pix.pixelformat = pixfmt; f.fmt.pix.field = FIELD_NONE
        fcntl.ioctl(fd, VIDIOC_S_FMT, f)
        W, H = f.fmt.pix.width, f.fmt.pix.height
        bpl, size = f.fmt.pix.bytesperline, f.fmt.pix.sizeimage
        print("fmt %ux%u fourcc=%s bpl=%u size=%u"
              % (W, H, fourcc_str(f.fmt.pix.pixelformat), bpl, size))
        # VIDIOC_S_FMT does NOT fail on an unsupported pixel format: the driver
        # silently substitutes one it does support. Every caller then decodes
        # the buffer as the format it asked for, which renders pure garbage.
        # Fail loudly instead -- a wrong image is far more expensive to debug.
        got = f.fmt.pix.pixelformat
        if got != pixfmt:
            raise SystemExit(
                "requested %s but the driver negotiated %s on %s -- this node does not "
                "support %s.\nCheck 'v4l2-ctl -d %s --list-formats' (do not pipe it "
                "through head; the format you want may be the last entry)."
                % (fourcc_str(pixfmt), fourcc_str(got), dev, fourcc_str(pixfmt), dev))

        rb = v4l2_requestbuffers(); rb.count = 4
        rb.type = BUF_TYPE_VIDEO_CAPTURE; rb.memory = MEMORY_MMAP
        fcntl.ioctl(fd, VIDIOC_REQBUFS, rb)

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
            # keep the first frame WITH data at/after save_idx (skips initial sync/zero frames);
            # otherwise keep the most recent as a fallback
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
            print("WARNING: no frame contained data in %d frames -- is the stream running "
                  "during the capture? Keeping last (empty) frame." % nframes)
        if raw_out and frame is not None:
            with open(raw_out, "wb") as o:
                o.write(frame)
            print("wrote raw -> %s" % raw_out)
        return W, H, bpl, size, frame
    finally:
        for mm in maps:
            mm.close()
        os.close(fd)


def show(g, img, W, H, title):
    """Pop up a window showing the rendered frame.

    Uses pure tkinter (Tk talks straight to X11 with NO D-Bus), which is fast
    and reliable over X11 forwarding / ssh -X / MobaXterm as a normal user.
    GNOME viewers (eog) are avoided as the primary path because they connect
    to the user's D-Bus session bus and can stall ~20-30s on dconf/gvfs --
    that was the 'slow without sudo' symptom (root via sudo -E is fast only
    because the session bus rejects root, so its eog runs bus-less).
    Falls back to eog, then matplotlib, if Tk is unavailable.

    `g` may be 2-D (greyscale) or 3-D (RGB); the matplotlib fallback picks the
    colormap accordingly."""
    if not os.environ.get("DISPLAY") and sys.platform.startswith("linux"):
        print("note: --display but $DISPLAY is unset; for a remote window use 'ssh -X' "
              "(or MobaXterm with X11 forwarding on)")
    # 1) pure Tk window -- no D-Bus, no GNOME services -> fast as normal user
    try:
        import tkinter as tk
        from PIL import ImageTk
        root = tk.Tk(); root.title(title)
        photo = ImageTk.PhotoImage(img)            # keep ref alive until mainloop ends
        tk.Label(root, image=photo).pack()
        root.bind("<Escape>", lambda e: root.destroy())
        root.bind("q", lambda e: root.destroy())
        print("showing window (Esc/q or close it to exit) ...")
        root.mainloop()
        return
    except Exception as e:
        print("tk display failed (%s); trying eog" % e)
    # 2) eog fallback (NO_AT_BRIDGE avoids the a11y-bus stall)
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
    # 3) matplotlib fallback
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
