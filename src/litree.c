/* =====================================================================
 * litree.c —— LI-Tree 索引的构建、查询与内存管理
 *
 * 相对原版的工程化改动：
 *   1. 区域分配器（arena）：原版每个节点、每个 int 数组都单独 malloc，
 *      一次构建可能上万个 malloc，释放还要递归 free；改成按 4 MiB 分块的
 *      指针递增分配，构建只做指针加法，释放一次遍历块链表即完成；
 *   2. 子节点改为"连续数组 + 下标"：原版是 Index **Nodes 二重指针，
 *      指针跳转分散；现在 child_num 个子节点在同一块内存里，局部性更好；
 *   3. 去掉 start_positions / end_positions：原版自己也在 PPT 里承认这是
 *      冗余信息，且构建时先 malloc 再覆盖，属于内存泄漏；
 *   4. 直方图直接累加在 cdf[b+1] 上再做前缀和，省掉原版的 pdfs[] VLA
 *      （方案二下 whole_size 可能上千，VLA 会撑爆栈）；
 *   5. 修正区间闭合的 off-by-one：原版统计 pdfs 时写 i < end，漏掉了区间
 *      最后一个元素，导致每个节点最大的那个键永远查不到；
 *   6. 退化分支补齐 start_num / division / end_bit 的初始化（原版是未初始化
 *      的栈内存，查询时算出的桶号是随机值）；
 *   7. 走桶时对下标做夹逼，避免超长/超短字符串造成堆越界写。
 * ===================================================================== */
#include "litree.h"

#include <stdlib.h>
#include <string.h>

#include "thread.h"

/* ---------------------------------------------------------------------
 * 并行直方图的参数
 * ------------------------------------------------------------------- */

/* 元素数低于此阈值时不并行：线程创建与合并的开销会盖过收益 */
#define LI_PAR_MIN_ELEMS 200000
/* 局部直方图的总内存上限，超了就减少分块数（否则 whole_size 很大时会爆内存） */
#define LI_PAR_MAX_BYTES (32u << 20)
/* 合并次数上限，避免线程数异常时行为不可控 */
#define LI_PAR_MAX_CHUNKS 64

/* =====================================================================
 * 一、区域分配器：4 MiB 一块，块内指针递增，块之间单链表
 * ===================================================================== */
#define ARENA_ALIGN 16u
#define ARENA_BLOCK (4u << 20)
#define ARENA_HDR ((sizeof(ArenaBlock) + (ARENA_ALIGN - 1)) & ~(size_t)(ARENA_ALIGN - 1))

typedef struct ArenaBlock {
    struct ArenaBlock *next;
    size_t cap;
    size_t used;
} ArenaBlock;

typedef struct {
    ArenaBlock *head;
    size_t total; /* 已向系统申请的字节数 */
    size_t used;  /* 实际分出去的有效负载字节数 */
} Arena;

static void *arena_alloc(Arena *a, size_t bytes)
{
    ArenaBlock *b;
    size_t need;
    size_t block;

    need = (bytes + (ARENA_ALIGN - 1)) & ~(size_t)(ARENA_ALIGN - 1);
    if (need == 0) {
        need = ARENA_ALIGN;
    }
    b = a->head;
    if (b == NULL || (b->cap - b->used) < need) {
        ArenaBlock *nb;
        block = (need > ARENA_BLOCK) ? need : (size_t)ARENA_BLOCK;
        nb = (ArenaBlock *)malloc(ARENA_HDR + block);
        if (nb == NULL) {
            return NULL;
        }
        nb->next = a->head;
        nb->cap = block;
        nb->used = 0;
        a->head = nb;
        a->total += ARENA_HDR + block;
        b = nb;
    }
    {
        void *p = (char *)b + ARENA_HDR + b->used;
        b->used += need;
        a->used += need;
        return p;
    }
}

static void arena_destroy(Arena *a)
{
    ArenaBlock *b = a->head;
    while (b != NULL) {
        ArenaBlock *next = b->next;
        free(b);
        b = next;
    }
    a->head = NULL;
    a->total = 0;
    a->used = 0;
}

/* =====================================================================
 * 二、节点构建
 * ===================================================================== */

/* 单桶叶子：整段区间交给二分查找。
 * 必须显式设置 start_num = 0 且 division = 1，否则查询侧算出的桶号不可用。 */
static int make_leaf(Arena *a, LiNode *node, int start, int end)
{
    int *cdf = (int *)arena_alloc(a, 2 * sizeof(int));
    int *idx = (int *)arena_alloc(a, sizeof(int));

    if (cdf == NULL || idx == NULL) {
        return -1;
    }
    cdf[0] = 0;
    cdf[1] = end - start + 1;
    idx[0] = 0;
    node->whole_size = 1;
    node->division = 1;
    node->child_num = 0;
    node->start_num = 0;
    node->cdf = cdf;
    node->index_node = idx;
    return 0;
}

/* ---------------------------------------------------------------------
 * 直方图：串行 / 并行两个版本
 *
 * 并行策略是把节点负责的区间切成若干段，每段线程只往自己的局部桶数组里累加
 * （彼此不共享可写内存，无需加锁），最后串行合并。这样做的两个理由：
 *   - 直方图是建索引时最重的一步（每个元素要做一次 94 进制编码 + 一次除法），
 *     也是唯一随 N 线性增长的部分，最适合并行；
 *   - 递归本身保持串行，区域分配器就不必加锁，正确性风险最低。
 * ------------------------------------------------------------------- */
typedef struct {
    char *const *row;
    int start;
    int end;
    int start_bit;
    int end_bit;
    long long num0;
    int division;
    long long whole;
    int chunk_elems; /* 每段元素数 */
    int *partial;    /* chunks * whole 的局部直方图 */
} HistJob;

static void hist_worker(void *ctx, int task)
{
    HistJob *h = (HistJob *)ctx;
    int lo = h->start + task * h->chunk_elems;
    int hi = lo + h->chunk_elems - 1;
    int *loc;
    int i;

    if (lo > h->end) {
        return;
    }
    if (hi > h->end) {
        hi = h->end;
    }
    loc = h->partial + (size_t)task * (size_t)h->whole;
    for (i = lo; i <= hi; ++i) {
        long long v = li_encode(h->row[i], h->start_bit, h->end_bit - h->start_bit);
        long long b = (v - h->num0) / h->division;
        if (b < 0) {
            b = 0;
        } else if (b >= h->whole) {
            b = h->whole - 1;
        }
        loc[b]++;
    }
}

/* 把 [start, end] 的元素按桶号累加到 cdf[1..whole]（调用前 cdf 必须已清零） */
static void build_histogram(const LiTree *t, char *const *row, int start, int end,
                            int start_bit, int end_bit, long long num0, int division,
                            long long whole, int *cdf)
{
    int total = end - start + 1;
    int i;

    if (t->cfg.threads > 1 && total >= LI_PAR_MIN_ELEMS) {
        size_t per_chunk_bytes = (size_t)whole * sizeof(int);
        size_t budget = (size_t)LI_PAR_MAX_BYTES / (per_chunk_bytes ? per_chunk_bytes : 1);
        int chunks = t->cfg.threads;
        int *partial;

        if (chunks > LI_PAR_MAX_CHUNKS) {
            chunks = LI_PAR_MAX_CHUNKS;
        }
        if ((size_t)chunks > budget) {
            chunks = (int)budget;
        }
        if (chunks > total) {
            chunks = total;
        }
        if (chunks >= 2) {
            partial = (int *)calloc((size_t)chunks * (size_t)whole, sizeof(int));
            if (partial != NULL) {
                HistJob job;
                int c;
                long long b;

                job.row = row;
                job.start = start;
                job.end = end;
                job.start_bit = start_bit;
                job.end_bit = end_bit;
                job.num0 = num0;
                job.division = division;
                job.whole = whole;
                job.chunk_elems = (total + chunks - 1) / chunks;
                job.partial = partial;
                li_parallel_for(chunks, chunks, hist_worker, &job);

                for (c = 0; c < chunks; ++c) {
                    const int *loc = partial + (size_t)c * (size_t)whole;
                    for (b = 0; b < whole; ++b) {
                        cdf[b + 1] += loc[b];
                    }
                }
                free(partial);
                return;
            }
            /* 内存不足：退回串行 */
        }
    }

    for (i = start; i <= end; ++i) {
        long long v = li_encode(row[i], start_bit, end_bit - start_bit);
        long long b = (v - num0) / division;
        if (b < 0) {
            b = 0;
        } else if (b >= whole) {
            b = whole - 1;
        }
        cdf[b + 1]++;
    }
}

static int build_node(LiTree *t, Arena *a, char *const *row, int start, int end, LiNode *node, int depth)
{
    int length;
    int l2;
    int start_bit = 0;
    int end_bit;
    int division;
    long long num0;
    long long num1;
    long long span;
    long long whole;
    long long thr;
    long long limit;
    int child_num;
    int *cdf;
    int *idx;
    int i;

    if (depth > LI_MAX_DEPTH) {
        return -1;
    }

    /* ---- 1. 公共前缀 ---- */
    length = (int)strlen(row[start]);
    l2 = (int)strlen(row[end]);
    if (l2 < length) {
        length = l2;
    }
    if (length > LI_STRIDE - 1) {
        length = LI_STRIDE - 1;
    }
    while (start_bit < length && row[start][start_bit] == row[end][start_bit]) {
        start_bit++;
    }

    node->start = start;
    node->end = end;
    node->start_bit = start_bit;
    node->end_bit = start_bit;
    node->start_num = 0;
    node->division = 1;
    node->whole_size = 1;
    node->child_num = 0;
    node->cdf = NULL;
    node->index_node = NULL;
    node->children = NULL;
    t->node_count++;

    /* ---- 2. 退化为单桶的情形 ---- */
    /* 公共前缀已覆盖较短的那个键，或截断位数超出字符串长度：
     * 这段区间无法再靠数值细分，直接交给二分。 */
    if (start_bit >= length || start_bit + LI_HYPER_BIT > length) {
        return make_leaf(a, node, start, end);
    }

    end_bit = start_bit + LI_HYPER_BIT;
    num0 = li_encode(row[start], start_bit, end_bit - start_bit);
    num1 = li_encode(row[end], start_bit, end_bit - start_bit);
    if (num1 < num0) {
        return make_leaf(a, node, start, end); /* 数据未排序时的兜底 */
    }

    /* ---- 3. 方案一：逐步加长截断位数，直到"每单位值"的元素数不超过 cap ---- */
    if (t->cfg.scheme == LI_SCHEME_FIXED_CDF) {
        while (end_bit < length && (end_bit - start_bit) < LI_MAX_TRUNC) {
            long long count = (long long)(end - start + 1);
            long long values = num1 - num0 + 1;
            if (count / values <= t->cfg.cap) {
                break;
            }
            num0 = num0 * LI_RADIX + li_digit((unsigned char)row[start][end_bit]);
            num1 = num1 * LI_RADIX + li_digit((unsigned char)row[end][end_bit]);
            end_bit++;
        }
    }

    node->end_bit = end_bit;
    node->start_num = num0;

    /* ---- 4. 桶宽与桶数 ---- */
    span = num1 - num0 + 1;
    division = 1;
    whole = span;
    if (t->cfg.scheme == LI_SCHEME_FIXED_CDF) {
        /* 每单位值平均挂了多少个元素；倒过来就是每个桶该覆盖多宽的值域 */
        long long per_value = (long long)(end - start + 1) / span + 1;
        division = (int)(t->cfg.cap / per_value);
        if (division < 1) {
            division = 1;
        }
        whole = (num1 - num0) / division + 1;
    }
    if (whole > LI_MAX_CDF) {
        /* 兜底：加大桶宽，把 CDF 压回可控长度 */
        division = (int)((span + LI_MAX_CDF - 1) / LI_MAX_CDF);
        if (division < 1) {
            division = 1;
        }
        whole = (num1 - num0) / division + 1;
    }
    if (whole < 1) {
        whole = 1;
    }

    node->division = division;
    node->whole_size = (int)whole;

    cdf = (int *)arena_alloc(a, (size_t)(whole + 1) * sizeof(int));
    idx = (int *)arena_alloc(a, (size_t)whole * sizeof(int));
    if (cdf == NULL || idx == NULL) {
        return -1;
    }
    memset(cdf, 0, (size_t)(whole + 1) * sizeof(int));
    memset(idx, 0, (size_t)whole * sizeof(int));
    node->cdf = cdf;
    node->index_node = idx;

    /* ---- 5. 直方图 + 前缀和：cdf[b+1] 先当计数器，再原地累加 ---- */
    build_histogram(t, row, start, end, start_bit, end_bit, num0, division, whole, cdf);
    for (i = 0; i < (int)whole; ++i) {
        cdf[i + 1] += cdf[i];
    }

    /* ---- 6. 稠密判定 ---- */
    if (t->cfg.scheme == LI_SCHEME_FIXED_CDF) {
        thr = t->cfg.cap;
    } else {
        thr = (long long)(end - start) / whole;
    }
    if (thr < LI_MIN_THRESHOLD) {
        thr = LI_MIN_THRESHOLD;
    }
    limit = thr * t->cfg.dense;

    child_num = 0;
    for (i = 0; i < (int)whole; ++i) {
        if ((long long)(cdf[i + 1] - cdf[i]) > limit) {
            child_num++;
            idx[i] = child_num;
        }
    }
    node->child_num = child_num;
    if (child_num == 0) {
        return 0;
    }

    /* ---- 7. 递归子节点（连续数组） ---- */
    {
        LiNode *kids = (LiNode *)arena_alloc(a, (size_t)child_num * sizeof(LiNode));
        if (kids == NULL) {
            return -1;
        }
        memset(kids, 0, (size_t)child_num * sizeof(LiNode));
        node->children = kids;
        for (i = 0; i < (int)whole; ++i) {
            int ci = idx[i] - 1;
            int cs;
            int ce;
            if (ci < 0) {
                continue;
            }
            cs = cdf[i] + start;
            ce = cdf[i + 1] - 1 + start;
            if (ce < cs) {
                continue;
            }
            if (build_node(t, a, row, cs, ce, &kids[ci], depth + 1) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

/* =====================================================================
 * 三、构建 / 释放
 * ===================================================================== */
int li_litree_build(LiTree *tree, char *const *row, int n, const LiTreeConfig *cfg)
{
    Arena *a;

    if (tree == NULL || row == NULL || n <= 0) {
        return -1;
    }
    memset(tree, 0, sizeof *tree);
    if (cfg != NULL) {
        tree->cfg = *cfg;
    }
    if (tree->cfg.scheme != LI_SCHEME_DENSE_CDF) {
        tree->cfg.scheme = LI_SCHEME_FIXED_CDF;
    }
    if (tree->cfg.cap < 1) {
        tree->cfg.cap = LI_DEFAULT_CAP;
    }
    if (tree->cfg.dense < 1) {
        tree->cfg.dense = LI_LAMBDA;
    }
    if (tree->cfg.threads < 1 || tree->cfg.threads > 64) {
        tree->cfg.threads = 1; /* 未设置或异常值一律串行，避免读未初始化字段 */
    }
    tree->count = n;

    a = (Arena *)calloc(1, sizeof(Arena));
    if (a == NULL) {
        return -1;
    }
    tree->arena = a;
    if (build_node(tree, a, row, 0, n - 1, &tree->root, 0) != 0) {
        li_litree_free(tree);
        return -2;
    }
    tree->memory = a->used;
    tree->reserved = a->total;
    return 0;
}

void li_litree_free(LiTree *tree)
{
    if (tree == NULL) {
        return;
    }
    if (tree->arena != NULL) {
        arena_destroy((Arena *)tree->arena);
        free(tree->arena);
    }
    memset(tree, 0, sizeof *tree);
}

/* =====================================================================
 * 四、查询
 * ===================================================================== */
static SearchResult bucket_binary(char *const *row, const char *key, int low, int high)
{
    int c = 0;

    while (low <= high) {
        int mid = low + ((high - low) >> 1);
        int r = li_cmp(key, row[mid], &c);
        if (r < 0) {
            high = mid - 1;
        } else if (r > 0) {
            low = mid + 1;
        } else {
            return sr_make(mid, c);
        }
    }
    return sr_make(-1, c);
}

static SearchResult search_node(const LiNode *node, char *const *row, const char *key, int depth)
{
    int i;
    int k;
    int low;
    int high;
    long long v;

    if (depth > LI_MAX_DEPTH) {
        return sr_make(-1, 0);
    }

    /* 公共前缀不参与数值编码，先做一次 O(start_bit) 的剪枝 */
    for (i = 0; i < node->start_bit; ++i) {
        if (key[i] != row[node->start][i]) {
            return sr_make(-1, 0);
        }
    }

    if (node->division < 1 || node->whole_size < 1 || node->cdf == NULL) {
        return sr_make(-1, 0);
    }
    v = li_encode(key, node->start_bit, node->end_bit - node->start_bit);
    k = (int)((v - node->start_num) / node->division);
    if (k < 0 || k >= node->whole_size) {
        return sr_make(-1, 0);
    }

    low = node->cdf[k] + node->start;
    high = node->cdf[k + 1] - 1 + node->start;

    if (node->child_num == 0 || node->index_node[k] == 0) {
        if (low > high) {
            return sr_make(-1, 0); /* 空桶 */
        }
        return bucket_binary(row, key, low, high);
    }
    return search_node(&node->children[node->index_node[k] - 1], row, key, depth + 1);
}

SearchResult li_litree_search(const LiTree *tree, char *const *row, const char *key)
{
    if (tree == NULL || row == NULL || key == NULL || tree->count <= 0) {
        return sr_make(-1, 0);
    }
    return search_node(&tree->root, row, key, 0);
}

SearchResult li_litree_search_fn(void *ctx, char *const *row, int n, const char *key)
{
    (void)n;
    return li_litree_search((const LiTree *)ctx, row, key);
}
