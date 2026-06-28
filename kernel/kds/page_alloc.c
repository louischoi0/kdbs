/* page_alloc.c */
#include <linux/kds.h>
#include <linux/kds_page.h>
#include <linux/kds_page_mgr.h>
#include <linux/kds_meta.h>
#include <linux/kds_page_alloc.h>
#include <linux/kds_proc.h>
#include <linux/bug.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

/*
 * This file is the glue between two responsibilities that are
 * deliberately kept in separate files:
 *
 *   - kds_meta.c:      WHICH page_id to hand out next (assign_page_id()/
 *                      alloc_page_id_batch()), persisted in the superblock.
 *   - kds_page_mgr.c:  HOW to get a page_id registered in memory
 *                      (kds_buf_alloc_new() for brand-new pages,
 *                      kds_buf_lookup_or_load() for existing ones).
 *
 * Neither of those files knows about the other; this allocator is
 * what calls both in the right order and adds policy on top
 * (pre-allocation ring buffer, freelist of reusable ids).
 */

static kds_page_allocator_t *g_alloc;
static kds_proc_t *kds_prealloc_proc = NULL;

static int __kds_create_proc_prealloc(void);
static void kds_init_g_alloc(void);
static kds_tiny_t kds_get_alloc_phase(void);
static u64 kds_get_pre_alloc_count(void);

#define KDS_PRE_ALLOC_PHASE_FACTOR 8

static inline void lock_g_alloc_range(void)
{
    spin_lock(&g_alloc->range_lock);
}

static inline void unlock_g_alloc_range(void)
{
    spin_unlock(&g_alloc->range_lock);
}

static inline void lock_g_alloc_freelist(void)
{
    spin_lock(&g_alloc->freelist_lock);
}

static inline void unlock_g_alloc_freelist(void)
{
    spin_unlock(&g_alloc->freelist_lock);
}

static inline void lock_g_alloc(void)
{
    spin_lock(&g_alloc->lock);
}

static inline void unlock_g_alloc(void)
{
    spin_unlock(&g_alloc->lock);
}

/*
 * Ring buffer bookkeeping. These three helpers only ever touch the
 * ring indices/array under g_alloc->lock -- they must never be called
 * around blocking work (disk I/O). Every caller below that does I/O
 * does so *before* taking the lock, then takes the lock only for the
 * pointer-shuffling itself.
 */

static inline u64 kds_ring_free_slots(void)
{
    return PRE_ALLOC_RING_BUFFER_SIZE - kds_get_pre_alloc_count();
}

static void kds_put_page_into_reserved(kds_frame_t *frame)
{
    g_alloc->ring[g_alloc->reserved] = frame;
    g_alloc->reserved = (g_alloc->reserved + 1) % PRE_ALLOC_RING_BUFFER_SIZE;
}

u64 kds_get_pre_alloc_count(void)
{
    return (g_alloc->cursor > g_alloc->reserved
                ? g_alloc->reserved + PRE_ALLOC_RING_BUFFER_SIZE
                : g_alloc->reserved) - g_alloc->cursor;
}

kds_tiny_t kds_get_alloc_phase(void)
{
    u64 pre_allocs = kds_get_pre_alloc_count();
    return (PRE_ALLOC_RING_BUFFER_SIZE - pre_allocs) >> 5;
}

kds_frame_t *kds_page_alloc(kds_page_type_t type)
{
    return kds_get_reserved_kpage(type);
}

kds_frame_t *kds_get_reserved_kpage(kds_page_type_t type)
{
    kds_frame_t *frame;

    lock_g_alloc();

    if (kds_get_pre_alloc_count() < 1) {
        unlock_g_alloc();
        return NULL;
    }

    frame = g_alloc->ring[g_alloc->cursor];
    g_alloc->ring[g_alloc->cursor] = NULL;
    g_alloc->cursor = (g_alloc->cursor + 1) % PRE_ALLOC_RING_BUFFER_SIZE;

    unlock_g_alloc();

    /* Setting the type is a content-lock operation on frame->kp, not
     * a ring-bookkeeping one -- it happens after releasing g_alloc's
     * lock so we're never holding two unrelated locks at once. */
    kds_page_lock(frame->kp);
    frame->kp->hdr.type = type;
    kds_page_unlock(frame->kp);

    return frame;
}

void kds_add_kpage_freelist(kds_page_id_t id)
{
    kds_free_page_node_t *node = kmalloc(sizeof(*node), GFP_KERNEL);

    if (!node) {
        pr_err("kds_page_alloc: failed to allocate freelist node for page %llu, id leaked\n",
               id);
        return;
    }

    node->id = id;

    lock_g_alloc_freelist();
    list_add_tail(&node->node, &g_alloc->freelist);
    unlock_g_alloc_freelist();
}

/*
 * Pops one id off the freelist if available. Returns true and fills
 * *out_id on success, false if the freelist is empty.
 */
static bool kds_take_freelist_id(kds_page_id_t *out_id)
{
    kds_free_page_node_t *node = NULL;

    lock_g_alloc_freelist();
    if (!list_empty(&g_alloc->freelist)) {
        node = list_first_entry(&g_alloc->freelist, kds_free_page_node_t, node);
        list_del(&node->node);
    }
    unlock_g_alloc_freelist();

    if (!node)
        return false;

    *out_id = node->id;
    kfree(node);
    return true;
}

/*
 * Draws the next id out of the standing [alloc_point, alloc_point +
 * remaining) range, refilling that range when it runs out or drops
 * below PRE_ALLOC_PAGE_THRES. Returns 0 on failure (matches
 * kds_meta.h's assign_page_id()/alloc_page_id_batch() convention
 * that 0 is never a valid page_id).
 *
 * Locking: range_lock protects only the plain counter math
 * (alloc_point/remaining bookkeeping, including the
 * alloc_page_id_batch() call itself -- that's just an atomic op on
 * the superblock, no I/O). It is released BEFORE doing anything that
 * can block: kds_add_kpage_freelist() (GFP_KERNEL allocation) for
 * recycling leftover ids, and kds_superblock_fsync() (synchronous
 * disk I/O) for persisting the new range. Holding a spinlock across
 * either of those would be the same "scheduling while atomic" class
 * of bug this file already fixed once in kds_alloc_pages().
 *
 * KNOWN GAP: alloc_point/remaining are in-memory only, not persisted
 * anywhere (the superblock only persists the high-water mark
 * max_page_id, which alloc_page_id_batch() advances). On every
 * module reload, this allocator starts with remaining == 0 and mints
 * a fresh PRE_ALLOC_NUM range immediately on first use, abandoning
 * whatever tail of the previous boot's range was never handed out --
 * that abandoned tail is simply lost (max_page_id has already moved
 * past it, and it was never written to the on-disk freelist either,
 * since there isn't one -- kds_add_kpage_freelist() is in-memory
 * only too). This does NOT reproduce the original "always allocates
 * a bunch of brand new pages on every boot" symptom in the same way
 * (within one boot, ranges are now reused via the threshold/recycle
 * logic instead of minting per-request), but it does mean each fresh
 * boot still starts its own new range rather than continuing the
 * previous one. Fully closing that gap means persisting
 * (alloc_point, remaining) -- e.g. a superblock field, or flushing
 * the leftover range to an on-disk freelist at shutdown -- which is
 * a separate change from what was asked for here.
 */
static kds_page_id_t kds_alloc_next_id(void)
{
    kds_page_id_t id;
    kds_page_id_t recycle_start = 0;
    u64 recycle_count = 0;
    bool need_fsync = false;

    lock_g_alloc_range();

    if (g_alloc->remaining == 0) {
        kds_page_id_t new_point = alloc_page_id_batch(PRE_ALLOC_NUM);

        if (!new_point) {
            unlock_g_alloc_range();
            return 0;
        }

        g_alloc->alloc_point = new_point;
        g_alloc->remaining = PRE_ALLOC_NUM;
        need_fsync = true;
    }

    id = g_alloc->alloc_point++;
    g_alloc->remaining--;

    if (g_alloc->remaining > 0 && g_alloc->remaining < PRE_ALLOC_PAGE_THRES) {
        kds_page_id_t new_point = alloc_page_id_batch(PRE_ALLOC_NUM);

        if (new_point) {
            recycle_start = g_alloc->alloc_point;
            recycle_count = g_alloc->remaining;

            g_alloc->alloc_point = new_point;
            g_alloc->remaining = PRE_ALLOC_NUM;
            need_fsync = true;
        }
        /* If new_point is 0 (superblock not ready), just keep
         * draining the existing low range -- the next call(s) will
         * retry this same refill attempt until it succeeds or
         * remaining hits 0 and the first branch above takes over. */
    }

    unlock_g_alloc_range();

    if (recycle_count > 0) {
        u64 i;

        for (i = 0; i < recycle_count; i++)
            kds_add_kpage_freelist(recycle_start + i);

        pr_info("kds_page_alloc: recycled %llu leftover id(s) starting at %llu onto freelist\n",
                recycle_count, (u64)recycle_start);
    }

    if (need_fsync)
        kds_superblock_fsync();

    return id;
}

/*
 * Allocates up to `count` new pages and stages them in the
 * pre-allocation ring. May allocate fewer than `count` if the
 * freelist + fresh id batch together don't fill the remaining ring
 * capacity -- callers should treat this as best-effort top-up, not a
 * guaranteed exact count.
 *
 * Disk I/O (kds_buf_alloc_new, kds_commit_page_hdr, kds_write_page)
 * happens with no allocator lock held; only the final ring insertion
 * is done under g_alloc->lock. This matters because all three of
 * those calls can block (synchronous bio submission), and blocking
 * while holding a spinlock is a kernel-level bug (scheduling while
 * atomic), not just a style preference.
 */
int kds_alloc_pages(kds_size_t count)
{
    kds_frame_t **staged;
    kds_size_t want, got = 0;
    kds_size_t i;
    int ret = 0;

    if (count == 0)
        return 0;

    want = min_t(kds_size_t, count, kds_ring_free_slots());
    if (want == 0)
        return 0;

    staged = vmalloc(sizeof(kds_frame_t *) * want);
    if (!staged)
        return -ENOMEM;

    /* Prefer reusing freed ids before minting new ones. */
    while (got < want) {
        kds_page_id_t id;

        if (!kds_take_freelist_id(&id))
            break;

        staged[got] = kds_buf_alloc_new(id);
        if (IS_ERR(staged[got])) {
            pr_warn("kds_page_alloc: failed to re-alloc freed page %llu: %ld\n",
                    id, PTR_ERR(staged[got]));
            continue; /* drop this id, try the next freelist entry */
        }

        kds_commit_page_hdr(staged[got]);
        kds_write_page(staged[got]);
        got++;
    }

    /* Mint ids for whatever is still missing, drawing from the
     * standing range one id at a time via kds_alloc_next_id() --
     * this is what actually limits how often the superblock counter
     * gets bumped (PRE_ALLOC_NUM ids per refill, not once per
     * request) and recycles any range leftover instead of abandoning
     * it. */
    if (got < want) {
        kds_size_t missing = want - got;

        for (i = 0; i < missing; i++) {
            kds_page_id_t id = kds_alloc_next_id();
            kds_frame_t *frame;

            if (!id) {
                ret = -ENOMEM;
                break;
            }

            frame = kds_buf_alloc_new(id);
            if (IS_ERR(frame)) {
                pr_err("kds_page_alloc: kds_buf_alloc_new(%llu) failed: %ld\n",
                       (u64)id, PTR_ERR(frame));
                ret = PTR_ERR(frame);
                break;
            }

            kds_commit_page_hdr(frame);
            kds_write_page(frame);
            staged[got++] = frame;
        }
    }

    if (got > 0) {
        lock_g_alloc();
        for (i = 0; i < got; i++)
            kds_put_page_into_reserved(staged[i]);
        unlock_g_alloc();
    }

    vfree(staged);
    return ret;
}

kds_proc_result_t kds_proc_prealloc(struct kds_proc *proc, u64 slice_ns)
{
    kds_tiny_t phase = kds_get_alloc_phase();

    if (phase == 0)
        return KDS_PROC_YIELD_RET;

    kds_alloc_pages((kds_size_t)phase * KDS_PRE_ALLOC_PHASE_FACTOR);

    return KDS_PROC_YIELD_RET;
}

/*
 * Scans every existing page_id on boot and stages already-usable
 * pages into the pre-allocation ring. No allocator lock is held
 * across kds_buf_lookup_or_load() (it can block on disk I/O); the
 * lock is only taken for the final ring insertion, mirroring
 * kds_alloc_pages() above.
 */
void collect_pre_allocated_pages(void)
{
    kds_page_id_t max_page = get_max_page_id();
    kds_page_id_t i;

    for (i = 0; i <= max_page; i++) {
        kds_frame_t *frame = kds_buf_lookup_or_load(i);

        if (IS_ERR(frame))
            continue;

        if (PAGE_USABLE(frame->kp)) {
            lock_g_alloc();
            kds_put_page_into_reserved(frame);
            unlock_g_alloc();
            /* Ownership of this pin transfers to the ring; it is
             * released whenever the frame is handed out and later
             * freed by the caller, not here. */
        } else {
            kds_buf_unpin(frame);
        }
    }
}

void kds_init_g_alloc(void)
{
    g_alloc = kmalloc(sizeof(kds_page_allocator_t), GFP_KERNEL);
    BUG_ON(!g_alloc);

    g_alloc->reserved = 0;
    g_alloc->cursor = 0;

    /* Resume the id pre-allocation range from the last clean
     * shutdown instead of always starting at (0, 0) -- see
     * kds_superblock_t's alloc_point/alloc_remaining field comment
     * (kds_meta.h) for why this is only safe because it's only ever
     * persisted at clean shutdown. */
    kds_meta_get_alloc_range(&g_alloc->alloc_point, &g_alloc->remaining);
    spin_lock_init(&g_alloc->range_lock);

    spin_lock_init(&g_alloc->lock);
    spin_lock_init(&g_alloc->freelist_lock);
    INIT_LIST_HEAD(&g_alloc->freelist);
}

int __kds_create_proc_prealloc(void)
{
    int ret;

    /*
     * vzalloc(), not vmalloc() -- vmalloc() does not zero memory.
     * kds_proc_register() (kds_proc.c) relies on proc->allowed_cpus
     * starting out as an all-zero cpumask (it checks
     * cpumask_empty(&proc->allowed_cpus) to decide whether to
     * initialize it to cpu_online_mask) and on proc->preferred_cpu
     * being a sane default. Only kind/name/static_prio/
     * dynamic_prio/run/ctx/state are set explicitly below --
     * allowed_cpus, preferred_cpu, cpu, pid, and the timing fields
     * are left for kds_proc_register() to fill in, which only works
     * correctly if they start zeroed. With plain vmalloc(), they
     * were garbage, which made kds_proc_register() skip the
     * cpumask_copy() initialization and compute a garbage CPU index
     * for the per-CPU runqueue lookup -- the actual cause of the
     * NULL-pointer store fault inside kds_proc_register() (a
     * spin_lock() on a wild per_cpu() pointer derived from that
     * garbage index).
     */
    kds_prealloc_proc = vzalloc(sizeof(kds_proc_t));
    if (!kds_prealloc_proc) {
        pr_err("KDS: Failed to allocate meta process\n");
        return -ENOMEM;
    }

    kds_prealloc_proc->kind = KDS_PROC_SYSTEM;
    kds_prealloc_proc->name = "kds_proc_prealloc";
    kds_prealloc_proc->static_prio = -1;
    kds_prealloc_proc->dynamic_prio = KDS_PROC_PRIORITY_SYSTEM_BACKGROUND;
    kds_prealloc_proc->run = kds_proc_prealloc;
    kds_prealloc_proc->ctx = NULL;
    kds_prealloc_proc->state = KDS_PROC_STATE_READY;

    ret = kds_proc_register(kds_prealloc_proc);
    if (ret) {
        pr_err("KDS: Failed to register meta process: %d\n", ret);
        vfree(kds_prealloc_proc);
        kds_prealloc_proc = NULL;
        return ret;
    }

    pr_info("KDS: Meta process registered successfully\n");
    return 0;
}

/*
 * Initial synchronous fill, separate from the background
 * kds_proc_prealloc refill cycle: without this, the ring starts
 * completely empty and stays that way until the cooperative
 * scheduler happens to run kds_prealloc_proc at least once -- which
 * depends on worker kthreads having started and kds_bootstrap()
 * having already completed, neither of which is synchronized with
 * anything a caller of kds_page_alloc()/kds_get_reserved_kpage()
 * can observe. In particular, the userspace TCP server (kds_init.c)
 * starts and logs "listening" completely independently of this
 * kernel module's own boot progress -- a client request arriving
 * right after that log line has no guarantee the ring has been
 * touched yet at all. This fill makes kds_page_alloc() usable
 * immediately once kds_init_page_alloc_system() returns, instead of
 * depending on scheduler timing.
 */
#define KDS_PAGE_ALLOC_INITIAL_FILL  PRE_ALLOC_RING_BUFFER_SIZE

int kds_init_page_alloc_system(void)
{
    int ret;

    kds_init_g_alloc();
    /* collect_pre_allocated_pages(); -- left disabled, as before:
     * scanning every page_id on every boot doesn't scale once the
     * id space is large. Revisit once free-space tracking exists. */

    ret = kds_alloc_pages(KDS_PAGE_ALLOC_INITIAL_FILL);
    if (ret) {
        /*
         * Not necessarily fatal to overall bootstrap -- the
         * background proc registered below will keep retrying on
         * every schedule -- but worth surfacing loudly, since every
         * kds_page_alloc() call between now and the first successful
         * background refill will fail with the same symptom this
         * fill exists to prevent.
         */
        pr_warn("kds_page_alloc: initial ring fill failed (%d) -- "
                "kds_page_alloc() may return NULL until the background "
                "prealloc proc successfully refills it\n", ret);
    } else {
        pr_info("kds_page_alloc: initial ring fill complete (%llu page(s) staged)\n",
                kds_get_pre_alloc_count());
    }

    return __kds_create_proc_prealloc();
}

/*
 * Drains and frees this allocator's own state on module unload:
 *   - every frame still staged in the pre-allocation ring is unpinned
 *     (it was pinned when kds_buf_alloc_new()/kds_buf_lookup_or_load()
 *     placed it there)
 *   - every node on the id-reuse freelist is freed
 *   - g_alloc itself is freed
 *   - kds_prealloc_proc is removed from the scheduler via
 *     kds_proc_unregister() and then its memory (and its ctx, if it
 *     had one -- it doesn't, ctx is NULL for this proc) is released.
 *
 * Order matters: kds_proc_unregister() must run before vfree() --
 * it's what actually takes the runqueue's per-CPU lock and removes
 * the proc from whatever list/rb-tree holds it. Freeing first would
 * leave that structure pointing at freed memory.
 */
void kds_shutdown_page_alloc_system(void)
{
    kds_frame_t *frame;
    LIST_HEAD(local_freelist);
    struct kds_free_page_node *node, *tmp;
    kds_page_id_t persist_point;
    u64 persist_remaining;

    if (kds_prealloc_proc) {
        kds_proc_unregister(kds_prealloc_proc);
        vfree(kds_prealloc_proc);
        kds_prealloc_proc = NULL;
    }

    if (!g_alloc)
        return;

    /*
     * Snapshot the range now, while we still hold range_lock and
     * nothing else can be drawing from it -- the prealloc proc was
     * already unregistered above, and main.c only calls this after
     * stopping every worker kthread, so there is no concurrent
     * kds_alloc_next_id() caller left at this point. This is exactly
     * the "clean shutdown" condition kds_meta_set_alloc_range()'s
     * crash-safety reasoning depends on: persist below is safe
     * because we know for certain no id in [persist_point,
     * persist_point + persist_remaining) will be handed out by this
     * exiting instance.
     */
    lock_g_alloc_range();
    persist_point = g_alloc->alloc_point;
    persist_remaining = g_alloc->remaining;
    unlock_g_alloc_range();

    kds_meta_set_alloc_range(persist_point, persist_remaining);
    kds_superblock_fsync();

    lock_g_alloc();
    while (kds_get_pre_alloc_count() > 0) {
        frame = g_alloc->ring[g_alloc->cursor];
        g_alloc->ring[g_alloc->cursor] = NULL;
        g_alloc->cursor = (g_alloc->cursor + 1) % PRE_ALLOC_RING_BUFFER_SIZE;

        if (frame)
            kds_buf_unpin(frame);
    }
    unlock_g_alloc();

    lock_g_alloc_freelist();
    list_splice_init(&g_alloc->freelist, &local_freelist);
    unlock_g_alloc_freelist();

    list_for_each_entry_safe(node, tmp, &local_freelist, node) {
        list_del(&node->node);
        kfree(node);
    }

    kfree(g_alloc);
    g_alloc = NULL;

    pr_info("kds_page_alloc: shutdown complete, persisted alloc range "
            "[%llu, %llu) for next boot\n",
            (u64)persist_point, (u64)(persist_point + persist_remaining));
}

void kds_page_alloc_get_stats(kds_page_id_t *out_alloc_point, u64 *out_alloc_remaining,
                               u64 *out_ring_count, u64 *out_freelist_count)
{
    if (out_alloc_point)
        *out_alloc_point = 0;
    if (out_alloc_remaining)
        *out_alloc_remaining = 0;
    if (out_ring_count)
        *out_ring_count = 0;
    if (out_freelist_count)
        *out_freelist_count = 0;

    if (!g_alloc)
        return;

    lock_g_alloc_range();
    if (out_alloc_point)
        *out_alloc_point = g_alloc->alloc_point;
    if (out_alloc_remaining)
        *out_alloc_remaining = g_alloc->remaining;
    unlock_g_alloc_range();

    if (out_ring_count) {
        lock_g_alloc();
        *out_ring_count = kds_get_pre_alloc_count();
        unlock_g_alloc();
    }

    if (out_freelist_count) {
        struct kds_free_page_node *node;
        u64 count = 0;

        lock_g_alloc_freelist();
        list_for_each_entry(node, &g_alloc->freelist, node)
            count++;
        unlock_g_alloc_freelist();

        *out_freelist_count = count;
    }
}