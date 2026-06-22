#ifndef _KDS_PAGE_MGR_H
#define _KDS_PAGE_MGR_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/rhashtable.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/kds.h>      /* kds_page_t, kds_page_id_t, kds_page_hdr_t */

/* ------------------------------------------------------------------
 * Buffer pool sizing
 *
 * KDS_BUF_NR_FRAMES is the fixed number of frames. This is the very
 * first cut of the page manager: NO eviction is implemented yet.
 * Once all frames are in use, kds_buf_lookup_or_load() simply fails
 * with -ENOSPC. Eviction will be added as a separate follow-up.
 * ------------------------------------------------------------------ */
#define KDS_BUF_NR_FRAMES      4096
#define KDS_BUF_PARTITIONS     16
#define KDS_BUF_INVALID_FRAME  ((u32)-1)

/* ------------------------------------------------------------------
 * Frame state
 *
 * A frame is a fixed slot that owns at most one kds_page_t instance
 * at a time. Frame memory (the array slot itself, its lock) is never
 * reallocated during the module's lifetime; only the page_id/kp it
 * holds changes.
 * ------------------------------------------------------------------ */
typedef enum kds_frame_state {
    KDS_FRAME_FREE      = 0,   /* not mapped to any page_id */
    KDS_FRAME_LOADING   = 1,   /* disk read in flight, not yet visible in map */
    KDS_FRAME_VALID     = 2,   /* registered in the mapping table, usable */
} kds_frame_state_t;

/*
 * kds_frame_t: one slot of the buffer pool.
 *
 * Ownership split (see kds_core.h for the kds_page_t side of this):
 *   - `page` here is the actual buffer memory. The frame is the sole
 *     owner of this memory; nothing else holds a pointer to it.
 *   - `kp` is the content-lock + metadata object. Its `lock` field is
 *     what get_write_ptr()/get_read_ptr() take before touching `page`
 *     below; kp itself has no way to reach `page` on its own.
 *
 * Locking:
 *   - frame_lock protects `state` transitions and `mapped_addr`
 *     bookkeeping (the "buffer lock" portion of this frame).
 *   - kp->lock (content lock, defined in kds.h) protects the actual
 *     8KB page content (`page` below) and `kp->hdr`. It is taken by
 *     kds_frame_get_*_ptr()/released by kds_frame_put_*_ptr().
 *
 * Lock ordering: frame_lock is only ever held for short bookkeeping
 * sections (state checks, refcnt checks) and is never held across a
 * kp->lock acquisition. Callers must not invert this order.
 */
typedef struct kds_frame {
    u32                 frame_id;      /* index into pool->frames[], for debugging */
    kds_frame_state_t   state;
    kds_page_id_t       page_id;       /* valid only when state != FREE */
    kds_page_t          *kp;           /* content lock + metadata, owned by this frame */
    struct page         *page;         /* the actual buffer memory, owned by this frame */
    void                *mapped_addr;  /* kmap result, valid between get/put pairs */
    spinlock_t          frame_lock;
    struct list_head    free_node;     /* linked into pool->free_frames when FREE */
} kds_frame_t;

/* Mapping table entry: page_id -> frame_id */
struct kds_buf_entry {
    kds_page_id_t       page_id;
    u32                 frame_id;
    struct rhash_head   node;
};

/* ------------------------------------------------------------------
 * Buffer pool global state
 * ------------------------------------------------------------------ */
typedef struct kds_buf_pool {
    kds_frame_t         frames[KDS_BUF_NR_FRAMES];

    struct rhashtable   map;            /* page_id -> kds_buf_entry, RCU lookup */

    /*
     * Partition locks serialize insert/remove against the mapping
     * table and also serialize concurrent loads of the same page_id
     * (so two threads racing on a cache miss don't both issue disk
     * reads / both insert a frame for the same page_id).
     * Plain lookups do not need these locks; they go through
     * rhashtable's own RCU-protected lookup.
     */
    spinlock_t          part_lock[KDS_BUF_PARTITIONS];

    struct list_head    free_frames;    /* frames with state == FREE */
    spinlock_t          free_list_lock;

    atomic_t            nr_free;        /* stats only */
} kds_buf_pool_t;

static inline spinlock_t *
kds_buf_partition_lock(kds_buf_pool_t *pool, kds_page_id_t page_id)
{
    return &pool->part_lock[page_id % KDS_BUF_PARTITIONS];
}

/* ------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------ */

/*
 * Called once at module load. Initializes the frame array (all FREE,
 * no backing struct page allocated yet -- pages are allocated lazily
 * on first load), the rhashtable and the free list.
 * Returns 0 on success, negative errno on failure.
 */
int kds_buf_pool_init(void);

/* Called at module unload. Frees any struct page still held by a
 * frame. Does NOT flush dirty frames in this minimal version --
 * callers are expected to have flushed/checkpointed before unload. */
void kds_buf_pool_destroy(void);

/* ------------------------------------------------------------------
 * Lookup / Load
 *
 * Usage pattern:
 *
 *   frame = kds_buf_lookup_or_load(page_id);
 *   if (IS_ERR(frame))
 *       return PTR_ERR(frame);
 *
 *   p = kds_frame_get_write_ptr(frame, offset, size);
 *   ... write directly into p, no memcpy through a staging buffer ...
 *   kds_frame_put_write_ptr(frame);
 *
 *   kds_buf_unpin(frame);
 * ------------------------------------------------------------------ */

/*
 * Cache-hit-only lookup. Does not touch disk. Pins the frame before
 * returning so it cannot be reused while the caller holds it.
 * Returns NULL on cache miss.
 */
kds_frame_t *kds_buf_lookup(kds_page_id_t page_id);

/*
 * Looks up page_id; on miss, reads it from disk into a freshly
 * acquired frame and registers it in the mapping table. Disk I/O is
 * performed outside of any partition lock. Concurrent loads of the
 * same page_id are serialized so only one of them actually hits the
 * disk; the losers just pick up the winner's frame.
 *
 * Returns a pinned frame, or ERR_PTR(-ENODEV/-ENOMEM/-EIO/-ENOSPC).
 * -ENOSPC means the pool is full (no eviction implemented yet).
 */
kds_frame_t *kds_buf_lookup_or_load(kds_page_id_t page_id);

/* Increment refcnt. Lookup functions already pin on success; call
 * this only when handing the frame to another context that will
 * unpin independently. */
void kds_buf_pin(kds_frame_t *frame);

/* Decrement refcnt. Every frame obtained via lookup()/lookup_or_load()
 * must be unpinned exactly once. */
void kds_buf_unpin(kds_frame_t *frame);

/* ------------------------------------------------------------------
 * Direct buffer access (eliminates the memory-to-memory copy)
 *
 * These replace the old "build data elsewhere, then memcpy into the
 * page" pattern. The caller gets a pointer straight into the mapped
 * page and writes/reads in place.
 * ------------------------------------------------------------------ */

/*
 * Takes the content lock (frame->kp->lock) and returns a writable
 * pointer to [offset, offset + size) within the page. Must be paired
 * with kds_frame_put_write_ptr(). Bounds-checked against KDS_PAGE_SIZE.
 */
void *kds_frame_get_write_ptr(kds_frame_t *frame, kds_offset_t offset, kds_size_t size);

/* Marks the frame dirty and releases the content lock acquired by
 * kds_frame_get_write_ptr(). */
void kds_frame_put_write_ptr(kds_frame_t *frame);

/*
 * Takes the content lock and returns a read-only pointer to
 * [offset, offset + size) within the page. Must be paired with
 * kds_frame_put_read_ptr().
 */
const void *kds_frame_get_read_ptr(kds_frame_t *frame, kds_offset_t offset, kds_size_t size);

/* Releases the content lock acquired by kds_frame_get_read_ptr(). Does
 * NOT mark the frame dirty. */
void kds_frame_put_read_ptr(kds_frame_t *frame);

/* ------------------------------------------------------------------
 * Flush
 * ------------------------------------------------------------------ */

/*
 * Writes frame's backing page out to disk via kds_write_logical_page()
 * (blkdev.c) if it is currently marked dirty, then clears the dirty
 * flag. No-op (returns 0) if the frame is not dirty.
 *
 * Caller must hold a pin on frame (i.e. have obtained it via
 * kds_buf_lookup()/kds_buf_lookup_or_load() and not yet unpinned).
 * This does NOT remove the frame from the mapping table or the
 * checkpointer's dirty list bookkeeping beyond clearing the flag --
 * integrating with the checkpointer's dirty list traversal is a
 * separate follow-up.
 */
int kds_frame_flush(kds_frame_t *frame);
kds_frame_t *kds_buf_alloc_new(kds_page_id_t page_id);

#endif /* _KDS_PAGE_MGR_H */