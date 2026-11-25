/*
 * mp4parse.h — 极简 MP4 解封装，只为 POC 服务
 *
 * 为什么不用 ffmpeg：板上没有 libavformat。为什么不用 libdemuxer：
 * SDK 里的 CdxMovParser 是给 xplayer 用的，接口复杂且要拉一堆依赖。
 *
 * 这个解析器只做 POC 需要的最小集：
 *   - 读 moov/trak/mdia/minf/stbl 拿到 stsd(avcC/hvcC)、stts、stsc、stsz、stco
 *   - 顺序吐出 sample 数据和 pts
 *   - 把 avcC 转成 AnnexB 的 SPS/PPS
 *
 * 不支持：分片 MP4、加密、多轨（只取第一个 video trak）
 */

#ifndef MP4PARSE_H
#define MP4PARSE_H

#include <stdint.h>

typedef struct {
    const uint8_t *data;
    uint32_t len;
    int64_t pts;        /* 单位 90kHz */
} mp4_sample_t;

typedef struct mp4_ctx mp4_ctx_t;

struct mp4_ctx {
    int width;
    int height;
    int codec;              /* 1=h265, 0=h264 */
    uint32_t duration_ms;
    uint32_t timescale;      /* mvhd 的时间基准 */
    /* mdhd 的 timescale —— sample PTS 的真实单位是 1/media_timescale 秒。
     * 和 mvhd 的 timescale 常常不同，算帧率必须用这个。 */
    uint32_t media_timescale;
    uint32_t media_duration_ms;

    uint8_t *extradata;     /* AnnexB 格式的 SPS/PPS */
    int extradata_len;

    /* 内部状态，外部不用管 */
    void *_priv;
};

mp4_ctx_t *mp4_open(const char *path);
void mp4_close(mp4_ctx_t *ctx);
int mp4_next_sample(mp4_ctx_t *ctx, mp4_sample_t *out);
void mp4_rewind(mp4_ctx_t *ctx);
uint32_t mp4_sample_count(const mp4_ctx_t *ctx);

#endif