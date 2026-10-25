#!/usr/bin/env python3
"""Compile PR #702's actual driver helpers with offline bus/state doubles."""
from pathlib import Path
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
SOURCE = (HERE.parents[1] / "kernel/realsense/d4xx_main.c").read_text()


def function(signature):
    start = SOURCE.index(signature)
    cursor = SOURCE.index("{", start) + 1
    depth = 1
    while depth:
        depth += (SOURCE[cursor] == "{") - (SOURCE[cursor] == "}")
        cursor += 1
    return SOURCE[start:cursor]


PREAMBLE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint32_t u32;
struct physical {
    int lock;
    bool device_mode_valid;
    u32 active_device_mode;
    bool depth_streaming, rgb_streaming, ir_streaming, imu_streaming;
};
struct ds5 { struct physical *ds5_dev; };
static u8 accepted;
static unsigned reads, writes;
static int read_error_at, write_error;
static bool refuse;
static void mutex_lock(int *lock) { assert(*lock == 0); *lock = 1; }
static void mutex_unlock(int *lock) { assert(*lock == 1); *lock = 0; }
static int ds5_raw_read(struct ds5 *s, unsigned reg, void *out, unsigned size) {
    assert(s->ds5_dev->lock == 1 && reg == D500_PASSIVE_DEPTH_XU_BASE && size == 1);
    ++reads;
    if ((int)reads == read_error_at) return -EREMOTEIO;
    *(u8*)out = accepted;
    return 0;
}
static int ds5_raw_write(struct ds5 *s, unsigned reg, const void *in, unsigned size) {
    assert(s->ds5_dev->lock == 1 && reg == D500_PASSIVE_DEPTH_XU_BASE && size == 1);
    ++writes;
    if (write_error) return write_error;
    if (!refuse) accepted = *(const u8*)in;
    return 0;
}
'''

MAIN = r'''
int main(void) {
    struct physical camera = {.device_mode_valid = true, .active_device_mode = 1};
    struct ds5 state = {&camera};
    u32 value = 99;
    assert(d500_get_passive_depth(&state, NULL) == -EINVAL);
    for (u32 mode = 0; mode <= 2; ++mode) {
        assert(d500_set_passive_depth(&state, mode) == 0);
        assert(d500_get_passive_depth(&state, &value) == 0 && value == mode);
    }
    assert(reads == 9 && writes == 3 && camera.lock == 0);
    reads = writes = 0;
    assert(d500_set_passive_depth(&state, 255) == -EINVAL);
    camera.device_mode_valid = false;
    assert(d500_get_passive_depth(&state, &value) == -EOPNOTSUPP);
    assert(d500_set_passive_depth(&state, 1) == -EOPNOTSUPP);
    camera.device_mode_valid = true;
    camera.active_device_mode = 0;
    assert(d500_set_passive_depth(&state, 1) == -EOPNOTSUPP);
    camera.active_device_mode = 1;
    assert(reads == 0 && writes == 0 && camera.lock == 0);
    bool *streams[] = {&camera.depth_streaming, &camera.rgb_streaming,
                      &camera.ir_streaming, &camera.imu_streaming};
    for (unsigned i = 0; i < 4; ++i) {
        *streams[i] = true;
        assert(d500_set_passive_depth(&state, 2) == -EBUSY);
        assert(d500_get_passive_depth(&state, &value) == 0 && value == 2);
        *streams[i] = false;
    }
    assert(writes == 0 && camera.lock == 0);
    accepted = 0xff;
    assert(d500_set_passive_depth(&state, 1) == -EOPNOTSUPP && writes == 0);
    accepted = 3;
    assert(d500_set_passive_depth(&state, 1) == -EBADMSG && writes == 0);
    accepted = 1;
    refuse = true;
    assert(d500_set_passive_depth(&state, 2) == -EIO && accepted == 1);
    /* PR #702 uses value readback, so idle same-value refusal is not observable. */
    assert(d500_set_passive_depth(&state, 1) == 0);
    refuse = false;
    reads = writes = 0;
    read_error_at = 1;
    assert(d500_set_passive_depth(&state, 1) == -EREMOTEIO && writes == 0);
    reads = 0;
    read_error_at = 0;
    write_error = -EIO;
    assert(d500_set_passive_depth(&state, 1) == -EIO && writes == 1 && reads == 1);
    reads = writes = 0;
    write_error = 0;
    read_error_at = 2;
    assert(d500_set_passive_depth(&state, 2) == -EREMOTEIO && writes == 1);
    assert(camera.lock == 0);
    puts("PASS: PR702 production GET/SET, all-camera idle incl IMU, probe/readback and errors");
    return 0;
}
'''

defines = "\n".join(
    re.search(r"^#define\s+" + name + r"\s+[^\n]+", SOURCE, re.M).group(0)
    for name in ("D500_PASSIVE_DEPTH_XU_BASE", "D500_GMSL_READ_FILL_BYTE"))
enums = "\n".join(
    re.search(r"enum " + name + r"\s*\{[^}]+};", SOURCE).group(0)
    for name in ("d500_device_mode", "d500_passive_depth_mode"))
helpers = "\n".join(function(signature) for signature in (
    "static bool d500_video_is_idle_locked(",
    "static bool d500_camera_is_idle_locked(",
    "static int d500_read_passive_depth_locked(",
    "static int d500_get_passive_depth(",
    "static int d500_set_passive_depth("))
with tempfile.TemporaryDirectory(prefix="pr702-", dir=HERE) as directory:
    source, executable = Path(directory) / "test.c", Path(directory) / "test"
    source.write_text(defines + "\n" + enums + "\n" + PREAMBLE + helpers + MAIN)
    subprocess.run([
        "cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
        str(source), "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
