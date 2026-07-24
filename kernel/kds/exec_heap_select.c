/* exec_heap_select.c
 *
 * Heap-clustered table select (scan) executor, plus the row-level
 * primitives shared with the btree select backend.
 *
 * This is the heap half of the SelectExec described in kds_executor.h.
 * The btree half lives in exec_btree_select.c -- the two clustering
 * strategies are kept in separate translation units on purpose (they
 * share the single kds_select_exec_t struct and the per-page scan
 * helpers below, but not a scan). The one public entry point,
 * kds_select_exec_init(), lives here and dispatches base.run by
 * rel->kind:
 *
 *   KDS_CLUSTERED_HEAP  -> kds_heap_select_exec_run   (this file)
 *   KDS_CLUSTERED_BTREE -> kds_btree_select_exec_run  (exec_btree_select.c)
 *
 * Phase sequence (heap):
 *
 *   SCAN
 *     Walk the heap page chain from rel->root_page_id, following
 *     next_page_id (heap.h) to the end. For every live tuple, evaluate
 *     the AND-combined WHERE conditions; matched rows are formatted and
 *     appended straight into the caller-owned out_buf. Resumable per
 *     row: cursor_page_id + cursor_slot + out_pos are all checkpointed
 *     in the exec struct so a CONTINUE picks back up at the exact next
 *     slot. On reaching the end of the chain (or filling out_buf) ->
 *     DONE.
 *
 *   DONE
 *     Trailing summary / truncation marker already appended.
 *
 * Output is streamed rather than returned: the matched-row count is
 * unbounded, so truncation has to be decided live against out_buf's
 * remaining space (see kds_executor.h's SelectExec comment).
 */

#include <linux/kds.h>
#include <linux/kds_executor.h>
#include <linux/kds_exec_select.h>
#include <linux/kds_relation.h>
#include <linux/kds_heap.h>
#include <linux/kds_types.h>
#include <linux/kds_page_mgr.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/string.h>

/* Max encoded row we can pull out of a heap tuple in one shot -- mirrors
 * KDS_DSHELL_ROW_MAX / KDS_EXEC_ROW_SCAN_BUF (not formally linked; if one
 * grows, check the others too). */
#define KDS_SELECT_ROW_MAX    256
/* Scratch buffer for formatting one output row line. */
#define KDS_SELECT_LINE_MAX   512
/* Space reserved at the tail of out_buf for the truncation marker. */
#define KDS_SELECT_TRUNC_MARGIN  24

/*
 * Provided by exec_btree_select.c. Declared here (not in the public
 * header) because kds_select_exec_init() is the single shared
 * dispatcher and this is the only site that needs to name the btree
 * backend; keeping the declaration local avoids widening
 * kds_executor.h's public API for an internal seam between the two
 * select translation units.
 */
kds_exec_result_t kds_btree_select_exec_run(kds_exec_state_t *base, u64 slice_ns);

static kds_exec_result_t kds_heap_select_exec_run(kds_exec_state_t *base, u64 slice_ns);

/* ------------------------------------------------------------------
 * Encoded-row helpers
 *
 * The on-disk row layout is column-by-column: fixed-width types occupy
 * their type descriptor's fixed_len bytes back-to-back; variable-width
 * types (varchar/char, fixed_len == 0) carry a u16 length prefix ahead
 * of their bytes. This matches kds_dshell_encode_row_from_vals() /
 * kds_dshell_decode_row() in dshell.c exactly.
 * ------------------------------------------------------------------ */

/*
 * Locates column `col_idx`'s encoded bytes within an encoded row.
 * On success sets *out_ptr / *out_len to that column's span and
 * returns 0. Returns -EINVAL if the row is malformed / too short, or
 * if a column type is unknown.
 */
static int select_col_span(const kds_schema_t *schema, const u8 *buf,
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

    return -EINVAL; /* col_idx out of range for this schema */
}

/*
 * WHERE-clause literals are encoded to on-disk bytes via the shared
 * kds_encode_ast_val() (parser.c) so SELECT, UPDATE, and INSERT agree
 * on encoding and the full uint64 range round-trips.
 */

/*
 * Decodes an encoded row into a human-readable "col=val, ..." string.
 * Local copy of dshell.c's kds_dshell_decode_row() -- kept here rather
 * than shared so the select executor doesn't depend on a dshell-private
 * static.
 */
static int select_decode_row(const kds_schema_t *schema, const u8 *buf,
                             u16 buf_len, char *out, size_t out_size)
{
    size_t off = 0;
    u32    i;
    int    n = 0;

    for (i = 0; i < schema->nr_cols; i++) {
        const kds_sys_column_t *col  = &schema->cols[i];
        const kds_type_desc_t  *desc =
            kds_type_lookup_by_val((kds_type_val_t)col->type_val);
        u16 value_len;
        int decoded;

        if (!desc)
            return -EINVAL;

        if (i > 0)
            n += scnprintf(out + n, out_size - n, ", ");

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

        n += scnprintf(out + n, out_size - n, "%s=", col->name);

        decoded = desc->decode(buf + off, value_len, out + n, out_size - n);
        if (decoded < 0)
            return decoded;
        n += decoded;

        off += value_len;
    }

    return n;
}

/* ------------------------------------------------------------------
 * WHERE evaluation
 * ------------------------------------------------------------------ */

static bool select_op_satisfied(kds_cond_op_t op, int cmp)
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

/*
 * Returns 1 if `buf` (an encoded row of buf_len bytes) satisfies every
 * resolved WHERE condition, 0 if it doesn't, or a negative errno on a
 * malformed row / unsupported comparison.
 *
 * EQ/NEQ use a raw encoded-byte comparison (works for every type);
 * ordering ops (< <= > >=) go through the type's compare_fn, which
 * kds_select_exec_init() already guaranteed is non-NULL for any column
 * an ordering op was resolved against.
 */
static int select_row_matches(const kds_select_exec_t *exec,
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

        ret = select_col_span(schema, buf, buf_len, cond->col_idx,
                              &col_ptr, &col_len);
        if (ret)
            return ret;

        ret = kds_encode_ast_val(desc, &cond->val, enc, sizeof(enc), &enc_len);
        if (ret)
            return ret;

        if (cond->op == KDS_OP_EQ || cond->op == KDS_OP_NEQ) {
            bool equal = (col_len == enc_len) &&
                         (memcmp(col_ptr, enc, col_len) == 0);
            cmp = equal ? 0 : 1; /* only equality is meaningful here */
        } else {
            if (!desc->compare)
                return -EOPNOTSUPP;
            cmp = desc->compare(col_ptr, col_len, enc, enc_len);
        }

        if (!select_op_satisfied(cond->op, cmp))
            return 0;
    }

    return 1;
}

/* ------------------------------------------------------------------
 * Output formatting
 * ------------------------------------------------------------------ */

/*
 * Formats one matched row into `line`, returning its length. Mirrors
 * kds_cmd_scan_table()'s per-row format so the on-wire output shape is
 * unchanged from the old synchronous scan.
 */
static int select_format_row(const kds_relation_t *rel,
                             kds_page_id_t page_id, u16 slot,
                             const kds_heap_tuple_hdr_t *hdr,
                             const u8 *buf, u16 buf_len,
                             char *line, size_t line_size)
{
    int n = 0;
    int decoded;

    n += scnprintf(line + n, line_size - n,
                   "\n  page=%llu [%u] xmin=%llu xmax=%llu ",
                   (u64)page_id, slot, hdr->xmin, hdr->xmax);

    decoded = select_decode_row(&rel->schema, buf, buf_len,
                                line + n, line_size - n);
    if (decoded < 0)
        return decoded;
    n += decoded;

    return n;
}

/* ------------------------------------------------------------------
 * Shared per-page scan (used by both heap and btree backends)
 * ------------------------------------------------------------------ */

kds_exec_result_t kds_select_scan_page(kds_select_exec_t *exec,
                                       kds_frame_t *frame, u16 *slot_io)
{
    kds_page_id_t page_id  = frame->kp->id;
    u16           nr_slots = heap_nr_slots(frame);

    while (*slot_io < nr_slots) {
        u16                   slot = *slot_io;
        kds_heap_tuple_hdr_t  hdr;
        u8                    row_buf[KDS_SELECT_ROW_MAX];
        int                   r;

        r = heap_read_tuple(frame, slot, &hdr, row_buf, sizeof(row_buf));
        if (r == -ENOENT) {
            /* dead slot -- still one unit of work, then move on */
            goto next_slot;
        }
        if (r < 0) {
            exec->base.ret = r;
            return KDS_EXEC_ERROR;
        }

        r = select_row_matches(exec, row_buf, hdr.data_len);
        if (r < 0) {
            exec->base.ret = r;
            return KDS_EXEC_ERROR;
        }

        if (r == 1) {
            char line[KDS_SELECT_LINE_MAX];
            int  ln = select_format_row(exec->rel, page_id, slot, &hdr,
                                        row_buf, hdr.data_len,
                                        line, sizeof(line));

            if (ln < 0) {
                exec->base.ret = ln;
                return KDS_EXEC_ERROR;
            }

            /* Does the formatted row still fit, leaving room for the
             * truncation marker? If not, stop the whole scan here. */
            if (exec->out_pos + (size_t)ln + KDS_SELECT_TRUNC_MARGIN
                    >= exec->out_size) {
                exec->truncated = true;
                return KDS_EXEC_DONE;
            }

            memcpy(exec->out_buf + exec->out_pos, line, ln);
            exec->out_pos += ln;
            exec->out_buf[exec->out_pos] = '\0';
            exec->rows_matched++;
        }

next_slot:
        (*slot_io)++;                  /* advance before any early return */
        exec->base.units_done++;

        if (kds_exec_slice_expired(&exec->base))
            return KDS_EXEC_CONTINUE;
    }

    return KDS_EXEC_DONE;
}

void kds_select_finish(kds_select_exec_t *exec)
{
    if (exec->truncated) {
        exec->out_pos += scnprintf(exec->out_buf + exec->out_pos,
                                   exec->out_size - exec->out_pos,
                                   "\n...(truncated)\n");
    } else {
        exec->out_pos += scnprintf(exec->out_buf + exec->out_pos,
                                   exec->out_size - exec->out_pos,
                                   "\n%u row(s) matched across %u page(s)\n",
                                   exec->rows_matched, exec->pages_visited);
    }
}

/* ------------------------------------------------------------------
 * Heap phase: SCAN
 * ------------------------------------------------------------------ */

static kds_exec_result_t heap_select_run_scan(kds_select_exec_t *exec)
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

        /* Count each page once, when we (re)enter it at its first slot. */
        if (exec->cursor_slot == 0)
            exec->pages_visited++;

        r = kds_select_scan_page(exec, frame, &exec->cursor_slot);
        if (r == KDS_EXEC_CONTINUE) {
            kds_buf_unpin(frame);
            return KDS_EXEC_CONTINUE;
        }
        if (r == KDS_EXEC_ERROR) {
            kds_buf_unpin(frame);
            return KDS_EXEC_ERROR;
        }
        if (exec->truncated) {
            kds_buf_unpin(frame);
            return KDS_EXEC_DONE;
        }

        /* Page exhausted -- advance to the next chain page. */
        next = heap_get_next_page_id(frame);
        kds_buf_unpin(frame);
        exec->cursor_page_id = next;
        exec->cursor_slot    = 0;

        if (exec->cursor_page_id != 0 && kds_exec_slice_expired(&exec->base))
            return KDS_EXEC_CONTINUE;
    }

    return KDS_EXEC_DONE;
}

/* ------------------------------------------------------------------
 * Heap run function
 * ------------------------------------------------------------------ */

static kds_exec_result_t
kds_heap_select_exec_run(kds_exec_state_t *base, u64 slice_ns)
{
    kds_select_exec_t *exec = container_of(base, kds_select_exec_t, base);

    if (!exec->rel || exec->rel->kind != KDS_CLUSTERED_HEAP) {
        base->ret = -EINVAL;
        return KDS_EXEC_ERROR;
    }

    for (;;) {
        switch (exec->phase) {

        case KDS_SELECT_PHASE_SCAN: {
            kds_exec_result_t r = heap_select_run_scan(exec);
            if (r != KDS_EXEC_DONE)
                return r;
            kds_select_finish(exec);
            exec->phase = KDS_SELECT_PHASE_DONE;
            continue;
        }

        case KDS_SELECT_PHASE_DONE:
            return KDS_EXEC_DONE;
        }
    }
}

/* ------------------------------------------------------------------
 * Index point-lookup run function
 *
 * Used when the planner (kds_select_exec_init) resolved a WHERE equality
 * on an indexed column to a single row-page. Scans exactly that one page
 * (exec->cursor_page_id) applying all WHERE conditions, then finishes.
 * cursor_page_id == 0 means the index proved no match -> zero rows. Works
 * for both heap and btree tables, since the index value is always a
 * single heap page id (a chain page or a bucket page).
 * ------------------------------------------------------------------ */

static kds_exec_result_t
kds_index_point_select_exec_run(kds_exec_state_t *base, u64 slice_ns)
{
    kds_select_exec_t *exec = container_of(base, kds_select_exec_t, base);

    for (;;) {
        switch (exec->phase) {

        case KDS_SELECT_PHASE_SCAN: {
            kds_frame_t      *frame;
            kds_exec_result_t r;

            if (exec->cursor_page_id == 0) {
                kds_select_finish(exec);       /* index: no matching row */
                exec->phase = KDS_SELECT_PHASE_DONE;
                continue;
            }

            frame = kds_buf_lookup_or_load(exec->cursor_page_id);
            if (IS_ERR(frame)) {
                exec->base.ret = PTR_ERR(frame);
                return KDS_EXEC_ERROR;
            }

            if (exec->cursor_slot == 0)
                exec->pages_visited++;

            r = kds_select_scan_page(exec, frame, &exec->cursor_slot);
            kds_buf_unpin(frame);

            if (r == KDS_EXEC_CONTINUE)
                return KDS_EXEC_CONTINUE;
            if (r == KDS_EXEC_ERROR)
                return KDS_EXEC_ERROR;

            /* Single page done (or output truncated). */
            kds_select_finish(exec);
            exec->phase = KDS_SELECT_PHASE_DONE;
            continue;
        }

        case KDS_SELECT_PHASE_DONE:
            return KDS_EXEC_DONE;
        }
    }
}

/*
 * Descends a btree-clustered table's clustering btree to the heap bucket
 * page that would hold primary key `key`, mirroring exec_btree_insert.c's
 * SEARCH_AND_PREPARE descent (read-only). On success sets *out_bucket to
 * the bucket page id -- 0 means "no bucket for that key range", i.e. no
 * such row. Returns false on an I/O error, so the caller full-scans
 * instead of wrongly reporting no match. Keys compare unsigned (u64),
 * matching the forced unsigned PK.
 */
static bool btree_bucket_for_pk(kds_page_id_t root, kds_tuple_id_t key,
                                kds_page_id_t *out_bucket)
{
    kds_page_id_t pid   = root;
    int           guard = 0;

    for (;;) {
        kds_frame_t      *frame;
        kds_btree_node_t  node;
        int               i, pos;

        if (!pid) {                 /* empty subtree -> no matching row */
            *out_bucket = 0;
            return true;
        }

        frame = kds_buf_lookup_or_load(pid);
        if (IS_ERR(frame))
            return false;           /* fall back to a full scan */
        load_btree_node(frame, &node);
        kds_buf_unpin(frame);

        pos = node.key_count;
        for (i = 0; i < node.key_count; i++) {
            if (key <= node.keys[i]) { pos = i; break; }
        }

        if (node.level == 0) {      /* leaf: the bucket is slots[pos] */
            *out_bucket = node.slots[pos];
            return true;
        }

        pid = node.slots[pos];
        if (++guard > BTREE_MAX_DEPTH)
            return false;
    }
}

/*
 * Planner: pin the scan to a single page when a WHERE equality lets us
 * skip the full scan. Two cases:
 *   (1) PK (column 0) equality on a btree-clustered table -> descend the
 *       clustering btree straight to the row's bucket page.
 *   (2) equality on a column with a secondary index -> kds_index_search().
 * Returns true if a plan was set (including a proven no-match, where
 * cursor_page_id is left 0). Best-effort: any snag falls back to a full
 * scan. All WHERE conditions are still applied per row on the chosen
 * page, so extra AND-conditions stay correct.
 */
static bool select_try_index_plan(kds_select_exec_t *exec, kds_relation_t *rel)
{
    u32 c;

    /* (1) PK equality on a btree-clustered table: clustering point lookup. */
    if (rel->kind == KDS_CLUSTERED_BTREE) {
        for (c = 0; c < exec->nr_conds; c++) {
            const kds_select_resolved_cond_t *cond = &exec->conds[c];
            const kds_type_desc_t            *desc;
            u8             enc[sizeof(u64)];
            u16            enc_len;
            kds_tuple_id_t key = 0;
            kds_page_id_t  bucket;

            if (cond->col_idx != 0 || cond->op != KDS_OP_EQ ||
                cond->val.type != KDS_VAL_INT)
                continue;

            desc = kds_type_lookup_by_val(
                (kds_type_val_t)rel->schema.cols[0].type_val);
            if (!desc)
                continue;
            if (kds_encode_ast_val(desc, &cond->val, enc, sizeof(enc), &enc_len))
                continue;
            if (enc_len == 0 || enc_len > sizeof(u64))
                continue;
            memcpy(&key, enc, enc_len);

            if (!btree_bucket_for_pk(rel->root_page_id, key, &bucket))
                break;                       /* descent failed -> full scan */

            exec->index_single_page = true;
            exec->cursor_page_id    = bucket; /* 0 => provably no matching row */
            exec->cursor_slot       = 0;
            return true;
        }
    }

    /* (2) Equality on a secondary-indexed column. */
    for (c = 0; c < exec->nr_conds; c++) {
        const kds_select_resolved_cond_t *cond = &exec->conds[c];
        kds_sys_index_t         idx;
        const kds_sys_column_t *col;
        const kds_type_desc_t  *desc;
        kds_relation_t         *irel;
        u8                      enc[sizeof(u64)];
        u16                     enc_len;
        kds_tuple_id_t          key = 0;
        kds_page_id_t           page_id = 0;
        int                     r;

        if (cond->op != KDS_OP_EQ || cond->val.type != KDS_VAL_INT)
            continue;

        /* Is this column indexed? (Only integer columns can be.) */
        if (kds_catalog_find_index_on_column(rel->oid, cond->col_idx, &idx))
            continue;

        col  = &rel->schema.cols[cond->col_idx];
        desc = kds_type_lookup_by_val((kds_type_val_t)col->type_val);
        if (!desc)
            continue;

        /* key = widen(encode(literal)) -- the exact widening index
         * maintenance applied to the stored column bytes, so equality
         * lines up. */
        if (kds_encode_ast_val(desc, &cond->val, enc, sizeof(enc), &enc_len))
            continue;
        if (enc_len == 0 || enc_len > sizeof(u64))
            continue;
        memcpy(&key, enc, enc_len);

        irel = kds_relation_open(idx.index_oid);
        if (IS_ERR(irel))
            continue;                       /* fall back to full scan */
        r = kds_index_search(irel, key, &page_id);
        kds_relation_close(irel);

        if (r == 0) {
            exec->index_single_page = true;
            exec->cursor_page_id    = page_id;
            exec->cursor_slot       = 0;
            return true;
        }
        if (r == -ENOENT) {
            exec->index_single_page = true;
            exec->cursor_page_id    = 0;    /* provably no matching row */
            exec->cursor_slot       = 0;
            return true;
        }
        /* other error -> ignore this index, try the next cond */
    }

    return false;
}

/* ------------------------------------------------------------------
 * Init (shared entry point -- dispatches by rel->kind)
 * ------------------------------------------------------------------ */

int kds_select_exec_init(kds_select_exec_t *exec, kds_relation_t *rel,
                         const kds_ast_cond_t *conds, u32 nr_conds,
                         char *out_buf, size_t out_size)
{
    u32 c;

    if (!exec || !rel || !out_buf || out_size == 0)
        return -EINVAL;
    if (nr_conds > KDS_PARSER_MAX_CONDS)
        return -EINVAL;

    exec->base.ret         = 0;
    exec->base.deadline_ns = 0;
    exec->base.units_done  = 0;

    exec->rel      = rel;
    exec->nr_conds = nr_conds;

    exec->out_buf       = out_buf;
    exec->out_size      = out_size;
    exec->rows_matched  = 0;
    exec->pages_visited = 0;
    exec->truncated     = false;

    exec->phase          = KDS_SELECT_PHASE_SCAN;
    exec->cursor_page_id = rel->root_page_id;
    exec->cursor_slot    = 0;

    memset(&exec->btree, 0, sizeof(exec->btree));
    /* The btree backend starts by descending root -> leftmost leaf;
     * the heap backend never reads this. */
    exec->btree.descending = (rel->kind == KDS_CLUSTERED_BTREE);

    /*
     * Resolve each WHERE column name to a schema index once, up front,
     * so the hot scan path never re-does a string lookup per row. This
     * step is synchronous (no I/O) so any failure is reported here
     * rather than from a run() call:
     *   -ENOENT      column name not in schema
     *   -EOPNOTSUPP  ordering op on a type with no compare_fn
     */
    for (c = 0; c < nr_conds; c++) {
        const kds_ast_cond_t   *src = &conds[c];
        const kds_sys_column_t *col = kds_schema_find_column(&rel->schema,
                                                             src->col_name);
        const kds_type_desc_t  *desc;

        if (!col)
            return -ENOENT;

        desc = kds_type_lookup_by_val((kds_type_val_t)col->type_val);
        if (!desc)
            return -EINVAL;

        if (src->op != KDS_OP_EQ && src->op != KDS_OP_NEQ && !desc->compare)
            return -EOPNOTSUPP;

        exec->conds[c].col_idx = (u32)(col - rel->schema.cols);
        exec->conds[c].op      = src->op;
        exec->conds[c].val     = src->val;
    }

    /* "OK " response prefix; matched rows stream in after it. */
    exec->out_pos = scnprintf(out_buf, out_size, "OK ");

    /*
     * Try an index point-lookup plan first. If one is chosen, the scan
     * runs against a single page via kds_index_point_select_exec_run
     * regardless of the table's clustering; otherwise fall back to the
     * kind-specific full scan.
     */
    exec->index_single_page = false;
    if (select_try_index_plan(exec, rel)) {
        exec->base.run = kds_index_point_select_exec_run;
    } else if (rel->kind == KDS_CLUSTERED_HEAP) {
        exec->base.run = kds_heap_select_exec_run;
    } else {
        exec->base.run = kds_btree_select_exec_run;
    }

    return 0;
}
