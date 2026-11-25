/*
 * avcc2annexb.c — MP4 的 AVCC 长度前缀格式转 AnnexB 起始码格式
 *
 * 为什么需要：
 *   MP4 里 H.264 的 sample 数据用 AVCC 格式存放 —— 每个 NAL 前面是
 *   4 字节（长度由 avcC 的 lengthSizeMinusOne 决定）大端长度，但没有
 *   起始码。而解码器（libvdecoder / 硬件 VE）吃的是 AnnexB 格式，
 *   每个 NAL 前要有 00 00 00 01。
 *
 *   实测证据（/opt/chk 输出）：
 *     sample0 前 4 字节 = 00 01 9b 02 = 105218
 *     而 sample0 总长 = 105222，105222 - 4 = 105218  ← 完全吻合
 *     说明前面就是 4 字节长度前缀。
 *   而 extradata 是 00 00 00 01 67 ... 00 00 00 01 68（已经转成 AnnexB）。
 *
 *   两者混用会让解码器解不出任何帧：DecodeVideoStream 一直返回
 *   VDECODE_RESULT_NO_BITSTREAM(5)。这个现象已实测确认。
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* 把 AVCC 格式的一帧转成 AnnexB，写入 out（容量 outcap）。
 * 返回转换后的长度，失败返回 -1。
 * 典型膨胀：每个 NAL 多 4 字节，2~4 个 NAL 一帧，膨胀很小。 */
int avcc_to_annexb(const uint8_t *in, int inlen,
                   uint8_t *out, int outcap, int nalu_len_size)
{
    int pos = 0, opos = 0;

    if (nalu_len_size < 1 || nalu_len_size > 4)
        nalu_len_size = 4;
    if (inlen <= 0)
        return -1;

    while (pos + nalu_len_size <= inlen) {
        uint32_t nlen = 0;
        int i;

        /* 读大端长度 */
        for (i = 0; i < nalu_len_size; i++)
            nlen = (nlen << 8) | in[pos + i];
        pos += nalu_len_size;

        if (nlen == 0 || (int)(pos + nlen) > inlen)
            break;      /* 数据不完整，丢弃尾部 */

        if (opos + 4 + (int)nlen > outcap)
            return -1;

        out[opos++] = 0;
        out[opos++] = 0;
        out[opos++] = 0;
        out[opos++] = 1;
        memcpy(out + opos, in + pos, nlen);
        opos += (int)nlen;
        pos += (int)nlen;
    }

    return (opos > 0) ? opos : -1;
}