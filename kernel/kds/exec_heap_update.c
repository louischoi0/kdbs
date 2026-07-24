/* exec_heap_update.c
 *
 * Heap-clustered table UPDATE executor, plus the per-page apply helper
 * shared with the btree backend (exec_btree_update.c). Structured like
 * exec_heap_select.c: this file owns the shared helpers and the single
 * public kds_update_exec_init() dispatcher, and implements the heap
 * chain scan; the btree leaf/bucket scan lives in its own file.
 *
 * Phase sequence (heap):
 *
 *   SCAN
 *     Walk the heap page chain from rel->root_page_id. For each page,
 *     snapshot its slot count, then for every live tuple that matches
 *     WHERE, rebuild the row with the SET values applied and hand it to
 *     kds_heap_update_tuple() (undo + WAL + sync). Resumable per row.
 *
 *   DONE
 *     Summary written by kds_update_finish().
 */

#include <linux/kds.h>
#include <linux/kds_executor.h>
#include <linux/kds_exec_update.h>
#include <linux/kds_relation.h>
#include <linux/kds_index_maint.h>
#include <linux/kds_heap.h>
#include <linux/kds_undo.h>
#include <linux/kds_types.h>
#include <linux/kds_page_mgr.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/string.h>

/* Max encoded row (old or rebuilt) handled in one shot. */
#define KDS_UPDATE_ROW_MAX  256

/* Provided by exec_btree_update.c (btree dispatch target). */
kds_exec_result_t kds_btree_update_exec_run(kds_exec_state_t *base, u64 slice_ns);

static kds_exec_result_t kds_heap_update_exec_run(kds_exec_state_t *base, u64 slice_ns);

/* ------------------------------------------------------------------
 * Encoded-row helpers (same on-disk layout as insert/select: fixed
 * types back-to-back; variable types u16-length-prefixed).
 * ------------------------------------------------------------------ */

/* SET/WHERE literals encode through the shared kds_encode_ast_val()
 * (parser.c) -- same path as SELECT and INSERT. */

static int update_col_span(const kds_schema_t *schema, const u8 *buf,
                           u16 buf_len, u32 col_idx,
                           const void **out_ptr, u16 *out_len)
{
    size_t off = 0;
    u32    i;

    for (i = 0; i < schema->nr_cols; i++) {
        const kds_sys_column_t *col  = &schema->cols[i];
        const kds_type_desc_t  *desc =
            kds_type_lookup_by_val((kds_type_val_t)col->type_val);
        u16 value_len;

        if (!desc)
            return -EINVAL;

        if (desc->fixed_len == 0) {
            if (off + sizeof(u16) > buf_len)
                return -EINVAL;
            memcpy(&value_len, buf + off, sizeof(u16));
            off += sizeof(u16);
        } else {
            value_len = desc->fixed_len;
        }

        if ((size_t)off + value_len > buf_len)
            return -EINVAL;

        if (i == col_idx) {
            *out_ptr = buf + off;
            *out_len = value_len;
            return 0;
        }

        off += value_len;
    }

    return -EINVAL;
}

/* ------------------------------------------------------------------
 * WHERE evaluation (identical semantics to SelectExec)
 * ------------------------------------------------------------------ */

static bool update_op_satisfied(kds_cond_op_t op, int cmp)
{
    switch (op) {
    case KDS_OP_EQ:  return cmp == 0;
    case KDS_OP_NEQ: return cmp != 0;
    case KDS_OP_LT:  return cmp < 0;
    case KDS_OP_LTE: return cmp <= 0;
    case KDS_OP_GT:  return cmp > 0;
    case KDS_OP_GTE: return cmp >= 0;
    }
    return false;
}

static int update_row_matches(const kds_update_exec_t *exec,
                              const u8 *buf, u16 buf_len)
{
    const kds_schema_t *schema = &exec->rel->schema;
    u32 c;

    for (c = 0; c < exec->nr_conds; c++) {
        const kds_select_resolved_cond_t *cond = &exec->conds[c];
        const kds_sys_column_t *col = &schema->cols[cond->col_idx];
        const kds_type_desc_t  *desc =
            kds_type_lookup_by_val((kds_type_val_t)col->type_val);
        const void *col_ptr;
        u16         col_len;
        u8          enc[KDS_PARSER_VAL_MAX];
        u16         enc_len;
        int         cmp;
        int         ret;

        if (!desc)
            return -EINVAL;

        ret = update_col_span(schema, buf, buf_len, cond->col_idx,
                              &col_ptr, &col_len);
        if (ret)
            return ret;

        ret = kds_encode_ast_val(desc, &cond->val, enc, sizeof(enc), &enc_len);
        if (ret)
            return ret;

        if (cond->op == KDS_OP_EQ || cond->op == KDS_OP_NEQ) {
            bool equal = (col_len == enc_len) &&
                         (memcmp(col_ptr, enc, col_len) == 0);
            cmp = equal ? 0 : 1;
        } else {
            if (!desc->compare)
                return -EOPNOTSUPP;
            cmp = desc->compare(col_ptr, col_len, enc, enc_len);
        }

        if (!update_op_satisfied(cond->op, cmp))
            return 0;
    }

    return 1;
}

/* ------------------------------------------------------------------
 * Row rebuild: copy the old encoded row column-by-column, substituting
 * the SET columns with freshly-encoded new values.
 * ------------------------------------------------------------------ */

static int update_build_row(const kds_update_exec_t *exec,
                            const u8 *old_buf, u16 old_len,
                            u8 *new_buf, size_t new_cap, u16 *out_len)
{
    const kds_schema_t *schema = &exec->rel->schema;
    size_t in_off  = 0;
    size_t out_off = 0;
    u32    i, a;

    for (i = 0; i < schema->nr_cols; i++) {
        const kds_sys_column_t *col  = &schema->cols[i];
        const kds_type_desc_t  *desc =
            kds_type_lookup_by_val((kds_type_val_t)col->type_val);
        const kds_ast_val_t    *set_val = NULL;
        u16 in_vlen;

        if (!desc)
            return -EINVAL;

        /* Length of column i in the old row. */
        if (desc->fixed_len == 0) {
            if (in_off + sizeof(u16) > old_len)
                return -EINVAL;
            memcpy(&in_vlen, old_buf + in_off, sizeof(u16));
            in_off += sizeof(u16);
        } else {
            in_vlen = desc->fixed_len;
        }
        if ((size_t)in_off + in_vlen > old_len)
            return -EINVAL;

        /* Is this column being SET? */
        for (a = 0; a < exec->nr_assigns; a++) {
            if (exec->assigns[a].col_idx == i) {
                set_val = &exec->assigns[a].val;
                break;
            }
        }

        if (set_val) {
            u16 enc_len;
            int ret;

            if (desc->fixed_len == 0) {
                if (out_off + sizeof(u16) > new_cap)
                    return -ENOSPC;
                ret = kds_encode_ast_val(desc, set_val,
                                        new_buf + out_off + sizeof(u16),
                                        new_cap - out_off - sizeof(u16),
                                        &enc_len);
                if (ret)
                    return ret;
                memcpy(new_buf + out_off, &enc_len, sizeof(u16));
                out_off += sizeof(u16) + enc_len;
            } else {
                if (out_off + desc->fixed_len > new_cap)
                    return -ENOSPC;
                ret = kds_encode_ast_val(desc, set_val, new_buf + out_off,
                                        new_cap - out_off, &enc_len);
                if (ret)
                    return ret;
                out_off += enc_len;   /* == fixed_len */
            }
        } else {
            /* Copy old column bytes verbatim (with var-width prefix). */
            if (desc->fixed_len == 0) {
                if (out_off + sizeof(u16) + in_vlen > new_cap)
                    return -ENOSPC;
                memcpy(new_buf + out_off, &in_vlen, sizeof(u16));
                out_off += sizeof(u16);
            } else {
                if (out_off + in_vlen > new_cap)
                    return -ENOSPC;
            }
            memcpy(new_buf + out_off, old_buf + in_off, in_vlen);
            out_off += in_vlen;
        }

        in_off += in_vlen;
    }

    *out_len = (u16)out_off;
    return 0;
}

/* ------------------------------------------------------------------
 * Shared per-page apply
 * ------------------------------------------------------------------ */

kds_exec_result_t kds_update_apply_page(kds_update_exec_t *exec,
                                        kds_frame_t *frame,
                                        u16 *slot_io, u16 slot_limit)
{
    while (*slot_io < slot_limit) {
        u16                   slot = *slot_io;
        kds_heap_tuple_hdr_t  hdr;
        u8                    old_buf[KDS_UPDATE_ROW_MAX];
        int                   r;

        r = heap_read_tuple(frame, slot, &hdr, old_buf, sizeof(old_buf));
        if (r == -ENOENT)
            goto next_slot;                 /* dead slot */
        if (r < 0) {
            exec->base.ret = r;
            return KDS_EXEC_ERROR;
        }

        r = update_row_matches(exec, old_buf, hdr.data_len);
        if (r < 0) {
            exec->base.ret = r;
            return KDS_EXEC_ERROR;
        }

        if (r == 1) {
            u8             new_buf[KDS_UPDATE_ROW_MAX];
            u16            new_len;
            kds_heap_tid_t out_tid;

            exec->rows_matched++;

            r = update_build_row(exec, old_buf, hdr.data_len,
                                 new_buf, sizeof(new_buf), &new_len);
            if (r < 0) {
                exec->base.ret = r;
                return KDS_EXEC_ERROR;
            }

            r = kds_heap_update_tuple(frame, slot, new_buf, new_len,
                                      exec->xid, exec->owner_oid, &out_tid);
            if (r < 0) {
                /* Non-atomic: rows updated before this one stay updated. */
                exec->base.ret = r;
                return KDS_EXEC_ERROR;
            }
            exec->rows_updated++;

            /*
             * Maintain any index whose keyed column changed value: delete
             * the old key, insert the new one (pointing at out_tid's
             * page). Skipped internally for unchanged columns, so a
             * non-indexed-column UPDATE touches no index. Not WAL-logged
             * (same crash caveat as the insert path); a new-key collision
             * (-EEXIST) fails the UPDATE without rolling this row back.
             */
            r = kds_index_maint_on_update(exec->owner_oid, &exec->rel->schema,
                                          old_buf, hdr.data_len,
                                          new_buf, new_len, out_tid.page_id);
            if (r < 0) {
                exec->base.ret = r;
                return KDS_EXEC_ERROR;
            }
        }

next_slot:
        (*slot_io)++;                       /* advance before any early return */
        exec->base.units_done++;

        if (kds_exec_slice_expired(&exec->base))
            return KDS_EXEC_CONTINUE;
    }

    return KDS_EXEC_DONE;
}

void kds_update_finish(kds_update_exec_t *exec)
{
    scnprintf(exec->out_buf, exec->out_size,
              "OK %u row(s) updated (%u matched across %u page(s))\n",
              exec->rows_updated, exec->rows_matched, exec->pages_visited);
}

/* ------------------------------------------------------------------
 * Heap phase: SCAN
 * ------------------------------------------------------------------ */

static kds_exec_result_t heap_update_run_scan(kds_update_exec_t *exec)
{
    while (exec->cursor_page_id != 0) {
        kds_frame_t      *frame;
        kds_page_id_t     next;
        kds_exec_result_t r;

        frame = kds_buf_lookup_or_load(exec->cursor_page_id);
        if (IS_ERR(frame)) {
            exec->base.ret = PTR_ERR(frame);
            return KDS_EXEC_ERROR;
        }

        /* Snapshot slot count once per page (skip on mid-page resume). */
        if (exec->cursor_slot == 0) {
            exec->pages_visited++;
            exec->page_slot_limit = heap_nr_slots(frame);
        }

        r = kds_update_apply_page(exec, frame, &exec->cursor_slot,
                                  exec->page_slot_limit);
        if (r == KDS_EXEC_CONTINUE) {
            kds_buf_unpin(frame);
            return KDS_EXEC_CONTINUE;
        }
        if (r == KDS_EXEC_ERROR) {
            kds_buf_unpin(frame);
            return KDS_EXEC_ERROR;
        }

        next = heap_get_next_page_id(frame);
        kds_buf_unpin(frame);
        exec->cursor_page_id = next;
        exec->cursor_slot    = 0;

        if (exec->cursor_page_id != 0 && kds_exec_slice_expired(&exec->base))
            return KDS_EXEC_CONTINUE;
    }

    return KDS_EXEC_DONE;
}

static kds_exec_result_t
kds_heap_update_exec_run(kds_exec_state_t *base, u64 slice_ns)
{
    kds_update_exec_t *exec = container_of(base, kds_update_exec_t, base);

    if (!exec->rel || exec->rel->kind != KDS_CLUSTERED_HEAP) {
        base->ret = -EINVAL;
        return KDS_EXEC_ERROR;
    }

    for (;;) {
        switch (exec->phase) {

        case KDS_UPDATE_PHASE_SCAN: {
            kds_exec_result_t r = heap_update_run_scan(exec);
            if (r != KDS_EXEC_DONE)
                return r;
            kds_update_finish(exec);
            exec->phase = KDS_UPDATE_PHASE_DONE;
            continue;
        }

        case KDS_UPDATE_PHASE_DONE:
            return KDS_EXEC_DONE;
        }
    }
}

/* ------------------------------------------------------------------
 * Init (shared entry point -- dispatches by rel->kind)
 * ------------------------------------------------------------------ */

int kds_update_exec_init(kds_update_exec_t *exec, kds_relation_t *rel,
                         const kds_ast_assign_t *assigns, u32 nr_assigns,
                         const kds_ast_cond_t *conds, u32 nr_conds,
                         u64 xid, char *out_buf, size_t out_size)
{
    u32 i;

    if (!exec || !rel || !out_buf || out_size == 0)
        return -EINVAL;
    if (nr_assigns == 0 || nr_assigns > KDS_SCHEMA_MAX_COLUMNS)
        return -EINVAL;
    if (nr_conds > KDS_PARSER_MAX_CONDS)
        return -EINVAL;

    memset(exec, 0, sizeof(*exec));

    exec->rel       = rel;
    exec->xid       = xid;
    exec->owner_oid = rel->oid;
    exec->out_buf   = out_buf;
    exec->out_size  = out_size;

    exec->phase          = KDS_UPDATE_PHASE_SCAN;
    exec->cursor_page_id = rel->root_page_id;
    exec->cursor_slot    = 0;
    exec->btree.descending = (rel->kind == KDS_CLUSTERED_BTREE);

    /* Resolve SET assignments; forbid touching the primary key (col 0). */
    for (i = 0; i < nr_assigns; i++) {
        const kds_sys_column_t *col =
            kds_schema_find_column(&rel->schema, assigns[i].col_name);
        u32 idx;

        if (!col)
            return -ENOENT;
        idx = (u32)(col - rel->schema.cols);
        if (idx == 0)
            return -EPERM;   /* cannot UPDATE the primary key */

        exec->assigns[i].col_idx = idx;
        exec->assigns[i].val     = assigns[i].val;
    }
    exec->nr_assigns = nr_assigns;

    /* Resolve WHERE conditions (same rules as SelectExec). */
    for (i = 0; i < nr_conds; i++) {
        const kds_sys_column_t *col =
            kds_schema_find_column(&rel->schema, conds[i].col_name);
        const kds_type_desc_t  *desc;

        if (!col)
            return -ENOENT;
        desc = kds_type_lookup_by_val((kds_type_val_t)col->type_val);
        if (!desc)
            return -EINVAL;
        if (conds[i].op != KDS_OP_EQ && conds[i].op != KDS_OP_NEQ &&
            !desc->compare)
            return -EOPNOTSUPP;

        exec->conds[i].col_idx = (u32)(col - rel->schema.cols);
        exec->conds[i].op      = conds[i].op;
        exec->conds[i].val     = conds[i].val;
    }
    exec->nr_conds = nr_conds;

    exec->base.run = (rel->kind == KDS_CLUSTERED_HEAP)
        ? kds_heap_update_exec_run
        : kds_btree_update_exec_run;

    return 0;
}
