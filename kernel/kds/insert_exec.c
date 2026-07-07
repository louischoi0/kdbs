#include <linux/kds.h>
#include <linux/kds_executor.h>
#include <linux/kds_relation.h>
#include <linux/kds_heap.h>
#include <linux/kds_btree.h>
#include <linux/kds_page_mgr.h>
#include <linux/kds_page_alloc.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/ktime.h>

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
 * Checks a single page for a live tuple matching new_pk.
 *
 * Returns 1 if found, 0 if not found on this page, or a negative
 * errno if the page's tuples couldn't be read. *out_next is set to
 * this page's next_page_id on a 0 or 1 return (irrelevant, but left
 * untouched, on a negative return) -- callers use it to advance
 * cursor_page_id regardless of which phase is calling this.
 */
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
            continue; /* dead slot */
        if (r) {
            ret = r;
            goto out;
        }

        if (hdr.data_len < sizeof(existing_pk))
            continue; /* malformed/too-short row -- not a PK match either way */

        memcpy(&existing_pk, buf, sizeof(existing_pk));
        if (existing_pk == target_pk) {
            ret = 1;
            goto out;
        }
    }

    ret = 0;

out:
    if (ret >= 0)
        *out_next = heap_get_next_page_id(frame);
    kds_buf_unpin(frame);
    return ret;
}

/*
 * Phase 1: walks the chain from exec->cursor_page_id looking for a
 * duplicate PK, one page per loop iteration, checking the slice
 * deadline after each page (not after each row -- a single page's
 * worth of row reads is small, bounded, fast work that isn't worth
 * interrupting mid-page).
 *
 * Returns one of:
 *   KDS_EXEC_DONE     phase finished clean (no duplicate found) --
 *                     caller advances to the next phase itself.
 *   KDS_EXEC_ERROR     duplicate found (base->ret = -EEXIST) or a
 *                     real I/O/lookup error.
 *   KDS_EXEC_CONTINUE  slice budget spent; exec->cursor_page_id has
 *                     been updated so the next call resumes on the
 *                     correct page.
 */
static kds_exec_result_t kds_heap_insert_run_dup_scan(kds_heap_insert_exec_t *exec)
{
    while (exec->cursor_page_id != 0) {
        kds_page_id_t next = 0;
        int ret;

        ret = kds_heap_scan_page_for_pk(exec->cursor_page_id, exec->new_pk, &next);
        if (ret < 0) {
            exec->base.ret = ret;
            return KDS_EXEC_ERROR;
        }
        if (ret == 1) {
            exec->base.ret = -EEXIST;
            return KDS_EXEC_ERROR;
        }

        exec->base.units_done++;
        exec->cursor_page_id = next;

        if (exec->cursor_page_id != 0 && kds_exec_slice_expired(&exec->base))
            return KDS_EXEC_CONTINUE;
    }

    return KDS_EXEC_DONE;
}

/*
 * Phase 2: walks the chain again from the root looking for a page
 * with room for the new tuple, growing the chain by one page if it
 * reaches the end still full. Same per-page deadline-check
 * granularity as the dup-scan phase.
 *
 * Returns DONE (exec->out_tid is valid), ERROR (base->ret set), or
 * CONTINUE (exec->cursor_page_id updated for resume).
 */
static kds_exec_result_t kds_heap_insert_run_find_tail(kds_heap_insert_exec_t *exec)
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

        ret = heap_insert_tuple(frame, exec->data, exec->data_len, exec->xid,
                                 &exec->out_tid);
        if (!ret) {
            kds_buf_unpin(frame);
            exec->base.units_done++;
            return KDS_EXEC_DONE;
        }

        if (ret != -ENOSPC) {
            /* A real error (bad arguments, oversized payload, etc.)
             * -- not the "page is full" condition this exec knows
             * how to grow past. */
            kds_buf_unpin(frame);
            exec->base.ret = ret;
            return KDS_EXEC_ERROR;
        }

        /* Page is full. If the chain already continues past this
         * page, just walk to the next one -- no allocation needed,
         * someone else's insert already grew the chain this far. */
        next = heap_get_next_page_id(frame);
        if (next != 0) {
            kds_buf_unpin(frame);
            exec->base.units_done++;
            exec->cursor_page_id = next;

            if (kds_exec_slice_expired(&exec->base))
                return KDS_EXEC_CONTINUE;

            continue;
        }

        /*
         * End of chain and full: this is the actual growth point.
         * kds_page_alloc() (the pre-allocation ring, kds_page_alloc.c)
         * is used here rather than kds_buf_alloc_new() directly --
         * same convention kds_catalog_create_table() already
         * follows for a brand-new heap-clustered table's first page.
         *
         * Deliberately NOT slice-checked before this block: growing
         * the chain and performing the insert into the brand-new
         * page is treated as a single atomic step from the resume
         * state's point of view -- there is no intermediate state
         * between "decided to grow" and "inserted into the new
         * page" that would be safe/meaningful to checkpoint
         * (a linked-but-empty tail page with no insert yet isn't a
         * resumable phase of its own, it's just this step
         * in-progress).
         */
        {
            kds_frame_t *new_frame = kds_page_alloc(KDS_PAGE_TYPE_HEAP);

            if (!new_frame) {
                kds_buf_unpin(frame);
                exec->base.ret = -ENOSPC; /* pre-allocation ring empty */
                return KDS_EXEC_ERROR;
            }

            heap_init_page(new_frame);

            ret = heap_set_next_page_id(frame, new_frame->kp->id);
            kds_buf_unpin(frame); /* done with the old tail either way */

            if (ret) {
                kds_buf_unpin(new_frame);
                exec->base.ret = ret;
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
                exec->base.ret = ret;
                return KDS_EXEC_ERROR;
            }

            exec->base.units_done++;
            return KDS_EXEC_DONE;
        }
    }
}

static kds_exec_result_t kds_heap_insert_exec_run(kds_exec_state_t *base, u64 slice_ns)
{
    kds_heap_insert_exec_t *exec = container_of(base, kds_heap_insert_exec_t, base);

    if (!exec->rel || exec->rel->kind != KDS_CLUSTERED_HEAP) {
        base->ret = -EINVAL;
        return KDS_EXEC_ERROR;
    }

    if (exec->data_len < sizeof(exec->new_pk)) {
        /* Every row must carry at least the PK column's 8 bytes --
         * this should already be guaranteed by CREATE TABLE's
         * first-column-is-int64 rule, but a caller bug elsewhere
         * shouldn't be able to slip a too-short row past this.
         * Checked here rather than in init() because init() has no
         * error return path; this only runs once since phase stays
         * DUP_SCAN until this check passes (failing here returns
         * before touching exec->phase or exec->cursor_page_id, so a
         * caller that -- incorrectly -- retried after ERROR would
         * just hit this same check again, not resume mid-scan). */
        base->ret = -EINVAL;
        return KDS_EXEC_ERROR;
    }

    for (;;) {
        switch (exec->phase) {
        case KDS_HEAP_INSERT_PHASE_DUP_SCAN: {
            kds_exec_result_t r = kds_heap_insert_run_dup_scan(exec);

            if (r != KDS_EXEC_DONE)
                return r; /* ERROR or CONTINUE -- pass straight through */

            exec->phase = KDS_HEAP_INSERT_PHASE_FIND_TAIL;
            exec->cursor_page_id = exec->rel->root_page_id;
            continue; /* re-check the (now-expired or not) slice before phase 2 */
        }

        case KDS_HEAP_INSERT_PHASE_FIND_TAIL: {
            kds_exec_result_t r;

            if (kds_exec_slice_expired(base))
                return KDS_EXEC_CONTINUE;

            r = kds_heap_insert_run_find_tail(exec);
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

void kds_heap_insert_exec_init(kds_heap_insert_exec_t *exec, kds_relation_t *rel,
                                const void *data, u16 data_len, u64 xid)
{
    exec->base.run = kds_heap_insert_exec_run;
    exec->base.ret = 0;
    exec->base.deadline_ns = 0;
    exec->base.units_done = 0;

    exec->rel = rel;
    exec->data = data;
    exec->data_len = data_len;
    exec->xid = xid;

    exec->out_tid.page_id = 0;
    exec->out_tid.slot = 0;

    exec->phase = KDS_HEAP_INSERT_PHASE_DUP_SCAN;
    exec->cursor_page_id = rel ? rel->root_page_id : 0;

    /*
     * data_len < sizeof(s64) is a caller bug (every row must carry
     * at least the PK column's 8 bytes -- guaranteed by CREATE
     * TABLE's first-column-is-int64 rule), but init() has no
     * KDS_EXEC_ERROR return path to report it through, so the
     * actual rejection happens on the first run() call instead
     * (mirrors the v1 design's check, just moved). new_pk is left
     * as 0 here and only decoded lazily on first run() if data_len
     * passes that check, to avoid reading out-of-bounds memory in
     * init() itself when data_len is too short.
     */
    if (data && data_len >= sizeof(exec->new_pk))
        memcpy(&exec->new_pk, data, sizeof(exec->new_pk));
    else
        exec->new_pk = 0;
}

/* ------------------------------------------------------------------
 * BtreeInsertExec implementation
 *
 * Clustered btree layout:
 *   btree leaf slots: (min_key, heap_page_id)
 *   Each slot points to a heap page holding rows for
 *   keys in [min_key, next_slot.min_key).
 *
 * Insert algorithm:
 *   SEARCH       → find leaf slot whose min_key <= key
 *   HEAP_INSERT  → insert row into that heap page;
 *                  if full, alloc new heap page, insert there
 *   BTREE_INSERT → (only if new heap page allocated)
 *                  register (key, new_page_id) in btree
 * ------------------------------------------------------------------ */

/*
 * XID placeholder used by btree insert -- same convention as heap.
 * Replace with real transaction manager output when available.
 */
#define KDS_BTREE_INSERT_XID  1

/*
 * PHASE_SEARCH: descends the btree one level per iteration.
 * On reaching the leaf sets exec->target_heap_page_id to the heap
 * page that covers exec->key (largest min_key <= key).
 */
static kds_exec_result_t kds_btree_insert_run_search(kds_btree_insert_exec_t *exec)
{
    while (exec->cursor.depth < BTREE_MAX_DEPTH) {
        kds_frame_t      *frame;
        kds_btree_node_t *node;
        int               pos, i;

        frame = kds_buf_lookup_or_load(exec->current_page_id);
        if (IS_ERR(frame)) {
            exec->base.ret = PTR_ERR(frame);
            btree_cursor_cleanup(&exec->cursor);
            return KDS_EXEC_ERROR;
        }

        node = &exec->cursor.nodes[exec->cursor.depth];
        load_btree_node(frame, node);

        pos = node->key_count;
        for (i = 0; i < node->key_count; i++) {
            if (exec->key <= node->keys[i]) {
                pos = i;
                break;
            }
        }
        exec->cursor.positions[exec->cursor.depth] = pos;

        /* Duplicate key check. */
        if (pos < node->key_count && node->keys[pos] == exec->key) {
            exec->base.ret = -EEXIST;
            btree_cursor_cleanup(&exec->cursor);
            return KDS_EXEC_ERROR;
        }

        exec->base.units_done++;

        if (node->level == 0) {
            /*
             * Leaf reached. slots[pos] is the heap page for exec->key.
             * pos == 0 and key < all existing keys: use slots[0].
             * Fall back to root_page_id if the slot is 0 (empty tree).
             */
            exec->target_heap_page_id = node->slots[pos];
            if (!exec->target_heap_page_id)
                exec->target_heap_page_id = exec->rel->root_page_id;
            return KDS_EXEC_DONE;
        }

        exec->current_page_id = node->slots[pos];
        exec->cursor.depth++;

        if (kds_exec_slice_expired(&exec->base))
            return KDS_EXEC_CONTINUE;
    }

    exec->base.ret = -E2BIG;
    btree_cursor_cleanup(&exec->cursor);
    return KDS_EXEC_ERROR;
}

/*
 * PHASE_HEAP_INSERT: insert exec->data into exec->target_heap_page_id.
 * If the page is full, allocate a new heap page and record it in
 * exec->new_heap_page_id for PHASE_BTREE_INSERT to register.
 */
static kds_exec_result_t kds_btree_insert_run_heap_insert(kds_btree_insert_exec_t *exec)
{
    kds_frame_t *frame;
    int          ret;

    frame = kds_buf_lookup_or_load(exec->target_heap_page_id);
    if (IS_ERR(frame)) {
        exec->base.ret = PTR_ERR(frame);
        return KDS_EXEC_ERROR;
    }

    ret = heap_insert_tuple(frame, exec->data, exec->data_len,
                             KDS_BTREE_INSERT_XID, &exec->out_tid);
    if (!ret) {
        /* Row fit in the existing heap page -- no btree update needed. */
        kds_buf_unpin(frame);
        exec->base.units_done++;
        exec->new_heap_page_id = 0;
        return KDS_EXEC_DONE;
    }

    kds_buf_unpin(frame);

    if (ret != -ENOSPC) {
        exec->base.ret = ret;
        return KDS_EXEC_ERROR;
    }

    /*
     * Target heap page full: allocate a new one, insert there, and
     * record it so PHASE_BTREE_INSERT can register it in the btree
     * with min_key = exec->key.
     */
    {
        kds_frame_t *new_frame = kds_page_alloc(KDS_PAGE_TYPE_HEAP);

        if (!new_frame) {
            exec->base.ret = -ENOSPC;
            return KDS_EXEC_ERROR;
        }

        heap_init_page(new_frame);

        ret = heap_insert_tuple(new_frame, exec->data, exec->data_len,
                                 KDS_BTREE_INSERT_XID, &exec->out_tid);
        if (ret) {
            kds_buf_unpin(new_frame);
            exec->base.ret = ret;
            return KDS_EXEC_ERROR;
        }

        exec->new_heap_page_id = new_frame->kp->id;
        exec->new_page_min_key = exec->key;
        exec->base.units_done++;

        kds_buf_unpin(new_frame);
        return KDS_EXEC_DONE;
    }
}

/*
 * PHASE_BTREE_INSERT: register the new heap page in the btree.
 * Uses the cursor populated by PHASE_SEARCH via kds_btree_cursor_insert(),
 * which owns and releases the cursor's pinned frames.
 */
static kds_exec_result_t kds_btree_insert_run_btree_insert(kds_btree_insert_exec_t *exec)
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

static kds_exec_result_t kds_btree_insert_exec_run(kds_exec_state_t *base, u64 slice_ns)
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

        case KDS_BTREE_INSERT_PHASE_SEARCH: {
            kds_exec_result_t r = kds_btree_insert_run_search(exec);
            if (r != KDS_EXEC_DONE)
                return r;
            exec->phase = KDS_BTREE_INSERT_PHASE_HEAP_INSERT;
            continue;
        }

        case KDS_BTREE_INSERT_PHASE_HEAP_INSERT: {
            kds_exec_result_t r;
            if (kds_exec_slice_expired(base))
                return KDS_EXEC_CONTINUE;
            r = kds_btree_insert_run_heap_insert(exec);
            if (r != KDS_EXEC_DONE)
                return r;
            exec->phase = exec->new_heap_page_id
                ? KDS_BTREE_INSERT_PHASE_BTREE_INSERT
                : KDS_BTREE_INSERT_PHASE_DONE;
            continue;
        }

        case KDS_BTREE_INSERT_PHASE_BTREE_INSERT: {
            kds_exec_result_t r;
            if (kds_exec_slice_expired(base))
                return KDS_EXEC_CONTINUE;
            r = kds_btree_insert_run_btree_insert(exec);
            /* kds_btree_cursor_insert() released cursor frames. */
            btree_cursor_cleanup(&exec->cursor);
            if (r != KDS_EXEC_DONE)
                return r;
            exec->phase = KDS_BTREE_INSERT_PHASE_DONE;
            continue;
        }

        case KDS_BTREE_INSERT_PHASE_DONE:
            /*
             * Fast path (no new heap page): cursor frames are still
             * pinned from SEARCH. Release them now.
             */
            btree_cursor_cleanup(&exec->cursor);
            return KDS_EXEC_DONE;
        }
    }
}

void kds_btree_insert_exec_init(kds_btree_insert_exec_t *exec, kds_relation_t *rel,
                                 kds_tuple_id_t key,
                                 const void *data, u16 data_len)
{
    exec->base.run         = kds_btree_insert_exec_run;
    exec->base.ret         = 0;
    exec->base.deadline_ns = 0;
    exec->base.units_done  = 0;

    exec->rel              = rel;
    exec->key              = key;
    exec->data             = data;
    exec->data_len         = data_len;

    exec->phase               = KDS_BTREE_INSERT_PHASE_SEARCH;
    exec->current_page_id     = rel ? rel->root_page_id : 0;
    exec->target_heap_page_id = 0;
    exec->new_heap_page_id    = 0;
    exec->new_page_min_key    = 0;

    exec->out_tid.page_id = 0;
    exec->out_tid.slot    = 0;

    btree_cursor_init(&exec->cursor);
}