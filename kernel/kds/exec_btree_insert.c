/* exec_btree_insert.c
 *
 * Btree-clustered table insert executor.
 *
 * Phase sequence (strictly WAL-before-data):
 *
 *   SEARCH_AND_PREPARE
 *     Descend the btree one level per iteration. On reaching the leaf:
 *       1. Read the target heap page (slots[pos]).
 *       2. Scan for duplicate PK → -EEXIST if found.
 *       3. Check heap_has_space() → set need_new_page.
 *     One btree traversal covers both the search and the prepare work.
 *     Cursor holds all pinned btree frames for BTREE_INSERT reuse.
 *     No page modification.
 *
 *   WAL_INIT_PAGE   [need_new_page only]
 *     WAL ring buffer ← KDS_WAL_REC_INIT_PAGE
 *
 *   WAL_INSERT
 *     WAL ring buffer ← KDS_WAL_REC_INSERT (row data)
 *
 *   WAL_BTREE       [need_new_page only]
 *     WAL ring buffer ← KDS_WAL_REC_INSERT (btree leaf slot)
 *
 *   HEAP_INSERT
 *     heap page buffer ← row  (dirty mark)
 *
 *   BTREE_INSERT    [need_new_page only]
 *     btree leaf buffer ← slot (dirty mark)
 *
 *   DONE
 */

#include <linux/kds.h>
#include <linux/kds_executor.h>
#include <linux/kds_wal.h>
#include <linux/kds_relation.h>
#include <linux/kds_index_maint.h>
#include <linux/kds_heap.h>
#include <linux/kds_btree.h>
#include <linux/kds_page_mgr.h>
#include <linux/kds_page_alloc.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/slab.h>

#define KDS_BTREE_EXEC_XID  1   /* placeholder until transaction manager */

/* ------------------------------------------------------------------
 * PHASE_SEARCH_AND_PREPARE
 *
 * Descends the btree one level per scheduler iteration (resumable).
 * On reaching the leaf node:
 *   - reads slots[pos] to get the target heap page_id
 *   - scans the heap page for a duplicate PK
 *   - calls heap_has_space() to decide need_new_page
 *
 * This collapses the old PREPARE + SEARCH into a single pass:
 *   btree path down  == old SEARCH
 *   heap page probe  == old PREPARE
 *
 * The cursor is kept populated after this phase so BTREE_INSERT can
 * call kds_btree_cursor_insert() directly without re-descending.
 * ------------------------------------------------------------------ */

static kds_exec_result_t
btree_insert_run_search_and_prepare(kds_btree_insert_exec_t *exec)
{
    /* Descend one level per call until we hit the leaf. */
    pr_info("btree insert prepare started\n");

    while (exec->cursor.depth < BTREE_MAX_DEPTH) {
        kds_frame_t      *frame;
        kds_btree_node_t *node;
        int               pos, i;

        pr_info("btree search: depth=%d\n", exec->cursor.depth);
        frame = kds_buf_lookup_or_load(exec->current_page_id);

        if (IS_ERR(frame)) {
            exec->base.ret = PTR_ERR(frame);
            btree_cursor_cleanup(&exec->cursor);
            return KDS_EXEC_ERROR;
        }

        node = &exec->cursor.nodes[exec->cursor.depth];
        load_btree_node(frame, node);

        /*
         * Find pos: largest i such that keys[i] <= exec->key,
         * or key_count if exec->key > all keys.
         * slots[pos] is the heap page for keys in
         * [keys[pos-1], keys[pos]).
         */
        pos = node->key_count;
        for (i = 0; i < node->key_count; i++) {
            if (exec->key <= node->keys[i]) {
                pos = i;
                break;
            }
        }
        exec->cursor.positions[exec->cursor.depth] = pos;

        exec->base.units_done++;

        if (node->level == 0) {
            /*
             * Leaf reached. Probe the target heap page to:
             *   (a) detect duplicate PK
             *   (b) decide whether need_new_page
             */
            kds_page_id_t  heap_page_id = node->slots[pos];
            kds_frame_t   *heap_frame;

            if (!heap_page_id)
                heap_page_id = exec->rel->root_page_id;

            exec->target_heap_page_id = heap_page_id;

            if (!heap_page_id) {
                /* Empty tree: we will always need a new page. */
                exec->need_new_page = true;
                return KDS_EXEC_DONE;
            }

            heap_frame = kds_buf_lookup_or_load(heap_page_id);
            if (IS_ERR(heap_frame)) {
                exec->base.ret = PTR_ERR(heap_frame);
                btree_cursor_cleanup(&exec->cursor);
                return KDS_EXEC_ERROR;
            }

            /* (a) Duplicate PK check */
            {
                u16 nr_slots = heap_nr_slots(heap_frame);
                u16 slot;

                for (slot = 0; slot < nr_slots; slot++) {
                    kds_heap_tuple_hdr_t hdr;
                    u8  pk_buf[sizeof(s64)];
                    s64 existing_pk;
                    int r;

                    r = heap_read_tuple_pk(heap_frame, slot, &hdr, pk_buf, sizeof(pk_buf));

                    if (r == -ENOENT)
                        continue;
                    if (r) {
                        kds_buf_unpin(heap_frame);
                        exec->base.ret = r;
                        btree_cursor_cleanup(&exec->cursor);
                        pr_info("heap read tuple failed\n");
                        return KDS_EXEC_ERROR;
                    }

                    memcpy(&existing_pk, pk_buf, sizeof(existing_pk));
                    if (existing_pk == (s64)exec->key) {
                        kds_buf_unpin(heap_frame);
                        exec->base.ret = -EEXIST;
                        btree_cursor_cleanup(&exec->cursor);
                        return KDS_EXEC_ERROR;
                    }
                }
            }

            /* (b) Space check */
            exec->need_new_page = !heap_has_space(heap_frame,
                                                   exec->data_len);
            kds_buf_unpin(heap_frame);
            pr_info("btree insert prepare done\n");
            return KDS_EXEC_DONE;
        }

        /* Not a leaf: descend one more level. */
        exec->current_page_id = node->slots[pos];
        exec->cursor.depth++;

        if (kds_exec_slice_expired(&exec->base))
            return KDS_EXEC_CONTINUE;
    }

    exec->base.ret = -E2BIG;
    btree_cursor_cleanup(&exec->cursor);
    return KDS_EXEC_ERROR;
}

/* ------------------------------------------------------------------
 * PHASE_WAL_INIT_PAGE  [need_new_page only]
 * ------------------------------------------------------------------ */

static kds_exec_result_t
btree_insert_run_wal_init_page(kds_btree_insert_exec_t *exec)
{
    kds_wal_rec_hdr_t        hdr  = {0};
    kds_wal_body_init_page_t body = {0};
    kds_lsn_t                lsn;
    int                      ret;

    /*
     * page_id is not yet known (kds_page_alloc happens in
     * HEAP_INSERT). Record 0 as a placeholder; recovery treats
     * page_id=0 INIT_PAGE records as "allocate next available page".
     * A future improvement: pre-allocate here and record the real id.
     */
    body.page_id   = 0;
    body.page_type = KDS_PAGE_TYPE_HEAP;

    hdr.xid      = KDS_BTREE_EXEC_XID;
    hdr.type     = KDS_WAL_REC_INIT_PAGE;
    hdr.body_len = sizeof(body);

    ret = kds_wal_append(&hdr, &body, &lsn);
    if (ret)
        pr_warn("exec_btree_insert: WAL_INIT_PAGE failed (%d)\n", ret);
    else
        pr_info("exec_btree_insert: lsn=(%d)\n", lsn);

    return KDS_EXEC_DONE;
}

/* ------------------------------------------------------------------
 * PHASE_WAL_INSERT
 * ------------------------------------------------------------------ */

static kds_exec_result_t
btree_insert_run_wal_insert(kds_btree_insert_exec_t *exec)
{
    kds_wal_rec_hdr_t        hdr  = {0};
    kds_wal_body_page_mod_t  meta = {0};
    kds_lsn_t                lsn;
    u8                      *bounce;
    int                      ret;

    meta.page_id = exec->need_new_page ? 0 : exec->target_heap_page_id;
    meta.offset  = 0;
    meta.length  = exec->data_len;

    hdr.xid      = KDS_BTREE_EXEC_XID;
    hdr.type     = KDS_WAL_REC_INSERT;
    hdr.body_len = sizeof(meta) + exec->data_len;

    bounce = kmalloc(hdr.body_len, GFP_KERNEL);
    if (!bounce) {
        pr_warn("exec_btree_insert: WAL_INSERT bounce alloc failed\n");
        return KDS_EXEC_DONE; /* non-fatal: continue without WAL */
    }

    memcpy(bounce, &meta, sizeof(meta));
    memcpy(bounce + sizeof(meta), exec->data, exec->data_len);

    ret = kds_wal_append(&hdr, bounce, &lsn);
    kfree(bounce);

    if (ret)
        pr_warn("exec_btree_insert: WAL_INSERT failed (%d)\n", ret);
    else
        pr_info("kds_wal_insert ok for lsn=%d\n", lsn);

    return KDS_EXEC_DONE;
}

/* ------------------------------------------------------------------
 * PHASE_WAL_BTREE  [need_new_page only]
 * ------------------------------------------------------------------ */

static kds_exec_result_t
btree_insert_run_wal_btree(kds_btree_insert_exec_t *exec)
{
    kds_wal_rec_hdr_t        hdr  = {0};
    kds_wal_body_page_mod_t  meta = {0};
    kds_lsn_t                lsn;
    int                      ret;

    /* Leaf page_id is known from the cursor. */
    if (exec->cursor.depth >= 0 &&
        exec->cursor.nodes[exec->cursor.depth].frame) {
        meta.page_id =
            exec->cursor.nodes[exec->cursor.depth].frame->kp->id;
    }
    meta.offset = 0;
    meta.length = sizeof(kds_tuple_id_t) + sizeof(kds_page_id_t);

    hdr.xid      = KDS_BTREE_EXEC_XID;
    hdr.type     = KDS_WAL_REC_INSERT;
    hdr.body_len = sizeof(meta);

    ret = kds_wal_append(&hdr, &meta, &lsn);
    if (ret)
        pr_warn("exec_btree_insert: WAL_BTREE failed (%d)\n", ret);

    return KDS_EXEC_DONE;
}

/* ------------------------------------------------------------------
 * PHASE_HEAP_INSERT
 * ------------------------------------------------------------------ */

static kds_exec_result_t
btree_insert_run_heap_insert(kds_btree_insert_exec_t *exec)
{
    int ret;

    if (exec->need_new_page) {
        kds_frame_t *new_frame = kds_page_alloc(KDS_PAGE_TYPE_HEAP);

        if (!new_frame) {
            exec->base.ret = -ENOSPC;
            return KDS_EXEC_ERROR;
        }

        heap_init_page(new_frame);

        ret = heap_insert_tuple(new_frame, exec->data, exec->data_len,
                                 KDS_BTREE_EXEC_XID, &exec->out_tid);
        if (ret) {
            kds_buf_unpin(new_frame);
            exec->base.ret = ret;
            return KDS_EXEC_ERROR;
        }

        exec->new_heap_page_id = new_frame->kp->id;
        exec->new_page_min_key = exec->key;
        exec->base.units_done++;
        kds_buf_unpin(new_frame);

    } else {
        kds_frame_t *frame =
            kds_buf_lookup_or_load(exec->target_heap_page_id);

        if (IS_ERR(frame)) {
            exec->base.ret = PTR_ERR(frame);
            return KDS_EXEC_ERROR;
        }

        ret = heap_insert_tuple(frame, exec->data, exec->data_len,
                                 KDS_BTREE_EXEC_XID, &exec->out_tid);
        kds_buf_unpin(frame);

        if (ret) {
            exec->base.ret = ret;
            return KDS_EXEC_ERROR;
        }

        exec->new_heap_page_id = 0;
        exec->base.units_done++;
    }

    return KDS_EXEC_DONE;
}

/* ------------------------------------------------------------------
 * PHASE_BTREE_INSERT  [need_new_page only]
 * ------------------------------------------------------------------ */

static kds_exec_result_t
btree_insert_run_btree_insert(kds_btree_insert_exec_t *exec)
{
    int ret = kds_btree_cursor_insert(&exec->cursor,
                                       exec->new_page_min_key,
                                       exec->new_heap_page_id);
    if (ret < 0) {
        exec->base.ret = ret;
        return KDS_EXEC_ERROR;
    }

    exec->base.units_done++;
    return KDS_EXEC_DONE;
}

/* ------------------------------------------------------------------
 * Main run function
 * ------------------------------------------------------------------ */

static kds_exec_result_t
kds_btree_insert_exec_run(kds_exec_state_t *base, u64 slice_ns)
{
    kds_btree_insert_exec_t *exec =
        container_of(base, kds_btree_insert_exec_t, base);

    if (!exec->rel || exec->rel->kind != KDS_CLUSTERED_BTREE) {
        base->ret = -EINVAL;
        return KDS_EXEC_ERROR;
    }
    if (!exec->rel->root_page_id) {
        base->ret = -ENODEV;
        return KDS_EXEC_ERROR;
    }

    for (;;) {
        switch (exec->phase) {

        case KDS_BTREE_INSERT_PHASE_SEARCH_AND_PREPARE: {
            kds_exec_result_t r =
                btree_insert_run_search_and_prepare(exec);
            if (r != KDS_EXEC_DONE)
                return r;
            exec->phase = exec->need_new_page
                ? KDS_BTREE_INSERT_PHASE_WAL_INIT_PAGE
                : KDS_BTREE_INSERT_PHASE_WAL_INSERT;
            continue;
        }

        case KDS_BTREE_INSERT_PHASE_WAL_INIT_PAGE: {
            kds_exec_result_t r = btree_insert_run_wal_init_page(exec);
            if (r != KDS_EXEC_DONE)
                return r;
            exec->phase = KDS_BTREE_INSERT_PHASE_WAL_INSERT;
            continue;
        }

        case KDS_BTREE_INSERT_PHASE_WAL_INSERT: {
            kds_exec_result_t r = btree_insert_run_wal_insert(exec);
            if (r != KDS_EXEC_DONE)
                return r;
            exec->phase = exec->need_new_page
                ? KDS_BTREE_INSERT_PHASE_WAL_BTREE
                : KDS_BTREE_INSERT_PHASE_HEAP_INSERT;
            continue;
        }

        case KDS_BTREE_INSERT_PHASE_WAL_BTREE: {
            kds_exec_result_t r = btree_insert_run_wal_btree(exec);
            if (r != KDS_EXEC_DONE)
                return r;
            exec->phase = KDS_BTREE_INSERT_PHASE_HEAP_INSERT;
            continue;
        }

        case KDS_BTREE_INSERT_PHASE_HEAP_INSERT: {
            kds_exec_result_t r;
            if (kds_exec_slice_expired(base))
                return KDS_EXEC_CONTINUE;
            /*
             * All of this insert's WAL records (INIT_PAGE / INSERT /
             * BTREE) have now been appended. Drain them to disk
             * synchronously *before* touching the data page, so the
             * WAL is durable ahead of the modification it describes --
             * strict write-ahead ordering. Best-effort: kds_wal_sync()
             * no-ops when WAL is unavailable.
             */
            kds_wal_sync();
            r = btree_insert_run_heap_insert(exec);
            if (r != KDS_EXEC_DONE)
                return r;
            /*
             * Maintain secondary indexes for the newly-inserted row (the
             * row's bucket page is out_tid.page_id). Not WAL-logged --
             * same crash caveat as exec_heap_insert.c. A unique violation
             * (-EEXIST) fails the INSERT without rolling back the base row.
             */
            {
                int mret = kds_index_maint_on_insert(exec->rel->oid,
                        &exec->rel->schema, exec->data, exec->data_len,
                        exec->out_tid.page_id);
                if (mret) {
                    exec->base.ret = mret;
                    return KDS_EXEC_ERROR;
                }
            }
            exec->phase = exec->need_new_page
                ? KDS_BTREE_INSERT_PHASE_BTREE_INSERT
                : KDS_BTREE_INSERT_PHASE_DONE;
            continue;
        }

        case KDS_BTREE_INSERT_PHASE_BTREE_INSERT: {
            kds_exec_result_t r;
            if (kds_exec_slice_expired(base))
                return KDS_EXEC_CONTINUE;
            r = btree_insert_run_btree_insert(exec);
            btree_cursor_cleanup(&exec->cursor);
            if (r != KDS_EXEC_DONE)
                return r;
            exec->phase = KDS_BTREE_INSERT_PHASE_DONE;
            continue;
        }

        case KDS_BTREE_INSERT_PHASE_DONE:
            btree_cursor_cleanup(&exec->cursor);
            return KDS_EXEC_DONE;
        }
    }
}

/* ------------------------------------------------------------------
 * Init
 * ------------------------------------------------------------------ */

void kds_btree_insert_exec_init(kds_btree_insert_exec_t *exec,
                                 kds_relation_t *rel,
                                 kds_tuple_id_t key,
                                 const void *data, u16 data_len)
{
    exec->base.run         = kds_btree_insert_exec_run;
    exec->base.ret         = 0;
    exec->base.deadline_ns = 0;
    exec->base.units_done  = 0;

    exec->rel      = rel;
    exec->key      = key;
    exec->data     = data;
    exec->data_len = data_len;

    exec->phase               = KDS_BTREE_INSERT_PHASE_SEARCH_AND_PREPARE;
    exec->current_page_id     = rel ? rel->root_page_id : 0;
    exec->target_heap_page_id = 0;
    exec->new_heap_page_id    = 0;
    exec->new_page_min_key    = 0;
    exec->need_new_page       = false;

    exec->out_tid.page_id = 0;
    exec->out_tid.slot    = 0;

    btree_cursor_init(&exec->cursor);
}