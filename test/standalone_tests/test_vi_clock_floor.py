#!/usr/bin/env python3
"""Test applied VI floor policy with production functions and intrusive lists.

Usage: python3 test_vi_clock_floor.py --source-root /path/to/patched/nvidia-oot

Mocks DT and clock providers. Exercises device identity, real list traversal,
private-wrapper offsets, shared clock votes, provider errors and public copies.
This host test does not replace target clock/readback or streaming validation.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
import textwrap


def definition(source, name, structure=False):
    """Extract a definition, rejecting prototypes rather than starting at one."""
    pattern = (r'^struct\s+' + re.escape(name) + r'\s*\{' if structure else
               r'^(?:static\s+)?int\s+' + re.escape(name) + r'\s*\([^;{}]*\)\s*\{')
    matches = list(re.finditer(pattern, source, re.M))
    if len(matches) != 1:
        raise ValueError(f'Expected one definition of {name}, found {len(matches)}')
    start = matches[0].start()
    # These production functions have no brace characters in string literals.
    end = source.index('{', start) + 1
    depth = 1
    while depth and end < len(source):
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    if depth:
        raise ValueError(f'Unterminated definition: {name}')
    return source[start:end] + (';\n' if structure else '\n')


PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <stddef.h>
#include <string.h>
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
typedef uint64_t u64;
typedef uint32_t u32;
struct list_head { struct list_head *next, *prev; };
static void init_list(struct list_head *head) { head->next = head->prev = head; }
static void add_tail(struct list_head *node, struct list_head *head) {
    node->next = head;
    node->prev = head->prev;
    head->prev->next = node;
    head->prev = node;
}
#define list_for_each_entry(pos, head, member) \
    for (pos = container_of((head)->next, __typeof__(*pos), member); \
         &(pos)->member != (head); \
         pos = container_of((pos)->member.next, __typeof__(*pos), member))
enum { HWTYPE_NONE, HWTYPE_CSI, HWTYPE_SLVSEC, HWTYPE_VI, HWTYPE_ISPA, HWTYPE_ISPB };
#define SENSORTYPE_SLVSEC 4
#define DEFAULT_PG_CLK_RATE 100000000
#define max(a,b) ((a) > (b) ? (a) : (b))
#define dev_warn_ratelimited(...) ((void)0)
struct platform_device { int dev; };
struct tegra_camera_dev_info;
struct tegra_camera_dev_ops { int (*set_rate)(struct tegra_camera_dev_info *, unsigned long); };
struct tegra_camera_dev_info {
    u32 overhead, bus_width, lane_num, ppc, hw_type;
    u64 lane_speed, pg_clk_rate;
    bool use_max, stream_on;
    struct platform_device *pdev;
    struct tegra_camera_dev_ops *ops;
    struct list_head device_node;
};
struct tegra_camera_info {
    u64 active_pixel_rate, phy_pixel_rate;
    u32 max_pixel_depth, ppc_divider, num_active_streams, sensor_type;
    bool pg_mode;
    struct list_head device_list;
};
struct host_vi5 { int clk; };
struct nvhost_device_data { void *private_data; };
static struct host_vi5 vi;
static struct nvhost_device_data pdata = { &vi };
#define platform_get_drvdata(p) ((void)(p), &pdata)
static unsigned long applied;
static int rate_error;
static int clk_set_rate(int clock, unsigned long rate) {
    (void)clock;
    applied = rate;
    return rate_error;
}
static unsigned long d5xx_vi_min_rate = 550400000;
struct device_node { const char *compatible; };
static bool of_device_is_compatible(const struct device_node *node, const char *name) {
    return node && !strcmp(node->compatible, name);
}
static int camera_device_register(struct tegra_camera_dev_info *info, void *priv, bool eligible) {
    (void)info;
    (void)priv;
    return eligible;
}
'''
TEST = r'''
int main(void) {
    struct tegra_camera_dev_ops ops = { vi5_set_rate };
    struct tegra_camera_dev_info vi_device = { .hw_type=HWTYPE_VI, .ops=&ops, .ppc=1 };
    struct tegra_camera_device cameras[3] = {0};
    struct tegra_camera_info info = { .active_pixel_rate=100000000, .ppc_divider=1 };
    struct device_node d4xx = { "intel,d4xx" }, d5xx = { "realsense,d5xx" }, other = { "other,camera" };
    unsigned long baseline;
    init_list(&info.device_list);
    assert(tegra_camera_device_register_sensor(NULL, NULL, NULL) == 0);
    assert(tegra_camera_device_register_sensor(NULL, NULL, &other) == 0);
    assert(tegra_camera_device_register_sensor(NULL, NULL, &d4xx) == 0);
    assert(tegra_camera_device_register_sensor(NULL, NULL, &d5xx) == 1);
#define CHECK() assert(calculate_and_set_device_clock(&info, &vi_device) == 0)
    CHECK(); assert(applied == 0);
    info.num_active_streams = 1;
    CHECK(); baseline = applied; assert(baseline == 100000001);
    /* Non-array order makes a stride-based mock or traversal fail. */
    add_tail(&cameras[2].info.device_node, &info.device_list);
    add_tail(&cameras[0].info.device_node, &info.device_list);
    add_tail(&cameras[1].info.device_node, &info.device_list);
    cameras[0].info.stream_on = true;
    CHECK(); assert(applied == baseline);
    cameras[0].needs_d5xx_floor = true;
    CHECK(); assert(applied == 550400000);
    struct tegra_camera_dev_info copy = vi_device;
    assert(calculate_and_set_device_clock(&info, &copy) == 0 && applied == 550400000);
    cameras[1].needs_d5xx_floor = cameras[1].info.stream_on = true;
    info.num_active_streams = 2;
    CHECK(); assert(applied == 550400000);
    cameras[0].info.stream_on = false;
    info.num_active_streams = 1;
    CHECK(); assert(applied == 550400000);
    cameras[1].info.stream_on = false;
    cameras[2].info.stream_on = true;
    CHECK(); assert(applied == baseline);
    info.num_active_streams = 0;
    cameras[2].info.stream_on = false;
    CHECK(); assert(applied == 0);
    info.num_active_streams = 1;
    cameras[0].info.stream_on = true;
    d5xx_vi_min_rate = 0;
    CHECK(); assert(applied == baseline);
    d5xx_vi_min_rate = 550400000;
    info.pg_mode = true;
    CHECK(); assert(applied == 550400000);
    info.pg_mode = false;
    info.active_pixel_rate = 700000000;
    CHECK(); assert(applied == 700000001);
    info.active_pixel_rate = 100000000;
    rate_error = -EIO;
    assert(calculate_and_set_device_clock(&info, &vi_device) == -EIO);
    puts("PASS: identities, empty/mixed lists, public copy, stop votes, opt-out, PG, throughput and errors");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, required=True)
    args = parser.parse_args()
    camera = (args.source_root / 'drivers/video/tegra/camera/tegra_camera_platform.c').read_text()
    vi = (args.source_root / 'drivers/video/tegra/host/vi/vi5.c').read_text()
    policy = definition(camera, 'calculate_and_set_device_clock')
    prototype = 'static int calculate_and_set_device_clock(struct tegra_camera_info *info, struct tegra_camera_dev_info *cdev);\n'
    assert definition(prototype + camera, 'calculate_and_set_device_clock') == policy
    wrapper = definition(camera, 'tegra_camera_device', structure=True)
    functions = (definition(camera, 'tegra_camera_device_register_sensor') +
                 definition(vi, 'vi5_set_rate') + policy)
    # container_of supports nonzero offsets; test that instead of assuming offset 0.
    shifted = wrapper.replace('struct tegra_camera_device {',
                              'struct tegra_camera_device {\n    unsigned long offset_probe;')
    with tempfile.TemporaryDirectory(prefix='vi-floor-test-') as tmp:
        for name, layout in [('production', wrapper), ('nonzero_offset', shifted)]:
            source = Path(tmp) / (name + '.c')
            source.write_text(textwrap.dedent(PREFIX) + layout + functions + textwrap.dedent(TEST))
            executable = source.with_suffix('')
            subprocess.run(['cc', '-std=gnu11', '-fsanitize=address,undefined',
                            '-fno-omit-frame-pointer', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-but-set-variable', str(source), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True)
            print(f'Validated {name} layout')


if __name__ == '__main__':
    main()
