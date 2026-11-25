#include <stdio.h>
#include "mp4parse.h"
int main(int argc,char**argv){
    mp4_ctx_t*m=mp4_open(argv[1]);
    if(!m)return 1;
    printf("mvhd.timescale=%u  mdhd.timescale=%u  dur=%ums samples=%u\n",m->timescale,m->media_timescale,m->media_duration_ms,mp4_sample_count(m));
    if(m->media_timescale) printf("每帧 = %.2f ms  → %.2f fps\n",
        1000.0*m->media_duration_ms/mp4_sample_count(m), 1000.0*mp4_sample_count(m)/m->media_duration_ms);
    mp4_sample_t s;int i;
    for(i=0;i<5;i++){ if(mp4_next_sample(m,&s)!=0)break; printf("  sample%d pts=%lld\n",i,(long long)s.pts);}
    mp4_close(m);return 0;
}
