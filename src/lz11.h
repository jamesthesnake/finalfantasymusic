#ifndef LZ11_H
#define LZ11_H

#include "utils.h"

#include <stdio.h>

namespace rideau {

u32 getLZ11RawSize(FILE *f);
void decompressLZ11(FILE *src, u8 *dst, u32 dstSize);
void compressLZ11(const u8 *src, u32 srcSize, FILE *dst);

} // namespace rideau

#endif
