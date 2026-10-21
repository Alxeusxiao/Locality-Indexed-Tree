/* =====================================================================
 * thread.c —— 极简并行原语实现
 * ===================================================================== */
#include "thread.h"

#include <stdlib.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

int li_cpu_count(void)
{
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (si.dwNumberOfProcessors > 0) ? (int)si.dwNumberOfProcessors : 1;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (int)n : 1;
#endif
}

/* ---------------------------------------------------------------------
 * 任务池：共享一个"下一个任务号"，线程用原子加法领取，实现动态负载均衡。
 * 各任务的耗时本来就不均匀（查询快慢取决于 key），静态切分会让先完成的
 * 线程空等，因此这里用领取式而不是按区间切分。
 * ------------------------------------------------------------------- */
typedef struct {
    void (*worker)(void *ctx, int task);
    void *ctx;
    int count;
    int next;
} LiJob;

static void job_run(LiJob *job)
{
    for (;;) {
        int task = __atomic_fetch_add(&job->next, 1, __ATOMIC_SEQ_CST);
        if (task >= job->count) {
            return;
        }
        job->worker(job->ctx, task);
    }
}

#if defined(_WIN32)

static DWORD WINAPI thread_entry(LPVOID arg)
{
    job_run((LiJob *)arg);
    return 0;
}

int li_parallel_for(int threads, int count, void (*worker)(void *ctx, int task), void *ctx)
{
    LiJob job;
    HANDLE handles[64];
    int want;
    int started = 0;
    int i;

    if (worker == NULL || count <= 0) {
        return -1;
    }
    if (threads <= 1 || count == 1) {
        for (i = 0; i < count; ++i) {
            worker(ctx, i);
        }
        return 0;
    }

    want = (threads < count) ? threads : count;
    if (want > 64) {
        want = 64;
    }
    job.worker = worker;
    job.ctx = ctx;
    job.count = count;
    job.next = 0;

    /* 主线程也参与干活，因此只需再起 want-1 个 */
    for (i = 0; i < want - 1; ++i) {
        handles[i] = CreateThread(NULL, 0, thread_entry, &job, 0, NULL);
        if (handles[i] == NULL) {
            break;
        }
        started++;
    }
    job_run(&job);
    for (i = 0; i < started; ++i) {
        WaitForSingleObject(handles[i], INFINITE);
        CloseHandle(handles[i]);
    }
    return 0;
}

#else /* POSIX */

static void *thread_entry(void *arg)
{
    job_run((LiJob *)arg);
    return NULL;
}

int li_parallel_for(int threads, int count, void (*worker)(void *ctx, int task), void *ctx)
{
    LiJob job;
    pthread_t tids[64];
    int want;
    int started = 0;
    int i;

    if (worker == NULL || count <= 0) {
        return -1;
    }
    if (threads <= 1 || count == 1) {
        for (i = 0; i < count; ++i) {
            worker(ctx, i);
        }
        return 0;
    }

    want = (threads < count) ? threads : count;
    if (want > 64) {
        want = 64;
    }
    job.worker = worker;
    job.ctx = ctx;
    job.count = count;
    job.next = 0;

    for (i = 0; i < want - 1; ++i) {
        if (pthread_create(&tids[i], NULL, thread_entry, &job) != 0) {
            break;
        }
        started++;
    }
    job_run(&job);
    for (i = 0; i < started; ++i) {
        pthread_join(tids[i], NULL);
    }
    return 0;
}

#endif
