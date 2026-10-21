/* =====================================================================
 * dataset.c —— 数据集实现
 * ===================================================================== */
#include "dataset.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

/* =====================================================================
 * 一、可复现的伪随机数（xorshift64*）
 *
 * 状态用线程局部存储：查询并行时每个线程各有一条独立随机流，
 * 既避免数据竞争，也保证各线程的抽样互不干扰。
 * ===================================================================== */

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#define LI_THREAD_LOCAL _Thread_local
#elif defined(__GNUC__)
#define LI_THREAD_LOCAL __thread
#else
#define LI_THREAD_LOCAL
#endif

static LI_THREAD_LOCAL unsigned long long g_rng = 0x243F6A8885A308D3ULL;

void li_rng_seed(unsigned long long seed)
{
    g_rng = (seed != 0) ? seed : 0x243F6A8885A308D3ULL;
}

unsigned long long li_rng_next(void)
{
    unsigned long long x = g_rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    g_rng = x;
    return x * 2685821657736338717ULL;
}

int li_rng_below(int n)
{
    if (n <= 1) {
        return 0;
    }
    return (int)(li_rng_next() % (unsigned long long)n);
}

/* (0,1) 开区间均匀分布，避免 log(0) 与 cos 取到端点 */
static double rng_open01(void)
{
    return ((double)(li_rng_next() >> 11) + 1.0) / 9007199254740994.0;
}

/* Box-Muller 变换（与原版 normalRandom 等价） */
static double rng_normal(double mu, double sigma)
{
    double v1 = rng_open01();
    double v2 = rng_open01();
    return cos(2.0 * LI_PI * v2) * sqrt(-2.0 * log(v1)) * sigma + mu;
}

/* 把任意整数折回 [0, 94) —— 原版直接 (int)normalRandom(...) % 94 会得到负值，
 * 进而生成小于 '!' 的字符，破坏"字符都在 33~126"的前提。 */
static int wrap_radix(long long v)
{
    int d = (int)(v % LI_RADIX);
    if (d < 0) {
        d += LI_RADIX;
    }
    return d;
}

/* =====================================================================
 * 二、五种字符串生成器（形状与原版一一对应）
 *
 * 约定：缓冲区长度 n（含结尾 '\0'），实际写入 n-1 个字符。
 * 原版只调整前 1~2 个字符的分布，其余字符均匀随机，这里保持一致。
 * ===================================================================== */

/* 把整数 v 编码成前两个字符（94 进制低位在前，与原版 j/k 的取法一致） */
static void put_pair(char *buf, long long v)
{
    int j = wrap_radix(v);
    int k = wrap_radix(v / LI_RADIX);
    buf[0] = (char)(k + LI_BASE);
    buf[1] = (char)(j + LI_BASE);
}

static void fill_random_tail(char *buf, int from, int n)
{
    int i;
    for (i = from; i < n - 1; ++i) {
        buf[i] = (char)(li_rng_below(LI_RADIX) + LI_BASE);
    }
}

/* 均匀分布：每个字符均匀取自 94 个可打印字符 */
static void gen_uniform(char *buf, int n)
{
    fill_random_tail(buf, 0, n);
    buf[n - 1] = '\0';
}

/* 首字符尖峰：首字符均值 47、标准差 22；其后均值 47、标准差 33 */
static void gen_nd(char *buf, int n)
{
    buf[0] = (char)(wrap_radix((long long)rng_normal(47.0, 22.0)) + LI_BASE);
    for (int i = 1; i < n - 1; ++i) {
        buf[i] = (char)(wrap_radix((long long)rng_normal(47.0, 33.0)) + LI_BASE);
    }
    buf[n - 1] = '\0';
}

/* 双尖峰：首字符均匀；若首字符为 '0'/'1'，第二个字符以 94/110 的概率砸到固定值 */
static void gen_nd2(char *buf, int n)
{
    for (int i = 0; i < n - 1; ++i) {
        int a;
        if (i == 1 && (buf[0] == '0' || buf[0] == '1')) {
            a = li_rng_below(110);
            if (a >= LI_RADIX) {
                /* 原版此处取 a = 48/49 再统一 +33，故实际尖峰落在 'Q'/'R' 上；
                 * 为与历史数据保持同一形状，这里保持原行为。 */
                a = (li_rng_below(2) == 0) ? 48 : 49;
            }
        } else {
            a = li_rng_below(LI_RADIX);
        }
        buf[i] = (char)(a + LI_BASE);
    }
    buf[n - 1] = '\0';
}

/* 正态分布：由单个正态数（均值 4418、标准差 128）决定前两位 */
static void gen_normal(char *buf, int n)
{
    put_pair(buf, (long long)rng_normal(4418.0, 128.0));
    fill_random_tail(buf, 2, n);
    buf[n - 1] = '\0';
}

/* 混合高斯（双峰）：2209 为中心的半高斯，一半概率平移到 4418 附近 */
static void gen_mix(char *buf, int n)
{
    long long l = (long long)rng_normal(2209.0, 1628.0);
    l /= 2;
    if (li_rng_below(2) == 0) {
        l += 4418;
    }
    put_pair(buf, l);
    fill_random_tail(buf, 2, n);
    buf[n - 1] = '\0';
}

typedef void (*GenFn)(char *buf, int n);

static GenFn const GEN_FNS[LI_DIST_COUNT] = {
    gen_uniform,
    gen_nd,
    gen_nd2,
    gen_normal,
    gen_mix,
};

static const char *const DIST_NAMES[LI_DIST_COUNT] = {
    "uniform",
    "nd",
    "nd2",
    "normal",
    "mix",
};

const char *li_dist_name(LiDist dist)
{
    if (dist < 0 || dist >= LI_DIST_COUNT) {
        return "?";
    }
    return DIST_NAMES[dist];
}

int li_dist_parse(const char *name, LiDist *out)
{
    int i;
    if (name == NULL || out == NULL) {
        return -1;
    }
    for (i = 0; i < LI_DIST_COUNT; ++i) {
        if (strcmp(name, DIST_NAMES[i]) == 0) {
            *out = (LiDist)i;
            return 0;
        }
    }
    return -1;
}

/* =====================================================================
 * 三、数据集的分配 / 释放 / 读写
 * ===================================================================== */
int li_dataset_alloc(LiDataset *ds, int capacity, int stride)
{
    size_t bytes;

    if (ds == NULL || capacity <= 0 || stride <= 0) {
        return -1;
    }
    memset(ds, 0, sizeof *ds);
    ds->stride = stride;
    ds->capacity = capacity;

    bytes = (size_t)capacity * (size_t)stride;
    ds->buf = (char *)calloc(bytes, 1);
    if (ds->buf == NULL) {
        return -1;
    }
    ds->row = (char **)calloc((size_t)capacity, sizeof(char *));
    if (ds->row == NULL) {
        free(ds->buf);
        memset(ds, 0, sizeof *ds);
        return -1;
    }
    for (int i = 0; i < capacity; ++i) {
        ds->row[i] = ds->buf + (size_t)i * (size_t)stride;
    }
    ds->count = 0;
    return 0;
}

void li_dataset_free(LiDataset *ds)
{
    if (ds == NULL) {
        return;
    }
    free(ds->buf);
    free(ds->row);
    memset(ds, 0, sizeof *ds);
}

int li_dataset_load(LiDataset *ds, const char *path, int capacity)
{
    static const size_t CHUNK = 1u << 16;
    char *chunk;
    FILE *fp;
    char *row;
    size_t col = 0;
    int n = 0;
    int done = 0;

    if (ds == NULL || path == NULL || capacity <= 0) {
        return -1;
    }
    if (li_dataset_alloc(ds, capacity, LI_STRIDE) != 0) {
        return -1;
    }
    fp = fopen(path, "rb");
    if (fp == NULL) {
        li_dataset_free(ds);
        return -2;
    }
    chunk = (char *)malloc(CHUNK);
    if (chunk == NULL) {
        fclose(fp);
        li_dataset_free(ds);
        return -1;
    }
    row = ds->buf;

    while (!done) {
        size_t got = fread(chunk, 1, CHUNK, fp);
        size_t i;
        if (got == 0) {
            break;
        }
        for (i = 0; i < got; ++i) {
            int ch = (unsigned char)chunk[i];
            if (ch == '\n') {
                row[col] = '\0';
                ds->row[n++] = row;
                row += ds->stride;
                col = 0;
                if (n >= capacity) {
                    done = 1;
                    break;
                }
            } else if (ch == '\r') {
                /* 忽略 CRLF 的 '\r' */
            } else if (col + 1 < (size_t)ds->stride) {
                row[col++] = (char)ch;
            }
            /* 超长行直接截断到 stride-1，行为与 fscanf("%s") 一致 */
        }
    }
    if (!done && col > 0 && n < capacity) {
        row[col] = '\0';
        ds->row[n++] = row;
    }
    free(chunk);
    fclose(fp);
    ds->count = n;
    return 0;
}

int li_dataset_save(const LiDataset *ds, const char *path)
{
    FILE *fp;
    static char iobuf[1u << 20];

    if (ds == NULL || path == NULL || ds->count <= 0) {
        return -1;
    }
    fp = fopen(path, "wb");
    if (fp == NULL) {
        return -2;
    }
    setvbuf(fp, iobuf, _IOFBF, sizeof iobuf);
    for (int i = 0; i < ds->count; ++i) {
        fputs(ds->row[i], fp);
        fputc('\n', fp);
    }
    fclose(fp);
    return 0;
}

/* 排序比较器：只交换指针，不再搬 128 字节的记录 */
static int cmp_row(const void *a, const void *b)
{
    const char *const *pa = (const char *const *)a;
    const char *const *pb = (const char *const *)b;
    return strcmp(*pa, *pb);
}

int li_dataset_generate(LiDataset *ds, int count, LiDist dist, unsigned long long seed)
{
    GenFn gen;

    if (ds == NULL || count <= 0 || dist < 0 || dist >= LI_DIST_COUNT) {
        return -1;
    }
    if (li_dataset_alloc(ds, count, LI_STRIDE) != 0) {
        return -1;
    }
    li_rng_seed(seed);
    gen = GEN_FNS[dist];
    for (int i = 0; i < count; ++i) {
        /* 长度与原版一致：rand() % (len - 32) + 32，故实际字符数为 31~126 */
        int n = li_rng_below(LI_STRIDE - 32) + 32;
        gen(ds->row[i], n);
    }
    ds->count = count;
    qsort(ds->row, (size_t)count, sizeof(char *), cmp_row);
    return 0;
}

/* =====================================================================
 * 四、校验
 * ===================================================================== */
int li_dataset_is_sorted(const LiDataset *ds)
{
    if (ds == NULL) {
        return -1;
    }
    for (int i = 1; i < ds->count; ++i) {
        if (strcmp(ds->row[i - 1], ds->row[i]) > 0) {
            return i; /* 返回第一处逆序的下标 */
        }
    }
    return 0;
}

int li_dataset_bad_chars(const LiDataset *ds)
{
    int bad = 0;

    if (ds == NULL) {
        return -1;
    }
    for (int i = 0; i < ds->count; ++i) {
        const unsigned char *p = (const unsigned char *)ds->row[i];
        for (; *p != '\0'; ++p) {
            if (*p < (unsigned char)LI_BASE || *p >(unsigned char)(LI_BASE + LI_RADIX - 1)) {
                bad++;
            }
        }
    }
    return bad;
}

double li_dataset_mean_length(const LiDataset *ds)
{
    long long total = 0;

    if (ds == NULL || ds->count <= 0) {
        return 0.0;
    }
    for (int i = 0; i < ds->count; ++i) {
        total += (long long)strlen(ds->row[i]);
    }
    return (double)total / (double)ds->count;
}
