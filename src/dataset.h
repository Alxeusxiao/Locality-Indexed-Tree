/* =====================================================================
 * dataset.h —— 数据集：生成、读写、校验
 *
 * 原版把 char str[num][len] 直接开在 main 的栈上（500 万条 ≈ 640 MB），
 * 一旦编译器没给大栈就会立刻崩溃；这里统一走堆上的连续块 + 行指针数组。
 * ===================================================================== */
#ifndef LI_DATASET_H
#define LI_DATASET_H

#include <stddef.h>

/* 数据分布类型，与原版 data_generate.c 中的 5 个生成函数一一对应 */
typedef enum {
    LI_DIST_UNIFORM = 0, /* 均匀分布          (genrnd)     */
    LI_DIST_ND,          /* 首位有尖峰        (genrndnd)   */
    LI_DIST_ND2,         /* 双尖峰            (genrndnd2)  */
    LI_DIST_NORMAL,      /* 正态分布          (genrndnp)   */
    LI_DIST_MIX,         /* 混合高斯（双峰）  (genrndmg)   */
    LI_DIST_COUNT
} LiDist;

typedef struct {
    char *buf;   /* capacity * stride 的连续字符块（已清零，尾部补 '\0'） */
    char **row;  /* capacity 个行指针，第 i 个指向 buf + i * stride */
    int count;   /* 实际有效行数 */
    int capacity;
    int stride;
} LiDataset;

/* ---- 生命周期 ---- */
int li_dataset_alloc(LiDataset *ds, int capacity, int stride);
void li_dataset_free(LiDataset *ds);

/* ---- 读写 ---- */
/* 从文本文件读取最多 capacity 行（自动兼容 CRLF / LF），成功返回 0 */
int li_dataset_load(LiDataset *ds, const char *path, int capacity);
int li_dataset_save(const LiDataset *ds, const char *path);

/* ---- 生成 ---- */
int li_dataset_generate(LiDataset *ds, int count, LiDist dist, unsigned long long seed);

/* ---- 校验 ---- */
/* 检查是否按 strcmp 升序；返回 0 表示有序 */
int li_dataset_is_sorted(const LiDataset *ds);
/* 检查所有字符是否都在 '!'~'~'；返回越界字符个数（正常为 0） */
int li_dataset_bad_chars(const LiDataset *ds);
/* 统计平均字符串长度 */
double li_dataset_mean_length(const LiDataset *ds);

/* ---- 名称映射（给命令行用） ---- */
const char *li_dist_name(LiDist dist);
int li_dist_parse(const char *name, LiDist *out);

/* ---- 可复现的伪随机数（xorshift64*）----
 * 原版用 rand() + ftime() 播种，同一份代码两次运行的数据集不同，
 * 实验无法复现；这里改成显式种子的 xorshift64*。 */
void li_rng_seed(unsigned long long seed);
unsigned long long li_rng_next(void);
int li_rng_below(int n);

#endif /* LI_DATASET_H */
