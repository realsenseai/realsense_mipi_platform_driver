#!/usr/bin/env python3
"""
Script to stream frames with different resolutions using V4L2 directly.
Supports streaming from one or two video devices simultaneously.
Uses direct V4L2 ioctl calls via Python's fcntl and ctypes.
"""

import argparse
import sys
import os
import threading
import mmap
import select
import fcntl
import ctypes
import time
from datetime import datetime

# V4L2 constants - calculated for ARM64
VIDIOC_QUERYCAP = 0x80685600
VIDIOC_S_FMT = 0xc0d05605
VIDIOC_G_FMT = 0xc0d05604
VIDIOC_REQBUFS = 0xc0145608
VIDIOC_QUERYBUF = 0xc0585609   # 88 bytes on 64-bit
VIDIOC_QBUF = 0xc058560f       # 88 bytes on 64-bit
VIDIOC_DQBUF = 0xc0585611      # 88 bytes on 64-bit
VIDIOC_STREAMON = 0x40045612
VIDIOC_STREAMOFF = 0x40045613
VIDIOC_S_PARM = 0xc0cc5616
VIDIOC_G_PARM = 0xc0cc5615

V4L2_BUF_TYPE_VIDEO_CAPTURE = 1
V4L2_MEMORY_MMAP = 1
V4L2_FIELD_NONE = 1

# V4L2 structures using ctypes
class v4l2_capability(ctypes.Structure):
    _fields_ = [
        ("driver", ctypes.c_char * 16),
        ("card", ctypes.c_char * 32),
        ("bus_info", ctypes.c_char * 32),
        ("version", ctypes.c_uint32),
        ("capabilities", ctypes.c_uint32),
        ("device_caps", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32 * 3),
    ]

class v4l2_pix_format(ctypes.Structure):
    _fields_ = [
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
        ("pixelformat", ctypes.c_uint32),
        ("field", ctypes.c_uint32),
        ("bytesperline", ctypes.c_uint32),
        ("sizeimage", ctypes.c_uint32),
        ("colorspace", ctypes.c_uint32),
        ("priv", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("ycbcr_enc", ctypes.c_uint32),
        ("quantization", ctypes.c_uint32),
        ("xfer_func", ctypes.c_uint32),
    ]

class v4l2_format(ctypes.Structure):
    class _u(ctypes.Union):
        _fields_ = [
            ("pix", v4l2_pix_format),
            ("raw_data", ctypes.c_char * 200),
        ]
    _fields_ = [
        ("type", ctypes.c_uint32),
        ("_pad", ctypes.c_uint32),  # Padding for 8-byte alignment on ARM64
        ("fmt", _u),
    ]

class v4l2_requestbuffers(ctypes.Structure):
    _fields_ = [
        ("count", ctypes.c_uint32),
        ("type", ctypes.c_uint32),
        ("memory", ctypes.c_uint32),
        ("capabilities", ctypes.c_uint32),
        ("flags", ctypes.c_uint8),
        ("reserved", ctypes.c_uint8 * 3),
    ]

class v4l2_timecode(ctypes.Structure):
    _fields_ = [
        ("type", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("frames", ctypes.c_uint8),
        ("seconds", ctypes.c_uint8),
        ("minutes", ctypes.c_uint8),
        ("hours", ctypes.c_uint8),
        ("userbits", ctypes.c_uint8 * 4),
    ]

class v4l2_buffer(ctypes.Structure):
    class _u(ctypes.Union):
        _fields_ = [
            ("offset", ctypes.c_uint32),
            ("userptr", ctypes.c_ulong),
            ("planes", ctypes.c_void_p),
            ("fd", ctypes.c_int32),
        ]
    _fields_ = [
        ("index", ctypes.c_uint32),
        ("type", ctypes.c_uint32),
        ("bytesused", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("field", ctypes.c_uint32),
        ("timestamp_sec", ctypes.c_long),
        ("timestamp_usec", ctypes.c_long),
        ("timecode", v4l2_timecode),
        ("sequence", ctypes.c_uint32),
        ("memory", ctypes.c_uint32),
        ("m", _u),
        ("length", ctypes.c_uint32),
        ("reserved2", ctypes.c_uint32),
        ("request_fd", ctypes.c_int32),
    ]

class v4l2_fract(ctypes.Structure):
    _fields_ = [
        ("numerator", ctypes.c_uint32),
        ("denominator", ctypes.c_uint32),
    ]

class v4l2_captureparm(ctypes.Structure):
    _fields_ = [
        ("capability", ctypes.c_uint32),
        ("capturemode", ctypes.c_uint32),
        ("timeperframe", v4l2_fract),
        ("extendedmode", ctypes.c_uint32),
        ("readbuffers", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32 * 4),
    ]

class v4l2_streamparm(ctypes.Structure):
    class _u(ctypes.Union):
        _fields_ = [
            ("capture", v4l2_captureparm),
            ("raw_data", ctypes.c_char * 200),
        ]
    _fields_ = [
        ("type", ctypes.c_uint32),
        ("parm", _u),
    ]


# Resolution profiles: (width, height, fps, name)
PROFILES = [
    (1280, 720, 30, "HD (720p)"),
    (640, 480, 30, "VGA (480p)"),
]

DEFAULT_FRAMES_PER_PROFILE = 100
NUM_BUFFERS = 4


def timestamp():
    """Return current timestamp string."""
    return datetime.now().strftime("%H:%M:%S")


def validate_device(device):
    """Check if video device exists."""
    if not os.path.exists(device):
        print(f"Error: Video device {device} does not exist")
        sys.exit(1)


def fourcc_to_int(fourcc):
    """Convert fourcc string to int."""
    return (ord(fourcc[0]) | (ord(fourcc[1]) << 8) | 
            (ord(fourcc[2]) << 16) | (ord(fourcc[3]) << 24))


class V4L2Device:
    """Direct V4L2 device access using ioctl."""
    
    def __init__(self, device_path):
        self.device_path = device_path
        self.fd = None
        self.buffers = []
        self.buffer_starts = []
        
    def open(self):
        """Open the video device."""
        self.fd = os.open(self.device_path, os.O_RDWR | os.O_NONBLOCK)
        
    def close(self):
        """Close the video device."""
        if self.fd is not None:
            self._cleanup_buffers()
            os.close(self.fd)
            self.fd = None
            
    def __enter__(self):
        self.open()
        return self
        
    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()
        return False
    
    def get_format(self):
        """Get current video format."""
        fmt = v4l2_format()
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE
        fcntl.ioctl(self.fd, VIDIOC_G_FMT, fmt)
        return fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.pixelformat
        
    def set_format(self, width, height, pixel_format=None):
        """Set video format. If pixel_format is None, keeps current format."""
        fmt = v4l2_format()
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE
        
        # First get current format to preserve pixel format if not specified
        fcntl.ioctl(self.fd, VIDIOC_G_FMT, fmt)
        current_pixfmt = fmt.fmt.pix.pixelformat
        
        # Set new dimensions
        fmt.fmt.pix.width = width
        fmt.fmt.pix.height = height
        if pixel_format:
            fmt.fmt.pix.pixelformat = fourcc_to_int(pixel_format)
        else:
            fmt.fmt.pix.pixelformat = current_pixfmt
        fmt.fmt.pix.field = V4L2_FIELD_NONE
        
        fcntl.ioctl(self.fd, VIDIOC_S_FMT, fmt)
            
        return fmt.fmt.pix.width, fmt.fmt.pix.height
        
    def set_fps(self, fps):
        """Set frame rate."""
        parm = v4l2_streamparm()
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE
        parm.parm.capture.timeperframe.numerator = 1
        parm.parm.capture.timeperframe.denominator = fps
        
        try:
            fcntl.ioctl(self.fd, VIDIOC_S_PARM, parm)
        except OSError:
            pass  # Some devices don't support setting frame rate
            
    def _request_buffers(self, count):
        """Request memory-mapped buffers."""
        req = v4l2_requestbuffers()
        req.count = count
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE
        req.memory = V4L2_MEMORY_MMAP
        
        fcntl.ioctl(self.fd, VIDIOC_REQBUFS, req)
        return req.count
        
    def _setup_buffers(self, count):
        """Setup memory-mapped buffers."""
        self._cleanup_buffers()
        
        actual_count = self._request_buffers(count)
        
        for i in range(actual_count):
            buf = v4l2_buffer()
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE
            buf.memory = V4L2_MEMORY_MMAP
            buf.index = i
            
            fcntl.ioctl(self.fd, VIDIOC_QUERYBUF, buf)
            
            # Memory map the buffer
            buffer_start = mmap.mmap(
                self.fd, buf.length,
                mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE,
                offset=buf.m.offset
            )
            self.buffer_starts.append(buffer_start)
            self.buffers.append(buf)
            
    def _cleanup_buffers(self):
        """Clean up memory-mapped buffers."""
        for buf_start in self.buffer_starts:
            buf_start.close()
        self.buffer_starts = []
        self.buffers = []
        
        # Free buffers
        try:
            req = v4l2_requestbuffers()
            req.count = 0
            req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE
            req.memory = V4L2_MEMORY_MMAP
            fcntl.ioctl(self.fd, VIDIOC_REQBUFS, req)
        except:
            pass
            
    def _queue_buffer(self, index):
        """Queue a buffer for capture."""
        buf = v4l2_buffer()
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE
        buf.memory = V4L2_MEMORY_MMAP
        buf.index = index
        fcntl.ioctl(self.fd, VIDIOC_QBUF, buf)
        
    def _dequeue_buffer(self):
        """Dequeue a filled buffer."""
        buf = v4l2_buffer()
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE
        buf.memory = V4L2_MEMORY_MMAP
        fcntl.ioctl(self.fd, VIDIOC_DQBUF, buf)
        return buf
        
    def _stream_on(self):
        """Start streaming."""
        buf_type = ctypes.c_uint32(V4L2_BUF_TYPE_VIDEO_CAPTURE)
        fcntl.ioctl(self.fd, VIDIOC_STREAMON, buf_type)
        
    def _stream_off(self):
        """Stop streaming."""
        buf_type = ctypes.c_uint32(V4L2_BUF_TYPE_VIDEO_CAPTURE)
        try:
            fcntl.ioctl(self.fd, VIDIOC_STREAMOFF, buf_type)
        except:
            pass
            
    def stream_frames(self, num_frames, on_frame_callback=None):
        """Stream the specified number of frames."""
        self._setup_buffers(NUM_BUFFERS)
        
        # Queue all buffers
        for i in range(len(self.buffers)):
            self._queue_buffer(i)
            
        self._stream_on()
        
        frames_captured = 0
        try:
            while frames_captured < num_frames:
                # Wait for buffer to be ready
                ready, _, _ = select.select([self.fd], [], [], 5.0)
                if not ready:
                    break
                    
                # Dequeue buffer
                buf = self._dequeue_buffer()
                frames_captured += 1
                
                # Call callback
                if on_frame_callback:
                    on_frame_callback(frames_captured)
                    
                # Re-queue buffer
                self._queue_buffer(buf.index)
                
        finally:
            self._stream_off()
            self._cleanup_buffers()
            
        return frames_captured


def stream_frames(device, width, height, fps, frames, profile_name, lock=None):
    """
    Set format and capture frames from a video device.
    Prints '>' for each frame received immediately.
    """
    prefix = f"[{device}]"
    
    def log(msg):
        if lock:
            with lock:
                print(msg, flush=True)
        else:
            print(msg, flush=True)
    
    log(f"[{timestamp()}] >>> {device}: Setting profile: {profile_name} ({width}x{height} @ {fps}fps)")
    
    def on_frame(frame_num):
        # Print > immediately for each frame, no newline
        if lock:
            with lock:
                print(">", end="", flush=True)
        else:
            print(">", end="", flush=True)
    
    try:
        with V4L2Device(device) as dev:
            actual_w, actual_h = dev.set_format(width, height)
            log(f"[{timestamp()}] {prefix} Format set to {actual_w}x{actual_h}")
            dev.set_fps(fps)
            log(f"[{timestamp()}] {prefix} Capturing {frames} frames...")

            # Print prefix before streaming starts
            if lock:
                with lock:
                    print(f"{prefix} ", end="", flush=True)
            else:
                print(f"{prefix} ", end="", flush=True)

            captured = dev.stream_frames(frames, on_frame)

            # Newline after all the > signs
            print(flush=True)

            log(f"[{timestamp()}] {prefix} ✓ Successfully streamed {captured} frames")
            log("")
            return True

    except Exception as e:
        print(flush=True)  # Ensure newline after any partial output
        log(f"[{timestamp()}] {prefix} Error: {e}")
        log("")
        return False


def run_profiles_single(device, repetitions, frames_per_profile, delay=1.0, stop_on_first_failure=False):
    """Run all profiles on a single device."""
    for rep in range(1, repetitions + 1):
        print("=" * 40)
        print(f"=== REPETITION {rep} of {repetitions} ===")
        print("=" * 40)
        print()
        
        print(f"[{timestamp()}] Streaming to {device}")
        print()
        
        for i, (width, height, fps, name) in enumerate(PROFILES):
            ok = stream_frames(device, width, height, fps, frames_per_profile, name)
            if not ok and stop_on_first_failure:
                print(f"[{timestamp()}] Stopping on first failure as requested")
                return False
            # Add delay between profiles (not after the last one)
            if i < len(PROFILES) - 1 and delay > 0:
                print(f"[{timestamp()}] Waiting {delay}s before next stream...")
                time.sleep(delay)
        
        print(f"[{timestamp()}] === Repetition {rep} completed ===")
        print()


def run_profiles_dual(device1, device2, repetitions, frames_per_profile, delay=1.0, stop_on_first_failure=False):
    """Run all profiles on two devices in parallel."""
    lock = threading.Lock()
    
    for rep in range(1, repetitions + 1):
        print("=" * 40)
        print(f"=== REPETITION {rep} of {repetitions} ===")
        print("=" * 40)
        print()
        
        print(f"[{timestamp()}] Streaming to both devices in parallel")
        print()
        
        for i, (width, height, fps, name) in enumerate(PROFILES):
            results = {}

            def worker(dev, key):
                results[key] = stream_frames(dev, width, height, fps, frames_per_profile, name, lock)

            t1 = threading.Thread(target=worker, args=(device1, "d1"))
            t2 = threading.Thread(target=worker, args=(device2, "d2"))

            t1.start()
            t2.start()

            t1.join()
            t2.join()
            print()

            # If either thread failed and stop_on_first_failure is set, stop.
            if stop_on_first_failure and (not results.get("d1", True) or not results.get("d2", True)):
                print(f"[{timestamp()}] Stopping on first failure as requested")
                return False
            
            # Add delay between profiles (not after the last one)
            if i < len(PROFILES) - 1 and delay > 0:
                print(f"[{timestamp()}] Waiting {delay}s before next stream...")
                time.sleep(delay)
        
        print(f"[{timestamp()}] === Repetition {rep} completed ===")
        print()


def main():
    parser = argparse.ArgumentParser(
        description="Stream frames with different resolutions using direct V4L2",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=f"""
Examples:
  Single device:  %(prog)s /dev/video0 3
  Dual device:    %(prog)s /dev/video0 /dev/video1 3
  With options:   %(prog)s -d /dev/video0 -d /dev/video2 -r 5

Profiles streamed (each with {DEFAULT_FRAMES_PER_PROFILE} frames by default):
  - HD (720p):  1280x720 @ 30fps
  - VGA:        640x480 @ 30fps
  - 848x480:    848x480 @ 30fps
  - 640x360:    640x360 @ 30fps
  - 480x270:    480x270 @ 30fps
"""
    )
    
    parser.add_argument(
        "devices",
        nargs="*",
        help="Video device(s) to stream from (e.g., /dev/video0)"
    )
    parser.add_argument(
        "-d", "--device",
        action="append",
        dest="device_list",
        help="Video device (can be specified multiple times, max 2)"
    )
    parser.add_argument(
        "-r", "--repetitions",
        type=int,
        default=1,
        help="Number of repetitions (default: 1)"
    )
    parser.add_argument(
        "-f", "--frames",
        type=int,
        default=DEFAULT_FRAMES_PER_PROFILE,
        help=f"Frames per profile (default: {DEFAULT_FRAMES_PER_PROFILE})"
    )
    parser.add_argument(
        "-D", "--delay",
        type=float,
        default=1.0,
        help="Delay in seconds between stopping and starting streams (default: 1.0)"
    )
    
    parser.add_argument(
        "--stop-on-first-failure",
        action="store_true",
        dest="stop_on_first_failure",
        help="Stop the run on the first streaming failure"
    )

    args = parser.parse_args()
    
    # Collect devices from both positional args and -d options
    devices = []
    repetitions = args.repetitions
    frames_per_profile = args.frames
    delay = args.delay
    
    if args.device_list:
        devices.extend(args.device_list)
    
    if args.devices:
        # Parse positional arguments (legacy mode: device [device2] repetitions)
        pos_args = args.devices
        if len(pos_args) >= 1:
            # Check if last arg is a number (repetitions)
            try:
                rep_val = int(pos_args[-1])
                repetitions = rep_val
                devices.extend(pos_args[:-1])
            except ValueError:
                # All args are devices
                devices.extend(pos_args)
    
    # Validate we have at least one device
    if not devices:
        parser.print_help()
        print("\nError: At least one video device is required")
        sys.exit(1)
    
    if len(devices) > 2:
        print("Error: Maximum of 2 video devices supported")
        sys.exit(1)
    
    if repetitions < 1:
        print("Error: Repetitions must be a positive number")
        sys.exit(1)
    
    # Validate devices exist
    for device in devices:
        validate_device(device)
    
    # Print startup info
    print("=== Video Streaming Script (Python) ===")
    print(f"Device 1: {devices[0]}")
    if len(devices) == 2:
        print(f"Device 2: {devices[1]}")
    print(f"Repetitions: {repetitions}")
    print(f"Frames per profile: {frames_per_profile}")
    print(f"Delay between streams: {delay}s")
    print()
    
    # Add CLI option effect: stop on first failure
    stop_on_first_failure = getattr(args, 'stop_on_first_failure', False)

    try:
        if len(devices) == 2:
            ok = run_profiles_dual(devices[0], devices[1], repetitions, frames_per_profile, delay, stop_on_first_failure=stop_on_first_failure)
        else:
            ok = run_profiles_single(devices[0], repetitions, frames_per_profile, delay, stop_on_first_failure=stop_on_first_failure)

        if ok is False:
            print("\nStopped due to failure")
            sys.exit(1)

        print("=" * 40)
        print("=== ALL REPETITIONS COMPLETED ===")
        print("=" * 40)
    except KeyboardInterrupt:
        print("\n\nInterrupted by user")
        sys.exit(130)


if __name__ == "__main__":
    main()
