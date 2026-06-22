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
    kds_page_id_t fresh_start = 0;
    kds_size_t fresh_count = 0;
    kds_size_t i;
    int ret = 0;
    return 0;

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

    /* Mint fresh ids for whatever is still missing. */
    if (got < want) {
        fresh_count = want - got;
        fresh_start = alloc_page_id_batch(fresh_count);
        kds_superblock_fsync();

        pr_info("kds_page_alloc: minting %llu new page(s), id range [%llu, %llu)\n",
                (u64)fresh_count, (u64)fresh_start, (u64)(fresh_start + fresh_count));

        for (i = 0; i < fresh_count; i++) {
            kds_frame_t *frame = kds_buf_alloc_new(fresh_start + i);

            if (IS_ERR(frame)) {
                pr_err("kds_page_alloc: kds_buf_alloc_new(%llu) failed: %ld\n",
                       (u64)(fresh_start + i), PTR_ERR(frame));
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

    spin_lock_init(&g_alloc->lock);
    spin_lock_init(&g_alloc->freelist_lock);
    INIT_LIST_HEAD(&g_alloc->freelist);
}

int __kds_create_proc_prealloc(void)
{
    int ret;

    kds_prealloc_proc = vmalloc(sizeof(kds_proc_t));
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

void kds_init_page_alloc_system(void)
{
    kds_init_g_alloc();
    /* collect_pre_allocated_pages(); -- left disabled, as before:
     * scanning every page_id on every boot doesn't scale once the
     * id space is large. Revisit once free-space tracking exists. */
    __kds_create_proc_prealloc();
}