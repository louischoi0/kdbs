#ifndef __KDS_EXEC_SELECT_H
#define __KDS_EXEC_SELECT_H

#include <linux/kds_executor.h>
#include <linux/kds_page_mgr.h>

/*
 * Row-level primitives shared between the two SelectExec backends,
 * exec_heap_select.c and exec_btree_select.c. The two clustering
 * strategies live in separate translation units on purpose (heap
 * follows a next_page_id chain; btree descends to the leftmost leaf
 * and walks the leaf sibling chain of heap "bucket" pages) -- they
 * share only these per-page primitives, they are never merged into
 * one scan.
 *
 * The definitions live in exec_heap_select.c, which is the common home
 * for select-shared code (both backends operate on the single
 * kds_select_exec_t declared in kds_executor.h).
 */

/*
 * Scans one already-loaded heap page's slots starting at *slot_io,
 * evaluating the exec's resolved WHERE conditions and streaming each
 * matched row into exec->out_buf. Advances *slot_io as it goes.
 *
 * Returns:
 *   KDS_EXEC_DONE      the page's slots are exhausted, OR out_buf
 *                      filled up -- in the latter case exec->truncated
 *                      is set and the caller must stop scanning.
 *   KDS_EXEC_CONTINUE  the slice budget was spent mid-page; *slot_io
 *                      points at the next slot to resume from.
 *   KDS_EXEC_ERROR     exec->base.ret holds the errno.
 *
 * Frame pin/unpin and pages_visited accounting stay with the caller,
 * since only the caller knows the page's role (heap chain page vs.
 * btree bucket page) and how it reached it.
 */
kds_exec_result_t kds_select_scan_page(kds_select_exec_t *exec,
                                       kds_frame_t *frame, u16 *slot_io);

/*
 * Appends the closing "N row(s) matched across M page(s)" summary to
 * exec->out_buf, or the "...(truncated)" marker when exec->truncated.
 * Called once, after the scan phase reports DONE.
 */
void kds_select_finish(kds_select_exec_t *exec);

#endif /* __KDS_EXEC_SELECT_H */
