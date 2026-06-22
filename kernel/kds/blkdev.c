#include <linux/kds.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/vmalloc.h>
#include <linux/bio.h>

#define DEVICE_NAME "kdsblkdev"

// #define DEVICE_SIZE (256 * 1024 * 1024)  // 256MB
#define DEVICE_SIZE (8 * 1024 * 1024)  // 8MB

#define NUM_HW_QUEUES 1
#define QUEUE_DEPTH 128

/*
 * Singleton block device handle. Owned by this file only.
 * Other .c files (page.c, page_mgr.c) reference it via the
 * `extern struct block_device *kds_bdev;` declaration in kds.h --
 * this is the single source of truth for "which device the whole
 * module talks to".
 */
struct block_device *kds_bdev = NULL;
static struct file *kds_bdev_filp;

int init_block_device(void)
{
    struct file *filp;
    struct block_device *bdev;
    dev_t devt;
    static const char dev_path[] = "/dev/vda";

    devt = MKDEV(254, 0);  // /dev/vda
    filp = bdev_file_open_by_dev(
        devt,
        BLK_OPEN_READ | BLK_OPEN_WRITE,
        NULL,   /* holder */
        NULL    /* holder_ops */
    );

    if (IS_ERR(filp)) {
        pr_err("kds: failed to open /dev/vda: %ld\n", PTR_ERR(filp));
        return PTR_ERR(filp);
    }

    bdev = file_bdev(filp);
    if (!bdev) {
        pr_err("kds: file_bdev failed\n");
        filp_close(filp, NULL);
        return -ENODEV;
    }

    /* sanity check */
    if (!bdev->bd_disk) {
        pr_err("kds: invalid block device\n");
        filp_close(filp, NULL);
        return -ENODEV;
    }

    kds_bdev_filp = filp;
    kds_bdev = bdev;

    pr_info(
        "kds: mapped /dev/vda (major=%d minor=%d) size=%llu sectors\n",
        MAJOR(bdev->bd_dev),
        MINOR(bdev->bd_dev),
        (unsigned long long)get_capacity(bdev->bd_disk)
    );

    return 0;
}

/*
 * Counterpart of init_block_device(). Must be called from the
 * module's exit path AFTER any subsystem that might still be issuing
 * I/O against kds_bdev (e.g. the page manager) has already been torn
 * down -- see kds_module_exit() ordering.
 */
void exit_block_device(void)
{
    if (kds_bdev_filp) {
        filp_close(kds_bdev_filp, NULL);
        kds_bdev_filp = NULL;
    }
    kds_bdev = NULL;
}

int request_write_sector_to_disk(
    sector_t sector,
    const char *data,
    struct bio **bio_out,
    struct page **page_out
)
{
    struct bio *bio;
    struct page *page;

    if (!kds_bdev)
        return -ENODEV;

    if (!bio_out || !page_out)
        return -EINVAL;

    page = alloc_page(GFP_KERNEL);
    if (!page)
        return -ENOMEM;

    memcpy(page_address(page), data, KDS_BLKDEV_SECTOR_SIZE);

    bio = bio_alloc(kds_bdev, 1, REQ_OP_WRITE, GFP_KERNEL);
    if (!bio) {
        __free_page(page);
        return -ENOMEM;
    }

    bio_set_dev(bio, kds_bdev);
    bio->bi_iter.bi_sector = sector;
    bio->bi_opf = REQ_OP_WRITE | REQ_FUA;

    if (bio_add_page(bio, page, KDS_BLKDEV_SECTOR_SIZE, 0)
        != KDS_BLKDEV_SECTOR_SIZE) {
        bio_put(bio);
        __free_page(page);
        return -EIO;
    }

    *bio_out = bio;
    *page_out = page;
    return 0;
}

int wait_free_bio(struct bio *bio, struct page *page)
{
    int ret;

    ret = submit_bio_wait(bio);
    if (ret)
        return ret;

    bio_put(bio);
    __free_page(page);
    return 0;
}

int write_sector_to_disk(sector_t sector, const char *data)
{
    struct bio *bio;
    struct page *page;
    int ret;

    ret = request_write_sector_to_disk(sector, data, &bio, &page);
    if (ret) {
        pr_info("request_write_sector_to_disk: setup failed, ret=%d\n", ret);
        return ret;
    }

    /* Reuse wait_free_bio() instead of repeating
     * submit_bio_wait/bio_put/__free_page here. */
    ret = wait_free_bio(bio, page);

    pr_info(
        "write_sector_to_disk: submit_bio_wait ret=%d, sector=%llu\n",
        ret,
        (unsigned long long)sector
    );

    return ret;
}

int blkdev_read_sector(sector_t sector, char *buffer)
{
    struct bio *bio;
    struct page *page;
    void *addr;
    int ret;

    if (!kds_bdev || !buffer)
        return -EINVAL;

    page = alloc_page(GFP_KERNEL);
    if (!page)
        return -ENOMEM;

    bio = bio_alloc(kds_bdev, 1, REQ_OP_READ, GFP_KERNEL);
    if (!bio) {
        __free_page(page);
        return -ENOMEM;
    }

    bio_set_dev(bio, kds_bdev);
    bio->bi_iter.bi_sector = sector;
    bio->bi_opf = REQ_OP_READ;

    if (bio_add_page(bio, page, PAGE_SIZE, 0) != PAGE_SIZE) {
        bio_put(bio);
        __free_page(page);
        return -EIO;
    }

    ret = submit_bio_wait(bio);

    if (!ret) {
        addr = kmap_local_page(page);
        memcpy(buffer, addr, PAGE_SIZE);
        kunmap_local(addr);
    }

    bio_put(bio);
    __free_page(page);
    return ret;
}

/*
 * Single base-page read/write.
 *
 * These exist as a deliberately separate, lighter-weight path from
 * kds_read_extent()/kds_write_extent() below. Callers that only ever
 * need exactly one base page (sector + struct page) should use these
 * instead of wrapping a single page into a 1-element array and going
 * through the vectored extent path -- that extra indirection buys
 * nothing for the single-page case and obscures intent at the call
 * site. Keeping this as its own implementation (rather than having
 * it call kds_read_extent(sector, &page, 1) internally) also leaves
 * room for the two paths to diverge later (e.g. different REQ_*
 * priority flags for single-page latency-sensitive reads vs
 * multi-page throughput-oriented extent I/O).
 */
int kds_read_page_sector(sector_t sector, struct page *page)
{
    struct bio *bio;
    int ret;

    if (!kds_bdev || !page)
        return -EINVAL;

    bio = bio_alloc(kds_bdev, 1, REQ_OP_READ | REQ_SYNC, GFP_KERNEL);
    if (!bio)
        return -ENOMEM;

    bio_set_dev(bio, kds_bdev);
    bio->bi_iter.bi_sector = sector;

    if (bio_add_page(bio, page, PAGE_SIZE, 0) != PAGE_SIZE) {
        bio_put(bio);
        return -EIO;
    }

    ret = submit_bio_wait(bio);
    bio_put(bio);
    return ret;
}

int kds_write_page_sector(sector_t sector, struct page *page)
{
    struct bio *bio;
    int ret;

    if (!kds_bdev || !page)
        return -EINVAL;

    bio = bio_alloc(kds_bdev, 1, REQ_OP_WRITE | REQ_SYNC, GFP_KERNEL);
    if (!bio)
        return -ENOMEM;

    bio_set_dev(bio, kds_bdev);
    bio->bi_iter.bi_sector = sector;

    if (bio_add_page(bio, page, PAGE_SIZE, 0) != PAGE_SIZE) {
        bio_put(bio);
        return -EIO;
    }

    ret = submit_bio_wait(bio);
    bio_put(bio);
    return ret;
}

/*
 * Generic multi-page extent write/read used by the page manager
 * (kds_page_mgr.c) so it does not need to duplicate bio setup for
 * its own KDS_PAGE_SIZE-sized (potentially multi-base-page) buffers.
 */
int kds_write_extent(
    sector_t start_sector,
    struct page **pages,
    unsigned int nr_pages
)
{
    struct bio *bio;
    unsigned int i;
    int ret;

    if (!kds_bdev)
        return -ENODEV;

    bio = bio_alloc(kds_bdev, nr_pages, REQ_OP_WRITE, GFP_KERNEL);
    if (!bio)
        return -ENOMEM;

    bio_set_dev(bio, kds_bdev);
    bio->bi_iter.bi_sector = start_sector;
    bio->bi_opf = REQ_OP_WRITE | REQ_SYNC;

    for (i = 0; i < nr_pages; i++) {
        if (bio_add_page(bio, pages[i], PAGE_SIZE, 0) != PAGE_SIZE) {
            bio_put(bio);
            return -EIO;
        }
    }

    ret = submit_bio_wait(bio);
    bio_put(bio);
    return ret;
}

/* ------------------------------------------------------------------
 * Logical page <-> base page mapping
 *
 * KDS_PAGE_SIZE is the engine's logical page size (e.g. 8KiB). The
 * kernel's native PAGE_SIZE is architecture/build dependent (4KiB on
 * x86_64, possibly 4/16/64KiB on arm64). These two helpers are the
 * single place that translates "one logical KDS page" into however
 * many base pages that actually requires, so callers (page_mgr.c,
 * page.c) never need to compute this themselves or build a
 * struct page* array by hand.
 *
 * Precondition, enforced at build time: KDS_PAGE_SIZE must be a
 * multiple of PAGE_SIZE. If a future build target has a native
 * PAGE_SIZE larger than KDS_PAGE_SIZE (e.g. 16KiB base pages with an
 * 8KiB logical page), this mapping breaks down and needs a different
 * strategy (sub-page logical pages), which is out of scope here.
 * ------------------------------------------------------------------ */

#if (KDS_PAGE_SIZE % PAGE_SIZE) != 0
#error "KDS_PAGE_SIZE must be a multiple of the kernel's PAGE_SIZE"
#endif

#define KDS_PAGE_NR_BASE_PAGES  (KDS_PAGE_SIZE / PAGE_SIZE)

/*
 * head_page must be the result of alloc_pages(GFP_KERNEL,
 * get_order(KDS_PAGE_SIZE)) (or an equivalent contiguous allocation
 * spanning KDS_PAGE_NR_BASE_PAGES base pages). On success, the full
 * KDS_PAGE_SIZE region starting at `sector` has been read into it.
 */
int kds_read_logical_page(sector_t sector, struct page *head_page)
{
    struct page *pages[KDS_PAGE_NR_BASE_PAGES];
    unsigned int i;

    if (!head_page)
        return -EINVAL;

    for (i = 0; i < KDS_PAGE_NR_BASE_PAGES; i++)
        pages[i] = nth_page(head_page, i);

    return kds_read_extent(sector, pages, KDS_PAGE_NR_BASE_PAGES);
}

/*
 * Counterpart of kds_read_logical_page(): writes the full
 * KDS_PAGE_SIZE region backed by head_page out to `sector`.
 */
int kds_write_logical_page(sector_t sector, struct page *head_page)
{
    struct page *pages[KDS_PAGE_NR_BASE_PAGES];
    unsigned int i;

    if (!head_page)
        return -EINVAL;

    for (i = 0; i < KDS_PAGE_NR_BASE_PAGES; i++)
        pages[i] = nth_page(head_page, i);

    return kds_write_extent(sector, pages, KDS_PAGE_NR_BASE_PAGES);
}

int kds_read_extent(
    sector_t start_sector,
    struct page **pages,
    unsigned int nr_pages
)
{
    struct bio *bio;
    unsigned int i;
    int ret;

    if (!kds_bdev)
        return -ENODEV;

    if (!pages || nr_pages == 0)
        return -EINVAL;

    bio = bio_alloc(kds_bdev, nr_pages, REQ_OP_READ, GFP_KERNEL);
    if (!bio)
        return -ENOMEM;

    bio_set_dev(bio, kds_bdev);
    bio->bi_iter.bi_sector = start_sector;
    bio->bi_opf = REQ_OP_READ;

    for (i = 0; i < nr_pages; i++) {
        if (!pages[i]) {
            bio_put(bio);
            return -EINVAL;
        }

        if (bio_add_page(bio, pages[i], PAGE_SIZE, 0) != PAGE_SIZE) {
            bio_put(bio);
            return -EIO;
        }
    }

    ret = submit_bio_wait(bio);
    bio_put(bio);
    return ret;
}
