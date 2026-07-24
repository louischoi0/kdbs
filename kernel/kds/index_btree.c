/* index_btree.c
 *
 * A correct, self-contained B+-tree used to back *secondary indexes*
 * (kds_index_insert/search/delete on a kds_relation_t whose kind is
 * KDS_CLUSTERED_BTREE and whose schema.nr_cols == 0 -- see
 * kds_relation.h / kds_relation_create_index()).
 *
 * WHY A SEPARATE B+-TREE, NOT THE ONE IN btree.c
 * ----------------------------------------------
 * btree.c implements a *separator B-tree* whose leaves the clustered
 * table paths (exec_btree_insert.c / exec_btree_select.c) interpret as
 * pointers to heap *bucket* pages, and whose split path (btree.c:
 * kds_btree_cursor_insert -> btree_node_insert_at into an already-full
 * node -> btree_split_node) has two problems that make it unsafe as an
 * exact key->value store:
 *
 *   1. It inserts into a full node *before* splitting, writing one past
 *      keys[BTREE_MAX_KEYS]/slots[BTREE_MAX_KEYS+1] (out of bounds).
 *   2. A leaf's value for keys[i] is written at slots[i+1] by
 *      btree_node_insert_at(), but btree_split_node() copies slots[i]
 *      aligned to keys[i], so leaf values misalign across a split.
 *      kds_index_search() (old, in relation.c) read slots[i] -- off by
 *      one even before a split.
 *
 * Fixing those in-place would mean reworking the split that the
 * clustered *table* code depends on, which cannot be verified without
 * booting the kernel. So secondary indexes get their own tree here,
 * with a clean B+ convention and bounds-safe splits, leaving the
 * clustered-table btree untouched. The clustered-table split bounds
 * bug remains a separate, independently-testable follow-up.
 *
 * CONVENTION
 * ----------
 * Reuses kds_btree_node_t / load_btree_node() / store_btree_node() and
 * the on-page layout, but interprets nodes as a B+-tree:
 *
 *   - Leaf (level == 0):   keys[0..key_count-1] with slots[i] == the
 *                          value (row's heap page_id) for keys[i].
 *                          `next` links to the right leaf sibling.
 *   - Internal (level>0):  keys[0..key_count-1] are separators;
 *                          slots[0..key_count] are child page ids
 *                          (slots[i] holds keys < keys[i]; slots[i] for
 *                          i==key_count holds keys >= keys[key_count-1]).
 *                          A separator equals the smallest key in the
 *                          subtree to its right (B+ semantics: the
 *                          separator is a *copy*; the real key lives on
 *                          in a leaf).
 *
 * Splits use small on-stack temp buffers (one element larger than a
 * node) so a full node is never written out of bounds. On a root split
 * the new root page id is persisted through
 * kds_catalog_update_relation_desc_page() and the in-memory
 * index_rel->root_page_id is refreshed.
 *
 * SCOPE / LIMITATIONS (all deliberate, documented in kds_relation.h):
 *   - Unique keys only: a duplicate key insert returns -EEXIST.
 *   - Keys are u64 and compared unsigned; callers index integer
 *     columns and only ever do equality lookups, so sign ordering does
 *     not matter here.
 *   - Delete does not merge/rebalance underflowed nodes (it only
 *     removes the key from its leaf). Search stays correct -- separators
 *     are routing boundaries that need not exist as real keys -- at the
 *     cost of not reclaiming sparse nodes.
 *   - No tree-level locking: matches the clustered btree's existing
 *     concurrency stance (correctness relies on the cooperative
 *     scheduler, not latches).
 */

#include <linux/kds.h>
#include <linux/kds_relation.h>
#include <linux/kds_catalog.h>
#include <linux/kds_btree.h>
#include <linux/kds_page.h>
#include <linux/kds_page_mgr.h>
#include <linux/kds_meta.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/string.h>

#define IDX_MAX        BTREE_MAX_KEYS     /* max keys per node (4)      */
#define IDX_MAX_DEPTH  BTREE_MAX_DEPTH    /* max tree height (16)       */

/* Root-to-leaf path recorded during a descent, so a leaf split can
 * propagate a separator back up without re-descending. */
struct idx_path {
    kds_page_id_t page_ids[IDX_MAX_DEPTH];
    int           positions[IDX_MAX_DEPTH]; /* child slot descended at each internal level */
    int           depth;                    /* index of the leaf in the arrays above */
};

/* ------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------ */

/* Child slot to descend for `key` in an internal node, and the key
 * position at which a separator for a split child is inserted: the
 * first i with key < keys[i], else key_count. */
static int idx_child_pos(const kds_btree_node_t *n, kds_tuple_id_t key)
{
    int i;

    for (i = 0; i < n->key_count; i++) {
        if (key < n->keys[i])
            return i;
    }
    return n->key_count;
}

/* Allocates a fresh, empty index page of the given level (0 == leaf).
 * Returns a pinned frame with its type/header committed, or ERR_PTR.
 * The caller is responsible for building and storing the node body. */
static kds_frame_t *idx_alloc_page(u64 level)
{
    kds_page_id_t id = assign_page_id();
    kds_frame_t  *f;

    if (!id)
        return ERR_PTR(-ENOMEM); /* superblock not up -- treat as alloc failure */

    f = kds_buf_alloc_new(id);
    if (IS_ERR(f))
        return f;

    f->kp->hdr.type = (level == 0) ? KDS_PAGE_TYPE_BTREE_DATA
                                   : KDS_PAGE_TYPE_BTREE_INTERNAL;
    f->kp->hdr.flags |= KDS_PAGE_FLAG_INIT;
    kds_commit_page_hdr(f);

    return f;
}

/* Patches page `pid`'s `prev` link to `new_prev` (leaf-chain fixup
 * after a split inserts a new leaf to the left of `pid`). Best-effort:
 * a failure to load only leaves a back-link stale, which the forward
 * `next` chain and top-down search never rely on. */
static void idx_fix_prev(kds_page_id_t pid, kds_page_id_t new_prev)
{
    kds_frame_t     *f;
    kds_btree_node_t n;

    if (!pid)
        return;

    f = kds_buf_lookup_or_load(pid);
    if (IS_ERR(f)) {
        pr_warn("index_btree: failed to relink prev of page %llu: %ld\n",
                (u64)pid, PTR_ERR(f));
        return;
    }

    load_btree_node(f, &n);
    n.prev = new_prev;
    store_btree_node(&n);
    kds_set_page_dirty(f->kp);
    kds_buf_unpin(f);
}

/* After a new root is created above `old_root_pid`, the old root is now
 * an ordinary child -- demote its page type from BTREE_ROOT so any
 * type-driven traversal (kds_btree_traverse()) classifies it correctly.
 * Search here keys off node.level, not type, so this is hygiene, not a
 * correctness requirement for this file. */
static void idx_demote_old_root(kds_page_id_t old_root_pid)
{
    kds_frame_t     *f;
    kds_btree_node_t n;

    f = kds_buf_lookup_or_load(old_root_pid);
    if (IS_ERR(f))
        return;

    load_btree_node(f, &n);
    f->kp->hdr.type = (n.level == 0) ? KDS_PAGE_TYPE_BTREE_DATA
                                     : KDS_PAGE_TYPE_BTREE_INTERNAL;
    kds_commit_page_hdr(f);
    kds_buf_unpin(f);
}

/* Descends from `root` to the leaf that would hold `key`, recording the
 * path. Leaves no frame pinned. Returns 0, or a negative errno. */
static int idx_descend(kds_page_id_t root, kds_tuple_id_t key,
                       struct idx_path *path)
{
    kds_page_id_t pid = root;
    int           d   = 0;

    for (;;) {
        kds_frame_t     *frame;
        kds_btree_node_t node;
        int              pos;

        if (d >= IDX_MAX_DEPTH)
            return -E2BIG;

        frame = kds_buf_lookup_or_load(pid);
        if (IS_ERR(frame))
            return PTR_ERR(frame);

        load_btree_node(frame, &node);
        path->page_ids[d] = pid;

        if (node.level == 0) {
            path->depth = d;
            kds_buf_unpin(frame);
            return 0;
        }

        pos = idx_child_pos(&node, key);
        path->positions[d] = pos;
        pid = node.slots[pos];
        kds_buf_unpin(frame);
        d++;
    }
}

/* ------------------------------------------------------------------
 * Split propagation
 *
 * A child at path->page_ids[level+1] just split, promoting separator
 * `sep` with new right-sibling page `right_pid`. Insert (sep,right_pid)
 * into each ancestor, splitting internal nodes as needed. If the split
 * reaches above the current root, a new root is created and its page id
 * is returned in *out_new_root (else *out_new_root is left 0).
 * ------------------------------------------------------------------ */
static int idx_propagate(struct idx_path *path, kds_tuple_id_t sep,
                         kds_page_id_t right_pid, kds_page_id_t *out_new_root)
{
    int level;

    for (level = path->depth - 1; level >= 0; level--) {
        kds_frame_t     *frame;
        kds_btree_node_t node;
        int              pos = path->positions[level];

        frame = kds_buf_lookup_or_load(path->page_ids[level]);
        if (IS_ERR(frame))
            return PTR_ERR(frame);

        load_btree_node(frame, &node);

        if (node.key_count < IDX_MAX) {
            /* Room: insert separator at key pos, right child at pos+1.
             * This is exactly btree_node_insert_at()'s internal-node
             * semantics, and is in-bounds because key_count < IDX_MAX. */
            btree_node_insert_at(&node, pos, sep, right_pid);
            store_btree_node(&node);
            kds_set_page_dirty(frame->kp);
            kds_buf_unpin(frame);
            return 0;
        }

        /* Full internal node: split via temp buffers (one larger than a
         * node) so nothing is written out of bounds. */
        {
            kds_tuple_id_t tk[IDX_MAX + 1];
            kds_page_id_t  tc[IDX_MAX + 2];
            int            nk = node.key_count;   /* == IDX_MAX */
            int            mid = (IDX_MAX + 1) / 2;
            int            rcount = nk - mid;
            kds_tuple_id_t promo;
            kds_frame_t   *rframe;
            kds_btree_node_t rnode;
            kds_page_id_t  new_right;
            int            i;

            /* children into tc, inserting right_pid at pos+1 */
            for (i = 0; i <= nk; i++)
                tc[i] = node.slots[i];
            for (i = nk + 1; i > pos + 1; i--)
                tc[i] = tc[i - 1];
            tc[pos + 1] = right_pid;

            /* keys into tk, inserting sep at pos */
            for (i = 0; i < nk; i++)
                tk[i] = node.keys[i];
            for (i = nk; i > pos; i--)
                tk[i] = tk[i - 1];
            tk[pos] = sep;

            promo = tk[mid];

            rframe = idx_alloc_page(node.level);
            if (IS_ERR(rframe)) {
                kds_buf_unpin(frame);
                return PTR_ERR(rframe);
            }
            new_right = rframe->kp->id;

            /* left keeps keys tk[0..mid-1], children tc[0..mid] */
            node.key_count = mid;
            for (i = 0; i < mid; i++)
                node.keys[i] = tk[i];
            for (i = 0; i <= mid; i++)
                node.slots[i] = tc[i];

            /* right gets keys tk[mid+1..nk], children tc[mid+1..nk+1] */
            memset(&rnode, 0, sizeof(rnode));
            rnode.level     = node.level;
            rnode.frame     = rframe;
            rnode.key_count = rcount;
            for (i = 0; i < rcount; i++)
                rnode.keys[i] = tk[mid + 1 + i];
            for (i = 0; i <= rcount; i++)
                rnode.slots[i] = tc[mid + 1 + i];

            store_btree_node(&node);
            kds_set_page_dirty(frame->kp);
            store_btree_node(&rnode);
            kds_set_page_dirty(rframe->kp);

            kds_buf_unpin(rframe);
            kds_buf_unpin(frame);

            /* promote `promo`/`new_right` into the next level up */
            sep       = promo;
            right_pid = new_right;
        }
    }

    /* Fell out above the root: grow the tree by one level. */
    {
        kds_frame_t     *rootf;
        kds_btree_node_t rootn;
        kds_page_id_t    new_root_id;
        u64              new_level = (u64)path->depth + 1;

        rootf = idx_alloc_page(new_level);
        if (IS_ERR(rootf))
            return PTR_ERR(rootf);

        rootf->kp->hdr.type = KDS_PAGE_TYPE_BTREE_ROOT;
        kds_commit_page_hdr(rootf);
        new_root_id = rootf->kp->id;

        memset(&rootn, 0, sizeof(rootn));
        rootn.level     = new_level;
        rootn.frame     = rootf;
        rootn.key_count = 1;
        rootn.keys[0]   = sep;
        rootn.slots[0]  = path->page_ids[0];
        rootn.slots[1]  = right_pid;

        store_btree_node(&rootn);
        kds_set_page_dirty(rootf->kp);
        kds_buf_unpin(rootf);

        idx_demote_old_root(path->page_ids[0]);

        *out_new_root = new_root_id;
    }

    return 0;
}

/* ------------------------------------------------------------------
 * Public API (declared in kds_relation.h)
 * ------------------------------------------------------------------ */

int kds_index_insert(kds_relation_t *index_rel, kds_tuple_id_t key,
                     kds_page_id_t value_page_id)
{
    struct idx_path  path;
    kds_btree_node_t leaf;
    kds_frame_t     *frame;
    kds_page_id_t    new_root = 0;
    int              pos, i, ret;

    if (!index_rel || index_rel->kind != KDS_CLUSTERED_BTREE)
        return -EINVAL;
    if (!index_rel->root_page_id)
        return -ENODEV;

    ret = idx_descend(index_rel->root_page_id, key, &path);
    if (ret)
        return ret;

    frame = kds_buf_lookup_or_load(path.page_ids[path.depth]);
    if (IS_ERR(frame))
        return PTR_ERR(frame);

    load_btree_node(frame, &leaf);

    /* Locate insert position; reject duplicates (unique index). */
    pos = leaf.key_count;
    for (i = 0; i < leaf.key_count; i++) {
        if (key == leaf.keys[i]) {
            kds_buf_unpin(frame);
            return -EEXIST;
        }
        if (key < leaf.keys[i]) {
            pos = i;
            break;
        }
    }

    if (leaf.key_count < IDX_MAX) {
        /* Room in the leaf: shift right and insert. */
        for (i = leaf.key_count; i > pos; i--) {
            leaf.keys[i]  = leaf.keys[i - 1];
            leaf.slots[i] = leaf.slots[i - 1];
        }
        leaf.keys[pos]  = key;
        leaf.slots[pos] = value_page_id;
        leaf.key_count++;

        store_btree_node(&leaf);
        kds_set_page_dirty(frame->kp);
        kds_buf_unpin(frame);
        return 0;
    }

    /* Leaf full: split into (leaf, right) via a temp buffer. */
    {
        kds_tuple_id_t tk[IDX_MAX + 1];
        kds_page_id_t  tv[IDX_MAX + 1];
        int            total = IDX_MAX + 1;
        int            L = (total + 1) / 2;   /* left keeps ceil(total/2) */
        int            R = total - L;
        kds_frame_t   *rframe;
        kds_btree_node_t rnode;
        kds_page_id_t  right_pid;
        kds_tuple_id_t sep;
        int            j;

        for (i = 0, j = 0; i < IDX_MAX; i++) {
            if (i == pos) {
                tk[j] = key;
                tv[j] = value_page_id;
                j++;
            }
            tk[j] = leaf.keys[i];
            tv[j] = leaf.slots[i];
            j++;
        }
        if (pos == IDX_MAX) {          /* new key is the largest */
            tk[j] = key;
            tv[j] = value_page_id;
            j++;
        }

        rframe = idx_alloc_page(0);
        if (IS_ERR(rframe)) {
            kds_buf_unpin(frame);
            return PTR_ERR(rframe);
        }
        right_pid = rframe->kp->id;

        /* Left half stays in `leaf`. */
        for (i = 0; i < L; i++) {
            leaf.keys[i]  = tk[i];
            leaf.slots[i] = tv[i];
        }
        leaf.key_count = L;

        /* Right half goes to the new leaf. */
        memset(&rnode, 0, sizeof(rnode));
        rnode.level     = 0;
        rnode.frame     = rframe;
        rnode.key_count = R;
        for (i = 0; i < R; i++) {
            rnode.keys[i]  = tk[L + i];
            rnode.slots[i] = tv[L + i];
        }

        /* Splice the new leaf into the sibling chain to `leaf`'s right. */
        rnode.next = leaf.next;
        rnode.prev = leaf.frame->kp->id;
        leaf.next  = right_pid;

        sep = rnode.keys[0];   /* B+ separator: min key of the right leaf */

        store_btree_node(&leaf);
        kds_set_page_dirty(frame->kp);
        store_btree_node(&rnode);
        kds_set_page_dirty(rframe->kp);

        if (rnode.next)
            idx_fix_prev(rnode.next, right_pid);

        kds_buf_unpin(rframe);
        kds_buf_unpin(frame);

        ret = idx_propagate(&path, sep, right_pid, &new_root);
        if (ret)
            return ret;
    }

    if (new_root) {
        index_rel->root_page_id = new_root;
        ret = kds_catalog_update_relation_desc_page(index_rel->oid, new_root);
        if (ret)
            pr_warn("index_btree: root grew but persisting new root %llu for "
                    "index oid %llu failed: %d (in-memory root updated)\n",
                    (u64)new_root, (u64)index_rel->oid, ret);
    }

    return 0;
}

int kds_index_search(kds_relation_t *index_rel, kds_tuple_id_t key,
                     kds_page_id_t *out_value_page_id)
{
    kds_page_id_t pid;
    int           depth = 0;

    if (!index_rel || !out_value_page_id ||
        index_rel->kind != KDS_CLUSTERED_BTREE)
        return -EINVAL;
    if (!index_rel->root_page_id)
        return -ENODEV;

    pid = index_rel->root_page_id;

    while (depth < IDX_MAX_DEPTH) {
        kds_frame_t     *frame;
        kds_btree_node_t node;
        int              i;

        frame = kds_buf_lookup_or_load(pid);
        if (IS_ERR(frame))
            return PTR_ERR(frame);

        load_btree_node(frame, &node);

        if (node.level == 0) {
            for (i = 0; i < node.key_count; i++) {
                if (node.keys[i] == key) {
                    *out_value_page_id = node.slots[i];
                    kds_buf_unpin(frame);
                    return 0;
                }
            }
            kds_buf_unpin(frame);
            return -ENOENT;
        }

        pid = node.slots[idx_child_pos(&node, key)];
        kds_buf_unpin(frame);
        depth++;
    }

    return -E2BIG;
}

/*
 * Removes `key` from the index. Deliberately does not merge or
 * rebalance an underflowed leaf (see this file's header): the key is
 * simply dropped from its leaf. Returns 0, -ENOENT if the key isn't
 * present, or a negative errno.
 */
int kds_index_delete(kds_relation_t *index_rel, kds_tuple_id_t key)
{
    struct idx_path  path;
    kds_btree_node_t leaf;
    kds_frame_t     *frame;
    int              i, found = -1, ret;

    if (!index_rel || index_rel->kind != KDS_CLUSTERED_BTREE)
        return -EINVAL;
    if (!index_rel->root_page_id)
        return -ENODEV;

    ret = idx_descend(index_rel->root_page_id, key, &path);
    if (ret)
        return ret;

    frame = kds_buf_lookup_or_load(path.page_ids[path.depth]);
    if (IS_ERR(frame))
        return PTR_ERR(frame);

    load_btree_node(frame, &leaf);

    for (i = 0; i < leaf.key_count; i++) {
        if (leaf.keys[i] == key) {
            found = i;
            break;
        }
    }

    if (found < 0) {
        kds_buf_unpin(frame);
        return -ENOENT;
    }

    for (i = found; i < leaf.key_count - 1; i++) {
        leaf.keys[i]  = leaf.keys[i + 1];
        leaf.slots[i] = leaf.slots[i + 1];
    }
    leaf.key_count--;

    store_btree_node(&leaf);
    kds_set_page_dirty(frame->kp);
    kds_buf_unpin(frame);
    return 0;
}
