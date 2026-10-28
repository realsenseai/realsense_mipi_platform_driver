#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run production SerDes timing helpers with mocked I2C; all bytes/links and faults."""
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


if __name__ == '__main__':
    main()
