#include <linux/kds.h>
#include <linux/kds_relation.h>
#include <linux/kds_catalog.h>
#include <linux/kds_page_mgr.h>
#include <linux/kds_page_alloc.h>
#include <linux/kds_btree.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/string.h>

kds_relation_t *kds_relation_open(kd_oid_t oid)
{
    kds_relation_t *rel;
    kds_sys_table_t row;
    int ret;

    ret = kds_catalog_get_sys_table_row(oid, &row);
    if (ret)
        return ERR_PTR(ret);

    rel = kzalloc(sizeof(*rel), GFP_KERNEL);
    if (!rel)
        return ERR_PTR(-ENOMEM);

    rel->oid = oid;
    rel->namespace_oid = row.namespace_oid;
    rel->root_page_id = row.desc_page_id;
    rel->kind = (kds_clustered_type_t)row.clustered_type;

    ret = kds_catalog_build_schema_from_columns(oid, &rel->schema);
    if (ret == -ENOENT) {
        /* No sys.columns rows -- expected for an index relation. */
        rel->schema.nr_cols = 0;
    } else if (ret) {
        kfree(rel);
        return ERR_PTR(ret);
    }

    pr_info("kds_relation_open: namespace_oid=%d, oid=%d, kind=%d root_pgid=%d\n", rel->namespace_oid, rel->oid, rel->kind, rel->root_page_id);
    return rel;
}

void kds_relation_close(kds_relation_t *rel)
{
    kfree(rel);
}

int kds_relation_create_index(kd_oid_t namespace_oid, kd_oid_t table_oid,
                               const char *target_col, kd_oid_t *out_index_oid)
{
    kds_frame_t *root_frame;
    kd_oid_t new_oid;
    char name[KDS_CATALOG_NAME_MAX];
    int ret;

    if (!target_col || !out_index_oid)
        return -EINVAL;

    root_frame = kds_page_alloc(KDS_PAGE_TYPE_BTREE_ROOT);
    if (!root_frame)
        return -ENOSPC; /* pre-allocation ring empty, see kds_page_alloc.h */

    btree_init_root_kpage(root_frame);

    new_oid = kds_catalog_generate_user_oid();
    snprintf(name, sizeof(name), "index_%s_%llu", target_col, (u64)table_oid);

    ret = kds_catalog_insert_object_row(new_oid, namespace_oid,
                                         KDS_OID_TYPE_INDEX, name);
    if (ret) {
        kds_buf_unpin(root_frame);
        return ret;
    }

    ret = kds_catalog_insert_relation_row(new_oid, namespace_oid, name,
                                           root_frame->kp->id, KDS_CLUSTERED_BTREE);
    kds_buf_unpin(root_frame);
    if (ret)
        return ret;

    *out_index_oid = new_oid;
    return 0;
}

int kds_index_insert(kds_relation_t *index_rel, kds_tuple_id_t key,
                      kds_page_id_t value_page_id)
{
    if (!index_rel || index_rel->kind != KDS_CLUSTERED_BTREE)
        return -EINVAL;

    return btree_insert(index_rel->root_page_id, key, value_page_id);
}

int kds_index_search(kds_relation_t *index_rel, kds_tuple_id_t key,
                      kds_page_id_t *out_value_page_id)
{
    kds_page_id_t current_id;
    kds_frame_t *frame;
    kds_btree_node_t node;
    int depth = 0;

    if (!index_rel || !out_value_page_id || index_rel->kind != KDS_CLUSTERED_BTREE)
        return -EINVAL;

    current_id = index_rel->root_page_id;

    while (depth < BTREE_MAX_DEPTH) {
        int i, match_pos = -1;

        frame = kds_buf_lookup_or_load(current_id);
        if (IS_ERR(frame))
            return PTR_ERR(frame);

        load_btree_node(frame, &node);

        for (i = 0; i < node.key_count; i++) {
            if (node.keys[i] == key) {
                match_pos = i;
                break;
            }
        }

        if (node.level == 0) {
            /* Leaf: exact match or not found -- see this function's
             * doc comment in kds_relation.h about the unverified
             * slots[pos] vs slots[pos+1] question. */
            if (match_pos < 0) {
                kds_buf_unpin(frame);
                return -ENOENT;
            }

            *out_value_page_id = node.slots[match_pos];
            kds_buf_unpin(frame);
            return 0;
        }

        /* Internal: descend via the first key >= search key, same
         * convention btree_cursor_search()'s btree_search_position()
         * uses. */
        {
            int desc_pos = node.key_count;

            for (i = 0; i < node.key_count; i++) {
                if (key <= node.keys[i]) {
                    desc_pos = i;
                    break;
                }
            }
            current_id = node.slots[desc_pos];
        }

        kds_buf_unpin(frame);
        depth++;
    }

    return -E2BIG; /* tree deeper than BTREE_MAX_DEPTH -- shouldn't happen */
}