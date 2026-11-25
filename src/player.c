/*
 * player.c — T113 H.264 硬解 + disp 直送的完整播放器
 *
 * 架构（三线程）：
 *   parser  线程 —— 从 MP4 读 sample，AVCC 转 AnnexB，SubmitVideoStreamData
 *   decoder 线程 —— DecodeVideoStream 循环
 *   主线程        —— RequestPicture 取帧，取 dma-buf fd，DISP_LAYER_SET_CONFIG2 送显
 *
 * 送显的关键几点（都是实测确认的）：
 *
 * 1. 用 DISP_LAYER_SET_CONFIG2（0x49），不是老的 DISP_CMD_VIDEO_SET_FB。
 *    这个内核的 sunxi_display2.h 里根本没有 VIDEO_SET_FB。
 *
 * 2. dma-buf fd 来源：VideoPicture.nBufFd 恒为 0，必须走 memops 虚表
 *    索引 23（get_fd_by_vir），偏移 23*4=92（32 位）。
 *    已用 readlink /proc/self/fd/N 验证返回的是 /dmabuf:，
 *    且 size 恰为 1280*736*1.5=1413120，确认无误。
 *
 * 3. 帧缓冲必须"持两帧再还"：
 *    disp 在扫当前帧的 dma-buf，如果解码器立刻复用这块缓冲，画面会撕裂
 *    或者显示脏数据。保留最近 2 帧不归还，还更早的那帧。
 *    这是 H618 的 video_decoder_cedar.c 里的做法，注释写得很明确。
 *
 * 4. 时钟自己算：base_pts + wall clock 推目标时刻，不用音频时钟。
 *    板上有音频时时钟会被音频拖住（之前 xplayer 卡死就是这个原因之一）。
 */

#define _DEFAULT_SOURCE
#include <stdbool.h>            /* 内核头 sunxi_display2.h 用了 bool，必须先引 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/mman.h>

#include "vdecoder.h"
#include "memoryAdapter.h"
#include "sc_interface.h"
/* osd.h 里带了内核类型(u32/s32)的定义，必须在 sunxi_display2.h 之前包含 */
#include "osd.h"
#include <video/sunxi_display2.h>

#include "mp4parse.h"
#include "avcc2annexb.h"

/* ---- disp ----
 *
 * 通道号必须用 0。
 *
 * 依据：de_feat.h 里 CHN_NUM=4，VI_CHN_NUM 为 1 或 2（按芯片配置）。
 * de_rtmx.c 的判断逻辑是：
 *     vi_chno = de_feat_get_num_vi_chns(sel);
 *     if (channel < vi_chno)  → 走 VI（视频）路径，支持 YUV
 *     else                    → 走 UI 路径
 * 而 UI 路径把数据当 RGB 解释 —— 把 NV12 喂进去就是紫色乱码 + 画面重排。
 *
 * 实测印证：之前放在 channel=1（UI 通道）时，屏幕上是紫绿色、左右重复的两份图；
 * 同一帧数据导出成 YUV 用正确方式渲染却是完全正常的照片，说明解码没错，
 * 就是通道选错了。
 *
 * 另外 layerControl_linux.cpp:23 也写着 "channel 0 for pad screen"，
 * 是原厂播放器的选择。
 */
#define DISP_DEV        "/dev/disp"
#define DISP_SCREEN     0       /* LCD 在 screen 0 */
#define DISP_CHANNEL    0       /* VI 视频通道，必须 0 */
#define DISP_VIDEO_LYR  0       /* 视频通道下的 layer 0 */
#define DISP_ZORDER     20      /* fb 驱动固定用 16，视频层要更大才不被盖住 */

/* OSD 放在 UI 通道上：C 层必须走 UI 通道才能做 RGB 混合。
 * 视频在 ch0/lyr0，OSD 在 ch1/lyr1，zorder 比视频还大。 */
#define OSD_CHANNEL     1
#define OSD_LAYER       1
#define OSD_ZORDER      30
/* OSD 面板：位置和大小按 1080p 屏幕定，字号 9x17
 * 5 行文字（分辨率 / fps / 时间 / cpu / 内存），每行 19 像素 + 上下留白 */
#define OSD_X           8
#define OSD_Y           8
#define OSD_PW          210
#define OSD_PH          111

/* ---- 解码状态 ---- */
#define ST_EXIT     (1u << 0)

typedef struct {
    VideoDecoder     *pdec;
    struct ScMemOpsS *memops;
    mp4_ctx_t        *mp4;

    pthread_t  th_parser;
    pthread_t  th_decoder;
    pthread_mutex_t lock;
    unsigned   state;

    int  disp_fd;
    int  scr_w, scr_h;

    /* 统计 */
    volatile int submitted;
    volatile int decoded;
    volatile int presented;
} ctx_t;

static int64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000ll + tv.tv_usec;
}

/* 信号处理只设一个 volatile 标志。
 *
 * 【为什么不在信号处理函数里直接操作 ctx】
 * pthread_mutex_lock 不是 async-signal-safe 的。如果在处理函数里
 * 加锁，而主线程正好持有该锁，就会死锁（而且是在中断上下文里死锁，
 * 没有任何日志，表现为进程卡死不退出）。
 * 所以这里只置标志，由主循环自己检查后正常退出。
 */
static volatile sig_atomic_t g_sig_quit = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_sig_quit = 1;
}

static void install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;        /* 不用 SA_RESTART，让阻塞的调用被打断 */
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

static int get_state(ctx_t *c)
{
    int s;
    pthread_mutex_lock(&c->lock);
    s = (int)c->state;
    pthread_mutex_unlock(&c->lock);
    return s;
}

static void set_state(ctx_t *c, unsigned bits)
{
    pthread_mutex_lock(&c->lock);
    c->state |= bits;
    pthread_mutex_unlock(&c->lock);
}

/* ---------- 播放时钟 ----------
 *
 * 【之前帧率不对的原因】
 * 原来写的是 target = base_wall + (pts - base_pts) * 1000，
 * 把 PTS 当成毫秒了。但这个 MP4 的 timescale 不是 1000：
 *   - samples = 4271，duration = 170859 ms  → 每帧 40ms → 25fps
 *   - 相邻 PTS 步进 512
 *   - 反推 timescale = 512 / 0.040s = 12800
 * 所以 PTS 单位是 1/12800 秒，不是毫秒。当成毫秒用会慢 12.8 倍。
 *
 * 正确换算：目标时刻(us) = base_wall + (pts - base_pts) * 1000000 / timescale
 */
typedef struct {
    int64_t base_pts;
    int64_t base_wall;
    uint32_t timescale;
    int     have;
    int64_t last_pts;       /* 上一帧的 PTS，用于检测循环 */
    int     loop_count;     /* 已循环次数 */
    /* 统计 */
    int64_t stat_t0;
    int     stat_frames;
    double  fps;            /* 内部按 PTS 算的帧率 */
    double  fps_wall;       /* 按实际墙上时间算的帧率 */
} pclock_t;

static void clock_start(pclock_t *k, uint32_t timescale)
{
    memset(k, 0, sizeof(*k));
    k->timescale = timescale ? timescale : 1000;
    k->stat_t0 = now_us();
}

/* 返回需要等待的微秒数（0 表示不用等） */
static int64_t clock_wait_us(pclock_t *k, int64_t pts)
{
    int64_t now = now_us();
    int64_t target;

    if (!k->have) {
        k->base_pts = pts;
        k->base_wall = now;
        k->last_pts = pts;
        k->have = 1;
        return 0;
    }

    /* 循环检测：PTS 明显回退 = 播放器重头开始了。
     *
     * 为什么必须处理：parser 线程 mp4_rewind() 之后 PTS 从末尾跳回 0，
     * 如果不重置基准，target 会被算成"很久以前"，于是每一帧都判定
     * "落后超过 1 秒"，触发下面的追赶重置逻辑，表现是画面一阵乱跳。
     *
     * 阈值取半个 timescale 单位（本文件是 0.5 秒），
     * 足够避开 B 帧带来的小幅 PTS 乱序（正常不超过几帧）。 */
    if (pts + (int64_t)(k->timescale / 2) < k->last_pts) {
        k->loop_count++;
        k->base_pts = pts;
        k->base_wall = now;
        k->last_pts = pts;
        k->stat_frames = 0;
        k->stat_t0 = now;
        return 0;
    }
    k->last_pts = pts;

    target = k->base_wall +
             (int64_t)((double)(pts - k->base_pts) * 1000000.0 / (double)k->timescale);

    if (now < target) {
        int64_t slp = target - now;
        /* 单次最多睡 80ms，避免大跳变；也让线程及时响应退出 */
        if (slp > 80000) slp = 80000;
        return slp;
    }
    /* 落后太多就重置基准，避免越追越远导致画面快进 */
    if (now - target > 1000000) {
        k->base_pts = pts;
        k->base_wall = now;
    }
    return 0;
}

static void clock_tick(pclock_t *k, int64_t pts)
{
    k->stat_frames++;
    if (pts > k->base_pts) {
        double sec = (double)(pts - k->base_pts) / (double)k->timescale;
        if (sec > 0.5)
            k->fps = k->stat_frames / sec;
    }
    {
        double wall = (double)(now_us() - k->stat_t0) / 1000000.0;
        if (wall > 0.5)
            k->fps_wall = k->stat_frames / wall;
    }
}

/* 当前播放位置（秒）。用 PTS 和 timescale 换算，不是墙上时间。 */
static double clock_position_s(const pclock_t *k, int64_t pts)
{
    if (!k->timescale)
        return 0.0;
    return (double)pts / (double)k->timescale;
}

/* 把秒格式化成 mm:ss */
static void fmt_time(char *buf, size_t n, double sec)
{
    int s = (int)(sec + 0.5);
    if (s < 0) s = 0;
    snprintf(buf, n, "%02d:%02d", s / 60, s % 60);
}

/* 读 /proc/meminfo 拿内存情况。
 *
 * 为什么要分开看两个值：
 *   MemFree    —— 完全没用的页。Linux 会尽量把空闲内存拿去做缓存，
 *                 所以这个值常年很低（本机约 9MB），不代表内存不够。
 *   MemAvailable —— 真正还能挪出来的量 = 空闲 + 可回收缓存。
 *                 这个才是"剩余内存"的准确含义。
 * OSD 上显示 MemAvailable 为主，MemFree 作为参考。 */
static void read_mem(int *total_mb, int *free_mb, int *avail_mb)
{
    FILE *fp;
    char key[32];
    long val;
    long total = 0, free_ = 0, avail = 0;

    *total_mb = *free_mb = *avail_mb = 0;

    fp = fopen("/proc/meminfo", "r");
    if (!fp)
        return;
    while (fscanf(fp, "%31s %ld %*s", key, &val) == 2) {
        if (strcmp(key, "MemTotal:") == 0) total = val;
        else if (strcmp(key, "MemFree:") == 0) free_ = val;
        else if (strcmp(key, "MemAvailable:") == 0) avail = val;
    }
    fclose(fp);

    *total_mb = (int)(total / 1024);
    *free_mb  = (int)(free_ / 1024);
    *avail_mb = (int)(avail / 1024);
}

/* 读 /proc/stat 计算 CPU 占用（整机，0~100） */
static double cpu_usage(void)
{
    static uint64_t prev_idle, prev_total;
    FILE *fp;
    char buf[256];
    uint64_t u, n, s, i, w, irq, sirq, steal, idle, total;

    fp = fopen("/proc/stat", "r");
    if (!fp)
        return -1.0;
    if (!fgets(buf, sizeof(buf), fp)) { fclose(fp); return -1.0; }
    fclose(fp);

    /* cpu  user nice system idle iowait irq softirq steal */
    if (sscanf(buf, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
               (unsigned long long *)&u, (unsigned long long *)&n,
               (unsigned long long *)&s, (unsigned long long *)&i,
               (unsigned long long *)&w, (unsigned long long *)&irq,
               (unsigned long long *)&sirq, (unsigned long long *)&steal) < 4)
        return -1.0;

    idle = i + w;
    total = u + n + s + i + w + irq + sirq + steal;

    if (prev_total == 0) {
        prev_idle = idle;
        prev_total = total;
        return 0.0;
    }
    {
        uint64_t dt = total - prev_total;
        uint64_t di = idle - prev_idle;
        prev_idle = idle;
        prev_total = total;
        if (dt == 0)
            return 0.0;
        return 100.0 * (double)(dt - di) / (double)dt;
    }
}
/* memops 虚表索引 23 = get_fd_by_vir（新版 sc_interface.h 的第 24 个成员）。
 * 32 位偏移 = 23 * 4 = 92。返回值已用 readlink 验证是 /dmabuf:。 */
static int picture_get_fd(struct ScMemOpsS *memops, VideoPicture *pic)
{
    typedef int (*fn_t)(void *);
    fn_t fn;

    if (pic->nBufFd > 0)
        return pic->nBufFd;         /* 万一以后版本会填就用现成的 */
    if (!memops || !pic->pData0)
        return -1;

    fn = ((fn_t *)(void *)memops)[23];
    if (!fn)
        return -1;
    return fn(pic->pData0);
}

/* ---------- disp 送帧 ---------- */

/* 读屏幕尺寸。
 * dev_disp.c:820 的实现是：
 *     case DISP_GET_SCN_WIDTH:
 *         mgr->device->get_resolution(device, &width, &height);
 *         ret = width;      ← 宽度从 ioctl 返回值给出，不是写 ubuffer
 *     case DISP_GET_SCN_HEIGHT: 同理返回 height
 * 所以要读 ioctl 的返回值本身。 */
static int disp_init(ctx_t *c)
{
    unsigned long arg[4];
    int w, h;

    c->disp_fd = open(DISP_DEV, O_RDWR);
    if (c->disp_fd < 0) {
        fprintf(stderr, "[player] open %s 失败: %s\n", DISP_DEV, strerror(errno));
        return -1;
    }

    memset(arg, 0, sizeof(arg));
    arg[0] = (unsigned long)DISP_SCREEN;
    w = ioctl(c->disp_fd, DISP_GET_SCN_WIDTH, (void *)arg);
    h = ioctl(c->disp_fd, DISP_GET_SCN_HEIGHT, (void *)arg);

    if (w > 0 && w < 8192 && h > 0 && h < 8192) {
        c->scr_w = w;
        c->scr_h = h;
        printf("[player] 屏幕尺寸 %dx%d（ioctl 读出）\n", w, h);
    } else {
        /* 读不到就退回 /sys 里看到的实际值 */
        c->scr_w = 1920;
        c->scr_h = 1080;
        printf("[player] 屏幕尺寸读取失败(w=%d,h=%d)，用默认 %dx%d\n",
               w, h, c->scr_w, c->scr_h);
    }

    printf("[player] disp 打开成功 fd=%d, channel=%d layer=%d zorder=%d\n",
           c->disp_fd, DISP_CHANNEL, DISP_VIDEO_LYR, DISP_ZORDER);
    return 0;
}

static int disp_show(ctx_t *c, int dma_fd, int buf_w, int buf_h,
                     int crop_w, int crop_h)
{
    struct disp_layer_config2 cfg;
    unsigned long arg[4];
    int ret;

    if (c->disp_fd < 0 || dma_fd < 0)
        return -1;

    memset(&cfg, 0, sizeof(cfg));
    cfg.channel  = DISP_CHANNEL;
    cfg.layer_id = DISP_VIDEO_LYR;
    cfg.enable   = 1;
    cfg.info.mode        = LAYER_MODE_BUFFER;
    cfg.info.zorder      = DISP_ZORDER;
    cfg.info.alpha_mode  = 1;
    cfg.info.alpha_value = 0xff;

    /* 目标区域：整个屏幕。1280x720 与 1920x1080 都是 16:9，
     * 用 crop 1280x720 缩放到 1920x1080 不会变形。 */
    cfg.info.screen_win.x      = 0;
    cfg.info.screen_win.y      = 0;
    cfg.info.screen_win.width  = (unsigned int)c->scr_w;
    cfg.info.screen_win.height = (unsigned int)c->scr_h;

    cfg.info.fb.fd        = dma_fd;
    cfg.info.fb.format    = DISP_FORMAT_YUV420_SP_UVUV;   /* NV12: U 在前 */
    cfg.info.fb.color_space = DISP_BT709;

    /* size = 缓冲的物理布局（含对齐填充）；crop = 实际要显示的区域。
     *
     * 解码器给的是 1280x736，736 = 720 向上对齐到 16。
     * 实测那多出来的 16 行全是 0（黑），所以 crop 必须取 720，
     * 否则黑边会被一起拉伸进画面，表现为纵向压扁/变形。 */
    cfg.info.fb.size[0].width  = (unsigned int)buf_w;
    cfg.info.fb.size[0].height = (unsigned int)buf_h;
    cfg.info.fb.size[1].width  = (unsigned int)(buf_w / 2);
    cfg.info.fb.size[1].height = (unsigned int)(buf_h / 2);
    cfg.info.fb.size[2].width  = 0;
    cfg.info.fb.size[2].height = 0;
    /* align 单位是字节（disp_al.c: DISPALIGN(size*ycnt, align)），
     * 16 字节对齐对 1280 宽度没有额外影响。 */
    cfg.info.fb.align[0] = 16;
    cfg.info.fb.align[1] = 16;
    cfg.info.fb.align[2] = 16;

    /* crop 是 32.32 定点（disp_al.c:374 crop.width = crop64->width >> 32） */
    cfg.info.fb.crop.x = 0;
    cfg.info.fb.crop.y = 0;
    cfg.info.fb.crop.width  = ((long long)crop_w) << 32;
    cfg.info.fb.crop.height = ((long long)crop_h) << 32;

    memset(arg, 0, sizeof(arg));
    arg[0] = (unsigned long)DISP_SCREEN;
    arg[1] = (unsigned long)&cfg;
    arg[2] = 1;

    ret = ioctl(c->disp_fd, DISP_LAYER_SET_CONFIG2, (void *)arg);
    if (ret != 0) {
        fprintf(stderr, "[player] DISP_LAYER_SET_CONFIG2 失败 ret=%d errno=%s\n",
                ret, strerror(errno));
        return -1;
    }
    return 0;
}

/* ---------- parser 线程 ---------- */

static void *parser_thread(void *arg)
{
    ctx_t *c = (ctx_t *)arg;
    static uint8_t *abuf;
    static int acap;

    while (!(get_state(c) & ST_EXIT)) {
        mp4_sample_t smp;
        char *pbuf = NULL, *pring = NULL;
        int pbufsize = 0, pringsize = 0;
        VideoStreamDataInfo di;
        int aneed, alen;

        if (VideoStreamFrameNum(c->pdec, 0) > 20) {
            usleep(20 * 1000);
            continue;
        }
        if (mp4_next_sample(c->mp4, &smp) != 0) {
            mp4_rewind(c->mp4);
            usleep(30 * 1000);
            continue;
        }
        if (RequestVideoStreamBuffer(c->pdec, (int)smp.len, &pbuf, &pbufsize,
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
        SubmitVideoStreamData(c->pdec, &di, 0);
        c->submitted++;
    }
    printf("[player] parser 退出, submitted=%d\n", c->submitted);
    return NULL;
}

/* ---------- decoder 线程 ---------- */

static void *decoder_thread(void *arg)
{
    ctx_t *c = (ctx_t *)arg;

    while (VideoStreamFrameNum(c->pdec, 0) < 10) {
        if (get_state(c) & ST_EXIT) return NULL;
        usleep(10 * 1000);
    }

    while (!(get_state(c) & ST_EXIT)) {
        int r = DecodeVideoStream(c->pdec, 0, 0, 0, 0);
        if (r == VDECODE_RESULT_KEYFRAME_DECODED ||
            r == VDECODE_RESULT_FRAME_DECODED) {
            c->decoded++;
        } else if (r == VDECODE_RESULT_NO_FRAME_BUFFER) {
            usleep(2000);
        } else if (r == VDECODE_RESULT_CONTINUE ||
                   r == VDECODE_RESULT_NO_BITSTREAM) {
            usleep(1000);
        } else if (r < 0) {
            usleep(1000);
        } else {
            usleep(500);
        }
    }
    printf("[player] decoder 退出, decoded=%d\n", c->decoded);
    return NULL;
}

/* ---------- disp 层清理 ---------- */

/* 清空 fb0 显存，抹掉开机 LOGO 残留。
 *
 * 【要解决的现象】
 * 开机后视频正常播放，但屏幕中央有一块黑底彩色文字的方框盖在视频上，
 * 四周画面是好的。那是 u-boot 打的 LOGO。
 *
 * 【为什么清掉 fb0 就能解决】
 * 内核启动时会 "Freeing logo buffer memory"，但那只是释放了内存，
 * fb0（ch1/lyr0）这一层的显存内容并没有被清 —— LOGO 还留在里面。
 *
 * 关键点是 fb0 层用的是逐像素 alpha（sys 里显示 a[pixel 255]），
 * 所以 LOGO 的像素是不透明的，会盖在视频层上面
 * （视频在 ch0/lyr0，zorder 20 > fb0 的 16，但 alpha 让 fb0 透出来）。
 *
 * 把 fb0 整个写成 0 字节 = BGRA 全透明，这一层就什么都不显示，
 * 视频完整露出。
 *
 * 【为什么用 mmap 而不是 write】
 * write() 在 fbdev 上会走 console 的 scroll 逻辑，慢且可能只清一部分。
 * mmap 后 memset 是直接改显存，一次到位。
 *
 * 【为什么放在 disp 初始化之后】
 * 要保证 disp 已经把 fb0 层挂上了，清的内容才不会在后续初始化里被覆盖。
 * 实际上驱动不会主动重画 fb0，但放在 disp_init 之后语义更清楚。 */
static void fb_clear_alpha(void)
{
    const char *dev = "/dev/fb0";
    int fd, w = 0, h = 0, bpp = 0;
    size_t bytes;
    void *map;
    FILE *f;

    /* 分辨率从 sysfs 读，别写死 1920x1080 —— 换屏就不用改代码 */
    f = fopen("/sys/class/graphics/fb0/virtual_size", "r");
    if (!f)
        return;
    if (fscanf(f, "%d,%d", &w, &h) != 2) {
        fclose(f);
        return;
    }
    fclose(f);

    f = fopen("/sys/class/graphics/fb0/bits_per_pixel", "r");
    if (f) {
        if (fscanf(f, "%d", &bpp) != 1)
            bpp = 32;
        fclose(f);
    } else {
        bpp = 32;
    }

    if (w <= 0 || h <= 0 || bpp <= 0)
        return;

    bytes = (size_t)w * (size_t)h * (size_t)(bpp / 8);

    fd = open(dev, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "[player] 打开 %s 失败，跳过清屏: %s\n",
                dev, strerror(errno));
        return;
    }

    map = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        fprintf(stderr, "[player] mmap fb0 失败，跳过清屏: %s\n",
                strerror(errno));
        close(fd);
        return;
    }

    memset(map, 0, bytes);
    munmap(map, bytes);
    close(fd);

    printf("[player] 已清空 fb0 开机 LOGO 残留 (%dx%d/%dbpp)\n", w, h, bpp);
}

/* 关闭指定的 disp 层。
 *
 * 【为什么必须做】
 * disp 层是驱动侧的状态，不会因为进程退出而自动释放。
 * 如果播放器退出时不关，那层会一直留在屏幕上——
 * 表现为：程序已经退出，但画面还定格在最后一帧；或者下次启动时
 * 新旧两层叠加，画面变成花屏。
 *
 * 【为什么用 SET_CONFIG2 而不是 DISP_LAYER_DISABLE】
 * 驱动头里虽然有 DISP_LAYER_DISABLE(0x41)，但本驱动版本实际走的是
 * DISP_LAYER_SET_CONFIG2 + enable=0 这条路径（实测有效，
 * 0x41 那个 ioctl 在本版本上不生效）。
 *
 * 注意：只关自己用到的层，别去动 ch1/lyr0（系统 fbdev 层），
 * 那层关了会直接黑屏。 */
static void disp_layer_disable(int fd, unsigned channel, unsigned layer)
{
    struct disp_layer_config2 cfg;
    unsigned long arg[4];

    if (fd < 0)
        return;

    memset(&cfg, 0, sizeof(cfg));
    cfg.channel = (int)channel;
    cfg.layer_id = (int)layer;
    cfg.enable = 0;

    memset(arg, 0, sizeof(arg));
    arg[0] = DISP_SCREEN;
    arg[1] = (unsigned long)&cfg;
    arg[2] = 1;             /* 配置项个数 */

    if (ioctl(fd, DISP_LAYER_SET_CONFIG2, (void *)arg) < 0)
        fprintf(stderr, "[player] 关闭 ch%u/lyr%u 失败: %s\n",
                channel, layer, strerror(errno));
}

/* ---------- main ---------- */

int main(int argc, char **argv)
{
    ctx_t c;
    VideoStreamInfo info;
    VConfig conf;
    int nret, i;
    const char *path;

    /* 双帧持有：disp 正在扫的帧不能马上还给解码器 */
    VideoPicture *held[2] = { NULL, NULL };
    int held_n = 0;

    pclock_t clk;
    double clk_target_fps = 0.0;
    double total_sec = 0.0;
    int64_t t0;
    osd_t osd;
    int osd_ok = 0;

    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <file.mp4> [seconds] [--loop]\n"
                "  seconds  播放秒数，默认 30；写 0 表示一直播不退出\n"
                "  --loop   等价于 seconds=0，语义更明确\n"
                "示例:\n"
                "  %s /opt/media/a.mp4 10     播 10 秒\n"
                "  %s /opt/media/a.mp4 --loop 一直循环播放（开机自启用这个）\n",
                argv[0], argv[0], argv[0]);
        return 2;
    }
    path = argv[1];

    /* 装信号处理：让 init 脚本 stop / Ctrl-C 能优雅退出，
     * 这样解码器和 disp 层能正常清理。 */
    install_signal_handlers();

    memset(&c, 0, sizeof(c));
    c.disp_fd = -1;
    pthread_mutex_init(&c.lock, NULL);

    printf("[player] === T113 H.264 硬解播放器 ===\n");
    printf("[player] file=%s\n", path);

    /* 1. 解码器 */
    AddVDPlugin();
    c.memops = MemAdapterGetOpsS();
    if (!c.memops) { fprintf(stderr, "[player] memops NULL\n"); return 1; }
    CdcMemOpen(c.memops);

    c.pdec = CreateVideoDecoder();
    if (!c.pdec) { fprintf(stderr, "[player] CreateVideoDecoder NULL\n"); return 1; }

    c.mp4 = mp4_open(path);
    if (!c.mp4) { fprintf(stderr, "[player] mp4_open 失败\n"); return 1; }
    printf("[player] mp4 %dx%d codec=%d dur=%ums samples=%u\n",
           c.mp4->width, c.mp4->height, c.mp4->codec,
           c.mp4->duration_ms, mp4_sample_count(c.mp4));

    memset(&info, 0, sizeof(info));
    memset(&conf, 0, sizeof(conf));
    info.eCodecFormat = (c.mp4->codec == 1) ? VIDEO_CODEC_FORMAT_H265
                                            : VIDEO_CODEC_FORMAT_H264;

    /* 实测有效的配置。bGpuBufValid 必须为 0，让解码器自己分配 FBM 缓冲；
     * 为 1 时需要外部 SetVideoFbmBufAddress 提供缓冲，否则永远 NO_FRAME_BUFFER。 */
    conf.eOutputPixelFormat = PIXEL_FORMAT_NV12;
    conf.bGpuBufValid = 0;
    conf.nAlignStride = 32;
    conf.nDeInterlaceHoldingFrameBufferNum = 0;
    conf.nDisplayHoldingFrameBufferNum = 10;
    conf.nRotateHoldingFrameBufferNum = 0;
    conf.nDecodeSmoothFrameBufferNum = 0;
    conf.memops = c.memops;
    conf.nVeFreq = 0;

    nret = InitializeVideoDecoder(c.pdec, &info, &conf);
    printf("[player] InitializeVideoDecoder ret=%d\n", nret);
    if (nret != 0) return 1;

    /* 2. 送 SPS/PPS */
    if (c.mp4->extradata_len > 0) {
        char *pbuf = NULL, *pring = NULL;
        int pbufsize = 0, pringsize = 0;
        VideoStreamDataInfo di;
        int el = c.mp4->extradata_len;
        if (RequestVideoStreamBuffer(c.pdec, el, &pbuf, &pbufsize,
                                     &pring, &pringsize, 0) == 0) {
            memcpy(pbuf, c.mp4->extradata,
                   (size_t)(el <= pbufsize ? el : pbufsize));
            memset(&di, 0, sizeof(di));
            di.pData = pbuf;
            di.nLength = el;
            di.bIsFirstPart = 1;
            di.bIsLastPart = 1;
            di.bValid = 1;
            SubmitVideoStreamData(c.pdec, &di, 0);
            printf("[player] 已送 extradata %d 字节\n", el);
        }
    }

    /* 3. disp */
    if (disp_init(&c) != 0)
        fprintf(stderr, "[player] disp 初始化失败，只做解码不出画面\n");

    /* 3.1 清掉 fb0 里残留的开机 LOGO。
     * 放在 disp 初始化之后、开始送帧之前 —— 这时 fb0 层已经挂好，
     * 清完立刻会有视频帧盖上来，中间不会出现黑屏空窗。 */
    if (c.disp_fd >= 0)
        fb_clear_alpha();

    /* 3.5 OSD 层（放在 UI 通道，逐像素 alpha 混合） */
    if (c.disp_fd >= 0) {
        if (osd_init(&osd, c.memops, c.disp_fd, DISP_SCREEN,
                     OSD_CHANNEL, OSD_LAYER, OSD_ZORDER,
                     c.scr_w, c.scr_h, OSD_PW, OSD_PH) == 0) {
            osd_ok = 1;
            printf("[player] OSD 就绪\n");
        } else {
            fprintf(stderr, "[player] OSD 初始化失败，只显示视频\n");
        }
    }

    /* 4. 起线程 */
    if (pthread_create(&c.th_parser, NULL, parser_thread, &c) != 0) return 1;
    if (pthread_create(&c.th_decoder, NULL, decoder_thread, &c) != 0) return 1;

    /* 5. 送显循环 */
    printf("[player] 开始播放\n");
/* 目标帧率：sample 数 / 总时长。用 mdhd 的时长，不是 mvhd 的 */
    {
        uint32_t n = mp4_sample_count(c.mp4);
        uint32_t d = c.mp4->media_duration_ms ? c.mp4->media_duration_ms
                                              : c.mp4->duration_ms;
        clk_target_fps = (d > 0) ? (1000.0 * n / d) : 0.0;
        /* 总时长（秒），OSD 显示用。取 mdhd 的时长——
         * 那才是这条视频轨的真实长度。 */
        total_sec = (double)d / 1000.0;
        printf("[player] timescale: mvhd=%u mdhd=%u, %u 帧 / %ums → %.2f fps\n",
               c.mp4->timescale, c.mp4->media_timescale, n, d, clk_target_fps);
        printf("[player] 总时长 %.2f 秒 (%02d:%02d)\n",
               total_sec, (int)total_sec / 60, (int)total_sec % 60);
    }
    clock_start(&clk, c.mp4->media_timescale);
    t0 = now_us();
    {
        /* 播放时长解析：
         *   --loop        无限循环（常驻模式，开机自启就用这个）
         *   数字 > 0      播这么多秒后退出
         *   0 / 负数      等同 --loop
         * 不传时默认 30 秒（保留原来的行为，方便手动测试）。 */
        int play_sec = 30;
        int forever = 0;
        int ai;

        for (ai = 2; ai < argc; ai++) {
            if (strcmp(argv[ai], "--loop") == 0) {
                forever = 1;
            } else if (argv[ai][0] != '-') {
                play_sec = atoi(argv[ai]);
                if (play_sec <= 0)
                    forever = 1;
            }
        }

        int64_t limit = forever ? 0 : (int64_t)play_sec * 1000000ll;
        int fail_show = 0;
        int64_t next_osd = 0;
        int vw = 0, vh = 0;   /* 当前视频的显示宽高（crop 后） */

        if (forever)
            printf("[player] 无限循环模式（Ctrl-C 退出）\n");

        /* forever 时 limit=0，条件恒成立；否则按时间退出。
         * 同时检查 ST_EXIT 和信号标志：开机启动的实例被 init 脚本 stop
         * （发 SIGTERM）时要能干净退出，否则会在 disp 里留下没清理的层。 */
        while (!(get_state(&c) & ST_EXIT) && !g_sig_quit &&
               (forever || now_us() - t0 < limit)) {
            VideoPicture *pic = RequestPicture(c.pdec, 0);
            int fd;
            int64_t slp;

            if (!pic) {
                usleep(2000);
                continue;
            }

            /* 时钟：按 PTS 节流。timescale 来自 mvhd，不是固定 1000。 */
            slp = clock_wait_us(&clk, pic->nPts);
            if (slp > 0)
                usleep((useconds_t)slp);

            fd = picture_get_fd(c.memops, pic);
            if (fd < 0) {
                ReturnPicture(c.pdec, pic);
                continue;
            }

            /* crop 高度用 MP4 声明的真实高度（720），
             * 缓冲高度用解码器给的（736，含 16 行对齐填充）。
             * 实测那 16 行全是 0，当成画面内容会变形。 */
            {
                int crop_h = c.mp4->height > 0 && c.mp4->height <= pic->nHeight
                             ? c.mp4->height : pic->nHeight;
                int crop_w = c.mp4->width > 0 && c.mp4->width <= pic->nWidth
                             ? c.mp4->width : pic->nWidth;
                vw = crop_w; vh = crop_h;
            }

            if (c.disp_fd >= 0) {
                if (disp_show(&c, fd, pic->nWidth, pic->nHeight,
                              vw, vh) != 0) {
                    if (++fail_show <= 3)
                        fprintf(stderr, "[player] 送显失败 #%d\n", fail_show);
                }
            }

            clock_tick(&clk, pic->nPts);

            /* OSD 每 250ms 刷新一次，不用每帧都重画（省 CPU） */
            if (osd_ok && now_us() >= next_osd) {
                char line[128];
                char cur[16], tot[16];
                double cpu = cpu_usage();
                double pos_s = clock_position_s(&clk, pic->nPts);

                /* 画布本身就是面板大小，坐标系从 (0,0) 开始 */
                osd_clear(&osd);
                osd_panel(&osd, 0, 0, OSD_PW, OSD_PH);

                snprintf(line, sizeof(line), "%dx%d %s",
                         vw, vh,
                         (c.mp4->codec == 1) ? "H265" : "H264");
                osd_text(&osd, 6, 5, line, 0xffffff, 255);

                snprintf(line, sizeof(line), "fps %.1f / %.1f",
                         clk.fps_wall, clk_target_fps);
                osd_text(&osd, 6, 24, line, 0x7cf07c, 255);

                /* 播放时间 / 总时间。时间取自 PTS，不是墙上时间，
                 * 所以循环回到开头时会跟着归零。 */
                fmt_time(cur, sizeof(cur), pos_s);
                fmt_time(tot, sizeof(tot), total_sec);
                if (clk.loop_count > 0)
                    snprintf(line, sizeof(line), "%s / %s  x%d",
                             cur, tot, clk.loop_count + 1);
                else
                    snprintf(line, sizeof(line), "%s / %s", cur, tot);
                osd_text(&osd, 6, 43, line, 0x8cc8ff, 255);

                snprintf(line, sizeof(line), "cpu %.0f%% dec %d",
                         cpu, c.decoded);
                osd_text(&osd, 6, 62, line, 0xffe066, 255);

                /* 剩余内存。主看 avail（真正可用的量），
                 * free 只是账面空闲值，因为大部分空闲内存被拿去当缓存了。 */
                {
                    int tot, fr, av;
                    read_mem(&tot, &fr, &av);
                    snprintf(line, sizeof(line), "mem %d/%dM free %dM",
                             av, tot, fr);
                    /* 可用低于 20MB 时标红提示 */
                    osd_text(&osd, 6, 81, line,
                             (av < 20) ? 0xff6666 : 0xc0a0ff, 255);
                }

                osd_flush(&osd);
                next_osd = now_us() + 250000;
            }

            /* 持两帧再还，保证 disp 扫的那帧不被复用 */
            if (held_n >= 2) {
                ReturnPicture(c.pdec, held[0]);
                held[0] = held[1];
                held[1] = pic;
            } else {
                held[held_n++] = pic;
            }

            c.presented++;
            if (c.presented == 1 || c.presented % 100 == 0) {
                printf("[player] 已显示 %d 帧, pts=%lld, fd=%d, %dx%d, fps=%.1f\n",
                       c.presented, (long long)pic->nPts, fd,
                       pic->nWidth, pic->nHeight, clk.fps_wall);
            }
        }
    }

    /* 6. 收尾 */
    set_state(&c, ST_EXIT);
    pthread_join(c.th_parser, NULL);
    pthread_join(c.th_decoder, NULL);

    for (i = 0; i < held_n; i++)
        if (held[i]) ReturnPicture(c.pdec, held[i]);

    printf("\n[player] ==== 统计 ====\n");
    printf("[player] submitted=%d decoded=%d presented=%d\n",
           c.submitted, c.decoded, c.presented);

    if (osd_ok)
        osd_deinit(&osd);

    /* 关掉自己占的两层：视频层 ch0/lyr0、OSD 层 ch1/lyr1。
     * 必须在 close(fd) 之前做，否则句柄关了就没法 ioctl 了。
     * 不关的话退出后画面会定格在最后一帧，下次启动还会叠加。 */
    disp_layer_disable(c.disp_fd, DISP_CHANNEL, DISP_VIDEO_LYR);
    if (osd_ok)
        disp_layer_disable(c.disp_fd, OSD_CHANNEL, OSD_LAYER);

    if (c.disp_fd >= 0) close(c.disp_fd);
    DestroyVideoDecoder(c.pdec);
    mp4_close(c.mp4);
    CdcMemClose(c.memops);

    return (c.presented > 0) ? 0 : 1;
}