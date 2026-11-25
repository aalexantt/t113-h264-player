/*
 * pixcheck.c — 验证解码出来的帧确实有画面内容
 *
 * 前面 poc4_test 证明了：能解出帧、PTS 递增、缓冲轮转。
 * 但那只能说明"有帧"，不能说明"帧里有画面"。如果解码器输出全黑或
 * 花屏，前面那些指标一样会正常。
 *
 * 这里直接统计 Y 分量的直方图：
 *   - 全 0 说明是黑帧（解码失败或没渲染）
 *   - 各帧直方图差异大说明画面在变
 *
 * 判定标准：
 *   Y 均值在 16~235 之间、方差 > 100、不同帧之间有差异 → 画面正常
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "vdecoder.h"
#include "memoryAdapter.h"
#include "sc_interface.h"
#include "mp4parse.h"
#include "avcc2annexb.h"

static VideoDecoder *g_pdec;
static mp4_ctx_t *g_mp4;
static volatile int g_exit;
static struct ScMemOpsS *g_memops;

static void *parser_thread(void *arg);
static void *decoder_thread(void *arg);

static void *decoder_thread(void *arg)
{
    (void)arg;
    while (!g_exit) {
        int nret = DecodeVideoStream(g_pdec, 0, 0, 0, 0);
        if (nret == VDECODE_RESULT_NO_FRAME_BUFFER)
            usleep(2000);
        else if (nret == VDECODE_RESULT_CONTINUE ||
                 nret == VDECODE_RESULT_NO_BITSTREAM)
            usleep(1000);
        else
            usleep(500);
    }
    return NULL;
}

static void *parser_thread(void *arg)
{
    (void)arg;
    int alen = 0;
    static uint8_t *abuf;
    static int acap;

    while (!g_exit) {
        mp4_sample_t smp;
        char *pbuf = NULL, *pring = NULL;
        int pbufsize = 0, pringsize = 0;
        VideoStreamDataInfo di;
        int aneed;

        if (VideoStreamFrameNum(g_pdec, 0) > 20) {
            usleep(20 * 1000);
            continue;
        }
        if (mp4_next_sample(g_mp4, &smp) != 0) {
            mp4_rewind(g_mp4);
            usleep(50 * 1000);
            continue;
        }
        if (RequestVideoStreamBuffer(g_pdec, (int)smp.len, &pbuf, &pbufsize,
                                     &pring, &pringsize, 0) != 0) {
            usleep(5 * 1000);
            continue;
        }
        if ((uint32_t)(pbufsize + pringsize) < smp.len) {
            usleep(5 * 1000);
            continue;
        }

        aneed = (int)smp.len + (int)smp.len / 8 + 256;
        if (aneed > acap) {
            uint8_t *nb = (uint8_t *)realloc(abuf, (size_t)aneed);
            if (!nb) { usleep(5 * 1000); continue; }
            abuf = nb;
            acap = aneed;
        }
        alen = avcc_to_annexb(smp.data, (int)smp.len, abuf, acap, 4);
        if (alen <= 0)
            alen = (int)smp.len;
        if (alen <= pbufsize) {
            memcpy(pbuf, abuf, (size_t)alen);
        } else {
            memcpy(pbuf, abuf, (size_t)pbufsize);
            memcpy(pring, abuf + pbufsize, (size_t)(alen - pbufsize));
        }

        memset(&di, 0, sizeof(di));
        di.pData = pbuf;
        di.nLength = alen;
        di.nPts = smp.pts;
        di.nPcr = smp.pts;
        di.bIsFirstPart = 1;
        di.bIsLastPart = 1;
        di.bValid = 1;
        SubmitVideoStreamData(g_pdec, &di, 0);
    }
    return NULL;
}

/* 统计 Y 分量的均值/方差/直方图 */
static void stat_y(const uint8_t *y, int w, int h, int stride,
                   double *mean, double *var, int *minv, int *maxv)
{
    int i, j;
    double sum = 0, sum2 = 0;
    int n = 0, mn = 255, mx = 0;

    for (j = 0; j < h; j++) {
        const uint8_t *row = y + (size_t)j * (size_t)stride;
        for (i = 0; i < w; i++) {
            int v = row[i];
            sum += v;
            sum2 += (double)v * v;
            if (v < mn) mn = v;
            if (v > mx) mx = v;
            n++;
        }
    }
    *mean = sum / n;
    *var = sum2 / n - (*mean) * (*mean);
    *minv = mn;
    *maxv = mx;
}

int main(int argc, char **argv)
{
    VideoStreamInfo info;
    VConfig conf;
    pthread_t th, th_dec;
    int nret, shown = 0;
    double prev_mean = -1;

    if (argc < 2) { fprintf(stderr, "usage: %s f.mp4\n", argv[0]); return 2; }

    AddVDPlugin();
    g_memops = MemAdapterGetOpsS();
    CdcMemOpen(g_memops);
    g_pdec = CreateVideoDecoder();
    g_mp4 = mp4_open(argv[1]);
    if (!g_mp4) return 1;

    memset(&info, 0, sizeof(info));
    memset(&conf, 0, sizeof(conf));
    info.eCodecFormat = VIDEO_CODEC_FORMAT_H264;

    /* 实测有效的配置：bGpuBufValid=0 让解码器自己分配 FBM 缓冲，
     * bGpuBufValid=1 时必须由外部提供缓冲（SetVideoFbmBufAddress），
     * 不提供就永远 NO_FRAME_BUFFER。 */
    conf.eOutputPixelFormat = PIXEL_FORMAT_NV12;
    conf.bGpuBufValid = 0;
    conf.nAlignStride = 32;
    conf.nDeInterlaceHoldingFrameBufferNum = 0;
    conf.nDisplayHoldingFrameBufferNum = 10;
    conf.nRotateHoldingFrameBufferNum = 0;
    conf.nDecodeSmoothFrameBufferNum = 0;
    conf.memops = g_memops;
    conf.nVeFreq = 0;

    nret = InitializeVideoDecoder(g_pdec, &info, &conf);
    if (nret != 0) { fprintf(stderr, "init fail %d\n", nret); return 1; }

    /* extradata */
    if (g_mp4->extradata_len > 0) {
        char *pbuf = NULL, *pring = NULL;
        int pbufsize = 0, pringsize = 0;
        VideoStreamDataInfo di;
        int el = g_mp4->extradata_len;
        if (RequestVideoStreamBuffer(g_pdec, el, &pbuf, &pbufsize,
                                     &pring, &pringsize, 0) == 0) {
            memcpy(pbuf, g_mp4->extradata,
                   (size_t)(el <= pbufsize ? el : pbufsize));
            memset(&di, 0, sizeof(di));
            di.pData = pbuf;
            di.nLength = el;
            di.bIsFirstPart = 1;
            di.bIsLastPart = 1;
            di.bValid = 1;
            SubmitVideoStreamData(g_pdec, &di, 0);
        }
    }

    pthread_create(&th, NULL, parser_thread, NULL);
    pthread_create(&th_dec, NULL, decoder_thread, NULL);
    usleep(300 * 1000);

    printf("[pix] 开始取帧统计（前 8 帧）\n");
    {
        int waited = 0;
        while (shown < 8 && waited < 20000) {   /* 最多等 20 秒 */
            VideoPicture *pic = RequestPicture(g_pdec, 0);
            if (!pic) {
                usleep(2000);
                waited++;
                if (waited % 2000 == 0)
                    printf("[pix] 等待中... queue=%d decoded=%d\n",
                           VideoStreamFrameNum(g_pdec, 0), shown);
                continue;
            }

        {
            double mean, var;
            int mn, mx, stride = pic->nLineStride;

            stat_y((const uint8_t *)pic->pData0, pic->nWidth, pic->nHeight,
                   stride, &mean, &var, &mn, &mx);

            printf("[pix] #%d %dx%d fmt=%d pts=%lld  Y:mean=%.1f var=%.0f "
                   "min=%d max=%d  %s\n",
                   shown + 1, pic->nWidth, pic->nHeight, pic->ePixelFormat,
                   (long long)pic->nPts, mean, var, mn, mx,
                   (var > 100.0) ? "有画面" : "疑似空白");

            if (prev_mean >= 0) {
                double d = mean - prev_mean;
                if (d < 0) d = -d;
                printf("[pix]      与上一帧均值差 = %.2f %s\n", d,
                       (d > 1.0) ? "(画面在变)" : "(画面相同)");
            }
            prev_mean = mean;
        }

        ReturnPicture(g_pdec, pic);
            shown++;
        }
    }

    if (shown == 0)
        printf("[pix] 20 秒内没取到任何帧 —— RequestPicture 一直失败\n");

    g_exit = 1;
    pthread_join(th, NULL);
    pthread_join(th_dec, NULL);
    DestroyVideoDecoder(g_pdec);
    mp4_close(g_mp4);
    CdcMemClose(g_memops);
    return 0;
}