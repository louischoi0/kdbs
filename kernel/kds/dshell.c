#include <linux/kds.h>
#include <linux/kds_dshell.h>
#include <linux/kds_catalog.h>
#include <linux/kds_relation.h>
#include <linux/kds_heap.h>
#include <linux/kds_page_mgr.h>
#include <linux/kds_meta.h>

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/printk.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
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
 * CRTB <table_name> <col_name>:<type> [<col_name>:<type> ...]
 *
 * Defines a new heap-clustered table in the public namespace.
 * Supported <type> values: int, varchar, char, bool (matching the
 * scalar types kds_catalog_bootstrap() registers in sys.types).
 *
 * btree-clustered tables are deliberately not exposed here -- this
 * command exists to exercise and validate the heap page layer in
 * isolation first, per the current focus; clustered_type is hardcoded
 * to KDS_CLUSTERED_HEAP.
 *
 * Example: CRTB users id:int name:varchar active:bool
 */
struct kds_dshell_type_entry {
    const char  *name;
    u32         type_val;
    u32         len;
};

static const struct kds_dshell_type_entry kds_dshell_types[] = {
    { "int",     0, sizeof(s64) },
    { "varchar", 1, 0 },
    { "char",    2, 0 },
    { "bool",    3, sizeof(u8) },
};

static const struct kds_dshell_type_entry *kds_dshell_find_type(const char *name)
{
    u32 i;

    for (i = 0; i < ARRAY_SIZE(kds_dshell_types); i++) {
        if (!strcmp(kds_dshell_types[i].name, name))
            return &kds_dshell_types[i];
    }
    return NULL;
}

static int kds_cmd_create_table(char **argv, int argc, char *out, size_t out_size)
{
    pr_info("kds cmd create table");
    const char  *table_name = argv[1];
    kds_schema_t *schema;
    kd_oid_t     new_oid;
    int          i;
    int          ret;

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

    for (i = 2; i < argc; i++) {
        char *spec = argv[i];
        char *col_name;
        char *type_name;
        const struct kds_dshell_type_entry *type;
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

        type = kds_dshell_find_type(type_name);
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
        col->len = type->len;
        col->notnull = true;

        schema->nr_cols++;
    }

    if (schema->nr_cols == 0) {
        scnprintf(out, out_size, "ERR table must have at least one column\n");
        kfree(schema);
        return -EINVAL;
    }

    ret = kds_catalog_create_table(KDS_OID_NAMESPACE_PUBLIC, table_name, schema,
                                    KDS_CLUSTERED_HEAP, &new_oid);
    kfree(schema);

    if (ret) {
        scnprintf(out, out_size, "ERR create_table failed: %d\n", ret);
        return ret;
    }

    scnprintf(out, out_size, "OK table '%s' created, oid=%llu, columns=%u\n",
              table_name, (u64)new_oid, schema->nr_cols);
    return 0;
}

/*
 * Row encoding used by INRW/SELC below -- there is no general
 * StructuredTuple-style codec on the C side yet (unlike the Python
 * POC), so this is a minimal, self-describing format just for
 * exercising heap.c:
 *
 *   int     -> 8 bytes, s64, native endianness
 *   bool    -> 1 byte, 0 or 1
 *   varchar/char -> u16 length prefix + that many raw bytes (no
 *                   embedded NUL assumed; column value is whatever
 *                   the command-line token contained)
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

        switch (col->type_val) {
        case 0: { /* int */
            s64 v;
            int ret = kstrtos64(val, 10, &v);

            if (ret || off + sizeof(v) > buf_size)
                return -EINVAL;
            memcpy(buf + off, &v, sizeof(v));
            off += sizeof(v);
            break;
        }
        case 3: { /* bool */
            u8 v = (!strcmp(val, "1") || !strcmp(val, "true")) ? 1 : 0;

            if (off + sizeof(v) > buf_size)
                return -EINVAL;
            buf[off++] = v;
            break;
        }
        default: { /* varchar / char */
            size_t len = strlen(val);
            u16 len16;

            if (len > U16_MAX || off + sizeof(len16) + len > buf_size)
                return -EINVAL;
            len16 = (u16)len;
            memcpy(buf + off, &len16, sizeof(len16));
            off += sizeof(len16);
            memcpy(buf + off, val, len);
            off += len;
            break;
        }
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

        if (i > 0)
            n += scnprintf(out + n, out_size - n, ", ");

        switch (col->type_val) {
        case 0: { /* int */
            s64 v;

            if (off + sizeof(v) > buf_len)
                return -EINVAL;
            memcpy(&v, buf + off, sizeof(v));
            off += sizeof(v);
            n += scnprintf(out + n, out_size - n, "%s=%lld", col->name, v);
            break;
        }
        case 3: { /* bool */
            u8 v;

            if (off + sizeof(v) > buf_len)
                return -EINVAL;
            v = buf[off++];
            n += scnprintf(out + n, out_size - n, "%s=%s", col->name,
                            v ? "true" : "false");
            break;
        }
        default: { /* varchar / char */
            u16 len;

            if (off + sizeof(len) > buf_len)
                return -EINVAL;
            memcpy(&len, buf + off, sizeof(len));
            off += sizeof(len);

            if (off + len > buf_len)
                return -EINVAL;

            n += scnprintf(out + n, out_size - n, "%s=", col->name);
            if (n + len < out_size) {
                memcpy(out + n, buf + off, len);
                n += len;
                out[n] = '\0';
            }
            off += len;
            break;
        }
        }
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
    kds_frame_t *frame;
    u8 row_buf[KDS_DSHELL_ROW_MAX];
    u16 row_len;
    kds_heap_tid_t tid;
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

    if (rel->kind != KDS_CLUSTERED_HEAP) {
        scnprintf(out, out_size, "ERR '%s' is not heap-clustered\n", table_name);
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

    frame = kds_buf_lookup_or_load(rel->root_page_id);
    if (IS_ERR(frame)) {
        ret = PTR_ERR(frame);
        scnprintf(out, out_size, "ERR lookup_or_load failed: %d\n", ret);
        kds_relation_close(rel);
        return ret;
    }

    ret = heap_insert_tuple(frame, row_buf, row_len, KDS_DSHELL_XID, &tid);
    kds_buf_unpin(frame);
    kds_relation_close(rel);

    if (ret) {
        scnprintf(out, out_size, "ERR heap_insert_tuple failed: %d\n", ret);
        return ret;
    }

    scnprintf(out, out_size, "OK inserted into '%s' at slot=%u\n", table_name, tid.slot);
    return 0;
}

/*
 * SELC <table_name> -- scans and prints every live tuple in the
 * table's single heap page.
 */
static int kds_cmd_scan_table(char **argv, int argc, char *out, size_t out_size)
{
    const char *table_name = argv[1];
    kd_oid_t oid;
    kds_relation_t *rel;
    kds_frame_t *frame;
    u16 nr_slots, slot;
    int n;
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

    if (rel->kind != KDS_CLUSTERED_HEAP) {
        scnprintf(out, out_size, "ERR '%s' is not heap-clustered\n", table_name);
        kds_relation_close(rel);
        return -EINVAL;
    }

    frame = kds_buf_lookup_or_load(rel->root_page_id);
    if (IS_ERR(frame)) {
        ret = PTR_ERR(frame);
        scnprintf(out, out_size, "ERR lookup_or_load failed: %d\n", ret);
        kds_relation_close(rel);
        return ret;
    }

    nr_slots = heap_nr_slots(frame);
    n = scnprintf(out, out_size, "OK %u slot(s)\n", nr_slots);

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
            break;
        }

        n += scnprintf(out + n, out_size - n, "  [%u] xmin=%llu xmax=%llu ",
                        slot, hdr.xmin, hdr.xmax);
        n += kds_dshell_decode_row(&rel->schema, row_buf, hdr.data_len,
                                    out + n, out_size - n);
        n += scnprintf(out + n, out_size - n, "\n");
    }

    kds_buf_unpin(frame);
    kds_relation_close(rel);
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

/* ------------------------------------------------------------------
 * Command table -- see kds_dshell.h for how to add a new command.
 * ------------------------------------------------------------------ */

static const kds_dshell_cmd_t kds_dshell_cmds[] = {
    { "CRTB", kds_cmd_create_table, 3 },
    { "INRW", kds_cmd_insert_row,   3 },
    { "SELC", kds_cmd_scan_table,   2 },
    { "META", kds_cmd_show_meta,    1 },
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

static dev_t kds_dshell_devt;
static struct cdev kds_dshell_cdev;
static struct class *kds_dshell_class;
static bool kds_dshell_inited;

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
 * One write() = one command. Parses it and immediately dispatches,
 * leaving the formatted response staged in client->resp_buf for the
 * client's subsequent read() to pick up. The write() itself always
 * "succeeds" (returns the number of bytes consumed) even if the
 * command failed -- the failure is reported through the response
 * text and the handler's errno, not through write()'s return value,
 * since the command was successfully *received and dispatched*.
 */
static ssize_t kds_dshell_write(struct file *filp, const char __user *ubuf,
                                 size_t len, loff_t *off)
{
    kds_dshell_client_t *client = filp->private_data;
    char *argv[DSHELL_MAX_ARGS];
    int argc;
    size_t n = min(len, sizeof(client->cmd_buf) - 1);

    if (!client)
        return -EINVAL;

    if (copy_from_user(client->cmd_buf, ubuf, n))
        return -EFAULT;

    client->cmd_buf[n] = '\0';

    argc = kds_split_cmd(client->cmd_buf, argv, DSHELL_MAX_ARGS);
    if (argc < 0) {
        scnprintf(client->resp_buf, sizeof(client->resp_buf),
                  "ERR failed to parse command\n");
    } else {
        __kds_dispatch_dshell_cmd(argv, argc, client->resp_buf, sizeof(client->resp_buf));
    }

    client->resp_len = strnlen(client->resp_buf, sizeof(client->resp_buf));
    client->resp_pos = 0;

    return len;
}

static ssize_t kds_dshell_read(struct file *filp, char __user *ubuf,
                                size_t len, loff_t *off)
{
    pr_info("kds_dshell_read");
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

    dev = device_create(kds_dshell_class, NULL, kds_dshell_devt, NULL,
                         KDS_DSHELL_DEV_NAME);
    if (IS_ERR(dev)) {
        ret = PTR_ERR(dev);
        pr_err("kds_dshell: device_create failed: %d\n", ret);
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
    if (!kds_dshell_inited)
        return;

    device_destroy(kds_dshell_class, kds_dshell_devt);
    class_destroy(kds_dshell_class);
    cdev_del(&kds_dshell_cdev);
    unregister_chrdev_region(kds_dshell_devt, 1);

    kds_dshell_class = NULL;
    kds_dshell_inited = false;

    pr_info("kds_dshell: /dev/%s removed\n", KDS_DSHELL_DEV_NAME);
}