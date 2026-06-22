/* kds_proc.h */
#ifndef _KDS_PROC_H
#define _KDS_PROC_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/rbtree.h>
#include <linux/cpumask.h>
#include <linux/percpu.h>

#define KDS_PRIO_MIN   0
#define KDS_PRIO_MAX   1000

typedef u64 kds_proc_id_t;

typedef enum {
    KDS_PROC_PRIORITY_IDLE = 0,
    KDS_PROC_PRIORITY_SYSTEM_BACKGROUND = 30,
    KDS_PROC_PRIORITY_PAGE_OPTIMIZER = 20,
    KDS_PROC_PRIORITY_GC = 50,
    KDS_PROC_PRIORITY_META_CHECKPOINT = 100,
    KDS_PROC_PRIORITY_TX_RUNNING = 800,
} kds_proc_priority_t;

typedef enum {
    KDS_PROC_SYSTEM = 0,
    KDS_PROC_SESSION = 1,
} kds_proc_kind_t;

typedef enum {
    KDS_PROC_STATE_READY = 0,
    KDS_PROC_STATE_RUN = 1,
    KDS_PROC_STATE_WAIT = 2,
    KDS_PROC_STATE_DONE = 3,
} kds_proc_state_t;

typedef enum {
    KDS_PROC_DONE_RET  = 0,
    KDS_PROC_YIELD_RET = 1,
    KDS_PROC_WAIT_RET  = 2,
    KDS_PROC_CONTINUE  = 3,
    KDS_PROC_ERR_RET   = -1,
} kds_proc_result_t;

struct kds_proc;

typedef kds_proc_result_t (*kds_proc_run_fn)(
    struct kds_proc *proc,
    u64 slice_ns
);

typedef struct kds_proc {
    kds_proc_id_t             pid;
    const char               *name;
    kds_proc_kind_t           kind;
    kds_proc_state_t          state;
    int                       static_prio;
    int                       dynamic_prio;
    u64                       last_run_ns;
    u64                       total_runtime_ns;
    u64                       wait_time_ns;
    
    /* CMP 관련 필드 */
    int                       cpu;            /* 현재 할당된 CPU */
    int                       preferred_cpu;  /* 선호 CPU (캐시 친화성) */
    cpumask_t                 allowed_cpus;   /* 실행 가능한 CPU 마스크 */
    atomic_t                  running;        /* 실행 중 플래그 */
    
    void                     *ctx;
    kds_proc_run_fn           run;
    
    struct list_head          list_node;
    struct rb_node            rb_node;
} kds_proc_t;

/* Per-CPU 런큐 구조체 */
typedef struct kds_runqueue {
    spinlock_t                lock;
    struct list_head          system_procs;
    struct rb_root            session_procs;
    kds_proc_t               *curr_proc;        /* 현재 실행 중인 프로세스 */
    u64                       total_runtime_ns;
    unsigned int              nr_running;     /* 실행 대기 중인 프로세스 수 */
} kds_runqueue_t;

/* 글로벌 스케줄러 통계 */
typedef struct kds_sched_stats {
    atomic64_t                total_schedules;
    atomic64_t                load_balances;
    atomic64_t                migrations;
} kds_sched_stats_t;

/* scheduler API */
int  kds_proc_register(kds_proc_t *proc);
void kds_proc_unregister(kds_proc_t *proc);
void kds_proc_schedule(int cpu, u64 slice_ns);
void kds_proc_schedule_all(u64 slice_ns);
int  kds_proc_set_affinity(kds_proc_t *proc, const struct cpumask *mask);
int  kds_sched_init(void);
void kds_sched_cleanup(void);

/* 로드 밸런싱 */
void kds_load_balance(void);

#endif