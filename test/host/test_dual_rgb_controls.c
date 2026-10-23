#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int16_t s16;
typedef int32_t s32;
typedef uint16_t __le16;
typedef uint32_t __le32;
#define __packed __attribute__((packed))
#define le16_to_cpu(value) (value)
#define le32_to_cpu(value) (value)
#define V4L2_EXPOSURE_APERTURE_PRIORITY 3
#define V4L2_EXPOSURE_MANUAL 1

struct fake_device { int lock; };
struct ds5 {
    struct fake_device *ds5_dev;
    int is_rgb, is_depth, is_y8;
};

static int writes, write_failure, reads, read_failure, lock_depth;
static int completion_reads, probe_ready;
static u16 registers[4], values[4];
static u8 replies[4][24];
static int reply_count;

static void mutex_lock(int *lock)
{
    (void)lock;
    assert(lock_depth++ == 0);
}

static void mutex_unlock(int *lock)
{
    (void)lock;
    assert(--lock_depth == 0);
}

static void usleep_range(unsigned int minimum, unsigned int maximum)
{
    assert(minimum == 5000 && maximum == 6000);
}

static int ds5_write(struct ds5 *state, u16 address, u16 value)
{
    (void)state;
    assert(writes < 4);
    registers[writes] = address;
    values[writes++] = value;
    return writes == write_failure ? -EREMOTEIO : 0;
}

static int ds5_raw_read(struct ds5 *state, u16 address, void *value, size_t size)
{
    const u8 *reply = replies[completion_reads < reply_count ?
                             completion_reads : reply_count - 1];

    (void)state;
    assert(address == 0x4820 && (size == 2 || size == 24));
    ++reads;
    if (reads == read_failure)
        return -EREMOTEIO;
    if (size == 2) {
        assert(!probe_ready);
        probe_ready = reply[0] == 1 && reply[1] == 24;
    } else {
        assert(probe_ready);
        probe_ready = 0;
        ++completion_reads;
    }
    memcpy(value, reply, size);
    return 0;
}

#define current get_current()
#include "production.h"
#undef current

static void store32(u8 *data, u32 value)
{
    memcpy(data, &value, sizeof(value));
}

static void reset(u16 control_id)
{
    writes = write_failure = reads = read_failure = lock_depth = 0;
    completion_reads = probe_ready = 0;
    reply_count = 2;
    memset(replies, 0, sizeof(replies));
    for (int index = 0; index < reply_count; ++index) {
        replies[index][0] = 1;
        replies[index][1] = 24;
    }
    replies[1][2] = 2;
    memcpy(&replies[1][4], &control_id, sizeof(control_id));
    store32(&replies[1][8], 1);
    store32(&replies[1][12], 1);
}

static void expect_result(struct ds5 *state, int expected)
{
    assert(d500_set_rgb_control(state, DS5_MANUAL_GAIN, 32) == expected);
    assert(lock_depth == 0);
}

int main(void)
{
    struct fake_device device = {0};
    struct ds5 state = {.ds5_dev = &device, .is_rgb = 1};
    assert(sizeof(struct d500_control_completion) == 24);
    reset(0x0102);
    expect_result(&state, 0);
    assert(writes == 1 && registers[0] == 0x4204 && values[0] == 32);
    assert(reads == 4 && completion_reads == 2);
    reset(0x0101);
    assert(d500_set_rgb_control(&state, DS5_MANUAL_EXPOSURE_LSB, 30) == 0);
    assert(writes == 2 && registers[0] == 0x4202 && registers[1] == 0x4200);
    assert(values[0] == 0 && values[1] == 30);
    reset(0x0103);
    assert(d500_set_rgb_control(&state, DS5_AUTO_EXPOSURE_MODE, 3) == 0);
    assert(registers[0] == 0x420c && values[0] == 8);
    reset(0x0103);
    assert(d500_set_rgb_control(&state, DS5_AUTO_EXPOSURE_MODE, 1) == 0);
    assert(values[0] == 1);

    reset(0x0102);
    replies[1][2] = 3;
    replies[1][6] = (u8)-9;
    replies[1][7] = 255;
    expect_result(&state, -EIO);
    reset(0x0102);
    replies[1][2] = 3;
    replies[1][6] = (u8)-12;
    replies[1][7] = 255;
    store32(&replies[1][20], 1);
    expect_result(&state, -EBUSY);
    reset(0x0102);
    replies[1][6] = (u8)-13;
    replies[1][7] = 255;
    store32(&replies[1][20], 1);
    expect_result(&state, -ESHUTDOWN);
    reset(0x0101);
    expect_result(&state, -EAGAIN);
    reset(0x0102);
    store32(&replies[1][8], 2);
    expect_result(&state, -EAGAIN);
    reset(0x0102);
    replies[0][0] = 255;
    expect_result(&state, -EOPNOTSUPP);
    assert(writes == 0 && reads == 1 && completion_reads == 0);
    reset(0x0102);
    replies[0][0] = 0;
    replies[0][1] = 0;
    expect_result(&state, -EOPNOTSUPP);
    assert(writes == 0 && reads == 1 && completion_reads == 0);
    for (int size = 23; size <= 25; size += 2) {
        reset(0x0102);
        replies[0][1] = size;
        expect_result(&state, -EOPNOTSUPP);
        assert(writes == 0 && reads == 1 && completion_reads == 0);
    }
    reset(0x0102);
    replies[0][2] = 255;
    expect_result(&state, -EBADMSG);
    reset(0x0102);
    read_failure = 1;
    expect_result(&state, -EREMOTEIO);
    assert(writes == 0 && reads == 1 && completion_reads == 0);
    reset(0x0102);
    read_failure = 2;
    expect_result(&state, -EREMOTEIO);
    assert(writes == 0 && reads == 2 && completion_reads == 0);
    reset(0x0102);
    read_failure = 3;
    expect_result(&state, -EREMOTEIO);
    assert(writes == 1 && reads == 3 && completion_reads == 1);
    reset(0x0102);
    read_failure = 4;
    expect_result(&state, -EREMOTEIO);
    assert(writes == 1 && reads == 4 && completion_reads == 1);
    reset(0x0102);
    write_failure = 1;
    expect_result(&state, -EREMOTEIO);
    reset(0x0101);
    write_failure = 2;
    assert(d500_set_rgb_control(&state, DS5_MANUAL_EXPOSURE_LSB, 30) == -EREMOTEIO);
    assert(writes == 2 && reads == 2 && lock_depth == 0);
    reset(0x0102);
    replies[0][2] = 1;
    reply_count = 1;
    expect_result(&state, -EBUSY);
    assert(writes == 0);
    reset(0x0102);
    replies[1][2] = 1;
    store32(&replies[1][12], 0);
    expect_result(&state, -ETIMEDOUT);
    reset(0x0102);
    store32(&replies[0][8], UINT32_MAX);
    store32(&replies[0][12], UINT32_MAX);
    store32(&replies[1][8], 0);
    store32(&replies[1][12], 0);
    expect_result(&state, 0);
    reset(0x0101);
    assert(d500_set_rgb_control(&state, DS5_MANUAL_EXPOSURE_LSB, 0) == -ERANGE);
    assert(writes == 0 && reads == 0);
    puts("Dual RGB production C: exposure/gain/AE, failures, timeout, sequence and wrap: PASS");
    return 0;
}