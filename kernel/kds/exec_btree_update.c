/* exec_btree_update.c
 *
 * Btree-clustered table UPDATE executor -- the btree half of UpdateExec.
 * The heap half, the shared kds_update_exec_t init, and the per-page
 * apply helper (kds_update_apply_page / kds_update_finish) live in
 * exec_heap_update.c; this file only provides kds_btree_update_exec_run
 * and the btree leaf/bucket walk, which is the read-side walk from
 * exec_btree_select.c reused to drive updates instead of a scan.
 *
 * A btree-clustered table's rows live in heap "bucket" pages hung off
 * the btree leaves (see exec_btree_insert.c). The walk descends
 * root -> leftmost leaf, then for each leaf visits every non-empty
 * bucket page in slots[], applying the update to it, then follows the
 * leaf `next` sibling link. The SET assignments never touch the PK
 * (enforced at init), so an updated row stays in its own key-range
 * bucket -- no btree restructuring is needed.
 *
 * Depends on leaf sibling links being maintained (same known btree-layer
 * assumption noted in exec_btree_select.c).
 */

#include <linux/kds.h>
#include <linux/kds_executor.h>
#include <linux/kds_exec_update.h>
#include <linux/kds_relation.h>
#include <linux/kds_heap.h>
#include <linux/kds_btree.h>
#include <linux/kds_page_mgr.h>
#include <linux/err.h>
#include <linux/errno.h>

/* ------------------------------------------------------------------
 * Leaf bookkeeping (same shape as exec_btree_select.c)
 * ------------------------------------------------------------------ */

static void btree_update_cache_leaf(kds_update_exec_t *exec,
                                    const kds_btree_node_t *node)
{
    u32 key_count = (u32)node->key_count;
    u32 i;

    if (key_count > BTREE_MAX_KEYS)
        key_count = BTREE_MAX_KEYS;

    exec->btree.key_count         = key_count;
    exec->btree.leaf_next_page_id = node->next;

    for (i = 0; i <= key_count; i++)
        exec->btree.slot_ids[i] = node->slots[i];

    exec->btree.slot_idx    = 0;
    exec->cursor_page_id    = 0;   /* no active bucket page yet */
    exec->cursor_slot       = 0;
}

static kds_exec_result_t btree_update_descend_leftmost(kds_update_exec_t *exec)
{
    kds_page_id_t pid   = exec->cursor_page_id;   /* starts at root_page_id */
    int           guard = 0;

    for (;;) {
        kds_frame_t      *frame;
        kds_btree_node_t  node;

        if (!pid) {
            exec->btree.key_count         = 0;
            exec->btree.leaf_next_page_id = 0;
            exec->btree.slot_ids[0]       = 0;
            exec->btree.slot_idx          = 0;
            exec->cursor_page_id          = 0;
            exec->btree.descending        = false;
            return KDS_EXEC_DONE;
        }

        frame = kds_buf_lookup_or_load(pid);
        if (IS_ERR(frame)) {
            exec->base.ret = PTR_ERR(frame);
            return KDS_EXEC_ERROR;
        }

        load_btree_node(frame, &node);
        kds_buf_unpin(frame);

        if (node.level == 0) {
            btree_update_cache_leaf(exec, &node);
            exec->btree.descending = false;
            return KDS_EXEC_DONE;
        }

        pid = node.slots[0];

        if (++guard > BTREE_MAX_DEPTH) {
            exec->base.ret = -E2BIG;
            return KDS_EXEC_ERROR;
        }
    }
}

static kds_exec_result_t btree_update_load_leaf(kds_update_exec_t *exec,
                                                kds_page_id_t pid)
{
    kds_frame_t      *frame;
    kds_btree_node_t  node;

    frame = kds_buf_lookup_or_load(pid);
    if (IS_ERR(frame)) {
        exec->base.ret = PTR_ERR(frame);
        return KDS_EXEC_ERROR;
    }

    load_btree_node(frame, &node);
    kds_buf_unpin(frame);

    btree_update_cache_leaf(exec, &node);
    return KDS_EXEC_DONE;
}

/* ------------------------------------------------------------------
 * Phase: SCAN
 * ------------------------------------------------------------------ */

static kds_exec_result_t btree_update_run_scan(kds_update_exec_t *exec)
{
    if (exec->btree.descending) {
        kds_exec_result_t r = btree_update_descend_leftmost(exec);
        if (r != KDS_EXEC_DONE)
            return r;
        exec->base.units_done++;
        if (kds_exec_slice_expired(&exec->base))
            return KDS_EXEC_CONTINUE;
    }

    for (;;) {
        /* (1) Mid-bucket? Continue applying to it. */
        if (exec->cursor_page_id != 0) {
            kds_frame_t      *frame;
            kds_exec_result_t r;

            frame = kds_buf_lookup_or_load(exec->cursor_page_id);
            if (IS_ERR(frame)) {
                exec->base.ret = PTR_ERR(frame);
                return KDS_EXEC_ERROR;
            }

            if (exec->cursor_slot == 0) {
                exec->pages_visited++;
                exec->page_slot_limit = heap_nr_slots(frame);
            }

            r = kds_update_apply_page(exec, frame, &exec->cursor_slot,
                                      exec->page_slot_limit);
            kds_buf_unpin(frame);

            if (r == KDS_EXEC_CONTINUE)
                return KDS_EXEC_CONTINUE;
            if (r == KDS_EXEC_ERROR)
                return KDS_EXEC_ERROR;

            /* Bucket done -- advance to the next slot in this leaf. */
            exec->cursor_page_id = 0;
            exec->btree.slot_idx++;
        }

        /* (2) Next non-empty bucket in the current leaf. */
        {
            bool found = false;

            while (exec->btree.slot_idx <= exec->btree.key_count) {
                u32           si  = exec->btree.slot_idx;
                kds_page_id_t bid = exec->btree.slot_ids[si];

                if (bid == 0 ||
                    (si > 0 && bid == exec->btree.slot_ids[si - 1])) {
                    exec->btree.slot_idx++;
                    continue;
                }

                exec->cursor_page_id = bid;
                exec->cursor_slot    = 0;
                found = true;
                break;
            }

            if (found)
                continue;
        }

        /* (3) Leaf exhausted -- follow the right-sibling link. */
        if (exec->btree.leaf_next_page_id == 0)
            return KDS_EXEC_DONE;

        {
            kds_page_id_t     next_leaf = exec->btree.leaf_next_page_id;
            kds_exec_result_t r = btree_update_load_leaf(exec, next_leaf);

            if (r != KDS_EXEC_DONE)
                return r;

            exec->base.units_done++;
            if (kds_exec_slice_expired(&exec->base))
                return KDS_EXEC_CONTINUE;
        }
    }
}

/* ------------------------------------------------------------------
 * Run function (non-static: named by kds_update_exec_init())
 * ------------------------------------------------------------------ */

kds_exec_result_t
kds_btree_update_exec_run(kds_exec_state_t *base, u64 slice_ns)
{
    kds_update_exec_t *exec = container_of(base, kds_update_exec_t, base);

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

        case KDS_UPDATE_PHASE_SCAN: {
            kds_exec_result_t r = btree_update_run_scan(exec);
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
