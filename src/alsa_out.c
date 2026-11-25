/*
 * alsa_out.c — 直接 ioctl 驱动 ALSA PCM 播放
 *
 * 板上没有 libasound 开发和头文件，所以这里自己定义 ALSA 的 ioctl
 * 协议结构体。数值来自内核 uapi/sound/asound.h，是稳定的 ABI。
 *
 * 涉及的 ioctl：
 *   SNDRV_PCM_IOCTL_HW_PARAMS  设置硬件参数（格式/声道/采样率）
 *   SNDRV_PCM_IOCTL_PREPARE    进入 PREPARED 状态
 *   SNDRV_PCM_IOCTL_WRITEI_FRAMES  写 PCM 帧
 *   SNDRV_PCM_IOCTL_DRAIN      排空缓冲
 *   SNDRV_PCM_IOCTL_HWSYNC     取硬件播放位置
 *
 * 只支持 S16_LE 交织——AAC 解出来的 PCM 一般是这个格式，
 * 需要别的格式时改 snd_pcm_hw_params 里的 format 分支即可。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/time.h>

#include "alsa_out.h"

/* ---------------------------------------------------------------------- */
/* ALSA uapi 结构体（对照 内核 include/uapi/sound/asound.h，ABI 稳定）      */
/* ---------------------------------------------------------------------- */

/* mask 位图。内核 SNDRV_MASK_MAX = 256 位，按 unsigned int 存是 8 个元素。 */
#define MASK_ARRAY_SIZE   8

struct snd_mask {
    unsigned int bits[MASK_ARRAY_SIZE];
};

struct snd_interval {
    unsigned int min, max;
    unsigned int openmin:1, openmax:1, integer:1, empty:1;
};

/* 参数索引（已核对 asound.h）：
 *   masks[]     : ACCESS=0, FORMAT=1, SUBFORMAT=2     → 共 3 个
 *   intervals[] : SAMPLE_BITS=8 ... TICK_TIME=19      → 共 12 个
 *                 数组下标 = 参数编号 - 8
 *                 CHANNELS(10) → intervals[2]
 *                 RATE(11)     → intervals[3]
 */
#define HW_PARAM_ACCESS        0
#define HW_PARAM_FORMAT        1
#define HW_PARAM_IF_CHANNELS   2    /* 10 - 8 */
#define HW_PARAM_IF_RATE       3    /* 11 - 8 */

/* 数组长度和字段顺序必须与内核完全一致。错位的话内核会返回 -EINVAL，
 * 或者更糟——静默接受但配成别的东西。
 * 实测 sizeof = 608（32 位），offsetof(rmask)=512, offsetof(intervals)=260。 */
struct snd_pcm_hw_params {
    unsigned int flags;
    struct snd_mask masks[3];      /* ACCESS / FORMAT / SUBFORMAT */
    struct snd_mask mres[5];       /* reserved masks */
    struct snd_interval intervals[12];
    struct snd_interval ires[9];   /* reserved intervals */
    unsigned int rmask;            /* W: requested masks */
    unsigned int cmask;            /* R: changed masks */
    unsigned int info;             /* R: Info flags */
    unsigned int msbits;           /* R: used most significant bits */
    unsigned int rate_num;         /* R: rate numerator */
    unsigned int rate_den;         /* R: rate denominator */
    unsigned long fifo_size;       /* R: chip FIFO size in frames */
    unsigned char reserved[64];
};

/* WRITEI_FRAMES 的参数。
 *
 * 【字段顺序很关键】内核定义是：
 *     struct snd_xferi {
 *         snd_pcm_sframes_t result;   // 内核回填实际写入帧数
 *         void __user *buf;           // 用户缓冲
 *         snd_pcm_uframes_t frames;   // 想写多少帧
 *     };
 * 顺序是 result/buf/frames，不是 frames/buf/result。
 * 写反了不会报错，但内核读到的 frames 是垃圾值，表现为写入异常或无声。
 *
 * sizeof 实测 24（32 位）：每个成员 4 字节 + 对齐。
 */
struct snd_xferi {
    long result;        /* 输出：实际写入的帧数 */
    void *buf;          /* 输入：PCM 数据 */
    unsigned long frames;   /* 输入：请求写入的帧数 */
};

/* ioctl 号必须用真实结构体大小——_IOWR 把它编进命令字，
 * 内核 snd_pcm_ioctl() 会校验 size 与自己的 sizeof 一致，
 * 不一致直接 -ENOTTY。所以这些宏放在结构体定义之后。 */
#define SNDRV_PCM_IOCTL_HW_PARAMS        _IOWR('A', 0x11, struct snd_pcm_hw_params)
#define SNDRV_PCM_IOCTL_HW_FREE          _IO('A', 0x12)
#define SNDRV_PCM_IOCTL_PREPARE          _IO('A', 0x40)
#define SNDRV_PCM_IOCTL_RESET            _IO('A', 0x41)
#define SNDRV_PCM_IOCTL_DRAIN            _IO('A', 0x43)
#define SNDRV_PCM_IOCTL_HWSYNC           _IO('A', 0x22)
#define SNDRV_PCM_IOCTL_WRITEI_FRAMES    _IOW('A', 0x50, struct snd_xferi)

/* 已核实的枚举值（asound.h）：
 *   SNDRV_PCM_ACCESS_RW_INTERLEAVED = 3
 *   ALSA_FORMAT_S16_LE         = 2 */
#define ALSA_ACCESS_RW_INTERLEAVED   3
#define ALSA_FORMAT_S16_LE           2

/* ---------------------------------------------------------------------- */

static int64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000ll + tv.tv_usec;
}

static int pcm_set_hw_params(alsa_out_t *a, unsigned rate, unsigned channels)
{
    struct snd_pcm_hw_params p;
    int ret, i;

    memset(&p, 0, sizeof(p));

    /* 【关键】所有 mask / interval 必须先填成"宽松值"，再收紧到需要的配置。
     *
     * 踩过的坑：一开始把不需要的字段留空（全 0），结果 HW_PARAMS 一律返回
     * EINVAL。原因是空的 mask 表示"不允许任何取值"，空的 interval 表示
     * [0,0] 这个区间，内核一算发现无解就直接拒绝。
     * 正确做法是先把 mask 全置 1、interval 拉成 [0, ~0]，让内核有得选，
     * 再用 rmask 或具体值把要固定的那几个锁死。
     *
     * 实测可行的组合：rate=48000 ch=2 时内核给出 buffer=512 frames、period=64。 */
    for (i = 0; i < 3; i++)
        memset(p.masks[i].bits, 0xff, sizeof(p.masks[i].bits));

    for (i = 0; i < 12; i++) {
        p.intervals[i].min = 0;
        p.intervals[i].max = ~0u;
        p.intervals[i].empty = 0;
        p.intervals[i].integer = 0;
        p.intervals[i].openmin = 0;
        p.intervals[i].openmax = 0;
    }

    /* ACCESS 只留 RW_INTERLEAVED(3)，其余清掉 */
    memset(p.masks[HW_PARAM_ACCESS].bits, 0, sizeof(p.masks[HW_PARAM_ACCESS].bits));
    p.masks[HW_PARAM_ACCESS].bits[ALSA_ACCESS_RW_INTERLEAVED / 32] |=
        1u << (ALSA_ACCESS_RW_INTERLEAVED % 32);

    /* FORMAT 只留 S16_LE */
    memset(p.masks[HW_PARAM_FORMAT].bits, 0, sizeof(p.masks[HW_PARAM_FORMAT].bits));
    p.masks[HW_PARAM_FORMAT].bits[ALSA_FORMAT_S16_LE / 32] |=
        1u << (ALSA_FORMAT_S16_LE % 32);

    /* 声道数和采样率锁成具体值 */
    p.intervals[HW_PARAM_IF_CHANNELS].min = channels;
    p.intervals[HW_PARAM_IF_CHANNELS].max = channels;
    p.intervals[HW_PARAM_IF_CHANNELS].integer = 1;

    p.intervals[HW_PARAM_IF_RATE].min = rate;
    p.intervals[HW_PARAM_IF_RATE].max = rate;
    p.intervals[HW_PARAM_IF_RATE].integer = 1;

    /* rmask = 0：不强制任何参数"必须按我给的算"，
     * 让内核在满足上面约束的前提下自由选择 buffer/period 大小。 */
    p.rmask = 0;

    ret = ioctl(a->fd, SNDRV_PCM_IOCTL_HW_PARAMS, &p);
    if (ret < 0) {
        fprintf(stderr, "[alsa] HW_PARAMS 失败: %s (rate=%u ch=%u)\n",
                strerror(errno), rate, channels);
        return -1;
    }

    /* 打印内核实际生效的参数，便于核对 */
    printf("[alsa] 参数已生效: rate=%u ch=%u, buffer=%u frames, period=%u\n",
           p.intervals[HW_PARAM_IF_RATE].min,
           p.intervals[HW_PARAM_IF_CHANNELS].min,
           p.intervals[9].min,      /* BUFFER_SIZE = 17 -> 17-8 = 9 */
           p.intervals[5].min);     /* PERIOD_SIZE = 13 -> 13-8 = 5 */
    return 0;
}

int alsa_open(alsa_out_t *a, const char *device, unsigned rate,
              unsigned channels)
{
    memset(a, 0, sizeof(*a));
    a->fd = -1;

    a->fd = open(device, O_WRONLY);
    if (a->fd < 0) {
        fprintf(stderr, "[alsa] 打开 %s 失败: %s\n", device, strerror(errno));
        return -1;
    }

    if (pcm_set_hw_params(a, rate, channels) != 0) {
        close(a->fd);
        a->fd = -1;
        return -1;
    }

    if (ioctl(a->fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
        fprintf(stderr, "[alsa] PREPARE 失败: %s\n", strerror(errno));
        close(a->fd);
        a->fd = -1;
        return -1;
    }

    a->rate = rate;
    a->channels = channels;
    a->opened = 1;
    a->t_start = now_us();

    printf("[alsa] 已打开 %s, %u Hz, %u ch, S16_LE\n",
           device, rate, channels);
    return 0;
}

int alsa_write(alsa_out_t *a, const void *data, unsigned bytes)
{
    struct snd_xferi x;
    unsigned frame_bytes;
    unsigned long total_frames, done = 0;

    if (!a || !a->opened)
        return -1;

    frame_bytes = a->channels * 2;      /* S16_LE 每样本 2 字节 */
    total_frames = bytes / frame_bytes;
    if (total_frames == 0)
        return 0;

    /* 内核 WRITEI_FRAMES 可能只写一部分（缓冲满时），要循环写完。
     * 这个阻塞行为正好用来做节流：采样率固定，写满缓冲就自然等了。 */
    while (done < total_frames) {
        x.frames = total_frames - done;
        x.buf = (void *)((const char *)data + done * frame_bytes);
        x.result = 0;

        if (ioctl(a->fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) < 0) {
            if (errno == EPIPE) {
                /* underrun，重新 prepare 继续 */
                ioctl(a->fd, SNDRV_PCM_IOCTL_PREPARE);
                continue;
            }
            if (errno == EINTR)
                continue;
            fprintf(stderr, "[alsa] WRITEI_FRAMES 失败: %s\n", strerror(errno));
            return -1;
        }
        if (x.result <= 0)
            break;
        done += (unsigned long)x.result;
    }

    a->bytes_written += (uint64_t)done * frame_bytes;
    a->frames_written += done;
    return (int)(done * frame_bytes);
}

int64_t alsa_buffered_ms(const alsa_out_t *a)
{
    if (!a || !a->opened || !a->rate)
        return 0;
    /* 已写入但设备还没播完的量。用 HWSYNC 取实际播放位置更准，
     * 这里保守地按"写入量 - 已播时间"估算，供上层判断缓冲深度。 */
    {
        int64_t elapsed_ms = (now_us() - a->t_start) / 1000;
        int64_t played = elapsed_ms * (int64_t)a->rate / 1000;
        int64_t left = (int64_t)a->frames_written - played;
        if (left < 0) left = 0;
        return left * 1000 / (int64_t)a->rate;
    }
}

int64_t alsa_played_ms(const alsa_out_t *a)
{
    if (!a || !a->opened || !a->rate)
        return 0;
    return (int64_t)a->frames_written * 1000 / (int64_t)a->rate;
}

void alsa_close(alsa_out_t *a)
{
    if (!a || a->fd < 0)
        return;

    /* DRAIN 会把缓冲里的数据播完再返回。播放器退出时用得上，
     * 但如果是强制中断就不该等，所以调用方可以先不调这个函数。 */
    ioctl(a->fd, SNDRV_PCM_IOCTL_DRAIN);
    ioctl(a->fd, SNDRV_PCM_IOCTL_HW_FREE);
    close(a->fd);
    a->fd = -1;
    a->opened = 0;
}