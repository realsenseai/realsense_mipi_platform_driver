#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the production MinZ fallback with mocked transfers and locks."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def block(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


PREFIX = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint16_t __le16;
typedef uint32_t __le32;
#define __packed __attribute__((packed))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define DS5_CAMERA_CID_BASE 0x009a4000
struct camera { int lock, depth_streaming, ir_streaming; };
struct ds5 { struct camera *ds5_dev; };
struct ds5_sensor { int streaming; };
struct v4l2_ctrl { u32 id; struct { u8 *p_u8; } p_new; };
static int reads, writes, held, read_error, write_error;
static u16 expected_base = 0x4500;
static u8 current[38], written[38];
static int ds5_raw_read(struct ds5 *state, u16 addr, void *out, size_t size) {
 (void)state; assert(addr == expected_base && size == 38); ++reads;
 if (addr == 0x4500) assert(held == 1);
 if (read_error) return read_error;
 memcpy(out, current, size); return 0;
}
static int ds5_raw_write(struct ds5 *state, u16 addr, const void *data, size_t size) {
 (void)state; assert(addr == expected_base && size == 38); ++writes;
 if (addr == 0x4500) assert(held == 1);
 memcpy(written, data, size); return write_error;
}
static void mutex_lock(int *lock) { assert(!*lock && !held); *lock = 1; held = 1; }
static void mutex_unlock(int *lock) { assert(*lock && held); *lock = 0; held = 0; }
"""

MAIN = r"""
int main(void) {
 struct camera camera = {0};
 struct ds5 state = {&camera};
 struct ds5_sensor sensor = {0};
 struct d500_dpp_xu_control payload = {0};
 struct v4l2_ctrl control = {D500_CAMERA_CID_MINZ, {(u8 *)&payload}};
 assert(sizeof(payload) == 38);
 payload.header.version = 1;
 payload.header.control_id = 8;
 payload.param_count = 8; payload.params[0] = 2; payload.params[1] = 1;
 camera.depth_streaming = 1;
 memcpy(current, &payload, 38);
 payload.params[2] = 1;
 assert(write_control(&state, &control, &sensor) == 0);
 assert(writes == 1 && reads == 1 && !held && !camera.lock);
 for (int stream = 0; stream < 3; ++stream) {
  camera.depth_streaming = stream == 1;
  camera.ir_streaming = stream == 2;
  for (unsigned trial = 0; trial < 256; ++trial) {
   for (unsigned i = 0; i < 38; ++i) ((u8 *)&payload)[i] = trial + i;
   payload.header.version = 1; payload.param_count = 8;
   reads = writes = 0;
   memcpy(current, &payload, sizeof(current));
   assert(write_control(&state, &control, &sensor) == 0);
   assert(writes == 1 && reads == (stream != 0) && !held && !camera.lock);
   assert(memcmp(written, &payload, 38) == 0);
  }
 }
 payload.param_count = 8;
 for (int error = -1; error >= -121; --error) {
  write_error = error; reads = writes = 0;
  assert(write_control(&state, &control, &sensor) == error);
  assert(writes == 1 && reads == 1 && !held && !camera.lock);
 }
 write_error = 0;
 for (int stream = 0; stream < 3; ++stream) {
  camera.depth_streaming = stream == 1;
  camera.ir_streaming = stream == 2;
  for (int count = 0; count <= 9; ++count) {
   if (count == 8) continue;
   payload.param_count = count; reads = writes = 0;
   assert(write_control(&state, &control, &sensor) == -EINVAL);
   assert(!reads && !writes && !held && !camera.lock);
  }
 }
 memset(&payload, 0, sizeof(payload));
 payload.header.version = 1; payload.header.control_id = 8; payload.param_count = 8;
 payload.params[0] = 2; payload.params[1] = 1;
 for (int stream = 1; stream < 3; ++stream) {
  camera.depth_streaming = stream == 1;
  camera.ir_streaming = stream == 2;
  memcpy(current, &payload, 38); reads = writes = 0;
  assert(write_control(&state, &control, &sensor) == 0);
  assert(reads == 1 && writes == 1 && !held);
  for (int ratio = 2; ratio <= 4; ratio *= 2) {
   payload.params[1] = ratio; reads = writes = 0;
   assert(write_control(&state, &control, &sensor) == -EBUSY);
   assert(reads == 1 && writes == 0 && !held && !camera.lock);
  }
  payload.params[1] = 1;
  for (int mode = 0; mode <= 2; ++mode) {
   payload.params[2] = mode; reads = writes = 0;
   assert(write_control(&state, &control, &sensor) == 0);
   assert(reads == 1 && writes == 1 && !held && !camera.lock);
  }
  payload.params[0] = 0; writes = 0;
  assert(write_control(&state, &control, &sensor) == 0 && writes == 1 && !held);
  payload.params[0] = 2; writes = 0;
  assert(write_control(&state, &control, &sensor) == 0 && writes == 1 && !held);
  payload.header.flags = 1; writes = 0;
  assert(write_control(&state, &control, &sensor) == -EBUSY && !writes && !held);
  payload.header.flags = 0;
  payload.params[4] = 126; writes = 0;
  assert(write_control(&state, &control, &sensor) == 0 && writes == 1 && !held);
  for (int error = -1; error >= -121; --error) {
   read_error = error; writes = 0;
   assert(write_control(&state, &control, &sensor) == error);
   assert(!writes && !held && !camera.lock);
  }
  read_error = 0; current[0] = 0xff; writes = 0;
  assert(write_control(&state, &control, &sensor) == -EOPNOTSUPP && !writes && !held);
 }
 camera.depth_streaming = camera.ir_streaming = 0;
 payload.params[1] = 4; reads = writes = 0;
 assert(write_control(&state, &control, &sensor) == 0);
 assert(reads == 0 && writes == 1 && !held);
 control.id = D500_CAMERA_CID_DECIMATION; expected_base = 0x4540;
 payload.param_count = 2;
 sensor.streaming = 1; writes = 0;
 assert(write_control(&state, &control, &sensor) == -EBUSY && !writes);
 sensor.streaming = 0;
 assert(write_control(&state, &control, &sensor) == 0 && writes == 1);
 control.id = D500_CAMERA_CID_TEMPORAL; expected_base = 0x4568;
 payload.param_count = 4;
 sensor.streaming = 1; writes = 0;
 assert(write_control(&state, &control, &sensor) == 0 && writes == 1);
 puts("PASS: eight-field exact writes, live ratio guards, HighResMode changes, seven-field rejection, errors, locks and DPP regression");
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=(
        Path(__file__).resolve().parents[2] / "kernel/realsense/d4xx_main.c"))
    source = parser.parse_args().source.read_text()
    definitions = "\n".join(re.findall(
        r"^#define (?:D500_(?:MINZ|DECIMATION|TEMPORAL)_XU_BASE|"
        r"D500_CAMERA_CID_(?:MINZ|DECIMATION|TEMPORAL)|D500_DPP_XU_VERSION)\b.*",
        source, re.M))
    start = source.index("enum d500_minz_param_index {")
    end = source.index("#define DS5_HWMC_DATA", start)
    setter = block(source, "static int ds5_s_ctrl(")
    code = PREFIX + definitions + "\n" + source[start:end]
    code += (
        "\nstatic int write_control(struct ds5 *state, struct v4l2_ctrl *ctrl,"
        " struct ds5_sensor *sensor) { int ret = -EINVAL;"
        " const struct d500_dpp_ctrl_desc *dpp_desc = d500_dpp_ctrl_desc(ctrl->id);\n"
    )
    code += block(setter, "if (dpp_desc)")
    code += "\nunlock: return ret; }\n" + MAIN
    with tempfile.TemporaryDirectory(prefix="minz-fallback-") as directory:
        test_source = Path(directory) / "test.c"
        executable = Path(directory) / "test"
        test_source.write_text(code)
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
            str(test_source), "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
