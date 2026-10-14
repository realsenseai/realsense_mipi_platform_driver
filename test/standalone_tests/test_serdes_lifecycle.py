#!/usr/bin/env python3
# Copyright (c) 2026 RealSense, Inc.
# SPDX-License-Identifier: GPL-2.0
"""Compile production lifecycle functions with fault-counting dependencies.

SERDES_MATRIX_DIR supplies MAX96712 sources replayed from NVIDIA tags.
This checks resource accounting, not kernel concurrency or hardware streaming.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def body(source, name):
    match = re.search(r'\b' + name + r'\([^;{}]*?\)\s*(?:#endif[^\n]*\n\s*)?\{', source)
    if not match:
        raise AssertionError(name)
    start = source.index('{', match.start())
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
struct mutex { bool locked; };
static void mutex_lock(struct mutex *m) { assert(!m->locked); m->locked = true; }
static void mutex_unlock(struct mutex *m) { assert(m->locked); m->locked = false; }
static inline void mutex_destroy(struct mutex *m) { assert(!m->locked); }
struct device { void *data; };
struct i2c_client { struct device dev; };
#define dev_get_drvdata(d) ((d)->data)
#define dev_warn(d, ...) ((void)(d))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define NV_I2C_DRIVER_STRUCT_REMOVE_RETURN_TYPE_INT 1
#define KERNEL_VERSION(a,b,c) (((a) << 16) | ((b) << 8) | (c))
#define LINUX_VERSION_CODE KERNEL_VERSION(5, 10, 0)
'''


class LifecycleTests(unittest.TestCase):
    def compile_run(self, code):
        compiler = shutil.which(os.environ.get('CC', 'cc'))
        if not compiler:
            self.skipTest('C compiler unavailable')
        with tempfile.TemporaryDirectory(prefix='serdes-lifecycle-') as tmp:
            source = Path(tmp) / 'test.c'
            binary = Path(tmp) / 'test'
            source.write_text(PRELUDE + code)
            result = subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra',
                                     '-Werror', str(source), '-o', str(binary)],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)], capture_output=True,
                                    text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_probe_rollback_owns_only_acquired_resources(self):
        source = (ROOT / 'kernel/realsense/d4xx.c').read_text()
        cleanup = body(source, 'ds5_serdes_probe_cleanup')
        self.compile_run(r'''
struct ds5;
struct ds5_dev {
    struct mutex lock;
    struct ds5 *ds5_primary;
    bool serdes_setup_complete;
};
struct ops {
    int (*reset_control)();
    int (*sdev_unpair)(struct device *, struct device *);
    int (*sdev_unregister)(struct device *, struct device *);
    void (*power_off)(struct device *);
};
struct ds5 {
    struct i2c_client *client;
    struct device *ser_dev, *dser_dev;
    struct { struct device *s_dev; } g_ctx;
    struct ds5_dev *ds5_dev;
    struct ops *ser_ops, *dser_ops;
    bool ser_primary, ser_paired, dser_registered, dser_powered;
};
static struct mutex serdes_lock__;
static int resets, unpairs, unregisters, powers;
static int reset() { resets++; return 0; }
static int unpair(struct device *d, struct device *s)
{ (void)d; (void)s; unpairs++; return 0; }
static int unregister_source(struct device *d, struct device *s)
{ (void)d; (void)s; unregisters++; return 0; }
static void power_off(struct device *d) { (void)d; powers++; }
static bool ds5_release_slot(struct ds5 *state)
{
    assert(!serdes_lock__.locked);
    if (state->ds5_dev && state->ds5_dev->ds5_primary == state) {
        state->ds5_dev->ds5_primary = NULL;
        return true;
    }
    return false;
}
static void cleanup(struct ds5 *state)
''' + cleanup + r'''
int main(void)
{
    struct i2c_client client = {0};
    struct ops ops = {reset, unpair, unregister_source, power_off};
    struct ds5_dev slot = {0};
    struct ds5 state = {.client = &client, .ds5_dev = &slot,
                       .ser_ops = &ops, .dser_ops = &ops};
    cleanup(&state); /* A sibling owns none of the shared acquisitions. */
    assert(resets == 0 && unpairs == 0 && unregisters == 0 && powers == 0);
    state.ser_primary = true;
    for (int stage = 0; stage < 4; stage++) {
        resets = unpairs = unregisters = powers = 0;
        slot.ds5_primary = &state;
        slot.serdes_setup_complete = true;
        state.ser_paired = stage >= 1;
        state.dser_registered = stage >= 2;
        state.dser_powered = stage >= 3;
        cleanup(&state);
        assert(unpairs == (stage >= 1));
        assert(unregisters == (stage >= 2));
        assert(powers == (stage >= 3));
        assert(resets == (stage >= 1) + (stage >= 2));
        assert(!slot.ds5_primary && !slot.serdes_setup_complete);
        struct ds5 replacement = {0};
        slot.ds5_primary = &replacement;
        slot.serdes_setup_complete = true;
        cleanup(&state); /* Idempotent even after the slot is reused. */
        assert(slot.ds5_primary == &replacement && slot.serdes_setup_complete);
        assert(unpairs == (stage >= 1) && unregisters == (stage >= 2));
    }
    return 0;
}
''')

    def test_every_probe_error_uses_rollback(self):
        source = (ROOT / 'kernel/realsense/d4xx.c').read_text()
        probe = body(source, 'ds5_probe')
        self.assertIn('e_regulator:', probe)
        self.assertIn('ds5_serdes_probe_cleanup(state);', probe.split('e_regulator:', 1)[1])
        setup = body(source, 'ds5_serdes_setup')
        self.assertIn('state->ser_paired = true;', setup)
        self.assertIn('state->dser_registered = true;', setup)
        self.assertIn('ds5_serdes_probe_cleanup(state);', setup)
        self.assertNotIn('serdes_setup_complete = true', setup)
        self.assertLess(probe.index('ds5_v4l_init(c, state)'),
                        probe.rindex('serdes_setup_complete = true'))
        recovery = probe.split('rec_state == DS5_DFU_MAGIC_LSW', 1)[1].split('return 0;', 1)[0]
        self.assertIn('serdes_setup_complete = true', recovery)

    def test_max96724_balances_one_enable_for_many_references(self):
        source = (ROOT / 'nvidia-oot/max96724.c').read_text()
        self.compile_run(r'''
struct max96724 {
    struct mutex lock;
    int pw_ref;
    void *reset_gpio, *vdd_cam_1v2;
};
static int disables, gpio_asserts;
static void usleep_range(int a, int b) { (void)a; (void)b; }
static void gpiod_set_value_cansleep(void *p, int v)
{ (void)p; assert(v == 0); gpio_asserts++; }
static int regulator_disable(void *p) { (void)p; disables++; return 0; }
static void power_off_locked(struct max96724 *priv)
''' + body(source, 'max96724_power_off_locked') + r'''
#define max96724_power_off_locked power_off_locked
static int remove_driver(struct i2c_client *client)
''' + body(source, 'max96724_remove') + r'''
int main(void)
{
    for (int refs = 0; refs < 4; refs++) {
        struct max96724 priv = {.pw_ref = refs};
        priv.reset_gpio = priv.vdd_cam_1v2 = &priv;
        struct i2c_client client = {.dev = {.data = &priv}};
        disables = gpio_asserts = 0;
        assert(remove_driver(&client) == 0);
        assert(priv.pw_ref == 0);
        assert(disables == (refs > 0) && gpio_asserts == (refs > 0));
    }
    struct max96724 shared = {.pw_ref = 2};
    shared.vdd_cam_1v2 = &shared;
    disables = 0;
    power_off_locked(&shared);
    assert(shared.pw_ref == 1 && disables == 0);
    power_off_locked(&shared);
    assert(shared.pw_ref == 0 && disables == 1);
    power_off_locked(&shared);
    assert(disables == 1);
    return 0;
}
''')

    def test_max96712_unbind_clears_its_own_slots_without_unregistering_client(self):
        matrix = os.environ.get('SERDES_MATRIX_DIR')
        if not matrix:
            self.skipTest('SERDES_MATRIX_DIR not set')
        sources = sorted(Path(matrix).glob('max96712-*.c'))
        self.assertTrue(sources)
        for path in sources:
            with self.subTest(version=path.stem):
                source = path.read_text()
                remove = body(source, 'max96712_remove')
                self.assertNotIn('i2c_unregister_device', remove)
                self.assertIn('.owner = THIS_MODULE', source)
                self.compile_run(r'''
struct max96712 { struct mutex lock; void *debugfs_dir; };
static struct max96712 *global_priv[4];
static struct mutex global_priv_lock;
static int debugfs_removes;
static void debugfs_remove_recursive(void *p) { (void)p; debugfs_removes++; }
static int remove_driver(struct i2c_client *client)
''' + remove + r'''
int main(void)
{
    struct max96712 own = {0}, sibling = {0};
    struct i2c_client client = {.dev = {.data = &own}};
    global_priv[0] = &own;
    global_priv[1] = &sibling;
    global_priv[3] = &own;
    assert(remove_driver(&client) == 0);
    assert(!global_priv[0] && !global_priv[3] && global_priv[1] == &sibling);
    assert(debugfs_removes == 1 && !global_priv_lock.locked);
    return 0;
}
''')

    def test_max96712_debugfs_failure_does_not_publish_dangling_slots(self):
        matrix = os.environ.get('SERDES_MATRIX_DIR')
        if not matrix:
            self.skipTest('SERDES_MATRIX_DIR not set')
        source = (Path(matrix) / 'max96712-6.2.2.c').read_text()
        self.compile_run(r'''
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
struct dentry { int dummy; };
struct max96712 {
    struct i2c_client *i2c_client;
    const char *channel;
    struct dentry *debugfs_dir;
};
static struct max96712 *global_priv[4];
static struct mutex global_priv_lock;
static struct dentry dir, file;
static const int max96712_debugfs_fops = 0;
static const char *channel = "a";
static int mode, removes;
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define IS_ERR_OR_NULL(p) (!(p) || IS_ERR(p))
#define PTR_ERR(p) ((long)(p))
static int of_property_read_string(void *np, const char *key, const char **value)
{ (void)np; (void)key; *value = channel; return mode == 1 ? -EINVAL : 0; }
static struct dentry *debugfs_create_dir(const char *name, void *parent)
{
    (void)name; (void)parent;
    if (mode == 2) return (struct dentry *)(intptr_t)-ENODEV;
    if (mode == 3) return (struct dentry *)(intptr_t)-ENOMEM;
    return &dir;
}
static struct dentry *debugfs_create_file(const char *name, int perms,
    struct dentry *parent, void *priv, const void *ops)
{
    (void)name; (void)perms; (void)parent; (void)priv; (void)ops;
    return mode == 4 ? NULL : &file;
}
static void debugfs_remove_recursive(void *p) { if (p) removes++; }
static int init_debugfs(const char *dir_name, struct dentry **d_entry,
                       struct dentry **f_entry, struct max96712 *priv)
''' + body(source, 'max96712_debugfs_init').replace(
            '{', '{\n\t(void)dir_name;', 1).replace(
                'client->dev.of_node', 'client->dev.data') + r'''
int main(void)
{
    struct i2c_client client = {.dev = {.data = &client}};
    struct max96712 own = {.i2c_client = &client}, sibling = {0};
    mode = 1;
    assert(init_debugfs(NULL, NULL, NULL, &own) == -EINVAL && !global_priv[0]);
    mode = 0;
    channel = "e";
    assert(init_debugfs(NULL, NULL, NULL, &own) == -EINVAL && !global_priv[0]);
    channel = "a";
    mode = 3;
    assert(init_debugfs(NULL, NULL, NULL, &own) == -ENOMEM && !global_priv[0]);
    mode = 4;
    assert(init_debugfs(NULL, NULL, NULL, &own) == -ENOMEM && !global_priv[0]);
    mode = 2; /* CONFIG_DEBUG_FS disabled still permits registration. */
    assert(init_debugfs(NULL, NULL, NULL, &own) == 0 && global_priv[0] == &own);
    assert(!own.debugfs_dir);
    global_priv[0] = &sibling;
    mode = 0;
    assert(init_debugfs(NULL, NULL, NULL, &own) == -EBUSY);
    assert(global_priv[0] == &sibling && !own.debugfs_dir);
    global_priv[0] = NULL;
    assert(init_debugfs(NULL, NULL, NULL, &own) == 0);
    assert(global_priv[0] == &own && own.debugfs_dir == &dir && removes == 2);
    return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
