/* =====================================================================
 * timer.h —— 单调高精度计时器
 *
 * 原版用 clock() 累加每次查找的 CPU 时间：分辨率低（约 1~15 ms）、
 * 且循环内的计时调用本身开销与查找同量级。这里改为"整段循环计时 / 次数"。
 * ===================================================================== */
#ifndef LI_TIMER_H
#define LI_TIMER_H

/* 返回单调递增的秒数（Windows 用 QPC，POSIX 用 CLOCK_MONOTONIC） */
double li_now_seconds(void);

/* 把控制台输出代码页切到 UTF-8，保证中文标签不乱码（非 Windows 平台为空操作） */
void li_console_utf8(void);

#endif /* LI_TIMER_H */
