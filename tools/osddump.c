/* 渲染一帧 OSD 画布到本地文件，验证文字确实画上去了（不依赖屏幕） */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "osd.h"
#include "font.h"

/* 用 memops 需要 vdecoder，这里不连硬件，直接用 malloc 模拟画布 */
int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    int W = 210, H = 111;
    unsigned char *buf = calloc(1, (size_t)W * H * 4);
    char line[128];

    /* 直接调用 osd 的绘制逻辑是静态的，这里复刻一遍最小绘制，
     * 只为确认字形数据和位置对不对 */
    #define PUT(x,y,c,a) do { \
        if ((x)>=0&&(x)<W&&(y)>=0&&(y)<H) \
            ((unsigned int*)buf)[(y)*W+(x)] = ((unsigned)(a)<<24)|((c)&0xffffff); \
    } while(0)

    /* 面板 */
    for (int j = 0; j < H; j++)
        for (int i = 0; i < W; i++)
            PUT(i, j, 0x000000, 150);
    for (int i = 0; i < W; i++) { PUT(i,0,0x60c0ff,200); PUT(i,H-1,0x60c0ff,200); }
    for (int j = 0; j < H; j++) { PUT(0,j,0x60c0ff,200); PUT(W-1,j,0x60c0ff,200); }

    /* 三行字 */
    const char *l1 = "1280x720 H264";
    const char *l2 = "fps 25.0 / 25.0";
    const char *l3 = "00:12 / 02:50  x2";
    const char *l4 = "cpu 42% dec 509";
    const char *l5 = "mem 75/108M free 9M";
    const char *ls[5] = {l1, l2, l3, l4, l5};
    unsigned int cs[5] = {0xffffff, 0x7cf07c, 0x8cc8ff, 0xffe066, 0xc0a0ff};
    int ys[5] = {5, 24, 43, 62, 81};

    for (int r = 0; r < 5; r++) {
        int x = 6;
        for (const char *s = ls[r]; *s; s++) {
            unsigned char ch = (unsigned char)*s;
            if (ch < FONT_FIRST || ch > FONT_LAST) { x += FONT_W; continue; }
            const unsigned char *g = font_map[ch - FONT_FIRST];
            for (int gy = 0; gy < FONT_H; gy++)
                for (int gx = 0; gx < FONT_W; gx++)
                    if (g[gy*FONT_BPR + (gx>>3)] & (0x80>>(gx&7)))
                        PUT(x+gx, ys[r]+gy, cs[r], 255);
            x += FONT_W;
        }
    }

    /* 存成 RGBA 转换后的 PPM 便于直接看 */
    FILE *fp = fopen("/tmp/osd.ppm", "wb");
    fprintf(fp, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W*H; i++) {
        unsigned int p = ((unsigned int*)buf)[i];
        unsigned int a = (p>>24)&0xff;
        unsigned char r = (p>>16)&0xff, g2 = (p>>8)&0xff, b = p&0xff;
        /* 叠在深灰背景上看效果 */
        unsigned char bg = 60;
        unsigned char or_ = (unsigned char)((r*a + bg*(255-a))/255);
        unsigned char og = (unsigned char)((g2*a + bg*(255-a))/255);
        unsigned char ob = (unsigned char)((b*a + bg*(255-a))/255);
        fputc(or_, fp); fputc(og, fp); fputc(ob, fp);
    }
    fclose(fp);
    printf("[osddump] 已写 /tmp/osd.ppm\n");


    free(buf);
    return 0;
}
