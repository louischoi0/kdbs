/*
 * kds_page_mgr.c
 *
 * Minimal buffer pool / page manager.
 *
 * Scope of this first cut:
 *   - fixed-size frame array, no eviction (pool full => -ENOSPC)
 *   - page_id -> frame_id mapping via rhashtable
 *   - lookup (cache-hit only) and lookup_or_load (miss triggers disk read)
 *   - direct write/read pointer API so upper layers (btree/executor) write
 *     straight into the mapped page instead of staging into a temporary
 *     buffer and memcpy-ing it in (see kds_set_page_buffer in page.c for
 *     the old pattern this replaces).
 *
 * NOT implemented here on purpose: eviction/victim selection, dirty list
 * integration with the checkpointer, CRC update on write. Those are
 * separate follow-ups once this minimal path is validated.
 */

#include <linux/kds.h>
#include <linux/kds_page.h>
#include <linux/kds_page_mgr.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/jhash.h>
#include <linux/rhashtable.h>

extern struct block_device *kds_bdev;

static kds_buf_pool_t *g_pool;

/* Order of struct page allocation needed to back one KDS_PAGE_SIZE
 * page given the kernel's native PAGE_SIZE (e.g. 8KiB / 4KiB => order 1).
 * Note: the split into individual base pages for I/O is no longer
 * this file's concern -- kds_read_logical_page()/kds_write_logical_page()
 * in blkdev.c own that mapping. This file only needs the order to
 * alloc_pages()/__free_pages() the contiguous backing memory. */
#define KDS_PAGE_ORDER  get_order(KDS_PAGE_SIZE)

/* ------------------------------------------------------------------
 * rhashtable params
 * ------------------------------------------------------------------ */

static u32 kds_buf_entry_hash(const void *data, u32 len, u32 seed)
{
    const kds_page_id_t *id = data;
    return jhash_2words((u32)(*id), (u32)(*id >> 32), seed);
}

static int kds_buf_entry_cmp(struct rhashtable_compare_arg *arg, const void *obj)
{
    const kds_page_id_t *id = arg->key;
    const struct kds_buf_entry *e = obj;
    return e->page_id != *id;
}

static const struct rhashtable_params kds_buf_params = {
    .head_offset    = offsetof(struct kds_buf_entry, node),
    .key_offset     = offsetof(struct kds_buf_entry, page_id),
    .key_len        = sizeof(kds_page_id_t),
    .hashfn         = kds_buf_entry_hash,
    .obj_cmpfn      = kds_buf_entry_cmp,
    .automatic_shrinking = true,
};

/* ------------------------------------------------------------------
 * sector mapping: page_id N starts at sector
 * DATA_PAGE_OFFSET + N * KDS_PAGE_SECTORS -- each page_id step must
 * advance by a full page's worth of sectors (KDS_PAGE_SECTORS), not
 * by 1 sector, or consecutive pages overlap on disk.
 * ------------------------------------------------------------------ */
static inline sector_t kds_page_sector(kds_page_id_t id)
{
    return (sector_t)(DATA_PAGE_OFFSET + id * KDS_PAGE_SECTORS);
}

/* ------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------ */

int kds_buf_pool_init(void)
{
    int i, ret;

    g_pool = vzalloc(sizeof(*g_pool));
    if (!g_pool)
        return -ENOMEM;

    ret = rhashtable_init(&g_pool->map, &kds_buf_params);
    if (ret) {
        vfree(g_pool);
        g_pool = NULL;
        return ret;
    }

    INIT_LIST_HEAD(&g_pool->free_frames);
    spin_lock_init(&g_pool->free_list_lock);

    for (i = 0; i < KDS_BUF_PARTITIONS; i++)
        spin_lock_init(&g_pool->part_lock[i]);

    for (i = 0; i < KDS_BUF_NR_FRAMES; i++) {
        kds_frame_t *f = &g_pool->frames[i];

        f->frame_id = i;
        f->state = KDS_FRAME_FREE;
        f->page_id = 0;
        f->kp = NULL;
        f->mapped_addr = NULL;
        spin_lock_init(&f->frame_lock);
        INIT_LIST_HEAD(&f->free_node);

        list_add_tail(&f->free_node, &g_pool->free_frames);
    }

    atomic_set(&g_pool->nr_free, KDS_BUF_NR_FRAMES);

    pr_info("kds_page_mgr: pool initialized, %d frames\n", KDS_BUF_NR_FRAMES);
    return 0;
}

void kds_buf_pool_destroy(void)
{
    int i;

    if (!g_pool)
        return;

    for (i = 0; i < KDS_BUF_NR_FRAMES; i++) {
        kds_frame_t *f = &g_pool->frames[i];

        if (f->state != KDS_FRAME_FREE && f->kp) {
            if (f->page)
                __free_pages(f->page, KDS_PAGE_ORDER);
            vfree(f->kp);
        }
    }

    rhashtable_destroy(&g_pool->map);
    vfree(g_pool);
    g_pool = NULL;
}

/* ------------------------------------------------------------------
 * Free frame acquisition (no eviction: fails if none available)
 * ------------------------------------------------------------------ */

static kds_frame_t *kds_buf_take_free_frame(void)
{
    kds_frame_t *f = NULL;
    unsigned long flags;

    spin_lock_irqsave(&g_pool->free_list_lock, flags);
    if (!list_empty(&g_pool->free_frames)) {
        f = list_first_entry(&g_pool->free_frames, kds_frame_t, free_node);
        list_del_init(&f->free_node);
        atomic_dec(&g_pool->nr_free);
    }
    spin_unlock_irqrestore(&g_pool->free_list_lock, flags);

    return f;
}

static void kds_buf_return_free_frame(kds_frame_t *f)
{
    unsigned long flags;

    spin_lock_irqsave(&g_pool->free_list_lock, flags);
    list_add_tail(&f->free_node, &g_pool->free_frames);
    atomic_inc(&g_pool->nr_free);
    spin_unlock_irqrestore(&g_pool->free_list_lock, flags);
}

/* ------------------------------------------------------------------
 * Disk read into a frame's backing page
 * ------------------------------------------------------------------ */

static int kds_frame_read_from_disk(kds_frame_t *f, kds_page_id_t page_id)
{
    struct page *page;
    sector_t sector;
    int ret;

    if (!kds_bdev)
        return -ENODEV;

    /* One contiguous allocation covering KDS_PAGE_SIZE, regardless of
     * how it relates to the kernel's native PAGE_SIZE. */
    page = alloc_pages(GFP_KERNEL, KDS_PAGE_ORDER);
    if (!page)
        return -ENOMEM;

    sector = kds_page_sector(page_id);

    /* kds_read_logical_page() (blkdev.c) owns the logical-page ->
     * base-page splitting; this file no longer builds a struct
     * page* array or calls kds_read_extent() directly. */
    ret = kds_read_logical_page(sector, page);
    if (ret) {
        __free_pages(page, KDS_PAGE_ORDER);
        return ret;
    }

    f->kp = vmalloc(sizeof(kds_page_t));
    if (!f->kp) {
        __free_pages(page, KDS_PAGE_ORDER);
        return -ENOMEM;
    }

    f->kp->id = page_id;
    f->page = page;
    spin_lock_init(&f->kp->lock);
    atomic64_set(&f->kp->refcnt, 0);
    INIT_LIST_HEAD(&f->kp->node);

    /* Header lives at the start of the page; same convention as
     * kds_read_page() in page.c. */
    {
        void *addr = kmap_local_page(page);
        memcpy(&f->kp->hdr, addr, KDS_PAGE_HDR_SIZE);
        kunmap_local(addr);
    }

    return 0;
}

/* ------------------------------------------------------------------
 * Lookup / Load
 * ------------------------------------------------------------------ */

kds_frame_t *kds_buf_lookup(kds_page_id_t page_id)
{
    struct kds_buf_entry *entry;
    kds_frame_t *f = NULL;

    if (!g_pool)
        return NULL;

    rcu_read_lock();
    entry = rhashtable_lookup(&g_pool->map, &page_id, kds_buf_params);
    if (entry) {
        f = &g_pool->frames[entry->frame_id];
        kds_buf_pin(f);
    }
    rcu_read_unlock();

    return f;
}

kds_frame_t *kds_buf_lookup_or_load(kds_page_id_t page_id)
{
    spinlock_t *plock;
    struct kds_buf_entry *entry, *new_entry;
    kds_frame_t *f;
    int ret;

    if (!g_pool)
        return ERR_PTR(-ENODEV);

    /* Fast path: already cached. */
    f = kds_buf_lookup(page_id);
    if (f)
        return f;

    /* Slow path: take a free frame and load from disk. The frame is
     * not registered in the mapping table yet (state == LOADING), so
     * concurrent lookups for the same page_id will not see it until
     * the load completes and it is inserted below. The partition
     * lock below is what actually serializes concurrent loaders of
     * the same page_id. */
    f = kds_buf_take_free_frame();
    if (!f)
        return ERR_PTR(-ENOSPC);

    f->state = KDS_FRAME_LOADING;
    f->page_id = page_id;

    ret = kds_frame_read_from_disk(f, page_id);
    if (ret) {
        f->state = KDS_FRAME_FREE;
        f->page_id = 0;
        kds_buf_return_free_frame(f);
        return ERR_PTR(ret);
    }

    new_entry = kzalloc(sizeof(*new_entry), GFP_KERNEL);
    if (!new_entry) {
        if (f->page)
            __free_pages(f->page, KDS_PAGE_ORDER);
        vfree(f->kp);
        f->kp = NULL;
        f->state = KDS_FRAME_FREE;
        f->page_id = 0;
        kds_buf_return_free_frame(f);
        return ERR_PTR(-ENOMEM);
    }
    new_entry->page_id = page_id;
    new_entry->frame_id = f->frame_id;

    plock = kds_buf_partition_lock(g_pool, page_id);
    spin_lock(plock);

    /* Re-check under the partition lock: another thread may have
     * loaded and inserted the same page_id while we were doing our
     * own disk read above. If so, discard our copy and use theirs. */
    entry = rhashtable_lookup_fast(&g_pool->map, &page_id, kds_buf_params);
    if (entry) {
        spin_unlock(plock);
        kfree(new_entry);

        __free_pages(f->page, KDS_PAGE_ORDER);
        vfree(f->kp);
        f->kp = NULL;
        f->state = KDS_FRAME_FREE;
        f->page_id = 0;
        kds_buf_return_free_frame(f);

        f = &g_pool->frames[entry->frame_id];
        kds_buf_pin(f);
        return f;
    }

    ret = rhashtable_insert_fast(&g_pool->map, &new_entry->node, kds_buf_params);
    if (ret) {
        spin_unlock(plock);
        kfree(new_entry);

        __free_pages(f->page, KDS_PAGE_ORDER);
        vfree(f->kp);
        f->kp = NULL;
        f->state = KDS_FRAME_FREE;
        f->page_id = 0;
        kds_buf_return_free_frame(f);
        return ERR_PTR(ret);
    }

    f->state = KDS_FRAME_VALID;
    kds_buf_pin(f);

    spin_unlock(plock);

    return f;
}

void kds_buf_pin(kds_frame_t *frame)
{
    atomic64_inc(&frame->kp->refcnt);
}

void kds_buf_unpin(kds_frame_t *frame)
{
    atomic64_dec(&frame->kp->refcnt);
}

kds_frame_t *kds_buf_alloc_new(kds_page_id_t page_id)
{
    spinlock_t *plock;
    struct kds_buf_entry *entry, *new_entry;
    kds_frame_t *f;
    struct page *page;
    int ret;

    if (!g_pool)
        return ERR_PTR(-ENODEV);

    /* Reject up front if it's already cached -- allocating a page_id
     * that's already live indicates a bug in the caller (the
     * extent/free-space allocator handed out an id that's still in
     * use), not a benign race to be resolved silently. */
    f = kds_buf_lookup(page_id);
    if (f) {
        kds_buf_unpin(f);
        return ERR_PTR(-EEXIST);
    }

    f = kds_buf_take_free_frame();
    if (!f)
        return ERR_PTR(-ENOSPC);

    f->state = KDS_FRAME_LOADING;
    f->page_id = page_id;

    page = alloc_pages(GFP_KERNEL, KDS_PAGE_ORDER);
    if (!page) {
        f->state = KDS_FRAME_FREE;
        f->page_id = 0;
        kds_buf_return_free_frame(f);
        return ERR_PTR(-ENOMEM);
    }

    f->kp = vmalloc(sizeof(kds_page_t));
    if (!f->kp) {
        __free_pages(page, KDS_PAGE_ORDER);
        f->state = KDS_FRAME_FREE;
        f->page_id = 0;
        kds_buf_return_free_frame(f);
        return ERR_PTR(-ENOMEM);
    }

    f->page = page;
    f->kp->id = page_id;
    spin_lock_init(&f->kp->lock);
    atomic64_set(&f->kp->refcnt, 0);
    INIT_LIST_HEAD(&f->kp->node);

    /* New page starts out type-less; the caller (e.g.
     * btree_init_root_kpage()/btree_init_data_kpage()) is expected to
     * set the real type. DIRTY is set unconditionally so this page is
     * guaranteed to reach disk even if the caller never marks it
     * dirty again before it gets flushed/evicted. */
    f->kp->hdr.type = KDS_PAGE_TYPE_INVALID;
    f->kp->hdr.crc = 0;
    f->kp->hdr.flags = KDS_PAGE_FLAG_ALLOC | KDS_PAGE_FLAG_INIT | KDS_PAGE_FLAG_DIRTY;

    {
        void *addr = kmap_local_page(page);

        memset(addr, 0, KDS_PAGE_SIZE);
        memcpy(addr, &f->kp->hdr, KDS_PAGE_HDR_SIZE);

        kunmap_local(addr);
    }

    new_entry = kzalloc(sizeof(*new_entry), GFP_KERNEL);
    if (!new_entry) {
        __free_pages(f->page, KDS_PAGE_ORDER);
        vfree(f->kp);
        f->kp = NULL;
        f->page = NULL;
        f->state = KDS_FRAME_FREE;
        f->page_id = 0;
        kds_buf_return_free_frame(f);
        return ERR_PTR(-ENOMEM);
    }
    new_entry->page_id = page_id;
    new_entry->frame_id = f->frame_id;

    plock = kds_buf_partition_lock(g_pool, page_id);
    spin_lock(plock);

    /* Re-check under the partition lock: if another caller raced us
     * and allocated the same page_id in the meantime, that is the
     * upstream bug described above -- fail loudly rather than
     * silently merging with their frame. */
    entry = rhashtable_lookup_fast(&g_pool->map, &page_id, kds_buf_params);
    if (entry) {
        spin_unlock(plock);
        kfree(new_entry);

        __free_pages(f->page, KDS_PAGE_ORDER);
        vfree(f->kp);
        f->kp = NULL;
        f->page = NULL;
        f->state = KDS_FRAME_FREE;
        f->page_id = 0;
        kds_buf_return_free_frame(f);

        return ERR_PTR(-EEXIST);
    }

    ret = rhashtable_insert_fast(&g_pool->map, &new_entry->node, kds_buf_params);
    if (ret) {
        spin_unlock(plock);
        kfree(new_entry);

        __free_pages(f->page, KDS_PAGE_ORDER);
        vfree(f->kp);
        f->kp = NULL;
        f->page = NULL;
        f->state = KDS_FRAME_FREE;
        f->page_id = 0;
        kds_buf_return_free_frame(f);
        return ERR_PTR(ret);
    }

    f->state = KDS_FRAME_VALID;
    kds_buf_pin(f);

    spin_unlock(plock);

    return f;
}

/* ------------------------------------------------------------------
 * Direct buffer access
 * ------------------------------------------------------------------ */

void *kds_frame_get_write_ptr(kds_frame_t *frame, kds_offset_t offset, kds_size_t size)
{
    void *base;

    if (offset + size > KDS_PAGE_SIZE)
        return ERR_PTR(-EINVAL);

    kds_page_lock(frame->kp);

    base = kmap_local_page(frame->page);
    frame->mapped_addr = base;

    return base + offset;
}

void kds_frame_put_write_ptr(kds_frame_t *frame)
{
    kunmap_local(frame->mapped_addr);
    frame->mapped_addr = NULL;

    kds_set_page_dirty(frame->kp);
    kds_page_unlock(frame->kp);
}

const void *kds_frame_get_read_ptr(kds_frame_t *frame, kds_offset_t offset, kds_size_t size)
{
    void *base;

    if (offset + size > KDS_PAGE_SIZE)
        return ERR_PTR(-EINVAL);

    kds_page_lock(frame->kp);

    base = kmap_local_page(frame->page);
    frame->mapped_addr = base;

    return base + offset;
}

void kds_frame_put_read_ptr(kds_frame_t *frame)
{
    kunmap_local(frame->mapped_addr);
    frame->mapped_addr = NULL;

    kds_page_unlock(frame->kp);
}

/* ------------------------------------------------------------------
 * Flush
 * ------------------------------------------------------------------ */

int kds_frame_flush(kds_frame_t *frame)
{
    sector_t sector;
    int ret;

    if (!frame || !frame->kp)
        return -EINVAL;

    kds_page_lock(frame->kp);

    if (!(frame->kp->hdr.flags & KDS_PAGE_FLAG_DIRTY)) {
        kds_page_unlock(frame->kp);
        return 0;
    }

    sector = kds_page_sector(frame->page_id);

    /* Header may have been updated in-place via get_write_ptr() without
     * going through kds_commit_page_hdr(); write it back out before the
     * data write so what lands on disk matches frame->kp->hdr. Done via
     * the same direct-pointer convention used elsewhere in this file. */
    {
        void *addr = kmap_local_page(frame->page);
        memcpy(addr, &frame->kp->hdr, KDS_PAGE_HDR_SIZE);
        kunmap_local(addr);
    }

    ret = kds_write_logical_page(sector, frame->page);
    if (ret) {
        kds_page_unlock(frame->kp);
        return ret;
    }

    frame->kp->hdr.flags &= ~KDS_PAGE_FLAG_DIRTY;

    kds_page_unlock(frame->kp);
    return 0;
}

void kds_buf_pool_get_stats(u32 *out_total, u32 *out_free, u32 *out_valid)
{
    u32 free_count = 0;
    u32 valid_count = 0;
    int i;

    if (out_total)
        *out_total = KDS_BUF_NR_FRAMES;

    if (!g_pool) {
        if (out_free)
            *out_free = 0;
        if (out_valid)
            *out_valid = 0;
        return;
    }

    /*
     * Plain scan, no locking: this is a best-effort debug snapshot,
     * not a value anything correctness-sensitive depends on. Each
     * frame's state can change concurrently while this loop runs;
     * the counts may be off by a frame or two under load, which is
     * an acceptable tradeoff against taking 4096 individual
     * frame_locks (or one global lock) just to report a number for
     * a diagnostic command.
     */
    for (i = 0; i < KDS_BUF_NR_FRAMES; i++) {
        switch (g_pool->frames[i].state) {
        case KDS_FRAME_FREE:
            free_count++;
            break;
        case KDS_FRAME_VALID:
            valid_count++;
            break;
        default:
            break;
        }
    }

    if (out_free)
        *out_free = free_count;
    if (out_valid)
        *out_valid = valid_count;
}