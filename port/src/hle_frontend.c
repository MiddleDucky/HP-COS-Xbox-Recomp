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

/* text draw (this = font; x, y, text) */
void sub_00033D40(void)
{
    if (g_in_menu && g_ecx == kFont && ARG(0) == kRowX && title_row_y(ARG(1))) {
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
    if (!enabled() || ARG(0) != kMsgFrame || MEM16(kFrontState) != 4) {
        /* the handler's other messages (0x10000004 and the rest) come every frame too: they leave the row alone */
        sub_000FFE60_gen();
        return;
    }
    if (MEM16(kScreen) != kTitleMenu) {
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

/* ── UK English ─────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * sub_00011290 turns the console's language into the title's language mask [0x51E06C] (1 UK English, 2 USA English,
 * 4 French, 8 German, 0x10 Spanish, 0x20 Italian; sub_0004A4B0 picks the language index from it): English is UK, made
 * USA when the game's region is North America, as this disc's is. The launcher's "English (UK)" (RECOMP_LANGUAGE
 * english_uk; the kernel answers English for it) keeps UK: the _UK folder's text and speech.
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
}

/* ── Key names in the game's prompts ────────────────────────────────────────────────────────────────────────────
 *
 * The text tables write a button as a character of its own, drawn from the font's picture of it (panel.geo's glyph
 * table at 0x6CAC4): 0x1701 A, 0x1702 X, 0x1703 B, 0x1704 Y, 0x1705 the left trigger, 0x1706 the right trigger,
 * 0x1707 BACK ("... by pressing BACK": the Remembrall); 0x1710 is a hand, not a button. With key prompts on
 * (app_KeyboardPrompts: F10 Controls, Button prompts) the text lookup's string comes back with the bound key's name
 * in their place - "PRESS Space TO CONTINUE", "equip to Mouse 5, E or Right Click". Each changed string is copied
 * once per set of key names to the game's heap; the game's own tables are left as they are. */
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

uint32_t hpcos_PromptText(uint32_t text)
{
    if (!text || !app_KeyboardPrompts()) return text;
    uint32_t n = 0;
    int any = 0;
    for (; n < 4096 && MEM16(text + 2 * n); n++) any |= prompt_button(MEM16(text + 2 * n)) >= 0;
    if (!any) return text;
    /* the key names now, as one string: a rebinding makes new copies */
    char sig[256];
    int sl = 0;
    static const int kButtons[] = {0, 1, 2, 3, 6, 7, 13};
    for (unsigned b = 0; b < sizeof(kButtons) / sizeof(kButtons[0]); b++)
        sl += snprintf(sig + sl, sizeof(sig) - (size_t)sl, "%s|", app_ButtonKeyLabel(kButtons[b]));
    enum { kCache = 128 };
    static struct { uint32_t src, copy; char sig[256]; } cache[kCache];
    static int next;
    for (int i = 0; i < kCache; i++)
        if (cache[i].src == text && !strcmp(cache[i].sig, sig)) return cache[i].copy;
    uint16_t out[4096 + 512];
    uint32_t m = 0;
    for (uint32_t i = 0; i < n && m < sizeof(out) / 2 - 40; i++) {
        uint16_t c = MEM16(text + 2 * i);
        int b = prompt_button(c);
        if (b < 0) {
            out[m++] = c;
            continue;
        }
        const char *name = app_ButtonKeyLabel(b);
        if (!*name) {
            out[m++] = c;  /* unbound: the button's picture */
            continue;
        }
        for (const char *p = name; *p && m < sizeof(out) / 2 - 2; p++) out[m++] = (uint8_t)*p;
    }
    out[m++] = 0;
    uint32_t copy = xbox_HeapAlloc(m * 2, 16);
    if (!copy) return text;
    for (uint32_t i = 0; i < m; i++) MEM16(copy + 2 * i) = out[i];
    cache[next].src = text;
    cache[next].copy = copy;
    snprintf(cache[next].sig, sizeof(cache[next].sig), "%s", sig);
    next = (next + 1) % kCache;
    return copy;
}
