#ifndef _KDS_CORE_H
#define _KDS_CORE_H
#include <linux/types.h>
#include <linux/list.h>
#include <linux/blk-mq.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>
#define KDS_BLKDEV_SECTOR_SIZE  512
#define KDS_EXT_SIZE            (512 * 1024)   /* 512 KiB */
#define KDS_PAGE_SIZE           (8 * 1024)     /* 8 KiB */
#define KDS_EXT_PAGES           (KDS_EXT_SIZE / KDS_PAGE_SIZE)
#define KDS_EXT_MAGIC           0x444424FUL
#define KDS_EXT_HDR_PAGE        0 
#define KDS_EXT_DATA_PAGE       1
#define META_SUPERBLOCK_SECTOR  2048
#define DATA_PAGE_OFFSET        32
#define KDS_SYS_RESERVED_PAGES   128

/* Sectors occupied by one logical page (KDS_PAGE_SIZE / sector size).
 * Any code converting a page_id to a disk sector MUST multiply by
 * this -- using `id` directly as a sector delta (as the old
 * kds_page_sector() in page.c/kds_page_mgr.c did) advances only 1
 * sector per page_id instead of KDS_PAGE_SECTORS sectors, which
 * makes consecutive pages overlap on disk by all but one sector. */
#define KDS_PAGE_SECTORS        (KDS_PAGE_SIZE / KDS_BLKDEV_SECTOR_SIZE)
typedef u64 kds_ext_id_t;
typedef u64 kds_page_id_t;
typedef u64 kds_ext_key_t;
typedef u64 kds_time_t;
typedef u64 kds_offset_t;
typedef u64 kds_size_t;
typedef u8  kds_page_flag_t;
typedef u64 kds_offset_t;
typedef u64 kds_size_t;
typedef u8  kds_tiny_t;
typedef u64 kds_tuple_id_t;
#define KDS_PAGE_TYPE_INVALID           0
#define KDS_PAGE_TYPE_BTREE_ROOT        1
#define KDS_PAGE_TYPE_BTREE_INTERNAL    2
#define KDS_PAGE_TYPE_BTREE_DATA        3
#define KDS_PAGE_TYPE_HEAP              4
#define KDS_PAGE_TYPE_UNDO              5
#define KDS_PAGE_TYPE_WAL               6
#define KDS_PAGE_TYPE_TOAST             7

typedef u32 kds_page_type_t;
typedef struct kds_page_hdr {
    kds_page_type_t     type;
    u32                 crc;
    kds_page_flag_t     flags;
    u8                  reserved1[32];
} __attribute__((packed)) kds_page_hdr_t;
#define KDS_PAGE_HDR_SIZE sizeof(kds_page_hdr_t)
#define KDS_PAGE_FLAG_ALLOC     1
#define KDS_PAGE_FLAG_INIT      2
#define KDS_PAGE_FLAG_FREE      4
#define KDS_PAGE_FLAG_DIRTY     8
/*
 * kds_page_t no longer owns the backing struct page* directly.
 *
 * Responsibility split:
 *   - kds_page_t  (this struct): identity (id), on-disk header
 *     metadata (hdr), the CONTENT LOCK (lock) that guards that
 *     metadata and whatever buffer currently backs it, and refcnt.
 *   - kds_frame_t (kds_page_mgr.h): owns the actual struct page*
 *     buffer. Buffer memory lifecycle (alloc/free, which frame holds
 *     it) is entirely the page manager's responsibility.
 *
 * Any code that needs to read/write the actual page contents must go
 * through the owning kds_frame_t and its kds_frame_get_write_ptr()/
 * kds_frame_get_read_ptr() (see kds_page_mgr.h) -- kds_page_t itself
 * has no field to reach the buffer directly.
 */
typedef struct kds_page {
    kds_page_id_t       id;
    kds_page_hdr_t      hdr;
    spinlock_t          lock;      /* content lock: guards hdr + the
                                     * buffer of the owning frame */
    atomic64_t          refcnt;
    struct list_head    node;
} kds_page_t;
typedef u64 kd_oid_t;

/*
 * Singleton block device handle. Defined in blkdev.c; every other
 * .c file in this module only ever references it via this extern
 * declaration. Never redefine this symbol elsewhere.
 */
extern struct block_device *kds_bdev;

int init_block_device(void);
void exit_block_device(void);
void kds_bootstrap(void);
int wait_free_bio(struct bio *bio, struct page *page);

/*
 * Single base-page read/write. Use these when the caller deals with
 * exactly one base page (one sector-aligned PAGE_SIZE buffer). This
 * is the lighter-weight counterpart of kds_read_extent()/
 * kds_write_extent() below -- it does not go through the vectored
 * bio_add_page loop for a 1-element array.
 */
int kds_read_page_sector(sector_t sector, struct page *page);
int kds_write_page_sector(sector_t sector, struct page *page);

/*
 * Multi-page extent read/write. Use these when the caller needs to
 * move more than one base page in a single bio -- e.g. the page
 * manager's KDS_PAGE_SIZE buffer when it spans multiple base pages,
 * or a checkpoint flush covering several frames at once.
 */
int kds_write_extent(sector_t start_sector, struct page **pages, unsigned int nr_pages);
int kds_read_extent(sector_t start_sector, struct page **pages, unsigned int nr_pages);

/*
 * Logical page (KDS_PAGE_SIZE) read/write. head_page must be a
 * contiguous allocation of get_order(KDS_PAGE_SIZE) (e.g. from
 * alloc_pages()). These are the preferred entry points for
 * page.c/page_mgr.c -- they internally split the logical page into
 * however many native PAGE_SIZE base pages it actually spans and
 * delegate to kds_read_extent()/kds_write_extent(), so callers never
 * need to compute that mapping themselves.
 */
int kds_read_logical_page(sector_t sector, struct page *head_page);
int kds_write_logical_page(sector_t sector, struct page *head_page);

int request_write_sector_to_disk(sector_t sector, const char *data, struct bio** bio_out, struct page** page_out);
int write_sector_to_disk(sector_t sector, const char *data);
int blkdev_read_sector(sector_t sector, char *buffer);
int write_cache_flush(void);
int __kds_write_page(kds_page_t* kd_page);

static inline sector_t
kds_page_sector(kds_page_id_t id)
{
    return (sector_t)id * (KDS_PAGE_SIZE / 512);
}

#endif