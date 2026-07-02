#include <linux/kds.h>
#include <linux/kds_dshell.h>
#include <linux/kds_catalog.h>
#include <linux/kds_types.h>
#include <linux/kds_relation.h>
#include <linux/kds_executor.h>
#include <linux/kds_heap.h>
#include <linux/kds_page_mgr.h>
#include <linux/kds_page_alloc.h>
#include <linux/kds_meta.h>
#include <linux/kds_proc.h>

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

#define KDS_DSHELL_EXEC_BUF_SIZE  sizeof(kds_heap_insert_exec_t)

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
 * CRTB <table_name> [HEAP|BTREE] <col_name>:<type> [<col_name>:<type> ...]
 *
 * Defines a new table in the public namespace. The clustered storage
 * type is optional and defaults to HEAP for backward compatibility
 * with existing scripts that never specified one.
 *
 * <type> is any name registered in kds_types.h's type table
 * (int8/int16/int32/int64/float/decimal/bool/varchar/char) --
 * adding a new type there makes it usable here automatically, no
 * change needed in this file.
 *
 * Disambiguation: argv[2] is treated as the clustered-type keyword
 * only if it has no ':' in it (every column spec is required to be
 * "name:type", so a bare token here can't be a valid column spec).
 * This means a column literally named "HEAP" or "BTREE" without a
 * ':type' suffix would be misread as the keyword -- not a concern in
 * practice since every column spec must carry a type, but worth
 * keeping in mind if this parsing is reused elsewhere.
 *
 * Examples:
 *   CRTB users id:int64 name:varchar active:bool
 *   CRTB users BTREE id:int64 name:varchar active:bool
 *   CRTB users HEAP  id:int64 name:varchar active:bool
 */
static int kds_cmd_create_table(char **argv, int argc, char *out, size_t out_size)
{
    const char  *table_name = argv[1];
    kds_schema_t *schema;
    kd_oid_t     new_oid;
    kds_clustered_type_t clustered_type = KDS_CLUSTERED_HEAP;
    int          col_start = 2;
    int          i;
    int          ret;

    /*
     * Optional clustered-type keyword right after the table name.
     * argv[2] is the keyword candidate only when there are still
     * column specs left after it (argc > 3) and it doesn't contain
     * ':' -- a real column spec always does.
     */
    if (argc > 3 && !strchr(argv[2], ':')) {
        if (!strcasecmp(argv[2], "HEAP")) {
            clustered_type = KDS_CLUSTERED_HEAP;
            col_start = 3;
        } else if (!strcasecmp(argv[2], "BTREE")) {
            clustered_type = KDS_CLUSTERED_BTREE;
            col_start = 3;
        } else {
            scnprintf(out, out_size,
                      "ERR unknown clustered type '%s' (expected HEAP or BTREE)\n",
                      argv[2]);
            return -EINVAL;
        }
    }

    /*
     * kds_schema_t embeds cols[KDS_SCHEMA_MAX_COLUMNS] (32 *
     * sizeof(kds_sys_column_t), ~3KB) -- keeping that on the stack
     * pushed this function's frame to 3040 bytes, well past the
     * kernel's 2048-byte stack-frame warning threshold and a real
     * stack-overflow risk on a call path that's already several
     * frames deep (dshell write handler -> dispatch -> this ->
     * catalog -> heap/btree -> page_mgr -> blkdev). Heap-allocating
     * it keeps this frame small regardless of how big
     * KDS_SCHEMA_MAX_COLUMNS grows in the future.
     */
    schema = kzalloc(sizeof(*schema), GFP_KERNEL);
    if (!schema) {
        scnprintf(out, out_size, "ERR out of memory\n");
        return -ENOMEM;
    }

    for (i = col_start; i < argc; i++) {
        char *spec = argv[i];
        char *col_name;
        char *type_name;
        const kds_type_desc_t *type;
        kds_sys_column_t *col;

        if (schema->nr_cols >= KDS_SCHEMA_MAX_COLUMNS) {
            scnprintf(out, out_size, "ERR too many columns (max %d)\n",
                      KDS_SCHEMA_MAX_COLUMNS);
            kfree(schema);
            return -EINVAL;
        }

        col_name = strsep(&spec, ":");
        type_name = spec;

        if (!type_name || !*type_name) {
            scnprintf(out, out_size, "ERR column '%s' missing :type\n", col_name);
            kfree(schema);
            return -EINVAL;
        }

        type = kds_type_lookup_by_name(type_name);
        if (!type) {
            scnprintf(out, out_size, "ERR unknown column type '%s'\n", type_name);
            kfree(schema);
            return -EINVAL;
        }

        col = &schema->cols[schema->nr_cols];
        memset(col, 0, sizeof(*col));
        col->pos = schema->nr_cols;
        strncpy(col->name, col_name, KDS_CATALOG_NAME_MAX - 1);
        col->name[KDS_CATALOG_NAME_MAX - 1] = '\0';
        col->type_val = type->type_val;
        col->len = type->fixed_len;
        col->notnull = true;

        schema->nr_cols++;
    }

    if (schema->nr_cols == 0) {
        scnprintf(out, out_size, "ERR table must have at least one column\n");
        kfree(schema);
        return -EINVAL;
    }

    /*
     * DB-wide constraint: the first column is always the table's
     * primary key, and always int64. This is purely positional --
     * there is no separate "is_pk" flag on kds_sys_column_t -- so
     * enforcing it here, at the one place tables get defined, is
     * what makes HeapPageInsertExec's duplicate-PK check (see
     * kds_executor.h) able to assume "first 8 bytes = PK" for every
     * heap table without any further bookkeeping. Applies equally
     * to btree-clustered tables, since the leaf-level key is the
     * same PK.
     */
    if (schema->cols[0].type_val != KDS_TYPE_INT64) {
        scnprintf(out, out_size,
                  "ERR first column ('%s') must be the primary key and type int64\n",
                  schema->cols[0].name);
        kfree(schema);
        return -EINVAL;
    }

    ret = kds_catalog_create_table(KDS_OID_NAMESPACE_PUBLIC, table_name, schema,
                                    clustered_type, &new_oid);
    kfree(schema);

    if (ret) {
        scnprintf(out, out_size, "ERR create_table failed: %d\n", ret);
        return ret;
    }

    scnprintf(out, out_size,
              "OK table '%s' created, oid=%llu, columns=%u, clustered=%s\n",
              table_name, (u64)new_oid, schema->nr_cols,
              clustered_type == KDS_CLUSTERED_BTREE ? "BTREE" : "HEAP");
    return 0;
}

/*
 * Row encoding used by INRW/SELC below -- there is no general
 * StructuredTuple-style codec on the C side yet (unlike the Python
 * POC), so this delegates per-column encode/decode entirely to
 * kds_types.h's registry: fixed-width types (int8/16/32/64/float/
 * decimal/bool) are stored back-to-back at their known fixed_len,
 * no length prefix needed; variable-width types (varchar/char) get
 * an explicit u16 length prefix ahead of their encoded bytes, same
 * as before.
 *
 * KNOWN LIMITATIONS (deliberate, to keep this a heap.c validation
 * tool rather than a real access method):
 *   - Only the table's single root heap page is touched
 *     (kds_relation_t.root_page_id) -- no multi-page table growth.
 *   - xmin is stamped with a fixed placeholder (KDS_DSHELL_XID)
 *     since there is no transaction manager yet to hand out real
 *     transaction ids.
 */
#define KDS_DSHELL_XID      1
#define KDS_DSHELL_ROW_MAX  256



static int kds_dshell_encode_row(const kds_schema_t *schema, char **argv,
                                  u8 *buf, size_t buf_size, u16 *out_len)
{
    size_t off = 0;
    u32 i;

    for (i = 0; i < schema->nr_cols; i++) {
        const kds_sys_column_t *col = &schema->cols[i];
        const char *val = argv[2 + i];
        const kds_type_desc_t *desc = kds_type_lookup_by_val((kds_type_val_t)col->type_val);
        u16 written_len;
        int ret;

        if (!desc)
            return -EINVAL;

        if (desc->fixed_len == 0) {
            /* Variable length: u16 length prefix, then the encoded
             * payload right after it. */
            if (off + sizeof(u16) > buf_size)
                return -ENOSPC;

            ret = desc->encode(val, buf + off + sizeof(u16),
                                buf_size - off - sizeof(u16), &written_len);
            if (ret)
                return ret;

            memcpy(buf + off, &written_len, sizeof(u16));
            off += sizeof(u16) + written_len;
        } else {
            if (off + desc->fixed_len > buf_size)
                return -ENOSPC;

            ret = desc->encode(val, buf + off, buf_size - off, &written_len);
            if (ret)
                return ret;

            off += written_len; /* == desc->fixed_len, by the type's own contract */
        }
    }

    *out_len = (u16)off;
    return 0;
}

static int kds_dshell_decode_row(const kds_schema_t *schema, const u8 *buf,
                                  u16 buf_len, char *out, size_t out_size)
{
    size_t off = 0;
    u32 i;
    int n = 0;

    for (i = 0; i < schema->nr_cols; i++) {
        const kds_sys_column_t *col = &schema->cols[i];
        const kds_type_desc_t *desc = kds_type_lookup_by_val((kds_type_val_t)col->type_val);
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

        if ((u32)off + value_len > buf_len)
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

/*
 * INRW <table_name> <val1> <val2> ... (one value per schema column,
 * in column order)
 *
 * Drives a kds_heap_insert_exec_t via the proc scheduler:
 *   1. Parse + validate inputs synchronously in write() context.
 *   2. Initialise the exec into client->exec_buf.
 *   3. Hand off to kds_dshell_submit_insert_exec(), which marks the
 *      client EXEC_PENDING and blocks until kds_dshell_proc_run()
 *      finishes the work slice-by-slice.
 *   4. On return, format the response into `out`.
 *
 * row_buf is stack-allocated here; the exec's data pointer points
 * into it, which is safe because this function doesn't return until
 * kds_dshell_submit_insert_exec() comes back (i.e. the exec is fully
 * complete before the stack frame is torn down).
 */
static int kds_cmd_insert_row(kds_dshell_client_t *client,
                               char **argv, int argc,
                               char *out, size_t out_size)
{
    const char *table_name = argv[1];
    kd_oid_t oid;
    kds_relation_t *rel;
    u8 row_buf[KDS_DSHELL_ROW_MAX];
    u16 row_len;
    kds_heap_insert_exec_t exec;
    int ret;

    ret = kds_catalog_find_table_oid_by_name(table_name, &oid);
    if (ret) {
        scnprintf(out, out_size, "ERR table '%s' not found: %d\n", table_name, ret);
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
                  "ERR '%s' is not heap-clustered or btree-clustered\n", table_name);
        kds_relation_close(rel);
        return -EINVAL;
    }

    if ((u32)(argc - 2) != rel->schema.nr_cols) {
        scnprintf(out, out_size, "ERR '%s' has %u column(s), got %d value(s)\n",
                  table_name, rel->schema.nr_cols, argc - 2);
        kds_relation_close(rel);
        return -EINVAL;
    }

    ret = kds_dshell_encode_row(&rel->schema, argv, row_buf, sizeof(row_buf), &row_len);
    if (ret) {
        scnprintf(out, out_size, "ERR failed to encode row (too long or bad value)\n");
        kds_relation_close(rel);
        return ret;
    }

    kds_heap_insert_exec_init(&exec, rel, row_buf, row_len, KDS_DSHELL_XID);

    /*
     * Submit exec to kds_dshell_proc_run() and block until done.
     * The proc slices the work with the scheduler's own slice_ns
     * budget, so INRW on a large table no longer monopolises the CPU.
     * resp_buf is written here (not in proc_run) for the success path:
     * kds_dshell_submit_insert_exec() returns only after the exec has
     * reached DONE or ERROR, so exec.out_tid is valid by the time we
     * read it below.
     */
    ret = kds_dshell_submit_insert_exec(client, &exec);
    if (ret == -ETIMEDOUT) {
        /* resp_buf already filled by the timeout handler */
        kds_relation_close(rel);
        return ret;
    }

    kds_relation_close(rel);

    /*
     * exec_buf holds the completed kds_heap_insert_exec_t; cast back
     * to read out_tid and base.ret (proc_run wrote ERR response for
     * ERROR, so we only need to handle DONE here).
     */
    {
        kds_heap_insert_exec_t *done_exec =
            (kds_heap_insert_exec_t *)client->exec_buf;

        if (done_exec->base.ret != 0) {
            if (done_exec->base.ret == -EEXIST) {
                scnprintf(out, out_size,
                          "ERR duplicate primary key in '%s'\n", table_name);
            } else {
                scnprintf(out, out_size,
                          "ERR insert failed: %d\n", done_exec->base.ret);
            }
            return done_exec->base.ret;
        }

        scnprintf(out, out_size,
                  "OK inserted into '%s' at page=%llu slot=%u\n",
                  table_name,
                  (u64)done_exec->out_tid.page_id,
                  done_exec->out_tid.slot);
    }

    return 0;
}

/*
 * SELC <table_name> -- scans and prints every live tuple in the
 * table's single heap page.
 */
/*
 * SELC <table_name> -- scans and prints every live tuple in the
 * table's heap page chain: starts at the table's root page
 * (kds_relation_t.root_page_id) and follows next_page_id (heap.h)
 * forward until it hits 0 (end of chain). This is a full scan with
 * no index of any kind -- there's no way to jump directly to a
 * particular page, only to walk the whole chain start to finish.
 */
static int kds_cmd_scan_table(char **argv, int argc, char *out, size_t out_size)
{
    const char *table_name = argv[1];
    kd_oid_t oid;
    kds_relation_t *rel;
    kds_page_id_t current_page_id;
    u32 page_index = 0;
    u32 total_slots = 0;
    int n;
    int ret;
    bool truncated = false;

    ret = kds_catalog_find_table_oid_by_name(table_name, &oid);
    if (ret) {
        scnprintf(out, out_size, "ERR table '%s' not found: %d\n", table_name, ret);
        return ret;
    }

    rel = kds_relation_open(oid);
    if (IS_ERR(rel)) {
        ret = PTR_ERR(rel);
        scnprintf(out, out_size, "ERR relation_open failed: %d\n", ret);
        return ret;
    }

    if (rel->kind != KDS_CLUSTERED_HEAP) {
        scnprintf(out, out_size, "ERR '%s' is not heap-clustered\n", table_name);
        kds_relation_close(rel);
        return -EINVAL;
    }

    n = scnprintf(out, out_size, "OK ");
    current_page_id = rel->root_page_id;

    while (current_page_id != 0 && !truncated) {
        kds_frame_t *frame;
        u16 nr_slots, slot;

        frame = kds_buf_lookup_or_load(current_page_id);
        if (IS_ERR(frame)) {
            n += scnprintf(out + n, out_size - n,
                            "\nERR lookup_or_load(page %llu) failed: %ld\n",
                            (u64)current_page_id, PTR_ERR(frame));
            kds_relation_close(rel);
            return PTR_ERR(frame);
        }

        nr_slots = heap_nr_slots(frame);

        for (slot = 0; slot < nr_slots; slot++) {
            kds_heap_tuple_hdr_t hdr;
            u8 row_buf[KDS_DSHELL_ROW_MAX];
            int r;

            r = heap_read_tuple(frame, slot, &hdr, row_buf, sizeof(row_buf));
            if (r == -ENOENT)
                continue; /* dead slot */
            if (r)
                break;

            if ((size_t)n + 16 >= out_size) {
                n += scnprintf(out + n, out_size - n, "...(truncated)\n");
                truncated = true;
                break;
            }

            n += scnprintf(out + n, out_size - n,
                            "\n  page=%llu [%u] xmin=%llu xmax=%llu ",
                            (u64)current_page_id, slot, hdr.xmin, hdr.xmax);
            n += kds_dshell_decode_row(&rel->schema, row_buf, hdr.data_len,
                                        out + n, out_size - n);
            total_slots++;
        }

        current_page_id = truncated ? 0 : heap_get_next_page_id(frame);
        kds_buf_unpin(frame);
        page_index++;
    }

    kds_relation_close(rel);

    if (!truncated)
        n += scnprintf(out + n, out_size - n,
                        "\n%u row(s) across %u page(s)\n", total_slots, page_index);

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
static int kds_cmd_show_meta(char **argv, int argc, char *out, size_t out_size)
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
static int kds_cmd_show_page_alloc_status(char **argv, int argc, char *out, size_t out_size)
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
 * Command table -- see kds_dshell.h for how to add a new command.
 * ------------------------------------------------------------------ */

static const kds_dshell_cmd_t kds_dshell_cmds[] = {
    { "CRTB", kds_cmd_create_table,           3 },
    { "SELC", kds_cmd_scan_table,             2 },
    { "META", kds_cmd_show_meta,              1 },
    { "PSTA", kds_cmd_show_page_alloc_status, 1 },
};

#define KDS_DSHELL_CMD_COUNT \
    (sizeof(kds_dshell_cmds) / sizeof(kds_dshell_cmds[0]))

/*
 * Dispatches one already-tokenized command line. Always produces a
 * response in `out` -- either the handler's own output, or a
 * generic "unknown command"/"too few arguments" message if dispatch
 * itself couldn't even reach a handler.
 */
/*
 * The dispatch table fn signature does not carry a client pointer
 * because most handlers (META, PSTA, CRTB, SELC) don't need it.
 * Only INRW needs to submit an exec to the proc, and it gets the
 * client pointer via the dedicated kds_dshell_exec_cmd_t entry type
 * below. To keep the common path simple, handlers that need a client
 * are registered in kds_dshell_exec_cmds[] (separate table) and
 * checked first in dispatch.
 */
typedef int (*kds_dshell_exec_fn)(kds_dshell_client_t *client,
                                   char **argv, int argc,
                                   char *out, size_t out_size);

typedef struct {
    const char          *op;
    kds_dshell_exec_fn   fn;
    int                  min_args;
} kds_dshell_exec_cmd_t;

static const kds_dshell_exec_cmd_t kds_dshell_exec_cmds[] = {
    { "INRW", kds_cmd_insert_row, 3 },
};

#define KDS_DSHELL_EXEC_CMD_COUNT \
    (sizeof(kds_dshell_exec_cmds) / sizeof(kds_dshell_exec_cmds[0]))

static int __kds_dispatch_dshell_cmd(kds_dshell_client_t *client,
                                      char **argv, int argc,
                                      char *out, size_t out_size)
{
    u32 i;

    if (argc < 1) {
        scnprintf(out, out_size, "ERR empty command\n");
        return -EINVAL;
    }

    /* Check exec-capable commands first (need client pointer). */
    for (i = 0; i < KDS_DSHELL_EXEC_CMD_COUNT; i++) {
        if (strcmp(argv[0], kds_dshell_exec_cmds[i].op))
            continue;

        if (argc < kds_dshell_exec_cmds[i].min_args) {
            scnprintf(out, out_size, "ERR %s requires at least %d arg(s), got %d\n",
                      argv[0], kds_dshell_exec_cmds[i].min_args, argc);
            return -EINVAL;
        }

        return kds_dshell_exec_cmds[i].fn(client, argv, argc, out, out_size);
    }

    /* Then lightweight commands (no client pointer needed). */
    for (i = 0; i < KDS_DSHELL_CMD_COUNT; i++) {
        if (strcmp(argv[0], kds_dshell_cmds[i].op))
            continue;

        if (argc < kds_dshell_cmds[i].min_args) {
            scnprintf(out, out_size, "ERR %s requires at least %d arg(s), got %d\n",
                      argv[0], kds_dshell_cmds[i].min_args, argc);
            return -EINVAL;
        }

        return kds_dshell_cmds[i].fn(argv, argc, out, out_size);
    }

    scnprintf(out, out_size, "ERR unknown command: %s\n", argv[0]);
    return -ENOENT;
}

int kds_split_cmd(char *buf, char *argv[], int max_args)
{
    int argc = 0;
    char *token;

    if (!buf || !argv || max_args <= 0)
        return -EINVAL;

    while ((token = strsep(&buf, " \t\n")) != NULL) {
        if (*token == '\0')
            continue;

        argv[argc++] = token;
        if (argc >= max_args)
            break;
    }

    return argc;
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
    char cmd_buf[DSHELL_LINE_MAX];
    char *argv[DSHELL_MAX_ARGS];
    size_t n;
    int argc;

    if (!client)
        return -EINVAL;

    n = min(len, sizeof(cmd_buf) - 1);
    if (copy_from_user(cmd_buf, ubuf, n))
        return -EFAULT;
    cmd_buf[n] = '\0';

    /* Reset response staging for this new command. */
    client->resp_buf[0] = '\0';
    client->resp_len = 0;
    client->resp_pos = 0;

    argc = kds_split_cmd(cmd_buf, argv, DSHELL_MAX_ARGS);
    if (argc < 0) {
        scnprintf(client->resp_buf, sizeof(client->resp_buf),
                  "ERR failed to parse command\n");
    } else {
        __kds_dispatch_dshell_cmd(client, argv, argc,
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