#ifndef __KDS_WAL_H
#define __KDS_WAL_H

#include <linux/kds.h>
#include <linux/kds_page.h>
#include <linux/kds_page_mgr.h>
#include <linux/kds_proc.h>
#include <linux/spinlock.h>
#include <linux/completion.h>
#include <linux/atomic.h>

/*
 * KDS Write-Ahead Log
 *
 * WAL records live on ordinary KDS pages (KDS_PAGE_TYPE_WAL) in the
 * same block device as data pages. There is no separate file or
 * device.
 *
 * Write path (caller, e.g. executor):
 *   1. kds_wal_append()  -- write record to WAL ring buffer (memory)
 *   2. Modify the data frame (page_mgr, mark dirty)
 *   3. kds_wal_commit()  -- append COMMIT record to ring buffer only;
 *                           no flush, no fsync.
 *
 * Flush path (kds_wal_checkpointer_proc, background kds_proc_t):
 *   1. Drain WAL ring buffer → WAL pages (KDS_PAGE_TYPE_WAL) on disk.
 *      Advances flush_lsn.
 *   2. Flush dirty data frames to their data pages on disk.
 *   3. Write a CHECKPOINT record so recovery knows how far to replay.
 *      Advances checkpointed_lsn.
 *
 * Recovery (kds_wal_recover, called at boot before catalog bootstrap):
 *   Replay all WAL records from checkpointed_lsn to flush_lsn.
 *
 * LSN encoding:
 *   u64  lsn = ((u64)wal_page_id << 32) | byte_offset_within_payload
 *
 *   wal_page_id   -- KDS page_id of the WAL page holding the record
 *   byte_offset   -- offset from the start of the WAL page's payload
 *                    area (i.e. after kds_page_hdr_t + kds_wal_page_hdr_t)
 *
 *   LSN 0 is the sentinel "no LSN / before any record".
 *
 * WAL page chain:
 *   WAL pages are linked via kds_wal_page_hdr_t.next_wal_page_id so
 *   recovery can walk forward without scanning all page_ids.
 *   The WAL head page_id is persisted in the superblock
 *   (kds_superblock_t.wal_head_page_id / wal_tail_page_id).
 */

/* ------------------------------------------------------------------
 * LSN
 * ------------------------------------------------------------------ */

typedef u64 kds_lsn_t;

#define KDS_LSN_INVALID  ((kds_lsn_t)0)

static inline kds_lsn_t kds_lsn_make(kds_page_id_t page_id, u32 offset)
{
    return ((kds_lsn_t)page_id << 32) | (kds_lsn_t)offset;
}

static inline kds_page_id_t kds_lsn_page_id(kds_lsn_t lsn)
{
    return (kds_page_id_t)(lsn >> 32);
}

static inline u32 kds_lsn_offset(kds_lsn_t lsn)
{
    return (u32)(lsn & 0xFFFFFFFFULL);
}

/* ------------------------------------------------------------------
 * WAL record types
 * ------------------------------------------------------------------ */

typedef enum kds_wal_rec_type {
    KDS_WAL_REC_INSERT      = 1,  /* tuple inserted into a heap/btree page  */
    KDS_WAL_REC_DELETE      = 2,  /* tuple/key deleted                       */
    KDS_WAL_REC_INIT_PAGE   = 3,  /* new page initialised (heap_init_page)   */
    KDS_WAL_REC_COMMIT      = 4,  /* transaction commit                      */
    KDS_WAL_REC_ABORT       = 5,  /* transaction abort                       */
    KDS_WAL_REC_CHECKPOINT  = 6,  /* checkpoint: all pages up to lsn on disk */
    KDS_WAL_REC_FULL_PAGE   = 7,  /* full page write for crash safety        */
    KDS_WAL_REC_BEGIN       = 8,  /* transaction begin (see kds_transaction) */
} kds_wal_rec_type_t;

/* ------------------------------------------------------------------
 * On-disk WAL page header
 *
 * Placed immediately after kds_page_hdr_t inside every
 * KDS_PAGE_TYPE_WAL page.
 * ------------------------------------------------------------------ */

typedef struct kds_wal_page_hdr {
    u64            seg_no;           /* monotonically increasing page sequence  */
    kds_lsn_t      start_lsn;        /* LSN of the first record on this page    */
    u32            used_bytes;        /* bytes written into the payload area     */
    kds_page_id_t  next_wal_page_id; /* 0 = this is the current tail page       */
    u32            _pad;
} __attribute__((packed)) kds_wal_page_hdr_t;

#define KDS_WAL_PAGE_HDR_SIZE   sizeof(kds_wal_page_hdr_t)

/*
 * Usable payload bytes per WAL page:
 *   KDS_PAGE_SIZE - sizeof(kds_page_hdr_t) - sizeof(kds_wal_page_hdr_t)
 *
 * Defined as a macro rather than a constant so it reacts to changes
 * in kds_page_hdr_t / kds_wal_page_hdr_t sizes automatically.
 */
#define KDS_WAL_PAGE_PAYLOAD  \
    (KDS_PAGE_SIZE - KDS_PAGE_HDR_SIZE - KDS_WAL_PAGE_HDR_SIZE)

/* ------------------------------------------------------------------
 * On-disk WAL record header  (fixed 28 bytes)
 * ------------------------------------------------------------------ */

typedef struct kds_wal_rec_hdr {
    kds_lsn_t          lsn;       /* this record's LSN                        */
    kds_lsn_t          prev_lsn;  /* previous record LSN (for undo chain)     */
    u64                xid;       /* transaction id                           */
    u8                 type;      /* kds_wal_rec_type_t                       */
    u8                 _pad[3];
    u32                body_len;  /* bytes of body immediately following hdr  */
    u32                crc;       /* crc32c over (hdr fields except crc) + body */
} __attribute__((packed)) kds_wal_rec_hdr_t;

#define KDS_WAL_REC_HDR_SIZE  sizeof(kds_wal_rec_hdr_t)

/* Maximum body size: a FULL_PAGE record holds an entire data page. */
#define KDS_WAL_MAX_BODY_SIZE  KDS_PAGE_SIZE

/* Maximum total record size (header + body). */
#define KDS_WAL_MAX_REC_SIZE   (KDS_WAL_REC_HDR_SIZE + KDS_WAL_MAX_BODY_SIZE)

/* ------------------------------------------------------------------
 * Record body layouts (written immediately after kds_wal_rec_hdr_t)
 * ------------------------------------------------------------------ */

/* KDS_WAL_REC_INSERT / KDS_WAL_REC_DELETE */
typedef struct kds_wal_body_page_mod {
    kds_page_id_t  page_id;
    u32            offset;     /* byte offset within page payload */
    u32            length;     /* bytes modified                  */
    /* `length` bytes of new data follow this struct */
} __attribute__((packed)) kds_wal_body_page_mod_t;

/* KDS_WAL_REC_INIT_PAGE */
typedef struct kds_wal_body_init_page {
    kds_page_id_t  page_id;
    u8             page_type;  /* kds_page_type_t of the new page */
    u8             _pad[3];
} __attribute__((packed)) kds_wal_body_init_page_t;

/* KDS_WAL_REC_COMMIT / KDS_WAL_REC_ABORT */
typedef struct kds_wal_body_xact {
    u64  xid;
} __attribute__((packed)) kds_wal_body_xact_t;

/* KDS_WAL_REC_CHECKPOINT */
typedef struct kds_wal_body_checkpoint {
    kds_lsn_t  checkpointed_lsn; /* all data pages <= this LSN are on disk */
    u64        checkpoint_time;  /* ktime_get_real_seconds() */
} __attribute__((packed)) kds_wal_body_checkpoint_t;

/* KDS_WAL_REC_FULL_PAGE */
typedef struct kds_wal_body_full_page {
    kds_page_id_t  page_id;
    u8             _pad[4];
    /* KDS_PAGE_SIZE bytes of raw page data follow */
} __attribute__((packed)) kds_wal_body_full_page_t;

/* ------------------------------------------------------------------
 * In-memory WAL buffer
 *
 * Ring buffer of raw bytes. Writers append records under wal_lock.
 * The checkpointer proc drains from flush_lsn to write_lsn.
 *
 * Size must be a power of two so index masking works.
 * Sized to hold at least a few full WAL pages worth of records while
 * the checkpointer is catching up -- 4MB is conservative.
 * ------------------------------------------------------------------ */

#define KDS_WAL_BUF_SIZE        (4 * 1024 * 1024)  /* 4 MiB, power of two */
#define KDS_WAL_BUF_MASK        (KDS_WAL_BUF_SIZE - 1)

/*
 * Threshold: when buf_used() > KDS_WAL_BUF_FLUSH_THRES, wake the
 * checkpointer immediately rather than waiting for the next schedule.
 */
#define KDS_WAL_BUF_FLUSH_THRES (KDS_WAL_BUF_SIZE * 3 / 4)

typedef struct kds_wal_buf {
    u8             buf[KDS_WAL_BUF_SIZE]; /* raw record bytes, circular     */

    /*
     * write_off: next byte to write (mod KDS_WAL_BUF_SIZE).
     * flush_off: next byte to flush to WAL pages.
     * Both are monotonically increasing absolute offsets; the actual
     * ring index is off & KDS_WAL_BUF_MASK.
     * write_off - flush_off == bytes currently buffered.
     */
    u64            write_off;   /* protected by lock */
    u64            flush_off;   /* updated by checkpointer only         */

    spinlock_t     lock;

    /*
     * LSN of the last byte written. Maintained alongside write_off so
     * callers get back the LSN of their record without a separate
     * page_id lookup.
     */
    kds_lsn_t      current_lsn; /* LSN assigned to the most recent append */

    /*
     * Current WAL tail page: the KDS page where the checkpointer
     * will next write buffered records. Allocated by kds_wal_init()
     * and grown as needed by the checkpointer.
     */
    kds_page_id_t  tail_page_id;
    u32            tail_used;   /* bytes already written into tail page   */
    u64            tail_seg_no; /* seg_no of the current tail page        */
} kds_wal_buf_t;

/* ------------------------------------------------------------------
 * In-memory WAL state
 * ------------------------------------------------------------------ */

typedef struct kds_wal_state {
    kds_wal_buf_t  *buf;

    /* LSN of the last record flushed to a WAL page on disk. */
    atomic64_t      flush_lsn;

    /* LSN up to which data pages have been flushed (checkpoint). */
    atomic64_t      checkpointed_lsn;

    /* previous record's LSN, for building prev_lsn chains. */
    kds_lsn_t       prev_lsn;   /* protected by buf->lock */
} kds_wal_state_t;

/* ------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------ */

/*
 * kds_wal_init(): allocate the WAL buffer and the first WAL page.
 * Must be called after the page allocator and buffer pool are ready,
 * but before any executor or catalog operation that writes data.
 */
int  kds_wal_init(void);

/*
 * kds_wal_shutdown(): flush any buffered WAL records to disk and
 * release all WAL system resources. Called during module unload after
 * all worker procs have been stopped.
 */
void kds_wal_shutdown(void);

/*
 * kds_wal_append(): write one WAL record into the in-memory ring
 * buffer. Fills rec_hdr->lsn and rec_hdr->prev_lsn automatically.
 * The caller must have set xid, type, body_len, and crc to 0 (crc
 * is computed here). body may be NULL when body_len == 0.
 *
 * Returns 0 on success, -ENOSPC if the buffer is full (checkpointer
 * has not drained fast enough), or another negative errno on error.
 *
 * On success, *out_lsn holds the LSN assigned to this record.
 */
int  kds_wal_append(kds_wal_rec_hdr_t *rec_hdr, const void *body,
                    kds_lsn_t *out_lsn);

/*
 * kds_wal_begin(): append a KDS_WAL_REC_BEGIN record marking the start
 * of transaction xid. Ring-buffer only (no flush) -- a BEGIN record is
 * not durability-critical the way COMMIT is; recovery treats it as
 * metadata. On success *out_lsn (may be NULL) holds the record's LSN.
 * Used by kds_txn_begin() (kds_transaction.h).
 */
int  kds_wal_begin(u64 xid, kds_lsn_t *out_lsn);

/*
 * kds_wal_commit(): append a KDS_WAL_REC_COMMIT record for xid into
 * the ring buffer. Does NOT flush to disk on its own -- callers that
 * need commit durability (kds_txn_commit()) follow it with
 * kds_wal_sync(); the checkpointer would otherwise flush on its next
 * cycle.
 */
int  kds_wal_commit(u64 xid);

/*
 * kds_wal_abort(): append a KDS_WAL_REC_ABORT record.
 */
int  kds_wal_abort(u64 xid);

/*
 * kds_wal_get_current_lsn(): return the LSN of the most recently
 * appended record.
 */
kds_lsn_t kds_wal_get_current_lsn(void);

/*
 * kds_wal_get_flush_lsn(): return the LSN up to which WAL records
 * have been written to WAL pages on disk.
 */
kds_lsn_t kds_wal_get_flush_lsn(void);

/*
 * kds_wal_get_checkpointed_lsn(): return the LSN up to which all
 * dirty data pages have been flushed (i.e. WAL before this point is
 * no longer needed for recovery).
 */
kds_lsn_t kds_wal_get_checkpointed_lsn(void);

/*
 * kds_wal_flush(): drain the ring buffer to WAL pages on disk, up to
 * the current write_lsn. Called by the checkpointer proc. Serialized
 * internally so it is safe to call concurrently with kds_wal_sync().
 * Returns 0 on success, -ENODEV if the WAL is not initialised.
 */
int  kds_wal_flush(void);

/*
 * kds_wal_sync(): synchronously drain the WAL ring buffer to WAL pages
 * on disk right now, in the caller's context. Used by the insert and
 * update paths to make a data modification's WAL record durable before
 * the operation reports success, instead of waiting for the background
 * checkpointer's next cycle. Shares kds_wal_flush()'s serialization, so
 * a synchronous flush and the checkpointer never race on the WAL tail
 * page. Best-effort: returns -ENODEV (not a crash) if the WAL is not
 * initialised, so callers on the "continue without WAL" path can invoke
 * it unconditionally. Returns 0 once the records are on disk.
 */
int  kds_wal_sync(void);

/*
 * kds_wal_checkpoint(): after flushing dirty data pages up to
 * data_flush_lsn, write a CHECKPOINT record and update
 * checkpointed_lsn. Called by the checkpointer proc after
 * kds_frame_flush() has been run on all dirty frames.
 */
int  kds_wal_checkpoint(kds_lsn_t data_flush_lsn);

/*
 * kds_wal_recover(): replay WAL records from checkpointed_lsn to
 * flush_lsn at boot time. Reads from WAL pages on disk.
 * Must be called before kds_catalog_bootstrap() on a non-first boot.
 */
int  kds_wal_recover(void);

/* ------------------------------------------------------------------
 * Checkpointer proc (registered as a kds_proc_t)
 * ------------------------------------------------------------------ */

/*
 * kds_wal_checkpointer_proc(): the background proc run function.
 * On each scheduler slice:
 *   1. kds_wal_flush()       -- WAL buffer → WAL pages
 *   2. flush dirty frames    -- data pages to disk
 *   3. kds_wal_checkpoint()  -- record checkpoint LSN
 */
struct kds_proc;
kds_proc_result_t kds_wal_checkpointer_proc(struct kds_proc *proc,
                                             u64 slice_ns);

int  kds_wal_checkpointer_init(void);
void kds_wal_checkpointer_shutdown(void);

#endif /* __KDS_WAL_H */