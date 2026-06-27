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
 * KNOWN LIMITATION: which column the index is on is only recorded in
 * the index's name string ("index_<col>_<table_oid>"), the same
 * convention catalog.py used. There is no dedicated sys.indexes
 * catalog tracking the key column's type/oid for programmatic
 * lookup -- adding one is a follow-up, not done here.
 */
int kds_relation_create_index(kd_oid_t namespace_oid, kd_oid_t table_oid,
                               const char *target_col, kd_oid_t *out_index_oid);

/*
 * Inserts (key -> value_page_id) into index_rel's btree. Thin
 * wrapper over btree_insert() (kds_btree.h).
 *
 * KNOWN GAP: if this insert triggers a root split (btree_insert() ->
 * btree_propagate_split()'s new-root path), the btree's actual root
 * page_id changes, but kds_btree.c's TODO there ("Update root
 * page_id in metadata") is still unresolved -- index_rel->root_page_id
 * (and the corresponding sys.tables.desc_page_id row on disk) will
 * silently go stale, pointing at what is now a demoted INTERNAL node
 * instead of the real root. This will not be caught here; until the
 * btree layer's root-update TODO is resolved, repeated inserts that
 * cause splits can eventually make this index unreachable via
 * index_rel->root_page_id.
 */
int kds_index_insert(kds_relation_t *index_rel, kds_tuple_id_t key,
                      kds_page_id_t value_page_id);

/*
 * Looks up `key` in index_rel's btree. On success, *out_value_page_id
 * is the value stored at that key and 0 is returned. Returns -ENOENT
 * if the key isn't present.
 *
 * UNVERIFIED -- see kds_btree.c's btree_node_insert_at(): it is used
 * for both internal-node and leaf-node inserts, and stores a newly
 * inserted (key, value) pair's value at slots[pos+1] (the
 * internal-node "right child" convention), not slots[pos]. This
 * function reads a matched leaf key's value from slots[pos] -- the
 * natural reading for a leaf's own key->value association -- but
 * that has NOT been confirmed to match what btree_insert() actually
 * wrote end to end. Treat this function as unverified until tested
 * with a real insert-then-search round trip; if results come back
 * off by one position, the fix likely belongs in
 * btree_node_insert_at()'s leaf-case handling, not here.
 */
int kds_index_search(kds_relation_t *index_rel, kds_tuple_id_t key,
                      kds_page_id_t *out_value_page_id);

#endif /* __KDS_RELATION_H */