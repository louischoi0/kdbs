/* transaction.c
 *
 * KDS transaction manager -- see kds_transaction.h for the design and
 * the explicitly out-of-scope items (MVCC snapshots, CLOG, real undo
 * rollback). This layer owns xid allocation, the active-transaction
 * set, and the begin/commit/abort lifecycle, driving each through the
 * write-ahead log (wal.c). Commit is made durable synchronously via
 * kds_wal_sync().
 */

#include <linux/kds.h>
#include <linux/kds_transaction.h>
#include <linux/kds_wal.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/atomic.h>
#include <linux/errno.h>

/*
 * Next xid to hand out. Statically initialised so xid allocation is
 * safe even before kds_txn_init() runs; kds_txn_init() resets it.
 *
 * KNOWN GAP (same as kds_catalog_generate_user_oid()): this counter is
 * in-memory only and restarts at KDS_XID_FIRST on every boot rather
 * than being persisted in the superblock. Persisting it would mean
 * adding a field to kds_superblock_t (kds_meta.h); flagged here rather
 * than changing that struct unilaterally.
 */
static atomic64_t g_next_xid = ATOMIC64_INIT(KDS_XID_FIRST);

/* Set of in-flight (ACTIVE) transactions. */
static LIST_HEAD(g_active_txns);
static DEFINE_SPINLOCK(g_txn_lock);

/* ------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------ */

int kds_txn_init(void)
{
    atomic64_set(&g_next_xid, (s64)KDS_XID_FIRST);

    spin_lock(&g_txn_lock);
    /* g_active_txns is a static LIST_HEAD; nothing should be on it yet,
     * but re-init defensively in case of a repeated bring-up. */
    INIT_LIST_HEAD(&g_active_txns);
    spin_unlock(&g_txn_lock);

    pr_info("kds_txn: transaction manager initialised (first xid=%llu)\n",
            (u64)KDS_XID_FIRST);
    return 0;
}

void kds_txn_shutdown(void)
{
    kds_transaction_t *txn, *tmp;

    spin_lock(&g_txn_lock);
    list_for_each_entry_safe(txn, tmp, &g_active_txns, node) {
        pr_warn("kds_txn: shutdown: dropping still-active xid=%llu\n",
                (u64)txn->xid);
        list_del_init(&txn->node);
        kfree(txn);
    }
    spin_unlock(&g_txn_lock);

    pr_info("kds_txn: transaction manager shut down\n");
}

/* ------------------------------------------------------------------
 * Operations
 * ------------------------------------------------------------------ */

kds_transaction_t *kds_txn_begin(void)
{
    kds_transaction_t *txn;
    kds_lsn_t          lsn = KDS_LSN_INVALID;

    txn = kzalloc(sizeof(*txn), GFP_KERNEL);
    if (!txn)
        return NULL;

    txn->xid   = (u64)atomic64_fetch_add(1, &g_next_xid);
    txn->state = KDS_XACT_ACTIVE;
    INIT_LIST_HEAD(&txn->node);

    /* BEGIN record is best-effort (not durability-critical). */
    kds_wal_begin(txn->xid, &lsn);
    txn->first_lsn = lsn;
    txn->last_lsn  = lsn;

    spin_lock(&g_txn_lock);
    list_add_tail(&txn->node, &g_active_txns);
    spin_unlock(&g_txn_lock);

    pr_info("kds_txn: begin xid=%llu\n", (u64)txn->xid);
    return txn;
}

/*
 * Shared commit/abort tail: append the terminal WAL record, make it
 * durable, transition state, and leave the active set.
 */
static int txn_end(kds_transaction_t *txn, bool commit)
{
    int ret;

    if (!txn)
        return -EINVAL;
    if (txn->state != KDS_XACT_ACTIVE)
        return -EINVAL;

    ret = commit ? kds_wal_commit(txn->xid) : kds_wal_abort(txn->xid);
    if (ret && ret != -ENODEV)
        return ret;

    if (!ret) {
        /*
         * Durability point: the transaction's outcome is only real once
         * its COMMIT/ABORT record is on stable storage. Flush the WAL
         * synchronously rather than waiting for the checkpointer.
         */
        kds_wal_sync();
        txn->last_lsn = kds_wal_get_current_lsn();
    }

    txn->state = commit ? KDS_XACT_COMMITTED : KDS_XACT_ABORTED;

    spin_lock(&g_txn_lock);
    list_del_init(&txn->node);
    spin_unlock(&g_txn_lock);

    pr_info("kds_txn: %s xid=%llu\n", commit ? "commit" : "abort",
            (u64)txn->xid);
    return 0;
}

int kds_txn_commit(kds_transaction_t *txn)
{
    return txn_end(txn, true);
}

int kds_txn_abort(kds_transaction_t *txn)
{
    /*
     * NOTE: this logs the ABORT and marks the txn ABORTED but does not
     * yet walk the undo chain (kds_undo.h) to restore pre-images -- see
     * the out-of-scope note in kds_transaction.h.
     */
    return txn_end(txn, false);
}

void kds_txn_free(kds_transaction_t *txn)
{
    if (!txn)
        return;

    /* A dropped handle must not leak an in-flight xid. */
    if (txn->state == KDS_XACT_ACTIVE)
        kds_txn_abort(txn);

    /* Defensive: ensure it is off the active list even if the abort
     * above failed (e.g. a WAL error left it linked). */
    spin_lock(&g_txn_lock);
    if (!list_empty(&txn->node))
        list_del_init(&txn->node);
    spin_unlock(&g_txn_lock);

    kfree(txn);
}

/* ------------------------------------------------------------------
 * Accessors
 * ------------------------------------------------------------------ */

u64 kds_txn_xid(const kds_transaction_t *txn)
{
    return txn ? txn->xid : KDS_XID_INVALID;
}

kds_xact_state_t kds_txn_state(const kds_transaction_t *txn)
{
    return txn ? txn->state : KDS_XACT_ABORTED;
}

u64 kds_txn_last_assigned_xid(void)
{
    /* g_next_xid is the *next* id to hand out; the last assigned is one
     * below it (or INVALID if none has been handed out yet). */
    s64 next = atomic64_read(&g_next_xid);

    return (next <= (s64)KDS_XID_FIRST) ? KDS_XID_INVALID : (u64)(next - 1);
}
