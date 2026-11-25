#ifndef AVCC2ANNEXB_H
#define AVCC2ANNEXB_H
#include <stdint.h>
int avcc_to_annexb(const uint8_t *in, int inlen,
                   uint8_t *out, int outcap, int nalu_len_size);
#endif
