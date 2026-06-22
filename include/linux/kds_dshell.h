#ifndef _KDS_DSHELL_H
#define _KDS_DSHELL_H

#define DSHELL_LINE_MAX 256
#define DSHELL_CMD_QUEUE_START_PAGE 8
#define DSHELL_BUFFER_LEN 64

int kds_init_dshell_system(void);
int kds_split_cmd(char *buf, char *argv[], int max_args);
int kds_cmd_get(char **argv);

#endif