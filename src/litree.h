/* =====================================================================
 * litree.h —— LI-Tree（截断式近似 CDF 索引）
 *
 * 结构：每个节点负责一段连续有序区间 [start, end]（闭区间），
 *   1) 先求首尾字符串的公共前缀长度 start_bit —— 这段所有键都相同，跳过；
 *   2) 再从 start_bit 起取 end_bit - start_bit 个字符，按 94 进制编码成整数，
 *      把区间值域 [start_num, end_num] 划成 whole_size 个桶（每桶宽 division）；
 *   3) cdf[b] 记录"桶 b 之前的元素个数"，于是给定 key 的桶号 b，
 *      就能 O(1) 得到候选区间 [cdf[b]+start, cdf[b+1]-1+start]；
 *   4) 桶内元素仍然过密时递归建子节点（下一层用更长的截断前缀区分）。
 *
 * 两套方案（对应原仓库的两个 zip）：
 *   LI_SCHEME_FIXED_CDF  —— 方案一：division = cap / 平均每单位值的元素数，
 *                           目标是让 CDF 长度稳定在 cap 量级、尽量只建一层；
 *   LI_SCHEME_DENSE_CDF  —— 方案二：division = 1，CDF 按值域完全展开（稀疏），
 *                           每层只截断 LI_HYPER_BIT 个字符，稠密阈值动态计算。
 * ===================================================================== */
#ifndef LI_LITREE_H
#define LI_LITREE_H

#include <stddef.h>

#include "common.h"

typedef enum {
    LI_SCHEME_FIXED_CDF = 1, /* 方案一：定长 CDF */
    LI_SCHEME_DENSE_CDF = 2  /* 方案二：值域展开的稀疏 CDF */
} LiScheme;

typedef struct LiNode {
    int start_bit;   /* 公共前缀长度（不含在数值编码内） */
    int end_bit;     /* 截断右端（开区间），编码区间为 [start_bit, end_bit) */
    int start;       /* 负责的区间左端（含） */
    int end;         /* 负责的区间右端（含） */
    long long start_num; /* 截断前缀编码的区间下界（查询只需下界） */
    int whole_size;      /* 桶数 = CDF 长度 - 1 */
    int child_num;       /* 子节点个数 */
    int division;        /* 每个桶覆盖的值域宽度 */
    int *cdf;            /* whole_size + 1 个前缀和 */
    int *index_node;     /* whole_size 个：桶 b 对应的子节点编号（0 表示无） */
    struct LiNode *children; /* child_num 个连续子节点（第 i 个对应编号 i+1） */
} LiNode;

typedef struct {
    int scheme;  /* LiScheme */
    int cap;     /* 桶内目标元素数，默认 LI_DEFAULT_CAP */
    int dense;   /* 稠密判定系数 lambda，默认 LI_LAMBDA */
    int threads; /* 建索引时用于并行直方图的线程数；<=1 表示串行（默认 1） */
} LiTreeConfig;

typedef struct {
    LiNode root;
    LiTreeConfig cfg;
    int count;        /* 索引覆盖的元素总数 */
    int node_count;   /* 节点总数 */
    size_t memory;    /* 索引有效负载字节数（节点 + 数组） */
    size_t reserved;  /* 向系统申请的总字节数（含块头与对齐开销） */
    void *arena;      /* 内部区域分配器，不对外暴露 */
} LiTree;

/* 构建索引；row 必须已按 strcmp 升序排列，成功返回 0 */
int li_litree_build(LiTree *tree, char *const *row, int n, const LiTreeConfig *cfg);

/* 释放索引占用的全部内存（O(1)：一次遍历块链表释放） */
void li_litree_free(LiTree *tree);

/* 查询；返回命中位置与桶内二分的 strcmp 次数 */
SearchResult li_litree_search(const LiTree *tree, char *const *row, const char *key);

/* 适配 LiSearchFn 签名的包装（ctx 为 LiTree*） */
SearchResult li_litree_search_fn(void *ctx, char *const *row, int n, const char *key);

#endif /* LI_LITREE_H */
