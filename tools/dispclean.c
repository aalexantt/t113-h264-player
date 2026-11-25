/*
 * dispclean.c — 清理测试残留的视频层 / OSD 层
 *
 * 【重要】不要关系统 UI 层 ch1/lyr0。
 *
 * 之前这里无差别遍历 ch0~3 / lyr0~3 全部 disable，结果把系统的
 * fbdev 层（ch1/lyr0，zorder 16）也关了，屏幕直接黑掉，
 * 而且程序退出后也不会自动恢复。恢复办法：
 *     echo 0 > /sys/class/graphics/fb0/blank
 *
 * 现在的策略：
 *   - ch0 全通道：视频层，可以全部关闭（这是我们自己用的）
 *   - ch1 lyr0：保留，那是系统 UI / fbdev
 *   - ch1 lyr1..3：OSD 层，可以关闭
 *   - ch2/ch3：不碰
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>

/* 内核头需要的类型 */
typedef uint64_t u64;
typedef int64_t  s64;
typedef uint32_t u32;
typedef int32_t  s32;
typedef uint16_t u16;
typedef int16_t  s16;
typedef uint8_t  u8;
typedef int8_t   s8;

#include <video/sunxi_display2.h>

#define VID_CHANNEL   0    /* 视频通道，可全清 */
#define UI_CHANNEL    1    /* UI 通道 */
#define UI_KEEP_LAYER 0    /* ch1/lyr0 是系统层，必须保留 */

static int disable_layer(int fd, int ch, int lyr)
{
    struct disp_layer_config2 cfg;
    unsigned long arg[4];

    memset(&cfg, 0, sizeof(cfg));
    cfg.channel = ch;
    cfg.layer_id = lyr;
    cfg.enable = 0;
    memset(arg, 0, sizeof(arg));
    arg[0] = 0;
    arg[1] = (unsigned long)&cfg;
    arg[2] = 1;
    return ioctl(fd, DISP_LAYER_SET_CONFIG2, (void *)arg);
}

int main(int argc, char **argv)
{
    int fd, lyr, n = 0;

    fd = open("/dev/disp", O_RDWR);
    if (fd < 0) {
        perror("open /dev/disp");
        return 1;
    }

    (void)argc; (void)argv;

    /* 视频通道全清 */
    for (lyr = 0; lyr < 4; lyr++) {
        if (disable_layer(fd, VID_CHANNEL, lyr) == 0) {
            printf("[clean] 关闭 ch[%d] lyr[%d]\n", VID_CHANNEL, lyr);
            n++;
        }
    }

    /* UI 通道：跳过 lyr0（系统层），清 lyr1..3 */
    for (lyr = 0; lyr < 4; lyr++) {
        if (lyr == UI_KEEP_LAYER) {
            printf("[clean] 保留 ch[%d] lyr[%d]（系统 UI 层）\n",
                   UI_CHANNEL, lyr);
            continue;
        }
        if (disable_layer(fd, UI_CHANNEL, lyr) == 0) {
            printf("[clean] 关闭 ch[%d] lyr[%d]\n", UI_CHANNEL, lyr);
            n++;
        }
    }

    close(fd);
    printf("[clean] 共关闭 %d 层。屏幕若仍黑，执行：\n", n);
    printf("        echo 0 > /sys/class/graphics/fb0/blank\n");
    return 0;
}