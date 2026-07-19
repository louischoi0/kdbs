#include <linux/kds.h>
#include <linux/kds_heap.h>
#include <linux/kds_page.h>
#include <linux/kds_page_mgr.h>
#include <linux/errno.h>
#include <linux/string.h>

/*
 * Every function below takes frame->kp->lock (the content lock) for
 * its entire duration and accesses frame->page directly via
 * kmap_local_page(), rather than chaining separate
 * kds_frame_get_write_ptr()/put_write_ptr() calls per field.
 *
 * This is intentional, not a layering shortcut: an insert touches
 * the page meta, a new slot entry, and the new tuple bytes all at
 * once, and those three writes must be atomic with respect to any
 * concurrent reader/writer of the same page. Acquiring and releasing
 * the content lock three times (once per field) would let another
 * thread observe a half-updated page (e.g. nr_slots bumped but the
 * tuple bytes not yet written). kds_btree.c's store_btree_node()
 * follows the same pattern for the same reason.
 */

static inline void *heap_meta_ptr(void *page_addr)
{
    return (char *)page_addr + KDS_HEAP_META_OFFSET;
}

static inline kds_heap_slot_t *heap_slot_ptr(void *page_addr, u16 idx)
{
    return (kds_heap_slot_t *)((char *)page_addr + KDS_HEAP_AREA_OFFSET
                                + (size_t)idx * sizeof(kds_heap_slot_t));
}

static inline bool is_heap_insert_possible(kds_heap_page_meta_t *meta, u16 needed) 
{
    return ((u16)(meta->upper - meta->lower)) >= needed;
}

void heap_init_page_as(kds_frame_t *frame, kds_page_type_t type)
{
    void *addr;
    kds_heap_page_meta_t meta;
    kds_page_id_t no_next = 0;

    if (!frame || !frame->kp || !frame->page)
        return;

    kds_page_lock(frame->kp);

    frame->kp->hdr.type = type;
    frame->kp->hdr.flags |= KDS_PAGE_FLAG_INIT;

    meta.nr_slots = 0;
    meta.lower = KDS_HEAP_AREA_OFFSET;
    /* Stops short of KDS_PAGE_SIZE by sizeof(kds_page_id_t) --
     * that tail is permanently reserved for the next_page_id link,
     * never available to the slot/tuple free-space calculation. */
    meta.upper = KDS_HEAP_NEXT_PAGE_OFFSET;
    meta.reserved = 0;

    addr = kmap_local_page(frame->page);
    memcpy(addr, &frame->kp->hdr, KDS_PAGE_HDR_SIZE);
    memcpy(heap_meta_ptr(addr), &meta, KDS_HEAP_META_SIZE);
    memcpy((char *)addr + KDS_HEAP_NEXT_PAGE_OFFSET, &no_next, sizeof(no_next));
    kunmap_local(addr);

    kds_page_unlock(frame->kp);
}

void heap_init_page(kds_frame_t *frame)
{
    heap_init_page_as(frame, KDS_PAGE_TYPE_HEAP);
}

u16 heap_free_space(kds_frame_t *frame)
{
    void *addr;
    kds_heap_page_meta_t meta;
    u16 free;

    if (!frame || !frame->kp || !frame->page)
        return 0;

    kds_page_lock(frame->kp);

    addr = kmap_local_page(frame->page);
    memcpy(&meta, heap_meta_ptr(addr), KDS_HEAP_META_SIZE);
    kunmap_local(addr);

    kds_page_unlock(frame->kp);

    free = (meta.upper > meta.lower) ? (meta.upper - meta.lower) : 0;
    return free;
}

u16 heap_nr_slots(kds_frame_t *frame)
{
    void *addr;
    kds_heap_page_meta_t meta;

    if (!frame || !frame->kp || !frame->page)
        return 0;

    kds_page_lock(frame->kp);

    addr = kmap_local_page(frame->page);
    memcpy(&meta, heap_meta_ptr(addr), KDS_HEAP_META_SIZE);
    kunmap_local(addr);

    kds_page_unlock(frame->kp);

    return meta.nr_slots;
}

int heap_insert_tuple_ex(kds_frame_t *frame, const void *data, u16 data_len,
                         u64 xmin, u64 xmax, u64 undo_ptr,
                         kds_heap_tid_t *out_tid)
{
    void *addr;
    kds_heap_page_meta_t meta;
    kds_heap_tuple_hdr_t tuple_hdr;
    kds_heap_slot_t slot;
    u16 needed;
    u16 new_upper;
    u16 new_slot_idx;

    if (!frame || !frame->kp || !frame->page || !out_tid)
        return -EINVAL;

    if (data_len > 0 && !data)
        return -EINVAL;

    /* Guard against u16 wraparound in the `needed` computation below
     * for pathological data_len values -- KDS_PAGE_SIZE is well
     * within u16 range, so anything larger can never fit anyway. */
    if (data_len > KDS_PAGE_SIZE)
        return -EINVAL;

    needed = sizeof(kds_heap_slot_t) + KDS_HEAP_TUPLE_HDR_SIZE + data_len;

    kds_page_lock(frame->kp);

    addr = kmap_local_page(frame->page);
    memcpy(&meta, heap_meta_ptr(addr), KDS_HEAP_META_SIZE);

    pr_info("heap_insert_tuple_ex: upper=%d, lower=%d, needed=%d\n", meta.upper, meta.lower, needed);

    if (!is_heap_insert_possible(&meta, needed)) {
        kunmap_local(addr);
        kds_page_unlock(frame->kp);
        return -ENOSPC;
    }

    new_upper = meta.upper - (KDS_HEAP_TUPLE_HDR_SIZE + data_len);
    new_slot_idx = meta.nr_slots;

    tuple_hdr.xmin = xmin;
    tuple_hdr.xmax = xmax;
    tuple_hdr.undo_ptr = undo_ptr;
    tuple_hdr.data_len = data_len;
    tuple_hdr.flags = 0;
    tuple_hdr.reserved = 0;

    memcpy((char *)addr + new_upper, &tuple_hdr, KDS_HEAP_TUPLE_HDR_SIZE);
    if (data_len > 0)
        memcpy((char *)addr + new_upper + KDS_HEAP_TUPLE_HDR_SIZE, data, data_len);

    slot.offset = new_upper;
    slot.length = KDS_HEAP_TUPLE_HDR_SIZE + data_len;
    slot.flags = 0;
    memcpy(heap_slot_ptr(addr, new_slot_idx), &slot, sizeof(slot));

    meta.nr_slots++;
    meta.lower += sizeof(kds_heap_slot_t);
    meta.upper = new_upper;
    memcpy(heap_meta_ptr(addr), &meta, KDS_HEAP_META_SIZE);

    kunmap_local(addr);

    kds_set_page_dirty(frame->kp);
    kds_page_unlock(frame->kp);

    out_tid->page_id = frame->kp->id;
    out_tid->slot = new_slot_idx;

    return 0;
}

int heap_insert_tuple(kds_frame_t *frame, const void *data, u16 data_len,
                      u64 xmin, kds_heap_tid_t *out_tid)
{
    return heap_insert_tuple_ex(frame, data, data_len, xmin, 0, 0, out_tid);
}

int heap_overwrite_tuple(kds_frame_t *frame, u16 slot_idx,
                          const void *new_data, u16 new_data_len,
                          u64 xmin, u64 xmax, u64 undo_ptr)
{
    void *addr;
    kds_heap_page_meta_t meta;
    kds_heap_slot_t slot;
    kds_heap_tuple_hdr_t tuple_hdr;

    if (!frame || !frame->kp || !frame->page)
        return -EINVAL;

    if (new_data_len > 0 && !new_data)
        return -EINVAL;

    kds_page_lock(frame->kp);

    addr = kmap_local_page(frame->page);
    memcpy(&meta, heap_meta_ptr(addr), KDS_HEAP_META_SIZE);

    if (slot_idx >= meta.nr_slots) {
        kunmap_local(addr);
        kds_page_unlock(frame->kp);
        return -ENOENT;
    }

    memcpy(&slot, heap_slot_ptr(addr, slot_idx), sizeof(slot));

    if ((slot.flags & KDS_HEAP_SLOT_DEAD) || slot.length == 0) {
        kunmap_local(addr);
        kds_page_unlock(frame->kp);
        return -ENOENT;
    }

    if ((u16)(KDS_HEAP_TUPLE_HDR_SIZE + new_data_len) > slot.length) {
        kunmap_local(addr);
        kds_page_unlock(frame->kp);
        return -ENOSPC;
    }

    tuple_hdr.xmin = xmin;
    tuple_hdr.xmax = xmax;
    tuple_hdr.undo_ptr = undo_ptr;
    tuple_hdr.data_len = new_data_len;
    tuple_hdr.flags = 0;
    tuple_hdr.reserved = 0;

    memcpy((char *)addr + slot.offset, &tuple_hdr, KDS_HEAP_TUPLE_HDR_SIZE);
    if (new_data_len > 0)
        memcpy((char *)addr + slot.offset + KDS_HEAP_TUPLE_HDR_SIZE,
               new_data, new_data_len);

    kunmap_local(addr);

    kds_set_page_dirty(frame->kp);
    kds_page_unlock(frame->kp);

    return 0;
}

int heap_retire_slot(kds_frame_t *frame, u16 slot_idx)
{
    void *addr;
    kds_heap_page_meta_t meta;
    kds_heap_slot_t slot;

    if (!frame || !frame->kp || !frame->page)
        return -EINVAL;

    kds_page_lock(frame->kp);

    addr = kmap_local_page(frame->page);
    memcpy(&meta, heap_meta_ptr(addr), KDS_HEAP_META_SIZE);

    if (slot_idx >= meta.nr_slots) {
        kunmap_local(addr);
        kds_page_unlock(frame->kp);
        return -ENOENT;
    }

    memcpy(&slot, heap_slot_ptr(addr, slot_idx), sizeof(slot));

    slot.flags |= KDS_HEAP_SLOT_DEAD;
    slot.length = 0;
    memcpy(heap_slot_ptr(addr, slot_idx), &slot, sizeof(slot));

    kunmap_local(addr);

    kds_set_page_dirty(frame->kp);
    kds_page_unlock(frame->kp);

    return 0;
}

int heap_slot_capacity(kds_frame_t *frame, u16 slot_idx, u16 *out_capacity)
{
    void *addr;
    kds_heap_page_meta_t meta;
    kds_heap_slot_t slot;

    if (!frame || !frame->kp || !frame->page || !out_capacity)
        return -EINVAL;

    kds_page_lock(frame->kp);

    addr = kmap_local_page(frame->page);
    memcpy(&meta, heap_meta_ptr(addr), KDS_HEAP_META_SIZE);

    if (slot_idx >= meta.nr_slots) {
        kunmap_local(addr);
        kds_page_unlock(frame->kp);
        return -ENOENT;
    }

    memcpy(&slot, heap_slot_ptr(addr, slot_idx), sizeof(slot));

    kunmap_local(addr);
    kds_page_unlock(frame->kp);

    if ((slot.flags & KDS_HEAP_SLOT_DEAD) || slot.length == 0)
        return -ENOENT;

    *out_capacity = slot.length - KDS_HEAP_TUPLE_HDR_SIZE;
    return 0;
}

kds_page_id_t heap_get_next_page_id(kds_frame_t *frame)
{
    void *addr;
    kds_page_id_t next_page_id;

    if (!frame || !frame->kp || !frame->page)
        return 0;

    kds_page_lock(frame->kp);

    addr = kmap_local_page(frame->page);
    memcpy(&next_page_id, (char *)addr + KDS_HEAP_NEXT_PAGE_OFFSET, sizeof(next_page_id));
    kunmap_local(addr);

    kds_page_unlock(frame->kp);

    return next_page_id;
}

int heap_set_next_page_id(kds_frame_t *frame, kds_page_id_t next_page_id)
{
    void *addr;

    if (!frame || !frame->kp || !frame->page)
        return -EINVAL;

    kds_page_lock(frame->kp);

    addr = kmap_local_page(frame->page);
    memcpy((char *)addr + KDS_HEAP_NEXT_PAGE_OFFSET, &next_page_id, sizeof(next_page_id));
    kunmap_local(addr);

    kds_set_page_dirty(frame->kp);
    kds_page_unlock(frame->kp);

    return 0;
}


static int _heap_read_tuple(kds_frame_t *frame, u16 slot_idx,
                     kds_heap_tuple_hdr_t *out_hdr,
                     void *out_buf, u16 out_buf_size, bool raise_not_size_matched)
{
    void *addr;
    kds_heap_page_meta_t meta;
    kds_heap_slot_t slot;
    kds_heap_tuple_hdr_t tuple_hdr;

    if (!frame || !frame->kp || !frame->page || !out_hdr)
        return -EINVAL;

    kds_page_lock(frame->kp);

    addr = kmap_local_page(frame->page);
    memcpy(&meta, heap_meta_ptr(addr), KDS_HEAP_META_SIZE);

    if (slot_idx >= meta.nr_slots) {
        kunmap_local(addr);
        kds_page_unlock(frame->kp);
        return -ENOENT;
    }

    memcpy(&slot, heap_slot_ptr(addr, slot_idx), sizeof(slot));

    if ((slot.flags & KDS_HEAP_SLOT_DEAD) || slot.length == 0) {
        kunmap_local(addr);
        kds_page_unlock(frame->kp);
        return -ENOENT;
    }

    memcpy(&tuple_hdr, (char *)addr + slot.offset, KDS_HEAP_TUPLE_HDR_SIZE);

    if (out_buf) {
        if (tuple_hdr.data_len > out_buf_size && raise_not_size_matched) {
            pr_info("tuple header data len=%d, out_buf_size=%d\n", tuple_hdr.data_len, out_buf_size);
            kunmap_local(addr);
            kds_page_unlock(frame->kp);
            return -ENOSPC;
        }
        memcpy(out_buf, (char *)addr + slot.offset + KDS_HEAP_TUPLE_HDR_SIZE, out_buf_size);
    }

    kunmap_local(addr);
    kds_page_unlock(frame->kp);

    *out_hdr = tuple_hdr;
    return 0;
}


int heap_read_tuple_pk(kds_frame_t *frame, u16 slot_idx,
                     kds_heap_tuple_hdr_t *out_hdr,
                     void *out_buf, u16 out_buf_size) 
{
    if (out_buf_size != 8) {
        return -ENOSPC;
    }

    return _heap_read_tuple(frame, slot_idx, out_hdr, out_buf, out_buf_size, false);
}

int heap_read_tuple(kds_frame_t *frame, u16 slot_idx,
                     kds_heap_tuple_hdr_t *out_hdr,
                     void *out_buf, u16 out_buf_size)
{
    return _heap_read_tuple(frame, slot_idx, out_hdr, out_buf, out_buf_size, true);
}

int heap_delete_tuple(kds_frame_t *frame, u16 slot_idx, u64 xmax)
{
    void *addr;
    kds_heap_page_meta_t meta;
    kds_heap_slot_t slot;
    kds_heap_tuple_hdr_t tuple_hdr;

    if (!frame || !frame->kp || !frame->page)
        return -EINVAL;

    kds_page_lock(frame->kp);

    addr = kmap_local_page(frame->page);
    memcpy(&meta, heap_meta_ptr(addr), KDS_HEAP_META_SIZE);

    if (slot_idx >= meta.nr_slots) {
        kunmap_local(addr);
        kds_page_unlock(frame->kp);
        return -ENOENT;
    }

    memcpy(&slot, heap_slot_ptr(addr, slot_idx), sizeof(slot));

    if ((slot.flags & KDS_HEAP_SLOT_DEAD) || slot.length == 0) {
        kunmap_local(addr);
        kds_page_unlock(frame->kp);
        return -ENOENT;
    }

    memcpy(&tuple_hdr, (char *)addr + slot.offset, KDS_HEAP_TUPLE_HDR_SIZE);
    tuple_hdr.xmax = xmax;
    memcpy((char *)addr + slot.offset, &tuple_hdr, KDS_HEAP_TUPLE_HDR_SIZE);

    kunmap_local(addr);

    kds_set_page_dirty(frame->kp);
    kds_page_unlock(frame->kp);

    return 0;
}

bool heap_has_space(kds_frame_t *frame, u16 data_len)
{
    u16 needed;
    u16 free;

    if (!frame || !frame->kp || !frame->page)
        return false;

    /* Guard against overflow: same check as heap_insert_tuple_ex(). */
    if (data_len > KDS_PAGE_SIZE)
        return false;

    needed = sizeof(kds_heap_slot_t) + KDS_HEAP_TUPLE_HDR_SIZE + data_len;
    free   = heap_free_space(frame);

    return free >= needed;
}