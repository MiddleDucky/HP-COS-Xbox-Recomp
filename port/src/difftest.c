/**
 * Differential test of the recompiled code against the original x86 (HPCOS_DIFFTEST build).
 *
 * Every direct and indirect call goes through RECOMP_ABI_CALL, which in this build asks recomp_difftest_pre whether
 * to test the target. A test runs the guest function's original bytes in Unicorn first, from the live guest state
 * (registers, x87 stack, the memory it touches, copied page by page as Unicorn reaches it), then lets the lifted C
 * run as usual, and compares the two: general registers, the x87 stack and every byte on the pages Unicorn touched.
 * A difference is a translation bug in the function or in something it calls.
 *
 * Only plain CPU code compares: a run that enters a function replaced by host code (HLE), a kernel import, MMIO or
 * anything that is not ordinary guest memory is dropped as "impure".
 *
 * F5 starts a sweep (each called function tested on up to kSamples calls) and F5 again ends it. Results go to
 * difftest.txt. unicorn.dll (Unicorn 2.1.4, GPLv2, from the PyPI win_amd64 wheel) is loaded at run time and must
 * sit next to the executable; this build is a debugging tool and is never shipped.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECOMP_GENERATED_CODE
#include "recomp/gen/recomp_types.h"

#include <unicorn/unicorn.h>

#include "difftest_hle.h"

extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3, g_xmm4, g_xmm5, g_xmm6, g_xmm7;

/* Unicorn, loaded by name */
static __typeof__(uc_open) *p_uc_open;
static __typeof__(uc_close) *p_uc_close;
static __typeof__(uc_mem_map) *p_uc_mem_map;
static __typeof__(uc_mem_map_ptr) *p_uc_mem_map_ptr;
static __typeof__(uc_mem_write) *p_uc_mem_write;
static __typeof__(uc_reg_write) *p_uc_reg_write;
static __typeof__(uc_reg_read) *p_uc_reg_read;
static __typeof__(uc_emu_start) *p_uc_emu_start;
static __typeof__(uc_emu_stop) *p_uc_emu_stop;
static __typeof__(uc_hook_add) *p_uc_hook_add;
static __typeof__(uc_strerror) *p_uc_strerror;

enum {
    kSamples = 12,                /* tests per function in a sweep */
    kMaxPages = 4096,             /* 16 MB of touched memory at most */
    kMaxInsns = 20000000,         /* a run longer than this is dropped */
    kGdt = 0xFFFF0000u,           /* where Unicorn's GDT lives (flash on the console: never plain memory) */
    kVaSlots = 0x400000,
};

typedef struct Page {
    uint32_t va;
    uint8_t *emu;                 /* Unicorn's copy: the original's result */
    uint8_t pre[4096];            /* the page before the call */
} Page;

static FILE *g_out;
static volatile LONG g_busy;
static int g_sweep, g_loaded;
static uint8_t *g_tried;          /* per guest va: tests started this sweep */
static unsigned g_tests, g_pure, g_bad, g_impure;

/* the state of one test between pre and post */
static Page *g_pages;
static unsigned g_npages;
static const char *g_impure_why;
static uint32_t g_impure_at;
static uint32_t g_in[8], g_ret;
static uint32_t g_out_regs[8];
static double g_out_fp[8];
static int g_out_top;
static unsigned g_active_va;

/* RECOMP_DIFF_PASS=1387A0,...: host wrappers that only add host work around the original body (Lock2DSurface's
 * render target sync) and so may be run through as plain code */
static int pass_through(uint32_t va)
{
    static uint32_t pass[16];
    static int n = -1;
    if (n < 0) {
        const char *e = getenv("RECOMP_DIFF_PASS");
        n = 0;
        while (e && *e && n < 16) {
            char *end;
            unsigned long v = strtoul(e, &end, 16);
            if (end == e) break;
            pass[n++] = (uint32_t)v;
            e = *end ? end + 1 : end;
        }
    }
    for (int i = 0; i < n; i++)
        if (pass[i] == va) return 1;
    return 0;
}

static int is_hle(uint32_t va)
{
    size_t lo = 0, hi = sizeof(k_hle_vas) / sizeof(k_hle_vas[0]);
    while (lo < hi) {
        size_t m = (lo + hi) / 2;
        if (k_hle_vas[m] == va) return va != 0x00034950u && !pass_through(va);  /* hle_debug.c's only forwards */
        if (k_hle_vas[m] < va) lo = m + 1; else hi = m;
    }
    return 0;
}

static int load_unicorn(void)
{
    if (g_loaded) return g_loaded > 0;
    HMODULE m = LoadLibraryA("unicorn.dll");
    g_loaded = -1;
    if (!m) {
        fprintf(stderr, "[DIFF] unicorn.dll not found next to the executable\n");
        return 0;
    }
#define LOAD(n) if (!(p_##n = (__typeof__(p_##n))(void *)GetProcAddress(m, #n))) return 0
    LOAD(uc_open); LOAD(uc_close); LOAD(uc_mem_map); LOAD(uc_mem_map_ptr); LOAD(uc_mem_write);
    LOAD(uc_reg_write); LOAD(uc_reg_read); LOAD(uc_emu_start); LOAD(uc_emu_stop); LOAD(uc_hook_add);
    LOAD(uc_strerror);
#undef LOAD
    g_tried = calloc(kVaSlots, 1);
    g_pages = calloc(kMaxPages, sizeof(Page));
    if (!g_tried || !g_pages) return 0;
    g_loaded = 1;
    return 1;
}

/* plain, readable guest memory: not MMIO, not a kernel thunk, not a hole */
static int plain_page(uint32_t va)
{
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery((void *)XBOX_PTR(va), &mi, sizeof(mi))) return 0;
    if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return 0;
    return (mi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                          PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)) != 0;
}

static void impure(uc_engine *uc, const char *why, uint32_t at)
{
    if (!g_impure_why) {
        g_impure_why = why;
        g_impure_at = at;
    }
    p_uc_emu_stop(uc);
}

static bool on_unmapped(uc_engine *uc, uc_mem_type type, uint64_t address, int size, int64_t value, void *user)
{
    (void)value; (void)user;
    uint32_t first = (uint32_t)address & ~0xFFFu, last = ((uint32_t)address + (uint32_t)(size ? size - 1 : 0)) & ~0xFFFu;
    for (uint32_t va = first;; va += 0x1000) {
        if (g_npages >= kMaxPages) { impure(uc, "too much memory", (uint32_t)address); return false; }
        if (!plain_page(va)) {
            impure(uc, type == UC_MEM_FETCH_UNMAPPED ? "jumps out of guest code (kernel/thunk)" : "not plain memory",
                   (uint32_t)address);
            return false;
        }
        Page *p = &g_pages[g_npages];
        if (!p->emu) p->emu = VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!p->emu) { impure(uc, "out of host memory", va); return false; }
        p->va = va;
        memcpy(p->pre, (void *)XBOX_PTR(va), 0x1000);
        memcpy(p->emu, p->pre, 0x1000);
        if (p_uc_mem_map_ptr(uc, va, 0x1000, UC_PROT_ALL, p->emu) != UC_ERR_OK) {
            impure(uc, "map failed", va);
            return false;
        }
        g_npages++;
        if (va == last) break;
    }
    return true;
}

static void on_block(uc_engine *uc, uint64_t address, uint32_t size, void *user)
{
    (void)size; (void)user;
    if (is_hle((uint32_t)address)) impure(uc, "calls host code (HLE)", (uint32_t)address);
}

static void on_insn_io(uc_engine *uc, uint32_t port, int size, void *user)
{
    (void)size; (void)user;
    impure(uc, "port I/O", port);
}

static void fp_to80(double d, uint8_t out[10])
{
    long double x = d;
    memcpy(out, &x, 10);
}

static double fp_from80(const uint8_t in[10])
{
    long double x = 0;
    memcpy(&x, in, 10);
    return (double)x;
}

/* the FS segment: a data descriptor based at the thread's fake TIB */
static int set_fs(uc_engine *uc)
{
    uint64_t gdt[4] = {0};
    uint32_t base = g_fs_base;
    gdt[2] = 0x00CF92000000FFFFull;  /* flat 4 GB data, 32-bit, ring 0: SS */
    gdt[3] = 0xFFFFull | ((uint64_t)(base & 0xFFFFFF) << 16) | (0x92ull << 40) | (0xCFull << 48) |
             ((uint64_t)(base >> 24) << 56);
    if (p_uc_mem_map(uc, kGdt, 0x1000, UC_PROT_READ | UC_PROT_WRITE) != UC_ERR_OK) return 0;
    if (p_uc_mem_write(uc, kGdt, gdt, sizeof(gdt)) != UC_ERR_OK) return 0;
    uc_x86_mmr gdtr = {0, kGdt, sizeof(gdt) - 1, 0};
    if (p_uc_reg_write(uc, UC_X86_REG_GDTR, &gdtr) != UC_ERR_OK) return 0;
    int fs = 3 << 3;
    if (p_uc_reg_write(uc, UC_X86_REG_FS, &fs) != UC_ERR_OK) return 0;
    /* SS flat and 32-bit too: left at Unicorn's default, push and ret used SP, the low 16 bits of esp, and every
     * run returned to address 0 */
    int ss = 2 << 3;
    return p_uc_reg_write(uc, UC_X86_REG_SS, &ss) == UC_ERR_OK;
}

static const int k_regs[8] = {UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
                              UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI};
static const char *const k_names[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};

/* run the original from the current state; 1 when the run is comparable */
static int emulate(uint32_t va)
{
    uc_engine *uc;
    uc_hook h;
    g_npages = 0;
    g_impure_why = NULL;
    if (p_uc_open(UC_ARCH_X86, UC_MODE_32, &uc) != UC_ERR_OK) { g_impure_why = "uc_open failed"; return 0; }
    uint32_t in[8] = {g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi};
    memcpy(g_in, in, sizeof(in));
    g_ret = MEM32(g_esp);
    for (int i = 0; i < 8; i++) p_uc_reg_write(uc, k_regs[i], &in[i]);
    uint32_t eflags = 0x202 | (g_df ? 0x400 : 0);
    p_uc_reg_write(uc, UC_X86_REG_EFLAGS, &eflags);
    for (int i = 0; i < 8; i++) {
        uint8_t f[10];
        fp_to80(g_fp_stack[i], f);
        p_uc_reg_write(uc, UC_X86_REG_FP0 + i, f);
    }
    int sw = ((g_fp_top & 7) << 11) | (g_fp_cc & 0x4700), cw = g_fp_control_word ? g_fp_control_word : 0x037F, tag = 0;
    p_uc_reg_write(uc, UC_X86_REG_FPSW, &sw);
    p_uc_reg_write(uc, UC_X86_REG_FPCW, &cw);
    p_uc_reg_write(uc, UC_X86_REG_FPTAG, &tag);
    RecompXmm *xmm[8] = {&g_xmm0, &g_xmm1, &g_xmm2, &g_xmm3, &g_xmm4, &g_xmm5, &g_xmm6, &g_xmm7};
    for (int i = 0; i < 8; i++) p_uc_reg_write(uc, UC_X86_REG_XMM0 + i, xmm[i]);
    if (!set_fs(uc)) { g_impure_why = "FS/GDT setup failed"; g_impure_at = g_fs_base; p_uc_close(uc); return 0; }
    p_uc_hook_add(uc, &h, UC_HOOK_MEM_UNMAPPED, (void *)on_unmapped, NULL, 1, 0);
    p_uc_hook_add(uc, &h, UC_HOOK_BLOCK, (void *)on_block, NULL, 1, 0);
    p_uc_hook_add(uc, &h, UC_HOOK_INSN, (void *)on_insn_io, NULL, 1, 0, UC_X86_INS_IN);
    p_uc_hook_add(uc, &h, UC_HOOK_INSN, (void *)on_insn_io, NULL, 1, 0, UC_X86_INS_OUT);
    uc_err e = p_uc_emu_start(uc, va, g_ret, 0, kMaxInsns);
    uint32_t eip = 0;
    p_uc_reg_read(uc, UC_X86_REG_EIP, &eip);
    if (!g_impure_why && e != UC_ERR_OK) { g_impure_why = p_uc_strerror(e); g_impure_at = eip; }
    if (!g_impure_why && eip != g_ret) { g_impure_why = "did not return (too long)"; g_impure_at = eip; }
    for (int i = 0; i < 8; i++) p_uc_reg_read(uc, k_regs[i], &g_out_regs[i]);
    p_uc_reg_read(uc, UC_X86_REG_FPSW, &sw);
    g_out_top = (sw >> 11) & 7;
    for (int i = 0; i < 8; i++) {
        uint8_t f[10];
        p_uc_reg_read(uc, UC_X86_REG_FP0 + i, f);
        g_out_fp[i] = fp_from80(f);
    }
    p_uc_close(uc);
    return g_impure_why == NULL;
}

static int near_float(uint32_t a, uint32_t b)
{
    float x, y;
    memcpy(&x, &a, 4);
    memcpy(&y, &b, 4);
    if (!isfinite(x) || !isfinite(y)) return 0;
    float m = fmaxf(fabsf(x), fabsf(y));
    return fabsf(x - y) <= 1e-5f * m + 1e-30f;
}

static void report_head(uint32_t va, int *first)
{
    if (!*first) return;
    *first = 0;
    g_bad++;
    fprintf(g_out, "MISMATCH sub_%08X (return %08X) in: eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X "
            "esi=%08X edi=%08X\n", va, g_ret, g_in[0], g_in[1], g_in[2], g_in[3], g_in[4], g_in[5], g_in[6], g_in[7]);
}

static void compare(uint32_t va)
{
    int first = 1;
    uint32_t lifted[8] = {g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi};
    for (int i = 0; i < 8; i++) {
        if (i == 5) continue;  /* ebp is a local in each lifted function */
        if (lifted[i] != g_out_regs[i]) {
            report_head(va, &first);
            fprintf(g_out, "  %s lifted %08X original %08X\n", k_names[i], lifted[i], g_out_regs[i]);
        }
    }
    /* RECOMP_DIFF_NO_X87=1: x87 left out. MMX code leaves NaNs in the x87 registers it shares on the console and
     * not in the lifted code, which keeps MMX apart: every MMX function differs there, harmlessly */
    static int no_x87 = -1;
    if (no_x87 < 0) no_x87 = getenv("RECOMP_DIFF_NO_X87") != NULL;
    if (no_x87) {
    } else if ((g_fp_top & 7) != g_out_top) {
        report_head(va, &first);
        fprintf(g_out, "  x87 top lifted %d original %d\n", g_fp_top & 7, g_out_top);
    } else {
        /* st0 and st1 carry results; deeper slots are the caller's and untouched */
        for (int i = 0; i < 2; i++) {
            int r = (g_out_top + i) & 7;
            double a = g_fp_stack[r], b = g_out_fp[r];
            if (!(fabs(a - b) <= 1e-5 * fmax(fabs(a), fabs(b)) + 1e-30) && !(isnan(a) && isnan(b))) {
                report_head(va, &first);
                fprintf(g_out, "  st%d lifted %.9g original %.9g\n", i, a, b);
            }
        }
    }
    unsigned shown = 0, quiet = 0;
    uint32_t sp = g_out_regs[4];
    for (unsigned k = 0; k < g_npages; k++) {
        Page *p = &g_pages[k];
        const uint8_t *live = (const uint8_t *)XBOX_PTR(p->va);
        if (!memcmp(live, p->emu, 0x1000)) continue;
        for (uint32_t o = 0; o < 0x1000; o += 4) {
            uint32_t a, b, pre;
            memcpy(&a, live + o, 4);
            memcpy(&b, p->emu + o, 4);
            memcpy(&pre, p->pre + o, 4);
            if (a == b || near_float(a, b)) continue;
            uint32_t at = p->va + o;
            if (at < sp) continue;           /* below the final esp: dead stack */
            if (b == pre) { quiet++; continue; }  /* only the lifted side (or another thread) changed it */
            report_head(va, &first);
            if (shown++ < 12)
                fprintf(g_out, "  [%08X] lifted %08X original %08X (before %08X)\n", at, a, b, pre);
        }
    }
    if (!first && quiet) fprintf(g_out, "  (+%u words changed only on the lifted side)\n", quiet);
    if (!first) fflush(g_out);
}

/* RECOMP_DIFF_AUTO=start,seconds: a sweep without the key, start seconds after the first call */
static int auto_toggle(DWORD now)
{
    static int state = -1;
    static DWORD t0, at, len;
    if (state < 0) {
        const char *e = getenv("RECOMP_DIFF_AUTO");
        unsigned a = 0, l = 0;
        state = e && sscanf(e, "%u,%u", &a, &l) == 2 ? 0 : 2;
        t0 = now; at = a * 1000; len = l * 1000;
    }
    if (state == 0 && now - t0 >= at) { state = 1; return 1; }
    if (state == 1 && now - t0 >= at + len) { state = 2; return 1; }
    return 0;
}

static void sweep_toggle(void);

static void sweep_key(DWORD now)
{
    if (!(GetAsyncKeyState(VK_F5) & 1) && !auto_toggle(now)) return;
    sweep_toggle();
}

/* a sweep started or ended from host code (hle_video.c: RECOMP_DIFF_XMV, a range of one movie's frames) */
void recomp_difftest_sweep(int on)
{
    if (!!on != !!g_sweep) sweep_toggle();
}

static void sweep_toggle(void)
{
    if (!load_unicorn()) return;
    g_sweep = !g_sweep;
    if (g_sweep) {
        memset(g_tried, 0, kVaSlots);
        g_tests = g_pure = g_bad = g_impure = 0;
        if (!g_out) g_out = fopen("difftest.txt", "w");
        fprintf(stderr, "[DIFF] sweep started: testing every called function against the original x86\n");
        if (g_out) fprintf(g_out, "# sweep start\n");
    } else {
        fprintf(stderr, "[DIFF] sweep done: %u tests, %u comparable, %u mismatches, %u impure -> difftest.txt\n",
                g_tests, g_pure, g_bad, g_impure);
        if (g_out) { fprintf(g_out, "# sweep end: %u tests, %u comparable, %u mismatches\n", g_tests, g_pure, g_bad); fflush(g_out); }
    }
}

/* RECOMP_DIFF_ONLY=57040,2F260,14E000-157000,...: test only these functions (hex), or those in a range, on up to
 * kOnlySamples calls each; RECOMP_DIFF_SAMPLES=n another limit, 0 every call (a decoder's rare path) */
enum { kOnlySamples = 250 };
static uint32_t g_only[64], g_only_end[64];
static int g_nonly = -1;
static unsigned g_only_samples = kOnlySamples;

static int only_pick(uint32_t va)
{
    if (g_nonly < 0) {
        g_nonly = 0;
        const char *e = getenv("RECOMP_DIFF_ONLY");
        const char *n = getenv("RECOMP_DIFF_SAMPLES");
        if (n && *n) g_only_samples = (unsigned)strtoul(n, NULL, 0);
        while (e && *e && g_nonly < 64) {
            char *end;
            unsigned long v = strtoul(e, &end, 16), w = v;
            if (end == e) break;
            if (*end == '-') w = strtoul(end + 1, &end, 16);
            g_only[g_nonly] = (uint32_t)v;
            g_only_end[g_nonly++] = (uint32_t)w;
            e = *end ? end + 1 : end;
        }
    }
    if (!g_nonly) return -1;
    for (int i = 0; i < g_nonly; i++)
        if (va >= g_only[i] && va <= g_only_end[i]) return 1;
    return 0;
}

int recomp_difftest_pre(uint32_t va)
{
    static DWORD last_key;
    DWORD now = GetTickCount();
    if (now - last_key > 50) { last_key = now; sweep_key(now); }
    if (!g_sweep || va >= kVaSlots || is_hle(va)) return 0;
    int only = only_pick(va);
    if (only == 0) return 0;
    if (only > 0 ? g_only_samples && g_tried[va] >= g_only_samples : g_tried[va] >= kSamples) return 0;
    if (InterlockedCompareExchange(&g_busy, 1, 0)) return 0;
    if (g_tried[va] < 255) g_tried[va]++;
    g_tests++;
    if (!emulate(va)) {
        g_impure++;
        if (g_out && g_impure <= 400)
            fprintf(g_out, "impure sub_%08X: %s at %08X\n", va, g_impure_why ? g_impure_why : "?", g_impure_at);
        InterlockedExchange(&g_busy, 0);
        return 0;
    }
    g_pure++;
    g_active_va = va;
    return 1;  /* g_busy stays set: nested calls inside the lifted run are not tested */
}

void recomp_difftest_post(uint32_t va)
{
    if (g_out && va == g_active_va) compare(va);
    InterlockedExchange(&g_busy, 0);
}
