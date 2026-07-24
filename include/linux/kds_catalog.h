#ifndef __KDS_CATALOG_H
#define __KDS_CATALOG_H

#include <linux/kds.h>
#include <linux/kds_page_mgr.h>
#include <linux/kds_heap.h>

/*
 * Kernel-side port of the Python POC's catalog.py, adapted to fit
 * the kernel/C environment rather than translated line-for-line. The
 * differences from the Python version, and why:
 *
 *   - Catalog rows are fixed-size __attribute__((packed)) structs
 *     copied straight into/out of heap tuples (heap_insert_tuple()/
 *     heap_read_tuple()), instead of the Python version's dynamic
 *     buffer_cursor serialization (varchar length prefixes, nested
 *     attribute lists, etc). Names are fixed KDS_CATALOG_NAME_MAX
 *     char arrays; anything needing to exceed that belongs in a
 *     KDS_PAGE_TYPE_TOAST page (already defined in kds_core.h) as a
 *     follow-up, not bolted onto this first version.
 *   - The in-memory sys-object registry (Python's SYS_OBJECTS /
 *     SYS_OBJECTS__NAME dicts) is a small fixed-capacity array with
 *     linear search, not a hash table. The set of sys objects is
 *     bootstrap-time and small (on the order of 20 entries) -- a
 *     hash table here would be complexity without payoff. If this
 *     ever needs to hold hundreds+ of entries, swap the array for an
 *     rhashtable the same way kds_page_mgr.c does for the buffer
 *     pool.
 *   - Catalog metadata pages (sys.objects/sys.tables/sys.columns/
 *     sys.types) live at fixed, well-known page_ids, registered
 *     directly via kds_buf_alloc_new() rather than going through the
 *     pre-allocation ring (kds_page_alloc()) -- the ring hands out
 *     whichever id is next available, which is the wrong tool for
 *     "this page must be exactly id 6".
 */

/* ------------------------------------------------------------------
 * Well-known OIDs / page ids (mirrors catalog.py's constants)
 * ------------------------------------------------------------------ */

#define KDS_CATALOG_NAME_MAX   64

#define KDS_OID_NAMESPACE_SYS       0
#define KDS_OID_NAMESPACE_PUBLIC    1

#define KDS_OID_TYPE_INT            12
#define KDS_OID_TYPE_VARCHAR        13
#define KDS_OID_TYPE_SCHEMA         14
#define KDS_OID_TYPE_BOOL           15
#define KDS_OID_TYPE_BYTES          16
#define KDS_OID_TYPE_NAMESPACE      17
#define KDS_OID_TYPE_ATTRIBUTE      18
#define KDS_OID_TYPE_COLUMN         19
#define KDS_OID_TYPE_PAGE           20
#define KDS_OID_TYPE_TABLE          21
#define KDS_OID_TYPE_OPERATOR       22
#define KDS_OID_TYPE_INDEX          23
/*
 * KDS_OID_TYPE_CHAR deliberately does NOT reuse 13. The Python
 * catalog.py this is ported from has type_char = SysObject(13, ...)
 * -- the same oid as type_varchar -- which looks like a copy-paste
 * bug rather than an intentional alias (the two are registered as
 * distinct dict entries by name, but share an oid, so any oid-based
 * lookup can't tell them apart). Giving it its own oid here fixes
 * that instead of reproducing it.
 */
#define KDS_OID_TYPE_CHAR           24

/*
 * Added alongside the kds_types.h type registry (int8/int16/int32/
 * float/decimal). KDS_OID_TYPE_INT (12) is kept as-is and now also
 * aliased as KDS_OID_TYPE_INT64, since kds_types.h's KDS_TYPE_INT64
 * is what "int" meant historically here -- not renumbered, to avoid
 * disturbing the existing oid.
 */
#define KDS_OID_TYPE_INT8           25
#define KDS_OID_TYPE_INT16          26
#define KDS_OID_TYPE_INT32          27
#define KDS_OID_TYPE_FLOAT          28
#define KDS_OID_TYPE_DECIMAL        29
#define KDS_OID_TYPE_UINT64         30   /* forced primary-key type */
#define KDS_OID_TYPE_INT64          KDS_OID_TYPE_INT

#define KDS_OID_SYS_TYPES_TABLE     100
#define KDS_OID_SYS_OBJECTS_TABLE   110
#define KDS_OID_SYS_COLUMNS_TABLE   111
#define KDS_OID_SYS_TABLES_TABLE    112
#define KDS_OID_SYS_INDEXES_TABLE   113
#define KDS_OID_SYS_PROC_TABLE      114

/* Starting point for user-created object oids -- mirrors catalog.py's
 * USER_OBJ_OID_COUNTER = 4000. See kds_catalog_generate_user_oid()'s
 * doc comment below for the persistence caveat. */
#define KDS_USER_OID_START          4000

/* Fixed page_ids for the bootstrap catalog heap pages.
 * These are reserved -- kds_buf_alloc_new() is called with these
 * exact values during kds_catalog_bootstrap(), they are never handed
 * out by the general-purpose allocator (kds_page_alloc.c). */
#define KDS_CATALOG_PAGE_TYPES      4
#define KDS_CATALOG_PAGE_COLUMNS    5
#define KDS_CATALOG_PAGE_OBJECTS    6
#define KDS_CATALOG_PAGE_TABLES     7
/*
 * sys.indexes -- one row per secondary index, recording which column
 * of which table it keys and the index relation's own oid. Added after
 * the original four fixed pages, so it only exists on databases
 * bootstrapped after this change (an older kdb.img must be re-created
 * with resetdata.sh before CREATE INDEX works). See kds_sys_index_t.
 */
#define KDS_CATALOG_PAGE_INDEXES    8

/* Transaction id stamped on every bootstrap-time tuple. Mirrors
 * PostgreSQL's FrozenTransactionId convention: bootstrap rows are
 * inserted before the transaction manager exists, so they need a
 * fixed, always-visible xmin rather than one from a real
 * transaction. */
#define KDS_BOOTSTRAP_XID          1

typedef enum {
    KDS_CLUSTERED_HEAP  = 0,
    KDS_CLUSTERED_BTREE = 1,
} kds_clustered_type_t;

/* ------------------------------------------------------------------
 * Fixed-layout catalog rows
 *
 * Each of these is inserted/read as a single heap tuple payload via
 * heap_insert_tuple()/heap_read_tuple() -- no further
 * serialization step needed.
 * ------------------------------------------------------------------ */

typedef struct kds_sys_object {
    kd_oid_t    oid;
    kd_oid_t    namespace_oid;
    kd_oid_t    type_oid;
    kd_oid_t    rel_id;
    char        name[KDS_CATALOG_NAME_MAX];
} __attribute__((packed)) kds_sys_object_t;

typedef struct kds_sys_table {
    kd_oid_t            oid;
    kd_oid_t            namespace_oid;
    char                name[KDS_CATALOG_NAME_MAX];
    kds_page_id_t       desc_page_id;
    u8                  clustered_type;     /* kds_clustered_type_t */
} __attribute__((packed)) kds_sys_table_t;

typedef struct kds_sys_column {
    kd_oid_t    oid;
    kd_oid_t    rel_id;
    u32         pos;
    char        name[KDS_CATALOG_NAME_MAX];
    u32         type_val;
    u32         len;
    bool        notnull;
} __attribute__((packed)) kds_sys_column_t;

typedef struct kds_sys_type {
    kd_oid_t    oid;
    char        name[KDS_CATALOG_NAME_MAX];
    u32         type_val;
    u32         len;
} __attribute__((packed)) kds_sys_type_t;

/*
 * sys.indexes row: one per secondary index.
 *   index_oid  -- the index relation's oid (its sys.tables/sys.objects
 *                 row; its btree root is that row's desc_page_id).
 *   table_oid  -- the table the index is built on.
 *   col_pos    -- position of the indexed column in the table schema
 *                 (kds_sys_column_t.pos).
 *   col_type   -- the indexed column's kds_types.h type_val, so INSERT/
 *                 UPDATE maintenance can decode the key without a schema
 *                 lookup.
 *   flags      -- KDS_INDEX_FLAG_* bitmask.
 */
#define KDS_INDEX_FLAG_UNIQUE   0x1u   /* the only mode supported today */

typedef struct kds_sys_index {
    kd_oid_t    index_oid;
    kd_oid_t    table_oid;
    u32         col_pos;
    u32         col_type;
    u8          flags;
} __attribute__((packed)) kds_sys_index_t;

/* ------------------------------------------------------------------
 * In-memory schema (built from sys.columns rows for a given rel_id)
 * ------------------------------------------------------------------ */

#define KDS_SCHEMA_MAX_COLUMNS  32

typedef struct kds_schema {
    kds_sys_column_t    cols[KDS_SCHEMA_MAX_COLUMNS];
    u32                 nr_cols;
} kds_schema_t;

const kds_sys_column_t *kds_schema_find_column(const kds_schema_t *schema, const char *name);

/* ------------------------------------------------------------------
 * Table access handle (mirrors catalog.py's TableAccess)
 * ------------------------------------------------------------------ */

typedef struct kds_table_access {
    kd_oid_t                namespace_oid;
    kd_oid_t                oid;
    kds_schema_t            schema;
    kds_page_id_t           desc_page_id;
    kds_clustered_type_t    clustered_type;
} kds_table_access_t;

/* ------------------------------------------------------------------
 * In-memory sys-object registry (small array, linear search -- see
 * file-level comment above for why this isn't a hash table)
 * ------------------------------------------------------------------ */

void kds_catalog_register_sys_object(const kds_sys_object_t *obj);
const kds_sys_object_t *kds_catalog_get_sys_object(kd_oid_t oid);
const kds_sys_object_t *kds_catalog_get_sys_object_by_name(const char *name);

/* ------------------------------------------------------------------
 * Bootstrap
 * ------------------------------------------------------------------ */

/* Registers the fixed namespace/type sys-objects in the in-memory
 * registry (no disk I/O -- these are well-known constants, not
 * stored as catalog rows themselves, same as catalog.py). */
void kds_catalog_init_well_known_objects(void);

/* Allocates the four fixed catalog pages (KDS_CATALOG_PAGE_*) and
 * populates them with the bootstrap rows for sys.types/objects/
 * columns/tables. Must be called exactly once, after the page
 * manager and meta system are both initialized. */
int kds_catalog_bootstrap(void);

/* ------------------------------------------------------------------
 * Table / index creation
 * ------------------------------------------------------------------ */

/*
 * Allocates a new object oid. This is a separate id space from
 * logical page_ids (kds_meta.h's assign_page_id()) -- object oids
 * identify catalog rows, page_ids identify storage pages.
 *
 * KNOWN GAP: this counter is in-memory only and resets to
 * KDS_USER_OID_START on every module reload, unlike page_ids (which
 * are persisted in the superblock). Persisting it would mean adding
 * a field to kds_superblock_t (kds_meta.h) -- deliberately not done
 * here since that's a layout change to a struct other code already
 * depends on; flagging it rather than changing kds_meta.h
 * unilaterally.
 */
kd_oid_t kds_catalog_generate_user_oid(void);

/*
 * Creates a new table: allocates its storage root page (heap or
 * btree depending on clustered_type), and inserts the corresponding
 * rows into sys.objects and sys.tables. Returns the new table's oid,
 * or a negative errno cast to kd_oid_t's error convention (callers
 * should check IS_ERR_VALUE() or compare against 0 depending on
 * caller's convention -- see kds_catalog_create_table()'s doc below).
 */
int kds_catalog_create_table(kd_oid_t namespace_oid, const char *name,
                              const kds_schema_t *schema,
                              kds_clustered_type_t clustered_type,
                              kd_oid_t *out_oid);

/* ------------------------------------------------------------------
 * Raw catalog access
 * ------------------------------------------------------------------ */

int kds_catalog_get_sys_table_row(kd_oid_t table_oid, kds_sys_table_t *out);

/*
 * Scans sys.objects (disk, not the small in-memory well-known-object
 * registry above -- kds_catalog_create_table() only writes the disk
 * row, it does not register user tables in that in-memory array) for
 * a row named `name` with type_oid == KDS_OID_TYPE_TABLE. Returns
 * -ENOENT if no such table exists.
 */
int kds_catalog_find_table_oid_by_name(const char *name, kd_oid_t *out_oid);

int kds_catalog_build_schema_from_columns(kd_oid_t rel_id, kds_schema_t *out_schema);
int kds_catalog_init_table_access(kd_oid_t namespace_oid, kd_oid_t oid,
                                   kds_table_access_t *out);

/*
 * Lower-level row-insert primitives, exposed so kds_relation.h's
 * index creation can reuse the exact same sys.objects/sys.tables row
 * format kds_catalog_create_table() uses for ordinary tables --
 * indexes are relations too (same pg_class-style unification
 * PostgreSQL uses: an index is just another row in sys.tables with
 * clustered_type btree and no sys.columns rows of its own).
 */
int kds_catalog_insert_object_row(kd_oid_t oid, kd_oid_t namespace_oid,
                                   kd_oid_t type_oid, const char *name);
int kds_catalog_insert_relation_row(kd_oid_t oid, kd_oid_t namespace_oid,
                                     const char *name, kds_page_id_t desc_page_id,
                                     kds_clustered_type_t clustered_type);

int kds_catalog_update_relation_desc_page(kd_oid_t table_oid,
                                           kds_page_id_t new_desc_page_id);

/* ------------------------------------------------------------------
 * sys.indexes access
 * ------------------------------------------------------------------ */

/* Appends a sys.indexes row. Called by the CREATE INDEX handler after
 * the index relation itself has been created. */
int kds_catalog_insert_index_row(kd_oid_t index_oid, kd_oid_t table_oid,
                                  u32 col_pos, u32 col_type, u8 flags);

/*
 * Collects every sys.indexes row for `table_oid` into out[0..max-1],
 * writing the number found to *count. Returns 0 on success (including
 * *count == 0 when the table has no indexes), or a negative errno.
 * Used by INSERT/UPDATE maintenance to find the indexes to keep in sync.
 */
int kds_catalog_find_indexes_for_table(kd_oid_t table_oid,
                                        kds_sys_index_t *out, u32 max,
                                        u32 *count);

/*
 * Finds the index (if any) on (table_oid, col_pos) and copies its row
 * to *out. Returns 0, -ENOENT if no such index exists, or a negative
 * errno. Used by the planner to pick an index for a WHERE equality.
 */
int kds_catalog_find_index_on_column(kd_oid_t table_oid, u32 col_pos,
                                      kds_sys_index_t *out);

#endif /* __KDS_CATALOG_H */