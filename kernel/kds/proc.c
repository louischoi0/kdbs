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

/* ------------------------------------------------------------------
 * Dynamic priority calculation
 * ------------------------------------------------------------------ */

static int kds_calc_dynamic_prio(kds_proc_t *p)
{
    return p->dynamic_prio;
}

/* ------------------------------------------------------------------
 * Red-black tree helpers (SESSION procs)
 * ------------------------------------------------------------------ */

static void kds_session_rb_insert(struct rb_root *root, kds_proc_t *proc)
{
    struct rb_node **link = &root->rb_node;
    struct rb_node  *parent = NULL;
    kds_proc_t      *entry;

    while (*link) {
        parent = *link;
        entry  = rb_entry(parent, kds_proc_t, rb_node);
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

/* ------------------------------------------------------------------
 * Runqueue selection (load-aware CPU picking)
 * ------------------------------------------------------------------ */

static int kds_select_cpu(kds_proc_t *proc)
{
    int cpu, best_cpu = -1;
    unsigned int min_load = UINT_MAX;
    kds_runqueue_t *rq;

    /* Prefer the proc's preferred CPU if it is still in the allowed set. */
    if (proc->preferred_cpu >= 0 &&
        cpumask_test_cpu(proc->preferred_cpu, &proc->allowed_cpus)) {
        rq = &per_cpu(kds_runqueues, proc->preferred_cpu);
        if (rq->nr_running < min_load)
            return proc->preferred_cpu;
    }

    /* Otherwise pick the least-loaded CPU in the allowed set. */
    for_each_cpu(cpu, &proc->allowed_cpus) {
        rq = &per_cpu(kds_runqueues, cpu);
        if (rq->nr_running < min_load) {
            min_load = rq->nr_running;
            best_cpu = cpu;
        }
    }

    return best_cpu >= 0 ? best_cpu : cpumask_first(&proc->allowed_cpus);
}

/* ------------------------------------------------------------------
 * Register / unregister
 * ------------------------------------------------------------------ */

int kds_proc_register(kds_proc_t *proc)
{
    kds_runqueue_t *rq;
    int cpu;

    if (!proc || !proc->run)
        return -EINVAL;

    proc->state            = KDS_PROC_STATE_READY;
    proc->last_run_ns      = 0;
    proc->total_runtime_ns = 0;
    proc->wait_time_ns     = 0;
    proc->pid              = atomic64_fetch_add(1, &kds_next_pid);
    atomic_set(&proc->running, 0);

    if (cpumask_empty(&proc->allowed_cpus))
        cpumask_copy(&proc->allowed_cpus, cpu_online_mask);

    cpu              = kds_select_cpu(proc);
    proc->cpu        = cpu;
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

/* ------------------------------------------------------------------
 * CPU affinity
 * ------------------------------------------------------------------ */

int kds_proc_set_affinity(kds_proc_t *proc, const struct cpumask *mask)
{
    kds_runqueue_t *old_rq, *new_rq;
    int old_cpu, new_cpu;

    if (!proc || cpumask_empty(mask))
        return -EINVAL;

    old_cpu = proc->cpu;
    old_rq  = &per_cpu(kds_runqueues, old_cpu);

    /* If the current CPU is still in the new mask, no migration needed. */
    if (cpumask_test_cpu(old_cpu, mask)) {
        cpumask_copy(&proc->allowed_cpus, mask);
        return 0;
    }

    /* Migrate to a CPU that is in the new mask. */
    spin_lock(&old_rq->lock);
    if (proc->kind == KDS_PROC_SYSTEM)
        list_del_init(&proc->list_node);
    else
        kds_session_rb_remove(&old_rq->session_procs, proc);
    old_rq->nr_running--;
    spin_unlock(&old_rq->lock);

    cpumask_copy(&proc->allowed_cpus, mask);
    new_cpu   = kds_select_cpu(proc);
    proc->cpu = new_cpu;
    new_rq    = &per_cpu(kds_runqueues, new_cpu);

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

/* ------------------------------------------------------------------
 * Wait-time accounting
 *
 * Called after every scheduling tick to age wait_time_ns of procs
 * that did not run and to reset it for the proc that did.
 * ------------------------------------------------------------------ */

static void kds_update_wait_time(kds_runqueue_t *rq, u64 elapsed_ns,
                                  kds_proc_t *executed)
{
    kds_proc_t *proc;

    list_for_each_entry(proc, &rq->system_procs, list_node) {
        if (proc->pid == executed->pid) {
            proc->last_run_ns       = elapsed_ns;
            proc->total_runtime_ns += elapsed_ns;
            proc->wait_time_ns      = 0;
        } else {
            proc->wait_time_ns += elapsed_ns;
            proc->dynamic_prio += 1;   /* priority boost for starving procs */
        }
    }
}

/* ------------------------------------------------------------------
 * Single-CPU scheduler  --  kds_proc_schedule()
 *
 * Fast-path (no context switch):
 *   When the number of runnable procs on this CPU is less than the
 *   number of online CPUs, there is spare CPU capacity across the
 *   system.  In that situation the currently running proc is
 *   re-selected without scanning the runqueue, avoiding unnecessary
 *   context-switch overhead and keeping hot cache state alive.
 *   cond_resched() is skipped as well since yielding to the Linux
 *   scheduler would only add latency when there is nothing else
 *   competing.
 *
 * Normal path (context switch):
 *   When rq->nr_running >= num_online_cpus() every CPU is busy and
 *   fairness matters; the highest-priority ready proc is selected by
 *   scanning the runqueue.
 * ------------------------------------------------------------------ */

void kds_proc_schedule(int cpu, u64 slice_ns)
{
    kds_runqueue_t   *rq;
    kds_proc_t       *proc, *best = NULL;
    kds_proc_result_t ret;
    u64               start_ns, now_ns;
    bool              fast_path;

    rq = &per_cpu(kds_runqueues, cpu);

    spin_lock(&rq->lock);

    /*
     * Fast-path check: if the number of procs waiting on this runqueue
     * is below the number of online CPUs, there is enough CPU capacity
     * to keep running the same proc without switching.  Re-use
     * rq->curr_proc directly when it is still ready; fall through to
     * the normal selection path if curr_proc is NULL (first call) or
     * is already marked running by another context.
     */
    fast_path = (rq->nr_running < (unsigned int)num_online_cpus()) &&
                rq->curr_proc != NULL &&
                !atomic_read(&rq->curr_proc->running);

    if (fast_path) {
        best = rq->curr_proc;
    } else {
        int best_prio = INT_MIN;
        int prio;

        list_for_each_entry(proc, &rq->system_procs, list_node) {
            if (atomic_read(&proc->running))
                continue;
            prio = kds_calc_dynamic_prio(proc);
            if (prio > best_prio) {
                best_prio = prio;
                best      = proc;
            }
        }
    }

    if (!best) {
        spin_unlock(&rq->lock);
        return;
    }

    atomic_set(&best->running, 1);
    best->state   = KDS_PROC_STATE_RUN;
    rq->curr_proc = best;
    spin_unlock(&rq->lock);

    /* Run the selected proc for one slice. */
    start_ns = ktime_get_ns();
    ret       = best->run(best, slice_ns);
    now_ns    = ktime_get_ns();

    spin_lock(&rq->lock);
    kds_update_wait_time(rq, now_ns - start_ns, best);
    rq->total_runtime_ns += (now_ns - start_ns);
    atomic_set(&best->running, 0);

    /*
     * On the fast-path keep curr_proc pointing at the same proc so
     * the next call can reuse it without scanning.  On the normal path
     * clear it to force a fresh selection next time.
     */
    if (!fast_path)
        rq->curr_proc = NULL;

    atomic64_inc(&kds_stats.total_schedules);
    spin_unlock(&rq->lock);

    /*
     * cond_resched() lets the Linux scheduler run if needed.  Skip it
     * on the fast-path: the whole point is to avoid the overhead of a
     * voluntary preemption when the system has spare CPU capacity.
     */
    if (!fast_path)
        cond_resched();
}

/* ------------------------------------------------------------------
 * Schedule all online CPUs
 * ------------------------------------------------------------------ */

void kds_proc_schedule_all(u64 slice_ns)
{
    int cpu;

    for_each_online_cpu(cpu)
        kds_proc_schedule(cpu, slice_ns);
}

/* ------------------------------------------------------------------
 * Load balancing
 *
 * Finds the most- and least-loaded runqueues.  If the load difference
 * exceeds KDS_LOAD_BALANCE_THRESHOLD, moves one proc from the heaviest
 * queue to the lightest.
 * ------------------------------------------------------------------ */

void kds_load_balance(void)
{
    int            src_cpu;
    kds_runqueue_t *src_rq, *dst_rq;
    kds_proc_t     *proc = NULL;
    unsigned int    max_load = 0, min_load = UINT_MAX;
    int             max_cpu = -1, min_cpu = -1;

    /* Find the most- and least-loaded CPUs. */
    for_each_online_cpu(src_cpu) {
        src_rq = &per_cpu(kds_runqueues, src_cpu);
        if (src_rq->nr_running > max_load) {
            max_load = src_rq->nr_running;
            max_cpu  = src_cpu;
        }
        if (src_rq->nr_running < min_load) {
            min_load = src_rq->nr_running;
            min_cpu  = src_cpu;
        }
    }

    /* Nothing to balance if the load difference is within the threshold. */
    if (max_cpu == -1 || min_cpu == -1 ||
        max_load - min_load <= KDS_LOAD_BALANCE_THRESHOLD)
        return;

    src_rq = &per_cpu(kds_runqueues, max_cpu);
    dst_rq = &per_cpu(kds_runqueues, min_cpu);

    spin_lock(&src_rq->lock);

    /*
     * Pick the first non-running proc on the overloaded queue that is
     * allowed to run on the target CPU.
     */
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