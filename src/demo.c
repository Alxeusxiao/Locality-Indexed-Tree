/* =====================================================================
 * demo.c —— li-demo：单次演示（对应原版 test_single.c / test_single_m2.c）
 *
 * 原版 test_single.c 把 printf 写在每个查找函数的循环里，既污染热路径又
 * 让"位置"和"比较次数"混在一起打印（ACDF 那行其实打印的是比较次数）。
 * 这里统一为：查找函数只返回结果，由本程序负责排版输出。
 *
 * 输出三类查询：
 *   1) 随机命中键；2) 随机不存在的键；3) 最大键（原版漏掉的那一个）。
 * ===================================================================== */
#include <stdio.h>
#include <string.h>

#include "common.h"
#include "dataset.h"
#include "litree.h"
#include "searcher.h"
#include "thread.h"
#include "timer.h"

static void usage(void)
{
    printf("用法: li-demo --data <文件> [--keys N] [--scheme 1|2] [--cap C] [--threads T] [--seed S]\n"
           "\n"
           "  --data    数据文件（每行一个字符串，需按字典序升序）\n"
           "  --keys    参与建索引的条数，默认 1000000\n"
           "  --scheme  1 = 定长 CDF（方案一）；2 = 展开式稀疏 CDF（方案二）\n"
           "  --cap     方案一的桶内目标元素数，默认 %d\n"
           "  --threads 建索引时的并行线程数，默认 1；0 = 用满全部核（本机 %d）\n"
           "  --seed    随机种子，默认 12345\n",
           LI_DEFAULT_CAP, li_cpu_count());
}

static void print_row(const char *name, SearchResult r, int expect)
{
    const char *mark = "";

    if (r.position < 0) {
        mark = (expect < 0) ? "  [正确:未命中]" : "  [异常:未命中]";
    } else if (expect < 0) {
        mark = "  [异常:误报命中]";
    } else {
        mark = "  [正确]";
    }
    printf("  %-20s 位置 %10d   比较 %3d 次%s\n", name, r.position, r.comparisons, mark);
}

static void run_all(const LiAlgo *algos, void **ctxs, char *const *row, int n,
                    const char *key, int expect)
{
    for (int a = 0; a < LI_ALGO_COUNT; ++a) {
        SearchResult r = algos[a].fn(ctxs[a], row, n, key);
        print_row(algos[a].name, r, expect);
    }
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    long long keys = 1000000;
    long long cap = LI_DEFAULT_CAP;
    long long threads = 1;
    int scheme = 1;
    unsigned long long seed = 12345ULL;
    LiDataset ds;
    LiTree tree;
    LiTreeConfig cfg;
    void *ctxs[8];
    char miss[LI_STRIDE];
    double t0;
    double t1;
    int n;
    int idx;
    int rc;

    li_console_utf8();

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            path = argv[++i];
        } else if (strcmp(argv[i], "--keys") == 0 && i + 1 < argc) {
            if (li_parse_int(argv[++i], &keys) != 0 || keys <= 0) {
                fprintf(stderr, "错误的 --keys\n");
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
        threads = li_cpu_count();
    }
    if (threads > 64) {
        threads = 64;
    }

    if (li_dataset_load(&ds, path, (int)keys) != 0) {
        fprintf(stderr, "读取失败: %s\n", path);
        return 1;
    }
    n = ds.count;
    if (n <= 0) {
        fprintf(stderr, "数据集为空\n");
        li_dataset_free(&ds);
        return 1;
    }

    printf("=== LI-Tree 单次演示 ===\n");
    printf("数据文件 : %s\n", path);
    printf("键数量   : %d\n", n);
    printf("平均长度 : %.1f 字符\n", li_dataset_mean_length(&ds));
    printf("有序性   : %s\n", (li_dataset_is_sorted(&ds) == 0) ? "通过" : "失败（结果不可信）");
    printf("方案     : %d（%s）\n", scheme,
           (scheme == 1) ? "定长 CDF" : "展开式稀疏 CDF");
    printf("线程数   : %d%s\n", (int)threads, (threads > 1) ? "（建索引直方图并行）" : "");

    cfg.scheme = scheme;
    cfg.cap = (int)cap;
    cfg.dense = LI_LAMBDA;
    cfg.threads = (int)threads;
    t0 = li_now_seconds();
    rc = li_litree_build(&tree, ds.row, n, &cfg);
    t1 = li_now_seconds();
    if (rc != 0) {
        fprintf(stderr, "索引构建失败\n");
        li_dataset_free(&ds);
        return 1;
    }
    printf("建索引   : %.3f s，%d 个节点，有效负载 %.1f KiB（分配 %.2f MiB）\n",
           t1 - t0, tree.node_count, (double)tree.memory / 1024.0,
           (double)tree.reserved / 1048576.0);
    printf("根节点   : 桶数 %d，公共前缀 %d 字符，截断 [%d,%d)\n",
           tree.root.whole_size, tree.root.start_bit, tree.root.start_bit, tree.root.end_bit);

    for (int a = 0; a < LI_ALGO_COUNT && a < 8; ++a) {
        ctxs[a] = LI_ALGOS[a].needs_index ? (void *)&tree : NULL;
    }

    li_rng_seed(seed);

    /* ---- 1. 命中：随机挑一个已有的键 ---- */
    idx = li_rng_below(n);
    printf("\n[命中] 下标 %d，key = %.60s%s\n", idx, ds.row[idx],
           (strlen(ds.row[idx]) > 60) ? "..." : "");
    run_all(LI_ALGOS, ctxs, ds.row, n, ds.row[idx], idx);

    /* ---- 2. 未命中：随机造一个大概率不存在的键 ---- */
    {
        int len = li_rng_below(LI_STRIDE - 32) + 32;
        for (int i = 0; i < len - 1; ++i) {
            miss[i] = (char)(li_rng_below(LI_RADIX) + LI_BASE);
        }
        miss[len - 1] = '\0';
    }
    {
        /* 先确认它确实不在数据里，否则换个说法重新演示 */
        int exists = (li_search_binary(ds.row, n, miss).position >= 0);
        printf("\n[未命中] key = %.60s%s%s\n", miss, (strlen(miss) > 60) ? "..." : "",
               exists ? "（意外存在，仅作演示）" : "");
        run_all(LI_ALGOS, ctxs, ds.row, n, miss, exists ? -2 : -1);
    }

    /* ---- 3. 最大键：原版漏掉的那一个，专门做回归验证 ---- */
    printf("\n[边界] 最大键，下标 %d，key = %.60s%s\n", n - 1, ds.row[n - 1],
           (strlen(ds.row[n - 1]) > 60) ? "..." : "");
    run_all(LI_ALGOS, ctxs, ds.row, n, ds.row[n - 1], n - 1);

    li_litree_free(&tree);
    li_dataset_free(&ds);
    return 0;
}
