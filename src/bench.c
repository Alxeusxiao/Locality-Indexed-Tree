/* =====================================================================
 * bench.c —— li-bench：主实验（对应原版 test_numbers.c / test_numbers_m2.c）
 *
 * 原版的做法是：每查一次就 clock() 两次，把 CPU 时间逐个累加。
 * 问题有三：clock() 分辨率只到毫秒级、计时调用本身的开销与查找同量级、
 * 而且只返回比较次数，无法发现"查错了但次数正常"。
 *
 * 这里改为：
 *   1. 整段查询循环统一计时，再除以次数（QPC 提供亚微秒精度）；
 *   2. 每算一次都比较返回位置上的字符串是否真的等于 key，统计错答；
 *   3. 命中与未命中两组用同一批随机种子，保证各算法面对的查询序列一致；
 *   4. 缺失键提前批量生成，不让随机串生成开销混进查找耗时；
 *   5. 查询循环可按 --threads 并行。各线程使用独立的随机流（task 0 沿用
 *      全局种子），因此 --threads 1 的结果与原单线程版本逐位一致。
 * ===================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "dataset.h"
#include "litree.h"
#include "searcher.h"
#include "thread.h"
#include "timer.h"

typedef struct {
    double us_per_query; /* 单次查询平均墙钟耗时（微秒） */
    double avg_cmp;      /* 单次查询平均 strcmp 次数 */
    long long bad;       /* 错答次数 */
} Stat;

/* ---------------------------------------------------------------------
 * 一个查询线程要做的全部事情
 * ------------------------------------------------------------------- */
typedef struct {
    LiSearchFn fn;
    void *ctx;
    char *const *row;
    int n;
    int miss;           /* 0 = 命中组，1 = 未命中组 */
    long long queries;  /* 全局查询总数 */
    long long per_task; /* 每个任务分到的查询数 */
    unsigned long long seed;
    char **missrow;
    int miss_n;
    long long *sum_cmp; /* 每个任务的输出：比较次数小计 */
    long long *bad;     /* 每个任务的输出：错答次数 */
} QueryJob;

static void query_worker(void *ctx, int task)
{
    QueryJob *q = (QueryJob *)ctx;
    long long from = (long long)task * q->per_task;
    long long to = from + q->per_task;
    long long sum = 0;
    long long bad = 0;
    long long k;

    if (from >= q->queries) {
        q->sum_cmp[task] = 0;
        q->bad[task] = 0;
        return;
    }
    if (to > q->queries) {
        to = q->queries;
    }
    /* task 0 沿用全局种子：threads=1 时与单线程版本完全同序列 */
    li_rng_seed((task == 0) ? q->seed
                            : (q->seed ^ (0x9E3779B97F4A7C15ULL * (unsigned long long)task)));

    if (q->miss == 0) {
        for (k = from; k < to; ++k) {
            int idx = li_rng_below(q->n);
            SearchResult r = q->fn(q->ctx, q->row, q->n, q->row[idx]);
            sum += r.comparisons;
            if (r.position < 0) {
                bad++;
            } else if (strcmp(q->row[r.position], q->row[idx]) != 0) {
                bad++;
            }
        }
    } else {
        for (k = from; k < to; ++k) {
            const char *key = q->missrow[k % q->miss_n];
            SearchResult r = q->fn(q->ctx, q->row, q->n, key);
            sum += r.comparisons;
            if (r.position >= 0) {
                bad++; /* 误报命中 */
            }
        }
    }
    q->sum_cmp[task] = sum;
    q->bad[task] = bad;
}

/* 跑完一组查询（命中或未命中），把结果写进 out[] */
static void run_group(Stat *out, void **ctxs, const LiDataset *ds, int n,
                      long long queries, int threads, unsigned long long seed,
                      char **missrow, int miss_n, int miss)
{
    int tasks = (queries < (long long)threads) ? (int)queries : threads;
    long long per_task = (queries + tasks - 1) / tasks;
    long long *sum_cmp = (long long *)calloc((size_t)tasks, sizeof(long long));
    long long *bad = (long long *)calloc((size_t)tasks, sizeof(long long));

    if (sum_cmp == NULL || bad == NULL) {
        free(sum_cmp);
        free(bad);
        fprintf(stderr, "内存不足\n");
        return;
    }

    for (int a = 0; a < LI_ALGO_COUNT; ++a) {
        QueryJob job;
        double t0;
        double t1;
        long long total_cmp = 0;
        long long total_bad = 0;

        job.fn = LI_ALGOS[a].fn;
        job.ctx = ctxs[a];
        job.row = ds->row;
        job.n = n;
        job.miss = miss;
        job.queries = queries;
        job.per_task = per_task;
        job.seed = seed;
        job.missrow = missrow;
        job.miss_n = miss_n;
        job.sum_cmp = sum_cmp;
        job.bad = bad;

        /* 预热：避免首次缺页与页表建立进入计时 */
        (void)job.fn(job.ctx, job.row, job.n, (miss == 0) ? job.row[0] : job.missrow[0]);

        t0 = li_now_seconds();
        li_parallel_for(tasks, tasks, query_worker, &job);
        t1 = li_now_seconds();

        for (int t = 0; t < tasks; ++t) {
            total_cmp += sum_cmp[t];
            total_bad += bad[t];
            sum_cmp[t] = 0;
            bad[t] = 0;
        }
        out[a].us_per_query = (t1 - t0) * 1e6 / (double)queries;
        out[a].avg_cmp = (double)total_cmp / (double)queries;
        out[a].bad = total_bad;
    }
    free(sum_cmp);
    free(bad);
}

static void print_stat_line(const char *name, const Stat *s, double base_us)
{
    double rel = (base_us > 0.0) ? s->us_per_query / base_us : 0.0;

    printf("  %-20s %12.4f %16.4f %12.2fx %10lld\n",
           name, s->avg_cmp, s->us_per_query, rel, s->bad);
}

static void usage(void)
{
    printf("用法: li-bench --data <文件> [选项]\n"
           "\n"
           "  --data     数据文件（每行一个字符串，需按字典序升序）\n"
           "  --keys     载入并建索引的条数，默认 1000000\n"
           "  --queries  每组查询次数，默认 1000000\n"
           "  --scheme   1 = 定长 CDF（方案一，默认）；2 = 展开式稀疏 CDF（方案二）\n"
           "  --cap      方案一的桶内目标元素数，默认 %d\n"
           "  --repeat   重复建索引次数（取平均），默认 1\n"
           "  --threads  并行线程数，默认 1；0 = 用满全部核（本机 %d）\n"
           "  --seed     随机种子，默认 12345\n"
           "\n"
           "说明：--threads 1 的结果与单线程版本逐位一致（task 0 沿用全局随机流）；\n"
           "      多线程时每个线程使用独立的随机流，平均比较次数波动约 ±0.2%%。\n",
           LI_DEFAULT_CAP, li_cpu_count());
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    long long keys = 1000000;
    long long queries = 1000000;
    long long repeat = 1;
    long long cap = LI_DEFAULT_CAP;
    long long threads = 1;
    int scheme = 1;
    unsigned long long seed = 12345ULL;
    LiDataset ds;
    LiTree tree;
    LiTreeConfig cfg;
    void *ctxs[8];
    Stat hit[8];
    Stat miss[8];
    char *missbuf = NULL;
    char **missrow = NULL;
    int miss_n;
    int n;
    int i;
    int a;
    double t0;
    double t1;
    double build_us = 0.0;
    long long total_bad = 0;

    li_console_utf8();
    memset(&ds, 0, sizeof ds);
    memset(&tree, 0, sizeof tree);
    memset(hit, 0, sizeof hit);
    memset(miss, 0, sizeof miss);

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            path = argv[++i];
        } else if (strcmp(argv[i], "--keys") == 0 && i + 1 < argc) {
            if (li_parse_int(argv[++i], &keys) != 0 || keys <= 0) {
                fprintf(stderr, "错误的 --keys\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--queries") == 0 && i + 1 < argc) {
            if (li_parse_int(argv[++i], &queries) != 0 || queries <= 0) {
                fprintf(stderr, "错误的 --queries\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--scheme") == 0 && i + 1 < argc) {
            long long v;
            if (li_parse_int(argv[++i], &v) != 0 || (v != 1 && v != 2)) {
                fprintf(stderr, "错误的 --scheme（只支持 1 或 2）\n");
                return 2;
            }
            scheme = (int)v;
        } else if (strcmp(argv[i], "--cap") == 0 && i + 1 < argc) {
            if (li_parse_int(argv[++i], &cap) != 0 || cap < 1) {
                fprintf(stderr, "错误的 --cap\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) {
            if (li_parse_int(argv[++i], &repeat) != 0 || repeat < 1) {
                fprintf(stderr, "错误的 --repeat\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            if (li_parse_int(argv[++i], &threads) != 0 || threads < 0) {
                fprintf(stderr, "错误的 --threads\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            long long v;
            if (li_parse_int(argv[++i], &v) != 0) {
                fprintf(stderr, "错误的 --seed\n");
                return 2;
            }
            seed = (unsigned long long)v;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        } else {
            fprintf(stderr, "未知参数: %s\n", argv[i]);
            usage();
            return 2;
        }
    }
    if (path == NULL) {
        fprintf(stderr, "缺少 --data\n");
        usage();
        return 2;
    }
    if (threads == 0) {
        threads = li_cpu_count(); /* 0 = 自动用满全部核 */
    }
    if (threads > 64) {
        threads = 64;
    }

    /* ---- 1. 载入数据 ---- */
    t0 = li_now_seconds();
    if (li_dataset_load(&ds, path, (int)keys) != 0) {
        fprintf(stderr, "读取失败: %s（可能是内存不足）\n", path);
        return 1;
    }
    t1 = li_now_seconds();
    n = ds.count;
    if (n <= 0) {
        fprintf(stderr, "数据集为空\n");
        li_dataset_free(&ds);
        return 1;
    }

    printf("=== LI-Tree 对比实验 ===\n");
    printf("数据文件   : %s\n", path);
    printf("键数量     : %d%s\n", n,
           ((long long)n < keys) ? "（文件行数不足，已按实际行数实验）" : "");
    printf("平均长度   : %.1f 字符\n", li_dataset_mean_length(&ds));
    printf("载入耗时   : %.3f s\n", t1 - t0);
    printf("有序性     : %s\n",
           (li_dataset_is_sorted(&ds) == 0) ? "通过" : "失败（排序假设被破坏）");
    printf("线程数     : %d%s\n", (int)threads,
           (threads > 1) ? "（多线程：查询循环并行）" : "（单线程）");

    /* ---- 2. 构建索引 ---- */
    cfg.scheme = scheme;
    cfg.cap = (int)cap;
    cfg.dense = LI_LAMBDA;
    cfg.threads = (int)threads;
    t0 = li_now_seconds();
    for (i = 0; i < (int)repeat; ++i) {
        if (li_litree_build(&tree, ds.row, n, &cfg) != 0) {
            fprintf(stderr, "索引构建失败\n");
            li_dataset_free(&ds);
            return 1;
        }
        if (i + 1 < (int)repeat) {
            li_litree_free(&tree);
        }
    }
    t1 = li_now_seconds();
    build_us = (t1 - t0) * 1e6 / (double)repeat;

    printf("方案       : %d（%s）\n", scheme,
           (scheme == 1) ? "定长 CDF" : "展开式稀疏 CDF");
    printf("索引参数   : cap = %d, lambda = %d\n", (int)cap, tree.cfg.dense);
    printf("建索引     : %.3f ms / 次（重复 %d 次）\n", build_us / 1000.0, (int)repeat);
    printf("索引规模   : %d 个节点，有效负载 %.1f KiB（%.1f 字节/键），分配 %.2f MiB\n",
           tree.node_count, (double)tree.memory / 1024.0,
           (double)tree.memory / (double)n,
           (double)tree.reserved / 1048576.0);
    printf("根节点     : 桶数 %d，截断 [%d,%d)\n",
           tree.root.whole_size, tree.root.start_bit, tree.root.end_bit);

    for (a = 0; a < LI_ALGO_COUNT && a < 8; ++a) {
        ctxs[a] = LI_ALGOS[a].needs_index ? (void *)&tree : NULL;
    }

    /* ---- 3. 预生成缺失键（提前做好，不混入计时） ---- */
    miss_n = (queries > 200000) ? 200000 : (int)queries;
    missbuf = (char *)calloc((size_t)miss_n * LI_STRIDE, 1);
    missrow = (char **)calloc((size_t)miss_n, sizeof(char *));
    if (missbuf == NULL || missrow == NULL) {
        fprintf(stderr, "内存不足\n");
        li_litree_free(&tree);
        li_dataset_free(&ds);
        free(missbuf);
        free(missrow);
        return 1;
    }
    li_rng_seed(seed ^ 0x9E3779B97F4A7C15ULL);
    for (i = 0; i < miss_n; ++i) {
        char *key = missbuf + (size_t)i * LI_STRIDE;
        int tries;
        missrow[i] = key;
        for (tries = 0; tries < 8; ++tries) {
            int len = li_rng_below(LI_STRIDE - 32) + 32;
            for (int j = 0; j < len - 1; ++j) {
                key[j] = (char)(li_rng_below(LI_RADIX) + LI_BASE);
            }
            key[len - 1] = '\0';
            if (li_search_binary(ds.row, n, key).position < 0) {
                break; /* 确认不存在 */
            }
        }
    }

    /* ---- 4. 命中组 / 未命中组 ---- */
    run_group(hit, ctxs, &ds, n, queries, (int)threads, seed, missrow, miss_n, 0);
    run_group(miss, ctxs, &ds, n, queries, (int)threads, seed, missrow, miss_n, 1);

    /* ---- 5. 输出 ---- */
    printf("\n命中查询（%lld 次/组，key 一定在数据集中）\n", queries);
    printf("  %-20s %12s %16s %12s %10s\n", "算法", "平均比较次数", "平均耗时(us)", "相对二分", "错答次数");
    for (a = 0; a < LI_ALGO_COUNT; ++a) {
        print_stat_line(LI_ALGOS[a].name, &hit[a], hit[0].us_per_query);
    }

    printf("\n未命中查询（%lld 次/组，key 一定不在数据集中）\n", queries);
    printf("  %-20s %12s %16s %12s %10s\n", "算法", "平均比较次数", "平均耗时(us)", "相对二分", "误报次数");
    for (a = 0; a < LI_ALGO_COUNT; ++a) {
        print_stat_line(LI_ALGOS[a].name, &miss[a], miss[0].us_per_query);
    }

    for (a = 0; a < LI_ALGO_COUNT; ++a) {
        total_bad += hit[a].bad + miss[a].bad;
    }

    printf("\n结论\n");
    {
        int li = -1;
        for (a = 0; a < LI_ALGO_COUNT; ++a) {
            if (LI_ALGOS[a].needs_index) {
                li = a;
            }
        }
        if (li >= 0 && hit[0].us_per_query > 0.0) {
            printf("  LI-Tree 命中耗时是二分的 %.2fx，比较次数是二分的 %.1f%%\n",
                   hit[li].us_per_query / hit[0].us_per_query,
                   100.0 * hit[li].avg_cmp /
                       ((hit[0].avg_cmp > 0.0) ? hit[0].avg_cmp : 1.0));
        }
        if (threads > 1) {
            printf("  多线程吞吐：命中 %.1f 万次/秒，未命中 %.1f 万次/秒（%d 线程）\n",
                   (hit[0].us_per_query > 0.0) ? 1e6 / (hit[0].us_per_query * 1e4) : 0.0,
                   (miss[0].us_per_query > 0.0) ? 1e6 / (miss[0].us_per_query * 1e4) : 0.0,
                   (int)threads);
        }
    }
    if (total_bad != 0) {
        printf("  [警告] 共 %lld 次错答，结果不可信\n", total_bad);
    } else {
        printf("  所有算法均无错答（每个返回位置上的字符串都与 key 相同）\n");
    }

    li_litree_free(&tree);
    li_dataset_free(&ds);
    free(missbuf);
    free(missrow);
    return (total_bad != 0) ? 1 : 0;
}
