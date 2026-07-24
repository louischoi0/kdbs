/* index_maint.c
 *
 * Secondary-index maintenance -- the one place that keeps every index
 * on a table in sync with the table's rows. Sits on top of the index
 * B+-tree (index_btree.c: kds_index_insert/search/delete) and the
 * sys.indexes catalog (catalog.c). Used by:
 *   - CREATE INDEX backfill      (kds_index_backfill)
 *   - INSERT maintenance          (kds_index_maint_on_insert)
 *   - UPDATE maintenance          (kds_index_maint_on_update)
 *
 * See kds_index_maint.h for the key model (integer column value widened
 * to u64; value == the row's heap page id; col_idx == schema array
 * index).
 */

#include <linux/kds.h>
#include <linux/kds_index_maint.h>
#include <linux/kds_relation.h>
#include <linux/kds_catalog.h>
#include <linux/kds_types.h>
#include <linux/kds_heap.h>
#include <linux/kds_btree.h>
#include <linux/kds_page_mgr.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/kernel.h>

/* ------------------------------------------------------------------
 * Column -> key extraction
 * ------------------------------------------------------------------ */

bool kds_index_type_supported(u32 type_val)
{
    switch ((kds_type_val_t)type_val) {
    case KDS_TYPE_INT8:
    case KDS_TYPE_INT16:
    case KDS_TYPE_INT32:
    case KDS_TYPE_INT64:
        return true;
    default:
        return false;
    }
}

/* Locate column col_idx's bytes in an encoded row (fixed types
 * back-to-back; variable types u16-length-prefixed) -- the same layout
 * walk exec_heap_update.c's update_col_span() uses. */
static int index_col_span(const kds_schema_t *schema, const u8 *buf,
                          u16 buf_len, u32 col_idx,
                          const void **out_ptr, u16 *out_len)
{
    size_t off = 0;
    u32    i;

    for (i = 0; i < schema->nr_cols; i++) {
        const kds_sys_column_t *col  = &schema->cols[i];
        const kds_type_desc_t  *desc =
            kds_type_lookup_by_val((kds_type_val_t)col->type_val);
        u16 value_len;

        if (!desc)
            return -EINVAL;

        if (desc->fixed_len == 0) {
            if (off + sizeof(u16) > buf_len)
                return -EINVAL;
            memcpy(&value_len, buf + off, sizeof(u16));
            off += sizeof(u16);
        } else {
            value_len = desc->fixed_len;
        }

        if ((size_t)off + value_len > buf_len)
            return -EINVAL;

        if (i == col_idx) {
            *out_ptr = buf + off;
            *out_len = value_len;
            return 0;
        }

        off += value_len;
    }

    return -EINVAL;
}

int kds_index_key_from_row(const kds_schema_t *schema, const u8 *row,
                           u16 row_len, u32 col_idx, kds_tuple_id_t *out_key)
{
    const void *ptr;
    u16         len;
    u64         key = 0;
    int         ret;

    if (!schema || !row || !out_key)
        return -EINVAL;

    /*
     * Engine self-constraint fast path: column 0 is always the u64
     * primary key stored at offset 0, so read it directly without the
     * column-span walk. (Any indexed column is integer; column 0 is the
     * only one guaranteed to sit at offset 0.)
     */
    if (col_idx == 0) {
        if (row_len < sizeof(u64))
            return -EINVAL;
        memcpy(out_key, row, sizeof(u64));
        return 0;
    }

    ret = index_col_span(schema, row, row_len, col_idx, &ptr, &len);
    if (ret)
        return ret;

    if (len == 0 || len > sizeof(u64))
        return -EINVAL;

    /* Zero-extend the fixed integer bytes (little-endian) into the key.
     * The identical widening is applied at insert, update and lookup, so
     * equality is consistent even for values whose sign bit is set. */
    memcpy(&key, ptr, len);
    *out_key = key;
    return 0;
}

/* ------------------------------------------------------------------
 * Backfill (CREATE INDEX)
 * ------------------------------------------------------------------ */

/* Insert every live tuple on one heap page into the index, keyed by the
 * indexed column, valued by page_id. */
static int backfill_page(kds_relation_t *index_rel, const kds_schema_t *schema,
                         u32 col_idx, kds_frame_t *frame, kds_page_id_t page_id)
{
    u16 nr_slots = heap_nr_slots(frame);
    u16 slot;

    for (slot = 0; slot < nr_slots; slot++) {
        kds_heap_tuple_hdr_t hdr;
        u8                   buf[KDS_INDEX_ROW_MAX];
        kds_tuple_id_t       key;
        int                  r;

        r = heap_read_tuple(frame, slot, &hdr, buf, sizeof(buf));
        if (r == -ENOENT)
            continue;               /* dead slot */
        if (r < 0)
            return r;

        r = kds_index_key_from_row(schema, buf, hdr.data_len, col_idx, &key);
        if (r)
            return r;

        r = kds_index_insert(index_rel, key, page_id);
        if (r)
            return r;                /* -EEXIST => duplicate value in column */
    }

    return 0;
}

static int backfill_heap(kds_relation_t *index_rel, kds_relation_t *table_rel,
                         u32 col_idx)
{
    kds_page_id_t pid = table_rel->root_page_id;

    while (pid) {
        kds_frame_t  *frame;
        kds_page_id_t next;
        int           r;

        frame = kds_buf_lookup_or_load(pid);
        if (IS_ERR(frame))
            return PTR_ERR(frame);

        r = backfill_page(index_rel, &table_rel->schema, col_idx, frame, pid);
        if (r) {
            kds_buf_unpin(frame);
            return r;
        }

        next = heap_get_next_page_id(frame);
        kds_buf_unpin(frame);
        pid = next;
    }

    return 0;
}

/* Btree-clustered table: rows live in heap bucket pages hung off the
 * clustering btree's leaves. Descend to the leftmost leaf, then walk
 * leaf siblings via `next`, backfilling every distinct bucket page. This
 * reads the table's clustering btree (btree.c layout via
 * load_btree_node), not the index B+-tree. */
static int backfill_btree(kds_relation_t *index_rel, kds_relation_t *table_rel,
                          u32 col_idx)
{
    kds_page_id_t    pid = table_rel->root_page_id;
    kds_btree_node_t node;
    int              guard = 0;

    /* Descend to leftmost leaf. */
    for (;;) {
        kds_frame_t *f = kds_buf_lookup_or_load(pid);

        if (IS_ERR(f))
            return PTR_ERR(f);
        load_btree_node(f, &node);
        kds_buf_unpin(f);

        if (node.level == 0)
            break;
        pid = node.slots[0];
        if (++guard > BTREE_MAX_DEPTH)
            return -E2BIG;
    }

    /* Walk leaf siblings. */
    for (;;) {
        u32 i;

        for (i = 0; i <= node.key_count; i++) {
            kds_page_id_t bid = node.slots[i];
            kds_frame_t  *bf;
            int           r;

            if (bid == 0)
                continue;
            if (i > 0 && bid == node.slots[i - 1])
                continue;            /* dedup repeated bucket pointers */

            bf = kds_buf_lookup_or_load(bid);
            if (IS_ERR(bf))
                return PTR_ERR(bf);
            r = backfill_page(index_rel, &table_rel->schema, col_idx, bf, bid);
            kds_buf_unpin(bf);
            if (r)
                return r;
        }

        if (node.next == 0)
            break;

        {
            kds_frame_t *f = kds_buf_lookup_or_load(node.next);

            if (IS_ERR(f))
                return PTR_ERR(f);
            load_btree_node(f, &node);
            kds_buf_unpin(f);
        }
    }

    return 0;
}

int kds_index_backfill(kds_relation_t *index_rel, kds_relation_t *table_rel,
                       u32 col_idx)
{
    if (!index_rel || !table_rel)
        return -EINVAL;

    if (table_rel->kind == KDS_CLUSTERED_HEAP)
        return backfill_heap(index_rel, table_rel, col_idx);

    return backfill_btree(index_rel, table_rel, col_idx);
}

/* ------------------------------------------------------------------
 * INSERT / UPDATE maintenance
 * ------------------------------------------------------------------ */

int kds_index_maint_on_insert(kd_oid_t table_oid, const kds_schema_t *schema,
                              const u8 *row, u16 row_len, kds_page_id_t page_id)
{
    kds_sys_index_t idxs[KDS_INDEX_MAX_PER_TABLE];
    u32             n = 0, i;
    int             ret;

    if (!schema || !row)
        return -EINVAL;

    ret = kds_catalog_find_indexes_for_table(table_oid, idxs,
                                             ARRAY_SIZE(idxs), &n);
    if (ret)
        return ret;

    for (i = 0; i < n; i++) {
        kds_relation_t *irel;
        kds_tuple_id_t  key;

        ret = kds_index_key_from_row(schema, row, row_len,
                                     idxs[i].col_pos, &key);
        if (ret)
            return ret;

        irel = kds_relation_open(idxs[i].index_oid);
        if (IS_ERR(irel))
            return PTR_ERR(irel);

        ret = kds_index_insert(irel, key, page_id);
        kds_relation_close(irel);
        if (ret)
            return ret;              /* -EEXIST bubbles up as a unique violation */
    }

    return 0;
}

int kds_index_maint_on_update(kd_oid_t table_oid, const kds_schema_t *schema,
                              const u8 *old_row, u16 old_len,
                              const u8 *new_row, u16 new_len,
                              kds_page_id_t page_id)
{
    kds_sys_index_t idxs[KDS_INDEX_MAX_PER_TABLE];
    u32             n = 0, i;
    int             ret;

    if (!schema || !old_row || !new_row)
        return -EINVAL;

    ret = kds_catalog_find_indexes_for_table(table_oid, idxs,
                                             ARRAY_SIZE(idxs), &n);
    if (ret)
        return ret;

    for (i = 0; i < n; i++) {
        kds_relation_t *irel;
        kds_tuple_id_t  old_key, new_key;

        ret = kds_index_key_from_row(schema, old_row, old_len,
                                     idxs[i].col_pos, &old_key);
        if (ret)
            return ret;
        ret = kds_index_key_from_row(schema, new_row, new_len,
                                     idxs[i].col_pos, &new_key);
        if (ret)
            return ret;

        if (old_key == new_key)
            continue;                /* indexed column unchanged for this index */

        irel = kds_relation_open(idxs[i].index_oid);
        if (IS_ERR(irel))
            return PTR_ERR(irel);

        ret = kds_index_delete(irel, old_key);
        if (ret && ret != -ENOENT) { /* tolerate a missing old key */
            kds_relation_close(irel);
            return ret;
        }

        ret = kds_index_insert(irel, new_key, page_id);
        kds_relation_close(irel);
        if (ret)
            return ret;
    }

    return 0;
}
