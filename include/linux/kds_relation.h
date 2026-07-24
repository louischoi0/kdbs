#ifndef __KDS_RELATION_H
#define __KDS_RELATION_H

#include <linux/kds.h>
#include <linux/kds_catalog.h>
#include <linux/kds_page_mgr.h>

/*
 * kds_relation_t is a runtime handle for an open table or index --
 * the C-side equivalent of catalog.py's TableAccess, generalized to
 * cover indexes too (PostgreSQL unifies tables and indexes as
 * "relations" in pg_class the same way; kds_catalog.h's sys.tables
 * already stores both under the same row format, see
 * kds_catalog_insert_relation_row()).
 *
 * `schema` is only meaningful for KDS_CLUSTERED_HEAP table relations.
 * Index relations have no columns of their own (schema.nr_cols == 0)
 * -- an index's "data" is just (key -> value_page_id) pairs in a
 * btree, not column-shaped rows.
 */
typedef struct kds_relation {
    kd_oid_t                oid;
    kd_oid_t                namespace_oid;
    kds_clustered_type_t    kind;
    kds_page_id_t           root_page_id;
    kds_schema_t            schema;
} kds_relation_t;

/*
 * Opens oid as a relation: looks it up in sys.tables and (for table
 * relations) builds its column schema from sys.columns. Returns
 * ERR_PTR on failure -- in particular, ERR_PTR(-ENOENT) if oid isn't
 * a relation at all.
 */
kds_relation_t *kds_relation_open(kd_oid_t oid);
void kds_relation_close(kds_relation_t *rel);

/* ------------------------------------------------------------------
 * Index-related methods
 *
 * These are the first concrete capability built on kds_relation_t --
 * table/heap-relation methods (scan, insert routed through
 * heap_insert_tuple(), etc.) are a separate, not-yet-written layer.
 * ------------------------------------------------------------------ */

/*
 * Creates a new index relation on table_oid's target_col: allocates
 * a fresh btree root page, and registers it in sys.objects (type
 * KDS_OID_TYPE_INDEX) and sys.tables (clustered_type
 * KDS_CLUSTERED_BTREE), exactly like kds_catalog_create_table() does
 * for an ordinary btree-clustered table -- just without any
 * sys.columns rows, since an index has no column schema of its own.
 *
 * This only creates the index *relation* (btree root + sys.objects +
 * sys.tables rows). The CREATE INDEX handler records which column it
 * keys in a dedicated sys.indexes row (kds_catalog_insert_index_row())
 * -- the index name still encodes the column for readability, but
 * programmatic lookups (maintenance, planner) go through sys.indexes.
 */
int kds_relation_create_index(kd_oid_t namespace_oid, kd_oid_t table_oid,
                               const char *target_col, kd_oid_t *out_index_oid);

/*
 * Inserts (key -> value_page_id) into index_rel's B+-tree (implemented
 * in index_btree.c, NOT btree.c's separator B-tree). Returns 0,
 * -EEXIST if `key` already exists (indexes are unique), or a negative
 * errno. A root split is bounds-safe and persists the new root page id
 * through kds_catalog_update_relation_desc_page(), also refreshing
 * index_rel->root_page_id in place -- the old KNOWN GAP (silent root
 * staleness / btree_propagate_split() panic) no longer applies here.
 *
 * `key` is the indexed column value widened to u64; `value_page_id` is
 * the heap page id where the row lives (equality lookups jump to that
 * page and scan it). Callers index integer columns only.
 */
int kds_index_insert(kds_relation_t *index_rel, kds_tuple_id_t key,
                      kds_page_id_t value_page_id);

/*
 * Looks up `key` in index_rel's B+-tree. On success *out_value_page_id
 * is the value stored at that key and 0 is returned; -ENOENT if the key
 * isn't present. Descends internal separators to the leaf and reads the
 * matched key's value from the leaf's aligned slot[i] (the B+ leaf
 * convention index_btree.c writes) -- the earlier off-by-one against
 * btree.c's separator layout no longer applies.
 */
int kds_index_search(kds_relation_t *index_rel, kds_tuple_id_t key,
                      kds_page_id_t *out_value_page_id);

/*
 * Removes `key` from index_rel's B+-tree. Returns 0, -ENOENT if absent,
 * or a negative errno. Used by UPDATE maintenance (delete the old key
 * before inserting the new one when an indexed column changes). Does
 * not merge/rebalance underflowed nodes -- see index_btree.c's header.
 */
int kds_index_delete(kds_relation_t *index_rel, kds_tuple_id_t key);

#endif /* __KDS_RELATION_H */