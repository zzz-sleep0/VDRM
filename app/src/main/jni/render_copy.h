/* render_copy.h —— App 侧 EGL 拷贝回退（见 render_copy.c） */
#ifndef VDRM_RENDER_COPY_H
#define VDRM_RENDER_COPY_H

#include <stdbool.h>
#include <stdint.h>
#include <android/hardware_buffer.h>

/* 准备（第一次调用会建 EGL context + 目标环）。尺寸/格式变了会重建。
 * 返回 false 表示这台机器上拷贝路径用不了，调用方退回零拷贝。*/
bool rcopy_ensure(uint32_t w, uint32_t h, uint32_t fmt);

/* 借一个空闲目标；没有就返回 NULL（调用方这一帧退回零拷贝）。*/
AHardwareBuffer *rcopy_acquire(uint32_t *w, uint32_t *h);

/* SF 用完这块目标了，还回环里。可跨线程调用。*/
void rcopy_give_back(AHardwareBuffer *dst);

/* 把 src 的内容 1:1 blit 到 dst（必须在 EGL 线程上调用）。*/
bool rcopy_blit(AHardwareBuffer *src, uint32_t sw, uint32_t sh,
		AHardwareBuffer *dst, uint32_t dw, uint32_t dh);

/* 把所有目标标记为空闲（回前台/重建 SurfaceControl 时用）。*/
void rcopy_release_all(void);

/* 诊断计数。*/
void rcopy_counters(unsigned long *frames, unsigned long *fails,
		    unsigned long *givebacks, int *free_targets);

/* 一次 blit 的耗时（微秒），诊断用。*/
unsigned long rcopy_blit_max_us(void);
unsigned long rcopy_blit_last_us(void);

#endif
