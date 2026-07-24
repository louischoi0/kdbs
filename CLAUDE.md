# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

KDS is a small relational database engine implemented as Linux kernel code
(C, not a userspace program). It
is not a loadable module — `kernel/kds/main.c` hooks in via `late_initcall()`
and runs as part of the kernel image itself. There is no in-repo
Makefile/Kconfig: `kernel/kds/` and `include/linux/kds*.h` are an overlay
meant to be dropped into a full Linux kernel source tree that already has
the necessary `obj-y`/`Kconfig` wiring (that wiring lives outside this repo,
inside the `riscv-kernel-build` docker container referenced by the build
scripts).

Clients talk to KDS from the guest via the character device `/dev/kds`
(`kernel/kds/dshell.c`), writing a line of the SQL subset described below
and reading back a text response. The `kds_init.c` guest init program that
would normally drive this is not part of this repo.

## Concept & goals

KDBS (KDB) is a **kernel-integrated database system**: the database engine
lives inside the kernel rather than running as a userspace process on top of
it. The point of that integration is control over the machine's resources.
Instead of delegating to the kernel's general-purpose subsystems and paying
the userspace/kernel boundary on every operation, KDBS **handles its own
scheduling and I/O at the kernel level** — this is why it ships its own
cooperative scheduler (`kernel/kds/proc.c`, see the concurrency section
below) and its own block/buffer/page-management stack rather than leaning on
the standard process scheduler and page cache.

The goals this integration serves are twofold:

- **Query execution speed** — keeping scheduling and I/O decisions inside
  the engine removes syscall/context-switch overhead and lets the database
  time-slice and order work with knowledge a generic OS scheduler doesn't
  have (e.g. resumable, nanosecond-budgeted query execs).
- **Power efficiency through resource optimization** — deliberately managing
  how CPU, network, and disk are used (when to run work, when to batch I/O,
  when to stay idle) to minimize wasted computation, which matters most on
  constrained, always-on hardware.

Because of that efficiency focus, KDBS targets running as a **lightweight
database on resource-constrained edge nodes** rather than as a large server
DBMS — e.g. an IoT hub/main node, a factory control tower, or similar
embedded coordination points. This is the lens for design decisions in this
repo: favor a small, tightly-managed, efficient core over broad feature
coverage (see the feature-scope notes throughout).

## Build / run

All of these assume a `riscv-kernel-build` docker container exists with a
Linux kernel source tree (this repo's `kernel/kds/` + `include/linux/kds*.h`
overlaid into it) and a `rootfs/` + `kds_init.c` for the guest.

- `sh build.sh` — cross-compiles `arch/riscv/boot/Image` inside the
  container (`make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- Image -j12`).
- `sh initbuild.sh` — cross-compiles `kds_init.c` into `rootfs/sbin/init`
  and repacks `rootfs/` into `rootfs.img` (the QEMU initramfs).
- `sh resetdata.sh` — zeroes out a fresh 128MB `kdb.img`, the KDS block
  device backing file. Run this to wipe all on-disk KDS state.
- `sh run.sh` — boots `arch/riscv/boot/Image` under `qemu-system-riscv64`
  (4 vCPUs, `kdb.img` as a virtio block device, virtio-net with
  `hostfwd tcp:127.0.0.1:15432 -> guest:10.0.2.15:15432` — nothing in this
  repo currently binds that port; it's not a client protocol today).
- `sh buildrun.sh` — `resetdata.sh && build.sh && run.sh` (full fresh
  rebuild + boot).
- `sh stop.sh` — kills **all** `qemu-system` processes on the host
  (`ps -ef | grep qemu`), not just this one. Be careful if other QEMU
  instances are running.

There are no unit tests in this repo; correctness is currently exercised by
booting the kernel and driving it through `/dev/kds`.

## Boot / initialization order

`kds_bootstrap()` in `kernel/kds/main.c` brings subsystems up in a strict,
load-bearing order — later stages assume earlier ones are live:

1. `init_block_device()` (blkdev.c)
2. `kds_buf_pool_init()` (page_mgr.c) — must precede anything touching
   `kds_buf_lookup_or_load()`/`kds_buf_alloc_new()`
3. `kds_sched_init()` (proc.c)
4. `kds_init_meta_system()` (meta.c) — superblock
5. `kds_init_dshell_system()` (dshell.c) — `/dev/kds`
6. `kds_init_page_alloc_system()` (page_alloc.c)
7. `kds_catalog_bootstrap()` (catalog.c) — **only** if
   `kds_meta_is_fresh_init()`; it unconditionally recreates catalog pages
   4-7, so running it against an existing database would clobber real data
8. `kds_wal_init()` / `kds_wal_checkpointer_init()` (wal.c)

Bootstrap failure is tri-state (`KDS_INIT_PENDING/DONE/FAILED`), not
boolean: per-CPU worker threads and the load balancer thread both wait on
`kds_init_wait` and must see `FAILED` explicitly and shut themselves down,
rather than blocking forever — a past bug here was workers hanging when
`kds_bootstrap()` returned early without signaling anything.
`kds_cleanup()` tears down in the reverse order, and only after every
kthread that might touch a page has been stopped.

## Concurrency model: KDS has its own scheduler, not just Linux's

`kernel/kds/proc.c` (API in `kds_proc.h`) implements a small cooperative
scheduler independent of the Linux CFS. One `kds_worker/<N>` kthread is
pinned per online CPU plus one load-balancer kthread; each worker calls
`kds_proc_schedule(cpu, KDS_TIME_SLICE_NS)` in a loop. Schedulable units are
`kds_proc_t` (two kinds: `KDS_PROC_SYSTEM` on a plain list, `KDS_PROC_SESSION`
on a per-CPU rbtree runqueue). Query execution and background jobs (the WAL
checkpointer, the dshell exec runner, page preallocation) are all
`kds_proc_t` instances scheduled this way, time-sliced in nanoseconds.

Long-running work (a full-table scan, a btree-clustered insert) is written
as a **resumable state machine**, not a straight-line function, because a
single scheduler slice may not be enough to finish it. This is the
`kds_exec_state_t` pattern in `kds_executor.h` (a deliberate C port of a
Python POC's state-pattern executor):

- Every concrete exec (`kds_heap_insert_exec_t`, `kds_btree_insert_exec_t`,
  and any future one) embeds `kds_exec_state_t base` as its first member and
  is invoked through `kds_exec_run(state, slice_ns)`, which sets an absolute
  `deadline_ns` and dispatches to the concrete `run()`.
- `run()` returns `KDS_EXEC_DONE`, `KDS_EXEC_ERROR`, or `KDS_EXEC_CONTINUE`.
  On `CONTINUE`, **all progress must already be checkpointed into the exec
  struct itself** (phase enum, cursor page ids, decoded state) — never on
  the C stack or in caller-locals — since the next call is a fresh
  invocation that only has the struct to resume from.
- Concrete `run()` implementations check `kds_exec_slice_expired(state)`
  at a fine granularity (per page/row, not per whole table) and only after
  doing at least one unit of work, to avoid spinning without progress.
- `kernel/kds/dshell.c` is the caller for client-driven execs: `write()` on
  `/dev/kds` sets up an exec in the client's buffer for heavy commands
  (insert/select) and blocks on a completion; a registered `kds_proc_t`
  (`kds_dshell_proc`) drives `kds_exec_run()` for all pending clients on
  every scheduler tick, so heavy queries are genuinely time-sliced across
  the whole system rather than hogging one write() call.

## Storage layering (bottom to top)

- **blkdev.c** — raw sector I/O against the single `kds_bdev` block device
  (backed by `kdb.img` via virtio). `kds.h` also exposes logical-page
  (`KDS_PAGE_SIZE` = 8KiB) read/write helpers that internally split into
  native-page-sized extents.
- **page.c / page_mgr.c** — the buffer pool. Deliberate ownership split:
  `kds_page_t` (`kds.h`) is identity + on-disk header + the *content lock*;
  `kds_frame_t` (`kds_page_mgr.h`) owns the actual `struct page *` buffer
  memory. Always reach page bytes through the owning frame's
  `kds_frame_get_write_ptr()`/`get_read_ptr()`, never through `kds_page_t`
  directly. Fixed `KDS_BUF_NR_FRAMES` (4096) frames, **no eviction yet** —
  `kds_buf_lookup_or_load()` returns `-ENOSPC` once full. The
  `page_id -> frame_id` map is an `rhashtable`; inserts/loads are
  serialized per-partition (`KDS_BUF_PARTITIONS` = 16) so racing loads of
  the same page_id don't double-fetch. Lock ordering is documented and
  load-bearing: `frame_lock` (buffer bookkeeping) is never held across a
  `kp->lock` (content) acquisition.
- **page_alloc.c** — mints fresh logical page ids on top of page_mgr.
  Range-based allocation: hands out ids one at a time from a standing
  `[alloc_point, alloc_point + remaining)` range, refilling
  `PRE_ALLOC_NUM` (256) more once `remaining < PRE_ALLOC_PAGE_THRES` (32);
  leftover ids from the old range go on a reuse freelist rather than being
  discarded. A background proc keeps a pre-allocation ring
  (`kds_get_reserved_kpage()`) topped up so allocation-heavy paths (e.g.
  growing a heap chain) don't stall on synchronous id minting.
- **meta.c** — the superblock: one fixed logical page holding
  `magic`/`version`, page-id counters, WAL head/tail, etc. A
  `static_assert` guards the struct staying exactly `KDS_PAGE_SIZE`.
- **catalog.c** — SQL catalog (`sys.objects`/`sys.tables`/`sys.columns`/
  `sys.types`), a C port of a Python POC's `catalog.py`. Catalog pages live
  at **fixed, well-known page_ids** (4-7), registered directly via
  `kds_buf_alloc_new()` rather than through the general allocator, since
  they must land at an exact id. Well-known OIDs are `#define`d in
  `kds_catalog.h`.
- **heap.c** — slotted heap pages (PostgreSQL-style): a growing-downward
  slot directory and growing-upward tuple data area, tracked by
  `meta.lower`/`meta.upper`. A table's storage is a singly-linked chain of
  heap pages (`next_page_id` in the last 8 bytes of each page); only full
  chain scans are possible without an index.
- **btree.c** — the alternative clustering strategy (`KDS_CLUSTERED_BTREE`,
  chosen at `CREATE TABLE`). `BTREE_MAX_KEYS` = 4, `BTREE_MAX_DEPTH` = 16.
  **Known gap**: `kds_relation.h` documents that a root split via
  `btree_propagate_split()` does not update the relation's persisted
  `root_page_id`, so repeated inserts that split the root can eventually
  make an index unreachable from its catalog entry. `kds_index_search()`'s
  slot convention is also flagged unverified against what
  `btree_node_insert_at()` actually writes — treat both as needing an
  insert-then-search round-trip test before relying on them.
- **relation.c** — `kds_relation_t`: a runtime handle unifying open tables
  and open indexes (à la PostgreSQL's relation abstraction). Index
  relations have `schema.nr_cols == 0`.
- **undo.c** — undo pages are physically ordinary heap pages (different
  header `type` only) storing `kds_undo_entry_t`. Updates are either
  in-place overwrites (if the new value fits) or retire-old-slot +
  insert-new; either way the prior version is copied to an undo entry
  first and the new tuple's `undo_ptr` chains back to it. This design
  deliberately has no PostgreSQL-style dead-tuple/VACUUM problem, but also
  does not reclaim retired slots' byte space (no compaction).
- **wal.c** — write-ahead log. Records live on ordinary `KDS_PAGE_TYPE_WAL`
  pages, not a separate file/device. LSN = `(page_id << 32) | byte_offset`.
  Write path: `kds_wal_append()` (in-memory ring) → modify data frame,
  mark dirty → `kds_wal_commit()` (append COMMIT record only, no fsync).
  A background checkpointer proc drains the ring to WAL pages, flushes
  dirty frames, then writes a CHECKPOINT record. Recovery replays
  checkpointed_lsn → flush_lsn at boot, before catalog bootstrap.
- **exec_heap_insert.c / exec_btree_insert.c** — the resumable executors
  described above. Both follow strict **WAL-before-data** phase ordering
  (append WAL record(s) for a modification before making the modification)
  so crash recovery can always re-apply from the log.
- **types.c** — centralized scalar type registry (int8/16/32/64, float,
  decimal, bool, varchar, char) so width/parse/format logic lives in one
  place instead of being duplicated across catalog.c/dshell.c/executors.
  Notable kernel-specific constraints: `KDS_TYPE_FLOAT` never does IEEE 754
  math in-kernel (FPU state isn't safely usable without
  `kernel_fpu_begin/end()`) — it stores/exchanges the raw 4-byte bit
  pattern as an 8-hex-char string, and interpreting it as a real float is
  the client's job. `KDS_TYPE_DECIMAL` is a fixed-point `s64` with a
  hardcoded scale (`KDS_DECIMAL_SCALE` = 4 digits), no arbitrary precision.
- **parser.c** — hand-rolled lexer/parser for the supported SQL subset:
  `CREATE TABLE ... (cols) [HEAP|BTREE]`, `INSERT INTO ... VALUES (...)`,
  `SELECT * FROM ... [WHERE <cond> [AND <cond>]*]`, `SHOW META`,
  `SHOW ALLOC`. Deliberately no JOINs/subqueries/GROUP BY/ORDER BY/
  aggregates/OR/NOT, no projection, no quote escaping in string literals.
  The full grammar and token/AST types are documented at the top of
  `kds_parser.h`. Every table's **first column is always its primary key
  and always `KDS_TYPE_INT64`** — a fixed positional convention enforced
  at `CREATE TABLE` time, relied on by the executors to read a row's PK
  straight out of the first 8 bytes of its encoded payload without
  consulting the schema.
- **dshell.c** — the `/dev/kds` character device and command dispatch.
  Fast, I/O-free commands (`SHOW META`, `CREATE TABLE`, ...) run inline
  inside `write()`; heavy commands (insert, select) go through the
  `kds_exec_state_t` machinery described above. Each `open()` gets an
  independent `kds_dshell_client_t` — clients are not re-entrant (a client
  must not call `write()` again while already blocked in a previous one).

## Conventions worth knowing before editing

- New page types get an entry in `KDS_PAGE_TYPE_*` (`kds.h`); new dshell
  commands are added by writing a handler matching `kds_dshell_fn_t` and
  wiring it into the statement-type dispatch in `dshell.c` — no other
  plumbing needed.
- Comments throughout the codebase frequently reference a prior **Python
  POC** (`catalog.py`, `executor.py`) that this C kernel code was ported
  from, and call out where the port deliberately diverges and why. When a
  comment says "this mirrors Python's X" or "unlike the Python version",
  that's explaining an intentional kernel/C constraint, not something to
  "fix" back into parity.
- Several comments document **known-unverified or known-incomplete**
  behavior explicitly (the btree root-split/`root_page_id` staleness gap,
  the `kds_index_search()` slot-offset assumption, no page compaction, no
  buffer-pool eviction). Check for one of these notes before assuming a
  code path is fully correct — they're flagged rather than fixed because
  fixing them is out of scope of whatever change introduced the note.
