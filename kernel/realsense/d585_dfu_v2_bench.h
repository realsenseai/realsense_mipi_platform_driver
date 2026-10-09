/* Default-off D585 timing prototype. Included after the legacy DFU helpers. */
#include <linux/crc32.h>
#include <linux/random.h>
#include <linux/uaccess.h>
#include <asm/unaligned.h>
#define D585_V2_CAP 0x5100
#define D585_V2_CTRL 0x5140
#define D585_V2_DATA 0x5200
#define D585_V2_ACK 0x5600
#define D585_V2_PAYLOAD 992
#define D585_V2_CAPS _IOR('G', 0x70, __u8[64])
struct d585_v2_begin {
    __u16 version, flags;
    __u32 size;
    __u8 md5[16];
    __u16 expected_bkc[4];
};
#define D585_V2_BEGIN _IOW('G', 0x71, struct d585_v2_begin)
#define D585_V2_FINISH _IO('G', 0x72)
#define D585_V2_STATUS _IOR('G', 0x73, __u8[64])
#define D585_V2_ABORT _IO('G', 0x74)
static bool d585_dfu_v2_bench;
module_param_named(d585_dfu_v2_bench, d585_dfu_v2_bench, bool, 0444);
MODULE_PARM_DESC(d585_dfu_v2_bench, "Opt-in partial D585 DFU protocol for timing only; SDK/legacy unchanged");
struct d585_dfu_v2 {
    u8 cap[64], control[64], staging[D585_V2_PAYLOAD], frame[1024];
    u64 session, boot;
    u32 total, consumed, accepted, sequence, fill;
    unsigned long receive_deadline, finish_deadline;
    bool failed, finish_submitted;
};
static bool d585_v2_eligible(struct ds5 *state)
{
    u16 pid = READ_ONCE(state->ds5_dev->d585_product_id);
    return d585_dfu_v2_bench && ds5_is_d58x(state) &&
        state->dfu_dev.dfu_state_flag != DS5_DFU_RECOVERY &&
        (pid == 0x0b6a || pid == 0x0c07 || pid == 0x0c08);
}
static u32 d585_v2_crc(u8 *frame, size_t length, unsigned int off)
{
    u32 saved = get_unaligned_le32(frame + off);
    u32 crc;
    put_unaligned_le32(0, frame + off);
    crc = crc32_le(~0U, frame, length) ^ ~0U;
    put_unaligned_le32(saved, frame + off);
    return crc;
}
/* Caller holds shared-camera v2_lock over the complete regmap transaction. */
static int d585_v2_read(struct ds5 *state, u16 reg, u8 *out, u32 magic)
{
    int ret = regmap_raw_read(state->regmap, reg, out, 64);
    if (ret) return ret;
    if (get_unaligned_le32(out) != magic || get_unaligned_le16(out + 4) != 2 ||
        get_unaligned_le16(out + 6) != 64 ||
        get_unaligned_le32(out + 60) != d585_v2_crc(out, 64, 60))
        return -EBADMSG;
    return 0;
}
static int d585_v2_ack(struct ds5 *state, u8 *out, bool match)
{
    struct d585_dfu_v2 *v = state->dfu_v2;
    int ret = d585_v2_read(state, D585_V2_ACK, out, 0x32414447);
    if (ret || !match) return ret;
    if (!v || get_unaligned_le64(out + 8) != v->boot) return -ESTALE;
    if (get_unaligned_le64(out + 16) != v->session ||
        get_unaligned_le32(out + 32) != v->total) return -ENOMSG;
    if (get_unaligned_le16(out + 36) > 7) return -EPROTO;
    /* Keep a Firmware terminal error distinct from a transient bus EIO. */
    if (get_unaligned_le16(out + 38)) return -EREMOTEIO;
    return 0;
}
static void d585_v2_control(struct d585_dfu_v2 *v, u16 command)
{
    put_unaligned_le16(command, v->control + 6);
    put_unaligned_le16(command - 1, v->control + 16);
    put_unaligned_le32(0, v->control + 60);
    put_unaligned_le32(d585_v2_crc(v->control, 64, 60), v->control + 60);
}
static int d585_v2_send_block(struct ds5 *state)
{
    struct d585_dfu_v2 *v = state->dfu_v2;
    u8 ack[64];
    unsigned long deadline = jiffies + msecs_to_jiffies(5000);
    unsigned int attempt;
    u32 crc, count = v->fill;
    int ret = -EIO;
    memset(v->frame, 0, 32);
    put_unaligned_le32(0x32464447, v->frame);
    put_unaligned_le16(2, v->frame + 4); put_unaligned_le16(32, v->frame + 6);
    put_unaligned_le64(v->session, v->frame + 8);
    put_unaligned_le32(v->sequence, v->frame + 16);
    put_unaligned_le32(v->accepted, v->frame + 20);
    put_unaligned_le16(count, v->frame + 24);
    memcpy(v->frame + 32, v->staging, count);
    crc = d585_v2_crc(v->frame, count + 32, 28);
    put_unaligned_le32(crc, v->frame + 28);
    for (attempt = 0; attempt < 4 && time_before(jiffies, deadline); attempt++) {
        if (signal_pending(current)) return -ERESTARTSYS;
        if (time_after_eq(jiffies, v->receive_deadline)) return -ETIMEDOUT;
        if (mutex_lock_interruptible(&state->ds5_dev->v2_lock)) return -ERESTARTSYS;
        /* A write errno is ambiguous: query ACK before deciding to replay. */
        (void)regmap_raw_write(state->regmap, D585_V2_DATA, v->frame, count + 32);
        ret = d585_v2_ack(state, ack, true);
        mutex_unlock(&state->ds5_dev->v2_lock);
        if (!ret) {
            u32 next = get_unaligned_le32(ack + 24), bytes = get_unaligned_le32(ack + 28);
            if (get_unaligned_le16(ack + 36) != 2) return -EIO;
            if (next == v->sequence + 1 && bytes == v->accepted + count &&
                get_unaligned_le32(ack + 44) == crc) {
                v->sequence++; v->accepted += count; v->fill = 0; return 0;
            }
            if (next != v->sequence || bytes != v->accepted) return -EIO;
        } else if (ret == -ENODEV || ret == -ESTALE || ret == -ENOMSG ||
                   ret == -EPROTO || ret == -EREMOTEIO) return ret;
        if (attempt < 3) {
            static const unsigned int delay[] = {20, 50, 100};
            if (msleep_interruptible(delay[attempt])) return -ERESTARTSYS;
        }
    }
    return ret ? ret : -ETIMEDOUT;
}
static ssize_t d585_v2_write(struct ds5 *state, const char __user *buffer, size_t length)
{
    struct d585_dfu_v2 *v = state->dfu_v2;
    size_t done = 0;
    int ret;
    if (v->failed || v->finish_submitted) return -EIO;
    if (length > v->total - v->consumed) return -EINVAL;
    while (done < length) {
        size_t n = min_t(size_t, length - done, D585_V2_PAYLOAD - v->fill);
        if (copy_from_user(v->staging + v->fill, buffer + done, n)) { ret = -EFAULT; goto failed; }
        v->fill += n; v->consumed += n; done += n;
        if (v->fill == D585_V2_PAYLOAD || v->consumed == v->total) {
            ret = d585_v2_send_block(state);
            if (ret) goto failed;
        }
    }
    return done;
failed:
    v->failed = true;
    return ret;
}
static int d585_v2_begin_session(struct ds5 *state, unsigned long arg)
{
    struct d585_v2_begin begin;
    struct d585_dfu_v2 *v;
    u8 ack[64];
    unsigned long deadline = jiffies + msecs_to_jiffies(5000);
    int ret;
    if (state->dfu_v2 || state->ds5_dev->v2_owner) return -EBUSY;
    if (copy_from_user(&begin, (void __user *)arg, sizeof(begin))) return -EFAULT;
    if (begin.version != 2 || begin.flags || !begin.size || begin.size > 25524288) return -EINVAL;
    v = kzalloc(sizeof(*v), GFP_KERNEL);
    if (!v) return -ENOMEM;
    ret = d585_v2_read(state, D585_V2_CAP, v->cap, 0x32434447);
    if (ret) goto free_context;
    if (get_unaligned_le16(v->cap + 16) != READ_ONCE(state->ds5_dev->d585_product_id) ||
        (get_unaligned_le16(v->cap + 18) & 0xf) != 0xf ||
        get_unaligned_le16(v->cap + 24) != 1024 || get_unaligned_le16(v->cap + 26) != 32 ||
        begin.size > get_unaligned_le32(v->cap + 20)) { ret = -EINVAL; goto free_context; }
    v->boot = get_unaligned_le64(v->cap + 8);
    if (!v->boot) { ret = -EINVAL; goto free_context; }
    do { v->session = get_random_u64(); } while (!v->session);
    v->total = begin.size;
    v->receive_deadline = jiffies + msecs_to_jiffies(max_t(u64, 300,
        120 + DIV_ROUND_UP_ULL(v->total, 8192)) * 1000);
    put_unaligned_le32(0x32464447, v->control); put_unaligned_le16(2, v->control + 4);
    put_unaligned_le64(v->session, v->control + 8); put_unaligned_le32(v->total, v->control + 20);
    memcpy(v->control + 24, begin.md5, 16);
    put_unaligned_le16(get_unaligned_le16(v->cap + 16), v->control + 40);
    put_unaligned_le64(v->boot, v->control + 44); d585_v2_control(v, 1);
    state->dfu_v2 = v; state->ds5_dev->v2_owner = state;
    while (time_before(jiffies, deadline)) {
        (void)regmap_raw_write(state->regmap, D585_V2_CTRL, v->control, 64);
        ret = d585_v2_ack(state, ack, true);
        if (!ret && get_unaligned_le16(ack + 36) == 2) {
            state->dfu_dev.dfu_state_flag = DS5_DFU_IN_PROGRESS; return 0;
        }
        if (signal_pending(current)) { ret = -ERESTARTSYS; break; }
        if (ret == -ENODEV || ret == -ESTALE || ret == -EPROTO || ret == -EREMOTEIO) break;
        /* READY wait only; DATA ready-path has no sleep. */
        if (msleep_interruptible(20)) { ret = -ERESTARTSYS; break; }
    }
    if (!ret) ret = -ETIMEDOUT;
    v->failed = true; /* uncertain BEGIN retained until explicit cancellation/close */
    return ret;
free_context:
    kfree(v); return ret;
}
static int d585_v2_finish(struct ds5 *state)
{
    struct d585_dfu_v2 *v = state->dfu_v2;
    u8 ack[64];
    int ret;
    if (!v) return -EINVAL;
    if (!v->finish_submitted) {
        if (v->failed || v->consumed != v->total || v->accepted != v->total || v->fill) return -EINVAL;
        d585_v2_control(v, 2); v->finish_submitted = true;
        v->finish_deadline = jiffies + msecs_to_jiffies(330000);
    }
    for (;;) {
        if (signal_pending(current)) return -ERESTARTSYS;
        if (mutex_lock_interruptible(&state->ds5_dev->v2_lock)) return -ERESTARTSYS;
        ret = d585_v2_ack(state, ack, true);
        if (!ret && get_unaligned_le16(ack + 36) == 2) {
            (void)regmap_raw_write(state->regmap, D585_V2_CTRL, v->control, 64);
            ret = d585_v2_ack(state, ack, true);
        }
        mutex_unlock(&state->ds5_dev->v2_lock);
        if (ret == -ENODEV || ret == -ESTALE || ret == -ENOMSG ||
            ret == -EPROTO || ret == -EREMOTEIO) return ret;
        if (!ret && get_unaligned_le16(ack + 36) == 5) {
            if ((s32)get_unaligned_le32(ack + 52)) return -EIO;
            state->dfu_dev.dfu_state_flag = DS5_DFU_DONE;
            WRITE_ONCE(state->dfu_dev.manifest_complete, true); return 0;
        }
        if (!ret && get_unaligned_le16(ack + 36) == 7) return -ECANCELED;
        if (time_after_eq(jiffies, v->finish_deadline)) return -ETIMEDOUT;
        if (msleep_interruptible(!ret && get_unaligned_le16(ack + 36) == 4 ? 1000 : 100))
            return -ERESTARTSYS;
    }
}
static int d585_v2_abort_locked(struct ds5 *state)
{
    struct d585_dfu_v2 *v = state->dfu_v2;
    u8 ack[64];
    int ret;
    if (!v) return -EINVAL;
    /* Read raw terminal error as well: a pre-Flash failed session is cancellable. */
    ret = d585_v2_ack(state, ack, false);
    if (ret || get_unaligned_le64(ack + 8) != v->boot || get_unaligned_le64(ack + 16) != v->session)
        return ret ? ret : -ENODEV;
    if (get_unaligned_le16(ack + 58) & (2 | 8)) return -EBUSY;
    if (get_unaligned_le16(ack + 36) == 3 || get_unaligned_le16(ack + 36) == 4 ||
        get_unaligned_le16(ack + 36) == 5) return -EBUSY;
    d585_v2_control(v, 3);
    ret = regmap_raw_write(state->regmap, D585_V2_CTRL, v->control, 64);
    if (ret) return ret;
    ret = d585_v2_ack(state, ack, false);
    if (!ret && get_unaligned_le16(ack + 36) == 7) {
        state->ds5_dev->v2_owner = NULL; return 0;
    }
    return ret ? ret : -EBUSY;
}
static long d585_v2_ioctl(struct file *file, unsigned int command, unsigned long arg)
{
    struct ds5 *state = file->private_data;
    u8 out[64];
    int ret;
    if (!d585_v2_eligible(state)) return -ENOTTY;
    if (command != D585_V2_CAPS && command != D585_V2_STATUS && !(file->f_mode & FMODE_WRITE))
        return -EBADF;
    if (mutex_lock_interruptible(&state->lock)) return -ERESTARTSYS;
    if (command == D585_V2_FINISH) {
        ret = d585_v2_finish(state); /* transport mutex acquired only per poll */
        goto out;
    }
    if (mutex_lock_interruptible(&state->ds5_dev->v2_lock)) { ret = -ERESTARTSYS; goto out; }
    switch (command) {
    case D585_V2_CAPS:
        ret = d585_v2_read(state, D585_V2_CAP, out, 0x32434447);
        if (!ret && copy_to_user((void __user *)arg, out, 64)) ret = -EFAULT;
        break;
    case D585_V2_STATUS:
        ret = d585_v2_ack(state, out, false);
        if (!ret && copy_to_user((void __user *)arg, out, 64)) ret = -EFAULT;
        break;
    case D585_V2_BEGIN: ret = d585_v2_begin_session(state, arg); break;
    case D585_V2_ABORT: ret = d585_v2_abort_locked(state); break;
    default: ret = -ENOTTY; break;
    }
    mutex_unlock(&state->ds5_dev->v2_lock);
out:
    mutex_unlock(&state->lock); return ret;
}
static void d585_v2_close(struct ds5 *state)
{
    if (!state->dfu_v2) return;
    mutex_lock(&state->ds5_dev->v2_lock);
    (void)d585_v2_abort_locked(state); /* never free/reset Firmware worker */
    kfree(state->dfu_v2); state->dfu_v2 = NULL;
    mutex_unlock(&state->ds5_dev->v2_lock);
    /* v2_owner remains quarantined when Flash may have started; test requires
     * verified UART reset/module reload before another session. */
}
