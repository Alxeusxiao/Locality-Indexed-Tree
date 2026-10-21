/* =====================================================================
 * searcher.c —— 四种经典字符串查找算法
 *
 * 与原版的对应关系：
 *   li_search_binary            <- Binary_search
 *   li_search_interpolation     <- Interpolation_Search
 *   li_search_interpolation_low <- Interpolation_Search_low
 *   li_search_cut               <- Cut_Search
 *
 * 相对原版的改动（均为缺陷修复，算法思路保持不变）：
 *   1. 返回 (位置, strcmp 次数)，不再在热路径里 printf，也不再拿"比较次数"
 *      冒充"位置"；
 *   2. strcmp 结果改用 <0 / >0 判断，不再假设它一定返回 ±1；
 *   3. 所有落点做闭区间夹逼，原版插值落点可能越界（str[mid] 甚至 row[n]）；
 *   4. 原版插值法在 key < str[mid] 时把上界写成 mid + 1（不但没排除 mid，
 *      mid == high 时还会读到 row[n]），这里改为 mid；
 *   5. 跳跃步长用整数递推替代 pow(3, j) 的浮点计算，并防止下标溢出。
 * ===================================================================== */
#include "searcher.h"

#include "litree.h"

/* ---------------------------------------------------------------------
 * 在 [low, high] 区间内二分
 * ------------------------------------------------------------------- */
static SearchResult bin_in(char *const *row, const char *key, int low, int high)
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

SearchResult li_search_binary(char *const *row, int n, const char *key)
{
    if (row == NULL || n <= 0 || key == NULL) {
        return sr_make(-1, 0);
    }
    return bin_in(row, key, 0, n - 1);
}

/* ---------------------------------------------------------------------
 * 插值查找
 * ------------------------------------------------------------------- */
SearchResult li_search_interpolation(char *const *row, int n, const char *key)
{
    int low = 0;
    int high = n - 1;
    int c = 0;
    const int inc = LI_INC_I;
    int mid;
    long long flow;
    long long fhigh;
    long long fkey;
    double p;

    if (row == NULL || n <= 0 || key == NULL) {
        return sr_make(-1, 0);
    }
    flow = li_encode(row[low], 0, inc);
    fhigh = li_encode(row[high], 0, inc) + 1;

    while (high - low >= LI_THRESHOLD_I) {
        int r;

        fkey = li_encode(key, 0, inc);
        p = (fhigh > flow) ? (double)(fkey - flow) / (double)(fhigh - flow) : 1.0;
        mid = low + (int)((double)(high - low) * p);
        /* 落点必须严格落在 (low, high) 内部 */
        if (mid <= low) {
            mid = low + 1;
        }
        if (mid >= high) {
            mid = high - 1;
        }

        if (mid - low <= 1) {
            /* 已贴到左端：按 3 的幂跳着走，再二分收尾 */
            int i = low;
            int step = 1;
            r = li_cmp(key, row[i], &c);
            while (r > 0) {
                low = i;
                if (step > n) {
                    step = n; /* 防止步长与下标溢出 */
                }
                i += step;
                step *= 3;
                if (i >= n) {
                    i = n - 1;
                    break;
                }
                r = li_cmp(key, row[i], &c);
            }
            if (r == 0) {
                return sr_make(i, c);
            }
            return bin_in(row, key, low, i);
        }
        if (high - mid <= 1) {
            /* 已贴到右端：同上，反方向跳 */
            int i = high;
            int step = 1;
            r = li_cmp(key, row[i], &c);
            while (r < 0) {
                high = i;
                if (step > n) {
                    step = n;
                }
                i -= step;
                step *= 3;
                if (i < 0) {
                    i = 0;
                    break;
                }
                r = li_cmp(key, row[i], &c);
            }
            if (r == 0) {
                return sr_make(i, c);
            }
            return bin_in(row, key, i, high);
        }

        if (fhigh - flow <= 1) {
            break; /* 数值区间已退化，直接二分收尾 */
        }

        r = li_cmp(key, row[mid], &c);
        if (r < 0) {
            high = mid;
        } else if (r > 0) {
            low = mid;
        } else {
            return sr_make(mid, c);
        }
        flow = li_encode(row[low], 0, inc);
        fhigh = li_encode(row[high], 0, inc) + 1;
    }
    return bin_in(row, key, low, high);
}

/* ---------------------------------------------------------------------
 * 简易插值：从插值落点开始逐格线性扫描（原版就是 O(n) 的朴素做法）
 * ------------------------------------------------------------------- */
SearchResult li_search_interpolation_low(char *const *row, int n, const char *key)
{
    int c = 0;
    const int inc = LI_INC_I;
    int i;
    int r;
    long long flow;
    long long fhigh;
    long long fkey;
    double p;

    if (row == NULL || n <= 0 || key == NULL) {
        return sr_make(-1, 0);
    }
    flow = li_encode(row[0], 0, inc);
    fhigh = li_encode(row[n - 1], 0, inc) + 1;
    fkey = li_encode(key, 0, inc);
    p = (fhigh > flow) ? (double)(fkey - flow) / (double)(fhigh - flow) : 0.0;
    i = (int)((double)(n - 1) * p);
    if (i < 0) {
        i = 0;
    }
    if (i > n - 1) {
        i = n - 1;
    }

    r = li_cmp(key, row[i], &c);
    if (r == 0) {
        return sr_make(i, c);
    }
    if (r > 0) {
        while (r > 0) {
            ++i;
            if (i >= n) {
                return sr_make(-1, c);
            }
            r = li_cmp(key, row[i], &c);
        }
    } else {
        while (r < 0) {
            --i;
            if (i < 0) {
                return sr_make(-1, c);
            }
            r = li_cmp(key, row[i], &c);
        }
    }
    return sr_make((r == 0) ? i : -1, c);
}

/* ---------------------------------------------------------------------
 * 割线查找
 * ------------------------------------------------------------------- */
SearchResult li_search_cut(char *const *row, int n, const char *key)
{
    int low = 0;
    int high = n - 1;
    int old = 0;
    int now = n - 1;
    int new_x;
    int c = 0;
    int inc = LI_INC_C;
    long long f_old;
    long long f_now;
    long long f_star;
    long long delta_f;
    double p;
    int guard = 0;

    if (row == NULL || n <= 0 || key == NULL) {
        return sr_make(-1, 0);
    }
    if (high - low < LI_THRESHOLD_C) {
        return bin_in(row, key, low, high);
    }

    f_old = li_encode(row[old], 0, inc);
    f_now = li_encode(row[now], 0, inc) + 1;
    f_star = li_encode(key, 0, inc);
    p = (f_now > f_old) ? (double)(f_star - f_old) / (double)(f_now - f_old) : 0.0;
    new_x = old + (int)((double)(now - old) * p);

    /* 第一步就是用一次插值猜位置；猜不中就退回二分 */
    if (new_x <= low || new_x >= high) {
        return bin_in(row, key, low, high);
    }

    low = old;
    {
        int r = li_cmp(key, row[new_x], &c);
        if (r == 0) {
            return sr_make(new_x, c);
        }
        if (r < 0) {
            f_old = f_now;
            high = new_x;
        } else {
            old = now;
            now = new_x;
            low = new_x;
        }
    }
    now = new_x;
    f_old = li_encode(row[old], 0, inc);

    while (high - low >= LI_THRESHOLD_C && guard++ < 1000) {
        long long cases;
        int r;

        f_now = li_encode(row[now], 0, inc);
        delta_f = f_old - f_now;
        if (delta_f <= 1 && delta_f >= -1) {
            /* 数值差太小，加大截断位数换取分辨率 */
            inc += inc;
            if (inc > LI_MAX_TRUNC) {
                inc = LI_MAX_TRUNC;
            }
            f_old = li_encode(row[old], 0, inc);
            f_now = li_encode(row[now], 0, inc);
            f_star = li_encode(key, 0, inc);
            delta_f = f_old - f_now;
            if (delta_f == 0) {
                break;
            }
        }
        p = (double)(f_star - f_now) / (double)delta_f;
        cases = (long long)((double)(old - now) * p);
        new_x = (int)((long long)now + cases);

        if (new_x <= low || new_x >= high) {
            new_x = (low + high) / 2;
            if (new_x <= low || new_x >= high) {
                break;
            }
            r = li_cmp(key, row[new_x], &c);
            if (r == 0) {
                return sr_make(new_x, c);
            }
            if (r < 0) {
                high = new_x;
                old = low;
            } else {
                low = new_x;
                old = high;
            }
            now = new_x;
            f_old = li_encode(row[old], 0, inc);
            f_now = li_encode(row[now], 0, inc);
        } else {
            r = li_cmp(key, row[new_x], &c);
            if (r == 0) {
                return sr_make(new_x, c);
            }
            if (cases <= 1 && cases >= -1) {
                break; /* 几乎没挪动，交回二分 */
            }
            old = now;
            now = new_x;
            f_old = f_now;
            if (r < 0) {
                high = new_x;
            } else {
                low = new_x;
            }
        }
    }
    return bin_in(row, key, low, high);
}

/* =====================================================================
 * 算法表：LI-Tree 需要索引上下文，用薄适配层统一签名
 * ===================================================================== */
static SearchResult ad_binary(void *ctx, char *const *row, int n, const char *key)
{
    (void)ctx;
    return li_search_binary(row, n, key);
}

static SearchResult ad_interpolation(void *ctx, char *const *row, int n, const char *key)
{
    (void)ctx;
    return li_search_interpolation(row, n, key);
}

static SearchResult ad_interpolation_low(void *ctx, char *const *row, int n, const char *key)
{
    (void)ctx;
    return li_search_interpolation_low(row, n, key);
}

static SearchResult ad_cut(void *ctx, char *const *row, int n, const char *key)
{
    (void)ctx;
    return li_search_cut(row, n, key);
}

const LiAlgo LI_ALGOS[] = {
    {"binary", ad_binary, 0},
    {"interpolation", ad_interpolation, 0},
    {"interpolation-low", ad_interpolation_low, 0},
    {"cut", ad_cut, 0},
    {"li-tree", li_litree_search_fn, 1},
};

const int LI_ALGO_COUNT = (int)(sizeof LI_ALGOS / sizeof LI_ALGOS[0]);
