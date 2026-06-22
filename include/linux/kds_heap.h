#ifndef __KDS_HEAP_H
#define __KDS_HEAP_H

#include <linux/kds.h>
#include <linux/kds_page_mgr.h>

/*
 * Heap page layout (within one KDS_PAGE_SIZE logical page):
 *
 *   [ kds_page_hdr_t  ]  <- common header, offset 0, KDS_PAGE_HDR_SIZE
 *   [ kds_heap_page_meta_t ]  <- heap-specific page metadata
 *   [ slot directory  ]  <- grows downward (toward higher offsets) as
 *                           tuples are inserted; meta.lower tracks
 *                           the offset just past the last slot
 *   [ ... free space ... ]
 *   [ tuple data      ]  <- grows upward (toward lower offsets) from
 *                           the end of the page; meta.upper tracks
 *                           the offset of the start of the last
 *                           tuple written
 *
 * meta.lower and meta.upper are absolute byte offsets from the start
 * of the page (not relative to the heap area). The page is full when
 * meta.upper - meta.lower is smaller than the next slot + tuple needs.
 *
 * This mirrors the slotted-page design PostgreSQL uses for heap
 * pages: slots are stable references (a tuple's slot index doesn't
 * change even if compaction moves the tuple bytes around), and
 * physical compaction is a separate, not-yet-implemented operation
 * (see heap_compact_page() note below).
 */

typedef struct kds_heap_page_meta {
    u16     nr_slots;
    u16     lower;      /* offset just past the slot directory */
    u16     upper;      /* offset of the start of the last tuple */
    u16     reserved;
} __attribute__((packed)) kds_heap_page_meta_t;

#define KDS_HEAP_META_SIZE  sizeof(kds_heap_page_meta_t)
#define KDS_HEAP_META_OFFSET  KDS_PAGE_HDR_SIZE
#define KDS_HEAP_AREA_OFFSET  (KDS_HEAP_META_OFFSET + KDS_HEAP_META_SIZE)

/*
 * Slot directory entry. `length == 0` marks a dead/unused slot --
 * the slot index itself remains reserved (never reused) so that any
 * outstanding kds_heap_tid_t referencing it can detect "tuple is
 * gone" instead of silently reading an unrelated tuple that was
 * later inserted into a reused slot index.
 */
typedef struct kds_heap_slot {
    u16     offset;     /* absolute page offset of the tuple, 0 if dead */
    u16     length;      /* total tuple size (header + data), 0 if dead */
    u8      flags;
} __attribute__((packed)) kds_heap_slot_t;

#define KDS_HEAP_SLOT_DEAD   0x1

/*
 * Tuple header. xmin/xmax follow the PostgreSQL-style MVCC visibility
 * convention (creating/deleting transaction ids); undo_ptr is the
 * hybrid-MVCC addition discussed for the "Louis engine" undo design
 * (Oracle-style undo log + xmin/xmax) -- it points at the UndoEntry
 * that holds the pre-image needed to reconstruct an older version of
 * this tuple, or 0 if there is none.
 *
 * Visibility evaluation (deciding whether a given snapshot can see
 * this tuple version) is NOT implemented here -- it needs the
 * transaction manager's snapshot/CLOG machinery, which lives outside
 * this file. heap.c only stores/retrieves xmin/xmax/undo_ptr; it does
 * not interpret them.
 */
typedef struct kds_heap_tuple_hdr {
    u64     xmin;
    u64     xmax;
    u64     undo_ptr;
    u16     data_len;   /* length of the user payload following this header */
    u8      flags;
    u8      reserved;
} __attribute__((packed)) kds_heap_tuple_hdr_t;

#define KDS_HEAP_TUPLE_HDR_SIZE  sizeof(kds_heap_tuple_hdr_t)

/*
 * Addressing a heap tuple requires both the logical page it lives on
 * and its slot index within that page -- this is the heap
 * equivalent of a PostgreSQL ItemPointer/TID. Deliberately a
 * separate type from kds_tuple_id_t (which is used as a plain key
 * value in the btree code) to avoid conflating "a btree search key"
 * with "a physical heap tuple address".
 */
typedef struct kds_heap_tid {
    kds_page_id_t   page_id;
    u16             slot;
} kds_heap_tid_t;

/* Initializes an empty heap page: sets the common header's type to
 * KDS_PAGE_TYPE_HEAP and resets the heap metadata (nr_slots = 0,
 * lower/upper bracketing an empty free-space region). Does not write
 * to disk -- caller is responsible for marking dirty / flushing. */
void heap_init_page(kds_frame_t *frame);

/* Returns the number of free bytes currently available for a new
 * slot + tuple in this page (i.e. upper - lower). */
u16 heap_free_space(kds_frame_t *frame);

/* Returns the page's current slot count, so callers (e.g. a full
 * table/catalog scan) know the valid range [0, heap_nr_slots()) to
 * iterate over with heap_read_tuple(). Individual slots within that
 * range may still be dead (heap_read_tuple() returns -ENOENT for
 * those) -- callers must skip those, not treat -ENOENT as "end of
 * page". */
u16 heap_nr_slots(kds_frame_t *frame);

/*
 * Inserts a new tuple with the given payload, stamping xmin (the
 * inserting transaction's id) into the tuple header. On success,
 * *out_tid is filled with the new tuple's address and 0 is returned.
 * Returns -ENOSPC if there isn't enough free space, -EINVAL for bad
 * arguments.
 */
int heap_insert_tuple(kds_frame_t *frame, const void *data, u16 data_len,
                      u64 xmin, kds_heap_tid_t *out_tid);

/*
 * Copies a tuple's header and payload out of the page.
 * out_buf/out_buf_size describe the caller's payload buffer (the
 * tuple header itself is returned separately via out_hdr, not copied
 * into out_buf). Returns -ENOENT if the slot is dead or out of
 * range, -ENOSPC if out_buf is too small for the stored payload.
 */
int heap_read_tuple(kds_frame_t *frame, u16 slot,
                     kds_heap_tuple_hdr_t *out_hdr,
                     void *out_buf, u16 out_buf_size);

/*
 * Logical delete: stamps xmax on the tuple at `slot` without moving
 * or freeing its bytes. The tuple remains physically present (older
 * snapshots may still need to see it) -- this is a metadata update,
 * not space reclamation. Returns -ENOENT if the slot is dead or out
 * of range.
 */
int heap_delete_tuple(kds_frame_t *frame, u16 slot, u64 xmax);

/*
 * NOT IMPLEMENTED: physically compacting a page (reclaiming space
 * from dead/old tuple versions) requires knowing which versions are
 * no longer visible to any active snapshot, which in turn requires
 * the transaction manager's snapshot/horizon tracking. That isn't
 * wired up yet, so there is deliberately no heap_compact_page() or
 * VACUUM-equivalent here -- adding one without that information
 * would risk reclaiming a tuple a still-running transaction needs.
 */

#endif /* __KDS_HEAP_H */