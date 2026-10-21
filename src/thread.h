/* =====================================================================
 * thread.h —— 极简并行原语（无第三方依赖）
 *
 * 只提供工程里真正用到的两件事：
 *   1. 查询可用 CPU 核数；
 *   2. 把 count 个互相独立的任务分给最多 threads 个线程执行。
 *
 * 设计取舍：不使用 OpenMP。原因是 MinGW 的 -fopenmp 会引入 libgomp 运行时
 * DLL，分发 exe 时容易缺文件；这里直接用 Win32 CreateThread / POSIX pthread，
 * 任务领取用 GCC 的 __atomic_fetch_add（不需要额外库），编译即用、零依赖。
 * ===================================================================== */
#ifndef LI_THREAD_H
#define LI_THREAD_H

/* 可用 CPU 核数，至少返回 1 */
int li_cpu_count(void);

/* 并行执行 count 个任务，任务号范围 [0, count)。
 *   threads <= 1 或 count <= 1   -> 退化为顺序执行（保证单线程路径与原行为一致）
 * 任务之间必须互不共享可写内存（每个任务只写自己那份结果）。
 * 返回 0 表示成功。 */
int li_parallel_for(int threads, int count,
                    void (*worker)(void *ctx, int task), void *ctx);

#endif /* LI_THREAD_H */
