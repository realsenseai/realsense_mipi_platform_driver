#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the production stream transition and cache clear after H_RST_N.

Platform/I2C boundaries are modeled; the actual stream/stop/cache bodies are
compiled with ASan/UBSan. No board access. Run with ASAN_OPTIONS=detect_leaks=0
when the execution environment uses ptrace.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/realsense/d4xx_main.c').read_text()
cache = source[source.index('static void ds5_config_cache_clear('):
               source.index('static void ds5_invalidate_sensor(')]
stream_start = source.index('static int ds5_mux_s_stream(struct v4l2_subdev *sd, int on)\n{')
helpers = source[source.rindex('static int ds5_release_dser_pipe('):stream_start]
stream = source[stream_start:source.index('static int ds5_mux_get_frame_desc(')]
SHIM = r'''
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
struct ds5_sensor { bool streaming,ser_stop_pending; int pipe_id; u16 pipe_data_type1,pipe_data_type2,pipe_vc_id; struct { struct format *format; } config;
 u16 cached_dt_value,cached_md_value,cached_override_value,cached_fps_value;
 u16 cached_width_value,cached_height_value; };
struct ds5_dev { int lock; bool depth_streaming,rgb_streaming,ir_streaming,imu_streaming,link_flush_pending; };
struct ser_interface { int (*stream_stop)(struct device *,u32); };
struct dser_interface { int (*release_pipe)(struct device *,int);
 int (*get_ser_vc_id)(struct device *,u32,u32);
 void (*reset_oneshot)(struct device *); void (*retrigger_datapath)(struct device *); };
struct ds5 { struct { struct { struct v4l2_subdev subdev; } sd; struct ds5_sensor *last_set; } mux;
 struct ds5_dev *ds5_dev; struct { struct ds5_sensor sensor; } ir;
 bool is_depth,is_rgb,is_y8,is_imu,d58x; int reset_ref_ds5;
 struct { int dev; } *client; struct { u32 dst_vc; } g_ctx;
 struct device *ser_dev,*dser_dev; struct ser_interface *ser_ops;struct dser_interface *dser_ops;u32 gmsl_link;
};
static int reset_gen, configs, stops, starts, releases, flushes, ser_stops;
static int stop_error, pending_ticks, stop_delay;
static int hw_inits, hw_init_error, firmware_width;
static bool firmware[4], fail_stop_completion, baseline_reset_case;
static int *ds5_get_reset_gen(struct ds5 *s) { (void)s;return &reset_gen; }
static int atomic_read(int *v) { return *v; }
static void ds5_invalidate_sensor(struct ds5 *s,struct ds5_sensor *n) { (void)s;(void)n; }
static void ds5_config_cache_clear(struct ds5_sensor *s);
static inline bool ds5_is_d58x(struct ds5 *s) { return s->d58x; }
static inline int ds5_hw_init(void *client, struct ds5 *s) {
 (void)client;assert(s->ds5_dev->lock);
 assert(!s->ds5_dev->depth_streaming && !s->ds5_dev->rgb_streaming &&
        !s->ds5_dev->ir_streaming && !s->ds5_dev->imu_streaming);
 hw_inits++;return hw_init_error;
}
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
 assert(!firmware[0] || baseline_reset_case);configs++;*armed=true;s->mux.last_set->pipe_id=0;
 if(s->d58x && s->mux.last_set->cached_width_value!=640) {
  firmware_width=640;s->mux.last_set->cached_width_value=640;
 }
 return 0;
}
static void ds5_flush_idle_link(struct ds5 *s) {
 if(s->ds5_dev->link_flush_pending) {
  for(int i=0;i<4;i++)assert(!firmware[i] || baseline_reset_case);
  flushes++;s->ds5_dev->link_flush_pending=false;
 }
}
static int release_pipe(struct device *d,int p) { (void)d;assert(p>=0);releases++;return 0; }
static int ser_stop(struct device *d,u32 vc) { (void)d;assert(!firmware[vc]);ser_stops++;return 0; }
static struct ser_interface ser_ops={.stream_stop=ser_stop};
static struct dser_interface dser_ops={.release_pipe=release_pipe};

static int ds5_io_sem;
static void ds5_dfu_read_lock(int *p) { (void)p; }
static void ds5_dfu_read_unlock(int *p) { (void)p; }
static bool ds5_reset_oneshot_link(struct ds5 *s) { (void)s;return true; }
'''
TEST = r'''
int main(void) {
 struct ds5_dev shared={0};
 struct ds5_sensor sensor={.pipe_id=PIPE_NOT_CONFIGURED,.cached_width_value=640};
 struct ds5 cam={.ds5_dev=&shared,.is_depth=true,.d58x=true,
  .mux.last_set=&sensor,.ser_ops=&ser_ops,.dser_ops=&dser_ops};
 // The host requests the same 640 width after each unannounced HKR reset.
 for(int reset=0;reset<3;reset++) {
  firmware_width=848;
  assert(ds5_mux_s_stream(&cam.mux.sd.subdev,1)==0);
  assert(firmware_width==640 && hw_inits==reset+1);
  assert(sensor.cached_dt_value==0xffff && firmware[0] && sensor.streaming);
  int old_configs=configs;
  assert(ds5_mux_s_stream(&cam.mux.sd.subdev,1)==0);
  assert(configs==old_configs && hw_inits==reset+1); /* duplicate start */
  assert(ds5_mux_s_stream(&cam.mux.sd.subdev,0)==0);
  assert(!sensor.streaming && !shared.depth_streaming && !firmware[0]);
 }
 // Refresh a joining role's FW registers without disturbing a live sibling.
 shared.rgb_streaming=true;firmware[1]=true;firmware_width=848;
 int old_inits=hw_inits,old_flushes=flushes;
 assert(ds5_mux_s_stream(&cam.mux.sd.subdev,1)==0);
 assert(firmware_width==640 && hw_inits==old_inits && flushes==old_flushes);
 assert(firmware[1] && shared.rgb_streaming);
 assert(ds5_mux_s_stream(&cam.mux.sd.subdev,0)==0);
 shared.rgb_streaming=false;firmware[1]=false;
 // Restore errors are returned before START and leave the camera lock free.
 hw_init_error=-EIO;int old_configs=configs,old_starts=starts;
 assert(ds5_mux_s_stream(&cam.mux.sd.subdev,1)==-EIO);
 assert(!shared.lock && !shared.depth_streaming && !sensor.streaming);
 assert(configs==old_configs && starts==old_starts);
 hw_init_error=0;
 // Existing D4xx starts preserve their cache and do not initialize HKR MIPI.
 cam.d58x=false;sensor.cached_dt_value=0x31;old_inits=hw_inits;
 assert(ds5_mux_s_stream(&cam.mux.sd.subdev,1)==0);
 assert(hw_inits==old_inits && sensor.cached_dt_value==0x31);
 assert(ds5_mux_s_stream(&cam.mux.sd.subdev,0)==0);
 assert(!shared.lock && !serdes_lock__);
 puts("PASS: repeated same-format H_RST_N cache recovery; duplicate start; active-peer isolation; MIPI error propagation; D4xx cache preserved");
}
'''

with tempfile.TemporaryDirectory(prefix='d58x-reset-config-') as temp:
    c = Path(temp) / 'test.c'
    binary = Path(temp) / 'test'
    c.write_text(SHIM + cache + helpers + stream + TEST)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                    '-Werror', '-DCONFIG_VIDEO_D4XX_SERDES', '-DCONFIG_TEGRA_CAMERA_PLATFORM',
                    '-fsanitize=address,undefined', '-g', str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
