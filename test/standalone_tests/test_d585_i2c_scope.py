#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the production D585 policy and SerDes helpers with mocked I2C."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def function(source, name):
    match = re.search(r'^(?:static )?(?:inline )?(?:int|bool) ' + name + r'\(', source, re.M)
    start = source.index('{', match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


def defines(source, prefix):
    lines = source.splitlines(True)
    result = []
    for i, line in enumerate(lines):
        if line.startswith('#define ' + prefix):
            result.append(line)
            while line.rstrip().endswith('\\'):
                i += 1
                line = lines[i]
                result.append(line)
    return ''.join(result)


PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
#define EXPORT_SYMBOL(x)
#define dev_err(...) ((void)0)
#define dev_warn(...) ((void)0)
#define READ_ONCE(x) (x)
struct device { void *priv; };
struct i2c_client { struct device dev; };
static void *dev_get_drvdata(struct device *d) { return d->priv; }
static int held, reads, writes, delays, fail_read, fail_write;
static unsigned int regs[4096];
static void mutex_lock(int *lock) { assert(!*lock && !held); *lock=1; held=1; }
static void mutex_unlock(int *lock) { assert(*lock && held); *lock=0; held=0; }
static void usleep_range(int lo,int hi) { assert(lo==100 && hi==110); ++delays; }
static int regmap_read(void *map,unsigned addr,unsigned *out) {
 (void)map;assert(held && addr<4096);++reads;
 if(fail_read)return -EREMOTEIO;
 *out=regs[addr];return 0;
}
static int regmap_write(void *map,unsigned addr,unsigned value) {
 (void)map;assert(held && addr<4096);++writes;regs[addr]=value;
 return fail_write ? -EIO : 0; /* A failed transfer may have written the byte. */
}
'''
SERDES_TEST = r'''
static void reset(struct CHIP *priv) {
 memset(priv,0,sizeof(*priv));priv->regmap=regs;
 reads=writes=delays=held=fail_read=fail_write=0;
 for(unsigned i=0;i<4096;++i)regs[i]=0x25;
}
int main(void) {
 struct CHIP priv;struct device dev={&priv}, unbound={0};struct i2c_client client;
 reset(&priv);
 assert(SET(NULL,0,true)==-EINVAL && !held && !reads && !writes);
 assert(SET(&unbound,0,true)==-ENODEV && !held && !reads && !writes);
 priv.regmap=NULL;
 assert(SET(&dev,0,true)==-ENODEV && !held && !reads && !writes);
 for(unsigned link=0;link<LINKS;++link)for(unsigned initial=0;initial<256;++initial) {
  reset(&priv);priv.i2c_client=&client;regs[ADDR(link)]=initial;
  assert(SET(&dev,link,false)==0 && !reads && !writes);
  assert(SET(&dev,link,true)==0 && regs[ADDR(link)]==((initial&~0x70)|0x70));
  assert(reads==1 && writes==1 && delays==1 && !held);
  for(unsigned other=0;other<LINKS;++other)
   if(other!=link)assert(regs[ADDR(other)]==0x25);
  regs[ADDR(link)]&=~0x70U; /* Reassert after hardware reset, retain the saved original. */
  assert(SET(&dev,link,true)==0 && regs[ADDR(link)]==((initial&~0x70)|0x70));
  assert(SET(&dev,link,false)==0 && regs[ADDR(link)]==initial && !held);
  assert(delays==writes);
 }
 reset(&priv);priv.i2c_client=&client;fail_read=1;
 assert(SET(&dev,0,true)==-EREMOTEIO && !writes && !held);
 fail_read=0;assert(SET(&dev,0,false)==0 && !writes);
 fail_write=1;assert(SET(&dev,0,true)==-EIO && !held);
 fail_write=0;assert(SET(&dev,0,false)==0 && regs[ADDR(0)]==0x25);
 assert(SET(&dev,0,true)==0);fail_write=1;
 assert(SET(&dev,0,false)==-EIO && !held);fail_write=0;
 assert(SET(&dev,0,false)==0 && regs[ADDR(0)]==0x25);
#if LINKS > 1
 reset(&priv);priv.i2c_client=&client;
 assert(SET(&dev,0,true)==0 && SET(&dev,2,true)==0);
 assert(SET(&dev,0,false)==0 && regs[ADDR(0)]==0x25 && regs[ADDR(2)]==0x75);
 assert(regs[ADDR(1)]==0x25 && regs[ADDR(3)]==0x25);
 int count=reads;assert(SET(&dev,LINKS,true)==-EINVAL && reads==count);
#endif
 puts("PASS: all register bytes/links, unrelated-link isolation, reset, rollback, errors, delays and locks");
}
'''
POLICY_PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
typedef uint16_t u16;typedef uint32_t u32;
#define READ_ONCE(x) (x)
#define dev_warn(...) ((void)0)
struct device { int unused; };
struct ser_interface { int (*set_i2c_fast_mode)(struct device *,bool); };
struct dser_interface { int (*set_i2c_fast_mode)(struct device *,u32,bool); };
struct ds5_dev { u16 cached_device_type,d585_product_id; };
struct ds5 {
 struct ds5_dev *ds5_dev;const struct ser_interface *ser_ops;const struct dser_interface *dser_ops;
 struct device *ser_dev,*dser_dev;u32 gmsl_link;
};
static int ser_on,ser_off,dser_on,dser_off,fail_ser,fail_dser,fail_ser_restore,fail_dser_restore;static u32 last_link;
static int ser_set(struct device *dev,bool enable) {
 (void)dev;if(enable){++ser_on;return fail_ser;}++ser_off;return fail_ser_restore;
}
static int dser_set(struct device *dev,u32 link,bool enable) {
 (void)dev;last_link=link;if(enable){++dser_on;return fail_dser;}++dser_off;return fail_dser_restore;
}
'''
POLICY_TEST = r'''
int main(void) {
 struct ds5_dev camera={0};struct ser_interface ser={ser_set};struct dser_interface dser={dser_set};
 struct ds5 s={.ds5_dev=&camera,.ser_ops=&ser,.dser_ops=&dser,.gmsl_link=2};
 for(unsigned type=0;type<16;++type)for(unsigned pid=0;pid<65536;++pid) {
  camera.cached_device_type=type;camera.d585_product_id=pid;
  ser_on=dser_on=ser_off=dser_off=0;
  assert(ds5_configure_d585_i2c(&s,true)==0);
  bool wanted=(type==DS5_DEVICE_TYPE_D58X && (pid==D585_2C_PROTO_PID || pid==D585_3C_PROTO_PID));
  assert(ser_on==wanted && dser_on==wanted && !ser_off && !dser_off);
  if(wanted)assert(last_link==2);
 }
 camera.cached_device_type=DS5_DEVICE_TYPE_D58X;camera.d585_product_id=D585_3C_PROTO_PID;
 ser_on=dser_on=ser_off=dser_off=0;fail_ser=-EIO;
 assert(ds5_configure_d585_i2c(&s,true)==0 && ser_on==1 && !dser_on && ser_off==1 && dser_off==1);
 fail_ser=0;fail_dser=-EREMOTEIO;ser_off=dser_off=0;
 assert(ds5_configure_d585_i2c(&s,true)==0 && ser_off==1 && dser_off==1);
 for(int ser_error=0;ser_error<2;++ser_error)for(int dser_error=0;dser_error<2;++dser_error) {
  fail_ser_restore=ser_error ? -EIO : 0;fail_dser_restore=dser_error ? -EBUSY : 0;
  ser_off=dser_off=0;
  assert(ds5_configure_d585_i2c(&s,true)==((ser_error || dser_error) ? -EREMOTEIO : 0));
  assert(ser_off==1 && dser_off==1);
 }
 fail_dser=0;fail_ser_restore=fail_dser_restore=0;
 assert(ds5_configure_d585_i2c(&s,false)==0);
 fail_ser_restore=-EIO;fail_dser_restore=-EBUSY;ser_off=dser_off=0;
 assert(ds5_configure_d585_i2c(&s,false)==-EIO && ser_off==1 && dser_off==1);
 fail_ser_restore=fail_dser_restore=0;
 fail_dser=0;ser.set_i2c_fast_mode=0;ser_on=dser_on=0;
 assert(ds5_configure_d585_i2c(&s,true)==0 && !ser_on && !dser_on);
 puts("PASS: all 16 device types x 65536 PIDs, recovery/unknown, exact D585 PIDs, selected link and rollback");
}
'''



RELEASE_PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#define dev_warn(...) ((void)0)
#define MAX_DS5_NUM 2
struct ds5;
struct dser_control { int lock;void *dser_dev; };
struct ds5_dev { int lock;struct ds5 *ds5_primary;bool serdes_setup_complete;struct dser_control *dser_control; };
struct ds5 { struct ds5_dev *ds5_dev;void *dser_dev; };
static struct ds5_dev ds5_inited[MAX_DS5_NUM];
static int serdes_lock__, restores, restore_error;
static void mutex_lock(int *p) { assert(!*p);*p=1; }
static void mutex_unlock(int *p) { assert(*p);*p=0; }
static int ds5_configure_d585_i2c(struct ds5 *state,bool enable) {
 assert(!enable && serdes_lock__ && !state->ds5_dev->lock);
 assert(!state->ds5_dev->ds5_primary);
 ++restores;return restore_error;
}
'''
RELEASE_TEST = r'''
int main(void) {
 int device;
 struct dser_control control={.dser_dev=&device};
 struct ds5 primary={.ds5_dev=&ds5_inited[0],.dser_dev=&device};
 struct ds5 sibling={.ds5_dev=&ds5_inited[0],.dser_dev=&device}, absent={0};
 assert(!ds5_release_slot(&absent) && !restores && !serdes_lock__);
 ds5_inited[0].ds5_primary=&primary;ds5_inited[0].dser_control=&control;
 assert(!ds5_release_slot(&sibling) && !restores && !serdes_lock__);
 ds5_inited[1].ds5_primary=&sibling;ds5_inited[1].dser_control=&control;
 assert(ds5_release_slot(&primary) && restores==1 && !serdes_lock__);
 assert(control.dser_dev==&device); /* Other cameras still own the deserializer. */
 assert(!ds5_release_slot(&primary) && restores==1);
 ds5_inited[1].ds5_primary=NULL;
 ds5_inited[0].ds5_primary=&primary;ds5_inited[0].dser_control=&control;restore_error=-1;
 assert(ds5_release_slot(&primary) && restores==2 && !control.dser_dev && !serdes_lock__);
 ds5_inited[0].ds5_primary=&primary;
 assert(ds5_release_slot(&primary) && restores==3 && !serdes_lock__);
 puts("PASS: primary-only restoration, slot lock released during I2C, topology lock retained, shared ownership and error cleanup");
}
'''

def run_test(text, directory, name):
    src = directory / (name + '.c')
    src.write_text(text)
    exe = directory / name
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-variable', '-fsanitize=address,undefined',
                    str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--max96712-source', type=Path, required=True,
                        help='Applied max96712.c for the desired JetPack patch carrier')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix='d585-i2c-scope-') as tmp:
        directory = Path(tmp)
        for chip in (96712, 96717, 96724):
            source = (args.max96712_source if chip == 96712 else
                      root / f'nvidia-oot/max{chip}.c').read_text()
            suffix = '' if chip == 96717 else '[4]'
            context = f'''struct max{chip} {{
 struct i2c_client *i2c_client;void *regmap;int lock;
 bool i2c_fast_mode{suffix};u8 i2c_saved_mst_bt{suffix};
}};
'''
            names = f'#define CHIP max{chip}\n#define LINKS {1 if chip==96717 else 4}\n'
            if chip == 96717:
                names += '#define ADDR(link) MAX96717_I2C1_ADDR\n#define SET(dev,link,on) max96717_set_i2c_fast_mode(dev,on)\n'
            else:
                names += f'#define ADDR(link) MAX{chip}_I2C_P0_MASTER_ADDR(link)\n#define SET(dev,link,on) max{chip}_set_i2c_fast_mode(dev,link,on)\n'
            production = (function(source, 'max96712_read_reg') if chip == 96712 else '')
            production += function(source, f'max{chip}_write_reg') + function(source, f'max{chip}_set_i2c_fast_mode')
            constants = defines(source, f'MAX{chip}_I2C') + defines(source, f'MAX{chip}_MAX_LINKS')
            run_test(PREFIX + constants + context + production + names + SERDES_TEST, directory, f'max{chip}')
        source = (root / 'kernel/realsense/d4xx.c').read_text()
        constants = (defines(source, 'DS5_DEVICE_TYPE_D58X') + defines(source, 'D585_2C_PROTO_PID')
                     + defines(source, 'D585_3C_PROTO_PID'))
        assert '#define DFU_I2C_FAST_MODE\t\t\t400000' in source
        assert '#define DFU_I2C_BUS_CLK_RATE\t\tDFU_I2C_FAST_MODE' in source
        production = function(source, 'ds5_is_d58x') + function(source, 'ds5_is_d585_proto') + function(source, 'ds5_configure_d585_i2c')
        run_test(POLICY_PREFIX + constants + production + POLICY_TEST, directory, 'policy')
        run_test(RELEASE_PREFIX + function(source, 'ds5_release_slot') + RELEASE_TEST, directory, 'release')


if __name__ == '__main__':
    main()
