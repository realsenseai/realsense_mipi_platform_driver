"""Metadata ctypes structs mirroring metadata.h, with CRC32 validation."""

import ctypes
import struct
import zlib
from typing import NamedTuple, Optional

# Embedded metadata line as the host driver walks it: a line header, then a
# chain of blocks, each starting with (id, size).
MD_LINE_BYTES = 255
MD_LINE_HEADER_BYTES = 12
MD_BLOCK_HEADER_BYTES = 8
MD_BLOCK_SIZE_OFF = 4

# Compressed-frame (byte-count) block: id, version, minimum size, field offsets.
MD_ID_COMPRESSED_FRAME = 0x80000026
MD_COMPRESSED_VERSION = 2
MD_COMPRESSED_BLOCK_BYTES = 20
MD_COMPRESSED_VERSION_OFF = 8
MD_COMPRESSED_BYTES_OFF = 12


class STMetaDataIdHeader(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("metaDataID", ctypes.c_uint32),
        ("size", ctypes.c_uint32),
    ]


class STMetaDataIntelCaptureTiming(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("metaDataIdHeader", STMetaDataIdHeader),
        ("version", ctypes.c_uint32),
        ("flag", ctypes.c_uint32),
        ("frameCounter", ctypes.c_uint32),
        ("opticalTimestamp", ctypes.c_uint32),
        ("readoutTime", ctypes.c_uint32),
        ("exposureTime", ctypes.c_uint32),
        ("frameInterval", ctypes.c_uint32),
        ("pipeLatency", ctypes.c_uint32),
    ]


class STMetaDataCaptureStats(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("metaDataIdHeader", STMetaDataIdHeader),
        ("Flags", ctypes.c_uint32),
        ("hwTimestamp", ctypes.c_uint32),
        ("ExposureTime", ctypes.c_uint64),
        ("ExposureCompensationFlags", ctypes.c_uint64),
        ("ExposureCompensationValue", ctypes.c_int32),
        ("IsoSpeed", ctypes.c_uint32),
        ("FocusState", ctypes.c_uint32),
        ("LensPosition", ctypes.c_uint32),
        ("WhiteBalance", ctypes.c_uint32),
        ("Flash", ctypes.c_uint32),
        ("FlashPower", ctypes.c_uint32),
        ("ZoomFactor", ctypes.c_uint32),
        ("SceneMode", ctypes.c_uint64),
        ("SensorFramerate", ctypes.c_uint64),
    ]


class STMetaDataIntelDepthControl(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("metaDataIdHeader", STMetaDataIdHeader),
        ("version", ctypes.c_uint32),
        ("flag", ctypes.c_uint32),
        ("manualGain", ctypes.c_uint32),
        ("manualExposure", ctypes.c_uint32),
        ("laserPower", ctypes.c_uint32),
        ("autoExposureMode", ctypes.c_uint32),
        ("exposurePriority", ctypes.c_uint32),
        ("exposureROILeft", ctypes.c_uint32),
        ("exposureROIRight", ctypes.c_uint32),
        ("exposureROITop", ctypes.c_uint32),
        ("exposureROIBottom", ctypes.c_uint32),
        ("preset", ctypes.c_uint32),
        ("projectorMode", ctypes.c_uint8),
        ("reserved", ctypes.c_uint8),
        ("ledPower", ctypes.c_uint16),
    ]


class STMetaDataIntelConfiguration(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("metaDataIdHeader", STMetaDataIdHeader),
        ("version", ctypes.c_uint32),
        ("flag", ctypes.c_uint32),
        ("HWType", ctypes.c_uint8),
        ("SKUsID", ctypes.c_uint8),
        ("cookie", ctypes.c_uint32),
        ("format", ctypes.c_uint16),
        ("width", ctypes.c_uint16),
        ("height", ctypes.c_uint16),
        ("FPS", ctypes.c_uint16),
        ("trigger", ctypes.c_uint16),
        ("calibrationCount", ctypes.c_uint16),
        ("Reserved", ctypes.c_uint8 * 6),
    ]


class STMetaDataDepthYNormalMode(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("intelCaptureTiming", STMetaDataIntelCaptureTiming),
        ("captureStats", STMetaDataCaptureStats),
        ("intelDepthControl", STMetaDataIntelDepthControl),
        ("intelConfiguration", STMetaDataIntelConfiguration),
        ("crc32", ctypes.c_uint32),
    ]


class STSubPresetInfo(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("value", ctypes.c_uint32),
    ]


class STMetaDataExtMipiDepthIR(ctypes.LittleEndianStructure):
    """The 'new depth struct' — extended MIPI depth/IR metadata."""
    _pack_ = 1
    _fields_ = [
        ("res", ctypes.c_uint32 * 3),
        ("Frame_counter", ctypes.c_uint32),
        ("metaDataID", ctypes.c_uint32),
        ("size", ctypes.c_uint32),
        ("version", ctypes.c_uint8),
        ("calibInfo", ctypes.c_uint16),
        ("reserved", ctypes.c_uint8 * 1),
        ("flags", ctypes.c_uint32),
        ("hwTimestamp", ctypes.c_uint32),
        ("opticalTimestamp", ctypes.c_uint32),
        ("exposureTime", ctypes.c_uint32),
        ("manualExposure", ctypes.c_uint32),
        ("laserPower", ctypes.c_uint16),
        ("trigger", ctypes.c_uint16),
        ("projectorMode", ctypes.c_uint8),
        ("preset", ctypes.c_uint8),
        ("manualGain", ctypes.c_uint8),
        ("autoExposureMode", ctypes.c_uint8),
        ("inputWidth", ctypes.c_uint16),
        ("inputHeight", ctypes.c_uint16),
        ("subpresetInfo", STSubPresetInfo),
        ("crc32", ctypes.c_uint32),
    ]


def parse_metadata(data):
    """Parse raw metadata bytes into a struct.

    Tries the extended MIPI struct first, falls back to normal mode.
    Returns (struct_instance, struct_type_name).
    """
    ext_size = ctypes.sizeof(STMetaDataExtMipiDepthIR)
    normal_size = ctypes.sizeof(STMetaDataDepthYNormalMode)

    if len(data) >= ext_size:
        md = STMetaDataExtMipiDepthIR()
        ctypes.memmove(ctypes.addressof(md), data[:ext_size], ext_size)
        return md, "ExtMipiDepthIR"

    if len(data) >= normal_size:
        md = STMetaDataDepthYNormalMode()
        ctypes.memmove(ctypes.addressof(md), data[:normal_size], normal_size)
        return md, "DepthYNormalMode"

    return None, None


def validate_crc32(data, struct_instance, struct_type):
    """Validate CRC32 of metadata.

    The CRC covers all bytes before the crc32 field.
    """
    if struct_type == "ExtMipiDepthIR":
        crc_offset = ctypes.sizeof(STMetaDataExtMipiDepthIR) - 4
    elif struct_type == "DepthYNormalMode":
        crc_offset = ctypes.sizeof(STMetaDataDepthYNormalMode) - 4
    else:
        return False

    payload = data[:crc_offset]
    expected_crc = struct_instance.crc32
    computed_crc = zlib.crc32(payload) & 0xFFFFFFFF
    return computed_crc == expected_crc


def find_block(data, block_id):
    """Offset of block `block_id` found by walking the block-ID chain, or None.

    Stops at the first block whose size is under a header or runs past the data.
    """
    off = MD_LINE_HEADER_BYTES
    while off + MD_BLOCK_HEADER_BYTES <= len(data):
        bid, size = struct.unpack_from("<II", data, off)
        if size < MD_BLOCK_HEADER_BYTES or off + size > len(data):
            return None
        if bid == block_id:
            return off
        off += size
    return None


class CompressedFrameBlock(NamedTuple):
    size: int
    version: int
    encoded_bytes: int


def parse_compressed_frame(data) -> Optional[CompressedFrameBlock]:
    """The byte-count block of a compressed colour frame, or None if absent."""
    off = find_block(data, MD_ID_COMPRESSED_FRAME)
    if off is None:
        return None
    size = struct.unpack_from("<I", data, off + MD_BLOCK_SIZE_OFF)[0]
    if size < MD_COMPRESSED_BLOCK_BYTES:
        return None
    return CompressedFrameBlock(
        size=size,
        version=struct.unpack_from("<I", data, off + MD_COMPRESSED_VERSION_OFF)[0],
        encoded_bytes=struct.unpack_from("<I", data, off + MD_COMPRESSED_BYTES_OFF)[0],
    )
