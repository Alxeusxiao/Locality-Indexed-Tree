/* =====================================================================
 * timer.c —— 单调高精度计时器实现
 * ===================================================================== */
#include "timer.h"

#ifdef _WIN32

#include <windows.h>

double li_now_seconds(void)
{
    static LARGE_INTEGER freq;
    static int ready = 0;
    LARGE_INTEGER now;

    if (!ready) {
        QueryPerformanceFrequency(&freq);
        if (freq.QuadPart <= 0) {
            freq.QuadPart = 1;
        }
        ready = 1;
    }
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
}

#else /* POSIX */

#include <time.h>

double li_now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

#endif

void li_console_utf8(void)
{
#ifdef _WIN32
    /* 源码是 UTF-8，把控制台输出代码页也切到 UTF-8，中文标签才不会乱码 */
    SetConsoleOutputCP(65001);
#endif
}
