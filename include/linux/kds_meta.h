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

    u8              reserved2[8120];
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

#endif /* _KDS_META_H */