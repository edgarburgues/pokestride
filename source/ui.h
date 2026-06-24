/*
 * citro2d wrapper for PokeStride.
 *
 * The key piece for the emulator is the walker LCD texture: a 96x64 pixel
 * buffer uploaded to a 128x128 RGBA8 GPU texture every frame and drawn as a
 * scaled quad on the top screen.
 */
#ifndef POKESTRIDE_UI_H
#define POKESTRIDE_UI_H

#include <citro2d.h>
#include <citro3d.h>
#include <3ds.h>
#include <stdbool.h>
#include <stdint.h>

#define TOP_W       400
#define TOP_H       240
#define BOT_W       320
#define BOT_H       240

/* Walker LCD source dimensions. Upload uploads exactly this many
 * pixels; the GPU texture is the next power of two (128x128). */
#define LCD_TEX_W    96
#define LCD_TEX_H    64

/* Colour palette (ABGR8 — citro2d packs colours alpha-high, BGR low). */
extern const u32 COL_BG;
extern const u32 COL_TITLEBG;
extern const u32 COL_TITLEFG;
extern const u32 COL_TEXT;
extern const u32 COL_DIM;
extern const u32 COL_ACCENT;       /* highlight / Pokewalker yellow */
extern const u32 COL_BTN_BG;
extern const u32 COL_BTN_BG_PRESSED;
extern const u32 COL_BTN_FG;
extern const u32 COL_BTN_BORDER;
extern const u32 COL_OK;
extern const u32 COL_ERR;
extern const u32 COL_FOOTBG;
extern const u32 COL_FOOTFG;

bool ui_init(void);
void ui_fini(void);

/* Frame lifecycle. frame_begin clears both screens and scenes the top
 * by default. Call scene_bottom() to draw on the bottom, scene_top()
 * to switch back. frame_end submits and swaps. */
void frame_begin(void);
void frame_end(void);
void scene_top(void);
void scene_bottom(void);

/* Drawing primitives. */
void draw_text(float x, float y, float size, u32 col, const char *s);
void draw_text_centered(float cx, float y, float size, u32 col, const char *s);
__attribute__((format(printf, 5, 6)))
void draw_textf(float x, float y, float size, u32 col, const char *fmt, ...);
void draw_rect(float x, float y, float w, float h, u32 col);
void draw_frame(float x, float y, float w, float h, float t, u32 col);

/* Walker LCD texture — call _init once after ui_init(), _upload every
 * frame after fillVideoBuffer(), and _draw inside a scene_top(). */
bool lcd_tex_init(void);
void lcd_tex_fini(void);
void lcd_tex_upload(const uint32_t *pixels_96x64);
void lcd_tex_draw(float x, float y, float scale);

/* Touch button helper: draws a labeled rounded-rectangle button and
 * returns true on the rising edge of a tap inside its rect this frame.
 * `pressed` shades the fill so we can flash the button visually. */
bool ui_button(float x, float y, float w, float h, const char *label,
               bool pressed_this_frame);

#endif
