#include <linux/kds.h>
#include <linux/kds_btree.h>
#include <linux/kds_page.h>
#include <linux/kds_page_mgr.h>
#include <linux/kds_page_alloc.h>
#include <linux/compiler.h>
#include <linux/bug.h>
#include <linux/errno.h>

/*
 * Throughout this file, what used to be a kds_page_t *kp is now a
 * kds_frame_t *frame:
 *   - frame->page  is the buffer (was kp->page)
 *   - frame->kp    is the content-lock + header object (lock via
 *                  kds_page_lock(frame->kp), header via frame->kp->hdr)
 *
 * kds_get_page_cache() (removed) is replaced by
 * kds_buf_lookup_or_load(), which already pins on success -- so the
 * separate pin_kpage() calls that used to follow it are gone too.
 * Matching unpin_kpage() calls become kds_buf_unpin().
 */

void load_btree_node(kds_frame_t *frame, kds_btree_node_t *out)
{
    char *addr;

    kds_page_lock(frame->kp);

    BUG_ON(!kds_is_btree_page(frame));

    addr = (char *)kmap_local_page(frame->page);
    kds_btree_data_copy(addr, out);

    out->frame = frame;

    kunmap_local(addr);
    kds_page_unlock(frame->kp);
}

void load_btree_node_data(kds_frame_t *frame, kds_btree_page_data_t *out)
{
    char *addr;

    kds_page_lock(frame->kp);

    BUG_ON(!kds_is_btree_page(frame));

    addr = (char *)kmap_local_page(frame->page);
    kds_btree_data_copy(addr, out);

    kunmap_local(addr);
    kds_page_unlock(frame->kp);
}

void btree_link_kpage(kds_frame_t *frame, kds_btree_page_data_t data)
{
    char *addr;

    kds_page_lock(frame->kp);
    BUG_ON(!kds_is_btree_page(frame));

    addr = (char *)kmap_local_page(frame->page);
    kds_btree_data_store(addr, &data);

    kunmap_local(addr);
    kds_page_unlock(frame->kp);
}

void store_btree_node(kds_btree_node_t *node)
{
    char *addr;
    kds_frame_t *frame = node->frame;

    kds_page_lock(frame->kp);
    BUG_ON(!kds_is_btree_page(frame));

    addr = (char *)kmap_local_page(frame->page);
    kds_btree_data_store(addr, node);

    kunmap_local(addr);

    kds_page_unlock(frame->kp);
}

void btree_cursor_init(kds_btree_cursor_t *cursor)
{
    memset(cursor, 0, sizeof(kds_btree_cursor_t));
    cursor->depth = 0;
}

void btree_cursor_cleanup(kds_btree_cursor_t *cursor)
{
    int i;
    for (i = 0; i <= cursor->depth; i++) {
        if (cursor->nodes[i].frame) {
            kds_buf_unpin(cursor->nodes[i].frame);
            cursor->nodes[i].frame = NULL;
        }
    }
}

static inline int btree_search_position(
    struct kds_btree_node *node,
    uint64_t key)
{
    int i;
    int nkeys = node->key_count;
    uint64_t *keys = node->keys;

    __builtin_prefetch(keys, 0, 3);

    for (i = 0; i < nkeys; i++) {
        if (key <= keys[i])
            return i;
    }

    return nkeys;
}

static int btree_search_position_binary(kds_btree_node_t *node, kds_tuple_id_t key)
{
    int left = 0;
    int right = node->key_count - 1;
    int pos = 0;

    while (left <= right) {
        int mid = (left + right) / 2;
        if (key < node->keys[mid]) {
            right = mid - 1;
            pos = mid;
        } else if (key > node->keys[mid]) {
            left = mid + 1;
            pos = mid + 1;
        } else {
            return mid;  /* exact match */
        }
    }

    return pos;  /* insertion position */
}

int btree_cursor_search(kds_btree_cursor_t *cursor, kds_page_id_t root_page_id,
                        kds_tuple_id_t key)
{
    kds_frame_t *frame;
    kds_btree_node_t *node;
    kds_page_id_t current_page_id = root_page_id;
    int pos;

    btree_cursor_init(cursor);

    while (cursor->depth < BTREE_MAX_DEPTH) {
        frame = kds_buf_lookup_or_load(current_page_id);
        if (IS_ERR(frame))
            return PTR_ERR(frame);

        node = &cursor->nodes[cursor->depth];
        load_btree_node(frame, node);

        pos = btree_search_position(node, key);
        cursor->positions[cursor->depth] = pos;

        /* Check for duplicate key */
        if (pos < node->key_count && node->keys[pos] == key)
            return -EEXIST;

        /* If leaf, we're done */
        if (node->level == 0)
            return 0;

        /* Descend to child */
        current_page_id = node->slots[pos];
        cursor->depth++;
    }

    return -E2BIG;  /* tree too deep */
}

static void btree_node_insert_at(kds_btree_node_t *node, int pos,
                                  kds_tuple_id_t key, kds_page_id_t slot)
{
    int i;

    /* Shift keys and slots to make room */
    for (i = node->key_count; i > pos; i--) {
        node->keys[i] = node->keys[i - 1];
        node->slots[i + 1] = node->slots[i];
    }

    node->keys[pos] = key;
    node->slots[pos + 1] = slot;
    node->key_count++;
}

/*
 * TODO -- BLOCKED: needs a "allocate a brand-new, empty logical page"
 * entry point in page_mgr.c.
 *
 * kds_get_reserved_kpage() (and kds_page_alloc(), used elsewhere)
 * used to hand back a kds_page_t* for a freshly reserved page with
 * no disk read involved. kds_buf_lookup_or_load() does not cover
 * this -- it always reads existing on-disk content into the frame.
 * There is currently no page_mgr.c function that takes a free frame
 * and registers it for a *new* page_id without a prior
 * kds_read_logical_page() call.
 *
 * Until that allocator exists, this function cannot be correctly
 * migrated -- guessing at a workaround here (e.g. silently skipping
 * the disk read) would risk operating on uninitialized/stale buffer
 * contents. Returns -ENOSYS so callers fail loudly instead of
 * corrupting btree structure.
 */
static int btree_split_node(kds_btree_node_t *node, kds_btree_split_result_t *result)
{
    pr_warn("btree_split_node: blocked, no frame-based \"allocate new page\" API yet\n");
    return -ENOSYS;
}

static int btree_propagate_split(kds_btree_cursor_t *cursor,
                                  kds_btree_split_result_t *split_result)
{
    int level;
    kds_btree_node_t *parent;
    int pos;

    /* Walk up from leaf */
    for (level = cursor->depth - 1; level >= 0; level--) {
        parent = &cursor->nodes[level];
        pos = cursor->positions[level];

        /* If parent has space, insert and done */
        if (parent->key_count < BTREE_MAX_KEYS) {
            btree_node_insert_at(parent, pos, split_result->promoted_key,
                                split_result->right_page_id);
            store_btree_node(parent);
            return 0;
        }

        /* Parent is full, need to split it too */
        kds_btree_split_result_t new_split;

        /* Temporarily insert into full node */
        btree_node_insert_at(parent, pos, split_result->promoted_key,
                            split_result->right_page_id);

        if (btree_split_node(parent, &new_split) < 0)
            return -ENOMEM;

        *split_result = new_split;
    }

    /*
     * TODO -- BLOCKED: same "allocate a brand-new logical page" gap
     * as btree_split_node() above (this path needs a fresh root
     * page). See the comment there.
     */
    pr_warn("btree_propagate_split: blocked, no frame-based \"allocate new page\" API yet\n");
    return -ENOSYS;
}

int btree_insert(kds_page_id_t root_page_id, kds_tuple_id_t key,
                 kds_page_id_t value_page_id)
{
    kds_btree_cursor_t      cursor;
    kds_btree_node_t        *leaf;
    int                     pos;
    int                     ret;

    ret = btree_cursor_search(&cursor, root_page_id, key);
    if (ret < 0) {
        btree_cursor_cleanup(&cursor);
        return ret;
    }

    leaf = &cursor.nodes[cursor.depth];
    pos = cursor.positions[cursor.depth];

    if (leaf->key_count < BTREE_MAX_KEYS) {
        btree_node_insert_at(leaf, pos, key, value_page_id);
        store_btree_node(leaf);
        btree_cursor_cleanup(&cursor);
        return 0;
    }

    kds_btree_split_result_t split_result;

    btree_node_insert_at(leaf, pos, key, value_page_id);

    ret = btree_split_node(leaf, &split_result);
    if (ret < 0) {
        btree_cursor_cleanup(&cursor);
        return ret;
    }

    ret = btree_propagate_split(&cursor, &split_result);

    btree_cursor_cleanup(&cursor);
    return ret;
}

static int __kds_btree_traverse_leaf(
    kds_btree_node_t *node,
    kds_frame_t *frame,
    struct kds_btree_traverse_ctx *ctx)
{
    int i, ret;

    ret = ctx->callback(node, frame, ctx->depth, -1, ctx->private);
    if (ret != 0)
        return ret;

    ctx->visited_nodes++;

    for (i = 0; i < node->key_count; i++) {
        ret = ctx->callback(node, frame, ctx->depth, i, ctx->private);
        if (ret != 0)
            return ret;

        ctx->visited_keys++;
    }

    return 0;
}

static int __kds_btree_traverse_internal(
    kds_btree_node_t *node,
    kds_frame_t *frame,
    struct kds_btree_traverse_ctx *ctx)
{
    kds_frame_t *child_frame;
    kds_btree_node_t child_node;
    int i, ret;

    ret = ctx->callback(node, frame, ctx->depth, -1, ctx->private);
    if (ret != 0)
        return ret;

    ctx->visited_nodes++;
    ctx->depth++;

    for (i = 0; i <= node->key_count; i++) {
        u64 child_page_id = node->slots[i];

        child_frame = kds_buf_lookup_or_load(child_page_id);
        if (IS_ERR(child_frame)) {
            pr_err("kds_btree: failed to load child page %llu, ret=%ld\n",
                   child_page_id, PTR_ERR(child_frame));
            ctx->depth--;
            return PTR_ERR(child_frame);
        }

        load_btree_node(child_frame, &child_node);

        if (child_node.frame->kp->hdr.type == KDS_PAGE_TYPE_BTREE_INTERNAL) {
            ret = __kds_btree_traverse_internal(&child_node, child_frame, ctx);
        } else {
            ret = __kds_btree_traverse_leaf(&child_node, child_frame, ctx);
        }

        kds_buf_unpin(child_frame);

        if (ret != 0) {
            ctx->depth--;
            return ret;
        }
    }

    ctx->depth--;
    return 0;
}

int kds_btree_traverse(
    kds_frame_t *root_frame,
    kds_btree_traverse_cb callback,
    void *private)
{
    kds_btree_node_t root_node;
    struct kds_btree_traverse_ctx ctx = {
        .callback = callback,
        .private = private,
        .depth = 0,
        .visited_nodes = 0,
        .visited_keys = 0,
    };
    int ret;

    if (!root_frame || !callback) {
        pr_err("kds_btree: invalid parameters\n");
        return -EINVAL;
    }

    load_btree_node(root_frame, &root_node);

    if (root_node.frame->kp->hdr.type == KDS_PAGE_TYPE_BTREE_INTERNAL) {
        ret = __kds_btree_traverse_internal(&root_node, root_frame, &ctx);
    } else {
        ret = __kds_btree_traverse_leaf(&root_node, root_frame, &ctx);
    }

    if (ret == 0) {
        pr_debug("kds_btree: traversed %llu nodes, %llu keys\n",
                 ctx.visited_nodes, ctx.visited_keys);
    }

    return ret;
}

static int print_key_callback(
    kds_btree_node_t *node,
    kds_frame_t *frame,
    int depth,
    int key_idx,
    void *private)
{
    int i;

    if (key_idx == -1) {
        for (i = 0; i < depth; i++)
            pr_cont("  ");
        pr_info("Node[%llu] type=%d nkeys=%u\n",
                frame->kp->id,
                node->frame->kp->hdr.type,
                node->key_count);
    } else {
        for (i = 0; i < depth; i++)
            pr_cont("  ");
        pr_info("  Key[%d]: %llu -> %llu\n",
                key_idx, node->keys[key_idx], node->slots[key_idx]);
    }

    return 0;
}

void btree_init_page_data(kds_btree_page_data_t* data) {
    data->level = 0;
    data->key_count = 0;
    data->next = 0;
    data->prev = 0;

    memset(data->keys, 0, sizeof(kds_tuple_id_t) * BTREE_MAX_KEYS);
    memset(data->slots, 0, sizeof(kds_page_id_t) * (BTREE_MAX_KEYS + 1));
}

void btree_init_root_kpage(kds_frame_t *frame) {
    frame->kp->hdr.type = KDS_PAGE_TYPE_BTREE_ROOT;
    frame->kp->hdr.flags |= KDS_PAGE_FLAG_INIT;
}

void btree_init_data_kpage(kds_frame_t *frame) {
    frame->kp->hdr.type = KDS_PAGE_TYPE_BTREE_DATA;
    frame->kp->hdr.flags |= KDS_PAGE_FLAG_INIT;
}

int kds_btree_print(kds_frame_t *root_frame)
{
    return kds_btree_traverse(root_frame, print_key_callback, NULL);
}

void print_btree_node(kds_btree_node_t *node) {
    pr_info("btree data level: %d\n", node->level);
    pr_info("btree data key_count: %llu\n", node->key_count);

    pr_info("btree data key 0: %d\n", node->keys[0]);
    pr_info("btree data key 1: %d\n", node->keys[1]);
    pr_info("btree data key 2: %d\n", node->keys[2]);

    pr_info("btree data slot 0: %d\n", node->slots[0]);
    pr_info("btree data slot 1: %d\n", node->slots[1]);
    pr_info("btree data slot 2: %d\n", node->slots[2]);

    pr_info("btree data next: %d\n", node->next);
    pr_info("btree data prev: %d\n", node->prev);
}