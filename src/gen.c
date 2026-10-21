/* =====================================================================
 * gen.c —— li-gen：生成模拟数据集
 *
 * 对应原版 data_generate.c。原版有两个毛病：
 *   1. 输出文件名写死成 Datatest1.txt，换数据集只能改源码重编译；
 *   2. 用 ftime() + rand() 播种，每次运行的数据都不同，实验无法复现。
 * 这里改为命令行指定文件名 / 条数 / 分布 / 随机种子，并且顺带做数据体检。
 * ===================================================================== */
#include <stdio.h>
#include <string.h>

#include "common.h"
#include "dataset.h"
#include "timer.h"

static void usage(void)
{
    printf("用法: li-gen --out <文件> [--count N] [--dist 名称] [--seed S] [--check]\n"
           "\n"
           "  --out    输出文件路径（每行一个字符串，按字典序升序）\n"
           "  --count  生成条数，默认 5000000\n"
           "  --dist   分布类型: uniform | nd | nd2 | normal | mix（默认 nd）\n"
           "  --seed   随机种子，默认 20221126（同种子必得同一份数据）\n"
           "  --check  生成后校验有序性与字符范围\n"
           "\n"
           "分布说明:\n"
           "  uniform  94 个可打印字符均匀随机\n"
           "  nd       首字符有尖峰（原 genrndnd）\n"
           "  nd2      双尖峰（原 genrndnd2）\n"
           "  normal   前两位由正态数决定（原 genrndnp）\n"
           "  mix      混合高斯双峰（原 genrndmg）\n");
}

int main(int argc, char **argv)
{
    const char *out = NULL;
    const char *dist_name = "nd";
    LiDist dist = LI_DIST_ND;
    long long count = 5000000;
    unsigned long long seed = 20221126ULL;
    int do_check = 0;
    LiDataset ds;
    double t0;
    double t1;
    int rc;

    li_console_utf8();

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            if (li_parse_int(argv[++i], &count) != 0 || count <= 0) {
                fprintf(stderr, "错误的 --count\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--dist") == 0 && i + 1 < argc) {
            dist_name = argv[++i];
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            long long s;
            if (li_parse_int(argv[++i], &s) != 0) {
                fprintf(stderr, "错误的 --seed\n");
                return 2;
            }
            seed = (unsigned long long)s;
        } else if (strcmp(argv[i], "--check") == 0) {
            do_check = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        } else {
            fprintf(stderr, "未知参数: %s\n", argv[i]);
            usage();
            return 2;
        }
    }
    if (out == NULL) {
        fprintf(stderr, "缺少 --out\n");
        usage();
        return 2;
    }
    if (li_dist_parse(dist_name, &dist) != 0) {
        fprintf(stderr, "未知分布: %s\n", dist_name);
        return 2;
    }

    t0 = li_now_seconds();
    rc = li_dataset_generate(&ds, (int)count, dist, seed);
    if (rc != 0) {
        fprintf(stderr, "生成失败（内存不足？共需约 %.1f MiB）\n",
                (double)count * (double)LI_STRIDE / 1048576.0);
        return 1;
    }
    t1 = li_now_seconds();

    printf("生成完成\n");
    printf("  条数      : %d\n", ds.count);
    printf("  分布      : %s\n", li_dist_name(dist));
    printf("  种子      : %llu\n", seed);
    printf("  槽宽      : %d 字节\n", ds.stride);
    printf("  平均长度  : %.1f 字符\n", li_dataset_mean_length(&ds));
    printf("  生成+排序 : %.3f s\n", t1 - t0);

    if (do_check) {
        int unsorted = li_dataset_is_sorted(&ds);
        int bad = li_dataset_bad_chars(&ds);
        printf("  有序性    : %s\n", (unsorted == 0) ? "通过" : "失败");
        printf("  字符范围  : %s\n", (bad == 0) ? "通过（全部落在 '!'~'~'）" : "存在越界字符");
        if (unsorted != 0 || bad != 0) {
            li_dataset_free(&ds);
            return 1;
        }
    }

    t0 = li_now_seconds();
    if (li_dataset_save(&ds, out) != 0) {
        fprintf(stderr, "写文件失败: %s\n", out);
        li_dataset_free(&ds);
        return 1;
    }
    t1 = li_now_seconds();
    printf("  已写入    : %s（%.3f s）\n", out, t1 - t0);

    li_dataset_free(&ds);
    return 0;
}
