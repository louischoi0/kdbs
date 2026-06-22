/* kds_proc.c */
#include <linux/kds_proc.h>
#include <linux/ktime.h>
#include <linux/spinlock.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/smp.h>
#include <linux/percpu.h>
#include <linux/cpumask.h>

static DEFINE_PER_CPU(kds_runqueue_t, kds_runqueues);
static atomic64_t kds_next_pid = ATOMIC64_INIT(0);
static kds_sched_stats_t kds_stats;

#define KDS_LOAD_BALANCE_THRESHOLD  2

int kds_sched_init(void)
{
    int cpu;
    kds_runqueue_t *rq;

    for_each_possible_cpu(cpu) {
        rq = &per_cpu(kds_runqueues, cpu);
        spin_lock_init(&rq->lock);
        INIT_LIST_HEAD(&rq->system_procs);
        rq->session_procs = RB_ROOT;
        rq->curr_proc = NULL;
        rq->total_runtime_ns = 0;
        rq->nr_running = 0;
    }

    atomic64_set(&kds_stats.total_schedules, 0);
    atomic64_set(&kds_stats.load_balances, 0);
    atomic64_set(&kds_stats.migrations, 0);

    pr_info("kds_sched: initialized for %d CPUs\n", num_possible_cpus());
    return 0;
}

void kds_sched_cleanup(void)
{
    pr_info("kds_sched: cleanup\n");
}

/* ============================
 * dynamic priority calculation
 * ============================ */
static int kds_calc_dynamic_prio(kds_proc_t *p)
{
    return p->dynamic_prio;
}

/* ============================
 * rb-tree helpers (SESSION)
 * ============================ */
static void kds_session_rb_insert(struct rb_root *root, kds_proc_t *proc)
{
    struct rb_node **link = &root->rb_node;
    struct rb_node *parent = NULL;
    kds_proc_t *entry;

    while (*link) {
        parent = *link;
        entry = rb_entry(parent, kds_proc_t, rb_node);
        if (proc->static_prio < entry->static_prio)
            link = &(*link)->rb_left;
        else
            link = &(*link)->rb_right;
    }

    rb_link_node(&proc->rb_node, parent, link);
    rb_insert_color(&proc->rb_node, root);
}

static void kds_session_rb_remove(struct rb_root *root, kds_proc_t *proc)
{
    rb_erase(&proc->rb_node, root);
}

/* ============================
 * 런큐 선택 (로드 밸런싱)
 * ============================ */
static int kds_select_cpu(kds_proc_t *proc)
{
    int cpu, best_cpu = -1;
    unsigned int min_load = UINT_MAX;
    kds_runqueue_t *rq;

    /* 선호 CPU가 허용된 경우 우선 선택 */
    if (proc->preferred_cpu >= 0 && 
        cpumask_test_cpu(proc->preferred_cpu, &proc->allowed_cpus)) {
        rq = &per_cpu(kds_runqueues, proc->preferred_cpu);
        if (rq->nr_running < min_load) {
            return proc->preferred_cpu;
        }
    }

    /* 가장 부하가 적은 CPU 찾기 */
    for_each_cpu(cpu, &proc->allowed_cpus) {
        rq = &per_cpu(kds_runqueues, cpu);
        if (rq->nr_running < min_load) {
            min_load = rq->nr_running;
            best_cpu = cpu;
        }
    }

    return best_cpu >= 0 ? best_cpu : cpumask_first(&proc->allowed_cpus);
}

/* ============================
 * register / unregister
 * ============================ */
int kds_proc_register(kds_proc_t *proc)
{
    kds_runqueue_t *rq;
    int cpu;

    if (!proc || !proc->run)
        return -EINVAL;

    proc->state = KDS_PROC_STATE_READY;
    proc->last_run_ns = 0;
    proc->total_runtime_ns = 0;
    proc->wait_time_ns = 0;
    proc->pid = atomic64_fetch_add(1, &kds_next_pid);
    atomic_set(&proc->running, 0);

    if (cpumask_empty(&proc->allowed_cpus))
        cpumask_copy(&proc->allowed_cpus, cpu_online_mask);

    cpu = kds_select_cpu(proc);
    proc->cpu = cpu;
    proc->preferred_cpu = cpu;

    rq = &per_cpu(kds_runqueues, cpu);
    spin_lock(&rq->lock);

    if (proc->kind == KDS_PROC_SYSTEM)
        list_add_tail(&proc->list_node, &rq->system_procs);
    else
        kds_session_rb_insert(&rq->session_procs, proc);

    rq->nr_running++;
    spin_unlock(&rq->lock);

    pr_info("kds_proc registered: %s (pid=%llu, cpu=%d)\n", 
            proc->name, proc->pid, cpu);
    return 0;
}

void kds_proc_unregister(kds_proc_t *proc)
{
    kds_runqueue_t *rq;

    rq = &per_cpu(kds_runqueues, proc->cpu);
    spin_lock(&rq->lock);

    if (proc->kind == KDS_PROC_SYSTEM)
        list_del_init(&proc->list_node);
    else
        kds_session_rb_remove(&rq->session_procs, proc);

    rq->nr_running--;
    spin_unlock(&rq->lock);

    pr_info("kds_proc unregistered: %s (pid=%llu)\n", proc->name, proc->pid);
}

/* ============================
 * CPU affinity 설정
 * ============================ */
int kds_proc_set_affinity(kds_proc_t *proc, const struct cpumask *mask)
{
    kds_runqueue_t *old_rq, *new_rq;
    int old_cpu, new_cpu;

    if (!proc || cpumask_empty(mask))
        return -EINVAL;

    old_cpu = proc->cpu;
    old_rq = &per_cpu(kds_runqueues, old_cpu);

    /* 현재 CPU가 새 마스크에 포함되면 이동 불필요 */
    if (cpumask_test_cpu(old_cpu, mask)) {
        cpumask_copy(&proc->allowed_cpus, mask);
        return 0;
    }

    /* 프로세스를 다른 CPU로 마이그레이션 */
    spin_lock(&old_rq->lock);

    if (proc->kind == KDS_PROC_SYSTEM)
        list_del_init(&proc->list_node);
    else
        kds_session_rb_remove(&old_rq->session_procs, proc);

    old_rq->nr_running--;
    spin_unlock(&old_rq->lock);

    cpumask_copy(&proc->allowed_cpus, mask);
    new_cpu = kds_select_cpu(proc);
    proc->cpu = new_cpu;

    new_rq = &per_cpu(kds_runqueues, new_cpu);
    spin_lock(&new_rq->lock);

    if (proc->kind == KDS_PROC_SYSTEM)
        list_add_tail(&proc->list_node, &new_rq->system_procs);
    else
        kds_session_rb_insert(&new_rq->session_procs, proc);

    new_rq->nr_running++;
    spin_unlock(&new_rq->lock);

    atomic64_inc(&kds_stats.migrations);
    pr_info("kds_proc migrated: %s from CPU%d to CPU%d\n", 
            proc->name, old_cpu, new_cpu);

    return 0;
}

/* ============================
 * 대기 시간 업데이트
 * ============================ */
static void kds_update_wait_time(kds_runqueue_t *rq, u64 elapsed_ns, 
                                  kds_proc_t *executed)
{
    kds_proc_t *proc;

    list_for_each_entry(proc, &rq->system_procs, list_node) {
        if (proc->pid == executed->pid) {
            proc->last_run_ns = elapsed_ns;
            proc->total_runtime_ns += elapsed_ns;
            proc->wait_time_ns = 0;
        } else {
            proc->wait_time_ns += elapsed_ns;
            proc->dynamic_prio += 1;
        }
    }
}

/* ============================
 * 단일 CPU 스케줄러
 * ============================ */
void kds_proc_schedule(int cpu, u64 slice_ns)
{
    kds_runqueue_t *rq;
    kds_proc_t *proc, *best = NULL;
    kds_proc_result_t ret;
    u64 start_ns, now_ns;
    int best_prio = INT_MIN;
    int prio;

    rq = &per_cpu(kds_runqueues, cpu);

    spin_lock(&rq->lock);

    list_for_each_entry(proc, &rq->system_procs, list_node) {
        if (atomic_read(&proc->running))
            continue;

        prio = kds_calc_dynamic_prio(proc);
        if (prio > best_prio) {
            best_prio = prio;
            best = proc;
        }
    }

    if (!best) {
        spin_unlock(&rq->lock);
        return;
    }

    /* 실행 중 플래그 설정 */
    atomic_set(&best->running, 1);
    best->state = KDS_PROC_STATE_RUN;
    rq->curr_proc = best;

    spin_unlock(&rq->lock);

    /* 프로세스 실행 */
    start_ns = ktime_get_ns();
    ret = best->run(best, slice_ns);
    now_ns = ktime_get_ns();

    spin_lock(&rq->lock);

    /* 통계 업데이트 */
    kds_update_wait_time(rq, now_ns - start_ns, best);
    rq->total_runtime_ns += (now_ns - start_ns);
    
    atomic_set(&best->running, 0);
    rq->curr_proc = NULL;
    atomic64_inc(&kds_stats.total_schedules);

    spin_unlock(&rq->lock);

    cond_resched();
}

/* ============================
 * 모든 CPU 스케줄링
 * ============================ */
void kds_proc_schedule_all(u64 slice_ns)
{
    int cpu;

    for_each_online_cpu(cpu) {
        kds_proc_schedule(cpu, slice_ns);
    }
}

/* ============================
 * 로드 밸런싱
 * ============================ */
void kds_load_balance(void)
{
    int src_cpu, dst_cpu;
    kds_runqueue_t *src_rq, *dst_rq;
    kds_proc_t *proc = NULL;
    unsigned int max_load = 0, min_load = UINT_MAX;
    int max_cpu = -1, min_cpu = -1;

    /* 가장 부하가 높은 CPU와 낮은 CPU 찾기 */
    for_each_online_cpu(src_cpu) {
        src_rq = &per_cpu(kds_runqueues, src_cpu);
        if (src_rq->nr_running > max_load) {
            max_load = src_rq->nr_running;
            max_cpu = src_cpu;
        }
        if (src_rq->nr_running < min_load) {
            min_load = src_rq->nr_running;
            min_cpu = src_cpu;
        }
    }

    /* 로드 차이가 임계값 이하면 밸런싱 불필요 */
    if (max_cpu == -1 || min_cpu == -1 || 
        max_load - min_load <= KDS_LOAD_BALANCE_THRESHOLD)
        return;

    src_rq = &per_cpu(kds_runqueues, max_cpu);
    dst_rq = &per_cpu(kds_runqueues, min_cpu);

    spin_lock(&src_rq->lock);

    /* 마이그레이션할 프로세스 선택 (우선순위 낮은 것) */
    list_for_each_entry(proc, &src_rq->system_procs, list_node) {
        if (atomic_read(&proc->running))
            continue;
        if (cpumask_test_cpu(min_cpu, &proc->allowed_cpus))
            break;
    }

    if (proc) {
        list_del_init(&proc->list_node);
        src_rq->nr_running--;
        spin_unlock(&src_rq->lock);

        spin_lock(&dst_rq->lock);
        list_add_tail(&proc->list_node, &dst_rq->system_procs);
        dst_rq->nr_running++;
        proc->cpu = min_cpu;
        spin_unlock(&dst_rq->lock);

        atomic64_inc(&kds_stats.load_balances);
        atomic64_inc(&kds_stats.migrations);

        pr_info("kds_load_balance: migrated %s from CPU%d to CPU%d\n",
                proc->name, max_cpu, min_cpu);
    } else {
        spin_unlock(&src_rq->lock);
    }
}

