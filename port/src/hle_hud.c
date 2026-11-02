/**
 * The title's 2D sprites, for true 16:9 (aspect '16:9' in F10 / hpcos.toml).
 *
 * Every 2D piece the title draws (HUD, text, menus) goes through sub_0001A360, AddSprite(def, xform): it writes a
 * sprite of 4 vertices (0x1C bytes each, x y first) into the pool at 0x4A0D78 (0x70 bytes per sprite, the next index
 * at 0x4BCDF4) and files it under a texture bucket; sub_0001A630 draws the buckets once a frame. In 16:9 the D3D12
 * renderer squeezes the 2D to 3/4 width about the centre (d3d12.cpp Squeeze2D), which keeps it 4:3 in the middle;
 * here sprites of HUD elements are moved so that, squeezed, they sit against the screen side they belong to.
 *
 * F12 also logs the next frame's sprites ([SPRITE]: the call chain above AddSprite and the sprite's rectangle),
 * which is how the elements below were found.
 *
 * Built into the recompile with --exclude-manual ../hpcos/src/hle_hud.c; gen/recomp_0000.c's generated bodies are
 * renamed sub_0001A360_gen by hand until then.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#define RECOMP_GENERATED_CODE
#include "recomp/gen/recomp_types.h"

extern void sub_0001A360_gen(void);
extern int hpcos_HudTracing(void);  /* renderer/xdk/hle_d3d8.cpp: F12's trace frame */
extern int hpcos_Widescreen(void);  /* renderer/xdk/hle_d3d8.cpp: booted in 16:9 */

#define SPRITE_POOL   0x004A0D78u
#define SPRITE_BYTES  0x70u
#define SPRITE_NEXT   0x004BCDF4u
#define VERTEX_BYTES  0x1Cu
#define LEVEL_INDEX   0x00527B78u  /* the progress block's first word */
#define LEVEL_QUIDDITCH 3u

/* the title's return addresses on the stack from esp: up to n, a call before each */
static int call_chain(uint32_t sp, uint32_t *out, int n)
{
    int found = 0;
    for (uint32_t i = 0; i < 96 && found < n; i++) {
        uint32_t v = MEM32(sp + 4 * i);
        if (v < 0x11005 || v >= 0x130620) continue;
        if (MEM8(v - 5) == 0xE8 || MEM8(v - 2) == 0xFF || MEM8(v - 3) == 0xFF || MEM8(v - 6) == 0xFF)
            out[found++] = v;
    }
    return found;
}

/* HUD elements by the title's code that draws them (a return address in the chain above AddSprite) */
enum { CENTRE, LEFT, RIGHT, BY_HALF, WIDE, BACKDROP, FRONT, COUNTER, BY_HEIGHT, QUIDDITCH_RIGHT, QUIDDITCH_MIDDLE };
/* out to a 16:9 side: 80 / 0.75 = 106.67 rounded to whole pixels (the squeeze leaves it 0.25 px past the side) */
#define SIDE_SHIFT 107.0f
/* QUIDDITCH_RIGHT: the right team's crest ends at x 550, 90 from the 4:3 side, the left one's starts 29.5 from its
 * side; this much further out mirrors them (whole pixels) */
#define QUIDDITCH_MIRROR 61.0f
/* QUIDDITCH_MIDDLE: the bars with the Snitch show from x 242 (the lower bar's icon) to 370 (the Snitch's tip),
 * centred on 306; this much to the right puts them on the screen's middle */
#define QUIDDITCH_CENTRE 14.0f
#define COUNTER_DROP 45.0f  /* COUNTER: the left side, and this much lower (its bottom from y 410 to 455 of 480) */
/* also: a second return address the chain must hold, 0 for none */
static const struct { uint32_t ret, also; int side; } k_elements[] = {
    {0x0004B3EDu, 0, BY_HALF},  /* HUD icons: the button cluster (top right) and the counter's icon (bottom left) */
    {0x0004B474u, 0, BY_HALF},  /* the same icons drawn over the inventory */
    /* the life meters, icon and bar, through the HUD's own sprite submitter (0x1BE13 in sub_0001BC70, called by the
     * HUD code only): Harry's at the top left (gameplay and inventory), an enemy's at the bottom right */
    {0x0001BE13u, 0, BY_HEIGHT},
    {0x000A9A20u, 0, LEFT},     /* the inventory's L button, under its green arrow */
    {0x000A9A80u, 0, RIGHT},    /* the inventory's R button */
    {0x000AD634u, 0, RIGHT},    /* the button cluster's prompt ("Dodge", "Talk To") */
    {0x000AD6AAu, 0, RIGHT},    /* the X button's prompt beside the cluster ("Decline") */
    {0x000AD80Fu, 0, COUNTER},  /* the counter's number (sickles, cards, Remembrall) */
    {0x000AC874u, 0, COUNTER},  /* the beans counter's number */
};

static int in_chain(const uint32_t *chain, int depth, uint32_t ret)
{
    for (int i = 0; i < depth; i++)
        if (chain[i] == ret) return 1;
    return 0;
}

static int in_range(const uint32_t *chain, int depth, uint32_t lo, uint32_t hi)
{
    for (int i = 0; i < depth; i++)
        if (chain[i] >= lo && chain[i] < hi) return 1;
    return 0;
}

/* a sprite's corners: x or y (off 0 or 4), lowest and highest */
static void sprite_span(uint32_t s, uint32_t off, float *lo, float *hi)
{
    *lo = 1e9f;
    *hi = -1e9f;
    for (int v = 0; v < 4; v++) {
        float a = MEMF(s + v * VERTEX_BYTES + off);
        if (a < *lo) *lo = a;
        if (a > *hi) *hi = a;
    }
}

static float sprite_height(uint32_t s)
{
    float lo, hi;
    sprite_span(s, 4, &lo, &hi);
    return hi - lo;
}

/* frames, counted where the pool starts again, and the last one with a front end backdrop (BACKDROP) */
static uint32_t g_frame, g_front_frame = 0xFFFFFFF0u;

static int front_end_screen(void)
{
    return g_frame - g_front_frame <= 1;
}

/* the last frame with the title's blue mist (WIDE), which only the main menu's backdrop has */
static uint32_t g_mist_frame = 0xFFFFFFF0u;

static int title_screen(void)
{
    return g_frame - g_mist_frame <= 1;
}

/* the last frame with a loading screen's picture (0x1BC5D: the full-width picture with its frame and bar track) */
static uint32_t g_loading_frame = 0xFFFFFFF0u;

static int loading_screen(void)
{
    return g_frame - g_loading_frame <= 1;
}

/* a screen the title lays out 4:3 (the inventory's camera projects 4 / 3; hle_d3d8.cpp's Swap shows it narrowed to the
 * middle): only the life meter and the button cluster at the top go out to the sides there */
static int four_three_screen(void)
{
    float num = MEMF(0x0050C2B8), den = MEMF(0x0050C2BC);
    float r = den != 0.0f ? num / den : 0.0f;
    return r > 4.0f / 3.0f - 0.01f && r < 4.0f / 3.0f + 0.01f;
}

static int element_side(const uint32_t *chain, int depth, uint32_t sprite, uint32_t def)
{
    int menu = four_three_screen();
    /* the main menu's backdrop (0x4D42F): two layers of three strips across the screen, the blue mist (textures
     * 0x22 to 0x24 of its set) and the logo over it (0x11 to 0x13). The mist, fading to black at its sides, is
     * spread over the 16:9 width; the logo keeps its shape in the middle */
    /* a loading screen (sprites through 0x1BC5D): the picture spans the frame and fills 16:9 by itself; its green
     * progress bar (a short sprite growing from the left) is widened with it, and the word LOADING (0x4B2ED) moves
     * out with the picture's left side */
    if (in_chain(chain, depth, 0x0001BC5Du)) {
        float lo, hi, top, bottom;
        sprite_span(sprite, 0, &lo, &hi);
        sprite_span(sprite, 4, &top, &bottom);
        if (lo <= 1.0f && hi >= 639.0f && bottom - top > 200.0f) g_loading_frame = g_frame;
        if (loading_screen() && bottom - top < 40.0f && top > 300.0f) return WIDE;
    }
    if (loading_screen() && in_chain(chain, depth, 0x0004B2EDu)) return FRONT;
    if (in_chain(chain, depth, 0x0004D42Fu) && def && sprite_height(sprite) > 400.0f) {
        uint32_t tex = MEM32(def + 8);
        if (tex >= 0x22 && tex <= 0x24) {
            g_mist_frame = g_frame;
            return WIDE;
        }
        /* the logo's textures depend on the language's panel set (panel.geo 0x11 to 0x13, panel_ita.geo 0x1A to 0x1C);
         * taken for an Options backdrop (0x09 to 0x0B) it was split and moved out, and the title's words with it
         * (FRONT). The Options backdrop reuses the logo's three defs, so the defs can't tell them apart; the mist,
         * which only the title has, can: a full strip while it shows (last frame's: the logo is added first) */
        if (tex >= 0x09 && tex <= 0x0B) return BACKDROP;  /* Options: also on the frame after the title's last */
        if ((tex >= 0x11 && tex <= 0x13) || (tex >= 0x1A && tex <= 0x1C) || title_screen()) return CENTRE;
        /* the other front end backdrops (Options, Continue): frame, picture and HP logo in one image */
        return BACKDROP;
    }
    /* on such a screen its words and button icons (the front end's code at 0xFF000 to 0x102000, the icons through
     * 0x4D42F) follow the widened frame */
    if (front_end_screen() && (in_chain(chain, depth, 0x0004D42Fu) || in_range(chain, depth, 0xFF000u, 0x102000u)))
        return FRONT;
    for (size_t e = 0; e < sizeof(k_elements) / sizeof(k_elements[0]); e++) {
        if (!in_chain(chain, depth, k_elements[e].ret)) continue;
        if (k_elements[e].also && !in_chain(chain, depth, k_elements[e].also)) continue;
        if (k_elements[e].side == BY_HEIGHT) {
            float top, bottom;
            sprite_span(sprite, 4, &top, &bottom);
            if (top < 240.0f) return LEFT;   /* Harry's, also over the inventory */
            /* an enemy's (a boss's), at its 16:9 side over the inventory too, as Harry's (2026-10-25: it moved to
             * the 4:3 frame's corner while the inventory was open) */
            return RIGHT;
        }
        if (k_elements[e].side != BY_HALF) {
            if (menu && k_elements[e].ret != 0x0001BE13u && k_elements[e].ret != 0x000AD634u &&
                k_elements[e].ret != 0x000AD6AAu)
                return CENTRE;
            return k_elements[e].side;
        }
        float cx = (MEMF(sprite) + MEMF(sprite + 2 * VERTEX_BYTES)) * 0.5f;
        float top = MEMF(sprite + 4) < MEMF(sprite + VERTEX_BYTES + 4) ? MEMF(sprite + 4) : MEMF(sprite + VERTEX_BYTES + 4);
        /* the button cluster, top right (its buttons 128.5 px square, their spell icons 64); the same code draws the
         * books' rows of stars, one strip 256.5 x 16.5 each, the first on the right page at y 124 or 144: a strip
         * stays on the page */
        float left, right;
        sprite_span(sprite, 0, &left, &right);
        int strip = right - left > 200.0f && sprite_height(sprite) < 40.0f;
        if (menu) return cx >= 320.0f && top < 180.0f && !strip ? RIGHT : CENTRE;
        /* Quidditch (level 3) draws its bottom row through here too: crest and score, the two bars with the Snitch,
         * the other score and crest. Each team out to its side, the bars in the middle, all at their own height */
        if (MEM32(LEVEL_INDEX) == LEVEL_QUIDDITCH && top >= 300.0f)
            return cx < 230.0f ? LEFT : cx > 380.0f ? QUIDDITCH_RIGHT : QUIDDITCH_MIDDLE;
        /* the broom race's countdown (2, 1, GO!: 128.5 px square at x 225.5, y 160.5) comes through here too: it
         * stays in the middle (it was taken for the counter's icon and moved to the bottom left) */
        if (cx > 200.0f && cx < 440.0f && top > 100.0f && top < 300.0f) return CENTRE;
        return cx < 320.0f ? COUNTER : RIGHT;  /* the left one is the counter's icon */
    }
    return CENTRE;
}

/* Widened by 4 / 3 about the centre: the squeeze brings it back to its 4:3 layout over the 16:9 width. The strips
 * overlap by half a pixel (0 to 256, then 255.5 on), which no 640x480 pixel centre falls in; widened, one can (a seam
 * blended twice), so a left edge is taken from the boundary half a pixel on and neighbours meet exactly. */
static void widen(uint32_t s)
{
    float left, right;
    sprite_span(s, 0, &left, &right);
    for (int v = 0; v < 4; v++) {
        float x = MEMF(s + v * VERTEX_BYTES);
        if (x == left) x += 0.5f;
        /* the screen's own sides go a little past it: the flush moves every sprite half a pixel on */
        if (x <= 0.0f) x -= 4.0f;
        if (x >= 640.0f) x += 4.0f;
        MEMF(s + v * VERTEX_BYTES) = (x - 320.0f) / 0.75f + 320.0f;
    }
}

/* A front end backdrop strip (Options, Continue: three full-height strips, frame, picture and HP logo in one image)
 * is cut above the top gold bar (y BACKDROP_SPLIT). Below, it is widened over 16:9 with the frame; above, black and
 * the HP logo keep their shape, moved out by the 16:9 margin so the logo sits at the right side; the left strip's band
 * reaches back to the left side on its own black, its texture wrapping. s: the strip AddSprite (sp: its call) wrote,
 * kept as the lower part; the upper part is the same call made again. */
#define BACKDROP_SPLIT 131.0f
static void split_backdrop(uint32_t sp, uint32_t s)
{
    float y0, y1, x0, x1;
    sprite_span(s, 4, &y0, &y1);
    sprite_span(s, 0, &x0, &x1);
    float vtop = 0, vbottom = 0, uleft = 0, uright = 0;
    for (int v = 0; v < 4; v++) {
        float x = MEMF(s + v * VERTEX_BYTES), y = MEMF(s + v * VERTEX_BYTES + 4);
        if (y == y0) vtop = MEMF(s + v * VERTEX_BYTES + 0x18);
        if (y == y1) vbottom = MEMF(s + v * VERTEX_BYTES + 0x18);
        if (x == x0) uleft = MEMF(s + v * VERTEX_BYTES + 0x14);
        if (x == x1) uright = MEMF(s + v * VERTEX_BYTES + 0x14);
    }
    if (y1 <= BACKDROP_SPLIT || y0 >= BACKDROP_SPLIT) {
        widen(s);
        return;
    }
    float vsplit = vtop + (BACKDROP_SPLIT - y0) / (y1 - y0) * (vbottom - vtop);

    uint32_t copy = MEM32(SPRITE_NEXT);
    g_esp = sp;  /* the same call again: its return address and arguments are still on the stack */
    sub_0001A360_gen();

    for (int v = 0; v < 4; v++) {  /* the lower part */
        if (MEMF(s + v * VERTEX_BYTES + 4) == y0) {
            MEMF(s + v * VERTEX_BYTES + 4) = BACKDROP_SPLIT;
            MEMF(s + v * VERTEX_BYTES + 0x18) = vsplit;
        }
    }
    widen(s);
    if (MEM32(SPRITE_NEXT) == copy) return;

    uint32_t c = SPRITE_POOL + copy * SPRITE_BYTES;
    const float shift = SIDE_SHIFT;
    for (int v = 0; v < 4; v++) {  /* the upper part */
        float x = MEMF(c + v * VERTEX_BYTES);
        if (MEMF(c + v * VERTEX_BYTES + 4) == y1) {
            MEMF(c + v * VERTEX_BYTES + 4) = BACKDROP_SPLIT;
            MEMF(c + v * VERTEX_BYTES + 0x18) = vsplit;
        }
        if (x == x0 && x0 <= 0.0f) {  /* reach back to the left side, the texture's own black wrapping round */
            float nx = -80.0f / 0.75f - 4.0f;
            MEMF(c + v * VERTEX_BYTES + 0x14) = uleft - (x0 + shift - nx) / (x1 - x0) * (uright - uleft);
            MEMF(c + v * VERTEX_BYTES) = nx;
        } else {
            MEMF(c + v * VERTEX_BYTES) = x + shift;
        }
    }
}

void sub_0001A360(void)
{
    uint32_t sp = g_esp, index = MEM32(SPRITE_NEXT);
    if (index == 0) g_frame++;
    uint32_t chain[8] = {0};
    int depth = 0;
    int trace = hpcos_HudTracing(), wide = hpcos_Widescreen();
    depth = call_chain(sp, chain, 8);
    sub_0001A360_gen();
    if (MEM32(SPRITE_NEXT) == index) return;  /* nothing added */
    if (in_chain(chain, depth, 0x0004DE22u)) {
        /* a developer leftover: 0x4DE0A prints "?" (0x18708C, beside "SLOW MOTION") at x 0, y 460, in a TV's overscan
         * but plain on a monitor; moved off the screen */
        uint32_t s = SPRITE_POOL + index * SPRITE_BYTES;
        for (int v = 0; v < 4; v++) {
            MEMF(s + v * VERTEX_BYTES) = -10000.0f;
            MEMF(s + v * VERTEX_BYTES + 4) = -10000.0f;
        }
        return;
    }
    if (wide) {
        /* squeezed to 3/4 about the centre, a 4:3 side at x = 0 lands 80 pixels in; moved out by 80 / 0.75 first,
         * it lands on the screen's side */
        uint32_t s = SPRITE_POOL + index * SPRITE_BYTES;
        int side = element_side(chain, depth, s, MEM32(sp + 4));
        if (side == WIDE) {
            widen(s);
        } else if (side == BACKDROP) {
            g_front_frame = g_frame;
            split_backdrop(sp, s);
        } else if (side == FRONT) {
            /* the left column (title, options, save slots, the A and B icons with Select and Exit) out with the
             * frame's left side, the values on the right a little to the right, inside the frame. A line of text
             * goes whole, by where it starts: a letter right after the last one on its line takes that one's move
             * (a save's "Whomping Willow" crosses the middle). Lines are kept by height: the shadow pass interleaves. */
            /* A line centred on the screen (a message box's text: New Game's "You will be unable to save your
             * progress...", 2026-10-25) stays where it is: once a line's ends are found symmetric about the middle
             * (it starts at x and reaches 640 - x, over 200 wide) its move is taken back from every letter so far
             * (the sprites are only drawn at the frame's end) and the rest join it unmoved */
            enum { kLineSprites = 128 };
            static struct { float y, x0, x1, dx; uint32_t frame; int n, centred; uint32_t idx[kLineSprites]; } lines[4];
            static int next;
            float lo, hi, top, bottom;
            sprite_span(s, 0, &lo, &hi);
            sprite_span(s, 4, &top, &bottom);
            float dx = (lo + hi) * 0.5f < 320.0f ? -90.0f : 40.0f;
            int i;
            for (i = 0; i < 4; i++) {
                if (lines[i].frame == g_frame && lines[i].y > top - 0.5f && lines[i].y < top + 0.5f &&
                    lo > lines[i].x1 - 3.0f && lo < lines[i].x1 + 24.0f) {
                    dx = lines[i].dx;
                    break;
                }
            }
            if (i == 4) {
                i = next++ & 3;
                lines[i].x0 = lo, lines[i].n = 0, lines[i].centred = 0;
            }
            lines[i].y = top;
            lines[i].x1 = hi;
            lines[i].dx = dx;
            lines[i].frame = g_frame;
            if (lines[i].n < kLineSprites) lines[i].idx[lines[i].n++] = index;
            if (!lines[i].centred && hi - lines[i].x0 > 200.0f && fabsf(lines[i].x0 + hi - 640.0f) < 24.0f) {
                lines[i].centred = 1;
                for (int k = 0; k < lines[i].n - 1; k++) {
                    uint32_t q = SPRITE_POOL + lines[i].idx[k] * SPRITE_BYTES;
                    for (int v = 0; v < 4; v++) MEMF(q + v * VERTEX_BYTES) -= lines[i].dx;
                }
                lines[i].dx = dx = 0.0f;
            }
            for (int v = 0; v < 4; v++) MEMF(s + v * VERTEX_BYTES) += dx;
        } else if (side != CENTRE) {
            /* a whole number of pixels: a HUD element made of pieces laid edge to edge on the console's pixel grid
             * (the enemy life bar: its cap ends at 329.5, the bar starts at 330) keeps them meeting; 106.67 left one
             * pixel to neither piece once their edges were snapped to the grid (d3d12.cpp, QuadHlsl) */
            float dx = side == RIGHT ? SIDE_SHIFT : side == QUIDDITCH_RIGHT ? SIDE_SHIFT + QUIDDITCH_MIRROR
                     : side == QUIDDITCH_MIDDLE ? QUIDDITCH_CENTRE : -SIDE_SHIFT;
            float dy = side == COUNTER ? COUNTER_DROP : 0.0f;
            for (int v = 0; v < 4; v++) {
                MEMF(s + v * VERTEX_BYTES) += dx;
                MEMF(s + v * VERTEX_BYTES + 4) += dy;
            }
        }
    }
    if (trace) {
        uint32_t s = SPRITE_POOL + index * SPRITE_BYTES;
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (int v = 0; v < 4; v++) {
            float x = MEMF(s + v * VERTEX_BYTES), y = MEMF(s + v * VERTEX_BYTES + 4);
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
        uint32_t def = MEM32(sp + 4);
        fprintf(stderr, "[SPRITE] %4u rect %6.1f %6.1f %6.1f %6.1f def %08X tex %08X chain", index, x0, y0, x1, y1, def,
                def ? MEM32(def + 8) : 0);
        for (int i = 0; i < depth; i++) fprintf(stderr, " %08X", chain[i]);
        fprintf(stderr, "\n");
    }
}

