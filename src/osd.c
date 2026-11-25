/*
 * osd.c — OSD 层实现
 *
 * 关键点：
 *   1. 缓冲必须是 dma-buf（disp 的 fb.fd 只认这个）。用 memops->palloc
 *      分配，它底层走 ion，能拿到 dma-buf。
 *   2. 逐像素 alpha（alpha_mode=0），背景 alpha 取低值做半透明，
 *      文字 alpha=255 全不透明。
 *   3. OSD 画布尺寸取屏幕实际尺寸，或者倍率放大后的尺寸，避免缩放模糊。
 *      这里直接在 1:1 的目标分辨率上画，不做缩放。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>

/* osd.h 定义了 u32/s32 等内核类型，必须在 sunxi_display2.h 之前包含 */
#include "osd.h"
#include "font.h"
#include <video/sunxi_display2.h>

/* memops 虚表索引（新版 24 成员布局，32 位）：
 *   4  = palloc          void*(int size)
 *   6  = pfree           void(void*)
 *   23 = get_fd_by_vir   int(void* virt)
 * 这几个索引来自 sc_interface.h 的成员顺序，且 23 已被 fdverify 实测验证。 */
#define VT_PALLOC        4
#define VT_PFREE         6
#define VT_GET_FD        23

typedef void *(*palloc_fn)(int);
typedef void  (*pfree_fn)(void *);
typedef int   (*getfd_fn)(void *);

static palloc_fn  g_palloc;
static pfree_fn   g_pfree;
static getfd_fn   g_getfd;

static int resolve_memops(osd_t *o)
{
    void **vt = (void **)(void *)o->memops;
    if (!vt)
        return -1;
    g_palloc = (palloc_fn)vt[VT_PALLOC];
    g_pfree  = (pfree_fn)vt[VT_PFREE];
    g_getfd  = (getfd_fn)vt[VT_GET_FD];
    if (!g_palloc || !g_pfree || !g_getfd) {
        fprintf(stderr, "[osd] memops 虚表项缺失: palloc=%p pfree=%p getfd=%p\n",
                (void *)g_palloc, (void *)g_pfree, (void *)g_getfd);
        return -1;
    }
    return 0;
}

/* 在 ARGB8888 缓冲里画点（带 alpha 混合或直接覆盖） */
static inline void put_px(osd_t *o, int x, int y, uint32_t rgb, int a)
{
    if (x < 0 || y < 0 || x >= o->w || y >= o->h || a <= 0)
        return;
    {
        uint32_t *row = (uint32_t *)((uint8_t *)o->vaddr + (size_t)y * o->stride);
        /* disp 的 ARGB_8888 是 MSB A-R-G-B LSB。
         * 小端机器上 32 位字的内存布局是 B,G,R,A（字节序），
         * 所以直接按 0xAARRGGBB 组字，写入后驱动读到的就是 ARGB。 */
        row[x] = ((uint32_t)a << 24) | (rgb & 0x00ffffff);
    }
}

int osd_init(osd_t *o, struct ScMemOpsS *memops, int disp_fd,
             int screen, int channel, int layer_id, int zorder,
             int scr_w, int scr_h, int osd_w, int osd_h)
{
    size_t bytes;
    int i;

    memset(o, 0, sizeof(*o));
    o->memops = memops;
    o->disp_fd = disp_fd;
    o->screen = screen;
    o->channel = channel;
    o->layer_id = layer_id;
    o->zorder = zorder;
    o->scr_w = scr_w;
    o->scr_h = scr_h;
    o->w = osd_w;
    o->h = osd_h;
    o->stride = osd_w * 4;
    o->origin_x = 0;
    o->origin_y = 0;

    if (resolve_memops(o) != 0)
        return -1;

    bytes = (size_t)o->stride * (size_t)o->h;
    /* 多分配一些余量：ion 分配和 4K 对齐可能要多留 */
    o->vaddr = g_palloc((int)bytes);
    if (!o->vaddr) {
        fprintf(stderr, "[osd] palloc(%zu) 失败\n", bytes);
        return -1;
    }
    memset(o->vaddr, 0, bytes);

    o->dma_fd = g_getfd(o->vaddr);
    if (o->dma_fd < 0) {
        fprintf(stderr, "[osd] get_fd_by_vir 失败\n");
        g_pfree(o->vaddr);
        o->vaddr = NULL;
        return -1;
    }

    printf("[osd] 画布 %dx%d stride=%d 字节=%zu vaddr=%p dma_fd=%d\n",
           o->w, o->h, o->stride, bytes, o->vaddr, o->dma_fd);

    /* 清成全透明并压到屏幕上，确认层能用 */
    for (i = 0; i < o->h; i++)
        memset((uint8_t *)o->vaddr + (size_t)i * o->stride, 0, (size_t)o->stride);

    o->inited = 1;
    return osd_flush(o);
}

void osd_deinit(osd_t *o)
{
    if (!o)
        return;

    /* 先关层 */
    if (o->disp_fd >= 0 && o->inited) {
        struct disp_layer_config2 cfg;
        unsigned long arg[4];
        memset(&cfg, 0, sizeof(cfg));
        cfg.channel = o->channel;
        cfg.layer_id = o->layer_id;
        cfg.enable = 0;
        memset(arg, 0, sizeof(arg));
        arg[0] = (unsigned long)o->screen;
        arg[1] = (unsigned long)&cfg;
        arg[2] = 1;
        ioctl(o->disp_fd, DISP_LAYER_SET_CONFIG2, (void *)arg);
    }

    if (o->vaddr && g_pfree)
        g_pfree(o->vaddr);
    o->vaddr = NULL;
    o->inited = 0;
}

void osd_clear(osd_t *o)
{
    int i;
    if (!o || !o->vaddr)
        return;
    for (i = 0; i < o->h; i++)
        memset((uint8_t *)o->vaddr + (size_t)i * o->stride, 0, (size_t)o->stride);
}

void osd_panel(osd_t *o, int x, int y, int w, int h)
{
    int i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            put_px(o, x + i, y + j, 0x000000, 150);   /* 半透明黑 */

    /* 一圈淡边框，让面板和画面分开 */
    for (i = 0; i < w; i++) {
        put_px(o, x + i, y, 0x60c0ff, 200);
        put_px(o, x + i, y + h - 1, 0x60c0ff, 200);
    }
    for (j = 0; j < h; j++) {
        put_px(o, x, y + j, 0x60c0ff, 200);
        put_px(o, x + w - 1, y + j, 0x60c0ff, 200);
    }
}

void osd_text(osd_t *o, int x, int y, const char *s, uint32_t color, int alpha)
{
    if (!o || !o->vaddr || !s)
        return;

    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        const unsigned char *g;
        int gy, gx;

        if (ch < FONT_FIRST || ch > FONT_LAST) {
            x += FONT_W;
            continue;
        }
        g = font_map[ch - FONT_FIRST];

        for (gy = 0; gy < FONT_H; gy++) {
            int base = gy * FONT_BPR;
            for (gx = 0; gx < FONT_W; gx++) {
                if (g[base + (gx >> 3)] & (0x80 >> (gx & 7)))
                    put_px(o, x + gx, y + gy, color, alpha);
            }
        }
        x += FONT_W;
    }
}

int osd_flush(osd_t *o)
{
    struct disp_layer_config2 cfg;
    unsigned long arg[4];
    int ret;

    if (!o || !o->inited || o->disp_fd < 0)
        return -1;

    memset(&cfg, 0, sizeof(cfg));
    cfg.channel  = o->channel;
    cfg.layer_id = o->layer_id;
    cfg.enable   = 1;
    cfg.info.mode        = LAYER_MODE_BUFFER;
    cfg.info.zorder      = o->zorder;
    /* alpha_mode = 0：逐像素 alpha。背景 alpha 低就半透明，
     * 文字 alpha 255 就不透明。 */
    cfg.info.alpha_mode  = 0;
    cfg.info.alpha_value = 0xff;

    /* 画布只有面板那么大，直接放在屏幕左上角 */
    cfg.info.screen_win.x      = OSD_ORIGIN_X;
    cfg.info.screen_win.y      = OSD_ORIGIN_Y;
    cfg.info.screen_win.width  = (unsigned int)o->w;
    cfg.info.screen_win.height = (unsigned int)o->h;

    cfg.info.fb.fd          = o->dma_fd;
    cfg.info.fb.format      = DISP_FORMAT_ARGB_8888;
    cfg.info.fb.color_space = DISP_BT709;
    cfg.info.fb.size[0].width  = (unsigned int)o->w;
    cfg.info.fb.size[0].height = (unsigned int)o->h;
    cfg.info.fb.size[1].width  = 0;
    cfg.info.fb.size[1].height = 0;
    cfg.info.fb.size[2].width  = 0;
    cfg.info.fb.size[2].height = 0;
    cfg.info.fb.align[0] = 4;
    cfg.info.fb.align[1] = 4;
    cfg.info.fb.align[2] = 4;

    cfg.info.fb.crop.x = 0;
    cfg.info.fb.crop.y = 0;
    cfg.info.fb.crop.width  = ((long long)o->w) << 32;
    cfg.info.fb.crop.height = ((long long)o->h) << 32;

    memset(arg, 0, sizeof(arg));
    arg[0] = (unsigned long)o->screen;
    arg[1] = (unsigned long)&cfg;
    arg[2] = 1;

    ret = ioctl(o->disp_fd, DISP_LAYER_SET_CONFIG2, (void *)arg);
    if (ret != 0) {
        static int shown;
        if (shown++ < 3)
            fprintf(stderr, "[osd] flush 失败 ret=%d errno=%s\n",
                    ret, strerror(errno));
        return -1;
    }
    return 0;
}