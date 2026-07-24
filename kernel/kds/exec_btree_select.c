/* exec_btree_select.c
 *
 * Btree-clustered table select (scan) executor -- the btree half of the
 * SelectExec described in kds_executor.h. The heap half, the shared
 * kds_select_exec_t struct's init, and the per-page row primitives
 * (kds_select_scan_page / kds_select_finish) all live in
 * exec_heap_select.c; this file provides only kds_btree_select_exec_run
 * and the btree-specific leaf walk. The two backends are never merged.
 *
 * Storage model (see exec_btree_insert.c's SEARCH_AND_PREPARE phase):
 * a btree-clustered table's rows live in heap "bucket" pages hung off
 * the btree's leaf level. Each leaf node holds up to BTREE_MAX_KEYS+1
 * bucket page ids in slots[]; leaf `next` links right-sibling leaves
 * into a chain. A full scan therefore:
 *
 *   1. descends root -> leftmost leaf (following slots[0] at each
 *      internal level),
 *   2. for each leaf, scans every non-empty bucket page in slots[],
 *   3. follows the leaf `next` link to the right sibling and repeats,
 *      until a leaf with next == 0.
 *
 * All progress is checkpointed in exec (the btree sub-struct plus the
 * shared cursor_page_id/cursor_slot for the bucket page currently being
 * scanned row-by-row), so a KDS_EXEC_CONTINUE resumes exactly where it
 * left off -- no kds_btree_cursor_t / pinned root-to-leaf path is kept,
 * since a read-only scan only ever needs the current leaf's slots and
 * its right sibling.
 *
 * DEPENDS ON leaf sibling links: this walk relies on each leaf's `next`
 * pointing at its right sibling (kds_btree_page_data_t.next). This is
 * the read-side counterpart of the btree insert/split maintaining those
 * links; if a split ever fails to thread `next`, this scan will stop
 * early at that leaf. Treat that as the same class of known btree-layer
 * gap flagged in kds_relation.h (root-split staleness) rather than a
 * defect in this file.
 */

#include <linux/kds.h>
#include <linux/kds_executor.h>
#include <linux/kds_exec_select.h>
#include <linux/kds_relation.h>
#include <linux/kds_heap.h>
#include <linux/kds_btree.h>
#include <linux/kds_page_mgr.h>
#include <linux/err.h>
#include <linux/errno.h>

/* ------------------------------------------------------------------
 * Leaf bookkeeping
 * ------------------------------------------------------------------ */

/*
 * Caches a just-loaded leaf node's bucket slots and right-sibling link
 * into the exec's btree resume state, and resets the per-leaf slot
 * cursor. `node` is a plain copy (its frame may already be unpinned).
 */
static void btree_select_cache_leaf(kds_select_exec_t *exec,
                                    const kds_btree_node_t *node)
{
    u32 key_count = (u32)node->key_count;
    u32 i;

    if (key_count > BTREE_MAX_KEYS)
        key_count = BTREE_MAX_KEYS;   /* defensive: never index past slots[] */

    exec->btree.key_count         = key_count;
    exec->btree.leaf_next_page_id = node->next;

    /* A leaf with key_count keys has key_count+1 bucket slots. */
    for (i = 0; i <= key_count; i++)
        exec->btree.slot_ids[i] = node->slots[i];

    exec->btree.slot_idx = 0;
    exec->cursor_page_id = 0;   /* no active bucket page yet */
    exec->cursor_slot    = 0;
}

/*
 * Walks root -> leftmost leaf, following slots[0] at each internal
 * level, and caches the leaf. Bounded by BTREE_MAX_DEPTH page loads, so
 * it runs to completion in one call (the caller checks the slice budget
 * afterwards). On return exec->btree.descending is cleared.
 */
static kds_exec_result_t btree_select_descend_leftmost(kds_select_exec_t *exec)
{
    kds_page_id_t pid   = exec->cursor_page_id;   /* starts at root_page_id */
    int           guard = 0;

    for (;;) {
        kds_frame_t      *frame;
        kds_btree_node_t  node;

        if (!pid) {
            /* Empty tree: nothing to scan. Present as an empty leaf. */
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
            btree_select_cache_leaf(exec, &node);
            exec->btree.descending = false;
            return KDS_EXEC_DONE;
        }

        /* Internal node -- descend to the leftmost child. */
        pid = node.slots[0];

        if (++guard > BTREE_MAX_DEPTH) {
            exec->base.ret = -E2BIG;   /* cycle or corrupt tree */
            return KDS_EXEC_ERROR;
        }
    }
}

/*
 * Loads the given right-sibling leaf and caches it. Unlike the initial
 * descent, no re-descent is needed -- leaf siblings are chained directly
 * via `next`.
 */
static kds_exec_result_t btree_select_load_leaf(kds_select_exec_t *exec,
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

    btree_select_cache_leaf(exec, &node);
    return KDS_EXEC_DONE;
}

/* ------------------------------------------------------------------
 * Phase: SCAN
 * ------------------------------------------------------------------ */

static kds_exec_result_t btree_select_run_scan(kds_select_exec_t *exec)
{
    /* One-time descent to the leftmost leaf. */
    if (exec->btree.descending) {
        kds_exec_result_t r = btree_select_descend_leftmost(exec);
        if (r != KDS_EXEC_DONE)
            return r;
        exec->base.units_done++;
        if (kds_exec_slice_expired(&exec->base))
            return KDS_EXEC_CONTINUE;
    }

    for (;;) {
        /* (1) Scanning a bucket page? Continue it row-by-row. */
        if (exec->cursor_page_id != 0) {
            kds_frame_t      *frame;
            kds_exec_result_t r;

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
            if (exec->truncated)
                return KDS_EXEC_DONE;

            /* Bucket exhausted -- move to the next slot in this leaf. */
            exec->cursor_page_id = 0;
            exec->btree.slot_idx++;
        }

        /* (2) Advance to the next non-empty bucket in the current leaf. */
        {
            bool found = false;

            while (exec->btree.slot_idx <= exec->btree.key_count) {
                u32           si  = exec->btree.slot_idx;
                kds_page_id_t bid = exec->btree.slot_ids[si];

                /* Skip empty slots and any slot that repeats the
                 * previous bucket id (defensive against an insert-side
                 * fallback pointing two ranges at one page). */
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
                continue;   /* go scan the bucket at (1) */
        }

        /* (3) Leaf exhausted -- follow the right-sibling link. */
        if (exec->btree.leaf_next_page_id == 0)
            return KDS_EXEC_DONE;

        {
            kds_page_id_t     next_leaf = exec->btree.leaf_next_page_id;
            kds_exec_result_t r = btree_select_load_leaf(exec, next_leaf);

            if (r != KDS_EXEC_DONE)
                return r;

            exec->base.units_done++;
            if (kds_exec_slice_expired(&exec->base))
                return KDS_EXEC_CONTINUE;
        }
    }
}

/* ------------------------------------------------------------------
 * Run function (non-static: named by kds_select_exec_init() in
 * exec_heap_select.c as the btree dispatch target)
 * ------------------------------------------------------------------ */

kds_exec_result_t
kds_btree_select_exec_run(kds_exec_state_t *base, u64 slice_ns)
{
    kds_select_exec_t *exec = container_of(base, kds_select_exec_t, base);

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

        case KDS_SELECT_PHASE_SCAN: {
            kds_exec_result_t r = btree_select_run_scan(exec);
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
