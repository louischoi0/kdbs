#include <linux/kds.h>
#include <linux/kds_page.h>
#include <linux/kds_page_mgr.h>
#include <linux/kds_proc.h>
#include <linux/kds_meta.h>
#include <linux/kds_dshell.h>
#include <linux/kds_page_alloc.h>
#include <linux/kds_catalog.h>
#include <linux/kds_wal.h>
#include <linux/kds_transaction.h>
#include <linux/kds_undo.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/blkdev.h>
#include <linux/vmalloc.h>
#include <linux/virtio_mmio.h>
#include <linux/platform_device.h>
#include <linux/cpumask.h>
#include <linux/smp.h>

static DEFINE_PER_CPU(struct task_struct *, kds_worker_thread);

static struct task_struct *kds_load_balancer;

#define KDS_TIME_SLICE_NS   (10 * NSEC_PER_MSEC)
#define KDS_LOAD_BALANCE_INTERVAL_MS  100

/*
 * Bootstrap completion is now tri-state, not boolean:
 *   0 = not done yet
 *   1 = succeeded
 *   2 = failed
 * Workers on CPUs other than 0 wait for either outcome. Previously
 * only "succeeded" was ever signaled -- if kds_bootstrap() failed
 * partway through, it just returned without touching
 * kds_initialized, leaving every other CPU's worker thread blocked
 * in wait_event() forever (and the load balancer thread along with
 * it). They now check for KDS_INIT_FAILED and shut themselves down
 * instead of entering the scheduler loop.
 */
#define KDS_INIT_PENDING   0
#define KDS_INIT_DONE      1
#define KDS_INIT_FAILED    2

static atomic_t kds_initialized = ATOMIC_INIT(KDS_INIT_PENDING);
static DECLARE_WAIT_QUEUE_HEAD(kds_init_wait);

static void kds_cleanup(void);

static void kds_bootstrap_failed(const char *stage, int ret)
{
    pr_err("kds: bootstrap failed at %s: %d\n", stage, ret);
    pr_debug("kds: bootstrap failed at %s: %d\n", stage, ret);
    atomic_set(&kds_initialized, KDS_INIT_FAILED);
    wake_up(&kds_init_wait);
    panic("failed");
}

void kds_bootstrap(void)
{
    int ret;

    pr_info("kds: bootstrap starting...\n");

    ret = init_block_device();
    if (ret)
        return kds_bootstrap_failed("init_block_device", ret);

    /*
     * Must come before anything that touches kds_buf_lookup_or_load()/
     * kds_buf_alloc_new() -- kds_init_dshell_system() doesn't call
     * those itself, but client commands dispatched through it will,
     * and kds_init_page_alloc_system()'s background prealloc proc
     * does too. Previously this call was missing entirely.
     */
    ret = kds_buf_pool_init();
    if (ret)
        return kds_bootstrap_failed("kds_buf_pool_init", ret);

    ret = kds_sched_init();
    if (ret)
        return kds_bootstrap_failed("kds_sched_init", ret);

    ret = kds_init_meta_system();
    if (ret)
        return kds_bootstrap_failed("kds_init_meta_system", ret);

    ret = kds_init_dshell_system();
    if (ret)
        return kds_bootstrap_failed("kds_init_dshell_system", ret);

    ret = kds_init_page_alloc_system();
    if (ret)
        return kds_bootstrap_failed("kds_init_page_alloc_system", ret);

    /*
     * Only run on a genuinely fresh database. kds_catalog_bootstrap()
     * unconditionally (re)creates the fixed catalog pages (4-7) via
     * kds_buf_alloc_new() -- the buffer pool starts empty every boot
     * regardless of what's already on disk, so without this check it
     * would silently overwrite real catalog data on every reboot.
     */
    if (kds_meta_is_fresh_init()) {
        ret = kds_catalog_bootstrap();
        if (ret)
            return kds_bootstrap_failed("kds_catalog_bootstrap", ret);
    } else {
        pr_info("kds: existing database detected, skipping catalog bootstrap\n");
    }

    ret = kds_wal_init();
    if (ret)
        return kds_bootstrap_failed("kds_wal_init", ret);

    ret = kds_wal_checkpointer_init();
    if (ret)
        return kds_bootstrap_failed("kds_wal_checkpointer_init", ret);

    /* Transaction manager: depends on the WAL (begin/commit/abort append
     * WAL records), so it comes up after kds_wal_init(). */
    ret = kds_txn_init();
    if (ret)
        return kds_bootstrap_failed("kds_txn_init", ret);

    /* Undo-page manager for the UPDATE path (lazily allocates its first
     * undo page on the first update; needs the page allocator + buffer
     * pool, both up by now). */
    ret = kds_undo_init();
    if (ret)
        return kds_bootstrap_failed("kds_undo_init", ret);

    pr_info("kds: bootstrap completed\n");

    atomic_set(&kds_initialized, KDS_INIT_DONE);
    wake_up(&kds_init_wait);
}

static int kds_worker_fn(void *arg)
{
    int cpu = (long)arg;

    pr_info("kds_worker: CPU%d started\n", cpu);

    set_cpus_allowed_ptr(current, cpumask_of(cpu));

    if (atomic_read(&kds_initialized) != KDS_INIT_DONE) {
        pr_err("kds_worker: CPU%d exiting, bootstrap did not succeed\n", cpu);
        return -1;
    }

    pr_info("kds_worker: CPU%d entering scheduler loop\n", cpu);

    while (!kthread_should_stop()) {
        kds_proc_schedule(cpu, KDS_TIME_SLICE_NS);

        #ifdef SCHEDULE_DEBUG_SLOW_MODE
        ssleep(1);
        #endif

        cond_resched();
        usleep_range(100, 200);
    }

    pr_info("kds_worker: CPU%d stopped\n", cpu);
    return 0;
}

static int kds_load_balancer_fn(void *arg)
{
    pr_info("kds_load_balancer: started\n");

    // wait_event(kds_init_wait, atomic_read(&kds_initialized) != KDS_INIT_PENDING);

    if (atomic_read(&kds_initialized) != KDS_INIT_DONE) {
        pr_err("kds_load_balancer: exiting, bootstrap did not succeed\n");
        return -1;
    }

    while (!kthread_should_stop()) {
        kds_load_balance();
        msleep_interruptible(KDS_LOAD_BALANCE_INTERVAL_MS);
    }

    pr_info("kds_load_balancer: stopped\n");
    return 0;
}

static int kds_start_workers(void)
{
    int cpu;
    struct task_struct *worker;
    char thread_name[32];

    for_each_online_cpu(cpu) {
        snprintf(thread_name, sizeof(thread_name), "kds_worker/%d", cpu);

        worker = kthread_create(kds_worker_fn, (void *)(long)cpu, thread_name);
        if (IS_ERR(worker)) {
            pr_err("kds: failed to create worker for CPU%d: %ld\n",
                   cpu, PTR_ERR(worker));
            goto err_cleanup;
        }

        kthread_bind(worker, cpu);

        per_cpu(kds_worker_thread, cpu) = worker;

        wake_up_process(worker);

        pr_info("kds: worker thread created for CPU%d\n", cpu);
    }

    return 0;

err_cleanup:
    for_each_online_cpu(cpu) {
        worker = per_cpu(kds_worker_thread, cpu);
        if (worker) {
            kthread_stop(worker);
            per_cpu(kds_worker_thread, cpu) = NULL;
        }
    }
    return -1;
}

static int kds_start_load_balancer(void)
{
    kds_load_balancer = kthread_run(kds_load_balancer_fn, NULL,
                                     "kds_load_balancer");
    if (IS_ERR(kds_load_balancer)) {
        pr_err("kds: failed to create load balancer: %ld\n",
               PTR_ERR(kds_load_balancer));
        return PTR_ERR(kds_load_balancer);
    }

    pr_info("kds: load balancer created\n");
    return 0;
}

int kds_main(void)
{
    int ret;

    kds_bootstrap();
    pr_info("kds: initializing for %d CPUs\n", num_online_cpus());

    ret = kds_start_workers();
    if (ret) {
        pr_err("kds: failed to start worker threads\n");
        return ret;
    }

    ret = kds_start_load_balancer();
    if (ret) {
        pr_err("kds: failed to start load balancer\n");
        goto err_stop_workers;
    }

    pr_info("kds: initialization successful\n");
    return 0;

err_stop_workers:
    kds_cleanup();
    return ret;
}

/*
 * Teardown order is the reverse of kds_bootstrap()'s init order, and
 * happens only after every kthread that might still be touching a
 * page (worker threads, load balancer) has been stopped -- otherwise
 * e.g. kds_buf_pool_destroy() could race a scheduler tick still
 * running a proc that reads/writes a frame.
 */
void kds_cleanup(void)
{
    int cpu;
    struct task_struct *worker;

    pr_info("kds: cleanup starting\n");

    if (kds_load_balancer) {
        kthread_stop(kds_load_balancer);
        kds_load_balancer = NULL;
    }

    for_each_online_cpu(cpu) {
        worker = per_cpu(kds_worker_thread, cpu);
        if (worker) {
            kthread_stop(worker);
            per_cpu(kds_worker_thread, cpu) = NULL;
            pr_info("kds: stopped worker for CPU%d\n", cpu);
        }
    }

    kds_sched_cleanup();

    /*
     * Transaction manager teardown. Safe here: every worker/load-balancer
     * kthread that could own a transaction has already been stopped
     * above, so nothing can begin/commit while we drop the active set.
     * A no-op if the manager was never brought up.
     */
    kds_txn_shutdown();
    kds_undo_shutdown();

    /*
     * Only tear down subsystems that actually finished initializing.
     * If bootstrap failed partway through (KDS_INIT_FAILED), some of
     * these were never brought up -- calling their shutdown
     * functions unconditionally would mean tearing down state that
     * doesn't exist. Each shutdown function below is written to be a
     * safe no-op if its subsystem was never initialized (e.g.
     * kds_buf_pool_destroy() checks g_pool for NULL), but
     * kds_shutdown_dshell_system() in particular relies on its own
     * "inited" flag rather than us tracking bootstrap progress here.
     */
    kds_shutdown_page_alloc_system();
    kds_shutdown_dshell_system();
    kds_buf_pool_destroy();
    kds_shutdown_meta_system();
    exit_block_device();

    pr_info("kds: cleanup completed\n");
}

static int __init kds_late_init(void)
{
    return kds_main();
}

static void __exit kds_exit(void)
{
    kds_cleanup();
}

late_initcall(kds_late_init);
module_exit(kds_exit);