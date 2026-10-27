/* Opt-in file-API feasibility prototype. Not release-qualified.
 * Full external control quiescence, PM and hot-remove qualification are pending.
 */
#include <crypto/hash.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <linux/completion.h>
#include <linux/kref.h>
#include <linux/sched/signal.h>
static bool d585_dfu_compat_bench;
module_param(d585_dfu_compat_bench, bool, 0444);
MODULE_PARM_DESC(d585_dfu_compat_bench,
    "Opt-in buffered write/close adapter; requires V2 bench FW, no SDK change");
#define D585_COMPAT_HEADER 412U
#define D585_COMPAT_MAX 25524288U
struct d585_compat {
    struct kref refs;
    struct work_struct work;
    struct delayed_work lease_work;
    struct completion done;
    struct ds5 *state;
    u8 prefix[D585_COMPAT_HEADER], *image;
    size_t prefix_size, total, used;
    u64 boot;
    unsigned long started, deadline, last_write;
    bool failed, submitted;
    int result;
};
static void d585_compat_free(struct kref *ref)
{
    struct d585_compat *a = container_of(ref, struct d585_compat, refs);
    struct ds5 *state = a->state;
    cancel_delayed_work_sync(&a->lease_work);
    mutex_lock(&state->lock);
    d585_v2_close(state);
    mutex_lock(&state->ds5_dev->v2_lock);
    if (state->ds5_dev->v2_owner == state && !state->ds5_dev->v2_quarantine_boot)
        state->ds5_dev->v2_owner = NULL;
    mutex_unlock(&state->ds5_dev->v2_lock);
    state->dfu_compat = NULL;
    state->dfu_dev.device_open_count = 0;
    if (state->dfu_dev.dfu_state_flag != DS5_DFU_RECOVERY)
        state->dfu_dev.dfu_state_flag = DS5_DFU_IDLE;
    mutex_unlock(&state->lock);
    kvfree(a->image);
    put_device(&state->client->dev);
    kfree(a);
}
/* An abandoned open cannot retain an image-sized allocation indefinitely.
 * File state remains failed until close, so expiry can never submit a job. */
static void d585_compat_lease(struct work_struct *work)
{
    struct d585_compat *a = container_of(work, struct d585_compat, lease_work.work);
    struct ds5 *state = a->state;
    unsigned long limit;
    mutex_lock(&state->lock);
    if (a->submitted) goto out;
    limit = a->started + msecs_to_jiffies(a->total ?
        max_t(unsigned long, 300, 120 + DIV_ROUND_UP(a->total,8192))*1000 : 5000);
    if (a->total && time_before(a->last_write + msecs_to_jiffies(60000), limit))
        limit = a->last_write + msecs_to_jiffies(60000);
    if (time_before(jiffies, limit)) {
        mod_delayed_work(system_unbound_wq, &a->lease_work, limit-jiffies);
        goto out;
    }
    a->failed = true; a->result = -ETIMEDOUT;
    kvfree(a->image); a->image = NULL;
    mutex_lock(&state->ds5_dev->v2_lock);
    if (state->ds5_dev->v2_owner == state && !state->ds5_dev->v2_quarantine_boot)
        state->ds5_dev->v2_owner = NULL;
    mutex_unlock(&state->ds5_dev->v2_lock);
out:
    mutex_unlock(&state->lock);
}
static int d585_compat_md5(const u8 *image, size_t size, u8 out[16])
{
    struct crypto_shash *tfm = crypto_alloc_shash("md5", 0, 0);
    struct shash_desc *desc;
    int ret;
    if (IS_ERR(tfm)) return PTR_ERR(tfm);
    desc = kmalloc(sizeof(*desc) + crypto_shash_descsize(tfm), GFP_KERNEL);
    if (!desc) { crypto_free_shash(tfm); return -ENOMEM; }
    desc->tfm = tfm;
    ret = crypto_shash_digest(desc, image, size, out);
    kfree(desc); crypto_free_shash(tfm); return ret;
}
static void d585_compat_work(struct work_struct *work)
{
    struct d585_compat *a = container_of(work, struct d585_compat, work);
    struct ds5 *state = a->state;
    struct d585_v2_begin begin = { .version = 2, .size = a->total };
    u64 start = ktime_get_ns(), receive_start = 0, receive_end = 0;
    unsigned long hash_deadline = jiffies + msecs_to_jiffies(30000);
    size_t pos;
    int ret;
    mutex_lock(&state->lock);
    if ((crc32_le(~0U, a->image + D585_COMPAT_HEADER,
          a->total - D585_COMPAT_HEADER) ^ ~0U) != get_unaligned_le32(a->image)) {
        ret = -EBADMSG; goto done;
    }
    ret = d585_compat_md5(a->image, a->total, begin.md5);
    if (ret) goto done;
    if (time_after_eq(jiffies, hash_deadline)) { ret = -ETIMEDOUT; goto done; }
    mutex_lock(&state->ds5_dev->v2_lock);
    ret = d585_v2_begin_kernel(state, &begin, a->boot);
    mutex_unlock(&state->ds5_dev->v2_lock);
    if (ret) goto done;
    dev_notice(&state->client->dev, "D585_COMPAT BEGIN bytes=%zu pid=%d\n", a->total, task_pid_nr(current));
    receive_start = ktime_get_ns();
    for (pos = 0; pos < a->total; pos += D585_V2_PAYLOAD) {
        struct d585_dfu_v2 *v = state->dfu_v2;
        size_t n = min_t(size_t, a->total - pos, D585_V2_PAYLOAD);
        if (time_after_eq(jiffies, a->deadline)) { ret = -ETIMEDOUT; goto done; }
        memcpy(v->staging, a->image + pos, n); v->fill = n; v->consumed += n;
        ret = d585_v2_send_block(state);
        if (ret) goto done;
    }
    receive_end = ktime_get_ns();
    ret = d585_v2_finish(state);
done:
    a->result = ret;
    dev_notice(&state->client->dev,
        "D585_COMPAT result=%d bytes=%zu receive_ms=%llu total_ms=%llu reset=0\n",
        ret, a->total, receive_end ? div_u64(receive_end - receive_start, 1000000) : 0,
        div_u64(ktime_get_ns() - start, 1000000));
    mutex_unlock(&state->lock);
    complete_all(&a->done);
    kref_put(&a->refs, d585_compat_free);
    module_put(THIS_MODULE);
}
/* 0 selects legacy; 1 means this open was handled; negative means reject. */
static int d585_compat_open(struct ds5 *state, struct file *file)
{
    struct d585_compat *a;
    u8 cap[64];
    int ret;
    if (!d585_dfu_compat_bench || !d585_v2_eligible(state) || !(file->f_mode & FMODE_WRITE))
        return 0;
    if (!mutex_trylock(&state->lock)) return -EBUSY;
    if (state->dfu_dev.device_open_count) { ret = -EBUSY; goto out; }
    mutex_lock(&state->ds5_dev->v2_lock);
    ret = d585_v2_cap_probe(state, cap);
    mutex_unlock(&state->ds5_dev->v2_lock);
    /* A damaged CAP may use the separate pure basic handshake. Without
     * positive no-session proof fail bounded; never treat timeout as absence. */
    if (ret) {
        u64 nonce = 0;
        mutex_lock(&state->ds5_dev->v2_lock);
        if (state->ds5_dev->v2_owner || state->ds5_dev->hwmc_owner) ret = -EBUSY;
        else ret = d585_v2_negotiate(state, 1, 0, &nonce);
        mutex_unlock(&state->ds5_dev->v2_lock);
        if (!ret) { ret = 0; goto out; } /* positive basic/no-session agreement */
        goto out;
    }
    if (state->ds5_dev->v2_quarantine_boot) {
        mutex_lock(&state->ds5_dev->v2_lock);
        ret = d585_v2_resolve_owner(state, get_unaligned_le64(cap + 8));
        mutex_unlock(&state->ds5_dev->v2_lock);
        if (ret) goto out;
    }
    if (get_unaligned_le16(cap+16) != state->ds5_dev->d585_product_id ||
        (get_unaligned_le16(cap+18) & 0x0f) != 0x0f) { ret = -EPROTO; goto out; }
    a = kzalloc(sizeof(*a), GFP_KERNEL);
    if (!a) { ret = -ENOMEM; goto out; }
    a->boot = get_unaligned_le64(cap + 8); a->state = state; kref_init(&a->refs); init_completion(&a->done);
    INIT_WORK(&a->work, d585_compat_work);
    INIT_DELAYED_WORK(&a->lease_work, d585_compat_lease); get_device(&state->client->dev);
    state->ds5_dev->dfu_mode = 2;
    state->dfu_compat = a; state->dfu_dev.device_open_count = 1;
    state->dfu_dev.dfu_state_flag = DS5_DFU_OPEN;
    WRITE_ONCE(state->dfu_dev.manifest_complete, false);
    file->private_data = state; ret = 1;
out:
    mutex_unlock(&state->lock); return ret;
}
/* Caller holds state->lock. */
static ssize_t d585_compat_write(struct d585_compat *a, const char __user *p, size_t len)
{
    size_t original = len, n;
    u32 payload;
    if (a->failed || a->submitted) return -EIO;
    if (!a->started) {
        mutex_lock(&a->state->ds5_dev->v2_lock);
        if (a->state->ds5_dev->hwmc_owner)
            (void)d585_dfu_hwmc_reconcile_locked(a->state);
        if (a->state->ds5_dev->v2_owner || a->state->ds5_dev->hwmc_owner ||
            a->state->ds5_dev->dfu_stream_starting ||
            READ_ONCE(a->state->ds5_dev->depth_streaming) || READ_ONCE(a->state->ds5_dev->rgb_streaming) ||
            READ_ONCE(a->state->ds5_dev->ir_streaming) || READ_ONCE(a->state->ds5_dev->imu_streaming)) {
            mutex_unlock(&a->state->ds5_dev->v2_lock); return -EBUSY;
        }
        a->state->ds5_dev->v2_owner = a->state;
        mutex_unlock(&a->state->ds5_dev->v2_lock);
        a->started = jiffies;
        mod_delayed_work(system_unbound_wq, &a->lease_work, msecs_to_jiffies(5000));
    }
    if (a->last_write && time_after_eq(jiffies, a->last_write + msecs_to_jiffies(60000))) {
        a->failed = true; return -ETIMEDOUT;
    }
    a->last_write = jiffies;
    if (time_after_eq(jiffies, a->started + msecs_to_jiffies(a->total ?
            max_t(unsigned long, 300, 120 + DIV_ROUND_UP(a->total,8192))*1000 : 5000))) {
        a->failed = true; return -ETIMEDOUT;
    }
    if (!a->image) {
        n = min_t(size_t, len, D585_COMPAT_HEADER - a->prefix_size);
        if (copy_from_user(a->prefix + a->prefix_size, p, n)) { a->failed = true; return -EFAULT; }
        a->prefix_size += n; p += n; len -= n;
        if (a->prefix_size < D585_COMPAT_HEADER) return original;
        payload = get_unaligned_le32(a->prefix + 406);
        if (get_unaligned_le32(a->prefix + 398) != 0x050c0764 || !payload ||
            payload > D585_COMPAT_MAX - D585_COMPAT_HEADER) { a->failed = true; return -EINVAL; }
        a->total = D585_COMPAT_HEADER + (size_t)payload;
        a->image = kvmalloc(a->total, GFP_KERNEL);
        if (!a->image) { a->failed = true; return -ENOMEM; }
        memcpy(a->image, a->prefix, D585_COMPAT_HEADER); a->used = D585_COMPAT_HEADER;
    }
    if (len > a->total - a->used) { a->failed = true; return -EINVAL; }
    if (copy_from_user(a->image + a->used, p, len)) { a->failed = true; return -EFAULT; }
    a->used += len; return original;
}
static int d585_compat_flush(struct ds5 *state)
{
    struct d585_compat *a = READ_ONCE(state->dfu_compat);
    long remaining, result;
    if (!smp_load_acquire(&a->submitted)) {
        if (mutex_lock_interruptible(&state->lock)) return -ERESTARTSYS;
        if (!a->submitted) {
            if ((current->flags & PF_EXITING) || fatal_signal_pending(current)) {
                a->failed = true; mutex_unlock(&state->lock); return -ECANCELED;
            }
            if (a->failed || !a->image || a->used != a->total) {
                a->failed = true; mutex_unlock(&state->lock); return -EINVAL;
            }
            if (!try_module_get(THIS_MODULE)) { mutex_unlock(&state->lock); return -ENODEV; }
            a->deadline = jiffies + msecs_to_jiffies((369UL + max_t(unsigned long,
                300, 120 + DIV_ROUND_UP(a->total,8192))) * 1000);
            kref_get(&a->refs);
            cancel_delayed_work(&a->lease_work);
            smp_store_release(&a->submitted, true);
            queue_work(system_unbound_wq, &a->work);
        }
        mutex_unlock(&state->lock);
    }
    remaining = time_after_eq(jiffies,a->deadline) ? 0 : a->deadline-jiffies;
    result = wait_for_completion_interruptible_timeout(&a->done, remaining);
    return result > 0 ? a->result : result == 0 ? -ETIMEDOUT : -ERESTARTSYS;
}
static void d585_compat_release(struct ds5 *state)
{
    struct d585_compat *a = READ_ONCE(state->dfu_compat);
    kref_put(&a->refs, d585_compat_free);
}
