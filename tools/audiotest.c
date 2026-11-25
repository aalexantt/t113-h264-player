/*
 * audiotest.c — 音频输出通路测试
 *
 * 用途：在还没有接入 AAC 解码之前，先用合成波形验证"声卡能不能出声"。
 * 这样能把「音频通路问题」和「解码问题」分开——和当初用 dumppic
 * 区分「解码问题/显示问题」是同一个思路。
 *
 * 用法：
 *   audiotest              # 440Hz 正弦，3 秒
 *   audiotest 1000 5       # 1000Hz，5 秒
 *   audiotest left         # 只左声道（判断左右接线）
 *   audiotest right        # 只右声道
 *   audiotest sweep        # 扫频 100~8000Hz，能听出频响
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "alsa_out.h"

#define DEVICE   "/dev/snd/pcmC0D0p"
#define RATE     48000
#define CH       2

/* 每块 20ms，够小以保证节流及时 */
#define BLOCK_FRAMES  (RATE / 50)
#define AMPLITUDE     14000      /* 约 -7dBFS，避免削顶又不至于太小 */

static int play_seconds(const char *mode, double freq, int seconds)
{
    alsa_out_t a;
    static short buf[BLOCK_FRAMES * CH];
    int blocks, i, b;
    int64_t phase = 0;

    if (alsa_open(&a, DEVICE, RATE, CH) != 0)
        return 1;

    blocks = seconds * 50;

    for (b = 0; b < blocks; b++) {
        for (i = 0; i < BLOCK_FRAMES; i++) {
            double f = freq;

            /* 扫频模式：频率随进度线性上升 */
            if (strcmp(mode, "sweep") == 0) {
                double t = (double)(b * BLOCK_FRAMES + i) / (double)(blocks * BLOCK_FRAMES);
                f = 100.0 + t * 7900.0;
            }

            {
                double ph = 2.0 * M_PI * f * (double)phase / (double)RATE;
                short v = (short)(AMPLITUDE * sin(ph));

                if (strcmp(mode, "left") == 0) {
                    buf[i * 2 + 0] = v;
                    buf[i * 2 + 1] = 0;
                } else if (strcmp(mode, "right") == 0) {
                    buf[i * 2 + 0] = 0;
                    buf[i * 2 + 1] = v;
                } else {
                    buf[i * 2 + 0] = v;
                    buf[i * 2 + 1] = v;
                }
                phase++;
            }
        }

        if (alsa_write(&a, buf, sizeof(buf)) < 0) {
            fprintf(stderr, "写入失败，第 %d 块\n", b);
            break;
        }
    }

    printf("完成：%d 秒，%llu 字节\n",
           seconds, (unsigned long long)a.bytes_written);
    alsa_close(&a);
    return 0;
}

int main(int argc, char **argv)
{
    const char *mode = "sine";
    double freq = 440.0;
    int seconds = 3;

    if (argc >= 2) {
        if (strcmp(argv[1], "left") == 0 || strcmp(argv[1], "right") == 0 ||
            strcmp(argv[1], "sweep") == 0) {
            mode = argv[1];
            if (strcmp(mode, "sweep") == 0)
                seconds = (argc >= 3) ? atoi(argv[2]) : 5;
        } else {
            freq = atof(argv[1]);
            if (argc >= 3)
                seconds = atoi(argv[2]);
        }
    }

    printf("模式=%s 频率=%.1fHz 时长=%d秒\n", mode, freq, seconds);
    return play_seconds(mode, freq, seconds);
}