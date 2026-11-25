#include <stdio.h>
#include <math.h>
#include "alsa_out.h"
int main(void){
    alsa_out_t a;
    static short buf[4800];
    int i,r;
    if(alsa_open(&a,"/dev/snd/pcmC0D0p",48000,2)!=0){printf("打开失败\n");return 1;}
    for(i=0;i<4800;i++){
        double t=(double)i/48000.0;
        buf[i]=(short)(14000*sin(2*M_PI*440*t));
    }
    for(r=0;r<50;r++){
        int w=alsa_write(&a,buf,sizeof(buf));
        if(w<0){printf("写入失败 r=%d\n",r);break;}
    }
    printf("已写 %llu 字节 = %lld ms\n",
        (unsigned long long)a.bytes_written,(long long)alsa_played_ms(&a));
    alsa_close(&a);
    return 0;
}
