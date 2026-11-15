/**
 * "Quit To Desktop" on the title menu and the pause menu: on a PC the desktop stands in for the dashboard.
 *
 * The front end is the level handler sub_000FFE60(message); 0x21000004 with [0x527B9E] == 4 is its frame, and
 * sub-screen [0x527B9C] == 4 the title menu (0x1005A0, confirm at 0x1018EE): New Game, Continue (only with a save,
 * bit 0x10 of [0x547030]) and Options, rows 0..2 in [0x546F7C], drawn on the font 0x5471F8 at x 0x140 and y 0x118 +
 * 0x26 a row (0x12C and 0x152 without a save). A fourth row below them, "Quit To Desktop" (the
 * title's damaged-save screen has "Quit To Dashboard", text 0x1091), leaves the way that screen does:
 * XLaunchNewImage(NULL, NULL) (sub_0010CB5A) -> HalReturnToFirmware, which ends the process.
 *
 * The title's rows move up half a row (sub_00033D40, the text draw, while the menu runs) so the four sit where the
 * three did, clear of the copyright line. While the fourth row is chosen the title sees no up, down or confirm
 * press and no highlighted row; down from Options picks it, up goes back. Confirming it asks first: the rows give
 * way to "Quit To Desktop" / "Are you sure?" (the pause box's words) over two rows of their own, Yes and No, lit as
 * the title lights its rows; up and down choose (No first), A or START takes the lit one, B is No.
 *
 * The pause menu (page [0x52FF40] == 0) is input sub_0004E540 and drawing sub_0004E9B0: rows Resume Game, Quit Game,
 * Options (+ Level Selector with bit 0x200 of [0x527F26], + the cheat row with 0x800), the chosen one in [0x52FF48],
 * drawn on the font 0x540128 at x 0x140, y 0xD7 + 0x1E a row; [0x52FF44] == 1 is Quit Game's "Are you sure?" box
 * (title "Quit Game" at y 0xC3, A yes, B no). "Quit To Desktop" goes below the last row ([0x52FF48] -1 while it is
 * chosen, so none of the game's rows is lit or acted on) and asks in that same box, its title renamed. Presses it
 * acts on are taken with the game's own sub_00039C10(buttons, pad), as the pause menu takes them; START still
 * closes the pause (Resume's way) from the new row.
 *
 * Built into the recompile with --exclude-manual ../hpcos/src/hle_frontend.c; gen/recomp_0004.c's sub_000FFE60,
 * gen/recomp_0000.c's sub_00033D40 and gen/recomp_0001.c's sub_0004E540 / sub_0004E9B0 bodies are renamed _gen by
 * hand until then. RECOMP_QUIT_ITEM=0 leaves both menus as they were.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECOMP_GENERATED_CODE
#include "recomp/gen/recomp_types.h"
#include "edition.h"

#define ARG(i) MEM32(g_esp + 4 + 4 * (i))

enum {
    kMsgFrame = 0x21000004, kFrontState = 0x527B9E, kScreen = 0x527B9C, kTitleMenu = 4, kRow = 0x546F7C,
    kFlags = 0x547030, kHasSave = 0x10, kLockout = 0x547038, kFont = 0x5471F8, kRowX = 0x140, kRowStep = 0x26, kShift = 0x13, kLastRow = 2,
    kSndMove = 0x30003C3, kSndConfirm = 0x30003C4, kSndBack = 0x30003C5,
    kPads = 0x51E0AC, kPadStride = 0x38,  /* buttons now, +4 last frame's */
    kUp = 0x000001 | 0x100000, kDown = 0x000002 | 0x200000, kConfirm = 0x40400, kBtnA = 0x40000, kBtnB = 0x80800,
    kStart = 0x400,
    kText = 0x53E754, kTxtSure = 0xDDB,  /* "Are you sure?" */
    kTxtYes = 0xE06, kTxtNo = 0xE07, kLanguage = 0x52FDA4,
    kPausePage = 0x52FF40, kPauseSub = 0x52FF44, kPauseRow = 0x52FF48, kPauseOpts = 0x527F26, kPauseFont = 0x540128,
    kPauseY = 0xD7, kPauseStep = 0x1E, kPauseTitleY = 0xC3, kBlink = 0x51D130,
};

extern void sub_000FFE60_gen(void);
extern void sub_00033D40_gen(void);
extern void sub_0004E540_gen(void);
extern void sub_0004E9B0_gen(void);
extern void sub_00032FD0(void);
extern void sub_00039C10(void);
extern void sub_0003BAC0(void);
extern void sub_00041A60(void);
extern void sub_0010CB5A(void);
extern void __stdcall xbox_HalReturnToFirmware(unsigned long routine);
uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);

static int g_in_menu;       /* the title menu is running: its rows move up */
static int g_quit_row;      /* the fourth row is chosen */
static int g_title_ask;     /* the title asks "Are you sure?" */
static int g_title_yes;     /* ... with Yes lit (else No) */
static int g_pause_row;     /* the pause menu's new row is chosen */
static int g_pause_ask;     /* ... and its "Are you sure?" box is up */
static int g_pause_draw;    /* the pause menu is drawing */

static int enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_QUIT_ITEM");
        on = !(e && *e == '0');
    }
    return on;
}

/* a recompiled function on this guest thread: thiscall's `this` in ecx, the arguments pushed right to left and a
 * return address; esp and the callee-saved registers as before whoever pops */
static uint32_t call_guest(void (*fn)(void), uint32_t this_, int n, const uint32_t *args)
{
    uint32_t s_esp = g_esp, s_ebx = g_ebx, s_esi = g_esi, s_edi = g_edi, s_ebp = g_ebp;
    uint32_t sp = s_esp;
    for (int i = n; i-- > 0;) {
        sp -= 4;
        MEM32(sp) = args[i];
    }
    sp -= 4;
    MEM32(sp) = 0;
    g_esp = sp;
    g_ecx = this_;
    fn();
    g_esp = s_esp, g_ebx = s_ebx, g_esi = s_esi, g_edi = s_edi, g_ebp = s_ebp;
    return g_eax;
}

static void sound(uint32_t id)
{
    uint32_t a[2] = {id, 0x3F800000u};  /* volume 1.0 */
    call_guest(sub_00041A60, 0, 2, a);
}

/* the game's own taking of a press: last frame's bits of `buttons` on pad 0 become this frame's */
static void take(uint32_t buttons)
{
    uint32_t a[2] = {buttons, 0};
    call_guest(sub_00039C10, 0, 2, a);
}

static void quit_to_desktop(void)
{
    fprintf(stderr, "[MENU] Quit To Desktop\n");
    uint32_t a[2] = {0, 0};
    call_guest(sub_0010CB5A, 0, 2, a);  /* XLaunchNewImage(NULL, NULL): does not come back */
    xbox_HalReturnToFirmware(2);
}

/* the presses this frame on any pad, as sub_000FF7B0 reads them */
static uint32_t pressed(void)
{
    uint32_t p = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t now = MEM32(kPads + kPadStride * i), was = MEM32(kPads + kPadStride * i + 4);
        p |= (now ^ was) & now;
    }
    return p & ~MEM32(kLockout);
}

/* a UTF-16 string in guest memory, as the title's text tables hold them */
static uint32_t guest_text(const char *s, uint32_t *va)
{
    if (!*va) {
        unsigned n = 0;
        while (s[n]) n++;
        *va = xbox_HeapAlloc((n + 1) * 2, 16);
        if (!*va) return 0;
        for (unsigned i = 0; i <= n; i++) MEM16(*va + 2 * i) = (uint8_t)s[i];
    }
    return *va;
}

/* a line of the game's text table (thiscall on 0x53E754) */
static uint32_t game_text(uint32_t id);

/* "Quit To Desktop" in the language chosen in Options: [0x52FDA4] indexes the title's language table (0x1F7A68,
 * 0x14 a language: USA, UK, French, German, Spanish, Italian), read at each draw so a change of language follows.
 * Worded as the game words "Quit To Dashboard" (text 0x1091); Latin-1, as the text tables hold their letters */
static uint32_t quit_text(void)
{
    static const char *const k_quit[] = {
        "Quit To Desktop", "Quit To Desktop", "Quitter vers le bureau", "Zur\374ck zum Desktop",
        "Salir al escritorio", "Esci al desktop",
    };
    static uint32_t va[6];
    uint32_t lang = MEM32(kLanguage);
    if (lang >= 6) lang = 0;
    return guest_text(k_quit[lang], &va[lang]);
}

/* the game's own Yes and No, in its language */
static uint32_t yes_text(void)
{
    return game_text(kTxtYes);
}

static uint32_t no_text(void)
{
    return game_text(kTxtNo);
}

static uint32_t empty_text(void)
{
    static uint32_t va;
    return guest_text("", &va);
}

/* a line of the game's text table (thiscall on 0x53E754) */
static uint32_t game_text(uint32_t id)
{
    uint32_t a[1] = {id};
    return call_guest(sub_0003BAC0, kText, 1, a);
}

static void draw_text(uint32_t font, uint32_t colour, uint32_t y, uint32_t text)
{
    if (!text) return;
    uint32_t col[1] = {colour};
    call_guest(sub_00032FD0, font, 1, col);
    uint32_t a[3] = {kRowX, y, text};
    call_guest(sub_00033D40_gen, font, 3, a);
}

/* the pause menu's rows (0x4E626) */
static int pause_rows(void)
{
    uint32_t opts = MEM32(kPauseOpts);
    return 3 + !!(opts & 0x200) + !!(opts & 0x800);
}

static int title_row_y(uint32_t y)
{
    return y == 0x118 || y == 0x13E || y == 0x164 || y == 0x12C || y == 0x152;
}

/* ── PC Settings: the F10 settings as the first row of Options, in the game's own menus ────────────────────────────
 *
 * The title's Options is front-end screen 5 of the same handler (sub_000FFE60; the title menu's row 2 goes there):
 * Language and four switches, then the sound and music volumes, rows 0..6 in [0x546F7C] (up and down stop at the
 * ends, sub_000FF7B0 reads the presses), drawn on the font 0x540128 at scale 0.95: labels at x 0x32 and values at
 * 0x190 (sub_00033D10, the volumes' numbers and "<   >" through sub_00033E30), y 145 + 25 a row, the lit one blinking
 * white/black and the rest 0xFFDCC828; the header "Options" on the font 0x5471F8 at (0x32, 0x46); B goes back to the
 * title on its Options row.
 *
 * The pause menu's Options is page 1 ([0x52FF40]): input sub_0004DFD0, drawing sub_0004EC10, rows 0..5 in
 * [0x52FF48] (the text ids at 0x1E7FE4, no Language), y 0x96 + 0x1E a row at the box's left + 10 (the box,
 * sub_0004E490, as wide as its words, from 0x8C to 0x168), the header (0xDD6) centred at y 0x6E and the legend at
 * 0x154 (sub_00033D40); B goes back to page 0, START closes the pause.
 *
 * "PC Settings" is the first row of both, chosen as Options opens; the game's rows are drawn a row lower (and the
 * pause box and its legend a row taller). While it is lit the game's row is -1 (title; the confirm switch at 0x1022BD
 * and the volume rows leave -1 alone) or the game's input step is not run (pause), and up and down move between it
 * and the game's first row. A (or START on the title) opens it: a list of pages (app_GameOptRows(-1), menu.cpp:
 * Display, Graphics, ...), then a page's rows with their values, all drawn in place of the game's rows with the
 * header renamed; up and down choose, left and right (held: repeating) or A change a value, B goes back a level. The
 * game sees none of those presses. The pause's box is made wider while a page is up, its values right-aligned.
 *
 * RECOMP_SETTINGS_ITEM=0 leaves both menus as they were. gen's sub_00033D10 / sub_00033E30 (recomp_0000.c) and
 * sub_0004DFD0 / sub_0004E490 / sub_0004EC10 (recomp_0001.c) bodies are renamed _gen by hand until a regen. */
enum {
    kTitleOptions = 5, kOptX = 0x32, kOptValueX = 0x190, kOptY0 = 145, kOptStep = 25, kOptPitch = 22, kOptRows = 8,
    kHeaderFont = 0x5471F8, kHeaderX = 0x32, kHeaderY = 0x46,
    kPauseOptions = 1, kPOptY0 = 0x96, kPOptStep = 0x1E, kPOptRows = 7, kPOptTop = 0x8C, kPOptBottom = 0x168,
    kPOptHeaderY = 0x6E, kPOptLegendY = 0x154, kPOptWideL = 64, kPOptWideR = 576,
    kLeft = 0x400000 | 4, kRight = 0x800000 | 8,
    kLit = 0xFFFFFFFFu, kLitBlink = 0xFF000000u, kUnlit = 0xFFDCC828u,
    kNotIn = -2, kPageList = -1,
};

extern void sub_0004DFD0_gen(void);
extern void sub_0004E490_gen(void);
extern void sub_0004EC10_gen(void);
extern void sub_00033D10_gen(void);
extern void sub_00033E30_gen(void);
extern void sub_000330D0(void);
extern void sub_00033150(void);
extern void sub_0001B900(void);  /* filled rectangle */
extern void sub_000333A0(void);
extern void sub_000333B0(void);
extern void sub_00033390(void);
extern void sub_000337A0(void);
/* renderer/app/menu.cpp: the settings' pages */
extern int app_GameOptRows(int page);
extern const char *app_GameOptLabel(int page, int row);
extern const char *app_GameOptValue(int page, int row);
extern void app_GameOptStep(int page, int row, int dir);
extern void app_GameOptLeave(int page);  /* a page's resolution choice applied */

/* one menu's PC Settings */
typedef struct {
    int pc_lit;  /* the PC Settings row is lit (in the game's Options list) */
    int page;    /* kNotIn, kPageList, or a page */
    int sel, top, list_sel;
    int rep_dir, rep_t;  /* left / right held */
} PcMenu;
static PcMenu g_tm = {0, kNotIn, 0, 0, 0, 0, 0}, g_pm = {0, kNotIn, 0, 0, 0, 0, 0};
static int g_topt_draw;     /* the title's Options is running (its frame) */
static int g_popt_draw;     /* the pause's Options is drawing */
static uint32_t g_popt_left, g_popt_right;  /* ... its box */

static int settings_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_SETTINGS_ITEM");
        on = !(e && *e == '0');
    }
    return on;
}

static void pc_reset(PcMenu *m, int lit)
{
    if (m->page >= 0) app_GameOptLeave(m->page);  /* Options or the pause closed on a page */
    m->pc_lit = lit;
    m->page = kNotIn;
    m->sel = m->top = m->list_sel = m->rep_dir = m->rep_t = 0;
}

/* "PC Settings" in the language chosen in Options, as quit_text */
static uint32_t settings_text(void)
{
    static const char *const k_text[] = {
        "PC Settings", "PC Settings", "Param\350tres PC", "PC-Einstellungen", "Ajustes de PC", "Impostazioni PC",
    };
    static uint32_t va[6];
    uint32_t lang = MEM32(kLanguage);
    if (lang >= 6) lang = 0;
    return guest_text(k_text[lang], &va[lang]);
}

/* a frame's words for the settings' rows: each draw its own guest string, written again the next frame */
enum { kSlots = 40, kSlotChars = 48 };
static uint32_t g_slot_va[kSlots];
static int g_slot_next;

static uint32_t slot_text(const char *s)
{
    int i = g_slot_next++ % kSlots;
    if (!g_slot_va[i]) g_slot_va[i] = xbox_HeapAlloc(kSlotChars * 2, 16);
    uint32_t va = g_slot_va[i];
    if (!va) return 0;
    int n = 0;
    for (; s && s[n] && n < kSlotChars - 1; n++) MEM16(va + 2 * n) = (uint8_t)s[n];
    MEM16(va + 2 * n) = 0;
    return va;
}

static uint32_t row_colour(int lit)
{
    return lit ? ((MEM8(kBlink) & 0x10) ? kLit : kLitBlink) : kUnlit;
}

/* left-aligned text on the options' font (sub_00033D10: this = font; x, y, text) */
static void draw_left(uint32_t colour, uint32_t x, uint32_t y, uint32_t text)
{
    if (!text) return;
    uint32_t col[1] = {colour}, on[1] = {(uint32_t)-1}, a[3] = {x, y, text};
    call_guest(sub_00032FD0, kPauseFont, 1, col);
    call_guest(sub_00033150, kPauseFont, 1, on);  /* as the options' rows are drawn */
    call_guest(sub_00033D10_gen, kPauseFont, 3, a);
}

/* a text's width on the options' font */
static uint32_t text_width(uint32_t text)
{
    uint32_t a[2] = {text, (uint32_t)-1};
    return call_guest(sub_000337A0, kPauseFont, 2, a);
}

static int pc_rows(const PcMenu *m)
{
    return app_GameOptRows(m->page);
}

/* the presses on a PC Settings page; returns the buttons it acted on (kept from the game) */
static uint32_t pc_input(PcMenu *m, uint32_t p, uint32_t held, uint32_t confirm, int shown)
{
    int n = pc_rows(m);
    uint32_t used = 0;
    if (p & kUp) {
        used |= kUp;
        if (m->sel > 0) m->sel--, sound(kSndMove);
    } else if (p & kDown) {
        used |= kDown;
        if (m->sel < n - 1) m->sel++, sound(kSndMove);
    }
    /* left and right: once on the press, then every 4 frames after 18 held */
    int dir = (held & kLeft) ? -1 : (held & kRight) ? 1 : 0;
    int step = 0;
    if (!dir) {
        m->rep_dir = m->rep_t = 0;
    } else if (dir != m->rep_dir || (p & (dir < 0 ? kLeft : kRight))) {
        m->rep_dir = dir, m->rep_t = 0, step = 1;
    } else if (++m->rep_t >= 18 && (m->rep_t - 18) % 4 == 0) {
        step = 1;
    }
    used |= held & (kLeft | kRight);
    if (step && m->page >= 0) {
        app_GameOptStep(m->page, m->sel, dir);
        sound(kSndMove);
    }
    if (p & confirm) {
        used |= p & confirm;
        sound(kSndConfirm);
        if (m->page == kPageList) {
            m->list_sel = m->sel;
            m->page = m->sel;
            m->sel = m->top = 0;
        } else {
            app_GameOptStep(m->page, m->sel, 1);
        }
    } else if (p & kBtnB) {
        used |= kBtnB;
        sound(kSndBack);
        if (m->page >= 0) {
            app_GameOptLeave(m->page);
            m->page = kPageList;
            m->sel = m->list_sel;
            m->top = 0;
        } else {
            pc_reset(m, 1);  /* back on the PC Settings row */
        }
    }
    if (m->page != kNotIn) {
        n = pc_rows(m);
        if (m->sel >= n) m->sel = n - 1;
        if (m->sel < 0) m->sel = 0;
        if (m->sel < m->top) m->top = m->sel;
        if (m->sel >= m->top + shown) m->top = m->sel - shown + 1;
    }
    return used;
}

/* The title's Options: the band between the gold bars and the game's text are drawn together after its handler
 * (0x10E685), over anything drawn while it runs, so its shapes wait for the frame's Swap (hpcos_FrontendSwap) */
static int g_defer_shapes;
enum { kRectQ = 96 };
static struct { int32_t l, t, r, b; uint32_t colour; } g_rect_q[kRectQ];
static int g_rect_n;

/* the game's filled rectangle (sub_0001B900, cdecl: left, top, right, bottom, colour; the pause box's own) */
static void fill_rect(int32_t l, int32_t t, int32_t r, int32_t b, uint32_t colour)
{
    if (g_defer_shapes) {
        if (g_rect_n < kRectQ) {
            g_rect_q[g_rect_n].l = l, g_rect_q[g_rect_n].t = t, g_rect_q[g_rect_n].r = r, g_rect_q[g_rect_n].b = b;
            g_rect_q[g_rect_n++].colour = colour;
        }
        return;
    }
    uint32_t a[5] = {(uint32_t)l, (uint32_t)t, (uint32_t)r, (uint32_t)b, colour};
    call_guest(sub_0001B900, 0, 5, a);
}

/* an asterisk, 7 by 7 from (x, y): the menu font has no '*' (panel.geo's glyph table) */
static void star(int32_t x, int32_t y, uint32_t colour)
{
    fill_rect(x + 3, y, x + 4, y + 7, colour);          /* | */
    fill_rect(x, y + 3, x + 7, y + 4, colour);          /* - */
    for (int i = 0; i < 7; i++) {
        if (i == 3) continue;
        fill_rect(x + i, y + i, x + i + 1, y + i + 1, colour);          /* \ */
        fill_rect(x + 6 - i, y + i, x + 7 - i, y + i + 1, colour);      /* / */
    }
}

/* a small triangle, 9 wide and 5 high, its point up or down, centred on cx from y: the font has no arrows */
static void arrow(int32_t cx, int32_t y, int up)
{
    for (int i = 0; i < 5; i++) {
        int32_t row = up ? y + i : y + 4 - i;
        fill_rect(cx - i, row, cx + i + 1, row + 1, kUnlit);
    }
}

/* a page's rows from y0: labels at x, values at vx (left-aligned) or ending at vr (right-aligned, vx 0); arrows centred
 * on ax beside the first and last rows shown when the page goes on above or below them; under a page with a setting
 * that waits for the next start (its label ends in '*'), a line saying so */
static void pc_draw_rows(const PcMenu *m, int shown, uint32_t x, uint32_t vx, uint32_t vr, uint32_t y0, uint32_t step,
                         int32_t ax)
{
    static uint32_t restart_va;
    int n = pc_rows(m), restart = 0;
    for (int i = 0; i < shown && m->top + i < n; i++) {
        int r = m->top + i;
        uint32_t y = y0 + step * (uint32_t)i, col = row_colour(r == m->sel);
        /* a label ending in '*' (a setting for the next start): its words, then a drawn asterisk */
        char words[kSlotChars];
        snprintf(words, sizeof(words), "%s", app_GameOptLabel(m->page, r));
        size_t len = strlen(words);
        int marked = len && words[len - 1] == '*';
        if (marked) {
            words[--len] = 0;
            while (len && words[len - 1] == ' ') words[--len] = 0;
            restart = 1;
        }
        uint32_t lt = slot_text(words);
        draw_left(col, x, y, lt);
        if (marked && lt) star((int32_t)(x + text_width(lt)) + 4, (int32_t)y + 4, col);
        if (m->page < 0) continue;
        uint32_t v = slot_text(app_GameOptValue(m->page, r));
        if (!v) continue;
        draw_left(col, vx ? vx : vr - text_width(v), y, v);
    }
    if (m->top > 0) arrow(ax, (int32_t)y0 + 6, 1);
    if (m->top + shown < n) arrow(ax, (int32_t)(y0 + step * (uint32_t)(shown - 1)) + 8, 0);
    if (restart && n + 1 < shown) {
        uint32_t y = y0 + step * (uint32_t)(n + 1);
        star((int32_t)x, (int32_t)y + 4, 0xFFA0A0A0u);
        draw_left(0xFFA0A0A0u, x + 12, y, guest_text("Applies after a restart", &restart_va));
    }
}

static uint32_t pc_header(const PcMenu *m)
{
    return m->page >= 0 ? slot_text(app_GameOptLabel(kPageList, m->page)) : settings_text();
}

/* the title's Options, one frame of the front end's handler */
static int g_title_prev_screen;
static void title_options_frame(void)
{
    if (g_title_prev_screen == kTitleMenu) pc_reset(&g_tm, 1);  /* Options opened: PC Settings first */
    uint32_t p = pressed(), held = 0, was[4], hide = 0;
    for (int i = 0; i < 4; i++) held |= MEM32(kPads + kPadStride * i);
    held &= ~MEM32(kLockout);
    g_slot_next = 0;
    if (g_tm.page != kNotIn) {
        pc_input(&g_tm, p, held, kConfirm, kOptRows);
        hide = kUp | kDown | kConfirm | kBtnB;  /* the screen sees none of its presses */
    } else if (g_tm.pc_lit) {
        hide = kUp | kDown | kConfirm;
        if (p & kConfirm) {
            sound(kSndConfirm);
            g_tm.page = kPageList;
            g_tm.sel = g_tm.top = 0;
            hide |= kBtnB;
        } else if (p & kDown) {
            g_tm.pc_lit = 0;
            MEM32(kRow) = 0;
            sound(kSndMove);
        }
    } else if ((p & kUp) && MEM32(kRow) == 0) {
        g_tm.pc_lit = 1;
        sound(kSndMove);
        hide = kUp | kDown | kConfirm;
    }
    int ours = g_tm.pc_lit || g_tm.page != kNotIn;
    if (ours) MEM32(kRow) = (uint32_t)-1;  /* none of the screen's rows lit or acted on */
    if (hide)
        for (int i = 0; i < 4; i++) {
            uint32_t w = kPads + kPadStride * i + 4;
            was[i] = MEM32(w);
            MEM32(w) |= MEM32(w - 4) & hide;
        }

    uint32_t s_esp = g_esp;
    g_topt_draw = 1;
    sub_000FFE60_gen();
    g_topt_draw = 0;

    if (hide)
        for (int i = 0; i < 4; i++) MEM32(kPads + kPadStride * i + 4) = was[i];
    g_title_prev_screen = kTitleOptions;
    if (MEM16(kScreen) != kTitleOptions) {  /* B: back to the title (or Language) */
        g_title_prev_screen = MEM16(kScreen);
        pc_reset(&g_tm, 0);
        return;
    }
    (void)s_esp;
}

/* The title's Options rows in the band between the gold bars: eight rows (PC Settings and the game's seven) 22 apart
 * from y 145 instead of the game's seven 25 apart. A game row at 145 + 25 r goes to 145 + 22 (r + 1) */
static uint32_t title_row_y_moved(uint32_t y)
{
    uint32_t r = (y - kOptY0 + kOptStep / 2) / kOptStep;
    return kOptY0 + kOptPitch * (r + 1);
}

/* PC Settings's rows on the title's Options, drawn as the game draws its header: the game's text goes out together
 * later in its frame (drawn after the frame, it went out with the 3D scene's band, small and off to the right), and
 * the font's state as the game sets it for its own rows (0x100A5C); the game sets it again for its rows after */
static void title_draw_ours(void)
{
    uint32_t a1[1] = {(uint32_t)-1}, a2[1] = {0xBB000000u}, a3[2] = {2, 0}, a4[1] = {1};
    uint32_t scale[2] = {0x3F733333u, 0x3F733333u};  /* 0.95 */
    call_guest(sub_00033150, kPauseFont, 1, a1);
    call_guest(sub_000333A0, kPauseFont, 1, a2);
    call_guest(sub_000333B0, kPauseFont, 2, a3);
    call_guest(sub_00033390, kPauseFont, 1, a4);
    call_guest(sub_000330D0, kPauseFont, 2, scale);
    if (g_tm.page == kNotIn)
        draw_left(row_colour(g_tm.pc_lit), kOptX, kOptY0, settings_text());
    else {
        g_defer_shapes = 1;
        pc_draw_rows(&g_tm, kOptRows, kOptX, kOptValueX, 0, kOptY0, kOptPitch, kOptX - 14);
        g_defer_shapes = 0;
    }
}

/* each Swap (hle_d3d8.cpp), before the frame goes out: the title's Options shapes (arrows, asterisks) over its band.
 * In 16:9 the front end's words are widened about the centre (hle_hud.c FRONT: x -> 320 + (x - 320) / 0.75) and the
 * renderer squeezes all 2D back by 0.75, so they land where laid out; these shapes, drawn here at the Swap, are not
 * the front end's code to hle_hud.c, so they are widened the same here. Unwidened they were pulled to the middle: the
 * asterisk of "Speakers*" far right of its word, the restart line's inside "Applies after" (2026-11-14) */
extern int hpcos_Widescreen(void); /* renderer/xdk/hle_d3d8.cpp: booted in 16:9 */
static int32_t front_x(int32_t x)
{
    float w = 320.0f + ((float)x - 320.0f) / 0.75f;
    return (int32_t)(w < 0.0f ? w - 0.5f : w + 0.5f);
}

void hpcos_FrontendSwap(void)
{
    int n = g_rect_n, wide = n && hpcos_Widescreen();
    g_rect_n = 0;
    for (int i = 0; i < n; i++) {
        int32_t l = g_rect_q[i].l, r = g_rect_q[i].r;
        if (wide) {
            l = front_x(l), r = front_x(r);
            if (r <= l) r = l + 1;
        }
        fill_rect(l, g_rect_q[i].t, r, g_rect_q[i].b, g_rect_q[i].colour);
    }
}

/* the pause's Options: its input step (no arguments; the return address popped when the game's is not run) */
void sub_0004DFD0(void)
{
    if (!settings_on()) {
        sub_0004DFD0_gen();
        return;
    }
    uint32_t now = MEM32(kPads), p = (now ^ MEM32(kPads + 4)) & now;
    if (g_pm.page != kNotIn) {
        uint32_t used = pc_input(&g_pm, p, now, kBtnA, kPOptRows);
        take(used | kBtnB);  /* B is ours here, not the pause's way back */
    } else if (g_pm.pc_lit) {
        if (p & kBtnA) {
            take(kBtnA);
            sound(kSndConfirm);
            g_pm.page = kPageList;
            g_pm.sel = g_pm.top = 0;
        } else if (p & kDown) {
            take(kUp | kDown);
            sound(kSndMove);
            g_pm.pc_lit = 0;
            MEM32(kPauseRow) = 0;
        } else if (p & kUp) {
            take(kUp);
        }
    } else if ((p & kUp) && MEM32(kPauseRow) == 0) {
        take(kUp);
        sound(kSndMove);
        g_pm.pc_lit = 1;
    } else {
        sub_0004DFD0_gen();
        return;
    }
    g_esp += 4;
}

/* a menu box (left, top, right, bottom): the pause's Options one a row taller, wider for a page of settings */
void sub_0004E490(void)
{
    if (g_popt_draw && ARG(1) == kPOptTop && ARG(3) == kPOptBottom) {
        ARG(3) += kPOptStep;
        if (g_pm.page != kNotIn && (int32_t)ARG(0) > kPOptWideL) ARG(0) = kPOptWideL, ARG(2) = kPOptWideR;
        g_popt_left = ARG(0);
        g_popt_right = ARG(2);
    }
    sub_0004E490_gen();
}

/* the pause's Options: its drawing */
void sub_0004EC10(void)
{
    if (!settings_on()) {
        sub_0004EC10_gen();
        return;
    }
    uint32_t row = MEM32(kPauseRow);
    if (g_pm.pc_lit || g_pm.page != kNotIn) MEM32(kPauseRow) = (uint32_t)-1;  /* none of the game's rows lit */
    g_popt_draw = 1;
    g_popt_left = g_popt_right = 0;
    g_slot_next = 0;
    uint32_t s_esp = g_esp;
    sub_0004EC10_gen();
    uint32_t ret_esp = g_esp;
    g_esp = s_esp;
    g_popt_draw = 0;
    MEM32(kPauseRow) = row;
    if (g_popt_left) {
        if (g_pm.page == kNotIn)
            draw_left(row_colour(g_pm.pc_lit), g_popt_left + 10, kPOptY0, settings_text());
        else
            pc_draw_rows(&g_pm, kPOptRows, g_popt_left + 26, 0, g_popt_right - 10, kPOptY0, kPOptStep,
                         (int32_t)g_popt_left + 13);
        uint32_t off[1] = {0};
        call_guest(sub_00033150, kPauseFont, 1, off);  /* as the game leaves it (0x4EE97) */
    }
    g_esp = ret_esp;
}

/* left-aligned text draw (this = font; x, y, text; ret 0xC): in Options, the game's rows a row lower for PC Settings,
 * or left out (and the header renamed) while a page of settings is up */
void sub_00033D10(void)
{
    if (g_topt_draw && g_ecx == kPauseFont && ARG(1) >= kOptY0 - 5) {
        if (g_tm.page != kNotIn) {
            g_esp += 4 + 12;
            return;
        }
        ARG(1) = title_row_y_moved(ARG(1));
    } else if (g_topt_draw && g_ecx == kHeaderFont && ARG(0) == kHeaderX && ARG(1) == kHeaderY) {
        if (g_tm.page != kNotIn) ARG(2) = pc_header(&g_tm);
        uint32_t s_esp = g_esp;
        sub_00033D10_gen();
        uint32_t ret_esp = g_esp;
        g_esp = s_esp;
        title_draw_ours();
        g_esp = ret_esp;
        return;
    } else if (g_popt_draw && g_ecx == kPauseFont && ARG(1) >= kPOptY0 && ARG(1) < kPOptLegendY) {
        if (g_pm.page != kNotIn) {
            g_esp += 4 + 12;
            return;
        }
        ARG(1) += kPOptStep;
    }
    sub_00033D10_gen();
}

/* formatted text draw (cdecl: font, x, y, format, ...): the volume numbers and their "<   >" in both Options (the
 * pause's at 0x4EF16 / 0x4EF6C, the arrows at 0x4EF3A / 0x4EF90) */
void sub_00033E30(void)
{
    if (g_topt_draw && ARG(0) == kPauseFont && ARG(2) >= kOptY0 - 5) {
        if (g_tm.page != kNotIn) {
            g_esp += 4;
            return;
        }
        ARG(2) = title_row_y_moved(ARG(2));
    } else if (g_popt_draw && ARG(0) == kPauseFont && ARG(2) >= kPOptY0 && ARG(2) < kPOptLegendY) {
        if (g_pm.page != kNotIn) {
            g_esp += 4;
            return;
        }
        ARG(2) += kPOptStep;
    }
    sub_00033E30_gen();
}

/* text draw (this = font; x, y, text) */
void sub_00033D40(void)
{
    if (g_popt_draw && g_ecx == kPauseFont && ARG(1) == kPOptLegendY) {
        ARG(1) += kPOptStep;  /* the pause's Options: its legend under the new row */
    } else if (g_popt_draw && g_ecx == kPauseFont && ARG(1) == kPOptHeaderY) {
        if (g_pm.page != kNotIn) ARG(2) = pc_header(&g_pm);  /* "Options": the page's name */
    } else if (g_in_menu && g_ecx == kFont && ARG(0) == kRowX && title_row_y(ARG(1))) {
        /* the title menu's rows half a row higher; none while it asks */
        ARG(1) -= kShift;
        if (g_title_ask) ARG(2) = empty_text();
    } else if (g_pause_draw && g_ecx == kPauseFont && ARG(0) == kRowX) {
        uint32_t y = ARG(1), last = kPauseY + kPauseStep * (pause_rows() - 1);
        if (g_pause_ask && y == kPauseTitleY) {
            ARG(2) = quit_text();  /* the box's title: "Quit To Desktop", not "Quit Game" */
        } else if (MEM32(kPauseSub) == 0 && y == last) {
            uint32_t s_esp = g_esp;
            sub_00033D40_gen();
            uint32_t ret_esp = g_esp;
            g_esp = s_esp;
            /* the new row under the last, lit as the pause menu lights its own (0x4EB90) */
            uint32_t col = g_pause_row ? ((MEM8(kBlink) & 0x10) ? 0xFFFFFFFFu : 0xFF000000u) : 0xFFDCC828u;
            draw_text(kPauseFont, col, last + kPauseStep, quit_text());
            g_esp = ret_esp;
            return;
        }
    }
    sub_00033D40_gen();
}

void sub_000FFE60(void)
{
    if (ARG(0) != kMsgFrame || MEM16(kFrontState) != 4) {
        /* the handler's other messages (0x10000004 and the rest) come every frame too: they leave the row alone */
        sub_000FFE60_gen();
        return;
    }
    if (MEM16(kScreen) == kTitleOptions && settings_on()) {
        g_quit_row = g_title_ask = 0;
        title_options_frame();
        return;
    }
    g_title_prev_screen = MEM16(kScreen);
    if (!enabled() || MEM16(kScreen) != kTitleMenu) {
        g_quit_row = g_title_ask = 0;
        sub_000FFE60_gen();
        return;
    }
    uint32_t p = pressed(), held[4], hide = 0;
    if (g_title_ask) {
        hide = kUp | kDown | kConfirm | kBtnB;  /* B would leave the menu */
        if ((p & kConfirm) && g_title_yes) {
            sound(kSndConfirm);
            quit_to_desktop();
        } else if (p & (kConfirm | kBtnB)) {
            g_title_ask = 0;
            sound(kSndBack);
        } else if (p & (kUp | kDown)) {
            g_title_yes = !g_title_yes;
            sound(kSndMove);
        }
    } else if (!g_quit_row) {
        if ((p & kDown) && (int32_t)MEM32(kRow) == kLastRow) {
            g_quit_row = 1;
            sound(kSndMove);
            hide = kUp | kDown | kConfirm;
        }
    } else {
        hide = kUp | kDown | kConfirm;
        if (p & kConfirm) {
            sound(kSndConfirm);
            g_title_ask = 1;
            g_title_yes = 0;
        } else if (p & kUp) {
            g_quit_row = 0;
            MEM32(kRow) = kLastRow;
            sound(kSndMove);
        }
    }
    if (g_quit_row) MEM32(kRow) = (uint32_t)-1;  /* no title row lit */
    if (hide)
        for (int i = 0; i < 4; i++) {
            /* no new press of these reaches the title this frame */
            uint32_t was = kPads + kPadStride * i + 4;
            held[i] = MEM32(was);
            MEM32(was) |= MEM32(was - 4) & hide;
        }

    uint32_t s_esp = g_esp;
    g_in_menu = 1;
    sub_000FFE60_gen();
    g_in_menu = 0;

    if (hide)
        for (int i = 0; i < 4; i++) MEM32(kPads + kPadStride * i + 4) = held[i];
    /* the title may have left the menu this frame (Options, New Game) */
    if (MEM16(kScreen) != kTitleMenu) {
        g_quit_row = g_title_ask = 0;
        return;
    }
    uint32_t ret_esp = g_esp;
    g_esp = s_esp;  /* our calls below the frame the title's handler had */
    uint32_t last = (MEM8(kFlags) & kHasSave) ? 0x164 : 0x152;
    if (g_title_ask) {
        /* in the four rows' places: the question in the pause box's yellow (0x4EADC), then Yes and No lit and dimmed
         * as the title's own rows */
        uint32_t y = 0x118 - kShift;
        draw_text(kFont, 0xFFFFFF00u, y, quit_text());
        draw_text(kFont, 0xFFFFFF00u, y + kRowStep, game_text(kTxtSure));
        draw_text(kFont, g_title_yes ? 0xFFFFFFFFu : 0xBB808080u, y + 2 * kRowStep, yes_text());
        draw_text(kFont, g_title_yes ? 0xBB808080u : 0xFFFFFFFFu, y + 3 * kRowStep, no_text());
    } else {
        draw_text(kFont, g_quit_row ? 0xFFFFFFFFu : 0xBB808080u, last + kRowStep - kShift, quit_text());
    }
    g_esp = ret_esp;
}

/* the pause menu's input step */
void sub_0004E540(void)
{
    /* xboxrecomp debug: RECOMP_LEVEL_SELECT=1 shows the developers' Level Selector row (bit 0x200 of [0x527F26]):
     * every level with its start points by name. The bit is in the progress block, so a save keeps it */
    static int level_select = -1;
    if (level_select < 0) {
        const char *e = getenv("RECOMP_LEVEL_SELECT");
        level_select = e && *e == '1';
    }
    if (level_select) MEM16(kPauseOpts) |= 0x200;
    {   /* Options opened from the pause's rows: PC Settings first; left or the pause closed: as new */
        static uint32_t prev_page;
        uint32_t page = MEM32(kPausePage);
        if (page != kPauseOptions) pc_reset(&g_pm, 0);
        else if (prev_page != kPauseOptions) pc_reset(&g_pm, 1);
        prev_page = page;
    }
    if (!enabled() || MEM32(kPausePage) != 0) {
        g_pause_row = g_pause_ask = 0;
        sub_0004E540_gen();
        return;
    }
    /* a pause opened afresh (sub_0004DE80) starts on Resume with no box */
    if (g_pause_row && MEM32(kPauseRow) != (uint32_t)-1) g_pause_row = g_pause_ask = 0;
    if (g_pause_ask && MEM32(kPauseSub) != 1) g_pause_ask = 0;

    uint32_t now = MEM32(kPads), p = (now ^ MEM32(kPads + 4)) & now;
    int n = pause_rows();
    if (g_pause_ask) {
        /* the box's A and B are ours; START closes the pause as with Quit Game's */
        if (p & kBtnA) {
            take(kBtnA);
            sound(kSndConfirm);
            quit_to_desktop();
        } else if (p & kBtnB) {
            take(kBtnB);
            sound(kSndBack);
            MEM32(kPauseSub) = 0;
            g_pause_ask = 0;
        }
    } else if (!g_pause_row) {
        if ((p & kDown) && MEM32(kPauseSub) == 0 && (int32_t)MEM32(kPauseRow) == n - 1) {
            take(kDown);
            sound(kSndMove);
            g_pause_row = 1;
        }
    } else if (MEM32(kPauseSub) == 0) {
        if (p & kStart) {
            /* START resumes from any row: hand it Resume's */
            g_pause_row = 0;
            MEM32(kPauseRow) = 0;
        } else if (p & kBtnA) {
            take(kBtnA);
            sound(kSndConfirm);
            MEM32(kPauseSub) = 1;
            g_pause_ask = 1;
        } else if (p & kUp) {
            take(kUp | kDown);
            sound(kSndMove);
            g_pause_row = 0;
            MEM32(kPauseRow) = n - 1;
        } else if (p & kDown) {
            take(kDown);
        }
    }
    if (g_pause_row) MEM32(kPauseRow) = (uint32_t)-1;  /* none of the game's rows lit or acted on */
    sub_0004E540_gen();
}

/* the pause menu's drawing */
void sub_0004E9B0(void)
{
    g_pause_draw = enabled() && MEM32(kPausePage) == 0;
    sub_0004E9B0_gen();
    g_pause_draw = 0;
}

/* ── The menus' A / B legend with keys ──────────────────────────────────────────────────────────────────────────────
 *
 * The front end's legends ("A Select", "B Exit": sub_000FF860 / FF940 / FFA90) put the A and B buttons up as small
 * models through sub_000A0430(id, model, x, y, size): 0x2000010F the A and 0x20000110 the B, each an entry of the
 * frame's list (its index back, -1 for none; sub_000A05F0 leaves an index past the list alone). They are not the font,
 * so key prompts (app_KeyboardPrompts) did not reach them. While key prompts are on, the button is the text font's own
 * picture of it (0x1701 A, 0x1703 B) where the model stood, which the renderer draws as the bound key
 * (d3d12.cpp PromptFontPicture); with the controller the models are as the game has them. */
extern void sub_000A0430_gen(void);
extern int app_KeyboardPrompts(void);

void sub_000A0430(void)
{
    uint32_t id = ARG(0);
    if ((id == 0x2000010Fu || id == 0x20000110u) && app_KeyboardPrompts()) {
        static uint32_t glyph_va[2];
        int b = id == 0x20000110u;
        if (!glyph_va[b]) {
            glyph_va[b] = xbox_HeapAlloc(4, 16);
            if (glyph_va[b]) MEM16(glyph_va[b]) = b ? 0x1703 : 0x1701, MEM16(glyph_va[b] + 2) = 0;
        }
        union { uint32_t u; float f; } x = {ARG(2)}, y = {ARG(3)};
        if (glyph_va[b]) {
            uint32_t col[1] = {0xFFFFFFFFu}, scale[2] = {0x3F666666u, 0x3F666666u};  /* white, 0.9 */
            uint32_t a[3] = {(uint32_t)(int32_t)(x.f + 4.0f), (uint32_t)(int32_t)(y.f + 2.0f), glyph_va[b]};
            call_guest(sub_00032FD0, kPauseFont, 1, col);
            call_guest(sub_000330D0, kPauseFont, 2, scale);
            call_guest(sub_00033D10_gen, kPauseFont, 3, a);
        }
        g_eax = 0xFFFFFFFFu;  /* no model */
        g_esp += 4;
        return;
    }
    sub_000A0430_gen();
}

/* ── UK English ─────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * sub_00011290 turns the console's language into the title's language mask [0x51E06C] (1 UK English, 2 USA English,
 * 4 French, 8 German, 0x10 Spanish, 0x20 Italian; sub_0004A4B0 picks the language index from it): English is UK, made
 * USA when the game's region is North America, as the NTSC-US disc's is. The launcher's "English (UK)" (RECOMP_LANGUAGE
 * english_uk; the kernel answers English for it) keeps UK: the _UK folder's text and speech. The PAL disc's region is
 * Europe, so the game keeps UK there by itself; its "English (US)" (english) makes it USA, from the _USA folder the PAL
 * disc carries too (hpcos_pal.exe, edition.h).
 * Built with --exclude-manual like the rest of this file; gen's sub_00011290 renamed _gen by hand until then. */
extern void sub_00011290_gen(void);
void sub_00011290(void)
{
    sub_00011290_gen();
    const char *e = getenv("RECOMP_LANGUAGE");
    if (e && !strcmp(e, "english_uk") && MEM32(0x51E06C) == 2) {
        MEM32(0x51E06C) = 1;
        fprintf(stderr, "[PORT] language: UK English\n");
    }
#if HPCOS_PAL
    if (e && !strcmp(e, "english") && MEM32(0x51E06C) == 1) {
        MEM32(0x51E06C) = 2;
        fprintf(stderr, "[PORT] language: US English\n");
    }
#endif
}

/* ── Key names in the game's prompts ────────────────────────────────────────────────────────────────────────────
 *
 * The text tables write a button as a character of its own, drawn from the font's picture of it (panel.geo's glyph
 * table at 0x6CAC4): 0x1701 A, 0x1702 X, 0x1703 B, 0x1704 Y, 0x1705 the left trigger, 0x1706 the right trigger,
 * 0x1707 BACK ("... by pressing BACK": the Remembrall); 0x1710 is a hand, not a button. With key prompts on
 * (app_KeyboardPrompts: F10 Controls, Button prompts) each comes back as the bound key in brackets, as PoA's prompts
 * name them: "PRESS [Space] TO CONTINUE", buttons side by side joined ("Equip to [E]/[Q]/[Right Click]"); the
 * game's fonts have [ ] and /.
 *
 * Every string with a button in it is given as a copy of its own on the game's heap (the game's tables are left as
 * they are), one per string, room for the longest key names: the copy holds the words for the device in use now and
 * is written again in place when that changes or a key is bound anew (hpcos_PromptRefresh, each Swap). A tutorial
 * box keeps the string it was given when it opened (one lookup per showing), so it switches with the device while it
 * is up, as PoA's tutorials do (2026-11-09).
 *
 * The inventory (the Remembrall's pages) keeps the button characters: they are drawn from the font's picture, which
 * the renderer draws with the keys as icons while key prompts are on (d3d12.cpp's PromptFontPicture, 2026-11-10), so
 * its "Press <X> or <Y> or <B> to assign" and "Equip to ..." show key icons in their colours, not words in its black
 * text. Its lookups are told apart by the code that asks (sub_0003BAC0's return address, icon_caller). So are the
 * legends, text that starts with a button ("<BACK> Exit", "<A> Close", "<A> Yes"): the game puts the word at a set
 * place after the button, so a key's name ran under it ("[Spac Close"); they keep the button too, shown as the key's
 * icon (2026-11-10). */
extern int app_KeyboardPrompts(void);
extern const char *app_ButtonKeyLabel(int pad_input);

static int prompt_button(uint16_t c)
{
    switch (c) {
        case 0x1701: return 0;   /* A */
        case 0x1702: return 2;   /* X */
        case 0x1703: return 1;   /* B */
        case 0x1704: return 3;   /* Y */
        case 0x1705: return 6;   /* left trigger */
        case 0x1706: return 7;   /* right trigger */
        case 0x1707: return 13;  /* BACK */
        default: return -1;
    }
}

enum { kLabelMax = 21, kPromptCopies = 256 };
static struct { uint32_t src, copy, cap; } g_prompt[kPromptCopies];
static int g_prompt_n, g_prompt_next;
static char g_prompt_sig[256];  /* what the copies hold: "pad", or the key names */

/* "pad" with the controller's prompts, else the key names now */
static void prompt_sig(char *sig, size_t size)
{
    static const int kButtons[] = {0, 1, 2, 3, 6, 7, 13};
    if (!app_KeyboardPrompts()) {
        snprintf(sig, size, "pad");
        return;
    }
    int sl = snprintf(sig, size, "keys|");
    for (unsigned b = 0; b < sizeof(kButtons) / sizeof(kButtons[0]) && sl < (int)size; b++)
        sl += snprintf(sig + sl, size - (size_t)sl, "%s|", app_ButtonKeyLabel(kButtons[b]));
}

/* src's words into copy (cap UTF-16 units): its own with the controller's, the keys' with keys */
static void prompt_write(uint32_t src, uint32_t copy, uint32_t cap, int keys)
{
    uint32_t m = 0;
    int joined = 0;  /* the last thing written was a key in brackets */
    for (uint32_t i = 0; m + 1 < cap; i++) {
        uint16_t c = MEM16(src + 2 * i);
        if (!c) break;
        int b = keys ? prompt_button(c) : -1;
        const char *name = b >= 0 ? app_ButtonKeyLabel(b) : "";
        if (!*name) {  /* not a button, or unbound: the button's picture */
            MEM16(copy + 2 * m++) = c;
            joined = 0;
            continue;
        }
        if (joined && m + 1 < cap) MEM16(copy + 2 * m++) = '/';
        if (m + 1 < cap) MEM16(copy + 2 * m++) = '[';
        for (int k = 0; name[k] && k < kLabelMax && m + 1 < cap; k++) MEM16(copy + 2 * m++) = (uint8_t)name[k];
        if (m + 1 < cap) MEM16(copy + 2 * m++) = ']';
        joined = 1;
    }
    MEM16(copy + 2 * m) = 0;
}

/* each Swap (hle_d3d8.cpp) and each lookup: the copies follow the device and the bindings */
void hpcos_PromptRefresh(void)
{
    char sig[256];
    prompt_sig(sig, sizeof(sig));
    if (!strcmp(sig, g_prompt_sig)) return;
    snprintf(g_prompt_sig, sizeof(g_prompt_sig), "%s", sig);
    int keys = strcmp(sig, "pad") != 0;
    for (int i = 0; i < g_prompt_n; i++) prompt_write(g_prompt[i].src, g_prompt[i].copy, g_prompt[i].cap, keys);
    fprintf(stderr, "[PROMPTS] %d prompt texts now %s\n", g_prompt_n, keys ? "name the keys" : "show the buttons");
}

/* the inventory's text lookups: the assign prompt (0xA1D09), the buttons beside the spell slots (0xA99F9, 0xA9A57),
 * the item, spell and chapter pages (0xAA353) */
static int icon_caller(uint32_t ret)
{
    return ret == 0x000A1D09u || ret == 0x000A99F9u || ret == 0x000A9A57u || ret == 0x000AA353u;
}

uint32_t hpcos_PromptText(uint32_t text, uint32_t ret)
{
    if (!text || icon_caller(ret)) return text;
    {   /* a legend: a button first (after any spaces) */
        uint32_t i = 0;
        while (i < 8 && MEM16(text + 2 * i) == ' ') i++;
        if (prompt_button(MEM16(text + 2 * i)) >= 0) return text;
    }
    uint32_t n = 0, buttons = 0;
    for (; n < 4096 && MEM16(text + 2 * n); n++) buttons += prompt_button(MEM16(text + 2 * n)) >= 0;
    if (!buttons) return text;
    hpcos_PromptRefresh();
    for (int i = 0; i < g_prompt_n; i++)
        if (g_prompt[i].src == text) return g_prompt[i].copy;
    /* each button may become "/[" + a name + "]" */
    uint32_t cap = n + buttons * (kLabelMax + 3) + 1;
    uint32_t copy = xbox_HeapAlloc(cap * 2, 16);
    if (!copy) return text;
    /* full: the oldest is no longer kept up to date (its copy stays where it is, for whoever holds it) */
    int slot = g_prompt_n < kPromptCopies ? g_prompt_n++ : (g_prompt_next++ % kPromptCopies);
    g_prompt[slot].src = text;
    g_prompt[slot].copy = copy;
    g_prompt[slot].cap = cap;
    prompt_write(text, copy, cap, strcmp(g_prompt_sig, "pad") != 0);
    return copy;
}
