/**
 * The debug trainer's game side (the trainer itself is hpcos_trainer.exe,
 * src/trainer): a named shared block (trainer_shm.h) and Harry's per-frame
 * update, sub_00053710(entity), as the place the requests are carried out --
 * the title's own thread, between its frames.
 *
 * What is known of the title (found for this):
 *   [0x530408]           Harry's entity; +0xAC his object
 *   object +0x14/18/1C   world position (y up), +0x3C yaw; the title's own
 *                        movers (the push code at 0x7DA7B) write them directly
 *   0x527B78             the progress block (0x2064 bytes, what a save holds);
 *                        its first dword is the level table index played
 *   0x527EF4 / 0x527EF6  health / its maximum (words, the maximum up to 255);
 *                        sub_00081860(n) heals, sub_00081890(n) raises the
 *                        maximum
 *   0x527F0C             the spells learnt (bits, HPCOS_ALL_SPELLS)
 *   0x527E0A + 2n        wizard card n (1..101), 0x527ED6 how many are held;
 *                        sub_00081A10(0x2000 + n) gives one (the pickup's
 *                        way: every 10th new card raises the maximum health)
 *   0x527EE0 / 0x527EE2  beans / the bag's capacity (50, +50 an upgrade)
 *   0x527B94 / 0x527DFC  the level to load next / its start point: the pause
 *                        menu's level selector (0x4F269) writes both
 *   0x51E094 / 0x51E098  the left stick after the title's deadzone;
 *                        sub_00036E10(x, y) is its angle, sub_00021BC0 adds
 *                        the camera's yaw -- the heading Harry runs along
 *
 * Noclip: Harry's update is skipped (no walking, falling, collision or
 * casting) and the left stick moves him along the heading the title would
 * give him, the right / left trigger up / down, A held 4x as fast.
 *
 * Undo: the first change the trainer makes keeps what it touches (cards,
 * health, beans, the bag, the spells) to put back.
 *
 * The debug report runs on a thread of its own (a hung title still answers):
 * every thread's native stack, the state below and a frame capture, into
 * the folder the trainer names; the trainer copies memory and logs itself.
 *
 * Built into the recompile with --exclude-manual ../hpcos/src/hle_trainer.c;
 * gen/recomp_0001.c's generated body is renamed sub_00053710_gen by hand
 * until then.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECOMP_GENERATED_CODE
#include "recomp/gen/recomp_types.h"
#include "input/xinput_xbox.h"
#include "trainer_shm.h"

#define ARG(i) MEM32(g_esp + 4 + 4 * (i))

enum {
    kPlayer = 0x530408, kProgress = 0x527B78, kHealth = 0x527EF4, kHealthMax = 0x527EF6, kHealthCap = 255,
    kSpells = 0x527F0C, kCards = 0x527E0A, kCardCount = 101, kCardTotal = 0x527ED6, kBeans = 0x527EE0,
    kBeansMax = 0x527EE2, kBagStep = 50, kLevelReq = 0x527B94, kLevelStart = 0x527DFC, kSelectorStart = 0x1EF704, kStickX = 0x51E094,
    kStickY = 0x51E098, kCardItem = 0x2000,
};

/* what the first trainer change found, to put back (Undo) */
static struct {
    int kept;
    uint16_t cards[kCardCount + 1], card_total;
    int16_t health, health_max, beans, beans_max;
    uint32_t spells;
} g_undo;
static uint8_t g_given[kCardCount + 1];  /* cards the trainer gave */

extern void sub_00053710_gen(void);
extern void sub_00081A10(void);
extern void sub_00041A60(void);
extern void sub_00081860(void);
extern void sub_00081890(void);
extern void sub_0004AD50(void);
extern void sub_00036E10(void);
extern void sub_00021BC0(void);
extern void hpcos_StackDumpTo(const char *path);
extern void hpcos_RequestCapture(void);
extern void hpcos_WideAspectTick(void); /* hle_video.c: 16:9 put back after a 4:3 screen */
extern ptrdiff_t g_xbox_mem_offset;

/* hle_input.c: the pad as the game last read it */
extern XBOX_GAMEPAD hpcos_last_pad;

static HpcosTrainerShm *g_shm;

/* A level's setup (message 0x20000003) sets its progress word from the selector's start only while the word is 0,
 * the level never played, or only some of its bits; on a save past it the start just places Harry and the level
 * stays done (the Library's Four Bolt Door duel, start 7, never comes back). Replay clears the word so the start sets it as on a new save.
 * The words, from each setup's check before its start switch: */
static const struct { uint16_t level; uint32_t addr; } kLevelProgress[] = {
    {2, 0x527CD8},  /* Library, sub_000EBA50: start 0..8 = 0 1 7 1F 3F 3F 17F 37F 77F */
    /* Chamber of Secrets, sub_000D5DF0: its starts always apply but keep the word's high bits (the fight's outcome);
     * cleared, start 2 At Chamber is the Basilisk fight as on a new save */
    {25, 0x527D20},
};

static void forget_level(uint32_t level)
{
    for (size_t i = 0; i < sizeof kLevelProgress / sizeof kLevelProgress[0]; i++)
        if (kLevelProgress[i].level == level) {
            fprintf(stderr, "[TRAINER] level %u progress %08X forgotten\n", level, MEM32(kLevelProgress[i].addr));
            MEM32(kLevelProgress[i].addr) = 0;
            return;
        }
    fprintf(stderr, "[TRAINER] level %u: where it keeps its progress isn't known, warping without replay\n", level);
}

static HpcosTrainerShm *shm(void);

/* hle_input.c: the trainer's injected pad state, added to port 0's while it lasts (inject_seq / inject_ms) */
void hpcos_TrainerInject(XBOX_GAMEPAD *g)
{
    static uint32_t seen;
    static DWORD until;
    HpcosTrainerShm *t = shm();
    if (!t) return;
    if (t->inject_seq != seen) {
        seen = t->inject_seq;
        until = GetTickCount() + t->inject_ms;
        fprintf(stderr, "[TRAINER] input %04X analog %02X%02X%02X%02X%02X%02X%02X%02X for %u ms\n", t->inject_buttons,
                t->inject_analog[0], t->inject_analog[1], t->inject_analog[2], t->inject_analog[3], t->inject_analog[4],
                t->inject_analog[5], t->inject_analog[6], t->inject_analog[7], t->inject_ms);
    }
    if ((LONG)(GetTickCount() - until) >= 0) return;
    g->wButtons |= t->inject_buttons;
    for (int i = 0; i < 8; i++)
        if (t->inject_analog[i] > g->bAnalogButtons[i]) g->bAnalogButtons[i] = t->inject_analog[i];
    if (t->inject_stick[0] || t->inject_stick[1]) g->sThumbLX = t->inject_stick[0], g->sThumbLY = t->inject_stick[1];
    if (t->inject_stick[2] || t->inject_stick[3]) g->sThumbRX = t->inject_stick[2], g->sThumbRY = t->inject_stick[3];
}

static HpcosTrainerShm *shm(void)
{
    static int tried;
    if (!g_shm && !tried) {
        tried = 1;
        HANDLE m = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(HpcosTrainerShm),
                                      HPCOS_TRAINER_NAME);
        if (m) g_shm = (HpcosTrainerShm *)MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(HpcosTrainerShm));
        if (g_shm) {
            memset(g_shm, 0, sizeof(*g_shm));
            g_shm->version = HPCOS_TRAINER_VERSION;
            g_shm->pid = GetCurrentProcessId();
            g_shm->mem_offset = (uint64_t)g_xbox_mem_offset;
            g_shm->magic = HPCOS_TRAINER_MAGIC;
        }
    }
    return g_shm;
}

/* runs a recompiled function on this guest thread, the arguments pushed right to left and a return address,
 * as `call` leaves them (hle_d3d8.cpp's CallGuest) */
static uint32_t call_guest(void (*fn)(void), int n, const uint32_t *args)
{
    uint32_t s_esp = g_esp, s_ebx = g_ebx, s_esi = g_esi, s_edi = g_edi, s_ebp = g_ebp;
    uint32_t sp = s_esp;
    for (int i = n; i-- > 0;) {
        sp -= 4;
        MEM32(sp) = args[i];
    }
    sp -= 4;
    MEM32(sp) = 0;  /* return address */
    g_esp = sp;
    fn();
    g_esp = s_esp, g_ebx = s_ebx, g_esi = s_esi, g_edi = s_edi, g_ebp = s_ebp;
    return g_eax;
}

/* the same for a function returning in st(0) */
static double call_guest_fp(void (*fn)(void), int n, const uint32_t *args)
{
    call_guest(fn, n, args);
    double r = g_fp_stack[g_fp_top & 7];
    g_fp_top = (g_fp_top + 1) & 7;
    return r;
}

static uint32_t fbits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static void place(uint32_t obj, const float p[3])
{
    MEMF(obj + 0x14) = p[0], MEMF(obj + 0x18) = p[1], MEMF(obj + 0x1C) = p[2];
}

static void fly(uint32_t obj, HpcosTrainerShm *t)
{
    static LARGE_INTEGER last, freq;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    float dt = last.QuadPart ? (float)((double)(now.QuadPart - last.QuadPart) / (double)freq.QuadPart) : 0;
    last = now;
    if (!(dt > 0 && dt < 0.25f)) dt = 1 / 60.0f;

    float x = MEMF(kStickX), y = MEMF(kStickY);
    float m = sqrtf(x * x + y * y);
    if (m > 1) m = 1;
    XBOX_GAMEPAD g = hpcos_last_pad;
    float up = (g.bAnalogButtons[7] - g.bAnalogButtons[6]) / 255.0f;
    if (m < 0.05f && up == 0) return;
    float speed = t->fly_speed > 0 ? t->fly_speed : 10.0f;
    if (g.bAnalogButtons[0] > 64) speed *= 4;
    float k = speed * dt;
    float p[3] = {MEMF(obj + 0x14), MEMF(obj + 0x18), MEMF(obj + 0x1C)};
    if (m >= 0.05f) {
        /* the heading the title turns Harry to for this stick (0x62690) */
        uint32_t a[2] = {fbits(x), fbits(y)};
        uint32_t s[1] = {fbits((float)call_guest_fp(sub_00036E10, 2, a))};
        float h = (float)call_guest_fp(sub_00021BC0, 1, s);
        /* forward along the yaw is (sin, cos) in x / z: Harry's own step back (0x563E0) subtracts those */
        MEMF(obj + 0x3C) = h;
        p[0] += sinf(h) * m * k;
        p[2] += cosf(h) * m * k;
    }
    p[1] += up * k;
    place(obj, p);
}

static void keep_for_undo(void)
{
    if (g_undo.kept) return;
    g_undo.kept = 1;
    for (int c = 1; c <= kCardCount; c++) g_undo.cards[c] = MEM16(kCards + 2 * c);
    g_undo.card_total = MEM16(kCardTotal);
    g_undo.health = (int16_t)MEM16(kHealth), g_undo.health_max = (int16_t)MEM16(kHealthMax);
    g_undo.beans = (int16_t)MEM16(kBeans), g_undo.beans_max = (int16_t)MEM16(kBeansMax);
    g_undo.spells = MEM32(kSpells);
}

static void undo(HpcosTrainerShm *t)
{
    if (!g_undo.kept) return;
    for (int c = 1; c <= kCardCount; c++) MEM16(kCards + 2 * c) = g_undo.cards[c];
    MEM16(kCardTotal) = g_undo.card_total;
    MEM16(kHealthMax) = (uint16_t)g_undo.health_max;
    MEM16(kHealth) = (uint16_t)(g_undo.health < g_undo.health_max ? g_undo.health : g_undo.health_max);
    MEM16(kBeansMax) = (uint16_t)g_undo.beans_max, MEM16(kBeans) = (uint16_t)g_undo.beans;
    MEM32(kSpells) = g_undo.spells;
    memset(g_given, 0, sizeof g_given);
    g_undo.kept = 0;
    t->all_spells = 0;  /* else the next frame sets them again */
    fprintf(stderr, "[TRAINER] undone: back to before the first trainer change\n");
}

static int cards_held(void)
{
    int n = 0;
    for (int c = 1; c <= kCardCount; c++) n += MEM16(kCards + 2 * c) != 0;
    return n;
}

static void requests(HpcosTrainerShm *t, uint32_t obj)
{
    static uint32_t tp_done, cards_done, cards_rm_done, fill_done, max_done, beans_done, bag_done, beans_set_done,
        undo_done, warp_done;
    if (t->cards_seq != cards_done) {
        cards_done = t->cards_seq;
        keep_for_undo();
        int n = 0;
        for (uint32_t c = 1; c <= kCardCount; c++) {
            if (MEM16(kCards + 2 * c) || c == t->cards_skip) continue;
            uint32_t a[1] = {kCardItem + c};
            call_guest(sub_00081A10, 1, a);
            g_given[c] = 1;
            n++;
        }
        fprintf(stderr, "[TRAINER] %d wizard cards given%s, health max %d\n", n,
                t->cards_skip ? " (Harry Potter's left out)" : "", (int16_t)MEM16(kHealthMax));
    }
    if (t->cards_remove_seq != cards_rm_done) {
        cards_rm_done = t->cards_remove_seq;
        int n = 0;
        for (int c = 1; c <= kCardCount; c++)
            if (g_given[c]) {
                if (MEM16(kCards + 2 * c)) {
                    MEM16(kCards + 2 * c) = 0;
                    n++;
                }
                g_given[c] = 0;
            }
        MEM16(kCardTotal) = (uint16_t)(MEM16(kCardTotal) > n ? MEM16(kCardTotal) - n : 0);
        fprintf(stderr, "[TRAINER] %d given cards taken back\n", n);
    }
    if (t->health_fill_seq != fill_done) {
        fill_done = t->health_fill_seq;
        keep_for_undo();
        uint32_t a[1] = {(uint32_t)(int16_t)MEM16(kHealthMax)};
        call_guest(sub_00081860, 1, a);
    }
    if (t->health_max_seq != max_done) {
        max_done = t->health_max_seq;
        keep_for_undo();
        uint32_t a[1] = {kHealthCap};
        call_guest(sub_00081890, 1, a);
        call_guest(sub_00081860, 1, a);
    }
    if (t->bag_seq != bag_done) {
        bag_done = t->bag_seq;
        keep_for_undo();
        int v = (int16_t)MEM16(kBeansMax) + kBagStep;
        if (v > 32000) v = 32000;
        MEM16(kBeansMax) = (uint16_t)v;
    }
    if (t->beans_seq != beans_done) {
        beans_done = t->beans_seq;
        keep_for_undo();
        MEM16(kBeans) = MEM16(kBeansMax);
    }
    if (t->beans_set_seq != beans_set_done) {
        beans_set_done = t->beans_set_seq;
        keep_for_undo();
        int v = t->beans_value, cap = (int16_t)MEM16(kBeansMax);
        MEM16(kBeans) = (uint16_t)(v < 0 ? 0 : v > cap ? cap : v);
    }
    if (t->undo_seq != undo_done) {
        undo_done = t->undo_seq;
        undo(t);
    }
    if (t->tp_seq != tp_done) {
        tp_done = t->tp_seq;
        float p[3] = {t->tp_pos[0], t->tp_pos[1], t->tp_pos[2]};
        if (obj) place(obj, p);
    }
    /* the selector's start choice ([0x1EF704], -1 none) is read by a level's setup (0x20000003, e.g. sub_000D5DF0:
     * Chamber of Secrets 0 Start .. 4 PolyJuice .. 7 Default); put back to none once the warped-to level has run a
     * while, so later loads follow the story again */
    static uint32_t warp_level = 0xFFFFFFFFu;
    static DWORD warp_seen;
    if (warp_level != 0xFFFFFFFFu && MEM32(0x527B78) == warp_level) {
        if (!warp_seen) warp_seen = GetTickCount();
        else if (GetTickCount() - warp_seen > 3000) {
            MEM32(kSelectorStart) = 0xFFFFFFFFu;
            warp_level = 0xFFFFFFFFu;
        }
    }
    if (t->warp_seq != warp_done) {
        warp_done = t->warp_seq;
        if (t->warp_level < HPCOS_LEVELS) {
            /* the pause menu's level selector's way (0x4F268): the level, and the start it chose */
            uint32_t a[1] = {t->warp_level};
            if (t->warp_replay) forget_level(t->warp_level);
            call_guest(sub_0004AD50, 1, a);
            MEM16(kLevelStart) = t->warp_start;
            MEM32(kSelectorStart) = t->warp_start;
            warp_level = t->warp_level, warp_seen = 0;
            fprintf(stderr, "[TRAINER] warp to level %u, start %u\n", t->warp_level, t->warp_start);
        }
    }
}

/* debug: RECOMP_GIVE_CARDS=65,46,... -- Ctrl+Shift+C asks for the next card of the list's call-out the way its pop-up
 * does (sub_000A0BE0: sub_00041A60(field 5 of the card's entry in 0x1F4218, 1.0)), from Harry's update; for testing
 * the call-outs (hle_debug.c's RECOMP_SND_TRACE). Giving the card itself (sub_00081A10) opens no pop-up */
static void give_card_key(uint32_t ent)
{
    static int cards[32], n = -1, next;
    static int was;
    if (n < 0) {
        n = 0;
        const char *e = getenv("RECOMP_GIVE_CARDS");
        while (e && *e && n < 32) {
            int c = atoi(e);
            if (c >= 1 && c <= kCardCount) cards[n++] = c;
            e = strchr(e, ',');
            if (e) e++;
        }
    }
    if (!n || ent != MEM32(kPlayer)) return;
    int down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
               (GetAsyncKeyState('C') & 0x8000);
    if (down && !was) {
        int c = cards[next++ % n];
        uint32_t a[2] = {MEM32(0x1F4218u + 20u * (uint32_t)(c - 1) + 0x10u), 0x3F800000u};
        fprintf(stderr, "[TRAINER] wizard card %d's call-out, sound %08X\n", c, a[0]);
        call_guest(sub_00041A60, 2, a);
    }
    was = down;
}

/* debug, RECOMP_HARRY_DIFF=1: [HDIFF] lines after a press of A, B, X, Y or a trigger: for 90 updates (1.5 s) every
 * dword of Harry's entity (0x400 bytes) and object (0x200) that changed since the update before, and the game's data
 * (0x1D0000 to 0x220000 and 0x500000 to 0x560000) against the update before the press 15 and 60 updates on
 * ([HDIFF-G]). What changes while no button is held (every 30 updates compared) is noise and left out of those (Harry's
 * bones, timers): to find the state a spell's cast keeps (2026-10-25: the L trigger during Alohomora moves the spell) */
static const struct { uint32_t va, n; } kHdRanges[] = {{0x1D0000u, 0x50000u / 4}, {0x500000u, 0x60000u / 4}};
#define HD_N (0x50000u / 4 + 0x60000u / 4)
static uint32_t hd_va(uint32_t k)
{
    return k < kHdRanges[0].n ? kHdRanges[0].va + 4 * k : kHdRanges[1].va + 4 * (k - kHdRanges[0].n);
}

static void harry_diff(uint32_t ent)
{
    static int on = -1, left, step, quiet;
    static uint32_t prev[0x180], held, ent0, *glo, *base;
    static uint8_t *noisy;
    static const char *const kNames[] = {"A", "B", "X", "Y", "Black", "White", "LT", "RT"};
    static int which;
    if (on < 0) {
        const char *e = getenv("RECOMP_HARRY_DIFF");
        on = e && e[0] == '1';
        if (on) glo = (uint32_t *)malloc(HD_N * 4), base = (uint32_t *)malloc(HD_N * 4), noisy = (uint8_t *)calloc(HD_N, 1);
        if (on && base) for (uint32_t k = 0; k < HD_N; k++) base[k] = MEM32(hd_va(k));
    }
    if (!on || !glo || !base || !noisy) return;
    uint32_t obj = MEM32(ent + 0xAC), down = 0;
    XBOX_GAMEPAD g = hpcos_last_pad;
    for (int i = 0; i < 8; i++)
        if (g.bAnalogButtons[i] > 30) down |= 1u << i;
    uint32_t pressed = down & ~held;
    held = down;
    /* the noise: changes over 30 updates with nothing held and no trace running */
    if (down || left) quiet = 0;
    else if (++quiet % 30 == 0) {
        for (uint32_t k = 0; k < HD_N; k++) {
            uint32_t v = MEM32(hd_va(k));
            if (v != base[k]) noisy[k] = 1, base[k] = v;
        }
    }
    static char line[16384];
    if (left) {
        step++;
        int n = snprintf(line, sizeof line, "[HDIFF] %s +%d:", kNames[which], step), any = 0;
        for (uint32_t k = 0; k < 0x180 && n < (int)sizeof line - 40; k++) {
            uint32_t v = MEM32(k < 0x100 ? ent0 + 4 * k : obj + 4 * (k - 0x100));
            if (v != prev[k]) {
                n += snprintf(line + n, sizeof line - n, " %s+%X %X->%X", k < 0x100 ? "e" : "o",
                              k < 0x100 ? 4 * k : 4 * (k - 0x100), prev[k], v);
                prev[k] = v, any = 1;
            }
        }
        if (any) fprintf(stderr, "%s\n", line);
        if (step == 15 || step == 60) {
            n = snprintf(line, sizeof line, "[HDIFF-G] %s +%d:", kNames[which], step);
            int c = 0;
            for (uint32_t k = 0; k < HD_N && c < 300; k++) {
                uint32_t v = MEM32(hd_va(k));
                if (v != glo[k] && !noisy[k]) n += snprintf(line + n, sizeof line - n, " %X:%X->%X", hd_va(k), glo[k], v), c++;
            }
            fprintf(stderr, "%s\n", line);
        }
        if (--left == 0) {
            fprintf(stderr, "[HDIFF] %s end\n", kNames[which]);
            for (uint32_t k = 0; k < HD_N; k++) base[k] = MEM32(hd_va(k));
        }
    }
    if (pressed && !left) {
        for (which = 0; !(pressed >> which & 1); which++) {}
        ent0 = ent, step = 0, left = 90;
        for (uint32_t k = 0; k < 0x180; k++) prev[k] = MEM32(k < 0x100 ? ent + 4 * k : obj + 4 * (k - 0x100));
        for (uint32_t k = 0; k < HD_N; k++) glo[k] = MEM32(hd_va(k));
        uint32_t nn = 0;
        for (uint32_t k = 0; k < HD_N; k++) nn += noisy[k];
        fprintf(stderr, "[HDIFF] %s pressed (%u noisy dwords left out)\n", kNames[which], nn);
    }
}

/* RECOMP_TRIG_TRACE=1: the level's triggers against Harry, to find the grounds' borders (2026-10-28). The geo loader
 * (sub_00046BA0) copies each level slot's trigger records (0x88 bytes in the .geo) into 0x84-byte records: the list
 * at [0x52434C + 4 slot], its count at [0x52438C + 4 slot]; +8 the record's type (.geo +0x3C), +0x1C its flags (.geo
 * +0x54). [TRIGS] dumps a list when it appears; [TRIG] logs a dword that changes after that, with Harry's position
 * (a field that keeps changing is left out after 12 changes); [TELEPORT] a jump of Harry's of more than 1.5 between
 * updates; [BLOCKED] the stick held for 0.3 s with Harry not moving; [LEVEL] the next-level request changing */
/* the collision query (sub_0002F260) walks the world 0x5276B0: 5 slots 0x84 apart, each with its flags at +0 (1 in use),
 * its objects (0x22C bytes) at [+0x48] and their count at +0x50. An object: +0x10 its position, +0x3C bit 4 its
 * inverse rotation at +0xA0 is ready, +0xF4 bit 0x100 skipped, +0x228 its descriptor (+0x40 the query mask, +0x44 bit
 * 0x100000 no collision, +0x58 / +0x64 its box's corners in its own space). The name nearest the descriptor, for the
 * log: the .geo keeps the pieces' names beside their data */
enum { kWorld = 0x5276B0, kWorldSlots = 5, kWorldSlot = 0x84, kObj = 0x22C };

static const char *obj_name(uint32_t desc, char *buf, size_t n)
{
    buf[0] = 0;
    for (uint32_t back = 0; back < 0x400 && desc > back; back++) {
        uint32_t a = desc - back, len = 0;
        while (len < 48 && isprint(MEM8(a + len))) len++;
        if (len >= 6 && MEM8(a + len) == 0 && (a == 0 || !isprint(MEM8(a - 1)))) {
            int caps = 1;
            for (uint32_t k = 0; k < len; k++)
                if (islower(MEM8(a + k))) caps = 0;
            if (!caps) continue;
            for (uint32_t k = 0; k < len && k + 1 < n; k++) buf[k] = (char)MEM8(a + k), buf[k + 1] = 0;
            return buf;
        }
    }
    return buf;
}

/* Harry's position in the object's own space: the inverse rotation when the title has it ready */
static void obj_local(uint32_t o, float x, float y, float z, float l[3])
{
    float d[3] = {x - MEMF(o + 0x10), y - MEMF(o + 0x14), z - MEMF(o + 0x18)};
    if (!(MEM8(o + 0x3C) & 4)) {
        l[0] = d[0], l[1] = d[1], l[2] = d[2];
        return;
    }
    uint32_t m = o + 0xA0;
    for (int c = 0; c < 3; c++) l[c] = d[0] * MEMF(m + 4 * c) + d[1] * MEMF(m + 0xC + 4 * c) + d[2] * MEMF(m + 0x18 + 4 * c);
}

static void world_dump(void)
{
    char nm[64];
    for (int s = 0; s < kWorldSlots; s++) {
        uint32_t w = kWorld + kWorldSlot * s, arr = MEM32(w + 0x48), n = MEM32(w + 0x50);
        if (!(MEM8(w) & 1) || !arr || n > 4096) continue;
        fprintf(stderr, "[WORLD] slot %d objects %u at %08X\n", s, n, arr);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t o = arr + i * kObj, desc = MEM32(o + 0x228);
            if (!desc) continue;
            fprintf(stderr, "[WORLD] %d#%u %08X desc %08X mask %08X flags %08X f4 %08X at %.1f %.1f %.1f box %.1f %.1f %.1f / %.1f %.1f %.1f %s\n",
                    s, i, o, desc, MEM32(desc + 0x40), MEM32(desc + 0x44), MEM32(o + 0xF4), MEMF(o + 0x10), MEMF(o + 0x14),
                    MEMF(o + 0x18), MEMF(desc + 0x58), MEMF(desc + 0x5C), MEMF(desc + 0x60), MEMF(desc + 0x64),
                    MEMF(desc + 0x68), MEMF(desc + 0x6C), obj_name(desc, nm, sizeof nm));
        }
    }
}

/* F10's "Mark this moment" (menu.cpp): what is near Harry, to find the torches' flames that vanish with distance
 * (2026-10-30): every entity within 60 of him (the list at [0x5242FC], next at +0x10: +0xC4 its class, +0xC8 its type,
 * +0xAC its object, whose +0x14 is the position and +0xF4 the hidden bits) and the level's own objects within 40 (their
 * box's centre), with their names. Two marks, beside a lit torch and where its flame is gone, tell the flame apart */
void hpcos_MarkObjects(void)
{
    uint32_t h = MEM32(MEM32(0x530408) + 0xAC);
    if (!h) return;
    float hx = MEMF(h + 0x14), hy = MEMF(h + 0x18), hz = MEMF(h + 0x1C);
    fprintf(stderr, "[NEAR] Harry at %.1f %.1f %.1f\n", hx, hy, hz);
    int n = 0, shown = 0;
    for (uint32_t e = MEM32(0x5242FC); e && n < 4096 && shown < 200; e = MEM32(e + 0x10), n++) {
        uint32_t o = MEM32(e + 0xAC);
        if (!o) continue;
        float dx = MEMF(o + 0x14) - hx, dy = MEMF(o + 0x18) - hy, dz = MEMF(o + 0x1C) - hz, d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (!(d < 60.0f)) continue;
        fprintf(stderr, "[NEAR] entity %08X class %08X type %08X at %.1f %.1f %.1f (%.1f away) +84 %08X obj f4 %08X\n", e,
                MEM32(e + 0xC4), MEM32(e + 0xC8), MEMF(o + 0x14), MEMF(o + 0x18), MEMF(o + 0x1C), d, MEM32(e + 0x84),
                MEM32(o + 0xF4));
        shown++;
    }
    char nm[64];
    uint32_t arr = MEM32(kWorld + 0x48), cnt = MEM32(kWorld + 0x50);
    if (!(MEM8(kWorld) & 1) || !arr || cnt > 4096) return;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t o = arr + i * kObj, desc = MEM32(o + 0x228);
        if (!desc) continue;
        float c[3];
        for (int k = 0; k < 3; k++) c[k] = MEMF(o + 0x10 + 4 * k) + (MEMF(desc + 0x58 + 4 * k) + MEMF(desc + 0x64 + 4 * k)) * 0.5f;
        float dx = c[0] - hx, dy = c[1] - hy, dz = c[2] - hz, d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (!(d < 40.0f)) continue;
        fprintf(stderr, "[NEAR] object #%u at %.1f %.1f %.1f (%.1f away) flags %08X f4 %08X %s\n", i, c[0], c[1], c[2], d,
                MEM32(desc + 0x44), MEM32(o + 0xF4), obj_name(desc, nm, sizeof nm));
    }
}

/* the colliding objects whose box Harry is within 1.0 of */
static void world_near(float x, float y, float z)
{
    char nm[64];
    for (int s = 0; s < kWorldSlots; s++) {
        uint32_t w = kWorld + kWorldSlot * s, arr = MEM32(w + 0x48), n = MEM32(w + 0x50);
        if (!(MEM8(w) & 1) || !arr || n > 4096) continue;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t o = arr + i * kObj, desc = MEM32(o + 0x228);
            if (!desc || (MEM32(desc + 0x44) & 0x100000) || (MEM32(o + 0xF4) & 0x100)) continue;
            float l[3];
            obj_local(o, x, y, z, l);
            int in = 1;
            for (int c = 0; c < 3; c++)
                if (l[c] < MEMF(desc + 0x58 + 4 * c) - 1.0f || l[c] > MEMF(desc + 0x64 + 4 * c) + 1.0f) in = 0;
            if (in)
                fprintf(stderr, "[BLOCKED]   by? %d#%u desc %08X mask %08X flags %08X local %.1f %.1f %.1f %s\n", s, i, desc,
                        MEM32(desc + 0x40), MEM32(desc + 0x44), l[0], l[1], l[2], obj_name(desc, nm, sizeof nm));
        }
    }
}

/* the collision query, sub_0002F260(world, mask, query), reports each object it ran into (tools/patch_gen.py: the
 * object, and the triangle strip's record whose +4 the query keeps at +0x6C). A query from within 2.0 of Harry is kept
 * with the time for [BLOCKED] (RECOMP_TRIG_TRACE), to find the grounds' border walls */
static uint32_t g_hit_obj[8], g_hit_strip[8];
static DWORD g_hit_at[8];
void hpcos_CollisionHit(uint32_t q, uint32_t o, uint32_t strip)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_TRIG_TRACE");
        on = e && e[0] == '1';
    }
    uint32_t ent = MEM32(kPlayer), obj = ent ? MEM32(ent + 0xAC) : 0;
    if (on != 1 || !q || !obj) return;
    float dx = MEMF(q) - MEMF(obj + 0x14), dy = MEMF(q + 4) - MEMF(obj + 0x18), dz = MEMF(q + 8) - MEMF(obj + 0x1C);
    if (dx * dx + dy * dy + dz * dz > 4.0f) return;
    int slot = (int)((o >> 3) & 7);
    for (int j = 0; j < 8; j++)
        if (g_hit_obj[j] == o) slot = j;
    g_hit_obj[slot] = o, g_hit_strip[slot] = strip, g_hit_at[slot] = GetTickCount();
}

/* the world's 5 level slots whenever one changes ([SLOTS]): per slot its flags (+0), level data (+0x44), rooms
 * ([+0x44]+0x28), objects (+0x50); to see whether a streamed level (the staircase's landings) takes a free slot or
 * pushes another out */
static void world_slots(float x, float y, float z)
{
    static uint32_t prev[kWorldSlots][3];
    int changed = 0;
    uint32_t cur[kWorldSlots][3];
    for (int s = 0; s < kWorldSlots; s++) {
        uint32_t w = kWorld + kWorldSlot * s, geo = MEM32(w + 0x44);
        cur[s][0] = MEM32(w), cur[s][1] = geo, cur[s][2] = MEM32(w + 0x50);
        if (memcmp(cur[s], prev[s], sizeof cur[s])) changed = 1;
    }
    if (!changed) return;
    memcpy(prev, cur, sizeof prev);
    char line[512];
    int k = snprintf(line, sizeof line, "[SLOTS] Harry %.1f %.1f %.1f:", x, y, z);
    for (int s = 0; s < kWorldSlots; s++)
        k += snprintf(line + k, sizeof line - k, " %d=%X/%08X rooms %u objs %u", s, cur[s][0], cur[s][1],
                      cur[s][1] ? MEM32(cur[s][1] + 0x28) : 0, cur[s][2]);
    fprintf(stderr, "%s\n", line);
}

/* the world's objects hidden (+0xF4 bit 0x100: not drawn, no collision) whenever that set changes */
static void world_hidden(void)
{
    static uint32_t prev_sum = 1;
    uint32_t w = kWorld, arr = MEM32(w + 0x48), n = MEM32(w + 0x50), sum = 0;
    if (!(MEM8(w) & 1) || !arr || n > 4096) return;
    for (uint32_t i = 0; i < n; i++)
        if (MEM32(arr + i * kObj + 0xF4) & 0x100) sum = sum * 31 + i + 1;
    if (sum == prev_sum) return;
    prev_sum = sum;
    char line[8192];
    int k = snprintf(line, sizeof line, "[HIDDEN] of %u:", n);
    for (uint32_t i = 0; i < n && k < (int)sizeof line - 16; i++)
        if (MEM32(arr + i * kObj + 0xF4) & 0x100) k += snprintf(line + k, sizeof line - k, " %u", i);
    fprintf(stderr, "%s\n", line);
}

static void trig_trace(uint32_t ent)
{
    enum { kSlots = 16, kMax = 2048 };
    static int on = -1;
    static uint32_t list[kSlots], count[kSlots], *snap[kSlots];
    static uint8_t *changes[kSlots];
    static float px, py, pz;
    static DWORD stuck_since, stuck_logged;
    static uint32_t level_req;
    if (on < 0) {
        const char *e = getenv("RECOMP_TRIG_TRACE");
        on = e && e[0] == '1';
    }
    if (!on) return;
    uint32_t obj = MEM32(ent + 0xAC);
    if (!obj) return;
    /* a hardware watch on his x and y (main.c): the code that warps him logs its call sites ([WATCH] jump). Only with
     * RECOMP_WATCH_HARRY=1: the game froze flying the broom over the grounds with it on, and the stall report then
     * never came (2026-10-28) */
    static uint32_t watched;
    static int watch = -1;
    if (watch < 0) {
        const char *e = getenv("RECOMP_WATCH_HARRY");
        watch = e && e[0] == '1';
    }
    if (watch && obj != watched) {
        extern void hpcos_WatchJumps(uint32_t va);
        watched = obj;
        hpcos_WatchJumps(obj + 0x14);
    }
    float x = MEMF(obj + 0x14), y = MEMF(obj + 0x18), z = MEMF(obj + 0x1C);
    for (int s = 0; s < kSlots; s++) {
        uint32_t l = MEM32(0x52434C + 4 * s), n = MEM32(0x52438C + 4 * s);
        if (n > kMax) n = 0;
        if (l != list[s] || n != count[s]) {
            free(snap[s]), free(changes[s]);
            snap[s] = NULL, changes[s] = NULL;
            list[s] = l, count[s] = n;
            if (!l || !n) continue;
            snap[s] = (uint32_t *)malloc(n * 0x84), changes[s] = (uint8_t *)calloc(n * 0x21, 1);
            if (!snap[s] || !changes[s]) { count[s] = 0; continue; }
            fprintf(stderr, "[TRIGS] slot %d list %08X count %u (Harry %.2f %.2f %.2f)\n", s, l, n, x, y, z);
            if (s == 0) world_dump();
            for (uint32_t i = 0; i < n; i++) {
                char line[0x21 * 9 + 64];
                int k = snprintf(line, sizeof line, "[TRIGS] %d#%u", s, i);
                for (uint32_t o = 0; o < 0x84; o += 4) {
                    snap[s][i * 0x21 + o / 4] = MEM32(l + i * 0x84 + o);
                    k += snprintf(line + k, sizeof line - k, " %08X", snap[s][i * 0x21 + o / 4]);
                }
                fprintf(stderr, "%s\n", line);
            }
            continue;
        }
        if (!snap[s]) continue;
        for (uint32_t i = 0; i < n; i++)
            for (uint32_t o = 0; o < 0x84; o += 4) {
                uint32_t k = i * 0x21 + o / 4, v = MEM32(l + i * 0x84 + o);
                if (v == snap[s][k]) continue;
                if (changes[s][k] < 12)
                    fprintf(stderr, "[TRIG] %d#%u type %08X +%X %08X->%08X (Harry %.2f %.2f %.2f)%s\n", s, i,
                            MEM32(l + i * 0x84 + 8), o, snap[s][k], v, x, y, z,
                            changes[s][k] == 11 ? " (left out from now)" : "");
                if (changes[s][k] < 255) changes[s][k]++;
                snap[s][k] = v;
            }
    }
    world_hidden();
    world_slots(x, y, z);
    float d = sqrtf((x - px) * (x - px) + (y - py) * (y - py) + (z - pz) * (z - pz));
    if (d > 1.5f) fprintf(stderr, "[TELEPORT] %.2f %.2f %.2f -> %.2f %.2f %.2f\n", px, py, pz, x, y, z);
    XBOX_GAMEPAD g = hpcos_last_pad;
    int stick = abs(g.sThumbLX) > 12000 || abs(g.sThumbLY) > 12000;
    DWORD now = GetTickCount();
    if (!stick || d > 0.01f) stuck_since = 0;
    else if (!stuck_since) stuck_since = now;
    else if (now - stuck_since > 300 && now - stuck_logged > 1000) {
        stuck_logged = now;
        fprintf(stderr, "[BLOCKED] Harry %.2f %.2f %.2f yaw %.2f stick %d %d\n", x, y, z, MEMF(obj + 0x3C), g.sThumbLX,
                g.sThumbLY);
        world_near(x, y, z);
        for (int j = 0; j < 8; j++)
            if (g_hit_obj[j] && now - g_hit_at[j] < 500) {
                uint32_t o = g_hit_obj[j], desc = MEM32(o + 0x228), arr = MEM32(kWorld + 0x48);
                fprintf(stderr, "[BLOCKED]   hit 0#%d obj %08X desc %08X flags %08X f4 %08X strip %08X id %08X at %.1f %.1f %.1f box %.1f %.1f %.1f / %.1f %.1f %.1f\n",
                        o >= arr ? (int)((o - arr) / kObj) : -1, o, desc, MEM32(desc + 0x44), MEM32(o + 0xF4), g_hit_strip[j],
                        MEM32(g_hit_strip[j] + 4), MEMF(o + 0x10), MEMF(o + 0x14), MEMF(o + 0x18), MEMF(desc + 0x58),
                        MEMF(desc + 0x5C), MEMF(desc + 0x60), MEMF(desc + 0x64), MEMF(desc + 0x68), MEMF(desc + 0x6C));
            }
    }
    px = x, py = y, pz = z;
    if (MEM32(kLevelReq) != level_req) {
        fprintf(stderr, "[LEVEL] request %08X -> %08X start %08X (Harry %.2f %.2f %.2f)\n", level_req,
                MEM32(kLevelReq), MEM32(kLevelStart), x, y, z);
        level_req = MEM32(kLevelReq);
    }
}

/* The flying lesson with the grounds' path warps off. The flying practice door (Harry about (13.7, 40, 70.5)) is a
 * warp to destination 2 of table 0x20AC28: no start point (-1), so sub_000E64E0 only calls its callback sub_000E65F0
 * -> sub_000DF810, which, when the lesson is due (an entity typed 0x5200012C), puts Harry at start 0x68 on the field,
 * spawns class 0x50000208 there and switches the area (0xDF92C); hooch.geo and the lesson's cutscene (cs_dy101_05,
 * Ron and the class) follow. The door's warp fires within a radius of a point in front of the arch, which can be
 * walked round with the borders gone; with the path warps off it stays off (the patched zone and door, patch_gen.py,
 * sub_000E6860 and sub_000E6760, ask here) and lesson_arch starts the same warp, sub_000E64B0(2), when Harry steps into
 * the arch between its torches (objects #186 and #187 at x 14.3 and 21.8, z 64, F12 capture 2026-10-29) while the
 * lesson is due (its day, until cs_dy101_11: lesson_arch). (Destination 3, start 0x67, is the Quidditch stadium; placing Hooch while walking found no markers.)
 * The lesson's area switch takes area 9 (no room left out) instead of the broom's 0xD, which hides room 0xFA; when
 * the lesson is not due there is no switch, so that lasts 3 s. Every warp
 * passed over is logged with where it would have gone (a start point as sub_000468F0 finds it: 16 level slots'
 * tables at [0x524410 + 4i], count [0x524450 + 4i], 0x2C-byte records, mask +4 (1 << [0x527B7C]), id +0xC, place +0x14) */
extern int hpcos_GroundsWarps(void);
static DWORD g_lesson_at;
static int warp_start_at(uint32_t start, float *x, float *y, float *z)
{
    uint32_t mask = 1u << (MEM32(0x527B7C) & 31);
    for (uint32_t slot = 0; slot < 16; slot++) {
        uint32_t rec = MEM32(0x524410 + slot * 4);
        int n = (int)MEM32(0x524450 + slot * 4);
        for (int k = 0; rec && k < n; k++, rec += 0x2C)
            if ((MEM32(rec + 4) & mask) && MEM32(rec + 0xC) == start) {
                *x = MEMF(rec + 0x14), *y = MEMF(rec + 0x18), *z = MEMF(rec + 0x1C);
                return 1;
            }
    }
    return 0;
}

static int night_grounds(void);
/* At night the flying practice field keeps its way back (the user asked, 2026-10-30): the night arch takes Harry there
 * (lesson_arch, destination 5), and the field's own exit, destination 8 (start 0x71, the courtyard at 8.7 40 74.4),
 * stays on with the path warps off; every other warp and the day's field exits stay off */
static int night_field_exit(uint32_t dest) { return dest == 8 && night_grounds(); }

/* the prompts of warp zones and warp objects (tools/patch_gen.py): shown when the warp they name is on */
int hpcos_GroundsPromptOpen(uint32_t dest) { return hpcos_GroundsWarps() || night_field_exit(dest); }

int hpcos_GroundsWarpAllowed(uint32_t dest)
{
    static uint32_t last_dest = ~0u;
    static DWORD last_at;
    if (hpcos_GroundsWarps()) return 1;
    if (night_field_exit(dest)) {
        fprintf(stderr, "[GROUNDS] the night field's exit (destination 8): back to the courtyard\n");
        return 1;
    }
    int32_t start = dest <= 8 ? (int32_t)MEM32(0x20AC28 + dest * 8) : -2;  /* -2: past the table's 9 entries */
    int lesson = start == -1 && MEM32(0x20AC28 + dest * 8 + 4) == 0x000E65F0u;
    float x = 0, y = 0, z = 0;
    int found = start >= 0 && warp_start_at((uint32_t)start, &x, &y, &z);
    DWORD now = GetTickCount();
    if (dest != last_dest || now - last_at > 2000) {
        uint32_t h = MEM32(MEM32(0x530408) + 0xAC);
        fprintf(stderr, "[GROUNDS] warp passed over: destination %u start %X%s at %.1f %.1f %.1f (Harry %.1f %.1f %.1f)%s\n",
                dest, (unsigned)start, found ? "" : " (no start point)", x, y, z, h ? MEMF(h + 0x14) : 0.f,
                h ? MEMF(h + 0x18) : 0.f, h ? MEMF(h + 0x1C) : 0.f, lesson ? ": the flying lesson's (its arch starts it)" : "");
        last_dest = dest, last_at = now;
    }
    return 0;  /* the lesson's warp is started by the arch between the torches instead (lesson_arch) */
}

uint32_t hpcos_GroundsArea(void)
{
    int lesson = g_lesson_at && GetTickCount() - g_lesson_at < 3000;
    g_lesson_at = 0;
    return lesson ? 9 : 0xD;
}

static int on_grounds(void)
{
    uint32_t arr = MEM32(kWorld + 0x48), n = MEM32(kWorld + 0x50);
    return (MEM8(kWorld) & 1) && arr && n == 266 && MEM32(arr + 247 * kObj + 0x228) &&
           MEMF(MEM32(arr + 247 * kObj + 0x228) + 0x68) > 600.0f;
}

/* the flying lesson's last cutscene opened (hle_shots.c's .geo path hook): the arch is done for this run */
static int g_lesson_done;
void hpcos_GeoOpened(const char *name)
{
    if (_stricmp(name, "cs_dy101_11.geo") || g_lesson_done) return;
    g_lesson_done = 1;
    fprintf(stderr, "[GROUNDS] flying lesson over (cs_dy101_11, day %u): its arch is off\n", MEM32(0x527B7C));
}

extern void sub_000E64B0(void);
/* At night (hogwarts_night.geo) the same arch takes Harry to the flying practice field whatever the day, so the field
 * can still be reached with the path warps off (the user asked, 2026-10-30): destination 5, start 0x68 with no
 * callback (table 0x20AC28), the night's own path warp beside the arch (the [GROUNDS] warp passed over lines) */
static void lesson_arch(void)
{
    static int inside;
    int night = night_grounds();
    if (hpcos_GroundsWarps() || !(on_grounds() || night)) {
        inside = 0;
        return;
    }
    uint32_t h = MEM32(MEM32(0x530408) + 0xAC);
    if (!h) return;
    float x = MEMF(h + 0x14), y = MEMF(h + 0x18), z = MEMF(h + 0x1C);
    int in = x > 14.3f && x < 21.8f && z > 61.0f && z < 66.5f && y > 37.0f && y < 45.0f;
    if (!in || inside) {
        inside = in;
        return;
    }
    inside = 1;
    if (night) {
        fprintf(stderr, "[GROUNDS] Harry in the flying practice arch at night (%.1f %.1f %.1f): to the field%s\n", x, y, z,
                MEM32(0x546120) ? " (a warp already running)" : "");
        if (MEM32(0x546120)) return;
        uint32_t dest = 5;
        call_guest(sub_000E64B0, 1, &dest);
        return;
    }
    /* due: what sub_000DF810 looks for (an entity typed 0x5200012C with +0x84 clear), on the lesson's day ([0x527B7C]
     * 2, the day the lesson ran on, 2026-10-29) and until its last cutscene (cs_dy101_11) has been opened this run.
     * After the lesson the warp is still there and only bounced Harry to the field and back: the practice's controller
     * (class 0x50000208, sub_000E23E0) ends at once with no rings, and the rings (type 0x52000075) only come with the
     * practice, so they cannot tell the lesson is due */
    int spot = 0, n = 0;
    for (uint32_t e = MEM32(0x5242FC); e && n < 4096; e = MEM32(e + 0x10), n++)
        spot |= MEM32(e + 0xC8) == 0x5200012C && !MEM32(e + 0x84);
    uint32_t day = MEM32(0x527B7C);
    int due = spot && day == 2 && !g_lesson_done;
    fprintf(stderr, "[GROUNDS] Harry in the flying practice arch (%.1f %.1f %.1f): lesson %s (spot %d, day %u%s)\n", x,
            y, z, due ? "due, its warp started" : "not due", spot, day, g_lesson_done ? ", lesson done" : "");
    if (!due || MEM32(0x546120)) return;
    uint32_t dest = 2;
    g_lesson_at = GetTickCount() | 1;
    call_guest(sub_000E64B0, 1, &dest);
}

/* The broom's landing (sub_00050300, from the update sub_00053330): in a landing zone (the zone flags under Harry,
 * [0x53030C], bit 0x01000000, found by sub_0005B3E0) the land button (action 0x1004) dismounts him (state 0x76,
 * dismount_broom). Only the game's landing zones. With F10's broom option (hpcos_BroomLandFlatOn), while he is on the
 * broom ([0x5302A0] bit 4), a flat surface up to 3 under him is one too: a ray from 0.5 over to 3 under his place
 * (sub_0002D1D0 into a result on the guest stack, the collision query sub_0002F260 fills it: hit height +0x2C, the
 * strip's id +0x6C) and the same 0.75 to the side and 0.75 ahead within 0.2 of its height. (2026-10-29: the first try
 * patched sub_00055E30, which is the grab states' test of the floor ahead of a pushed block, not the broom's) */
extern int hpcos_BroomLandFlatOn(void);
extern void sub_0002D1D0(void);
extern void sub_0002F260(void);
static int ground_at(float x, float lo_y, float hi_y, float z, float *hit)
{
    uint32_t save = g_esp, res = save - 0x100;
    g_esp = save - 0x180;  /* the result (0x80 bytes) lives on the guest stack above the calls' arguments */
    union { float f; uint32_t u; } a = {x}, lo = {lo_y}, hi = {hi_y}, c = {z};
    uint32_t seg[8] = {a.u, lo.u, c.u, a.u, hi.u, c.u, res, 0};
    call_guest(sub_0002D1D0, 8, seg);
    uint32_t q[3] = {kWorld, 1u << (MEM32(0x527B7C) & 31), res};
    int ok = (call_guest(sub_0002F260, 3, q) & 0xFF) != 0;
    if (ok) *hit = MEMF(res + 0x2C);
    g_esp = save;
    return ok;
}

static int lake_at(float x, float z, float g);
int hpcos_BroomLandHere(void)
{
    if (!hpcos_BroomLandFlatOn() || !(MEM8(0x5302A0) & 4)) return 0;
    uint32_t h = MEM32(MEM32(0x530408) + 0xAC);
    if (!h) return 0;
    float x = MEMF(h + 0x14), y = MEMF(h + 0x18), z = MEMF(h + 0x1C), g, g1, g2;
    if (!ground_at(x, y - 3.0f, y + 0.5f, z, &g) || lake_at(x, z, g)) return 0;  /* not on the lake */
    int flat = ground_at(x + 0.75f, g - 0.5f, g + 0.5f, z, &g1) && ground_at(x, g - 0.5f, g + 0.5f, z + 0.75f, &g2) &&
               fabsf(g1 - g) < 0.2f && fabsf(g2 - g) < 0.2f;
    static DWORD last;
    static int last_flat = -1;
    DWORD now = GetTickCount();
    if (flat != last_flat || now - last > 5000) {
        fprintf(stderr, "[BROOM] over %s at %.1f %.1f %.1f, %.1f above it%s\n", flat ? "a flat surface" : "a slope",
                x, g, z, y - g, flat ? ": the land button lands" : "");
        last = now, last_flat = flat;
    }
    /* as in a landing zone: the zone flags under Harry say so, which the HUD reads to light the land button
     * (sub_000AD2E0 greys it on the broom without the bit); sub_0005B3E0 sets them anew each frame */
    if (flat) MEM32(0x53030C) |= 0x01000000;
    return flat;
}

/* The lake's edge while exploring the grounds (the path warps off). The lake is grounds object #70, a slab whose top
 * (desc +0x68, y 8) is walkable, x 4.6..179.9 and z -264.1..-58.9 (2026-10-29, F12 capture of Harry standing on it).
 * Walking (not on the broom, [0x5302A0] bit 4), a step that takes Harry over it (the first thing under him, ground_at
 * to 30 under his feet, is the lake: lake_at) is undone, so its edge is a wall: he never leaves the pier or the shore
 * (the first try put him back only once he stood on the water, and a step off the pier left him falling in the air
 * for ever, 2026-10-29). Further than 3 from his last place off it is left alone. The broom option does not land him
 * on it either */
/* the lake's surface at x, z, height g (the ground found there): inside grounds object #70's box, g within 0.1 of its top */
static int lake_at(float x, float z, float g)
{
    if (!on_grounds()) return 0;
    uint32_t lake = MEM32(kWorld + 0x48) + 70 * kObj, desc = MEM32(lake + 0x228);
    if (!desc) return 0;
    float x0 = MEMF(lake + 0x10) + MEMF(desc + 0x58), x1 = MEMF(lake + 0x10) + MEMF(desc + 0x64);
    float z0 = MEMF(lake + 0x18) + MEMF(desc + 0x60), z1 = MEMF(lake + 0x18) + MEMF(desc + 0x6C);
    float top = MEMF(lake + 0x14) + MEMF(desc + 0x68);
    if (x1 - x0 < 100.0f || z1 - z0 < 100.0f) return 0;  /* not the lake: the grounds' objects are not as loaded */
    return x > x0 && x < x1 && z > z0 && z < z1 && fabsf(g - top) < 0.1f;
}

static void lake_edge(void)
{
    static float sx, sy, sz;
    static int have;
    if (hpcos_GroundsWarps() || !on_grounds() || (MEM8(0x5302A0) & 4)) {
        have = 0;
        return;
    }
    uint32_t h = MEM32(MEM32(0x530408) + 0xAC);
    if (!h) return;
    float x = MEMF(h + 0x14), y = MEMF(h + 0x18), z = MEMF(h + 0x1C), g;
    /* over the water: the first thing straight under him is the lake (a ray to 30 under his feet) */
    int over = ground_at(x, y - 30.0f, y + 0.5f, z, &g) && lake_at(x, z, g);
    if (!over) {
        sx = x, sy = y, sz = z, have = 1;
        return;
    }
    float dx = x - sx, dz = z - sz;
    if (!have || dx * dx + dz * dz > 3.0f * 3.0f) return;  /* not a step over the edge: left as the game has it */
    MEMF(h + 0x14) = sx, MEMF(h + 0x18) = sy, MEMF(h + 0x1C) = sz;
    static DWORD last;
    if (GetTickCount() - last > 2000) {
        fprintf(stderr, "[GROUNDS] the lake's edge: Harry kept on the shore at %.1f %.1f %.1f\n", sx, sy, sz);
        last = GetTickCount();
    }
}

/* The grounds' borders with the path warps off. Flying the broom, the level's script hides three invisible pieces
 * (object+0xF4 bit 0x100: not drawn, no collision) that walking keeps (the trigger trace's [HIDDEN] lines, 2026-10-28):
 *   #166  a thin slab at y 39.5..40.5 over the courtyard and the paths' ends (the low walls along the paths)
 *   #191  a tall box over the west side, y 9..73
 *   #247  a huge one, y -67..637, round the grounds
 * Only #166's collision is taken away now: #191 and #247 are the grounds' outer limits and stay (2026-10-30). Here its
 * collision alone is taken away (its descriptor's +0x44 bit 0x100000, which the collision query
 * sub_0002F260 passes over), every 30 of Harry's updates while the warps are off, and given back with them. The
 * grounds are known by their 266 objects and #247's height.
 * At night the grounds are another level (hogwarts_night.geo, 307 objects, built differently: none of the day's three
 * boxes is in it). There #176 held Harry at the entrance (box -7.8 0 -0.5 / 16.3 40.5 87.1: the day's entrance slab
 * #166's place and top, reaching further south; the [BLOCKED] report, 2026-10-30); it is known by that box */
static int night_grounds(void)
{
    uint32_t arr = MEM32(kWorld + 0x48), n = MEM32(kWorld + 0x50), desc;
    if (!(MEM8(kWorld) & 1) || !arr || n != 307 || !(desc = MEM32(arr + 176 * kObj + 0x228))) return 0;
    return fabsf(MEMF(desc + 0x58) + 7.8f) < 0.1f && fabsf(MEMF(desc + 0x68) - 40.5f) < 0.1f &&
           fabsf(MEMF(desc + 0x6C) - 87.1f) < 0.1f;
}

/* the HUD's broom button (sub_000AD2E0, action 0x1004) at night: unavailable on foot on the night grounds (the user,
 * 2026-10-30); day and night are both level 0x1A, so the game's own test lit it at night as in the day */
int hpcos_NightGrounds(void) { return night_grounds(); }

static void grounds_borders(void)
{
    /* the entrance's piece alone: the outer limits (#191, #247) stay solid, as the user asked (2026-10-30) */
    static const uint32_t kDay[] = {166}, kNight[] = {176};
    static uint32_t done_arr, tick;
    static const uint32_t *done_list;
    static size_t done_n;
    if (++tick % 30) return;
    uint32_t arr = MEM32(kWorld + 0x48);
    const uint32_t *list = NULL;
    size_t nl = 0;
    if (on_grounds()) list = kDay, nl = sizeof kDay / sizeof kDay[0];
    else if (night_grounds()) list = kNight, nl = sizeof kNight / sizeof kNight[0];
    int off = list && !hpcos_GroundsWarps();
    if (off && done_arr != arr) {
        for (size_t i = 0; i < nl; i++) MEM32(MEM32(arr + list[i] * kObj + 0x228) + 0x44) |= 0x100000;
        done_arr = arr, done_list = list, done_n = nl;
        fprintf(stderr, "[GROUNDS] borders' collision off (%s grounds, %zu object(s) from #%u; path warps off)\n",
                list == kNight ? "night" : "day", nl, list[0]);
    } else if (!off && done_arr && done_arr == arr && list) {
        for (size_t i = 0; i < done_n; i++) MEM32(MEM32(arr + done_list[i] * kObj + 0x228) + 0x44) &= ~0x100000u;
        done_arr = 0;
        fprintf(stderr, "[GROUNDS] borders' collision back (path warps on)\n");
    } else if (!list) {
        done_arr = 0;
    }
}

void sub_00053710(void)
{
    HpcosTrainerShm *t = shm();
    uint32_t ent = ARG(0);
    give_card_key(ent);
    if (ent && ent == MEM32(kPlayer)) hpcos_WideAspectTick(), harry_diff(ent), trig_trace(ent), grounds_borders(), lesson_arch(), lake_edge();
    if (!t || !ent || ent != MEM32(kPlayer)) {
        sub_00053710_gen();
        return;
    }
    static uint32_t seen;
    if (seen != ent) {
        seen = ent;
        fprintf(stderr, "[TRAINER] Harry's update: entity %08X object %08X\n", ent, MEM32(ent + 0xAC));
    }
    uint32_t obj = MEM32(ent + 0xAC);

    if (t->god && (int16_t)MEM16(kHealthMax) > 0) MEM16(kHealth) = MEM16(kHealthMax);
    if (t->all_spells && (MEM32(kSpells) & HPCOS_ALL_SPELLS) != HPCOS_ALL_SPELLS) {
        keep_for_undo();
        MEM32(kSpells) |= HPCOS_ALL_SPELLS;
    }
    requests(t, obj);

    int noclip = t->noclip && obj;
    if (noclip) {
        fly(obj, t);
        g_esp += 4;  /* the update's ret */
    } else {
        sub_00053710_gen();
    }

    t->noclip_on = (uint8_t)noclip;
    t->player = ent;
    if (obj) {
        t->pos[0] = MEMF(obj + 0x14), t->pos[1] = MEMF(obj + 0x18), t->pos[2] = MEMF(obj + 0x1C);
        t->yaw = MEMF(obj + 0x3C);
    }
    t->health = (int16_t)MEM16(kHealth), t->health_max = (int16_t)MEM16(kHealthMax);
    t->spells = MEM32(kSpells);
    t->cards = (uint16_t)cards_held();
    uint16_t given = 0;
    for (int c = 1; c <= kCardCount; c++) given += g_given[c];
    t->cards_given = given;
    t->undo_ready = (uint8_t)g_undo.kept;
    t->beans = (int16_t)MEM16(kBeans), t->beans_max = (int16_t)MEM16(kBeansMax);
    t->level = (uint16_t)MEM32(kProgress);
    t->frame++;
}

static void write_state(const char *path, HpcosTrainerShm *t)
{
    FILE *f = fopen(path, "w");
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "time %04u-%02u-%02u %02u:%02u:%02u, %.1f s since boot\n", st.wYear, st.wMonth, st.wDay, st.wHour,
            st.wMinute, st.wSecond, GetTickCount64() / 1000.0);
    fprintf(f, "guest -> host offset %+lld\n", (long long)g_xbox_mem_offset);
    fprintf(f, "Harry's updates %u (last player %08X)%s\n", t->frame, t->player, t->noclip_on ? ", noclip" : "");
    uint32_t pl = MEM32(kPlayer);
    fprintf(f, "player entity [0x530408] %08X", pl);
    if (pl) {
        uint32_t obj = MEM32(pl + 0xAC);
        fprintf(f, " object %08X", obj);
        if (obj)
            fprintf(f, " position %.3f %.3f %.3f yaw %.3f", MEMF(obj + 0x14), MEMF(obj + 0x18), MEMF(obj + 0x1C),
                    MEMF(obj + 0x3C));
    }
    fprintf(f, "\nlevel index [0x527B78] %u, next level [0x527B94] %d start [0x527DFC] %d\n", MEM32(kProgress),
            (int16_t)MEM16(kLevelReq), (int16_t)MEM16(kLevelStart));
    fprintf(f, "player state [0x530298] %d flags [0x5302A0] %08X [0x5302A4] %08X\n", (int16_t)MEM16(0x530298),
            MEM32(0x5302A0), MEM32(0x5302A4));
    fprintf(f, "health %d / %d, beans %d / %d, cards %d (total word %u, trainer gave %u), spells %08X\n",
            (int16_t)MEM16(kHealth), (int16_t)MEM16(kHealthMax), (int16_t)MEM16(kBeans), (int16_t)MEM16(kBeansMax),
            cards_held(), MEM16(kCardTotal), t->cards_given, MEM32(kSpells));
    fprintf(f, "stick after deadzone %.3f %.3f\n", MEMF(kStickX), MEMF(kStickY));
    XBOX_GAMEPAD g = hpcos_last_pad;
    fprintf(f, "pad buttons %04X analog", g.wButtons);
    for (int i = 0; i < 8; i++) fprintf(f, " %u", g.bAnalogButtons[i]);
    fprintf(f, " sticks %d %d %d %d\n", g.sThumbLX, g.sThumbLY, g.sThumbRX, g.sThumbRY);
    fprintf(f, "trainer: noclip %u god %u all spells %u undo kept %d\n", t->noclip, t->god, t->all_spells,
            g_undo.kept);
    fclose(f);
}

static DWORD WINAPI report_thread(LPVOID arg)
{
    HpcosTrainerShm *t = (HpcosTrainerShm *)arg;
    for (;; Sleep(100)) {
        uint32_t seq = t->report_seq;
        if (seq == t->report_done) continue;
        char dir[260], path[300];
        memcpy(dir, (const char *)t->report_dir, sizeof dir);
        dir[sizeof dir - 1] = 0;
        hpcos_RequestCapture();
        snprintf(path, sizeof path, "%s\\stacks.txt", dir);
        hpcos_StackDumpTo(path);
        snprintf(path, sizeof path, "%s\\game_state.txt", dir);
        write_state(path, t);
        fprintf(stderr, "[TRAINER] debug report written to %s\n", dir);
        t->report_done = seq;
    }
    return 0;
}

/* at boot, so the trainer finds the block before Harry exists */
void hpcos_TrainerInit(void)
{
    HpcosTrainerShm *t = shm();
    if (!t) return;
    fprintf(stderr, "[TRAINER] shared block %s ready\n", HPCOS_TRAINER_NAME);
    CloseHandle(CreateThread(NULL, 0, report_thread, t, 0, NULL));
}
