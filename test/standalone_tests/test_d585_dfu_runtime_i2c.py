#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile the production DFU bus policy with fault-injectable host stubs.

Run from any directory; requires a C compiler with ASan/UBSan and pthreads.
No board access. The policy bodies are extracted, not reimplemented here.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/realsense/d4xx_main.c').read_text()
start = source.index('static DECLARE_RWSEM(ds5_maintenance_sem)')
end = source.index('\n#else\nstatic int ds5_dfu_bus_begin', start)
policy = source[start:end]
policy = '\n'.join(line for line in policy.splitlines() if not line.startswith('#if '))
SHIM = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
typedef uint16_t u16;
typedef uint32_t u32;
#define GFP_KERNEL 0
#define MAX_DS5_NUM 16
#define D585_2C_PROTO_PID 0xc07
#define D585_3C_PROTO_PID 0xc08
#define DS5_DFU_RECOVERY 1
#define D585_DFU_I2C_RATE 980000
#define READ_ONCE(x) (x)
#define dev_info(...) ((void)0)
#define DEFINE_MUTEX(n) pthread_mutex_t n = PTHREAD_MUTEX_INITIALIZER
#define DECLARE_RWSEM(n) pthread_rwlock_t n = PTHREAD_RWLOCK_INITIALIZER
#define mutex_lock pthread_mutex_lock
#define mutex_unlock pthread_mutex_unlock
#define down_write pthread_rwlock_wrlock
#define up_write pthread_rwlock_unlock
struct list_head { struct list_head *next, *prev; };
#define LIST_HEAD(n) struct list_head n = { &n, &n }
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define list_for_each_entry(p,h,m) \
 for (struct list_head *it=(h)->next; it!=(h) && ((p)=container_of(it,__typeof__(*(p)),m),1); it=it->next)
static void list_add(struct list_head *n, struct list_head *h) {
 n->next=h->next; n->prev=h; h->next->prev=n; h->next=n;
}
static void list_del(struct list_head *n) { n->prev->next=n->next; n->next->prev=n->prev; }
struct device_node { const char *compatible; struct device_node *parent; };
struct device { struct device *parent; struct device_node *of_node; int refs, fast, fail_enable, fail_restore; };
struct i2c_adapter { struct device dev; struct i2c_adapter *parent; int rate, nr; };
struct i2c_client { struct device dev; struct i2c_adapter *adapter; };
struct ds5_dfu_bus;
struct ds5;
struct ds5_dev { struct ds5 *ds5_primary; u16 d585_product_id; bool depth_streaming, rgb_streaming, ir_streaming, imu_streaming; };
struct ser_interface { int (*set_i2c_fast_mode)(struct device *,bool); };
struct dser_interface { int (*set_i2c_fast_mode)(struct device *,u32,bool); };
struct ds5 { struct i2c_client *client; struct ds5_dev *ds5_dev; bool d58x;
 struct { struct ds5_dfu_bus *bus; int dfu_state_flag; } dfu_dev;
 struct ser_interface *ser_ops; struct dser_interface *dser_ops;
 struct device *ser_dev, *dser_dev; u32 gmsl_link; void *regmap;
};
static struct ds5_dev ds5_inited[MAX_DS5_NUM];
static DEFINE_MUTEX(serdes_lock__);
static int alloc_fail, transfers;
static void *kzalloc(size_t n, int flags) { (void)flags; return alloc_fail ? NULL : calloc(1,n); }
#define kfree free
static void get_device(struct device *d) { d->refs++; }
static void put_device(struct device *d) { assert(d->refs>0); d->refs--; }
static struct i2c_adapter *i2c_parent_is_i2c_adapter(struct i2c_adapter *a) { return a->parent; }
static bool of_device_is_compatible(const struct device_node *n,const char *s) { return n && !strcmp(n->compatible,s); }
static struct device_node *of_get_parent(struct device_node *n) { return n ? n->parent : NULL; }
static void of_node_put(struct device_node *n) { (void)n; }
static bool ds5_is_d58x(struct ds5 *s) { return s->d58x; }
static int i2c_get_adapter_bus_clk_rate(struct i2c_adapter *a) { return a->rate; }
static int i2c_set_adapter_bus_clk_rate(struct i2c_adapter *a,int r) { a->rate=r; return 0; }
static int regmap_raw_read(void *m,u16 r,void *v,size_t n) {
 (void)m;(void)r; memset(v,0,n); transfers++; return 0;
}
static int regmap_raw_write(void *m,u16 r,const void *v,size_t n) {
 (void)m;(void)r;(void)v;(void)n; transfers++; return 0;
}
static int set_ser(struct device *d,bool f) {
 if (f) { d->fast=1; return d->fail_enable ? -EIO : 0; }
 if (d->fail_restore) return -EIO;
 d->fast=0; return 0;
}
static int set_dser(struct device *d,u32 l,bool f) { (void)l; return set_ser(d,f); }
static struct ser_interface ser_ops={set_ser};
static struct dser_interface dser_ops={set_dser};
'''
TEST = r'''
static struct i2c_adapter root={.rate=100000,.nr=2}, other={.rate=400000,.nr=3};
static struct device_node mux={.compatible="i2c-mux-gpio"}, channel={.parent=&mux};
static struct device_node d585_node={.compatible="realsense,d5xx"}, d400_node={.compatible="intel,d4xx"};
static struct i2c_adapter leg={.parent=&root,.dev={.parent=&root.dev,.of_node=&channel}};
static struct i2c_adapter leg2={.parent=&root,.dev={.parent=&root.dev,.of_node=&channel}};
static struct device ser[4], deser[4];
static struct i2c_client clients[4];
static struct ds5 cams[4];
static void init(void) {
 for (int i=0;i<4;i++) {
  clients[i].adapter=i==3 ? &other : i==1 ? &leg2 : &leg;
  clients[i].dev.of_node=i<2 ? &d585_node : &d400_node;
  cams[i]=(struct ds5){.client=&clients[i],.ds5_dev=&ds5_inited[i],
   .d58x=i<2,.ser_ops=&ser_ops,.dser_ops=&dser_ops,.ser_dev=&ser[i],.dser_dev=&deser[i]};
  ds5_inited[i].ds5_primary=&cams[i]; ds5_inited[i].d585_product_id=i<2 ? 0xc07 : 0;
 }
}
static void assert_idle(void) {
 assert(root.rate==100000 && root.dev.refs==0);
 assert(ds5_dfu_buses.next==&ds5_dfu_buses);
}
static void *writer(void *arg) { assert(ds5_dfu_bus_begin(arg)==0); return NULL; }
int main(void) {
 u16 v=0; init();
 assert(ds5_normal_access(&cams[2])==0);
 assert(ds5_camera_write(&cams[2],0,&v,2)==0); assert(transfers==1);
 // Drain in-flight camera IO before publishing the fast session.
 pthread_t thread; mutex_lock(&ds5_io_lock);
 assert(!pthread_create(&thread,NULL,writer,&cams[0])); usleep(10000);
 assert(root.rate==100000); mutex_unlock(&ds5_io_lock); pthread_join(thread,NULL);
 assert(root.rate==980000 && ser[0].fast && deser[0].fast);
 assert(ds5_normal_access(&cams[0])==-EBUSY);
 assert(ds5_normal_access(&cams[2])==-EBUSY);
 assert(ds5_normal_access(&cams[3])==0);
 assert(ds5_camera_read(&cams[2],0,&v,2)==-EBUSY && transfers==1);
 assert(ds5_camera_write(&cams[2],0,&v,2)==-EBUSY && transfers==1);
 assert(ds5_camera_read(&cams[0],0,&v,2)==0 && transfers==2);
 // Real same-root, different-mux-branch concurrent sessions; first close cannot restore host.
 assert(ds5_dfu_bus_begin(&cams[1])==0);
 assert(cams[0].dfu_dev.bus==cams[1].dfu_dev.bus && cams[0].dfu_dev.bus->users==2);
 assert(ds5_dfu_bus_begin(&cams[1])==0 && cams[1].dfu_dev.bus->users==2);
 assert(ds5_dfu_bus_end(&cams[0])==0 && root.rate==980000 && !ser[0].fast && ser[1].fast);
 assert(ds5_normal_access(&cams[2])==-EBUSY);
 assert(ds5_dfu_bus_end(&cams[1])==0); assert_idle();
 assert(ds5_dfu_bus_end(&cams[1])==0); assert_idle();
 // Legacy DFU and fast DFU cannot overlap; legacy alone preserves the original rate.
 assert(ds5_dfu_bus_begin(&cams[2])==0 && root.rate==100000);
 assert(ds5_dfu_bus_begin(&cams[0])==-EBUSY);
 assert(ds5_dfu_bus_end(&cams[2])==0); assert_idle();
 // All four target stream roles prevent maintenance entry before touching timing.
 bool *roles[]={&ds5_inited[0].depth_streaming,&ds5_inited[0].rgb_streaming,
  &ds5_inited[0].ir_streaming,&ds5_inited[0].imu_streaming};
 for (int i=0;i<4;i++) { *roles[i]=true; assert(ds5_dfu_bus_begin(&cams[0])==-EBUSY); *roles[i]=false; assert_idle(); }
 // D400 streaming is independent of its blocked I2C controls.
 ds5_inited[2].depth_streaming=true;
 assert(ds5_dfu_bus_begin(&cams[0])==0 && ds5_inited[2].depth_streaming);
 assert(ds5_normal_access(&cams[2])==-EBUSY);
 assert(ds5_dfu_bus_end(&cams[0])==0 && ds5_inited[2].depth_streaming);
 ds5_inited[2].depth_streaming=false; assert_idle();
 // Reverse direction: D400 DFU preserves D585 controls and existing stream.
 ds5_inited[0].depth_streaming=true;
 assert(ds5_dfu_bus_begin(&cams[2])==0);
 assert(ds5_normal_access(&cams[0])==0 && root.rate==100000);
 assert(ds5_camera_read(&cams[0],0,&v,2)==0);
 assert(ds5_dfu_bus_end(&cams[2])==0 && ds5_inited[0].depth_streaming);
 ds5_inited[0].depth_streaming=false; assert_idle();
 alloc_fail=1; assert(ds5_dfu_bus_begin(&cams[0])==-ENOMEM); alloc_fail=0; assert_idle();
 // A failed first/joining enable restores the attempted link without releasing other owners.
 ser[0].fail_enable=1; assert(ds5_dfu_bus_begin(&cams[0])==-EIO); ser[0].fail_enable=0; assert_idle();
 assert(ds5_dfu_bus_begin(&cams[0])==0); deser[1].fail_enable=1;
 assert(ds5_dfu_bus_begin(&cams[1])==-EIO && root.rate==980000 && !ser[1].fast && !deser[1].fast);
 deser[1].fail_enable=0; assert(ds5_dfu_bus_end(&cams[0])==0); assert_idle();
 // Unknown identity and I2C mux chips never get overclocked.
 cams[0].d58x=false; assert(!ds5_dfu_fast_capable(&cams[0]));
 cams[0].dfu_dev.dfu_state_flag=DS5_DFU_RECOVERY;
 assert(ds5_dfu_fast_capable(&cams[0]));
 clients[0].dev.of_node=&d400_node; assert(!ds5_dfu_fast_capable(&cams[0]));
 clients[0].dev.of_node=&d585_node; cams[0].d58x=true;
 mux.compatible="nxp,pca9548"; assert(!ds5_dfu_fast_capable(&cams[0])); mux.compatible="i2c-mux-gpio";
 assert(ds5_dfu_bus_begin(&cams[0])==0); ser[0].fail_restore=1;
 assert(ds5_dfu_bus_end(&cams[0])==-EIO && root.rate==100000);
 assert(ds5_normal_access(&cams[2])==-EIO && ds5_dfu_bus_begin(&cams[1])==-EIO);
 assert(cams[0].dfu_dev.bus==NULL); // fault state holds no dangling camera pointer
 ds5_dfu_bus_free(ds5_dfu_bus_find(&cams[0])); assert_idle();
 puts("PASS: production policy, root/mux scope, parallel DFU ownership, IO exclusion, streaming, rollback, recovery and fail-closed restoration");
}
'''
with tempfile.TemporaryDirectory(prefix='d585-runtime-i2c-') as temp:
    c = Path(temp) / 'test.c'
    binary = Path(temp) / 'test'
    c.write_text(SHIM + policy + TEST)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                    '-Werror', '-fsanitize=address,undefined', '-g', '-pthread',
                    str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
# Boundary checks supplement the executable policy tests.
for name, next_name in [('ds5_dfu_device_open', 'ds5_dfu_device_flush'),
                        ('ds5_dfu_device_read', 'ds5_dfu_finalize_download')]:
    body = source[source.index('static ', source.index(name) - 20):source.index(next_name, source.index(name))]
    assert 'i2c_set_adapter_bus_clk_rate' not in body
    assert 'ds5_dfu_bus_begin' not in body
print('PASS: open/read do not switch the bus clock')
