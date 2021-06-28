#ifndef FILE_UTILS_H
#define FILE_UTILS_H

#include <stdio.h>

#include "utils.h"


namespace rideau {

u32 readu32le(FILE *f);
u32 readu32le(const u8 **p);
void writeu32le(u8 **p, u32 x);
void writeu32le(FILE *f, u32 x);
u16 readu16le(FILE *f);
u16 readu16le(const u8 **p);
u8 readu8(const u8 **p);
void writeu8(FILE *f, u8 x);
u8 readu8(FILE *f);
bool atFileEnd(FILE *f);

} // namespace rideau

#endif
