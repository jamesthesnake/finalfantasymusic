#include "lz11.h"

#include "file_utils.h"

#include <stdio.h>

namespace rideau {

u32 getLZ11RawSize(FILE *f) {
  int ret = fseek(f, 0, SEEK_SET);
  ENSURE(ret == 0);

  u32 size = readu32le(f);
  const u8 type = size & 0xFF;
  size >>= 8;

  ENSURE(type == 0x11); // LZ11

  return size;
}

void decompressLZ11(FILE *src, u8 *dst, u32 dstSize) {
  u32 size = getLZ11RawSize(src);
  ENSURE(dstSize >= size);

  u8 *out = dst;

  const u8 *fin = out + size;
  while (out < fin) {
    u8 flags = readu8(src);

    for (u8 bit = 0; bit < 8; ++bit) {
      if (out >= fin)
        break;

      if ((flags & 0x80) == 0) {
        *out++ = readu8(src);
      } else {
        const u8 x = readu8(src);
        const u8 hi = x >> 4;
        const u8 lo = x & 0xF;

        u16 len;
        u16 disp;

        if (hi == 1) {
          const u8 b0 = readu8(src);
          const u8 b1 = readu8(src);
          const u8 b2 = readu8(src);

          len = ((lo << 12) | (b0 << 4) | (b1 >> 4)) + 0x111;
          disp = (((b1 & 0xF) << 8) | b2) + 1;
        } else if (hi == 0) {
          const u8 b0 = readu8(src);
          const u8 b1 = readu8(src);

          len = ((lo << 4) | (b0 >> 4)) + 0x11;
          disp = (((b0 & 0xF) << 8) | b1) + 1;
        } else {
          const u8 b0 = readu8(src);

          len = hi + 1;
          disp = ((lo << 8) | b0) + 1;
        }

        for (u16 i = 0; i < len; ++i) {
          *out = *(out - disp);
          out++;
        }
      }

      flags <<= 1;
    }
  }
}

void compressLZ11(const u8 *src, u32 srcSize, FILE *dst) {
  ENSURE(src != nullptr);
  ENSURE(dst != nullptr);

  const u32 lz11Size = (srcSize << 8) | 0x11;
  writeu32le(dst, lz11Size);

  const u8 *const fin = src + srcSize;

  // TODO: actual compression
  while (src < fin) {
    u8 flags = 0x00;
    writeu8(dst, flags);

    for (u8 bit = 0; bit < 8; ++bit) {
      if (src < fin) {
        writeu8(dst, *src++);
      } else {
        writeu8(dst, 0);
      }
    }
  }
}

} // namespace rideau
