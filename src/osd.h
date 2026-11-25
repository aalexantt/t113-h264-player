/*
 * osd.h — 视频上叠加信息文字
 *
 * 方案：独立的 disp layer，ARGB8888，pixel alpha 混合。
 * 背景画成半透明黑，字画成不透明白/黄，这样既看得清又不完全挡画面。
 *
 * 缓冲用 memops 的 palloc 分配（底层就是 ion），再通过 get_fd_by_vir
 * 取 dma-buf fd 交给 disp。这条取 fd 的路径在 fdverify 里验证过：
 * readlink /proc/self/fd/N 返回 /dmabuf:，size 与分配量一致。
 */

#ifndef OSD_H
#define OSD_H

#include <stdint.h>
#include <stdbool.h>

/* 内核头 sunxi_display2.h 用了 u32/s32 这些内核类型。
 * 用户态没有 typedef，这里补上。必须在包含内核头之前生效，
 * 所以放在 osd.h 里（player.c 先包含 osd.h 再包含内核头）。 */
#ifndef OSD_KERNEL_TYPES_DEFINED
#define OSD_KERNEL_TYPES_DEFINED
typedef uint64_t u64;
typedef int64_t  s64;
typedef uint32_t u32;
typedef int32_t  s32;
typedef uint16_t u16;
typedef int16_t  s16;
typedef uint8_t  u8;
typedef int8_t   s8;
#endif

#include "sc_interface.h"

/* OSD 画布在屏幕上的位置。画布只覆盖面板区域以省内存
 * （1920x1080 ARGB 要 8MB，板子空闲内存吃紧）。 */
#ifndef OSD_ORIGIN_X
#define OSD_ORIGIN_X  8
#endif
#ifndef OSD_ORIGIN_Y
#define OSD_ORIGIN_Y  8
#endif

typedef struct {
    struct ScMemOpsS *memops;
    void    *vaddr;         /* 虚拟地址，用于 CPU 画字 */
    int      dma_fd;
    int      w, h;          /* OSD 画布尺寸（像素） */
    int      stride;        /* 字节/行，= w*4 */

    int      disp_fd;
    int      screen;
    int      channel;
    int      layer_id;
    int      zorder;
    int      scr_w, scr_h;  /* 屏幕尺寸，用于定位 */
    int      origin_x, origin_y;

    int      inited;
} osd_t;

/* 初始化 OSD 层。scr_w/scr_h 是屏幕尺寸，osd_w/osd_h 是画布尺寸。
 * 画布会被放在屏幕左上角。返回 0 成功。 */
int osd_init(osd_t *o, struct ScMemOpsS *memops, int disp_fd,
             int screen, int channel, int layer_id, int zorder,
             int scr_w, int scr_h, int osd_w, int osd_h);

void osd_deinit(osd_t *o);

/* 清空画布（全透明） */
void osd_clear(osd_t *o);

/* 画一块半透明底色 + 细边框，作为文字背景 */
void osd_panel(osd_t *o, int x, int y, int w, int h);

/* 在画布上画字符串。color 是 0xRRGGBB，alpha 0~255 */
void osd_text(osd_t *o, int x, int y, const char *s, uint32_t color, int alpha);

/* 把画布提交给 disp */
int osd_flush(osd_t *o);

#endif