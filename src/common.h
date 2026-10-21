/* =====================================================================
 * common.h —— 全工程共享的常量、类型与内联工具
 *
 * LI-Tree 实验工程（原 li-tree 实验代码重构版）
 * ===================================================================== */
#ifndef LI_COMMON_H
#define LI_COMMON_H

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------
 * 基本常量
 * ------------------------------------------------------------------- */

/* 每条字符串的固定槽宽（含结尾 '\0'），与原版宏 len 一致。
 * 原版用 char str[num][len] 的二维数组，这里改为等宽的连续块 + 行指针，
 * 既能放在堆上，也能让行排序只交换指针而不再搬 128 字节。 */
#define LI_STRIDE 128

/* 字符集：可打印 ASCII '!'(33) ~ '~'(126)，共 94 个，
 * 即以 94 为进制基数把字符串前缀编码成整数。 */
#define LI_RADIX 94
#define LI_BASE 33

/* 索引构建的默认参数（原版是编译期宏，这里改成运行时可变） */
#define LI_DEFAULT_CAP 150 /* 目标：单个 CDF 桶内的平均元素数 */
#define LI_HYPER_BIT 2     /* 初始截断的字符数 */
#define LI_LAMBDA 3        /* 稠密桶判定系数：桶内元素 > LAMBDA * 阈值 才继续分子节点 */
#define LI_MIN_THRESHOLD 20 /* 稠密阈值下限，避免浅层过度切分 */

/* 安全护栏 */
#define LI_MAX_CDF (1 << 22) /* 单节点 CDF 长度上限，超了自动加大 division */
#define LI_MAX_DEPTH 64      /* 递归深度上限 */
#define LI_MAX_TRUNC 9       /* 截断字符数上限：94^9 仍在 int64 范围内 */

/* 高阶查找算法的参数（原版为同名的编译期宏） */
#define LI_THRESHOLD_I 100 /* 插值法转入二分的区间阈值 */
#define LI_THRESHOLD_C 100 /* 割线法转入二分的区间阈值 */
#define LI_INC_I 2         /* 插值法的初始截断字符数 */
#define LI_INC_C 3         /* 割线法的初始截断字符数 */

#define LI_PI 3.14159265358979323846

/* ---------------------------------------------------------------------
 * 查找结果：原版有的函数返回位置、有的只返回比较次数，无法统一比较。
 * 这里统一为「位置 + strcmp 比较次数」。
 * ------------------------------------------------------------------- */
typedef struct {
    int position;    /* 命中下标；-1 表示未找到 */
    int comparisons; /* 本次查找中 strcmp 的调用次数 */
} SearchResult;

static inline SearchResult sr_make(int position, int comparisons)
{
    SearchResult r;
    r.position = position;
    r.comparisons = comparisons;
    return r;
}

/* 带计数的字符串比较：计数口径 = strcmp 调用次数 */
static inline int li_cmp(const char *a, const char *b, int *counter)
{
    (*counter)++;
    return strcmp(a, b);
}

/* 单个字符的数值：越界或结束符按最小字符 '!'（即 0）处理，
 * 既保证不会读越界，也保证编码后的数值仍与字典序一致。 */
static inline int li_digit(int ch)
{
    int d;
    if (ch < LI_BASE) {
        return 0; /* '\0' 或非法控制字符：补最小位 */
    }
    d = ch - LI_BASE;
    if (d >= LI_RADIX) {
        d = LI_RADIX - 1; /* 罕见：超出 '~' 的字符截到最大位 */
    }
    return d;
}

/* 把 s[tran, tran+inc) 这段前缀按 94 进制编码成整数。
 * 原版直接用 int 且不检查 '\0'，既可能溢出也可能读越界；这里用 int64，
 * 并在越界处补 0 位。inc 为 0 时返回 0（表示"该节点不需要数值定位"）。 */
static inline long long li_encode(const char *s, int tran, int inc)
{
    long long v = 0;
    int j;
    if (inc <= 0 || tran < 0) {
        return 0;
    }
    for (j = 0; j < inc; ++j) {
        int pos = tran + j;
        int ch = (pos < LI_STRIDE) ? (unsigned char)s[pos] : 0;
        v = v * LI_RADIX + li_digit(ch);
    }
    return v;
}

/* 解析十进制整数，成功返回 0 */
static inline int li_parse_int(const char *s, long long *out)
{
    char *end = NULL;
    long long v;
    if (s == NULL || *s == '\0') {
        return -1;
    }
    v = strtoll(s, &end, 10);
    if (end == s || *end != '\0') {
        return -1;
    }
    *out = v;
    return 0;
}

#endif /* LI_COMMON_H */
