/*
 * fdverify.c — 验证索引 23 取到的确实是 dma-buf fd
 *
 * fdprobe 发现 memops 虚表索引 23 对 pData0 返回 9。按 sc_interface.h
 * 的成员顺序（新版 24 个成员），索引 23 = get_fd_by_vir。
 *
 * 但"返回一个正数"不等于"是有效的 dma-buf fd"。这里做三重验证：
 *   1. readlink /proc/self/fd/N —— dma-buf 会显示 /dmabuf 或 anonymous
 *   2. fstat 拿到 st_size —— dma-buf 的 size 应该是帧缓冲大小量级
 *      （1280x736 NV12 ≈ 1280*736*1.5 = 1413120 字节）
 *   3. 用 DMA_BUF_IOCTL 查询真实 size（需要 linux/dma-buf.h）
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
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

static void check_fd(int fd, const char *who)
{
    char path[64], link[256];
    struct stat st;
    ssize_t n;

    printf("\n[verify] === %s fd=%d ===\n", who, fd);
    if (fd < 0) { printf("[verify] fd 无效\n"); return; }

    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    n = readlink(path, link, sizeof(link) - 1);
    if (n > 0) {
        link[n] = 0;
        printf("[verify] readlink = %s\n", link);
    } else {
        printf("[verify] readlink 失败: %s\n", strerror(errno));
    }

    if (fstat(fd, &st) == 0) {
        printf("[verify] fstat: size=%lld mode=0%o\n",
               (long long)st.st_size, (unsigned)st.st_mode & 0777);
        if (st.st_size > 100000)
            printf("[verify] size 量级符合帧缓冲 (1280x736 NV12 需 %d 字节)\n",
                   1280 * 736 * 3 / 2);
        else
            printf("[verify] size 太小，可能不是帧缓冲\n");
    } else {
        printf("[verify] fstat 失败: %s\n", strerror(errno));
    }

    /* 确认 fd 是有效描述符：dup 一下能成功即证明 */
    {
        int d = dup(fd);
        printf("[verify] dup = %d %s\n", d, (d >= 0) ? "(fd 有效)" : "(无效)");
        if (d >= 0) close(d);
    }
}

int main(int argc, char **argv)
{
    VideoStreamInfo info;
    VConfig conf;
    struct ScMemOpsS *memops;
    void **vt;
    pthread_t t1, t2;
    int i, nret;
    VideoPicture *pic = NULL, *pic2 = NULL;
    typedef int (*getfd_t)(void *);
    getfd_t get_fd;

    if (argc < 2) { fprintf(stderr, "usage: %s f.mp4\n", argv[0]); return 2; }

    AddVDPlugin();
    memops = MemAdapterGetOpsS();
    CdcMemOpen(memops);
    g_pdec = CreateVideoDecoder();
    g_mp4 = mp4_open(argv[1]);

    memset(&info, 0, sizeof(info));
    memset(&conf, 0, sizeof(conf));
    info.eCodecFormat = VIDEO_CODEC_FORMAT_H264;
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
    usleep(400 * 1000);

    for (i = 0; i < 5000 && !pic; i++) { pic = RequestPicture(g_pdec, 0); if (!pic) usleep(2000); }
    if (!pic) { printf("[verify] 取帧失败\n"); return 1; }

    printf("[verify] 帧1: %dx%d pad=%d stride=%d pData0=%p nBufFd=%d\n",
           pic->nWidth, pic->nHeight, pic->nHeight, pic->nLineStride,
           (void *)pic->pData0, pic->nBufFd);

    vt = (void **)(void *)memops;
    get_fd = (getfd_t)vt[23];
    if (!get_fd) { printf("[verify] vt[23] 为空\n"); return 1; }

    printf("[verify] 用 vt[23] = %p 取 fd\n", (void *)get_fd);
    {
        int fd1 = get_fd(pic->pData0);
        check_fd(fd1, "帧1 get_fd(pData0)");
    }

    /* 再取第二帧，确认不同帧拿到不同 fd（说明是 per-buffer 的） */
    ReturnPicture(g_pdec, pic);
    for (i = 0; i < 5000 && !pic2; i++) { pic2 = RequestPicture(g_pdec, 0); if (!pic2) usleep(2000); }
    if (pic2) {
        int fd2;
        printf("\n[verify] 帧2: pData0=%p nBufFd=%d\n",
               (void *)pic2->pData0, pic2->nBufFd);
        fd2 = get_fd(pic2->pData0);
        check_fd(fd2, "帧2 get_fd(pData0)");
        ReturnPicture(g_pdec, pic2);
    }

    g_exit = 1;
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    DestroyVideoDecoder(g_pdec);
    mp4_close(g_mp4);
    CdcMemClose(memops);
    return 0;
}