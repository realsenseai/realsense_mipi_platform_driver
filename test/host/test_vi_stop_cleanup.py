#!/usr/bin/env python3
"""Fault-inject the production VI function extracted from a prepared BSP.

Requires gcc and sources_6.2.1, or pass the path to a patched channel.c.
"""
from pathlib import Path
import subprocess, tempfile, sys

root = Path(__file__).resolve().parents[2]
s = (
    Path(sys.argv[1])
    if len(sys.argv) > 1
    else root
    / "sources_6.2.1/nvidia-oot/drivers/media/platform/tegra/camera/vi/channel.c"
).read_text()
f = s[
    s.index("int tegra_channel_set_stream(") : s.index(
        "EXPORT_SYMBOL(tegra_channel_set_stream);"
    )
]
stubs = r"""

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#define ENOIOCTLCMD 515
struct v4l2_subdev {
    const char *name;
    int id;
};
struct deskew {
    int deskew_lanes;
};
struct vi {
    void *dev;
};
struct tegra_channel {
    int is_streaming;
    int num_subdevs;
    bool bypass, pg_mode;
    struct v4l2_subdev *subdev[2];
    struct deskew *deskew_ctx;
    struct vi *vi;
};
static int calls[10], count, fail_id, fail_errno, cleanup_errno, clocks,
    stop_fail_id = -1;
#define atomic_read(p) (*(p))
#define atomic_set(p, v) (*(p) = (v))
#define trace_tegra_channel_set_stream(n, on) ((void)0)
#define dev_warn(...) ((void)0)
#define spec_bar() ((void)0)
static void tegra_camera_update_clknbw(struct tegra_channel *c, bool on) {
    (void)c;
    clocks += on ? 1 : -1;
}
static int nvcsi_deskew_apply_check(struct deskew *d) {
    (void)d;
    return 0;
}
static int invoke(struct v4l2_subdev *s, bool on) {
    calls[count++] = (on ? 10 : 20) + s->id;
    return on ? (s->id == fail_id ? fail_errno : 0)
              : ((stop_fail_id < 0 || s->id == stop_fail_id) ? cleanup_errno : 0);
}
#define v4l2_subdev_call(s, v, f, on) invoke(s, on)
"""
tests = r"""

int main(void) {
    struct v4l2_subdev a = {"CSI", 0}, b = {"sensor", 1};
    struct deskew d = {0};
    struct vi v = {0};
    struct tegra_channel c = {0, 2, false, false, {&a, &b}, &d, &v};
    for (int id = 0; id < 2; id++)
        for (int stoperror = 0; stoperror < 2; stoperror++) {
            count = clocks = 0;
            fail_id = id;
            fail_errno = -121;
            cleanup_errno = stoperror ? -5 : 0;
            c.is_streaming = 0;
            assert(tegra_channel_set_stream(&c, true) == -121);
            assert(!c.is_streaming && clocks == 0);
            if (id == 0) {
                assert(count == 2 && calls[0] == 10 && calls[1] == 20);
            } else {
                assert(count == 4 && calls[0] == 10 && calls[1] == 11 && calls[2] == 21 &&
                       calls[3] == 20);
            }
        }
    count = clocks = 0;
    fail_id = 1;
    fail_errno = -ENOIOCTLCMD;
    cleanup_errno = 0;
    c.is_streaming = 0;
    assert(tegra_channel_set_stream(&c, true) == 0 && c.is_streaming == 1 && clocks == 1);
    assert(tegra_channel_set_stream(&c, true) == 0 && count == 2 && clocks == 1);
    assert(tegra_channel_set_stream(&c, false) == 0 && c.is_streaming == 0 &&
           clocks == 0);
    assert(count == 4 && calls[2] == 21 && calls[3] == 20);
    for (int id = 0; id < 2; id++) {
        count = 0;
        clocks = 1;
        cleanup_errno = -121;
        stop_fail_id = id;
        c.is_streaming = 1;
        assert(tegra_channel_set_stream(&c, false) == -121);
        assert(!c.is_streaming && clocks == 0 && count == 2);
        cleanup_errno = 0;
        fail_id = -1;
        count = 0;
        assert(tegra_channel_set_stream(&c, true) == 0);
        assert(c.is_streaming && clocks == 1 && count == 2);
        assert(tegra_channel_set_stream(&c, false) == 0 && clocks == 0);
    }
    puts("PASS: production VI rollback, both failed-start positions, cleanup errors, "
         "original errno, clock balance, failed stop followed by real restart, normal "
         "start/stop and duplicate guard");
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
