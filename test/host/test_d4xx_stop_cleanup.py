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
start = next(
    index
    for index in (
        s.rindex("static int ds5_release_dser_pipe(")
        if "static int ds5_release_dser_pipe(" in s
        else -1,
        s.index("static int ds5_stop_serdes(")
        if "static int ds5_stop_serdes(" in s
        else -1,
        s.index("static int ds5_mux_s_stream(struct v4l2_subdev *sd, int on)\n{"),
    )
    if index >= 0
)
f = s[start : s.index("static int ds5_mux_get_frame_desc(")]
if "static int ds5_release_pipe_for_reconfigure(" in s:
    config_helper_start = s.rfind(
        "#ifdef CONFIG_VIDEO_D4XX_SERDES",
        0,
        s.index("static int ds5_release_pipe_for_reconfigure("),
    )
    config_helper_end = s.index("static int ds5_configure(", config_helper_start)
    f = s[config_helper_start:config_helper_end] + f
if "static int ds5_mux_s_stream_guarded(" in s:
    f += s[s.index("static int ds5_mux_s_stream_guarded("):s.index("static const struct v4l2_subdev_video_ops ds5_sensor_video_ops")]
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
    bool ser_stop_pending;
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
    fw_on, configured, configure_ret, access_ret, allocated;
static bool fail_reconfigure_release;
enum configure_mode {
    CONFIGURE_NORMAL,
    CONFIGURE_REUSE_PIPE,
    CONFIGURE_ATTEMPT_THEN_NO_PIPE,
    CONFIGURE_NO_PIPE,
};
static enum configure_mode configure_mode;
static int ds5_release_pipe_for_reconfigure(struct ds5 *, struct ds5_sensor *);
static int ds5_maintenance_sem, ds5_io_sem;
static void ds5_dfu_read_lock(int *s) { (void)s; }
static void ds5_dfu_read_unlock(int *s) { (void)s; }
static int ds5_check_i2c_access(struct ds5 *s, bool poll) {
    (void)s; (void)poll; return access_ret;
}
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
    int ret;
    configured++;
    if (configure_mode == CONFIGURE_REUSE_PIPE) {
        assert(s->mux.last_set->pipe_id >= 0);
        return 0;
    }
    if (configure_mode == CONFIGURE_ATTEMPT_THEN_NO_PIPE) {
        if (configured == 1) {
            *armed = true;
            s->mux.last_set->pipe_id = 0;
            return -EIO;
        }
        ret = ds5_release_pipe_for_reconfigure(s, s->mux.last_set);
        if (ret < 0) return ret;
        s->mux.last_set->pipe_id = PIPE_NOT_CONFIGURED;
        return -ENOSR;
    }
    if (configure_mode == CONFIGURE_NO_PIPE)
        return -ENOSR;
    *armed = true;
    if (configure_ret) return configure_ret;
    if (fail_reconfigure_release && s->mux.last_set->pipe_id >= 0) {
        ret = ds5_release_pipe_for_reconfigure(s, s->mux.last_set);
        if (ret < 0) return ret;
    }
    s->mux.last_set->pipe_id = 0;
    allocated++;
    return 0;
}
static int ds5_read(struct ds5 *s, u16 reg, u16 *out) {
    (void)s;
    if (scenario == 2)
        return -121;
    /* Stream status reaches IDLE but the config-status read keeps failing. */
    if (scenario == 3 && reg == DS5_DEPTH_CONFIG_STATUS)
        return -121;
    /* A START is acknowledged, but its config status remains unreadable. */
    if (scenario == 4 && reg == DS5_IR_CONFIG_STATUS && fw_on)
        return -121;
    *out = (reg == DS5_DEPTH_STREAM_STATUS || reg == DS5_IR_STREAM_STATUS) ?
        (fw_on ? 2 : 1) : (fw_on ? 1 : 0);
    return 0;
}
static int ds5_write(struct ds5 *s, u16 reg, u16 value) {
    (void)s;
    (void)reg;
    if (scenario == 2 || (scenario == 4 && (value & DS5_STREAM_STOP)) ||
        (scenario == 5 && (value & DS5_STREAM_START)))
        return -121;
    fw_on = !!(value & DS5_STREAM_START);
    return 0;
}
"""
# Macros must precede fake register I/O.
stubs = (
    "\n".join(f"#define {name} {i+10}" for i, name in enumerate(macros)) + "\n" + stubs
)
if "static int ds5_release_pipe_for_reconfigure(" not in s:
    fallback = r"""
static int ds5_release_pipe_for_reconfigure(struct ds5 *s, struct ds5_sensor *sensor) {
    int ret;
    if (sensor->pipe_id < 0) return 0;
    mutex_lock(&serdes_lock__);
    ret = s->dser_ops->release_pipe(s->dser_dev, sensor->pipe_id);
    mutex_unlock(&serdes_lock__);
    return ret;
}
"""
    stubs = stubs.replace("static int ds5_configure(", fallback + "static int ds5_configure(", 1)
tests = r"""

int main(void) {
    struct format fmt = {0};
    struct ds5_sensor sensor = {.streaming = true, .pipe_id = 0, .config = {&fmt}};
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
        sensor.ser_stop_pending = false;
        fw_on = mode == 1 ? 0 : 1;
        int ret = ds5_mux_s_stream(&sd, 0);
        assert(!sensor.streaming);
        assert(sensor.pipe_id == -1 && released == 1);
        if (mode == 2) {
            /* Unacknowledged FW stop stays pending with serializer re-arm. */
            assert(ret == -EAGAIN && stopped == 0 && fw_on);
            assert(dev.depth_streaming && sensor.ser_stop_pending);
            assert(ds5_mux_s_stream(&sd, 0) == -EAGAIN && stopped == 0);
            scenario = 0;
            assert(ds5_mux_s_stream(&sd, 0) == 0 && !fw_on);
        } else {
            assert(ret == 0);
        }
        assert(released == 1 && stopped == 1);
        assert(!dev.depth_streaming && !sensor.ser_stop_pending);
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
    /* Mapping failure leaves serializer cleanup retryable after pipe release. */
    assert(sensor.pipe_id == -1 && sensor.ser_stop_pending);
    mapping_error = 0;
    int release_count = released;
    assert(ds5_mux_s_stream(&sd, 0) == 0 && stopped == 1);
    assert(released == release_count && !sensor.ser_stop_pending);
    dop.get_ser_vc_id = NULL;
    state.gmsl_link = 0;
    state.g_ctx.dst_vc = 0;

    /* Review #714: release succeeds, RX reset fails, second STREAMOFF retries. */
    sensor.pipe_id = 0;
    released = stopped = 0;
    stop_ret = -EREMOTEIO;
    assert(ds5_mux_s_stream(&sd, 0) == -EREMOTEIO);
    assert(sensor.pipe_id == -1 && sensor.ser_stop_pending);
    assert(released == 1 && stopped == 1);
    stop_ret = 0;
    assert(ds5_mux_s_stream(&sd, 0) == 0);
    assert(released == 1 && stopped == 2 && !sensor.ser_stop_pending);
    assert(ds5_mux_s_stream(&sd, 0) == 0 && stopped == 2);

    /* VI can go straight to START after a failed stop: finish cleanup first. */
    sensor.pipe_id = 0;
    stop_ret = -EREMOTEIO;
    assert(ds5_mux_s_stream(&sd, 0) == -EREMOTEIO);
    configured = 0;
    fw_on = 0;
    assert(ds5_mux_s_stream(&sd, 1) == -EREMOTEIO && configured == 0);
    stop_ret = 0;
    assert(ds5_mux_s_stream(&sd, 1) == 0 && configured == 1);
    assert(sensor.streaming && !sensor.ser_stop_pending);
    assert(ds5_mux_s_stream(&sd, 0) == 0);

    /* Failed start rollback retains its original errno and cleanup state. */
    configure_ret = -ENOSR;
    stop_ret = -EREMOTEIO;
    assert(ds5_mux_s_stream(&sd, 1) == -ENOSR);
    assert(!sensor.streaming && sensor.ser_stop_pending);
    configure_ret = stop_ret = 0;
    assert(ds5_mux_s_stream(&sd, 1) == 0);
    assert(ds5_mux_s_stream(&sd, 0) == 0);

    /* No setup attempt and no pipe means there is no serializer to stop. */
    configure_mode = CONFIGURE_NO_PIPE;
    configured = released = stopped = 0;
    sensor.pipe_id = -1;
    assert(ds5_mux_s_stream(&sd, 1) == -ENOSR);
    assert(configured == 1 && released == 0 && stopped == 0);
    assert(!sensor.streaming && !sensor.ser_stop_pending);

    /* A reused pipe needs cleanup even without a new serializer setup attempt. */
    configure_mode = CONFIGURE_REUSE_PIPE;
    configured = released = stopped = allocated = 0;
    sensor.pipe_id = 0;
    scenario = 5;
    assert(ds5_mux_s_stream(&sd, 1) == -EAGAIN);
    assert(configured == 1 && released == 1 && stopped == 1 && allocated == 0);
    assert(sensor.pipe_id == -1 && !sensor.streaming && !sensor.ser_stop_pending);
    scenario = 0;

    /* An earlier setup attempt still needs cleanup after allocation loses the pipe ID. */
    configure_mode = CONFIGURE_ATTEMPT_THEN_NO_PIPE;
    configured = released = stopped = 0;
    stop_ret = -EREMOTEIO;
    assert(ds5_mux_s_stream(&sd, 1) == -ENOSR);
    assert(configured == 2 && released == 1 && stopped == 1);
    assert(sensor.pipe_id == -1 && !sensor.streaming && sensor.ser_stop_pending);
    stop_ret = 0;
    assert(ds5_mux_s_stream(&sd, 0) == 0);
    assert(released == 1 && stopped == 2 && !sensor.ser_stop_pending);

    /* Serializer backends without a stop callback require no deferred stop. */
    sop.stream_stop = NULL;
    configured = released = stopped = 0;
    assert(ds5_mux_s_stream(&sd, 1) == -ENOSR);
    assert(configured == 2 && released == 1 && stopped == 0);
    assert(sensor.pipe_id == -1 && !sensor.ser_stop_pending);
    sop.stream_stop = stop;
    configure_mode = CONFIGURE_NORMAL;

    /* DFU gate releases DES only; defer serializer re-arm until FW is idle. */
    assert(ds5_mux_s_stream(&sd, 1) == 0);
    access_ret = -EBUSY;
    stopped = 0;
    assert(ds5_mux_s_stream_guarded(&sd, 0) == -EBUSY);
    assert(sensor.pipe_id == -1 && sensor.ser_stop_pending && stopped == 0);
    assert(!sensor.streaming && dev.depth_streaming && fw_on);
    assert(ds5_mux_s_stream_guarded(&sd, 1) == -EBUSY && stopped == 0);
    access_ret = 0;
    stop_ret = -EREMOTEIO;
    assert(ds5_mux_s_stream_guarded(&sd, 1) == -EREMOTEIO);
    assert(!fw_on && !sensor.streaming && sensor.ser_stop_pending);
    stop_ret = 0;
    assert(ds5_mux_s_stream_guarded(&sd, 1) == 0);
    assert(sensor.streaming && !sensor.ser_stop_pending);
    assert(ds5_mux_s_stream_guarded(&sd, 0) == 0);

    /* A deferred stop can also finish through STREAMOFF after DFU releases I2C. */
    assert(ds5_mux_s_stream_guarded(&sd, 1) == 0);
    access_ret = -EBUSY;
    stopped = 0;
    assert(ds5_mux_s_stream_guarded(&sd, 0) == -EBUSY && stopped == 0);
    access_ret = 0;
    assert(ds5_mux_s_stream_guarded(&sd, 0) == 0 && stopped == 1);
    assert(!sensor.streaming && !dev.depth_streaming && !sensor.ser_stop_pending);

    /* FW stop failure remains the reported error if SerDes cleanup also fails. */
    assert(ds5_mux_s_stream(&sd, 1) == 0);
    scenario = 2;
    release_ret = -EIO;
    stop_ret = -EREMOTEIO;
    assert(ds5_mux_s_stream(&sd, 0) == -EAGAIN);
    assert(!sensor.streaming && dev.depth_streaming);
    assert(sensor.pipe_id == 0 && sensor.ser_stop_pending);
    scenario = release_ret = stop_ret = 0;
    assert(ds5_mux_s_stream(&sd, 0) == 0);
    assert(sensor.pipe_id == -1 && !sensor.ser_stop_pending && !dev.depth_streaming);

    /* Review #723: a STREAMOFF timeout must not restart FW. The next START
     * completes the pending FW stop and reconfigures the released pipe. */
    assert(ds5_mux_s_stream(&sd, 1) == 0 && fw_on && sensor.pipe_id == 0);
    scenario = 3;
    stopped = 0;
    assert(ds5_mux_s_stream(&sd, 0) == -EAGAIN);
    assert(!fw_on && !sensor.streaming && dev.depth_streaming);
    assert(sensor.pipe_id == -1 && sensor.ser_stop_pending && stopped == 0);
    scenario = 0;
    configured = 0;
    assert(ds5_mux_s_stream(&sd, 1) == 0);
    assert(configured == 1 && stopped == 1 && sensor.pipe_id == 0);
    assert(fw_on && sensor.streaming && dev.depth_streaming && !sensor.ser_stop_pending);
    assert(ds5_mux_s_stream(&sd, 0) == 0 && !fw_on && !dev.depth_streaming);

    /* A failed old-pipe release must abort configuration and retain ownership. */
    sensor.pipe_id = 0;
    release_ret = -EREMOTEIO;
    released = allocated = configured = 0;
    fail_reconfigure_release = true;
    bool armed = false;
    assert(ds5_configure(&state, &armed) == -EREMOTEIO);
    assert(sensor.pipe_id == 0 && released == 1 && allocated == 0);
    fail_reconfigure_release = false;
    release_ret = 0;

    /* Failed Y8 START and its cleanup retries must not reset all DES links. */
    state.is_depth = false;
    state.is_y8 = true;
    fmt.data_type = GMSL_CSI_DT_RGB_888;
    sensor.streaming = sensor.ser_stop_pending = false;
    sensor.pipe_id = -1;
    dev.ir_streaming = false;
    fw_on = 0;
    scenario = 4;
    release_ret = -EIO;
    stop_ret = -EREMOTEIO;
    released = stopped = reset_count = 0;
    assert(ds5_mux_s_stream(&sd, 1) == -EAGAIN);
    assert(fw_on && !sensor.streaming && !dev.ir_streaming);
    assert(sensor.pipe_id == 0 && sensor.ser_stop_pending);
    assert(released == 1 && stopped == 1 && reset_count == 0);

    /* VI can issue another STOP while the failed START's FW state is unknown. */
    assert(ds5_mux_s_stream(&sd, 0) == -EIO);
    assert(sensor.pipe_id == 0 && sensor.ser_stop_pending);
    assert(released == 2 && stopped == 2 && reset_count == 0);

    assert(ds5_mux_s_stream(&sd, 1) == -EIO);
    assert(sensor.pipe_id == 0 && sensor.ser_stop_pending);
    assert(released == 3 && stopped == 3 && reset_count == 0);

    scenario = fw_on = release_ret = stop_ret = 0;
    dop.retrigger_datapath = NULL;
    assert(ds5_mux_s_stream(&sd, 1) == 0);
    assert(sensor.streaming && !sensor.ser_stop_pending && reset_count == 0);
    assert(ds5_mux_s_stream(&sd, 0) == 0);
    assert(!sensor.streaming && !fw_on && reset_count == 1);
    dop.retrigger_datapath = reset;
    state.is_depth = true;
    state.is_y8 = false;

    /* The coarse one-shot reset is explicit and omitted for a pending FW stop. */
    state.is_y8 = true;
    fmt.data_type = GMSL_CSI_DT_RGB_888;
    sensor.pipe_id = 0;
    reset_count = 0;
    assert(ds5_release_dser_pipe(&state, &sensor, false) == 0);
    assert(reset_count == 0 && sensor.pipe_id == -1);
    sensor.pipe_id = 0;
    assert(ds5_release_dser_pipe(&state, &sensor, true) == 0);
    assert(reset_count == 1 && sensor.pipe_id == -1);
    state.is_y8 = false;

    /* Serializer backends without stream_stop retain ordinary pipe cleanup. */
    sop.stream_stop = NULL;
    sensor.pipe_id = 0;
    released = 0;
    assert(ds5_mux_s_stream(&sd, 0) == 0);
    assert(sensor.pipe_id == -1 && released == 1 && !sensor.ser_stop_pending);
    puts("PASS: serializer retry on second stop/next start, failed-start rollback, "
         "DFU-blocked stop/restart, mapped VC retry, optional serializer callback");
    puts("PASS: production d4xx normal stop, FW already idle, inaccessible FW, reset "
         "invalidation, duplicate stop, cleanup errors, stop timeout restart, "
         "reconfigure release failure, failed-start reset gating, reused/missing pipe");
    return 0;
}
"""
if "static int ds5_release_dser_pipe(" not in s:
    tests = tests.replace(
        """    /* The coarse one-shot reset is explicit and omitted for a pending FW stop. */
    state.is_y8 = true;
    fmt.data_type = GMSL_CSI_DT_RGB_888;
    sensor.pipe_id = 0;
    reset_count = 0;
    assert(ds5_release_dser_pipe(&state, &sensor, false) == 0);
    assert(reset_count == 0 && sensor.pipe_id == -1);
    sensor.pipe_id = 0;
    assert(ds5_release_dser_pipe(&state, &sensor, true) == 0);
    assert(reset_count == 1 && sensor.pipe_id == -1);
    state.is_y8 = false;

""",
        "",
    )
with tempfile.TemporaryDirectory() as td:
    p = Path(td) / "test.c"
    p.write_text(stubs + f + tests)
    out = Path(td) / "test"
    subprocess.run(
        ["gcc", "-std=gnu99", "-Wall", "-Wextra", "-Werror", str(p), "-o", str(out)],
        check=True,
    )
    subprocess.run([str(out)], check=True)
