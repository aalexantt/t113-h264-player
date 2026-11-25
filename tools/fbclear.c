/* fbclear — 清空 fb0 显存，抹掉开机 LOGO 残留
 *
 * 【要解决的问题】
 * 开机时 u-boot 会把 LOGO 打到 framebuffer 里。内核启动后虽然
 * "free logo buffer" 释放了那块内存，但 fb0（ch1/lyr0）这一层
 * 的内容没有被清掉，LOGO 就一直留在屏幕上。
 *
 * 因为 fb0 层是逐像素 alpha（a[pixel 255]），LOGO 那些像素的
 * alpha 不透明，就会盖在视频层上面 —— 实测表现为屏幕中央一个
 * 黑底彩色文字的方框，四周视频正常。
 *
 * 【做法】
 * 把整个 fb0 写成 0 字节。BGRA 全 0 = 全透明黑，
 * 于是这一层什么都不显示，视频就能完整露出来。
 *
 * 【为什么不用 RGB 色值清屏】
 * 写成不透明黑（alpha=255）会把视频整个遮住；必须 alpha=0。
 *
 * 用法：
 *   fbclear            清空 fb0（按 /sys 读到的分辨率）
 *   fbclear -s         静默（只在出错时输出）
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define FB_DEV   "/dev/fb0"
#define FB_SYS_W "/sys/class/graphics/fb0/virtual_size"
#define FB_SYS_B "/sys/class/graphics/fb0/bits_per_pixel"

/* 读 fb0 的分辨率和色深。
 * 用 sysfs 而不是 FBIOGET_VSCREENINFO，少引一个头文件，
 * 而且 virtual_size 就是显存的真实布局尺寸，比 var 更可靠。 */
static int fb_size(int *w, int *h, int *bpp)
{
    FILE *f;

    *w = *h = 0;

    f = fopen(FB_SYS_W, "r");
    if (!f)
        return -1;
    if (fscanf(f, "%d,%d", w, h) != 2) {
        fclose(f);
        return -1;
    }
    fclose(f);

    *bpp = 32;
    f = fopen(FB_SYS_B, "r");
    if (f) {
        if (fscanf(f, "%d", bpp) != 1)
            *bpp = 32;
        fclose(f);
    }

    if (*w <= 0 || *h <= 0 || *bpp <= 0)
        return -1;

    return 0;
}

int main(int argc, char **argv)
{
    int w, h, bpp, fd, quiet = 0;
    size_t bytes;
    void *map;

    if (argc >= 2 && strcmp(argv[1], "-s") == 0)
        quiet = 1;

    if (fb_size(&w, &h, &bpp) != 0) {
        fprintf(stderr, "fbclear: 无法读取 %s 的尺寸\n", FB_DEV);
        return 1;
    }

    bytes = (size_t)w * (size_t)h * (size_t)(bpp / 8);

    fd = open(FB_DEV, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "fbclear: open %s 失败: %s\n", FB_DEV, strerror(errno));
        return 1;
    }

    /* 直接 mmap 而不是 write()。
     * write() 在 fbdev 上会走 console 的 scroll 逻辑，慢且可能只清一部分；
     * mmap 后 memset 是直接改显存，最可靠。 */
    map = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        fprintf(stderr, "fbclear: mmap 失败: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    memset(map, 0, bytes);

    munmap(map, bytes);
    close(fd);

    if (!quiet)
        printf("fbclear: 已清空 %dx%d/%dbpp（%zu 字节，全透明）\n",
               w, h, bpp, bytes);

    return 0;
}