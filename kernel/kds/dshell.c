#include <linux/kds.h>
#include <linux/kds_proc.h>
#include <linux/kds_dshell.h>
#include <linux/kds_page.h>
#include <linux/kds_page_mgr.h>
#include <linux/kds_btree.h>
#include <linux/kds_page_alloc.h>

#include <linux/kernel.h>
#include <linux/sched.h>

#include <linux/net.h>
#include <linux/in.h>
#include <linux/socket.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/printk.h>

#include <net/sock.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/mm.h>

typedef int (*kds_dshell_fn_t)(char **argv);

typedef struct kds_dshell_cmd {
    const char        *op;
    kds_dshell_fn_t    fn;
    int                min_args;
} kds_dshell_cmd_t;

/*
 * All commands below that touch page contents now go through
 * kds_buf_lookup_or_load() (kds_page_mgr.h) instead of the removed
 * kds_get_page_cache(). That returns a pinned kds_frame_t*, which
 * every callee that used to take kds_page_t* (kds_set_page_buffer,
 * kds_update_page_hdr, kds_commit_page_hdr, ...) now takes instead.
 * Every successful lookup_or_load() must be matched with
 * kds_buf_unpin() before returning -- including on error paths after
 * the frame was obtained.
 */

int kds_cmd_write_page_buffer(char **argv)
{
    kds_page_id_t   page_id;
    kds_offset_t    offset;
    char*           buffer;
    kds_size_t      size;
    kds_frame_t     *frame;
    int ret;

    ret = kstrtou64(argv[1], 10, &page_id);
    if (ret)
        return ret;

    ret = kstrtou64(argv[2], 10, &offset);
    if (ret)
        return ret;

    ret = kstrtou64(argv[3], 10, &size);
    if (ret)
        return ret;

    if (offset + size > KDS_PAGE_SIZE)
        return -EINVAL;

    buffer = argv[4];

    frame = kds_buf_lookup_or_load(page_id);
    if (IS_ERR(frame))
        return PTR_ERR(frame);

    ret = kds_set_page_buffer(frame, buffer, offset, size);

    kds_buf_unpin(frame);

    pr_info("write page: %s\n", buffer);
    return ret;
}

int kds_cmd_get_page_hdr(char **argv)
{
    int ret;
    kds_page_id_t   page_id;
    kds_frame_t     *frame;

    ret = kstrtou64(argv[1], 10, &page_id);
    if (ret)
        return ret;

    frame = kds_buf_lookup_or_load(page_id);
    if (IS_ERR(frame))
        return PTR_ERR(frame);

    pr_info("kds page hdr: htype=%d, crc=%d, flags=%d\n",
            frame->kp->hdr.type, frame->kp->hdr.crc, frame->kp->hdr.flags);

    kds_buf_unpin(frame);
    return 0;
}

/*
 * BSET / BGET / BIN are disabled for now.
 *
 * They depend on kds_page_alloc() (slated for removal per earlier
 * discussion) and on kds_btree.h functions (btree_init_root_kpage(),
 * btree_link_kpage(), load_btree_node(), btree_insert(), ...) whose
 * signatures we have not migrated in this pass -- we don't have
 * visibility into kds_btree.c/kds_page_alloc.c to know whether they
 * still expect kds_page_t* or have already moved to kds_frame_t*.
 * Guessing here would compile but silently pass the wrong type
 * across that boundary. These three commands need to be revisited
 * together with the btree/page_alloc migration, not guessed at here.
 */
int kds_cmd_set_btree_as_root(char **argv)
{
    pr_warn("BSET: disabled pending kds_btree.h / kds_page_alloc migration to kds_frame_t*\n");
    return -ENOSYS;
}

int kds_cmd_get_btree_hdr(char **argv)
{
    pr_warn("BGET: disabled pending kds_btree.h migration to kds_frame_t*\n");
    return -ENOSYS;
}

int kds_cmd_insert_btree_value(char **argv)
{
    pr_warn("BIN: disabled pending kds_btree.h / kds_page_alloc migration to kds_frame_t*\n");
    return -ENOSYS;
}

int kds_cmd_set_page_hdr(char **argv)
{
    int ret;
    kds_page_id_t   page_id;
    u32 htype;
    u32 crc;
    u64 min_key;
    kds_page_hdr_t hdr;
    kds_frame_t *frame;

    ret = kstrtou64(argv[1], 10, &page_id);
    if (ret)
        return ret;

    ret = kstrtou32(argv[2], 10, &htype);
    if (ret)
        return ret;

    ret = kstrtou32(argv[3], 10, &crc);
    if (ret)
        return ret;

    ret = kstrtou64(argv[4], 10, &min_key);
    if (ret)
        return ret;

    hdr.type = htype;
    hdr.crc = crc;
    /* min_key no longer lives in the common header -- see the
     * kds_page_hdr_t redesign discussed earlier. Not settable here
     * until the btree-specific metadata struct is wired up. */

    frame = kds_buf_lookup_or_load(page_id);
    if (IS_ERR(frame))
        return PTR_ERR(frame);

    /* kds_update_page_hdr() already writes hdr into the page and
     * assigns it into frame->kp->hdr -- no need to also assign it
     * manually beforehand (the old code did both, redundantly). */
    ret = kds_update_page_hdr(frame, &hdr);

    kds_buf_unpin(frame);
    return ret;
}

int kds_cmd_get_page_buffer(char **argv)
{
    kds_page_id_t   page_id;
    kds_offset_t    offset;
    kds_size_t      size;
    kds_frame_t     *frame;
    const void      *src;
    int ret;

    ret = kstrtou64(argv[1], 10, &page_id);
    if (ret)
        return ret;

    ret = kstrtou64(argv[2], 10, &offset);
    if (ret)
        return ret;

    ret = kstrtou64(argv[3], 10, &size);
    if (ret)
        return ret;

    if (size >= 256)
        return -EINVAL;

    frame = kds_buf_lookup_or_load(page_id);
    if (IS_ERR(frame))
        return PTR_ERR(frame);

    /*
     * Direct kmap_local_page(kds_page->page) is gone -- kds_page_t
     * no longer has a `page` field. Use the frame's read-pointer API
     * instead, which takes the content lock internally.
     */
    src = kds_frame_get_read_ptr(frame, offset, size);
    if (IS_ERR(src)) {
        kds_buf_unpin(frame);
        return PTR_ERR(src);
    }

    {
        char buffer[256];

        memset(buffer, 0, sizeof(buffer));
        memcpy(buffer, src, size);

        kds_frame_put_read_ptr(frame);
        kds_buf_unpin(frame);

        pr_info("page buffer: %s\n", buffer);
    }

    return 0;
}

static const struct kds_dshell_cmd kds_dshell_cmds[] = {
    { "GET",    kds_cmd_get_page_buffer,  3 },
    { "WRITE",  kds_cmd_write_page_buffer,  4 },
    { "SETHDR", kds_cmd_set_page_hdr, 4 },
    { "GETHDR", kds_cmd_get_page_hdr, 4 },
    { "BSET", kds_cmd_set_btree_as_root, 1 },
    { "BGET", kds_cmd_get_btree_hdr, 1 },
    { "BIN", kds_cmd_insert_btree_value, 2 }
};

#define KDS_DSHELL_CMD_COUNT \
    (sizeof(kds_dshell_cmds) / sizeof(kds_dshell_cmds[0]))


static int __kds_dispatch_dshell_cmd(char **argv, int argc)
{
    char *op_code = argv[0];

    pr_info("kds_dispatch: %s\n", op_code);

    for(int i = 0; i < KDS_DSHELL_CMD_COUNT; i++) {
        if (!strcmp(argv[0], kds_dshell_cmds[i].op))    
            return kds_dshell_cmds[i].fn(argv);
    }

    return -1;
}

#define KDS_SHM_NAME "kds_shm"
#define KDS_SHM_RING_SIZE PAGE_ALIGN(sizeof(shm_ring_t))
#define KDS_SHM_RING_LEN 4096
#define MAX_DSHELL_ARGS 256

/* shared ring 구조체 (유저와 동일해야 함) */
typedef struct {
    volatile u32 head;
    volatile u32 tail;
    char data[KDS_SHM_RING_LEN][256];

} shm_ring_t;

/* shm globals */
static dev_t kds_shm_dev;
static struct cdev kds_shm_cdev;
static struct class *kds_shm_class;
static shm_ring_t *kds_shm_ring;
static bool kds_shm_inited;

int kds_split_cmd(char *buf, char *argv[], int max_args)
{
    int argc = 0;
    char *token;

    if (!buf || !argv || max_args <= 0)
        return -EINVAL;

    while ((token = strsep(&buf, " \t\n")) != NULL) {
        if (*token == '\0')
            continue; 

        argv[argc++] = token;
        if (argc >= max_args)
            break;
    }

    return argc;
}

static int kds_shm_mmap(struct file *filp, struct vm_area_struct *vma)
{
    unsigned long pfn;

    if (!kds_shm_ring)
        return -EINVAL;

    if ((vma->vm_end - vma->vm_start) > KDS_SHM_RING_SIZE)
        return -EINVAL;

    pfn = virt_to_phys(kds_shm_ring) >> PAGE_SHIFT;

    return remap_pfn_range(vma,
                           vma->vm_start,
                           pfn,
                           KDS_SHM_RING_SIZE,
                           vma->vm_page_prot);
}

static const struct file_operations kds_shm_fops = {
    .owner = THIS_MODULE,
    .mmap  = kds_shm_mmap,
};

static int kds_init_shm_device(void)
{
    int ret;
    struct device *dev;

    if (kds_shm_inited)
        return 0;

    ret = alloc_chrdev_region(&kds_shm_dev, 0, 1, KDS_SHM_NAME);
    if (ret)
        return ret;

    kds_shm_ring = kmalloc(KDS_SHM_RING_SIZE, GFP_KERNEL | __GFP_ZERO);
    if (!kds_shm_ring)
        return -ENOMEM;

    cdev_init(&kds_shm_cdev, &kds_shm_fops);
    kds_shm_cdev.owner = THIS_MODULE;

    ret = cdev_add(&kds_shm_cdev, kds_shm_dev, 1);
    pr_info("kds_shm: after cdev_add ret=%d\n", ret);

    if (ret)
        return ret;

    kds_shm_class = class_create("kds");

    pr_info("kds_shm: class ptr=%p\n", kds_shm_class);

    dev = device_create(kds_shm_class,
                  NULL,
                  kds_shm_dev,
                  NULL,
                  KDS_SHM_NAME);

    if (IS_ERR(dev)) {
        pr_err("kds_shm: device_create failed: %ld\n", PTR_ERR(dev));
        return PTR_ERR(dev);
    }

    pr_info("kds_shm: devt major=%d minor=%d\n",
        MAJOR(kds_shm_dev), MINOR(kds_shm_dev));

    kds_shm_inited = true;

    pr_info("kds_shm: /dev/%s initialized\n", KDS_SHM_NAME);
    return 0;
}



/* ================================
 * dshell state machine
 * ================================ */
typedef enum {
    KDS_DSHELL_STATE_INIT = 0,
    KDS_DSHELL_STATE_ACCEPT = 1,
    KDS_DSHELL_STATE_RECV = 2,
    KDS_DSHELL_STATE_EXEC = 3,
    KDS_DSHELL_STATE_SEND = 4,
} kds_dshell_state_t;

/* ================================
 * dshell context (proc ctx)
 * ================================ */
typedef struct kds_dshell_ctx {
    kds_dshell_state_t state;
} kds_dshell_ctx_t;

/* ================================
 * command handler (minimal)
 * ================================ */
static void kds_dshell_exec(kds_dshell_ctx_t *ctx)
{
}

/* ================================
 * dshell proc run()
 * ================================ */
static kds_proc_result_t
kds_proc_dshell_run(kds_proc_t *proc, u64 slice_ns)
{
    kds_dshell_ctx_t *ctx = proc->ctx;
    int ret;
    char *argv[MAX_DSHELL_ARGS];
    int argc;
    cond_resched();

    if (kds_shm_ring) {
        u32 head = READ_ONCE(kds_shm_ring->head);
        u32 tail = READ_ONCE(kds_shm_ring->tail);

        if (head != tail) {
            char *buf = kds_shm_ring->data[head];

            pr_info("shm ring recv [idx=%u]: %.*s\n",
                    head,
                    256,
                    buf);

            argc = kds_split_cmd(buf, argv, MAX_DSHELL_ARGS);
            pr_info("parsed argc len: %d\n", argc);

            pr_info("op: %s\n", argv[0]);

            for (int i = 0; i < argc; i++) {
                pr_info("argv[%d] = '%s'\n", i, argv[i]);
            }

            ret = __kds_dispatch_dshell_cmd(argv, argc);
            if(ret)
                pr_info("cmd func returned: %d\n", ret);

            smp_wmb();
            WRITE_ONCE(kds_shm_ring->head, (head + 1) % KDS_SHM_RING_LEN);
        }
    }

    return KDS_PROC_CONTINUE;
}

/* ================================
 * proc registration
 * ================================ */
int kds_init_dshell_system(void)
{

    kds_proc_t *proc;
    kds_dshell_ctx_t *ctx;

    ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
    if (!ctx)
        return 1;

    proc = kzalloc(sizeof(*proc), GFP_KERNEL);
    if (!proc) {
        kfree(ctx);
        return 1;
    }

    proc->name         = "kds_dshell";
    proc->kind         = KDS_PROC_SYSTEM;
    proc->static_prio  = KDS_PROC_PRIORITY_SYSTEM_BACKGROUND;
    proc->dynamic_prio = proc->static_prio;
    proc->run          = kds_proc_dshell_run;
    proc->ctx          = ctx;
    proc->state        = KDS_PROC_STATE_READY;

    kds_proc_register(proc);

    pr_info("dshell: registered as kds_proc\n");
    return 0;
}

static int __init kds_shm_early_init(void)
{
    if(kds_init_shm_device()) {
        pr_info("kds_init shm device failed!");
    } else {
        pr_info("kds_init shm device ok!");
    }
    return 0;
}
late_initcall(kds_shm_early_init);