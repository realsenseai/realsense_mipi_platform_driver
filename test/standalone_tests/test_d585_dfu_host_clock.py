#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the production DFU clock policy, root-adapter lease and open/close."""
import re
import subprocess
import tempfile
from pathlib import Path


def function(source, name):
    match = re.search(r'^(?:static )?(?:inline )?(?:int|bool) ' + name + r'\(', source, re.M)
    start = source.index('{', match.start())
    end, depth = start + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#define CONFIG_VIDEO_D4XX_SERDES 1
typedef uint16_t u16; typedef uint32_t u32;
#define GFP_KERNEL 0
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define i2c_adapter_id(a) ((a)->id)
#define dev_info(...) ((void)0)
#define dev_err(...) ((void)0)
#define dev_warn(...) ((void)0)
#define DEFINE_MUTEX(name) int name
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
struct list_head { struct list_head *next,*prev; };
#define LIST_HEAD(name) struct list_head name={&name,&name}
#define list_for_each_entry(pos,head,member) \
 for(struct list_head *cursor=(head)->next; \
     cursor!=(head) && ((pos)=container_of(cursor,__typeof__(*(pos)),member)); \
     cursor=cursor->next)
static void list_add(struct list_head *n,struct list_head *head) {
 n->next=head->next;n->prev=head;head->next->prev=n;head->next=n;
}
static void list_del(struct list_head *n) {n->prev->next=n->next;n->next->prev=n->prev;}
static void mutex_lock(int *p) {assert(!*p);*p=1;}
static void mutex_unlock(int *p) {assert(*p);*p=0;}
struct device {int references;u32 *of_node;};
static int of_property_read_u32(u32 *node,const char *name,u32 *value) {
 assert(!strcmp(name,"clock-frequency"));if(!node)return -ENOENT;*value=*node;return 0;
}
struct i2c_adapter {struct device dev;struct i2c_adapter *parent;int rate,id;};
struct i2c_client {struct device dev;struct i2c_adapter *adapter;};

struct ds5_dev {u16 cached_device_type,d585_product_id;};
struct ser_interface {int (*set_i2c_fast_mode)(struct device *,bool);};
struct dser_interface {int (*set_i2c_fast_mode)(struct device *,u32,bool);};
struct cdev {int unused;};
struct ds5_dfu_dev {
 struct cdev ds5_cdev;int device_open_count,dfu_state_flag,manifest_state;
 bool manifest_complete;unsigned char *dfu_msg;struct ds5_dfu_i2c_clock *i2c_clock;
};
struct ds5 {
 struct ds5_dev *ds5_dev;struct i2c_client *client;
 const struct ser_interface *ser_ops;const struct dser_interface *dser_ops;
 struct ds5_dfu_dev dfu_dev;int lock;u16 fw_version,fw_build;
};
struct inode {struct cdev *i_cdev;};
struct file {void *private_data;};
#define DFU_BLOCK_SIZE 1024
#define DS5_DFU_RECOVERY 1
#define DS5_DFU_OPEN 2
#define DS5_DFU_IDLE 3
#define dfuMANIFEST 4
#define dfuMANIFEST_WAIT_RESET 5
#define DS5_FW_VERSION 0x30c
#define DS5_FW_BUILD 0x30e
static int fail_alloc,allocations,sets,fail_set,fail_get,fail_read;
static void *kzalloc(size_t n,int flags) {
 (void)flags;if(fail_alloc && --fail_alloc==0)return NULL;
 void *p=calloc(1,n);assert(p);++allocations;return p;
}
static void kfree(void *p) {if(p){--allocations;free(p);}}
static void *devm_kzalloc(struct device *d,size_t n,int flags) {(void)d;return kzalloc(n,flags);}
static void devm_kfree(struct device *d,void *p) {(void)d;kfree(p);}
static struct i2c_adapter *i2c_parent_is_i2c_adapter(struct i2c_adapter *a) {return a->parent;}
static int i2c_get_adapter_bus_clk_rate(struct i2c_adapter *a) {return fail_get ? fail_get : a->rate;}
static int i2c_set_adapter_bus_clk_rate(struct i2c_adapter *a,int rate) {
 ++sets;if(fail_set)return fail_set;a->rate=rate;return 0;
}
static void get_device(struct device *d) {++d->references;}
static void put_device(struct device *d) {--d->references;assert(d->references>=0);}
static int ds5_read(struct ds5 *s,int reg,u16 *v) {(void)s;(void)reg;*v=1;return fail_read;}
static void msleep_range(int ms) {(void)ms;}
static int ser_set(struct device *d,bool e) {(void)d;(void)e;return 0;}
static int dser_set(struct device *d,u32 link,bool e) {(void)d;(void)link;(void)e;return 0;}
'''

TEST = r'''
int main(void) {
 u32 fast=980000,baseline=400000,invalid=1000000;
 struct i2c_adapter root={.rate=980000,.id=2},mux={.parent=&root},leaf={.parent=&mux};
 struct i2c_adapter other={.rate=100000,.id=3};
 struct i2c_client a={.adapter=&leaf},b={.adapter=&mux},c={.adapter=&other};
 struct ds5_dev identity={.cached_device_type=DS5_DEVICE_TYPE_D58X,.d585_product_id=D585_3C_PROTO_PID};
 struct ds5_dev unknown={0};
 struct ser_interface ser={ser_set};struct dser_interface dser={dser_set};
 struct ds5 s={.ds5_dev=&identity,.client=&a,.ser_ops=&ser,.dser_ops=&dser};
 struct ds5 neighbor={.ds5_dev=&unknown,.client=&b,.ser_ops=&ser,.dser_ops=&dser};
 struct ds5 separate={.ds5_dev=&unknown,.client=&c,.ser_ops=&ser,.dser_ops=&dser};
 root.dev.of_node=&fast;
 assert(ds5_dfu_host_clock_rate(&root)==980000);
 root.dev.of_node=NULL;leaf.dev.of_node=&fast;
 assert(ds5_dfu_host_clock_rate(&root)==400000);
 root.dev.of_node=&baseline;assert(ds5_dfu_host_clock_rate(&root)==400000);
 root.dev.of_node=&invalid;assert(ds5_dfu_host_clock_rate(&root)==400000);
 root.dev.of_node=&fast;
 struct inode inode={&s.dfu_dev.ds5_cdev},inode_b={&neighbor.dfu_dev.ds5_cdev};
 struct file file={0},file_b={0};
 fail_alloc=1;assert(ds5_dfu_device_open(&inode,&file)==-ENOMEM);
 assert(!s.dfu_dev.device_open_count && !s.dfu_dev.i2c_clock && !s.dfu_dev.dfu_msg && !s.lock && !allocations);
 fail_alloc=2;assert(ds5_dfu_device_open(&inode,&file)==-ENOMEM);
 assert(!s.dfu_dev.device_open_count && !s.dfu_dev.i2c_clock && !s.dfu_dev.dfu_msg && !s.lock && !allocations);
 fail_alloc=0;
 assert(ds5_dfu_device_open(&inode,&file)==0 && file.private_data==&s);
 assert(root.rate==980000 && s.dfu_dev.i2c_clock->adapter==&root && root.dev.references==1);
 assert(s.dfu_dev.i2c_clock->saved_rate==980000 && !mux.rate && !leaf.rate);
 assert(ds5_dfu_device_open(&inode,&file)==-EBUSY);
 assert(ds5_dfu_device_open(&inode_b,&file_b)==0 && root.rate==980000);
 assert(s.dfu_dev.i2c_clock==neighbor.dfu_dev.i2c_clock && root.dev.references==1);
 assert(s.dfu_dev.i2c_clock->users==2);
 assert(ds5_dfu_host_clock_acquire(&separate)==0 && other.rate==400000);
 assert(ds5_dfu_host_clock_release(&separate)==0 && other.rate==100000 && !other.dev.references);
 assert(ds5_dfu_device_release(&inode,&file)==0 && root.rate==980000);
 assert(ds5_dfu_device_release(&inode_b,&file_b)==0 && root.rate==980000 && !root.dev.references && !allocations);
 assert(!s.dfu_dev.device_open_count && !s.dfu_dev.i2c_clock && !s.dfu_dev.dfu_msg);
 assert(ds5_dfu_host_clock_release(&s)==0);
 // Legacy 100 kHz DT: concurrent DFU opens retain 400 kHz until the last close.
 root.dev.of_node=NULL;root.rate=100000;
 assert(ds5_dfu_device_open(&inode_b,&file_b)==0 && root.rate==400000);
 assert(ds5_dfu_device_open(&inode,&file)==0 && root.rate==400000);
 assert(s.dfu_dev.i2c_clock->saved_rate==100000);
 assert(ds5_dfu_device_release(&inode_b,&file_b)==0 && root.rate==400000);
 assert(ds5_dfu_device_release(&inode,&file)==0 && root.rate==100000 && !allocations);
 // Mixed profile uses the baseline for both identified and unknown cameras.
 root.dev.of_node=&baseline;root.rate=400000;
 assert(ds5_dfu_device_open(&inode,&file)==0 && root.rate==400000);
 assert(ds5_dfu_device_open(&inode_b,&file_b)==0 && root.rate==400000);
 assert(ds5_dfu_device_release(&inode,&file)==0 && root.rate==400000);
 assert(ds5_dfu_device_release(&inode_b,&file_b)==0 && root.rate==400000 && !allocations);
 // A statically configured Fast-mode Plus bus stays fast during recovery too.
 root.dev.of_node=&fast;root.rate=980000;s.dfu_dev.dfu_state_flag=DS5_DFU_RECOVERY;identity.d585_product_id=0;
 assert(ds5_dfu_device_open(&inode,&file)==0 && root.rate==980000);
 assert(s.dfu_dev.dfu_state_flag==DS5_DFU_RECOVERY);
 fail_read=-EREMOTEIO;
 assert(ds5_dfu_device_release(&inode,&file)==-EREMOTEIO && root.rate==980000 && !allocations);
 fail_read=0;identity.d585_product_id=D585_3C_PROTO_PID;
 fail_get=-EIO;assert(ds5_dfu_device_open(&inode,&file)==-EIO && !s.dfu_dev.device_open_count && !allocations);
 fail_get=0;root.rate=0;
 assert(ds5_dfu_device_open(&inode,&file)==-EINVAL && !s.dfu_dev.device_open_count && !allocations);
 root.rate=980000;fail_set=-EIO;
 assert(ds5_dfu_device_open(&inode,&file)==-EIO && !s.dfu_dev.device_open_count && !allocations && root.rate==980000);
 fail_set=0;
 assert(ds5_dfu_device_open(&inode,&file)==0 && root.rate==980000);
 fail_set=-EIO;
 assert(ds5_dfu_device_release(&inode,&file)==-EIO && !s.dfu_dev.i2c_clock && !root.dev.references && !allocations);
 fail_set=0;
 assert(ds5_dfu_i2c_clocks.next==&ds5_dfu_i2c_clocks && !ds5_dfu_i2c_clock_lock);
 puts("PASS: continuous Fast-mode Plus preserved, physical root/mux scope, concurrent DFU opens, legacy/mixed clock policy, recovery, allocation/error cleanup and last-close restoration");
}
'''


def main():
    root = Path(__file__).resolve().parents[2]
    source = (root / 'kernel/realsense/d4xx.c').read_text()
    constants = '\n'.join(line for line in source.splitlines()
                          if line.startswith(('#define DS5_DEVICE_TYPE_D58X',
                                              '#define D585_2C_PROTO_PID',
                                              '#define D585_3C_PROTO_PID',
                                              '#define DFU_I2C_')))
    functions = ''.join(function(source, name) for name in (
        'ds5_is_d58x', 'ds5_dfu_host_clock_rate',
        'ds5_dfu_host_clock_acquire', 'ds5_dfu_host_clock_release',
        'ds5_dfu_device_open', 'ds5_dfu_device_release'))
    clock_type = source[source.index('struct ds5_dfu_i2c_clock {'):source.index('};', source.index('struct ds5_dfu_i2c_clock {')) + 2] + '\n'
    globals_ = clock_type + 'static DEFINE_MUTEX(ds5_dfu_i2c_clock_lock);\nstatic LIST_HEAD(ds5_dfu_i2c_clocks);\n'
    with tempfile.TemporaryDirectory(prefix='d585-dfu-clock-') as work:
        directory = Path(work)
        c = directory / 'test.c'
        c.write_text(PREFIX + constants + '\n' + globals_ + functions + TEST)
        exe = directory / 'test'
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-fsanitize=address,undefined',
                        str(c), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
