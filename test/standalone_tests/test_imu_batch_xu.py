#!/usr/bin/env python3
"""Host-only RSDEV-13850 tests of the production d4xx XU register helpers."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

def main():
    root = Path(__file__).resolve().parents[2]
    source = (root / 'kernel/realsense/d4xx_main.c').read_text()
    cids = dict((name, int(offset)) for name, offset in re.findall(
        r'#define\s+(\w+_CID_\w+)\s+\(DS5_CAMERA_CID_BASE\s*\+\s*(\d+)\)', source))
    assert cids['D500_CAMERA_CID_IMU_BATCH'] == 56
    assert cids['D500_CAMERA_CID_IMU_BATCH_STATUS'] == 57
    assert [name for name, offset in cids.items() if offset == 57] == ['D500_CAMERA_CID_IMU_BATCH_STATUS']
    config = source[source.index('static const struct v4l2_ctrl_config ds5_ctrl_imu_batch_d58x'):source.index('static const struct v4l2_ctrl_config ds5_ctrl_gyro_sensitivity_d58x')]
    assert '.min = D500_IMU_BATCH_DEFAULT' in config and '.max = D500_IMU_BATCH_PAIR' in config
    assert 'V4L2_CTRL_FLAG_READ_ONLY' in config
    mode_config = config[:config.index('static const struct v4l2_ctrl_config ds5_ctrl_imu_batch_status_d58x')]
    assert '.type = V4L2_CTRL_TYPE_MENU' in mode_config and '.qmenu = ds5_imu_batch_menu' in mode_config
    assert cids['D500_CAMERA_CID_ACCEL_SENSITIVITY'] == 39
    assert [name for name, offset in cids.items() if offset == 56] == ['D500_CAMERA_CID_IMU_BATCH']
    assert 'ds5_ctrl_accel_sensitivity_d58x' in source[source.index('// IMU custom'):]
    assert 'ds5_ctrl_imu_batch_d58x' in source[source.index('// IMU custom'):]
    def function(name):
        start = source.index('static int ' + name + '(')
        return source[start:source.index('\n}', start) + 2]
    constants = '\n'.join(line for line in source.splitlines()
                          if line.startswith(('#define D500_IMU_BATCH_',
                                              '#define D500_GMSL_READ_FILL_BYTE')))
    prefix = r'''
    #include <assert.h>
    #include <stdbool.h>
    #include <stdint.h>
    #include <stdio.h>
    #include <errno.h>
    typedef uint8_t u8;
    typedef uint32_t u32;
    #define BIT(n) (1U << (n))
    #define GENMASK(h,l) (((1U << ((h)+1))-1) & ~((1U << (l))-1))
    struct shared { int lock; bool imu_streaming; };
    struct ds5 { struct shared *ds5_dev; };
    static struct shared shared;
    static struct ds5 state = { &shared };
    static u8 wire, pending;
    static bool pending_active, drop_capability;
    static int writes, reads, delay_reads, fail_read, fail_write;
    static int readback_mode, readback_status;
    static void mutex_lock(int *m) { assert(!*m); *m=1; }
    static void mutex_unlock(int *m) { assert(*m); *m=0; }
    static int ds5_raw_read(struct ds5 *s, unsigned addr, u8 *data, unsigned size) {
        (void)s;assert(addr==0x4590 && size==1);
        ++reads;
        if (writes && delay_reads>=0 && delay_reads--==0) {
            wire=(wire & 0xfc)|pending;
            if (pending_active) wire |= D500_IMU_BATCH_ACTIVE;
            if (drop_capability) wire &= ~D500_IMU_BATCH_SUPPORTED;
            if (readback_mode>=0) wire=(wire & 0xfc)|readback_mode;
            if (readback_status>=0) wire=readback_status;
        }
        if (reads == fail_read) return -EIO;
        *data=wire;return 0;
    }
    static int ds5_raw_write(struct ds5 *s,unsigned addr,const u8 *data,unsigned size) {
        (void)s;assert(addr==0x4590 && size==1);writes++;
        if (fail_write) return -EIO;
        pending=*data;return 0;
    }
    static void reset(u8 status) {
        shared.lock=0;shared.imu_streaming=false;wire=status;pending=0;
        pending_active=drop_capability=false;
        writes=reads=delay_reads=fail_read=fail_write=0;
        readback_mode=readback_status=-1;
    }
    '''
    tests = r'''
    int main(void) {
        u8 status;
        for (unsigned raw=0; raw<256; ++raw) {
            reset(raw);
            int result = d500_get_imu_batch_control(&state, true, &status);
            if (result) { assert(result == -EBADMSG); continue; }
            assert((status & 0x1f) == 0);
            assert(d500_get_imu_batch_control(&state, false, &status) == 0);
            assert(status == 1 || status == 2);
            if (raw != 0xff && (raw & 0x80)) {
                unsigned mode = status;
                assert(mode == ((raw & 3) == 2 ? 2U : 1U));
                if (!(raw & 0x20) && (mode == 1 || (raw & 0x40))) {
                    // An idle supported MODE GET can be written back unchanged.
                    assert(d500_set_imu_batch(&state, mode) == 0);
                }
            } else assert(status == 1);
        }
        reset(0);assert(d500_get_imu_batch(&state,&status)==0 && status==0);
        reset(0x7f);assert(d500_get_imu_batch(&state,&status)==0 && status==0);
        reset(0xff);assert(d500_get_imu_batch(&state,&status)==0 && status==0);
        reset(0xc4);assert(d500_get_imu_batch(&state,&status)==-EBADMSG);
        reset(0xc3);assert(d500_get_imu_batch(&state,&status)==-EBADMSG);
        reset(0xc0);fail_read=1;assert(d500_get_imu_batch(&state,&status)==-EIO);
        for (unsigned mode=0;mode<=2;mode++) {
            reset(0xc1);assert(d500_set_imu_batch(&state,mode)==0);
            assert(writes==1 && reads==2 && wire==(0xc0|mode) && !shared.lock);
        }
        for (unsigned mode=0;mode<=1;mode++) {
            reset(0xc2);assert(d500_set_imu_batch(&state,mode)==0 && wire==(0xc0|mode));
            reset(0x81);assert(d500_set_imu_batch(&state,mode)==0 && wire==(0x80|mode));
            /* Both disable encodings must accept either canonical FW reply. */
            for (unsigned reply=0;reply<=1;reply++) {
                reset(0xc2);readback_mode=reply;
                assert(d500_set_imu_batch(&state,mode)==0 && wire==(0xc0|reply));
                assert(writes==1 && reads==2 && !shared.lock);
            }
            reset(0xc2);readback_mode=2;
            assert(d500_set_imu_batch(&state,mode)==-EIO && !shared.lock);
        }
        for (unsigned reply=0;reply<=1;reply++) {
            reset(0xc1);readback_mode=reply;
            assert(d500_set_imu_batch(&state,2)==-EIO && !shared.lock);
        }
        for (unsigned invalid=3;invalid<=256;invalid++) {
            reset(0xc1);assert(d500_set_imu_batch(&state,invalid)==-EINVAL && !writes && !reads);
        }
        for (unsigned mode=0;mode<=2;mode++) {
            reset(0xc1);shared.imu_streaming=true;
            assert(d500_set_imu_batch(&state,mode)==-EBUSY && !writes && !shared.lock);
            reset(0xe2);assert(d500_set_imu_batch(&state,mode)==-EBUSY && !writes && !shared.lock);
        }
        reset(0x81);assert(d500_set_imu_batch(&state,2)==-EOPNOTSUPP && !writes);
        reset(0);assert(d500_set_imu_batch(&state,2)==-EOPNOTSUPP && !writes);
        for (unsigned mode=0;mode<=2;mode++) {
            reset(0xff);assert(d500_set_imu_batch(&state,mode)==-EOPNOTSUPP);
            assert(!writes && reads==1 && !shared.lock);
            reset(0xc1);readback_status=0xff;
            assert(d500_set_imu_batch(&state,mode)==-EIO);
            assert(writes==1 && reads==2 && !shared.lock);
            reset(0xc1);readback_status=0xc4;
            assert(d500_set_imu_batch(&state,mode)==-EBADMSG && !shared.lock);
        }
        reset(0xc1);fail_write=1;assert(d500_set_imu_batch(&state,2)==-EIO);
        reset(0xc1);pending_active=true;
        assert(d500_set_imu_batch(&state,2)==-EBUSY && wire==0xe2 && writes==1 && reads==2 && !shared.lock);
        for (unsigned mode=0;mode<=2;mode++) {
            reset(0xc1);drop_capability=true;
            assert(d500_set_imu_batch(&state,mode)==-EIO);
            assert(writes==1 && reads==2 && wire==(0x40|mode) && !shared.lock);
        }
        reset(0xc1);fail_read=2;assert(d500_set_imu_batch(&state,2)==-EIO && wire==0xc2 && !shared.lock);
        reset(0xc1);delay_reads=3;assert(d500_set_imu_batch(&state,2)==-EIO && reads==2);
        reset(0xc1);delay_reads=-1;
        assert(d500_set_imu_batch(&state,2)==-EIO && reads==2 && !shared.lock);
        puts("RSDEV-13850 XU: capability, modes, busy, tunnel, I2C errors and readback tests passed");
    }
    '''
    with tempfile.TemporaryDirectory(prefix='d4xx-imu-xu-') as tmp:
        c=Path(tmp)/'test.c';exe=Path(tmp)/'test'
        c.write_text(prefix.replace('    struct shared', constants+'\n    struct shared', 1)+'\n'+function('d500_get_imu_batch')+'\n'+function('d500_get_imu_batch_control')+'\n'+function('d500_set_imu_batch')+tests)
        subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined',str(c),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)


if __name__ == "__main__":
    main()
