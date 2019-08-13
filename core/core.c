#include <limits.h>
#include <time.h>
#include <string.h>
#include <sys/epoll.h>
#include <semaphore.h>
#include <pthread.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <stdarg.h>
#include <errno.h>

#define DBG_SUBSYS S_LTG_CORE

#include "ltg_net.h"
#include "ltg_utils.h"
#include "ltg_core.h"
#include "ltg_core.h"
#include "ltg_net.h"

#define CORE_CHECK_KEEPALIVE_INTERVAL 1
#define CORE_CHECK_CALLBACK_INTERVAL 5
#define CORE_CHECK_HEALTH_INTERVAL 30
#define CORE_CHECK_HEALTH_TIMEOUT 180

static core_t *__core_array__[256];
static uint64_t __core_mask__;
static __thread core_t *__core__;

int core_ring_init(core_t *core);
int core_ring_count(core_t *core);
int core_request_va1(int hash, int priority, const char *name,
                     func_va_t exec, va_list ap);

core_t *core_self()
{
        //return core_tls_get(VARIABLE_CORE);
        return __core__;
}

int core_usedby(uint64_t mask, int idx)
{
        LTG_ASSERT(idx <= CORE_MAX);
        return mask & ((LLU)1 << idx);
}

int core_used(int idx)
{
        return core_usedby(__core_mask__, idx);
}

uint64_t core_mask()
{
        return __core_mask__;
}

STATIC void *__core_check_health__(void *_arg)
{
        int ret;
        core_t *core = NULL;
        time_t now;
        (void)_arg;

        while (1) {
                sleep(CORE_CHECK_HEALTH_INTERVAL);

                now = gettime();
                for (int i = 0; i < CORE_MAX; i++) {
                        if (!core_used(i))
                                continue;

                        core = core_get(i);
                        if (unlikely(core == NULL))
                                continue;

                        ret = ltg_spin_lock(&core->keepalive_lock);
                        if (unlikely(ret))
                                continue;

                        if (unlikely(now - core->keepalive > CORE_CHECK_HEALTH_TIMEOUT)) {
                                ltg_spin_unlock(&core->keepalive_lock);
                                DERROR("polling core[%d] block !!!!!\n", core->hash);
                                LTG_ASSERT(0);
                                EXIT(EAGAIN);
                        }

                        ltg_spin_unlock(&core->keepalive_lock);
                }
        }
}

static void __core_check_keepalive(core_t *core, time_t now)
{
        int ret;

        if (likely(now - core->keepalive < CORE_CHECK_KEEPALIVE_INTERVAL)) {
                return;
        }

        ret = ltg_spin_lock(&core->keepalive_lock);
        if (unlikely(ret))
                return;

        core->keepalive = now;

        ltg_spin_unlock(&core->keepalive_lock);
}

static void IO_FUNC core_stat(core_t *core)
{
        int sid, taskid, task_wait, task_used, task_runable, ring_count;
        uint64_t run_time, io_time, c_runtime;
        uint32_t queue_count, io_queue = 0, io_lat = 0;

        sche_stat(&sid, &taskid, &task_runable, &task_wait, &task_used, &run_time, &queue_count, &io_time, &c_runtime);
        ring_count = core_ring_count(core);

        _gettimeofday(&core->stat_t2, NULL);
        uint64_t used = _time_used(&core->stat_t1, &core->stat_t2);
        if (used > 0) {
            if (queue_count == 0)
                io_queue = 0;
            else {
                io_queue = io_time / used;
                io_lat = io_time / queue_count;
            }
#if ENABLE_PERF
                      DINFO("%s[%d] pps:%jd task:%u/%u/%lu/%u/%u ring:%u, counter:%ju cpu_usage(%ju%), nvme_io_stat(%u/%u) perf %u\n",
#else
                      DINFO("%s[%d] pps:%jd task:%u/%u/%lu/%u/%u ring:%u counter:%ju cpu %ju io %u/%u\n",
#endif
                      core->name, core->hash,
                      (core->stat_nr2 - core->stat_nr1) * 1000000 / used,
                      TASK_MAX, task_used, c_runtime / used, task_wait, task_runable,
                      ring_count, core->sche->counter / (core->stat_nr2 - core->stat_nr1), (run_time * 100)/ used,
#if ENABLE_PERF
                      io_lat, io_queue , get_io());
#else
                      io_lat, io_queue);
#endif
                core->stat_t1 = core->stat_t2;
                core->stat_nr1 = core->stat_nr2;
        }
}

void IO_FUNC core_worker_run(core_t *core)
{
        struct list_head *pos;
        routine_t *routine;
        void *ctx = NULL;

        core->stat_nr2++;

#if SCHE_ANALYSIS
        ANALYSIS_BEGIN(0);
#endif

        sche_run(core->sche);

        list_for_each(pos, &core->poller_list) {
                routine = (void *)pos;
                routine->func(core, ctx, routine->ctx);

                //sche_run(core->sche);
        }

        sche_run(core->sche);

        list_for_each(pos, &core->routine_list) {
                routine = (void *)pos;
                routine->func(core, ctx, routine->ctx);

                //sche_run(core->sche);
        }

        time_t now = gettime();

        if (unlikely(now - core->last_scan > 3)) {
                core->last_scan = now;

                list_for_each(pos, &core->scan_list) {
                        routine = (void *)pos;
                        routine->func(core, ctx, routine->ctx);
                }


                __core_check_keepalive(core, gettime());

                sche_scan(core->sche);

                core_stat(core);
        }

        gettime_refresh(ctx);
        timer_expire(ctx);

#if ENABLE_ANALYSIS
        analysis_merge(ctx);
#endif

#if SCHE_ANALYSIS
        ANALYSIS_QUEUE(0, IO_WARN, NULL);
#endif
}

static int __core_worker_init(core_t *core)
{
        int ret;
        char name[MAX_NAME_LEN];

        DINFO("core[%u] init begin, polling %s\n", core->hash,
              core->flag & CORE_FLAG_POLLING ? "on" : "off");

        INIT_LIST_HEAD(&core->poller_list);
        INIT_LIST_HEAD(&core->routine_list);
        INIT_LIST_HEAD(&core->destroy_list);
        INIT_LIST_HEAD(&core->scan_list);

        snprintf(name, sizeof(name), "%s[%u]", core->name, core->hash);

        __core__ = core;
        core_tls_set(VARIABLE_CORE, core);

        int nodeid = -1;
        if (core->main_core) {
                ret = cpuset_set(name, core->main_core->cpu_id);
                if (unlikely(ret)) {
                        ret = EINVAL;
                        GOTO(err_ret, ret);
                }

                DINFO("%s[%u] cpu set cpu id %d\n", core->name, core->hash,
                      core->main_core->cpu_id);

                nodeid = core->main_core->node_id;
        }

        if (ltgconf.daemon) {
                void *hugepage = hugepage_private_init(core->hash, nodeid);
                core_tls_set(VARIABLE_HUGEPAGE, hugepage);
        }

        core->interrupt_eventfd = -1;
        int *interrupt = !(core->flag & CORE_FLAG_POLLING) ? &core->interrupt_eventfd : NULL;

        snprintf(name, sizeof(name), core->name);
        ret = sche_create(interrupt, name, &core->sche_idx, &core->sche, NULL);
        if (unlikely(ret)) {
                GOTO(err_ret, ret);
        }

        core_tls_set(VARIABLE_SCHEDULE, core->sche);

        DINFO("%s[%u] sche[%d] inited\n", core->name, core->hash, core->sche_idx);

        if (!interrupt) {
                ret = timer_init(1);
                if (unlikely(ret))
                        GOTO(err_ret, ret);
        }

        DINFO("%s[%u] timer inited\n", core->name, core->hash);

        ret = gettime_private_init();
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = slab_stream_private_init();
        if (ret)
                GOTO(err_ret, ret);

        ret = slab_static_private_init();
        if (ret)
                GOTO(err_ret, ret);

        if (ltgconf.daemon) {
                ret = mem_ring_private_init(core->hash);
                if (unlikely(ret))
                        GOTO(err_ret, ret);
        }

        ret = core_ring_init(core);
        if (ret)
                GOTO(err_ret, ret);

        DINFO("%s[%u] mem inited\n", core->name, core->hash);

        if (ltgconf.performance_analysis) {
                snprintf(name, sizeof(name), "%s[%u]", core->name, core->hash);

                ret = analysis_private_create(name);
                if (unlikely(ret)) {
                        GOTO(err_ret, ret);
                }

                DINFO("%s[%u] analysis inited\n", core->name, core->hash);
        }

        //core_register_tls(VARIABLE_CORE, private_mem);

        sem_post(&core->sem);

        return 0;
err_ret:
        return ret;
}

static void * IO_FUNC __core_worker(void *_args)
{
        int ret;
        core_t *core = _args;

        DINFO("start %s idx %d\n", core->name, core->hash);

        ret = __core_worker_init(core);
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        while (1) {
                core_worker_run(core);
        }

        DFATAL("name %s sche[%d] hash %d\n", core->name, core->sche_idx, core->hash);
        return NULL;
}

#define POLLING_LOCK 1

static int __core_create(core_t **_core, const char *name, int hash, int flag)
{
        int ret, lock;
        core_t *core;

        UNIMPLEMENTED(__WARN__);//slab_stream_alloc

        ret = ltg_malloc((void **)&core, sizeof(*core));
        if (unlikely(ret))
                GOTO(err_ret, ret);

        memset(core, 0x0, sizeof(*core));

#if POLLING_LOCK
        lock = ltgconf.daemon && (flag & CORE_FLAG_POLLING);
#else
        lock = ltgconf.daemon;
#endif

        if (lock) {
                ret = cpuset_lock(hash, &core->main_core);
                if (unlikely(ret))
                        GOTO(err_ret, ret);
        } else {
                core->main_core = NULL;
        }

        strcpy(core->name, name);
        core->sche_idx = -1;
        core->hash = hash;
        core->flag = flag;
        core->keepalive = gettime();
        core->last_scan = gettime();

        ret = ltg_spin_init(&core->keepalive_lock);
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        ret = sem_init(&core->sem, 0, 0);
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        ret = ltg_thread_create(__core_worker, core, "__core_worker");
        if (ret == -1) {
                ret = errno;
                GOTO(err_free, ret);
        }

        *_core = core;

        return 0;
err_free:
        ltg_free((void **)&core);
err_ret:
        return ret;
}

int core_init(uint64_t mask, int flag)
{
        int ret;
        core_t *core = NULL;

        if (mask == 0) {
                LTG_ASSERT(ltgconf.polling_timeout || ltgconf.daemon);
                flag = flag ^ CORE_FLAG_POLLING;

                //mask = (LLU)1 << (CORE_MAX - 1);
                mask = 1;
                DINFO("set coremask default\n");
                ltgconf.coremask = mask;
        }

        __core_mask__ = mask;

        ret = cpuset_init();
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        //DINFO("core init begin %u %u flag %d\n", polling_core, cpuset_useable(), flag);

        ret = hugepage_init(ltgconf.daemon, mask, ltgconf.use_huge);
        if (ret)
                GOTO(err_ret, ret);

        ret = mem_ring_init();
        if (ret)
                GOTO(err_ret, ret);

        ret = slab_stream_init();
        if (ret)
                GOTO(err_ret, ret);

        ret = slab_static_init();
        if (ret)
                GOTO(err_ret, ret);

        for (int i = 0; i < CORE_MAX; i++) {
                if (!core_used(i))
                        continue;

                ret = __core_create(&core, "core", i, flag);
                if (unlikely(ret))
                        UNIMPLEMENTED(__DUMP__);

                __core_array__[i] = core;

                DINFO("core[%d] hash %d  sche[%d]\n",
                      i, core->hash, core->sche_idx);
        }

        for (int i = 0; i < CORE_MAX; i++) {
                if (!core_used(i))
                        continue;

                core = __core_array__[i];
                ret = _sem_wait(&core->sem);
                if (unlikely(ret)) {
                        UNIMPLEMENTED(__DUMP__);
                }
        }

        ret = ltg_thread_create(__core_check_health__, NULL, "core_check_health");
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        ret = corenet_init(flag);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = corerpc_init();
        if (unlikely(ret))
                GOTO(err_ret, ret);

        ret = corenet_maping_init();
        if (unlikely(ret))
                GOTO(err_ret, ret);

#if 1
        ret = core_latency_init();
        if (unlikely(ret))
                GOTO(err_ret, ret);
#endif

        return 0;
err_ret:
        return ret;
}

int core_attach(int hash, const sockid_t *sockid, const char *name,
                void *ctx, core_exec func, func_t reset, func_t check)
{
        int ret;
        core_t *core;

        DINFO("attach hash %d fd %d name %s\n", hash, sockid->sd, name);

        core = core_get(hash);

        ret = corenet_attach(core->corenet, sockid, ctx, func, reset, check, NULL, name);
        if (unlikely(ret))
                GOTO(err_ret, ret);

        sche_post(core->sche);

        return 0;
err_ret:
        return ret;
}

core_t *core_get(int hash)
{
        LTG_ASSERT(core_used(hash));
        return __core_array__[hash];
}

void core_tls_set(int type, void *ptr)
{
        core_t *core = core_self();

        if (core == NULL)
                LTG_ASSERT(0);

        LTG_ASSERT(type <= TLS_MAX);

        core->tls[type] = ptr;
}

void core_iterator(func1_t func, const void *opaque)
{
        core_t *core;

        for (int i = 0; i < CORE_MAX; i++) {
                if (!core_used(i))
                        continue;

                core = __core_array__[i];
                func(core, (void *)opaque);
        }
}

static void __core_dump_memory(void *_core, void *_arg)
{
        core_t *core = _core;
        uint64_t *memory = _arg;

        sche_t *sche = core->sche;
        *memory += sizeof(core_t) +
                   sizeof(sche_t) +
                   (sizeof(taskctx_t) + DEFAULT_STACK_SIZE) * sche->size;
}

/**
 * 获取内存使用量
 *
 * @return
 */
int core_dump_memory(uint64_t *memory)
{
        *memory = 0;

        core_iterator(__core_dump_memory, memory);

        return 0;
}


static int __core_register(struct list_head *list, const char *name, func2_t func, void *ctx)
{
        int ret;
        routine_t *routine;

        ret = slab_static_alloc1((void **)&routine, sizeof(*routine));
        if(ret)
                GOTO(err_ret, ret);

        strncpy(routine->name, name, 64 - 1);
        routine->func = func;
        routine->ctx = ctx;
        list_add_tail(&routine->hook, list);

        return 0;
err_ret:
        return ret;
}

int core_register_poller(const char *name, func2_t func, void *ctx)
{
        int ret;
        core_t *core = core_self();

        ret = __core_register(&core->poller_list, name, func, ctx);
        if(ret)
                GOTO(err_ret, ret);

        DINFO("register poller[%d], name: %s\r\n",
              core->hash, name);

        return 0;
err_ret:
        return ret;
}

int core_register_routine(const char *name, func2_t func, void *ctx)
{
        int ret;
        core_t *core = core_self();

        ret = __core_register(&core->routine_list, name, func, ctx);
        if(ret)
                GOTO(err_ret, ret);

        DINFO("register routine[%d], name: %s\r\n",
              core->hash, name);

        return 0;
err_ret:
        return ret;
}

int core_register_scan(const char *name, func2_t func, void *ctx)
{
        int ret;
        core_t *core = core_self();

        ret = __core_register(&core->scan_list, name, func, ctx);
        if(ret)
                GOTO(err_ret, ret);

        DINFO("register scan[%d], name: %s\r\n",
              core->hash, name);

        return 0;
err_ret:
        return ret;
}


int core_register_destroy(const char *name, func2_t func, void *ctx)
{
        int ret;
        core_t *core = core_self();

        ret = __core_register(&core->destroy_list, name, func, ctx);
        if(ret)
                GOTO(err_ret, ret);

        DINFO("register destroy[%d], name: %s\r\n",
              core->hash, name);

        return 0;
err_ret:
        return ret;
}

int core_islocal(const coreid_t *coreid)
{
        core_t *core;

        if (!net_islocal(&coreid->nid)) {
                DBUG("nid %u\n", coreid->nid.id);
                return 0;
        }

        core = core_self();

        if (unlikely(core == NULL))
                return 0;

        if (core->hash != (int)coreid->idx) {
                DBUG("idx %u %u\n", core->hash, coreid->idx);
                return 0;
        }

        return 1;
}

int core_getid(coreid_t *coreid)
{
        int ret;
        core_t *core = core_self();

        if (unlikely(core == NULL)) {
                ret = ENOSYS;
                GOTO(err_ret, ret);
        }

        coreid->nid = *net_getnid();
        LTG_ASSERT(coreid->nid.id > 0);
        coreid->idx = core->hash;

        return 0;
err_ret:
        return ret;
}

int core_init_modules(const char *name, func_va_t exec, ...)
{
        int ret;
        va_list ap;

        va_start(ap, exec);

        for (int i = 0; i < CORE_MAX; i++) {
                if (!core_used(i))
                        continue;

                ret = core_request_va1(i, -1, name, exec, ap);
                if (ret)
                        GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}

void core_occupy(const char *name, uint64_t coremask)
{
        core_t *core;
        char tmp[MAX_NAME_LEN];

        for (int i = 0; i < CORE_MAX; i++) {
                if (!core_usedby(coremask, i)) {
                        continue;
                }

                LTG_ASSERT(core_used(i));

                core = core_get(i);

                if (strcmp(core->name, "core")) {
                        snprintf(tmp, MAX_NAME_LEN, "%s|%s", core->name, name);
                        strcpy(core->name, tmp);
                } else {
                        strcpy(core->name, name);
                }
        }
}

int core_init_modules1(const char *name, uint64_t coremask, func_va_t exec, ...)
{
        int ret;
        va_list ap;

        va_start(ap, exec);

        for (int i = 0; i < CORE_MAX; i++) {
                if (!core_usedby(coremask, i))
                        continue;

                LTG_ASSERT(core_used(i));

                ret = core_request_va1(i, -1, name, exec, ap);
                if (ret)
                        GOTO(err_ret, ret);
        }

        return 0;
err_ret:
        return ret;
}

typedef struct {
        task_t task;
        sem_t sem;
        func_va_t exec;
        va_list ap;
        int type;
        int retval;
} arg1_t;

#define REQUEST_SEM 1
#define REQUEST_TASK 2

static void __core_request__(void *_ctx)
{
        arg1_t *ctx = _ctx;

        ctx->retval = ctx->exec(ctx->ap);

        if (ctx->type == REQUEST_SEM) {
                sem_post(&ctx->sem);
        } else {
                sche_task_post(&ctx->task, 0, NULL);
        }
}

int core_request_va1(int hash, int priority, const char *name, func_va_t exec, va_list ap)
{
        int ret;
        core_t *core;
        sche_t *sche;
        arg1_t ctx;

        core = core_get(hash);
        sche = core->sche;
        if (unlikely(sche == NULL)) {
                ret = ENOSYS;
                GOTO(err_ret, ret);
        }

        ctx.exec = exec;
        va_copy(ctx.ap, ap);

        task_t task;
        int yield = 0;
        if (sche_running()) {
                sche_t *sche = sche_self();

                ret = sche_task_get1(sche, &task);
                if (ret) {
                        DWARN("task busy\n");
                        UNIMPLEMENTED(__DUMP__);
                } else {
                        yield = 1;
                }
        }

        if (likely(yield)) {
                ctx.type = REQUEST_TASK;
                ctx.task = task;

                ret = sche_request(sche, priority, __core_request__, &ctx, name);
                if (unlikely(ret))
                        GOTO(err_ret, ret);

                ret = sche_yield1(name, NULL, NULL, NULL, -1);
                if (unlikely(ret)) {
                        GOTO(err_ret, ret);
                }
        } else {
                ctx.type = REQUEST_SEM;
                ret = sem_init(&ctx.sem, 0, 0);
                if (unlikely(ret))
                        UNIMPLEMENTED(__DUMP__);

                ret = sche_request(sche, priority, __core_request__, &ctx, name);
                if (unlikely(ret))
                        GOTO(err_ret, ret);

                if (core_self()) {
                        int retry = 0;

                        while (1) {
                                struct timespec ts;
                                clock_gettime(CLOCK_REALTIME, &ts);
                                ts.tv_nsec += 1000 * 10;
                                ret = _sem_timedwait(&ctx.sem, &ts);
                                if (unlikely(ret)) {
                                        if (ret == ETIMEDOUT) {
                                                DWARN("wait %d\n", retry);
                                                retry++;
                                                core_worker_run(core_self());
                                                continue;
                                        } else
                                                GOTO(err_ret, ret);
                                }

                                break;
                        }
                } else {
                        ret = _sem_wait(&ctx.sem);
                        if (unlikely(ret)) {
                                GOTO(err_ret, ret);
                        }
                }
        }

        return ctx.retval;
err_ret:
        return ret;
}

int core_request(int coreid, int group, const char *name, func_va_t exec, ...)
{
        va_list ap;

        va_start(ap, exec);

        return core_request_va1(coreid, group, name, exec, ap);
}

void * IO_FUNC core_tls_getfrom(void *_core, int type)
{
        core_t *core = _core;

        if (unlikely(core == NULL || core->tls[type] == NULL))
                return NULL;

        return core->tls[type];
}

void * IO_FUNC core_tls_get(int type)
{
        core_t *core = core_self();

        return core_tls_getfrom(core, type);
}

void * IO_FUNC core_tls_getfrom1(void *core, int type)
{
        (void) core;

        return core_tls_get(type);
}


void coremask_trans(coremask_t *coremask, uint64_t mask)
{
        char tmp[MAX_NAME_LEN];

        memset(coremask, 0x0, sizeof(*coremask));

        tmp[0] = '\0';
        for (int i = 0; i < CORE_MAX; i++) {
                if (!core_usedby(mask, i)) {
                        continue;
                }

                coremask->coreid[coremask->count] = i;
                coremask->count++;

                snprintf(tmp + strlen(tmp), MAX_NAME_LEN, "%d,", i);
        }

        LTG_ASSERT(coremask->count);

        DINFO("mask 0x%x %s\n", mask, tmp);
}

int coremask_hash(const coremask_t *coremask, uint64_t id)
{
        LTG_ASSERT(coremask->count);

        int hash = id % coremask->count;

        return coremask->coreid[hash];
}