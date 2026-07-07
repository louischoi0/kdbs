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

#define KDS_DSHELL_EXEC_BUF_SIZE  \
    (sizeof(kds_heap_insert_exec_t) > sizeof(kds_btree_insert_exec_t) \
     ? sizeof(kds_heap_insert_exec_t) : sizeof(kds_btree_insert_exec_t))

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
 * Decodes a binary row into a human-readable "col=val, ..." string.
 * Used by kds_cmd_scan_table() to format SELC output.
 */
static int kds_dshell_decode_row(const kds_schema_t *schema, const u8 *buf,
                                  u16 buf_len, char *out, size_t out_size)
{
    size_t off = 0;
    u32    i;
    int    n = 0;

    for (i = 0; i < schema->nr_cols; i++) {
        const kds_sys_column_t *col  = &schema->cols[i];
        const kds_type_desc_t  *desc =
            kds_type_lookup_by_val((kds_type_val_t)col->type_val);
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
     * DB-wide constraint: the first column must be the primary key
     * and must be int64. The parser does not enforce this because it
     * has no knowledge of DB semantics -- enforce it here, at the
     * boundary between parsing and catalog access.
     */
    if (schema->cols[0].type_val != KDS_TYPE_INT64) {
        scnprintf(out, out_size,
                  "ERR first column ('%s') must be the primary key and type int64\n",
                  schema->cols[0].name);
        kfree(schema);
        return -EINVAL;
    }

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
         * Convert the AST value to a string representation that the
         * type descriptor's encode() function can consume. encode()
         * always takes a NUL-terminated string input -- this is the
         * same convention kds_dshell_encode_row() used via argv[].
         */
        {
            char val_str[KDS_PARSER_VAL_MAX];

            switch (val->type) {
            case KDS_VAL_INT:
                scnprintf(val_str, sizeof(val_str), "%lld", (long long)val->int_val);
                break;
            case KDS_VAL_STR:
                strncpy(val_str, val->str_val, sizeof(val_str) - 1);
                val_str[sizeof(val_str) - 1] = '\0';
                break;
            case KDS_VAL_NULL:
                val_str[0] = '\0';
                break;
            default:
                return -EINVAL;
            }

            if (desc->fixed_len == 0) {
                if (off + sizeof(u16) > buf_size)
                    return -ENOSPC;
                ret = desc->encode(val_str, buf + off + sizeof(u16),
                                   buf_size - off - sizeof(u16), &written_len);
                if (ret)
                    return ret;
                memcpy(buf + off, &written_len, sizeof(u16));
                off += sizeof(u16) + written_len;
            } else {
                if (off + desc->fixed_len > buf_size)
                    return -ENOSPC;
                ret = desc->encode(val_str, buf + off,
                                   buf_size - off, &written_len);
                if (ret)
                    return ret;
                off += written_len;
            }
        }
    }

    *out_len = (u16)off;
    return 0;
}

/*
 * Handler for KDS_STMT_INSERT.
 *
 * rel->kind를 조회해서 HEAP이면 kds_heap_insert_exec_t,
 * BTREE이면 kds_btree_insert_exec_t를 사용한다.
 *
 * HEAP: 행 전체를 인코딩해서 heap 페이지에 저장하고
 *       out_tid(page_id + slot)를 응답에 출력한다.
 *
 * BTREE: PK(첫 번째 컬럼, int64)를 key로, 인코딩된 행 전체를
 *        저장할 별도 heap 페이지 id를 value로 삼는다.
 *        현재는 행 데이터를 heap 페이지에 먼저 삽입한 뒤
 *        그 tid.page_id를 btree의 value_page_id로 넣는다.
 *        (heap insert → btree index insert 순서)
 */
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
        /* --------------------------------------------------------
         * HEAP insert: 행 전체를 heap 페이지에 직접 삽입.
         * -------------------------------------------------------- */
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
        /* --------------------------------------------------------
         * BTREE clustered insert:
         *   exec 내부에서 btree 탐색 → 올바른 heap 페이지 선택
         *   → 행 삽입. heap 페이지가 가득 차면 새 페이지 할당 후
         *   btree에 (min_key, new_page_id) 등록까지 처리한다.
         * -------------------------------------------------------- */
        kds_btree_insert_exec_t exec;
        kds_tuple_id_t          pk;

        if (row_len < sizeof(pk)) {
            scnprintf(out, out_size, "ERR row too short to contain PK\n");
            kds_relation_close(rel);
            return -EINVAL;
        }
        memcpy(&pk, row_buf, sizeof(pk));

        kds_btree_insert_exec_init(&exec, rel, pk, row_buf, row_len);

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
                              "ERR duplicate key %lld in '%s'\n",
                              (long long)pk, stmt->table_name);
                else
                    scnprintf(out, out_size,
                              "ERR btree insert failed: %d\n",
                              done->base.ret);
                return done->base.ret;
            }

            scnprintf(out, out_size,
                      "OK inserted into '%s' (btree) key=%lld "
                      "page=%llu slot=%u\n",
                      stmt->table_name, (long long)pk,
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
static int kds_cmd_scan_table(const kds_stmt_select_t *stmt,
                               char *out, size_t out_size)
{
    const char *table_name = stmt->table_name;
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

static int kds_dispatch_stmt(kds_dshell_client_t *client,
                              const kds_stmt_t    *stmt,
                              char *out, size_t out_size)
{
    switch (stmt->type) {
    case KDS_STMT_CREATE_TABLE:
        return kds_cmd_create_table(&stmt->create_table, out, out_size);

    case KDS_STMT_INSERT:
        return kds_cmd_insert_row(client, &stmt->insert, out, out_size);

    case KDS_STMT_SELECT:
        return kds_cmd_scan_table(&stmt->select, out, out_size);

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