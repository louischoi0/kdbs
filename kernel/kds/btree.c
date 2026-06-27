#include <linux/kds.h>
#include <linux/kds_btree.h>
#include <linux/kds_page.h>
#include <linux/kds_page_mgr.h>
#include <linux/kds_page_alloc.h>
#include <linux/kds_meta.h>
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
 * Allocates a fresh page_id (kds_meta.h's assign_page_id() -- the
 * superblock-backed counter, the same one page_alloc.c uses for
 * brand-new pages) and registers it in the buffer pool via
 * kds_buf_alloc_new(). This is the "allocate a brand-new, empty
 * logical page" primitive btree_split_node()/btree_propagate_split()
 * previously had no way to reach -- now that kds_buf_alloc_new()
 * exists in page_mgr.c, both functions below use it directly rather
 * than going through page_alloc.c's pre-allocation ring (that ring
 * hands out whichever id happens to be staged next; a btree split
 * needs a specific, freshly-minted id it can address immediately as
 * "the new sibling page", not an arbitrary one).
 *
 * Returns a pinned frame with its type already set and committed to
 * the page's on-disk header bytes, or an ERR_PTR.
 */
static kds_frame_t *btree_alloc_new_page(kds_page_type_t type)
{
    kds_page_id_t new_id = assign_page_id();
    kds_frame_t *frame;

    if (!new_id)
        return ERR_PTR(-ENOMEM); /* assign_page_id() returns 0 only if the
                                   * superblock isn't up yet -- treated as
                                   * an allocation failure here. */

    frame = kds_buf_alloc_new(new_id);
    if (IS_ERR(frame))
        return frame;

    /*
     * frame is exclusively ours at this point -- just allocated,
     * not yet visible to any other cursor or traversal -- so setting
     * its header directly without the content lock is safe, the
     * same convention btree_init_root_kpage()/btree_init_data_kpage()
     * already use below.
     */
    frame->kp->hdr.type = type;
    frame->kp->hdr.flags |= KDS_PAGE_FLAG_INIT;
    kds_commit_page_hdr(frame);

    return frame;
}

static int btree_split_node(kds_btree_node_t *node, kds_btree_split_result_t *result)
{
    kds_frame_t *new_frame;
    kds_btree_node_t new_node;
    int mid = BTREE_MAX_KEYS / 2;
    int i, j;

    /* New sibling keeps whatever type the node being split has
     * (ROOT/INTERNAL/DATA) -- matches the original pre-migration
     * behavior of copying node->page->hdr.type verbatim. */
    new_frame = btree_alloc_new_page(node->frame->kp->hdr.type);
    if (IS_ERR(new_frame))
        return PTR_ERR(new_frame);

    memset(&new_node, 0, sizeof(kds_btree_node_t));
    new_node.level = node->level;
    new_node.frame = new_frame;
    new_node.next = node->next;
    new_node.prev = node->frame->kp->id;

    result->promoted_key = node->keys[mid];
    result->right_page_id = new_frame->kp->id;

    for (i = mid + 1, j = 0; i < BTREE_MAX_KEYS; i++, j++) {
        new_node.keys[j] = node->keys[i];
        new_node.slots[j] = node->slots[i];
    }
    new_node.slots[j] = node->slots[BTREE_MAX_KEYS];
    new_node.key_count = BTREE_MAX_KEYS - mid - 1;

    node->key_count = mid;
    node->next = new_frame->kp->id;

    store_btree_node(node);
    store_btree_node(&new_node);

    if (new_node.next != 0) {
        kds_frame_t *next_frame = kds_buf_lookup_or_load(new_node.next);

        if (!IS_ERR(next_frame)) {
            kds_btree_node_t next_node;

            load_btree_node(next_frame, &next_node);
            next_node.prev = new_frame->kp->id;
            store_btree_node(&next_node);
            kds_buf_unpin(next_frame);
        } else {
            pr_warn("btree_split_node: failed to relink next page %llu: %ld\n",
                    new_node.next, PTR_ERR(next_frame));
        }
    }

    kds_set_page_dirty(node->frame->kp);
    kds_set_page_dirty(new_frame->kp);

    kds_buf_unpin(new_frame);

    return 0;
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

    /* Every level was full and got split; we need a new root one
     * level above the old one. */
    {
        kds_frame_t *new_root_frame;
        kds_btree_node_t new_root;

        new_root_frame = btree_alloc_new_page(KDS_PAGE_TYPE_BTREE_ROOT);
        if (IS_ERR(new_root_frame))
            return PTR_ERR(new_root_frame);

        memset(&new_root, 0, sizeof(new_root));
        new_root.level = cursor->nodes[0].level + 1;
        new_root.frame = new_root_frame;
        new_root.key_count = 1;
        new_root.keys[0] = split_result->promoted_key;
        new_root.slots[0] = cursor->nodes[0].frame->kp->id;
        new_root.slots[1] = split_result->right_page_id;

        /* Demote old root */
        cursor->nodes[0].frame->kp->hdr.type = KDS_PAGE_TYPE_BTREE_INTERNAL;
        kds_set_page_dirty(cursor->nodes[0].frame->kp);

        store_btree_node(&new_root);
        kds_set_page_dirty(new_root_frame->kp);

        kds_buf_unpin(new_root_frame);

        /*
         * TODO: Update root page_id in metadata -- whatever points
         * callers at "the root page for this btree" (e.g. a
         * catalog row's desc_page_id, see catalog.c's
         * kds_sys_table_t) must be updated to new_root_frame->kp->id
         * now, or every future lookup will keep finding the old,
         * now-demoted-to-INTERNAL root and never reach the real one.
         * This was an unresolved TODO in the pre-migration version
         * of this function too -- carried forward rather than
         * guessed at here, since the right fix depends on the
         * catalog layer (which table/index this btree belongs to),
         * which this function has no way to know.
         */
    }

    return 0;
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