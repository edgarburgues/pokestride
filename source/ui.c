/*
 * citro2d UI primitives + walker LCD texture for PokeStride.
 *
 * The LCD texture is the one non-trivial piece. PICA200 textures are
 * morton-tiled in VRAM, so a linear 128x128 RGBA8 staging buffer holds the
 * walker pixels in its top-left 96x64 region each frame, then
 * C3D_SyncDisplayTransfer converts linear -> tiled into the GPU-side
 * texture. citro2d draws it via a normal C2D_Image with the subregion
 * clipped via Tex3DS_SubTexture.
 */
#include "ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Colour palette (ABGR8888). citro2d packs alpha in the high byte then
 * B/G/R, so 0xAABBGGRR. */
const u32 COL_BG             = 0xFF1B1B1F;   /* almost black */
const u32 COL_TITLEBG        = 0xFFFFFFFF;
const u32 COL_TITLEFG        = 0xFF161A1A;
const u32 COL_TEXT           = 0xFFE0E0E0;
const u32 COL_DIM            = 0xFF8A8A8A;
const u32 COL_ACCENT         = 0xFF2DD4F4;   /* Pokewalker yellow (BGR'd) */
const u32 COL_BTN_BG         = 0xFF2F2F36;
const u32 COL_BTN_BG_PRESSED = 0xFFC4427A;
const u32 COL_BTN_FG         = 0xFFFFFFFF;
const u32 COL_BTN_BORDER     = 0xFF606068;
const u32 COL_OK             = 0xFF40C840;
const u32 COL_ERR            = 0xFF5050FF;
const u32 COL_FOOTBG         = 0xFF2B2B2E;
const u32 COL_FOOTFG         = 0xFFFFFFFF;

static C3D_RenderTarget *g_top    = NULL;
static C3D_RenderTarget *g_bottom = NULL;
static C2D_TextBuf       g_tbuf   = NULL;

/* Walker LCD texture state. */
#define LCD_POT      128                          /* next POT >= max(96,64) */
#define LCD_STRIDE   (LCD_POT * 4)                /* RGBA8 row stride */
static C3D_Tex            g_lcd_tex;
static Tex3DS_SubTexture  g_lcd_subtex;
static C2D_Image          g_lcd_img;
static uint32_t          *g_lcd_linear = NULL;    /* linear staging */
static bool               g_lcd_ready  = false;

/* ------------------------------------------------------------------ */
/* lifecycle                                                          */
/* ------------------------------------------------------------------ */

bool ui_init(void) {
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    g_top    = C2D_CreateScreenTarget(GFX_TOP,    GFX_LEFT);
    g_bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    g_tbuf   = C2D_TextBufNew(4096);
    return g_top && g_bottom && g_tbuf;
}

void ui_fini(void) {
    lcd_tex_fini();
    if (g_tbuf) { C2D_TextBufDelete(g_tbuf); g_tbuf = NULL; }
    C2D_Fini();
    C3D_Fini();
    gfxExit();
}

void frame_begin(void) {
    C2D_TextBufClear(g_tbuf);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(g_top,    COL_BG);
    C2D_TargetClear(g_bottom, COL_BG);
    C2D_SceneBegin(g_top);
}

void frame_end(void) {
    C3D_FrameEnd(0);
}

void scene_top(void)    { C2D_SceneBegin(g_top); }
void scene_bottom(void) { C2D_SceneBegin(g_bottom); }

/* ------------------------------------------------------------------ */
/* drawing primitives                                                 */
/* ------------------------------------------------------------------ */

void draw_text(float x, float y, float size, u32 col, const char *s) {
    if (!s || !*s) return;
    C2D_Text t;
    C2D_TextFontParse(&t, NULL, g_tbuf, s);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, x, y, 0.5f, size, size, col);
}

void draw_text_centered(float cx, float y, float size, u32 col, const char *s) {
    if (!s || !*s) return;
    C2D_Text t;
    C2D_TextFontParse(&t, NULL, g_tbuf, s);
    C2D_TextOptimize(&t);
    float w = 0, h = 0;
    C2D_TextGetDimensions(&t, size, size, &w, &h);
    C2D_DrawText(&t, C2D_WithColor, cx - w * 0.5f, y, 0.5f, size, size, col);
}

void draw_textf(float x, float y, float size, u32 col, const char *fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    draw_text(x, y, size, col, buf);
}

void draw_rect(float x, float y, float w, float h, u32 col) {
    C2D_DrawRectSolid(x, y, 0.0f, w, h, col);
}

void draw_frame(float x, float y, float w, float h, float t, u32 col) {
    draw_rect(x,         y,         w, t, col);
    draw_rect(x,         y + h - t, w, t, col);
    draw_rect(x,         y,         t, h, col);
    draw_rect(x + w - t, y,         t, h, col);
}

/* ------------------------------------------------------------------ */
/* walker LCD texture                                                 */
/* ------------------------------------------------------------------ */

bool lcd_tex_init(void) {
    if (g_lcd_ready) return true;

    if (!C3D_TexInit(&g_lcd_tex, LCD_POT, LCD_POT, GPU_RGBA8)) {
        return false;
    }
    C3D_TexSetFilter(&g_lcd_tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&g_lcd_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    /* Tex3DS u/v are normalised, with y flipped (top=1.0). We expose
     * just the top-left 96x64 sub-region of the 128x128 texture. */
    g_lcd_subtex.width  = LCD_TEX_W;
    g_lcd_subtex.height = LCD_TEX_H;
    g_lcd_subtex.left   = 0.0f;
    g_lcd_subtex.top    = 1.0f;
    g_lcd_subtex.right  = (float)LCD_TEX_W / (float)LCD_POT;
    g_lcd_subtex.bottom = 1.0f - (float)LCD_TEX_H / (float)LCD_POT;

    g_lcd_img.tex    = &g_lcd_tex;
    g_lcd_img.subtex = &g_lcd_subtex;

    g_lcd_linear = (uint32_t *)linearAlloc(LCD_POT * LCD_POT * 4);
    if (!g_lcd_linear) {
        C3D_TexDelete(&g_lcd_tex);
        return false;
    }
    memset(g_lcd_linear, 0, LCD_POT * LCD_POT * 4);
    g_lcd_ready = true;
    return true;
}

void lcd_tex_fini(void) {
    if (!g_lcd_ready) return;
    if (g_lcd_linear) { linearFree(g_lcd_linear); g_lcd_linear = NULL; }
    C3D_TexDelete(&g_lcd_tex);
    g_lcd_ready = false;
}

/* Walker fillVideoBuffer writes pixels as 0xAARRGGBB, with alpha = 0x00 in
 * the palette. On the GPU alpha=0 means fully transparent, which would tint
 * the walker's grayscale with the background, so alpha is forced to 0xFF
 * here. PICA200's GPU_RGBA8 format stores bytes in memory as [A, B, G, R]
 * (despite the "RGBA" name), so the written uint32 must be
 * (R<<24) | (G<<16) | (B<<8) | A. */
void lcd_tex_upload(const uint32_t *pixels_96x64) {
    if (!g_lcd_ready || !pixels_96x64) return;

    for (int y = 0; y < LCD_TEX_H; y++) {
        const uint32_t *src = pixels_96x64 + y * LCD_TEX_W;
        uint32_t *dst = g_lcd_linear + y * LCD_POT;
        for (int x = 0; x < LCD_TEX_W; x++) {
            uint32_t c = src[x];
            uint32_t r = (c >> 16) & 0xFF;
            uint32_t g = (c >>  8) & 0xFF;
            uint32_t b =  c        & 0xFF;
            dst[x] = (r << 24) | (g << 16) | (b << 8) | 0xFFu;
        }
    }

    GSPGPU_FlushDataCache(g_lcd_linear, LCD_POT * LCD_POT * 4);
    C3D_SyncDisplayTransfer(
        (u32 *)g_lcd_linear,    GX_BUFFER_DIM(LCD_POT, LCD_POT),
        (u32 *)g_lcd_tex.data,  GX_BUFFER_DIM(LCD_POT, LCD_POT),
        GX_TRANSFER_OUT_TILED(1) |
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
        GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
        GX_TRANSFER_FLIP_VERT(0)
    );
}

void lcd_tex_draw(float x, float y, float scale) {
    if (!g_lcd_ready) return;
    C2D_DrawImageAt(g_lcd_img, x, y, 0.5f, NULL, scale, scale);
}

/* ------------------------------------------------------------------ */
/* button widget                                                       */
/* ------------------------------------------------------------------ */

bool ui_button(float x, float y, float w, float h, const char *label,
               bool pressed_this_frame) {
    u32 bg = pressed_this_frame ? COL_BTN_BG_PRESSED : COL_BTN_BG;
    draw_rect(x, y, w, h, bg);
    draw_frame(x, y, w, h, 1.5f, COL_BTN_BORDER);
    if (label) {
        draw_text_centered(x + w * 0.5f, y + h * 0.5f - 7.0f, 0.55f,
                            COL_BTN_FG, label);
    }
    return pressed_this_frame;
}
