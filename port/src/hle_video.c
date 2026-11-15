/**
 * The NV2A video overlay, for XMV movies.
 *
 * The XDK's movie player decodes each frame into a YUY2 surface and the game
 * hands it to D3DDevice_UpdateOverlay, which points the GPU's overlay engine
 * (PVIDEO) at it; the CRTC mixes it over the framebuffer at scan-out. Nothing
 * here scans PVIDEO out, so the movies played black. UpdateOverlay is
 * implemented on the host instead: the frame is converted to XRGB and handed
 * to the framebuffer window, which draws it over the framebuffer.
 *
 * Also wraps XMVDecoder_GetNextFrame and D3DDevice_GetDisplayFieldStatus with
 * a trace (RECOMP_XMV_TRACE=1) for the intro that stops decoding part way.
 *
 * Every entry point keeps the generated body as sub_X_gen; RECOMP_HLE_OVERLAY=0
 * runs the XDK's own UpdateOverlay/EnableOverlay again.
 *
 * Built into the recompile with a second --exclude-manual naming this file.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* the register names (eax, esp, ...) the runtime macros expand to */
#define RECOMP_GENERATED_CODE
#include "recomp/gen/recomp_types.h"

void xbox_FramebufferWindowOverlay(const uint32_t *xrgb, uint32_t w, uint32_t h,
                                   int dst_x, int dst_y, int dst_w, int dst_h);
void xbox_FramebufferWindowOverlayOff(void);

#define ARG(i)  MEM32(g_esp + 4 + 4 * (i))

extern uint32_t xbox_Nv2aVblankCount(void);
extern void hle_d3d8_overlay_present(void);  /* renderer/xdk/hle_d3d8.cpp */

#define XBOX_CONTIG_BASE  0x80000000u
#define XBOX_RAM_SIZE     0x04000000u

/* Xbox D3DFORMAT values, from bits 8-15 of a resource's Format field */
#define XFMT_YUY2  0x24
#define XFMT_UYVY  0x25

static int g_hle = -1, g_trace;
static int g_movies, g_movie_frames;  /* movies started (EnableOverlay(1)), new frames of the one playing */

static int hle_on(void)
{
    if (g_hle < 0) {
        const char *e = getenv("RECOMP_HLE_OVERLAY");
        const char *t = getenv("RECOMP_XMV_TRACE");
        g_hle = !(e && *e == '0');
        g_trace = t && *t && *t != '0';
        fprintf(stderr, "[OVERLAY] %s\n", g_hle ? "host overlay (HLE)" : "XDK overlay (PVIDEO, not scanned out)");
    }
    return g_hle;
}

static void ret_stdcall(uint32_t result, int nargs)
{
    g_eax = result;
    g_esp += 4 + 4 * nargs;
}

/* A resource's Data is the physical address of its memory. */
static uint32_t data_va(uint32_t data)
{
    return data < XBOX_RAM_SIZE ? XBOX_CONTIG_BASE + data : data;
}

static uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* BT.601, studio range: what WMV-in-XMV decodes to. */
static uint32_t yuv_to_xrgb(int y, int u, int v)
{
    int c = 298 * (y - 16), d = u - 128, e = v - 128;
    return (uint32_t)clamp8((c + 409 * e + 128) >> 8) << 16 |
           (uint32_t)clamp8((c - 100 * d - 208 * e + 128) >> 8) << 8 |
           (uint32_t)clamp8((c + 516 * d + 128) >> 8);
}

static uint32_t *g_frame;
static uint32_t g_frame_cap;

/* D3DDevice_UpdateOverlay(pSurface, SrcRect, DstRect, EnableColorKey, ColorKey)
 *
 * D3DSurface: Common, Data, Lock, Format, Size, Parent. Size packs width-1
 * (bits 0-11), height-1 (12-23) and pitch/64-1 (24-31). */
extern void sub_00136520_gen(void);
void sub_00136520(void)
{
    uint32_t surf, src_rect, dst_rect, fmt, size, w, h, pitch, base;
    int sx = 0, sy = 0, sw, sh, dx = 0, dy = 0, dw = 640, dh = 480;
    static int logged;

    if (!hle_on()) { sub_00136520_gen(); return; }
    surf = ARG(0);
    src_rect = ARG(1);
    dst_rect = ARG(2);
    if (!surf) {
        xbox_FramebufferWindowOverlayOff();
        ret_stdcall(0, 5);
        return;
    }
    fmt = (MEM32(surf + 0xC) >> 8) & 0xFF;
    size = MEM32(surf + 0x10);
    w = (size & 0xFFF) + 1;
    h = ((size >> 12) & 0xFFF) + 1;
    pitch = (((size >> 24) & 0xFF) + 1) * 64;
    base = data_va(MEM32(surf + 4));
    sw = (int)w;
    sh = (int)h;
    if (src_rect) {
        sx = (int)MEM32(src_rect);
        sy = (int)MEM32(src_rect + 4);
        sw = (int)MEM32(src_rect + 8) - sx;
        sh = (int)MEM32(src_rect + 12) - sy;
    }
    if (dst_rect) {
        dx = (int)MEM32(dst_rect);
        dy = (int)MEM32(dst_rect + 4);
        dw = (int)MEM32(dst_rect + 8) - dx;
        dh = (int)MEM32(dst_rect + 12) - dy;
    }
    if (logged++ < 3)
        fprintf(stderr, "[OVERLAY] surface %08X fmt=%02X %ux%u pitch=%u data=%08X src=%d,%d %dx%d dst=%d,%d %dx%d\n",
                surf, fmt, w, h, pitch, base, sx, sy, sw, sh, dx, dy, dw, dh);
    if (g_trace) {
        /* which address holds the picture: mean luma over every 16th pixel */
        static int probes;
        if (probes++ % 30 == 0) {
            uint32_t cand[2] = {MEM32(surf + 4), base}, sum[2] = {0, 0};
            for (int k = 0; k < 2; k++)
                for (uint32_t i = 0; i < h * pitch; i += 32) sum[k] += MEM8(cand[k] + i);
            fprintf(stderr, "[OVERLAY] luma @%08X=%u @%08X=%u\n", cand[0], sum[0] / (h * pitch / 32),
                    cand[1], sum[1] / (h * pitch / 32));
        }
    }
    if (g_trace) {
        /* is the picture still being written while it is shown? the surface hashed now and 5 ms on */
        uint32_t h1 = 2166136261u, h2 = 2166136261u;
        for (uint32_t i = 0; i < h * pitch; i += 4) h1 = (h1 ^ MEM32(base + i)) * 16777619u;
        Sleep(5);
        for (uint32_t i = 0; i < h * pitch; i += 4) h2 = (h2 ^ MEM32(base + i)) * 16777619u;
        if (h1 != h2) fprintf(stderr, "[OVERLAY] surface %08X changed while shown\n", base);
    }
    if ((fmt != XFMT_YUY2 && fmt != XFMT_UYVY) || sw <= 0 || sh <= 0 ||
        sx < 0 || sy < 0 || (uint32_t)(sx + sw) > w || (uint32_t)(sy + sh) > h) {
        static int warned;
        if (!warned++) fprintf(stderr, "[OVERLAY] unsupported surface (fmt %02X), not shown\n", fmt);
        ret_stdcall(0, 5);
        return;
    }
    if ((uint32_t)(sw * sh) > g_frame_cap) {
        free(g_frame);
        g_frame = malloc((size_t)sw * sh * 4);
        g_frame_cap = g_frame ? (uint32_t)(sw * sh) : 0;
        if (!g_frame) { ret_stdcall(0, 5); return; }
    }
    for (int y = 0; y < sh; y++) {
        const uint8_t *row = (const uint8_t *)XBOX_PTR(base + (uint32_t)(sy + y) * pitch);
        uint32_t *out = g_frame + (size_t)y * sw;
        for (int x = 0; x < sw; x++) {
            int px = sx + x;
            const uint8_t *m = row + (px & ~1) * 2;  /* one macropixel: two pixels */
            int yy, u, v;
            if (fmt == XFMT_YUY2) { yy = m[(px & 1) * 2]; u = m[1]; v = m[3]; }
            else                  { yy = m[1 + (px & 1) * 2]; u = m[0]; v = m[2]; }
            out[x] = yuv_to_xrgb(yy, u, v);
        }
    }
    {
        /* RECOMP_OVERLAY_DUMP=<dir>: every overlay frame as dir/overlay_NNNN.ppm, to set against a reference
         * decode of the same XMV (ffmpeg reads them) */
        static const char *dump = (const char *)1;
        static unsigned n;
        if (dump == (const char *)1) dump = getenv("RECOMP_OVERLAY_DUMP");
        if (dump && *dump) {
            char path[512];
            FILE *f;
            snprintf(path, sizeof(path), "%s/overlay_%04u.ppm", dump, n++);
            if ((f = fopen(path, "wb"))) {
                fprintf(f, "P6\n%d %d\n255\n", sw, sh);
                for (int i = 0; i < sw * sh; i++) {
                    uint8_t rgb[3] = {(uint8_t)(g_frame[i] >> 16), (uint8_t)(g_frame[i] >> 8), (uint8_t)g_frame[i]};
                    fwrite(rgb, 1, 3, f);
                }
                fclose(f);
            }
        }
    }
    xbox_FramebufferWindowOverlay(g_frame, (uint32_t)sw, (uint32_t)sh, dx, dy, dw, dh);
    hle_d3d8_overlay_present();
    ret_stdcall(0, 5);
}

/* D3DDevice_EnableOverlay(Enable). The XDK version spins on PVIDEO when it
 * stops the overlay; here it only shows or hides the host overlay. */
extern void sub_001366A0_gen(void);
void sub_001366A0(void)
{
    if (!hle_on()) { sub_001366A0_gen(); return; }
    if (g_trace) fprintf(stderr, "[OVERLAY] EnableOverlay(%u)\n", ARG(0));
    if (ARG(0)) { g_movies++; g_movie_frames = 0; }
    if (!ARG(0)) xbox_FramebufferWindowOverlayOff();
    ret_stdcall(0, 1);
}

/* ── Trace only ─────────────────────────────────────────────────────────── */

/* XMVDecoder_GetNextFrame(pDecoder, pSurface, pResult, prtTimestamp):
 * XMV_NOFRAME 0, XMV_NEWFRAME 1, XMV_ENDOFFILE 2, XMV_FAIL 3. Logs each
 * change of result and, while it stays put, a count once a second. */
/* RECOMP_XMV_CHECK=movie,frames.txt,file.xmv (debug): before each new frame of the movie'th movie is decoded, find
 * its compressed bytes in guest memory and compare them with the file. frames.txt is ffprobe's "size,pos" per video
 * packet (-select_streams v -show_entries packet=size,pos -of csv=p=0). A difference means the bitstream was
 * overwritten before the decoder read it (a read buffer handed back too early), not a decoder bug. */
static void xmv_check_frame(int k)
{
    static int movie = -1, nframes, done;
    static uint32_t *fsize, *fpos;
    static uint8_t *file;
    if (movie < 0) {
        const char *e = getenv("RECOMP_XMV_CHECK");
        char list[512], path[512];
        FILE *f;
        movie = 0;
        if (!e || sscanf(e, "%d,%511[^,],%511s", &movie, list, path) != 3) { movie = 0; return; }
        if ((f = fopen(path, "rb"))) {
            long n;
            fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
            file = malloc((size_t)n);
            if (file && fread(file, 1, (size_t)n, f) != (size_t)n) { free(file); file = NULL; }
            fclose(f);
        }
        fsize = calloc(4096, 4); fpos = calloc(4096, 4);
        if ((f = fopen(list, "r"))) {
            unsigned s, p;
            while (nframes < 4096 && fscanf(f, "%u,%u", &s, &p) == 2) { fsize[nframes] = s; fpos[nframes++] = p; }
            fclose(f);
        }
        fprintf(stderr, "[XMVCHK] movie %d: %d frames listed, file %s\n", movie, nframes, file ? "read" : "MISSING");
        if (!file || !nframes) movie = 0;
    }
    if (!movie || g_movies != movie || k < 1 || k > nframes || k <= done) return;
    done = k;
    {
        const uint8_t *want = file + fpos[k - 1];
        uint32_t size = fsize[k - 1], need = size < 32 ? size : 32, at = 0, best = 0, best_diff = 0xFFFFFFFFu;
        const uint8_t *ram = (const uint8_t *)XBOX_PTR(XBOX_CONTIG_BASE);
        /* every place the frame's first bytes appear; the one closest to the file wins */
        for (uint32_t a = 0; a + size <= XBOX_RAM_SIZE; a++) {
            if (ram[a] != want[0] || memcmp(ram + a, want, need)) continue;
            uint32_t diff = 0;
            for (uint32_t i = 0; i < size; i++) diff += ram[a + i] != want[i];
            if (diff < best_diff) { best_diff = diff; best = a; at = 1; }
            if (!diff) break;
        }
        if (!at)
            fprintf(stderr, "[XMVCHK] frame %d (%u bytes at file %u): its start is nowhere in memory\n", k, size,
                    fpos[k - 1]);
        else if (best_diff) {
            uint32_t first = 0;
            while (ram[best + first] == want[first]) first++;
            fprintf(stderr, "[XMVCHK] frame %d (%u bytes at file %u) at %08X: %u bytes differ, the first at +%u\n", k,
                    size, fpos[k - 1], XBOX_CONTIG_BASE + best, best_diff, first);
        } else
            fprintf(stderr, "[XMVCHK] frame %d at %08X intact\n", k, XBOX_CONTIG_BASE + best);
    }
}

extern void sub_0014ED3D_gen(void);
void sub_0014ED3D(void)
{
    static uint32_t last = 0xFFFFFFFF, calls, frames;
    static DWORD t0;
    uint32_t presult = ARG(2);
#ifdef RECOMP_DIFFTEST
    /* RECOMP_DIFF_XMV=movie,first,last: the differential test (difftest.c) runs only while the movie'th movie (1 the
     * first played) decodes its frames first..last (1 based), the frames a bad decode was seen on */
    {
        extern void recomp_difftest_sweep(int on);
        static int movie = -1, first, last;
        if (movie < 0) {
            const char *e = getenv("RECOMP_DIFF_XMV");
            if (!e || sscanf(e, "%d,%d,%d", &movie, &first, &last) != 3) movie = 0;
        }
        if (movie > 0 && g_movies == movie && presult) {
            if (g_movie_frames + 1 == first) recomp_difftest_sweep(1);
            if (g_movie_frames == last) recomp_difftest_sweep(0);
        }
    }
#endif
    xmv_check_frame(g_movie_frames + 1);
    sub_0014ED3D_gen();
    hle_on();
    if (presult && MEM32(presult) == 1) g_movie_frames++;
    if (!g_trace) return;
    {
        uint32_t r = presult ? MEM32(presult) : 0xFFFFFFFF;
        DWORD now = GetTickCount();
        calls++;
        if (r == 1) frames++;
        if (r != last && r != 1 && r != 0) fprintf(stderr, "[XMV] GetNextFrame -> %u\n", r);
        last = r;
        if (now - t0 >= 1000) {
            fprintf(stderr, "[XMV] %u calls, %u new frames in the last %lu ms (last result %u)\n",
                    calls, frames, (unsigned long)(now - t0), r);
            calls = frames = 0;
            t0 = now;
        }
    }
}

/* The movie player's loop (sub_00018D40) goes round again at once on XMV_NOFRAME,
 * millions of calls a second. GetNextFrame polls the field status first thing (its
 * call returns to 0x14F063), so a poll that sees the field it saw last time cannot
 * have a new frame due: wait for the next field there. The kernel timer thread that
 * counts the fields shares the guest core, so spinning can starve the movie's own
 * clock, as it did on PS on Windows. It must stay parked for the whole field: a single
 * Sleep(1) per poll made PS's movies slow again. RECOMP_XMV_WAIT=0: spin as before. */
#define XMV_FIELD_POLL_RET 0x0014F063u

static int xmv_wait_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_XMV_WAIT");
        on = !(e && *e == '0');
    }
    return on;
}

static uint32_t xmv_field_wait(void)
{
    static uint32_t last = 0xFFFFFFFFu;
    uint32_t v = xbox_Nv2aVblankCount();
    int ms;
    /* bounded: with no vblank clock (RECOMP_VBLANK unset) the count never moves */
    for (ms = 0; v == last && ms < 20; ms++) {
        Sleep(1);
        v = xbox_Nv2aVblankCount();
    }
    last = v;
    return v;
}

/* D3DDevice_GetDisplayFieldStatus(pFieldStatus): { Field, VBlankCount } */
extern void sub_00133790_gen(void);
void sub_00133790(void)
{
    static DWORD t0;
    static int d3d_hle = -1;
    uint32_t p = ARG(0);
    if (d3d_hle < 0) {
        const char *e = getenv("RECOMP_HLE_D3D");
        d3d_hle = !(e && *e == '0');
    }
    if (d3d_hle && p) {
        /* The D3D8 HLE (renderer/xdk/hle_d3d8.cpp) replaces CreateDevice, so the
         * XDK's vblank interrupt never runs and the device's counter at
         * +0x1980 stays 0: the XMV player waits for the next field forever.
         * Report the runtime's 60 Hz count, progressive (as the XDK does when
         * the mode is not interlaced). */
        MEM32(p + 4) = MEM32(g_esp) == XMV_FIELD_POLL_RET && xmv_wait_on()
                           ? xmv_field_wait() : xbox_Nv2aVblankCount();
        MEM32(p) = 3;
        g_esp += 8;
    } else {
        sub_00133790_gen();
    }
    hle_on();
    if (g_trace && p) {
        DWORD now = GetTickCount();
        if (now - t0 >= 1000) {
            fprintf(stderr, "[XMV] field status: field=%u vblank=%u\n", MEM32(p), MEM32(p + 4));
            t0 = now;
        }
    }
}

/* ── Widescreen ─────────────────────────────────────────────────────────── */

/* The title's "is the display widescreen?" test, compiled down to
 * `xor al, al; ret` in the retail build. Two of its callers are the camera
 * projections, which pick a 16:9 aspect for D3DXMatrixPerspectiveFovLH when
 * it says yes: a wider horizontal field of view (Hor+) that the title's own
 * culling follows. With RECOMP_ASPECT=16:9 it says yes; the runtime then
 * squeezes the 2D over the scene and stretches the frame to 16:9. */
extern void sub_000AE1F0_gen(void);
void sub_000AE1F0(void)
{
    static int ws = -1;
    if (ws < 0) {
        const char *a = getenv("RECOMP_ASPECT");
        ws = a && !strcmp(a, "16:9");
        fprintf(stderr, "[WIDESCREEN] %s\n", ws ? "16:9 camera" : "4:3 camera");
    }
    /* a screen the title lays out 4:3 (the inventory's camera projects 4 / 3 into 0x50C2B8 / 0x50C2BC) is shown 4:3
     * in the middle (hle_d3d8.cpp's Swap): its projections stay 4:3 too, or the book's page text, placed through
     * them, runs into the other page */
    /* Only the two camera projections (returning to 0x155B8, 0x15B56) hear yes. The same stub is also called by
     * game-state code: sub_000AC240 makes bit 0 of the flag word 0x53E79C from it, sub_000CFA60 and sub_000D8800
     * call it with arguments; on the console it is 0 for them all, and a yes there changed the game (the Whomping
     * Willow aimed away from Harry in 16:9). */
    {
        uint32_t ret = MEM32(g_esp);
        float num = MEMF(0x0050C2B8), den = MEMF(0x0050C2BC), r = den != 0.0f ? num / den : 0.0f;
        int four_three = r > 4.0f / 3.0f - 0.01f && r < 4.0f / 3.0f + 0.01f;
        int camera = ret == 0x000155B8u || ret == 0x00015B56u;
        g_eax = (g_eax & ~0xFFu) | (uint32_t)(ws && camera && !four_three);
    }
    g_esp += 4;
}

/* A state change (sub_00034D00 -> sub_0004ABF0 -> sub_00028380: the inventory, a wizard card's screen) resets the
 * camera to 4 / 3 in the frame's aspect (0x50C2B8 / 0x50C2BC) and, from it, the camera frustum's side angles; back in
 * the game nothing sets the aspect again (the Xbox's own is 4 / 3 too, it never mattered). In 16:9 the 4 / 3 stayed:
 * the HUD laid out as on a 4:3 screen (a boss's health bar back in the middle, 2026-10-25) and objects at the screen's
 * edges were culled. Harry's update (hle_trainer.c) stops while such a screen is up; the first update after such a
 * pause (150 ms or more without one: the screen was closed) with the aspect still 4 / 3, or half a second of updates
 * with it (a 4:3 screen that didn't pause him), puts back the boot's aspect (0x1E2FC0, 16 / 9) and makes the
 * frustum's sides again as sub_000348C0 makes them, from its top angle: left / right = -/+ atan(tan(top) * aspect) */
extern int hpcos_Widescreen(void); /* renderer/xdk/hle_d3d8.cpp: booted in 16:9 */

/* Harry's update ran in the last 150 ms: the game itself is up, not the inventory or a wizard card's screen (his
 * update stops while one is up); the field of view slider holds only then (hle_shots.c) */
static volatile DWORD g_harry_ms;
int hpcos_InGameView(void)
{
    if (GetTickCount() - g_harry_ms > 150) return 0;
    if (!hpcos_Widescreen()) return 1;
    float num = MEMF(0x0050C2B8), den = MEMF(0x0050C2BC), r = den != 0.0f ? num / den : 0.0f;
    return !(r > 4.0f / 3.0f - 0.01f && r < 4.0f / 3.0f + 0.01f);  /* a 4:3 screen in 16:9: the inventory */
}

void hpcos_WideAspectTick(void)
{
    static DWORD since, last;
    g_harry_ms = GetTickCount();
    if (!hpcos_Widescreen()) return;
    float num = MEMF(0x0050C2B8), den = MEMF(0x0050C2BC), r = den != 0.0f ? num / den : 0.0f;
    DWORD now = GetTickCount();
    int four_three = r > 4.0f / 3.0f - 0.01f && r < 4.0f / 3.0f + 0.01f;
    int resumed = last && now - last >= 150;
    if (!four_three || now - last > 100) since = now;
    last = now;
    if (!four_three || (!resumed && now - since < 500)) return;
    float wide = MEMF(0x001E2FC0), top = MEMF(0x0050C1D4 + 8);
    MEMF(0x0050C2B8) = wide;
    MEMF(0x0050C2BC) = 1.0f;
    float side = atanf(tanf(fabsf(top)) * wide);
    MEMF(0x0050C1D4) = -side;
    MEMF(0x0050C1D4 + 4) = side;
    fprintf(stderr, "[WIDESCREEN] back in the game with the 4:3 screen's aspect left: %.4f put back\n", wide);
}
