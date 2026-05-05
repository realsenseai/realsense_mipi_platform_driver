#!/usr/bin/env python3
#
# ext_sync_gen.py - Control TSC signal generators via /dev/cdi_tsc.
#
# Uses the kernel CDI TSC driver ioctl interface. Supports runtime
# frequency and duty cycle changes via CDI_TSC_SET_RATE ioctl.
# Does not require root if /dev/cdi_tsc permissions allow user access
# (e.g. udev rule: KERNEL=="cdi_tsc", MODE="0666").

import argparse
import fcntl
import os
import struct
import sys

# CDI_TSC_FSYNC    = _IOW('T', 1, int)              = 0x40045401
# CDI_TSC_SET_RATE = _IOW('T', 2, struct{u32,u32})  = 0x40085402
CDI_TSC_FSYNC    = 0x40045401
CDI_TSC_SET_RATE = 0x40085402

CDI_TSC_DEV = "/dev/cdi_tsc"


def tsc_fsync(fd, on):
    """Send start (on=1) or stop (on=0) to the TSC driver."""
    fcntl.ioctl(fd, CDI_TSC_FSYNC, struct.pack("i", on))


def tsc_set_rate(fd, freq_hz, duty_cycle):
    """Set frequency and duty cycle for all generators."""
    fcntl.ioctl(fd, CDI_TSC_SET_RATE, struct.pack("II", freq_hz, duty_cycle))


def main():
    parser = argparse.ArgumentParser(
        description="Control TSC signal generators via /dev/cdi_tsc")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--enable", action="store_true",
                       help="Start all enabled TSC generators")
    group.add_argument("--disable", action="store_true",
                       help="Stop all TSC generators")
    parser.add_argument("--fps", type=int, default=None,
                        help="Signal frequency in Hz (1-120, sets before start - without it - default taken from DTS)")
    parser.add_argument("--duty", type=int, default=None,
                        help="Duty cycle in percent (1-99, sets before start - without it - default taken from DTS)")
    args = parser.parse_args()

    if not os.path.exists(CDI_TSC_DEV):
        print(f"Error: {CDI_TSC_DEV} not found (TSC driver not loaded?)",
              file=sys.stderr)
        sys.exit(1)

    try:
        fd = os.open(CDI_TSC_DEV, os.O_RDWR)
        try:
            if args.enable:
                if args.fps is not None or args.duty is not None:
                    fps = args.fps if args.fps is not None else 30
                    duty = args.duty if args.duty is not None else 25
                    tsc_set_rate(fd, fps, duty)
                    print(f"Rate set: {fps} Hz, {duty}% duty")
                tsc_fsync(fd, 1)
                print("TSC generators started")
            elif args.disable:
                tsc_fsync(fd, 0)
                print("TSC generators stopped")
        finally:
            os.close(fd)
    except PermissionError:
        print(f"Error: permission denied on {CDI_TSC_DEV}. "
              "Add udev rule: KERNEL==\"cdi_tsc\", MODE=\"0666\"",
              file=sys.stderr)
        sys.exit(1)
    except OSError as e:
        print(f"Error: ioctl failed: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
