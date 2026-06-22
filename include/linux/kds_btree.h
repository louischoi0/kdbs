#ifndef __KDS_BTREE_H
#define __KDS_BTREE_H

#include <linux/kds.h>
#include <linux/kds_page_mgr.h>

#define BTREE_MAX_KEYS 4
#define BTREE_MAX_DEPTH 16
#define PRINT_FIELD_INFO(struct_type, field) \
    pr_info("  %-12s offset %3zu, size %2zu, end %3zu\n", \
            #field, \
            offsetof(struct_type, field), \
            sizeof(((struct_type*)0)->field), \
            offsetof(struct_type, field) + sizeof(((struct_type*)0)->field))

typedef struct {
    u64               level;
    u64               key_count;
    kds_tuple_id_t    keys[BTREE_MAX_KEYS];
    kds_page_id_t     slots[BTREE_MAX_KEYS + 1];
    kds_page_id_t     next;
    kds_page_id_t     prev;
} __attribute__((packed)) kds_btree_page_data_t;

/*
 * kds_btree_node_t no longer embeds a kds_page_t* directly. It now
 * holds the kds_frame_t* that owns both the buffer (frame->page) and
 * the content lock + header (frame->kp) -- see the kds_page_t /
 * kds_frame_t ownership split in kds_core.h / kds_page_mgr.h.
 */
typedef struct kds_btree_node {
    u64               level;
    u64               key_count;
    kds_tuple_id_t    keys[BTREE_MAX_KEYS];
    kds_page_id_t     slots[BTREE_MAX_KEYS + 1];
    kds_page_id_t     next;
    kds_page_id_t     prev;

    kds_frame_t*      frame;
} __attribute__((packed)) kds_btree_node_t;

typedef struct {
    kds_btree_node_t  nodes[BTREE_MAX_DEPTH];
    int               positions[BTREE_MAX_DEPTH];  /* insertion position at each level */
    int               depth;                        /* current depth (0 = root) */
} kds_btree_cursor_t;

typedef struct {
    kds_tuple_id_t    promoted_key;
    kds_page_id_t     right_page_id;
} kds_btree_split_result_t;

typedef int (*kds_btree_traverse_cb)(
    struct kds_btree_node *node,
    kds_frame_t *frame,
    int depth,
    int key_idx,
    void *private);

typedef struct kds_btree_traverse_ctx {
    kds_btree_traverse_cb callback;
    void *private;
    int depth;
    u64 visited_nodes;
    u64 visited_keys;
} kds_btree_traverse_ctx_t;

/*
 * These macros used to take a kds_page_t* (x->hdr.type). They now
 * take a kds_frame_t* and go through frame->kp->hdr.type, since hdr
 * lives on kp, not on the frame itself.
 */
#define is_btree_root(x) \
     ((x)->kp->hdr.type == KDS_PAGE_TYPE_BTREE_ROOT)

#define is_btree_internal(x) \
     ((x)->kp->hdr.type == KDS_PAGE_TYPE_BTREE_INTERNAL)

#define is_btree_data(x) \
     ((x)->kp->hdr.type == KDS_PAGE_TYPE_BTREE_DATA)

#define kds_is_btree_page(x) \
    (((x)->kp->hdr.type == KDS_PAGE_TYPE_BTREE_ROOT) || \
     ((x)->kp->hdr.type == KDS_PAGE_TYPE_BTREE_INTERNAL) || \
     ((x)->kp->hdr.type == KDS_PAGE_TYPE_BTREE_DATA))

#define kds_btree_data_offset(addr) ((char*)addr + sizeof(kds_page_hdr_t))

#define kds_btree_data_copy(addr, btree_data) \
    do { \
        memcpy((char*)btree_data, kds_btree_data_offset(addr), \
               sizeof(kds_btree_page_data_t)); \
    } while (0)

#define kds_btree_data_store(addr, btree_data) \
    do { \
        memcpy(kds_btree_data_offset(addr), (btree_data), \
               sizeof(kds_btree_page_data_t)); \
    } while (0)

static inline bool kds_btree_is_leaf(const kds_btree_node_t *node)
{
    return node->level == 0;
}

static inline bool kds_btree_is_full(const kds_btree_node_t *node)
{
    return node->key_count >= BTREE_MAX_KEYS;
}

void btree_link_kpage(kds_frame_t* frame, kds_btree_page_data_t data);
void btree_init_root_kpage(kds_frame_t* frame);
void btree_init_data_kpage(kds_frame_t* frame);
void btree_init_page_data(kds_btree_page_data_t* data);
void load_btree_node(kds_frame_t *frame, kds_btree_node_t *out);
int btree_insert(kds_page_id_t root_page_id, kds_tuple_id_t key, kds_page_id_t value_page_id);
int kds_btree_print(kds_frame_t *root_frame);
void print_btree_node(kds_btree_node_t *node);
void load_btree_node_data(kds_frame_t *frame, kds_btree_page_data_t *out);

static inline void print_layout(void)
{
    pr_info("=== kds_btree_page_data_t ===\n");
    pr_info("Total size: %zu bytes\n\n", sizeof(kds_btree_page_data_t));

    PRINT_FIELD_INFO(kds_btree_page_data_t, level);
    PRINT_FIELD_INFO(kds_btree_page_data_t, key_count);
    PRINT_FIELD_INFO(kds_btree_page_data_t, keys);
    PRINT_FIELD_INFO(kds_btree_page_data_t, slots);
    PRINT_FIELD_INFO(kds_btree_page_data_t, next);
    PRINT_FIELD_INFO(kds_btree_page_data_t, prev);

    pr_info("\n=== kds_btree_node_t ===\n");
    pr_info("Total size: %zu bytes\n\n", sizeof(kds_btree_node_t));

    PRINT_FIELD_INFO(kds_btree_node_t, level);
    PRINT_FIELD_INFO(kds_btree_node_t, key_count);
    PRINT_FIELD_INFO(kds_btree_node_t, keys);
    PRINT_FIELD_INFO(kds_btree_node_t, slots);
    PRINT_FIELD_INFO(kds_btree_node_t, next);
    PRINT_FIELD_INFO(kds_btree_node_t, prev);
    PRINT_FIELD_INFO(kds_btree_node_t, frame);
}

#endif