/* meta.c */
#include <linux/kds.h>
#include <linux/kds_meta.h>
#include <linux/kds_proc.h>
#include <linux/slab.h>
#include <linux/bio.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/compiler.h>
#include <linux/printk.h>
#include <linux/crc32c.h>
#include <linux/ktime.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>

kds_superblock_t *superblock = NULL;
static DEFINE_SPINLOCK(superblock_lock);

/*
 * Dedicated I/O buffer for the superblock. kds_read_logical_page()/
 * kds_write_logical_page() (blkdev.c) need a struct page* spanning
 * exactly KDS_PAGE_SIZE -- they do not know how to read/write the
 * vmalloc'd `superblock` object directly. `superblock` stays the
 * long-lived in-memory copy that assign_page_id()/etc. operate on;
 * sb_io_page is only ever used as a scratch buffer for the disk
 * round-trip, with data copied in/out via kmap.
 */
#define KDS_PAGE_ORDER  get_order(KDS_PAGE_SIZE)
static struct page *sb_io_page;

static int init_superblock_and_fsync(kds_superblock_t *block)
{
    void *addr;
    int ret;

    if (unlikely(!block || !sb_io_page))
        return -EINVAL;

    block->magic = SUPERBLOCK_MAGIC;
    block->version = KDS_VERSION;
    atomic64_set(&block->max_page_id, 0);
    atomic64_set(&block->total_pages, 0);
    atomic64_set(&block->free_pages, 0);
    atomic64_set(&block->last_commit_page_id, 0);

    block->create_time = ktime_get_real_seconds();
    block->last_mount_time = block->create_time;
    block->last_fsync_time = 0;

    pr_info("KDS: Initializing superblock for first boot\n");

    addr = kmap_local_page(sb_io_page);
    memcpy(addr, block, sizeof(*block));
    kunmap_local(addr);

    ret = kds_write_logical_page(META_SUPERBLOCK_SECTOR, sb_io_page);
    if (ret)
        pr_err("KDS: Failed to write initial superblock: %d\n", ret);

    return ret;
}

void lock_meta_superblock(unsigned long *flags)
{
    spin_lock_irqsave(&superblock_lock, *flags);
}

void unlock_meta_superblock(unsigned long *flags)
{
    spin_unlock_irqrestore(&superblock_lock, *flags);
}

int kds_superblock_fsync(void)
{
    unsigned long flags;
    void *addr;
    int ret;

    if (unlikely(!superblock || !sb_io_page))
        return -EINVAL;

    lock_meta_superblock(&flags);

    addr = kmap_local_page(sb_io_page);
    memcpy(addr, superblock, sizeof(*superblock));
    kunmap_local(addr);

    ret = kds_write_logical_page(META_SUPERBLOCK_SECTOR, sb_io_page);
    if (!ret) {
        superblock->last_fsync_time = ktime_get_real_seconds();
        atomic64_set(&superblock->last_commit_page_id,
                     atomic64_read(&superblock->max_page_id));
    }

    unlock_meta_superblock(&flags);

    if (ret)
        pr_err("KDS: Failed to fsync superblock: %d\n", ret);

    return ret;
}

/*
 * TODO -- BLOCKED: there is currently no async, multi-base-page
 * "submit without waiting" entry point in blkdev.c --
 * kds_write_extent()/kds_write_logical_page() are both synchronous
 * (submit_bio_wait()). request_write_sector_to_disk() (the old async
 * primitive this function used to wrap) only ever moves
 * KDS_BLKDEV_SECTOR_SIZE (512) bytes, which is far smaller than the
 * now-KDS_PAGE_SIZE superblock -- reusing it here would reintroduce
 * the exact truncation bug this file just fixed for the synchronous
 * path. Disabled until an async logical-page write exists; use
 * kds_superblock_fsync() (synchronous) instead.
 */
static int kds_request_superblock_fsync(struct bio **bio_out, struct page **page_out)
{
    pr_warn("kds_request_superblock_fsync: disabled, no async logical-page write API yet; use kds_superblock_fsync()\n");
    return -ENOSYS;
}

static int kds_load_or_init_superblock(void)
{
    void *addr;
    int ret;

    sb_io_page = alloc_pages(GFP_KERNEL, KDS_PAGE_ORDER);
    if (!sb_io_page) {
        pr_err("KDS: Failed to allocate superblock I/O page\n");
        return -ENOMEM;
    }

    superblock = vmalloc(sizeof(kds_superblock_t));
    if (!superblock) {
        pr_err("KDS: Failed to allocate superblock\n");
        __free_pages(sb_io_page, KDS_PAGE_ORDER);
        sb_io_page = NULL;
        return -ENOMEM;
    }

    ret = kds_read_logical_page(META_SUPERBLOCK_SECTOR, sb_io_page);
    if (ret) {
        pr_err("KDS: Failed to read superblock: %d\n", ret);
        vfree(superblock);
        superblock = NULL;
        __free_pages(sb_io_page, KDS_PAGE_ORDER);
        sb_io_page = NULL;
        return ret;
    }

    addr = kmap_local_page(sb_io_page);
    memcpy(superblock, addr, sizeof(*superblock));
    kunmap_local(addr);

    if (superblock->magic == SUPERBLOCK_MAGIC) {
        pr_info("KDS: Superblock loaded successfully\n");
        pr_info("  Magic: 0x%llx\n", superblock->magic);
        pr_info("  Version: %u\n", superblock->version);
        pr_info("  Max Page ID: %lld\n", atomic64_read(&superblock->max_page_id));

        superblock->last_mount_time = ktime_get_real_seconds();
        return 0;
    }

    pr_warn("KDS: Invalid superblock magic (0x%llx), initializing new one\n",
            superblock->magic);

    ret = init_superblock_and_fsync(superblock);
    if (ret) {
        pr_err("KDS: Failed to initialize superblock: %d\n", ret);
        vfree(superblock);
        superblock = NULL;
        __free_pages(sb_io_page, KDS_PAGE_ORDER);
        sb_io_page = NULL;
        return ret;
    }

    return 0;
}

int kds_init_meta_system(void)
{
    int ret;

    pr_info("KDS: Initializing meta system\n");

    ret = kds_load_or_init_superblock();
    if (ret) {
        pr_err("KDS: Failed to load superblock: %d\n", ret);
        return ret;
    }

    pr_info("KDS: Meta system initialized successfully\n");
    return 0;
}

void kds_shutdown_meta_system(void)
{
    if (superblock) {
        /* Best-effort final flush; ignore the return value here --
         * if this fails there is nothing else shutdown can do, and
         * the caller (module exit) is not in a position to retry. */
        kds_superblock_fsync();

        vfree(superblock);
        superblock = NULL;
    }

    if (sb_io_page) {
        __free_pages(sb_io_page, KDS_PAGE_ORDER);
        sb_io_page = NULL;
    }

    pr_info("KDS: Meta system shut down\n");
}