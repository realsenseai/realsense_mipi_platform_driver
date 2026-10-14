"""Host fault tests for the production MAX96712 DT parser/init sequence.

Run: python3 -m unittest discover -s test -p test_max96712_output_rate.py
Only kernel/DT/I2C primitives are mocked; functions are extracted from the patch.
"""
import pathlib
import re
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
PATCH = ROOT / 'nvidia-oot/6.0/0003-Adding-max96712-support-for-D4xx.patch'


def function(source, name):
    match = re.search(r'^(?:static )?int ' + name + r'\(', source, re.M)
    start = match.start()
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class OutputRateTest(unittest.TestCase):
    def test_production_parser_and_i2c_failures(self):
        source = '\n'.join(line[1:] for line in PATCH.read_text().splitlines()
                           if line.startswith('+') and not line.startswith('+++'))
        defines = '\n'.join(line for line in source.splitlines()
                            if line.startswith('#define MAX96712_'))
        functions = '\n'.join(function(source, name) for name in (
            'max96712_parse_output_rate', 'max96712_set_registers',
            'max96712_init_settings'))
        harness = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
#define BIT(n) (1U << (n))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define dev_err(...) ((void)0)
struct device { void *data; };
struct i2c_client { struct device dev; };
struct device_node { int present, error; u32 rate; };
struct max96712 {
    struct i2c_client *i2c_client;
    void *regmap;
    int lock, lane_cnt;
    bool force_clk0, multi_vc_configured[4];
    u32 csi_output_rate_mbps;
};
struct reg_pair { u16 addr; u8 val; };
static u8 regs[0x2000];
static unsigned calls, fail_at, enabled, deskew_calls;
static void *of_find_property(struct device_node *np, const char *key, void *len)
{ (void)key; (void)len; return np && np->present ? np : NULL; }
static int of_property_read_u32(struct device_node *np, const char *key, u32 *v)
{ (void)key; *v=np->rate; return np->error; }
static void *dev_get_drvdata(struct device *dev) { return dev->data; }
static void mutex_lock(int *lock) { assert(!*lock); *lock=1; }
static void mutex_unlock(int *lock) { assert(*lock); *lock=0; }
static int max96712_write_reg(struct max96712 *p, u16 addr, unsigned val)
{
    assert(p->lock);
    if (++calls == fail_at) return -EIO;
    if (addr == 0x418 && p->csi_output_rate_mbps)
        assert(!(regs[0x40b] & 2));
    if (addr == 0x40b && (val & 2)) {
        if (p->csi_output_rate_mbps == 2200) {
            assert(regs[0x418] == 0x36 && regs[0x1d00] == 0xf5);
            assert(regs[0x943] & 0x80); assert(regs[0x944] & 0x80);
        }
        ++enabled;
    }
    regs[addr]=val; return 0;
}
static int regmap_update_bits(void *map, unsigned addr, unsigned mask, unsigned val)
{
    ++deskew_calls;
    return max96712_write_reg(map, addr, (regs[addr] & ~mask) | (val & mask));
}
'''
        checks = r'''
static void reset(struct max96712 *p)
{
    memset(regs,0,sizeof(regs)); calls=enabled=deskew_calls=fail_at=0;
    regs[0x40b]=2; regs[0x943]=3; regs[0x944]=5;
    memset(p,0,sizeof(*p)); p->lane_cnt=4; p->regmap=p;
    memset(p->multi_vc_configured,1,sizeof(p->multi_vc_configured));
}
int main(void)
{
    struct max96712 p;
    struct device d = { &p };
    struct device_node np = { 0, 0, 2200 };
    unsigned writes, i;
    reset(&p);
    assert(max96712_parse_output_rate(&p, &np)==0 && !p.csi_output_rate_mbps);
    assert(max96712_init_settings(&d)==0);
    assert(regs[0x418]==0x2d && !deskew_calls && enabled==1);
    reset(&p); p.lane_cnt=2;
    assert(max96712_init_settings(&d)==0);
    assert(regs[0x418]==0x39 && !deskew_calls);
    np.present=1;
    assert(max96712_parse_output_rate(&p,&np)==-EINVAL);
    p.lane_cnt=4;
    np.error=-ENODATA; assert(max96712_parse_output_rate(&p,&np)==-ENODATA);
    np.error=0; np.rate=2199; assert(max96712_parse_output_rate(&p,&np)==-EINVAL);
    np.rate=0; assert(max96712_parse_output_rate(&p,&np)==-EINVAL);
    reset(&p); np.rate=1300;
    assert(max96712_parse_output_rate(&p,&np)==0);
    assert(max96712_init_settings(&d)==0 && regs[0x418]==0x2d && !deskew_calls);
    reset(&p); np.rate=2200;
    assert(max96712_parse_output_rate(&p,&np)==0);
    assert(max96712_init_settings(&d)==0 && regs[0x418]==0x36 && !p.lock);
    assert(regs[0x943]==0x83 && regs[0x944]==0x85);
    assert(!p.multi_vc_configured[0]);
    writes=calls;
    /* A hardware reset must be followed by the same full initialization. */
    memset(regs,0,sizeof(regs));
    assert(max96712_init_settings(&d)==0 && regs[0x418]==0x36);
    assert(regs[0x943]==0x80 && regs[0x944]==0x80);
    for(i=1;i<=writes;i++) {
        reset(&p); p.csi_output_rate_mbps=2200; fail_at=i;
        assert(max96712_init_settings(&d)==-EIO);
        assert(!p.lock && !enabled && calls==i);
        if(i>1) assert(!(regs[0x40b]&2));
    }
    printf("DT/default/reinitialization tests passed; %u I2C failure points passed\n",writes);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            cfile = pathlib.Path(tmp) / 'probe.c'
            exe = pathlib.Path(tmp) / 'probe'
            cfile.write_text(harness + defines + '\n' + functions + checks)
            subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror',
                            str(cfile), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_only_mixed_overlay_opts_in_and_matches_receiver_rate(self):
        opted = [p for p in (ROOT / 'hardware/realsense').glob('*.dts')
                 if 'maxim,csi-output-rate-mbps' in p.read_text()]
        self.assertEqual([p.name for p in opted], [
            'tegra234-camera-d4xx-overlay-fg12-4ch-cams-0-1-2-3-d5xx-3d4xx.dts'])
        text = opted[0].read_text()
        self.assertEqual(text.count('serdes_pix_clk_hz = "550000000";'), 16)
        self.assertIn('max_lane_speed = <2200000>;', text)
        self.assertIn('maxim,csi-output-rate-mbps = <2200>;', text)


if __name__ == '__main__':
    unittest.main()
