/**
 * Debug overrides for the missing-geometry hunt (floor holes in Borgin & Burkes).
 *
 * sub_00034950 is the game's view frustum test (this = frustum, args: bounds, matrix, radius scale; 1 visible,
 * 0 culled), reached from the per-object cull sub_00039880 that decides whether the scene draws an object. F6 makes
 * every object pass it; a floor that comes back then was culled, not lost on the way to the GPU.
 *
 * Built into the recompile with --exclude-manual ../hpcos/src/hle_debug.c.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RECOMP_GENERATED_CODE
#include "recomp/gen/recomp_types.h"

extern void sub_00034950_gen(void);
extern int app_DrawAll(void); /* hpcos.toml draw_all_objects (renderer/app/app.cpp) */

static int g_nocull;
static unsigned g_tests, g_culled;
static DWORD g_t0;

/* per frustum (this): the camera's own is 0x50C1D4; the angles are [left, right, top, bottom] in radians at +0..+0xC,
 * near/far at +0x10/+0x14, set by sub_000348C0 from the vertical fov and the aspect at +0xE4/+0xE8 */
static struct { uint32_t fr; unsigned tests, culled; } g_frs[8];

/* draw_all_objects: the room walk (sub_00048A10) tests a room's objects against a copy of the camera's frustum
 * narrowed to the doorway or arch it is seen through (angles from the room's portal, list+0x1C +0x44), which left
 * out parts of the library still on screen; with the setting an object that test leaves out gets a second test
 * against the camera's own frustum, so all that is in view is drawn and what is behind the camera is still left out
 * (every test passing cost Quidditch 12 fps) */
#define CAMERA_FRUSTUM 0x50C1D4u

extern int hpcos_Widescreen(void); /* renderer/xdk/hle_d3d8.cpp: booted in 16:9 */
extern uint32_t hpcos_FovPercent(void); /* renderer/app/app.cpp: the field of view slider, 80 to 130 */
extern int hpcos_InGameView(void);       /* hle_video.c: the game itself is up (not the inventory) */

/* the second test's frustum is the camera's widened to the picture shown plus a margin (RECOMP_CULL_MARGIN, degrees,
 * default 8): objects at the screen's edges vanished as the camera turned (2026-10-25, the Burrow's barn), and after a
 * wizard card the camera's frustum stays at the card screen's 4:3 (sub_00028380 writes 4 / 3 into its aspect, +0xE4)
 * while the gameplay picture is still 16:9. The test reads only the angles at +0..+0xC, near/far and the matrix */
static float cull_margin(void)
{
    static float m = -1.0f;
    if (m < 0.0f) {
        const char *e = getenv("RECOMP_CULL_MARGIN");
        m = e ? (float)atof(e) : 8.0f;
        if (m < 0.0f) m = 0.0f;
        if (m > 30.0f) m = 30.0f;
        fprintf(stderr, "[CULL] objects left out get a second test against the camera's view widened by %.1f degrees\n", m);
        m *= 0.017453292f;
    }
    return m;
}

static unsigned g_widened;

/* draw_all_objects: every room of the loaded levels drawn, not only those this frame's doorway walk reached: the
 * visibility pass (sub_00049EB0) marks them all seen with the camera's whole view, as the broom's room list does on
 * the grounds, and the room draw (sub_00048F60) no longer skips an unmarked one (tools/patch_gen.py); their objects
 * are still culled one by one below */
int hpcos_AllRooms(void) { return app_DrawAll(); }

/* the grounds' path warps (tools/patch_gen.py's fix in sub_000E6860, the grounds' script): standing in a warp zone
 * (type 0x52000166) or coming near a warp object (sub_000E6760) fades out and puts Harry at its destination, and the
 * area switch there (sub_000E4470, sub_000E5E90) hides every piece of the grounds not on the new area's list. Off,
 * the warps are passed over and every area switch takes the broom's area (0xD, sub_00052D60): the whole grounds drawn
 * and solid but room 0xFA, as when flying */
int app_GroundsWarps(void);
int hpcos_GroundsWarps(void) { return app_GroundsWarps(); }

/* F10's broom option: any flat surface is a landing zone (tools/patch_gen.py's fix in sub_00050300, hle_trainer.c) */
int app_BroomLandFlat(void);
int hpcos_BroomLandFlatOn(void) { return app_BroomLandFlat(); }

static void test_widened(void)
{
    uint32_t f = CAMERA_FRUSTUM;
    float s[4] = {MEMF(f), MEMF(f + 4), MEMF(f + 8), MEMF(f + 0xC)}, m = cull_margin();
    float v = fmaxf(fabsf(s[2]), fabsf(s[3]));
    /* the picture as the field of view slider widens it (hle_shots.c), so objects at its sides are drawn */
    uint32_t pct = hpcos_InGameView() ? hpcos_FovPercent() : 100u;
    if (pct > 100) v = atanf(tanf(v) * (float)pct / 100.0f);
    float h = fmaxf(fmaxf(fabsf(s[0]), fabsf(s[1])), atanf(tanf(v) * (hpcos_Widescreen() ? 16.0f / 9.0f : 4.0f / 3.0f)));
    h = fminf(h + m, 1.4f), v = fminf(v + m, 1.4f);
    MEMF(f) = -h, MEMF(f + 4) = h, MEMF(f + 8) = v, MEMF(f + 0xC) = -v;
    g_ecx = f;
    sub_00034950_gen();
    MEMF(f) = s[0], MEMF(f + 4) = s[1], MEMF(f + 8) = s[2], MEMF(f + 0xC) = s[3];
    if (g_eax) g_widened++;
}

/* only the drawing's callers get the second test: the per-object cull (sub_00039880, the room walk's objects) and
 * sub_0003B520 (tests against the camera's frustum, then draws through sub_000395C0). The game's own "is it on
 * screen?" (sub_000F55D0, called from spell and object code at 0xE132D, 0xF6FC0, 0xF94F1, 0xFA096...) keeps the
 * Xbox's answer: with the margin it said yes for things just off screen (2026-10-25, Alohomora's aim moved) */
static int drawing_caller(uint32_t ret)
{
    return ret == 0x000398ECu || ret == 0x0003B57Bu;
}

void sub_00034950(void)
{
    uint32_t fr = g_ecx, ret = MEM32(g_esp);
    /* the game's own test first, so nothing it draws is ever left out (an object's animation sounds play from its
     * drawing: a door's swing went quiet as the camera passed it); only an object it leaves out is tested again
     * against the camera's widened frustum. The test writes into its argument slots, so they are put back for the
     * second */
    uint32_t s_esp = g_esp, a0 = MEM32(g_esp + 4), a1 = MEM32(g_esp + 8), a2 = MEM32(g_esp + 12);
    uint32_t s_esi = g_esi, s_edi = g_edi;
    sub_00034950_gen();
    if (!g_eax && g_nocull != app_DrawAll() && drawing_caller(ret)) {
        g_esp = s_esp, g_esi = s_esi, g_edi = s_edi;
        MEM32(g_esp + 4) = a0, MEM32(g_esp + 8) = a1, MEM32(g_esp + 12) = a2;
        test_widened();
    }
    g_tests++;
    if (!g_eax) g_culled++;
    for (int i = 0; i < 8; i++)
        if (g_frs[i].fr == fr || !g_frs[i].fr) {
            g_frs[i].fr = fr;
            g_frs[i].tests++;
            if (!g_eax) g_frs[i].culled++;
            break;
        }
    if (GetAsyncKeyState(VK_F6) & 1) {
        g_nocull = !g_nocull;
        fprintf(stderr, "[CULL] objects tested against %s\n", g_nocull != app_DrawAll() ? "the camera's view" : "the room's portal view (Xbox)");
    }
    DWORD now = GetTickCount();
    if (now - g_t0 >= 5000) {
        fprintf(stderr, "[CULL] %u frustum tests, %u culled, %u kept by the widened view in %lu ms\n", g_tests, g_culled,
                g_widened, (unsigned long)(now - g_t0));
        for (int i = 0; i < 8 && g_frs[i].fr; i++) {
            uint32_t f = g_frs[i].fr;
            const double d = 57.29577951308232;
            fprintf(stderr, "[CULL]   frustum %08X: %u tests %u culled, angles L %.2f R %.2f T %.2f B %.2f, near %g far %g, aspect %g/%g\n",
                    f, g_frs[i].tests, g_frs[i].culled, MEMF(f) * d, MEMF(f + 4) * d, MEMF(f + 8) * d, MEMF(f + 0xC) * d,
                    MEMF(f + 0x10), MEMF(f + 0x14), MEMF(f + 0xE4), MEMF(f + 0xE8));
            g_frs[i].fr = 0, g_frs[i].tests = g_frs[i].culled = 0;
        }
        g_tests = g_culled = g_widened = 0;
        g_t0 = now;
    }
}



/* RECOMP_SND_TRACE=1: every sound the game asks for, as it fills in a voice for it ([SND]): the id (0x02xxxxxx speech,
 * 0x03xxxxxx effects; a wizard card's name is field 5 of its 20 byte entry at 0x1F4218), the voice and the code that
 * asked. The sound manager looks the id up in its banks later; RECOMP_DSOUND_TRACE's buffer lines after it in the same
 * log show whether anything then played. sub_00040A00 is __thiscall (ecx = voice; id, ... on the stack). The generated
 * body is renamed sub_00040A00_gen by hand in gen/recomp_0000.c until a regen with --exclude-manual for this file. */
/* a speech voice's 0x70 bytes again 60 and 400 ms after its request ([SND+]), from a thread of its own: what the sound
 * manager made of it, found in a loaded bank or not */
static struct { uint32_t voice, id; DWORD t; } g_snd_watch[16];
static volatile LONG g_snd_watch_n;

static void snd_dump(const char *when, uint32_t voice, uint32_t id)
{
    char line[1024];
    int n = snprintf(line, sizeof line, "[SND+] %s id %08X voice %08X:", when, id, voice);
    for (uint32_t o = 0; o < 0x70 && n < (int)sizeof line - 12; o += 4) n += snprintf(line + n, sizeof line - n, " %08X", MEM32(voice + o));
    fprintf(stderr, "%s\n", line);
}

static DWORD WINAPI snd_watch_thread(LPVOID unused)
{
    (void)unused;
    uint8_t done[16] = {0};
    for (;;) {
        Sleep(20);
        LONG n = g_snd_watch_n;
        for (LONG i = 0; i < n && i < 16; i++) {
            DWORD age = GetTickCount() - g_snd_watch[i].t;
            if (!(done[i] & 1) && age >= 60) snd_dump("60ms", g_snd_watch[i].voice, g_snd_watch[i].id), done[i] |= 1;
            if (!(done[i] & 2) && age >= 400) snd_dump("400ms", g_snd_watch[i].voice, g_snd_watch[i].id), done[i] |= 2;
        }
    }
    return 0;
}

extern void sub_00040A00_gen(void);
void sub_00040A00(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_SND_TRACE");
        on = e && *e && *e != '0';
    }
    if (on) {
        uint32_t id = MEM32(g_esp + 4);
        /* the caller's caller too: sub_00041A60 / sub_000419B0 sit between the game code and here */
        char line[512];
        int n = snprintf(line, sizeof line, "[SND] %lu id %08X voice %08X from %08X %08X %08X | calls", (unsigned long)GetTickCount(),
                         id, g_ecx, MEM32(g_esp), MEM32(g_esp + 0x24), MEM32(g_esp + 0x3C));
        /* the game's code addresses on the stack (return addresses, a call before each): who asked for it */
        for (uint32_t a = g_esp; a < g_esp + 0x180 && n < (int)sizeof line - 12; a += 4) {
            uint32_t v = MEM32(a);
            if (v >= 0x11005 && v < 0x130620 && (MEM8(v - 5) == 0xE8 || MEM8(v - 2) == 0xFF || MEM8(v - 3) == 0xFF || MEM8(v - 6) == 0xFF))
                n += snprintf(line + n, sizeof line - n, " %08X", v);
        }
        fprintf(stderr, "%s\n", line);
        uint32_t voice = g_ecx;
        sub_00040A00_gen();
        if ((id & 0xFF000000u) == 0x02000000u && g_snd_watch_n < 16) {
            static int started;
            if (!started) started = 1, CloseHandle(CreateThread(NULL, 0, snd_watch_thread, NULL, 0, NULL));
            snd_dump("0ms", voice, id);
            LONG i = g_snd_watch_n;
            g_snd_watch[i].voice = voice, g_snd_watch[i].id = id, g_snd_watch[i].t = GetTickCount();
            InterlockedIncrement(&g_snd_watch_n);
        }
        return;
    }
    sub_00040A00_gen();
}

/* House points ([PTS], always on: these run a few times per level). Points are words at 0x527EE4 (Gryffindor,
 * Slytherin, Ravenclaw, Hufflepuff). sub_000818C0(pts, house) adds, sub_000818E0(pts, house) takes away (floor 0).
 * sub_000C7850(any_amount, budget) rolls what getting caught costs ({0,5,10,20} or {5,10,20}, at most budget) into
 * 0x545358; byte 0x527EFA set = Harry is disguised as Goyle, the cost then comes off Slytherin from a budget of 30
 * (0x5283D8). sub_000E6A40(i) moves Slytherin/Ravenclaw/Hufflepuff after the house above them, capped 300/250/200.
 * The generated bodies are renamed *_gen by hand until a regen with --exclude-manual for this file. */
static const char *const g_house[4] = { "Gryffindor", "Slytherin", "Ravenclaw", "Hufflepuff" };

static void pts_line(const char *what, uint32_t ret)
{
    fprintf(stderr, "[PTS] %lu %s from %08X | G %u S %u R %u H %u | day %u level %u goyle %u slyth-budget %d\n",
            (unsigned long)GetTickCount(), what, ret, MEM16(0x527EE4), MEM16(0x527EE6), MEM16(0x527EE8), MEM16(0x527EEA),
            MEM32(0x527B7C), MEM32(0x527B78), MEM8(0x527EFA), (int32_t)MEM32(0x5283D8));
}

static void pts_change(const char *verb, void (*body)(void))
{
    uint32_t ret = MEM32(g_esp), pts = MEM16(g_esp + 4), house = MEM32(g_esp + 8);
    body();
    char what[64];
    snprintf(what, sizeof what, "%s %u %s", verb, pts, house < 4 ? g_house[house] : "?");
    pts_line(what, ret);
}

extern void sub_000818C0_gen(void);
void sub_000818C0(void) { pts_change("add", sub_000818C0_gen); }

extern void sub_000818E0_gen(void);
void sub_000818E0(void) { pts_change("take", sub_000818E0_gen); }

extern void sub_000C7850_gen(void);
void sub_000C7850(void)
{
    uint32_t ret = MEM32(g_esp), any = MEM8(g_esp + 4), budget = MEM32(g_esp + 8);
    sub_000C7850_gen();
    char what[64];
    snprintf(what, sizeof what, "caught: roll %s budget %d -> costs %u", any ? "0/5/10/20" : "5/10/20", (int32_t)budget,
             MEM32(0x545358));
    pts_line(what, ret);
}

/* House point caps (not debug: the user's choice). Each night's board moves Slytherin after Gryffindor, Ravenclaw after
 * Slytherin and Hufflepuff after Ravenclaw (sub_000E6A40(i) over the table at 0x20AD70, 32 bytes an entry, cap at
 * +8), capped 300 / 250 / 200 on the Xbox: past 300 Gryffindor leads every night and the points stop mattering.
 * Here Slytherin has no cap (it can still lead a night, the board's "Slytherin ahead" ending) and the other two 600.
 * RECOMP_HOUSE_CAPS=0 keeps the Xbox's. */
static void house_caps(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_HOUSE_CAPS");
        on = !(e && *e == '0');
    }
    if (!on) return;
    MEM32(0x20AD70 + 32 * 1 + 8) = 600;          /* Hufflepuff, after Ravenclaw (Xbox 200) */
    MEM32(0x20AD70 + 32 * 2 + 8) = 600;          /* Ravenclaw, after Slytherin (Xbox 250) */
    MEM32(0x20AD70 + 32 * 3 + 8) = 0x7FFFFFFF;   /* Slytherin, after Gryffindor (Xbox 300): none */
}

extern void sub_000E6A40_gen(void);
void sub_000E6A40(void)
{
    uint32_t ret = MEM32(g_esp), i = MEM32(g_esp + 4);
    house_caps();
    sub_000E6A40_gen();
    char what[64];
    snprintf(what, sizeof what, "follow %s", i >= 1 && i <= 3 ? g_house[4 - i] : "?");
    pts_line(what, ret);
}

/* The Grand Staircase gold vases on their plinths and the boxes whose lid opens when hit ([VASE], always on: one
 * class). A vase is an entity whose setup is
 * sub_000D18B0 (entity+0x70) and whose update is sub_000D19E0: it gets a physics body, a hit sets the body's 0x20000
 * flag, the update then spawns what the vase gives and moves to state 1 (entity+0x84). sub_00042D60(entity, flag)
 * removes an entity. Logs setup, each state change, and the removal of a vase with the code addresses on the stack,
 * to find what makes vases vanish after going through a door (the generated bodies are renamed *_gen by hand). */
static void vase_line(const char *what, uint32_t ent)
{
    uint32_t obj = MEM32(ent + 0xAC);
    float p[3] = { 0, 0, 0 };
    if (obj) memcpy(p, (const void *)XBOX_PTR(obj + 0x14), sizeof p);
    char line[512];
    int n = snprintf(line, sizeof line, "[VASE] %lu %s ent %08X obj %08X pos %.3f %.3f %.3f state %u level %u |",
                     (unsigned long)GetTickCount(), what, ent, obj, p[0], p[1], p[2], MEM32(ent + 0x84),
                     MEM32(0x527B78));
    for (uint32_t a = g_esp; a < g_esp + 0x200 && n < (int)sizeof line - 12; a += 4) {
        uint32_t v = MEM32(a);
        if (v >= 0x11000 && v < 0x130620) n += snprintf(line + n, sizeof line - n, " %08X", v);
    }
    fprintf(stderr, "%s\n", line);
}

extern void sub_000D18B0_gen(void);
void sub_000D18B0(void)
{
    uint32_t ent = MEM32(g_esp + 4), rec = MEM32(ent + 0xA8);
    /* the box model without the lid animation its record asks for (1000027D: only B0000276, which makes the box
     * vanish; the other box model 1000027B has B00001ED and is only loaded where that box is) is set up as the
     * other box model, which the setup then loads: the same box with a lid that opens. RECOMP_BOX_ANIM=0: as the
     * Xbox */
    static int fix = -1;
    if (fix < 0) {
        const char *e = getenv("RECOMP_BOX_ANIM");
        fix = !(e && e[0] == '0');
    }
    if (fix && rec && MEM32(rec + 0x20) == 0x1000027Du && MEM32(rec + 0x28) == 0xB00001EDu) {
        MEM32(rec + 0x20) = 0x1000027Bu;
        fprintf(stderr, "[VASE] box record %08X: model 1000027D (no lid animation) set up as 1000027B\n", rec);
    }
    sub_000D18B0_gen();
    vase_line("setup", ent);
}

extern void sub_000D19E0_gen(void);
void sub_000D19E0(void)
{
    uint32_t ent = MEM32(g_esp + 4), state = MEM32(ent + 0x84), body = MEM32(ent + 0xB0);
    uint32_t hit = body ? MEM32(body + 0x20) & 0x20000u : 0;
    if (hit) vase_line("hit", ent);
    sub_000D19E0_gen();
    if (MEM32(ent + 0x84) != state) vase_line("state", ent);
    /* a hit's animation (a box's lid opening, 2026-10-25: one Grand Staircase box went back to state 0 a frame
     * later, unanimated): the update starts it with sub_0003D020 from the block at 0x205CA8 (its +0 the animation,
     * looked up by sub_00030600 / sub_00050B60 from the level record's +0x20, else the body's +0x5C; +0x14 the
     * record's +0x28 or +0x2C) and keeps the player at entity+0x8C, 0 when none started; state 1 then waits for the
     * player to finish (+0x155 == 2), and without one goes back to 0 at once */
    if (hit && MEM32(ent + 0x84) == 1 && state != 1) {
        uint32_t rec = MEM32(ent + 0xA8);
        fprintf(stderr, "[VASE]   animation: player %08X | record %08X +20 %08X +28 %08X +2C %08X | block %08X %08X "
                "%08X %08X %08X %08X %08X\n", MEM32(ent + 0x8C), rec, rec ? MEM32(rec + 0x20) : 0,
                rec ? MEM32(rec + 0x28) : 0, rec ? MEM32(rec + 0x2C) : 0, MEM32(0x205CA8), MEM32(0x205CAC),
                MEM32(0x205CB0), MEM32(0x205CB4), MEM32(0x205CB8), MEM32(0x205CBC), MEM32(0x205CC0));
    }
}

/* Entities a Grand Staircase group spawner made (sub_000FC800 stores them in its list, entry+0), so their removal by
 * anything else shows up too: a list entry still pointing at a freed slot would make the spawner's clean-up
 * (sub_000FC830) remove whatever lives there next. */
static uint32_t g_spawned[64];

static int spawned_find(uint32_t ent)
{
    for (int i = 0; i < 64; i++)
        if (g_spawned[i] == ent) return i;
    return -1;
}

static void ent_line(const char *what, uint32_t ent, uint32_t entry)
{
    uint32_t obj = ent ? MEM32(ent + 0xAC) : 0;
    float p[3] = { 0, 0, 0 };
    if (obj) memcpy(p, (const void *)XBOX_PTR(obj + 0x14), sizeof p);
    char line[512];
    int n = snprintf(line, sizeof line, "[SPAWN] %lu %s entry %08X ent %08X alive %u handler %08X pos %.3f %.3f %.3f level %u |",
                     (unsigned long)GetTickCount(), what, entry, ent, ent ? MEM32(ent) & 1 : 0, ent ? MEM32(ent + 0x70) : 0,
                     p[0], p[1], p[2], MEM32(0x527B78));
    for (uint32_t a = g_esp; a < g_esp + 0x100 && n < (int)sizeof line - 12; a += 4) {
        uint32_t v = MEM32(a);
        if (v >= 0x11000 && v < 0x130620) n += snprintf(line + n, sizeof line - n, " %08X", v);
    }
    fprintf(stderr, "%s\n", line);
}

extern void sub_00042D60_gen(void);
void sub_00042D60(void)
{
    uint32_t ent = MEM32(g_esp + 4);
    if (ent && MEM32(ent + 0x70) == 0x000D18B0u) vase_line("remove", ent);
    int i = ent ? spawned_find(ent) : -1;
    if (i >= 0) {
        ent_line("remove spawned", ent, 0);
        g_spawned[i] = 0;
    }
    sub_00042D60_gen();
}

/* sub_000FC800(spawner, entry): entry = {entity, kind, id, level record}; spawns the record's entity into entry+0 */
extern void sub_000FC800_gen(void);
void sub_000FC800(void)
{
    uint32_t entry = MEM32(g_esp + 8), before = MEM32(entry);
    sub_000FC800_gen();
    uint32_t ent = MEM32(entry);
    char what[48];
    snprintf(what, sizeof what, "spawn (entry had %08X)", before);
    ent_line(what, ent, entry);
    if (ent && spawned_find(ent) < 0) {
        int i = spawned_find(0);
        if (i >= 0) g_spawned[i] = ent;
    }
}

/* sub_000FC830(list): the spawner's clean-up, removes every entry's entity (count list+0x14, entries list+0x18) */
extern void sub_000FC830_gen(void);
void sub_000FC830(void)
{
    uint32_t list = MEM32(g_esp + 4), count = MEM32(list + 0x14), entries = MEM32(list + 0x18);
    for (uint32_t k = 0; k < count; k++) {
        uint32_t entry = entries + k * 16, ent = MEM32(entry);
        if (k < 64) ent_line("clean-up", ent, entry);
        /* The level's streaming (sub_00046700) also removes these entities, clearing the level record but not this
         * entry, and the slot then goes to another entity: the Grand Staircase's left vase after the library annex,
         * which this clean-up then removed (the original does it too). sub_00043430 gives each entity its level
         * record at +0xA8, the record this entry spawned (entry+0xC) for one of ours: anything else is left alone. */
        if (ent && MEM32(ent + 0xA8) != MEM32(entry + 0xC)) {
            ent_line("clean-up skips (not this entry's)", ent, entry);
            MEM32(entry) = 0;
        }
    }
    sub_000FC830_gen();
}

/* F8: an object trace ([OBJ]) of the next frames beside a capture of one of them (the F12 capture). The game draws an
 * object through sub_00031E20(obj, ...) or sub_00020210(obj, ...) (this = the model); each line gives the draws it
 * produced, numbered as the capture's frame.txt (hpcos_draw_seq), and the object's flags (+0xE8) and colour (+0x114,
 * alpha at +0x117), to find what marks a fading object (dissolving sludge, a cut curtain). */
extern volatile uint32_t hpcos_draw_seq;
extern void hpcos_RequestCapture(void);
static int g_obj_frames;
static uint32_t g_obj_last_seq;

static void obj_trace_begin(uint32_t *seq0)
{
    if (GetAsyncKeyState(VK_F8) & 1) {
        g_obj_frames = 4;
        g_obj_last_seq = hpcos_draw_seq;
        hpcos_RequestCapture();
        fprintf(stderr, "[OBJ] trace of the next frames, capture requested\n");
    }
    if (g_obj_frames && hpcos_draw_seq < g_obj_last_seq) {
        g_obj_frames--;
        fprintf(stderr, "[OBJ] ---- frame\n");
    }
    g_obj_last_seq = hpcos_draw_seq;
    *seq0 = hpcos_draw_seq;
}

static void obj_trace_end(const char *fn, uint32_t obj, uint32_t seq0, uint32_t ret)
{
    if (!g_obj_frames || !obj) return;
    float p[3];  /* a drawn object keeps its position at +0x14 (entity+0xAC points at it) */
    memcpy(p, (const void *)XBOX_PTR(obj + 0x14), sizeof p);
    fprintf(stderr, "[OBJ] %s obj %08X draws %u-%u flags %08X colour %08X d0 %08X model %08X from %08X pos %.2f %.2f %.2f\n",
            fn, obj, seq0, hpcos_draw_seq, MEM32(obj + 0xE8), MEM32(obj + 0x114), MEM32(obj + 0xDC), MEM32(obj + 0x134),
            ret, p[0], p[1], p[2]);
    g_obj_last_seq = hpcos_draw_seq;
}

/* [FLAMES] (RECOMP_TRIG_TRACE): the torches' flames are particles, objects with model 0x1D4660 (the F8 trace, 2026-10-30:
 * ~190 drawn beside lit torches, none where they are unlit, all from the draw-list walk). Draw lists take objects through
 * the sorted insert sub_00049440 (this = the list, [esp+4] the node, whose +0 is the object, [esp+8] its priority); every
 * 2 s this counts the particles inserted and the title's code that inserted them, to find what leaves them out by
 * distance. Built into the recompile with --exclude-manual: gen's body is sub_00049440_gen */
extern void sub_00049440_gen(void);
void sub_00049440(void)
{
    static int on = -1;
    if (on < 0) on = getenv("RECOMP_TRIG_TRACE") && *getenv("RECOMP_TRIG_TRACE") != '0';
    if (on) {
        enum { kChains = 8 };
        static uint32_t chain[kChains][4], count[kChains], total;
        static DWORD last;
        uint32_t node = MEM32(g_esp + 4), obj = node ? MEM32(node) : 0;
        if (obj && MEM32(obj + 0x134) == 0x1D4660u) {
            uint32_t c[4] = {MEM32(g_esp), 0, 0, 0};
            int found = 1;
            for (uint32_t i = 3; i < 96 && found < 4; i++) {  /* the title's return addresses above it, a call before each */
                uint32_t v = MEM32(g_esp + 4 * i);
                if (v < 0x11005 || v >= 0x130620) continue;
                if (MEM8(v - 5) == 0xE8 || MEM8(v - 2) == 0xFF || MEM8(v - 3) == 0xFF || MEM8(v - 6) == 0xFF) c[found++] = v;
            }
            int k;
            for (k = 0; k < kChains && count[k] && memcmp(chain[k], c, sizeof c); k++) {}
            if (k < kChains) {
                if (!count[k]) memcpy(chain[k], c, sizeof c);
                count[k]++;
            }
            total++;
        }
        DWORD now = GetTickCount();
        static uint32_t prev;
        if (now - last > 2000 && (total || prev)) {  /* quiet while there are none, one line when they stop */
            fprintf(stderr, "[FLAMES] flame particles put in draw lists, last 2 s: %u\n", total);
            for (int k = 0; k < kChains && count[k]; k++)
                fprintf(stderr, "[FLAMES]   %u from %08X %08X %08X %08X\n", count[k], chain[k][0], chain[k][1], chain[k][2],
                        chain[k][3]);
            memset(count, 0, sizeof count);
            prev = total, total = 0, last = now;
        } else if (now - last > 2000) {
            last = now;
        }
    }
    sub_00049440_gen();
}

/* [TEXT] (RECOMP_TEXT_TRACE=1): the game's text lookup sub_0003BAC0 (__thiscall, this = the text tables 0x53E754, ret 4:
 * a text id; returns its UTF-16 string, 0x1E15F0 when missing): subtitles, prompts and menus all take their words
 * from it. Each id with its text and the title's code that asked, once per id every 5 s, to find a spoken line's id
 * and what shows and clears its subtitle (2026-10-30). Built into the recompile with --exclude-manual: gen's body is
 * sub_0003BAC0_gen */
extern void sub_0003BAC0_gen(void);
extern uint32_t hpcos_PromptText(uint32_t text);
void sub_0003BAC0(void)
{
    static int on = -1;
    if (on < 0) on = getenv("RECOMP_TEXT_TRACE") && *getenv("RECOMP_TEXT_TRACE") != '0';
    uint32_t id = on ? MEM32(g_esp + 4) : 0, ret = on ? MEM32(g_esp) : 0, up = 0;
    if (on) {  /* the next of the title's return addresses above, a call before it: who asked the asker */
        for (uint32_t i = 2; i < 64 && !up; i++) {
            uint32_t v = MEM32(g_esp + 4 * i);
            if (v >= 0x11005 && v < 0x130620 && (MEM8(v - 5) == 0xE8 || MEM8(v - 2) == 0xFF || MEM8(v - 3) == 0xFF || MEM8(v - 6) == 0xFF)) up = v;
        }
    }
    sub_0003BAC0_gen();
    g_eax = hpcos_PromptText(g_eax);  /* hle_frontend.c: key names for the buttons with key prompts on */
    if (!on) return;
    enum { kSeen = 64 };
    static uint32_t seen_id[kSeen];
    static DWORD seen_at[kSeen];
    static int next;
    DWORD now = GetTickCount();
    for (int k = 0; k < kSeen; k++)
        if (seen_id[k] == id && now - seen_at[k] < 5000) return;
    seen_id[next] = id, seen_at[next] = now, next = (next + 1) % kSeen;
    char text[200];
    int n = 0;
    for (uint32_t a = eax; eax && n < (int)sizeof text - 1; a += 2) {
        uint16_t c = MEM16(a);
        if (!c) break;
        text[n++] = c == 0x2019 ? '\'' : (c < 0x80 ? (char)c : '?');
    }
    text[n] = 0;
    fprintf(stderr, "[TEXT] %lu id %08X from %08X %08X: %s\n", (unsigned long)now, id, ret, up, text);
}

extern void sub_00031E20_gen(void);
void sub_00031E20(void)
{
    uint32_t obj = MEM32(g_esp + 4), ret = MEM32(g_esp), seq0;
    obj_trace_begin(&seq0);
    sub_00031E20_gen();
    obj_trace_end("31E20", obj, seq0, ret);
}

extern void sub_00020210_gen(void);
void sub_00020210(void)
{
    uint32_t obj = MEM32(g_esp + 4), ret = MEM32(g_esp), seq0;
    obj_trace_begin(&seq0);
    sub_00020210_gen();
    obj_trace_end("20210", obj, seq0, ret);
}

/* The music manager (0x1D3C20, EA PathFinder: a level's .mus/.mpf loaded by sub_0001CE70) switches with
 * sub_0001C9B0(state, arg) (__thiscall, ret 8). In the Basilisk fight (level 25, Chamber of Secrets, phase word
 * [0x527DFC] == 2) Harry picking up Gryffindor's sword asks for state 2 (0xD6992), the sword's theme, and dropping it
 * for state 1 (0xD68DB), the fight's music, so the theme stops whenever the sword is put down. Once the theme has
 * started it plays on, looping, for the rest of the fight: the switch back to 1 and every later state 2 (which would
 * start the theme over) are left out; any other state (the fight's end, a cutscene, another level) goes through. RECOMP_SWORD_MUSIC=0 keeps the Xbox's switching.
 * [MUSIC] logs every request: the state, its argument, the caller, the code above it, the level and the phase.
 *
 * A level script's request (opcode 84 of the interpreter sub_0006F010, returning to 0x7083B) for the state the manager
 * is in already ([0x1D3C2C], set by each switch, -1 after a level's music loads) is left out: it starts that state's
 * music over. The one known (2026-10-24): DADA
 * Expelliarmus (level 1), the spell book's pickup asks for state 10, its jingle (0x87D71); closing the spell
 * screen after it, the built-in script that asks for 10 again (0x1EC4C4, after the book's cutscene descriptor
 * cs_bu) ran and the jingle played a second time. On the console (checked over xbdm) that request never comes; no
 * other script in any log repeats the state playing. RECOMP_MUSIC_REPEAT=1 lets it through.
 *
 * The Entrance Hall (level 27, sub_000D82C0) and the Grand Staircase (level 29, sub_000FC450) switch music by distance
 * in one part of the story: each frame, Harry within an object's radius (sub_000B3ED0: the object's property
 * 0x100001B, radius at +0x38) asks for state 1, outside it for state 0 again. The object moves, so the music changed
 * every few seconds with Harry standing still; the user asked for both gone (2026-10-24). The request for 1 is left
 * out, and so is the one back to 0 when 0 is playing still (it would start it over); the levels' other cases (states 2
 * and 3) are kept. Off by default since the same day: with the switching gone the hall's track played once and the
 * music stopped. RECOMP_HALL_MUSIC=0 turns the removal on. */
#define LEVEL_CHAMBER     25u
#define PHASE_BASILISK    2u
#define MUSIC_FIGHT       1u
#define MUSIC_SWORD       2u

static int hall_music_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_HALL_MUSIC");
        on = !(e && *e == '0');
    }
    return on;
}

static int music_repeat_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_MUSIC_REPEAT");
        on = e && *e == '1';
    }
    return on;
}

static int sword_music_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_SWORD_MUSIC");
        on = !(e && *e == '0');
    }
    return on;
}

/* the Chamber's finale: the fight is won (level 25 asks for music state 4, 0xD6370) and the last cutscene plays, until
 * the next music request (the next level's). Read by the renderer (d3d12.cpp DrawEyesShrunk: Riddle's eyes). */
#define MUSIC_FIGHT_WON   4u
volatile int hpcos_chamber_finale;

extern void sub_0001C9B0_gen(void);
void sub_0001C9B0(void)
{
    static int sword_heard;  /* the sword's theme has played in this fight */
    uint32_t state = MEM32(g_esp + 4), arg = MEM32(g_esp + 8), ret = MEM32(g_esp);
    uint32_t level = MEM32(0x00527B78), phase = MEM16(0x00527DFC);
    int fight = level == LEVEL_CHAMBER && phase == PHASE_BASILISK;
    if (!fight) sword_heard = 0;
    int keep = fight && sword_heard && (state == MUSIC_FIGHT || state == MUSIC_SWORD) && sword_music_enabled();
    int repeat = ret == 0x0007083Bu && MEM32(0x1D3C24) && MEM32(0x1D3C2C) == state && !music_repeat_enabled();
    int hall = !hall_music_enabled() &&
               ((level == 27u && (ret == 0x000D838Bu || (ret == 0x000D833Bu && MEM32(0x1D3C2C) == state))) ||
                (level == 29u && (ret == 0x000FC584u || (ret == 0x000FC537u && MEM32(0x1D3C2C) == state))));
    /* the title's return addresses on the stack above it (a call before each), to tell which code asked */
    char script[160] = " chain";
    for (uint32_t i = 1, n = 0, len = 6; i < 160 && n < 10; i++) {
        uint32_t v = MEM32(g_esp + 4 * i);
        if (v < 0x11005 || v >= 0x180000) continue;
        if (MEM8(v - 5) != 0xE8 && MEM8(v - 2) != 0xFF && MEM8(v - 3) != 0xFF && MEM8(v - 6) != 0xFF) continue;
        len += snprintf(script + len, sizeof script - len, " %05X", v);
        n++;
    }
    fprintf(stderr, "[MUSIC] %lu state %u arg %u from %08X%s level %u phase %u, manager player %08X buffer %08X state %d%s%s\n",
            (unsigned long)GetTickCount(), state, arg, ret, script, level, phase, MEM32(0x1D3C20), MEM32(0x1D3C24),
            (int32_t)MEM32(0x1D3C2C), MEM32(0x1D3C24) ? "" : " (no music loaded: the request does nothing)",
            keep ? " (left out: the sword's theme plays on)"
            : repeat ? " (left out: a script asking for the state playing)"
            : hall ? " (left out: the hall's music by distance)" : "");
    if (keep || repeat || hall) {
        g_esp += 12;  /* ret 8 */
        return;
    }
    if (fight && state == MUSIC_SWORD) sword_heard = 1;
    hpcos_chamber_finale = level == LEVEL_CHAMBER && state == MUSIC_FIGHT_WON;
    sub_0001C9B0_gen();
}

/* [MUSICLOAD] a level's music load, sub_0001CE70 (__thiscall on the manager 0x1D3C20, ret 20; the .mpf's path is the
 * 2nd argument): it reads the .mpf (sub_00032250 size, sub_00035DC0 + sub_00032DD0) into +8, allocates the stream's
 * buffer from the game heap [0x51DEB4] (sub_00035D80, tag 0x17E288) into +4 and makes the player (sub_00126250) into
 * +0. A failed buffer clears the manager (player -1, buffer 0, .mpf 0), and every sub_0001C9B0 request after it does
 * nothing: no .mus opens. A tester's fresh install (2026-10-30) had no music in the Burrow and Diagon Alley with the
 * .mpf read in full and the requests passing; the tester's logs come late, so one run has to tell every step apart:
 *   [MUSICLOAD] load / alloc / player / done: each step of the load, the buffer's size, the game heap's free bytes and
 *     largest free block, the player sub_00126250 made, and the .mus beside the exe (game\EAMusic, its size);
 *   [MUSICREQ] what PathFinder's request sub_00126800(player, state, arg) answered;
 *   [HEAPFAIL] any game heap allocation refused (sub_00035730(5) after the free list walk, sub_00035D80 giving 0), the
 *     tag, size and heap state, the first 40;
 *   [MUSICSTATE] every 10 s with the [AUDIO] report (hle_dsound.c): the manager's fields, the music volume
 *     ([0x1E1658] x [0x17DEF0], what sub_0001C9B0 hands sub_00126E20), and how often PathFinder's update sub_00124760
 *     and the streamer's refill sub_00117550 ran: zero means that thread stopped.
 * The game heap (sub_00035C40): blocks with a 0x24-byte header, +0 bit 0 free, +0x14 size; free list from heap+4 by +8.
 * Gen's bodies are renamed *_gen by hand (regen's --exclude-manual does it): sub_0001CE70, sub_00035D80, sub_00035730,
 * sub_00126250, sub_00126800, sub_00124760, sub_00117550. */
extern void sub_0001CE70_gen(void);
extern void sub_00035D80_gen(void);
extern void sub_00035730_gen(void);
extern void sub_00126250_gen(void);
extern void sub_00126800_gen(void);
extern void sub_00124760_gen(void);
extern void sub_00117550_gen(void);
static int g_music_loading, g_music_alloc;  /* 1 asked and given, -1 refused */
static volatile LONG g_pf_updates, g_mus_refills, g_heap_fails;

static int guest_ptr_ok(uint32_t a)
{
    return (a >= 0x10000u && a < 0x04000000u) || (a >= 0x80000000u && a < 0x84000000u);
}

/* free bytes, largest free block and free block count of a game heap (its free list) */
static void game_heap_stats(uint32_t heap, uint32_t *free_bytes, uint32_t *largest, uint32_t *count)
{
    *free_bytes = *largest = *count = 0;
    if (!guest_ptr_ok(heap)) return;
    for (uint32_t b = MEM32(heap + 4), n = 0; guest_ptr_ok(b) && n < 100000; b = MEM32(b + 8), n++) {
        if (!(MEM32(b) & 1)) continue;
        uint32_t sz = MEM32(b + 0x14);
        *free_bytes += sz, (*count)++;
        if (sz > *largest) *largest = sz;
    }
}

static void guest_cstr(uint32_t p, char *out, int cap)
{
    int n = 0;
    for (; guest_ptr_ok(p) && n < cap - 1; p++) {
        uint8_t c = MEM8(p);
        if (!c) break;
        out[n++] = c >= 0x20 && c < 0x7F ? (char)c : '?';
    }
    out[n] = 0;
}

static void heap_fail(const char *how, uint32_t heap, uint32_t tag, uint32_t size, uint32_t ret)
{
    if (InterlockedIncrement(&g_heap_fails) > 40) return;
    uint32_t fr, big, cnt;
    game_heap_stats(heap, &fr, &big, &cnt);
    char name[64] = "";
    if (tag >= 0x11000u && tag < 0x200000u) guest_cstr(tag, name, sizeof name);
    fprintf(stderr, "[HEAPFAIL] %lu %s heap %08X%s tag %08X '%s' size %u from %08X: free %u bytes in %u blocks, largest %u,"
            " level %u%s\n", (unsigned long)GetTickCount(), how, heap, heap == MEM32(0x51DEB4) ? " (main)" : "", tag, name,
            size, ret, fr, cnt, big, MEM32(0x00527B78), g_music_loading ? " (during a music load)" : "");
}

void sub_00035D80(void)
{
    uint32_t tag = MEM32(g_esp + 4), size = MEM32(g_esp + 8), ret = MEM32(g_esp);
    sub_00035D80_gen();
    if (!g_eax) heap_fail("alloc refused", MEM32(0x51DEB4), tag, size, ret);
    if (g_music_loading && tag == 0x17E288u) {
        uint32_t fr, big, cnt;
        game_heap_stats(MEM32(0x51DEB4), &fr, &big, &cnt);
        g_music_alloc = g_eax ? 1 : -1;
        fprintf(stderr, "[MUSICLOAD] alloc %u bytes (%.1f KB) from %08X -> %08X%s; heap now free %u in %u blocks, largest %u\n",
                size, size / 1024.0, ret, g_eax, g_eax ? "" : " (the game heap refused it)", fr, cnt, big);
    }
}

/* the free list walk found no block (sub_000358D0, thiscall on the heap, code 5) */
void sub_00035730(void)
{
    uint32_t heap = g_ecx, code = MEM32(g_esp + 4), ret = MEM32(g_esp);
    if (code == 5) heap_fail("no free block", heap, 0, 0, ret);
    sub_00035730_gen();
}

void sub_00126250(void)
{
    uint32_t ret = MEM32(g_esp);
    sub_00126250_gen();
    if (g_music_loading || ret == 0x0001CF54u)
        fprintf(stderr, "[MUSICLOAD] player from sub_00126250 -> %08X\n", g_eax);
}

/* [PFRACE] PathFinder's busy word [0x54DF18] (0 idle, -1 the tick, else the selected context) is a plain
 * check-then-set: each API call and the tick sub_00125F00 test it for 0, take it (sub_001240A0) and clear it at the end.
 * On the console one core with the tick on a higher-priority thread keeps them apart; here a thread can block inside
 * (the audio trace's stderr writes) and another walk in. With the audio trace on and the device at 192 kHz (Windows,
 * 2026-10-30) the level's .mus never opened after an accepted request and PathFinder's update sub_00124760 stopped.
 * Every entry is tracked by host thread: a second thread entering while one is inside is logged ([PFRACE], the first
 * 30), and the tick's outcomes are counted for [MUSICSTATE]: ran, paused ([0x54DF15]), off ([0x54DF14] 0), busy
 * ([0x54DF18] not 0), by caller (0x125FC7 the 100 ms service, 0x125FD7 the streamer's callback list). */
static volatile LONG g_pf_owner, g_pf_depth, g_pf_races;
static const char *volatile g_pf_owner_fn;
static volatile LONG g_tick_ran, g_tick_paused, g_tick_off, g_tick_busy, g_tick_svc, g_tick_list, g_tick_max_us;

static int pf_enter(const char *fn)
{
    LONG me = (LONG)GetCurrentThreadId();
    LONG owner = InterlockedCompareExchange(&g_pf_owner, me, 0);
    if (owner && owner != me) {
        if (InterlockedIncrement(&g_pf_races) <= 30)
            fprintf(stderr, "[PFRACE] %lu %s on thread %ld entered while thread %ld is in %s; busy word %08X, ret %08X\n",
                    (unsigned long)GetTickCount(), fn, me, owner, g_pf_owner_fn ? g_pf_owner_fn : "?", MEM32(0x54DF18),
                    MEM32(g_esp));
        return 0;
    }
    if (!owner) g_pf_owner_fn = fn;
    InterlockedIncrement(&g_pf_depth);
    return 1;
}

static void pf_leave(int owned)
{
    if (!owned) return;
    if (InterlockedDecrement(&g_pf_depth) == 0) g_pf_owner_fn = 0, InterlockedExchange(&g_pf_owner, 0);
}

#define PF_WRAP(addr, name) \
    extern void sub_##addr##_gen(void); \
    void sub_##addr(void) { int o = pf_enter(name); sub_##addr##_gen(); pf_leave(o); }
PF_WRAP(00124100, "sub_00124100")
PF_WRAP(00126290, "sub_00126290 (player)")
PF_WRAP(00126710, "sub_00126710")
PF_WRAP(00126B10, "sub_00126B10")
PF_WRAP(00126D60, "sub_00126D60")
PF_WRAP(00126E20, "sub_00126E20 (volume)")
PF_WRAP(00126F10, "sub_00126F10")

/* PathFinder's pause (player, 1 pause / 0 resume): the game's music pause sub_0001C920 and resume sub_0001C930, and the
 * request itself; a pause left on stops the tick ([0x54DF15]) */
extern void sub_00126C40_gen(void);
void sub_00126C40(void)
{
    uint32_t player = MEM32(g_esp + 4), on = MEM32(g_esp + 8), ret = MEM32(g_esp), up = 0;
    for (uint32_t i = 1; i < 32 && !up; i++) {
        uint32_t v = MEM32(g_esp + 4 * i);
        if (v >= 0x11005 && v < 0x130620 && v != ret && MEM8(v - 5) == 0xE8) up = v;
    }
    int o = pf_enter("sub_00126C40 (pause)");
    sub_00126C40_gen();
    pf_leave(o);
    fprintf(stderr, "[MUSIC] %lu PathFinder %s player %08X from %08X %08X -> %08X, pause byte now %u\n",
            (unsigned long)GetTickCount(), on ? "pause" : "resume", player, ret, up, g_eax, MEM8(0x54DF15));
}

extern void sub_00125F00_gen(void);
void sub_00125F00(void)
{
    uint32_t ret = MEM32(g_esp);
    InterlockedIncrement(ret == 0x00125FC7u ? &g_tick_svc : &g_tick_list);
    if (MEM8(0x54DF15)) InterlockedIncrement(&g_tick_paused);
    else if (!MEM8(0x54DF14)) InterlockedIncrement(&g_tick_off);
    else if (MEM32(0x54DF18)) InterlockedIncrement(&g_tick_busy);
    else InterlockedIncrement(&g_tick_ran);
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    int o = pf_enter("sub_00125F00 (tick)");
    sub_00125F00_gen();
    pf_leave(o);
    QueryPerformanceCounter(&t1);
    LONG us = (LONG)((t1.QuadPart - t0.QuadPart) * 1000000 / f.QuadPart);
    for (LONG m = g_tick_max_us; us > m && InterlockedCompareExchange(&g_tick_max_us, us, m) != m; m = g_tick_max_us) {}
}

void sub_00126800(void)
{
    uint32_t player = MEM32(g_esp + 4), state = MEM32(g_esp + 8), arg = MEM32(g_esp + 12), ret = MEM32(g_esp);
    uint32_t busy = MEM32(0x54DF18);
    int o = pf_enter("sub_00126800 (request)");
    sub_00126800_gen();
    pf_leave(o);
    fprintf(stderr, "[MUSICREQ] %lu PathFinder request player %08X state %u arg %u from %08X -> %08X (busy word before %08X,"
            " after %08X)%s\n", (unsigned long)GetTickCount(), player, state, arg, ret, g_eax, busy, MEM32(0x54DF18),
            g_eax == 0xFFFFFFEEu ? " (DROPPED: PathFinder was busy)" : "");
}

void sub_00124760(void)
{
    InterlockedIncrement(&g_pf_updates);
    sub_00124760_gen();
}

void sub_00117550(void)
{
    InterlockedIncrement(&g_mus_refills);
    sub_00117550_gen();
}

/* hle_dsound.c's output thread, every 10 s */
void hpcos_music_report(void)
{
    float vol = MEMF(0x1E1658), scale = MEMF(0x17DEF0);
    fprintf(stderr, "[MUSICSTATE] %lu level %u: manager player %08X buffer %08X mpf %08X state %d, volume %.3f x %.1f,"
            " PathFinder updates %ld, streamer refills %ld, game heap failures %ld; ticks ran %ld paused %ld off %ld busy %ld"
            " (service %ld, streamer list %ld, longest %.2f ms), busy word %08X paused %u on %u, races %ld\n",
            (unsigned long)GetTickCount(), MEM32(0x00527B78), MEM32(0x1D3C20), MEM32(0x1D3C24), MEM32(0x1D3C28),
            (int32_t)MEM32(0x1D3C2C), vol, scale, InterlockedExchange(&g_pf_updates, 0),
            InterlockedExchange(&g_mus_refills, 0), g_heap_fails, InterlockedExchange(&g_tick_ran, 0),
            InterlockedExchange(&g_tick_paused, 0), InterlockedExchange(&g_tick_off, 0),
            InterlockedExchange(&g_tick_busy, 0), InterlockedExchange(&g_tick_svc, 0),
            InterlockedExchange(&g_tick_list, 0), InterlockedExchange(&g_tick_max_us, 0) / 1000.0, MEM32(0x54DF18),
            MEM8(0x54DF15), MEM8(0x54DF14), g_pf_races);
}

/* the .mus beside the exe for a .mpf path (game\EAMusic\<name>.mus): its size, -1 missing */
static long long host_mus_size(const char *mpf, char *base, int cap)
{
    const char *s = mpf;
    for (const char *p = mpf; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':') s = p + 1;
    int n = 0;
    for (; s[n] && s[n] != '.' && n < cap - 1; n++) base[n] = s[n];
    base[n] = 0;
    char path[160];
    snprintf(path, sizeof path, "game\\EAMusic\\%s.mus", base);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) return -1;
    return ((long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
}

void sub_0001CE70(void)
{
    uint32_t ret = MEM32(g_esp), a[5], mgr = g_ecx;
    for (int i = 0; i < 5; i++) a[i] = MEM32(g_esp + 4 + 4 * i);
    char name[96], base[64];
    guest_cstr(a[1], name, sizeof name);
    long long mus = host_mus_size(name, base, sizeof base);
    uint32_t fr, big, cnt;
    game_heap_stats(MEM32(0x51DEB4), &fr, &big, &cnt);
    fprintf(stderr, "[MUSICLOAD] %lu load '%s' from %08X args %08X %08X %08X %08X %08X, before: player %08X buffer %08X"
            " mpf %08X; game\\EAMusic\\%s.mus %lld bytes%s; game heap free %u in %u blocks, largest %u\n",
            (unsigned long)GetTickCount(), name, ret, a[0], a[1], a[2], a[3], a[4], MEM32(mgr), MEM32(mgr + 4),
            MEM32(mgr + 8), base, mus, mus < 0 ? " (MISSING)" : mus == 0 ? " (EMPTY)" : "", fr, cnt, big);
    g_music_loading = 1, g_music_alloc = 0;
    sub_0001CE70_gen();
    g_music_loading = 0;
    uint32_t player = MEM32(mgr), buffer = MEM32(mgr + 4), mpf = MEM32(mgr + 8);
    fprintf(stderr, "[MUSICLOAD] %lu '%s' done: player %08X buffer %08X mpf %08X%s\n", (unsigned long)GetTickCount(), name,
            player, buffer, mpf,
            g_music_alloc < 0 ? " (FAILED: the game heap refused the stream buffer)"
            : !mpf ? " (FAILED: the .mpf didn't load)"
            : !buffer ? " (FAILED: no stream buffer)"
            : !player || player == 0xFFFFFFFFu ? " (FAILED: no player)" : "");
}

/* [VASE] why a hit's animation didn't start: sub_0003CF20(block, -1, 0x5276B0) (from sub_0003D020) makes the player and
 * gives 0 when the entry (block+0) has no data (+0x24) or bit 2 of its flags (+0x14) is set, when the animation
 * (block+0x14) isn't among the entry's (sub_0003C600: data+0x34 records of 16 bytes from data+0x38 + data+0x38, the id
 * first), when the player pool (sub_00039020(4), 0x51DF80) is out, or when sub_0003CE20 refuses the player. Only for
 * calls from the box / vase update (sub_0003D020 returning to 0xD1B99). The generated bodies are renamed *_gen by hand */
extern void sub_00039020_gen(void);
static uint32_t g_last_player_alloc;
void sub_00039020(void)
{
    sub_00039020_gen();
    g_last_player_alloc = g_eax;
}

extern void sub_0003CF20_gen(void);

/* an entry's animations (sub_0003C600's walk): data+0x34 records of 16 bytes from data+0x38 + [data+0x38], the id
 * first; the index of id, or -1, and the count */
static int entry_animation(uint32_t data, uint32_t id, int *count)
{
    *count = data ? (int32_t)MEM32(data + 0x34) : 0;
    uint32_t base = data && MEM32(data + 0x38) ? data + 0x38 + MEM32(data + 0x38) : 0;
    if (!base) *count = 0;
    for (int i = 0; i < *count; i++)
        if (MEM32(base + 16u * (uint32_t)i) == id) return i;
    return -1;
}

/* A box whose model (level record +0x20 1000027D, on the Grand Staircase) lacks the lid animation its record asks
 * for (B00001ED; it has only B0000276, which made the box vanish): the game's data, the lid never opened
 * (2026-10-25). A box or vase whose model lacks the asked animation plays it from the first model in the table
 * (0x1D3D80, 0x2D6 entries of 0x4C) that has it loaded: the other box model (1000027B), whose lid opens.
 * RECOMP_BOX_ANIM=0: as the Xbox */
void sub_0003CF20(void)
{
    uint32_t block = MEM32(g_esp + 4), outer = MEM32(g_esp + 0x10);
    int ours = MEM32(g_esp) == 0x0003D031u && outer == 0x000D1B99u;
    static int fix = -1;
    if (fix < 0) {
        const char *e = getenv("RECOMP_BOX_ANIM");
        fix = !(e && e[0] == '0');
    }
    uint32_t entry = ours ? MEM32(block) : 0, data = entry ? MEM32(entry + 0x24) : 0, id = MEM32(block + 0x14);
    int count;
    if (fix && data && entry_animation(data, id, &count) < 0) {
        for (uint32_t e = 0x1D3D80u, k = 0; k < 0x2D6; k++, e += 0x4C) {
            uint32_t d = MEM32(e + 0x24);
            int n;
            if (e == entry || !d || (MEM32(e + 0x14) & 4) || entry_animation(d, id, &n) < 0) continue;
            MEM32(block) = e;
            fprintf(stderr, "[VASE]   model %08X has no animation %08X: played from model %08X\n", entry, id, e);
            entry = e, data = d;
            break;
        }
    }
    g_last_player_alloc = 0xFFFFFFFFu;
    sub_0003CF20_gen();
    if (!ours || g_eax) return;
    uint32_t flags = entry ? MEM32(entry + 0x14) : 0;
    int found = entry_animation(data, id, &count);
    const char *why = !entry || !data ? "the entry has no data" : (flags & 4) ? "the entry's flag bit 2 is set"
                      : found < 0 ? "the animation isn't the entry's" : g_last_player_alloc == 0 ? "the player pool is out"
                      : "sub_0003CE20 refused the player";
    fprintf(stderr, "[VASE]   no player: %s | entry %08X flags %08X data %08X animation %08X, entry has %d\n", why,
            entry, flags, data, id, count);
}
