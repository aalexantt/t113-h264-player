#include <stdio.h>
#include <stddef.h>
#include "vdecoder.h"
int main(void)
{
    printf("  sizeof: VConfig=%zu VideoStreamInfo=%zu VideoPicture=%zu\n",
           sizeof(VConfig), sizeof(VideoStreamInfo), sizeof(VideoPicture));
    printf("  offset: eOutputPixelFormat=%zu  nDisplayHoldingFrameBufferNum=%zu"
           "  nDecodeSmoothFrameBufferNum=%zu  memops=%zu\n",
           offsetof(VConfig, eOutputPixelFormat),
           offsetof(VConfig, nDisplayHoldingFrameBufferNum),
           offsetof(VConfig, nDecodeSmoothFrameBufferNum),
           offsetof(VConfig, memops));
    return 0;
}
