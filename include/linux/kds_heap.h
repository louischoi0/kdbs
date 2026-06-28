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
 *                           meta.upper; meta.upper tracks the offset
 *                           of the start of the last tuple written
 *   [ next_page_id    ]  <- last sizeof(kds_page_id_t) bytes of the
 *                           page, see KDS_HEAP_NEXT_PAGE_OFFSET below
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
 *
 * next_page_id chaining: the last sizeof(kds_page_id_t) bytes of
 * every heap page are reserved (meta.upper never grows past
 * KDS_HEAP_NEXT_PAGE_OFFSET, see heap_init_page_as()) for a single
 * forward link to the next heap page belonging to the same table, or
 * 0 if this is the last page in the chain. This makes a table's
 * heap storage a singly-linked list of pages, so a table can grow
 * past one page and still be scanned start-to-finish (root page,
 * follow next_page_id, repeat until 0) with no index of any kind.
 * The obvious limitation: only a full scan is possible this way --
 * there's no way to jump to "the page containing key K" without
 * walking the whole chain, which is exactly the gap a real index
 * (btree) exists to close. See heap_get_next_page_id()/
 * heap_set_next_page_id() below.
 */

#define KDS_HEAP_NEXT_PAGE_OFFSET  (KDS_PAGE_SIZE - sizeof(kds_page_id_t))

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

/*
 * A tuple header's undo_ptr field is a plain u64 -- these pack/unpack
 * a kds_heap_tid_t into/from that single field so an undo entry's
 * location can be stamped directly into the tuple header it
 * describes. page_id is assumed to fit in 48 bits (281 trillion
 * pages -- not a real constraint in practice) leaving the top 16
 * bits for the slot index. 0 is reserved to mean "no undo entry",
 * matching page_id 0 never being a valid assigned page_id (see
 * kds_meta.h's assign_page_id(), which starts counting from 1).
 */
static inline u64 kds_heap_tid_pack(kds_heap_tid_t tid)
{
    return (tid.page_id & 0xFFFFFFFFFFFFULL) | ((u64)tid.slot << 48);
}

static inline kds_heap_tid_t kds_heap_tid_unpack(u64 packed)
{
    kds_heap_tid_t tid;

    tid.page_id = packed & 0xFFFFFFFFFFFFULL;
    tid.slot = (u16)(packed >> 48);
    return tid;
}

/* Initializes an empty page with the given page type (HEAP, or e.g.
 * UNDO -- an undo page is physically just a heap page, see
 * kds_undo.h). heap_init_page() below is a thin wrapper for the
 * common KDS_PAGE_TYPE_HEAP case. */
void heap_init_page_as(kds_frame_t *frame, kds_page_type_t type);

/* Initializes an empty heap page: sets the common header's type to
 * KDS_PAGE_TYPE_HEAP and resets the heap metadata (nr_slots = 0,
 * lower/upper bracketing an empty free-space region, with upper
 * already excluding the tail next_page_id reservation). Also writes
 * 0 (no next page) into the tail. Does not write to disk -- caller
 * is responsible for marking dirty / flushing. */
void heap_init_page(kds_frame_t *frame);

/*
 * Reads/writes the tail next_page_id link. 0 means "no next page"
 * (end of chain) -- consistent with page_id 0 never being a valid
 * assigned page_id elsewhere in this codebase (kds_meta.h's
 * assign_page_id() starts counting from 1).
 */
kds_page_id_t heap_get_next_page_id(kds_frame_t *frame);
int heap_set_next_page_id(kds_frame_t *frame, kds_page_id_t next_page_id);

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
 * arguments. Equivalent to heap_insert_tuple_ex() with xmax=0,
 * undo_ptr=0 (a brand-new, live tuple with no prior version).
 */
int heap_insert_tuple(kds_frame_t *frame, const void *data, u16 data_len,
                      u64 xmin, kds_heap_tid_t *out_tid);

/*
 * Full-control insert: same as heap_insert_tuple() but lets the
 * caller stamp xmax and undo_ptr directly at creation time. This is
 * what the update path (kds_undo.h's kds_heap_update_tuple()) uses
 * when it has to relocate a tuple to a new slot -- the new slot's
 * undo_ptr needs to point at the just-written undo entry from the
 * moment it's created, not patched in afterward.
 */
int heap_insert_tuple_ex(kds_frame_t *frame, const void *data, u16 data_len,
                         u64 xmin, u64 xmax, u64 undo_ptr,
                         kds_heap_tid_t *out_tid);

/*
 * Overwrites the tuple at `slot_idx` in place (same physical offset)
 * with new header fields and new payload bytes. Used for the
 * HOT-style update path: when new_data_len fits within the slot's
 * existing reserved span (slot.length - KDS_HEAP_TUPLE_HDR_SIZE),
 * the tuple can be updated without touching the slot directory or
 * moving anything. Returns -ENOSPC if new_data_len doesn't fit in
 * the existing reservation -- callers should fall back to
 * heap_retire_slot() + heap_insert_tuple_ex() in that case.
 */
int heap_overwrite_tuple(kds_frame_t *frame, u16 slot_idx,
                          const void *new_data, u16 new_data_len,
                          u64 xmin, u64 xmax, u64 undo_ptr);

/*
 * Physically retires a slot: marks it dead and zeroes its length,
 * unconditionally (no xmax bookkeeping). This is NOT the same
 * operation as heap_delete_tuple() (a user-visible MVCC delete that
 * keeps the tuple readable to older snapshots via xmax) -- this is
 * used internally by the update-relocate path once a tuple's prior
 * content has already been safely copied into an undo entry, so
 * nothing needs to read this slot directly anymore (older snapshots
 * reach the prior version through the new tuple's undo_ptr chain
 * instead). The dead slot's bytes are not reclaimed/compacted; see
 * the file-level note in kds_undo.h about why that's fine here.
 */
int heap_retire_slot(kds_frame_t *frame, u16 slot_idx);

/*
 * Returns the data capacity (slot.length - KDS_HEAP_TUPLE_HDR_SIZE)
 * reserved for the tuple at `slot_idx`, i.e. the largest new payload
 * heap_overwrite_tuple() could write there without relocating.
 * Returns -ENOENT if the slot is dead or out of range.
 */
int heap_slot_capacity(kds_frame_t *frame, u16 slot_idx, u16 *out_capacity);

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