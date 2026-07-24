#ifndef __KDS_UNDO_H
#define __KDS_UNDO_H

#include <linux/kds.h>
#include <linux/kds_page_mgr.h>
#include <linux/kds_heap.h>

/*
 * Undo pages are physically ordinary heap pages -- the only
 * difference is the common header's type (KDS_PAGE_TYPE_UNDO instead
 * of KDS_PAGE_TYPE_HEAP) and the fact that what's stored in their
 * tuples is kds_undo_entry_t rather than table row data. They are
 * created the same way as any other heap page
 * (heap_init_page_as(frame, KDS_PAGE_TYPE_UNDO)) and undo entries are
 * inserted/read with the same heap_insert_tuple_ex()/heap_read_tuple()
 * primitives used for everything else.
 *
 * Design rationale (no VACUUM needed):
 *
 * Updates never keep a second live copy of a row in the same heap
 * page the way PostgreSQL's MVCC does (old tuple physically stays in
 * place until VACUUM reclaims it). Instead:
 *   - If the new value fits in the old tuple's reserved slot space,
 *     it's overwritten in place (heap_overwrite_tuple()) -- a HOT-like
 *     update.
 *   - If it doesn't fit, the old slot is immediately retired
 *     (heap_retire_slot()) and the new value is inserted as a new
 *     tuple (heap_insert_tuple_ex()).
 * In both cases, the prior tuple version is first copied into an undo
 * entry, and the new/overwritten tuple's undo_ptr is stamped with
 * that entry's address. Any reader needing an older snapshot follows
 * the undo_ptr chain instead of finding stale bytes still sitting in
 * the heap page. There is therefore no separate "dead tuple in the
 * middle of a live page" state for VACUUM to clean up -- a retired
 * slot is simply never read again by heap scans.
 *
 * What this design does NOT solve (deliberately out of scope here):
 *   - Reclaiming the *byte space* a retired slot's tuple occupied
 *     within its page (no compaction). A page can still end up
 *     "full of garbage" over time; reclaiming that space is a
 *     separate future concern (this is close to the "Migration Pool"
 *     idea referenced in project notes), not implemented here.
 *   - Reclaiming/expiring old undo entries once no snapshot can
 *     possibly need them anymore (an undo-log retention/truncation
 *     policy). Undo pages just grow; rotation is a follow-up.
 *   - Relocating an updated tuple to a *different* page when the
 *     current page has no room for the relocated copy -- 
 *     kds_heap_update_tuple() below returns -ENOSPC in that case
 *     rather than guessing at a migration target.
 */

#define KDS_UNDO_OP_UPDATE  1
#define KDS_UNDO_OP_DELETE  2

/* Maximum size of the OLD tuple payload an undo entry can hold
 * inline. A row whose pre-update payload exceeds this cannot be
 * updated through kds_heap_update_tuple() yet -- see its doc comment
 * below. Chaining large old values across multiple undo entries (or
 * routing them through a TOAST-style page) is a follow-up, not
 * guessed at here. */
#define KDS_UNDO_MAX_OLD_DATA  256

typedef struct kds_undo_entry {
    kd_oid_t        owner_oid;       /* table oid the row belongs to */
    kds_heap_tid_t  old_tid;         /* where the prior version physically lived */
    u64             prev_undo_ptr;   /* link to the next-older undo entry, 0 = none */
    u64             xmin;            /* prior version's xmin */
    u64             xmax;            /* xid that ended the prior version's validity */
    u8              operation;       /* KDS_UNDO_OP_UPDATE / KDS_UNDO_OP_DELETE */
    u16             old_data_len;
    u8              old_data[KDS_UNDO_MAX_OLD_DATA];
} __attribute__((packed)) kds_undo_entry_t;

/*
 * Writes a new undo entry into undo_frame (an already-initialized
 * KDS_PAGE_TYPE_UNDO heap page). Returns -ENOSPC if undo_frame has no
 * room -- callers are responsible for undo page rotation (allocating
 * a fresh undo page and retrying), same as any other heap-full
 * condition in this codebase.
 */
int kds_undo_write_entry(kds_frame_t *undo_frame, kd_oid_t owner_oid,
                          kds_heap_tid_t old_tid, u64 prev_undo_ptr,
                          u64 xmin, u64 xmax, u8 operation,
                          const void *old_data, u16 old_data_len,
                          u64 write_xid, kds_heap_tid_t *out_tid);

int kds_undo_read_entry(kds_frame_t *undo_frame, u16 slot,
                         kds_undo_entry_t *out_entry);

/* ------------------------------------------------------------------
 * Undo-page manager
 *
 * The update path needs somewhere to write undo entries. Rather than
 * making every caller hand-manage a KDS_PAGE_TYPE_UNDO page and its
 * rotation, undo.c owns a single current undo "tail" page: it is
 * allocated lazily on first use and rotated (a fresh page allocated)
 * when full. kds_heap_update_tuple() drives it internally, so callers
 * only pass the row change.
 * ------------------------------------------------------------------ */

int  kds_undo_init(void);
void kds_undo_shutdown(void);

/*
 * Updates the tuple at (target_frame, slot_idx):
 *   1. Reads the current tuple version.
 *   2. Copies it into a new undo entry on the current undo page
 *      (allocated/rotated internally), chained onto whatever undo
 *      entry the current version already pointed at (prev_undo_ptr =
 *      current tuple's undo_ptr), so the full history stays reachable.
 *   3. If new_data fits within the existing slot's reserved space,
 *      overwrites in place (HOT-style). Otherwise retires the old
 *      slot and inserts the new value as a new tuple -- this may land
 *      on target_frame itself if there's room, or fail with -ENOSPC
 *      if not (see the file-level note above: no cross-page relocation
 *      here).
 *   4. Either way, the resulting live tuple's undo_ptr is stamped with
 *      the new undo entry's address, and the change is written-ahead
 *      logged and flushed synchronously (see undo.c).
 *
 * Returns -EMSGSIZE if the tuple's current payload exceeds
 * KDS_UNDO_MAX_OLD_DATA (can't be recorded in an undo entry yet).
 * On success, *out_tid is the tuple's address after the update
 * (unchanged from the input slot if overwritten in place, or the new
 * slot if relocated).
 */
int kds_heap_update_tuple(kds_frame_t *target_frame, u16 slot_idx,
                          const void *new_data, u16 new_data_len,
                          u64 xid, kd_oid_t owner_oid,
                          kds_heap_tid_t *out_tid);

#endif /* __KDS_UNDO_H */