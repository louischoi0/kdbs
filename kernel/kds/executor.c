#include <linux/kds.h>
#include <linux/kds_executor.h>
#include <linux/kds_relation.h>
#include <linux/kds_heap.h>
#include <linux/kds_page_mgr.h>
#include <linux/kds_page_alloc.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/string.h>

/*
 * Every table's first column is always its int64 primary key (a
 * fixed positional convention enforced at CREATE TABLE time, see
 * dshell.c's kds_cmd_create_table()) -- so the PK is always exactly
 * the first sizeof(s64) bytes of any encoded row, regardless of
 * which table this is. That means this check can stay completely
 * decoupled from kds_schema_t/kds_sys_column_t; it doesn't need to
 * know anything about the table's columns beyond that fixed
 * assumption.
 *
 * KDS_EXEC_ROW_SCAN_BUF must be large enough to hold any row this
 * exec will ever see in full (heap_read_tuple() doesn't support
 * partial reads -- it's all-or--ENOSPC). This mirrors dshell.c's
 * KDS_DSHELL_ROW_MAX; the two aren't formally tied together, so if
 * one grows to support larger rows, check whether the other needs
 * to as well.
 */
#define KDS_EXEC_ROW_SCAN_BUF  256

/*
 * Returns 1 if a live tuple with this PK already exists anywhere in
 * the chain starting at root_page_id, 0 if not, or a negative errno
 * if the chain couldn't be walked (a lookup failure partway through
 * -- treated as "couldn't verify", not as "no duplicate", so callers
 * don't silently let a duplicate through just because one page in
 * the chain happened to be unreadable).
 */
static int kds_heap_check_pk_duplicate(kds_page_id_t root_page_id, s64 new_pk)
{
    kds_page_id_t current_id = root_page_id;

    while (current_id != 0) {
        kds_frame_t *frame;
        u16 nr_slots, slot;
        kds_page_id_t next;

        frame = kds_buf_lookup_or_load(current_id);
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
                continue; /* dead slot */
            if (r) {
                kds_buf_unpin(frame);
                return r;
            }

            if (hdr.data_len < sizeof(existing_pk))
                continue; /* malformed/too-short row -- not a PK match either way */

            memcpy(&existing_pk, buf, sizeof(existing_pk));
            if (existing_pk == new_pk) {
                kds_buf_unpin(frame);
                return 1;
            }
        }

        next = heap_get_next_page_id(frame);
        kds_buf_unpin(frame);
        current_id = next;
    }

    return 0;
}

static kds_exec_result_t kds_heap_insert_exec_run(kds_exec_state_t *base)
{
    kds_heap_insert_exec_t *exec = container_of(base, kds_heap_insert_exec_t, base);
    kds_page_id_t current_id;
    kds_frame_t *frame;
    s64 new_pk;
    int ret;

    if (!exec->rel || exec->rel->kind != KDS_CLUSTERED_HEAP) {
        base->ret = -EINVAL;
        return KDS_EXEC_ERROR;
    }

    if (exec->data_len < sizeof(new_pk)) {
        /* Every row must carry at least the PK column's 8 bytes --
         * this should already be guaranteed by CREATE TABLE's
         * first-column-is-int64 rule, but a caller bug elsewhere
         * shouldn't be able to slip a too-short row past this. */
        base->ret = -EINVAL;
        return KDS_EXEC_ERROR;
    }
    memcpy(&new_pk, exec->data, sizeof(new_pk));

    ret = kds_heap_check_pk_duplicate(exec->rel->root_page_id, new_pk);
    if (ret < 0) {
        base->ret = ret;
        return KDS_EXEC_ERROR;
    }
    if (ret == 1) {
        base->ret = -EEXIST;
        return KDS_EXEC_ERROR;
    }

    current_id = exec->rel->root_page_id;

    for (;;) {
        kds_page_id_t next;

        frame = kds_buf_lookup_or_load(current_id);
        if (IS_ERR(frame)) {
            base->ret = PTR_ERR(frame);
            return KDS_EXEC_ERROR;
        }

        ret = heap_insert_tuple(frame, exec->data, exec->data_len, exec->xid,
                                 &exec->out_tid);
        if (!ret) {
            kds_buf_unpin(frame);
            return KDS_EXEC_DONE;
        }

        if (ret != -ENOSPC) {
            /* A real error (bad arguments, oversized payload, etc.)
             * -- not the "page is full" condition this exec knows
             * how to grow past. */
            kds_buf_unpin(frame);
            base->ret = ret;
            return KDS_EXEC_ERROR;
        }

        /* Page is full. If the chain already continues past this
         * page, just walk to the next one -- no allocation needed,
         * someone else's insert already grew the chain this far. */
        next = heap_get_next_page_id(frame);
        if (next != 0) {
            kds_buf_unpin(frame);
            current_id = next;
            continue;
        }

        /*
         * End of chain and full: this is the actual growth point.
         * kds_page_alloc() (the pre-allocation ring, kds_page_alloc.c)
         * is used here rather than kds_buf_alloc_new() directly --
         * same convention kds_catalog_create_table() already
         * follows for a brand-new heap-clustered table's first page.
         */
        {
            kds_frame_t *new_frame = kds_page_alloc(KDS_PAGE_TYPE_HEAP);

            if (!new_frame) {
                kds_buf_unpin(frame);
                base->ret = -ENOSPC; /* pre-allocation ring empty */
                return KDS_EXEC_ERROR;
            }

            heap_init_page(new_frame);

            ret = heap_set_next_page_id(frame, new_frame->kp->id);
            kds_buf_unpin(frame); /* done with the old tail either way */

            if (ret) {
                kds_buf_unpin(new_frame);
                base->ret = ret;
                return KDS_EXEC_ERROR;
            }

            ret = heap_insert_tuple(new_frame, exec->data, exec->data_len,
                                     exec->xid, &exec->out_tid);
            kds_buf_unpin(new_frame);

            if (ret) {
                /*
                 * A freshly initialized, completely empty page
                 * couldn't fit this tuple -- only possible if
                 * data_len itself is too large to ever fit in one
                 * page. Growing again wouldn't help; surface the
                 * error instead of looping forever.
                 */
                base->ret = ret;
                return KDS_EXEC_ERROR;
            }

            return KDS_EXEC_DONE;
        }
    }
}

void kds_heap_insert_exec_init(kds_heap_insert_exec_t *exec, kds_relation_t *rel,
                                const void *data, u16 data_len, u64 xid)
{
    exec->base.run = kds_heap_insert_exec_run;
    exec->base.ret = 0;

    exec->rel = rel;
    exec->data = data;
    exec->data_len = data_len;
    exec->xid = xid;

    exec->out_tid.page_id = 0;
    exec->out_tid.slot = 0;
}