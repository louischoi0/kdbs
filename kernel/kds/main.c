#include <linux/kds.h>
#include <linux/kds_page.h>
#include <linux/kds_proc.h>
#include <linux/kds_meta.h>
#include <linux/kds_dshell.h>
#include <linux/kds_page_alloc.h>
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

static atomic_t kds_initialized = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(kds_init_wait);

static void kds_cleanup(void);

void kds_bootstrap(void) 
{
    int ret;
    
    pr_info("kds: bootstrap starting...\n");
    
    ret = init_block_device();
    if (ret) {
        pr_err("kds: init block device failed: %d\n", ret);
        return;
    }
    
    ret = kds_sched_init();
    if (ret) {
        pr_err("kds: scheduler init failed: %d\n", ret);
        return;
    }
    
    kds_init_meta_system();
    
    kds_init_dshell_system();
    // kds_init_checkpointer_system();
    kds_init_page_alloc_system();
    
    pr_info("kds: bootstrap completed\n");
    
    atomic_set(&kds_initialized, 1);
    wake_up(&kds_init_wait);
}

static int kds_worker_fn(void *arg)
{
    int cpu = (long)arg;
    
    pr_info("kds_worker: CPU%d started\n", cpu);
    
    set_cpus_allowed_ptr(current, cpumask_of(cpu));
    
    if (cpu == 0) {
        kds_bootstrap();
    } else {
        wait_event(kds_init_wait, atomic_read(&kds_initialized) == 1);
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
    
    wait_event(kds_init_wait, atomic_read(&kds_initialized) == 1);
    
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