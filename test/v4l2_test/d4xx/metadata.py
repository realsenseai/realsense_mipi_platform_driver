"""Metadata ctypes structs mirroring metadata.h, with CRC32 validation."""

import ctypes
import zlib


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
