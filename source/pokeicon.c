/*
 * HGSS box icon atlas: loads the per-species icon data into a citro2d
 * texture atlas at boot.
 */
#include "pokeicon.h"
#include "log.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DATA_DIR    "sdmc:/3ds/pokeStride/data"
#define ATLAS_W     1024
#define ATLAS_H     1024
#define ATLAS_COLS  (ATLAS_W / POKEICON_DIM)

#define GLYPH_FILE  DATA_DIR "/pokeicon_glyphs.bin"
#define PAL_FILE    DATA_DIR "/pokeicon_palettes.bin"
#define IDX_FILE    DATA_DIR "/pokeicon_pal_index.bin"

#define GLYPH_TOTAL  (POKEICON_COUNT * 512u)
#define PAL_TOTAL    (3u * 16u * 2u)
#define IDX_TOTAL    ((unsigned)POKEICON_COUNT)

static C3D_Tex            g_tex;
static Tex3DS_SubTexture  g_subtex[POKEICON_COUNT];
static bool               g_loaded = false;

bool pokeicon_loaded(void) { return g_loaded; }

static bool load_file(const char *path, void *dst, size_t expected) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        LOG("pokeicon: missing %s", path);
        return false;
    }
    size_t got = fread(dst, 1, expected, f);
    fclose(f);
    if (got != expected) {
        LOG("pokeicon: short read %s (%zu / %zu)", path, got, expected);
        return false;
    }
    return true;
}

static inline void bgr555_to_rgb(uint16_t c, uint8_t *r, uint8_t *g, uint8_t *b) {
    uint8_t r5 = (uint8_t)((c       ) & 0x1F);
    uint8_t g5 = (uint8_t)((c >>  5 ) & 0x1F);
    uint8_t b5 = (uint8_t)((c >> 10 ) & 0x1F);
    *r = (uint8_t)((r5 << 3) | (r5 >> 2));
    *g = (uint8_t)((g5 << 3) | (g5 >> 2));
    *b = (uint8_t)((b5 << 3) | (b5 >> 2));
}

/* Decode one species's 32x32 frame-0 icon into a 32x32 block at
 * (dst_x, dst_y) within a `stride`-px-wide linear RGBA buffer.
 *   tiles : 16 tiles of 8x8 4bpp, laid out 4x4 (= 512 B)
 *   pal   : 16 colours BGR555 LE
 * Pixel byte order in the linear buffer is (A, B, G, R) — matches
 * PICA200 GPU_RGBA8 memory layout. */
static void decode_icon(const uint8_t *tiles, const uint8_t *pal,
                        uint8_t *linear, int stride, int dst_x, int dst_y)
{
    for (int ty = 0; ty < 4; ty++) {
        for (int tx = 0; tx < 4; tx++) {
            const uint8_t *tile = tiles + (ty * 4 + tx) * 32;
            for (int py = 0; py < 8; py++) {
                for (int px = 0; px < 8; px += 2) {
                    uint8_t pair = tile[py * 4 + (px / 2)];
                    uint8_t i_lo = (uint8_t)(pair & 0x0F);
                    uint8_t i_hi = (uint8_t)((pair >> 4) & 0x0F);
                    for (int k = 0; k < 2; k++) {
                        uint8_t idx = (k == 0) ? i_lo : i_hi;
                        int px_x = dst_x + tx * 8 + px + k;
                        int px_y = dst_y + ty * 8 + py;
                        uint8_t *p = linear + (px_y * stride + px_x) * 4;
                        if (idx == 0) {
                            p[0] = p[1] = p[2] = p[3] = 0;
                        } else {
                            uint16_t c = (uint16_t)pal[idx * 2]
                                       | (uint16_t)(pal[idx * 2 + 1] << 8);
                            uint8_t r, g, b;
                            bgr555_to_rgb(c, &r, &g, &b);
                            p[0] = 0xFF; p[1] = b; p[2] = g; p[3] = r;
                        }
                    }
                }
            }
        }
    }
}

bool pokeicon_init(void) {
    uint8_t *glyphs   = NULL;
    uint8_t *palettes = NULL;
    uint8_t *pal_idx  = NULL;
    uint8_t *linear   = NULL;
    bool     ok       = false;

    glyphs   = (uint8_t *)malloc(GLYPH_TOTAL);
    palettes = (uint8_t *)malloc(PAL_TOTAL);
    pal_idx  = (uint8_t *)malloc(IDX_TOTAL);
    if (!glyphs || !palettes || !pal_idx) {
        LOG("pokeicon: malloc failed");
        goto cleanup;
    }
    if (!load_file(GLYPH_FILE, glyphs,   GLYPH_TOTAL)) goto cleanup;
    if (!load_file(PAL_FILE,   palettes, PAL_TOTAL))   goto cleanup;
    if (!load_file(IDX_FILE,   pal_idx,  IDX_TOTAL))   goto cleanup;

    linear = (uint8_t *)linearAlloc((size_t)ATLAS_W * ATLAS_H * 4);
    if (!linear) {
        LOG("pokeicon: linearAlloc(%u) failed",
            (unsigned)((size_t)ATLAS_W * ATLAS_H * 4));
        goto cleanup;
    }
    memset(linear, 0, (size_t)ATLAS_W * ATLAS_H * 4);

    for (int sp = 0; sp < POKEICON_COUNT; sp++) {
        int col = sp % ATLAS_COLS;
        int row = sp / ATLAS_COLS;
        int dx  = col * POKEICON_DIM;
        int dy  = row * POKEICON_DIM;
        uint8_t pal_id = (pal_idx[sp] < 3) ? pal_idx[sp] : 0;
        decode_icon(glyphs   + sp * 512,
                    palettes + pal_id * 32,
                    linear, ATLAS_W, dx, dy);
    }

    if (!C3D_TexInit(&g_tex, ATLAS_W, ATLAS_H, GPU_RGBA8)) {
        LOG("pokeicon: C3D_TexInit failed");
        goto cleanup;
    }

    GSPGPU_FlushDataCache(linear, (size_t)ATLAS_W * ATLAS_H * 4);
    u32 flags = GX_TRANSFER_FLIP_VERT(0)
              | GX_TRANSFER_OUT_TILED(1)
              | GX_TRANSFER_RAW_COPY(0)
              | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8)
              | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8)
              | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
    C3D_SyncDisplayTransfer((u32 *)linear,
                            GX_BUFFER_DIM(ATLAS_W, ATLAS_H),
                            (u32 *)g_tex.data,
                            GX_BUFFER_DIM(ATLAS_W, ATLAS_H),
                            flags);

    for (int sp = 0; sp < POKEICON_COUNT; sp++) {
        int col = sp % ATLAS_COLS;
        int row = sp / ATLAS_COLS;
        Tex3DS_SubTexture *st = &g_subtex[sp];
        st->width  = POKEICON_DIM;
        st->height = POKEICON_DIM;
        st->left   = (col * POKEICON_DIM) / (float)ATLAS_W;
        st->right  = ((col + 1) * POKEICON_DIM) / (float)ATLAS_W;
        st->top    = 1.0f - (row * POKEICON_DIM) / (float)ATLAS_H;
        st->bottom = 1.0f - ((row + 1) * POKEICON_DIM) / (float)ATLAS_H;
    }

    LOG("pokeicon: loaded %d icons into %dx%d RGBA atlas",
        POKEICON_COUNT, ATLAS_W, ATLAS_H);
    ok = true;
    g_loaded = true;

cleanup:
    if (linear)   linearFree(linear);
    if (glyphs)   free(glyphs);
    if (palettes) free(palettes);
    if (pal_idx)  free(pal_idx);
    return ok;
}

void pokeicon_fini(void) {
    if (g_loaded) {
        C3D_TexDelete(&g_tex);
        g_loaded = false;
    }
}

bool pokeicon_get(uint16_t species, C2D_Image *out) {
    if (!g_loaded || species >= POKEICON_COUNT || !out) return false;
    out->tex    = &g_tex;
    out->subtex = &g_subtex[species];
    return true;
}
