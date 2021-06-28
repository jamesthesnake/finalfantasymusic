#include "file_utils.h"

namespace rideau {

u32 readu32le(FILE *f) {
  ENSURE(f != nullptr);

  u8 x[4];
  size_t readSize = fread(&x, sizeof(u8), ARRAY_SIZE(x), f);
  ENSURE(readSize == ARRAY_SIZE(x));
  return (x[3] << 24) | (x[2] << 16) | (x[1] << 8) | x[0];
}

u32 readu32le(const u8 **p) {
  ENSURE(p != nullptr);

  u32 x = ((*p)[3] << 24) | ((*p)[2] << 16) | ((*p)[1] << 8) | (*p)[0];
  *p += 4;
  return x;
}

void writeu32le(u8 **p, u32 x) {
  ENSURE(p != nullptr);

  (*p)[0] = x & 0xFF;
  (*p)[1] = (x >> 8) & 0xFF;
  (*p)[2] = (x >> 16) & 0xFF;
  (*p)[3] = (x >> 24) & 0xFF;
  *p += 4;
}

void writeu32le(FILE *f, u32 x) {
  ENSURE(f != nullptr);

  u8 xle[4];
  xle[0] = x & 0xFF;
  xle[1] = (x >> 8) & 0xFF;
  xle[2] = (x >> 16) & 0xFF;
  xle[3] = (x >> 24) & 0xFF;

  size_t writeSize = fwrite(xle, sizeof(u8), ARRAY_SIZE(xle), f);
  ENSURE(writeSize == ARRAY_SIZE(xle));
}

u16 readu16le(FILE *f) {
  ENSURE(f != nullptr);

  u8 x[2];
  size_t readSize = fread(&x, sizeof(u8), ARRAY_SIZE(x), f);
  ENSURE(readSize == ARRAY_SIZE(x));
  return (x[1] << 8) | x[0];
}

u16 readu16le(const u8 **p) {
  ENSURE(p != nullptr);

  u16 x = ((*p)[1] << 8) | (*p)[0];
  *p += 2;
  return x;
}

u8 readu8(const u8 **p) {
  ENSURE(p != nullptr);

  u8 x = (*p)[0];
  *p += 1;
  return x;
}

void writeu8(FILE *f, u8 x) {
  ENSURE(f != nullptr);

  size_t writeSize = fwrite(&x, sizeof(u8), 1, f);
  ENSURE(writeSize == 1);
}

u8 readu8(FILE *f) {
  ENSURE(f != nullptr);

  u8 x;
  size_t readSize = fread(&x, sizeof(u8), 1, f);
  ENSURE(readSize == 1);
  return x;
}

bool atFileEnd(FILE *f) {
  ENSURE(f != nullptr);

  u8 dumb;
  size_t readSize = fread(&dumb, sizeof(dumb), 1, f);
  if (readSize == 0) {
    ENSURE(feof(f));
    return true;
  } else
    return false;
}

}
