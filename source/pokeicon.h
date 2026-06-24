/*
 * HGSS box icon sprites (32x32) loaded into a citro2d texture atlas at boot.
 *
 * Reads three data files under sdmc:/3ds/pokeStride/data/:
 *   pokeicon_glyphs.bin    POKEICON_COUNT x 512 B raw NCGR tiles
 *   pokeicon_palettes.bin  3 x 32 B    BGR555 palettes
 *   pokeicon_pal_index.bin POKEICON_COUNT B  per-species palette id
 *
 * If any file is missing pokeicon_init returns false and the rest of the
 * emulator runs without colour icons; callers must check pokeicon_loaded()
 * before drawing.
 */
#ifndef POKESTRIDE_POKEICON_H
#define POKESTRIDE_POKEICON_H

#include <citro2d.h>
#include <stdbool.h>
#include <stdint.h>

#define POKEICON_DIM    32
#define POKEICON_COUNT 544

bool pokeicon_init(void);
void pokeicon_fini(void);
bool pokeicon_loaded(void);
bool pokeicon_get(uint16_t species, C2D_Image *out);

#endif
