#!/usr/bin/env python3
"""Compile production stream-off code against fake hardware, then inject failures.

Requires gcc. Optional argument: an alternate d4xx_main.c for before/after checks.
"""
from pathlib import Path
import subprocess, tempfile, sys

root = Path(__file__).resolve().parents[2]
s = Path(
    sys.argv[1] if len(sys.argv) > 1 else root / "kernel/realsense/d4xx_main.c"
).read_text()
start = (
    s.index("static int ds5_stop_serdes(")
    if "static int ds5_stop_serdes(" in s
    else s.index("static int ds5_mux_s_stream(struct v4l2_subdev *sd, int on)\n{")
)
f = s[start : s.index("static int ds5_mux_get_frame_desc(")]
macros = [
    "DS5_DEPTH_CONFIG_STATUS",
    "DS5_DEPTH_STREAM_STATUS",
    "DS5_RGB_CONFIG_STATUS",
    "DS5_RGB_STREAM_STATUS",
    "DS5_IR_CONFIG_STATUS",
    "DS5_IR_STREAM_STATUS",
    "DS5_IMU_CONFIG_STATUS",
    "DS5_IMU_STREAM_STATUS",
]
stubs = r"""

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint16_t u16;
typedef uint32_t u32;
#define CONFIG_VIDEO_D4XX_SERDES
#define CONFIG_TEGRA_CAMERA_PLATFORM
#define PIPE_NOT_CONFIGURED -1
#define DS5_MAX_STREAMS 4
#define GMSL_CSI_DT_RGB_888 0x24
#define DS5_STREAM_DEPTH 0
#define DS5_STREAM_RGB 1
#define DS5_STREAM_IR 2
#define DS5_STREAM_IMU 3
#define DS5_STREAM_START 0x100
#define DS5_STREAM_STOP 0x200
#define DS5_START_STOP_STREAM 0x400
#define DS5_STREAM_STREAMING 2
#define DS5_STREAM_IDLE 1
#define DS5_STATUS_STREAMING 1
#define DS5_STATUS_INVALID_DT 2
#define DS5_STATUS_INVALID_RES 4
#define DS5_STATUS_INVALID_FPS 8
#define MAX_DS5_CONFIG_RETRIES 1
#define DS5_START_MAX_TIME 3
#define DS5_START_POLL_TIME 1
#define dev_dbg(...) ((void)0)
#define dev_warn(...) ((void)0)
#define dev_info(...) ((void)0)
#define mutex_lock(p) ((void)(p))
#define mutex_unlock(p) ((void)(p))
#define atomic_read(p) (*(p))
#define time_before(a, b) ((a) < (b))
#define msecs_to_jiffies(x) (x)
#define jiffies_to_msecs(x) (x)
static unsigned long jiffies;
static void msleep_range(unsigned n) { jiffies += n + 1; }
struct format {
    int data_type;
};
struct ds5_sensor {
    bool streaming;
    int pipe_id;
    struct {
        struct format *format;
    } config;
};
struct device {};
struct ds5_dev {
    int lock, gen;
    bool depth_streaming, rgb_streaming, ir_streaming, imu_streaming, link_flush_pending;
};
struct ds5 {
    struct {
        struct ds5_sensor *last_set;
    } mux;
    struct ds5_dev *ds5_dev;
    int reset_ref_ds5;
    int gmsl_link;
    bool is_depth, is_rgb, is_y8, is_imu;
    struct {
        unsigned dst_vc;
    } g_ctx;
    struct {
        struct ds5_sensor sensor;
    } ir;
    struct device *dser_dev, *ser_dev;
    struct dser_ops *dser_ops;
    struct ser_ops *ser_ops;
};
struct v4l2_subdev {
    struct ds5 *owner;
};
#define container_of(p, t, m) ((p)->owner)
struct dser_ops {
    int (*get_ser_vc_id)(struct device *, unsigned, unsigned);
    int (*release_pipe)(struct device *, int);
    void (*reset_oneshot)(struct device *);
    void (*retrigger_datapath)(struct device *);
};
struct ser_ops {
    int (*stream_stop)(struct device *, unsigned);
};
static int mapping_error;
static int map_vc(struct device *d, unsigned link, unsigned vc) {
    (void)d;
    assert(link == 1 && vc == 4);
    return mapping_error;
}
static int serdes_lock__, scenario, release_ret, stop_ret, released, stopped, reset_count,
    fw_on;
static int release(struct device *d, int p) {
    (void)d;
    assert(p == 0);
    released++;
    return release_ret;
}
static int stop(struct device *d, unsigned vc) {
    (void)d;
    assert(vc == 0);
    stopped++;
    return stop_ret;
}
static void reset(struct device *d) {
    (void)d;
    reset_count++;
}
static int *ds5_get_reset_gen(struct ds5 *s) { return &s->ds5_dev->gen; }
static void ds5_invalidate_sensor(struct ds5 *s, struct ds5_sensor *n) {
    (void)s;
    (void)n;
}
static void ds5_config_cache_clear(struct ds5_sensor *s) { (void)s; }
static void ds5_flush_idle_link(struct ds5 *s) { (void)s; }
static int ds5_configure(struct ds5 *s, bool *armed) {
    (void)s;
    *armed = true;
    return 0;
}
static int ds5_read(struct ds5 *s, u16 reg, u16 *out) {
    (void)s;
    if (scenario == 2)
        return -121;
    *out = reg == DS5_DEPTH_STREAM_STATUS ? (fw_on ? 2 : 1) : (fw_on ? 1 : 0);
    return 0;
}
static int ds5_write(struct ds5 *s, u16 reg, u16 value) {
    (void)s;
    (void)reg;
    if (scenario == 2)
        return -121;
    fw_on = !!(value & DS5_STREAM_START);
    return 0;
}
"""
# Macros must precede fake register I/O.
stubs = (
    "\n".join(f"#define {name} {i+10}" for i, name in enumerate(macros)) + "\n" + stubs
)
tests = r"""

int main(void) {
    struct format fmt = {0};
    struct ds5_sensor sensor = {true, 0, {&fmt}};
    struct ds5_dev dev = {0};
    struct dser_ops dop = {0, release, reset, reset};
    struct ser_ops sop = {stop};
    struct ds5 state = {0};
    struct v4l2_subdev sd = {&state};
    state.mux.last_set = &sensor;
    state.ds5_dev = &dev;
    state.is_depth = true;
    state.dser_ops = &dop;
    state.ser_ops = &sop;
    state.ir.sensor.config.format = &fmt;
    for (int mode = 0; mode < 5; mode++) {
        scenario = mode == 2 ? 2 : 0;
        jiffies = 0;
        released = stopped = reset_count = release_ret = stop_ret = 0;
        state.reset_ref_ds5 = 0;
        dev.gen = mode == 3 ? 1 : 0;
        sensor.streaming = mode != 4;
        dev.depth_streaming = sensor.streaming && mode != 3;
        sensor.pipe_id = 0;
        fw_on = mode == 1 ? 0 : 1;
        int ret = ds5_mux_s_stream(&sd, 0);
        assert(ret == (mode == 2 ? -EAGAIN : 0));
        assert(!sensor.streaming);
        assert(sensor.pipe_id == -1);
        assert(released == 1 && stopped == 1);
        assert(!dev.depth_streaming);
        assert(ds5_mux_s_stream(&sd, 0) == 0);
        assert(released == 1 && stopped == 1);
    }
    /* A failed release retains the resource ID and its error while still
     * stopping the serializer; a second stop can retry the pending release. */
    scenario = 0;
    sensor.streaming = false;
    sensor.pipe_id = 0;
    release_ret = -EIO;
    stop_ret = -121;
    released = stopped = 0;
    assert(ds5_mux_s_stream(&sd, 0) == -EIO);
    assert(sensor.pipe_id == 0 && released == 1 && stopped == 1);
    release_ret = stop_ret = 0;
    assert(ds5_mux_s_stream(&sd, 0) == 0);
    assert(sensor.pipe_id == -1 && released == 2 && stopped == 2);
    /* Propagate serializer I/O failure, do not synthesize success. */
    sensor.pipe_id = 0;
    stop_ret = -121;
    assert(ds5_mux_s_stream(&sd, 0) == -121);
    dop.get_ser_vc_id = map_vc;
    state.gmsl_link = 1;
    state.g_ctx.dst_vc = 4;
    sensor.pipe_id = 0;
    stop_ret = 0;
    stopped = 0;
    mapping_error = 0;
    assert(ds5_mux_s_stream(&sd, 0) == 0 && stopped == 1);
    sensor.pipe_id = 0;
    stopped = 0;
    mapping_error = -EINVAL;
    assert(ds5_mux_s_stream(&sd, 0) == -EINVAL && stopped == 0);
    puts("PASS: production d4xx normal stop, FW already idle, inaccessible FW, reset "
         "invalidation, duplicate stop, cleanup error propagation");
    return 0;
}
"""
with tempfile.TemporaryDirectory() as td:
    p = Path(td) / "test.c"
    p.write_text(stubs + f + tests)
    out = Path(td) / "test"
    subprocess.run(
        ["gcc", "-std=gnu99", "-Wall", "-Wextra", "-Werror", str(p), "-o", str(out)],
        check=True,
    )
    subprocess.run([str(out)], check=True)
