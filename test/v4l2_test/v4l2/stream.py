"""StreamContext: buffer management, mmap or user memory, streamon/streamoff, frame capture."""

import ctypes
import mmap
import select

from . import ioctls
from . import structs as S


class StreamContext:
    """Context manager for V4L2 streaming I/O.

    MMAP maps the driver's buffers; USERPTR queues page-aligned anonymous
    mappings of `buf_size` bytes (the format's sizeimage).
    """

    def __init__(self, device, buf_type=ioctls.V4L2_BUF_TYPE_VIDEO_CAPTURE,
                 buf_count=4, memory=ioctls.V4L2_MEMORY_MMAP, buf_size=0):
        if memory == ioctls.V4L2_MEMORY_USERPTR and buf_size <= 0:
            raise ValueError("USERPTR streaming needs buf_size")
        self.device = device
        self.buf_type = buf_type
        self.buf_count = buf_count
        self.memory = memory
        self.buf_size = buf_size
        self.buffers = []
        self._userptr = []
        self._streaming = False

    def __enter__(self):
        self._reqbufs()
        if self.memory == ioctls.V4L2_MEMORY_USERPTR:
            self._alloc_user_buffers()
        else:
            self._mmap_buffers()
        self._queue_all()
        self._streamon()
        return self

    def __exit__(self, *exc):
        self._streamoff()
        self._unmap_buffers()
        self._reqbufs_release()
        return False

    def _reqbufs(self):
        req = S.v4l2_requestbuffers()
        req.count = self.buf_count
        req.type = self.buf_type
        req.memory = self.memory
        self.device.ioctl(ioctls.VIDIOC_REQBUFS, req)
        self.buf_count = req.count

    def _reqbufs_release(self):
        req = S.v4l2_requestbuffers()
        req.count = 0
        req.type = self.buf_type
        req.memory = self.memory
        try:
            self.device.ioctl(ioctls.VIDIOC_REQBUFS, req)
        except OSError:
            pass

    def _mmap_buffers(self):
        for i in range(self.buf_count):
            buf = S.v4l2_buffer()
            buf.type = self.buf_type
            buf.memory = ioctls.V4L2_MEMORY_MMAP
            buf.index = i
            self.device.ioctl(ioctls.VIDIOC_QUERYBUF, buf)

            mm = mmap.mmap(
                self.device.fileno(),
                buf.length,
                mmap.MAP_SHARED,
                mmap.PROT_READ | mmap.PROT_WRITE,
                offset=buf.m.offset,
            )
            self.buffers.append((buf, mm))

    def _alloc_user_buffers(self):
        for i in range(self.buf_count):
            buf = S.v4l2_buffer()
            buf.index = i
            buf.length = self.buf_size
            mm = mmap.mmap(-1, self.buf_size)
            # The ctypes view pins the mapping and gives the address to queue.
            view = ctypes.c_char.from_buffer(mm)
            self._userptr.append(view)
            self.buffers.append((buf, mm))

    def _unmap_buffers(self):
        self._userptr.clear()
        for _, mm in self.buffers:
            mm.close()
        self.buffers.clear()

    def _qbuf(self, index):
        buf = S.v4l2_buffer()
        buf.type = self.buf_type
        buf.memory = self.memory
        buf.index = index
        if self.memory == ioctls.V4L2_MEMORY_USERPTR:
            buf.m.userptr = ctypes.addressof(self._userptr[index])
            buf.length = self.buf_size
        self.device.ioctl(ioctls.VIDIOC_QBUF, buf)

    def _queue_all(self):
        for i in range(self.buf_count):
            self._qbuf(i)

    def _streamon(self):
        buf_type = ctypes.c_int(self.buf_type)
        self.device.ioctl(ioctls.VIDIOC_STREAMON, buf_type)
        self._streaming = True

    def _streamoff(self):
        if self._streaming:
            buf_type = ctypes.c_int(self.buf_type)
            try:
                self.device.ioctl(ioctls.VIDIOC_STREAMOFF, buf_type)
            except OSError:
                pass
            self._streaming = False

    def dequeue(self, timeout=5.0):
        """Dequeue a single buffer. Returns (v4l2_buffer, data_bytes)."""
        ready, _, _ = select.select([self.device.fileno()], [], [], timeout)
        if not ready:
            raise TimeoutError(f"No frame within {timeout}s")

        buf = S.v4l2_buffer()
        buf.type = self.buf_type
        buf.memory = self.memory
        self.device.ioctl(ioctls.VIDIOC_DQBUF, buf)

        _, mm = self.buffers[buf.index]
        mm.seek(0)
        data = mm.read(buf.bytesused)
        return buf, data

    def requeue(self, buf):
        """Re-queue a dequeued buffer."""
        self._qbuf(buf.index)

    def capture_frames(self, count, timeout=5.0):
        """Capture `count` frames, returning list of (v4l2_buffer, data)."""
        frames = []
        for _ in range(count):
            buf, data = self.dequeue(timeout=timeout)
            frames.append((buf, data))
            self.requeue(buf)
        return frames
