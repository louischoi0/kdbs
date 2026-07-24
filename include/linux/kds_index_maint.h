#ifndef __KDS_INDEX_MAINT_H
#define __KDS_INDEX_MAINT_H

#include <linux/kds.h>
#include <linux/kds_catalog.h>
#include <linux/kds_relation.h>

/*
 * Secondary-index maintenance: the single place that knows how to keep
 * every index on a table in sync with the table's rows. Backfill
 * (CREATE INDEX), INSERT, and UPDATE all funnel through here so the
 * "for each index of the table" logic and the column->key extraction
 * live in exactly one file (index_maint.c), on top of the B+-tree in
 * index_btree.c (kds_index_insert/search/delete).
 *
 * KEY MODEL: an index key is the indexed column's value widened to u64.
 * Only integer columns (INT8/16/32/64) are indexable -- the B+-tree
 * keys are u64 and indexes are used for equality only, so the fixed
 * column bytes are zero-extended into the key and the same widening is
 * applied everywhere (backfill, maintenance, planner), which keeps
 * equality consistent regardless of sign. The index value is the heap
 * page id where the row lives.
 *
 * col_idx throughout is the column's array index in the *opened*
 * relation schema (kds_relation_open() rebuilds it deterministically),
 * i.e. the same index the executors use as (col - schema.cols). This is
 * what sys.indexes stores in its col_pos field.
 */

/* Max indexes handled per table in one maintenance pass (stack array). */
#define KDS_INDEX_MAX_PER_TABLE   8

/* Largest encoded row index maintenance will decode a key out of. */
#define KDS_INDEX_ROW_MAX         256

/* Whether type_val is an integer type CLA can build an index on. */
bool kds_index_type_supported(u32 type_val);

/*
 * Extracts column col_idx's value from an encoded row and widens it to
 * a u64 index key. Returns 0, or -EINVAL if the column can't be located
 * or isn't 1..8 bytes wide.
 */
int kds_index_key_from_row(const kds_schema_t *schema, const u8 *row,
                           u16 row_len, u32 col_idx, kds_tuple_id_t *out_key);

/*
 * Populates a freshly-created index by scanning every existing row of
 * table_rel (heap chain or btree-clustered buckets) and inserting
 * (column value -> row page id) into index_rel. Returns 0, -EEXIST if
 * the indexed column has duplicate values (indexes are unique), or a
 * negative errno. Synchronous (not scheduler-sliced) -- CREATE INDEX is
 * a one-shot DDL op on small edge-node tables.
 */
int kds_index_backfill(kds_relation_t *index_rel, kds_relation_t *table_rel,
                       u32 col_idx);

/*
 * After a row has been inserted for table_oid and lives on heap page
 * page_id, add it to every index on the table. Returns 0, -EEXIST on a
 * unique violation, or a negative errno. A no-op (returns 0) if the
 * table has no indexes.
 */
int kds_index_maint_on_insert(kd_oid_t table_oid, const kds_schema_t *schema,
                              const u8 *row, u16 row_len,
                              kds_page_id_t page_id);

/*
 * After a row on page_id has been updated from old_row to new_row, for
 * every index whose keyed column value changed, delete the old key and
 * insert the new one. Returns 0, -EEXIST if a new key collides, or a
 * negative errno. A no-op if no indexed column changed.
 */
int kds_index_maint_on_update(kd_oid_t table_oid, const kds_schema_t *schema,
                              const u8 *old_row, u16 old_len,
                              const u8 *new_row, u16 new_len,
                              kds_page_id_t page_id);

#endif /* __KDS_INDEX_MAINT_H */
