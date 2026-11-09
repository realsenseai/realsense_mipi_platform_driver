#!/usr/bin/env python3
"""Host-only regression: compile the production REG parser arm and executors.

Run from the repository root: python3 test/standalone_tests/test_descriptor_extent.py
No NVIDIA kernel tree or camera is needed. Scratch stays inside this worktree.
"""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
driver = root / "kernel/realsense"
main = (driver / "d4xx_main.c").read_text()
parser = (driver / "d4xx_desc.c").read_text()
header = "\n".join(line for line in (driver / "d4xx_desc.h").read_text().splitlines()
                   if not line.startswith("#include"))
executors = main[main.index("static int ds5_ctrl_uvc_errno("):
                 main.index("/* HWMC transport")]
start = parser.index("if (rec->transport == RS_CTRL_XPORT_REG)", parser.index("/* rule 7:"))
end = parser.index("\n\t} else {", start)
reg_arm = parser[start:end] + "\n}"
prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <linux/videodev2.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
typedef int32_t s32; typedef int64_t s64; typedef uint64_t u64;
typedef _Bool bool;
#define true 1
#define false 0
#define BIT(n) (1U << (n))
#define U16_MAX UINT16_MAX
#define GFP_KERNEL 0
#define I2C_SMBUS_BLOCK_MAX 32
#define I2C_FUNC_I2C 1
#define DS5_HWMC_MAX_TIME 5
#define msecs_to_jiffies(n) (n)
#define time_before(a,b) ((a)<(b))
#define dev_dbg(...) ((void)0)
#define ctrl_warn(...) ((void)0)
struct i2c_adapter_quirks {
    u16 max_write_len, max_read_len, max_comb_1st_msg_len, max_comb_2nd_msg_len;
    int max_num_msgs;
};
struct i2c_adapter { const struct i2c_adapter_quirks *quirks; bool raw; };
struct client { struct i2c_adapter *adapter; };
struct device { int lock; };
struct ds5 { struct client *client; struct device *ds5_dev; };
struct v4l2_ctrl {
    bool is_ptr; u32 elems, elem_size; s32 val;
    union { u8 *p_u8; } p_new;
};
static unsigned allocated, io_count, fail_io, allocations_fail, fail_seq, seq_polls;
static u16 status_word, code_word;
static size_t last_len;
static u16 last_addr;
static u8 wire[1024];
static unsigned long jiffies;
static void *kzalloc(size_t n, int flags) {
    (void)flags; if (allocations_fail) return NULL;
    void *p = calloc(1,n); assert(p); ++allocated; return p;
}
static void *kmemdup(const void *src, size_t n, int flags) {
    void *p = kzalloc(n,flags); if(p) memcpy(p,src,n); return p;
}
static void kfree(void *p) { assert(p && allocated); --allocated; free(p); }
static void mutex_lock(int *p) { assert(!*p); ++*p; }
static void mutex_unlock(int *p) { assert(*p==1); --*p; }
static bool i2c_check_functionality(const struct i2c_adapter *a, int func) {
    (void)func; return a->raw;
}
static void msleep_range(int n) { jiffies += n; }
static u32 sign_extend32(u32 val, int bit) {
    return (s32)(val << (31-bit)) >> (31-bit);
}
static int ds5_raw_write(struct ds5 *s,u16 addr,const void *p,size_t n) {
    assert(s->ds5_dev->lock); ++io_count; last_len=n; last_addr=addr;
    if(fail_io) return -EREMOTEIO;
    memcpy(wire,p,n); return 0;
}
static int ds5_raw_read(struct ds5 *s,u16 addr,void *p,size_t n) {
    (void)s; ++io_count; last_len=n; last_addr=addr;
    memcpy(p,wire,n); return fail_io ? -EREMOTEIO : 0;
}
static int ds5_read(struct ds5 *s,u16 addr,u16 *v) {
    (void)s; if(fail_seq) return -EREMOTEIO;
    *v = addr == 0x401a ? code_word : 12; return 0;
}
static int ds5_read_poll(struct ds5 *s,u16 addr,u16 *v) {
    (void)s; ++seq_polls;
    *v = addr == 0x401e ? status_word : 13; return 0;
}
'''
parser_support = r'''
struct rs_desc_mech_reg { u16 addr, extent, rsvd; };
struct rs_desc_control {
    u16 elems, flags; u8 v4l2_type, transport;
    union { struct rs_desc_mech_reg reg; } mech;
};
struct rs_desc_stream { u16 ctrl_base, status_addr; };
static bool ds5_desc_is_array_type(u8 type) {
    return type==RS_CTRL_T_U8_ARRAY || type==RS_CTRL_T_U16_ARRAY ||
           type==RS_CTRL_T_U32_ARRAY;
}
static int parse_reg(const struct rs_desc_control *rec,
                     const struct rs_desc_stream *st, struct ds5_desc_ctrl *out) {
    struct ds5_desc_ctrl c = {0};
    const struct ds5_desc_ctrl *done = NULL;
    unsigned n_done = 0, i;
    c.elem_size = rec->v4l2_type == RS_CTRL_T_U16_ARRAY ? 2 :
                  rec->v4l2_type == RS_CTRL_T_U32_ARRAY ? 4 : 1;
'''
checks = r'''
int main(void) {
    struct i2c_adapter_quirks q = {0};
    struct i2c_adapter a = {&q,true};
    struct client client = {&a}; struct device dev = {0};
    struct ds5 state = {&client,&dev};
    u8 payload[1024], before[1024];
    for (unsigned i=0;i<sizeof(payload);++i) payload[i]=(u8)(i*17);
    struct ds5_desc_ctrl c = {.type=V4L2_CTRL_TYPE_U8,.elems=1024,.elem_size=1,
                             .reg={.addr=0x7000,.extent=1024}};
    struct v4l2_ctrl ctrl = {.is_ptr=true,.elems=1024,.elem_size=1,.p_new={payload}};
    struct rs_desc_control rec = {.elems=1024,.v4l2_type=RS_CTRL_T_U8_ARRAY,
                                  .transport=RS_CTRL_XPORT_REG,
                                  .mech={.reg={.addr=0x7000,.extent=1024}}};
    struct rs_desc_stream st = {.status_addr=0x401e};
    struct ds5_desc_ctrl parsed;
    assert(parse_reg(&rec,&st,&parsed)==0 && parsed.reg.extent==1024);
    rec.v4l2_type=RS_CTRL_T_U16_ARRAY; rec.elems=512;
    assert(parse_reg(&rec,&st,&parsed)==0);
    rec.v4l2_type=RS_CTRL_T_U32_ARRAY; rec.elems=256;
    assert(parse_reg(&rec,&st,&parsed)==0);
    rec.v4l2_type=RS_CTRL_T_U8_ARRAY;
    rec.mech.reg.extent=1025; rec.elems=1025;
    assert(parse_reg(&rec,&st,&parsed)==-EINVAL);
    rec.mech.reg.extent=0; assert(parse_reg(&rec,&st,&parsed)==-EINVAL);
    rec.mech.reg.extent=1024; rec.elems=1023;
    assert(parse_reg(&rec,&st,&parsed)==-EINVAL);
    rec.elems=1024; rec.mech.reg.addr=0xfc01;
    assert(parse_reg(&rec,&st,&parsed)==-EINVAL);
    rec.mech.reg.addr=0xfc00;
    assert(parse_reg(&rec,&st,&parsed)==0);
    assert(ds5_ctrl_reg_set(&state,&c,&ctrl)==0 && last_len==1024);
    assert(io_count==1 && last_addr==0x7000 && !memcmp(payload,wire,1024));
    memcpy(before,payload,1024); memset(payload,0,1024);
    assert(ds5_ctrl_reg_get(&state,&c,&ctrl)==0 && last_len==1024);
    assert(!memcmp(payload,before,1024));
    memset(payload,0xa5,1024); memcpy(before,payload,1024);
    fail_io=1;
    assert(ds5_ctrl_reg_get(&state,&c,&ctrl)==-EREMOTEIO);
    assert(!memcmp(payload,before,1024) && !allocated);
    assert(ds5_ctrl_reg_set(&state,&c,&ctrl)==-EREMOTEIO && !allocated && !dev.lock);
    fail_io=0; allocations_fail=1;
    assert(ds5_ctrl_reg_set(&state,&c,&ctrl)==-ENOMEM);
    assert(ds5_ctrl_reg_get(&state,&c,&ctrl)==-ENOMEM);
    allocations_fail=0; c.status_addr=0x401e; fail_seq=1;
    assert(ds5_ctrl_reg_set(&state,&c,&ctrl)==-EREMOTEIO && !allocated && !dev.lock);
    fail_seq=0;
    const struct {u16 status,code; int error;} cases[] = {
        {RS_CTRL_STATUS_OK,0,0}, {RS_CTRL_STATUS_BUSY,0,-EBUSY},
        {RS_CTRL_STATUS_ACCESS_DENIED,0,-EACCES},
        {RS_CTRL_STATUS_ERROR,8,-EINVAL}, {RS_CTRL_STATUS_ERROR,4,-ERANGE},
        {RS_CTRL_STATUS_ERROR,2,-EILSEQ}, {RS_CTRL_STATUS_ERROR,7,-EIO}
    };
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        status_word=cases[i].status; code_word=cases[i].code;
        assert(ds5_ctrl_reg_set(&state,&c,&ctrl)==cases[i].error);
        assert(!allocated && !dev.lock);
    }
    io_count=0; q.max_write_len=1025;
    assert(ds5_ctrl_reg_set(&state,&c,&ctrl)==-EMSGSIZE && !io_count);
    q.max_write_len=1026; q.max_read_len=1023;
    assert(ds5_ctrl_reg_get(&state,&c,&ctrl)==-EMSGSIZE && !io_count);
    q.max_read_len=1024; q.max_comb_2nd_msg_len=1023;
    assert(ds5_ctrl_reg_get(&state,&c,&ctrl)==-EMSGSIZE && !io_count);
    q.max_comb_2nd_msg_len=1024;
    assert(ds5_ctrl_reg_get(&state,&c,&ctrl)==0 && last_len==1024);
    a.raw=false; io_count=0;
    assert(ds5_ctrl_reg_set(&state,&c,&ctrl)==-EOPNOTSUPP && !io_count);
    a.raw=true; --ctrl.elems;
    assert(ds5_ctrl_reg_set(&state,&c,&ctrl)==-EINVAL && !io_count);
    ++ctrl.elems; c.reg.extent=1025;
    assert(ds5_ctrl_reg_set(&state,&c,&ctrl)==-EINVAL && !io_count);
    c.reg.extent=4; c.type=V4L2_CTRL_TYPE_INTEGER; c.flags=RS_CTRL_F_LSW_LATCH;
    c.status_addr=0; ctrl.is_ptr=false; ctrl.val=0x12345678; q.max_write_len=4;
    assert(ds5_ctrl_reg_set(&state,&c,&ctrl)==0 && io_count==2 && last_len==2);
    assert(!allocated && !dev.lock);
    puts("PASS: descriptor bounds, atomic 1024-byte REG sizing, cleanup and completion errno");
}
'''
out = root / ".build-descriptor-extent"
out.mkdir(exist_ok=True)
cfile, exe = out / "extent.c", out / "extent"
cfile.write_text(prefix + header + "\n" + executors + parser_support + reg_arm +
                 "\n*out=c; return 0;\n}\n" + checks)
subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-g", str(cfile), "-o", str(exe)], check=True)
subprocess.run([str(exe)], check=True)
cfile.unlink()
exe.unlink()
out.rmdir()
