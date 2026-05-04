#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: Copyright (c) 2024, INTEL CORPORATION.  All rights reserved.
#
# tsc_sync.py - Control TSC signal generators on Tegra234 for camera sync.
#
# Programs up to 3 generators via /dev/mem:
#   EDGE_OUT #0 (generator@380, PBB.02)
#   EDGE_OUT #1 (generator@400, PAA.04)
#   EDGE_OUT #2 (generator@480, PAA.07)
# Requires: pinmux for target pins set to "tsc" function (via DT overlay).
# Must run as root.

import argparse
import mmap
import os
import struct
import sys
import time

# --- TSC register map ---
TSC_CONTROLLER_BASE = 0xc6a0000
TSC_REG_SIZE         = 0x10000

# Generator bases (absolute addresses)
TSC_GEN_BASES = [
    0xc6a0380,   # generator@380 (EDGE_OUT #0, PBB.02)
    0xc6a0400,   # generator@400 (EDGE_OUT #1, PAA.04)
    0xc6a0480,   # generator@480 (EDGE_OUT #2, PAA.07)
]
TSC_GEN_NAMES = [
    "@380 (PBB.02)",
    "@400 (PAA.04)",
    "@480 (PAA.07)",
]
NUM_GENERATORS = len(TSC_GEN_BASES)

# Controller registers (offsets from TSC_CONTROLLER_BASE)
TSC_MTSCCNTCV0 = 0x10
TSC_MTSCCNTCV1 = 0x14

# Generator registers (offsets from TSC_GEN0_BASE)
TSC_GENX_CTRL   = 0x00
TSC_GENX_START0 = 0x04
TSC_GENX_START1 = 0x08
TSC_GENX_STATUS = 0x0C
TSC_GENX_EDGE0  = 0x18
TSC_GENX_EDGE1  = 0x1C

# Bit definitions
TSC_GENX_CTRL_ENABLE      = (1 << 0)
TSC_GENX_CTRL_INITIAL_VAL = (1 << 1)

TSC_GENX_STATUS_WAITING = (1 << 0)
TSC_GENX_STATUS_RUNNING = (1 << 1)

TSC_GENX_EDGEX_TOGGLE = (1 << 29)
TSC_GENX_EDGEX_LOOP   = (1 << 28)
TSC_GENX_EDGEX_OFFSET_MASK = 0x0FFFFFFF

TSC_TICKS_PER_HZ     = 31250000
TSC_NS_PER_TICK       = 32
NS_PER_MS             = 1000000
START_OFFSET_MS       = 100
MAX_FREQ_HZ           = 120


class TSCController:
    def __init__(self):
        self.fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
        self.mm = mmap.mmap(self.fd, TSC_REG_SIZE,
                            mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE,
                            offset=TSC_CONTROLLER_BASE)

    def close(self):
        self.mm.close()
        os.close(self.fd)

    def _read32(self, offset):
        """Read 32-bit register at offset from TSC_CONTROLLER_BASE."""
        self.mm.seek(offset)
        return struct.unpack("<I", self.mm.read(4))[0]

    def _write32(self, offset, val):
        """Write 32-bit register at offset from TSC_CONTROLLER_BASE."""
        self.mm.seek(offset)
        self.mm.write(struct.pack("<I", val & 0xFFFFFFFF))

    # Generator register helpers (offset relative to controller base)
    GEN_OFFSETS = [b - TSC_CONTROLLER_BASE for b in TSC_GEN_BASES]

    def gen_read(self, gen_idx, reg):
        return self._read32(self.GEN_OFFSETS[gen_idx] + reg)

    def gen_write(self, gen_idx, reg, val):
        self._write32(self.GEN_OFFSETS[gen_idx] + reg, val)

    def is_running(self, gen_idx):
        status = self.gen_read(gen_idx, TSC_GENX_STATUS)
        return bool(status & (TSC_GENX_STATUS_RUNNING | TSC_GENX_STATUS_WAITING))

    def read_tsc_counter(self):
        lo = self._read32(TSC_MTSCCNTCV0) & 0xFFFFFFFF
        hi = self._read32(TSC_MTSCCNTCV1) & 0x00FFFFFF
        return (hi << 32) | lo

    def stop(self, generators=None):
        """Stop generators. If generators is None, stop all enabled ones."""
        if generators is None:
            generators = range(NUM_GENERATORS)
        ok = True
        for gi in generators:
            self.gen_write(gi, TSC_GENX_CTRL, 0)
            for _ in range(100):
                if not self.is_running(gi):
                    break
                time.sleep(0.001)
            else:
                ok = False
        return ok

    def start(self, freq_hz, duty_cycle, generators=None):
        """Start generators with given frequency and duty cycle."""
        if generators is None:
            generators = range(NUM_GENERATORS)
        if freq_hz <= 0 or freq_hz > MAX_FREQ_HZ:
            print(f"Error: frequency must be 1-{MAX_FREQ_HZ} Hz", file=sys.stderr)
            return False
        if duty_cycle <= 0 or duty_cycle >= 100:
            print("Error: duty cycle must be 1-99%", file=sys.stderr)
            return False

        # Stop first if running
        for gi in generators:
            if self.is_running(gi):
                self.gen_write(gi, TSC_GENX_CTRL, 0)
                for _ in range(100):
                    if not self.is_running(gi):
                        break
                    time.sleep(0.001)
                else:
                    print(f"Error: generator {gi} failed to stop", file=sys.stderr)
                    return False

        # Calculate edge ticks
        ticks_in_period = TSC_TICKS_PER_HZ // freq_hz
        ticks_active = ticks_in_period * duty_cycle // 100
        ticks_inactive = ticks_in_period - ticks_active

        if ticks_active > TSC_GENX_EDGEX_OFFSET_MASK or \
           ticks_inactive > TSC_GENX_EDGEX_OFFSET_MASK:
            print("Error: tick values overflow edge registers", file=sys.stderr)
            return False

        # Program edges
        edge0 = TSC_GENX_EDGEX_TOGGLE | (ticks_active & TSC_GENX_EDGEX_OFFSET_MASK)
        edge1 = TSC_GENX_EDGEX_TOGGLE | TSC_GENX_EDGEX_LOOP | \
                (ticks_inactive & TSC_GENX_EDGEX_OFFSET_MASK)

        # Program start time: current TSC + 100ms (same start for all)
        now = self.read_tsc_counter()
        start_ticks = now + (START_OFFSET_MS * NS_PER_MS // TSC_NS_PER_TICK)

        for gi in generators:
            self.gen_write(gi, TSC_GENX_EDGE0, edge0)
            self.gen_write(gi, TSC_GENX_EDGE1, edge1)
            self.gen_write(gi, TSC_GENX_START0, start_ticks & 0xFFFFFFFF)
            self.gen_write(gi, TSC_GENX_START1, (start_ticks >> 32) & 0x00FFFFFF)
            # Enable: INITIAL_VAL=1 (start high), ENABLE=1
            self.gen_write(gi, TSC_GENX_CTRL,
                           TSC_GENX_CTRL_INITIAL_VAL | TSC_GENX_CTRL_ENABLE)

        # Verify they started
        time.sleep(0.15)
        for gi in generators:
            if not self.is_running(gi):
                print(f"Warning: generator {gi} ({TSC_GEN_NAMES[gi]}) not running",
                      file=sys.stderr)
                return False

        return True

    def status(self):
        """Print status of all generators."""
        for gi in range(NUM_GENERATORS):
            st = self.gen_read(gi, TSC_GENX_STATUS)
            running = bool(st & TSC_GENX_STATUS_RUNNING)
            waiting = bool(st & TSC_GENX_STATUS_WAITING)
            ctrl = self.gen_read(gi, TSC_GENX_CTRL)
            enabled = bool(ctrl & TSC_GENX_CTRL_ENABLE)
            print(f"Generator{TSC_GEN_NAMES[gi]}: enabled={enabled} running={running} "
                  f"waiting={waiting} (STATUS=0x{st:08x} CTRL=0x{ctrl:08x})")


def main():
    parser = argparse.ArgumentParser(
        description="Control TSC signal generators for camera sync")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--enable", action="store_true",
                       help="Enable TSC signal output")
    group.add_argument("--disable", action="store_true",
                       help="Disable TSC signal output")
    group.add_argument("--status", action="store_true",
                       help="Print generator status")
    parser.add_argument("--fps", type=int, default=30,
                        help="Signal frequency in Hz (default: 30)")
    parser.add_argument("--duty", type=int, default=25,
                        help="Duty cycle in percent (default: 25)")
    parser.add_argument("--gen", type=int, nargs="+", metavar="N",
                        help="Generator indices to control (0-2, default: all)")
    args = parser.parse_args()

    if os.geteuid() != 0:
        print("Error: must run as root", file=sys.stderr)
        sys.exit(1)

    gens = None
    if args.gen is not None:
        for g in args.gen:
            if g < 0 or g >= NUM_GENERATORS:
                print(f"Error: generator index must be 0-{NUM_GENERATORS-1}",
                      file=sys.stderr)
                sys.exit(1)
        gens = args.gen

    tsc = TSCController()
    try:
        if args.status:
            tsc.status()
        elif args.disable:
            if tsc.stop(gens):
                print("TSC generators stopped")
            else:
                print("Error: failed to stop generators", file=sys.stderr)
                sys.exit(1)
        elif args.enable:
            if tsc.start(args.fps, args.duty, gens):
                print(f"TSC generators started: {args.fps} Hz, {args.duty}% duty cycle")
            else:
                sys.exit(1)
    finally:
        tsc.close()


if __name__ == "__main__":
    main()
