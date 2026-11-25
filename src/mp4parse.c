/*
 * mp4parse.c — 极简 MP4 解封装实现
 *
 * 只解析 POC 需要的 box：stsd / stts / stsc / stsz / stco / stss
 * 假设：单 video trak、非分片、sample 按 stsc 顺序排列
 *
 * 这个文件不追求通用性，目标是把测试文件的帧按顺序喂给解码器。
 * 真正产品化时应该换成完整解封装（CdxMovParser 或 ffmpeg）。
 */

#include "mp4parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SAMPLES 200000

typedef struct {
    uint32_t count;
    uint32_t delta;
} stts_entry_t;

typedef struct {
    uint32_t first_chunk;
    uint32_t samples_per_chunk;
    uint32_t sample_desc;
} stsc_entry_t;

typedef struct {
    /* 文件 */
    FILE *fp;
    int64_t file_size;

    /* stbl 解析结果 */
    uint32_t *sample_sizes;
    uint32_t sample_count;
    uint32_t *chunk_offsets;
    uint32_t chunk_count;
    stsc_entry_t *stsc;
    uint32_t stsc_count;
    stts_entry_t *stts;
    uint32_t stts_count;
    uint32_t *sync_samples;     /* 1-based */
    uint32_t sync_count;

    /* 逐 sample 遍历的状态 */
    uint32_t cur_sample;        /* 0-based */
    uint32_t cur_chunk;         /* 0-based */
    uint32_t cur_in_chunk;      /* 已从当前 chunk 读了多少 */
    uint32_t cur_stsc;          /* 当前 stsc 条目下标 */
    int64_t cur_pts;
    int64_t cur_dts;

    /* 每个 sample 的 pts 可以预计算，简单起见按 stts 累加 */
    int64_t *sample_pts;

    uint8_t *buf;               /* 读 sample 用的缓冲 */
    size_t buf_cap;
} mp4_priv_t;

/* ---------- 基础读取 ---------- */

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t rd16(const uint8_t *p)
{
    return ((uint32_t)p[0] << 8) | (uint32_t)p[1];
}

static int find_box(FILE *fp, int64_t start, int64_t end,
                    const char *type, int64_t *out_pos, int64_t *out_size)
{
    int64_t pos = start;
    uint8_t hdr[16];

    while (pos + 8 <= end) {
        size_t got;
        if (fseek(fp, (long)pos, SEEK_SET) != 0)
            return -1;
        got = fread(hdr, 1, 8, fp);
        if (got < 8)
            return -1;

        uint64_t size = rd32(hdr);
        char t[5];
        memcpy(t, hdr + 4, 4);
        t[4] = 0;

        int64_t hdrlen = 8;
        if (size == 1) {
            if (fread(hdr + 8, 1, 8, fp) < 8)
                return -1;
            size = ((uint64_t)rd32(hdr + 8) << 32) | rd32(hdr + 12);
            hdrlen = 16;
        } else if (size == 0) {
            size = (uint64_t)(end - pos);
        }
        if (size < (uint64_t)hdrlen || pos + (int64_t)size > end)
            return -1;

        if (strcmp(t, type) == 0) {
            *out_pos = pos;
            *out_size = (int64_t)size;
            return 0;
        }
        pos += (int64_t)size;
    }
    return -1;
}

/* 读整个 box 到内存 */
static uint8_t *read_box(FILE *fp, int64_t pos, int64_t size)
{
    uint8_t *buf = (uint8_t *)malloc((size_t)size);
    if (!buf)
        return NULL;
    if (fseek(fp, (long)pos, SEEK_SET) != 0) {
        free(buf);
        return NULL;
    }
    if (fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        free(buf);
        return NULL;
    }
    return buf;
}

/* ---------- AnnexB extradata ---------- */

/* 把 avcC/hvcC 里的 SPS/PPS 转成 AnnexB（00 00 00 01 前缀） */
static int build_extradata_avc(mp4_ctx_t *ctx, const uint8_t *avcC, uint32_t len)
{
    uint32_t i;
    int out_cap, out_len = 0;
    uint8_t *out;

    if (len < 7)
        return -1;

    out_cap = (int)len * 2 + 64;
    out = (uint8_t *)malloc((size_t)out_cap);
    if (!out)
        return -1;

    /* avcC: ver(1) prof(1) compat(1) level(1) lenSize-1(1) numSPS(5bit) */
    int nsps = avcC[5] & 0x1f;
    uint32_t off = 6;
    for (i = 0; i < (uint32_t)nsps && off + 2 <= len; i++) {
        uint32_t slen = rd16(avcC + off);
        off += 2;
        if (off + slen > len)
            break;
        if (out_len + 4 + (int)slen > out_cap) {
            out_cap *= 2;
            uint8_t *nb = (uint8_t *)realloc(out, (size_t)out_cap);
            if (!nb) { free(out); return -1; }
            out = nb;
        }
        out[out_len++] = 0; out[out_len++] = 0;
        out[out_len++] = 0; out[out_len++] = 1;
        memcpy(out + out_len, avcC + off, slen);
        out_len += (int)slen;
        off += slen;
    }
    if (off < len) {
        int npps = avcC[off++];
        for (i = 0; i < (uint32_t)npps && off + 2 <= len; i++) {
            uint32_t plen = rd16(avcC + off);
            off += 2;
            if (off + plen > len)
                break;
            if (out_len + 4 + (int)plen > out_cap) {
                out_cap *= 2;
                uint8_t *nb = (uint8_t *)realloc(out, (size_t)out_cap);
                if (!nb) { free(out); return -1; }
                out = nb;
            }
            out[out_len++] = 0; out[out_len++] = 0;
            out[out_len++] = 0; out[out_len++] = 1;
            memcpy(out + out_len, avcC + off, plen);
            out_len += (int)plen;
            off += plen;
        }
    }

    ctx->extradata = out;
    ctx->extradata_len = out_len;
    return 0;
}

/* ---------- stbl 解析 ---------- */

static int parse_stbl(mp4_ctx_t *ctx, mp4_priv_t *p,
                      FILE *fp, int64_t stbl_pos, int64_t stbl_size)
{
    int64_t pos, size;

    /* stsd */
    if (find_box(fp, stbl_pos + 8, stbl_pos + stbl_size, "stsd", &pos, &size) == 0) {
        uint8_t *b = read_box(fp, pos, size);
        if (b) {
            /* stsd: ver/flags(4) entry_count(4) then entries */
            uint32_t n = rd32(b + 12);
            if (n > 0) {
                const uint8_t *e = b + 16;
                char et[5];
                memcpy(et, e + 4, 4);
                et[4] = 0;
                if (strcmp(et, "avc1") == 0 || strcmp(et, "avc3") == 0 ||
                    strcmp(et, "hvc1") == 0 || strcmp(et, "hev1") == 0) {
                    /* VisualSampleEntry: 6 reserved + 2 data_ref_idx
                     * + 16 predefined/reserved + width(2) height(2) */
                    ctx->width = (int)rd16(e + 8 + 24);
                    ctx->height = (int)rd16(e + 8 + 26);
                    ctx->codec = (et[0] == 'h') ? 1 : 0;

                    /* 找 avcC / hvcC */
                    const char *cfgname = (ctx->codec == 1) ? "hvcC" : "avcC";
                    uint32_t entry_size = rd32(e);
                    int64_t cpos, csize;
                    /* 子 box 从 entry 的 86 字节后开始（VisualSampleEntry 固定部分） */
                    FILE *sub = fmemopen((void *)e, entry_size, "rb");
                    if (sub) {
                        if (find_box(sub, 86, entry_size, cfgname,
                                     &cpos, &csize) == 0) {
                            if (ctx->codec == 0 && csize > 8)
                                build_extradata_avc(ctx, e + cpos + 8,
                                                    (uint32_t)(csize - 8));
                        }
                        fclose(sub);
                    }
                }
            }
            free(b);
        }
    }

    /* stsz */
    if (find_box(fp, stbl_pos + 8, stbl_pos + stbl_size, "stsz", &pos, &size) == 0) {
        uint8_t *b = read_box(fp, pos, size);
        if (b) {
            uint32_t sample_size = rd32(b + 12);
            uint32_t count = rd32(b + 16);
            if (count > 0 && count < MAX_SAMPLES) {
                p->sample_count = count;
                p->sample_sizes = (uint32_t *)calloc(count, sizeof(uint32_t));
                if (p->sample_sizes) {
                    if (sample_size != 0) {
                        uint32_t i;
                        for (i = 0; i < count; i++)
                            p->sample_sizes[i] = sample_size;
                    } else {
                        uint32_t i;
                        for (i = 0; i < count && (20 + i * 4 + 4) <= (uint32_t)size; i++)
                            p->sample_sizes[i] = rd32(b + 20 + i * 4);
                    }
                }
            }
            free(b);
        }
    }

    /* stco / co64 */
    if (find_box(fp, stbl_pos + 8, stbl_pos + stbl_size, "stco", &pos, &size) == 0) {
        uint8_t *b = read_box(fp, pos, size);
        if (b) {
            uint32_t count = rd32(b + 12);
            if (count > 0 && count < MAX_SAMPLES) {
                p->chunk_count = count;
                p->chunk_offsets = (uint32_t *)calloc(count, sizeof(uint32_t));
                if (p->chunk_offsets) {
                    uint32_t i;
                    for (i = 0; i < count && (16 + i * 4 + 4) <= (uint32_t)size; i++)
                        p->chunk_offsets[i] = rd32(b + 16 + i * 4);
                }
            }
            free(b);
        }
    }

    /* stsc */
    if (find_box(fp, stbl_pos + 8, stbl_pos + stbl_size, "stsc", &pos, &size) == 0) {
        uint8_t *b = read_box(fp, pos, size);
        if (b) {
            uint32_t count = rd32(b + 12);
            if (count > 0 && count < 4096) {
                p->stsc_count = count;
                p->stsc = (stsc_entry_t *)calloc(count, sizeof(stsc_entry_t));
                if (p->stsc) {
                    uint32_t i;
                    for (i = 0; i < count && (16 + i * 12 + 12) <= (uint32_t)size; i++) {
                        p->stsc[i].first_chunk = rd32(b + 16 + i * 12);
                        p->stsc[i].samples_per_chunk = rd32(b + 16 + i * 12 + 4);
                        p->stsc[i].sample_desc = rd32(b + 16 + i * 12 + 8);
                    }
                }
            }
            free(b);
        }
    }

    /* stts */
    if (find_box(fp, stbl_pos + 8, stbl_pos + stbl_size, "stts", &pos, &size) == 0) {
        uint8_t *b = read_box(fp, pos, size);
        if (b) {
            uint32_t count = rd32(b + 12);
            if (count > 0 && count < 65536) {
                p->stts_count = count;
                p->stts = (stts_entry_t *)calloc(count, sizeof(stts_entry_t));
                if (p->stts) {
                    uint32_t i;
                    for (i = 0; i < count && (16 + i * 8 + 8) <= (uint32_t)size; i++) {
                        p->stts[i].count = rd32(b + 16 + i * 8);
                        p->stts[i].delta = rd32(b + 16 + i * 8 + 4);
                    }
                }
            }
            free(b);
        }
    }

    return 0;
}

/* 预计算每个 sample 的 pts（90kHz） */
static void build_pts_table(mp4_ctx_t *ctx, mp4_priv_t *p)
{
    uint32_t i, sidx = 0;
    int64_t t = 0;

    (void)ctx;
    if (!p->stts || p->sample_count == 0)
        return;
    p->sample_pts = (int64_t *)calloc(p->sample_count, sizeof(int64_t));
    if (!p->sample_pts)
        return;

    for (i = 0; i < p->stts_count && sidx < p->sample_count; i++) {
        uint32_t k;
        for (k = 0; k < p->stts[i].count && sidx < p->sample_count; k++) {
            p->sample_pts[sidx++] = t;
            t += p->stts[i].delta;
        }
    }
    while (sidx < p->sample_count)
        p->sample_pts[sidx++] = t;
}

/* ---------- 公开接口 ---------- */

mp4_ctx_t *mp4_open(const char *path)
{
    FILE *fp;
    int64_t fsize, moov_pos, moov_size;
    mp4_ctx_t *ctx;
    mp4_priv_t *p;
    int64_t trak_pos, trak_size, mdia_pos, mdia_size, minf_pos, minf_size;
    int64_t stbl_pos, stbl_size;

    fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "[mp4] open %s failed\n", path);
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    fsize = ftell(fp);
    rewind(fp);

    /* 找 moov（可能在尾部） */
    int found = 0;
    int64_t pos = 0;
    uint8_t hdr[16];
    while (pos + 8 <= fsize) {
        if (fseek(fp, (long)pos, SEEK_SET) != 0) break;
        if (fread(hdr, 1, 8, fp) < 8) break;
        uint64_t sz = rd32(hdr);
        char t[5];
        memcpy(t, hdr + 4, 4); t[4] = 0;
        int64_t hl = 8;
        if (sz == 1) {
            if (fread(hdr + 8, 1, 8, fp) < 8) break;
            sz = ((uint64_t)rd32(hdr + 8) << 32) | rd32(hdr + 12);
            hl = 16;
        } else if (sz == 0) {
            sz = (uint64_t)(fsize - pos);
        }
        if (sz < (uint64_t)hl || pos + (int64_t)sz > fsize) break;
        if (strcmp(t, "moov") == 0) {
            moov_pos = pos; moov_size = (int64_t)sz; found = 1;
            break;
        }
        pos += (int64_t)sz;
    }
    if (!found) {
        fprintf(stderr, "[mp4] no moov found\n");
        fclose(fp);
        return NULL;
    }

    ctx = (mp4_ctx_t *)calloc(1, sizeof(*ctx));
    p = (mp4_priv_t *)calloc(1, sizeof(*p));
    if (!ctx || !p) { free(ctx); free(p); fclose(fp); return NULL; }
    p->fp = fp;
    p->file_size = fsize;
    ctx->_priv = p;

    /* mvhd 拿时长 */
    {
        int64_t vpos, vsize;
        if (find_box(fp, moov_pos + 8, moov_pos + moov_size, "mvhd",
                     &vpos, &vsize) == 0) {
            uint8_t *b = read_box(fp, vpos, vsize);
            if (b && vsize >= 28) {
                uint8_t ver = b[8];
                uint32_t ts, dur;
                if (ver == 0) {
                    ts = rd32(b + 20);
                    dur = rd32(b + 24);
                } else {
                    ts = rd32(b + 28);
                    dur = rd32(b + 32);
                }
                if (ts) {
                    ctx->duration_ms = (uint32_t)((uint64_t)dur * 1000 / ts);
                    /* PTS 的单位是 1/timescale 秒。之前 player.c 里直接
                     * 把 pts 当毫秒用，导致播放速度差得很远。
                     * 这里把 timescale 暴露出去，由调用方正确换算。 */
                    ctx->timescale = ts;
                }
            }
            free(b);
        }
    }

    /* 第一个 trak（假设是 video） */
    if (find_box(fp, moov_pos + 8, moov_pos + moov_size, "trak",
                 &trak_pos, &trak_size) != 0) {
        fprintf(stderr, "[mp4] no trak\n");
        goto fail;
    }
    if (find_box(fp, trak_pos + 8, trak_pos + trak_size, "mdia",
                 &mdia_pos, &mdia_size) != 0)
        goto fail;

    /* 读 mdhd 拿这个 track 自己的 timescale。
     *
     * 这是修帧率的关键：MP4 里有两种 timescale
     *   mvhd.timescale  —— 只管 movie 的 duration
     *   mdhd.timescale  —— 决定这个 track 里 stts/pts 的单位
     * 相邻 sample 的 PTS 增量（stts.delta）用 mdhd.timescale 换算才是秒。
     *
     * 实测这个文件：mvhd.timescale=1000，但 sample PTS 步进 512，
     * 4271 帧 / 170.859s 反推出 mdhd.timescale=12800。
     * 用 1000 去换算会把 512 当 512 毫秒，播放速度只有 25/12.8≈1.95fps 的假象。
     *
     * mdhd 布局（version 0）：
     *   size(4) type(4) version+flags(4) creation(4) modification(4)
     *   timescale(4) duration(4) ...
     * version 1 时 creation/modification 各 8 字节，timescale 位置后移。
     */
    {
        int64_t vpos, vsize;
        if (find_box(fp, mdia_pos + 8, mdia_pos + mdia_size, "mdhd",
                     &vpos, &vsize) == 0) {
            uint8_t *b = read_box(fp, vpos, vsize);
            if (b && vsize >= 24) {
                uint8_t ver = b[8];
                uint32_t mts = 0;
                if (ver == 1) {
                    if (vsize >= 32)
                        mts = rd32(b + 28);
                } else {
                    mts = rd32(b + 20);
                }
                if (mts > 0) {
                    ctx->media_timescale = mts;
                    /* duration 也一起读，version 1 是 8 字节 */
                    if (ver == 1 && vsize >= 40) {
                        uint64_t d = ((uint64_t)rd32(b + 32) << 32) | rd32(b + 36);
                        if (d && mts)
                            ctx->media_duration_ms = (uint32_t)(d * 1000 / mts);
                    } else if (ver == 0 && vsize >= 28) {
                        uint32_t d = rd32(b + 24);
                        if (d && mts)
                            ctx->media_duration_ms = (uint32_t)((uint64_t)d * 1000 / mts);
                    }
                }
            }
            free(b);
        }
    }

    if (find_box(fp, mdia_pos + 8, mdia_pos + mdia_size, "minf",
                 &minf_pos, &minf_size) != 0)
        goto fail;
    if (find_box(fp, minf_pos + 8, minf_pos + minf_size, "stbl",
                 &stbl_pos, &stbl_size) != 0)
        goto fail;

    if (parse_stbl(ctx, p, fp, stbl_pos, stbl_size) != 0)
        goto fail;

    /* mdhd 没读到就用 mvhd 的兜底 */
    if (ctx->media_timescale == 0)
        ctx->media_timescale = ctx->timescale;
    if (ctx->media_duration_ms == 0)
        ctx->media_duration_ms = ctx->duration_ms;

    build_pts_table(ctx, p);

    if (p->sample_count == 0 || !p->chunk_offsets) {
        fprintf(stderr, "[mp4] empty sample table (count=%u chunks=%u)\n",
                p->sample_count, p->chunk_count);
        goto fail;
    }

    return ctx;

fail:
    mp4_close(ctx);
    return NULL;
}

void mp4_close(mp4_ctx_t *ctx)
{
    mp4_priv_t *p;
    if (!ctx)
        return;
    p = (mp4_priv_t *)ctx->_priv;
    if (p) {
        if (p->fp) fclose(p->fp);
        free(p->sample_sizes);
        free(p->chunk_offsets);
        free(p->stsc);
        free(p->stts);
        free(p->sync_samples);
        free(p->sample_pts);
        free(p->buf);
        free(p);
    }
    free(ctx->extradata);
    free(ctx);
}

void mp4_rewind(mp4_ctx_t *ctx)
{
    mp4_priv_t *p;
    if (!ctx)
        return;
    p = (mp4_priv_t *)ctx->_priv;
    if (!p)
        return;
    p->cur_sample = 0;
    p->cur_chunk = 0;
    p->cur_in_chunk = 0;
    p->cur_stsc = 0;
    p->cur_pts = 0;
    p->cur_dts = 0;
}

int mp4_next_sample(mp4_ctx_t *ctx, mp4_sample_t *out)
{
    mp4_priv_t *p;
    uint32_t chunk_idx, offset;
    uint32_t spc;

    if (!ctx || !out)
        return -1;
    p = (mp4_priv_t *)ctx->_priv;
    if (!p || p->cur_sample >= p->sample_count)
        return -1;

    /* 确定当前 sample 在哪个 chunk、chunk 内第几个 */
    if (p->cur_chunk == 0 && p->cur_in_chunk == 0 && p->cur_stsc == 0) {
        /* 初始位置 */
    }

    /* 找到当前 chunk 对应的 samples_per_chunk */
    while (p->cur_stsc + 1 < p->stsc_count &&
           p->stsc[p->cur_stsc + 1].first_chunk <= p->cur_chunk + 1)
        p->cur_stsc++;

    spc = p->stsc_count ? p->stsc[p->cur_stsc].samples_per_chunk : 1;
    if (spc == 0)
        spc = 1;

    chunk_idx = p->cur_chunk;
    if (chunk_idx >= p->chunk_count)
        return -1;

    /* 当前 chunk 内的字节偏移 */
    offset = p->chunk_offsets[chunk_idx];
    {
        uint32_t i;
        for (i = 0; i < p->cur_in_chunk && i < p->cur_sample; i++)
            offset += p->sample_sizes[p->cur_sample - p->cur_in_chunk + i];
    }

    uint32_t ssize = p->sample_sizes[p->cur_sample];
    if (ssize == 0 || ssize > 8 * 1024 * 1024)
        return -1;

    if (ssize > p->buf_cap) {
        uint8_t *nb = (uint8_t *)realloc(p->buf, ssize);
        if (!nb)
            return -1;
        p->buf = nb;
        p->buf_cap = ssize;
    }

    if (fseek(p->fp, (long)offset, SEEK_SET) != 0)
        return -1;
    if (fread(p->buf, 1, ssize, p->fp) != ssize)
        return -1;

    out->data = p->buf;
    out->len = ssize;
    out->pts = p->sample_pts ? p->sample_pts[p->cur_sample] : 0;

    p->cur_sample++;
    p->cur_in_chunk++;
    if (p->cur_in_chunk >= spc) {
        p->cur_chunk++;
        p->cur_in_chunk = 0;
    }

    return 0;
}
uint32_t mp4_sample_count(const mp4_ctx_t *ctx)
{
    const mp4_priv_t *p;
    if (!ctx || !ctx->_priv)
        return 0;
    p = (const mp4_priv_t *)ctx->_priv;
    return p->sample_count;
}
