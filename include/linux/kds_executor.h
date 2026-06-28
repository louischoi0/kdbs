#ifndef __KDS_EXECUTOR_H
#define __KDS_EXECUTOR_H

#include <linux/kds.h>
#include <linux/kds_relation.h>
#include <linux/kds_heap.h>

/*
 * C port of the Python POC's executor.py state-pattern design
 * (QueryExecState and its concrete subclasses like
 * BtreePageInsertTupleState). C has no inheritance, so the pattern
 * here is: every concrete exec embeds a kds_exec_state_t as its
 * first member, and casts back via container_of() inside its own
 * run() implementation. kds_exec_state_t.run is what callers
 * actually invoke; they never need to know which concrete exec type
 * they're holding beyond having called the matching *_init().
 *
 * v1 scope: kds_exec_state_t.run() is single-shot (does all its
 * work in one call and returns DONE/ERROR), unlike kds_proc_t's
 * run(proc, slice_ns), which is explicitly re-entrant/time-sliced.
 * Heap insert is fast enough that this doesn't need to yield
 * mid-operation yet -- if a future exec type needs to span multiple
 * scheduler slices (e.g. a multi-page btree rebuild), that's a
 * reason to add a slice_ns parameter and a CONTINUE result, not to
 * redesign this from scratch.
 */

typedef enum kds_exec_result {
    KDS_EXEC_DONE,
    KDS_EXEC_ERROR,
} kds_exec_result_t;

typedef struct kds_exec_state kds_exec_state_t;

typedef kds_exec_result_t (*kds_exec_run_fn)(kds_exec_state_t *state);

struct kds_exec_state {
    kds_exec_run_fn      run;
    int                  ret;     /* errno, meaningful when run() returned KDS_EXEC_ERROR */
};

static inline kds_exec_result_t kds_exec_run(kds_exec_state_t *state)
{
    return state->run(state);
}

/* ------------------------------------------------------------------
 * HeapPageInsertExec
 *
 * Inserts one tuple into rel's heap page chain. If the current tail
 * page is full, THIS exec -- not heap.c, not page_mgr.c -- is
 * responsible for allocating a new heap page (via kds_page_alloc(),
 * the pre-allocation ring) and linking it onto the chain
 * (heap_set_next_page_id()) before retrying the insert there.
 * Growing a table's storage is an executor-level policy decision,
 * not something the generic page-manipulation primitives below it
 * should know about.
 *
 * PRIMARY KEY CONSTRAINT: every table's first column is always its
 * primary key and always KDS_TYPE_INT64 -- a fixed, positional
 * convention enforced at CREATE TABLE time (see kds_cmd_create_table()
 * in dshell.c), not a per-column flag on kds_sys_column_t. Because
 * of that fixed layout, this exec can read the new row's PK straight
 * out of the first 8 bytes of `data` without needing to consult the
 * schema at all. Before attempting the insert, run() does a full
 * scan of the table's entire page chain checking for an existing
 * live tuple with the same PK; if found, returns KDS_EXEC_ERROR with
 * base.ret == -EEXIST and does not insert anything.
 *
 * KNOWN COST: that duplicate check is, unavoidably, a full table
 * scan on every single insert (no PK index exists yet -- see the
 * project's "lookup 없이 풀스캔" design discussion). This is
 * expected to get expensive as tables grow past a handful of pages;
 * a real PK index is the eventual fix, not attempted here.
 *
 * KNOWN INEFFICIENCY: there is no cached "tail page" anywhere
 * (kds_relation_t/sys.tables only know the chain's root page), so
 * the insert-after-dup-check walk also starts from the root every
 * time. Fine for short chains; if tables routinely grow to many
 * pages, this becomes an O(pages) cost per insert on top of the
 * duplicate-check scan above. Caching the tail page_id would need a
 * new field somewhere persistent (e.g. sys.tables or a separate
 * per-relation runtime cache) -- not added here to avoid changing a
 * catalog struct other code depends on without being asked to.
 * ------------------------------------------------------------------ */

typedef struct kds_heap_insert_exec {
    kds_exec_state_t     base;

    /* input, set by kds_heap_insert_exec_init() */
    kds_relation_t       *rel;
    const void           *data;
    u16                  data_len;
    u64                  xid;

    /* output, valid after a KDS_EXEC_DONE run() */
    kds_heap_tid_t       out_tid;
} kds_heap_insert_exec_t;

void kds_heap_insert_exec_init(kds_heap_insert_exec_t *exec, kds_relation_t *rel,
                                const void *data, u16 data_len, u64 xid);

#endif /* __KDS_EXECUTOR_H */