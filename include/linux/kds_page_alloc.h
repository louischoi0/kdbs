/* kds_page_alloc.h */
#ifndef _KDS_PAGE_ALLOC_H
#define _KDS_PAGE_ALLOC_H
#include <linux/kds.h>
#include <linux/kds_meta.h>
#include <linux/kds_page_mgr.h>
#include <linux/list.h>
#include <linux/spinlock.h>

#define PRE_ALLOC_RING_BUFFER_SIZE 128

/*
 * Range-based id minting: rather than calling alloc_page_id_batch()
 * (which bumps the superblock-persisted counter) for every single
 * fresh page, the allocator keeps a standing contiguous range
 * [alloc_point, alloc_point + remaining) it hands ids out of one at
 * a time. When `remaining` drops below PRE_ALLOC_PAGE_THRES, a brand
 * new range of PRE_ALLOC_NUM ids is minted; whatever was left of the
 * old range is NOT discarded -- each of those leftover ids is pushed
 * onto the freelist (the same one kds_add_kpage_freelist() feeds) so
 * they get reused before the new range is touched.
 *
 * PRE_ALLOC_NUM must be larger than PRE_ALLOC_PAGE_THRES, or every
 * single id draw would trigger another refill.
 */
#define PRE_ALLOC_NUM           256
#define PRE_ALLOC_PAGE_THRES    32

int kds_init_page_alloc_system(void);
void kds_shutdown_page_alloc_system(void);

/*
 * Returns a pinned, freshly-typed kds_frame_t* for a new logical
 * page, or NULL if the pre-allocation ring is currently empty
 * (the background kds_proc_prealloc refill hasn't caught up).
 * Caller owns the returned pin and must kds_buf_unpin() it when done,
 * same as frames obtained via kds_buf_lookup_or_load().
 */
kds_frame_t *kds_get_reserved_kpage(kds_page_type_t type);
kds_frame_t *kds_page_alloc(kds_page_type_t type);

/*
 * Returns a previously freed page_id to the allocator's freelist for
 * future reuse. Deliberately takes a bare kds_page_id_t rather than
 * a kds_page_t*  /kds_frame_t* -- by the time a page is freed, its
 * frame may already be gone (evicted/reused), so the freelist must
 * not hold a pointer into frame/page memory whose lifetime it
 * doesn't own. Reusing a freed id is the allocator's job, not the
 * buffer pool's.
 */
void kds_add_kpage_freelist(kds_page_id_t id);

/*
 * Internal node type for the freelist above. Exposed in the header
 * only because kds_page_allocator_t embeds the list_head; callers
 * should go through kds_add_kpage_freelist(), not this struct,
 * directly.
 */
typedef struct kds_free_page_node {
    kds_page_id_t       id;
    struct list_head    node;
} kds_free_page_node_t;

typedef struct kds_page_allocator {
    u64                 reserved;
    u64                 cursor;
    kds_frame_t         *ring[PRE_ALLOC_RING_BUFFER_SIZE];

    /* Standing id range -- see the PRE_ALLOC_NUM/PRE_ALLOC_PAGE_THRES
     * comment above. Protected by range_lock, a separate lock from
     * `lock` (ring bookkeeping) and `freelist_lock` (the id-reuse
     * freelist) -- these are three independent concerns and none of
     * them should serialize on the others. */
    kds_page_id_t       alloc_point;
    u64                 remaining;
    spinlock_t          range_lock;

    struct list_head    freelist;

    spinlock_t          lock;
    spinlock_t          freelist_lock;
} kds_page_allocator_t;

#endif