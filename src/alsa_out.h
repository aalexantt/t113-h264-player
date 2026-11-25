/*
 * alsa_out.h — 直接驱动 ALSA PCM 设备播放 16bit 立体声
 *
 * 为什么不用 libasound：
 *   板上只有 libasound.so.2（运行时库），既没有 .so 开发链接符号，
 *   也没有 alsa/asoundlib.h 头文件。交叉编译时 -lasound 会失败。
 *   而 /dev/snd/pcmC0D0p 的 ioctl 接口很稳定，自己定义结构体直接调
 *   更省事，也和本项目 disp 部分"直接 ioctl"的风格一致。
 *
 * 只实现播放需要的子集：
 *   - 打开 PCM 播放设备
 *   - 设置 16bit 小端、双声道、指定采样率
 *   - 写 PCM 数据（阻塞式，用于控制节奏）
 *   - 关掉设备
 */

#ifndef ALSA_OUT_H
#define ALSA_OUT_H

#include <stdint.h>

typedef struct {
    int      fd;
    unsigned rate;          /* 采样率 */
    unsigned channels;      /* 声道数 */
    int      opened;
    /* 统计 */
    uint64_t bytes_written;
    int64_t  t_start;       /* 开始播放的时刻（微秒），用于计算已播时长 */
    uint64_t frames_written;
} alsa_out_t;

/* 打开 PCM 播放设备。device 形如 "/dev/snd/pcmC0D0p"。
 * rate 采样率，channels 声道数。返回 0 成功。 */
int alsa_open(alsa_out_t *a, const char *device, unsigned rate,
              unsigned channels);

/* 写 PCM 数据（S16_LE 交织）。阻塞直到写完。
 * 返回实际写入字节数，负值表示错误。 */
int alsa_write(alsa_out_t *a, const void *data, unsigned bytes);

/* 已播放的时长（毫秒）。按写入字节数和采样率算，不是墙上时间。
 * 音频时钟就靠这个值驱动视频。 */
int64_t alsa_played_ms(const alsa_out_t *a);

/* 缓冲区里还有多少没播完（毫秒） */
int64_t alsa_buffered_ms(const alsa_out_t *a);

void alsa_close(alsa_out_t *a);

#endif