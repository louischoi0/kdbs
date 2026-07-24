#ifndef __KDS_EXEC_UPDATE_H
#define __KDS_EXEC_UPDATE_H

#include <linux/kds_executor.h>
#include <linux/kds_page_mgr.h>

/*
 * Per-page primitives shared between the two UpdateExec backends,
 * exec_heap_update.c and exec_btree_update.c -- mirrors kds_exec_select.h.
 * Heap and btree keep their own scan (heap chain vs. btree leaf/bucket
 * walk) in separate translation units; they share only these helpers.
 * Definitions live in exec_heap_update.c.
 */

/*
 * Applies the exec's SET assignments to every WHERE-matching live tuple
 * in an already-loaded heap page, from *slot_io up to (but not
 * including) slot_limit. slot_limit is the page's slot count captured
 * when the page was first entered, so a tuple relocated to a higher
 * slot by the retire+insert path is not re-updated in the same pass.
 * Advances *slot_io. Returns:
 *   KDS_EXEC_DONE      slots [*slot_io, slot_limit) exhausted.
 *   KDS_EXEC_CONTINUE  slice budget spent; *slot_io is the next slot.
 *   KDS_EXEC_ERROR     exec->base.ret set (read/build/update failure).
 * Frame pin/unpin, the slot_limit snapshot, and pages_visited are the
 * caller's responsibility (only it knows the page's role/origin).
 */
kds_exec_result_t kds_update_apply_page(kds_update_exec_t *exec,
                                        kds_frame_t *frame,
                                        u16 *slot_io, u16 slot_limit);

/* Writes the closing "OK N row(s) updated ..." summary into out_buf. */
void kds_update_finish(kds_update_exec_t *exec);

#endif /* __KDS_EXEC_UPDATE_H */
