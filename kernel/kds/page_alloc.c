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
 * Page allocator -- glue between kds_meta.c (which page_id to issue)
 * and kds_page_mgr.c (how to register a page in the buffer pool).
 *
 * Design:
 *
 *   Ring buffer (cursor / reserved, one-slot-wasted):
 *     cursor == reserved          → empty
 *     (reserved+1)%SIZE == cursor → full  (SIZE-1 usable slots)
 *
 *   alloc_point (persisted in superblock):
 *     0   → first boot; no pre-allocated pages exist on disk yet.
 *     > 0 → subsequent boot; the last batch started at alloc_point.
 *           Reclaim range on boot:
 *             [alloc_point, min(alloc_point + PRE_ALLOC_NUM, max_page_id))
 *           Pages in this range that are ALLOC-but-not-INIT were
 *           staged in the ring but never used; recycle them.
 *
 *   Batch allocation (kds_batch_alloc):
 *     1. alloc_page_id_batch(count) → base
 *     2. kds_meta_set_alloc_range(base) persisted immediately so the
 *        next boot knows where to start reclaiming.
 *     3. Pages [base, base+count) are created and pushed into the ring.
 *
 *   Threshold refill:
 *     kds_proc_prealloc() fires whenever the ring drops below
 *     PRE_ALLOC_PAGE_THRES and calls kds_batch_alloc() to top up.
 */

static kds_page_allocator_t *g_alloc;
static kds_proc_t            *kds_prealloc_proc;

/* ------------------------------------------------------------------
 * Locking helpers
 * ------------------------------------------------------------------ */

static inline void lock_g_alloc(void)   { spin_lock(&g_alloc->lock); }
static inline void unlock_g_alloc(void) { spin_unlock(&g_alloc->lock); }

/* ------------------------------------------------------------------
 * Ring buffer primitives  (one-slot-wasted circular buffer)
 *
 *   cursor == reserved              → empty (0 items)
 *   (reserved + 1) % SIZE == cursor → full  (SIZE-1 items max)
 *   count = (reserved - cursor + SIZE) % SIZE
 * ------------------------------------------------------------------ */

static inline bool kds_ring_is_full(void)
{
    return (g_alloc->reserved + 1) % PRE_ALLOC_RING_BUFFER_SIZE
           == g_alloc->cursor;
}

static inline u64 kds_ring_count(void)
{
    return (g_alloc->reserved - g_alloc->cursor
            + PRE_ALLOC_RING_BUFFER_SIZE) % PRE_ALLOC_RING_BUFFER_SIZE;
}

static inline u64 kds_ring_free_slots(void)
{
    return PRE_ALLOC_RING_BUFFER_SIZE - 1 - kds_ring_count();
}

/* Caller must hold g_alloc->lock and verify !kds_ring_is_full(). */
static void ring_push(kds_frame_t *frame)
{
    g_alloc->ring[g_alloc->reserved] = frame;
    g_alloc->reserved = (g_alloc->reserved + 1) % PRE_ALLOC_RING_BUFFER_SIZE;
}

/* ------------------------------------------------------------------
 * Consumer: hand out one pre-allocated frame
 * ------------------------------------------------------------------ */

kds_frame_t *kds_page_alloc(kds_page_type_t type)
{
    kds_frame_t *frame;

    lock_g_alloc();

    if (kds_ring_count() == 0) {
        unlock_g_alloc();
        pr_warn("kds_page_alloc: ring empty\n");
        return NULL;
    }

    frame = g_alloc->ring[g_alloc->cursor];
    g_alloc->ring[g_alloc->cursor] = NULL;
    g_alloc->cursor = (g_alloc->cursor + 1) % PRE_ALLOC_RING_BUFFER_SIZE;

    unlock_g_alloc();

    kds_page_lock(frame->kp);
    frame->kp->hdr.type = type;
    kds_page_unlock(frame->kp);
    pr_info("kds_page_alloc: cursor=%d, reserved=%d, ret=%d, pgid=%d\n", g_alloc->cursor, g_alloc->reserved, frame, frame->kp->id);

    return frame;
}

kds_frame_t *kds_get_reserved_kpage(kds_page_type_t type)
{
    return kds_page_alloc(type);
}

/* ------------------------------------------------------------------
 * Core batch allocator
 *
 * Issues `count` brand-new page ids, persists alloc_point = base
 * (the start of this batch) immediately, then creates and stages
 * each page into the ring.
 *
 * Persisting alloc_point before touching any pages ensures that a
 * crash mid-batch leaves a recoverable footprint: the next boot will
 * scan [base, min(base + PRE_ALLOC_NUM, max_page_id)) and reclaim
 * any ALLOC-but-not-INIT pages rather than issuing duplicate ids.
 *
 * Disk I/O is done outside the ring lock; the lock is taken only for
 * ring_push() of each individual frame.
 *
 * Returns the number of pages successfully staged (>= 0).
 * ------------------------------------------------------------------ */

static kds_size_t kds_batch_alloc(kds_size_t count)
{
    kds_page_id_t base;
    kds_size_t    want, got = 0;
    kds_size_t    i;

    if (count == 0)
        return 0;

    /* Clamp to available ring slots. */
    want = min_t(kds_size_t, count, kds_ring_free_slots());
    if (want == 0)
        return 0;

    base = alloc_page_id_batch(want);
    if (base == 0) {
        base += KDS_SYS_RESERVED_PAGES;
    }

    /*
     * Persist alloc_point = base (the *start* of this batch) before
     * writing any pages. On the next boot, the reclaim scan covers
     * [base, min(base + PRE_ALLOC_NUM, max_page_id)).
     */

    for (i = 0; i < want; i++) {
        kds_page_id_t id    = base + i;
        kds_frame_t  *frame = kds_buf_alloc_new(id);

        if (IS_ERR(frame)) {
            pr_err("kds_page_alloc: kds_buf_alloc_new(%llu) failed: %ld\n",
                   (u64)id, PTR_ERR(frame));
            break;
        }

        /*
         * kds_buf_alloc_new() already writes the header to disk
         * internally -- no separate kds_commit_page_hdr() or
         * kds_write_page() call needed here.
         */
        lock_g_alloc();
        if (kds_ring_is_full()) {
            unlock_g_alloc();
            kds_buf_unpin(frame);
            break;
        }
        ring_push(frame);
        unlock_g_alloc();

        got++;
    }

    kds_meta_set_alloc_range(base, 0);
    kds_superblock_fsync();

    pr_debug("kds_page_alloc: batch_alloc: base=%llu want=%zu got=%zu\n", (u64)base, want, got);
    return got;
}

/* ------------------------------------------------------------------
 * Boot-time reclaim scan
 *
 * Scans [alloc_point, min(alloc_point + PRE_ALLOC_NUM, max_page_id))
 * for pages that were allocated (KDS_PAGE_FLAG_ALLOC set) but never
 * initialised with user data (KDS_PAGE_FLAG_INIT not set).
 *
 * These pages were staged in the previous boot's ring but never
 * handed out to a caller, or were handed out but the caller crashed
 * before writing data. Either way they are safe to recycle.
 *
 * Pages with INIT set belong to live catalog or user-table data and
 * must not be touched.
 *
 * Returns the number of pages staged into the ring.
 * ------------------------------------------------------------------ */

static kds_size_t kds_reclaim_uninitialized_pages(kds_page_id_t alloc_point)
{
    kds_page_id_t max_id   = get_max_page_id();
    kds_page_id_t scan_end = min_t(kds_page_id_t,
                                    alloc_point + PRE_ALLOC_NUM,
                                    max_id);
    kds_page_id_t id;
    kds_size_t    got = 0;

    if (alloc_point >= scan_end) {
        pr_info("kds_page_alloc: reclaim: alloc_point=%llu max_id=%llu "
                "-- nothing to reclaim\n",
                (u64)alloc_point, (u64)max_id);
        return 0;
    }

    pr_info("kds_page_alloc: reclaim scan [%llu, %llu)\n",
            (u64)alloc_point, (u64)scan_end);

    for (id = alloc_point; id < scan_end; id++) {
        kds_frame_t *frame;
        bool is_alloc, is_init;

        /* Stop early if the ring is already full. */
        lock_g_alloc();
        if (kds_ring_is_full()) {
            unlock_g_alloc();
            break;
        }
        unlock_g_alloc();

        frame = kds_buf_lookup_or_load(id);
        if (IS_ERR(frame)) {
            pr_debug("kds_page_alloc: reclaim: cannot load id %llu (%ld), "
                     "skipping\n", (u64)id, PTR_ERR(frame));
            continue;
        }

        kds_page_lock(frame->kp);
        is_alloc = !!(frame->kp->hdr.flags & KDS_PAGE_FLAG_ALLOC);
        is_init  = !!(frame->kp->hdr.flags & KDS_PAGE_FLAG_INIT);
        kds_page_unlock(frame->kp);

        if (!is_alloc || is_init) {
            kds_buf_unpin(frame);
            continue;
        }

        lock_g_alloc();
        if (kds_ring_is_full()) {
            unlock_g_alloc();
            kds_buf_unpin(frame);
            break;
        }
        ring_push(frame);
        unlock_g_alloc();

        pr_debug("kds_page_alloc: reclaim: recycled id %llu\n", (u64)id);
        got++;
    }

    pr_info("kds_page_alloc: reclaim: recovered %zu page(s)\n", got);
    return got;
}

/* ------------------------------------------------------------------
 * Background prealloc proc
 *
 * Fires on every scheduler round. Tops up the ring when it drops
 * below PRE_ALLOC_PAGE_THRES.
 * ------------------------------------------------------------------ */

kds_proc_result_t kds_proc_prealloc(struct kds_proc *proc, u64 slice_ns)
{
    u64 count;

    lock_g_alloc();
    count = kds_ring_count();
    unlock_g_alloc();

    if (count < PRE_ALLOC_PAGE_THRES) {
        pr_info("kds_proc_prealloc has executed kds batch alloc\n");
        kds_batch_alloc(PRE_ALLOC_RING_BUFFER_SIZE - count);
    }

    return KDS_PROC_YIELD_RET;
}

/* ------------------------------------------------------------------
 * Initialisation
 * ------------------------------------------------------------------ */

static void kds_init_g_alloc(void)
{
    g_alloc = kzalloc(sizeof(kds_page_allocator_t), GFP_KERNEL);
    BUG_ON(!g_alloc);

    g_alloc->cursor   = 0;
    g_alloc->reserved = 0;

    spin_lock_init(&g_alloc->lock);
    INIT_LIST_HEAD(&g_alloc->freelist);
}

static int kds_create_proc_prealloc(void)
{
    int ret;

    kds_prealloc_proc = vzalloc(sizeof(kds_proc_t));
    if (!kds_prealloc_proc)
        return -ENOMEM;

    kds_prealloc_proc->kind         = KDS_PROC_SYSTEM;
    kds_prealloc_proc->name         = "kds_proc_prealloc";
    kds_prealloc_proc->static_prio  = -1;
    kds_prealloc_proc->dynamic_prio = KDS_PROC_PRIORITY_SYSTEM_BACKGROUND;
    kds_prealloc_proc->run          = kds_proc_prealloc;
    kds_prealloc_proc->state        = KDS_PROC_STATE_READY;

    ret = kds_proc_register(kds_prealloc_proc);
    if (ret) {
        pr_err("kds_page_alloc: failed to register prealloc proc: %d\n", ret);
        vfree(kds_prealloc_proc);
        kds_prealloc_proc = NULL;
        return ret;
    }

    pr_info("kds_page_alloc: prealloc proc registered\n");
    return 0;
}

int kds_init_page_alloc_system(void)
{
    kds_page_id_t alloc_point;
    kds_size_t    filled = 0;

    kds_init_g_alloc();

    /*
     * alloc_point == 0 → first boot, no pages on disk yet.
     * alloc_point >  0 → subsequent boot; the last batch started at
     *                    alloc_point. Reclaim before minting new ids.
     */
    kds_meta_get_alloc_range(&alloc_point, NULL);

    if (alloc_point == 0) {
        pr_info("kds_page_alloc: first boot -- allocating initial batch\n");
        filled = kds_batch_alloc(PRE_ALLOC_RING_BUFFER_SIZE);
    } else {
        pr_info("kds_page_alloc: boot -- alloc_point=%llu, reclaiming\n", (u64)alloc_point);
        filled = kds_reclaim_uninitialized_pages(alloc_point);

        if (filled < PRE_ALLOC_PAGE_THRES) {
            kds_size_t need = PRE_ALLOC_RING_BUFFER_SIZE - filled;

            pr_info("kds_page_alloc: reclaim gave %zu page(s), "
                    "allocating %zu more\n", filled, need);
            filled += kds_batch_alloc(need);
        }
    }

    pr_info("kds_page_alloc: init complete -- %llu page(s) staged\n",
            kds_ring_count());

    return kds_create_proc_prealloc();
}

/* ------------------------------------------------------------------
 * Shutdown
 * ------------------------------------------------------------------ */

void kds_shutdown_page_alloc_system(void)
{
    kds_frame_t *frame;

    if (kds_prealloc_proc) {
        kds_proc_unregister(kds_prealloc_proc);
        vfree(kds_prealloc_proc);
        kds_prealloc_proc = NULL;
    }

    if (!g_alloc)
        return;

    lock_g_alloc();
    while (kds_ring_count() > 0) {
        frame = g_alloc->ring[g_alloc->cursor];
        g_alloc->ring[g_alloc->cursor] = NULL;
        g_alloc->cursor = (g_alloc->cursor + 1) % PRE_ALLOC_RING_BUFFER_SIZE;
        if (frame)
            kds_buf_unpin(frame);
    }
    unlock_g_alloc();

    kfree(g_alloc);
    g_alloc = NULL;

    pr_info("kds_page_alloc: shutdown complete\n");
}

/* ------------------------------------------------------------------
 * Stats (for PSTA dshell command)
 * ------------------------------------------------------------------ */

void kds_page_alloc_get_stats(kds_page_id_t *out_alloc_point,
                               u64           *out_alloc_remaining,
                               u64           *out_ring_count,
                               u64           *out_freelist_count)
{
    if (out_alloc_point)     *out_alloc_point     = 0;
    if (out_alloc_remaining) *out_alloc_remaining = 0;
    if (out_ring_count)      *out_ring_count      = 0;
    if (out_freelist_count)  *out_freelist_count  = 0;

    if (!g_alloc)
        return;

    if (out_alloc_point) {
    kds_page_id_t ap;
        kds_meta_get_alloc_range(&ap, NULL);
        *out_alloc_point = ap;
    }

    if (out_ring_count) {
        lock_g_alloc();
        *out_ring_count = kds_ring_count();
        unlock_g_alloc();
    }
}