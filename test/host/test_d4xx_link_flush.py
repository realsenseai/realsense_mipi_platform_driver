#!/usr/bin/env python3
"""Compile the production cold-start link flush against a fake deserializer.

RSDEV-16420: a sibling stream must not reach its FW start ahead of the cold
link flush, on builds with and without the ds5_io_sem hold. Camera transfers
wait out the link-down under that hold (RSDSO-21053), which ends once the
camera answers again rather than after the deserializer's full settle delay.

Requires gcc and pthreads. Optional argument: an alternate d4xx_main.c for
before/after checks.
"""
from pathlib import Path
import re, subprocess, tempfile, sys

root = Path(__file__).resolve().parents[2]
s = Path(
    sys.argv[1] if len(sys.argv) > 1 else root / "kernel/realsense/d4xx_main.c"
).read_text()


def function(name):
    start = s.index(f"static {name}(")
    return s[start : s.index("\n}\n", start) + 3]


defines = "\n".join(
    re.findall(r"^#define (?:DS5_LINK_FLUSH_\w+|DS5_FW_VERSION)\s.*$", s, re.M)
)
code = "\n".join(
    [
        function("bool ds5_reset_oneshot_link"),
        function("void ds5_wait_link_up"),
        function("void ds5_flush_idle_link"),
    ]
)

harness = r"""
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

""" + defines + r"""
#define READ_ONCE(x) (x)
#define xchg(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define EIO 5
#define EBUSY 16
#define EREMOTEIO 121
typedef unsigned short u16;
typedef unsigned int u32;
typedef long long s64;
typedef long long ktime_t;

struct device { int unused; };
struct i2c_client { struct device dev; };
struct dser_interface {
    void (*reset_oneshot_link)(struct device *, u32);
    int (*trigger_oneshot_link)(struct device *, u32);
};
struct ds5_dev {
    pthread_mutex_t lock;
    bool link_flush_pending;
    pthread_mutex_t link_flush_lock;
};
struct ds5 {
    struct ds5_dev *ds5_dev;
    const struct dser_interface *dser_ops;
    struct device *dser_dev;
    u32 gmsl_link;
    struct i2c_client *client;
    int *regmap;
};

static struct ds5_dev dev = {
    PTHREAD_MUTEX_INITIALIZER, false, PTHREAD_MUTEX_INITIALIZER };
static pthread_rwlock_t ds5_io_sem = PTHREAD_RWLOCK_INITIALIZER;
static pthread_mutex_t ev_lock = PTHREAD_MUTEX_INITIALIZER;
static bool write_held;
static int write_locks, reads, warns, infos, dev_lock_taken;
static char events[64];
/* Fake clock in us; the link is down from the trigger until link_up_us. */
static long long now_us, link_down_us, link_up_us, held_us, hold_start_us;
static int trigger_ret, access_ret;
static void (*during_flush)(void);

static void event(char e) {
    pthread_mutex_lock(&ev_lock);
    events[strlen(events)] = e;
    pthread_mutex_unlock(&ev_lock);
}

static void mutex_lock(pthread_mutex_t *m) {
    /* Control paths hold ds5_dev->lock across camera i2c, which waits on
     * the ds5_io_sem hold: taking it here could deadlock. */
    if (m == &dev.lock)
        dev_lock_taken++;
    pthread_mutex_lock(m);
}
static void mutex_unlock(pthread_mutex_t *m) { pthread_mutex_unlock(m); }

#if HOLD
static void ds5_dfu_write_lock(pthread_rwlock_t *sem) {
    pthread_rwlock_wrlock(sem);
    write_held = true;
    write_locks++;
    hold_start_us = now_us;
}
static void ds5_dfu_write_unlock(pthread_rwlock_t *sem) {
    write_held = false;
    if (now_us - hold_start_us > held_us)
        held_us = now_us - hold_start_us;
    pthread_rwlock_unlock(sem);
}
#define ds5_dfu_read_lock(s) pthread_rwlock_rdlock(s)
#define ds5_dfu_read_unlock(s) pthread_rwlock_unlock(s)
#else
/* Non-Tegra and >= 6.8 builds compile the hold out. */
#define ds5_dfu_write_lock(s) do { } while (0)
#define ds5_dfu_write_unlock(s) do { } while (0)
#define ds5_dfu_read_lock(s) do { } while (0)
#define ds5_dfu_read_unlock(s) do { } while (0)
#endif

static void msleep(unsigned ms) { now_us += ms * 1000LL; }
static void usleep_range(unsigned lo, unsigned hi) { (void)hi; now_us += lo; }
static ktime_t ktime_get(void) { return now_us * 1000; }
static s64 ktime_ms_delta(ktime_t a, ktime_t b) { return (a - b) / 1000000; }
static int ds5_check_i2c_access(struct ds5 *state, bool internal) {
    (void)state; (void)internal;
    return access_ret;
}
static int regmap_raw_read(int *map, unsigned reg, void *val, unsigned len) {
    (void)map; (void)val;
    assert(write_held == HOLD && reg == DS5_FW_VERSION && len == 2);
    reads++;
    if (now_us >= link_down_us && now_us < link_up_us)
        return -EREMOTEIO;
    event('U');
    return 0;
}
#define dev_info(d, ...) ((void)(d), infos++)
#define dev_warn(d, ...) ((void)(d), warns++)

static void flush_op(struct device *d, u32 link) {
    (void)d;
    assert(link == 1 && write_held == HOLD);
    event('F');
    if (during_flush)
        during_flush();
    now_us += 100000;
    event('U');
}
static int trigger_op(struct device *d, u32 link) {
    (void)d;
    assert(link == 1 && write_held == HOLD);
    event('F');
    if (trigger_ret)
        return trigger_ret;
    link_down_us = now_us + 600;
    if (during_flush)
        during_flush();
    return 0;
}

static const struct dser_interface max96712 = {
    .reset_oneshot_link = flush_op, .trigger_oneshot_link = trigger_op };
static const struct dser_interface full_only = { .reset_oneshot_link = flush_op };
static const struct dser_interface max9296 = { 0 };
static struct device dser;
static struct i2c_client client;
static int regmap;
static struct ds5 depth = { &dev, &max96712, &dser, 1, &client, &regmap };
static struct ds5 rgb = { &dev, &max96712, &dser, 1, &client, &regmap };

""" + code + r"""

/* A stream's tail after ds5_configure(): the flush, then its FW start. */
static void start_stream(struct ds5 *state, char start) {
    ds5_flush_idle_link(state);
    ds5_dfu_read_lock(&ds5_io_sem);
    event(start);
    ds5_dfu_read_unlock(&ds5_io_sem);
}

static void *rgb_thread(void *arg) {
    (void)arg;
    start_stream(&rgb, 'R');
    return NULL;
}

/* RGB finishes its configure while depth's flush has the link down; give
 * it real time to run ahead if nothing stops it. */
static pthread_t rgb_tid;
static void rgb_arrives(void) {
    pthread_create(&rgb_tid, NULL, rgb_thread, NULL);
    usleep(30000);
}

static void reset(bool pending, const struct dser_interface *ops, long long up_ms) {
    memset(events, 0, sizeof(events));
    write_held = false;
    write_locks = reads = warns = infos = 0;
    during_flush = NULL;
    dev.link_flush_pending = pending;
    depth.dser_ops = rgb.dser_ops = ops;
    now_us = held_us = 0;
    link_down_us = 1LL << 60;
    link_up_us = up_ms * 1000;
    trigger_ret = access_ret = 0;
}

static void race(const struct dser_interface *ops) {
    reset(true, ops, 43);
    during_flush = rgb_arrives;
    start_stream(&depth, 'D');
    pthread_join(rgb_tid, NULL);
    printf("HOLD=%d sibling race: %s, hold %lld us, %d polls\n",
           HOLD, events, held_us, reads);
    /* The sibling's start never reaches the FW before the link is back. */
    assert(strcmp(events, "FURD") == 0 || strcmp(events, "FUDR") == 0);
    assert(!dev.link_flush_pending && warns == 0);
}

int main(void) {
    /* The hold ends when the camera answers again (43 ms), not after the
     * 100 ms settle. */
    race(&max96712);
    assert(!HOLD || (held_us >= 43000 && held_us < 46000));

    /* A deserializer without the trigger op keeps the whole op under the
     * hold. */
    race(&full_only);
    assert(!HOLD || held_us == 100000);
    assert(reads == 0);

    /* No polling inside the minimum link-down time. */
    reset(true, &max96712, 1);
    start_stream(&depth, 'D');
    assert(now_us == DS5_LINK_FLUSH_MIN_DOWN_MS * 1000 && reads == 1);

    /* A camera that never answers bounds the wait to the old settle. */
    reset(true, &max96712, 1000);
    start_stream(&depth, 'D');
    assert(now_us >= DS5_LINK_FLUSH_MAX_DOWN_MS * 1000 &&
           now_us < (DS5_LINK_FLUSH_MAX_DOWN_MS + 3) * 1000 && warns == 1);

    /* i2c blocked (DFU, fault): no probe, the full settle, and a log line. */
    reset(true, &max96712, 43);
    access_ret = -EBUSY;
    start_stream(&depth, 'D');
    assert(now_us == DS5_LINK_FLUSH_MAX_DOWN_MS * 1000 && reads == 0 && infos == 1);

    /* A failed trigger never dropped the link: no wait. */
    reset(true, &max96712, 43);
    trigger_ret = -EIO;
    start_stream(&depth, 'D');
    assert(strcmp(events, "FD") == 0 && now_us == 0 && reads == 0 && warns == 1);

    /* Warm start: nothing pending, so no hold stalls the siblings. */
    reset(false, &max96712, 43);
    start_stream(&depth, 'D');
    assert(strcmp(events, "D") == 0 && now_us == 0);

    /* max9296 wires no op: no hold, no flush. */
    reset(true, &max9296, 43);
    start_stream(&depth, 'D');
    assert(strcmp(events, "D") == 0 && write_locks == 0);

#if HOLD
    /* The post-HW-reset and calibration one-shot keeps its full hold. */
    reset(true, &max96712, 43);
    assert(ds5_reset_oneshot_link(&depth) && held_us == 100000 && reads == 0);
#endif

    assert(dev_lock_taken == 0);
    puts("PASS");
    return 0;
}
"""

rc = 0
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / "t.c"
    src.write_text(harness)
    for hold in (1, 0):
        exe = Path(tmp) / f"t{hold}"
        subprocess.run(
            ["gcc", "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function",
             "-Wno-unused-variable",
             f"-DHOLD={hold}", "-pthread", "-o", str(exe), str(src)],
            check=True,
        )
        rc |= subprocess.run([str(exe)]).returncode
sys.exit(rc)
