/*
 * dumppic.c — 把解码出的帧原样写成裸 NV12 文件，用于离线核对
 *
 * 目的：屏幕上的画面是"紫绿 + 左右重复 + 不满屏"。要分清责任在谁：
 *   - 如果导出的裸帧本身正常 → disp 配置错
 *   - 如果导出的裸帧就不对 → 解码输出格式/布局理解错
 *
 * 输出格式（便于 ffmpeg 直接读）：
 *   frame<N>_<w>x<h>_stride<W>.yuv
 *   = Y 平面 (h 行 x stride 字节) + UV 平面 (h/2 行 x stride 字节)
 *
 * 注意 w/h 用解码器给的 nWidth/nHeight（可能是 736，含 16 行对齐填充），
 * 同时把 MP4 里声明的真实宽高也打出来，方便判断哪部分是填充。
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

static void *parser_thread(void *a)
{
    static uint8_t *abuf;
    static int acap;
    (void)a;
    while (!g_exit) {
        mp4_sample_t smp;
        char *pbuf = NULL, *pring = NULL;
        int pbufsize = 0, pringsize = 0;
        VideoStreamDataInfo di;
        int aneed, alen;

        if (VideoStreamFrameNum(g_pdec, 0) > 20) { usleep(20*1000); continue; }
        if (mp4_next_sample(g_mp4, &smp) != 0) { mp4_rewind(g_mp4); usleep(50*1000); continue; }
        if (RequestVideoStreamBuffer(g_pdec, (int)smp.len, &pbuf, &pbufsize,
                                     &pring, &pringsize, 0) != 0) { usleep(5000); continue; }
        if ((uint32_t)(pbufsize + pringsize) < smp.len) { usleep(5000); continue; }

        aneed = (int)smp.len + (int)smp.len / 8 + 256;
        if (aneed > acap) {
            uint8_t *nb = (uint8_t *)realloc(abuf, (size_t)aneed);
            if (!nb) { usleep(5000); continue; }
            abuf = nb; acap = aneed;
        }
        alen = avcc_to_annexb(smp.data, (int)smp.len, abuf, acap, 4);
        if (alen <= 0) alen = (int)smp.len;
        if (alen <= pbufsize) memcpy(pbuf, abuf, (size_t)alen);
        else { memcpy(pbuf, abuf, (size_t)pbufsize);
               memcpy(pring, abuf + pbufsize, (size_t)(alen - pbufsize)); }

        memset(&di, 0, sizeof(di));
        di.pData = pbuf; di.nLength = alen;
        di.nPts = smp.pts; di.nPcr = smp.pts;
        di.bIsFirstPart = 1; di.bIsLastPart = 1; di.bValid = 1;
        SubmitVideoStreamData(g_pdec, &di, 0);
    }
    return NULL;
}

static void *decoder_thread(void *a)
{
    (void)a;
    while (!g_exit) {
        int r = DecodeVideoStream(g_pdec, 0, 0, 0, 0);
        if (r == VDECODE_RESULT_NO_FRAME_BUFFER) usleep(2000);
        else if (r == VDECODE_RESULT_CONTINUE || r == VDECODE_RESULT_NO_BITSTREAM) usleep(1000);
        else usleep(500);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    VideoStreamInfo info;
    VConfig conf;
    struct ScMemOpsS *memops;
    pthread_t t1, t2;
    int i, nret, want = 20, got = 0;
    VideoPicture *pic = NULL;

    if (argc < 3) { fprintf(stderr, "usage: %s f.mp4 out_prefix [frameNo]\n", argv[0]); return 2; }
    if (argc >= 4) want = atoi(argv[3]);

    AddVDPlugin();
    memops = MemAdapterGetOpsS();
    CdcMemOpen(memops);
    g_pdec = CreateVideoDecoder();
    g_mp4 = mp4_open(argv[1]);
    if (!g_mp4) return 1;

    printf("[dump] mp4 声明: %dx%d codec=%d samples=%u\n",
           g_mp4->width, g_mp4->height, g_mp4->codec, mp4_sample_count(g_mp4));

    memset(&info, 0, sizeof(info));
    memset(&conf, 0, sizeof(conf));
    info.eCodecFormat = (g_mp4->codec == 1) ? VIDEO_CODEC_FORMAT_H265 : VIDEO_CODEC_FORMAT_H264;
    conf.eOutputPixelFormat = PIXEL_FORMAT_NV12;
    conf.bGpuBufValid = 0;
    conf.nAlignStride = 32;
    conf.nDisplayHoldingFrameBufferNum = 10;
    conf.memops = memops;

    nret = InitializeVideoDecoder(g_pdec, &info, &conf);
    if (nret) { fprintf(stderr, "init fail %d\n", nret); return 1; }

    if (g_mp4->extradata_len > 0) {
        char *pbuf = NULL, *pring = NULL;
        int pbufsize = 0, pringsize = 0;
        VideoStreamDataInfo di;
        int el = g_mp4->extradata_len;
        if (RequestVideoStreamBuffer(g_pdec, el, &pbuf, &pbufsize, &pring, &pringsize, 0) == 0) {
            memcpy(pbuf, g_mp4->extradata, (size_t)(el <= pbufsize ? el : pbufsize));
            memset(&di, 0, sizeof(di));
            di.pData = pbuf; di.nLength = el;
            di.bIsFirstPart = 1; di.bIsLastPart = 1; di.bValid = 1;
            SubmitVideoStreamData(g_pdec, &di, 0);
        }
    }

    pthread_create(&t1, NULL, parser_thread, NULL);
    pthread_create(&t2, NULL, decoder_thread, NULL);

    /* 丢前 want-1 帧，第 want 帧落盘（避开首帧 I 帧之前的空帧） */
    for (i = 0; i < want + 200 && got < 1; i++) {
        pic = RequestPicture(g_pdec, 0);
        if (!pic) { usleep(2000); continue; }
        if (i < want - 1) { ReturnPicture(g_pdec, pic); pic = NULL; continue; }
        got = 1;
    }

    if (!pic) { printf("[dump] 没取到帧\n"); return 1; }

    printf("[dump] 帧: w=%d h=%d stride=%d fmt=%d pts=%lld pData0=%p pData1=%p\n",
           pic->nWidth, pic->nHeight, pic->nLineStride, pic->ePixelFormat,
           (long long)pic->nPts, (void *)pic->pData0, (void *)pic->pData1);

    {
        char path[512];
        FILE *fp;
        int w = pic->nWidth, h = pic->nHeight, stride = pic->nLineStride;
        size_t ysz = (size_t)stride * (size_t)h;
        size_t uvsz = (size_t)stride * (size_t)(h / 2);

        snprintf(path, sizeof(path), "%s_%dx%d_stride%d.yuv", argv[2], w, h, stride);
        fp = fopen(path, "wb");
        if (!fp) { printf("[dump] 打不开 %s\n", path); return 1; }
        fwrite(pic->pData0, 1, ysz, fp);
        if (pic->pData1)
            fwrite(pic->pData1, 1, uvsz, fp);
        fclose(fp);
        printf("[dump] 已写 %s  (Y=%zu 字节, UV=%zu 字节, 合计=%zu)\n",
               path, ysz, uvsz, ysz + uvsz);
        printf("[dump] 校验: stride*h + stride*h/2 = %d*%d + %d*%d = %zu\n",
               stride, h, stride, h / 2, ysz + uvsz);
    }

    ReturnPicture(g_pdec, pic);
    g_exit = 1;
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    DestroyVideoDecoder(g_pdec);
    mp4_close(g_mp4);
    CdcMemClose(memops);
    return 0;
}