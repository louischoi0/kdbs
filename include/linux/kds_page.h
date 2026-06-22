#ifndef __KDS_PAGE_H
#define __KDS_PAGE_H
#include <linux/kds.h>
#include <linux/kds_page_mgr.h>
#include <linux/vmalloc.h>

/*
 * kds_page_t no longer owns a struct page* (see kds_core.h). All
 * buffer-touching operations below now take a kds_frame_t* (the
 * page manager's frame, which owns the actual buffer) instead of a
 * bare kds_page_t*. kds_page_t itself is only used for its content
 * lock (`lock`), identity (`id`), header (`hdr`) and refcnt.
 */

int kds_init_ext_system(void);

kds_frame_t *kds_read_page(kds_page_id_t id);
int kds_write_page(kds_frame_t *frame);
int kds_write_pages(kds_frame_t **frame_array, unsigned int nr_frames);

int kds_set_page_buffer(kds_frame_t *frame, const char* data, kds_offset_t offset, kds_size_t size);
int kds_update_page_hdr(kds_frame_t *frame, kds_page_hdr_t* hdr);
int kds_commit_page_hdr(kds_frame_t *frame);

bool check_valid_page(kds_frame_t *frame);
void kds_update_page_crc(kds_frame_t *frame);
int kds_free_page(kds_frame_t *frame);

#define PAGE_USABLE(page) \
    (((page)->hdr.flags & (KDS_PAGE_FLAG_ALLOC | KDS_PAGE_FLAG_INIT | KDS_PAGE_FLAG_FREE)) != 0)

static inline void kds_set_page_dirty(kds_page_t *kp)
{
    kp->hdr.flags |= KDS_PAGE_FLAG_DIRTY;
    // kds_dirty_list_add(kp);
}

static inline void kds_set_page_init(kds_page_t *kp)
{
    kp->hdr.flags |= KDS_PAGE_FLAG_INIT;
}

/*
 * pin/unpin and lock/unlock still operate on kds_page_t (the content
 * lock + refcnt object); these are unchanged in meaning, only their
 * relationship to the buffer is now indirect (via the owning frame).
 */
static inline void pin_kpage(kds_page_t *kp)
{
    atomic64_inc(&kp->refcnt);
}

static inline void unpin_kpage(kds_page_t *kp)
{
    atomic64_dec(&kp->refcnt);
}

static inline void kds_page_lock(kds_page_t *page)
{
    spin_lock(&page->lock);
}

static inline void kds_page_unlock(kds_page_t *page)
{
    spin_unlock(&page->lock);
}

/*
 * kds_alloc_kpage()/kds_free_kpage()/kds_free_kpages() and
 * kds_get_page_cache()/kds_put_page_cache() are intentionally
 * REMOVED from this header.
 *
 * They used to allocate/free `kp->page` directly and manage their
 * own ad-hoc page cache -- both responsibilities now belong solely
 * to kds_page_mgr.c's buffer pool:
 *   - allocation/freeing of the backing struct page*  -> kds_frame_t
 *     lifecycle inside page_mgr.c (kds_frame_read_from_disk(),
 *     kds_buf_pool_destroy(), etc.)
 *   - page_id -> in-memory lookup                      -> kds_buf_lookup()
 *     / kds_buf_lookup_or_load() (kds_page_mgr.h)
 *
 * If any caller still references kds_alloc_kpage()/
 * kds_get_page_cache() etc., that call site needs to be migrated to
 * the kds_page_mgr.h equivalents (kds_buf_lookup_or_load(),
 * kds_buf_pin()/kds_buf_unpin()) as part of this same change.
 */

#define kds_page_lock_irqsave(page, flags) \
    spin_lock_irqsave(&(page)->lock, flags)
#define kds_page_lock_irqstore(page, flags) \
    spin_lock_irqstore(&(page)->lock, flags)
#endif