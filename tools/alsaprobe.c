#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <stdint.h>

typedef unsigned int u32;
struct snd_mask { u32 bits[8]; };
struct snd_interval { u32 min,max; u32 openmin:1,openmax:1,integer:1,empty:1; };
struct hw_params_t {
    u32 flags;
    struct snd_mask masks[3];
    struct snd_mask mres[5];
    struct snd_interval intervals[12];
    struct snd_interval ires[9];
    u32 rmask, cmask, info, msbits, rate_num, rate_den;
    unsigned long fifo_size;
    unsigned char reserved[64];
};
/* 严格照内核 struct snd_pcm_info 定义 */
struct sync_id { unsigned char id[16]; };
struct pcm_info_t {
    unsigned int device, subdevice;
    int stream, card;
    unsigned char id[64];
    unsigned char name[80];
    unsigned char subname[32];
    int dev_class, dev_subclass;
    unsigned int subdevices_count, subdevices_avail;
    struct sync_id sync;
    unsigned char reserved[64];
};
#define IOCTL_INFO       _IOR('A',0x01,struct pcm_info_t)
#define IOCTL_HW_PARAMS  _IOWR('A',0x11,struct hw_params_t)
#define IOCTL_PREPARE    _IO('A',0x40)

int main(void){
    int fd,r;
    struct pcm_info_t info;
    struct hw_params_t p;

    fd=open("/dev/snd/pcmC0D0p",O_RDWR);
    if(fd<0){printf("open 失败: %s\n",strerror(errno));return 1;}
    printf("sizeof(info)=%d sizeof(hw_params)=%d\n",
        (int)sizeof(struct pcm_info_t),(int)sizeof(struct hw_params_t));

    memset(&info,0,sizeof(info));
    r=ioctl(fd,IOCTL_INFO,&info);
    printf("INFO: r=%d %s\n",r,r<0?strerror(errno):"OK");
    if(r==0){
        printf("  stream=%d card=%d subdev=%u/%u\n",
            info.stream,info.card,info.subdevices_avail,info.subdevices_count);
        printf("  name='%s'\n",info.name);
    }

    memset(&p,0,sizeof(p));
    r=ioctl(fd,IOCTL_HW_PARAMS,&p);
    printf("HW_PARAMS: r=%d %s\n",r,r<0?strerror(errno):"OK");
    if(r==0){
        printf("  默认 rate=%u~%u ch=%u~%u\n",
            p.intervals[3].min,p.intervals[3].max,
            p.intervals[2].min,p.intervals[2].max);
        r=ioctl(fd,IOCTL_PREPARE);
        printf("PREPARE: r=%d %s\n",r,r<0?strerror(errno):"OK");
    }

    /* 完整指定：access+format+channels+rate+period+buffer */
    {
        int i;
        memset(&p,0,sizeof(p));
        /* masks 全部先置全 1，再按需清 */
        for(i=0;i<3;i++) memset(p.masks[i].bits,0xff,sizeof(p.masks[i].bits));
        /* intervals 全部设成宽松区间 */
        for(i=0;i<12;i++){p.intervals[i].min=0;p.intervals[i].max=~0u;p.intervals[i].empty=0;p.intervals[i].integer=0;}
        /* access: RW_INTERLEAVED(3) */
        memset(p.masks[0].bits,0,sizeof(p.masks[0].bits));
        p.masks[0].bits[0] |= 1u<<3;
        /* format: S16_LE(2) */
        memset(p.masks[1].bits,0,sizeof(p.masks[1].bits));
        p.masks[1].bits[0] |= 1u<<2;
        /* 具体值 */
        p.intervals[2].min=p.intervals[2].max=2;   p.intervals[2].integer=1;  /* CHANNELS */
        p.intervals[3].min=p.intervals[3].max=48000; p.intervals[3].integer=1; /* RATE */
        p.rmask=0;   /* 让内核自己挑，不给约束 */
        r=ioctl(fd,IOCTL_HW_PARAMS,&p);
        printf("HW_PARAMS(full): r=%d %s\n",r,r<0?strerror(errno):"OK");
        if(r==0){
            printf("  生效 rate=%u ch=%u buffer=%u frames period=%u\n",
                p.intervals[3].min,p.intervals[2].min,
                p.intervals[9].min,p.intervals[5].min);
        }
    }
    close(fd);return 0;
}
