#!/usr/bin/env python3
"""Compile production MAX96717 stream arm/stop code against fake I2C, then inject failures.

Requires gcc. Optional argument: an alternate max96717.c for before/after checks.
"""
from pathlib import Path
import subprocess, tempfile, sys

root = Path(__file__).resolve().parents[2]
s = Path(sys.argv[1] if len(sys.argv) > 1 else root / "nvidia-oot/max96717.c").read_text()
f = s[s.index("static int max96717_mipi_rx_reset_pulse(") : s.index("int max96717_init_settings(")]
f += s[s.index("int max96717_set_pipe(") : s.index("#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)\nstatic int max96717_probe")]
stubs = r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define BIT(n) (1u << (n))
#define EXPORT_SYMBOL(x)
#define dev_dbg(...) ((void)0)
#define mutex_lock(p) ((void)(p))
#define mutex_unlock(p) ((void)(p))
#define MAX96717_MAX_VC 16
#define MAX96717_MIPI_RX0_ADDR 0x330
#define MAX96717_MIPI_RX0_RESET 0x48
#define MAX96717_MIPI_RX0_NORMAL 0x40
struct device {};
struct max96717 {
    int lock;
    bool pixel_mode;
    u16 active_vc_mask;
    bool rx_rearm_pending;
};
static struct max96717 priv;
static struct max96717 *dev_get_drvdata(struct device *d) { (void)d; return &priv; }
static void usleep_range(unsigned a, unsigned b) { (void)a; (void)b; }
static bool rx_in_reset, fail_deassert;
static int pulses;
static int max96717_write_reg(struct device *d, u16 addr, u8 val) {
    (void)d;
    assert(addr == MAX96717_MIPI_RX0_ADDR);
    if (val == MAX96717_MIPI_RX0_RESET) {
        pulses++;
        rx_in_reset = true;
        return 0;
    }
    if (fail_deassert)
        return -EREMOTEIO;
    rx_in_reset = false;
    return 0;
}
"""
tests = r"""
int main(void) {
    struct device dev;
    priv.pixel_mode = true;
    assert(max96717_set_pipe(&dev, 0, 0, 0, 0) == 0 && priv.active_vc_mask == 1);

    /* Last stream stops but the reset-deassert write fails: RX is held in reset. */
    fail_deassert = true;
    assert(max96717_stream_stop(&dev, 0) != 0);
    assert(rx_in_reset && priv.active_vc_mask == 0);

    /* Arming a different VC retries the shared recovery and fails closed. */
    pulses = 0;
    assert(max96717_set_pipe(&dev, 0, 0, 0, 1) != 0);
    assert(pulses == 1 && rx_in_reset && priv.active_vc_mask == 0);

    /* Once I2C recovers, arming any VC completes the re-arm first. */
    fail_deassert = false;
    pulses = 0;
    assert(max96717_set_pipe(&dev, 0, 0, 0, 1) == 0);
    assert(pulses == 1 && !rx_in_reset && priv.active_vc_mask == 2);
    pulses = 0;
    assert(max96717_set_pipe(&dev, 0, 0, 0, 2) == 0 && pulses == 0);

    /* Recovery completed through a retried stop is not repeated on arm. */
    assert(max96717_stream_stop(&dev, 1) == 0 && pulses == 0);
    fail_deassert = true;
    assert(max96717_stream_stop(&dev, 2) != 0 && rx_in_reset);
    fail_deassert = false;
    pulses = 0;
    assert(max96717_stream_stop(&dev, 2) == 0 && pulses == 1 && !rx_in_reset);
    assert(max96717_set_pipe(&dev, 0, 0, 0, 0) == 0 && pulses == 1);
    puts("PASS: production MAX96717 failed last-stream re-arm is completed before "
         "arming any VC, retried by stop, and not repeated once done");
    return 0;
}
"""
with tempfile.TemporaryDirectory() as td:
    p = Path(td) / "test.c"
    p.write_text(stubs + f + tests)
    out = Path(td) / "test"
    subprocess.run(
        ["gcc", "-std=gnu99", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Werror",
         str(p), "-o", str(out)],
        check=True,
    )
    subprocess.run([str(out)], check=True)
