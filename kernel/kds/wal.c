/* wal.c */
#include <linux/kds.h>
#include <linux/kds_wal.h>
#include <linux/kds_page.h>
#include <linux/kds_page_mgr.h>
#include <linux/kds_page_alloc.h>
#include <linux/kds_meta.h>
#include <linux/kds_proc.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/crc32c.h>
#include <linux/ktime.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/string.h>

/*
 * KDS_PAGE_TYPE_WAL: page type for WAL pages in the shared block device.
 * Defined here until kds_page.h is updated to include it.
 * Must not collide with existing KDS_PAGE_TYPE_* values.
 */
#ifndef KDS_PAGE_TYPE_WAL
#define KDS_PAGE_TYPE_WAL  0x57414C00U  /* 'WAL\0' */
#endif

/*
 * WAL metadata stubs: kds_meta_set_wal_pages() and
 * kds_meta_set_wal_checkpoint_lsn() are not yet in meta.c.
 * Stored in module-static variables until kds_superblock_t is
 * extended and the real meta functions are added.
 */
static kds_page_id_t g_wal_head_page_id;
static kds_page_id_t g_wal_tail_page_id;
/*
 * Single global WAL state. Initialised by kds_wal_init(), torn down
 * by kds_wal_shutdown().
 */
static kds_wal_state_t *g_wal;
static kds_proc_t      *g_ckpt_proc;

static inline void kds_meta_set_wal_pages(kds_page_id_t head,
                                           kds_page_id_t tail)
{
    g_wal_head_page_id = head;
    g_wal_tail_page_id = tail;
}

static inline void kds_meta_set_wal_checkpoint_lsn(kds_lsn_t lsn)
{
    atomic64_set(&g_wal->checkpointed_lsn, (s64) lsn);
}

/*
 * kds_buf_flush_dirty_frames() is defined in page_mgr.c.
 * Forward-declared here until kds_page_mgr.h is updated.
 */
extern void kds_buf_flush_dirty_frames(void);

static inline bool kds_wal_need_checkpoint(kds_lsn_t flush_lsn)
{
    return flush_lsn != (s64) KDS_LSN_INVALID && flush_lsn > atomic64_read(&g_wal->checkpointed_lsn);
}

static inline bool kds_wal_need_flush(void)
{
    return g_wal->buf->write_off != &g_wal->buf->flush_off;
}


/* ------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------ */

static inline u64 wal_buf_used(kds_wal_buf_t *b)
{
    return b->write_off - b->flush_off;
}

static inline u64 wal_buf_free(kds_wal_buf_t *b)
{
    return KDS_WAL_BUF_SIZE - wal_buf_used(b);
}

/*
 * Compute CRC32C over a WAL record header (crc field zeroed) + body.
 * Matches kds_wal_record_verify() on the read path.
 */
static u32 wal_rec_crc(const kds_wal_rec_hdr_t *hdr, const void *body)
{
    kds_wal_rec_hdr_t tmp = *hdr;
    u32 crc;

    tmp.crc = 0;
    crc = crc32c(~0, &tmp, KDS_WAL_REC_HDR_SIZE);
    if (body && hdr->body_len > 0)
        crc = crc32c(crc, body, hdr->body_len);
    return crc ^ ~0;
}

static bool wal_rec_crc_ok(const kds_wal_rec_hdr_t *hdr, const void *body)
{
    return hdr->crc == wal_rec_crc(hdr, body);
}

/*
 * Pointer to the payload area of a WAL page (past both page headers).
 */
static inline u8 *wal_page_payload(void *page_addr)
{
    return (u8 *)page_addr + KDS_PAGE_HDR_SIZE + KDS_WAL_PAGE_HDR_SIZE;
}

static inline kds_wal_page_hdr_t *wal_page_hdr(void *page_addr)
{
    return (kds_wal_page_hdr_t *)((u8 *)page_addr + KDS_PAGE_HDR_SIZE);
}

/* ------------------------------------------------------------------
 * WAL page allocation
 *
 * Allocate one new KDS_PAGE_TYPE_WAL page from the pre-alloc ring.
 * Initialises both the common kds_page_hdr_t and kds_wal_page_hdr_t.
 * Returns a pinned frame on success, ERR_PTR on failure.
 * ------------------------------------------------------------------ */

static kds_frame_t *wal_alloc_page(u64 seg_no, kds_lsn_t start_lsn)
{
    kds_frame_t      *frame;
    void             *addr;
    kds_page_hdr_t   *phdr;
    kds_wal_page_hdr_t *whdr;

    frame = kds_page_alloc(KDS_PAGE_TYPE_WAL);
    if (!frame)
        return ERR_PTR(-ENOSPC);

    kds_page_lock(frame->kp);

    addr = kmap_local_page(frame->page);

    /* Common page header */
    phdr = (kds_page_hdr_t *)addr;
    phdr->type  = KDS_PAGE_TYPE_WAL;
    phdr->flags = KDS_PAGE_FLAG_ALLOC | KDS_PAGE_FLAG_INIT | KDS_PAGE_FLAG_DIRTY;
    phdr->crc   = 0;

    /* WAL page header */
    whdr = wal_page_hdr(addr);
    whdr->seg_no          = seg_no;
    whdr->start_lsn       = start_lsn;
    whdr->used_bytes       = 0;
    whdr->next_wal_page_id = 0;
    whdr->_pad             = 0;

    /* Zero the payload area */
    memset(wal_page_payload(addr), 0, KDS_WAL_PAGE_PAYLOAD);

    kunmap_local(addr);

    frame->kp->hdr = *phdr;
    kds_set_page_dirty(frame->kp);

    kds_page_unlock(frame->kp);
    return frame;
}

/* ------------------------------------------------------------------
 * kds_wal_init
 * ------------------------------------------------------------------ */

int kds_wal_init(void)
{
    kds_wal_state_t *state;
    kds_wal_buf_t   *buf;
    kds_frame_t     *first_page;
    kds_lsn_t        init_lsn;

    state = kzalloc(sizeof(*state), GFP_KERNEL);
    if (!state)
        return -ENOMEM;

    buf = vzalloc(sizeof(*buf));
    if (!buf) {
        kfree(state);
        return -ENOMEM;
    }

    spin_lock_init(&buf->lock);
    buf->write_off  = 0;
    buf->flush_off  = 0;
    buf->current_lsn = KDS_LSN_INVALID;
    buf->tail_seg_no = 1;

    /*
     * Allocate the first WAL page. start_lsn is INVALID at this point
     * because no records have been written yet; the first append will
     * set it via lsn_make(page_id, 0).
     */
    first_page = wal_alloc_page(buf->tail_seg_no, KDS_LSN_INVALID);
    if (IS_ERR(first_page)) {
        vfree(buf);
        kfree(state);
        return PTR_ERR(first_page);
    }

    init_lsn = kds_lsn_make(first_page->kp->id, 0);
    {
        void *addr = kmap_local_page(first_page->page);
        wal_page_hdr(addr)->start_lsn = init_lsn;
        kunmap_local(addr);
    }

    buf->tail_page_id = first_page->kp->id;
    buf->tail_used    = 0;

    /* Persist head/tail in superblock */
    kds_meta_set_wal_pages(first_page->kp->id, first_page->kp->id);

    kds_buf_unpin(first_page);

    atomic64_set(&state->flush_lsn,        (s64)KDS_LSN_INVALID);
    atomic64_set(&state->checkpointed_lsn, (s64)KDS_LSN_INVALID);
    state->prev_lsn = KDS_LSN_INVALID;
    state->buf = buf;

    g_wal = state;

    pr_info("kds_wal: initialised, first WAL page id=%llu lsn=0x%llx\n",
            (u64)buf->tail_page_id, (u64)init_lsn);
    return 0;
}

/* ------------------------------------------------------------------
 * kds_wal_append
 * ------------------------------------------------------------------ */

int kds_wal_append(kds_wal_rec_hdr_t *hdr, const void *body,
                   kds_lsn_t *out_lsn)
{
    kds_wal_buf_t *buf;
    u32            total;
    u64            write_idx;
    kds_lsn_t      lsn;

    if (!g_wal)
        return -ENODEV;

    buf   = g_wal->buf;
    total = KDS_WAL_REC_HDR_SIZE + hdr->body_len;

    if (total > KDS_WAL_MAX_REC_SIZE)
        return -EINVAL;

    spin_lock(&buf->lock);

    if (wal_buf_free(buf) < total) {
        spin_unlock(&buf->lock);
        pr_warn("kds_wal: buffer full, dropping record (type=%u)\n",
                hdr->type);
        return -ENOSPC;
    }

    /*
     * Assign LSN. We don't know the exact WAL page_id until flush
     * time, so the LSN here uses tail_page_id and the current
     * tail_used offset. This works because tail_page_id only advances
     * during flush (which also holds the lock briefly when updating
     * tail_page_id).
     */
    lsn = kds_lsn_make(buf->tail_page_id,
                        buf->tail_used + (u32)wal_buf_used(buf));

    hdr->lsn      = lsn;
    hdr->prev_lsn = g_wal->prev_lsn;
    hdr->crc      = wal_rec_crc(hdr, body);

    /* Copy header into ring buffer */
    write_idx = buf->write_off & KDS_WAL_BUF_MASK;
    if (write_idx + KDS_WAL_REC_HDR_SIZE <= KDS_WAL_BUF_SIZE) {
        memcpy(buf->buf + write_idx, hdr, KDS_WAL_REC_HDR_SIZE);
    } else {
        /* Wrap-around: split across ring boundary */
        u32 first = KDS_WAL_BUF_SIZE - (u32)write_idx;
        memcpy(buf->buf + write_idx, hdr, first);
        memcpy(buf->buf, (u8 *)hdr + first,
               KDS_WAL_REC_HDR_SIZE - first);
    }
    buf->write_off += KDS_WAL_REC_HDR_SIZE;

    /* Copy body */
    if (body && hdr->body_len > 0) {
        write_idx = buf->write_off & KDS_WAL_BUF_MASK;
        if (write_idx + hdr->body_len <= KDS_WAL_BUF_SIZE) {
            memcpy(buf->buf + write_idx, body, hdr->body_len);
        } else {
            u32 first = KDS_WAL_BUF_SIZE - (u32)write_idx;
            memcpy(buf->buf + write_idx, body, first);
            memcpy(buf->buf, (u8 *)body + first,
                   hdr->body_len - first);
        }
        buf->write_off += hdr->body_len;
    }

    buf->current_lsn = lsn;
    g_wal->prev_lsn  = lsn;

    if (out_lsn)
        *out_lsn = lsn;

    spin_unlock(&buf->lock);
    pr_info("kds_wal_append: current_lsn=%d, flush_lsn=%d, ckpt_lsn=%d, write_off=%d\n", buf->current_lsn, g_wal->flush_lsn, g_wal->checkpointed_lsn, buf->write_off);
    return 0;
}

/* ------------------------------------------------------------------
 * kds_wal_commit / kds_wal_abort
 * ------------------------------------------------------------------ */

int kds_wal_commit(u64 xid)
{
    kds_wal_rec_hdr_t     hdr = {0};
    kds_wal_body_xact_t   body = { .xid = xid };
    kds_lsn_t             lsn;

    hdr.xid      = xid;
    hdr.type     = KDS_WAL_REC_COMMIT;
    hdr.body_len = sizeof(body);
    pr_info("kds_wal_commit: xid=%d\n", xid);
    return kds_wal_append(&hdr, &body, &lsn);
}

int kds_wal_abort(u64 xid)
{
    kds_wal_rec_hdr_t     hdr = {0};
    kds_wal_body_xact_t   body = { .xid = xid };
    kds_lsn_t             lsn;

    hdr.xid      = xid;
    hdr.type     = KDS_WAL_REC_ABORT;
    hdr.body_len = sizeof(body);

    return kds_wal_append(&hdr, &body, &lsn);
}

/* ------------------------------------------------------------------
 * kds_wal_get_*_lsn
 * ------------------------------------------------------------------ */

kds_lsn_t kds_wal_get_current_lsn(void)
{
    kds_lsn_t lsn;

    if (!g_wal)
        return KDS_LSN_INVALID;

    spin_lock(&g_wal->buf->lock);
    lsn = g_wal->buf->current_lsn;
    spin_unlock(&g_wal->buf->lock);
    return lsn;
}

kds_lsn_t kds_wal_get_flush_lsn(void)
{
    BUG_ON(!g_wal);
    return (kds_lsn_t)atomic64_read(&g_wal->flush_lsn);
}

kds_lsn_t kds_wal_get_checkpointed_lsn(void)
{
    if (!g_wal)
        return KDS_LSN_INVALID;
    return (kds_lsn_t)atomic64_read(&g_wal->checkpointed_lsn);
}

/* ------------------------------------------------------------------
 * kds_wal_flush
 *
 * Drains the WAL ring buffer to KDS_PAGE_TYPE_WAL pages on disk.
 * Called by the checkpointer proc on each cycle.
 *
 * Algorithm:
 *   1. Snapshot write_off under the lock so we know how many bytes
 *      to flush without holding the lock across disk I/O.
 *   2. Walk the bytes from flush_off to snapshot_write_off, filling
 *      WAL pages one at a time. When a page is full, link it to the
 *      next via next_wal_page_id and allocate a fresh page.
 *   3. After all bytes are written, update flush_lsn.
 * ------------------------------------------------------------------ */

int kds_wal_flush(void)
{
    kds_wal_buf_t *buf;
    u64            flush_start, flush_end;
    u64            bytes_to_flush;

    BUG_ON(!g_wal);

    buf = g_wal->buf;

    /* Snapshot the target under the lock. */
    spin_lock(&buf->lock);
    flush_start   = buf->flush_off;
    flush_end     = buf->write_off;
    spin_unlock(&buf->lock);

    if (flush_start == flush_end)
        return 0; /* nothing to do */

    pr_info("kds_wal_flush: flush_start=%d, flush_end=%d\n", flush_start, flush_end);

    bytes_to_flush = flush_end - flush_start;

    while (bytes_to_flush > 0) {
        kds_frame_t        *frame;
        void               *addr;
        kds_wal_page_hdr_t *whdr;
        u8                 *payload;
        u32                 space, chunk;
        u64                 src_idx;

        /* Load the current tail WAL page into the buffer pool. */
        frame = kds_buf_lookup_or_load(buf->tail_page_id);
        if (IS_ERR(frame)) {
            pr_err("kds_wal: flush: failed to load WAL page %llu: %ld\n",
                   (u64)buf->tail_page_id, PTR_ERR(frame));
            return PTR_ERR(frame);
        }

        kds_page_lock(frame->kp);
        addr    = kmap_local_page(frame->page);
        whdr    = wal_page_hdr(addr);
        payload = wal_page_payload(addr);

        space = KDS_WAL_PAGE_PAYLOAD - whdr->used_bytes;

        if (space == 0) {
            /*
             * This page is already full (shouldn't normally happen
             * since we track tail_used, but be safe). Allocate a new
             * WAL page and link it.
             */
            kunmap_local(addr);
            kds_page_unlock(frame->kp);
            kds_buf_unpin(frame);

            {
                kds_frame_t *new_frame;
                void        *new_addr;

                buf->tail_seg_no++;
                new_frame = wal_alloc_page(buf->tail_seg_no,
                                           kds_lsn_make(buf->tail_page_id,
                                                        buf->tail_used));
                if (IS_ERR(new_frame))
                    return PTR_ERR(new_frame);

                /* Link old tail → new page */
                frame = kds_buf_lookup_or_load(buf->tail_page_id);
                if (!IS_ERR(frame)) {
                    kds_page_lock(frame->kp);
                    new_addr = kmap_local_page(frame->page);
                    wal_page_hdr(new_addr)->next_wal_page_id =
                        new_frame->kp->id;
                    kunmap_local(new_addr);
                    kds_set_page_dirty(frame->kp);
                    kds_page_unlock(frame->kp);
                    kds_buf_unpin(frame);
                }

                buf->tail_page_id = new_frame->kp->id;
                buf->tail_used    = 0;
                kds_buf_unpin(new_frame);
            }
            continue;
        }

        chunk   = (u32)min_t(u64, bytes_to_flush, (u64)space);
        src_idx = flush_start & KDS_WAL_BUF_MASK;

        /* Copy from ring buffer to WAL page payload (handle wrap). */
        if (src_idx + chunk <= KDS_WAL_BUF_SIZE) {
            memcpy(payload + whdr->used_bytes,
                   buf->buf + src_idx, chunk);
        } else {
            u32 first = KDS_WAL_BUF_SIZE - (u32)src_idx;
            memcpy(payload + whdr->used_bytes,
                   buf->buf + src_idx, first);
            memcpy(payload + whdr->used_bytes + first,
                   buf->buf, chunk - first);
        }

        whdr->used_bytes += chunk;
        kds_set_page_dirty(frame->kp);

        kunmap_local(addr);
        kds_page_unlock(frame->kp);

        /* Flush this WAL page to disk. */
        kds_frame_flush(frame);
        kds_buf_unpin(frame);

        flush_start    += chunk;
        bytes_to_flush -= chunk;
        buf->tail_used  = whdr->used_bytes;

        /*
         * If the page is now full, pre-allocate the next one so the
         * next flush iteration doesn't stall waiting for a new page.
         */
        if (buf->tail_used >= KDS_WAL_PAGE_PAYLOAD) {
            kds_frame_t *new_frame;
            kds_frame_t *old_frame;
            void        *old_addr;

            buf->tail_seg_no++;
            new_frame = wal_alloc_page(
                buf->tail_seg_no,
                kds_lsn_make(buf->tail_page_id, buf->tail_used));
            if (IS_ERR(new_frame))
                break; /* not fatal; will retry next flush cycle */

            /* Link current tail → new page */
            old_frame = kds_buf_lookup_or_load(buf->tail_page_id);
            if (!IS_ERR(old_frame)) {
                kds_page_lock(old_frame->kp);
                old_addr = kmap_local_page(old_frame->page);
                wal_page_hdr(old_addr)->next_wal_page_id =
                    new_frame->kp->id;
                kunmap_local(old_addr);
                kds_set_page_dirty(old_frame->kp);
                kds_page_unlock(old_frame->kp);
                kds_frame_flush(old_frame);
                kds_buf_unpin(old_frame);
            }

            buf->tail_page_id = new_frame->kp->id;
            buf->tail_used    = 0;
            kds_buf_unpin(new_frame);
        }
    }

    /* Advance flush_off under the lock. */
    spin_lock(&buf->lock);
    buf->flush_off = flush_end;
    spin_unlock(&buf->lock);

    atomic64_set(&g_wal->flush_lsn,
                 (s64)kds_lsn_make(buf->tail_page_id, buf->tail_used));

    pr_debug("kds_wal: flush finished. flush_lsn=0x%llx\n", (u64)kds_wal_get_flush_lsn());

    return 0;
}

/* ------------------------------------------------------------------
 * kds_wal_checkpoint
 * ------------------------------------------------------------------ */

int kds_wal_checkpoint(kds_lsn_t data_flush_lsn)
{
    kds_wal_rec_hdr_t          hdr  = {0};
    kds_wal_body_checkpoint_t  body = {0};
    kds_lsn_t                  lsn;
    kds_lsn_t                  flush_end_lsn;
    int                        ret;

    body.checkpointed_lsn = data_flush_lsn;
    body.checkpoint_time  = ktime_get_real_seconds();

    hdr.xid      = 0;
    hdr.type     = KDS_WAL_REC_CHECKPOINT;
    hdr.body_len = sizeof(body);

    ret = kds_wal_append(&hdr, &body, &lsn);
    if (ret)
        return ret;

    /* Flush the checkpoint record itself to disk immediately. */
    ret = kds_wal_flush();
    if (ret) {
        panic("kds wal flush failed %d\n", ret);
        return ret;
    }

    flush_end_lsn = kds_wal_get_flush_lsn();
    atomic64_set(&g_wal->checkpointed_lsn, (s64) flush_end_lsn);

    /* Persist checkpointed_lsn in superblock so recovery knows
     * where to start on the next boot. */
    kds_meta_set_wal_checkpoint_lsn(flush_end_lsn);
    kds_superblock_fsync();

    pr_info("kds_wal_checkpoint: checkpoint at lsn=0x%llx\n", (u64) flush_end_lsn);
    return 0;
}

kds_proc_result_t kds_wal_checkpointer_proc(struct kds_proc *proc, u64 slice_ns)
{
    kds_lsn_t flush_lsn;
    int       ret;

    if (!g_wal) 
        return KDS_PROC_YIELD_RET;

    if (kds_wal_need_flush()) {
        ret = kds_wal_flush();
        if (ret) {
            panic("initial flush failed\n");
        }
    }

    flush_lsn = kds_wal_get_flush_lsn();

    if (!kds_wal_need_checkpoint(flush_lsn))
        return KDS_PROC_YIELD_RET;

    kds_buf_flush_dirty_frames();

    ret = kds_wal_checkpoint(flush_lsn);
    if (ret) {
        pr_warn("kds_wal: checkpointer: checkpoint failed: %d\n", ret);
        return KDS_PROC_YIELD_RET;
    }

    return KDS_PROC_YIELD_RET;
}

/* ------------------------------------------------------------------
 * kds_wal_recover
 *
 * Replays WAL records from checkpointed_lsn forward through all
 * linked WAL pages. Called at boot before any user operation.
 * ------------------------------------------------------------------ */

int kds_wal_recover(void)
{
    kds_page_id_t   page_id;
    kds_lsn_t       ckpt_lsn;
    u64             records_replayed = 0;

    if (!g_wal)
        return -ENODEV;

    ckpt_lsn = (kds_lsn_t)atomic64_read(&g_wal->checkpointed_lsn);
    page_id  = kds_lsn_page_id(ckpt_lsn);

    if (page_id == 0) {
        pr_info("kds_wal: recovery: no WAL to replay\n");
        return 0;
    }

    pr_info("kds_wal: recovery: replaying from page %llu (lsn=0x%llx)\n",
            (u64)page_id, (u64)ckpt_lsn);

    while (page_id != 0) {
        kds_frame_t        *frame;
        void               *addr;
        kds_wal_page_hdr_t *whdr;
        u8                 *payload;
        u32                 off = 0;
        kds_page_id_t       next;

        frame = kds_buf_lookup_or_load(page_id);
        if (IS_ERR(frame)) {
            pr_err("kds_wal: recovery: failed to load WAL page %llu\n",
                   (u64)page_id);
            return PTR_ERR(frame);
        }

        addr    = kmap_local_page(frame->page);
        whdr    = wal_page_hdr(addr);
        payload = wal_page_payload(addr);
        next    = whdr->next_wal_page_id;

        /* Walk records within this page. */
        while (off + KDS_WAL_REC_HDR_SIZE <= whdr->used_bytes) {
            kds_wal_rec_hdr_t *rec = (kds_wal_rec_hdr_t *)(payload + off);
            const void        *body = payload + off + KDS_WAL_REC_HDR_SIZE;

            /* Skip records before the checkpoint LSN. */
            if (rec->lsn != KDS_LSN_INVALID && rec->lsn < ckpt_lsn) {
                off += KDS_WAL_REC_HDR_SIZE + rec->body_len;
                continue;
            }

            if (!wal_rec_crc_ok(rec, body)) {
                pr_warn("kds_wal: recovery: CRC mismatch at page %llu "
                        "off %u, stopping replay\n",
                        (u64)page_id, off);
                break;
            }

            if (off + KDS_WAL_REC_HDR_SIZE + rec->body_len >
                whdr->used_bytes)
                break; /* incomplete record at end of page */

            /*
             * Apply the record. Only INSERT and INIT_PAGE need active
             * replay; COMMIT/ABORT/CHECKPOINT are metadata only.
             * Full recovery logic (applying page modifications) will
             * be completed as a follow-up when executor WAL calls are
             * wired in.
             */
            switch (rec->type) {
            case KDS_WAL_REC_INSERT:
            case KDS_WAL_REC_DELETE: {
                const kds_wal_body_page_mod_t *mod =
                    (const kds_wal_body_page_mod_t *)body;
                /* TODO: re-apply page modification to frame */
                pr_debug("kds_wal: recovery: replay %s page=%llu "
                         "off=%u len=%u\n",
                         rec->type == KDS_WAL_REC_INSERT
                         ? "INSERT" : "DELETE",
                         (u64)mod->page_id, mod->offset, mod->length);
                records_replayed++;
                break;
            }
            case KDS_WAL_REC_INIT_PAGE: {
                const kds_wal_body_init_page_t *ip =
                    (const kds_wal_body_init_page_t *)body;
                pr_debug("kds_wal: recovery: replay INIT_PAGE page=%llu "
                         "type=%u\n", (u64)ip->page_id, ip->page_type);
                records_replayed++;
                break;
            }
            case KDS_WAL_REC_CHECKPOINT:
                pr_debug("kds_wal: recovery: found CHECKPOINT record\n");
                break;
            case KDS_WAL_REC_COMMIT:
            case KDS_WAL_REC_ABORT:
                /* transaction state handled by transaction manager */
                break;
            default:
                pr_warn("kds_wal: recovery: unknown record type %u, "
                        "stopping\n", rec->type);
                goto done_page;
            }

            off += KDS_WAL_REC_HDR_SIZE + rec->body_len;
        }

done_page:
        kunmap_local(addr);
        kds_buf_unpin(frame);
        page_id = next;
    }

    pr_info("kds_wal: recovery complete: %llu record(s) replayed\n",
            records_replayed);
    return 0;
}

int kds_wal_checkpointer_init(void)
{
    int ret;

    g_ckpt_proc = vzalloc(sizeof(kds_proc_t));
    if (!g_ckpt_proc)
        return -ENOMEM;

    g_ckpt_proc->kind         = KDS_PROC_SYSTEM;
    g_ckpt_proc->name         = "kds_wal_checkpointer";
    g_ckpt_proc->static_prio  = -1;
    g_ckpt_proc->dynamic_prio = KDS_PROC_PRIORITY_SYSTEM_BACKGROUND;
    g_ckpt_proc->run          = kds_wal_checkpointer_proc;
    g_ckpt_proc->state        = KDS_PROC_STATE_READY;

    ret = kds_proc_register(g_ckpt_proc);
    if (ret) {
        pr_err("kds_wal: failed to register checkpointer proc: %d\n", ret);
        vfree(g_ckpt_proc);
        g_ckpt_proc = NULL;
        return ret;
    }

    pr_info("kds_wal: checkpointer proc registered\n");
    return 0;
}

void kds_wal_checkpointer_shutdown(void)
{
    if (g_ckpt_proc) {
        kds_proc_unregister(g_ckpt_proc);
        vfree(g_ckpt_proc);
        g_ckpt_proc = NULL;
    }
}

/* ------------------------------------------------------------------
 * kds_wal_shutdown
 * ------------------------------------------------------------------ */

void kds_wal_shutdown(void)
{
    if (!g_wal)
        return;

    /* Final flush before teardown */
    kds_wal_flush();

    vfree(g_wal->buf);
    kfree(g_wal);
    g_wal = NULL;

    pr_info("kds_wal: shutdown complete\n");
}