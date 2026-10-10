#!/usr/bin/env python3
"""Same-image legacy/V2 timing caller. Dry-run by default; --run writes Flash.
The caller must hold the selected camera and Jetson reservations for the entire run.
Successful updates are activated with SDK HWMC rst; no host reboot is issued.
"""
import argparse
import fcntl
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import struct
import subprocess
import time
import zipfile
import zlib

CAPS, BEGIN, FINISH, STATUS, ABORT = 0x80404770, 0x40204771, 0x4772, 0x80404773, 0x4774

def validated_snapshot(fd, command, magic):
    out = bytearray(64)
    fcntl.ioctl(fd, command, out, True)
    saved = struct.unpack_from('<I', out, 60)[0]
    struct.pack_into('<I', out, 60, 0)
    if struct.unpack_from('<IHH', out) != (magic, 2, 64) or zlib.crc32(out) != saved:
        raise RuntimeError('Invalid V2 snapshot; refuse to continue')
    struct.pack_into('<I', out, 60, saved)
    return out

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('--mode', choices=['legacy', 'v2'], required=True)
    parser.add_argument('--dev', default='/dev/d4xx-dfu-0')
    parser.add_argument('--chunk', type=int, default=1024)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--serial', help='Exact D585 serial; required for --run')
    parser.add_argument('--run', action='store_true', help='Write Flash and issue HWMC rst; bench must be reserved')
    args = parser.parse_args()
    if args.chunk <= 0:
        parser.error('--chunk must be positive')
    image = args.image.read_bytes()
    offset = 412 if len(image) >= 412 and struct.unpack_from('<I', image, 398)[0] == 0x050c0764 else 0
    if image[offset:offset+4] != b'PK\x03\x04' or len(image) > 25524288:
        parser.error('Only build-generated compressed images within the D585 wire limit are accepted')
    with zipfile.ZipFile(io.BytesIO(image)) as archive:
        if archive.namelist() != ['image']:
            parser.error('Expected one ZIP entry named image')
        entry = archive.getinfo('image')
        if not 4096 <= entry.file_size <= 25000000 or entry.compress_type not in (0, 8):
            parser.error('Unsupported compression or raw image exceeds 25 MB')
        # Validate the ZIP CRC outside timed transport; do not recompress the artifact.
        raw = archive.read('image')
    del raw
    result = dict(mode=args.mode, image=str(args.image.resolve()), wire_bytes=len(image),
                  raw_bytes=entry.file_size, sha256=hashlib.sha256(image).hexdigest(),
                  md5_wire=hashlib.md5(image).hexdigest(), chunk=args.chunk, run=args.run,
                  kernel=platform.release(), pid=os.getpid())
    if offset:
        result['expected_bkc'] = list(struct.unpack_from('<4H', image, 260))
    if not args.run:
        print(json.dumps(result, indent=2)); return
    if not args.serial:
        parser.error('--serial is required for --run')
    import pyrealsense2 as rs
    context = rs.context()
    devices = [d for d in context.query_devices()
               if d.get_info(rs.camera_info.serial_number) == args.serial]
    if len(devices) != 1:
        raise RuntimeError('Expected exactly one device matching --serial')
    device = devices[0]
    result['serial'] = args.serial
    result['firmware_before'] = device.get_info(rs.camera_info.firmware_version)
    host_boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
    result['driver_srcversion'] = subprocess.check_output(['modinfo', '-F', 'srcversion', 'd4xx'], text=True).strip()
    if Path('/sys/module/d4xx/parameters/d585_dfu_v2_bench').read_text().strip() != 'Y':
        raise RuntimeError('Load the prepared module with d585_dfu_v2_bench=1 for both A/B modes')
    fd = os.open(args.dev, os.O_WRONLY | os.O_CLOEXEC)
    started = time.monotonic_ns()
    try:
        caps = validated_snapshot(fd, CAPS, 0x32434447)
        pid, flags = struct.unpack_from('<HH', caps, 16)
        if pid not in (0x0b6a, 0x0c07, 0x0c08) or flags & 0xf != 0xf:
            raise RuntimeError('Expected the D585 timing prototype with partial flags 0x0f')
        result['product_id'] = hex(pid)
        result['caps_hex'] = caps.hex()
        result['boot_nonce'] = hex(struct.unpack_from('<Q', caps, 8)[0])
        result['running_bkc'] = list(struct.unpack_from('<4H', caps, 36))
        if not struct.unpack_from('<Q', caps, 8)[0] or result['running_bkc'] != result['expected_bkc']:
            raise RuntimeError('Running firmware does not match benchmark image')
        if args.mode == 'v2':
            request = struct.pack('<HHI16s4H', 2, 0, len(image), hashlib.md5(image).digest(),
                                  *result.get('expected_bkc', [0]*4))
            fcntl.ioctl(fd, BEGIN, request)
        ready = time.monotonic_ns()
        position = 0
        while position < len(image):
            end = min(position + args.chunk, len(image))
            written = os.write(fd, image[position:end])
            if written != end-position:
                raise RuntimeError(f'Short write {written}/{end-position}; session must be inspected, not replayed blindly')
            position = end
        sent = time.monotonic_ns()
        if args.mode == 'v2':
            fcntl.ioctl(fd, FINISH)
            finished = time.monotonic_ns()
            ack = validated_snapshot(fd, STATUS, 0x32414447)
            if struct.unpack_from('<H', ack, 36)[0] != 5 or struct.unpack_from('<i', ack, 52)[0]:
                raise RuntimeError('Firmware did not report successful WAIT_RESET')
            result['ack_hex'] = ack.hex()
            result['accepted_bytes'] = struct.unpack_from('<I', ack, 28)[0]
            result['next_seq'] = struct.unpack_from('<I', ack, 24)[0]
            result['phase'] = struct.unpack_from('<H', ack, 36)[0]
            result['flash_result'] = struct.unpack_from('<i', ack, 52)[0]
            if result['accepted_bytes'] != len(image):
                raise RuntimeError('Accepted byte count does not match image')
            result['receive_s'] = (sent-ready)/1e9
            result['finish_s'] = (finished-sent)/1e9
        else:
            # For aligned legacy images flush/close performs the finalization.
            # For a short tail it happened inside the last write. Kernel
            # DFU_BENCH legacy log with this PID is the authoritative split.
            result['write_calls_s_includes_tail_manifest'] = (sent-ready)/1e9
        result['prepare_s'] = (ready-started)/1e9
    finally:
        os.close(fd)
    result['close_included_total_s'] = (time.monotonic_ns()-started)/1e9
    result['activation_included'] = False
    # Preserve the DFU timing even if activation fails after Flash was written.
    def save_result():
        if args.output:
            args.output.write_text(json.dumps(result, indent=2)+'\n')
    save_result()
    reset_started = time.monotonic()
    print('DFU complete; sending HWMC rst', flush=True)
    try:
        device.hardware_reset()
    except RuntimeError as error:
        # A delivered reset may disconnect before its acknowledgement. Require
        # independent new-boot CAP evidence rather than treating errno as success.
        result['reset_call_error'] = str(error)
    deadline = time.monotonic() + 30
    last_error = None
    while time.monotonic() < deadline:
        time.sleep(.5)
        try:
            check_fd = os.open(args.dev, os.O_WRONLY | os.O_CLOEXEC)
            try:
                new_caps = validated_snapshot(check_fd, CAPS, 0x32434447)
            finally:
                os.close(check_fd)
            new_boot = struct.unpack_from('<Q', new_caps, 8)[0]
            if not new_boot or hex(new_boot) == result['boot_nonce']:
                continue
            if list(struct.unpack_from('<4H', new_caps, 36)) != result['expected_bkc']:
                raise RuntimeError('Activated firmware BKC does not match image')
            fresh = rs.context()
            found = [d for d in fresh.query_devices()
                     if d.get_info(rs.camera_info.serial_number) == args.serial]
            if len(found) != 1:
                continue
            if Path('/proc/sys/kernel/random/boot_id').read_text().strip() != host_boot:
                raise RuntimeError('Unexpected host reboot during activation')
            result['boot_nonce_after'] = hex(new_boot)
            result['firmware_after'] = found[0].get_info(rs.camera_info.firmware_version)
            result['reset_and_enumeration_s'] = time.monotonic()-reset_started
            result['host_rebooted'] = False
            result['activation_verified'] = True
            break
        except (OSError, RuntimeError) as error:
            last_error = str(error)
    else:
        result['activation_verified'] = False
        result['activation_error'] = last_error or 'No fresh firmware boot after rst'
        save_result()
        raise RuntimeError(result['activation_error'])
    save_result()
    print(json.dumps(result, indent=2))

if __name__ == '__main__':
    main()
