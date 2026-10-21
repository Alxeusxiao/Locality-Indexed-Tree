/* =====================================================================
 * searcher.h —— 四种经典字符串查找算法（对照基线）
 *
 * 原版的四种算法分散在 4 个文件里，且 test_single.c 版本把 printf 写进了
 * 查找热路径（既拖慢计时又混入输出）；这里统一成纯函数：
 * 输入已排序的行指针数组，返回 {位置, strcmp 比较次数}。
 * ===================================================================== */
#ifndef LI_SEARCHER_H
#define LI_SEARCHER_H

#include "common.h"

/* 统一的查找函数签名：ctx 供 LI-Tree 传入索引对象，其余算法传 NULL */
typedef SearchResult (*LiSearchFn)(void *ctx, char *const *row, int n, const char *key);

/* 标准二分查找 */
SearchResult li_search_binary(char *const *row, int n, const char *key);

/* 插值查找：先用前 LI_INC_I 个字符编码成数值做插值，
 * 区间小于 LI_THRESHOLD_I 后用二分收尾（原 Interpolation_Search） */
SearchResult li_search_interpolation(char *const *row, int n, const char *key);

/* 简易插值：从插值落点开始线性扫描（原 Interpolation_Search_low） */
SearchResult li_search_interpolation_low(char *const *row, int n, const char *key);

/* 割线查找：用割线迭代逼近期望位置，
 * 区间小于 LI_THRESHOLD_C 后用二分收尾（原 Cut_Search） */
SearchResult li_search_cut(char *const *row, int n, const char *key);

/* 算法表：名称 + 函数指针 + 是否需要索引上下文 */
typedef struct {
    const char *name;
    LiSearchFn fn;
    int needs_index; /* 1 = 需要 LI-Tree 上下文 */
} LiAlgo;

extern const LiAlgo LI_ALGOS[];
extern const int LI_ALGO_COUNT;

#endif /* LI_SEARCHER_H */
