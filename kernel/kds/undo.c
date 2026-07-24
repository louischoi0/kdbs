#include <linux/kds.h>
#include <linux/kds_undo.h>
#include <linux/kds_heap.h>
#include <linux/kds_page_mgr.h>
#include <linux/kds_page_alloc.h>
#include <linux/kds_wal.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/mutex.h>

/*
 * Write-ahead-log the redo image of an updated tuple, then flush the
 * WAL to disk synchronously so the record is durable before
 * kds_heap_update_tuple() returns. The dirty data page itself is
 * flushed later by the checkpointer (after its own WAL flush), so
 * on-disk WAL-before-data ordering is preserved.
 *
 * The record is a KDS_WAL_REC_INSERT page-mod carrying the new tuple
 * bytes (offset 0; recovery re-applies by page_id + data, the same
 * convention the insert executors use). Best-effort: WAL failures are
 * logged and swallowed rather than failing the update, matching the
 * "continue without WAL" stance elsewhere.
 */
static void update_wal_log_and_sync(kds_page_id_t page_id,
                                    const void *new_data, u16 new_data_len,
                                    u64 xid)
{
    kds_wal_rec_hdr_t        hdr  = {0};
    kds_wal_body_page_mod_t  meta = {0};
    kds_lsn_t                lsn;
    u8                      *bounce;
    int                      ret;

    meta.page_id = page_id;
    meta.offset  = 0;
    meta.length  = new_data_len;

    hdr.xid      = xid;
    hdr.type     = KDS_WAL_REC_INSERT;
    hdr.body_len = sizeof(meta) + new_data_len;

    bounce = kmalloc(hdr.body_len, GFP_KERNEL);
    if (!bounce) {
        pr_warn("kds_heap_update_tuple: WAL bounce alloc failed, "
                "continuing without WAL\n");
        return;
    }

    memcpy(bounce, &meta, sizeof(meta));
    if (new_data_len > 0)
        memcpy(bounce + sizeof(meta), new_data, new_data_len);

    ret = kds_wal_append(&hdr, bounce, &lsn);
    kfree(bounce);
    if (ret) {
        pr_warn("kds_heap_update_tuple: WAL append failed (%d), "
                "continuing without WAL\n", ret);
        return;
    }

    /* Make the update's WAL record durable on disk right now. */
    kds_wal_sync();
}

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

/* ------------------------------------------------------------------
 * Undo-page manager
 *
 * A single current undo "tail" page, allocated lazily and rotated when
 * full. Serialized by g_undo_lock so concurrent updates on different
 * CPUs don't race on the tail id or its free-space check. Undo pages
 * are never chained for reads: an undo entry is addressed directly by
 * the packed tid stored in a tuple's undo_ptr, so nothing needs to
 * walk them page-to-page.
 * ------------------------------------------------------------------ */

static kds_page_id_t g_undo_tail_page_id;
static DEFINE_MUTEX(g_undo_lock);

int kds_undo_init(void)
{
    mutex_lock(&g_undo_lock);
    g_undo_tail_page_id = 0;   /* first update allocates the first page */
    mutex_unlock(&g_undo_lock);
    return 0;
}

void kds_undo_shutdown(void)
{
    mutex_lock(&g_undo_lock);
    g_undo_tail_page_id = 0;
    mutex_unlock(&g_undo_lock);
}

/* Allocate a fresh undo page and make it the tail. Caller holds g_undo_lock. */
static int undo_alloc_tail_locked(void)
{
    kds_frame_t *f = kds_page_alloc(KDS_PAGE_TYPE_UNDO);

    if (!f)
        return -ENOSPC;

    heap_init_page_as(f, KDS_PAGE_TYPE_UNDO);
    g_undo_tail_page_id = f->kp->id;
    kds_buf_unpin(f);
    return 0;
}

/*
 * Write one undo entry to the current tail page, allocating the first
 * page and rotating on a full page. Returns 0 and *out_undo_tid on
 * success, or a negative errno.
 */
static int undo_log_entry(kd_oid_t owner_oid, kds_heap_tid_t old_tid,
                          u64 prev_undo_ptr, u64 xmin, u64 xmax, u8 operation,
                          const void *old_data, u16 old_data_len,
                          u64 write_xid, kds_heap_tid_t *out_undo_tid)
{
    int ret;
    int attempt;

    mutex_lock(&g_undo_lock);

    for (attempt = 0; attempt < 2; attempt++) {
        kds_frame_t *uf;

        if (!g_undo_tail_page_id) {
            ret = undo_alloc_tail_locked();
            if (ret)
                goto out;
        }

        uf = kds_buf_lookup_or_load(g_undo_tail_page_id);
        if (IS_ERR(uf)) {
            ret = PTR_ERR(uf);
            goto out;
        }

        ret = kds_undo_write_entry(uf, owner_oid, old_tid, prev_undo_ptr,
                                    xmin, xmax, operation,
                                    old_data, old_data_len, write_xid,
                                    out_undo_tid);
        kds_buf_unpin(uf);

        if (ret != -ENOSPC)
            goto out;   /* success, or a hard error */

        /* Tail page is full -- rotate to a fresh page and retry once. */
        ret = undo_alloc_tail_locked();
        if (ret)
            goto out;
    }

    ret = -ENOSPC;   /* a single entry didn't fit even a fresh page */
out:
    mutex_unlock(&g_undo_lock);
    return ret;
}

int kds_heap_update_tuple(kds_frame_t *target_frame, u16 slot_idx,
                          const void *new_data, u16 new_data_len,
                          u64 xid, kd_oid_t owner_oid,
                          kds_heap_tid_t *out_tid)
{
    kds_heap_tuple_hdr_t old_hdr;
    void *old_data = NULL;
    kds_heap_tid_t old_tid, undo_tid;
    u64 undo_ptr;
    u16 old_capacity;
    int ret;

    if (!target_frame || !target_frame->kp || !out_tid)
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

    ret = undo_log_entry(owner_oid, old_tid,
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
    } else {
        /* Doesn't fit: retire the old slot, insert the new value as a
         * new tuple. May land on target_frame itself if there's room;
         * otherwise -ENOSPC -- no cross-page relocation here, see
         * kds_undo.h's file-level note. */
        ret = heap_retire_slot(target_frame, slot_idx);
        if (ret)
            return ret;

        ret = heap_insert_tuple_ex(target_frame, new_data, new_data_len,
                                    xid, 0, undo_ptr, out_tid);
        if (ret)
            return ret;
    }

    /* Log this update's redo image and make it durable on disk before
     * returning (synchronous WAL for updates, mirroring inserts). */
    update_wal_log_and_sync(out_tid->page_id, new_data, new_data_len, xid);

    return 0;
}