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
#include <linux/kref.h>
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
 * /dev/kds is a synchronous request/response character device:
 * one write() = one command, the following read() = that command's
 * response. There is no background polling process and no shared
 * ring buffer anymore -- a client's write()/read() pair maps
 * directly onto one command dispatch, which is what makes this a
 * usable RPC channel (the previous shm-ring version had no response
 * path at all; userspace just guessed "OK").
 *
 * Concurrency: each open() gets its own kds_dshell_client_t in
 * file->private_data, so multiple clients (multiple opens of
 * /dev/kds) are independent of each other. A single client's own
 * write()-then-read() is expected to be used sequentially (no
 * internal locking against a client racing itself); cross-client
 * state lives only in the command handlers themselves (e.g. the
 * catalog layer's own locking), not in this file.
 */

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

/*
 * Fixed per-call time budget handed to kds_exec_run() from dshell
 * command handlers. 50ms is generous relative to a single page
 * visit (page lookup/load + a handful of tuple reads) -- chosen so
 * that small-to-medium tables finish their entire insert (dup scan
 * + tail find) in one call, and only pathologically large tables
 * (many thousands of pages in the duplicate-PK scan) ever observe
 * KDS_EXEC_CONTINUE here. Not tied to kds_proc.h's own slice
 * constants -- dshell does not go through the proc scheduler at all
 * right now (see the file-level comment above), so this is purely a
 * local "don't hog the CPU on one syscall for an unbounded amount of
 * time" cap, not a scheduling-fairness budget.
 */
#define KDS_DSHELL_EXEC_SLICE_NS  (50ULL * NSEC_PER_MSEC)

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
 */
static int kds_cmd_insert_row(char **argv, int argc, char *out, size_t out_size)
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
        scnprintf(out, out_size, "ERR '%s' is not heap-clustered or btree-clustrered\n", table_name);
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

    /*
     * HeapPageInsertExec owns the "page full -> allocate a new page
     * and link it onto the chain" decision now -- this handler no
     * longer touches kds_buf_lookup_or_load()/heap_insert_tuple()
     * directly, matching the new executor layer's responsibility
     * split (see kds_executor.h).
     */
    kds_heap_insert_exec_init(&exec, rel, row_buf, row_len, KDS_DSHELL_XID);

    /*
     * dshell handlers run synchronously inside the write() syscall
     * (no proc-scheduler hop -- see the file-level comment above),
     * so there is no outer slice budget being handed down here.
     * KDS_DSHELL_EXEC_SLICE_NS is a generous fixed budget chosen so
     * that the overwhelming majority of inserts (small tables, no
     * PK-scan blowup yet) finish in a single kds_exec_run() call --
     * CONTINUE is the exceptional path, not the common one. When it
     * does come back CONTINUE (a large table's duplicate-PK scan
     * spanning many pages), this loop just calls back in
     * immediately with a fresh budget rather than returning
     * partial progress to the client -- INRW's contract is still
     * "one write() = one finished command", same as every other
     * dshell command; only the *internal* CPU usage is now sliced,
     * not the user-visible request/response shape.
     */
    while (kds_exec_run(&exec.base, KDS_DSHELL_EXEC_SLICE_NS) == KDS_EXEC_CONTINUE)
        ; /* exec carries all resume state in itself -- nothing to do here */

    if (exec.base.ret != 0) {
        if (exec.base.ret == -EEXIST) {
            scnprintf(out, out_size,
                      "ERR duplicate primary key in '%s'\n", table_name);
        } else {
            scnprintf(out, out_size, "ERR insert failed: %d\n", exec.base.ret);
        }
        kds_relation_close(rel);
        return exec.base.ret;
    }

    kds_relation_close(rel);

    scnprintf(out, out_size, "OK inserted into '%s' at page=%llu slot=%u\n",
              table_name, (u64)exec.out_tid.page_id, exec.out_tid.slot);
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
    { "CRTB", kds_cmd_create_table,          3 },
    { "INRW", kds_cmd_insert_row,            3 },
    { "SELC", kds_cmd_scan_table,            2 },
    { "META", kds_cmd_show_meta,             1 },
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
static int __kds_dispatch_dshell_cmd(char **argv, int argc, char *out, size_t out_size)
{
    u32 i;

    if (argc < 1) {
        scnprintf(out, out_size, "ERR empty command\n");
        return -EINVAL;
    }

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

#define KDS_DSHELL_DEV_NAME "kds"

typedef struct kds_dshell_client {
    char    cmd_buf[DSHELL_LINE_MAX];
    char    resp_buf[DSHELL_RESP_MAX];
    size_t  resp_len;
    size_t  resp_pos;
} kds_dshell_client_t;

/*
 * A queued command request. Heap-allocated and refcounted (kref),
 * NOT stack-allocated, for a specific reason: write() waits on
 * req->done with a *timeout* (kds_dshell_write() below), and if that
 * wait times out, write() must be able to give up and return without
 * the kds_proc consumer (kds_dshell_proc_run()) crashing if it's
 * already holding a pointer to req and is mid-way through filling in
 * resp_buf. A stack-allocated struct would make that a use-after-
 * return memory corruption bug the moment the timeout path returns
 * while the consumer is still touching it.
 *
 * With kref: write() holds one reference, the queue (and whoever
 * dequeues it) holds the other. Whichever side finishes last is the
 * one that actually frees it -- safe regardless of which side gives
 * up or finishes first.
 */
typedef struct kds_dshell_request {
    struct kref         kref;
    char                cmd_buf[DSHELL_LINE_MAX];
    char                resp_buf[DSHELL_RESP_MAX];
    struct completion   done;
    struct list_head    queue_node;
} kds_dshell_request_t;

#define KDS_DSHELL_REQUEST_TIMEOUT_MS  5000

static LIST_HEAD(kds_dshell_queue);
static DEFINE_SPINLOCK(kds_dshell_queue_lock);

static dev_t kds_dshell_devt;
static struct cdev kds_dshell_cdev;
static struct class *kds_dshell_class;
static bool kds_dshell_inited;
static kds_proc_t *kds_dshell_proc;

static void kds_dshell_request_release(struct kref *kref)
{
    kds_dshell_request_t *req = container_of(kref, kds_dshell_request_t, kref);

    kfree(req);
}

static int kds_dshell_open(struct inode *inode, struct file *filp)
{
    kds_dshell_client_t *client = kzalloc(sizeof(*client), GFP_KERNEL);

    if (!client)
        return -ENOMEM;

    filp->private_data = client;
    return 0;
}

static int kds_dshell_release(struct inode *inode, struct file *filp)
{
    kfree(filp->private_data);
    filp->private_data = NULL;
    return 0;
}

/*
 * Drains every request currently on the queue, running each one
 * through the same parse+dispatch path the old synchronous write()
 * handler used to run inline. This is what makes dshell command
 * execution happen on the scheduler's terms (this function only
 * runs when kds_proc_schedule() picks kds_dshell_proc, exactly like
 * any other system proc) instead of directly in whatever process
 * context called write().
 *
 * Drains the whole queue per call rather than capping how many
 * requests it services -- simplest behavior, and fine for the
 * expected dshell load (a debug/admin channel, not a hot data path).
 * If this ever needs to yield mid-drain to be fairer to other system
 * procs sharing the CPU, that's a straightforward follow-up (check
 * an elapsed-time budget against slice_ns inside the loop).
 */
static kds_proc_result_t kds_dshell_proc_run(kds_proc_t *proc, u64 slice_ns)
{
    kds_dshell_request_t *req;
    char *argv[DSHELL_MAX_ARGS];
    int argc;

    for (;;) {
        spin_lock(&kds_dshell_queue_lock);
        if (list_empty(&kds_dshell_queue)) {
            spin_unlock(&kds_dshell_queue_lock);
            break;
        }
        req = list_first_entry(&kds_dshell_queue, kds_dshell_request_t, queue_node);
        /* list_del_init(), not list_del() -- kds_dshell_write()'s
         * timeout path checks list_empty(&req->queue_node) to tell
         * whether this request is still queued (and therefore safe
         * for it to remove and reclaim itself) versus already
         * dequeued here (in which case it must back off and leave
         * req alone). list_del_init() is what makes that check mean
         * what it needs to mean. */
        list_del_init(&req->queue_node);
        spin_unlock(&kds_dshell_queue_lock);

        argc = kds_split_cmd(req->cmd_buf, argv, DSHELL_MAX_ARGS);
        if (argc < 0) {
            scnprintf(req->resp_buf, sizeof(req->resp_buf),
                      "ERR failed to parse command\n");
        } else {
            __kds_dispatch_dshell_cmd(argv, argc, req->resp_buf, sizeof(req->resp_buf));
        }

        /* Signal completion before dropping the queue's reference --
         * order doesn't actually matter for correctness here (kref
         * guarantees req isn't freed until both sides have dropped
         * their reference, regardless of order), but completing
         * first means a waiter that's been timing out right at this
         * instant has the best chance of seeing its real response
         * instead of racing a timeout. */
        complete(&req->done);
        kref_put(&req->kref, kds_dshell_request_release);
    }

    return KDS_PROC_YIELD_RET;
}

/*
 * Enqueues the command for kds_dshell_proc_run() to service on its
 * own schedule, then blocks (with a bounded timeout, not forever)
 * until that happens. write() itself does no command parsing or
 * dispatch anymore -- it only builds the request and waits.
 */
static ssize_t kds_dshell_write(struct file *filp, const char __user *ubuf,
                                 size_t len, loff_t *off)
{
    kds_dshell_client_t *client = filp->private_data;
    kds_dshell_request_t *req;
    size_t n;
    long wait_ret;

    if (!client)
        return -EINVAL;

    req = kzalloc(sizeof(*req), GFP_KERNEL);
    if (!req)
        return -ENOMEM;

    n = min(len, sizeof(req->cmd_buf) - 1);

    if (copy_from_user(req->cmd_buf, ubuf, n)) {
        kfree(req);
        return -EFAULT;
    }
    req->cmd_buf[n] = '\0';

    kref_init(&req->kref);          /* refcount = 1, this call's reference */
    init_completion(&req->done);
    INIT_LIST_HEAD(&req->queue_node);

    kref_get(&req->kref);            /* refcount = 2, the queue/consumer's reference */
    spin_lock(&kds_dshell_queue_lock);
    list_add_tail(&req->queue_node, &kds_dshell_queue);
    spin_unlock(&kds_dshell_queue_lock);

    wait_ret = wait_for_completion_timeout(&req->done,
                    msecs_to_jiffies(KDS_DSHELL_REQUEST_TIMEOUT_MS));

    if (wait_ret == 0) {
        /* Timed out. Try to cancel by removing it from the queue
         * ourselves -- only safe if it's still sitting there
         * untouched. */
        bool still_queued;

        spin_lock(&kds_dshell_queue_lock);
        still_queued = !list_empty(&req->queue_node);
        if (still_queued)
            list_del_init(&req->queue_node);
        spin_unlock(&kds_dshell_queue_lock);

        if (still_queued) {
            /* We successfully cancelled it -- drop the queue's
             * reference ourselves, since kds_dshell_proc_run() will
             * never see this request now. */
            kref_put(&req->kref, kds_dshell_request_release);
        }
        /* Either way, do not touch req->resp_buf -- if it wasn't
         * still queued, kds_dshell_proc_run() dequeued it sometime
         * between our timeout firing and this check, and may be
         * writing into it right now. */

        scnprintf(client->resp_buf, sizeof(client->resp_buf),
                  "ERR request timed out after %dms waiting for dshell scheduler\n",
                  KDS_DSHELL_REQUEST_TIMEOUT_MS);
    } else {
        memcpy(client->resp_buf, req->resp_buf, sizeof(client->resp_buf));
    }

    kref_put(&req->kref, kds_dshell_request_release); /* drop this call's reference */

    client->resp_len = strnlen(client->resp_buf, sizeof(client->resp_buf));
    client->resp_pos = 0;

    return len;
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
    return n;
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

    kds_dshell_proc->kind = KDS_PROC_SYSTEM;
    kds_dshell_proc->name = "kds_dshell_proc";
    kds_dshell_proc->static_prio = -1;
    kds_dshell_proc->dynamic_prio = KDS_PROC_PRIORITY_SYSTEM_BACKGROUND;
    kds_dshell_proc->run = kds_dshell_proc_run;
    kds_dshell_proc->ctx = NULL;
    kds_dshell_proc->state = KDS_PROC_STATE_READY;

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
    kds_dshell_request_t *req, *tmp;
    LIST_HEAD(local_queue);

    if (!kds_dshell_inited)
        return;

    device_destroy(kds_dshell_class, kds_dshell_devt);

    /*
     * Unregister the proc BEFORE draining the queue -- once this
     * returns, the scheduler will never call kds_dshell_proc_run()
     * again, so anything still on the queue at this point is
     * guaranteed to stay untouched by it. Without this ordering, a
     * concurrent schedule() on another CPU could still be draining
     * the queue while we're also draining it below -- two drainers
     * racing on the same list.
     */
    if (kds_dshell_proc) {
        kds_proc_unregister(kds_dshell_proc);
        vfree(kds_dshell_proc);
        kds_dshell_proc = NULL;
    }

    /*
     * Wake up any writer still blocked in wait_for_completion_timeout()
     * with an explicit shutdown error, instead of making them wait
     * out the full timeout during module removal.
     */
    spin_lock(&kds_dshell_queue_lock);
    list_splice_init(&kds_dshell_queue, &local_queue);
    spin_unlock(&kds_dshell_queue_lock);

    list_for_each_entry_safe(req, tmp, &local_queue, queue_node) {
        list_del_init(&req->queue_node);
        scnprintf(req->resp_buf, sizeof(req->resp_buf),
                  "ERR dshell shutting down\n");
        complete(&req->done);
        kref_put(&req->kref, kds_dshell_request_release);
    }

    class_destroy(kds_dshell_class);
    cdev_del(&kds_dshell_cdev);
    unregister_chrdev_region(kds_dshell_devt, 1);

    kds_dshell_class = NULL;
    kds_dshell_inited = false;

    pr_info("kds_dshell: /dev/%s removed\n", KDS_DSHELL_DEV_NAME);
}