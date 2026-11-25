/*
 * adec.h — AAC 音频解码 + 播放
 *
 * 用 libadecoder.so 的 cedarc 音频解码器解 AAC，PCM 直接写 ALSA 设备。
 *
 * 和视频解码器的关系：两者完全独立，各跑一个线程，通过共享的时钟基准
 * 做同步——音频是主时钟，视频跟着音频走。
 *
 * 为什么音频做主时钟：
 *   音频有硬性的采样率约束，写得快了会 underrun 断音，慢了会积压延迟。
 *   视频帧是"可以丢可以等"的，让视频迁就音频才是正确做法。
 *   （这也是为什么纯视频播放时可以自推时钟，但一加音频就必须换。）
 */

#ifndef ADEC_H
#define ADEC_H

#include <stdint.h>
#include "mp4parse.h"

typedef struct adec_s adec_t;

/* 打开音频解码。成功返回非 NULL。
 * track 是 mp4_audio_track_t，含 AAC 的 AudioSpecificConfig。 */
adec_t *adec_open(const mp4_audio_track_t *track);

/* 送一个 AAC 帧（裸 AAC，不带 ADTS）。返回 0 成功。 */
int adec_submit(adec_t *d, const void *data, int len, int64_t pts);

/* 解一帧并送 ALSA。返回解出的 PCM 字节数，0 表示还没数据，负值错误。 */
int adec_decode_and_play(adec_t *d);

/* 已播放到的时间（毫秒）。音频时钟就用这个值。 */
int64_t adec_clock_ms(adec_t *d);

/* 是否还有未播完的数据 */
int adec_pending(adec_t *d);

void adec_close(adec_t *d);

#endif