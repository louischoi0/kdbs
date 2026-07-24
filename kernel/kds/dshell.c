#include <linux/kds.h>
#include <linux/kds_dshell.h>
#include <linux/kds_catalog.h>
#include <linux/kds_types.h>
#include <linux/kds_relation.h>
#include <linux/kds_index_maint.h>
#include <linux/kds_executor.h>
#include <linux/kds_heap.h>
#include <linux/kds_page_mgr.h>
#include <linux/kds_page_alloc.h>
#include <linux/kds_meta.h>
#include <linux/kds_proc.h>
#include <linux/kds_parser.h>

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/printk.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/completion.h>
#include <linux/sched.h>
#include <linux/jiffies.h>
#include <linux/time64.h>
#include <linux/vmalloc.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/mutex.h>

/*
 * /dev/kds request/response architecture (v3 -- queue removed):
 *
 * write() parses and dispatches the command synchronously. Commands
 * that are fast and I/O-free (META, PSTA, CRTB) run to completion
 * right inside write() and return immediately. Commands that drive a
 * kds_exec_state_t (INRW, SELC, and any future heavy operation)
 * initialise an exec into client->exec_buf, mark the client as
 * EXEC_PENDING, and block on client->done waiting for the proc
 * scheduler to finish the work.
 *
 * kds_dshell_proc_run() is called by the KDS proc scheduler on its
 * normal schedule (same as any other kds_proc_t). It iterates over
 * all open clients, finds any in EXEC_PENDING state, and calls
 * kds_exec_run() with the scheduler's slice_ns budget. On
 * KDS_EXEC_CONTINUE the proc returns KDS_PROC_YIELD_RET so other
 * procs get a turn; the same client resumes on the next scheduling
 * round. On DONE or ERROR the proc writes the response into
 * client->resp_buf, marks the client EXEC_IDLE, and signals
 * client->done so write() can return to userspace.
 *
 * This means:
 *   - No queue, no kref, no list_head, no spinlock per-write.
 *   - Heavy exec work (PK-scan, future full btree traversal) is
 *     genuinely time-sliced by the scheduler, not just capped by
 *     a local while-CONTINUE loop inside write().
 *   - write()/read() contract is unchanged from the user's
 *     perspective: one write() = one command, one read() = its
 *     response. The only visible difference is that write() may
 *     block longer (up to KDS_DSHELL_EXEC_TIMEOUT_MS) if the
 *     scheduler is busy with other procs.
 *
 * Concurrency: each open() gets its own kds_dshell_client_t in
 * file->private_data. Multiple clients are independent. A single
 * client must not call write() again while still blocked in a
 * previous write() (no re-entrancy guard -- expected sequential
 * use from a single-threaded tool like kds_client.py).
 */

/* ------------------------------------------------------------------
 * Client type, globals, and exec helpers
 *
 * Defined before the command handlers because kds_cmd_insert_row()
 * takes a kds_dshell_client_t * and kds_dshell_submit_insert_exec()
 * must be visible at its call site. Everything else in the chardev
 * section (open/release/proc_run/write/read/init/shutdown) follows
 * after the command table.
 * ------------------------------------------------------------------ */

#define KDS_DSHELL_DEV_NAME        "kds"
#define KDS_DSHELL_EXEC_TIMEOUT_MS 5000

typedef enum {
    KDS_DSHELL_EXEC_IDLE    = 0,
    KDS_DSHELL_EXEC_PENDING = 1,
} kds_dshell_exec_state_t;

/*
 * The client's single inline exec_buf must be able to hold whichever
 * exec a command sets up: heap/btree insert, or select. Select's exec
 * struct is the largest (it carries resolved WHERE conditions), so it
 * dominates the max -- if a new, larger exec type is added, extend
 * this max the same way.
 */
#define KDS_DSHELL_MAX2(a, b)  ((a) > (b) ? (a) : (b))
#define KDS_DSHELL_EXEC_BUF_SIZE                                            \
    KDS_DSHELL_MAX2(                                                        \
        KDS_DSHELL_MAX2(KDS_DSHELL_MAX2(sizeof(kds_heap_insert_exec_t),     \
                                        sizeof(kds_btree_insert_exec_t)),   \
                        sizeof(kds_select_exec_t)),                         \
        sizeof(kds_update_exec_t))

typedef struct kds_dshell_client {
    /* response staging for read() */
    char                    resp_buf[DSHELL_RESP_MAX];
    size_t                  resp_len;
    size_t                  resp_pos;

    /* heavy-exec path */
    kds_dshell_exec_state_t exec_status;
    u8                      exec_buf[KDS_DSHELL_EXEC_BUF_SIZE];
    struct completion       done;

    /* intrusive list -- all open clients so proc_run() can iterate */
    struct list_head        node;
} kds_dshell_client_t;

static LIST_HEAD(kds_dshell_clients);
static DEFINE_SPINLOCK(kds_dshell_clients_lock);

static dev_t         kds_dshell_devt;
static struct cdev   kds_dshell_cdev;
static struct class *kds_dshell_class;
static bool          kds_dshell_inited;
static kds_proc_t   *kds_dshell_proc;

/*
 * Forward declaration -- kds_dshell_run_exec_via_proc() is defined
 * in the chardev section below but called from kds_cmd_insert_row()
 * in the command-handlers section above it.
 */
static int kds_dshell_run_exec_via_proc(kds_dshell_client_t *client);
static int kds_dshell_submit_insert_exec(kds_dshell_client_t *client,
                                          kds_heap_insert_exec_t *exec_init);

/* ------------------------------------------------------------------
 * Command handlers
 *
 * Each handler formats its result (success or error) into `out` via
 * scnprintf() and returns 0/negative errno. See kds_dshell.h for the
 * extension contract.
 * ------------------------------------------------------------------ */

/*
 * Placeholder transaction id used by all dshell commands until a
 * real transaction manager exists.
 */
#define KDS_DSHELL_XID      1

/*
 * Maximum encoded row size. Must be large enough to hold any row
 * this dshell can INSERT or scan (mirrors KDS_EXEC_ROW_SCAN_BUF in
 * executor.c -- the two aren't formally linked, so if one grows,
 * check the other too).
 */
#define KDS_DSHELL_ROW_MAX  256

/*
 * Row decoding for SELECT output now lives inside the select executor
 * (exec_heap_select.c), which formats matched rows directly into the
 * response buffer as it scans -- see kds_cmd_select() below.
 */

/*
 * Handler for KDS_STMT_CREATE_TABLE.
 * All parsing (table name, clustered type, column names and types) has
 * already been done by kds_parse(); this function only drives the
 * catalog layer and formats the response.
 */
static int kds_cmd_create_table(const kds_stmt_create_table_t *stmt,
                                 char *out, size_t out_size)
{
    kds_schema_t *schema;
    kd_oid_t      new_oid;
    u32           i;
    int           ret;

    schema = kzalloc(sizeof(*schema), GFP_KERNEL);
    if (!schema) {
        scnprintf(out, out_size, "ERR out of memory\n");
        return -ENOMEM;
    }

    for (i = 0; i < stmt->nr_cols; i++) {
        const kds_ast_col_def_t *src = &stmt->cols[i];
        kds_sys_column_t        *col = &schema->cols[i];

        col->pos      = i;
        col->type_val = src->type_val;
        col->len      = src->len;
        col->notnull  = true;
        strncpy(col->name, src->name, KDS_CATALOG_NAME_MAX - 1);
        col->name[KDS_CATALOG_NAME_MAX - 1] = '\0';
    }
    schema->nr_cols = stmt->nr_cols;

    /*
     * DB-wide self-constraint: the first column is the PRIMARY KEY and
     * is a 64-bit UNSIGNED integer -- the system forces it. The parser
     * doesn't know DB semantics, so enforce it here. Accept a declared
     * uint64 or int64 (both are 8-byte 64-bit ints) for ergonomics, then
     * FORCE the stored type to uint64 so every PK path is unsigned;
     * reject anything else.
     */
    if (schema->cols[0].type_val != KDS_TYPE_UINT64 &&
        schema->cols[0].type_val != KDS_TYPE_INT64) {
        scnprintf(out, out_size,
                  "ERR first column ('%s') must be the primary key "
                  "(declare it uint64 or int64)\n",
                  schema->cols[0].name);
        kfree(schema);
        return -EINVAL;
    }
    schema->cols[0].type_val = KDS_TYPE_UINT64;
    schema->cols[0].len      = 8;

    ret = kds_catalog_create_table(KDS_OID_NAMESPACE_PUBLIC,
                                    stmt->table_name, schema,
                                    stmt->clustered, &new_oid);
    kfree(schema);

    if (ret) {
        scnprintf(out, out_size, "ERR create_table failed: %d\n", ret);
        return ret;
    }

    scnprintf(out, out_size,
              "OK table '%s' created, oid=%llu, columns=%u, clustered=%s\n",
              stmt->table_name, (u64)new_oid, stmt->nr_cols,
              stmt->clustered == KDS_CLUSTERED_BTREE ? "BTREE" : "HEAP");
    return 0;
}

/*
 * Row encoding -- converts kds_ast_val_t[] (parser output) into the
 * on-disk binary row format. Fixed-width types are stored back-to-back
 * at their known fixed_len; variable-width types get a u16 length
 * prefix ahead of their encoded bytes.
 */
static int kds_dshell_encode_row_from_vals(const kds_schema_t   *schema,
                                            const kds_ast_val_t  *vals,
                                            u32                   nr_vals,
                                            u8 *buf, size_t buf_size,
                                            u16 *out_len)
{
    size_t off = 0;
    u32    i;

    for (i = 0; i < schema->nr_cols; i++) {
        const kds_sys_column_t *col  = &schema->cols[i];
        const kds_ast_val_t    *val  = &vals[i];
        const kds_type_desc_t  *desc =
            kds_type_lookup_by_val((kds_type_val_t)col->type_val);
        u16 written_len;
        int ret;

        if (!desc)
            return -EINVAL;

        /*
         * Encode via the shared kds_encode_ast_val() so INSERT, UPDATE,
         * and WHERE all agree on literal encoding (and so a uint64 PK's
         * full range round-trips). Variable-width columns get a u16
         * length prefix; fixed-width columns are written in place.
         */
        if (desc->fixed_len == 0) {
            if (off + sizeof(u16) > buf_size)
                return -ENOSPC;
            ret = kds_encode_ast_val(desc, val, buf + off + sizeof(u16),
                                     buf_size - off - sizeof(u16), &written_len);
            if (ret)
                return ret;
            memcpy(buf + off, &written_len, sizeof(u16));
            off += sizeof(u16) + written_len;
        } else {
            if (off + desc->fixed_len > buf_size)
                return -ENOSPC;
            ret = kds_encode_ast_val(desc, val, buf + off,
                                     buf_size - off, &written_len);
            if (ret)
                return ret;
            off += written_len;
        }
    }

    *out_len = (u16)off;
    return 0;
}

static int kds_cmd_insert_row(kds_dshell_client_t        *client,
                               const kds_stmt_insert_t    *stmt,
                               char *out, size_t out_size)
{
    kd_oid_t        oid;
    kds_relation_t *rel;
    u8              row_buf[KDS_DSHELL_ROW_MAX];
    u16             row_len;
    int             ret;

    ret = kds_catalog_find_table_oid_by_name(stmt->table_name, &oid);
    if (ret) {
        scnprintf(out, out_size, "ERR table '%s' not found: %d\n",
                  stmt->table_name, ret);
        return ret;
    }

    rel = kds_relation_open(oid);
    if (IS_ERR(rel)) {
        ret = PTR_ERR(rel);
        scnprintf(out, out_size, "ERR relation_open failed: %d\n", ret);
        return ret;
    }

    if (stmt->nr_values != rel->schema.nr_cols) {
        scnprintf(out, out_size,
                  "ERR '%s' has %u column(s), got %u value(s)\n",
                  stmt->table_name, rel->schema.nr_cols, stmt->nr_values);
        kds_relation_close(rel);
        return -EINVAL;
    }

    ret = kds_dshell_encode_row_from_vals(&rel->schema,
                                           stmt->values, stmt->nr_values,
                                           row_buf, sizeof(row_buf), &row_len);
    if (ret) {
        scnprintf(out, out_size,
                  "ERR failed to encode row (too long or bad value): %d\n", ret);
        kds_relation_close(rel);
        return ret;
    }


    if (rel->kind == KDS_CLUSTERED_HEAP) {
        kds_heap_insert_exec_t exec;

        kds_heap_insert_exec_init(&exec, rel, row_buf, row_len,
                                   KDS_DSHELL_XID);

        ret = kds_dshell_submit_insert_exec(client, &exec);
        kds_relation_close(rel);

        if (ret == -ETIMEDOUT)
            return ret;

        {
            kds_heap_insert_exec_t *done =
                (kds_heap_insert_exec_t *)client->exec_buf;

            if (done->base.ret != 0) {
                if (done->base.ret == -EEXIST)
                    scnprintf(out, out_size,
                              "ERR duplicate primary key in '%s'\n",
                              stmt->table_name);
                else
                    scnprintf(out, out_size,
                              "ERR insert failed: %d\n", done->base.ret);
                return done->base.ret;
            }

            scnprintf(out, out_size,
                      "OK inserted into '%s' at page=%llu slot=%u\n",
                      stmt->table_name,
                      (u64)done->out_tid.page_id,
                      done->out_tid.slot);
        }

    } else if (rel->kind == KDS_CLUSTERED_BTREE) {
        kds_btree_insert_exec_t exec;
        kds_tuple_id_t          pk;

        if (row_len < sizeof(pk)) {
            scnprintf(out, out_size, "ERR row too short to contain PK\n");
            kds_relation_close(rel);
            return -EINVAL;
        }
        memcpy(&pk, row_buf, sizeof(pk));

        kds_btree_insert_exec_init(&exec, rel, pk, row_buf, row_len);
        pr_info("btree insert exec init ok\n");

        BUILD_BUG_ON(sizeof(exec) > KDS_DSHELL_EXEC_BUF_SIZE);
        memcpy(client->exec_buf, &exec, sizeof(exec));

        ret = kds_dshell_run_exec_via_proc(client);
        kds_relation_close(rel);

        if (ret == -ETIMEDOUT)
            return ret;

        {
            kds_btree_insert_exec_t *done =
                (kds_btree_insert_exec_t *)client->exec_buf;

            if (done->base.ret != 0) {
                if (done->base.ret == -EEXIST)
                    scnprintf(out, out_size,
                              "ERR duplicate key %llu in '%s'\n",
                              (unsigned long long)pk, stmt->table_name);
                else
                    scnprintf(out, out_size,
                              "ERR btree insert failed: %d\n",
                              done->base.ret);
                return done->base.ret;
            }

            scnprintf(out, out_size,
                      "OK inserted into '%s' (btree) key=%llu "
                      "page=%llu slot=%u\n",
                      stmt->table_name, (unsigned long long)pk,
                      (u64)done->out_tid.page_id,
                      done->out_tid.slot);
        }

    } else {
        scnprintf(out, out_size,
                  "ERR '%s' has unsupported clustered type %d\n",
                  stmt->table_name, rel->kind);
        kds_relation_close(rel);
        return -EINVAL;
    }

    return 0;
}

/*
 * SELECT * FROM <table> [WHERE <cond> [AND <cond>]*]
 *
 * Drives the resumable, time-sliced SelectExec (kds_select_exec_t --
 * exec_heap_select.c for heap-clustered tables, exec_btree_select.c for
 * btree-clustered ones) through the same proc machinery insert uses, so
 * a large scan is spread across scheduler slices instead of hogging one
 * write() call. The exec streams matched rows straight into the
 * response buffer (`out`, which is client->resp_buf) as it scans, so on
 * completion the response is already formatted; this handler only sets
 * the exec up, keeps the relation open for the whole run, and maps a
 * synchronous init failure to an ERR line.
 *
 * WHERE-column resolution happens synchronously in kds_select_exec_init()
 * (no I/O); only the scan itself is deferred to the proc.
 */
static int kds_cmd_select(kds_dshell_client_t     *client,
                          const kds_stmt_select_t *stmt,
                          char *out, size_t out_size)
{
    kd_oid_t           oid;
    kds_relation_t    *rel;
    kds_select_exec_t *exec;
    int                ret;

    BUILD_BUG_ON(sizeof(kds_select_exec_t) > KDS_DSHELL_EXEC_BUF_SIZE);

    ret = kds_catalog_find_table_oid_by_name(stmt->table_name, &oid);
    if (ret) {
        scnprintf(out, out_size, "ERR table '%s' not found: %d\n",
                  stmt->table_name, ret);
        return ret;
    }

    rel = kds_relation_open(oid);
    if (IS_ERR(rel)) {
        ret = PTR_ERR(rel);
        scnprintf(out, out_size, "ERR relation_open failed: %d\n", ret);
        return ret;
    }

    if (rel->kind != KDS_CLUSTERED_HEAP && rel->kind != KDS_CLUSTERED_BTREE) {
        scnprintf(out, out_size,
                  "ERR '%s' has unsupported clustered type %d\n",
                  stmt->table_name, rel->kind);
        kds_relation_close(rel);
        return -EINVAL;
    }

    /*
     * Build the exec directly in the client's exec_buf: the struct is
     * large (it carries the resolved WHERE conditions) and must outlive
     * this stack frame anyway, since the proc runs it asynchronously.
     * Init writes the "OK " prefix into `out` and resolves the WHERE
     * columns; the run() calls that follow do the actual scanning.
     */
    exec = (kds_select_exec_t *)client->exec_buf;
    ret = kds_select_exec_init(exec, rel,
                               stmt->conds,
                               stmt->has_where ? stmt->nr_conds : 0,
                               out, out_size);
    if (ret) {
        if (ret == -ENOENT)
            scnprintf(out, out_size,
                      "ERR unknown column in WHERE clause\n");
        else if (ret == -EOPNOTSUPP)
            scnprintf(out, out_size,
                      "ERR ordering comparison unsupported for that column type\n");
        else
            scnprintf(out, out_size, "ERR select init failed: %d\n", ret);
        kds_relation_close(rel);
        return ret;
    }

    ret = kds_dshell_run_exec_via_proc(client);

    /*
     * Keep the relation open until the exec has fully finished: the
     * scan dereferences rel->schema and rel->root_page_id on the proc's
     * call stack, so rel must outlive the run (mirrors
     * kds_cmd_insert_row()).
     */
    kds_relation_close(rel);

    if (ret == -ETIMEDOUT)
        return ret;

    /*
     * On DONE the response (OK + rows + summary) was already streamed
     * into resp_buf by the exec. On ERROR, kds_dshell_proc_run() has
     * already replaced it with an "ERR exec failed: N" line. Either
     * way `out` is fully populated -- just surface the exec's errno.
     */
    if (exec->base.ret != 0)
        return exec->base.ret;

    return 0;
}

/*
 * UPDATE <table> SET <col>=<val>[,...] [WHERE ...]
 *
 * Drives the resumable UpdateExec (kds_update_exec_t --
 * exec_heap_update.c / exec_btree_update.c) through the same proc
 * machinery insert/select use. The exec applies the SET values to every
 * WHERE-matching row (via kds_heap_update_tuple: undo + synchronous WAL)
 * and writes only a summary line into the response buffer on completion.
 * WHERE/SET resolution -- including rejecting an attempt to SET the
 * primary key -- happens synchronously in kds_update_exec_init().
 */
static int kds_cmd_update(kds_dshell_client_t     *client,
                          const kds_stmt_update_t *stmt,
                          char *out, size_t out_size)
{
    kd_oid_t           oid;
    kds_relation_t    *rel;
    kds_update_exec_t *exec;
    int                ret;

    BUILD_BUG_ON(sizeof(kds_update_exec_t) > KDS_DSHELL_EXEC_BUF_SIZE);

    ret = kds_catalog_find_table_oid_by_name(stmt->table_name, &oid);
    if (ret) {
        scnprintf(out, out_size, "ERR table '%s' not found: %d\n",
                  stmt->table_name, ret);
        return ret;
    }

    rel = kds_relation_open(oid);
    if (IS_ERR(rel)) {
        ret = PTR_ERR(rel);
        scnprintf(out, out_size, "ERR relation_open failed: %d\n", ret);
        return ret;
    }

    if (rel->kind != KDS_CLUSTERED_HEAP && rel->kind != KDS_CLUSTERED_BTREE) {
        scnprintf(out, out_size,
                  "ERR '%s' has unsupported clustered type %d\n",
                  stmt->table_name, rel->kind);
        kds_relation_close(rel);
        return -EINVAL;
    }

    exec = (kds_update_exec_t *)client->exec_buf;
    ret = kds_update_exec_init(exec, rel,
                               stmt->assigns, stmt->nr_assigns,
                               stmt->conds,
                               stmt->has_where ? stmt->nr_conds : 0,
                               KDS_DSHELL_XID, out, out_size);
    if (ret) {
        if (ret == -ENOENT)
            scnprintf(out, out_size,
                      "ERR unknown column in SET or WHERE clause\n");
        else if (ret == -EPERM)
            scnprintf(out, out_size,
                      "ERR cannot UPDATE the primary key column\n");
        else if (ret == -EOPNOTSUPP)
            scnprintf(out, out_size,
                      "ERR ordering comparison unsupported for that column type\n");
        else
            scnprintf(out, out_size, "ERR update init failed: %d\n", ret);
        kds_relation_close(rel);
        return ret;
    }

    ret = kds_dshell_run_exec_via_proc(client);

    /* Keep the relation open across the run (mirrors kds_cmd_select). */
    kds_relation_close(rel);

    if (ret == -ETIMEDOUT)
        return ret;

    /* On DONE the summary is already in resp_buf; on ERROR the proc
     * wrote "ERR exec failed: N". Surface the exec's errno. */
    if (exec->base.ret != 0)
        return exec->base.ret;

    return 0;
}

/*
 * META -- dumps every field in the superblock (kds_meta.h's
 * kds_superblock_t). Reads are taken under the same
 * lock_meta_superblock()/unlock_meta_superblock() pair every other
 * superblock access in this codebase uses -- this is a plain
 * spinlock-protected memory read, no I/O involved, so there's
 * nothing to defer or worry about blocking here (unlike
 * kds_superblock_fsync(), which this command does NOT call -- it
 * only reads the in-memory copy, not a fresh read from disk).
 */
static int kds_cmd_show_meta(char *out, size_t out_size)
{
    kds_superblock_t *sb = ref_superblock();
    unsigned long flags;
    u64 magic, max_page_id, last_commit_page_id, total_pages, free_pages;
    u64 create_time, last_mount_time, last_fsync_time;
    u64 alloc_point, alloc_remaining;
    u32 version;

    if (!sb) {
        scnprintf(out, out_size, "ERR meta system not initialized\n");
        return -ENODEV;
    }

    lock_meta_superblock(&flags);
    magic = sb->magic;
    version = sb->version;
    max_page_id = atomic64_read(&sb->max_page_id);
    last_commit_page_id = atomic64_read(&sb->last_commit_page_id);
    total_pages = atomic64_read(&sb->total_pages);
    free_pages = atomic64_read(&sb->free_pages);
    create_time = sb->create_time;
    last_mount_time = sb->last_mount_time;
    last_fsync_time = sb->last_fsync_time;
    alloc_point = sb->alloc_point;
    alloc_remaining = sb->alloc_remaining;
    unlock_meta_superblock(&flags);

    scnprintf(out, out_size,
              "OK magic=0x%llx version=%u\n"
              "   max_page_id=%llu last_commit_page_id=%llu\n"
              "   total_pages=%llu free_pages=%llu\n"
              "   create_time=%llu last_mount_time=%llu last_fsync_time=%llu\n"
              "   alloc_point=%llu alloc_remaining=%llu\n",
              magic, version,
              max_page_id, last_commit_page_id,
              total_pages, free_pages,
              create_time, last_mount_time, last_fsync_time,
              alloc_point, alloc_remaining);

    return 0;
}

/*
 * PSTA -- dumps page-allocation status: the id allocator's standing
 * range/ring/freelist (kds_page_alloc.c) and the buffer pool's frame
 * occupancy (kds_page_mgr.c). Useful for debugging exactly the class
 * of issue seen earlier (ring staged at 0, or never refilling).
 */
static int kds_cmd_show_page_alloc_status(char *out, size_t out_size)
{
    kds_page_id_t alloc_point;
    u64 alloc_remaining, ring_count, freelist_count;
    u32 total_frames, free_frames, valid_frames;

    kds_page_alloc_get_stats(&alloc_point, &alloc_remaining, &ring_count, &freelist_count);
    kds_buf_pool_get_stats(&total_frames, &free_frames, &valid_frames);

    scnprintf(out, out_size,
              "OK allocator: alloc_point=%llu alloc_remaining=%llu "
              "ring_staged=%llu freelist=%llu\n"
              "   frame_pool: total=%u free=%u valid=%u loading=%u\n",
              (u64)alloc_point, alloc_remaining, ring_count, freelist_count,
              total_frames, free_frames, valid_frames,
              total_frames - free_frames - valid_frames);

    return 0;
}

/* ------------------------------------------------------------------
 * Statement dispatcher
 *
 * Receives a fully-parsed kds_stmt_t and routes it to the appropriate
 * handler. No string parsing happens here -- all token/keyword work
 * was done by kds_parse() before this is called.
 * ------------------------------------------------------------------ */

/*
 * Handler for KDS_STMT_CREATE_INDEX -- CREATE INDEX <name> ON <table> (<col>);
 *
 * Runs inline like CREATE TABLE: resolve the table + column, enforce the
 * index constraints (integer column, unique, not already indexed), create
 * the index relation + its sys.indexes row, then backfill the index from
 * the table's existing rows. Backfill is synchronous -- CREATE INDEX is a
 * one-shot DDL op on small edge-node tables, not a time-sliced query.
 *
 * On a duplicate-value column the backfill fails with -EEXIST after the
 * index relation and its catalog rows already exist, leaving an empty/
 * partial index (there is no DROP INDEX yet -- resetdata.sh to clear).
 * Proper rollback needs transactional DDL the engine doesn't have; flagged
 * rather than fixed here.
 */
static int kds_cmd_create_index(const kds_stmt_create_index_t *stmt,
                                char *out, size_t out_size)
{
    kd_oid_t                table_oid, index_oid;
    kds_relation_t         *table_rel, *index_rel;
    const kds_sys_column_t *col;
    kds_sys_index_t         existing;
    u32                     col_idx;
    int                     ret;

    ret = kds_catalog_find_table_oid_by_name(stmt->table_name, &table_oid);
    if (ret) {
        scnprintf(out, out_size, "ERR table '%s' not found: %d\n",
                  stmt->table_name, ret);
        return ret;
    }

    table_rel = kds_relation_open(table_oid);
    if (IS_ERR(table_rel)) {
        ret = PTR_ERR(table_rel);
        scnprintf(out, out_size, "ERR relation_open failed: %d\n", ret);
        return ret;
    }

    col = kds_schema_find_column(&table_rel->schema, stmt->col_name);
    if (!col) {
        scnprintf(out, out_size, "ERR column '%s' not found in table '%s'\n",
                  stmt->col_name, stmt->table_name);
        kds_relation_close(table_rel);
        return -ENOENT;
    }
    col_idx = (u32)(col - table_rel->schema.cols);

    if (!kds_index_type_supported(col->type_val)) {
        scnprintf(out, out_size,
                  "ERR can only index integer columns (int8/16/32/64); "
                  "'%s' is not\n", stmt->col_name);
        kds_relation_close(table_rel);
        return -EOPNOTSUPP;
    }

    /* Reject a second index on the same column. */
    ret = kds_catalog_find_index_on_column(table_oid, col_idx, &existing);
    if (ret == 0) {
        scnprintf(out, out_size,
                  "ERR column '%s' is already indexed (index oid=%llu)\n",
                  stmt->col_name, (u64)existing.index_oid);
        kds_relation_close(table_rel);
        return -EEXIST;
    } else if (ret != -ENOENT) {
        scnprintf(out, out_size, "ERR index lookup failed: %d\n", ret);
        kds_relation_close(table_rel);
        return ret;
    }

    /* Create the index relation (btree root + sys.objects/sys.tables). */
    ret = kds_relation_create_index(table_rel->namespace_oid, table_oid,
                                    stmt->col_name, &index_oid);
    if (ret) {
        scnprintf(out, out_size, "ERR create index relation failed: %d\n", ret);
        kds_relation_close(table_rel);
        return ret;
    }

    /* Record which column it keys, for maintenance + planner. */
    ret = kds_catalog_insert_index_row(index_oid, table_oid, col_idx,
                                       col->type_val, KDS_INDEX_FLAG_UNIQUE);
    if (ret) {
        scnprintf(out, out_size, "ERR sys.indexes insert failed: %d\n", ret);
        kds_relation_close(table_rel);
        return ret;
    }

    /* Backfill from existing rows. */
    index_rel = kds_relation_open(index_oid);
    if (IS_ERR(index_rel)) {
        ret = PTR_ERR(index_rel);
        scnprintf(out, out_size, "ERR open new index failed: %d\n", ret);
        kds_relation_close(table_rel);
        return ret;
    }

    ret = kds_index_backfill(index_rel, table_rel, col_idx);
    kds_relation_close(index_rel);
    kds_relation_close(table_rel);

    if (ret == -EEXIST) {
        scnprintf(out, out_size,
                  "ERR column '%s' has duplicate values; a unique index "
                  "cannot be built (index left partial -- resetdata to clear)\n",
                  stmt->col_name);
        return ret;
    }
    if (ret) {
        scnprintf(out, out_size, "ERR index backfill failed: %d\n", ret);
        return ret;
    }

    scnprintf(out, out_size,
              "OK index '%s' created on %s(%s), oid=%llu\n",
              stmt->index_name, stmt->table_name, stmt->col_name,
              (u64)index_oid);
    return 0;
}

static int kds_dispatch_stmt(kds_dshell_client_t *client,
                              const kds_stmt_t    *stmt,
                              char *out, size_t out_size)
{
    switch (stmt->type) {
    case KDS_STMT_CREATE_TABLE:
        return kds_cmd_create_table(&stmt->create_table, out, out_size);

    case KDS_STMT_CREATE_INDEX:
        return kds_cmd_create_index(&stmt->create_index, out, out_size);

    case KDS_STMT_INSERT:
        return kds_cmd_insert_row(client, &stmt->insert, out, out_size);

    case KDS_STMT_SELECT:
        return kds_cmd_select(client, &stmt->select, out, out_size);

    case KDS_STMT_UPDATE:
        return kds_cmd_update(client, &stmt->update, out, out_size);

    case KDS_STMT_SHOW_META:
        return kds_cmd_show_meta(out, out_size);

    case KDS_STMT_SHOW_ALLOC:
        return kds_cmd_show_page_alloc_status(out, out_size);

    default:
        scnprintf(out, out_size, "ERR unknown statement type %d\n", stmt->type);
        return -EINVAL;
    }
}

/* ------------------------------------------------------------------
 * Character device: /dev/kds
 * ------------------------------------------------------------------ */

static int kds_dshell_open(struct inode *inode, struct file *filp)
{
    kds_dshell_client_t *client;

    BUILD_BUG_ON(KDS_DSHELL_EXEC_BUF_SIZE < sizeof(kds_heap_insert_exec_t));

    client = kzalloc(sizeof(*client), GFP_KERNEL);
    if (!client)
        return -ENOMEM;

    init_completion(&client->done);
    INIT_LIST_HEAD(&client->node);
    client->exec_status = KDS_DSHELL_EXEC_IDLE;

    spin_lock(&kds_dshell_clients_lock);
    list_add_tail(&client->node, &kds_dshell_clients);
    spin_unlock(&kds_dshell_clients_lock);

    filp->private_data = client;
    return 0;
}

static int kds_dshell_release(struct inode *inode, struct file *filp)
{
    kds_dshell_client_t *client = filp->private_data;

    if (!client)
        return 0;

    spin_lock(&kds_dshell_clients_lock);
    list_del(&client->node);
    spin_unlock(&kds_dshell_clients_lock);

    /*
     * If write() is still blocked on done, wake it with a shutdown
     * signal. In normal usage release() is only called after write()
     * has returned, but be defensive: a process that closes the fd
     * from a signal handler while write() is sleeping is possible.
     */
    if (client->exec_status == KDS_DSHELL_EXEC_PENDING) {
        scnprintf(client->resp_buf, sizeof(client->resp_buf),
                  "ERR client closed while exec in flight\n");
        complete(&client->done);
    }

    kfree(client);
    filp->private_data = NULL;
    return 0;
}

/*
 * Called by the KDS proc scheduler. Iterates all open clients and
 * advances any that are in EXEC_PENDING state by one slice_ns worth
 * of work. Returns KDS_PROC_YIELD_RET after finding one CONTINUE
 * result (so other procs get a turn), or after servicing all pending
 * clients if they all finished in this slice.
 */
static kds_proc_result_t kds_dshell_proc_run(kds_proc_t *proc, u64 slice_ns)
{
    kds_dshell_client_t *client;
    kds_exec_result_t result;

    spin_lock(&kds_dshell_clients_lock);

    list_for_each_entry(client, &kds_dshell_clients, node) {
        if (client->exec_status != KDS_DSHELL_EXEC_PENDING)
            continue;

        /*
         * Drop the lock before calling kds_exec_run(): exec work
         * may touch page_mgr, heap, btree, etc., all of which have
         * their own internal locking. Holding clients_lock across
         * that would create a lock-ordering hazard and would block
         * open()/release() for the full duration of the slice.
         *
         * Re-acquire after exec returns to update exec_status and
         * signal done. This is safe: the client is pinned in memory
         * for as long as the file descriptor is open
         * (kds_dshell_release() removes from the list and then frees,
         * in that order, and it takes the lock to do the removal --
         * so a client can't disappear under us between the
         * list_for_each_entry and the re-lock below).
         */
        spin_unlock(&kds_dshell_clients_lock);

        result = kds_exec_run((kds_exec_state_t *)client->exec_buf, slice_ns);

        spin_lock(&kds_dshell_clients_lock);

        if (result == KDS_EXEC_CONTINUE) {
            /* Budget spent; yield to other procs and resume next round. */
            spin_unlock(&kds_dshell_clients_lock);
            return KDS_PROC_YIELD_RET;
        }

        /* DONE or ERROR: finalise the response and wake write(). */
        if (result == KDS_EXEC_ERROR) {
            kds_exec_state_t *es = (kds_exec_state_t *)client->exec_buf;

            scnprintf(client->resp_buf, sizeof(client->resp_buf),
                      "ERR exec failed: %d\n", es->ret);
        }
        /* On DONE, resp_buf was already filled by the handler that
         * set up the exec (see kds_cmd_insert_row() below) -- the
         * handler writes success output after kds_exec_run() returns
         * in the fast path, but for the slow (proc) path it can't
         * do that because it's not on the proc's call stack. So the
         * convention is: the handler leaves resp_buf empty and the
         * proc fills it here. See the INRW handler for the actual
         * pattern. */

        client->resp_len = strnlen(client->resp_buf, sizeof(client->resp_buf));
        client->resp_pos = 0;
        client->exec_status = KDS_DSHELL_EXEC_IDLE;

        complete(&client->done);
    }

    spin_unlock(&kds_dshell_clients_lock);
    return KDS_PROC_YIELD_RET;
}

/*
 * Runs a heavy exec (one that drives a kds_exec_state_t) from
 * write() context. Registers the exec into client->exec_buf, marks
 * the client EXEC_PENDING so kds_dshell_proc_run() can pick it up,
 * then blocks on client->done.
 *
 * The resp_buf is filled either:
 *   - by the proc (DONE/ERROR path in kds_dshell_proc_run()), or
 *   - by this function's timeout handler.
 *
 * Returns 0 if the exec completed (DONE), or a negative errno.
 */
static int kds_dshell_run_exec_via_proc(kds_dshell_client_t *client)
{
    long wait_ret;

    reinit_completion(&client->done);

    /*
     * Publish the exec to the proc *after* reinit_completion() --
     * if the proc is already running on another CPU, it must see the
     * freshly reset completion, not a stale one from a previous
     * command. WRITE_ONCE on exec_status provides the needed barrier
     * without requiring a spinlock here (the proc reads exec_status
     * inside kds_dshell_clients_lock, but this store happens in
     * write() context, which is single-threaded per client by
     * construction, so there's no write-write race).
     */
    WRITE_ONCE(client->exec_status, KDS_DSHELL_EXEC_PENDING);

    wait_ret = wait_for_completion_timeout(
        &client->done,
        msecs_to_jiffies(KDS_DSHELL_EXEC_TIMEOUT_MS));

    if (wait_ret == 0) {
        /*
         * Timed out. Try to cancel: grab the lock, only clear
         * PENDING if the proc hasn't already picked it up and
         * started running (it signals done before clearing PENDING
         * in kds_dshell_proc_run(), so if exec_status is still
         * PENDING here, the proc hasn't touched it yet).
         */
        spin_lock(&kds_dshell_clients_lock);
        if (client->exec_status == KDS_DSHELL_EXEC_PENDING)
            client->exec_status = KDS_DSHELL_EXEC_IDLE;
        spin_unlock(&kds_dshell_clients_lock);

        scnprintf(client->resp_buf, sizeof(client->resp_buf),
                  "ERR exec timed out after %dms\n",
                  KDS_DSHELL_EXEC_TIMEOUT_MS);
        client->resp_len = strnlen(client->resp_buf, sizeof(client->resp_buf));
        client->resp_pos = 0;
        return -ETIMEDOUT;
    }

    return 0;
}

/*
 * kds_cmd_insert_row() needs to hand its exec to the proc and then
 * format the success response once done. This helper wraps that
 * two-step: run via proc, then let the caller inspect out_tid.
 * Called from kds_cmd_insert_row() *instead of* the old
 * while-CONTINUE loop -- the loop now lives inside
 * kds_dshell_proc_run().
 */
static int kds_dshell_submit_insert_exec(kds_dshell_client_t *client,
                                          kds_heap_insert_exec_t *exec_init)
{
    BUILD_BUG_ON(sizeof(*exec_init) > KDS_DSHELL_EXEC_BUF_SIZE);
    memcpy(client->exec_buf, exec_init, sizeof(*exec_init));
    return kds_dshell_run_exec_via_proc(client);
}

static ssize_t kds_dshell_write(struct file *filp, const char __user *ubuf,
                                 size_t len, loff_t *off)
{
    kds_dshell_client_t *client = filp->private_data;
    char                 sql_buf[DSHELL_LINE_MAX];
    kds_stmt_t           stmt;
    char                 parse_err[KDS_PARSER_ERR_MAX];
    size_t               n;
    int                  ret;

    if (!client)
        return -EINVAL;

    n = min(len, sizeof(sql_buf) - 1);
    if (copy_from_user(sql_buf, ubuf, n))
        return -EFAULT;
    sql_buf[n] = '\0';

    /* Reset response staging for this new command. */
    client->resp_buf[0] = '\0';
    client->resp_len    = 0;
    client->resp_pos    = 0;

    ret = kds_parse(sql_buf, &stmt, parse_err, sizeof(parse_err));
    if (ret) {
        scnprintf(client->resp_buf, sizeof(client->resp_buf),
                  "ERR parse error: %s\n", parse_err);
    } else {
        kds_dispatch_stmt(client, &stmt,
                          client->resp_buf, sizeof(client->resp_buf));
    }

    client->resp_len = strnlen(client->resp_buf, sizeof(client->resp_buf));
    client->resp_pos = 0;

    return (ssize_t)len;
}

static ssize_t kds_dshell_read(struct file *filp, char __user *ubuf,
                                size_t len, loff_t *off)
{
    kds_dshell_client_t *client = filp->private_data;
    size_t remaining;
    size_t n;

    if (!client)
        return -EINVAL;

    remaining = client->resp_len - client->resp_pos;
    if (remaining == 0)
        return 0; /* nothing staged (or already fully read) -- EOF for this response */

    n = min(len, remaining);
    if (copy_to_user(ubuf, client->resp_buf + client->resp_pos, n))
        return -EFAULT;

    client->resp_pos += n;
    return (ssize_t)n;
}

static const struct file_operations kds_dshell_fops = {
    .owner   = THIS_MODULE,
    .open    = kds_dshell_open,
    .release = kds_dshell_release,
    .write   = kds_dshell_write,
    .read    = kds_dshell_read,
};

/*
 * vzalloc(), not vmalloc() -- see page_alloc.c's
 * __kds_create_proc_prealloc() for the NULL-pointer crash this
 * exact mistake caused there. kds_proc_register() depends on
 * fields it doesn't set itself (allowed_cpus, preferred_cpu, ...)
 * starting out zeroed.
 */
static int kds_register_dshell_proc(void)
{
    int ret;

    kds_dshell_proc = vzalloc(sizeof(kds_proc_t));
    if (!kds_dshell_proc)
        return -ENOMEM;

    kds_dshell_proc->kind          = KDS_PROC_SYSTEM;
    kds_dshell_proc->name          = "kds_dshell_proc";
    kds_dshell_proc->static_prio   = -1;
    kds_dshell_proc->dynamic_prio  = KDS_PROC_PRIORITY_SYSTEM_BACKGROUND;
    kds_dshell_proc->run           = kds_dshell_proc_run;
    kds_dshell_proc->ctx           = NULL;
    kds_dshell_proc->state         = KDS_PROC_STATE_READY;

    ret = kds_proc_register(kds_dshell_proc);
    if (ret) {
        pr_err("kds_dshell: failed to register scheduler proc: %d\n", ret);
        vfree(kds_dshell_proc);
        kds_dshell_proc = NULL;
        return ret;
    }

    pr_info("kds_dshell: scheduler proc registered\n");
    return 0;
}

int kds_init_dshell_system(void)
{
    int ret;
    struct device *dev;

    if (kds_dshell_inited)
        return 0;

    ret = alloc_chrdev_region(&kds_dshell_devt, 0, 1, KDS_DSHELL_DEV_NAME);
    if (ret) {
        pr_err("kds_dshell: alloc_chrdev_region failed: %d\n", ret);
        return ret;
    }

    cdev_init(&kds_dshell_cdev, &kds_dshell_fops);
    kds_dshell_cdev.owner = THIS_MODULE;

    ret = cdev_add(&kds_dshell_cdev, kds_dshell_devt, 1);
    if (ret) {
        pr_err("kds_dshell: cdev_add failed: %d\n", ret);
        unregister_chrdev_region(kds_dshell_devt, 1);
        return ret;
    }

    kds_dshell_class = class_create(KDS_DSHELL_DEV_NAME);
    if (IS_ERR(kds_dshell_class)) {
        ret = PTR_ERR(kds_dshell_class);
        pr_err("kds_dshell: class_create failed: %d\n", ret);
        cdev_del(&kds_dshell_cdev);
        unregister_chrdev_region(kds_dshell_devt, 1);
        return ret;
    }

    ret = kds_register_dshell_proc();
    if (ret) {
        class_destroy(kds_dshell_class);
        kds_dshell_class = NULL;
        cdev_del(&kds_dshell_cdev);
        unregister_chrdev_region(kds_dshell_devt, 1);
        return ret;
    }

    dev = device_create(kds_dshell_class, NULL, kds_dshell_devt, NULL,
                         KDS_DSHELL_DEV_NAME);
    if (IS_ERR(dev)) {
        ret = PTR_ERR(dev);
        pr_err("kds_dshell: device_create failed: %d\n", ret);
        kds_proc_unregister(kds_dshell_proc);
        vfree(kds_dshell_proc);
        kds_dshell_proc = NULL;
        class_destroy(kds_dshell_class);
        kds_dshell_class = NULL;
        cdev_del(&kds_dshell_cdev);
        unregister_chrdev_region(kds_dshell_devt, 1);
        return ret;
    }

    kds_dshell_inited = true;
    pr_info("kds_dshell: /dev/%s ready (major=%d minor=%d)\n",
            KDS_DSHELL_DEV_NAME, MAJOR(kds_dshell_devt), MINOR(kds_dshell_devt));
    return 0;
}

void kds_shutdown_dshell_system(void)
{
    kds_dshell_client_t *client;

    if (!kds_dshell_inited)
        return;

    device_destroy(kds_dshell_class, kds_dshell_devt);

    /*
     * Unregister the proc first -- once kds_proc_unregister()
     * returns, the scheduler will never call kds_dshell_proc_run()
     * again, so no new complete() calls will arrive. Then wake any
     * write() still blocked on client->done with a shutdown error.
     */
    if (kds_dshell_proc) {
        kds_proc_unregister(kds_dshell_proc);
        vfree(kds_dshell_proc);
        kds_dshell_proc = NULL;
    }

    spin_lock(&kds_dshell_clients_lock);
    list_for_each_entry(client, &kds_dshell_clients, node) {
        if (client->exec_status == KDS_DSHELL_EXEC_PENDING) {
            client->exec_status = KDS_DSHELL_EXEC_IDLE;
            scnprintf(client->resp_buf, sizeof(client->resp_buf),
                      "ERR dshell shutting down\n");
            complete(&client->done);
        }
    }
    spin_unlock(&kds_dshell_clients_lock);

    class_destroy(kds_dshell_class);
    cdev_del(&kds_dshell_cdev);
    unregister_chrdev_region(kds_dshell_devt, 1);

    kds_dshell_class = NULL;
    kds_dshell_inited = false;

    pr_info("kds_dshell: /dev/%s removed\n", KDS_DSHELL_DEV_NAME);
}