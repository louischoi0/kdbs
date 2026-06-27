#ifndef __KDS_META_H
#define __KDS_META_H
#include <linux/kds.h>
#include <linux/atomic.h>
#include <linux/types.h>
#include <linux/build_bug.h>

#define SUPERBLOCK_MAGIC    0x4B44424FULL
#define KDS_VERSION         1

/*
 * The superblock is read/written as exactly one logical page
 * (KDS_PAGE_SIZE) via kds_read_logical_page()/kds_write_logical_page()
 * (blkdev.c) -- the same I/O path everything else in this module
 * uses. reserved2 is sized so the struct's total size is exactly
 * KDS_PAGE_SIZE; the static_assert below catches any future field
 * addition/removal that would break that invariant instead of
 * silently truncating writes or overflowing reads (which is exactly
 * the bug this sizing fixes -- the previous reserved2[3968] made the
 * struct ~4040 bytes, but the old sector-based I/O functions moved
 * exactly 512 bytes on write and PAGE_SIZE bytes on read, neither of
 * which matched the struct size).
 */
typedef struct kds_superblock {
    u64             magic;
    u32             version;
    u32             reserved1;

    atomic64_t      max_page_id;
    atomic64_t      last_commit_page_id;

    u64             create_time;
    u64             last_mount_time;
    u64             last_fsync_time;

    atomic64_t      total_pages;
    atomic64_t      free_pages;

    /*
     * Page-id pre-allocation range, persisted ONLY at clean shutdown
     * (kds_shutdown_page_alloc_system() -> kds_meta_set_alloc_range()
     * + kds_superblock_fsync()) -- see that function's doc comment
     * for why mid-range persistence would risk handing out the same
     * page_id twice after an unclean shutdown. On an unclean
     * shutdown (crash, force unload), these simply hold whatever the
     * last clean shutdown left them as (or the zeroed boot default
     * if there never was one), and the allocator mints a fresh range
     * instead of trusting a stale in-flight one -- safe, at the cost
     * of abandoning whatever range was active when it crashed.
     */
    kds_page_id_t   alloc_point;
    u64             alloc_remaining;

    u8              reserved2[8104];
} __attribute__((packed)) kds_superblock_t;

static_assert(sizeof(kds_superblock_t) == KDS_PAGE_SIZE,
              "kds_superblock_t must be exactly one logical page (KDS_PAGE_SIZE)");

/*
 * Bitmask process states. Each value must be a distinct power of two
 * so they can be OR'd/AND'd together (the previous READ=1, CHECK=1
 * duplicate made that impossible to do correctly).
 */
typedef enum {
    KDS_META_PROC_STATE_IDLE    = 0,
    KDS_META_PROC_STATE_READ    = 1,
    KDS_META_PROC_STATE_CHECK   = 2,
    KDS_META_PROC_STATE_COMMIT  = 4,
    KDS_META_PROC_STATE_IOWAIT  = 8,
} kds_proc_meta_state_t;

static inline kds_superblock_t *ref_superblock(void)
{
    extern kds_superblock_t *superblock;
    return superblock;
}

static inline kds_page_id_t assign_page_id(void)
{
    kds_superblock_t *sb = ref_superblock();
    if (unlikely(!sb))
        return 0;

    return atomic64_inc_return(&sb->max_page_id);
}

static inline kds_page_id_t alloc_page_id_batch(u64 count)
{
    kds_superblock_t *sb = ref_superblock();
    if (unlikely(!sb || count == 0))
        return 0;

    return atomic64_fetch_add(count, &sb->max_page_id);
}

static inline kds_page_id_t get_max_page_id(void)
{
    kds_superblock_t *sb = ref_superblock();
    if (unlikely(!sb))
        return 0;

    return atomic64_read(&sb->max_page_id);
}

int kds_init_meta_system(void);
void kds_shutdown_meta_system(void);
int kds_superblock_fsync(void);
void kds_init_proc_meta_ctx(void);

/*
 * Returns true if kds_init_meta_system() created a brand-new
 * superblock this boot (invalid/missing magic), false if it loaded
 * an existing one. Callers that bootstrap on-disk structures keyed
 * off fixed page_ids (e.g. kds_catalog_bootstrap()) must check this
 * before running -- the buffer pool starts empty every boot
 * regardless of what's already on disk, so without this check a
 * bootstrap routine that unconditionally (re)creates its fixed pages
 * would silently wipe out existing data on every reboot.
 */
bool kds_meta_is_fresh_init(void);

/*
 * Both take unsigned long* (not unsigned long) -- spin_lock_irqsave()
 * needs to write the saved IRQ flag back into the caller's local
 * variable, so the caller's `flags` must be passed by address:
 *
 *   unsigned long flags;
 *   lock_meta_superblock(&flags);
 *   ...
 *   unlock_meta_superblock(&flags);
 *
 * The previous by-value signature compiled inconsistently with how
 * it was actually called (call sites already passed &flags) and,
 * had it compiled, would have discarded the saved flags on unlock,
 * silently leaving IRQs in the wrong state.
 */
void lock_meta_superblock(unsigned long *flags);
void unlock_meta_superblock(unsigned long *flags);

/*
 * Used by kds_page_alloc.c to resume its id pre-allocation range
 * across a clean restart instead of always starting at (0, 0) and
 * minting a brand-new range on first use. See the alloc_point/
 * alloc_remaining field comment in kds_superblock_t above for the
 * crash-safety reasoning behind only persisting these at clean
 * shutdown.
 *
 * Neither function calls kds_superblock_fsync() itself -- the
 * caller (kds_shutdown_page_alloc_system()) is responsible for that,
 * since it knows when it's actually safe to persist (after it has
 * stopped handing out any more ids from the range).
 */
void kds_meta_set_alloc_range(kds_page_id_t alloc_point, u64 remaining);
void kds_meta_get_alloc_range(kds_page_id_t *out_alloc_point, u64 *out_remaining);

#endif /* _KDS_META_H */