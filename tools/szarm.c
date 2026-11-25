#include <stdio.h>
#include <stddef.h>
typedef unsigned int u32;
struct snd_mask { u32 bits[8]; };
struct snd_interval { u32 min,max; u32 openmin:1,openmax:1,integer:1,empty:1; };
struct arm_hw_params {
    u32 flags;
    struct snd_mask masks[3];
    struct snd_mask mres[5];
    struct snd_interval intervals[12];
    struct snd_interval ires[9];
    u32 rmask, cmask, info, msbits, rate_num, rate_den;
    unsigned long fifo_size;
    unsigned char reserved[64];
};
struct arm_xferi { long result; void*buf; unsigned long frames; };
int main(void){
    printf("sizeof(hw_params)=%zu\n",sizeof(struct arm_hw_params));
    printf("sizeof(xferi)=%zu\n",sizeof(struct arm_xferi));
    printf("off(rmask)=%zu off(intervals)=%zu\n",
        offsetof(struct arm_hw_params,rmask),offsetof(struct arm_hw_params,intervals));
    return 0;
}
