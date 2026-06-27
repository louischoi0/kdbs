#ifndef _KDS_DSHELL_H
#define _KDS_DSHELL_H

#include <linux/types.h>

#define DSHELL_LINE_MAX     256   /* max bytes accepted per write() */
#define DSHELL_RESP_MAX     512   /* max bytes a command can write into its response */
#define DSHELL_MAX_ARGS     16

/*
 * Every command handler gets argv/argc from the parsed command line
 * and an output buffer to format its response into (success message
 * or error detail -- the handler decides which, but it must always
 * produce *some* response, even just "OK\n" or an error string, so
 * the client reading back from /dev/kds always gets something).
 *
 * Returns 0 on success, a negative errno on failure. The return
 * value only controls what gets logged kernel-side
 * (__kds_dispatch_dshell_cmd()); what the client actually sees is
 * entirely the `out` buffer's contents, written via the handler
 * itself (typically with scnprintf()).
 */
typedef int (*kds_dshell_fn_t)(char **argv, int argc, char *out, size_t out_size);

typedef struct kds_dshell_cmd {
    const char          *op;
    kds_dshell_fn_t      fn;
    int                  min_args;     /* including argv[0] (the op itself) */
} kds_dshell_cmd_t;

/*
 * Adding a new command:
 *   1. Write a handler matching kds_dshell_fn_t's signature.
 *   2. Add one line to the kds_dshell_cmds[] table in dshell.c.
 * No other wiring is needed -- dispatch, argument count validation,
 * and response delivery are all handled generically by
 * __kds_dispatch_dshell_cmd() and the character device's
 * read()/write() handlers.
 */

int kds_init_dshell_system(void);
void kds_shutdown_dshell_system(void);

int kds_split_cmd(char *buf, char *argv[], int max_args);

#endif