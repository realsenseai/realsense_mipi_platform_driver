#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile the production DFU bus policy with fault-injectable host stubs.

Run from any directory; requires a C compiler with ASan/UBSan and pthreads.
No board access. The policy bodies are extracted, not reimplemented here.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/realsense/d4xx_main.c').read_text()
start = source.index('static DECLARE_RWSEM(ds5_maintenance_sem)')
end = source.index('\n/* Pad ops */', start)
policy = source[start:end]
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
#include <time.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define GFP_KERNEL 0
#define MAX_DS5_NUM 16
#define D585_2C_PROTO_PID 0xc07
#define D585_3C_PROTO_PID 0xc08
#define DS5_DFU_RECOVERY 1
#define DS5_DFU_IN_PROGRESS 2
#define DS5_DFU_ERROR 3
#define PIPE_NOT_CONFIGURED (-1)
#define D585_DFU_I2C_RATE 980000
#define DFU_I2C_STANDARD_MODE 100000
#define DFU_I2C_FAST_MODE 400000
#define DFU_I2C_BUS_CLK_RATE DFU_I2C_FAST_MODE
#define KERNEL_VERSION(a,b,c) (((a)<<16)+((b)<<8)+(c))
#define READ_ONCE(x) (x)
#define dev_info(...) ((void)0)
#define dev_warn(...) ((void)0)
#define dev_err(...) ((void)0)
#define dev_dbg(...) ((void)0)
#define DS5_DFU_IDLE 0
#define DS5_I2C_RETRY_COUNT 5
#define DS5_I2C_RETRY_DELAY_US 5000
#define usleep_range(a,b) ((void)0)
#define DEFINE_MUTEX(n) pthread_mutex_t n = PTHREAD_MUTEX_INITIALIZER
#define DECLARE_RWSEM(n) pthread_rwlock_t n = PTHREAD_RWLOCK_INITIALIZER
#define mutex_lock pthread_mutex_lock
#define mutex_unlock pthread_mutex_unlock
#define down_read pthread_rwlock_rdlock
#define up_read pthread_rwlock_unlock
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
struct ds5_dfu_status;
struct ds5;
struct ds5_dev { struct ds5_dfu_status *dfu_status; struct ds5 *ds5_primary; u16 d585_product_id; bool serdes_setup_complete, depth_streaming, rgb_streaming, ir_streaming, imu_streaming; };
struct ser_interface { int (*set_i2c_fast_mode)(struct device *,bool); };
struct dser_interface { int (*set_i2c_fast_mode)(struct device *,u32,bool); int (*release_pipe)(struct device *,int); };
struct v4l2_subdev { int unused; };
struct ds5_sensor { bool streaming; int pipe_id; };
struct ds5 { pthread_mutex_t lock; struct { struct { struct v4l2_subdev subdev; } sd; struct ds5_sensor *last_set; } mux; struct i2c_client *client; struct ds5_dev *ds5_dev; bool d58x, is_depth;
 struct { int dfu_state_flag; } dfu_dev;
 struct ser_interface *ser_ops; struct dser_interface *dser_ops;
 struct device *ser_dev, *dser_dev; u32 gmsl_link; void *regmap;
};
static struct ds5_dev ds5_inited[MAX_DS5_NUM];
static DEFINE_MUTEX(serdes_lock__);
static int alloc_fail, transfers, fail_rate=-1;
static bool clear_pid_on_enable;
static int transfer_error, transfer_failures;
static pthread_mutex_t progress_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t progress=PTHREAD_COND_INITIALIZER;
static bool hold_block, block_entered, peer_done;

static void *kzalloc(size_t n, int flags) { (void)flags; return alloc_fail ? NULL : calloc(1,n); }
#define kfree free
static struct device *get_device(struct device *d) { d->refs++; return d; }
static void put_device(struct device *d) { assert(d->refs>0); d->refs--; }
static struct i2c_adapter *i2c_parent_is_i2c_adapter(struct i2c_adapter *a) { return a->parent; }
static bool of_device_is_compatible(const struct device_node *n,const char *s) { return n && !strcmp(n->compatible,s); }
static struct device_node *of_get_parent(struct device_node *n) { return n ? n->parent : NULL; }
static void of_node_put(struct device_node *n) { (void)n; }
static bool ds5_is_d58x(struct ds5 *s) { return s->d58x; }
static int i2c_get_adapter_bus_clk_rate(struct i2c_adapter *a) { return a->rate; }
static int i2c_set_adapter_bus_clk_rate(struct i2c_adapter *a,int r) { a->rate=r; return r==fail_rate ? -EIO : 0; }
static int regmap_raw_read(void *m,u16 r,void *v,size_t n) {
 (void)m;(void)r; memset(v,0,n); transfers++; return transfer_failures-- > 0 ? transfer_error : 0;
}
static int regmap_raw_write(void *m,u16 r,const void *v,size_t n) {
 (void)m;(void)r;(void)v; transfers++;
 int ret=transfer_failures-- > 0 ? transfer_error : 0;
 if(n==1024) {
  pthread_mutex_lock(&progress_lock);
  block_entered=true; pthread_cond_broadcast(&progress);
  while(hold_block)pthread_cond_wait(&progress,&progress_lock);
  pthread_mutex_unlock(&progress_lock);
 }
 return ret;
}
static int set_ser(struct device *d,bool f) {
 if (f) { if(clear_pid_on_enable)ds5_inited[0].d585_product_id=0; d->fast=1; return d->fail_enable ? -EIO : 0; }
 if (d->fail_restore) return -EIO;
 d->fast=0; return 0;
}
static int set_dser(struct device *d,u32 l,bool f) { (void)l; return set_ser(d,f); }
static struct ser_interface ser_ops={set_ser};
static int released_pipes, release_error;
static int release_pipe(struct device *d,int pipe) { (void)d;assert(pipe>=0);if(release_error)return release_error;released_pipes++;return 0; }
static struct dser_interface dser_ops={set_dser,release_pipe};
'''
# Exercise the production stream gate and control entry with the real policy.
stream_gate = source[source.index('static int ds5_mux_s_stream_guarded('):
                     source.index('static const struct v4l2_subdev_video_ops ds5_sensor_video_ops')]
ctrl = source[source.index('static int ds5_s_ctrl('):source.index('static int ds5_gvd(')]
ctrl_entry = ctrl[ctrl.index('\tmutex_lock(&state->lock);'):ctrl.index('\tdpp_desc =')]
gates = r'''
static int normal_calls;
static int ds5_mux_s_stream(struct v4l2_subdev *sd, int on) {
 (void)sd; (void)on; normal_calls++; return 0;
}
''' + stream_gate + 'static int test_control_entry(struct ds5 *state) { int ret;\n' + ctrl_entry + r'''
 normal_calls++;
 up_read(&ds5_maintenance_sem);
 mutex_unlock(&state->lock);
 return 0;
}
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
static struct ds5_sensor sensors[4];
static void init(void) {
 for (int i=0;i<4;i++) {
  clients[i].adapter=i==3 ? &other : i==1 ? &leg2 : &leg;
  clients[i].dev.of_node=i!=2 ? &d585_node : &d400_node;
  cams[i]=(struct ds5){.client=&clients[i],.ds5_dev=&ds5_inited[i],
   .d58x=i!=2,.is_depth=true,.ser_ops=&ser_ops,.dser_ops=&dser_ops,.ser_dev=&ser[i],.dser_dev=&deser[i]};
  sensors[i].pipe_id=PIPE_NOT_CONFIGURED;cams[i].mux.last_set=&sensors[i];
  pthread_mutex_init(&cams[i].lock,NULL);
  ds5_inited[i].ds5_primary=&cams[i]; ds5_inited[i].serdes_setup_complete=true; ds5_inited[i].d585_product_id=i!=2 ? 0xc07 : 0;
 }
}
static void assert_idle(void) {
 assert(root.rate==100000 && root.dev.refs==0);
 assert(ds5_dfu_buses.next==&ds5_dfu_buses);
 for(int i=0;i<4;i++)assert(!ser[i].refs && !deser[i].refs);
}
static void *writer(void *arg) { assert(ds5_dfu_session_begin(arg)==0); return NULL; }
static void *block_writer(void *arg) {
 u8 image[1024]={0};
 assert(ds5_raw_write(arg,0x4a00,image,sizeof(image))==0);return NULL;
}
static void *peer_reader(void *arg) {
 struct ds5 *peer=arg;u16 v=0;
 assert(test_control_entry(peer)==0);
 assert(ds5_mux_s_stream_guarded(&peer->mux.sd.subdev,1)==0);
 assert(ds5_read(peer,0,&v)==0 && ds5_write(peer,0,v)==0);
 assert(ds5_raw_read(peer,0,&v,sizeof(v))==0);
 assert(ds5_raw_write(peer,0,&v,sizeof(v))==0);
 assert(ds5_read_poll(peer,0,&v)==0);
 pthread_mutex_lock(&progress_lock);
 peer_done=true;pthread_cond_broadcast(&progress);
 pthread_mutex_unlock(&progress_lock);
 return NULL;
}

int main(void) {
 // Direct admission predicates run without concurrent state changes.
 u16 v=0; init();
 assert(ds5_check_i2c_access(&cams[2], false)==0);
 assert(ds5_raw_write(&cams[2],0,&v,2)==0); assert(transfers==1);
 // Drain in-flight camera IO before publishing the fast session.
 pthread_t thread; down_read(&ds5_io_sem);
 assert(!pthread_create(&thread,NULL,writer,&cams[0])); usleep(10000);
 assert(root.rate==100000); up_read(&ds5_io_sem); pthread_join(thread,NULL);
 assert(root.rate==980000 && ser[0].fast && deser[0].fast && ser[1].fast && deser[1].fast);
 assert(ds5_check_i2c_access(&cams[0], false)==-EBUSY);
 assert(ds5_check_i2c_access(&cams[2], false)==-EBUSY);
 assert(ds5_check_i2c_access(&cams[3], false)==0 && !ser[3].fast && !deser[3].fast);
 assert(ds5_raw_read(&cams[2],0,&v,2)==-EBUSY && transfers==1);
 assert(ds5_raw_write(&cams[2],0,&v,2)==-EBUSY && transfers==1);
 assert(ds5_raw_read(&cams[0],0,&v,2)==0 && transfers==2);
 // A peer can enumerate, read and write without joining DFU; legacy stays blocked.
 assert(cams[1].ds5_dev->dfu_status==NULL && ds5_check_i2c_access(&cams[1], false)==0);
 assert(ds5_raw_read(&cams[1],0,&v,2)==0);
 assert(ds5_raw_write(&cams[1],0,&v,2)==0);
 assert(root.rate==980000);
 // Real same-root, different-mux-branch concurrent sessions; first close cannot restore host.
 assert(ds5_dfu_session_begin(&cams[1])==0);
 assert(cams[0].ds5_dev->dfu_status==cams[1].ds5_dev->dfu_status && cams[0].ds5_dev->dfu_status->users==2);
 assert(ds5_dfu_session_begin(&cams[1])==0 && cams[1].ds5_dev->dfu_status->users==2);
 assert(ds5_dfu_session_end(&cams[0])==0 && root.rate==980000 && ser[0].fast && ser[1].fast);
 assert(ds5_check_i2c_access(&cams[2], false)==-EBUSY);
 assert(ds5_dfu_session_end(&cams[1])==0); assert_idle();
 assert(!ser[0].fast && !ser[1].fast && !deser[0].fast && !deser[1].fast);
 assert(ds5_dfu_session_end(&cams[1])==0); assert_idle();
 // Preserve legacy 100 -> 400 -> 100 kHz, with compatible D400/D585 controls.
 assert(ds5_dfu_session_begin(&cams[2])==0 && root.rate==400000);
 assert(ds5_check_i2c_access(&cams[2], false)==0 && ds5_check_i2c_access(&cams[0], false)==0);
 assert(ds5_raw_read(&cams[2],0,&v,2)==0);
 assert(ds5_raw_write(&cams[0],0,&v,2)==0);
 assert(ds5_dfu_session_begin(&cams[0])==-EBUSY);
 assert(ds5_dfu_session_end(&cams[2])==0); assert_idle();
 // All four target stream roles prevent maintenance entry before touching timing.
 bool *roles[]={&ds5_inited[0].depth_streaming,&ds5_inited[0].rgb_streaming,
  &ds5_inited[0].ir_streaming,&ds5_inited[0].imu_streaming};
 for (int i=0;i<4;i++) { *roles[i]=true; assert(ds5_dfu_session_begin(&cams[0])==-EBUSY); *roles[i]=false; assert_idle(); }
 // D400 streaming is independent of its blocked I2C controls.
 ds5_inited[2].depth_streaming=true;
 assert(ds5_dfu_session_begin(&cams[0])==0 && ds5_inited[2].depth_streaming);
 assert(ds5_check_i2c_access(&cams[2], false)==-EBUSY);
 assert(ds5_dfu_session_end(&cams[0])==0 && ds5_inited[2].depth_streaming);
 ds5_inited[2].depth_streaming=false; assert_idle();
 // Reverse direction: D400 DFU preserves D585 controls and existing stream.
 ds5_inited[0].depth_streaming=true;
 assert(ds5_dfu_session_begin(&cams[2])==0);
 assert(ds5_check_i2c_access(&cams[0], false)==0 && root.rate==400000);
 assert(ds5_raw_read(&cams[0],0,&v,2)==0);
 assert(ds5_dfu_session_end(&cams[2])==0 && ds5_inited[0].depth_streaming);
 ds5_inited[0].depth_streaming=false; assert_idle();
 // Two legacy opens share one original clock; do not restore on the first close.
 cams[1].d58x=false;
 assert(ds5_dfu_session_begin(&cams[2])==0 && root.rate==400000);
 assert(ds5_dfu_session_begin(&cams[1])==0 && cams[1].ds5_dev->dfu_status->users==2);
 assert(ds5_check_i2c_access(&cams[0], false)==0 && ds5_check_i2c_access(&cams[1], false)==0);
 assert(ds5_dfu_session_end(&cams[2])==0 && root.rate==400000);
 assert(ds5_dfu_session_end(&cams[1])==0); assert_idle(); cams[1].d58x=true;
 // Legacy information opens remain usable while that camera is streaming.
 ds5_inited[2].depth_streaming=true;
 assert(ds5_dfu_session_begin(&cams[2])==0 && root.rate==400000);
 assert(ds5_check_i2c_access(&cams[2], false)==0);
 assert(ds5_dfu_session_end(&cams[2])==0); assert_idle();
 ds5_inited[2].depth_streaming=false;
 // Original direct-adapter path has no legacy boost.
 clients[2].adapter=&root;
 assert(ds5_dfu_session_begin(&cams[2])==0 && !cams[2].ds5_dev->dfu_status); assert_idle();
 clients[2].adapter=&leg;
 // I2C-controlled muxes retain legacy 400 kHz, never use 980 kHz.
 mux.compatible="nxp,pca9548";
 assert(ds5_dfu_session_begin(&cams[0])==0 && root.rate==400000 && !ser[0].fast);
 assert(ds5_check_i2c_access(&cams[2], false)==0 && ds5_check_i2c_access(&cams[1], false)==0);
 assert(ds5_dfu_session_end(&cams[0])==0); assert_idle(); mux.compatible="i2c-mux-gpio";
 // Legacy clock errors restore the baseline or retain the existing fault gate.
 fail_rate=400000; assert(ds5_dfu_session_begin(&cams[2])==-EIO);
 fail_rate=-1; assert_idle();
 assert(ds5_dfu_session_begin(&cams[2])==0); fail_rate=100000;
 assert(ds5_dfu_session_end(&cams[2])==-EIO && !cams[2].ds5_dev->dfu_status);
 assert(ds5_check_i2c_access(&cams[0], false)==-EIO);
 fail_rate=-1; ds5_free_dfu_status(ds5_find_root_dfu_status(&cams[2])); assert_idle();
 alloc_fail=1; assert(ds5_dfu_session_begin(&cams[0])==-ENOMEM); alloc_fail=0; assert_idle();
 // A failed first enable rolls back every prepared/partially prepared peer link.
 ser[0].fail_enable=1; assert(ds5_dfu_session_begin(&cams[0])==-EIO); ser[0].fail_enable=0; assert_idle();
 deser[1].fail_enable=1;
 assert(ds5_dfu_session_begin(&cams[0])==-EIO); assert_idle();
 assert(!ser[0].fast && !deser[0].fast && !ser[1].fast && !deser[1].fast);
 deser[1].fail_enable=0;
 // A streaming peer still has controls and matched timing during another DFU.
 ds5_inited[1].depth_streaming=true;
 assert(ds5_dfu_session_begin(&cams[0])==0 && ds5_check_i2c_access(&cams[1], false)==0);
 assert(ds5_dfu_session_begin(&cams[1])==-EBUSY && root.rate==980000);
 assert(ds5_dfu_session_end(&cams[0])==0 && ds5_inited[1].depth_streaming); assert_idle();
 ds5_inited[1].depth_streaming=false;
 // Bootloader identity is unavailable and the DFU state changes mid-session.
 cams[0].d58x=false; ds5_inited[0].d585_product_id=0;
 cams[0].dfu_dev.dfu_state_flag=DS5_DFU_RECOVERY;
 assert(ds5_dfu_session_begin(&cams[0])==0 && root.rate==980000);
 cams[0].dfu_dev.dfu_state_flag=DS5_DFU_IN_PROGRESS;
 assert(!d5xx_i2c_fast_capable(&cams[0])); // raw probe identity really disappeared
 int recovery_transfers=transfers;
 assert(ds5_raw_write(&cams[0],0x4a00,&v,2)==0 && transfers==recovery_transfers+1);
 assert(ds5_read_poll(&cams[0],0,&v)==0);
 assert(ds5_check_i2c_access(&cams[0],false)==-EBUSY);
 assert(ds5_dfu_session_begin(&cams[1])==0);
 cams[0].dfu_dev.dfu_state_flag=DS5_DFU_ERROR;
 assert(ds5_write(&cams[0],0x5010,0)==0); // error cleanup must reach firmware too
 assert(ds5_dfu_session_end(&cams[0])==0 && root.rate==980000);
 // Registry removal and PID reset cannot change the saved restore set.
 ds5_inited[0].ds5_primary=NULL; ds5_inited[0].serdes_setup_complete=false;
 ds5_inited[1].d585_product_id=0;
 assert(ds5_raw_read(&cams[0],0,&v,2)==0 && ds5_raw_read(&cams[1],0,&v,2)==0);
 assert(ds5_dfu_session_end(&cams[1])==0);
 assert(!ser[0].fast && !deser[0].fast && !ser[1].fast && !deser[1].fast);assert_idle();
 ds5_inited[0].ds5_primary=&cams[0];ds5_inited[0].serdes_setup_complete=true;
 ds5_inited[0].d585_product_id=ds5_inited[1].d585_product_id=0xc07;
 cams[0].d58x=true;cams[0].dfu_dev.dfu_state_flag=DS5_DFU_IDLE;
 // Partially failed setup must also restore after the identity changed.
 clear_pid_on_enable=true;fail_rate=980000;
 assert(ds5_dfu_session_begin(&cams[0])==-EIO);
 assert(!ser[0].fast && !deser[0].fast && !ser[1].fast && !deser[1].fast);assert_idle();
 clear_pid_on_enable=false;fail_rate=-1;ds5_inited[0].d585_product_id=0xc07;
 // A newly qualifying link was not prepared and cannot join an active bus.
 assert(ds5_dfu_session_begin(&cams[0])==0);
 cams[2].d58x=true;ds5_inited[2].d585_product_id=0xc07;
 assert(ds5_raw_read(&cams[2],0,&v,2)==-EBUSY);
 assert(ds5_dfu_session_begin(&cams[2])==-EBUSY);
 cams[2].d58x=false;ds5_inited[2].d585_product_id=0;
 assert(ds5_dfu_session_end(&cams[0])==0);assert_idle();
 puts("PASS: recovery firmware transfers and cleanup survive DFU-state changes; final owner restores saved endpoints after registry/PID changes");
 // Blocked host stops must release pipes without sending forbidden I2C or
 // pretending the firmware stopped. Repeated failed starts cannot allocate.
 ds5_inited[2].depth_streaming=true;sensors[2].streaming=true;sensors[2].pipe_id=2;
 assert(ds5_dfu_session_begin(&cams[0])==0);
 int stopped_transfers=transfers, pipes_before=released_pipes;
 for(int i=0;i<8;i++) {
  assert(ds5_mux_s_stream_guarded(&cams[2].mux.sd.subdev,0)==-EBUSY);
  assert(ds5_mux_s_stream_guarded(&cams[2].mux.sd.subdev,1)==-EBUSY);
 }
 assert(!sensors[2].streaming && ds5_inited[2].depth_streaming);
 assert(sensors[2].pipe_id==PIPE_NOT_CONFIGURED && released_pipes==pipes_before+1);
 assert(transfers==stopped_transfers);
 // A failed host-pipe release retains its ID so a later stop can retry it.
 sensors[2].pipe_id=2;release_error=-EIO;
 assert(ds5_mux_s_stream_guarded(&cams[2].mux.sd.subdev,0)==-EBUSY && sensors[2].pipe_id==2);
 release_error=0;
 assert(ds5_mux_s_stream_guarded(&cams[2].mux.sd.subdev,0)==-EBUSY && sensors[2].pipe_id==PIPE_NOT_CONFIGURED);
 assert(ds5_dfu_session_end(&cams[0])==0);assert_idle();
 ds5_inited[2].depth_streaming=false;
 puts("PASS: blocked stops release host pipes, retain physical streaming flags and never transfer camera I2C");
 // Unknown identity and I2C mux chips never get overclocked.
 cams[0].d58x=false; assert(!d5xx_i2c_fast_capable(&cams[0]));
 cams[0].dfu_dev.dfu_state_flag=DS5_DFU_RECOVERY;
 assert(d5xx_i2c_fast_capable(&cams[0]));
 clients[0].dev.of_node=&d400_node; assert(!d5xx_i2c_fast_capable(&cams[0]));
 clients[0].dev.of_node=&d585_node; cams[0].d58x=true;
 mux.compatible="nxp,pca9548"; assert(!d5xx_i2c_fast_capable(&cams[0])); mux.compatible="i2c-mux-gpio";
 assert(ds5_dfu_session_begin(&cams[0])==0); ser[0].fail_restore=1;
 assert(ds5_dfu_session_end(&cams[0])==0 && root.rate==100000);
 assert(!ser[1].fast && !deser[1].fast); // other links are restored even after an error
 assert(ds5_check_i2c_access(&cams[2], false)==0 && !cams[0].ds5_dev->dfu_status); assert_idle();
 assert(ds5_read(&cams[2],0,&v)==0); // a resetting serializer cannot poison peers
 ser[0].fail_restore=0;
 assert(ds5_dfu_session_begin(&cams[1])==0 && ds5_dfu_session_end(&cams[1])==0); assert_idle();
 // Legacy paths keep their old rate handling; only 980 kHz entry validates baseline.
 for (int i=0;i<2;i++) {
  root.rate=i ? 500000 : 0;
  assert(ds5_dfu_session_begin(&cams[0])==-EINVAL && !cams[0].ds5_dev->dfu_status);
  assert(ds5_dfu_session_begin(&cams[2])==0 && root.rate==400000);
  assert(ds5_dfu_session_end(&cams[2])==0 && root.rate==(i ? 500000 : 0));
 }
 root.rate=100000; assert_idle();
 // All sensor roles share the target session; sibling removal cannot end it.
 struct ds5 sibling[3];
 for (int i=0;i<3;i++) {
  sibling[i]=cams[0]; sibling[i].is_depth=false;
  pthread_mutex_init(&sibling[i].lock,NULL);
 }
 assert(ds5_dfu_session_begin(&cams[0])==0);
 int calls=normal_calls;
 for(int i=0;i<4;i++) {
  struct ds5 *node=i ? &sibling[i-1] : &cams[0];
  assert(test_control_entry(node)==-EBUSY);
  assert(ds5_mux_s_stream_guarded(&node->mux.sd.subdev,1)==-EBUSY);
  assert(ds5_mux_s_stream_guarded(&node->mux.sd.subdev,0)==-EBUSY);
  if(i)assert(ds5_dfu_session_end(node)==0 && root.rate==980000 && cams[0].ds5_dev->dfu_status->users==1);
 }
 assert(normal_calls==calls);
 // The same entry rejects a D400 reset control before the reset body runs.
 assert(test_control_entry(&cams[2])==-EBUSY && normal_calls==calls);
 assert(ds5_mux_s_stream_guarded(&cams[2].mux.sd.subdev,1)==-EBUSY);
 assert(test_control_entry(&cams[1])==0);
 assert(ds5_mux_s_stream_guarded(&cams[1].mux.sd.subdev,1)==0);
 assert(ds5_dfu_session_end(&cams[0])==0); assert_idle();
 for(int i=0;i<3;i++) {
  assert(test_control_entry(&sibling[i])==0);
  assert(ds5_mux_s_stream_guarded(&sibling[i].mux.sd.subdev,1)==0);
 }
 assert(test_control_entry(&cams[2])==0);
 // An admitted control/reset holds the read-side until completion, preventing
 // a new fast session from entering in the middle of its readiness loop.
 down_read(&ds5_maintenance_sem);
 assert(!pthread_create(&thread,NULL,writer,&cams[0])); usleep(10000);
 assert(root.rate==100000 && !cams[0].ds5_dev->dfu_status);
 up_read(&ds5_maintenance_sem); pthread_join(thread,NULL);
 assert(root.rate==980000);
 assert(ds5_dfu_session_end(&cams[0])==0); assert_idle();
 puts("PASS: sibling controls/stream transitions blocked, peer D585 admitted, sibling teardown preserves ownership, reset admission and maintenance drain");
 // A DFU block stays in regmap while another controller completes all five
 // I/O entries and normal control/stream admission. No timing assumption about
 // the actual block length: release it only after the peer has completed.
 assert(ds5_dfu_session_begin(&cams[0])==0);
 cams[3].d58x=false; // a D400 on an independent root is also eligible
 pthread_t block_thread,peer_thread;
 pthread_mutex_lock(&progress_lock);hold_block=true;block_entered=peer_done=false;
 assert(!pthread_create(&block_thread,NULL,block_writer,&cams[0]));
 while(!block_entered)pthread_cond_wait(&progress,&progress_lock);
 assert(pthread_rwlock_trywrlock(&ds5_io_sem)==EBUSY);
 assert(!pthread_create(&peer_thread,NULL,peer_reader,&cams[3]));
 struct timespec deadline;assert(!clock_gettime(CLOCK_REALTIME,&deadline));deadline.tv_sec+=2;
 while(!peer_done) {
  int err=pthread_cond_timedwait(&progress,&progress_lock,&deadline);
  if(err==ETIMEDOUT) {
   fputs("FAIL: independent-root I/O serialized behind DFU block\n",stderr);
   exit(97);
  }
  assert(!err);
 }
 assert(hold_block && root.rate==980000);
 hold_block=false;pthread_cond_broadcast(&progress);pthread_mutex_unlock(&progress_lock);
 pthread_join(peer_thread,NULL);pthread_join(block_thread,NULL);cams[3].d58x=true;
 assert(ds5_dfu_session_end(&cams[0])==0);assert_idle();
 puts("PASS: unrelated-root controls, stream admission and all I/O entries complete during an in-flight DFU block; clock changes require exclusive access");
 // Adapter EBUSY is retried; admission EBUSY performs no transfer.
 for (int op=0;op<4;op++) {
  int before=transfers, ret;
  transfer_error=-EBUSY; transfer_failures=2;
  switch(op) {
  case 0: ret=ds5_read(&cams[2],0,&v); break;
  case 1: ret=ds5_write(&cams[2],0,v); break;
  case 2: ret=ds5_raw_read(&cams[2],0,&v,2); break;
  default: ret=ds5_raw_write(&cams[2],0,&v,2); break;
  }
  assert(!ret && transfers==before+3);
 }
 int before=transfers;
 transfer_error=-EBUSY; transfer_failures=100;
 assert(ds5_read(&cams[2],0,&v)==-EBUSY && transfers==before+DS5_I2C_RETRY_COUNT);
 before=transfers;
 assert(ds5_read_poll(&cams[2],0,&v)==-EBUSY && transfers==before+1);
 transfer_failures=0;
 assert(ds5_dfu_session_begin(&cams[0])==0);
 before=transfers;
 assert(ds5_read(&cams[2],0,&v)==-EBUSY);
 assert(ds5_write(&cams[2],0,v)==-EBUSY);
 assert(ds5_raw_read(&cams[2],0,&v,2)==-EBUSY);
 assert(ds5_raw_write(&cams[2],0,&v,2)==-EBUSY);
 assert(ds5_read_poll(&cams[2],0,&v)==-EBUSY && transfers==before);
 assert(ds5_dfu_session_end(&cams[0])==0); assert_idle();
 puts("PASS: production policy, root/mux scope, parallel DFU ownership, peer high-speed controls, legacy IO exclusion, streaming, rollback, recovery and fail-closed restoration");
}
'''
with tempfile.TemporaryDirectory(prefix='d585-runtime-i2c-') as temp:
    c = Path(temp) / 'test.c'
    binary = Path(temp) / 'test'
    c.write_text(SHIM + policy + gates + TEST)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                    '-Werror', '-DCONFIG_TEGRA_CAMERA_PLATFORM', '-DCONFIG_VIDEO_D4XX_SERDES',
                    '-DLINUX_VERSION_CODE=331961', '-fsanitize=address,undefined', '-g', '-pthread',
                    str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    exclusive = (policy + gates).replace('down_read(&ds5_io_sem);', 'down_write(&ds5_io_sem);')
    c.write_text(SHIM + exclusive + TEST)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                    '-Werror', '-DCONFIG_TEGRA_CAMERA_PLATFORM', '-DCONFIG_VIDEO_D4XX_SERDES',
                    '-DLINUX_VERSION_CODE=331961', '-g', '-pthread', str(c), '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
    assert result.returncode == 97, result.stderr
    assert 'independent-root I/O serialized behind DFU block' in result.stderr
    print('PASS: exclusive-I/O negative control reproduces cross-root blocking')
# Information reads never acquire a session; open retains only the legacy rate.
read_body = source[source.index('static ssize_t ds5_dfu_device_read('):
                   source.index('static int ds5_dfu_finalize_download(')]
assert 'ds5_dfu_session_begin' not in read_body
assert 'i2c_set_adapter_bus_clk_rate' not in read_body

# An SDK enumeration must reject an already-open DFU target without waiting
# behind its full-image write; competing first opens still need the locked check.
open_body = source[source.index('static int ds5_dfu_device_open('):
                   source.index('static int ds5_dfu_device_flush(')]
open_shim = r"""
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define DS5_DFU_IDLE 0
#define DS5_DFU_RECOVERY 2
#define DS5_DFU_ERROR 3
#define DS5_DFU_OPEN 1
#define DFU_BLOCK_SIZE 1024
#define GFP_KERNEL 0
#define dfuMANIFEST_WAIT_RESET 8
#define dfuMANIFEST 7
struct inode { void *i_cdev; };
struct file { void *private_data; };
struct client { int dev; };
struct ds5 { int lock; struct client *client;
 struct { int ds5_cdev, device_open_count, dfu_state_flag, manifest_state;
          void *dfu_msg; } dfu_dev; };
static int lock_calls, competing_open, allocation_failure, allocations;
static int fast_capable=1, begin_calls, begin_error;
static int d5xx_i2c_fast_capable(struct ds5 *s) { (void)s; return fast_capable; }
static int ds5_dfu_session_begin(struct ds5 *s) { assert(s->lock); begin_calls++; return begin_error; }
static void devm_kfree(void *d,void *p) { (void)d; free(p); }
static struct ds5 *target;
static void mutex_lock(int *lock) {
 assert(!*lock); *lock=1; lock_calls++;
 if (competing_open) target->dfu_dev.device_open_count=1;
}
static void mutex_unlock(int *lock) { assert(*lock); *lock=0; }
static int ds5_is_d58x(struct ds5 *s) { (void)s; return 1; }
static void *devm_kzalloc(void *dev,size_t n,int flags) {
 (void)dev; (void)flags; allocations++;
 return allocation_failure ? NULL : calloc(1,n);
}
"""
open_test = r"""
int main(void) {
 struct client client={0}; struct ds5 state={.client=&client};
 struct inode inode={.i_cdev=&state.dfu_dev.ds5_cdev}; struct file file={0};
 target=&state;
 state.lock=1; state.dfu_dev.device_open_count=1;
 assert(ds5_dfu_device_open(&inode,&file)==-EBUSY && lock_calls==0 && !allocations);
 assert(!file.private_data && state.lock==1);
 state.lock=0; state.dfu_dev.device_open_count=0; competing_open=1;
 assert(ds5_dfu_device_open(&inode,&file)==-EBUSY && lock_calls==1 && !allocations);
 assert(!state.lock && !file.private_data);
 competing_open=0; state.dfu_dev.device_open_count=0; allocation_failure=1;
 assert(ds5_dfu_device_open(&inode,&file)==-ENOMEM && !state.lock);
 assert(!state.dfu_dev.device_open_count && !file.private_data);
 allocation_failure=0;
 assert(!ds5_dfu_device_open(&inode,&file) && !state.lock);
 assert(state.dfu_dev.device_open_count==1 && file.private_data==&state);
 assert(!begin_calls); free(state.dfu_dev.dfu_msg);
 state.dfu_dev.device_open_count=0; file.private_data=NULL; fast_capable=0;
 assert(!ds5_dfu_device_open(&inode,&file) && begin_calls==1);
 free(state.dfu_dev.dfu_msg);
 state.dfu_dev.device_open_count=0; file.private_data=NULL; begin_error=-EBUSY;
 assert(ds5_dfu_device_open(&inode,&file)==-EBUSY && begin_calls==2);
 assert(!state.dfu_dev.device_open_count && !file.private_data && !state.dfu_dev.dfu_msg && !state.lock);
 // Failed opens have no release callback. Preserve IDLE for V4L2 opens,
 // and preserve RECOVERY/ERROR so a later DFU retry sees the original state.
 for (int entry=DS5_DFU_IDLE;entry<=DS5_DFU_ERROR;entry++) {
  for (int fast=0;fast<=1;fast++) {
   fast_capable=fast; state.dfu_dev.dfu_state_flag=entry;
   allocation_failure=1;
   assert(ds5_dfu_device_open(&inode,&file)==-ENOMEM);
   assert(state.dfu_dev.dfu_state_flag==entry && !state.lock);
   assert(!state.dfu_dev.device_open_count && !state.dfu_dev.dfu_msg && !file.private_data);
   allocation_failure=0;
   if (!fast) {
    int failures[]={-EBUSY,-EIO,-ENOMEM};
    for (unsigned int i=0;i<sizeof(failures)/sizeof(failures[0]);i++) {
     begin_error=failures[i];
     assert(ds5_dfu_device_open(&inode,&file)==failures[i]);
     assert(state.dfu_dev.dfu_state_flag==entry && !state.lock);
     assert(!state.dfu_dev.device_open_count && !state.dfu_dev.dfu_msg && !file.private_data);
    }
   }
   begin_error=0;
   assert(ds5_dfu_device_open(&inode,&file)==0);
   assert(state.dfu_dev.dfu_state_flag==(entry==DS5_DFU_RECOVERY ? entry : DS5_DFU_OPEN));
   assert(state.dfu_dev.device_open_count==1 && file.private_data==&state && !state.lock);
   free(state.dfu_dev.dfu_msg);state.dfu_dev.dfu_msg=NULL;
   state.dfu_dev.device_open_count=0;file.private_data=NULL;
  }
 }
 puts("PASS: failed DFU opens preserve entry state; allocation/session failures and retry covered");
 puts("PASS: legacy open acquires 400 kHz ownership; D585 open does not; failed open rolls back");
 puts("PASS: busy DFU enumeration does not wait for a full-image write; exclusive-open race and allocation failure remain protected");
}
"""
with tempfile.TemporaryDirectory(prefix='d585-dfu-open-') as temp:
    c = Path(temp) / 'test.c'
    binary = Path(temp) / 'test'
    c.write_text(open_shim + open_body + open_test)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                    '-Werror', '-fsanitize=address,undefined', '-g',
                    str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

# Legacy 400 kHz was available without CONFIG_VIDEO_D4XX_SERDES too.
legacy_test = r"""
int main(void) {
 struct i2c_adapter root={.rate=100000}, branch={.parent=&root};
 struct i2c_client client={.adapter=&branch};
 struct ds5_dev shared={0};
 struct ds5 state={.client=&client,.ds5_dev=&shared,.is_depth=true};
 assert(!d5xx_i2c_fast_capable(&state));
 assert(ds5_dfu_session_begin(&state)==0);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,8,0)
 assert(root.rate==400000 && state.ds5_dev->dfu_status);
 assert(ds5_check_i2c_access(&state, false)==0);
#else
 assert(root.rate==100000 && !state.ds5_dev->dfu_status);
#endif
 assert(ds5_dfu_session_end(&state)==0 && root.rate==100000 && !root.dev.refs);
 puts("PASS: non-SerDes legacy path / JP7 clock API guard");
}
"""
for version in (331961, 395264):
    with tempfile.TemporaryDirectory(prefix='legacy-dfu-config-') as temp:
        c=Path(temp)/'test.c'; binary=Path(temp)/'test'
        c.write_text(SHIM+policy+legacy_test)
        subprocess.run([os.environ.get('CC','cc'), '-std=gnu11', '-Wall', '-Wextra',
                        '-Werror', '-Wno-unused-function', '-Wno-unused-variable',
                        '-Wno-unused-parameter', '-DCONFIG_TEGRA_CAMERA_PLATFORM',
                        f'-DLINUX_VERSION_CODE={version}', '-fsanitize=address,undefined',
                        '-g', '-pthread', str(c), '-o', str(binary)], check=True)
        subprocess.run([str(binary)],check=True)

# Compile the actual ERB/EWB cases with the actual control lock entry/exit.
# Other controls are omitted so the harness can inject HWMC failures directly.
ctrl_body = source[source.index('static int ds5_s_ctrl('):source.index('static int ds5_gvd(')]
entry = ctrl_body[ctrl_body.index('\tmutex_lock(&state->lock);'):ctrl_body.index('\tdpp_desc =')]
cases = ctrl_body[ctrl_body.index('\tcase DS5_CAMERA_CID_ERB:'):ctrl_body.index('\tcase DS5_CAMERA_CID_HWMC:')]
cleanup = ctrl_body[ctrl_body.index('unlock:\n') + len('unlock:\n'):]
cleanup = cleanup[:cleanup.index('\n}') + 2]
control_shim = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
#define GFP_KERNEL 0
#define DS5_CAMERA_CID_ERB 1
#define DS5_CAMERA_CID_EWB 2
#define DS5_HWMC_BUFFER_SIZE 1024
#define dev_err(...) ((void)0)
#define dev_dbg(...) ((void)0)
struct client { int dev; };
struct ds5 { int lock; struct client *client; };
struct v4l2_ctrl { int id; unsigned int dims[1]; union { u8 *p_u8; } p_new; };
struct hwm_cmd { int header, param1, param2; u8 Data[]; };
static struct hwm_cmd erb, ewb;
static int ds5_maintenance_sem, ds5_io_sem, fault, allocations, admission;
static void mutex_lock(int *l) { assert(!*l); *l=1; }
static void mutex_unlock(int *l) { assert(*l); *l=0; }
#define down_read mutex_lock
#define up_read mutex_unlock
static int ds5_check_i2c_access(struct ds5 *s, int maintenance) {
 assert(s->lock && ds5_io_sem && ds5_maintenance_sem && !maintenance);
 return admission;
}
#define false 0
static void *devm_kzalloc(void *d, size_t n, int flags) {
 (void)d; (void)flags; if(fault==1)return NULL; allocations++; return calloc(1,n);
}
static void devm_kfree(void *d, void *p) { (void)d; assert(allocations>0); allocations--; free(p); }
static int ds5_hwmc_xfer(struct ds5 *s, size_t n, struct hwm_cmd *cmd,
 void *response, size_t capacity, u16 *size, int raw, int empty) {
 (void)s;(void)n;(void)response;(void)capacity;(void)raw;(void)empty;
 if(fault==2 || fault==3)return -EIO;
 if(size)*size=4+cmd->param2;
 return 0;
}
'''
control_test = r'''
int main(void) {
 struct client client={0}; struct ds5 state={.client=&client};
 u8 data[12]={0,0,0,8}; struct v4l2_ctrl ctrl={.dims={12},.p_new.p_u8=data};
 for(int id=1;id<=2;id++) for(fault=0;fault<=3;fault++) {
  data[2]=0; data[3]=8;
  ctrl.id=id; int ret=test_ctrl(&state,&ctrl);
  assert(ret==(fault==1 ? -ENOMEM : fault ? -EAGAIN : 0));
  assert(!state.lock && !ds5_maintenance_sem && !ds5_io_sem && !allocations);
  // A subsequent speed transition can acquire the maintenance semaphore.
  mutex_lock(&ds5_maintenance_sem); mutex_unlock(&ds5_maintenance_sem);
 }
 fault=0; admission=-EBUSY;
 assert(test_ctrl(&state,&ctrl)==-EBUSY && !state.lock && !ds5_maintenance_sem);
 puts("PASS: ERB/EWB allocation/send/response failures free buffers and release both locks");
}
'''
with tempfile.TemporaryDirectory(prefix='dfu-control-cleanup-') as temp:
    c=Path(temp)/'test.c'; binary=Path(temp)/'test'
    c.write_text(control_shim + 'static int test_ctrl(struct ds5 *state, struct v4l2_ctrl *ctrl) {\nint ret=0;\n' +
                 entry + 'switch(ctrl->id) {\n' + cases + '}\n' + cleanup + control_test)
    subprocess.run([os.environ.get('CC','cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-g', str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)],check=True)

# Firmware manifestation status is independent of timing cleanup, while close
# must still retry and report a real communication failure (including EBUSY).
finalize_body = source[source.index('static int ds5_dfu_finalize_download('):source.index('static ssize_t ds5_dfu_device_write(')]
release_body = source[source.index('static int ds5_dfu_device_release('):source.index('static const struct file_operations ds5_device_file_ops')]
completion_shim = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#define WRITE_ONCE(x,v) ((x)=(v))
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define dev_warn(...) ((void)0)
#define DS5_DFU_IDLE 0
#define DS5_DFU_RECOVERY 1
#define DS5_DFU_DONE 2
#define DS5_FW_VERSION 0
#define DS5_FW_BUILD 1
struct inode { void *i_cdev; }; struct file { int unused; };
struct client { int dev; };
enum dfu_fw_state { dfuMANIFEST_WAIT_RESET=8 };
struct ds5 {
 int lock, fw_version, fw_build; struct client *client;
 struct { int ds5_cdev, device_open_count, dfu_state_flag;
          void *dfu_msg; bool manifest_complete; enum dfu_fw_state manifest_state; } dfu_dev;
};
static int write_error, manifest_error, restore_error, end_calls, busy_reads, reads;
static void mutex_lock(int *l) { assert(!*l); *l=1; }
static void mutex_unlock(int *l) { assert(*l); *l=0; }
static int ds5_write(struct ds5 *s,int r,int v) { (void)r;(void)v;assert(s->lock);return write_error; }
static int ds5_dfu_wait_for_get_dfu_status(struct ds5 *s,enum dfu_fw_state m) {
 assert(s->lock && m==dfuMANIFEST_WAIT_RESET);return manifest_error;
}
static int ds5_dfu_session_end(struct ds5 *s) { assert(s->lock);end_calls++;return restore_error; }
static void devm_kfree(void *d,void *p) { (void)d;(void)p; }
static void msleep_range(int n) { assert(n==10); }
static int ds5_read(struct ds5 *s,int r,int *v) {
 (void)r;(void)v;assert(s->lock);reads++;return busy_reads-- > 0 ? -EBUSY : 0;
}
'''
completion_test = r'''
int main(void) {
 struct client c={0};struct ds5 s={.client=&c}; struct inode inode={.i_cdev=&s.dfu_dev.ds5_cdev};
 for(int phase=0;phase<4;phase++) {
  s.lock=1;s.dfu_dev.dfu_state_flag=1;s.dfu_dev.manifest_complete=false;
  s.dfu_dev.manifest_state=dfuMANIFEST_WAIT_RESET;end_calls=0;
  write_error=phase==0 ? -EIO : 0; manifest_error=phase==1 ? -EIO : 0;
  restore_error=phase==2 ? -EREMOTEIO : 0;
  int ret=ds5_dfu_finalize_download(&s);
  assert(ret==(phase<2 ? -EIO : 0));
  assert(s.dfu_dev.manifest_complete==(phase>=2));
  assert(s.dfu_dev.dfu_state_flag==(phase<2 ? 1 : DS5_DFU_DONE));
  assert(end_calls==(phase>=2)); s.lock=0;
 }
 restore_error=0;busy_reads=1;reads=0;s.dfu_dev.device_open_count=1;
 assert(ds5_dfu_device_release(&inode,NULL)==0 && reads==3 && !s.lock);
 busy_reads=100;reads=0;s.dfu_dev.device_open_count=1;
 assert(ds5_dfu_device_release(&inode,NULL)==-EBUSY && reads==11 && !s.lock);
 puts("PASS: completed DFU survives timing-cleanup errors; close retries transport EBUSY and reports persistent failures");
}
'''
with tempfile.TemporaryDirectory(prefix='dfu-completion-') as temp:
    c=Path(temp)/'test.c'; binary=Path(temp)/'test'
    c.write_text(completion_shim+finalize_body+release_body+completion_test)
    subprocess.run([os.environ.get('CC','cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-fsanitize=address,undefined', '-g', str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)],check=True)

# Run the actual stream transition body after a host-only stop: firmware can
# remain active while VI has released capture. Reuse both existing flag sets.
stream_body = source[source.index('static int ds5_mux_s_stream(struct v4l2_subdev *sd, int on)\n{'):
                     source.index('static int ds5_mux_get_frame_desc(')]
stream_shim = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
typedef uint16_t u16;
typedef uint32_t u32;
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define dev_dbg(...) ((void)0)
#define dev_info(...) ((void)0)
#define dev_warn(...) ((void)0)
#define MAX_DS5_CONFIG_RETRIES 3
#define DS5_START_MAX_TIME 20
#define DS5_START_POLL_TIME 1
#define DS5_DEPTH_CONFIG_STATUS 0
#define DS5_RGB_CONFIG_STATUS 1
#define DS5_IR_CONFIG_STATUS 2
#define DS5_IMU_CONFIG_STATUS 3
#define DS5_DEPTH_STREAM_STATUS 4
#define DS5_RGB_STREAM_STATUS 5
#define DS5_IR_STREAM_STATUS 6
#define DS5_IMU_STREAM_STATUS 7
#define DS5_STREAM_DEPTH 0
#define DS5_STREAM_RGB 1
#define DS5_STREAM_IR 2
#define DS5_STREAM_IMU 3
#define DS5_STREAM_START 0x100
#define DS5_STREAM_STOP 0x200
#define DS5_START_STOP_STREAM 8
#define DS5_STREAM_STREAMING 1
#define DS5_STREAM_IDLE 0
#define DS5_STATUS_STREAMING 1
#define DS5_STATUS_INVALID_DT 2
#define DS5_STATUS_INVALID_RES 4
#define DS5_STATUS_INVALID_FPS 8
#define PIPE_NOT_CONFIGURED (-1)
#define GMSL_CSI_DT_RGB_888 0x24
static unsigned long jiffies;
#define msecs_to_jiffies(x) (x)
#define time_before(a,b) ((a)<(b))
static void msleep_range(int n) { jiffies+=n+1; }
static void mutex_lock(int *m) { assert(!*m);*m=1; }
static void mutex_unlock(int *m) { assert(*m);*m=0; }
static int serdes_lock__;
struct device { int unused; };
struct v4l2_subdev { int unused; };
struct format { int data_type; };
struct ds5_sensor { bool streaming; int pipe_id; struct { struct format *format; } config; };
struct ds5_dev { int lock; bool depth_streaming,rgb_streaming,ir_streaming,imu_streaming,link_flush_pending; };
struct ser_interface { int (*stream_stop)(struct device *,u32); };
struct dser_interface { int (*release_pipe)(struct device *,int);
 int (*get_ser_vc_id)(struct device *,u32,u32);
 void (*reset_oneshot)(struct device *); void (*retrigger_datapath)(struct device *); };
struct ds5 { struct { struct { struct v4l2_subdev subdev; } sd; struct ds5_sensor *last_set; } mux;
 struct ds5_dev *ds5_dev; struct { struct ds5_sensor sensor; } ir;
 bool is_depth,is_rgb,is_y8,is_imu; int reset_ref_ds5;
 struct { int dev; } *client; struct { u32 dst_vc; } g_ctx;
 struct device *ser_dev,*dser_dev; struct ser_interface *ser_ops;struct dser_interface *dser_ops;u32 gmsl_link;
};
static int reset_gen, configs, stops, starts, releases, flushes, ser_stops;
static int stop_error, pending_ticks, stop_delay;
static bool firmware[4], fail_stop_completion;
static int *ds5_get_reset_gen(struct ds5 *s) { (void)s;return &reset_gen; }
static int atomic_read(int *v) { return *v; }
static void ds5_invalidate_sensor(struct ds5 *s,struct ds5_sensor *n) { (void)s;(void)n; }
static void ds5_config_cache_clear(struct ds5_sensor *s) { (void)s; }
static int ds5_read(struct ds5 *s,int reg,u16 *v) {
 (void)s;jiffies++;
 if(pending_ticks && !--pending_ticks)firmware[0]=false;
 *v=firmware[reg%4] ? DS5_STATUS_STREAMING : DS5_STREAM_IDLE;return 0;
}
static int ds5_write(struct ds5 *s,int reg,u16 v) {
 (void)s;assert(reg==DS5_START_STOP_STREAM);int role=v&3;
 if(v&DS5_STREAM_STOP) {
  stops++;if(stop_error)return stop_error;
  if(!fail_stop_completion) { if(stop_delay)pending_ticks=stop_delay;else firmware[role]=false; }
 } else { starts++;firmware[role]=true; }
 return 0;
}
static int ds5_configure(struct ds5 *s,bool *armed) {
 assert(!firmware[0]);configs++;*armed=true;s->mux.last_set->pipe_id=0;return 0;
}
static void ds5_flush_idle_link(struct ds5 *s) {
 if(s->ds5_dev->link_flush_pending) {
  for(int i=0;i<4;i++)assert(!firmware[i]);
  flushes++;s->ds5_dev->link_flush_pending=false;
 }
}
static int release_pipe(struct device *d,int p) { (void)d;assert(p>=0);releases++;return 0; }
static int ser_stop(struct device *d,u32 vc) { (void)d;(void)vc;ser_stops++;return 0; }
static struct ser_interface ser_ops={.stream_stop=ser_stop};
static struct dser_interface dser_ops={.release_pipe=release_pipe};
'''
stream_test = r'''
int main(void) {
 struct ds5_dev shared={0};struct ds5_sensor sensor={.pipe_id=PIPE_NOT_CONFIGURED};
 struct ds5 cam={.ds5_dev=&shared,.is_depth=true,.mux.last_set=&sensor,.ser_ops=&ser_ops,.dser_ops=&dser_ops};
 // State left by a blocked stop: VI down/pipe free, firmware still streaming.
 shared.depth_streaming=true;firmware[0]=true;
 assert(ds5_mux_s_stream(&cam.mux.sd.subdev,1)==0);
 assert(stops==1 && configs==1 && starts==1 && sensor.streaming && firmware[0]);
 assert(ds5_mux_s_stream(&cam.mux.sd.subdev,0)==0);
 assert(!sensor.streaming && !shared.depth_streaming && !firmware[0] && releases==1);
 // Firmware stop is asynchronous: wait for idle before configuring/flushing.
 shared.depth_streaming=true;firmware[0]=true;stop_delay=3;
 assert(ds5_mux_s_stream(&cam.mux.sd.subdev,1)==0 && configs==2 && !pending_ticks);
 stop_delay=0;assert(ds5_mux_s_stream(&cam.mux.sd.subdev,0)==0);
 // Sibling still physically active after its blocked stop: never flush its link.
 shared.depth_streaming=true;firmware[0]=true;shared.rgb_streaming=true;firmware[1]=true;
 int old_flushes=flushes;
 assert(ds5_mux_s_stream(&cam.mux.sd.subdev,1)==0 && flushes==old_flushes && firmware[1]);
 assert(ds5_mux_s_stream(&cam.mux.sd.subdev,0)==0);
 shared.rgb_streaming=false;firmware[1]=false;
 // Retrying stop must not be mistaken for a duplicate or release pipe -1.
 shared.depth_streaming=true;firmware[0]=true;int old_releases=releases;
 assert(ds5_mux_s_stream(&cam.mux.sd.subdev,0)==0);
 assert(!shared.depth_streaming && !firmware[0] && releases==old_releases);
 // A failed physical stop must not clear its flag or configure a new stream.
 shared.depth_streaming=true;firmware[0]=true;stop_error=-EIO;int old_configs=configs;
 assert(ds5_mux_s_stream(&cam.mux.sd.subdev,1)==-EIO);
 assert(shared.depth_streaming && !sensor.streaming && configs==old_configs);
 stop_error=0;fail_stop_completion=true;
 assert(ds5_mux_s_stream(&cam.mux.sd.subdev,1)==-ETIMEDOUT);
 assert(shared.depth_streaming && !sensor.streaming && configs==old_configs);
 fail_stop_completion=false;
 assert(ds5_mux_s_stream(&cam.mux.sd.subdev,1)==0);
 assert(ds5_mux_s_stream(&cam.mux.sd.subdev,0)==0);
 assert(!shared.lock && !serdes_lock__);
 puts("PASS: real stream body restarts/retries blocked stops, waits for firmware idle, preserves active sibling links and reports stop failures without reconfiguration");
}
'''
with tempfile.TemporaryDirectory(prefix='dfu-blocked-stop-') as temp:
    c=Path(temp)/'test.c'; binary=Path(temp)/'test'
    c.write_text(stream_shim+stream_body+stream_test)
    subprocess.run([os.environ.get('CC','cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-DCONFIG_VIDEO_D4XX_SERDES','-DCONFIG_TEGRA_CAMERA_PLATFORM',
                    '-fsanitize=address,undefined', '-g', str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)],check=True)

# Compile the complete probe, including its actual early returns/cleanup, so a
# deferred probe never initializes SerDes and every exit releases maintenance.
probe_body = source[source.index('static int ds5_probe('):
                    source.index('\n#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 8, 12)',
                                 source.index('static int ds5_probe('))]
probe_shim = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
typedef uint16_t u16;
#define EPROBE_DEFER 517
#define KERNEL_VERSION(a,b,c) (((a)<<16)+((b)<<8)+(c))
#define GFP_KERNEL 0
#define DS5_DS5U 0
#define DS5_DEPTH_CONTROL_BASE 1
#define DS5_RGB_CONTROL_BASE 2
#define DS5_DEPTH_CONTROL_STATUS 3
#define DS5_RGB_CONTROL_STATUS 4
#define DS5_FW_VERSION 1
#define DS5_FW_BUILD 2
#define DS5_DFU_MAGIC_REG 3
#define DS5_DFU_MAGIC_LSW 4
#define DS5_DFU_RECOVERY 2
#define DS5_DEVICE_TYPE_D58X 9
#define DS5_GVD_LEN_D5XX 606
#define D585_GVD_PID_OFFSET 18
#define DS5_MUX_PAD_DEPTH 0
#define DS5_MUX_PAD_COUNT 4
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define dev_warn(...) ((void)0)
#define dev_err(...) ((void)0)
#define dev_dbg(...) ((void)0)
#define dev_info(...) ((void)0)
#define mutex_init(p) (*(p)=0)
#define mutex_lock(p) ((void)(p))
#define mutex_unlock(p) ((void)(p))
#define down_read pthread_rwlock_rdlock
#define up_read pthread_rwlock_unlock
#define IS_ERR(p) ((intptr_t)(p)<0)
#define PTR_ERR(p) ((int)(intptr_t)(p))
struct device_node { int unused; };
struct device { struct device_node *of_node; int kobj; };
struct i2c_client { struct device dev; unsigned int addr; void *data; };
struct i2c_device_id { int driver_data; };
struct ds5;
struct ds5_dev { int lock; u16 d585_product_id; struct ds5 *role_inst[4]; };
struct ds5 { int lock; struct i2c_client *client; int *variant; void *vcc,*regmap;
 struct ds5_dev *ds5_dev; int reset_ref_ds5; u16 fw_version,fw_build;
 int is_depth,is_y8,is_rgb,is_imu,control_base,control_status_reg,metadata_enabled;
 struct { void *ds5_class; int dfu_state_flag; } dfu_dev;
};
static pthread_rwlock_t ds5_maintenance_sem=PTHREAD_RWLOCK_INITIALIZER;
static pthread_rwlock_t ds5_io_sem=PTHREAD_RWLOCK_INITIALIZER;
static int ds5_variants[1],ds5_regmap_config,ds5_attr_group;
enum failure { NONE, ALLOC, REG_GET, REG_ENABLE, REGMAP, SERDES, FW,
 MODE0, METADATA, CHARDEV, DEVICE_TYPE, V4L, RECOVERY, LATE_FW, FW_BUILD };
static enum failure failure;
static int admission,setup_calls,reads,reg_enabled,chr_created,role,v4l_calls;
static struct ds5 *allocated;
static struct ds5_dev shared;
static void held(void) { int ret=pthread_rwlock_trywrlock(&ds5_maintenance_sem);
 if (!ret) pthread_rwlock_unlock(&ds5_maintenance_sem); assert(ret==EBUSY); }
static int ds5_check_i2c_access(struct ds5 *s,bool maintenance) {
 held();assert(maintenance && !s->ds5_dev);
 int ret=pthread_rwlock_trywrlock(&ds5_io_sem);
 if (!ret) pthread_rwlock_unlock(&ds5_io_sem); assert(ret==EBUSY);
 return admission;
}
static void *devm_kzalloc(void *d,size_t n,int f) { (void)d;(void)f;
 allocated=failure==ALLOC ? NULL : calloc(1,n);return allocated; }
static int of_property_read_u32(void *n,const char *p,uint32_t *v) { (void)n;(void)p;(void)v;return -EINVAL; }
static void *devm_regulator_get(void *d,const char *n) { (void)d;(void)n;held();return failure==REG_GET ? (void *)(intptr_t)-EPROBE_DEFER : &reg_enabled; }
static int regulator_enable(void *p) { (void)p;held();if(failure==REG_ENABLE)return -EIO;reg_enabled++;return 0; }
static void regulator_disable(void *p) { (void)p;held();reg_enabled--; }
static void *devm_regmap_init_i2c(void *c,void *r) { (void)c;(void)r;held();return failure==REGMAP ? (void *)(intptr_t)-EIO : &reads; }
static int ds5_serdes_setup(struct ds5 *s) { held();setup_calls++;s->ds5_dev=&shared;return failure==SERDES ? -EPROBE_DEFER : 0; }
#define ds5_get_reset_gen(s) (&(s)->reset_ref_ds5)
#define atomic_read(p) (*(p))
static int ds5_read(struct ds5 *s,int r,u16 *v) { (void)s;held();reads++;*v=0;
 if(r==DS5_FW_VERSION && (failure==FW || (failure==LATE_FW && reads>1)))return -EIO;
 if(r==DS5_FW_BUILD && failure==FW_BUILD)return -EREMOTEIO;
 if(r==DS5_DFU_MAGIC_REG && failure==RECOVERY)*v=DS5_DFU_MAGIC_LSW;return 0; }
static int of_property_read_string(void *n,const char *p,const char **v) { (void)n;
 if(!strcmp(p,"cam-type")){ const char *roles[]={"Depth","Y8","RGB","IMU"};*v=roles[role];return 0; }
 *v="1";return failure==METADATA ? -EINVAL : 0; }
static struct device_node *of_get_child_by_name(void *n,const char *name) { (void)name;return failure==MODE0 ? NULL : n; }
#define of_node_put(n) ((void)(n))
static int ds5_chrdev_init(void *c,struct ds5 *s) { (void)c;held();if(failure==CHARDEV)return -ENOMEM;s->dfu_dev.ds5_class=&chr_created;chr_created++;return 0; }
static void ds5_chrdev_remove(struct ds5 *s) { (void)s;held();chr_created--; }
static void i2c_set_clientdata(struct i2c_client *c,struct ds5 *s) { c->data=s; }
static int ds5_wait_device_type(struct ds5 *s,u16 *v) { (void)s;held();*v=DS5_DEVICE_TYPE_D58X;return failure==DEVICE_TYPE ? -EIO : 0; }
#define kzalloc(n,f) calloc(1,n)
#define kfree free
static int ds5_gvd(struct ds5 *s,unsigned char *data,int n) { (void)s;(void)n;held();data[D585_GVD_PID_OFFSET]=7;return 0; }
static int ds5_v4l_init(void *c,struct ds5 *s) { (void)c;(void)s;held();v4l_calls++;return failure==V4L ? -EIO : 0; }
static int ds5_state_to_pad(struct ds5 *s) { (void)s;return role; }
static int sysfs_create_group(void *k,void *g) { (void)k;(void)g;held();return 0; }
static const struct i2c_device_id *i2c_client_get_device_id(struct i2c_client *c) __attribute__((unused));
static const struct i2c_device_id *i2c_client_get_device_id(struct i2c_client *c) { (void)c;static const struct i2c_device_id id={0};return &id; }
'''
probe_test = r'''
static int run(void) {
 struct device_node node={0};struct i2c_client c={.dev.of_node=&node};
 setup_calls=reads=reg_enabled=chr_created=v4l_calls=0;memset(&shared,0,sizeof(shared));
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,3,0)
 const struct i2c_device_id id={0};int ret=ds5_probe(&c,&id);
#else
 int ret=ds5_probe(&c);
#endif
 assert(!pthread_rwlock_trywrlock(&ds5_maintenance_sem));pthread_rwlock_unlock(&ds5_maintenance_sem);
 assert(!pthread_rwlock_trywrlock(&ds5_io_sem));pthread_rwlock_unlock(&ds5_io_sem);
 free(allocated);allocated=NULL;return ret;
}
int main(void) {
 failure=NONE;
 for(role=0;role<4;role++) {
  admission=-EBUSY;assert(run()==-EPROBE_DEFER);
  assert(!setup_calls && !reads && !reg_enabled && !chr_created && !shared.role_inst[role]);
  admission=-EIO;assert(run()==-EIO && !setup_calls && !reads);
  admission=0;assert(run()==0 && setup_calls==1 && reads>0 && shared.role_inst[role]);
 }
 role=0;
 for(failure=ALLOC;failure<=RECOVERY;failure++) {
  int expected[]={0,-ENOMEM,-EPROBE_DEFER,-EIO,-EIO,-EPROBE_DEFER,
                  -EIO,0,-EINVAL,-ENOMEM,-EIO,-EIO,0};
  assert(run()==expected[failure]);
  if(failure!=RECOVERY)assert(!reg_enabled && !chr_created);
 }
 // Fail each post-chardev identity read independently, then retry probe.
 for(role=0;role<4;role++) {
  for(failure=LATE_FW;failure<=FW_BUILD;failure++) {
   assert(run()==(failure==LATE_FW ? -EIO : -EREMOTEIO));
   assert(!reg_enabled && !chr_created && !v4l_calls && !shared.role_inst[role]);
  }
  failure=NONE;assert(run()==0 && v4l_calls==1);
 }
 puts("PASS: both late firmware-read errors release maintenance, chardev and regulator; all roles retry successfully");
 puts("PASS: probe defers before SerDes/registration on busy root; baseline retry and every exit release guards");
}
'''
with tempfile.TemporaryDirectory(prefix='d585-probe-') as temp:
    c = Path(temp) / 'test.c'
    binary = Path(temp) / 'test'
    # Preserve production macro return behavior, including in old-head controls.
    read_macro = source[source.index('#define ds5_read_with_check('):
                        source.index('#define ds5_raw_read_with_check(')]
    c.write_text(probe_shim + read_macro + probe_body + probe_test)
    for version in ('331961', '395264'):
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                        '-Werror', '-Wno-unused-but-set-variable', '-Wno-misleading-indentation',
                        '-DCONFIG_OF', '-DCONFIG_VIDEO_D4XX_SERDES', '-DCONFIG_SYSFS',
                        '-DLINUX_VERSION_CODE=' + version, '-fsanitize=address,undefined',
                        '-g', '-pthread', str(c), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
