#include <linux/kds.h>
#include <linux/kds_catalog.h>
#include <linux/kds_types.h>
#include <linux/kds_page_mgr.h>
#include <linux/kds_page_alloc.h>
#include <linux/kds_heap.h>
#include <linux/kds_btree.h>
#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>

/* ------------------------------------------------------------------
 * In-memory sys-object registry
 *
 * Small fixed-capacity array + linear search -- see the rationale in
 * kds_catalog.h's file-level comment. Protected by a spinlock mostly
 * out of caution: registration only happens during bootstrap (single
 * threaded, early init), but lookups could in principle be called
 * from anywhere later.
 * ------------------------------------------------------------------ */

#define KDS_CATALOG_MAX_SYS_OBJECTS 64

static kds_sys_object_t g_sys_objects[KDS_CATALOG_MAX_SYS_OBJECTS];
static u32 g_sys_objects_count;
static DEFINE_SPINLOCK(g_sys_objects_lock);

static atomic64_t g_user_oid_counter = ATOMIC64_INIT(KDS_USER_OID_START);

void kds_catalog_register_sys_object(const kds_sys_object_t *obj)
{
    unsigned long flags;

    if (!obj)
        return;

    spin_lock_irqsave(&g_sys_objects_lock, flags);

    if (g_sys_objects_count >= KDS_CATALOG_MAX_SYS_OBJECTS) {
        spin_unlock_irqrestore(&g_sys_objects_lock, flags);
        /*
         * The well-known-object registry is a fixed-size compile-time
         * constant. Overflowing it means KDS_CATALOG_MAX_SYS_OBJECTS
         * is too small for the objects kds_catalog_init_well_known_objects()
         * registers -- this is a developer error, not a runtime condition,
         * so a panic is appropriate.
         */
        panic("KDS: sys-object registry overflow (cap=%d) -- "
              "increase KDS_CATALOG_MAX_SYS_OBJECTS (oid=%llu name=%s)\n",
              KDS_CATALOG_MAX_SYS_OBJECTS, obj->oid, obj->name);
    }

    g_sys_objects[g_sys_objects_count++] = *obj;

    spin_unlock_irqrestore(&g_sys_objects_lock, flags);
}

const kds_sys_object_t *kds_catalog_get_sys_object(kd_oid_t oid)
{
    unsigned long flags;
    u32 i;
    const kds_sys_object_t *found = NULL;

    spin_lock_irqsave(&g_sys_objects_lock, flags);
    for (i = 0; i < g_sys_objects_count; i++) {
        if (g_sys_objects[i].oid == oid) {
            found = &g_sys_objects[i];
            break;
        }
    }
    spin_unlock_irqrestore(&g_sys_objects_lock, flags);

    return found;
}

const kds_sys_object_t *kds_catalog_get_sys_object_by_name(const char *name)
{
    unsigned long flags;
    u32 i;
    const kds_sys_object_t *found = NULL;

    if (!name)
        return NULL;

    spin_lock_irqsave(&g_sys_objects_lock, flags);
    for (i = 0; i < g_sys_objects_count; i++) {
        if (!strncmp(g_sys_objects[i].name, name, KDS_CATALOG_NAME_MAX)) {
            found = &g_sys_objects[i];
            break;
        }
    }
    spin_unlock_irqrestore(&g_sys_objects_lock, flags);

    return found;
}

/* ------------------------------------------------------------------
 * Schema helpers
 * ------------------------------------------------------------------ */

const kds_sys_column_t *kds_schema_find_column(const kds_schema_t *schema, const char *name)
{
    u32 i;

    if (!schema || !name)
        return NULL;

    for (i = 0; i < schema->nr_cols; i++) {
        if (!strncmp(schema->cols[i].name, name, KDS_CATALOG_NAME_MAX))
            return &schema->cols[i];
    }

    return NULL;
}

static inline void set_name(char *dst, const char *src)
{
    strncpy(dst, src, KDS_CATALOG_NAME_MAX - 1);
    dst[KDS_CATALOG_NAME_MAX - 1] = '\0';
}

/* ------------------------------------------------------------------
 * Well-known objects (no disk I/O -- pure in-memory registration)
 * ------------------------------------------------------------------ */

static void register_namespace(kd_oid_t oid, const char *name)
{
    kds_sys_object_t obj = {0};

    obj.oid = oid;
    obj.namespace_oid = oid; /* a namespace's own namespace is itself, per catalog.py */
    obj.type_oid = KDS_OID_TYPE_NAMESPACE;
    obj.rel_id = 0;
    set_name(obj.name, name);

    kds_catalog_register_sys_object(&obj);
}

static void register_type(kd_oid_t oid, const char *name)
{
    kds_sys_object_t obj = {0};

    obj.oid = oid;
    obj.namespace_oid = KDS_OID_NAMESPACE_SYS;
    obj.type_oid = oid; /* a type object's type is itself, per catalog.py's type_type */
    obj.rel_id = 0;
    set_name(obj.name, name);

    kds_catalog_register_sys_object(&obj);
}

void kds_catalog_init_well_known_objects(void)
{
    register_namespace(KDS_OID_NAMESPACE_SYS, "namespaceSys");
    register_namespace(KDS_OID_NAMESPACE_PUBLIC, "namespacePublic");

    register_type(KDS_OID_TYPE_INT, "type_int");
    register_type(KDS_OID_TYPE_VARCHAR, "type_varchar");
    register_type(KDS_OID_TYPE_CHAR, "type_char");
    register_type(KDS_OID_TYPE_SCHEMA, "type_schema");
    register_type(KDS_OID_TYPE_BOOL, "type_bool");
    register_type(KDS_OID_TYPE_BYTES, "type_bytes");
    register_type(KDS_OID_TYPE_NAMESPACE, "type_namespace");
    register_type(KDS_OID_TYPE_ATTRIBUTE, "type_attribute");
    register_type(KDS_OID_TYPE_COLUMN, "type_column");
    register_type(KDS_OID_TYPE_PAGE, "type_page");
    register_type(KDS_OID_TYPE_TABLE, "type_table");
    register_type(KDS_OID_TYPE_OPERATOR, "type_operator");
    register_type(KDS_OID_TYPE_INDEX, "type_index");

    register_type(KDS_OID_TYPE_INT8, "type_int8");
    register_type(KDS_OID_TYPE_INT16, "type_int16");
    register_type(KDS_OID_TYPE_INT32, "type_int32");
    register_type(KDS_OID_TYPE_FLOAT, "type_float");
    register_type(KDS_OID_TYPE_DECIMAL, "type_decimal");
}

/* ------------------------------------------------------------------
 * Bootstrap: allocate the four fixed catalog pages and populate them
 * ------------------------------------------------------------------ */

static int bootstrap_one_catalog_page(kds_page_id_t page_id)
{
    kds_frame_t *frame = kds_buf_alloc_new(page_id);

    if (IS_ERR(frame)) {
        pr_err("kds_catalog: failed to allocate catalog page %llu: %ld\n",
               (u64)page_id, PTR_ERR(frame));
        return PTR_ERR(frame);
    }

    heap_init_page(frame);
    kds_buf_unpin(frame);
    return 0;
}

int kds_catalog_insert_object_row(kd_oid_t oid, kd_oid_t namespace_oid,
                                   kd_oid_t type_oid, const char *name)
{
    kds_frame_t *frame;
    kds_sys_object_t row = {0};
    kds_heap_tid_t tid;
    int ret;

    frame = kds_buf_lookup_or_load(KDS_CATALOG_PAGE_OBJECTS);
    pr_debug("lookup\n");
    if (IS_ERR(frame))
        return PTR_ERR(frame);
    pr_debug("ok\n");

    row.oid = oid;
    row.namespace_oid = namespace_oid;
    row.type_oid = type_oid;
    row.rel_id = 0;
    set_name(row.name, name);

    ret = heap_insert_tuple(frame, &row, sizeof(row), KDS_BOOTSTRAP_XID, &tid);
    kds_buf_unpin(frame);
    return ret;
}

int kds_catalog_insert_relation_row(kd_oid_t oid, kd_oid_t namespace_oid,
                                     const char *name, kds_page_id_t desc_page_id,
                                     kds_clustered_type_t clustered_type)
{
    kds_frame_t *frame;
    kds_sys_table_t row = {0};
    kds_heap_tid_t tid;
    int ret;

    frame = kds_buf_lookup_or_load(KDS_CATALOG_PAGE_TABLES);
    pr_debug("kds_catalog: insert_relation_row: lookup page %d → %s\n",
             KDS_CATALOG_PAGE_TABLES, IS_ERR(frame) ? "ERR" : "OK");
    if (IS_ERR(frame))
        return PTR_ERR(frame);

    row.oid = oid;
    row.namespace_oid = namespace_oid;
    set_name(row.name, name);
    row.desc_page_id = desc_page_id;
    row.clustered_type = (u8)clustered_type;

    ret = heap_insert_tuple(frame, &row, sizeof(row), KDS_BOOTSTRAP_XID, &tid);
    kds_buf_unpin(frame);

    pr_debug("kds_catalog: insert_relation_row: oid=%llu namespace_oid=%llu ret=%d\n",
             (u64)oid, (u64)namespace_oid, ret);

    return ret;
}

static int insert_sys_column_row(kd_oid_t oid, kd_oid_t rel_id, u32 pos,
                                  const char *name, u32 type_val, u32 len,
                                  bool notnull)
{
    kds_frame_t *frame;
    kds_sys_column_t row = {0};
    kds_heap_tid_t tid;
    int ret;

    frame = kds_buf_lookup_or_load(KDS_CATALOG_PAGE_COLUMNS);
    if (IS_ERR(frame))
        return PTR_ERR(frame);

    row.oid = oid;
    row.rel_id = rel_id;
    row.pos = pos;
    set_name(row.name, name);
    row.type_val = type_val;
    row.len = len;
    row.notnull = notnull;

    ret = heap_insert_tuple(frame, &row, sizeof(row), KDS_BOOTSTRAP_XID, &tid);
    kds_buf_unpin(frame);
    return ret;
}

static int insert_sys_type_row(kd_oid_t oid, const char *name, u32 type_val, u32 len)
{
    kds_frame_t *frame;
    kds_sys_type_t row = {0};
    kds_heap_tid_t tid;
    int ret;

    frame = kds_buf_lookup_or_load(KDS_CATALOG_PAGE_TYPES);
    if (IS_ERR(frame))
        return PTR_ERR(frame);

    row.oid = oid;
    set_name(row.name, name);
    row.type_val = type_val;
    row.len = len;

    ret = heap_insert_tuple(frame, &row, sizeof(row), KDS_BOOTSTRAP_XID, &tid);
    kds_buf_unpin(frame);
    return ret;
}

/*
 * Bootstrap panic helper.
 *
 * Called on any failure inside kds_catalog_bootstrap(). The catalog
 * system tables are a hard precondition for every subsequent KDS
 * operation -- a partially-initialised catalog is worse than no
 * catalog at all, since it will cause silent data corruption or
 * assertion failures deep inside unrelated code paths. Panicking
 * immediately with a clear message is the only safe response.
 */
#define KDS_BOOTSTRAP_PANIC(fmt, ...) \
    panic("KDS: catalog bootstrap FATAL: " fmt "\n", ##__VA_ARGS__)

int kds_catalog_bootstrap(void)
{
    static const struct {
        kd_oid_t      oid;
        const char   *name;
        kds_page_id_t page_id;
    } sys_tables[] = {
        { KDS_OID_SYS_TYPES_TABLE,   "types",   KDS_CATALOG_PAGE_TYPES   },
        { KDS_OID_SYS_OBJECTS_TABLE, "objects", KDS_CATALOG_PAGE_OBJECTS },
        { KDS_OID_SYS_COLUMNS_TABLE, "columns", KDS_CATALOG_PAGE_COLUMNS },
        { KDS_OID_SYS_TABLES_TABLE,  "tables",  KDS_CATALOG_PAGE_TABLES  },
    };
    int ret;
    u32 i;

    kds_catalog_init_well_known_objects();

    /* ----------------------------------------------------------
     * Phase 1: allocate the four fixed catalog heap pages.
     * ---------------------------------------------------------- */
    for (i = 0; i < ARRAY_SIZE(sys_tables); i++) {
        ret = bootstrap_one_catalog_page(sys_tables[i].page_id);
        if (ret)
            KDS_BOOTSTRAP_PANIC(
                "failed to allocate catalog page %llu ('%s'): %d",
                (u64)sys_tables[i].page_id, sys_tables[i].name, ret);
    }

    /* ----------------------------------------------------------
     * Phase 2: sys.objects rows for the four catalog tables.
     * ---------------------------------------------------------- */
    for (i = 0; i < ARRAY_SIZE(sys_tables); i++) {
        ret = kds_catalog_insert_object_row(sys_tables[i].oid,
                                             KDS_OID_NAMESPACE_SYS,
                                             KDS_OID_TYPE_TABLE,
                                             sys_tables[i].name);
        if (ret)
            KDS_BOOTSTRAP_PANIC(
                "failed to insert sys.objects row for '%s': %d",
                sys_tables[i].name, ret);
    }

    /* ----------------------------------------------------------
     * Phase 3: sys.tables rows for the four catalog tables.
     * ---------------------------------------------------------- */
    for (i = 0; i < ARRAY_SIZE(sys_tables); i++) {
        ret = kds_catalog_insert_relation_row(sys_tables[i].oid,
                                               KDS_OID_NAMESPACE_SYS,
                                               sys_tables[i].name,
                                               sys_tables[i].page_id,
                                               KDS_CLUSTERED_HEAP);
        if (ret)
            KDS_BOOTSTRAP_PANIC(
                "failed to insert sys.tables row for '%s': %d",
                sys_tables[i].name, ret);
    }

    /* ----------------------------------------------------------
     * Phase 4: sys.types rows for the well-known scalar types.
     * type_val/len are pulled from the kds_types.h registry so
     * adding a new type there does NOT require touching this list
     * unless it also needs a sys.types bootstrap row.
     * ---------------------------------------------------------- */
    {
        static const struct {
            kd_oid_t    oid;
            const char *type_name;
        } types[] = {
            { KDS_OID_TYPE_INT8,    "int8"    },
            { KDS_OID_TYPE_INT16,   "int16"   },
            { KDS_OID_TYPE_INT32,   "int32"   },
            { KDS_OID_TYPE_INT64,   "int64"   },
            { KDS_OID_TYPE_FLOAT,   "float"   },
            { KDS_OID_TYPE_DECIMAL, "decimal" },
            { KDS_OID_TYPE_BOOL,    "bool"    },
            { KDS_OID_TYPE_VARCHAR, "varchar" },
            { KDS_OID_TYPE_CHAR,    "char"    },
        };

        for (i = 0; i < ARRAY_SIZE(types); i++) {
            const kds_type_desc_t *desc =
                kds_type_lookup_by_name(types[i].type_name);

            if (!desc)
                KDS_BOOTSTRAP_PANIC(
                    "no type descriptor for '%s' -- "
                    "kds_types.h registry is out of sync with catalog.c",
                    types[i].type_name);

            ret = insert_sys_type_row(types[i].oid, desc->name,
                                       desc->type_val, desc->fixed_len);
            if (ret)
                KDS_BOOTSTRAP_PANIC(
                    "failed to insert sys.types row for '%s': %d",
                    desc->name, ret);
        }
    }

    pr_info("kds_catalog: bootstrap complete\n");
    return 0;
}

/* ------------------------------------------------------------------
 * OID generation
 * ------------------------------------------------------------------ */

kd_oid_t kds_catalog_generate_user_oid(void)
{
    return atomic64_inc_return(&g_user_oid_counter);
}

/* ------------------------------------------------------------------
 * Table creation
 * ------------------------------------------------------------------ */

int kds_catalog_create_table(kd_oid_t namespace_oid, const char *name,
                              const kds_schema_t *schema,
                              kds_clustered_type_t clustered_type,
                              kd_oid_t *out_oid)
{
    kds_frame_t *table_root;
    kd_oid_t new_oid;
    int ret;
    u32 i;

    if (!name || !schema || !out_oid)
        return -EINVAL;

    pr_info("create table namespace_oid=%d, name=%s, clustered_type=%d \n", namespace_oid, name, clustered_type);

    if (clustered_type == KDS_CLUSTERED_HEAP) {
        table_root = kds_page_alloc(KDS_PAGE_TYPE_HEAP);
        if (!table_root)
            return -ENOSPC; /* pre-alloc ring empty, see kds_page_alloc.h */
        heap_init_page(table_root);
    } else if (clustered_type == KDS_CLUSTERED_BTREE) {
        table_root = kds_page_alloc(KDS_PAGE_TYPE_BTREE_ROOT);
        if (!table_root) {
            return -ENOSPC;
        }

        btree_init_root_kpage(table_root);
        /*
         * NOTE: a freshly initialized btree root has no leaf
         * structure yet. The first btree_insert() into an empty
         * root is fine; growth beyond one page goes through
         * btree_split_node(), which is currently blocked
         * (-ENOSYS) pending the new-page-allocation wiring
         * discussed earlier (kds_btree.c). Table creation itself
         * still succeeds here -- only later inserts that need a
         * split would fail.
         */
    } else {
        return -EINVAL;
    }

    new_oid = kds_catalog_generate_user_oid();

    ret = kds_catalog_insert_object_row(new_oid, namespace_oid, KDS_OID_TYPE_TABLE, name);
    if (ret) {
        pr_debug("catalog insert obj row failed errno=%d\n", ret);
        kds_buf_unpin(table_root);
        return ret;
    }

    ret = kds_catalog_insert_relation_row(new_oid, namespace_oid, name,
                                table_root->kp->id, clustered_type);
    if (ret) {
        pr_info("catalog insert rel row failed errno=%d\n", ret);
        kds_buf_unpin(table_root);
        return ret;
    }

    for (i = 0; i < schema->nr_cols; i++) {
        const kds_sys_column_t *col = &schema->cols[i];

        ret = insert_sys_column_row(kds_catalog_generate_user_oid(), new_oid,
                                     col->pos, col->name, col->type_val,
                                     col->len, col->notnull);
        if (ret) {
            pr_err("kds_catalog: failed to insert column '%s' for table '%s': %d\n",
                   col->name, name, ret);
            kds_buf_unpin(table_root);
            return ret;
        }
    }

    kds_buf_unpin(table_root);
    *out_oid = new_oid;
    return 0;
}

/* ------------------------------------------------------------------
 * Raw catalog reads
 * ------------------------------------------------------------------ */

int kds_catalog_get_sys_table_row(kd_oid_t table_oid, kds_sys_table_t *out)
{
    kds_frame_t *frame;
    kds_heap_tuple_hdr_t hdr;
    u16 nr_slots, slot;
    int ret = -ENOENT;

    if (!out)
        return -EINVAL;

    frame = kds_buf_lookup_or_load(KDS_CATALOG_PAGE_TABLES);
    if (IS_ERR(frame))
        return PTR_ERR(frame);

    nr_slots = heap_nr_slots(frame);

    for (slot = 0; slot < nr_slots; slot++) {
        kds_sys_table_t row;
        int r = heap_read_tuple(frame, slot, &hdr, &row, sizeof(row));

        if (r == -ENOENT)
            continue; /* dead slot, keep scanning */
        if (r)
            break;

        if (row.oid == table_oid) {
            *out = row;
            ret = 0;
            break;
        }
    }

    kds_buf_unpin(frame);
    return ret;
}

int kds_catalog_find_table_oid_by_name(const char *name, kd_oid_t *out_oid)
{
    kds_frame_t *frame;
    kds_heap_tuple_hdr_t hdr;
    u16 nr_slots, slot;
    int ret = -ENOENT;

    if (!name || !out_oid)
        return -EINVAL;

    frame = kds_buf_lookup_or_load(KDS_CATALOG_PAGE_OBJECTS);
    if (IS_ERR(frame))
        return PTR_ERR(frame);

    nr_slots = heap_nr_slots(frame);

    for (slot = 0; slot < nr_slots; slot++) {
        kds_sys_object_t row;
        int r = heap_read_tuple(frame, slot, &hdr, &row, sizeof(row));

        if (r == -ENOENT)
            continue; /* dead slot, keep scanning */
        if (r)
            break;

        if (row.type_oid == KDS_OID_TYPE_TABLE &&
            !strncmp(row.name, name, KDS_CATALOG_NAME_MAX)) {
            *out_oid = row.oid;
            ret = 0;
            break;
        }
    }

    kds_buf_unpin(frame);
    return ret;
}

int kds_catalog_build_schema_from_columns(kd_oid_t rel_id, kds_schema_t *out_schema)
{
    kds_frame_t *frame;
    kds_heap_tuple_hdr_t hdr;
    u16 nr_slots, slot;

    if (!out_schema)
        return -EINVAL;

    out_schema->nr_cols = 0;

    frame = kds_buf_lookup_or_load(KDS_CATALOG_PAGE_COLUMNS);
    if (IS_ERR(frame))
        return PTR_ERR(frame);

    nr_slots = heap_nr_slots(frame);

    for (slot = 0; slot < nr_slots && out_schema->nr_cols < KDS_SCHEMA_MAX_COLUMNS; slot++) {
        kds_sys_column_t col;
        int r = heap_read_tuple(frame, slot, &hdr, &col, sizeof(col));

        if (r == -ENOENT)
            continue;
        if (r) {
            kds_buf_unpin(frame);
            return r;
        }

        if (col.rel_id == rel_id)
            out_schema->cols[out_schema->nr_cols++] = col;
    }

    kds_buf_unpin(frame);

    if (out_schema->nr_cols == 0)
        return -ENOENT;

    return 0;
}

int kds_catalog_init_table_access(kd_oid_t namespace_oid, kd_oid_t oid,
                                   kds_table_access_t *out)
{
    kds_sys_table_t row;
    int ret;

    if (!out)
        return -EINVAL;

    ret = kds_catalog_get_sys_table_row(oid, &row);
    if (ret)
        return ret;

    ret = kds_catalog_build_schema_from_columns(oid, &out->schema);
    if (ret)
        return ret;

    out->namespace_oid = namespace_oid;
    out->oid = oid;
    out->desc_page_id = row.desc_page_id;
    out->clustered_type = (kds_clustered_type_t)row.clustered_type;

    return 0;
}