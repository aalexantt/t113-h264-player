/* dcol — disp 图层纯色测试工具
 *
 * 用途：判断某个 channel/layer 的显示通路是否可用。
 *
 * 为什么需要：当前问题是"ch0 上送 NV12 视频层不显示，ch1 上的 OSD 层正常"。
 * 无法区分到底是
 *   (a) ch0 通路本身死了（VI 通道没使能 / iommu 没映射）
 *   (b) NV12 + dma-buf 的导入有问题
 *
 * LAYER_MODE_COLOR 不需要任何 buffer，直接让硬件输出一块纯色。
 * 如果连纯色都不显示 → 说明是 (a)，通路问题，跟 NV12 无关。
 * 如果纯色能显示     → 说明是 (b)，buffer/fd/格式问题。
 *
 * 用法：
 *   dcol <channel> <layer> <color> [zorder]
 *   color 用 0xRRGGBB
 *
 * 例：
 *   dcol 0 0 0xFF0000      ch0/lyr0 显示纯红
 *   dcol 1 2 0x00FF00      ch1/lyr2 显示纯绿
 *   dcol 0 0 0xFF0000 20   同时指定 zorder
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>

/* 内核头 sunxi_display2.h 用了 u8/u16/u32/u64 这些内核类型，
 * 用户空间没有定义，得自己补上（同 dispclean.c 的做法）。 */
typedef uint64_t u64;
typedef int64_t  s64;
typedef uint32_t u32;
typedef int32_t  s32;
typedef uint16_t u16;
typedef int16_t  s16;
typedef uint8_t  u8;
typedef int8_t   s8;

#include <video/sunxi_display2.h>

#define DISP_DEV "/dev/disp"

/* LCD 挂在 screen 0 上。这个宏在内核头里没有，player.c 里也是自己定义的。 */
#define DISP_SCREEN 0

static int set_color_layer(int fd, unsigned ch, unsigned lyr,
                           unsigned color, unsigned zorder, int enable)
{
    struct disp_layer_config2 cfg;
    unsigned long arg[4];
    int ret;

    memset(&cfg, 0, sizeof(cfg));
    cfg.channel  = (int)ch;
    cfg.layer_id = (int)lyr;
    cfg.enable   = (bool)enable;
    cfg.info.mode        = LAYER_MODE_COLOR;
    cfg.info.zorder      = (unsigned char)zorder;
    cfg.info.alpha_mode  = 1;        /* 全局 alpha */
    cfg.info.alpha_value = 0xff;
    cfg.info.color       = color;

    /* 铺满全屏。宽度高度用 ioctl 读，避免写死。 */
    arg[0] = (unsigned long)DISP_SCREEN;
    cfg.info.screen_win.x = 0;
    cfg.info.screen_win.y = 0;
    cfg.info.screen_win.width  = (unsigned int)ioctl(fd, DISP_GET_SCN_WIDTH, (void *)arg);
    cfg.info.screen_win.height = (unsigned int)ioctl(fd, DISP_GET_SCN_HEIGHT, (void *)arg);
    if ((int)cfg.info.screen_win.width <= 0 || (int)cfg.info.screen_win.height <= 0) {
        cfg.info.screen_win.width  = 1920;
        cfg.info.screen_win.height = 1080;
    }

    memset(arg, 0, sizeof(arg));
    arg[0] = (unsigned long)DISP_SCREEN;
    arg[1] = (unsigned long)&cfg;
    arg[2] = 1;                      /* 配置项个数 */

    ret = ioctl(fd, DISP_LAYER_SET_CONFIG2, (void *)arg);
    if (ret != 0) {
        fprintf(stderr, "ch%u/lyr%u %s 失败: ret=%d errno=%s\n",
                ch, lyr, enable ? "使能" : "关闭", ret, strerror(errno));
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int fd, ch, lyr, enable = 1, zorder = 20;
    unsigned color;

    if (argc < 3) {
        fprintf(stderr,
                "usage: %s <channel> <layer> [0xRRGGBB|-] [zorder]\n"
                "  给 '-' 表示关闭该层\n"
                "例：\n"
                "  %s 0 0 0xFF0000      ch0/lyr0 纯红，zorder 20\n"
                "  %s 1 2 0x00FF00 25   ch1/lyr2 纯绿，zorder 25\n"
                "  %s 0 0 -            关闭 ch0/lyr0\n",
                argv[0], argv[0], argv[0], argv[0]);
        return 2;
    }

    ch  = atoi(argv[1]);
    lyr = atoi(argv[2]);

    if (argc >= 4 && strcmp(argv[3], "-") != 0) {
        color = (unsigned)strtoul(argv[3], NULL, 0);
    } else if (argc >= 4) {
        enable = 0;
        color  = 0;
    } else {
        color = 0xFF0000;
    }

    if (argc >= 5)
        zorder = atoi(argv[4]);

    fd = open(DISP_DEV, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open %s 失败: %s\n", DISP_DEV, strerror(errno));
        return 1;
    }

    if (set_color_layer(fd, (unsigned)ch, (unsigned)lyr,
                        color, (unsigned)zorder, enable) != 0) {
        close(fd);
        return 1;
    }

    if (enable)
        printf("ch%d/lyr%d 已显示 0x%06X（zorder %d）\n", ch, lyr, color, zorder);
    else
        printf("ch%d/lyr%d 已关闭\n", ch, lyr);

    close(fd);
    return 0;
}