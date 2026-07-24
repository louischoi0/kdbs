#ifndef __KDS_EXECUTOR_H
#define __KDS_EXECUTOR_H

#include <linux/kds.h>
#include <linux/kds_relation.h>
#include <linux/kds_heap.h>
#include <linux/kds_btree.h>
#include <linux/kds_parser.h>
#include <linux/ktime.h>

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
 * v2 scope (supersedes the v1 single-shot design): run() is now
 * re-entrant and time-sliced, mirroring kds_proc_t's run(proc,
 * slice_ns) signature exactly -- on purpose, since the intended
 * caller of kds_exec_run() is a kds_proc_t's own run() callback
 * (see kds_dshell.c's dshell proc, or any future query-executing
 * proc), and a single shared time-budget convention between the two
 * layers means a proc can simply hand its own slice_ns straight
 * through without any unit conversion or re-scaling.
 *
 * Every concrete exec is responsible for checking its elapsed time
 * against the budget at reasonably fine granularity (e.g. once per
 * page visited, once per row scanned -- NOT once per whole table)
 * and returning KDS_EXEC_CONTINUE as soon as it's spent the budget,
 * having first saved enough state in itself (not on the stack) to
 * resume exactly where it left off on the next call. This is the
 * same "checkpoint progress into the struct, not into call-stack
 * locals" discipline kds_btree_cursor_t already uses for multi-page
 * btree walks -- run() must never assume it gets to keep its stack
 * frame between calls, because it doesn't.
 *
 * kds_exec_run() takes over budget-checking duties that used to be
 * entirely the caller's problem: it tracks wall-clock time itself
 * via ktime_get() so individual exec implementations only need to
 * compare against an absolute deadline already computed for them
 * (state->deadline_ns) rather than each reimplementing "how much
 * time do I have left" bookkeeping.
 */

typedef enum kds_exec_result {
    KDS_EXEC_DONE,
    KDS_EXEC_ERROR,
    /*
     * Slice budget exhausted before the work finished. All progress
     * has been checkpointed inside the concrete exec struct (NOT in
     * kds_exec_state_t itself, which has no room for per-exec
     * progress fields) -- the caller is expected to call
     * kds_exec_run() again, with a fresh slice_ns, to resume. The
     * caller must not free or otherwise invalidate the exec struct
     * between CONTINUE and the next run() call, since that's where
     * all the resume state lives.
     */
    KDS_EXEC_CONTINUE,
} kds_exec_result_t;

typedef struct kds_exec_state kds_exec_state_t;

typedef kds_exec_result_t (*kds_exec_run_fn)(kds_exec_state_t *state, u64 slice_ns);

struct kds_exec_state {
    kds_exec_run_fn      run;
    int                  ret;     /* errno, meaningful when run() returned KDS_EXEC_ERROR */

    /*
     * Absolute deadline for the *current* run() call, in ktime_get()
     * nanoseconds. Set fresh by kds_exec_run() on every call from
     * the slice_ns just handed to it -- a concrete exec's run()
     * checks ktime_get() against this rather than tracking its own
     * elapsed time, so the "have I used up my slice yet" check is
     * one comparison everywhere, not reimplemented per exec type.
     */
    u64                  deadline_ns;

    /*
     * Running count of "units of work" done across the exec's
     * entire lifetime (every CONTINUE and the final DONE) -- a unit
     * is whatever the concrete exec considers one checkpoint-able
     * step (one page visited, one row scanned, etc). Purely
     * informational: not used by kds_exec_run() itself, but every
     * concrete exec increments it at the same granularity it checks
     * the deadline at, so callers/debugging tools (e.g. a future
     * PSTA-style dshell command for in-flight execs) have a
     * consistent progress signal regardless of exec type.
     */
    u64                  units_done;
};

/*
 * Computes this call's deadline from slice_ns and dispatches to the
 * concrete exec's run(). Concrete run() implementations should call
 * kds_exec_slice_expired() (below) rather than reading
 * state->deadline_ns directly, in case the expiry check itself ever
 * needs to grow (e.g. a minimum-progress guarantee) without having
 * to touch every exec type.
 */
static inline kds_exec_result_t kds_exec_run(kds_exec_state_t *state, u64 slice_ns)
{
    state->deadline_ns = ktime_get_ns() + slice_ns;
    return state->run(state, slice_ns);
}

/*
 * True once the current run() call's slice budget has been spent.
 * Concrete execs call this at the top of each loop iteration (after
 * completing at least one unit of work -- never before doing any
 * work at all, or a slice_ns of 0 / an already-elapsed deadline
 * would make run() return CONTINUE without ever making progress,
 * which would spin forever one call at a time).
 */
static inline bool kds_exec_slice_expired(kds_exec_state_t *state)
{
    return ktime_get_ns() >= state->deadline_ns;
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
 * schema at all.
 *
 * PHASES (this is the state machine -- see kds_executor.c for the
 * implementation of each):
 *
 *   KDS_HEAP_INSERT_PHASE_DUP_SCAN
 *     Walking the chain from root_page_id checking for an existing
 *     live tuple with the same PK. Resumable: scan_page_id is the
 *     next page to load, picking up exactly where the last call
 *     left off. On finding a duplicate: KDS_EXEC_ERROR / -EEXIST.
 *     On reaching the end of chain with no match: advance to
 *     PHASE_FIND_TAIL.
 *
 *   KDS_HEAP_INSERT_PHASE_FIND_TAIL
 *     Walking the chain again (from root_page_id, since the dup
 *     scan didn't keep the frames pinned or remember which page had
 *     space) looking for a page with room, or the literal end of
 *     chain if none do. Resumable the same way as the scan phase.
 *     On success: advance to PHASE_DONE having performed the
 *     insert (allocating + linking a new tail page first if
 *     needed).
 *
 * This is a full table scan on every insert in the worst case (no
 * PK index exists yet), same known cost as the v1 design -- splitting
 * it into a resumable state machine changes *when* the CPU time is
 * spent (across multiple scheduler slices instead of one
 * uninterrupted burst) but not *how much total work* an insert
 * does. A real PK index remains the eventual fix for the total work
 * itself.
 * ------------------------------------------------------------------ */

typedef enum kds_heap_insert_phase {
    KDS_HEAP_INSERT_PHASE_DUP_SCAN = 0,
    KDS_HEAP_INSERT_PHASE_FIND_TAIL,
    /*
     * WAL_INSERT: append KDS_WAL_REC_INSERT record to the WAL ring
     * buffer before the data page is modified. The page_id and slot
     * are known at this point (FIND_TAIL has located the target page);
     * the WAL record is written first so crash recovery can re-apply
     * the modification if the data page flush is interrupted.
     */
    KDS_HEAP_INSERT_PHASE_WAL_INSERT,
    /*
     * WAL_INIT_PAGE: append KDS_WAL_REC_INIT_PAGE before a newly
     * allocated heap page is first written to. Only entered when
     * FIND_TAIL needed to grow the chain.
     */
    KDS_HEAP_INSERT_PHASE_WAL_INIT_PAGE,
    KDS_HEAP_INSERT_PHASE_DONE,
} kds_heap_insert_phase_t;

typedef struct kds_heap_insert_exec {
    kds_exec_state_t     base;

    /* input, set by kds_heap_insert_exec_init() */
    kds_relation_t       *rel;
    const void           *data;
    u16                  data_len;
    u64                  xid;

    /* output, valid after a KDS_EXEC_DONE run() */
    kds_heap_tid_t       out_tid;

    /* --------------------------------------------------------------
     * Resume state -- everything a CONTINUE needs to pick back up
     * exactly where the last run() call left off. Nothing about
     * this exec's progress is allowed to live anywhere else (not on
     * run()'s C stack, not in caller-local variables), since the
     * struct itself is the only thing guaranteed to survive between
     * one run() call and the next.
     * -------------------------------------------------------------- */
    kds_heap_insert_phase_t  phase;
    kds_page_id_t            cursor_page_id; /* next page to visit in the current phase */
    s64                      new_pk;          /* decoded once, reused by both phases */

    /*
     * WAL state: set by FIND_TAIL before transitioning to
     * WAL_INSERT / WAL_INIT_PAGE, consumed by those phases.
     */
    kds_page_id_t            wal_target_page_id;  /* page about to be modified    */
    u32                      wal_target_offset;   /* byte offset of new slot      */
    bool                     wal_need_init_page;  /* page was newly allocated     */
} kds_heap_insert_exec_t;

void kds_heap_insert_exec_init(kds_heap_insert_exec_t *exec, kds_relation_t *rel,
                                const void *data, u16 data_len, u64 xid);

/* ------------------------------------------------------------------
 * BtreeInsertExec
 *
 * Inserts one row into a btree-clustered table.
 *
 * PHASES (strictly WAL-before-data order):
 *
 *   SEARCH_AND_PREPARE
 *     Single btree descent that simultaneously:
 *       - finds the leaf slot (min_key <= key) and its heap page
 *       - loads the heap page to check for duplicate PK
 *       - checks whether the heap page has space (need_new_page)
 *     Resumable per btree level. Cursor holds pinned frames for
 *     later reuse in BTREE_INSERT.
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
 *
 *   Background checkpointer:
 *     WAL ring buffer → WAL pages on disk
 *     dirty page buffers → data pages on disk
 * ------------------------------------------------------------------ */

typedef enum kds_btree_insert_phase {
    KDS_BTREE_INSERT_PHASE_SEARCH_AND_PREPARE = 0,
    KDS_BTREE_INSERT_PHASE_WAL_INIT_PAGE,  /* WAL: new heap page init  */
    KDS_BTREE_INSERT_PHASE_WAL_INSERT,     /* WAL: row data            */
    KDS_BTREE_INSERT_PHASE_WAL_BTREE,      /* WAL: btree leaf slot     */
    KDS_BTREE_INSERT_PHASE_HEAP_INSERT,    /* data: write row to heap  */
    KDS_BTREE_INSERT_PHASE_BTREE_INSERT,   /* data: write btree slot   */
    KDS_BTREE_INSERT_PHASE_DONE,
} kds_btree_insert_phase_t;

typedef struct kds_btree_insert_exec {
    kds_exec_state_t         base;

    /* input */
    kds_relation_t           *rel;
    kds_tuple_id_t            key;           /* PK of the row being inserted */
    const void               *data;          /* encoded row payload           */
    u16                       data_len;

    /* resume state */
    kds_btree_insert_phase_t  phase;
    kds_btree_cursor_t        cursor;        /* owns pinned frames during SEARCH */
    kds_page_id_t             current_page_id;

    /*
     * Set by PHASE_SEARCH: the heap page_id where key should be
     * inserted (the value stored in the leaf slot whose min_key
     * is the largest key <= exec->key, or root_page_id if the
     * btree is empty / key is smaller than all existing keys).
     */
    kds_page_id_t             target_heap_page_id;

    /*
     * Set by PHASE_HEAP_INSERT when the target heap page was full
     * and a new one was allocated. PHASE_BTREE_INSERT uses this to
     * register the new page in the btree.
     */
    kds_page_id_t             new_heap_page_id;
    kds_tuple_id_t            new_page_min_key; /* first key in new heap page */

    /*
     * Set by PREPARE: true when the target heap page has no room and
     * a new page will need to be allocated. Drives conditional WAL
     * phases (WAL_INIT_PAGE, WAL_BTREE, BTREE_INSERT).
     */
    bool                      need_new_page;

    /* output */
    kds_heap_tid_t            out_tid;
} kds_btree_insert_exec_t;

void kds_btree_insert_exec_init(kds_btree_insert_exec_t *exec, kds_relation_t *rel,
                                 kds_tuple_id_t key,
                                 const void *data, u16 data_len);

/* ------------------------------------------------------------------
 * SelectExec
 *
 * Scans a table -- heap-clustered (a chain of heap pages) or
 * btree-clustered (a leaf-level chain of range-bucketed heap pages,
 * per exec_btree_insert.c's SEARCH_AND_PREPARE phase: each btree leaf
 * key marks the start of a heap "bucket" page holding every row in
 * that key range) -- applying WHERE conditions, resumable and
 * time-sliced exactly like the insert execs above.
 *
 * Unlike insert, which dispatches to two free-standing struct types
 * by rel->kind at the call site, select's heap/btree resume state is
 * unioned inside ONE struct tagged implicitly by rel->kind -- callers
 * (dshell.c) only ever hold a single kds_select_exec_t regardless of
 * a table's clustering, the same way kds_relation_t already unifies
 * heap/btree access behind one handle.
 *
 * Output is a difference from the insert execs: rather than leaving
 * response formatting to the caller after DONE, this exec streams
 * matched rows directly into the caller-owned out_buf/out_size
 * (dshell.c hands it client->resp_buf) as they're found, because the
 * row count is unbounded and truncation has to be decided live
 * against the buffer's remaining space. out_pos is checkpointed in
 * the struct like every other piece of progress, so a CONTINUE
 * resumes appending exactly where the last call left off.
 * ------------------------------------------------------------------ */

typedef enum kds_select_phase {
    KDS_SELECT_PHASE_SCAN = 0,
    KDS_SELECT_PHASE_DONE,
} kds_select_phase_t;

/*
 * A WHERE condition with its column name already resolved to a
 * schema index, so the hot scan path never re-does a string lookup
 * per row. Resolved once, synchronously, in kds_select_exec_init().
 */
typedef struct kds_select_resolved_cond {
    u32            col_idx;
    kds_cond_op_t  op;
    kds_ast_val_t  val;
} kds_select_resolved_cond_t;

typedef struct kds_select_exec {
    kds_exec_state_t            base;

    /* input, set by kds_select_exec_init() */
    kds_relation_t               *rel;
    kds_select_resolved_cond_t    conds[KDS_PARSER_MAX_CONDS];
    u32                           nr_conds;

    /* output: streamed directly into caller-owned text buffer */
    char                         *out_buf;
    size_t                        out_size;
    size_t                        out_pos;
    u32                           rows_matched;
    u32                           pages_visited;
    bool                          truncated;

    /* --------------------------------------------------------------
     * Resume state -- see kds_heap_insert_exec_t's comment on why
     * none of this may live on run()'s C stack.
     * -------------------------------------------------------------- */
    kds_select_phase_t            phase;

    /*
     * Planner flag: when set, a WHERE equality on an indexed column was
     * resolved to a single row-page via kds_index_search(), so the scan
     * visits only cursor_page_id (never following heap `next` links or
     * walking btree leaves) and stops. cursor_page_id == 0 in this mode
     * means the index proved no row matches -- scan nothing. All WHERE
     * conditions are still applied per row on that one page, so extra
     * AND-conditions remain correct. Set in kds_select_exec_init().
     */
    bool                          index_single_page;

    /*
     * The heap page currently being scanned row-by-row: the chain
     * page itself for a heap-clustered table, or the btree leaf's
     * current bucket page for a btree-clustered one. cursor_slot is
     * the next slot to read within it.
     */
    kds_page_id_t                 cursor_page_id;
    u16                           cursor_slot;

    /*
     * btree-only resume state (unused when rel->kind == KDS_CLUSTERED_HEAP).
     * No kds_btree_cursor_t here on purpose -- a read-only scan never
     * needs to keep a whole root-to-leaf path pinned for a later
     * write like the insert exec's cursor does; it only needs the
     * *current* leaf's slot ids and its right sibling to keep walking.
     */
    struct {
        bool           descending;              /* still walking root -> leftmost leaf */
        u32            slot_idx;                /* next slot in the current leaf */
        u32            key_count;                /* current leaf's key_count (cached) */
        kds_page_id_t  slot_ids[BTREE_MAX_KEYS + 1]; /* current leaf's heap bucket ids */
        kds_page_id_t  leaf_next_page_id;        /* current leaf's right sibling, 0 = last leaf */
    } btree;
} kds_select_exec_t;

/*
 * Resolves conds against rel->schema and initializes the exec.
 * Returns 0 on success, or fails synchronously (before any run() call
 * -- no I/O needed for this step) with:
 *   -ENOENT     a WHERE column name isn't in rel->schema
 *   -EOPNOTSUPP an ordering op (<, <=, >, >=) was used on a column
 *               type with no compare_fn (kds_types.h) -- currently
 *               float/decimal/bool/varchar/char. EQ/NEQ work on every
 *               type via raw encoded-byte comparison and don't need
 *               compare_fn.
 */
int kds_select_exec_init(kds_select_exec_t *exec, kds_relation_t *rel,
                          const kds_ast_cond_t *conds, u32 nr_conds,
                          char *out_buf, size_t out_size);

/* ------------------------------------------------------------------
 * UpdateExec
 *
 * Applies SET assignments to every row matching the WHERE clause of an
 * UPDATE statement. Structured exactly like SelectExec: one unified
 * kds_update_exec_t tagged by rel->kind, dispatched to a heap or btree
 * run() by kds_update_exec_init(); the two scan strategies live in
 * exec_heap_update.c / exec_btree_update.c and share the per-page
 * apply helper (kds_exec_update.h). It is resumable and time-sliced the
 * same way, and reuses SelectExec's resolved-condition form for WHERE.
 *
 * The actual per-row modification goes through kds_heap_update_tuple()
 * (kds_undo.h): prior version copied to an undo entry, new value either
 * overwritten in place or retired+reinserted, and the change WAL-logged
 * and synchronously flushed. To avoid re-updating a row that the
 * retire+insert path just appended at a higher slot, each page is
 * scanned only up to the slot count it had when first entered
 * (page_slot_limit).
 *
 * PRIMARY KEY: assigning to column 0 (the fixed INT64 PK) is rejected
 * by kds_update_exec_init() with -EPERM -- the PK positional convention
 * the whole engine relies on must not be mutated by an UPDATE.
 *
 * Output: unlike SelectExec, no per-row streaming -- only a final
 * summary line ("OK N row(s) updated ...") written by kds_update_finish().
 * ------------------------------------------------------------------ */

typedef enum kds_update_phase {
    KDS_UPDATE_PHASE_SCAN = 0,
    KDS_UPDATE_PHASE_DONE,
} kds_update_phase_t;

/* A SET assignment with its column name resolved to a schema index. */
typedef struct kds_update_resolved_assign {
    u32            col_idx;
    kds_ast_val_t  val;
} kds_update_resolved_assign_t;

typedef struct kds_update_exec {
    kds_exec_state_t              base;

    /* input, set by kds_update_exec_init() */
    kds_relation_t               *rel;
    u64                           xid;
    kd_oid_t                      owner_oid;    /* rel->oid, stamped into undo */
    kds_update_resolved_assign_t  assigns[KDS_SCHEMA_MAX_COLUMNS];
    u32                           nr_assigns;
    kds_select_resolved_cond_t    conds[KDS_PARSER_MAX_CONDS];
    u32                           nr_conds;

    /* output: only the final summary is written here (in finish) */
    char                         *out_buf;
    size_t                        out_size;
    u32                           rows_matched;
    u32                           rows_updated;
    u32                           pages_visited;

    /* --------------------------------------------------------------
     * Resume state -- see kds_heap_insert_exec_t's comment on why none
     * of this may live on run()'s C stack.
     * -------------------------------------------------------------- */
    kds_update_phase_t            phase;
    kds_page_id_t                 cursor_page_id;
    u16                           cursor_slot;
    u16                           page_slot_limit;  /* nr_slots snapshot at page entry */

    /* btree-only resume state (unused for KDS_CLUSTERED_HEAP), same
     * shape as SelectExec's leaf/bucket walk. */
    struct {
        bool           descending;
        u32            slot_idx;
        u32            key_count;
        kds_page_id_t  slot_ids[BTREE_MAX_KEYS + 1];
        kds_page_id_t  leaf_next_page_id;
    } btree;
} kds_update_exec_t;

/*
 * Resolves assignments and WHERE conditions against rel->schema and
 * initializes the exec. Synchronous (no I/O); fails before any run()
 * with:
 *   -ENOENT      a SET or WHERE column name isn't in rel->schema
 *   -EPERM       a SET targets column 0 (the primary key)
 *   -EOPNOTSUPP  an ordering op on a WHERE column type with no compare_fn
 *   -EINVAL      no assignments, or too many
 */
int kds_update_exec_init(kds_update_exec_t *exec, kds_relation_t *rel,
                          const kds_ast_assign_t *assigns, u32 nr_assigns,
                          const kds_ast_cond_t *conds, u32 nr_conds,
                          u64 xid, char *out_buf, size_t out_size);

#endif /* __KDS_EXECUTOR_H */