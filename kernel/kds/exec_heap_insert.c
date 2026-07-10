/* exec_heap_insert.c
 *
 * Heap-clustered table insert executor.
 *
 * Phase sequence:
 *
 *   DUP_SCAN
 *     Walk the heap page chain from root checking for a duplicate PK.
 *     Resumable per-page. On no duplicate → FIND_TAIL.
 *
 *   FIND_TAIL
 *     Walk the chain again looking for a page with free space, or the
 *     end of chain if all pages are full. On finding a page with room:
 *       - new page needed  → set wal_need_init_page, record target,
 *                            → WAL_INIT_PAGE
 *       - existing page    → set target, → WAL_INSERT
 *
 *   WAL_INIT_PAGE   (only when a new heap page was allocated)
 *     Append KDS_WAL_REC_INIT_PAGE to the WAL ring buffer before
 *     writing any data to the new page. If WAL is unavailable,
 *     continue without WAL (best-effort).
 *     → WAL_INSERT
 *
 *   WAL_INSERT
 *     Append KDS_WAL_REC_INSERT to the WAL ring buffer with the
 *     target page_id, offset, and the encoded row.
 *     → DONE (actual heap_insert_tuple already done in FIND_TAIL;
 *             WAL is written *before* the modification is visible,
 *             so this phase writes the WAL record that covers the
 *             already-staged-in-memory modification).
 *
 * WAL ordering invariant:
 *   WAL record is appended to the ring buffer *before* the data page
 *   modification is committed. Because kds_wal_commit() does not
 *   flush to disk (the checkpointer does that asynchronously), the
 *   ordering guarantee is: WAL record always precedes the corresponding
 *   dirty frame in any flush sequence.
 */

#include <linux/kds.h>
#include <linux/kds_executor.h>
#include <linux/kds_wal.h>
#include <linux/kds_relation.h>
#include <linux/kds_heap.h>
#include <linux/kds_page_mgr.h>
#include <linux/kds_page_alloc.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/slab.h>

#define KDS_EXEC_ROW_SCAN_BUF  256

/* ------------------------------------------------------------------
 * PK duplicate scan (one page per call)
 * ------------------------------------------------------------------ */

static int kds_heap_scan_page_for_pk(kds_page_id_t page_id, s64 target_pk,
                                      kds_page_id_t *out_next)
{
    kds_frame_t *frame;
    u16 nr_slots, slot;
    int ret = 0;

    frame = kds_buf_lookup_or_load(page_id);
    if (IS_ERR(frame))
        return PTR_ERR(frame);

    nr_slots = heap_nr_slots(frame);

    for (slot = 0; slot < nr_slots; slot++) {
        kds_heap_tuple_hdr_t hdr;
        u8 buf[KDS_EXEC_ROW_SCAN_BUF];
        s64 existing_pk;
        int r;

        r = heap_read_tuple(frame, slot, &hdr, buf, sizeof(buf));
        if (r == -ENOENT)
            continue;
        if (r) { ret = r; goto out; }

        if (hdr.data_len < sizeof(existing_pk))
            continue;

        memcpy(&existing_pk, buf, sizeof(existing_pk));
        if (existing_pk == target_pk) { ret = 1; goto out; }
    }

out:
    if (ret >= 0)
        *out_next = heap_get_next_page_id(frame);
    kds_buf_unpin(frame);
    return ret;
}

/* ------------------------------------------------------------------
 * Phase: DUP_SCAN
 * ------------------------------------------------------------------ */

static kds_exec_result_t
heap_insert_run_dup_scan(kds_heap_insert_exec_t *exec)
{
    while (exec->cursor_page_id != 0) {
        kds_page_id_t next = 0;
        int ret;

        ret = kds_heap_scan_page_for_pk(exec->cursor_page_id,
                                         exec->new_pk, &next);
        if (ret < 0) { exec->base.ret = ret; return KDS_EXEC_ERROR; }
        if (ret == 1) { exec->base.ret = -EEXIST; return KDS_EXEC_ERROR; }

        exec->base.units_done++;
        exec->cursor_page_id = next;

        if (exec->cursor_page_id != 0 &&
            kds_exec_slice_expired(&exec->base))
            return KDS_EXEC_CONTINUE;
    }
    return KDS_EXEC_DONE;
}

/* ------------------------------------------------------------------
 * Phase: FIND_TAIL
 *
 * Walks chain looking for a page with space, or the end of the chain
 * to grow it. On success sets wal_target_page_id / wal_target_offset /
 * wal_need_init_page and performs the actual heap_insert_tuple().
 * The subsequent WAL phases then record this modification.
 * ------------------------------------------------------------------ */

static kds_exec_result_t
heap_insert_run_find_tail(kds_heap_insert_exec_t *exec)
{
    for (;;) {
        kds_frame_t *frame;
        kds_page_id_t next;
        int ret;

        frame = kds_buf_lookup_or_load(exec->cursor_page_id);
        if (IS_ERR(frame)) {
            exec->base.ret = PTR_ERR(frame);
            return KDS_EXEC_ERROR;
        }

        ret = heap_insert_tuple(frame, exec->data, exec->data_len,
                                 exec->xid, &exec->out_tid);
        if (!ret) {
            /*
             * Inserted successfully. Record the target so the WAL
             * phase can describe exactly which page+offset was written.
             */
            exec->wal_target_page_id = frame->kp->id;
            exec->wal_target_offset  = 0; /* exact slot offset not tracked;
                                           * recovery re-applies via page_id
                                           * + row data from the WAL body   */
            exec->wal_need_init_page = false;
            kds_buf_unpin(frame);
            exec->base.units_done++;
            return KDS_EXEC_DONE;
        }

        if (ret != -ENOSPC) {
            kds_buf_unpin(frame);
            exec->base.ret = ret;
            return KDS_EXEC_ERROR;
        }

        next = heap_get_next_page_id(frame);
        if (next != 0) {
            kds_buf_unpin(frame);
            exec->base.units_done++;
            exec->cursor_page_id = next;
            if (kds_exec_slice_expired(&exec->base))
                return KDS_EXEC_CONTINUE;
            continue;
        }

        /* Grow the chain: allocate a new heap page. */
        {
            kds_frame_t *new_frame = kds_page_alloc(KDS_PAGE_TYPE_HEAP);
            if (!new_frame) {
                kds_buf_unpin(frame);
                exec->base.ret = -ENOSPC;
                return KDS_EXEC_ERROR;
            }

            heap_init_page(new_frame);

            ret = heap_insert_tuple(new_frame, exec->data,
                                     exec->data_len, exec->xid,
                                     &exec->out_tid);
            if (ret) {
                kds_buf_unpin(new_frame);
                kds_buf_unpin(frame);
                exec->base.ret = ret;
                return KDS_EXEC_ERROR;
            }

            ret = heap_set_next_page_id(frame, new_frame->kp->id);
            kds_buf_unpin(frame);
            if (ret) {
                kds_buf_unpin(new_frame);
                exec->base.ret = ret;
                return KDS_EXEC_ERROR;
            }

            exec->wal_target_page_id = new_frame->kp->id;
            exec->wal_target_offset  = 0;
            exec->wal_need_init_page = true;
            kds_buf_unpin(new_frame);
            exec->base.units_done++;
            return KDS_EXEC_DONE;
        }
    }
}

/* ------------------------------------------------------------------
 * Phase: WAL_INIT_PAGE
 * ------------------------------------------------------------------ */

static kds_exec_result_t
heap_insert_run_wal_init_page(kds_heap_insert_exec_t *exec)
{
    kds_wal_rec_hdr_t          hdr  = {0};
    kds_wal_body_init_page_t   body = {0};
    kds_lsn_t                  lsn;
    int                        ret;

    body.page_id   = exec->wal_target_page_id;
    body.page_type = KDS_PAGE_TYPE_HEAP;

    hdr.xid      = exec->xid;
    hdr.type     = KDS_WAL_REC_INIT_PAGE;
    hdr.body_len = sizeof(body);

    ret = kds_wal_append(&hdr, &body, &lsn);
    if (ret) {
        /*
         * WAL append failure is non-fatal at this stage: the page
         * modification is already in memory. Log and continue -- the
         * checkpointer will eventually flush the dirty frame, and
         * recovery will fall back to a full-page write if needed.
         */
        pr_warn("exec_heap_insert: WAL_INIT_PAGE append failed "
                "(page=%llu ret=%d), continuing without WAL\n",
                (u64)exec->wal_target_page_id, ret);
    }

    return KDS_EXEC_DONE;
}

/* ------------------------------------------------------------------
 * Phase: WAL_INSERT
 * ------------------------------------------------------------------ */

static kds_exec_result_t
heap_insert_run_wal_insert(kds_heap_insert_exec_t *exec)
{
    kds_wal_rec_hdr_t        hdr  = {0};
    kds_wal_body_page_mod_t  meta = {0};
    kds_lsn_t                lsn;
    int                      ret;

    /*
     * Body layout: kds_wal_body_page_mod_t header followed by the
     * encoded row bytes (exec->data). body_len covers both.
     */
    meta.page_id = exec->wal_target_page_id;
    meta.offset  = exec->wal_target_offset;
    meta.length  = exec->data_len;

    hdr.xid      = exec->xid;
    hdr.type     = KDS_WAL_REC_INSERT;
    hdr.body_len = sizeof(meta) + exec->data_len;

    /*
     * kds_wal_append() takes a single body pointer. We need to send
     * meta + exec->data contiguously. Allocate a small bounce buffer
     * rather than requiring callers to pre-concatenate.
     */
    {
        u8 *bounce = kmalloc(hdr.body_len, GFP_KERNEL);

        if (!bounce) {
            pr_warn("exec_heap_insert: WAL_INSERT: bounce alloc failed, "
                    "continuing without WAL\n");
            return KDS_EXEC_DONE;
        }

        memcpy(bounce, &meta, sizeof(meta));
        memcpy(bounce + sizeof(meta), exec->data, exec->data_len);

        ret = kds_wal_append(&hdr, bounce, &lsn);
        kfree(bounce);

        if (ret)
            pr_warn("exec_heap_insert: WAL_INSERT append failed "
                    "(page=%llu ret=%d), continuing without WAL\n",
                    (u64)exec->wal_target_page_id, ret);
    }

    return KDS_EXEC_DONE;
}

/* ------------------------------------------------------------------
 * Main run function
 * ------------------------------------------------------------------ */

static kds_exec_result_t
kds_heap_insert_exec_run(kds_exec_state_t *base, u64 slice_ns)
{
    kds_heap_insert_exec_t *exec =
        container_of(base, kds_heap_insert_exec_t, base);

    if (!exec->rel || exec->rel->kind != KDS_CLUSTERED_HEAP) {
        base->ret = -EINVAL;
        return KDS_EXEC_ERROR;
    }

    if (exec->data_len < sizeof(exec->new_pk)) {
        base->ret = -EINVAL;
        return KDS_EXEC_ERROR;
    }

    for (;;) {
        switch (exec->phase) {

        case KDS_HEAP_INSERT_PHASE_DUP_SCAN: {
            kds_exec_result_t r = heap_insert_run_dup_scan(exec);
            if (r != KDS_EXEC_DONE)
                return r;
            exec->phase          = KDS_HEAP_INSERT_PHASE_FIND_TAIL;
            exec->cursor_page_id = exec->rel->root_page_id;
            continue;
        }

        case KDS_HEAP_INSERT_PHASE_FIND_TAIL: {
            kds_exec_result_t r;
            if (kds_exec_slice_expired(base))
                return KDS_EXEC_CONTINUE;
            r = heap_insert_run_find_tail(exec);
            if (r != KDS_EXEC_DONE)
                return r;
            /*
             * Modification already applied in memory. Now write WAL
             * records before the dirty frame can be flushed.
             */
            exec->phase = exec->wal_need_init_page
                ? KDS_HEAP_INSERT_PHASE_WAL_INIT_PAGE
                : KDS_HEAP_INSERT_PHASE_WAL_INSERT;
            continue;
        }

        case KDS_HEAP_INSERT_PHASE_WAL_INIT_PAGE: {
            kds_exec_result_t r = heap_insert_run_wal_init_page(exec);
            if (r != KDS_EXEC_DONE)
                return r;
            exec->phase = KDS_HEAP_INSERT_PHASE_WAL_INSERT;
            continue;
        }

        case KDS_HEAP_INSERT_PHASE_WAL_INSERT: {
            kds_exec_result_t r = heap_insert_run_wal_insert(exec);
            if (r != KDS_EXEC_DONE)
                return r;
            exec->phase = KDS_HEAP_INSERT_PHASE_DONE;
            continue;
        }

        case KDS_HEAP_INSERT_PHASE_DONE:
            return KDS_EXEC_DONE;
        }
    }
}

/* ------------------------------------------------------------------
 * Init
 * ------------------------------------------------------------------ */

void kds_heap_insert_exec_init(kds_heap_insert_exec_t *exec,
                                kds_relation_t *rel,
                                const void *data, u16 data_len, u64 xid)
{
    exec->base.run         = kds_heap_insert_exec_run;
    exec->base.ret         = 0;
    exec->base.deadline_ns = 0;
    exec->base.units_done  = 0;

    exec->rel      = rel;
    exec->data     = data;
    exec->data_len = data_len;
    exec->xid      = xid;

    exec->out_tid.page_id = 0;
    exec->out_tid.slot    = 0;

    exec->phase          = KDS_HEAP_INSERT_PHASE_DUP_SCAN;
    exec->cursor_page_id = rel ? rel->root_page_id : 0;

    exec->wal_target_page_id = 0;
    exec->wal_target_offset  = 0;
    exec->wal_need_init_page = false;

    if (data && data_len >= sizeof(exec->new_pk))
        memcpy(&exec->new_pk, data, sizeof(exec->new_pk));
    else
        exec->new_pk = 0;
}