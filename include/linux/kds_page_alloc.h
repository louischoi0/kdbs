/* kds_page_alloc.h */
#ifndef _KDS_PAGE_ALLOC_H
#define _KDS_PAGE_ALLOC_H
#include <linux/kds.h>
#include <linux/kds_meta.h>
#include <linux/kds_page_mgr.h>
#include <linux/list.h>
#include <linux/spinlock.h>

#define PRE_ALLOC_RING_BUFFER_SIZE 128

void kds_init_page_alloc_system(void);

/*
 * Returns a pinned, freshly-typed kds_frame_t* for a new logical
 * page, or NULL if the pre-allocation ring is currently empty
 * (the background kds_proc_prealloc refill hasn't caught up).
 * Caller owns the returned pin and must kds_buf_unpin() it when done,
 * same as frames obtained via kds_buf_lookup_or_load().
 */
kds_frame_t *kds_get_reserved_kpage(kds_page_type_t type);
kds_frame_t *kds_page_alloc(kds_page_type_t type);


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

    struct list_head    freelist;

    spinlock_t          lock;
    spinlock_t          freelist_lock;
} kds_page_allocator_t;

#endif