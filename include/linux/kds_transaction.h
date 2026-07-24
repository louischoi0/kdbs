#ifndef __KDS_TRANSACTION_H
#define __KDS_TRANSACTION_H

#include <linux/kds.h>
#include <linux/kds_wal.h>
#include <linux/list.h>

/*
 * KDS transaction manager.
 *
 * A minimal, WAL-backed transaction layer: it hands out monotonically
 * increasing transaction ids (xids), tracks the set of in-flight
 * transactions, and drives the three lifecycle operations through the
 * write-ahead log:
 *
 *   begin   -> KDS_WAL_REC_BEGIN   (ring buffer)
 *   commit  -> KDS_WAL_REC_COMMIT  (ring buffer) + kds_wal_sync()
 *   abort   -> KDS_WAL_REC_ABORT   (ring buffer) + kds_wal_sync()
 *
 * The durability rule is the classic one: a transaction is committed
 * only once its COMMIT record is on stable storage, so kds_txn_commit()
 * synchronously flushes the WAL (kds_wal_sync(), wal.c) before returning
 * COMMITTED -- it does not wait for the background checkpointer.
 *
 * Deliberately out of scope for this first version (kept minimal per
 * the engine's "small but strong" design; flagged, not half-built):
 *   - Snapshot / MVCC visibility (xmin/xmax are stored by heap.c but no
 *     snapshot evaluates them yet) and a CLOG mapping xid -> outcome.
 *   - Rollback that actually walks the undo chain (kds_undo.h) to
 *     restore pre-images; kds_txn_abort() currently logs the ABORT and
 *     marks the txn ABORTED but does not yet undo its page changes.
 *   - Nested/savepoint transactions and multi-statement grouping.
 * The existing executors still stamp the placeholder xid (1); wiring
 * them to real begin/commit is the follow-up that builds on this.
 */

/*
 * Transaction outcome states (the classic CLOG statuses). A brand-new
 * transaction is ACTIVE; it ends exactly once, COMMITTED or ABORTED.
 */
typedef enum kds_xact_state {
    KDS_XACT_ACTIVE = 0,
    KDS_XACT_COMMITTED,
    KDS_XACT_ABORTED,
} kds_xact_state_t;

/*
 * Reserved transaction ids:
 *   0 = invalid / "no transaction".
 *   1 = frozen bootstrap xid (matches KDS_BOOTSTRAP_XID in
 *       kds_catalog.h): rows written before the txn manager exists are
 *       stamped with it and are always visible.
 * Real transactions are assigned ids >= KDS_XID_FIRST.
 */
#define KDS_XID_INVALID   ((u64)0)
#define KDS_XID_FROZEN    ((u64)1)
#define KDS_XID_FIRST     ((u64)2)

typedef struct kds_transaction {
    u64               xid;
    kds_xact_state_t  state;
    kds_lsn_t         first_lsn;   /* LSN of this txn's BEGIN record   */
    kds_lsn_t         last_lsn;    /* LSN of its most recent record    */
    struct list_head  node;        /* membership in the active-txn list */
} kds_transaction_t;

/* ------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------ */

/*
 * kds_txn_init(): initialise the transaction manager. Must be called
 * after kds_wal_init() (begin/commit/abort append WAL records) during
 * kds_bootstrap(). Safe to call once; idempotent re-init resets the
 * xid counter and active set.
 */
int  kds_txn_init(void);

/*
 * kds_txn_shutdown(): release any lingering active-transaction records.
 * Called from kds_cleanup(). Does not itself abort/commit them -- by
 * teardown every worker that could own a txn has already stopped.
 */
void kds_txn_shutdown(void);

/* ------------------------------------------------------------------
 * Operations
 * ------------------------------------------------------------------ */

/*
 * kds_txn_begin(): allocate a fresh xid, append a WAL BEGIN record,
 * register the transaction as active, and return it ACTIVE. Returns
 * NULL on allocation failure. The returned handle is owned by the
 * caller and must eventually be passed to kds_txn_commit() or
 * kds_txn_abort(), then freed with kds_txn_free().
 */
kds_transaction_t *kds_txn_begin(void);

/*
 * kds_txn_commit(): append a COMMIT record and flush the WAL to disk
 * synchronously, then mark the transaction COMMITTED and remove it from
 * the active set. Returns 0 on success, -EINVAL if txn is NULL or not
 * ACTIVE, or a negative errno if the WAL append fails. When the WAL is
 * unavailable (-ENODEV) the commit still succeeds (nothing to make
 * durable), matching the engine's best-effort WAL stance.
 */
int  kds_txn_commit(kds_transaction_t *txn);

/*
 * kds_txn_abort(): append an ABORT record, flush synchronously, and
 * mark the transaction ABORTED. See the header note: undo of the txn's
 * page changes is not yet applied here.
 */
int  kds_txn_abort(kds_transaction_t *txn);

/*
 * kds_txn_free(): release a transaction handle after it has committed
 * or aborted. Passing an ACTIVE transaction aborts it first (so a
 * dropped handle can't silently leak an in-flight xid).
 */
void kds_txn_free(kds_transaction_t *txn);

/* ------------------------------------------------------------------
 * Accessors
 * ------------------------------------------------------------------ */

u64              kds_txn_xid(const kds_transaction_t *txn);
kds_xact_state_t kds_txn_state(const kds_transaction_t *txn);

/*
 * kds_txn_last_assigned_xid(): the highest xid handed out so far
 * (diagnostics / future snapshot upper bound).
 */
u64  kds_txn_last_assigned_xid(void);

#endif /* __KDS_TRANSACTION_H */
