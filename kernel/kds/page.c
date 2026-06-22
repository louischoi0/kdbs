#include <linux/kds.h>
#include <linux/kds_page.h>
#include <linux/kds_page_mgr.h>
#include <linux/kds_page_alloc.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/crc32c.h>
#include <linux/compiler.h>
#include <linux/kds_meta.h>
#include <linux/errno.h>
#include <linux/vmalloc.h>
#include <linux/pagemap.h>

/*
 * kds_bdev is owned by blkdev.c. Do NOT redefine it here -- a second
 * definition in this file would cause a multiple-definition link
 * error (or silently shadow the real one, depending on toolchain).
 * Only reference it via extern.
 */
extern struct block_device *kds_bdev;

static LIST_HEAD(free_list);
static DEFINE_SPINLOCK(free_list_lock);

void kds_free_list_add(kds_page_t *kp)
{
  unsigned long flags;
  spin_lock_irqsave(&free_list_lock, flags);

  list_add_tail(&kp->node, &free_list);

  spin_unlock_irqrestore(&free_list_lock, flags);
}

static inline sector_t
kds_page_sector(kds_page_id_t id)
{
    return (sector_t)( (id + DATA_PAGE_OFFSET) << 9);
}

/*
 * CRC and zero-fill below operate on KDS_PAGE_SIZE (the logical page
 * size), not the kernel's native PAGE_SIZE. The buffer itself is no
 * longer reachable from kds_page_t -- it is owned by the kds_frame_t
 * that holds this kp (see kds_core.h / kds_page_mgr.h for the
 * ownership split), so every function below now takes a kds_frame_t*
 * and operates on frame->page, locking via frame->kp->lock.
 */
static uint32_t kds_calc_data_crc(uint8_t *page_data)
{
    uint8_t *data_start;
    size_t data_size;

    data_start = page_data + sizeof(kds_page_hdr_t);
    data_size = KDS_PAGE_SIZE - sizeof(kds_page_hdr_t);

    return crc32(0, data_start, data_size);
}

bool check_valid_page(kds_frame_t *frame)
{
    uint32_t crc;
    uint8_t* addr;
    bool res;

    if (!frame || !frame->kp || !frame->page)
        return false;

    kds_page_lock(frame->kp);

    addr = kmap_local_page(frame->page);
    crc = kds_calc_data_crc(addr);
    res = crc == frame->kp->hdr.crc;

    kunmap_local(addr);
    kds_page_unlock(frame->kp);

    return res;
}

void kds_update_page_crc(kds_frame_t *frame)
{
    uint8_t* addr;

    if (!frame || !frame->kp || !frame->page)
        return;

    kds_page_lock(frame->kp);

    addr = kmap_local_page(frame->page);
    frame->kp->hdr.crc = kds_calc_data_crc(addr);

    kunmap_local(addr);
    kds_page_unlock(frame->kp);
}

/*
 * kds_read_page() delegates entirely to the page manager's cache.
 * NOTE: returning kds_page_t* here would no longer expose a usable
 * buffer to the caller (kp has no `page` field anymore), so this
 * compatibility shim from the earlier migration step now returns the
 * kds_frame_t* directly. Callers must use the frame-based accessors
 * (kds_frame_get_write_ptr/kds_set_page_buffer(frame, ...), etc.) and
 * eventually kds_buf_unpin() it.
 */
kds_frame_t *kds_read_page(kds_page_id_t page_id)
{
    return kds_buf_lookup_or_load(page_id);
}

/*
 * Writes the full KDS_PAGE_SIZE logical page backing frame out to
 * disk via kds_write_logical_page() (blkdev.c), which owns the
 * logical-page -> base-page split.
 */
int kds_write_page(kds_frame_t *frame)
{
    sector_t sector;

    if (!frame || !frame->kp || !frame->page)
        return -EINVAL;

    if (!kds_bdev)
        return -ENODEV;

    sector = kds_page_sector(frame->kp->id);
    pr_info("kds_write_page: sector=%llu\n", (unsigned long long)sector);

    return kds_write_logical_page(sector, frame->page);
}

/*
 * Writes nr_frames logical pages out, one at a time via
 * kds_write_logical_page(). Intentionally not a single combined bio
 * across multiple logical pages -- frame_array entries are not
 * guaranteed to be sector-contiguous with each other.
 */
int kds_write_pages(kds_frame_t **frame_array, unsigned int nr_frames)
{
    unsigned int i;
    int ret;

    if (!frame_array || nr_frames == 0)
        return -EINVAL;

    if (!kds_bdev)
        return -ENODEV;

    for (i = 0; i < nr_frames; i++) {
        if (!frame_array[i] || !frame_array[i]->kp || !frame_array[i]->page) {
            pr_err("kds_write_pages: invalid frame at index %u\n", i);
            return -EINVAL;
        }
    }

    for (i = 0; i < nr_frames; i++) {
        sector_t sector = kds_page_sector(frame_array[i]->kp->id);

        ret = kds_write_logical_page(sector, frame_array[i]->page);
        if (ret) {
            pr_err("kds_write_pages: failed at index %u, ret=%d\n", i, ret);
            return ret;
        }
    }

    pr_info("kds_write_pages: successfully wrote %u logical pages\n", nr_frames);
    return 0;
}

int kds_set_page_buffer(kds_frame_t *frame, const char* data, kds_offset_t offset, kds_size_t size)
{
    char *p;

    if (!frame || !frame->kp || !frame->page)
        return -EINVAL;

    if (offset + size > KDS_PAGE_SIZE)
        return -EINVAL;

    kds_page_lock(frame->kp);

    p = (char*) kmap_local_page(frame->page);
    memcpy(p + offset, data, size);

    kunmap_local(p);

    kds_set_page_dirty(frame->kp);
    kds_page_unlock(frame->kp);

    return 0;
}

int kds_commit_page_hdr(kds_frame_t *frame)
{
    // no set dirty
    void *p;

    if (!frame || !frame->kp || !frame->page)
        return -EINVAL;

    kds_page_lock(frame->kp);

    p = kmap_local_page(frame->page);
    memcpy(p, &frame->kp->hdr, KDS_PAGE_HDR_SIZE);
    kunmap_local(p);

    kds_page_unlock(frame->kp);

    return 0;
}

int kds_update_page_hdr(kds_frame_t *frame, kds_page_hdr_t* hdr)
{
    void *p;

    if (!frame || !frame->kp || !frame->page)
        return -EINVAL;

    kds_page_lock(frame->kp);

    p = kmap_local_page(frame->page);
    memcpy(p, hdr, KDS_PAGE_HDR_SIZE);
    kunmap_local(p);

    kds_set_page_dirty(frame->kp);
    frame->kp->hdr = *hdr;

    kds_page_unlock(frame->kp);

    return 0;
}

int kds_free_page(kds_frame_t *frame)
{
    void* addr;

    if (!frame || !frame->kp || !frame->page)
        return -EINVAL;

    kds_page_lock(frame->kp);
    addr = kmap_local_page(frame->page);

    /* KDS_PAGE_SIZE, not PAGE_SIZE -- see kds_calc_data_crc() comment
     * above. */
    memset(addr, 0, KDS_PAGE_SIZE);
    kunmap_local(addr);

    kds_set_page_dirty(frame->kp);
    frame->kp->hdr.flags |= KDS_PAGE_FLAG_FREE;
    kds_free_list_add(frame->kp);

    kds_page_unlock(frame->kp);

    return 0;
}