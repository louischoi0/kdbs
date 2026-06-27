#include <linux/kds.h>
#include <linux/kds_undo.h>
#include <linux/kds_heap.h>
#include <linux/kds_page_mgr.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/string.h>

int kds_undo_write_entry(kds_frame_t *undo_frame, kd_oid_t owner_oid,
                          kds_heap_tid_t old_tid, u64 prev_undo_ptr,
                          u64 xmin, u64 xmax, u8 operation,
                          const void *old_data, u16 old_data_len,
                          u64 write_xid, kds_heap_tid_t *out_tid)
{
    kds_undo_entry_t entry;

    if (!undo_frame || !out_tid)
        return -EINVAL;

    if (old_data_len > KDS_UNDO_MAX_OLD_DATA)
        return -EMSGSIZE;

    if (old_data_len > 0 && !old_data)
        return -EINVAL;

    memset(&entry, 0, sizeof(entry));
    entry.owner_oid = owner_oid;
    entry.old_tid = old_tid;
    entry.prev_undo_ptr = prev_undo_ptr;
    entry.xmin = xmin;
    entry.xmax = xmax;
    entry.operation = operation;
    entry.old_data_len = old_data_len;
    if (old_data_len > 0)
        memcpy(entry.old_data, old_data, old_data_len);

    /* Inserted as a fixed-size tuple (sizeof(entry)) rather than just
     * old_data_len worth of payload -- simpler than a variable-length
     * undo tuple format, at the cost of always reserving
     * KDS_UNDO_MAX_OLD_DATA bytes even for small rows. Acceptable
     * for a first version; revisit if undo page churn becomes a
     * concern. */
    return heap_insert_tuple_ex(undo_frame, &entry, sizeof(entry),
                                 write_xid, 0, 0, out_tid);
}

int kds_undo_read_entry(kds_frame_t *undo_frame, u16 slot,
                         kds_undo_entry_t *out_entry)
{
    kds_heap_tuple_hdr_t hdr;

    if (!undo_frame || !out_entry)
        return -EINVAL;

    return heap_read_tuple(undo_frame, slot, &hdr, out_entry, sizeof(*out_entry));
}

int kds_heap_update_tuple(kds_frame_t *target_frame, u16 slot_idx,
                          const void *new_data, u16 new_data_len,
                          u64 xid, kd_oid_t owner_oid,
                          kds_frame_t *undo_frame,
                          kds_heap_tid_t *out_tid)
{
    kds_heap_tuple_hdr_t old_hdr;
    void *old_data = NULL;
    kds_heap_tid_t old_tid, undo_tid;
    u64 undo_ptr;
    u16 old_capacity;
    int ret;

    if (!target_frame || !target_frame->kp || !undo_frame || !out_tid)
        return -EINVAL;

    if (new_data_len > 0 && !new_data)
        return -EINVAL;

    /* First call with out_buf = NULL: just fetch the header so we
     * know how big the old payload is before allocating a buffer for
     * it. */
    ret = heap_read_tuple(target_frame, slot_idx, &old_hdr, NULL, 0);
    if (ret)
        return ret;

    if (old_hdr.data_len > KDS_UNDO_MAX_OLD_DATA)
        return -EMSGSIZE;

    if (old_hdr.data_len > 0) {
        old_data = kmalloc(old_hdr.data_len, GFP_KERNEL);
        if (!old_data)
            return -ENOMEM;

        ret = heap_read_tuple(target_frame, slot_idx, &old_hdr,
                               old_data, old_hdr.data_len);
        if (ret) {
            kfree(old_data);
            return ret;
        }
    }

    old_tid.page_id = target_frame->kp->id;
    old_tid.slot = slot_idx;

    ret = kds_undo_write_entry(undo_frame, owner_oid, old_tid,
                                old_hdr.undo_ptr, old_hdr.xmin, xid,
                                KDS_UNDO_OP_UPDATE, old_data, old_hdr.data_len,
                                xid, &undo_tid);
    kfree(old_data);
    if (ret)
        return ret;

    undo_ptr = kds_heap_tid_pack(undo_tid);

    ret = heap_slot_capacity(target_frame, slot_idx, &old_capacity);
    if (ret)
        return ret;

    if (new_data_len <= old_capacity) {
        /* HOT-style: overwrite in place, same slot, same tid. */
        ret = heap_overwrite_tuple(target_frame, slot_idx, new_data,
                                    new_data_len, xid, 0, undo_ptr);
        if (ret)
            return ret;

        out_tid->page_id = target_frame->kp->id;
        out_tid->slot = slot_idx;
        return 0;
    }

    /* Doesn't fit: retire the old slot, insert the new value as a
     * new tuple. May land on target_frame itself if there's room;
     * otherwise -ENOSPC -- no cross-page relocation here, see
     * kds_undo.h's file-level note. */
    ret = heap_retire_slot(target_frame, slot_idx);
    if (ret)
        return ret;

    return heap_insert_tuple_ex(target_frame, new_data, new_data_len,
                                 xid, 0, undo_ptr, out_tid);
}