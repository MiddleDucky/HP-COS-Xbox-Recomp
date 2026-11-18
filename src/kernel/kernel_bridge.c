/**
 * kernel_bridge.c - Bridge between translated game code and kernel functions
 *
 * Problem:
 *   Translated game code calls kernel functions via indirect calls through
 *   the kernel thunk table at VA 0x0036B7C0. In the XBE file, these entries
 *   contain unresolved ordinals (0x80000000 | ordinal). On real Xbox hardware,
 *   the kernel loader replaces these with actual function pointers before the
 *   game runs.
 *
 * Solution:
 *   1. After xbox_MemoryLayoutInit copies .rdata, call xbox_kernel_bridge_init()
 *   2. Replace each ordinal entry in Xbox memory with a synthetic VA
 *   3. When RECOMP_ICALL encounters a synthetic VA, route it to a per-ordinal
 *      bridge function that reads args from the simulated Xbox stack, translates
 *      pointer arguments from Xbox VA→native, and calls the kernel function.
 *
 * Synthetic VA scheme:
 *   Each thunk slot i gets VA 0xFE000000 + i*4
 *   The lookup function checks this range and dispatches appropriately.
 *
 * Why per-ordinal bridges instead of a generic trampoline:
 *   Kernel functions receive Xbox pointers (32-bit VAs) that must be translated
 *   to native pointers by adding g_xbox_mem_offset. Different functions have
 *   different parameter layouts (pointer vs value), so each needs its own bridge.
 */

#include "kernel.h"
#include "guest_vmem.h"
#include "guest_vad.h"
#include "xbox_memory_layout.h"
#include "recomp_icall_feedback.h"
#ifdef _WIN32
#include <mmsystem.h>   /* timeBeginPeriod, for the timer thread */
#endif
#include <stdio.h>
/* stdlib.h is load-bearing, not tidiness. Without it C89 implicit declaration
 * makes malloc return `int`, so bridge_spawn_thread truncated its heap pointer
 * to 32 bits and sign-extended it into a `struct bridge_thread_start *`. Every
 * subsequent s->field wrote to an address that had nothing to do with the
 * allocation. MSVC says so (C4013 + C4047 "differs in levels of indirection")
 * but only as warnings, and this file is compiled with /W4 /WX-. */
#include <stdlib.h>
#include <float.h>
#include <xmmintrin.h>
/* Section B string helpers: wcslen helper, case folding.
 * ctype.h/wctype.h are not pulled in by the platform headers on either host. */
#include <string.h>
#include <ctype.h>
#include <wctype.h>

/* Access to recompiled code registers. Per-thread: RECOMP_TLS comes from
 * xbox_memory_layout.h and must match the definitions there -- a plain extern
 * here binds to the TLS template rather than the calling thread's copy, which
 * reads as every register being zero. */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi, g_ebp;
extern uint32_t g_xbox_code_lo, g_xbox_code_hi;
extern RECOMP_TLS uint32_t g_seh_ebp;
extern ptrdiff_t g_xbox_mem_offset;

/* Dispatch table lookup (for function pointer args) */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
/* Timer DPCs run on a host thread, which needs a guest stack under it. */
int  xbox_worker_stack_alloc(void);

recomp_func_t recomp_lookup_manual(uint32_t xbox_va);

/* Memory access - same as recomp_types.h MEM32 but without the #define guard */
#define BRIDGE_MEM32(addr) (*(volatile uint32_t *)((uintptr_t)(addr) + g_xbox_mem_offset))

/* Translate Xbox VA to native pointer (NULL-safe: 0 → NULL) */
#define XBOX_TO_NATIVE(va) ((va) ? (void*)((uintptr_t)(va) + g_xbox_mem_offset) : NULL)

/* ── Guest buffers the host is about to touch ───────────
 *
 * A bridge turns a guest VA into a host pointer by adding an offset, so a VA
 * the guest got wrong does not fail the call -- it faults inside the kernel
 * implementation, on a host stack with no recompiled frame in it and a fault
 * address that means nothing on its own.
 *
 * The dangerous shape is an address AND a length that both come from the
 * guest. NtReadFile is the clearest case: the host WRITES `length` bytes
 * through the pointer, so a buffer near the top of the mapping, or a length
 * that does not match the buffer it names, walks the host past the end of
 * guest memory writing file contents into whatever follows. Nothing above this
 * layer can catch it, because xbox_NtReadFile receives a host pointer and a
 * count and cannot know where the mapping ends.
 */
static int bridge_va_mapped(uint32_t va, uint32_t bytes)
{
    uint64_t end = (uint64_t)va + bytes;
    uint64_t mapped = g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram;

    if (va < 0x1000)            /* page zero is deliberately unmapped */
        return 0;
    if (end <= mapped)
        return 1;
    return va >= XBOX_CONTIG_BASE
        && end <= (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE;
}

/* STATUS_ACCESS_VIOLATION is what NT answers for a user buffer it cannot
 * touch, and it is far more useful to a title than a host crash: the call
 * fails, the guest gets a status it has a branch for, and the log names the
 * export, the buffer and the length. Warned once per export so a title that
 * does this in a loop does not bury the rest of the log. */
static int bridge_buf_ok(uint32_t va, uint32_t bytes, const char *export_name)
{
    static const char *seen[16];
    static int distinct;
    int i;

    if (!bytes)                                   /* nothing is accessed */
        return 1;
    if (va && bridge_va_mapped(va, bytes))
        return 1;

    for (i = 0; i < distinct; ++i)
        if (seen[i] == export_name)
            return 0;
    if (distinct < (int)(sizeof(seen) / sizeof(seen[0])))
        seen[distinct++] = export_name;

    fprintf(stderr,
            "  [KERNEL] %s: buffer 0x%08X length %u is not mapped guest "
            "memory; returning STATUS_ACCESS_VIOLATION\n",
            export_name, va, (unsigned)bytes);
    fflush(stderr);
    return 0;
}

/* ── Synthetic VA range (for function exports) ─────────── */

#define KERNEL_VA_BASE  0xFE000000u
#define KERNEL_VA_END   (KERNEL_VA_BASE + XBOX_KERNEL_THUNK_TABLE_SIZE * 4)

/* ── Kernel data exports ──────────────────────────────────
 *
 * Some kernel ordinals are DATA exports (structs/variables), not functions.
 * The game reads their thunk entries and dereferences the result to access
 * the data. These cannot use synthetic VAs — they must point to real,
 * dereferenceable addresses in the Xbox VA space.
 *
 * We allocate a "kernel data area" at XBOX_KERNEL_DATA_BASE and populate
 * it with the expected structures.
 */

#define BRIDGE_MEM16(addr) (*(volatile uint16_t *)((uintptr_t)(addr) + g_xbox_mem_offset))
#define BRIDGE_MEM8(addr)  (*(volatile uint8_t  *)((uintptr_t)(addr) + g_xbox_mem_offset))

/**
 * Get the Xbox VA of data for a kernel DATA export ordinal.
 * Returns 0 if the ordinal is not a data export (i.e., it's a function).
 */
static uint32_t kernel_data_va_for_ordinal(ULONG ordinal)
{
    /* Ordinals here are checked BEFORE function routing (see the thunk build
     * loop), so an ordinal listed by mistake turns a real kernel function into
     * a data address -- the title then calls it and jumps into kernel data.
     *
     * This table had a whole block shifted. 17 (ExFreePool), 65
     * (IoCreateDevice), 327 (XeLoadSection) and 328 (XeUnloadSection) are all
     * functions and were all being handed data addresses; Crimson Skies imports
     * every one of them. In the other direction, the genuine exports at 16, 353,
     * 354, 355, 356 and 357 got no thunk at all, so a title reading
     * XboxLANKey or KeTimeIncrement read whatever the function fallback left.
     *
     * test_bridge_ordinals.py now checks every entry below against the export
     * table, which is why the block cannot drift again unnoticed. */
    switch (ordinal) {
    case  16: return XBOX_KERNEL_DATA_BASE + KDATA_EVENT_OBJ_TYPE;
    case  22: return XBOX_KERNEL_DATA_BASE + KDATA_MUTANT_OBJ_TYPE;
    case  30: return XBOX_KERNEL_DATA_BASE + KDATA_SEMAPHORE_OBJ_TYPE;
    case  31: return XBOX_KERNEL_DATA_BASE + KDATA_TIMER_OBJ_TYPE;
    case  40: return XBOX_KERNEL_DATA_BASE + KDATA_DISK_CACHE_PARTS;
    case  41: return XBOX_KERNEL_DATA_BASE + KDATA_DISK_MODEL_STR;
    case  42: return XBOX_KERNEL_DATA_BASE + KDATA_DISK_SERIAL_STR;
    case  64: return XBOX_KERNEL_DATA_BASE + KDATA_IO_COMPLETION_TYPE;
    case  70: return XBOX_KERNEL_DATA_BASE + KDATA_IO_DEVICE_TYPE;
    case  71: return XBOX_KERNEL_DATA_BASE + KDATA_FILE_OBJ_TYPE;
    case 156: return XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT;
    case 157: return XBOX_KERNEL_DATA_BASE + KDATA_TIME_INCREMENT;
    case 164: return XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE;
    case 259: return XBOX_KERNEL_DATA_BASE + KDATA_THREAD_OBJ_TYPE;
    case 322: return XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO;
    case 323: return XBOX_KERNEL_DATA_BASE + KDATA_HD_KEY;
    case 324: return XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION;
    case 325: return XBOX_KERNEL_DATA_BASE + KDATA_SIGNATURE_KEY;
    case 326: return XBOX_KERNEL_DATA_BASE + KDATA_XE_IMAGE_FILENAME;
    case 353: return XBOX_KERNEL_DATA_BASE + KDATA_LAN_KEY;
    case 354: return XBOX_KERNEL_DATA_BASE + KDATA_ALT_SIGNATURE_KEYS;
    case 355: return XBOX_KERNEL_DATA_BASE + KDATA_XE_PUBLIC_KEY;
    case 356: return XBOX_KERNEL_DATA_BASE + KDATA_BOOT_SMC_VIDEO;
    case 357: return XBOX_KERNEL_DATA_BASE + KDATA_IDEX_CHANNEL;
    default:  return 0;  /* Not a data export */
    }
}

/**
 * Initialize kernel data export values at the kernel data area.
 * Called during bridge init, after Xbox memory is mapped.
 */
static void kernel_data_init(void)
{
    /* XboxHardwareInfo (ordinal 322) - XBOX_HARDWARE_INFO
     *   +0: ULONG Flags (0 = retail, 0x20 = devkit)
     *   +4: UCHAR GpuRevision
     *   +5: UCHAR McpRevision
     */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 0) = 0;   /* Retail */
    BRIDGE_MEM8(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 4) = 0xA1; /* NV2A A1 */
    BRIDGE_MEM8(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 5) = 0xB1; /* MCPX B1 */

    /* XboxKrnlVersion (ordinal 324) - XBOX_KRNL_VERSION
     *   +0: USHORT Major (1)
     *   +2: USHORT Minor (0)
     *   +4: USHORT Build (5849 = XDK version)
     *   +6: USHORT Qfe (0)
     */
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 0) = 1;
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 2) = 0;
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 4) = 5849;
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 6) = 0;

    /* KeTickCount (ordinal 156) - initialized to current tick count.
     * A background thread in main.c updates this every ~1ms. */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT) = xbox_TickCountMs();

    /* LaunchDataPage (ordinal 164).
     *
     * This is how a title receives a command line. XGetLaunchInfo reads the
     * page, takes LaunchDataType from its first dword, and copies 3072 bytes
     * from +0x400; a type of 3 means those bytes *are* the command line.
     * Half-Life 2 does exactly that in sub_00596710, and falls back to the
     * empty string at 0x00772EA7 when the call fails -- which is what a NULL
     * page produces, so the engine started with no arguments and no map.
     *
     * On hardware the launcher fills this in before rebooting into the title.
     * RECOMP_CMDLINE does the same thing here, so a title can be told to load
     * a level the way the console would tell it: "+map intro".
     */
    {
        const char *cmdline = getenv("RECOMP_CMDLINE");

        if (cmdline && *cmdline) {
            uint32_t page = xbox_HeapAlloc(0x1000 + 0x0C00, 4096);
            if (page) {
                size_t n = strlen(cmdline);
                size_t i;

                if (n > 0x0BFF)
                    n = 0x0BFF;
                BRIDGE_MEM32(page + 0) = 3;          /* LaunchDataType */
                BRIDGE_MEM32(page + 4) = 0x45410091; /* title id */
                for (i = 0; i < n; i++)
                    BRIDGE_MEM8(page + 0x400 + (uint32_t)i) =
                        (uint8_t)cmdline[i];
                BRIDGE_MEM8(page + 0x400 + (uint32_t)n) = 0;

                BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE) =
                    page;
                fprintf(stderr, "  Launch data: type 3 at 0x%08X, "
                                "command line %s\n", page, cmdline);
            } else {
                BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE) = 0;
            }
        } else {
            BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE) = 0;
        }
    }

    /* Object-type exports. Each gets a DISTINCT non-zero value rather than 0.
     *
     * They were all zero, which is wrong twice over: a title that null-checks
     * one sees "no such type", and a title that distinguishes two of them --
     * ObReferenceObjectByHandle takes an expected type and compares it -- sees
     * every type as equal, so a mutant handle passes a check meant for events.
     * The values are opaque to the game; only identity and non-nullness matter,
     * so they are the export ordinal offset into the kernel data area, which
     * also makes a stray one recognisable in a crash dump.
     */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_THREAD_OBJ_TYPE)    = XBOX_KERNEL_DATA_BASE + KDATA_THREAD_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_EVENT_OBJ_TYPE)     = XBOX_KERNEL_DATA_BASE + KDATA_EVENT_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_MUTANT_OBJ_TYPE)    = XBOX_KERNEL_DATA_BASE + KDATA_MUTANT_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_SEMAPHORE_OBJ_TYPE) = XBOX_KERNEL_DATA_BASE + KDATA_SEMAPHORE_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_TIMER_OBJ_TYPE)     = XBOX_KERNEL_DATA_BASE + KDATA_TIMER_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_FILE_OBJ_TYPE)      = XBOX_KERNEL_DATA_BASE + KDATA_FILE_OBJ_TYPE;

    /* KeTimeIncrement (ordinal 157) - 100ns units per clock tick. 0x2710 is
     * 1 ms, which is what KeTickCount above is updated at. A title dividing by
     * this to convert ticks to time gets a division by zero if it is left 0. */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_TIME_INCREMENT) = 0x2710;

    /* HalBootSMCVideoMode (ordinal 356) - SMC video mode word from boot. 0 is
     * "no video mode reported", which titles treat as auto-detect. */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_BOOT_SMC_VIDEO) = 0;

    /* IdexChannelObject is a structure, not an opaque pointer. Guest file-close
     * code walks DeviceQueue.DeviceListHead at +0x28. Host-backed synchronous
     * I/O does not enqueue guest IRPs, so this must be an empty circular list.
     * Reserve separate storage: the old 16-byte slot overlapped the key exports. */
    {
        uint32_t channel=XBOX_KERNEL_DATA_BASE + KDATA_IDEX_CHANNEL;
        memset(XBOX_TO_NATIVE(channel),0,0x200);
        BRIDGE_MEM32(channel+0x28)=channel+0x28;
        BRIDGE_MEM32(channel+0x2C)=channel+0x28;
    }

    /* HalDiskCachePartitionCount (ordinal 40) - number of cache partitions.
     * Retail consoles report 3 (X, Y, Z). Titles size a partition array from
     * this, so 0 gives a zero-length array and 1 hides two drives. */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_DISK_CACHE_PARTS) = 3;

    /* IoCompletionObjectType (ordinal 64) - type object */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_IO_COMPLETION_TYPE) = 0;

    /* IoDeviceObjectType (ordinal 71) - type object (stub: 0) */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_IO_DEVICE_TYPE) = 0;

    /* XboxHDKey (ordinal 323) - 16 bytes of zeros (no key) */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_HD_KEY) + g_xbox_mem_offset), 0, 16);

    /* XboxSignatureKey (ordinal 325) - 16 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_SIGNATURE_KEY) + g_xbox_mem_offset), 0, 16);

    /* XboxLANKey (ordinals 326, 355) - 16 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_LAN_KEY) + g_xbox_mem_offset), 0, 16);

    /* XboxAlternateSignatureKeys (ordinals 327, 356) - 256 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_ALT_SIGNATURE_KEYS) + g_xbox_mem_offset), 0, 256);

    /* XePublicKeyData (ordinal 357) - 284 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_XE_PUBLIC_KEY) + g_xbox_mem_offset), 0, 284);

    /* HAL disk identity strings (ordinals 41/42). The exported symbol is an
     * XBOX_ANSI_STRING whose Buffer must be an Xbox VA the title can deref --
     * HalRandGather reads the bytes for entropy. Build the struct and its text
     * inside the kernel data area so both are addressable. */
    {
        struct { uint32_t str_off, buf_off; const char *text; } d[] = {
            { KDATA_DISK_MODEL_STR,  KDATA_DISK_MODEL_BUF,  "XBOXRECOMP VIRTUAL HDD" },
            { KDATA_DISK_SERIAL_STR, KDATA_DISK_SERIAL_BUF, "XR0000000000" },
            /* XeImageFileName (ordinal 326). Declared but never filled in, so
             * its Buffer held whatever was in the page -- Half-Life 2's CRT
             * reads it while working out the running image's path, took the
             * uninitialised bytes as a char*, and dereferenced 0x68737572
             * (the ASCII "rush"). A disc-booted title's value looks like this. */
            { KDATA_XE_IMAGE_FILENAME, KDATA_XE_IMAGE_BUF,
              "\\Device\\CdRom0\\default.xbe" },
        };
        for (int k = 0; k < (int)(sizeof(d) / sizeof(d[0])); k++) {
            uint32_t str_va = XBOX_KERNEL_DATA_BASE + d[k].str_off;
            uint32_t buf_va = XBOX_KERNEL_DATA_BASE + d[k].buf_off;
            size_t len = strlen(d[k].text);
            memcpy(XBOX_TO_NATIVE(buf_va), d[k].text, len + 1);
            BRIDGE_MEM16(str_va + 0) = (uint16_t)len;        /* Length */
            BRIDGE_MEM16(str_va + 2) = (uint16_t)(len + 1);  /* MaximumLength */
            BRIDGE_MEM32(str_va + 4) = buf_va;               /* Buffer (Xbox VA) */
        }
    }

    fprintf(stderr, "  Kernel data exports: initialized at Xbox VA 0x%08X\n",
            XBOX_KERNEL_DATA_BASE);
}

/* ── Per-slot ordinal and bridge function ────────────────── */

/* Ordinal for each slot (read from Xbox memory during init) */
static ULONG g_slot_ordinals[XBOX_KERNEL_THUNK_TABLE_SIZE];

/* Log counter - limit output to avoid flooding */
/* Calls per ordinal, for the ranking in the periodic summary. 378 counters
 * is smaller than one of the strings this file prints. */
static unsigned long long g_ordinal_calls[XBOX_KERNEL_THUNK_TABLE_SIZE];
/* 64-bit: a title that polls the clock through the kernel passes 2^31 calls
 * within minutes (X-Men Legends does). As a 32-bit int it wrapped negative,
 * "count <= log budget" turned true, and every kernel call then wrote two log
 * lines through the shared stderr lock -- the frame rate fell to ~1 FPS. */
static long long g_kernel_call_count = 0;

/* How many kernel calls get logged before the log goes quiet.
 *
 * The cap keeps a title that makes thousands of calls from burying the
 * console, but a bring-up that gets past early init then has no visibility at
 * exactly the point it stops being obvious. Override with
 * RECOMP_KERNEL_LOG_BUDGET, the same way RECOMP_TRACE_BUDGET works for the
 * function tracer. 0 silences the log entirely.
 */
static long kernel_log_budget(void)
{
    static long budget = -1;

    if (budget < 0) {
        const char *env = getenv("RECOMP_KERNEL_LOG_BUDGET");
        budget = env ? strtol(env, NULL, 0) : 200;
        if (budget < 0)
            budget = 0;
    }
    return budget;
}

#define KERNEL_LOG_ON()      (g_kernel_call_count <= kernel_log_budget())
/* Some sites logged at a tighter cap than the rest; keep them proportional. */
#define KERNEL_LOG_ON_HALF() (g_kernel_call_count <= kernel_log_budget() / 2)

/* Read Xbox stack arg as uint32_t.
 * After kernel_thunk_dispatch pops the dummy return address (g_esp += 4),
 * arg0 is at g_esp+0, arg1 at g_esp+4, etc. */
#define STACK_ARG(n) ((uint32_t)BRIDGE_MEM32(g_esp + (n) * 4))

/* Guest return address of the call currently in a bridge. */
RECOMP_TLS uint32_t g_xbox_kernel_caller;

/* ── Per-ordinal bridge functions ─────────────────────────
 *
 * Each bridge reads args from the Xbox stack, translates pointer
 * args from Xbox VA→native, calls the kernel function, and stores
 * the result in g_eax.
 *
 * Xbox cdecl: args pushed right-to-left, caller cleans stack.
 * Xbox stdcall: args pushed right-to-left, callee cleans stack.
 * In our case the caller (translated code) does "PUSH32" for each arg
 * before calling, and the kernel function's ret-N is handled by the
 * translated code's own stack adjustment.
 */

/* ── PsCreateSystemThreadEx (ordinal 255) ────────────────
 * NTSTATUS PsCreateSystemThreadEx(
 *   PHANDLE ThreadHandle,      // arg0: Xbox VA → pointer
 *   ULONG ThreadExtraSize,     // arg1: value
 *   ULONG KernelStackSize,     // arg2: value
 *   ULONG TlsDataSize,         // arg3: value
 *   PULONG ThreadId,           // arg4: Xbox VA → pointer (can be NULL)
 *   PVOID StartContext1,       // arg5: Xbox VA → opaque
 *   PVOID StartContext2,       // arg6: Xbox VA → opaque
 *   BOOLEAN CreateSuspended,   // arg7: value
 *   BOOLEAN DebugStack,        // arg8: value
 *   PXBOX_SYSTEM_ROUTINE StartRoutine  // arg9: Xbox function pointer
 * )
 *
 * For static recompilation, we don't create a real thread.
 * Instead we call the StartRoutine synchronously via RECOMP_ICALL.
 * This is correct because on Xbox, the entry point creates a system
 * thread and returns, and the thread runs the actual game.
 */
static int g_thread_call_count = 0;

/* Thread entry shim. Sets up the new thread's own simulated stack, pushes the
 * two Xbox start-context arguments plus the dummy return address the callee's
 * `ret` consumes, and runs. */
/* Set on threads this bridge spawned; see PsTerminateSystemThread. */
static RECOMP_TLS int g_is_spawned_thread = 0;

/* This thread's simulated stack, so both exits can give it back. Thread-local
 * for the obvious reason, and needed at all because the common exit is
 * ExitThread from PsTerminateSystemThread -- bridge_thread_main's own return
 * path is the rare one. */
static RECOMP_TLS uint32_t g_thread_stack_top = 0;

struct bridge_thread_start {
    recomp_func_t fn;
    uint32_t ctx1, ctx2, stack_top;
};

struct ob_managed { struct ob_managed *next; uint32_t body; char *name; };
static struct ob_managed *g_ob_list;
#define OB_HANDLE_TAG ((uintptr_t)0x7E0000000000ull)
#define OB_REFS(b)    BRIDGE_MEM32((b) - 0x10)
#define OB_HANDLES(b) BRIDGE_MEM32((b) - 0xC)
#define OB_TYPE(b)    BRIDGE_MEM32((b) - 8)
#define OB_FLAGS(b)   BRIDGE_MEM32((b) - 4)
static struct ob_managed *ob_find_name(const char *name);
static uint32_t ob_from_handle(uint32_t handle);
static NTSTATUS bridge_write_handle(uint32_t handle_va, HANDLE h);

static void bridge_run_thread_inline(recomp_func_t fn, uint32_t ctx1,
                                     uint32_t ctx2)
{
    g_esp -= 4; BRIDGE_MEM32(g_esp) = ctx2;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = ctx1;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
    g_seh_ebp = g_esp;
    fn();
    g_esp += 12;
}

void guest_cpu_join(void);
void guest_cpu_part(void);
void xbox_PinToGuestCore(void);

static void bridge_thread_mark_exit(uint32_t status);

/* The console converts text as Latin-1 whatever the PC's language: a byte widens
 * to the same number, a character above 0xFF becomes '?'. Same shape as the Win32
 * calls: -1 counts the NUL, no output buffer returns the size. */
static int l1_mb2wc(const char *s, int n, WCHAR *d, int max)
{
    int i;
    if (n < 0) n = (int)strlen(s) + 1;
    if (!max) return n;
    if (n > max) n = max;
    for (i = 0; i < n; i++) d[i] = (WCHAR)(unsigned char)s[i];
    return n;
}

static int l1_wc2mb(const WCHAR *s, int n, char *d, int max)
{
    int i;
    if (n < 0) {
        for (n = 0; s[n]; n++) { }
        n++;
    }
    if (!max) return n;
    if (n > max) n = max;
    for (i = 0; i < n; i++) d[i] = s[i] < 0x100 ? (char)s[i] : '?';
    return n;
}

/* RtlUpperChar and RtlLowerChar's table, for bytes. */
static int l1_upper(int c)
{
    c &= 0xFF;
    if (c >= 'a' && c <= 'z') return c - 0x20;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 0x20;
    return c == 0xFF ? '?' : c;
}

static int l1_lower(int c)
{
    c &= 0xFF;
    if (c >= 'A' && c <= 'Z') return c + 0x20;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 0x20;
    return c;
}
void xbox_IsrLock(void);
void xbox_IsrUnlock(void);
static const char* bridge_get_xbox_path(uint32_t obj_attrs_va);
static const WCHAR *bridge_object_name(uint32_t oa_va, WCHAR *buf, size_t chars);
static NTSTATUS bridge_named_status(HANDLE h);

/* The kernel turns interrupts off around these list and counter updates. */
static volatile LONG s_ex_lock;
static void ex_lock(void)
{
    while (InterlockedExchange(&s_ex_lock, 1)) SwitchToThread();
}
static void ex_unlock(void)
{
    InterlockedExchange(&s_ex_lock, 0);
}
static uint32_t s_thread_notify[8];     /* PsSetCreateThreadNotifyRoutine */
static void bridge_thread_notify(HANDLE th, uint32_t create);
static RECOMP_TLS uint32_t t_in_dpc;   /* KeIsExecutingDpc */
KIRQL xbox_CurrentIrql(void);

/* The host's own native call of the same name, where Win32 has no equal: these
 * objects behave the same and the structures match. */
static void *bridge_nt_fn(const char *name)
{
#ifdef _WIN32
    return (void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), name);
#else
    (void)name;
    return NULL;
#endif
}

recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
static uint32_t bridge_nt_query(const char *name, uint32_t token, void *buf, ULONG len);

/* Runs a user APC routine, VOID Routine(Context, Arg1, Arg2), on this thread:
 * it is in an alertable wait, as on the console. */
static void bridge_call_user_apc(uint32_t routine, uint32_t ctx, uint32_t a1, uint32_t a2)
{
    recomp_func_t fn = recomp_lookup(routine);
    uint32_t esp = g_esp, eax = g_eax;
    if (!fn) fn = recomp_lookup_manual(routine);
    if (!fn) fn = recomp_lookup_kernel(routine);
    if (!fn) {
        fprintf(stderr, "  [KERNEL] user APC 0x%08X not in dispatch\n", routine);
        return;
    }
    g_esp -= 4; BRIDGE_MEM32(g_esp) = a2;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = a1;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = ctx;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
    fn();
    g_esp = esp;
    g_eax = eax;
}

struct bridge_user_apc { uint32_t routine, ctx, a1, a2; int once; };

static VOID WINAPI bridge_user_apc_host(ULONG_PTR p)
{
    struct bridge_user_apc *a = (struct bridge_user_apc *)p;
    bridge_call_user_apc(a->routine, a->ctx, a->a1, a->a2);
    free(a);
}

static VOID bridge_timer_apc_host(PVOID p, DWORD low, DWORD high)
{
    struct bridge_user_apc *a = (struct bridge_user_apc *)p;
    bridge_call_user_apc(a->routine, a->ctx, low, high);
    if (a->once) free(a);
}

static DWORD WINAPI bridge_thread_main(LPVOID param)
{
    struct bridge_thread_start *s = (struct bridge_thread_start *)param;
    recomp_func_t fn = s->fn;
    uint32_t ctx1 = s->ctx1, ctx2 = s->ctx2;

    /* Own register set (RECOMP_TLS), own simulated stack -- and own TIB.
     *
     * The TIB carries the SEH chain head and, through fs:[4], the CRT's
     * per-thread data. Sharing one made "which thread am I" a single answer
     * for every thread, which is how two of them ended up inside _lock() each
     * holding the lock the other wanted. */
    g_is_spawned_thread = 1;
    g_esp = s->stack_top;
    g_thread_stack_top = s->stack_top;
    {
        uint32_t tib = xbox_AllocThreadTib();
        if (tib)
            g_fs_base = tib;
        else
            fprintf(stderr, "  [KERNEL] worker thread has no TIB of its own;"
                            " it shares the main thread's\n");
    }
    free(s);

    guest_cpu_join();
    bridge_run_thread_inline(fn, ctx1, ctx2);
    guest_cpu_part();

    fprintf(stderr, "  [KERNEL] worker thread returned (eax=0x%08X)\n", g_eax);
    fflush(stderr);
    /* The routine returned instead of calling PsTerminateSystemThread; the
     * stack is still ours to give back. */
    bridge_thread_mark_exit(0);
    xbox_FreeThreadStack(g_thread_stack_top);
    g_thread_stack_top = 0;
    return 0;
}

static HANDLE bridge_spawn_thread(recomp_func_t fn, uint32_t ctx1,
                                  uint32_t ctx2, uint32_t stack_top)
{
    struct bridge_thread_start *s = malloc(sizeof(*s));
    HANDLE th;

    if (!s) return NULL;
    s->fn = fn; s->ctx1 = ctx1; s->ctx2 = ctx2; s->stack_top = stack_top;

    /* Held until the caller has stored the handle, and for good if the title
     * asked for a suspended thread: on the console it has not run by then. */
    th = CreateThread(NULL, 0, bridge_thread_main, s, CREATE_SUSPENDED, NULL);
    if (!th) free(s);
    /* Record the game thread so a host-tick-driven title's watchdog can sample
     * it via xbox_thread_debug_handle. Harmless for default-model titles: they
     * spawn workers too, but never read it back. See kernel_thread.c. */
    else xbox_set_game_thread(th);
    return th;
}

/* Two ways a title expects its first PsCreateSystemThreadEx to behave.
 *
 * INLINE (default): the first call IS the game starting -- run the routine
 * inline, inheriting register state, and it drives its own main loop forever.
 * This is what Halo and Crimson Skies need and the historical behavior.
 *
 * SPAWN: the title's entry spawns an init thread and RETURNS, expecting the host
 * to drive the per-frame tick afterwards (Burnout 3 is tick-driven). Here the
 * first call must spawn a real thread and return, so control comes back to the
 * host. Opt in with xbox_SetThreadMode before the game starts. See
 * docs/technical/burnout3-reunification.md. */
/* XBOX_THREAD_MODE_* and xbox_SetThreadMode are declared in xbox_memory_layout.h. */
static int g_thread_mode = XBOX_THREAD_MODE_INLINE;
void xbox_SetThreadMode(int mode) { g_thread_mode = mode; }

static void bridge_PsCreateSystemThreadEx(void)
{
    uint32_t xbox_handle_ptr = STACK_ARG(0);
    uint32_t start_context1  = STACK_ARG(5);
    uint32_t start_context2  = STACK_ARG(6);
    uint32_t start_routine   = STACK_ARG(9);
    /* In SPAWN mode there is no privileged "first call": every thread is real,
     * so the entry can return. In INLINE mode the first call runs the game. */
    int is_first_call = (g_thread_mode == XBOX_THREAD_MODE_INLINE)
                        && (g_thread_call_count == 0);
    g_thread_call_count++;

    fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx #%d: routine=0x%08X ctx1=0x%08X ctx2=0x%08X\n",
            g_thread_call_count, start_routine, start_context1, start_context2);
    fflush(stderr);

    /* Write a fake handle to the output pointer */
    if (xbox_handle_ptr) {
        BRIDGE_MEM32(xbox_handle_ptr) = 0xBEEF0001;  /* fake handle */
    }

    /* Call the start routine synchronously through the recomp dispatch.
     * Xbox thread start routines receive two parameters:
     *   void ThreadRoutine(PVOID StartContext1, PVOID StartContext2)
     * We push both onto the simulated stack (right-to-left).
     *
     * First call: the game's main thread entry point. Must run synchronously
     * and inherit the current register state (this IS the game starting).
     *
     * Subsequent calls: worker threads. Must save/restore ALL global registers
     * because on real Xbox each thread has its own register set. Without this,
     * the worker clobbers the caller's g_esi, g_ebx, etc. */
    if (start_routine) {
        recomp_func_t fn = recomp_lookup(start_routine);
        if (!fn) fn = recomp_lookup_manual(start_routine);
        if (fn) {
            if (is_first_call) {
                /* Main game thread: run directly, inheriting register state */
                g_esp -= 4; BRIDGE_MEM32(g_esp) = start_context2;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = start_context1;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
                fn();
                g_esp += 12;
                fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: main thread returned (g_eax=0x%08X)\n", g_eax);
                fflush(stderr);
            } else {
                /* Worker thread: a real one.
                 *
                 * This used to run the routine synchronously and restore the
                 * caller's registers afterwards, which is fine only for a
                 * worker that finishes. Halo's cache/file worker does not -- it
                 * blocks on an event waiting for requests, so CreateThread
                 * never returned and startup deadlocked before the main loop.
                 *
                 * Now that the register set is thread-local (RECOMP_TLS), a
                 * spawned thread gets its own, and the caller's is untouched by
                 * construction rather than by save/restore. */
                /* RECOMP_WORKERS=inline runs a title's worker routines on
                 * the calling thread instead of spawning one.
                 *
                 * Not a mode to ship a title in -- a worker that blocks
                 * waiting for requests never returns, and the caller never
                 * gets control back. It is a bisecting tool: when something
                 * only goes wrong with two guest threads running, this says so
                 * in one run, and the answer separates a concurrency bug from
                 * everything else it might have been. */
                const char *inline_workers = getenv("RECOMP_WORKERS");
                uint32_t stack_top;

                if (inline_workers && !strcmp(inline_workers, "inline")) {
                    fprintf(stderr, "  [KERNEL] RECOMP_WORKERS=inline: running "
                            "worker 0x%08X on this thread\n", start_routine);
                    fflush(stderr);
                    bridge_run_thread_inline(fn, start_context1, start_context2);
                    return;
                }

                stack_top = xbox_AllocThreadStackSized(STACK_ARG(2));

                if (!stack_top) {
                    fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: out of "
                            "thread stacks, running worker 0x%08X inline\n",
                            start_routine);
                    fflush(stderr);
                    bridge_run_thread_inline(fn, start_context1, start_context2);
                } else {
                    HANDLE th = bridge_spawn_thread(fn, start_context1,
                                                    start_context2, stack_top);
                    fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: spawned "
                            "worker 0x%08X (ctx=0x%08X, stack top 0x%08X)\n",
                            start_routine, start_context1, stack_top);
                    fflush(stderr);
                    if (!th) {
                        g_eax = 0xC000009Au;   /* STATUS_INSUFFICIENT_RESOURCES */
                        return;
                    }
                    {
                        uint32_t id_va = STACK_ARG(4);
                        if (id_va) BRIDGE_MEM32(id_va) = (uint32_t)GetThreadId(th);
                        if (xbox_handle_ptr) bridge_write_handle(xbox_handle_ptr, th);
                        bridge_thread_notify(th, 1);
                        if (!(uint8_t)STACK_ARG(7)) ResumeThread(th);   /* CreateSuspended */
                    }
                }
            }
        } else {
            fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: start routine 0x%08X not found in dispatch!\n",
                    start_routine);
        }
    }

    g_eax = 0; /* STATUS_SUCCESS */
}

/* ── NtClose (ordinal 187) ───────────────────────────────
 * NTSTATUS NtClose(HANDLE Handle)
 * Handle is a value (not a pointer), so safe for generic call.
 */

/* Asynchronous-handle bookkeeping, defined with the file bridges below. */
static void bridge_note_async_handle(uint32_t token);
static int bridge_handle_is_async(uint32_t token);
static void bridge_forget_async_handle(uint32_t token);
static int  bridge_handle_is_async(uint32_t token);
/* The file each open handle names, for [SLOWCALL]; defined with them. */
static void bridge_note_file_name(uint32_t token);
static void bridge_forget_file_name(uint32_t token);

/* Handle-table helpers; defined further below. Xbox memory slots are 32-bit
 * but native HANDLEs are 64-bit pointers, so handles are kept in a table and
 * referenced by tagged 32-bit tokens. */
static NTSTATUS bridge_write_handle(uint32_t handle_va, HANDLE h);
static HANDLE bridge_take_handle(uint32_t token);
static void ob_close_handle(uint32_t body);

static void bridge_NtClose(void)
{
    uint32_t raw_handle = STACK_ARG(0);

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] NtClose: handle=0x%08X\n", raw_handle);
        fflush(stderr);
    }

    bridge_forget_async_handle(raw_handle);
    bridge_forget_file_name(raw_handle);

    /* Fake handles the bridge gives out have nothing behind them. */
    if ((raw_handle >= 0xDEAE0000u && raw_handle < 0xDEAE0000u + 1024) ||
        raw_handle == 0xDEAD0001u || raw_handle == 0xDECD0001u ||
        raw_handle == 0xBEEF0001u || raw_handle == 0xBEEF0002u ||
        raw_handle == 0xBEEF0010u) {
        g_eax = 0; /* STATUS_SUCCESS */
        return;
    }
    {
        uint32_t ob_body = ob_from_handle(raw_handle);
        HANDLE h = bridge_take_handle(raw_handle);
        if (!h) {
            g_eax = 0xC0000008u; /* STATUS_INVALID_HANDLE, as the kernel's NtClose */
            return;
        }
        if (ob_body) {
            ob_close_handle(ob_body);
            g_eax = 0;
            return;
        }
        if (h != INVALID_HANDLE_VALUE) {
#ifdef _WIN32
            xbox_dir_context_drop(h);
#endif
            CloseHandle(h);
        }
    }
    g_eax = 0; /* STATUS_SUCCESS */
}

/* ── MmAllocateContiguousMemory (ordinal 165) ─────────────
 * PVOID MmAllocateContiguousMemory(ULONG NumberOfBytes)
 */
static void bridge_MmAllocateContiguousMemory(void)
{
    uint32_t size = STACK_ARG(0);

    /* From the contiguous window, not the general heap: the caller is
     * entitled to assume (VA & 0x0FFFFFFF) | 0x80000000 == VA, because that
     * is how a driver converts between the address it holds and the one it
     * hands the hardware. See xbox_ContiguousAlloc. */
    uint32_t xbox_va = xbox_ContiguousAlloc(size, 4096);

    if (KERNEL_LOG_ON_HALF()) {
        fprintf(stderr, "  [KERNEL] MmAllocateContiguousMemory: size=%u → Xbox VA 0x%08X\n",
                size, xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

/* ── MmAllocateSystemMemory (ordinal 167) ─────────────────
 * PVOID MmAllocateSystemMemory(ULONG NumberOfBytes, ULONG Protect)
 *
 * The kernel's own page allocator: whole pages, not the title's heap, and not
 * required to be physically contiguous. Titles use it for large buffers they
 * intend to keep -- Half-Life 2 takes one while loading a level, right after
 * it opens the .bsp.
 *
 * Unbridged this returned 0, and a NULL from an allocator is not a failure the
 * caller checks for here; it carried the pointer into graphics setup and hung
 * touching the NV2A aperture with it. Page-aligned out of the ordinary heap is
 * the right answer: the distinction the console draws between system memory
 * and the title heap is about which pool the pages come from, and there is one
 * pool here.
 *
 * Zeroed, because the console hands out zeroed pages and callers assume it.
 */
static void bridge_MmAllocateSystemMemory(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t prot = STACK_ARG(1);
    uint32_t xbox_va;

    (void)prot;
    if (!size) {
        g_eax = 0;
        return;
    }
    xbox_va = xbox_HeapAlloc(size, 4096);
    if (xbox_va)
        memset((void *)((uintptr_t)xbox_va + g_xbox_mem_offset), 0, size);
    else
        fprintf(stderr, "  [KERNEL] MmAllocateSystemMemory: %u bytes REFUSED\n",
                size);
    g_eax = xbox_va;
}

/* ── MmAllocateContiguousMemoryEx (ordinal 166) ───────────
 * PVOID MmAllocateContiguousMemoryEx(SIZE_T size, ULONG_PTR low, ULONG_PTR high,
 *                                     ULONG alignment, ULONG protect)
 */
/* Contiguous memory is addressed through the physical-memory mirror: physical
 * page P is visible at 0x80000000 + P. Titles that pin buffers at fixed
 * physical addresses check the returned pointer against that, so the address
 * has to be honoured rather than satisfied from the general heap. */
#define XBOX_PHYSICAL_MIRROR_BASE 0x80000000u

static void bridge_MmAllocateContiguousMemoryEx(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t low = STACK_ARG(1);
    uint32_t high = STACK_ARG(2);
    uint32_t align = STACK_ARG(3);
    uint32_t prot = STACK_ARG(4);
    uint32_t xbox_va;

    (void)prot;

    /*
     * A caller that constrains the range to exactly one allocation's worth is
     * demanding a specific physical address, not expressing a preference.
     * Halo does this for its two big pools and asserts on the result
     * (physical_memory_map.c:46) - XPhysicalAlloc passes lowest = the address
     * it wants and highest = lowest + size - 1, then requires
     * 0x80000000 | lowest back. Satisfying that from the heap fails the assert
     * and leaves its whole memory map wrong.
     */
    if (low && high >= low && (high - low + 1) <= size + 0x1000) {
        xbox_va = XBOX_PHYSICAL_MIRROR_BASE + low;

        /* The console hands out zeroed pages here, and titles rely on it:
         * pool headers and free-list roots are assumed clear, so whatever the
         * backing view happened to contain shows up later as structures that
         * are "allocated" but full of garbage. */
        memset((void *)((uintptr_t)xbox_va + g_xbox_mem_offset), 0, size);

        if (KERNEL_LOG_ON_HALF()) {
            fprintf(stderr, "  [KERNEL] MmAllocateContiguousMemoryEx: size=%u "
                    "pinned phys 0x%08X -> Xbox VA 0x%08X (zeroed)\n",
                    size, low, xbox_va);
            fflush(stderr);
        }
        g_eax = xbox_va;
        return;
    }

    if (align < 4096) align = 4096;
    xbox_va = xbox_ContiguousAlloc(size, align);

    if (KERNEL_LOG_ON_HALF()) {
        fprintf(stderr, "  [KERNEL] MmAllocateContiguousMemoryEx: size=%u align=%u → Xbox VA 0x%08X\n",
                size, align, xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

/* ── MmFreeContiguousMemory (ordinal 171) ─────────────────
 * VOID MmFreeContiguousMemory(PVOID BaseAddress)
 */
static void bridge_MmFreeContiguousMemory(void)
{
    extern int xbox_ContiguousFree(uint32_t addr);
    uint32_t addr = STACK_ARG(0);
    /* Contiguous blocks come from their own arena, not the heap: passing
     * them to xbox_HeapFree found nothing, so none ever came back. */
    if (!xbox_ContiguousFree(addr))
        xbox_HeapFree(addr);
    g_eax = 0;
}

/* RECOMP_VAD=1: reservations and commits tracked, as the console's checks (see
 * guest_vad.h). The memory still comes from the heap, committed from the start. */
static void bridge_vad_zero(uint32_t va, uint32_t bytes)
{
    memset(XBOX_TO_NATIVE(va), 0, bytes);
}

/* 0 when these arguments would pass the console's checks. */
static uint32_t bridge_vad_check(uint32_t zero_bits, uint32_t type, uint32_t size, uint32_t base)
{
    if (zero_bits > 0x15u || (type & 0xFF67CFFFu) || !(type & 0x83000u) ||
        ((type & 0x80000u) && type != 0x80000u))
        return 0xC000000Du;
    if (base > 0x7FFDFFFFu || 0x7FFE0000u - base < size || !size)
        return 0xC000000Du;
    return 0;
}

/* ── NtAllocateVirtualMemory (ordinal 184) ────────────────
 * NTSTATUS NtAllocateVirtualMemory(PVOID *BaseAddress, ULONG ZeroBits,
 *     PULONG AllocationSize, ULONG AllocationType, ULONG Protect)
 */
static void bridge_NtAllocateVirtualMemory(void)
{
    uint32_t base_ptr = STACK_ARG(0);  /* PVOID* in Xbox VA */
    uint32_t zero_bits = STACK_ARG(1);
    uint32_t size_ptr = STACK_ARG(2);  /* PULONG in Xbox VA */
    uint32_t alloc_type = STACK_ARG(3);
    uint32_t protect = STACK_ARG(4);

    /* Read the requested size from Xbox memory */
    uint32_t size = size_ptr ? BRIDGE_MEM32(size_ptr) : 0;
    /* Read the base address hint (0 = let kernel choose) */
    uint32_t base_hint = base_ptr ? BRIDGE_MEM32(base_ptr) : 0;

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] NtAllocateVirtualMemory: base=0x%08X size=%u type=0x%X prot=0x%X\n",
                base_hint, size, alloc_type, protect);
        fflush(stderr);
    }

    /* RECOMP_EXT_VMA: a request for a specific address above the RAM mirrors
     * gets exactly that address, or a failure the caller can act on. The heap
     * path below never honours a base; it substitutes whatever its cursor is
     * at, which breaks any title that checks what it got against what it asked
     * for. Everything else (base = 0, or an address in mapped RAM) carries on
     * below. */
    if (base_ptr && size_ptr) {
        uint32_t vm_base = base_hint;
        uint32_t vm_size = size;
        uint32_t vm_status;

        if (guest_vmem_allocate(&vm_base, &vm_size, alloc_type, protect, &vm_status)) {
            if (KERNEL_LOG_ON()) {
                fprintf(stderr, "  [KERNEL] NtAllocateVirtualMemory (extended VMA): "
                                "base=0x%08X size=%u status=0x%08X\n",
                        vm_base, vm_size, vm_status);
                fflush(stderr);
            }
            if (vm_status == 0) {
                BRIDGE_MEM32(base_ptr) = vm_base;
                BRIDGE_MEM32(size_ptr) = vm_size;
            }
            g_eax = vm_status;
            return;
        }
    }

    if (guest_vad_enabled()) {
        guest_vad_init((uint32_t)(g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram));
        g_eax = bridge_vad_check(zero_bits, alloc_type, size, base_hint);
        if (g_eax) return;
        if (base_hint != 0 && (alloc_type & 0x2000) == 0) {   /* MEM_COMMIT alone */
            uint32_t b = base_hint, s = size;
            g_eax = guest_vad_commit(&b, &s, protect, bridge_vad_zero);
            if (g_eax == 0) {
                BRIDGE_MEM32(base_ptr) = b;
                BRIDGE_MEM32(size_ptr) = s;
            }
            return;
        }
    }

    if (size == 0) {
        g_eax = 0xC0000045u; /* STATUS_INVALID_PAGE_PROTECTION */
        return;
    }

    /*
     * Xbox NtAllocateVirtualMemory supports two modes:
     * - MEM_RESERVE (0x2000): Reserve virtual address space
     * - MEM_COMMIT  (0x1000): Commit pages within a reserved region
     * - MEM_RESERVE|MEM_COMMIT (0x3000): Both in one call
     *
     * Our Xbox heap (bump allocator) always commits memory immediately,
     * so MEM_COMMIT on an already-reserved region is a no-op.
     * Only allocate new memory when MEM_RESERVE is requested.
     */
    /* An address above physical RAM is not free memory -- it aliases.
     *
     * The runtime maps 64 MB and then mirrors it at 64 MB intervals,
     * because real Xbox RAM wraps on a 26-bit address bus. So a guest that
     * sub-allocates past the top of RAM does not get fresh pages, it gets
     * low memory that something else already owns, and the two quietly
     * share storage. Half-Life 2 put a CUtlRBTree element array at
     * 0x0CB80000, which aliases 0x00B80000; other regions it took land
     * inside the live heap (0x05B80000 -> 0x01B80000).
     *
     * Real hardware wraps *physical* addresses while translating virtual
     * ones, so this never happens there. Modelling every guest address as
     * physical is the gap, and that is a bigger change than a bridge fix.
     * Until then, say so: silent aliasing surfaces as corrupted data
     * structures far from here, which is the worst way to find it.
     */
    if (base_hint >= (g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram)) {
        static unsigned warned;
        if (warned++ < 8)
            fprintf(stderr,
                    "  [KERNEL] WARNING: allocation at 0x%08X is above "
                    "%u MB mapped; it aliases 0x%08X\n",
                    base_hint,
                    (unsigned)((g_xbox_map_size ? g_xbox_map_size
                                                : g_xbox_total_ram)
                               / (1024 * 1024)),
                    (uint32_t)(base_hint % (g_xbox_map_size
                                            ? g_xbox_map_size
                                            : g_xbox_total_ram)));
        fflush(stderr);
    }

    if (base_hint != 0 && (alloc_type & 0x2000) == 0) {
        /* MEM_COMMIT only, on an already-reserved region.
         * The memory is already committed by our bump allocator.
         * Don't change the base address - just return success. */
        if (KERNEL_LOG_ON()) {
            fprintf(stderr, "  [KERNEL] → MEM_COMMIT on existing region 0x%08X, no-op\n", base_hint);
            fflush(stderr);
        }
        g_eax = 0; /* STATUS_SUCCESS */
        return;
    }

    /* Allocate from Xbox heap (MEM_RESERVE or MEM_RESERVE|MEM_COMMIT).
     *
     * A pure MEM_RESERVE costs no RAM on real hardware -- it takes address
     * space out of a 4 GB range, not pages out of the 64 MB of memory -- so
     * titles reserve far more than the console physically has and commit a
     * fraction of it. Our heap is a bump allocator that commits everything it
     * hands out, so a large reserve asks for RAM that does not exist.
     *
     * Half-Life 2's XBE header sets PeHeapReserve to 128 MB, and its CRT
     * reserves exactly that during RtlCreateHeap. Failing it returned
     * STATUS_NO_MEMORY, RtlCreateHeap returned 0, and CRT init aborted before
     * main -- on a console with 64 MB, asking for 128 MB is normal, not an
     * error.
     *
     * So a reserve that does not fit is clamped to what the heap can actually
     * back, and the caller is told the real size through the IN/OUT RegionSize
     * parameter, which is where the API already reports the rounded figure.
     *
     * ponytail: the honest fix is a reserve that costs nothing and a commit
     * that backs pages on demand, which needs the allocator to separate the
     * two. This clamp is enough for a title that reserves generously and
     * commits little, and it fails loudly and later rather than silently and
     * at startup if one does not. */
    /* A reservation's base is 64 KB aligned on the console. */
    uint32_t xbox_va = xbox_HeapAlloc(size, guest_vad_enabled() ? 65536 : 4096);
    if (!xbox_va && (alloc_type & 0x2000) && !(alloc_type & 0x1000)) {
        /* A pure reservation too big for the heap. Take it from the mapped
         * space above RAM, where it costs no heap and the pages are distinct.
         *
         * Clamping instead -- handing back a fraction of what was asked for --
         * is what broke Half-Life 2. It reserves 128 MB and then 200 MB, got
         * 32 MB and 12.5 MB, and then sub-allocated across the range it
         * believed it owned. That walks past the top of RAM, where the mirrors
         * alias low memory, so its containers quietly shared storage with the
         * live heap. Granting the full range is both more honest and less
         * damaging. Returns 0 unless the title asked for a mapping larger than
         * RAM, so nothing changes for titles that did not. */
        xbox_va = xbox_ReserveAlloc(size, 4096);
        if (xbox_va) {
            fprintf(stderr, "  [KERNEL] NtAllocateVirtualMemory: reserve of %u"
                            " granted at 0x%08X above RAM\n", size, xbox_va);
            fflush(stderr);
        }
    }
    if (!xbox_va && (alloc_type & 0x2000) && !(alloc_type & 0x1000)) {
        uint32_t want = size;
        while (want > 0x10000 && !xbox_va) {
            want /= 2;
            xbox_va = xbox_HeapAlloc(want, 4096);
        }
        if (xbox_va) {
            fprintf(stderr, "  [KERNEL] NtAllocateVirtualMemory: reserve of %u "
                            "clamped to %u (heap cannot back the full range)\n",
                    size, want);
            fflush(stderr);
            size = want;
        }
    }
    if (!xbox_va) {
        g_eax = 0xC0000017u; /* STATUS_NO_MEMORY */
        return;
    }

    if (guest_vad_enabled())
        guest_vad_reserve(xbox_va, &size, protect, (alloc_type & 0x1000) != 0);

    /* Write back the allocated address and actual size */
    if (base_ptr) BRIDGE_MEM32(base_ptr) = xbox_va;
    if (size_ptr) BRIDGE_MEM32(size_ptr) = size;

    g_eax = 0; /* STATUS_SUCCESS */
}

/* ── NtFreeVirtualMemory (ordinal 199) ────────────────────
 * NTSTATUS NtFreeVirtualMemory(PVOID *BaseAddress, PULONG FreeSize,
 *     ULONG FreeType)
 */
/* -- NtQueryVirtualMemory (ordinal 217, 2 args = 8 bytes) --------------
 *
 * Xbox takes two arguments, not NT's four:
 *
 *     NTSTATUS NtQueryVirtualMemory(PVOID BaseAddress,
 *                                   PMEMORY_BASIC_INFORMATION Info);
 *
 * This has to be a guest-side answer. The existing xbox_NtQueryVirtualMemory
 * in kernel_memory.c calls the host VirtualQuery and memcpy's a host
 * MEMORY_BASIC_INFORMATION into guest memory, whose pointer fields are 64-bit
 * on an x64 build -- so every field after BaseAddress lands in the wrong place.
 *
 * It also has to exist at all. Without a bridge entry the thunk is left
 * unbridged, and a title's CRT heap creation calls this to probe its heap
 * region: RtlCreateHeap does
 *
 *     call NtQueryVirtualMemory ; test eax,eax ; jl fail
 *     cmp  mbi.BaseAddress, requested ; jne fail
 *     cmp  mbi.State, MEM_FREE        ; je  fail
 *
 * and returns 0 on any of those. On Half-Life 2 that null heap propagated
 * silently through the rest of CRT init.
 *
 * Guest MEMORY_BASIC_INFORMATION, 32-bit, 28 bytes:
 *     +0x00 BaseAddress   +0x04 AllocationBase  +0x08 AllocationProtect
 *     +0x0C RegionSize    +0x10 State           +0x14 Protect
 *     +0x18 Type
 *
 * The guest is one flat committed mapping, so that is what we report: any
 * address inside it is MEM_COMMIT / PAGE_READWRITE / MEM_PRIVATE, and anything
 * outside is MEM_FREE rather than an error, which is the honest answer and the
 * one that lets a caller distinguish the two.
 */
static void bridge_NtQueryVirtualMemory(void)
{
    uint32_t base_va = STACK_ARG(0);
    uint32_t info_va = STACK_ARG(1);
    uint32_t page_base = base_va & ~0xFFFu;

    if (!info_va || base_va > 0x7FFDFFFFu) {
        g_eax = 0xC000000Du;               /* STATUS_INVALID_PARAMETER, past the title's range as the kernel */
        return;
    }

    /* Anything the extended-VMA tracker owns is answered from its own records.
     * The generic answer below calls everything above RAM free, so a caller
     * walking its address space for a free range would be handed one that is
     * live. */
    {
        uint32_t vm_info[7];

        if (guest_vmem_query(base_va, vm_info)) {
            BRIDGE_MEM32(info_va + 0x00) = vm_info[0]; /* BaseAddress */
            BRIDGE_MEM32(info_va + 0x04) = vm_info[1]; /* AllocationBase */
            BRIDGE_MEM32(info_va + 0x08) = vm_info[2]; /* AllocationProtect */
            BRIDGE_MEM32(info_va + 0x0C) = vm_info[3]; /* RegionSize */
            BRIDGE_MEM32(info_va + 0x10) = vm_info[4]; /* State */
            BRIDGE_MEM32(info_va + 0x14) = vm_info[5]; /* Protect */
            BRIDGE_MEM32(info_va + 0x18) = vm_info[6]; /* Type */
            g_eax = 0;
            return;
        }
    }

    if (guest_vad_enabled()) {
        uint32_t vi[7], k;
        guest_vad_init((uint32_t)(g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram));
        g_eax = guest_vad_query(base_va, vi);
        if (g_eax == 0)
            for (k = 0; k < 7; k++)
                BRIDGE_MEM32(info_va + k * 4u) = vi[k];
        return;
    }

    BRIDGE_MEM32(info_va + 0x00) = page_base;          /* BaseAddress */
    BRIDGE_MEM32(info_va + 0x04) = page_base;          /* AllocationBase */
    BRIDGE_MEM32(info_va + 0x08) = 0x04;               /* PAGE_READWRITE */
    BRIDGE_MEM32(info_va + 0x14) = 0x04;               /* Protect */
    BRIDGE_MEM32(info_va + 0x18) = 0x20000;            /* MEM_PRIVATE */

    if (page_base >= g_xbox_code_lo && page_base < XBOX_TOTAL_RAM) {
        BRIDGE_MEM32(info_va + 0x0C) = XBOX_TOTAL_RAM - page_base; /* RegionSize */
        BRIDGE_MEM32(info_va + 0x10) = 0x1000;         /* MEM_COMMIT */
    } else {
        /* Free to the next boundary, not one page. A caller enumerating free
         * memory then steps across the range instead of crawling it 4 KB at a
         * time; past the user range that is 0x80000 queries per lap, and on the
         * title this came from the walk wrapped at 0xFFFFFFFF and never ended.
         * Only with RECOMP_EXT_VMA, since it changes what every query above
         * the image answers. */
        uint32_t region = 0x1000;

        if (guest_vmem_active()) {
            /* Only where nothing else owns the answer: below the image, and
             * above the tracker's range. */
            uint32_t end = 0;

            if (page_base < g_xbox_code_lo)
                end = g_xbox_code_lo;
            else if (page_base >= GUEST_VMEM_TOP)
                end = 0xFFFFFFFFu;
            if (end > page_base)
                region = end - page_base;
        }
        BRIDGE_MEM32(info_va + 0x0C) = region;
        BRIDGE_MEM32(info_va + 0x10) = 0x10000;        /* MEM_FREE */
        BRIDGE_MEM32(info_va + 0x08) = 0;
        BRIDGE_MEM32(info_va + 0x18) = 0;
    }

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] NtQueryVirtualMemory: base=0x%08X -> "
                        "state=0x%X size=%u\n", base_va,
                        BRIDGE_MEM32(info_va + 0x10),
                        BRIDGE_MEM32(info_va + 0x0C));
        fflush(stderr);
    }
    g_eax = 0;                                          /* STATUS_SUCCESS */
}

/* RECOMP_FREE_TRACE: who is freeing, and the state of the heap behind it.
 *
 * A title's RtlHeap decommits a free block only once its free total passes
 * DeCommitTotalFreeThreshold, so a decommit the console never makes says the
 * heap's bookkeeping has drifted from the console's. The heap is found by
 * looking up the caller's frame for a pointer whose +0x10 is the heap
 * signature 0xEEFFEEFF. Logs the first 16 calls. */
static void free_trace(uint32_t base_ptr, uint32_t size_ptr, uint32_t free_type)
{
    static int on = -1;
    static int logged;
    uint32_t heap = 0;
    int i;

    if (on < 0)
        on = getenv("RECOMP_FREE_TRACE") != NULL;
    if (!on || logged >= 16)
        return;
    logged++;

    for (i = 3; i < 64 && !heap; i++) {
        uint32_t p = BRIDGE_MEM32(g_esp + i * 4);
        if (p >= 0x10000 && p < g_xbox_total_ram - 0x80 &&
            BRIDGE_MEM32(p + 0x10) == 0xEEFFEEFFu)
            heap = p;
    }
    fprintf(stderr, "  [FREE] ret=0x%08X base=0x%08X size=0x%X type=0x%X heap=0x%08X\n",
            BRIDGE_MEM32(g_esp - 4),
            base_ptr ? BRIDGE_MEM32(base_ptr) : 0,
            size_ptr ? BRIDGE_MEM32(size_ptr) : 0, free_type, heap);
    if (heap) {
        for (i = 0; i < 0x80; i += 0x20)
            fprintf(stderr, "  [FREE]   +%02X %08X %08X %08X %08X %08X %08X %08X %08X\n", i,
                    BRIDGE_MEM32(heap + i), BRIDGE_MEM32(heap + i + 4),
                    BRIDGE_MEM32(heap + i + 8), BRIDGE_MEM32(heap + i + 12),
                    BRIDGE_MEM32(heap + i + 16), BRIDGE_MEM32(heap + i + 20),
                    BRIDGE_MEM32(heap + i + 24), BRIDGE_MEM32(heap + i + 28));
    }
    fflush(stderr);
}

static void bridge_NtFreeVirtualMemory(void)
{
    uint32_t base_ptr = STACK_ARG(0);
    uint32_t size_ptr = STACK_ARG(1);
    uint32_t free_type = STACK_ARG(2);

    free_trace(base_ptr, size_ptr, free_type);

    if (base_ptr && size_ptr) {
        uint32_t vm_base = BRIDGE_MEM32(base_ptr);
        uint32_t vm_size = BRIDGE_MEM32(size_ptr);
        uint32_t vm_status;

        if (guest_vmem_free(&vm_base, &vm_size, free_type, &vm_status)) {
            if (vm_status == 0) {
                BRIDGE_MEM32(base_ptr) = vm_base;
                BRIDGE_MEM32(size_ptr) = vm_size;
            }
            g_eax = vm_status;
            return;
        }
    }

    /* Memory NtAllocateVirtualMemory took from the guest heap. The call below
     * reads the 32-bit guest slots as host pointers and hands them to
     * VirtualFree, which fails, so none of it ever came back. Under
     * RECOMP_HEAP_RECLAIM a release returns the block to the heap and a
     * decommit zeroes the pages it names, since heap memory stays committed. */
    if (guest_vad_enabled() && base_ptr && size_ptr) {
        uint32_t b = BRIDGE_MEM32(base_ptr), s = BRIDGE_MEM32(size_ptr);
        int released = 0;
        g_eax = guest_vad_free(&b, &s, free_type, bridge_vad_zero, &released);
        if (g_eax == 0) {
            if (released && xbox_HeapReclaimEnabled())
                xbox_HeapFree(b);
            BRIDGE_MEM32(base_ptr) = b;
            BRIDGE_MEM32(size_ptr) = s;
        }
        return;
    }

    if (xbox_HeapReclaimEnabled() && base_ptr && size_ptr) {
        uint32_t vm_base = BRIDGE_MEM32(base_ptr);
        uint32_t left = xbox_HeapBlockSize(vm_base);

        if (left) {
            if (free_type & 0x8000) {              /* MEM_RELEASE */
                xbox_HeapFree(vm_base);
                BRIDGE_MEM32(size_ptr) = 0;
            } else {
                /* MEM_DECOMMIT keeps the block, but on the console the pages
                 * are gone and a later MEM_COMMIT (a no-op here) brings them
                 * back zeroed. Zero them now so it does. */
                uint32_t vm_size = BRIDGE_MEM32(size_ptr);
                uint32_t n = (vm_size && vm_size < left) ? vm_size : left;
                memset(XBOX_TO_NATIVE(vm_base), 0, n);
            }
            g_eax = 0;
            return;
        }
    }

    g_eax = (uint32_t)xbox_NtFreeVirtualMemory(
        XBOX_TO_NATIVE(base_ptr), XBOX_TO_NATIVE(size_ptr), free_type);
}

/* ── ExAllocatePool / ExAllocatePoolWithTag (ordinals 15, 16) ─
 * Must allocate from Xbox heap so the returned pointer is an Xbox VA
 * that can be accessed via MEM32(). Native HeapAlloc returns 64-bit
 * pointers that get truncated and produce garbage Xbox VAs.
 */
/* ExQueryNonVolatileSetting(ValueIndex, Type, Value, ValueLength, ResultLength)
 *
 * Titles read region, language and AV settings from EEPROM through this very
 * early in boot. Ordinal 24 was previously routed to bridge_ExQueryPoolBlockSize,
 * so the call returned a pool size where the game expected a settings blob. */
/* ── FscGetCacheSize (35) / FscSetCacheSize (37) ───────────
 *
 * The Xbox filesystem cache, sized in 4 KB pages. A title that streams from a
 * pack file resizes it around the work: Half-Life 2's pack scanner
 * (sub_0041E650) reads the current size, sets 1 MB for the scan, and puts the
 * old value back when it is done.
 *
 * There is no cache here -- reads go to the host filesystem, which has its
 * own -- so the size is only ever a number the title stores and restores.
 * Keeping it is still worth doing: unbridged, the getter returned 0, and a
 * title that saves that and restores it later is restoring a cache size of
 * zero pages. 64 KB is the console's own default.
 */
#define XBOX_FSCACHE_DEFAULT_PAGES 16u      /* 64 KB in 4 KB pages */

static uint32_t g_fscache_pages = XBOX_FSCACHE_DEFAULT_PAGES;

static void bridge_FscGetCacheSize(void)
{
    g_eax = g_fscache_pages;
}

static void bridge_FscSetCacheSize(void)
{
    uint32_t pages = STACK_ARG(0);
    if (pages > 0x800) {
        g_eax = 0xC000000Du;             /* STATUS_INVALID_PARAMETER */
        return;
    }

    /* The kernel rejects a request it cannot satisfy and leaves the current
     * size alone; the caller checks for a negative status. Nothing here can
     * fail, so accept it and remember what was asked for. */
    g_fscache_pages = pages;
    g_eax = STATUS_SUCCESS;
}

/* ── RtlCompareMemory (268) / RtlCompareMemoryUlong (269) ──
 *
 * Both answer "how far do these match", counted in bytes from the start, and
 * both are used to decide whether a buffer needs work rather than to do it.
 * Unbridged they returned 0, which reads as "differs at the first byte" -- the
 * safe-looking answer that is wrong whenever the caller is checking for a
 * region it can skip.
 *
 * RtlCompareMemoryUlong compares against a repeating ULONG and only ever
 * examines whole ULONGs, so a length that is not a multiple of four leaves the
 * remainder uncompared; the count it returns is still in bytes.
 */
static void bridge_RtlCompareMemory(void)
{
    uint32_t a_va   = STACK_ARG(0);
    uint32_t b_va   = STACK_ARG(1);
    uint32_t length = STACK_ARG(2);
    const uint8_t *a, *b;
    uint32_t i;

    if (!a_va || !b_va || !length) {
        g_eax = 0;
        return;
    }
    a = (const uint8_t *)XBOX_TO_NATIVE(a_va);
    b = (const uint8_t *)XBOX_TO_NATIVE(b_va);
    for (i = 0; i < length; i++) {
        if (a[i] != b[i])
            break;
    }
    g_eax = i;
}

static void bridge_RtlCompareMemoryUlong(void)
{
    uint32_t base_va = STACK_ARG(0);
    uint32_t length  = STACK_ARG(1);
    uint32_t pattern = STACK_ARG(2);
    uint32_t i;

    if (!base_va) {
        g_eax = 0;
        return;
    }
    length &= ~3u;                      /* whole ULONGs only */
    for (i = 0; i < length; i += 4) {
        if (BRIDGE_MEM32(base_va + i) != pattern)
            break;
    }
    g_eax = i;
}

static void bridge_ExQueryNonVolatileSetting(void)
{
    uint32_t value_index  = STACK_ARG(0);
    uint32_t type_va      = STACK_ARG(1);
    uint32_t value_va     = STACK_ARG(2);
    uint32_t value_length = STACK_ARG(3);
    uint32_t result_va    = STACK_ARG(4);

    NTSTATUS st = xbox_ExQueryNonVolatileSetting(
        value_index,
        type_va   ? (PULONG)&BRIDGE_MEM32(type_va)   : NULL,
        value_va  ? (PVOID)((uintptr_t)value_va + g_xbox_mem_offset) : NULL,
        value_length,
        result_va ? (PULONG)&BRIDGE_MEM32(result_va) : NULL);

    g_eax = (uint32_t)st;
}

/* HalReturnToFirmware(Routine) - the title asking to reboot or quit.
 *
 * It never returns on hardware. Returning here would let the game run on past
 * a decision to quit, which reads as a hang rather than an exit. */
static void bridge_run_shutdown_notifications(void);

static void bridge_HalReturnToFirmware(void)
{
    uint32_t routine = STACK_ARG(0);

    /* Routine 2 is a quick reboot, which on Xbox is how a title hands off to
     * another image: XLaunchNewImage fills the launch data page and reboots.
     * So "the title is exiting" and "the title is launching something" look
     * identical here, and the launch page is what tells them apart. */
    {
        uint32_t page = BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE);

        if (page) {
            char path[64];
            uint32_t i;

            for (i = 0; i < sizeof(path) - 1; i++) {
                uint8_t c = BRIDGE_MEM8(page + 8 + i);
                if (!c) break;
                path[i] = (char)c;
            }
            path[i] = 0;
            fprintf(stderr, "  [KERNEL] launch data page 0x%08X:"
                            " type=%u titleid=0x%08X path='%s'\n",
                    page, BRIDGE_MEM32(page), BRIDGE_MEM32(page + 4), path);
            /* XapiBootToDash packs its reason and two parameters into the
             * front of the launch data, so this says why the title asked to
             * leave rather than merely that it did. */
            fprintf(stderr, "  [KERNEL]   launch data:");
            for (i = 0; i < 8; i++)
                fprintf(stderr, " %08X", BRIDGE_MEM32(page + 1024 + i * 4));
            fprintf(stderr, "\n");
        } else {
            fprintf(stderr, "  [KERNEL] no launch data page set\n");
        }
    }

    /* Who asked to quit.
     *
     * A title exiting looks identical whether it finished cleanly, hit an
     * error path, or was told to reboot -- and the routine number does not say
     * which. The guest call chain does. Same GS format tools/stackwalk.py
     * reads. */
    {
        const uint8_t *mem = (const uint8_t *)g_xbox_mem_offset;
        uint32_t i;

        fprintf(stderr, "  [KERNEL] exit requested, guest esp=0x%08X:\n", g_esp);
        for (i = 0; i < 200; i++) {
            uint32_t a = g_esp + i * 4;
            if (a < 0x00010000u || a >= 0x04000000u) break;
            fprintf(stderr, "    GS %08X %08X\n", a,
                    *(const uint32_t *)(mem + a));
        }
        fflush(stderr);
    }

    xbox_PeekSample("exit peek");
    fprintf(stderr, "  [KERNEL] HalReturnToFirmware: routine=%u - title is exiting\n",
            routine);
    fflush(stderr);

    /* Write the indirect-branch targets before the process goes away. This
     * path ends in TerminateProcess, which does not run atexit handlers, so the
     * host's registered dump never fires -- and a title that gives up during
     * boot is exactly the one whose targets are worth having. No-op unless
     * RECOMP_ICALL_FEEDBACK is on. */
    RECOMP_ICALL_FEEDBACK_DUMP();

    bridge_run_shutdown_notifications();

    /* Let a host-played FMV finish before the process goes away.
     *
     * The title is not the one presenting it, so it has no reason to wait --
     * it opens the file, carries on, and quits, which would kill the video
     * thread part-way through a five-second clip. Waiting here is what makes
     * the clip actually watchable, and it costs nothing when no video is
     * playing. Bounded, so a stuck player cannot stop the process exiting. */
    {
        extern int xbox_VideoIsPlaying(void);
        int waited = 0;

        while (xbox_VideoIsPlaying() && waited < 60000) {
            Sleep(50);
            waited += 50;
        }
        if (waited)
            fprintf(stderr, "  [KERNEL] waited %dms for the video to finish\n",
                    waited);
    }

    xbox_HalReturnToFirmware(routine);
}

/* The kernel's pool: over 0xFD8 bytes whole pages, page aligned; otherwise 32-byte units less an 8-byte header, and
 * that is the size ExQueryPoolBlockSize reports. Small blocks stay 16 aligned here (the console's sit 8 past 32). */
static uint32_t pool_alloc(uint32_t size)
{
    if (size > 0xFD8)
        return xbox_HeapAlloc((size + 0xFFF) & ~0xFFFu, 4096);
    return xbox_HeapAlloc(((size + 0x27) & ~31u) - 8, 16);
}

static void bridge_ExAllocatePool(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t xbox_va = pool_alloc(size);

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] ExAllocatePool: size=%u → Xbox VA 0x%08X\n",
                size, xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

static void bridge_ExAllocatePoolWithTag(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t tag = STACK_ARG(1);
    uint32_t xbox_va = pool_alloc(size);

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] ExAllocatePoolWithTag: size=%u tag='%c%c%c%c' → Xbox VA 0x%08X\n",
                size,
                (char)(tag & 0xFF), (char)((tag >> 8) & 0xFF),
                (char)((tag >> 16) & 0xFF), (char)((tag >> 24) & 0xFF),
                xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

/* ── KfRaiseIrql / KfLowerIrql (ordinals 160, 161) ────── */
static void bridge_KfRaiseIrql(void)
{
    uint32_t new_irql = g_ecx; /* fastcall: KIRQL is passed in CL */
    g_eax = (uint32_t)xbox_KfRaiseIrql((UCHAR)new_irql);
}

static void bridge_KfLowerIrql(void)
{
    uint32_t new_irql = g_ecx; /* fastcall: KIRQL is passed in CL */
    xbox_KfLowerIrql((UCHAR)new_irql);
    g_eax = 0;
}

/* ── KeRaiseIrqlToDpcLevel (ordinal 129) ─────────────────── */
static void bridge_KeRaiseIrqlToDpcLevel(void)
{
    g_eax = (uint32_t)xbox_KeRaiseIrqlToDpcLevel();
}

/* ── RtlInitializeCriticalSection / Enter / Leave (ordinals 291, 277, 294) ─ */
/* The host lock does the work; the guest's own fields are kept as the kernel keeps them, for a title that looks at
 * them: +0x10 LockCount (-1 free, one more for each holder and waiter), +0x14 RecursionCount, +0x18 OwningThread. */
static uint32_t bridge_thread_object(uint32_t token);
static uint32_t cs_this_thread(void)
{
    static __thread uint32_t self;
    if (!self) self = bridge_thread_object(0xFFFFFFFEu);
    return self;
}
static void cs_entered(uint32_t cs_va)
{
    BRIDGE_MEM32(cs_va + 0x18) = cs_this_thread();
    BRIDGE_MEM32(cs_va + 0x14) = BRIDGE_MEM32(cs_va + 0x14) + 1;
}
static void cs_leaving(uint32_t cs_va)
{
    uint32_t r = BRIDGE_MEM32(cs_va + 0x14) - 1;
    BRIDGE_MEM32(cs_va + 0x14) = r;
    if (r == 0) BRIDGE_MEM32(cs_va + 0x18) = 0;
    InterlockedDecrement((volatile LONG *)XBOX_TO_NATIVE(cs_va + 0x10));
}

static void bridge_RtlInitializeCriticalSection(void)
{
    /* The kernel's fields: an event header, LockCount -1 (free), RecursionCount and OwningThread 0 */
    uint32_t cs_va = STACK_ARG(0);
    BRIDGE_MEM8(cs_va) = 1;
    BRIDGE_MEM8(cs_va + 2) = 4;
    BRIDGE_MEM32(cs_va + 4) = 0;
    BRIDGE_MEM32(cs_va + 8) = cs_va + 8;
    BRIDGE_MEM32(cs_va + 0xC) = cs_va + 8;
    BRIDGE_MEM32(cs_va + 0x10) = 0xFFFFFFFFu;
    BRIDGE_MEM32(cs_va + 0x14) = 0;
    BRIDGE_MEM32(cs_va + 0x18) = 0;
    xbox_RtlInitializeCriticalSection(XBOX_TO_NATIVE(cs_va));
    g_eax = 0;
}

static void bridge_RtlEnterCriticalSection(void)
{
    uint32_t cs_va = STACK_ARG(0);
    InterlockedIncrement((volatile LONG *)XBOX_TO_NATIVE(cs_va + 0x10));   /* waiters count too */
    xbox_RtlEnterCriticalSection(XBOX_TO_NATIVE(cs_va));
    cs_entered(cs_va);
    g_eax = 0;
}

static void bridge_RtlLeaveCriticalSection(void)
{
    uint32_t cs_va = STACK_ARG(0);
    cs_leaving(cs_va);
    xbox_RtlLeaveCriticalSection(XBOX_TO_NATIVE(cs_va));
    g_eax = 0;
}

/* ── KeQueryPerformanceCounter / Frequency (ordinals 126, 127) ─ */
#define XBOX_COUNTER_HZ 3375000u

static void bridge_KeQueryPerformanceCounter(void)
{
    /* The console's counter runs at 3,375,000 Hz whatever the PC's does, and its
     * 32-bit low part wraps every 21 minutes, not every 7 at 10 MHz. */
    LARGE_INTEGER li = xbox_KeQueryPerformanceCounter();
    LARGE_INTEGER hz = xbox_KeQueryPerformanceFrequency();
    uint64_t c = (uint64_t)li.QuadPart, f = (uint64_t)hz.QuadPart;
    c = (c / f) * XBOX_COUNTER_HZ + (c % f) * XBOX_COUNTER_HZ / f;
    g_eax = (uint32_t)c;
    g_edx = (uint32_t)(c >> 32);
}

static void bridge_KeQueryPerformanceFrequency(void)
{
    g_eax = XBOX_COUNTER_HZ;
    g_edx = 0;
}

/* ── KeQuerySystemTime (ordinal 128) ─────────────────────── */
static void bridge_KeQuerySystemTime(void)
{
    uint32_t time_ptr = STACK_ARG(0);
    xbox_KeQuerySystemTime(XBOX_TO_NATIVE(time_ptr));
    g_eax = 0;
}

/* ── MmQueryStatistics (ordinal 181) ─────────────────────── */
static void bridge_MmQueryStatistics(void)
{
    uint32_t stats_ptr = STACK_ARG(0);
    if (BRIDGE_MEM32(stats_ptr) != 0x24u) {   /* the caller says how long its record is */
        g_eax = 0xC000000Du;
        return;
    }
    g_eax = (uint32_t)xbox_MmQueryStatistics(XBOX_TO_NATIVE(stats_ptr));
}

/* Named objects: the same name opens the same object, with
 * STATUS_OBJECT_NAME_EXISTS, as on the console. The host keeps the names, under a
 * prefix, with its separator out of the way. NULL when the object has no name. */
static const WCHAR *bridge_object_name(uint32_t oa_va, WCHAR *buf, size_t chars)
{
    const char *name = oa_va ? bridge_get_xbox_path(oa_va) : NULL;
    size_t i, n;
    static const char prefix[] = "Local\\xbox_";
    if (!name || !*name) return NULL;
    n = strlen(prefix);
    for (i = 0; i < n && i + 1 < chars; i++) buf[i] = (WCHAR)prefix[i];
    for (; *name && i + 1 < chars; name++, i++)
        buf[i] = *name == '\\' ? L'_' : (WCHAR)(unsigned char)*name;
    buf[i] = 0;
    return buf;
}

/* The status of a create that just ran. */
static NTSTATUS bridge_named_status(HANDLE h)
{
    if (!h)
        return GetLastError() == ERROR_INVALID_HANDLE ? (NTSTATUS)0xC0000024L   /* other type */
                                                      : (NTSTATUS)0xC000009AL;
    return GetLastError() == ERROR_ALREADY_EXISTS ? (NTSTATUS)0x40000000L : 0;
}

/* ── NtCreateEvent (ordinal 189) ─────────────────────────── */
static void bridge_NtCreateEvent(void)
{
    uint32_t handle_ptr = STACK_ARG(0);
    uint32_t obj_attr_ptr = STACK_ARG(1);
    uint32_t event_type = STACK_ARG(2);
    uint32_t initial_state = STACK_ARG(3);

    /* Use local HANDLE to avoid 8-byte write to 4-byte Xbox memory slot.
     * On x64, HANDLE is 8 bytes but Xbox expects 4-byte handles. */
    HANDLE local_handle = NULL;
    NTSTATUS status;
    if (event_type > 1) {
        g_eax = 0xC000000Du;             /* STATUS_INVALID_PARAMETER */
        return;
    }
    {
        WCHAR name[256];
        const WCHAR *named = bridge_object_name(obj_attr_ptr, name, 256);
        if (named) {
            local_handle = CreateEventW(NULL, event_type == 0, (BOOL)(initial_state & 0xFF), named);
            status = bridge_named_status(local_handle);
        } else {
            status = xbox_NtCreateEvent(&local_handle, XBOX_TO_NATIVE(obj_attr_ptr),
                                        event_type, initial_state);
        }
    }

    if (handle_ptr && NT_SUCCESS(status)) {
        NTSTATUS written = bridge_write_handle(handle_ptr, local_handle);
        if (!NT_SUCCESS(written)) status = written;
    }

    fprintf(stderr, "  [BRIDGE] NtCreateEvent: handle_ptr=0x%08X type=%u init=%u → status=0x%08X handle=0x%08X\n",
            handle_ptr, event_type, initial_state, (uint32_t)status,
            (uint32_t)(uintptr_t)local_handle);

    g_eax = (uint32_t)status;
}

static HANDLE ke_shadow_lookup(uint32_t guest_va);
static void ke_shadow_insert(uint32_t guest_va, HANDLE host);
static HANDLE bridge_resolve_handle(uint32_t token);
static uint32_t bridge_handle_status(HANDLE h, uint32_t st);
static HANDLE ke_guest_event(uint32_t guest_va, int *type);

/* A KEVENT of the synchronization kind: Type 1, Size 4 */
/* An object a satisfied wait takes something from: a synchronization event or
 * timer goes back to 0, a semaphore loses one. */
static int ke_header_is_sync_event(uint32_t va)
{
    uint32_t hdr = BRIDGE_MEM32(va);
    uint32_t type = hdr & 0xFFu, size = (hdr >> 16) & 0xFFu;
    return (type == 1 && size == 4) || (type == 9 && size == 10) || (type == 5 && size == 5);
}

static void ke_wait_satisfied(uint32_t va)
{
    if (BRIDGE_MEM8(va) == 5)
        InterlockedDecrement((volatile LONG *)&BRIDGE_MEM32(va + 4));
    else
        BRIDGE_MEM32(va + 4) = 0;
}

/* ── KeSetEvent (ordinal 145) ────────────────────────────── */
/* The state an event had, for the previous-state results. Win32 has no read of
 * it: a zero wait tells, and for an auto-reset event consumes the signal, so
 * every caller here signals or pulses straight after. */
static uint32_t bridge_event_state(HANDLE h)
{
    return WaitForSingleObject(h, 0) == WAIT_OBJECT_0;
}

static void bridge_KeSetEvent(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t increment = STACK_ARG(1);
    uint32_t wait = STACK_ARG(2);
    HANDLE h;

    (void)increment;
    (void)wait;

    {   /* An event the title built itself (see ke_guest_event). */
        int type;
        HANDLE ge = ke_guest_event(guest_va, &type);
        if (ge) {
            g_eax = BRIDGE_MEM32(guest_va + 4) != 0;   /* previous state */
            BRIDGE_MEM32(guest_va + 4) = 1;
            SetEvent(ge);
            return;
        }
    }
    h = ke_shadow_lookup(guest_va);
    if (h)
        BRIDGE_MEM32(guest_va + 4) = 1;   /* the object's own SignalState, as the kernel keeps it */
    if (!h)
        h = bridge_resolve_handle(guest_va);
    if (!h)
        h = XBOX_TO_NATIVE(guest_va);
    if (h) {
        g_eax = bridge_event_state(h);   /* previous state */
        SetEvent(h);
    } else {
        g_eax = 0;
    }
}

/* ── KeWaitForSingleObject (ordinal 159) ─────────────────── */
static void bridge_KeWaitForSingleObject(void)
{
    uint32_t object = STACK_ARG(0);
    uint32_t wait_reason = STACK_ARG(1);
    uint32_t wait_mode = STACK_ARG(2);
    uint32_t alertable = STACK_ARG(3);
    uint32_t timeout_ptr = STACK_ARG(4);
    HANDLE h;

    {   /* An event the title built itself (see ke_guest_event). Its
         * SignalState in guest memory is the truth -- XDK code resets it by
         * writing 0 there -- so bring the host event in line first. */
        int type;
        HANDLE ge = ke_guest_event(object, &type);
        if (ge) {
            if (BRIDGE_MEM32(object + 4) == 0) {
                ResetEvent(ge);
                if (BRIDGE_MEM32(object + 4) != 0)     /* set meanwhile */
                    SetEvent(ge);
            } else {
                SetEvent(ge);                          /* set by a header write */
            }
            g_eax = (uint32_t)xbox_KeWaitForSingleObject(
                ge, wait_reason, wait_mode,
                (BOOLEAN)alertable, XBOX_TO_NATIVE(timeout_ptr));
            if (g_eax == 0 && type == 1)               /* synchronization: consumed */
                BRIDGE_MEM32(object + 4) = 0;
            return;
        }
    }

    h = ke_shadow_lookup(object);
    {
        int shadow_sync = h && ke_header_is_sync_event(object);
        if (!h)
            h = bridge_resolve_handle(object);
        if (!h)
            h = XBOX_TO_NATIVE(object);

        g_eax = (uint32_t)xbox_KeWaitForSingleObject(
            h, wait_reason, wait_mode,
            (BOOLEAN)alertable, XBOX_TO_NATIVE(timeout_ptr));
        if (g_eax == 0 && shadow_sync)   /* consumed, in the object's own SignalState too */
            ke_wait_satisfied(object);
    }
}

/* ── NtWaitForSingleObject (ordinal 233) ─────────────────── */
/*
 * The synchronous sibling of ...Ex. Halo's synchronous ReadFile issues the read
 * and then waits on its completion event through this; unbridged it fell to the
 * "return 0" default (STATUS_SUCCESS = "already signalled"), so the read handshake
 * completed before the data arrived and the UI-map precache never made progress.
 */
static void bridge_NtWaitForSingleObject(void)
{
    HANDLE   handle      = bridge_resolve_handle(STACK_ARG(0));
    uint32_t alertable   = STACK_ARG(1);
    uint32_t timeout_ptr = STACK_ARG(2);

    g_eax = (uint32_t)xbox_NtWaitForSingleObject(
        handle, (BOOLEAN)alertable, XBOX_TO_NATIVE(timeout_ptr));
}

/* ── NtClearEvent (ordinal 186) ──────────────────────────── */
/* Resets an event to non-signalled. Halo clears the read-completion event
 * before each async map read; a no-op here left the event stuck signalled. */
static void bridge_NtClearEvent(void)
{
    HANDLE handle = bridge_resolve_handle(STACK_ARG(0));
    g_eax = bridge_handle_status(handle, (uint32_t)xbox_NtClearEvent(handle));
}

/* ── NtSetEvent (ordinal 225) ────────────────────────────── */
/* Signals an event and optionally returns its previous state. Unbridged it
 * no-op'd, so a producer's "work ready" signal never landed -- Halo's map-copy
 * worker thread then slept forever in WaitForSingleObject on the decompress
 * context's go-event and only the first 14 KB of the map ever loaded. */
static void bridge_NtSetEvent(void)
{
    HANDLE   handle = bridge_resolve_handle(STACK_ARG(0));
    uint32_t prev   = STACK_ARG(1);
    if (!handle) { g_eax = 0xC0000008u; return; }   /* STATUS_INVALID_HANDLE */
    if (prev) BRIDGE_MEM32(prev) = bridge_event_state(handle);
    prev = 0;                                       /* written above */
    g_eax = bridge_handle_status(handle, (uint32_t)xbox_NtSetEvent(handle, XBOX_TO_NATIVE(prev)));
}

/* NtSetEvent for host-side code (HLE replacements of XDK functions) that has
 * to complete something the title waits on, by the title's event handle. */
void xbox_GuestSetEvent(uint32_t handle)
{
    xbox_NtSetEvent(bridge_resolve_handle(handle), NULL);
}

/* ── NtPulseEvent (ordinal 205) ──────────────────────────── */
/* Signal-then-reset: releases threads currently waiting, then leaves the event
 * non-signalled. Same unbridged-no-op hazard as NtSetEvent in the map-load
 * handoff chain. PulseEvent carries the (deprecated, lossy) Xbox semantics
 * faithfully -- a waiter not yet blocked misses it, exactly as on hardware. */
static void bridge_NtPulseEvent(void)
{
    HANDLE handle = bridge_resolve_handle(STACK_ARG(0));
    uint32_t prev = STACK_ARG(1);
    if (!handle) { g_eax = 0xC0000008u; return; }   /* STATUS_INVALID_HANDLE */
    if (prev) BRIDGE_MEM32(prev) = bridge_event_state(handle);
    g_eax = PulseEvent(handle) ? 0 : bridge_handle_status(handle, 0xC0000008u);
}

/* ── NtWaitForSingleObjectEx (ordinal 234) ───────────────── */
/*
 * Unbridged, this fell through to the "no bridge, returning 0" default -- and 0
 * is STATUS_SUCCESS, so every wait returned instantly as though the object were
 * already signalled. Halo's main loop then spun: 91 million calls in 100
 * seconds, no blocking, no progress. A wait that always succeeds is worse than
 * one that always fails, because it looks like the game is running.
 */
static HANDLE bridge_resolve_handle(uint32_t token);

static void bridge_NtWaitForSingleObjectEx(void)
{
    HANDLE   handle      = bridge_resolve_handle(STACK_ARG(0));
    uint32_t wait_mode   = STACK_ARG(1);
    uint32_t alertable   = STACK_ARG(2);
    uint32_t timeout_ptr = STACK_ARG(3);

    static int logged = 0;
    if (logged++ < 20) {
        fprintf(stderr, "  [KERNEL] NtWaitForSingleObjectEx: token=0x%08X "
                "handle=%p timeout=%s\n",
                STACK_ARG(0), handle, timeout_ptr ? "finite" : "INFINITE");
        fflush(stderr);
    }

    g_eax = (uint32_t)xbox_NtWaitForSingleObjectEx(
        handle, (KPROCESSOR_MODE)wait_mode, (BOOLEAN)alertable,
        XBOX_TO_NATIVE(timeout_ptr));
}

/* ── MmQueryAddressProtect (ordinal 179) ─────────────────── */
/* NtWaitForMultipleObjectsEx (ordinal 235, 6 args = 24 bytes)
 *
 * NTSTATUS NtWaitForMultipleObjectsEx(ULONG Count, HANDLE *Handles,
 *                                     WAIT_TYPE WaitType,
 *                                     KPROCESSOR_MODE WaitMode,
 *                                     BOOLEAN Alertable,
 *                                     PLARGE_INTEGER Timeout);
 *
 * Six, not five: WaitMode sits between WaitType and Alertable. Half-Life 2's
 * own call site settles it -- sub_0059BE0F pushes six dwords before the
 * thunk (esi, eax, ebx, 1, [ebp+0x18], edi) and the callee is expected to pop
 * them all.
 *
 * Getting it wrong cost 4 bytes of guest stack per call and shifted every
 * argument after WaitType, so the wait read Alertable as its timeout pointer
 * and reported INFINITE for every wait. The leak was invisible to the esp
 * invariant because sub_0059BE0F restores esp with `leave`: its own frame
 * came back correct while `pop edi; pop esi; pop ebx` took their values one
 * slot out, so the caller's `this` -- kept in ebx by sub_005ACDC0 -- came
 * back holding what esi had, and the next [ebx+0x94] read a string as a
 * pointer.
 *
 * xbox_NtWaitForMultipleObjectsEx has been in kernel_sync.c all along; only
 * the bridge wrapper was missing, so the thunk fell through to the fallback
 * and returned 0 -- STATUS_SUCCESS, meaning 'object 0 is signalled'. A wait
 * that always reports signalled turns a blocking wait into a busy loop, which
 * is exactly what Half-Life 2 does after spawning its first worker: the main
 * thread spins in a CUtlLinkedList walk making no indirect calls at all.
 *
 * Handles is a guest array of tokens, so each has to be resolved
 * individually -- the array cannot just be pointed at. Bounded because a
 * bogus Count would otherwise read arbitrary guest memory onto the stack;
 * MAXIMUM_WAIT_OBJECTS is the real kernel's own limit.
 */
static void bridge_NtWaitForMultipleObjectsEx(void)
{
    uint32_t count       = STACK_ARG(0);
    uint32_t handles_va  = STACK_ARG(1);
    uint32_t wait_type   = STACK_ARG(2);
    uint32_t wait_mode   = STACK_ARG(3);   /* KernelMode / UserMode */
    uint32_t alertable   = STACK_ARG(4);
    uint32_t timeout_ptr = STACK_ARG(5);

    (void)wait_mode;
    HANDLE   handles[MAXIMUM_WAIT_OBJECTS];
    uint32_t i;

    if (count == 0 || count > MAXIMUM_WAIT_OBJECTS || !handles_va) {
        g_eax = 0xC000000Du;             /* STATUS_INVALID_PARAMETER */
        return;
    }
    if (wait_type > 1) {
        g_eax = 0xC000000Du;             /* STATUS_INVALID_PARAMETER */
        return;
    }
    for (i = 0; i < count; i++) {
        handles[i] = bridge_resolve_handle(BRIDGE_MEM32(handles_va + i * 4));
        if (!handles[i]) {
            g_eax = 0xC0000008u;         /* STATUS_INVALID_HANDLE */
            return;
        }
    }
    if (wait_type == 0) {                /* WaitAll takes an object only once */
        uint32_t j;
        for (i = 0; i < count; i++)
            for (j = i + 1; j < count; j++)
                if (handles[i] == handles[j]) {
                    g_eax = 0xC0000030u; /* STATUS_INVALID_PARAMETER_MIX */
                    return;
                }
    }

    {
        static int logged;
        if (logged++ < 20) {
            fprintf(stderr, "  [KERNEL] NtWaitForMultipleObjectsEx: count=%u type=%u timeout=%s\n",
                    count, wait_type, timeout_ptr ? "finite" : "INFINITE");
            for (i = 0; i < count; i++)
                fprintf(stderr, "      [%u] token=0x%08X host=%p\n", i,
                        BRIDGE_MEM32(handles_va + i * 4), handles[i]);
            fflush(stderr);
        }
    }

    g_eax = (uint32_t)xbox_NtWaitForMultipleObjectsEx(
        count, handles, wait_type, (BOOLEAN)alertable,
        XBOX_TO_NATIVE(timeout_ptr));
}

/*
 * Takes an Xbox VA, so the native pointer has to be formed before the query --
 * an unbridged 0 return reads as PAGE_NOACCESS. Halo walks all 22 MB of its
 * physical memory map asserting every page is PAGE_READWRITE
 * (physical_memory_map.c:77), so a zero here stops startup on the first page.
 */
static void bridge_MmQueryAddressProtect(void)
{
    uint32_t address = STACK_ARG(0);

    g_eax = address ? (uint32_t)xbox_MmQueryAddressProtect(XBOX_TO_NATIVE(address))
                    : 0;
}

/* ── NtUserIoApcDispatcher (ordinal 232) ─────────────────── */
/*
 * The kernel side of XAPI's ReadFileEx/WriteFileEx. XAPI passes *this* as the
 * ApcRoutine to NtReadFile and puts the title's completion routine in
 * ApcContext, so the dispatcher's only job is to call it with Win32 argument
 * shape:
 *
 *   VOID CALLBACK Completion(DWORD dwErrorCode,
 *                            DWORD dwNumberOfBytesTransfered,
 *                            LPOVERLAPPED lpOverlapped)   // __stdcall, ret 12
 *
 * lpOverlapped is the IO_STATUS_BLOCK pointer: an NT OVERLAPPED begins with
 * Internal/InternalHigh, which is exactly a IO_STATUS_BLOCK, so the title's
 * OVERLAPPED and the block it handed to NtReadFile are the same address.
 * Halo's cache_files_windows completion relies on that -- it reads its own
 * field at lpOverlapped+0x10 and sets the flag the setup loop polls.
 */
static void bridge_NtUserIoApcDispatcher(void)
{
    uint32_t apc_context = STACK_ARG(0);
    uint32_t iostatus    = STACK_ARG(1);
    uint32_t status      = iostatus ? BRIDGE_MEM32(iostatus) : 0;
    uint32_t information = iostatus ? BRIDGE_MEM32(iostatus + 4) : 0;
    recomp_func_t fn;

    fn = recomp_lookup(apc_context);
    if (!fn) fn = recomp_lookup_manual(apc_context);
    if (!fn) {
        fprintf(stderr, "  [KERNEL] NtUserIoApcDispatcher: completion routine "
                "0x%08X not in dispatch\n", apc_context);
        fflush(stderr);
        g_eax = 0;
        return;
    }

    /* __stdcall, right-to-left. The callee's `ret 12` consumes the dummy
     * return address and all three arguments, so g_esp needs no fixup here. */
    g_esp -= 4; BRIDGE_MEM32(g_esp) = iostatus;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = information;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = (status == 0) ? 0 : status;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
    fn();

    g_eax = 0;
}

/* ── KeDelayExecutionThread (ordinal 99) ─────────────────── */
/* Unbridged this returned instantly, turning every "sleep and retry" in the
 * title into a hot spin. Halo's cache-partition setup retries this way. */
static void bridge_KeDelayExecutionThread(void)
{
    uint32_t wait_mode    = STACK_ARG(0);
    uint32_t alertable    = STACK_ARG(1);
    uint32_t interval_ptr = STACK_ARG(2);


    g_eax = (uint32_t)xbox_KeDelayExecutionThread(
        (KPROCESSOR_MODE)wait_mode, (BOOLEAN)alertable,
        XBOX_TO_NATIVE(interval_ptr));
}

/* ── KeBugCheck (ordinal 95) / KeBugCheckEx (96) ─────────── */
/*
 * The title asking the kernel to die. Unbridged this returned 0 and execution
 * carried on into whatever the bug check was there to prevent, so the real
 * failure surfaced later somewhere unrelated. Report the code and stop
 * pretending the call succeeded.
 */
/* A console never returns from a bug check. RECOMP_BUGCHECK_STOP=1 does the same,
 * for testing; by default the title carries on, with STATUS 0, so a port survives
 * a check that may only reflect our own kernel. */
static void bridge_bugcheck_end(uint32_t code)
{
    const char *e = getenv("RECOMP_BUGCHECK_STOP");
    if (e && *e && *e != '0') {
        fprintf(stderr, "[EXIT] bug check 0x%08X: RECOMP_BUGCHECK_STOP is set, stopping as a console does\n", code);
        fflush(stderr);
        ExitProcess(code);
    }
    g_eax = 0;
}

static void bridge_KeBugCheck(void)
{
    fprintf(stderr, "  [KERNEL] *** KeBugCheck: code=0x%08X ***\n",
            STACK_ARG(0));
    fflush(stderr);
    bridge_bugcheck_end(STACK_ARG(0));
}

static void bridge_KeBugCheckEx(void)
{
    fprintf(stderr, "  [KERNEL] *** KeBugCheckEx: code=0x%08X "
            "(0x%08X, 0x%08X, 0x%08X, 0x%08X) ***\n",
            STACK_ARG(0), STACK_ARG(1), STACK_ARG(2),
            STACK_ARG(3), STACK_ARG(4));
    fflush(stderr);
    bridge_bugcheck_end(STACK_ARG(0));
}

/* ── NtYieldExecution (ordinal 238) ──────────────────────── */
static void bridge_NtYieldExecution(void)
{
    g_eax = (uint32_t)xbox_NtYieldExecution();
}

/* ── MmGetPhysicalAddress (ordinal 173) ──────────────────── */
static void bridge_MmGetPhysicalAddress(void)
{
    uint32_t addr = STACK_ARG(0);
    /* Calls the same xbox_* the thunk table exposes, so there is one
     * implementation rather than two that have to be kept in agreement.
     * Note this bridge itself is not covered by any test: bridge functions
     * are static and driven by guest CPU state, and nothing in tests/ can
     * reach them. */
    g_eax = (uint32_t)xbox_MmGetPhysicalAddress((PVOID)(uintptr_t)addr);
}

/* ── MmSetAddressProtect (ordinal 182) ───────────────────── */
static int mm_protect_ok(uint32_t p);

static void bridge_MmSetAddressProtect(void)
{
    uint32_t addr = STACK_ARG(0);
    uint32_t size = STACK_ARG(1);
    uint32_t prot = STACK_ARG(2);

    if (mm_protect_ok(prot))   /* otherwise nothing, as the kernel */
        xbox_MmSetAddressProtect(XBOX_TO_NATIVE(addr), size, prot);
    g_eax = 0;
}

/* ── AvSetDisplayMode (ordinal 3) ────────────────────────── */
static void bridge_AvSetDisplayMode(void)
{
    uint32_t addr = STACK_ARG(0);
    uint32_t step = STACK_ARG(1);
    uint32_t mode = STACK_ARG(2);
    uint32_t format = STACK_ARG(3);
    uint32_t pitch = STACK_ARG(4);
    uint32_t fb = STACK_ARG(5);

    /* The framebuffer the display is meant to scan out, and the format it is
     * in. This is the only place the address is stated: the title never writes
     * PCRTC_START itself, so without this there is nothing that says where the
     * guest believes its picture is. */
    fprintf(stderr, "  [AV] SetDisplayMode mode=0x%08X format=0x%08X"
                    " pitch=%u fb=0x%08X\n", mode, format, pitch, fb);
    fflush(stderr);

    {
        /* Point the framebuffer window at whatever the title just set, and
         * start it on the first display mode -- before that there is nothing
         * to show and no pitch to interpret it with. */
        extern void xbox_FramebufferWindowSet(uint32_t, uint32_t);
        extern void xbox_FramebufferWindowStart(void);
        uint32_t fb_va = fb;

        /* AvSetDisplayMode reports the scanout address the way the CRTC wants
         * it -- a physical address. The window has to read it the way the CPU
         * sees it. Where the title allocated says which: a framebuffer from
         * MmAllocateContiguousMemory lives in the window at XBOX_CONTIG_BASE,
         * so physical P is visible at XBOX_CONTIG_BASE + P. Reading P
         * directly lands in the loaded image instead, which is why the window
         * showed black while the executor was clearing and rasterising
         * correctly a few megabytes away. */
        if (fb_va && fb_va < XBOX_CONTIG_SIZE)
            fb_va = XBOX_CONTIG_BASE + fb_va;

        xbox_FramebufferWindowSet(fb_va, pitch);
        xbox_FramebufferWindowStart();

        /* Record the resolved address, not the physical one the title passed.
         * Every reader of this wants to read guest memory with it, and the
         * physical form lands in the loaded image -- so the checksum probe
         * reported an unchanging zero while the title was drawing correctly a
         * few megabytes away, and so did every dump that asked for "the"
         * framebuffer. Resolving once here is the fix; resolving in each
         * caller is how there came to be two of them disagreeing. */
        xbox_SetDisplayFramebuffer(fb_va, pitch);
    }
    xbox_AvSetDisplayMode(XBOX_TO_NATIVE(addr), step, mode, format, pitch, fb);
    g_eax = 0;
}

/* ── PsTerminateSystemThread (ordinal 258) ───────────────
 * VOID PsTerminateSystemThread(NTSTATUS ExitStatus)
 *
 * On real Xbox, this terminates the calling thread (never returns).
 * In our recompiled version, threads run synchronously, so we just
 * return. The caller (sub_001D1818) handles this gracefully.
 */
static void bridge_PsTerminateSystemThread(void)
{
    uint32_t exit_status = STACK_ARG(0);

    fprintf(stderr, "  [KERNEL] PsTerminateSystemThread: status=0x%08X%s\n",
            exit_status, g_is_spawned_thread ? " (worker)" : " (main)");
    fflush(stderr);

    g_eax = exit_status;

    /*
     * This does not return on hardware. Returning was survivable while every
     * thread ran on the host's main thread, but a spawned worker that returns
     * here falls off the end of its start routine and into whatever bytes
     * follow -- Halo's input worker landed on an int 3, and the resulting
     * breakpoint took down the whole process while the main thread was still
     * inside input_initialize.
     *
     * The main thread still returns: it is the host's thread and unwinding
     * back to main() is how the process shuts down cleanly.
     */
    if (g_is_spawned_thread) {
        /* The normal exit for a worker, and therefore the one that has to
         * return the stack -- ExitThread never comes back to bridge_thread_main
         * to do it. */
        bridge_thread_notify(NULL, 0);
        bridge_thread_mark_exit(exit_status);
        xbox_FreeThreadStack(g_thread_stack_top);
        g_thread_stack_top = 0;
        ExitThread(exit_status);
    }
}

/* ── HalReadSMCTrayState (ordinal 47) ─────────────────────
 * VOID HalReadSMCTrayState(PDWORD TrayState, PDWORD TrayStateChangeCount)
 *
 * Returns DVD tray state. 0x10 = no disc, 0x14 = tray closed with disc.
 */
static void bridge_HalReadSMCTrayState(void)
{
    uint32_t state_ptr = STACK_ARG(0);
    uint32_t count_ptr = STACK_ARG(1);

    /* As the kernel, with the tray never moving: media present (0x60; 0x10 is an open tray), no changes counted. The
     * count pointer holding 0x11223344 asks for a second counter, also 0 here. */
    if (state_ptr) BRIDGE_MEM32(state_ptr) = 0x60;
    if (count_ptr) BRIDGE_MEM32(count_ptr) = 0;
    g_eax = 0;
}

/* -- KeInsertQueueDpc (ordinal 119) ------------------------
 * BOOLEAN KeInsertQueueDpc(PKDPC Dpc, PVOID SystemArgument1,
 *                          PVOID SystemArgument2)
 *
 * Unbridged this returned 0, which reads to a driver as "already queued" and
 * means the deferred routine never runs. That is not a small loss: an
 * interrupt service routine is expected to do almost nothing except mask the
 * source and queue a DPC, so with this missing every driver that follows the
 * normal pattern acknowledges its interrupt and then does none of the work.
 * Half-Life 2's USB stack does exactly that -- its ISR masks the master
 * interrupt enable and queues here, and the enumeration it should have started
 * lives entirely in the deferred routine.
 *
 * ponytail: runs the routine inline rather than queueing it. A real DPC runs
 * at DISPATCH_LEVEL shortly after the ISR returns, and this runs it before the
 * ISR returns, on whichever thread queued it. That ordering difference has not
 * mattered for anything here yet; when it does, the upgrade is a real queue
 * drained by the thread that lowered IRQL, not a second call site.
 */
/* Run a DPC's deferred routine on the calling thread.
 *
 * VOID DeferredRoutine(PKDPC, PVOID Context, PVOID Arg1, PVOID Arg2),
 * __stdcall: its `ret 16` consumes the dummy return address and all four
 * arguments, so g_esp needs no fixup afterwards. The caller must already have
 * a guest stack -- true on a guest thread, and true on a host thread that has
 * taken a worker slice.
 *
 * Returns 1 if the routine was found and called.
 */
static int kernel_run_dpc(uint32_t dpc_va, uint32_t arg1, uint32_t arg2)
{
    uint32_t routine, context;
    recomp_func_t fn;

    if (!dpc_va)
        return 0;
    routine = BRIDGE_MEM32(dpc_va + 12);
    context = BRIDGE_MEM32(dpc_va + 16);
    if (!routine)
        return 0;

    fn = recomp_lookup(routine);
    if (!fn) fn = recomp_lookup_manual(routine);
    if (!fn) {
        fprintf(stderr, "  [KERNEL] DPC routine 0x%08X not in dispatch\n",
                routine);
        fflush(stderr);
        return 0;
    }

    BRIDGE_MEM32(dpc_va + 20) = arg1;
    BRIDGE_MEM32(dpc_va + 24) = arg2;

    g_esp -= 4; BRIDGE_MEM32(g_esp) = arg2;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = arg1;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = context;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = dpc_va;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
    { int _irql = xbox_IrqlEnterInterrupt(2); fn(); xbox_IrqlLeaveInterrupt(_irql); }
    return 1;
}

/* -- KeSynchronizeExecution (ordinal 153) ------------------
 * BOOLEAN KeSynchronizeExecution(PKINTERRUPT Interrupt,
 *                                PKSYNCHRONIZE_ROUTINE SynchronizeRoutine,
 *                                PVOID SynchronizeContext)
 *
 * Runs a routine while holding the interrupt's spinlock at the ISR's IRQL,
 * which is how a driver touches its hardware from anywhere that is not the
 * ISR without racing the ISR. Unbridged it returned 0 and the routine never
 * ran -- the same failure as an unqueued DPC, and just as quiet: the driver
 * asks for exclusive access, is told it did not get it, and skips the work.
 *
 * ponytail: no lock is taken. Nothing else here runs at ISR IRQL, and the one
 * caller that matters is a device model on its own thread; if two of those
 * ever contend, this wants the interrupt object's own lock rather than a
 * global one.
 */
static void bridge_KeSynchronizeExecution(void)
{
    uint32_t routine = STACK_ARG(1);
    uint32_t context = STACK_ARG(2);
    recomp_func_t fn;

    fn = routine ? recomp_lookup(routine) : NULL;
    if (!fn && routine) fn = recomp_lookup_manual(routine);
    if (!fn) {
        fprintf(stderr, "  [KERNEL] KeSynchronizeExecution: routine 0x%08X "
                        "not in dispatch\n", routine);
        fflush(stderr);
        g_eax = 0;
        return;
    }

    /* BOOLEAN SynchronizeRoutine(PVOID Context), __stdcall. Its `ret 4` takes
     * the dummy return address and the argument, so g_esp needs no fixup. */
    g_esp -= 4; BRIDGE_MEM32(g_esp) = context;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
    xbox_IsrLock();
    fn();
    xbox_IsrUnlock();
    /* g_eax is whatever the routine returned, which is this call's result. */
}

/* -- KeRemoveQueueDpc (ordinal 137) ------------------------
 * BOOLEAN KeRemoveQueueDpc(PKDPC Dpc)
 *
 * Cancels a queued DPC, returning whether it was still in the queue. DPCs run
 * inline here (see KeInsertQueueDpc), so by the time anyone can call this the
 * routine has already run and there is nothing to cancel. FALSE is both the
 * honest answer and the one that keeps a caller's bookkeeping right.
 */
static void bridge_KeRemoveQueueDpc(void);   /* defined with the queue */

/* The pending DPC queue.
 *
 * These used to run inline, on whichever thread queued them, before the caller
 * returned. That is not what a DPC is: an interrupt service routine masks its
 * source and queues, and the deferred routine runs afterwards at DISPATCH_LEVEL
 * -- after the ISR has returned. Running it inline inverts that, so a routine
 * that re-enables interrupts does it while the ISR that masked them is still on
 * the stack, and a driver's state machine re-enters itself from inside its own
 * interrupt.
 *
 * Queued properly now, and drained by the timer thread, which is the one thread
 * here that already has a guest stack and a TIB and runs nothing else urgent.
 *
 * ponytail: one queue, no IRQL, no per-processor list, and a DPC queued from a
 * DPC runs on the next drain rather than immediately. Nothing here depends on
 * DPC ordering beyond "after the ISR".
 */
/* The console links a DPC through the object itself: no limit. This queue grows. */
#define XBOX_FIRST_PENDING_DPC 64
typedef struct { uint32_t dpc, arg1, arg2; } PendingDpc;
static PendingDpc *g_dpc_queue;
static LONG g_dpc_cap;
static volatile LONG g_dpc_head, g_dpc_tail;

/* The queue is fed from several host threads at once -- the USB and APU
 * controller threads raise interrupts whose ISRs queue DPCs, and the title
 * queues its own -- and was an unlocked ring: two inserts could take the same
 * slot and one DPC was lost. A lost USB DPC is a done queue the driver never
 * acknowledges; the controller then completes nothing more and the pad goes
 * dead mid-game, which is what it did.
 *
 * And a DPC already queued is not queued twice: the kernel keeps an Inserted
 * flag in the KDPC (+2) and KeInsertQueueDpc returns FALSE while it is set.
 * Running a driver's DPC twice for one interrupt makes it walk a done list it
 * has already consumed. */
static CRITICAL_SECTION g_dpc_lock;
static INIT_ONCE g_dpc_lock_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK dpc_lock_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&g_dpc_lock);
    return TRUE;
}

static void dpc_lock(void)
{
    InitOnceExecuteOnce(&g_dpc_lock_once, dpc_lock_init, NULL, NULL);
    EnterCriticalSection(&g_dpc_lock);
}

static void bridge_KeInsertQueueDpc(void)
{
    uint32_t dpc  = STACK_ARG(0);
    uint32_t arg1 = STACK_ARG(1);
    uint32_t arg2 = STACK_ARG(2);
    LONG tail, next;

    if (!dpc) { g_eax = 0; return; }

    dpc_lock();
    if (BRIDGE_MEM8(dpc + 2)) {                 /* already queued */
        LeaveCriticalSection(&g_dpc_lock);
        g_eax = 0;
        return;
    }
    tail = g_dpc_tail;
    next = g_dpc_cap ? (tail + 1) % g_dpc_cap : 0;
    if (!g_dpc_cap || next == g_dpc_head) {
        LONG cap = g_dpc_cap ? g_dpc_cap * 2 : XBOX_FIRST_PENDING_DPC, n = 0, i;
        PendingDpc *q = (PendingDpc *)malloc((size_t)cap * sizeof(PendingDpc));
        if (q) {
            for (i = g_dpc_head; i != tail; i = (i + 1) % g_dpc_cap)
                q[n++] = g_dpc_queue[i];
            free(g_dpc_queue);
            g_dpc_queue = q;
            g_dpc_cap = cap;
            g_dpc_head = 0;
            tail = n;
            g_dpc_tail = n;
            next = n + 1;
        }
    }
    if (!g_dpc_cap || next == g_dpc_head) {
        LeaveCriticalSection(&g_dpc_lock);
        fprintf(stderr, "  [KERNEL] DPC queue full, dropping 0x%08X\n", dpc);
        fflush(stderr);
        g_eax = 0;
        return;
    }
    g_dpc_queue[tail].dpc  = dpc;
    g_dpc_queue[tail].arg1 = arg1;
    g_dpc_queue[tail].arg2 = arg2;
    BRIDGE_MEM8(dpc + 2) = 1;
    g_dpc_tail = next;
    LeaveCriticalSection(&g_dpc_lock);
    g_eax = 1;
}

/* Cancel a queued DPC: take it out of the queue if it is still there. */
static void bridge_KeRemoveQueueDpc(void)
{
    uint32_t dpc = STACK_ARG(0);
    LONG i;

    g_eax = 0;
    if (!dpc)
        return;
    dpc_lock();
    if (BRIDGE_MEM8(dpc + 2)) {
        for (i = g_dpc_head; i != g_dpc_tail; i = (i + 1) % g_dpc_cap)
            if (g_dpc_queue[i].dpc == dpc)
                g_dpc_queue[i].dpc = 0;         /* drained as a no-op */
        BRIDGE_MEM8(dpc + 2) = 0;
        g_eax = 1;
    }
    LeaveCriticalSection(&g_dpc_lock);
}

/* Call a connected interrupt service routine.
 *
 * BOOLEAN ServiceRoutine(PKINTERRUPT, PVOID ServiceContext), __stdcall: its
 * `ret 8` takes the dummy return address and both arguments, so g_esp needs no
 * fixup. The caller must already have a guest stack and a TIB, which the timer
 * thread has.
 *
 * Returns what the routine returned -- an ISR that does not recognise the
 * interrupt returns FALSE, and that is worth seeing rather than assuming.
 */
uint32_t xbox_GetConnectedInterrupt(uint32_t vector);   /* defined below */

static int kernel_raise_interrupt(uint32_t vector)
{
    uint32_t kint = xbox_GetConnectedInterrupt(vector);
    uint32_t routine, context;
    recomp_func_t fn;

    if (!kint)
        return -1;
    routine = BRIDGE_MEM32(kint + 0);
    context = BRIDGE_MEM32(kint + 4);
    if (!routine)
        return -1;
    fn = recomp_lookup(routine);
    if (!fn) fn = recomp_lookup_manual(routine);
    if (!fn)
        return -1;

    g_esp -= 4; BRIDGE_MEM32(g_esp) = context;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = kint;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
    { int _irql = xbox_IrqlEnterInterrupt(16); fn(); xbox_IrqlLeaveInterrupt(_irql); }
    return (int)(g_eax & 1u);
}

/* The GPU's vertical blank, delivered rather than merely enabled.
 *
 * The D3D8 library linked into a title installs an ISR for this and then waits
 * on it. Nothing ever raised it, so a title whose frame loop waits for vblank
 * rather than polling stops after its first clear -- which is exactly where
 * Half-Life 2's loader stops, with its videos open, its 23 MB of UI textures
 * loaded, and no second frame.
 *
 * The ISR reads the NV2A's own interrupt status to decide whether the
 * interrupt is its business, so the registers have to say vblank before the
 * routine is called: PMC_INTR_0 bit 24 says the PCRTC block is the source.
 * Without it the handler looks, finds nothing, and correctly declines.
 * xbox_Nv2aRaiseVblank sets it and keeps it set until the guest writes
 * PCRTC_INTR_0 to acknowledge, because D3D's DPC looks again after the ISR,
 * and only counts the vblank if the bit is still there.
 *
 * ponytail: a fixed 60 Hz off the timer tick rather than anything tied to the
 * display mode, and no field or interlace handling. A title that measures
 * refresh rate from this will read 60; a title that needs the real one wants
 * the mode AvSetDisplayMode was given, which is recorded a few files away.
 */
#define NV2A_VECTOR            3u

/* A host routine run on every vblank from the timer thread (guest stack and TIB in place), for an HLE Direct3D that
 * never connects the XDK's vblank ISR but owes the title its D3DDevice_SetVerticalBlankCallback calls. */
static void (*g_vblank_hook)(void);

void xbox_SetVblankHook(void (*fn)(void))
{
    g_vblank_hook = fn;
}

static void kernel_vblank_tick(void)
{
    static int enabled = -1;
    static long long next_ms;   /* performance-counter ticks */
    long long now;

    if (enabled < 0)
        enabled = getenv("RECOMP_VBLANK") != NULL;
    if (!enabled)
        return;

    /* A fixed NTSC field grid (60000/1001 Hz) on the performance counter.
     * Scheduling each tick 16 ms after whenever the last one was seen let the
     * timer thread's own wake-up delay accumulate: with a 10 ms poll the clock
     * ran at ~46 Hz, and an XMV movie, which shows frames by field count,
     * played at three-quarter speed in bursts. Stepping the deadline by the
     * period keeps the average exact whatever the poll; a backlog of more
     * than a few fields (the process was stopped) resyncs rather than firing
     * them all at once. */
    {
        static LARGE_INTEGER freq;
        LARGE_INTEGER qpc;
        long long period;
        if (!freq.QuadPart)
            QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&qpc);
        now = qpc.QuadPart;
        period = freq.QuadPart * 1001 / 60000;
        if (!next_ms || now - next_ms > 4 * period)
            next_ms = now;
        if (now < next_ms)
            return;
        next_ms += period;
    }

    xbox_Nv2aCountVblank();
    if (g_vblank_hook)
        g_vblank_hook();
    if (!xbox_GetConnectedInterrupt(NV2A_VECTOR))
        return;

    xbox_Nv2aRaiseVblank();

    {
        static unsigned n;
        int claimed = kernel_raise_interrupt(NV2A_VECTOR);
        if (n++ < 3)
            fprintf(stderr, "  [NV2A] vblank -> ISR %s\n",
                    claimed < 0 ? "not callable" :
                    claimed ? "claimed it" : "declined it");
        fflush(stderr);
    }
}

/* Run whatever is queued. Called from the timer thread, which has the guest
 * stack and TIB that a deferred routine needs. */
static void kernel_drain_dpcs(void)
{
    for (;;) {
        PendingDpc d;
        dpc_lock();
        if (g_dpc_head == g_dpc_tail) {
            LeaveCriticalSection(&g_dpc_lock);
            break;
        }
        d = g_dpc_queue[g_dpc_head];
        g_dpc_head = (g_dpc_head + 1) % g_dpc_cap;
        /* Cleared before the routine runs, as the kernel does: the routine
         * may queue itself again. */
        if (d.dpc)
            BRIDGE_MEM8(d.dpc + 2) = 0;
        LeaveCriticalSection(&g_dpc_lock);
        if (d.dpc)
        {
            t_in_dpc = 1;
            kernel_run_dpc(d.dpc, d.arg1, d.arg2);
            t_in_dpc = 0;
        }
    }
}

/* ── KeInitializeDpc (ordinal 107) ────────────────────────
 * VOID KeInitializeDpc(PKDPC Dpc, PKDEFERRED_ROUTINE DeferredRoutine,
 *                       PVOID DeferredContext)
 *
 * Initializes a DPC object. The Xbox KDPC structure is 32 bytes.
 * We zero it and set the routine and context pointers.
 */
static void bridge_KeInitializeDpc(void)
{
    uint32_t dpc_va = STACK_ARG(0);
    uint32_t routine = STACK_ARG(1);
    uint32_t context = STACK_ARG(2);

    /* XBOX_TO_NATIVE maps a guest 0 to NULL, so an unchecked object pointer
     * makes this memset write through NULL inside the bridge. The export
     * returns void, so refusing is doing nothing -- which is what the real
     * kernel does with an object it cannot write. */
    if (!bridge_buf_ok(dpc_va, 28, "KeInitializeDpc")) {
        g_eax = 0;
        return;
    }

    /* As the kernel: only these fields. A KDPC is 0x1C bytes, and whatever the
     * title keeps right after it isn't ours to clear. */
    BRIDGE_MEM8(dpc_va + 2) = 0;       /* Inserted */
    BRIDGE_MEM16(dpc_va + 0) = 0x13;   /* Type */
    BRIDGE_MEM32(dpc_va + 12) = routine; /* DeferredRoutine */
    BRIDGE_MEM32(dpc_va + 16) = context; /* DeferredContext */
    g_eax = 0;
}

/* ── NV2A interrupt plumbing (ordinals 44, 98, 109) ───────
 *
 * The D3D8 library linked into a title installs an ISR for the GPU's vblank /
 * command-completion interrupt. There is no NV2A here and nothing ever raises
 * that interrupt, so these exist to let initialisation complete rather than to
 * deliver anything.
 *
 * KeConnectInterrupt reports success: reporting failure sends Halo's
 * rasterizer down an error path during preinitialize, and the goal is to get
 * past setup, not to pretend the hardware is broken.
 *
 * ponytail: no interrupt is ever delivered. Code that *waits* on the ISR
 * rather than polling will hang here, and the fix for that is to bridge the
 * D3D8 entry point that owns the wait, not to synthesise NV2A interrupts.
 */

/* ULONG HalGetInterruptVector(ULONG BusInterruptLevel, PKIRQL Irql) */
static void bridge_HalGetInterruptVector(void)
{
    uint32_t level   = STACK_ARG(0);
    uint32_t irql_va = STACK_ARG(1);

    /* As the kernel: levels 0 to 0x1A are vectors 0x30 up, the IRQL falling as the vector rises; any other level is
     * no vector, with the IRQL left alone. */
    uint32_t vector = level + 0x30;
    if (vector < 0x30 || vector > 0x4A) {
        g_eax = 0;
        return;
    }
    if (irql_va)
        BRIDGE_MEM8(irql_va) = (uint8_t)(0x4A - vector);
    g_eax = vector;
}

/* VOID KeInitializeInterrupt(PKINTERRUPT, ServiceRoutine, ServiceContext,
 *                            Vector, Irql, InterruptMode, ShareVector) */
static void bridge_KeInitializeInterrupt(void)
{
    uint32_t interrupt_va = STACK_ARG(0);
    uint32_t routine      = STACK_ARG(1);
    uint32_t context      = STACK_ARG(2);
    uint32_t vector       = STACK_ARG(3);

    if (!bridge_buf_ok(interrupt_va, 44, "KeInitializeInterrupt")) {
        g_eax = 0;
        return;
    }

    /* As the kernel: +0 routine, +4 context, +8 the level (vector less 0x30), +0xC IRQL, +0x10 connected, +0x12 mode.
     * The dispatch code it copies to +0x18 has no use here, and nothing past its own fields is touched. */
    BRIDGE_MEM32(interrupt_va + 0)  = routine;
    BRIDGE_MEM32(interrupt_va + 4)  = context;
    BRIDGE_MEM32(interrupt_va + 8)  = vector >= 0x30 ? vector - 0x30 : vector;
    BRIDGE_MEM32(interrupt_va + 0xC) = STACK_ARG(4) & 0xFF;
    BRIDGE_MEM8(interrupt_va + 0x10) = 0;
    BRIDGE_MEM8(interrupt_va + 0x12) = (uint8_t)STACK_ARG(5);
    g_eax = 0;
}

/* The interrupt objects a title has connected, by vector.
 *
 * KeInitializeInterrupt already writes the service routine, its context and
 * the vector into the guest KINTERRUPT; connecting is what says the title is
 * ready to be called on it. Recording that is what lets a device model raise
 * an interrupt at all -- without it the routine is written down in guest
 * memory and nothing on this side knows it is there.
 *
 * A small fixed table rather than a list: an Xbox has 26 interrupt vectors and
 * a title connects a handful, so the whole thing is smaller than the comment.
 */
#define XBOX_MAX_VECTORS 32
static uint32_t g_connected_isr[XBOX_MAX_VECTORS];   /* KINTERRUPT guest VA */

/* BOOLEAN KeConnectInterrupt(PKINTERRUPT Interrupt) */
static void bridge_KeConnectInterrupt(void)
{
    uint32_t interrupt_va = STACK_ARG(0);

    if (interrupt_va && BRIDGE_MEM8(interrupt_va + 0x10)) {   /* already connected: FALSE, as the kernel */
        g_eax = 0;
        return;
    }
    if (interrupt_va) {
        uint32_t vector = BRIDGE_MEM32(interrupt_va + 8);
        if (vector < XBOX_MAX_VECTORS) {
            BRIDGE_MEM8(interrupt_va + 0x10) = 1;
            g_connected_isr[vector] = interrupt_va;
            fprintf(stderr, "  [KERNEL] KeConnectInterrupt: vector %u -> "
                            "routine 0x%08X context 0x%08X\n",
                    vector, BRIDGE_MEM32(interrupt_va + 0),
                    BRIDGE_MEM32(interrupt_va + 4));
            fflush(stderr);
        }
    }
    g_eax = 1;  /* connected -- see the note above */
}

/* The KINTERRUPT a title connected on this vector, or 0. Device models use it
 * to find the routine to call; the routine and its context are at +0 and +4. */
uint32_t xbox_GetConnectedInterrupt(uint32_t vector)
{
    return (vector < XBOX_MAX_VECTORS) ? g_connected_isr[vector] : 0;
}

/* ── MmClaimGpuInstanceMemory (ordinal 168) ───────────────
 * PVOID MmClaimGpuInstanceMemory(SIZE_T NumberOfBytes, SIZE_T *Padding)
 *
 * Reserves the GPU instance memory the NV2A keeps its object context in. On
 * hardware it sits at the very top of physical RAM, so the returned address is
 * the end of the contiguous window minus the request. D3D8 stores this and
 * indexes off it, so returning 0 (the unbridged default) had it building
 * pointers from a null base.
 *
 * MAXULONG_PTR means "claim everything left"; the console answers with the
 * default instance size rather than the whole of RAM.
 */
static void bridge_MmClaimGpuInstanceMemory(void)
{
    uint32_t bytes      = STACK_ARG(0);
    uint32_t padding_va = STACK_ARG(1);

    if (bytes == 0xFFFFFFFFu) {
        bytes = XBOX_GPU_INSTANCE_DEFAULT;
    }
    if (padding_va) {
        BRIDGE_MEM32(padding_va) = 0;
    }
    g_eax = XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE - bytes;
}

/* VOID HalRegisterShutdownNotification(PHAL_SHUTDOWN_REGISTRATION, BOOLEAN)
 * Records a callback for console shutdown. Nothing here ever shuts down that
 * way, so registration is accepted and dropped. */
/* HalRegisterShutdownNotification(Registration, Register): the record is { Routine, Priority, ListEntry }, kept highest
 * priority first and, among equals, in the order registered. The kernel calls each with the record as HalReturnToFirmware
 * runs; here only with RECOMP_SHUTDOWN_NOTIFY=1, until tried in games, since a routine that waits would stop us exiting. */
#define XBOX_SHUTDOWN_MAX 256
static uint32_t g_shutdown_reg[XBOX_SHUTDOWN_MAX];
static int g_shutdown_count;
static SRWLOCK g_shutdown_lock = SRWLOCK_INIT;

static void bridge_HalRegisterShutdownNotification(void)
{
    uint32_t reg = STACK_ARG(0), add = STACK_ARG(1) & 0xFF;
    int i;
    AcquireSRWLockExclusive(&g_shutdown_lock);
    for (i = 0; i < g_shutdown_count && g_shutdown_reg[i] != reg; i++) {}
    if (i < g_shutdown_count) {  /* already there: taken out, and put back below if asked */
        memmove(&g_shutdown_reg[i], &g_shutdown_reg[i + 1], (g_shutdown_count - i - 1) * sizeof(uint32_t));
        g_shutdown_count--;
    }
    if (add && g_shutdown_count < XBOX_SHUTDOWN_MAX) {
        int32_t priority = (int32_t)BRIDGE_MEM32(reg + 4);
        for (i = 0; i < g_shutdown_count && priority <= (int32_t)BRIDGE_MEM32(g_shutdown_reg[i] + 4); i++) {}
        memmove(&g_shutdown_reg[i + 1], &g_shutdown_reg[i], (g_shutdown_count - i) * sizeof(uint32_t));
        g_shutdown_reg[i] = reg;
        g_shutdown_count++;
    }
    ReleaseSRWLockExclusive(&g_shutdown_lock);
    g_eax = 0;
}

static void bridge_run_shutdown_notifications(void)
{
    const char *e = getenv("RECOMP_SHUTDOWN_NOTIFY");
    if (!e || *e != '1') return;
    for (;;) {
        uint32_t reg;
        AcquireSRWLockExclusive(&g_shutdown_lock);
        if (!g_shutdown_count) {
            ReleaseSRWLockExclusive(&g_shutdown_lock);
            return;
        }
        reg = g_shutdown_reg[0];
        memmove(&g_shutdown_reg[0], &g_shutdown_reg[1], (g_shutdown_count - 1) * sizeof(uint32_t));
        g_shutdown_count--;
        ReleaseSRWLockExclusive(&g_shutdown_lock);
        bridge_call_user_apc(BRIDGE_MEM32(reg), reg, 0, 0);
    }
}

/* ── Kernel timers (ordinals 113, 149, 150, 97) ───────────
 *
 * Timers are waitable: KeInitializeTimerEx backs every guest KTIMER with a
 * Win32 event in the shadow table (manual reset for notification timers, auto
 * reset for synchronization timers), so a KeWaitForSingleObject on a timer
 * sleeps on a real object. KeSetTimer/KeSetTimerEx arm the polling table below
 * whose thread fires DPCs and signals that event; KeCancelTimer clears both. */

/* ── KeInitializeTimerEx (ordinal 113) ────────────────────
 * VOID KeInitializeTimerEx(PKTIMER Timer, TIMER_TYPE Type)
 */
static void bridge_KeInitializeTimerEx(void)
{
    uint32_t timer_va = STACK_ARG(0);
    uint32_t type     = STACK_ARG(1);
    HANDLE   ev;

    if (!timer_va) {
        g_eax = 0;
        return;
    }

    memset(XBOX_TO_NATIVE(timer_va), 0, 40);

    /* Dispatcher header. 0x08 = notification timer, 0x09 = synchronization. */
    BRIDGE_MEM8(timer_va + 0)  = (uint8_t)(0x08 + (type & 1));
    BRIDGE_MEM8(timer_va + 2)  = 10;      /* Size, in dwords */
    BRIDGE_MEM8(timer_va + 3)  = 0;       /* Inserted */
    BRIDGE_MEM32(timer_va + 4) = 0;       /* SignalState */
    BRIDGE_MEM32(timer_va + 8) = 0;       /* WaitListHead */
    BRIDGE_MEM32(timer_va + 12) = 0;

    ev = CreateEventW(NULL, (type == 0) ? TRUE : FALSE, FALSE, NULL);
    if (!ev) {
        fprintf(stderr, "  [KERNEL] KeInitializeTimerEx: CreateEventW failed "
                        "(error %u)\n", GetLastError());
        g_eax = 0;
        return;
    }
    ke_shadow_insert(timer_va, ev);
    g_eax = 0;
}

/* -- KeSetTimer / KeSetTimerEx (ordinal 149/150) ----------
 * BOOLEAN KeSetTimer(PKTIMER Timer, LARGE_INTEGER DueTime, PKDPC Dpc)
 * BOOLEAN KeSetTimerEx(PKTIMER Timer, LARGE_INTEGER DueTime, LONG Period,
 *                      PKDPC Dpc)
 *
 * These used to return FALSE and do nothing, on the grounds that timers were
 * not needed for basic execution. They are needed for more than that: a driver
 * that polls its hardware does it from a timer DPC, and one that never fires
 * is a state machine that never advances. Half-Life 2's USB stack sets one up
 * during XInitDevices and enumerates from it.
 *
 * A due time is in 100 ns units, negative for relative and positive for an
 * absolute time since 1601. Only the relative form is honoured here; an
 * absolute due time is treated as immediate, which is wrong in principle and
 * has not come up in practice.
 *
 * ponytail: one thread, a fixed table, and a 10 ms tick, so a due time is late
 * by up to a tick and a periodic timer drifts. Nothing here is scheduling
 * audio off a timer. A title that needs better wants the host's timer queue,
 * not a smaller sleep.
 */
#define XBOX_TIMERS_PER_BLOCK 64
#define XBOX_TIMER_BLOCKS 4096
typedef struct {
    uint32_t  timer_va;     /* the guest KTIMER, 0 for a slot never used */
    uint32_t  dpc_va;
    long long due_ms;       /* host tick when it fires */
    long      period_ms;    /* 0 for one-shot */
    int       armed;        /* in the table and due to fire */
    /* [TIMER] log */
    uint32_t  caller;       /* guest code that set it last */
    uint32_t  sets, fires;  /* in the current 10 s window */
    uint32_t  late_max_ms;  /* fired this long after its due time, at worst */
    uint32_t  total_sets;
    long long first_set_ms, last_set_ms, last_fire_ms;
    int       stall_told;   /* the watchdog line was written for this lull */
} XboxTimer;
/* The console keeps a timer in the title's own KTIMER, so it has no limit. Ours
 * are in blocks that never move or go away: the timer thread holds a row across
 * its lock. Rows are reused once idle. */
static XboxTimer *g_timer_blocks[XBOX_TIMER_BLOCKS];
static volatile LONG g_timer_count;
#define timer_at(i) (&g_timer_blocks[(i) / XBOX_TIMERS_PER_BLOCK][(i) % XBOX_TIMERS_PER_BLOCK])

/* A new row, -1 out of memory. g_timer_lock held. */
static int timer_add(void)
{
    int i = g_timer_count, b = i / XBOX_TIMERS_PER_BLOCK;
    if (b >= XBOX_TIMER_BLOCKS)
        return -1;
    if (!g_timer_blocks[b]) {
        g_timer_blocks[b] = (XboxTimer *)calloc(XBOX_TIMERS_PER_BLOCK, sizeof(XboxTimer));
        if (!g_timer_blocks[b])
            return -1;
    }
    g_timer_count = i + 1;
    return i;
}
static CRITICAL_SECTION g_timer_lock;
static int g_timer_started;

/* Firing and setting are ordered by g_timer_lock, signal included.
 *
 * A timer is signalled (SetEvent + SignalState) inside the lock at the moment
 * it fires, and KeSetTimer resets it inside the same lock, so between the two
 * there is no window left: a firing either happened before the set (and the
 * set clears it, as the real KeSetTimer does) or after it (and stands).
 *
 * 2026-10-30: the reset used to come after the lock was let go. The XDK's
 * timeSetEvent thread re-arms a one-shot timer with due 0 when its callback ran
 * late; the timer thread fired it in that gap, the reset then wiped the signal
 * of a timer already gone from the table, and the waiter slept for good. In
 * CoS that stopped the title's 10 ms task clock now and then, and the level
 * music with it. Signalling before the DPC runs is also the real kernel's
 * order (a timer's waiters are satisfied at expiry, its DPC queued after).
 *
 * [TIMER] lines: each timer the first time it is set (who set it, due, period,
 * DPC), every 10 s the timers that were set or fired (counts, worst lateness),
 * and a watchdog: a timer that was being set again and again and has gone
 * quiet for 2 s after firing - whoever waits on it may be stuck. */
static void timer_log_window(long long now)
{
    static long long last;
    int i, any = 0;

    if (!last) last = now;
    if (now - last < 10000)
        return;
    for (i = 0; i < g_timer_count; i++) {
        XboxTimer *t = timer_at(i);
        if (!t->timer_va || (!t->sets && !t->fires))
            continue;
        if (!any++)
            fprintf(stderr, "[TIMER] %lld ms last %llds:\n", now, (now - last) / 1000);
        fprintf(stderr, "[TIMER]   %08X set by %08X: set %u, fired %u, latest %u ms late, period %ld ms, dpc %08X, %s\n",
                t->timer_va, t->caller, t->sets, t->fires, t->late_max_ms, t->period_ms, t->dpc_va,
                t->armed ? "armed" : "idle");
        t->sets = t->fires = t->late_max_ms = 0;
    }
    last = now;
}

static void timer_watchdog(long long now)
{
    int i;

    for (i = 0; i < g_timer_count; i++) {
        XboxTimer *t = timer_at(i);
        long long gap;
        if (!t->timer_va || t->armed || t->stall_told || t->total_sets < 20 || !t->last_fire_ms)
            continue;
        gap = (t->last_set_ms - t->first_set_ms) / (long long)(t->total_sets - 1);  /* its usual interval */
        if (gap > 500 || now - t->last_fire_ms < 2000)
            continue;
        t->stall_told = 1;
        fprintf(stderr, "[TIMER] %lld ms: timer %08X (set by %08X, every ~%lld ms, %u times) fired %lld ms ago and "
                "has not been set again - a thread waiting on it may be stuck\n", now, t->timer_va, t->caller, gap,
                t->total_sets, now - t->last_fire_ms);
    }
}

static DWORD WINAPI kernel_timer_thread(LPVOID unused)
{
    int slot = xbox_worker_stack_alloc();

    (void)unused;
    xbox_PinToGuestCore();   /* runs the vblank ISR, DPCs and timer routines: guest code */
    if (slot < 0) {
        fprintf(stderr, "  [KERNEL] timer thread has no worker stack; "
                        "timer DPCs will not run\n");
        fflush(stderr);
        return 0;
    }
    g_esp = XBOX_WORKER_STACK_TOP(slot);
    {
        /* Its own TIB, for the same reason bridge_thread_main gives one to
         * every worker: a DPC routine with an SEH prologue reads fs:[0],
         * and a thread whose g_fs_base is zero faults inside the runtime
         * before the routine runs. */
        uint32_t tib = xbox_AllocThreadTib();
        if (!tib) {
            fprintf(stderr, "  [KERNEL] timer thread has no TIB; "
                            "timer DPCs will not run\n");
            fflush(stderr);
            return 0;
        }
        g_fs_base = tib;
    }

    /* 1 ms scheduler granularity, so Sleep(1) below is 1 ms and not a 15.6 ms
     * system tick: the vblank and every guest timer run off this loop. */
#ifdef _WIN32
    timeBeginPeriod(1);
#endif

    for (;;) {
        long long now;
        int i;

        Sleep(1);
        kernel_vblank_tick();  /* the GPU's frame clock */
        kernel_drain_dpcs();   /* deferred work, before due timers */
        now = (long long)GetTickCount64();

        for (i = 0; i < g_timer_count; i++) {
            XboxTimer *t = timer_at(i);
            uint32_t dpc, va;
            HANDLE ev;

            EnterCriticalSection(&g_timer_lock);
            va = t->armed && now >= t->due_ms ? t->timer_va : 0;
            LeaveCriticalSection(&g_timer_lock);
            if (!va)
                continue;
            /* The event looked up afresh (outside the lock: the shadow table
             * has its own), then the row checked again under the lock: it may
             * have been set or cancelled meanwhile. */
            ev = ke_shadow_lookup(va);
            EnterCriticalSection(&g_timer_lock);
            if (t->timer_va != va || !t->armed || now < t->due_ms) {
                LeaveCriticalSection(&g_timer_lock);
                continue;
            }
            dpc = t->dpc_va;
            if ((uint32_t)(now - t->due_ms) > t->late_max_ms)
                t->late_max_ms = (uint32_t)(now - t->due_ms);
            if (t->period_ms > 0)
                t->due_ms = now + t->period_ms;
            else
                t->armed = 0;                  /* one-shot, done */
            t->fires++;
            t->last_fire_ms = now;
            /* Signalled inside the lock: a KeSetTimer on another thread resets
             * it either before this (and it stands) or after (as it should). */
            if (ev) {
                SetEvent(ev);
                BRIDGE_MEM32(va + 4) = 1;   /* SignalState */
            }
            LeaveCriticalSection(&g_timer_lock);

            /* Outside the lock: the routine can set or cancel timers. */
            if (dpc)
                kernel_run_dpc(dpc, 0, 0);
        }

        EnterCriticalSection(&g_timer_lock);
        timer_watchdog(now);
        timer_log_window(now);
        LeaveCriticalSection(&g_timer_lock);
    }
}

/* Shared by KeSetTimer and KeSetTimerEx; period is 0 for the former. */
static void kernel_set_timer(uint32_t timer_va, long long due_100ns,
                             long period_ms, uint32_t dpc_va)
{
    /* Negative: relative. Otherwise absolute, in system time, as the console
     * (a time already past fires at once). Rounded up: never early. */
    long long delay_ms;
    if (due_100ns < 0) {
        delay_ms = (-due_100ns + 9999) / 10000;
    } else {
        LARGE_INTEGER now_time;
        long long left;
        xbox_KeQuerySystemTime(&now_time);   /* a clock the title set with NtSetSystemTime counts */
        left = due_100ns - now_time.QuadPart;
        delay_ms = left > 0 ? (left + 9999) / 10000 : 0;
    }
    long long now;
    HANDLE ev = ke_shadow_lookup(timer_va);   /* outside the timer lock: it takes its own */
    int i, slot = -1, idle = -1;
    uint32_t was_set = 0;

    if (!g_timer_started) {
        InitializeCriticalSection(&g_timer_lock);
        g_timer_started = 1;
        CloseHandle(CreateThread(NULL, 0, kernel_timer_thread, NULL, 0, NULL));
    }

    EnterCriticalSection(&g_timer_lock);
    now = (long long)GetTickCount64();
    for (i = 0; i < g_timer_count; i++) {
        if (timer_at(i)->timer_va == timer_va) { slot = i; break; }
        if (!timer_at(i)->armed && idle < 0) idle = i;
    }
    if (slot < 0) {   /* a timer not seen before: a new row; an idle one only once there are many (timers on the
                       * stack come and go), so that timers set in turn don't keep taking each other's rows */
        slot = (idle >= 0 && g_timer_count >= XBOX_TIMERS_PER_BLOCK) ? idle : timer_add();
        if (slot < 0) slot = idle;
        if (slot >= 0) {
            memset(timer_at(slot), 0, sizeof(XboxTimer));
            timer_at(slot)->timer_va = timer_va;
            timer_at(slot)->first_set_ms = now;
            fprintf(stderr, "[TIMER] %lld ms: new timer %08X set by %08X: due in %lld ms, period %ld ms, dpc %08X%s\n",
                    now, timer_va, g_xbox_kernel_caller, delay_ms, period_ms, dpc_va,
                    ev ? "" : " (no event: KeInitializeTimerEx not seen)");
        } else {
            static int told;
            if (!told++)
                fprintf(stderr, "[TIMER] %lld ms: out of memory; timer %08X set by %08X will never fire\n",
                        now, timer_va, g_xbox_kernel_caller);
        }
    }
    if (slot >= 0) {
        XboxTimer *t = timer_at(slot);
        was_set = (uint32_t)t->armed;
        /* A set timer starts unsignaled, like the real KeSetTimer: inside the
         * lock, so no firing can fall between this and the arming below. */
        if (ev)
            ResetEvent(ev);
        BRIDGE_MEM32(timer_va + 4) = 0;   /* SignalState */
        t->dpc_va    = dpc_va;
        t->due_ms    = now + delay_ms;
        t->period_ms = period_ms;
        t->armed     = 1;
        t->caller    = g_xbox_kernel_caller;
        t->sets++;
        t->total_sets++;
        t->last_set_ms = now;
        t->stall_told = 0;
    }
    LeaveCriticalSection(&g_timer_lock);
    g_eax = was_set;
}

static void bridge_KeSetTimer(void)
{
/* LARGE_INTEGER is two stack slots. */
    long long due = (long long)((uint64_t)STACK_ARG(1)
                              | ((uint64_t)STACK_ARG(2) << 32));
    kernel_set_timer(STACK_ARG(0), due, 0, STACK_ARG(3));
}

static void bridge_KeSetTimerEx(void)
{
    long long due = (long long)((uint64_t)STACK_ARG(1)
                              | ((uint64_t)STACK_ARG(2) << 32));
    kernel_set_timer(STACK_ARG(0), due, (long)STACK_ARG(3), STACK_ARG(4));
}

/* Drop a timer so it stops firing. Returns whether it was armed. The slot
 * keeps the timer (and its [TIMER] counts) until another timer needs it. */
int xbox_kernel_cancel_timer(uint32_t timer_va)
{
    int i, was_set = 0;

    if (!g_timer_started)
        return 0;
    EnterCriticalSection(&g_timer_lock);
    for (i = 0; i < g_timer_count; i++)
        if (timer_at(i)->timer_va == timer_va && timer_at(i)->armed) {
            timer_at(i)->armed = 0;
            timer_at(i)->stall_told = 1;   /* cancelled on purpose: not a stall */
            was_set = 1;
        }
    LeaveCriticalSection(&g_timer_lock);
    return was_set;
}

/* ── ExQueryPoolBlockSize (ordinal 24) ────────────────────
 * ULONG ExQueryPoolBlockSize(PVOID PoolBlock)
 *
 * Returns the size of a pool memory block.
 * Since we use HeapAlloc, we can query the Windows heap.
 */
static void bridge_ExQueryPoolBlockSize(void)
{
    /* Pool blocks come from xbox_HeapAlloc, so the block table has the real
     * answer. It used to return a literal 0 on the theory that this is only
     * ever used for stats -- which is a guess about the caller, and a title
     * that sizes a copy from it copies nothing. */
    uint32_t s = xbox_HeapBlockSize(STACK_ARG(0));
    if (s > 0xFD8) g_eax = (s + 0xFFF) & ~0xFFFu;
    else g_eax = s ? ((s + 8) & ~31u) - 8 : 0;  /* the heap may have rounded to 16 */
}

/* ── RtlNtStatusToDosError (ordinal 301) ─────────────────
 * ULONG RtlNtStatusToDosError(NTSTATUS Status)
 *
 * Converts an NTSTATUS to a Win32 error code.
 */
static void bridge_RtlNtStatusToDosError(void)
{
    g_eax = xbox_RtlNtStatusToDosError((NTSTATUS)STACK_ARG(0));
}

/* ── File I/O bridge helpers ─────────────────────────────── */

/*
 * Xbox structures use 32-bit pointers. On Win64, the C structs
 * (XBOX_OBJECT_ATTRIBUTES, etc.) have 64-bit pointers, so we can't
 * cast Xbox memory to them directly. Instead, parse the 32-bit
 * Xbox layout manually:
 *
 * XBOX_OBJECT_ATTRIBUTES (12 bytes):
 *   offset 0: RootDirectory  (uint32_t)
 *   offset 4: ObjectName     (uint32_t, Xbox VA to ANSI_STRING)
 *   offset 8: Attributes     (uint32_t)
 *
 * XBOX_ANSI_STRING (8 bytes):
 *   offset 0: Length          (uint16_t)
 *   offset 2: MaximumLength   (uint16_t)
 *   offset 4: Buffer          (uint32_t, Xbox VA to char[])
 *
 * XBOX_IO_STATUS_BLOCK (8 bytes):
 *   offset 0: Status          (uint32_t)
 *   offset 4: Information     (uint32_t)
 */

/* Extract the ANSI path string from an Xbox OBJECT_ATTRIBUTES */
static const char* bridge_get_xbox_path(uint32_t obj_attrs_va)
{
    uint32_t ansi_str_va, buf_va;
    if (!obj_attrs_va) return NULL;
    ansi_str_va = BRIDGE_MEM32(obj_attrs_va + 4);
    if (!ansi_str_va) return NULL;
    buf_va = BRIDGE_MEM32(ansi_str_va + 4);
    if (!buf_va) return NULL;
    /* XDK directory searches pass a counted prefix of "directory\\*".
     * The byte after Length need not be NUL or part of the object name. */
    static RECOMP_TLS char path[65536];
    uint16_t length=BRIDGE_MEM16(ansi_str_va);
    if(length>BRIDGE_MEM16(ansi_str_va+2)) return NULL;
    /* Length <= MaximumLength says the string is self-consistent; it does not
     * say the buffer it names is inside the mapping. Without this, a string
     * near the top of guest memory reads up to 64 KB off the end. */
    if (length && !bridge_va_mapped(buf_va, length)) return NULL;
    memcpy(path,XBOX_TO_NATIVE(buf_va),length);
    path[length]='\0';
    return path;
}

/* Write NTSTATUS + Information into Xbox IO_STATUS_BLOCK */
static void bridge_write_iostatus(uint32_t ios_va, NTSTATUS status, uint32_t info)
{
    if (ios_va) {
        BRIDGE_MEM32(ios_va + 0) = (uint32_t)status;
        BRIDGE_MEM32(ios_va + 4) = info;
    }
}

/*
 * Handle table, as the Xbox kernel keeps it (read from xboxkrnl over xbdm,
 * 2026-11-04: ObInsertObject and the allocator, lookup and free it calls).
 *
 * A handle is a slot number times four: 4, 8, 12... Handle 0 is reserved and
 * the low two bits are ignored. Slots come in blocks of 64, added when the free
 * list runs dry, reached through a root table that grows by 8 pointers. Free
 * slots form a list, last freed first. There is no maximum: the only failure
 * is memory, reported as STATUS_INSUFFICIENT_RESOURCES.
 *
 * The console's slot holds the object; ours holds the native HANDLE, which is
 * 64 bits and so doesn't fit a guest slot. Guest memory only ever has the
 * handle value. Fake handles (0xBEEF0010, 0xDEAD0001...) are above any real
 * one, and the kernel raises IRQL where we take a lock.
 *
 * HP:CoS leaks an event per rumble call (sub_00018F20), 60 a second while a
 * spell charges. It held 933,888 of them on a console, stopping only when
 * memory ran low. A fixed table of 16,384 ran out after 92 minutes, every open
 * after that got handle 0, and the title reported a dirty disc (issue #15).
 */
#define OB_HANDLES_PER_BLOCK   64u
#define OB_BLOCKS_PER_ROOT_ADD 8u
#define OB_BLOCK_SPAN          0x100u  /* handle values per block */
#define OB_ROOT_ADD_SPAN       0x800u  /* handle values per root growth */
#define OB_NO_FREE_HANDLE      0xFFFFFFFFu

typedef struct { HANDLE object; uint32_t next_free; } ob_slot;

static ob_slot **s_ob_root;
static uint32_t  s_ob_first_free = OB_NO_FREE_HANDLE;
static uint32_t  s_ob_next_needing_pool;  /* first handle value with no block */
static uint32_t  s_ob_count;
static CRITICAL_SECTION s_ob_lock;
static volatile LONG s_ob_lock_state;     /* 0 none, 1 being set up, 2 ready */

static void ob_lock(void)
{
    if (s_ob_lock_state != 2) {
        if (InterlockedCompareExchange(&s_ob_lock_state, 1, 0) == 0) {
            InitializeCriticalSection(&s_ob_lock);
            s_ob_lock_state = 2;
        } else {
            while (s_ob_lock_state != 2) Sleep(0);
        }
    }
    EnterCriticalSection(&s_ob_lock);
}
#define ob_unlock() LeaveCriticalSection(&s_ob_lock)

/* Add a block of free slots. Lock held. */
static int ob_extend(void)
{
    uint32_t next = s_ob_next_needing_pool, i;
    ob_slot *block = (ob_slot *)calloc(OB_HANDLES_PER_BLOCK, sizeof(ob_slot));
    if (!block) return 0;
    if ((next & (OB_ROOT_ADD_SPAN - 1u)) == 0) {
        uint32_t used = next / OB_BLOCK_SPAN;
        ob_slot **root = (ob_slot **)malloc((used + OB_BLOCKS_PER_ROOT_ADD) * sizeof(ob_slot *));
        if (!root) { free(block); return 0; }
        if (used) memcpy(root, s_ob_root, used * sizeof(ob_slot *));
        free(s_ob_root);
        s_ob_root = root;
    }
    s_ob_root[next / OB_BLOCK_SPAN] = block;
    for (i = 0; i + 1u < OB_HANDLES_PER_BLOCK; i++)
        block[i].next_free = next + (i + 1u) * 4u;
    block[OB_HANDLES_PER_BLOCK - 1u].next_free = OB_NO_FREE_HANDLE;
    s_ob_first_free = next ? next : block[0].next_free;  /* handle 0 stays unused */
    s_ob_next_needing_pool = next + OB_BLOCK_SPAN;
    return 1;
}

static ob_slot *ob_slot_of(uint32_t value)
{
    return &s_ob_root[value / OB_BLOCK_SPAN][(value % OB_BLOCK_SPAN) / 4u];
}

/* Give a native HANDLE a slot and return its handle value, 0 if out of memory. */
static uint32_t bridge_handle_token(HANDLE h)
{
    uint32_t value = 0, count = 0;
    if (!h || h == INVALID_HANDLE_VALUE) return 0;
    ob_lock();
    if (s_ob_first_free != OB_NO_FREE_HANDLE || ob_extend()) {
        ob_slot *slot;
        value = s_ob_first_free;
        slot = ob_slot_of(value);
        s_ob_first_free = slot->next_free;
        slot->object = h;
        count = ++s_ob_count;
    }
    ob_unlock();
    if (!value)
        fprintf(stderr, "  [BRIDGE] out of memory for a new handle\n");
    else if (count >= 16384u && (count & (count - 1u)) == 0)
        fprintf(stderr, "[HANDLES] %u open\n", count);
    return value;
}

/* The native HANDLE behind a handle value; NULL if it isn't an open handle. */
static HANDLE bridge_handle_lookup(uint32_t value)
{
    HANDLE h = NULL;
    value &= ~3u;
    ob_lock();
    if (value < s_ob_next_needing_pool) h = ob_slot_of(value)->object;
    ob_unlock();
    return h;
}

static int bridge_handle_is_table(uint32_t value)
{
    int in_range;
    ob_lock();
    in_range = (value & ~3u) < s_ob_next_needing_pool;
    ob_unlock();
    return in_range;
}

/* Store a native HANDLE into a 32-bit Xbox memory slot. Out of memory closes
 * the object and returns STATUS_INSUFFICIENT_RESOURCES, as ObInsertObject does. */
static NTSTATUS bridge_write_handle(uint32_t handle_va, HANDLE h)
{
    uint32_t value;
    if (!handle_va) return 0;
    value = bridge_handle_token(h);
    BRIDGE_MEM32(handle_va) = value;
    if (!value && h && h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        return (NTSTATUS)0xC000009Au;
    }
    return 0;
}

/* Resolve a 32-bit Xbox handle slot back to a native HANDLE. */
static HANDLE bridge_read_handle(uint32_t va)
{
    uint32_t token = BRIDGE_MEM32(va);
    if (bridge_handle_is_table(token))
        return bridge_handle_lookup(token);
    /* Above the table: synthetic/dummy handle -- pass through unchanged. */
    return (HANDLE)(uintptr_t)token;
}

/* Resolve a token to a HANDLE and release its table slot (for NtClose). */
/* Resolve a handle token passed BY VALUE, without consuming it.
 *
 * Three accessors, easily confused, and confusing two of them broke all file
 * I/O: bridge_read_handle(va) reads a token *from memory* and suits a PHANDLE
 * out-parameter; bridge_take_handle(token) resolves and CLEARS the table slot,
 * which is NtClose semantics; this one resolves and leaves the slot alone,
 * which is what every by-value HANDLE argument needs.
 *
 * NtSetInformationFile and friends take the handle by value, but were calling
 * bridge_read_handle on it -- dereferencing the token as if it were an address.
 * Halo created its save file successfully and then failed the very next call,
 * which surfaced as "couldn't open or create saved game file". */
static HANDLE bridge_resolve_handle(uint32_t token)
{
    if (bridge_handle_is_table(token))
        return bridge_handle_lookup(token);
    /* Pseudo-handles (NtCurrentProcess() = -1, NtCurrentThread() = -2) are
     * negative. Zero-extending them on a 64-bit host yields 0x00000000FFFFFFFE,
     * which Win32 rejects: X-Men Legends' CRT duplicates NtCurrentThread()
     * and spun forever on the STATUS_UNSUCCESSFUL that came back. */
    if (token >= 0xFFFFFFF0u)
        return (HANDLE)(intptr_t)(int32_t)token;
    /* Untagged: synthetic/dummy handle -- pass through unchanged. */
    return (HANDLE)(uintptr_t)token;
}

/*
 * Thread objects for ObReferenceObjectByHandle.
 *
 * XAPI SetThreadPriority references the thread handle and passes the object to
 * KeSetBasePriorityThread. The bridge used to return object 0, so no priority
 * ever reached Windows: CoS's TIME_CRITICAL streaming thread ran at normal
 * priority behind the game loop on the shared guest core and refilled its
 * 50 ms music loops late (crackle in gameplay only). Each host thread gets a
 * small zeroed guest block (safe to dereference) mapped to its host HANDLE.
 */
#define BRIDGE_THREAD_OBJ_MAX 4096
static struct { DWORD tid; uint32_t obj; HANDLE h; } s_thread_objs[BRIDGE_THREAD_OBJ_MAX];
static int s_thread_obj_count;

static uint32_t bridge_thread_object(uint32_t token)
{
    HANDLE h = NULL;
    DWORD tid;
    int i;
    if (token == 0xFFFFFFFEu) {           /* NtCurrentThread() */
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                             GetCurrentProcess(), &h, 0, FALSE,
                             DUPLICATE_SAME_ACCESS))
            return 0;
    } else {
        h = bridge_handle_lookup(token);
        if (!h) return 0;
    }
    tid = h ? GetThreadId(h) : 0;
    if (!tid) {
        if (token == 0xFFFFFFFEu && h) CloseHandle(h);
        return 0;                          /* not a thread handle */
    }
    for (i = 0; i < s_thread_obj_count; i++)
        if (s_thread_objs[i].tid == tid) {
            if (token == 0xFFFFFFFEu) CloseHandle(h);
            return s_thread_objs[i].obj;
        }
    if (s_thread_obj_count >= BRIDGE_THREAD_OBJ_MAX) {
        if (token == 0xFFFFFFFEu) CloseHandle(h);
        return 0;
    }
    {
        uint32_t obj = xbox_HeapAlloc(0x200, 16), o;
        HANDLE own = NULL;
        if (!obj) return 0;
        for (o = 0; o < 0x200; o += 4) BRIDGE_MEM32(obj + o) = 0;
        if (token == 0xFFFFFFFEu) own = h;  /* already our own copy */
        else DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &own,
                             0, FALSE, DUPLICATE_SAME_ACCESS);
        {   /* It may have ended before anyone asked. */
            DWORD code;
            if (own && WaitForSingleObject(own, 0) == WAIT_OBJECT_0 && GetExitCodeThread(own, &code)) {
                BRIDGE_MEM32(obj + 4) = 1;
                BRIDGE_MEM32(obj + 0x120) = code;
            }
        }
        s_thread_objs[s_thread_obj_count].tid = tid;
        s_thread_objs[s_thread_obj_count].obj = obj;
        s_thread_objs[s_thread_obj_count].h = own;
        s_thread_obj_count++;
        return obj;
    }
}

/* The thread's end, where XAPI's GetExitCodeThread reads it: SignalState and
 * ExitStatus (+0x120, as PsTerminateSystemThread stores it). */
/* Routine(Thread, ThreadId, Create), for each that was registered. */
static void bridge_thread_notify(HANDLE th, uint32_t create)
{
    uint32_t i, obj = 0;
    for (i = 0; i < 8; i++)
        if (s_thread_notify[i]) {
            uint32_t tok;
            if (!obj) {
                tok = th ? bridge_handle_token(th) : 0xFFFFFFFEu;
                obj = bridge_thread_object(tok);
                if (th && tok) bridge_take_handle(tok);
            }
            bridge_call_user_apc(s_thread_notify[i], obj, (uint32_t)(th ? GetThreadId(th) : GetCurrentThreadId()), create);
        }
}

static void bridge_thread_mark_exit(uint32_t status)
{
    DWORD tid = GetCurrentThreadId();
    int i;
    for (i = 0; i < s_thread_obj_count; i++)
        if (s_thread_objs[i].tid == tid) {
            BRIDGE_MEM32(s_thread_objs[i].obj + 0x120) = status;
            BRIDGE_MEM32(s_thread_objs[i].obj + 4) = 1;
            return;
        }
}

static HANDLE bridge_thread_host(uint32_t obj, DWORD *tid)
{
    int i;
    for (i = 0; i < s_thread_obj_count; i++)
        if (s_thread_objs[i].obj == obj) {
            if (tid) *tid = s_thread_objs[i].tid;
            return s_thread_objs[i].h;
        }
    return NULL;
}

/* Xbox base-priority increment -> Win32 thread priority (XAPI maps
 * THREAD_PRIORITY_TIME_CRITICAL to 16 and IDLE to -16 before the call). */
/* A raised priority short of time-critical (+1 to +14) is run at normal while guest threads share one core
 * (RECOMP_GUEST_CORES unset or a mask). On the console such a thread is a loader that sleeps on the DVD most of the
 * time; here its reads finish at once and it stays on the core, and Windows -- which, unlike Wine, keeps strict
 * priorities -- left the title's main thread waiting behind it. HP:CoS's file cache worker (0x1146C0, +1) copying
 * into potterfilecache held the shared core for 100 to 400 ms at a time on a Windows PC (2026-10-25, [HITCH]: the time
 * in RtlLeave/EnterCriticalSection of lock 0x80011130 it shares with the main thread). At normal the two take turns
 * on the core, as under Wine. Not below normal: the main thread polls the worker through that lock while the
 * inventory loads, never blocking, and a worker below it ran only on Windows' starvation boost (1.3 to 2.2 s
 * loads). Time-critical threads (the audio streamers) keep their priority.
 * RECOMP_GUEST_BOOST=keep: as the console */
int xbox_GuestCoreShared(void);

static int bridge_win32_priority(LONG inc)
{
    if (inc >= 1 && inc < 15 && xbox_GuestCoreShared()) return THREAD_PRIORITY_NORMAL;
    if (inc >= 15) return THREAD_PRIORITY_TIME_CRITICAL;
    if (inc <= -15) return THREAD_PRIORITY_IDLE;
    if (inc >= 2) return THREAD_PRIORITY_HIGHEST;
    if (inc <= -2) return THREAD_PRIORITY_LOWEST;
    return (int)inc;   /* -1, 0, 1 match Win32 */
}

static void bridge_apply_priority(const char *fn, uint32_t obj, LONG inc)
{
    DWORD tid = 0;
    HANDLE h = bridge_thread_host(obj, &tid);
    int pri = bridge_win32_priority(inc);
    BOOL ok = h ? SetThreadPriority(h, pri) : FALSE;
    fprintf(stderr, "[THREAD] %s obj=%08X tid=%lu increment=%ld -> win32 %d %s\n",
            fn, obj, (unsigned long)tid, (long)inc, pri,
            ok ? "ok" : (h ? "FAILED" : "no host thread"));
}

static HANDLE bridge_take_handle(uint32_t token)
{
    HANDLE h = NULL;
    uint32_t value = token & ~3u;
    ob_lock();
    if (value < s_ob_next_needing_pool) {
        ob_slot *slot = ob_slot_of(value);
        h = slot->object;
        if (h) {                /* last freed is the next one handed out */
            slot->object = NULL;
            slot->next_free = s_ob_first_free;
            s_ob_first_free = value;
            s_ob_count--;
        }
    }
    ob_unlock();
    return h;                   /* NULL: not an open handle */
}

/* Build a native OBJECT_ATTRIBUTES wrapping the translated Xbox path. */
static void bridge_build_oa(uint32_t obj_attrs_va,
                            XBOX_OBJECT_ATTRIBUTES* oa, XBOX_ANSI_STRING* name)
{
    const char* path = bridge_get_xbox_path(obj_attrs_va);
    name->Buffer        = (PCHAR)path;
    name->Length        = path ? (USHORT)strlen(path) : 0;
    name->MaximumLength = (USHORT)(name->Length + 1);
    uint32_t root = obj_attrs_va ? BRIDGE_MEM32(obj_attrs_va) : 0;
    /* -3 is the XDK DOS-device namespace, not a file handle. */
    oa->RootDirectory = root && root != 0xFFFFFFFDu ? bridge_resolve_handle(root) : NULL;
    oa->ObjectName    = name;
    oa->Attributes    = 0;
}

/*
 * The DVD drive as a device, not as a directory.
 *
 * A title that checks its media opens "\\Device\\CdRom0" itself -- the bare
 * device, with nothing after it -- and then issues IOCTLs on the handle. The
 * path table in kernel_path.c only carries the "\\Device\\CdRom0\\" form with
 * the separator, which is the prefix for reading a *file* off the disc, so the
 * bare open matched no rule, was reported as "Unrecognized Xbox path", and came
 * back STATUS_OBJECT_PATH_NOT_FOUND. DDS9 reads that as "no disc" and exits
 * through HalReturnToFirmware before it draws a frame.
 *
 * There is nothing on the host to open here: the game directory is a
 * directory, and a directory handle would not answer the IOCTLs that follow.
 * So the open returns a synthetic handle, in the same style as the ones
 * NtCreateDirectoryObject and the partition devices already hand out. It is
 * deliberately untagged, which bridge_resolve_handle passes through unchanged,
 * and distinct so bridge_NtDeviceIoControlFile can recognise it by value.
 *
 * Accepts the "\??\" prefix, since titles reach the device both ways.
 */
#define BRIDGE_CDROM_HANDLE 0xDECD0001u

static int bridge_is_cdrom_device(const char *path)
{
    if (!path) return 0;
    if (_strnicmp(path, "\\??\\", 4) == 0) path += 4;
    return _stricmp(path, "\\Device\\CdRom0") == 0;
}

/* Open a file by delegating to the ported xbox_NtCreateFile kernel HLE. */
static NTSTATUS bridge_create_file_impl(
    uint32_t handle_va, ACCESS_MASK access, uint32_t obj_attrs_va,
    uint32_t iostatus_va, ULONG file_attrs, ULONG share,
    ULONG disposition, ULONG options)
{
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING       name;
    XBOX_IO_STATUS_BLOCK   ios;
    HANDLE   h  = NULL;
    NTSTATUS st;

    bridge_build_oa(obj_attrs_va, &oa, &name);
    if (!name.Buffer) {
        bridge_write_iostatus(iostatus_va, STATUS_OBJECT_PATH_NOT_FOUND, 0);
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }

    if (bridge_is_cdrom_device(name.Buffer)) {
        fprintf(stderr, "  [FILE] %s -> synthetic DVD device handle\n",
                name.Buffer);
        if (handle_va)
            BRIDGE_MEM32(handle_va) = BRIDGE_CDROM_HANDLE;
        bridge_write_iostatus(iostatus_va, 0, 1 /* FILE_OPENED */);
        return 0;
    }

    memset(&ios, 0, sizeof(ios));

    st = xbox_NtCreateFile(&h, access, &oa, &ios, NULL,
                           file_attrs, share, disposition, options);

    if (NT_SUCCESS(st))
        st = bridge_write_handle(handle_va, h);
    if (NT_SUCCESS(st)) {
        bridge_write_iostatus(iostatus_va, ios.Status, (uint32_t)ios.Information);
    } else {
        bridge_write_iostatus(iostatus_va, st, 0);
    }
    return st;
}

/* ── RtlInitAnsiString (ordinal 289) ──────────────────────
 * VOID RtlInitAnsiString(PANSI_STRING Destination, PCSZ Source)
 *
 * Fills an ANSI_STRING { USHORT Length; USHORT MaximumLength; PCHAR Buffer; }.
 * Unbridged this returned 0 and wrote nothing, so every path a title built
 * this way arrived at NtCreateFile as a null Buffer and failed with
 * STATUS_OBJECT_PATH_NOT_FOUND -- which looks like a missing file rather than
 * a missing bridge. Halo builds its map paths exactly this way.
 */
/* -- RtlEqualString (ordinal 279, 3 args) ----------------
 * BOOLEAN RtlEqualString(PSTRING String1, PSTRING String2, BOOLEAN CaseInSens)
 *
 * The fields are read out by hand rather than casting the guest struct. A
 * guest ANSI_STRING is {USHORT Length, USHORT MaximumLength, 32-bit Buffer},
 * eight bytes; the native one has a 64-bit PCHAR, so a cast would read
 * MaximumLength and Buffer from the wrong offsets and then dereference a guest
 * VA as a host address. RtlInitAnsiString stores a guest VA in that field --
 * see the bridge below -- so it has to be translated, not passed through.
 *
 * Stubbed, this returned 0: "never equal". Wreckless initialises a string and
 * compares it in a critical-section-protected lookup, so every comparison
 * missing turned that lookup into unbounded recursion and the process died of
 * a host stack overflow 200 kernel calls in.
 */
static void bridge_RtlEqualString(void)
{
    uint32_t s1_va  = STACK_ARG(0);
    uint32_t s2_va  = STACK_ARG(1);
    uint32_t nocase = STACK_ARG(2);
    XBOX_ANSI_STRING a, b;

    if (!s1_va || !s2_va) {
        g_eax = 0;
        return;
    }
    a.Length        = BRIDGE_MEM16(s1_va + 0);
    a.MaximumLength = BRIDGE_MEM16(s1_va + 2);
    a.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(s1_va + 4));
    b.Length        = BRIDGE_MEM16(s2_va + 0);
    b.MaximumLength = BRIDGE_MEM16(s2_va + 2);
    b.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(s2_va + 4));

    if (!a.Buffer || !b.Buffer) {
        g_eax = 0;
        return;
    }
    g_eax = xbox_RtlEqualString(&a, &b, (BOOLEAN)nocase) ? 1 : 0;
}

static void bridge_RtlInitAnsiString(void)
{
    uint32_t dest_va = STACK_ARG(0);
    uint32_t src_va  = STACK_ARG(1);

    if (!dest_va) {
        g_eax = 0;
        return;
    }
    if (src_va) {
        const char *src = (const char *)XBOX_TO_NATIVE(src_va);
        size_t len = strlen(src);
        if (len > 0xFFFE) {
            len = 0xFFFE;
        }
        BRIDGE_MEM16(dest_va + 0) = (uint16_t)len;
        BRIDGE_MEM16(dest_va + 2) = (uint16_t)(len + 1);
        BRIDGE_MEM32(dest_va + 4) = src_va;
    } else {
        BRIDGE_MEM16(dest_va + 0) = 0;
        BRIDGE_MEM16(dest_va + 2) = 0;
        BRIDGE_MEM32(dest_va + 4) = 0;
    }
    g_eax = 0;
}

/* ── NtCreateFile (ordinal 190, 9 args = 36 bytes) ─────── */
static void bridge_NtCreateFile(void)
{
    uint32_t handle_va   = STACK_ARG(0);  /* PHANDLE */
    uint32_t access      = STACK_ARG(1);  /* ACCESS_MASK */
    uint32_t obj_attrs   = STACK_ARG(2);  /* POBJECT_ATTRIBUTES */
    uint32_t iostatus    = STACK_ARG(3);  /* PIO_STATUS_BLOCK */
    /* arg4: AllocationSize - ignored */
    uint32_t file_attrs  = STACK_ARG(5);  /* FileAttributes */
    uint32_t share       = STACK_ARG(6);  /* ShareAccess */
    uint32_t disposition = STACK_ARG(7);  /* CreateDisposition */
    uint32_t options     = STACK_ARG(8);  /* CreateOptions */

    /* The out-parameter addresses matter as much as the result: this bridge
     * hands them to a real Win32 call, so a bogus one has Windows itself write
     * into Xbox memory. That is how a wild write ends up with a stack inside
     * ntdll and no recompiled frame to blame. */
    g_eax = (uint32_t)bridge_create_file_impl(
        handle_va, access, obj_attrs, iostatus,
        file_attrs, share, disposition, options);

    /* FILE_SYNCHRONOUS_IO_ALERT | FILE_SYNCHRONOUS_IO_NONALERT. Neither set
     * means the caller wants asynchronous completion on this handle. */
    if (g_eax == 0 && handle_va && (options & 0x30u) == 0u)
        bridge_note_async_handle(BRIDGE_MEM32(handle_va));
    if (g_eax == 0 && handle_va)
        bridge_note_file_name(BRIDGE_MEM32(handle_va));

    /* An FMV the host can decode itself.
     *
     * The title's own decoder is emulated like everything else, but it only
     * produces pixels once there is something to execute its GPU work -- so on
     * a bring-up where that does not exist yet, the video the game just asked
     * for can still be shown. The trigger is the title opening the file, so
     * this plays when the game decides to play it, not on a timer, and it
     * plays the file the game chose.
     *
     * Off unless RECOMP_FMV_HOST is set: it is a substitute for the title's
     * own output, and that should be a decision rather than a default. */
    if (g_eax == 0 && getenv("RECOMP_FMV_HOST")) {
        /* Declared here rather than included: the player lives in xbox_video,
         * which links xbox_d3d8, and having the kernel include its header
         * would make the dependency circular for no gain. Both land in the
         * same executable. */
        extern int xbox_VideoPlayFile(const char *host_path);
        extern int xbox_VideoIsPlaying(void);

        char host[MAX_PATH * 2];
        size_t n = 0;

        const wchar_t *w = xbox_LastHostPath();

        while (n < sizeof(host) - 1 && w[n]) {
            host[n] = (char)w[n];
            n++;
        }
        host[n] = 0;
        if (n > 4 && _stricmp(host + n - 4, ".wmv") == 0
                && !xbox_VideoIsPlaying())
            xbox_VideoPlayFile(host);
    }

    /* Paired with the [PATH] line the translation just printed: that says what
     * was asked for, this says whether it opened. A failed open is not itself
     * a bug -- a title probing the cache partition before the disc expects one
     * -- so the status is what separates a probe from a real miss. */
    {
        extern uint32_t xbox_LastFileError(void);
        uint32_t _e = g_eax ? xbox_LastFileError() : 0u;
        if (g_eax)
            fprintf(stderr, "  [FILE] -> 0x%08X FAILED (win32 err=%u%s)\n",
                    g_eax, _e,
                    _e == 32u ? " ERROR_SHARING_VIOLATION"
                  : _e ==  2u ? " ERROR_FILE_NOT_FOUND"
                  : _e ==  3u ? " ERROR_PATH_NOT_FOUND" : "");
        else
            fprintf(stderr, "  [FILE] -> 0x%08X\n", g_eax);
    }
    fflush(stderr);
}

/* ── NtOpenFile (ordinal 202, 6 args = 24 bytes) ──────── */
static void bridge_NtOpenFile(void)
{
    uint32_t handle_va = STACK_ARG(0);  /* PHANDLE */
    uint32_t access    = STACK_ARG(1);  /* ACCESS_MASK */
    uint32_t obj_attrs = STACK_ARG(2);  /* POBJECT_ATTRIBUTES */
    uint32_t iostatus  = STACK_ARG(3);  /* PIO_STATUS_BLOCK */
    uint32_t share     = STACK_ARG(4);  /* ShareAccess */
    uint32_t options   = STACK_ARG(5);  /* OpenOptions */

    /* NtOpenFile = NtCreateFile with FILE_OPEN disposition */
    g_eax = (uint32_t)bridge_create_file_impl(
        handle_va, access, obj_attrs, iostatus,
        0, share, 1 /* FILE_OPEN */, options);
    if (g_eax == 0 && handle_va)
        bridge_note_file_name(BRIDGE_MEM32(handle_va));
}

/*
 * Completion for a file request that carried an Event or an APC routine.
 *
 * Both bridges below do the I/O synchronously, and used to drop args 1-3
 * (Event, ApcRoutine, ApcContext) on the floor. A title that issues an async
 * request and waits alertably for the completion then waits forever: Halo's
 * cache-partition setup does exactly that, gives up after its 5-second SleepEx,
 * and asserts "setup for new cache file failed (#0)".
 *
 * ponytail: the APC runs inline here rather than at the next alertable wait.
 * The data really is ready by then, so the observable result matches; a title
 * that depends on the APC *not* having run yet would notice. A per-thread
 * deferred queue drained at alertable waits was tried for Halo's map streamer
 * and made no difference (it still issues one 14 KB batch and stops), so it was
 * dropped rather than risk changing this shared path for the other titles.
 */
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);

static void deliver_one_apc(uint32_t apc_routine, uint32_t apc_context,
                            uint32_t iostatus)
{
    /* The APC can be game code or a kernel export. Halo's XAPI passes the
     * latter -- 0xFE0000FC, one of our own synthetic thunk VAs -- so the recomp
     * dispatch correctly fails to find it and the kernel fallback is the one
     * that matters. Checking only recomp_lookup left it undelivered. */
    recomp_func_t fn = recomp_lookup(apc_routine);
    if (!fn) fn = recomp_lookup_manual(apc_routine);
    if (!fn) fn = recomp_lookup_kernel(apc_routine);
    if (fn) {
        /* VOID ApcRoutine(PVOID ApcContext, PIO_STATUS_BLOCK, ULONG) */
        g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = iostatus;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = apc_context;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;   /* dummy return address */
        fn();
        g_esp += 12;
    } else {
        uint32_t ord = 0;
        if (apc_routine >= KERNEL_VA_BASE && apc_routine < KERNEL_VA_END) {
            ord = g_slot_ordinals[(apc_routine - KERNEL_VA_BASE) / 4];
        }
        fprintf(stderr, "  [KERNEL] file I/O APC 0x%08X unresolved"
                " (kernel ordinal %u)\n", apc_routine, ord);
        fflush(stderr);
    }
}

/* Per-thread pending-APC ring. An APC is delivered on the thread that issued
 * the request, which is also the thread that waits, so thread-local is right. */
static void bridge_complete_file_io(uint32_t event_token, uint32_t apc_routine,
                                    uint32_t apc_context, uint32_t iostatus)
{
    if (event_token) {
        HANDLE ev = bridge_resolve_handle(event_token);
        if (ev) SetEvent(ev);
    }
    if (apc_routine) {
        deliver_one_apc(apc_routine, apc_context, iostatus);
    }
}

/* -- XeLoadSection / XeUnloadSection (ordinals 327/328, 1 arg = 4 bytes) --
 *
 * NTSTATUS XeLoadSection(PXBE_SECTION_HEADER Section);
 *
 * On hardware a section marked non-preload is read from disc on its first load, and the last
 * unload frees it (0x80035FA5-0x8003628C). So a title sees fresh bytes each time, and two
 * sections at one address can take turns. Sections stay resident here, nothing frees them, but
 * the first load copies the XBE's bytes back so the title sees what it would.
 *
 * Done against guest memory rather than the PXBE_SECTION_HEADER struct: the
 * on-disc header is nine 32-bit fields and a digest, and the native struct
 * declares some of them as pointers, so on x64 its layout is not the 56 bytes
 * actually there.
 *
 *   +0x04 address  +0x08 size  +0x0C file offset  +0x10 file size
 *   +0x14  section name address      +0x18  section reference count
 */
#define XBE_SECTION_REFCOUNT_OFFSET 0x18

static void bridge_XeSection(int load)
{
    uint32_t section = STACK_ARG(0);
    uint32_t count;

    if (!section) {
        g_eax = 0xC000000Du;              /* STATUS_INVALID_PARAMETER */
        return;
    }
    count = BRIDGE_MEM32(section + XBE_SECTION_REFCOUNT_OFFSET);
    if (!load && !count) {            /* not loaded: the kernel refuses */
        g_eax = 0xC000000Du;
        return;
    }
    if (load) {
        extern void xbox_section_reload(uint32_t va, uint32_t size, uint32_t raw_off, uint32_t raw_size);
        if (!count)
            xbox_section_reload(BRIDGE_MEM32(section + 4), BRIDGE_MEM32(section + 8),
                                BRIDGE_MEM32(section + 0xC), BRIDGE_MEM32(section + 0x10));
        count++;
    } else {
        count--;
    }
    BRIDGE_MEM32(section + XBE_SECTION_REFCOUNT_OFFSET) = count;

    if (KERNEL_LOG_ON())
        fprintf(stderr, "  [XBE] Xe%sSection(0x%08X) refcount=%u\n",
                load ? "Load" : "Unload", section, count);
    g_eax = 0;
}

static void bridge_XeLoadSection(void)   { bridge_XeSection(1); }
static void bridge_XeUnloadSection(void) { bridge_XeSection(0); }

/* -- RtlUnwind (ordinal 312, 4 args = 16 bytes) -------------------------
 *
 * VOID RtlUnwind(PVOID TargetFrame, PVOID TargetIp,
 *                PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue);
 *
 * Discards the SEH registration frames between the current one and
 * TargetFrame, letting each handler run its __finally blocks on the way past,
 * and leaves fs:[0] pointing at TargetFrame. fs:[0] is guest address 0 here,
 * because the runtime models the TIB at the bottom of guest memory.
 *
 * Left unbridged this returned 0 without touching anything, which is not a
 * harmless stub: MSVC's _global_unwind2 calls it and then carries on as if the
 * frames were gone, so the chain kept pointing into stack that had already
 * been reused and the next dispatch walked records built out of live locals.
 *
 * The walk is bounded and checked rather than trusting the chain, since it
 * lives in guest stack memory that a title can corrupt: records must climb
 * toward the stack top, stay inside the stack, and stay 4-byte aligned. A
 * chain that breaks any of those is truncated instead of followed.
 */
#define XBOX_SEH_END_OF_CHAIN 0xFFFFFFFFu
#define XBOX_EXCEPTION_UNWINDING  0x02u
#define XBOX_EXCEPTION_EXIT_UNWIND 0x04u
#define XBOX_SEH_MAX_FRAMES 0x100000   /* the climbing check below ends a cycle */

static void bridge_RtlUnwind(void)
{
    uint32_t target_frame = STACK_ARG(0);
    uint32_t exc_record   = STACK_ARG(2);
    uint32_t reg          = BRIDGE_MEM32(XBOX_FS_BASE);
    uint32_t prev_reg     = 0;
    uint32_t scratch      = 0;
    int      guard;

    /* An unwind with no record of its own still has to tell the handlers it
     * is an unwind, so synthesise one below the stack pointer. */
    if (!exc_record) {
        g_esp -= 0x50;
        scratch = g_esp;
        memset((uint8_t *)XBOX_TO_NATIVE(scratch), 0, 0x50);
        BRIDGE_MEM32(scratch) = 0xC0000027u;   /* STATUS_UNWIND */
        exc_record = scratch;
    }
    BRIDGE_MEM32(exc_record + 4) |= XBOX_EXCEPTION_UNWINDING
        | (target_frame ? 0u : XBOX_EXCEPTION_EXIT_UNWIND);

    for (guard = 0; guard < XBOX_SEH_MAX_FRAMES; guard++) {
        uint32_t next, handler;

        if (reg == XBOX_SEH_END_OF_CHAIN || reg == 0 || reg == target_frame)
            break;
        /* Worker stacks come from the heap, so any mapped address can be a frame:
         * only the main thread's stack is in XBOX_STACK_BASE..TOP. */
        if ((reg & 3u) || reg < 0x10000u ||
            reg >= (g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram))
            break;                       /* not a stack frame: chain is broken */
        if (prev_reg && reg <= prev_reg)
            break;                       /* not climbing: cycle or corruption */

        next    = BRIDGE_MEM32(reg);
        handler = BRIDGE_MEM32(reg + 4);

        /* Pop before dispatching. The handler may raise, and it must not see
         * its own frame still on the chain. */
        BRIDGE_MEM32(XBOX_FS_BASE) = next;

        if (handler) {
            recomp_func_t fn = recomp_lookup(handler);
            if (!fn) fn = recomp_lookup_manual(handler);
            if (!fn) fn = recomp_lookup_kernel(handler);
            if (fn) {
                /* EXCEPTION_DISPOSITION handler(record, frame, context,
                 * dispatcher) -- cdecl, so the caller pops. */
                g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = reg;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = exc_record;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;  /* return address */
                fn();
                /* 16, not 20: the handler's own `ret` has already taken the
                 * return address off, leaving just the four arguments for the
                 * caller to drop. Cleaning 20 leaves esp four bytes high, and
                 * every argument the unwound-into frame reads after that comes
                 * from one slot over. */
                g_esp += 16;
            }
        }

        prev_reg = reg;
        reg      = next;
    }

    /* Land on the target even if the walk stopped early: leaving fs:[0] on a
     * discarded frame is worse than losing a __finally. */
    if (target_frame && target_frame != XBOX_SEH_END_OF_CHAIN)
        BRIDGE_MEM32(XBOX_FS_BASE) = target_frame;

    if (scratch)
        g_esp += 0x50;
}


/* Which open file handles were asked for asynchronously.
 *
 * NtCreateFile takes FILE_SYNCHRONOUS_IO_ALERT (0x10) and
 * FILE_SYNCHRONOUS_IO_NONALERT (0x20) in CreateOptions. With neither, the
 * handle is asynchronous: NtReadFile on it returns STATUS_PENDING even with
 * no event, and the caller waits on the handle. Completing every read
 * synchronously answers a different question than the one that was asked.
 *
 * A flat array because a title has a handful of files open at once and a
 * linear scan of sixty-four entries costs less than a hash would.
 */
#define BRIDGE_ASYNC_MAX 4096
static uint32_t g_async_handles[BRIDGE_ASYNC_MAX];

static void bridge_note_async_handle(uint32_t token)
{
    int i;
    if (!token)
        return;
    for (i = 0; i < BRIDGE_ASYNC_MAX; i++)
        if (g_async_handles[i] == token)
            return;
    for (i = 0; i < BRIDGE_ASYNC_MAX; i++)
        if (!g_async_handles[i]) { g_async_handles[i] = token; return; }
}

static void bridge_forget_async_handle(uint32_t token)
{
    int i;
    for (i = 0; i < BRIDGE_ASYNC_MAX; i++)
        if (g_async_handles[i] == token) { g_async_handles[i] = 0; return; }
}

static int bridge_handle_is_async(uint32_t token)
{
    int i;
    if (!token)
        return 0;
    for (i = 0; i < BRIDGE_ASYNC_MAX; i++)
        if (g_async_handles[i] == token)
            return 1;
    return 0;
}

/* Which file each open handle is, by the end of its host path.
 *
 * A slow read was a line saying "NtReadFile took 180 ms" on handle 0x3A4, and
 * the [PATH] line that opened 0x3A4 could be thousands of lines up, or before
 * the log was even rotated. Naming the file on the slow line itself is what
 * makes it read as "the music file" rather than as a handle to go hunting for.
 * Same flat-array reasoning as the async handles above. */
#define BRIDGE_FILE_NAMES 128
static struct { uint32_t token; char name[72]; } g_file_names[BRIDGE_FILE_NAMES];
static CRITICAL_SECTION g_file_names_lock;
static LONG g_file_names_init;

static void bridge_file_names_lock(void)
{
    if (InterlockedCompareExchange(&g_file_names_init, 1, 0) == 0) {
        InitializeCriticalSection(&g_file_names_lock);
        InterlockedExchange(&g_file_names_init, 2);
    }
    while (g_file_names_init != 2)
        Sleep(0);
    EnterCriticalSection(&g_file_names_lock);
}

static void bridge_note_file_name(uint32_t token)
{
    const wchar_t *w = xbox_LastHostPath();
    size_t len = wcslen(w), from = len > 70 ? len - 70 : 0, i;
    int slot = -1, k;
    if (!token)
        return;
    bridge_file_names_lock();
    for (k = 0; k < BRIDGE_FILE_NAMES; k++) {
        if (g_file_names[k].token == token) { slot = k; break; }
        if (slot < 0 && !g_file_names[k].token) slot = k;
    }
    if (slot >= 0) {
        g_file_names[slot].token = token;
        for (i = 0; from + i < len && i < sizeof g_file_names[slot].name - 1; i++)
            g_file_names[slot].name[i] = (char)(w[from + i] < 128 ? w[from + i] : '?');
        g_file_names[slot].name[i] = 0;
    }
    LeaveCriticalSection(&g_file_names_lock);
}

static void bridge_forget_file_name(uint32_t token)
{
    int k;
    if (!token)
        return;
    bridge_file_names_lock();
    for (k = 0; k < BRIDGE_FILE_NAMES; k++)
        if (g_file_names[k].token == token) { g_file_names[k].token = 0; break; }
    LeaveCriticalSection(&g_file_names_lock);
}

/* Copies the name out: the slot can be reused by another thread's open. */
static int bridge_file_name(uint32_t token, char *out, size_t size)
{
    int k, found = 0;
    if (!token || !size)
        return 0;
    bridge_file_names_lock();
    for (k = 0; k < BRIDGE_FILE_NAMES; k++)
        if (g_file_names[k].token == token) {
            snprintf(out, size, "%s", g_file_names[k].name);
            found = 1;
            break;
        }
    LeaveCriticalSection(&g_file_names_lock);
    return found;
}

/* Off unless RECOMP_ASYNC_IO is set, so the two behaviours stay comparable
 * rather than one being swapped in blind. */
static int bridge_async_io_enabled(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("RECOMP_ASYNC_IO") ? 1 : 0;
    return on;
}

/* ── NtReadFile (ordinal 219, 8 args = 32 bytes) ──────── */
static void bridge_NtReadFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t iostatus  = STACK_ARG(4);
    uint32_t buffer_va = STACK_ARG(5);
    uint32_t length    = STACK_ARG(6);
    uint32_t offset_va = STACK_ARG(7);
    XBOX_IO_STATUS_BLOCK ios;
    LARGE_INTEGER  off = {0};
    PLARGE_INTEGER poff = NULL;

    memset(&ios, 0, sizeof(ios));
    if (!bridge_buf_ok(buffer_va, length, "NtReadFile")) {
        bridge_write_iostatus(iostatus, (NTSTATUS)0xC0000005, 0);
        g_eax = 0xC0000005u;                 /* STATUS_ACCESS_VIOLATION */
        return;
    }
    if (offset_va) {
        off.LowPart  = BRIDGE_MEM32(offset_va);
        off.HighPart = (LONG)BRIDGE_MEM32(offset_va + 4);
        poff = &off;
    }
    {
        /* RECOMP_READ_DELAY_MS=n: streaming-sized reads take about as long as the console's DVD would, to tell
         * whether a title's streaming relies on reads not being instant */
        static int delay = -1;
        if (delay < 0) { const char *e = getenv("RECOMP_READ_DELAY_MS"); delay = e ? atoi(e) : 0; }
        if (delay > 0 && length >= 65536) Sleep((DWORD)delay);  /* streaming-sized reads only */
    }
    g_eax = (uint32_t)xbox_NtReadFile(handle, NULL, NULL, NULL, &ios,
                XBOX_TO_NATIVE(buffer_va), length, poff);

    /* What a read actually delivered. A decoder that rejects its input cannot
     * say whether the bytes were wrong or the read was, and the two look
     * identical from inside the title -- the first bytes settle it. */
    {
        const uint8_t *p = (const uint8_t *)XBOX_TO_NATIVE(buffer_va);
        uint32_t got = (uint32_t)ios.Information;
        /* The offset matters as much as the length. A title streaming a pack
         * file reads sector-aligned chunks, so the first bytes belong to
         * whatever precedes the file it actually wants, and a read that stops
         * early looks identical to one that never started -- until you can
         * see where each one landed. */
        if (poff)
            fprintf(stderr, "  [READ] from=0x%08X ev=%08X apc=%08X @%lld want=%u got=%u st=0x%08X  %02X %02X %02X %02X\n",
                    g_xbox_kernel_caller, STACK_ARG(1), STACK_ARG(2),
                    (long long)off.QuadPart, length, got,
                    (uint32_t)ios.Status,
                    got > 0 ? p[0] : 0, got > 1 ? p[1] : 0,
                    got > 2 ? p[2] : 0, got > 3 ? p[3] : 0);
        else
            fprintf(stderr, "  [READ] from=0x%08X @seq want=%u got=%u st=0x%08X  %02X %02X %02X %02X\n",
                    g_xbox_kernel_caller,
                    length, got, (uint32_t)ios.Status,
                    got > 0 ? p[0] : 0, got > 1 ? p[1] : 0,
                    got > 2 ? p[2] : 0, got > 3 ? p[3] : 0);
        fflush(stderr);
    }
    bridge_write_iostatus(iostatus, ios.Status, (uint32_t)ios.Information);
    bridge_complete_file_io(STACK_ARG(1), STACK_ARG(2), STACK_ARG(3),
                            iostatus);

    /* An asynchronous request returns STATUS_PENDING, even when the data was
     * already there.
     *
     * A caller that asked to be told later is told later on Windows: the read
     * returns 0x00000103 and the status block carries the result once the
     * handle signals. Returning STATUS_SUCCESS instead is not a harmless
     * shortcut, it is a different contract, and code written against the real
     * one takes the branch that says nothing is in flight.
     *
     * The read itself stays synchronous here: the status block is already
     * written and the event already signalled, so a caller that waits is
     * satisfied immediately. Only the answer changes.
     *
     * An event or APC alone does not make a read asynchronous: on a
     * synchronous handle the kernel waits and returns the final status. */
    if (bridge_async_io_enabled() && bridge_handle_is_async(STACK_ARG(0)))
        g_eax = STATUS_PENDING;
}

/* ── NtWriteFile (ordinal 236, 8 args = 32 bytes) ─────── */
static void bridge_NtWriteFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t iostatus  = STACK_ARG(4);
    uint32_t buffer_va = STACK_ARG(5);
    uint32_t length    = STACK_ARG(6);
    uint32_t offset_va = STACK_ARG(7);
    XBOX_IO_STATUS_BLOCK ios;
    LARGE_INTEGER  off = {0};
    PLARGE_INTEGER poff = NULL;

    /* As the kernel: an offset is only negative to mean the current position (-2) or the end of the file (-1), and a
     * file not opened for synchronous I/O has no current position, so it must be given one. */
    if (offset_va) {
        LONG hi = (LONG)BRIDGE_MEM32(offset_va + 4);
        uint32_t lo = BRIDGE_MEM32(offset_va);
        if (hi < 0 && !(hi == -1 && (lo == 0xFFFFFFFFu || lo == 0xFFFFFFFEu))) {
            g_eax = 0xC000000Du;
            return;
        }
    } else if (handle && bridge_handle_is_async(STACK_ARG(0))) {
        g_eax = 0xC000000Du;
        return;
    }
    memset(&ios, 0, sizeof(ios));
    if (!bridge_buf_ok(buffer_va, length, "NtWriteFile")) {
        bridge_write_iostatus(iostatus, (NTSTATUS)0xC0000005, 0);
        g_eax = 0xC0000005u;                 /* STATUS_ACCESS_VIOLATION */
        return;
    }
    if (offset_va) {
        off.LowPart  = BRIDGE_MEM32(offset_va);
        off.HighPart = (LONG)BRIDGE_MEM32(offset_va + 4);
        poff = &off;
    }
    g_eax = (uint32_t)xbox_NtWriteFile(handle, NULL, NULL, NULL, &ios,
                XBOX_TO_NATIVE(buffer_va), length, poff);
    /* Paired with [READ]: what a write asked for and what it got. */
    fprintf(stderr, "  [WRITE] from=0x%08X @%lld want=%u got=%u st=0x%08X\n",
            g_xbox_kernel_caller, poff ? (long long)off.QuadPart : -1LL,
            length, (uint32_t)ios.Information, g_eax);
    bridge_write_iostatus(iostatus, ios.Status, (uint32_t)ios.Information);
    bridge_complete_file_io(STACK_ARG(1), STACK_ARG(2), STACK_ARG(3),
                            iostatus);
}

/* ── NtQueryInformationFile (ordinal 211, 5 args = 20 bytes) */
static void bridge_NtQueryInformationFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va    = STACK_ARG(1);
    uint32_t info_va   = STACK_ARG(2);
    uint32_t length    = STACK_ARG(3);
    uint32_t infoclass = STACK_ARG(4);
    XBOX_IO_STATUS_BLOCK ios;

        memset(&ios, 0, sizeof(ios));

    if (infoclass >= 0x25) {
        g_eax = 0xC0000003u;                 /* STATUS_INVALID_INFO_CLASS */
        return;
    }
    if (!bridge_buf_ok(info_va, length, "NtQueryInformationFile")) {
        bridge_write_iostatus(ios_va, (NTSTATUS)0xC0000005, 0);
        g_eax = 0xC0000005u;                 /* STATUS_ACCESS_VIOLATION */
        return;
    }
    /* The I/O manager answers these itself. Our handles are synchronous, with
     * byte alignment. */
    if ((infoclass == 16 || infoclass == 17) && length >= 4) {
        BRIDGE_MEM32(info_va) = infoclass == 16 ? 0x20u /* FILE_SYNCHRONOUS_IO_NONALERT */ : 0u;
        bridge_write_iostatus(ios_va, 0, 4);
        g_eax = 0;
        return;
    }
    g_eax = (uint32_t)xbox_NtQueryInformationFile(handle, &ios,
                XBOX_TO_NATIVE(info_va), length,
                (XBOX_FILE_INFORMATION_CLASS)infoclass);
    fprintf(stderr, "  [FINFO] QueryInformationFile from=0x%08X class=%u len=%u st=0x%08X  %08X %08X\n",
            g_xbox_kernel_caller, infoclass, length, g_eax,
            length >= 4 ? BRIDGE_MEM32(info_va) : 0, length >= 8 ? BRIDGE_MEM32(info_va + 4) : 0);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtSetInformationFile (ordinal 226, 5 args = 20 bytes) ─ */
static void bridge_NtSetInformationFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va    = STACK_ARG(1);
    uint32_t info_va   = STACK_ARG(2);
    uint32_t length    = STACK_ARG(3);
    uint32_t infoclass = STACK_ARG(4);
    XBOX_IO_STATUS_BLOCK ios;

        memset(&ios, 0, sizeof(ios));

    if (infoclass >= 0x25) {
        g_eax = 0xC0000003u;                 /* STATUS_INVALID_INFO_CLASS */
        return;
    }
    if (!bridge_buf_ok(info_va, length, "NtSetInformationFile")) {
        bridge_write_iostatus(ios_va, (NTSTATUS)0xC0000005, 0);
        g_eax = 0xC0000005u;                 /* STATUS_ACCESS_VIOLATION */
        return;
    }
    /* Rename: ReplaceIfExists +0, RootDirectory +4, FileName (Length, Max, Buffer) +8. */
    if (infoclass == 10 && length >= 16) {
        uint32_t nbytes = BRIDGE_MEM16(info_va + 8), buf = BRIDGE_MEM32(info_va + 12);
        char path[512];
        NTSTATUS st;
        if (!buf || nbytes == 0 || nbytes >= sizeof path) {
            g_eax = 0xC000000Du;
            return;
        }
        memcpy(path, XBOX_TO_NATIVE(buf), nbytes);
        path[nbytes] = 0;
        st = xbox_RenameFile(handle, path, BRIDGE_MEM32(info_va + 4)
                                 ? bridge_resolve_handle(BRIDGE_MEM32(info_va + 4)) : NULL,
                             BRIDGE_MEM8(info_va) != 0);
        bridge_write_iostatus(ios_va, st, 0);
        g_eax = (uint32_t)st;
        return;
    }
    /* Position, end of file and allocation can't be negative. */
    if ((infoclass == 14 || infoclass == 19 || infoclass == 20) && length >= 8 &&
        (int32_t)BRIDGE_MEM32(info_va + 4) < 0) {
        g_eax = 0xC000000Du;                 /* STATUS_INVALID_PARAMETER */
        return;
    }
    g_eax = (uint32_t)xbox_NtSetInformationFile(handle, &ios,
                XBOX_TO_NATIVE(info_va), length,
                (XBOX_FILE_INFORMATION_CLASS)infoclass);
    fprintf(stderr, "  [FINFO] SetInformationFile from=0x%08X class=%u len=%u st=0x%08X  %08X %08X\n",
            g_xbox_kernel_caller, infoclass, length, g_eax,
            length >= 4 ? BRIDGE_MEM32(info_va) : 0, length >= 8 ? BRIDGE_MEM32(info_va + 4) : 0);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtQueryVolumeInformationFile (ordinal 218, 5 args = 20 bytes) */
static void bridge_NtQueryVolumeInformationFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va    = STACK_ARG(1);
    uint32_t info_va   = STACK_ARG(2);
    uint32_t length    = STACK_ARG(3);
    uint32_t infoclass = STACK_ARG(4);
    XBOX_IO_STATUS_BLOCK ios;
    /* The kernel's table of volume classes and their sizes; 0 is no such class */
    static const uint8_t kFsClassSize[9] = {0, 24, 0, 24, 8, 16, 48, 32, 64};

    if (infoclass >= 9 || !kFsClassSize[infoclass]) {
        g_eax = 0xC0000003u;                 /* STATUS_INVALID_INFO_CLASS */
        return;
    }
    if (length < kFsClassSize[infoclass]) {
        g_eax = 0xC0000004u;                 /* STATUS_INFO_LENGTH_MISMATCH */
        return;
    }
    memset(&ios, 0, sizeof(ios));
    if (!bridge_buf_ok(info_va, length, "NtQueryVolumeInformationFile")) {
        bridge_write_iostatus(ios_va, (NTSTATUS)0xC0000005, 0);
        g_eax = 0xC0000005u;                 /* STATUS_ACCESS_VIOLATION */
        return;
    }
    g_eax = (uint32_t)xbox_NtQueryVolumeInformationFile(handle, &ios,
                XBOX_TO_NATIVE(info_va), length,
                (XBOX_FS_INFORMATION_CLASS)infoclass);
    fprintf(stderr, "  [FINFO] QueryVolumeInformationFile from=0x%08X class=%u len=%u st=0x%08X  %08X %08X\n",
            g_xbox_kernel_caller, infoclass, length, g_eax,
            length >= 4 ? BRIDGE_MEM32(info_va) : 0, length >= 8 ? BRIDGE_MEM32(info_va + 4) : 0);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtQueryFullAttributesFile (ordinal 210, 2 args = 8 bytes) */
static void bridge_NtQueryFullAttributesFile(void)
{
    uint32_t obj_attrs = STACK_ARG(0);
    uint32_t info_va   = STACK_ARG(1);
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING       name;

    bridge_build_oa(obj_attrs, &oa, &name);
    if (!name.Buffer) { g_eax = STATUS_OBJECT_PATH_NOT_FOUND; return; }
    g_eax = (uint32_t)xbox_NtQueryFullAttributesFile(&oa,
                (PXBOX_FILE_NETWORK_OPEN_INFORMATION)XBOX_TO_NATIVE(info_va));
}

/* ── NtFlushBuffersFile (ordinal 198, 2 args = 8 bytes) ─── */
static void bridge_NtFlushBuffersFile(void)
{
    HANDLE   handle = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va = STACK_ARG(1);
    XBOX_IO_STATUS_BLOCK ios;

    if (!handle) { g_eax = 0xC0000008u; return; }   /* as the kernel, no status block written */
    memset(&ios, 0, sizeof(ios));
    g_eax = (uint32_t)xbox_NtFlushBuffersFile(handle, &ios);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtDeleteFile (ordinal 195, 1 arg = 4 bytes) ─────── */
static void bridge_NtDeleteFile(void)
{
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING       name;

    bridge_build_oa(STACK_ARG(0), &oa, &name);
    if (!name.Buffer) { g_eax = STATUS_OBJECT_PATH_NOT_FOUND; return; }
    g_eax = (uint32_t)xbox_NtDeleteFile(&oa);
}

/* ── NtQueryDirectoryFile (ordinal 207, 10 args = 40 bytes) ─ */
static void bridge_NtQueryDirectoryFile(void)
{
    HANDLE   handle      = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va      = STACK_ARG(4);
    uint32_t info_va     = STACK_ARG(5);
    uint32_t length      = STACK_ARG(6);
    uint32_t info_class  = STACK_ARG(7);
    uint32_t filename_va = STACK_ARG(8);  /* PXBOX_ANSI_STRING */
    uint32_t restart     = STACK_ARG(9);  /* BOOLEAN */
    XBOX_IO_STATUS_BLOCK ios;
    XBOX_ANSI_STRING     fn;
    PXBOX_ANSI_STRING    pfn = NULL;

    memset(&ios, 0, sizeof(ios));
    if (filename_va) {
        /* Xbox ANSI_STRING: 0=Length(u16), 2=MaximumLength(u16), 4=Buffer(u32) */
        uint32_t fn_buf  = BRIDGE_MEM32(filename_va + 4);
        fn.Length        = BRIDGE_MEM16(filename_va);
        fn.MaximumLength = BRIDGE_MEM16(filename_va + 2);
        fn.Buffer        = fn_buf ? (PCHAR)XBOX_TO_NATIVE(fn_buf) : NULL;
        if (fn.Buffer) pfn = &fn;
    }
    g_eax = (uint32_t)xbox_NtQueryDirectoryFile(handle, NULL, NULL, NULL, &ios,
                XBOX_TO_NATIVE(info_va), length, (XBOX_FILE_INFORMATION_CLASS)info_class,
                pfn, (BOOLEAN)restart);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtOpenSymbolicLinkObject (ordinal 203, 2 args = 8 bytes) */
#define BRIDGE_LINK_TOKEN 0xDEAE0000u
#define BRIDGE_LINK_OPENS 1024
static const char *g_link_opened[BRIDGE_LINK_OPENS];
static LONG g_link_open_count;

static void bridge_NtOpenSymbolicLinkObject(void)
{
    /* A link the title created answers for itself, and \\??\\D: and \\??\\CdRom0: are the disc,
     * as the kernel's own. Anything else does not exist: C0000034, as the object manager
     * answers (0x8002626D). The disc's token is written raw (untagged) so NtClose recognises
     * and skips it. */
    extern const char *xbox_SymlinkTarget(const char *);
    uint32_t handle_va = STACK_ARG(0), oa = STACK_ARG(1), token = 0;
    uint32_t str = oa ? BRIDGE_MEM32(oa + 4) : 0;
    char name[64] = "";
    if (str) {
        uint16_t n = BRIDGE_MEM16(str);
        if (n < sizeof name && BRIDGE_MEM32(str + 4)) {
            memcpy(name, XBOX_TO_NATIVE(BRIDGE_MEM32(str + 4)), n);
            name[n] = 0;
        }
    }
    {
        const char *target = name[0] ? xbox_SymlinkTarget(name) : NULL;
        if (target) {
            LONG i = InterlockedIncrement(&g_link_open_count) - 1;
            if (i < BRIDGE_LINK_OPENS) {
                g_link_opened[i] = target;
                token = BRIDGE_LINK_TOKEN + (uint32_t)i;
            } else {
                token = 0xDEAD0001u;
            }
        } else if (!_stricmp(name, "\\??\\D:") || !_stricmp(name, "\\??\\CdRom0:")) {
            token = 0xDEAD0001u;
        }
    }
    if (handle_va) BRIDGE_MEM32(handle_va) = token;
    if (!token)
        fprintf(stderr, "  [LINK] no link '%s' from 0x%08X\n", name, g_xbox_kernel_caller);
    g_eax = token ? STATUS_SUCCESS : 0xC0000034u;
}

/* ── NtQuerySymbolicLinkObject (ordinal 215, 3 args = 12 bytes) */
static void bridge_NtQuerySymbolicLinkObject(void)
{
    uint32_t handle = STACK_ARG(0);
    uint32_t target_va = STACK_ARG(1);
    uint32_t retlen_va = STACK_ARG(2);
    const char* target = "\\Device\\CdRom0";
    USHORT len;

    if (handle >= BRIDGE_LINK_TOKEN && handle < BRIDGE_LINK_TOKEN + BRIDGE_LINK_OPENS)
        target = g_link_opened[handle - BRIDGE_LINK_TOKEN];
    len = (USHORT)strlen(target);

    if (retlen_va) BRIDGE_MEM32(retlen_va) = (uint32_t)len;

    /* Say so when the buffer could not be filled.
     *
     * Reporting STATUS_SUCCESS with an untouched output buffer is the same
     * defect that left ordinal 215 unrouted: the caller believes it has a
     * device path and parses whatever was already in that memory. Half-Life 2
     * does exactly that -- it walked uninitialised bytes and dereferenced
     * 0x68737572, the ASCII "rush", as a pointer. That only looked survivable
     * because the RAM mirrors happened to back the address; with a mapping
     * that does not alias, it faults immediately.
     *
     * STATUS_BUFFER_TOO_SMALL is the honest answer, and it is one the caller
     * already has to handle -- it is what a real kernel returns when the
     * ANSI_STRING it was handed has no room. */
    if (!target_va) {
        g_eax = 0xC0000023u;             /* STATUS_BUFFER_TOO_SMALL */
        return;
    }
    {
        uint16_t max_len = BRIDGE_MEM16(target_va + 2);
        uint32_t buf_va  = BRIDGE_MEM32(target_va + 4);

        /* As the kernel: what fits is copied, and too little room says so. No NUL: it is a counted string. */
        uint16_t copy = len <= max_len ? len : max_len;
        if (buf_va && copy) memcpy(XBOX_TO_NATIVE(buf_va), target, copy);
        BRIDGE_MEM16(target_va) = copy;
        if (buf_va && copy < max_len) BRIDGE_MEM8(buf_va + copy) = 0;   /* kept for code that has come to rely on one */
        if (len > max_len) {
            g_eax = 0xC0000023u;
            return;
        }
    }
    g_eax = STATUS_SUCCESS;
}

/* ── IoCreateFile (ordinal 67, 10 args = 40 bytes) ────── */
static void bridge_IoCreateFile(void)
{
    /* Same as NtCreateFile with an extra Options arg at the end */
    uint32_t handle_va   = STACK_ARG(0);
    uint32_t access      = STACK_ARG(1);
    uint32_t obj_attrs   = STACK_ARG(2);
    uint32_t iostatus    = STACK_ARG(3);
    uint32_t file_attrs  = STACK_ARG(5);
    uint32_t share       = STACK_ARG(6);
    uint32_t disposition = STACK_ARG(7);
    uint32_t options     = STACK_ARG(8);

    g_eax = (uint32_t)bridge_create_file_impl(
        handle_va, access, obj_attrs, iostatus,
        file_attrs, share, disposition, options);
}

/* -- NtDeviceIoControlFile (ordinal 196, 10 args = 40 bytes) ----
 *
 * NTSTATUS NtDeviceIoControlFile(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID,
 *                                PIO_STATUS_BLOCK, ULONG IoControlCode,
 *                                PVOID In, ULONG InLen,
 *                                PVOID Out, ULONG OutLen);
 */
#define IOCTL_DISK_GET_DRIVE_GEOMETRY 0x00070000u
#define IOCTL_DISK_GET_PARTITION_INFO 0x00074004u

/* The retail hard disk, in the units DISK_GEOMETRY reports. Deliberately the
 * same geometry kernel_path.c writes into the partition table it synthesises,
 * so a title that reads both sees one disk rather than two. */
#define XBOX_DISK_BYTES_PER_SECTOR   512u
#define XBOX_DISK_SECTORS_PER_TRACK  63u
#define XBOX_DISK_TRACKS_PER_CYL     255u
#define XBOX_DISK_CYLINDERS          1216u    /* ~10 GB, a retail 8 GB drive */

static void bridge_NtDeviceIoControlFile(void)
{
    uint32_t handle  = STACK_ARG(0);
    uint32_t ios_va  = STACK_ARG(4);
    uint32_t ioctl   = STACK_ARG(5);
    uint32_t out_va  = STACK_ARG(8);
    uint32_t out_len = STACK_ARG(9);

    /* IOCTLs aimed at the DVD device (see bridge_is_cdrom_device).
     *
     * These are the media check: the title asks the drive to confirm a disc is
     * present and that it is the one it expects. There is no drive here and no
     * disc to describe, so the honest answer is the one that lets the title
     * proceed -- the alternative is STATUS_NOT_SUPPORTED, which it reads as a
     * failed check and answers with HalReturnToFirmware.
     *
     * Reported rather than silent: which codes a title sends is the useful
     * fact when the check still fails, and guessing at them from documentation
     * is how this layer accumulates handlers for IOCTLs nothing ever sends. The
     * output buffer is zeroed, so a title that reads a result field back sees a
     * defined value instead of whatever was on its heap. */
    if (handle == BRIDGE_CDROM_HANDLE) {
        uint32_t in_va  = STACK_ARG(6);
        uint32_t in_len = STACK_ARG(7);

        /* The media check arrives as a SCSI pass-through, so the answer the
         * title reads is not the IOCTL's output buffer -- that is NULL here,
         * with length zero -- but the DataBuffer the request points at.
         * Returning STATUS_SUCCESS alone leaves that buffer as the title
         * zeroed it, which it reads as a failed check; DDS9 retries five
         * times and then exits through HalReturnToFirmware.
         *
         * SCSI_PASS_THROUGH_DIRECT, 32-bit layout, 44 bytes:
         *   0 Length(USHORT)  2 ScsiStatus  3 PathId  4 TargetId  5 Lun
         *   6 CdbLength  7 SenseInfoLength  8 DataIn
         *   12 DataTransferLength  16 TimeOutValue  20 DataBuffer
         *   24 SenseInfoOffset  28 Cdb[16] */
        if (in_va && in_len >= 44 && BRIDGE_MEM8(in_va + 28) == 0x5A) {
            uint32_t data_va  = BRIDGE_MEM32(in_va + 20);
            uint32_t data_len = BRIDGE_MEM32(in_va + 12);
            uint32_t page     = BRIDGE_MEM8(in_va + 30) & 0x3F;

            fprintf(stderr, "  [FILE] DVD MODE SENSE(10) page 0x%02X, "
                            "%u bytes -> authentication page\n", page, data_len);

            if (data_va && data_len) {
                uint32_t i;
                for (i = 0; i < data_len; i++)
                    BRIDGE_MEM8(data_va + i) = 0;

                /* An 8-byte MODE SENSE(10) parameter header, then the page.
                 * The three bytes that matter are named by the title's own
                 * validation at guest 0x0021EA56-0x0021EA6D, which is the only
                 * specification of this page there is: byte 11 must be exactly
                 * 1, and bytes 10 and 12 must both be non-zero. Anything else
                 * is read as "not the expected disc". */
                if (data_len >= 2) {
                    BRIDGE_MEM8(data_va + 0) = 0;
                    BRIDGE_MEM8(data_va + 1) = 26;   /* mode data length */
                }
                if (data_len >= 10) {
                    BRIDGE_MEM8(data_va + 8) = 0x3E; /* page code */
                    BRIDGE_MEM8(data_va + 9) = 18;   /* page length */
                }
                if (data_len >= 13) {
                    BRIDGE_MEM8(data_va + 10) = 1;   /* non-zero */
                    BRIDGE_MEM8(data_va + 11) = 1;   /* exactly 1 */
                    BRIDGE_MEM8(data_va + 12) = 1;   /* non-zero */
                }
            }

            BRIDGE_MEM8(in_va + 2) = 0;              /* ScsiStatus = GOOD */
            bridge_write_iostatus(ios_va, 0, in_len);
            g_eax = 0;
            return;
        }

        fprintf(stderr, "  [FILE] DVD device IOCTL 0x%X (in=%u out=%u) "
                        "-> STATUS_SUCCESS\n", ioctl, in_len, out_len);
        if (out_va && out_len) {
            uint32_t i;
            for (i = 0; i < out_len; i++)
                BRIDGE_MEM8(out_va + i) = 0;
        }
        bridge_write_iostatus(ios_va, 0, out_len);
        g_eax = 0;
        return;
    }

    if (ioctl == IOCTL_DISK_GET_DRIVE_GEOMETRY) {
        /* DISK_GEOMETRY: Cylinders (LARGE_INTEGER), MediaType,
         * TracksPerCylinder, SectorsPerTrack, BytesPerSector -- 24 bytes. */
        if (!out_va || out_len < 24) {
            bridge_write_iostatus(ios_va, 0xC0000023u, 0); /* BUFFER_TOO_SMALL */
            g_eax = 0xC0000023u;
            return;
        }
        BRIDGE_MEM32(out_va +  0) = XBOX_DISK_CYLINDERS;
        BRIDGE_MEM32(out_va +  4) = 0;
        BRIDGE_MEM32(out_va +  8) = 0x0B;      /* FixedMedia */
        BRIDGE_MEM32(out_va + 12) = XBOX_DISK_TRACKS_PER_CYL;
        BRIDGE_MEM32(out_va + 16) = XBOX_DISK_SECTORS_PER_TRACK;
        BRIDGE_MEM32(out_va + 20) = XBOX_DISK_BYTES_PER_SECTOR;
        bridge_write_iostatus(ios_va, 0, 24);
        g_eax = 0;
        return;
    }

    if (ioctl == IOCTL_DISK_GET_PARTITION_INFO) {
        /* PARTITION_INFORMATION: StartingOffset and PartitionLength as
         * LARGE_INTEGERs, then HiddenSectors, PartitionNumber, and three
         * bytes of type/flags -- 32 bytes. The length is the image's own
         * size, which kernel_path.c set from the partition table. */
        HANDLE h = bridge_resolve_handle(STACK_ARG(0));
        LARGE_INTEGER size;

        if (!out_va || out_len < 32) {
            bridge_write_iostatus(ios_va, 0xC0000023u, 0);
            g_eax = 0xC0000023u;
            return;
        }
        size.QuadPart = 0;
        if (h && h != INVALID_HANDLE_VALUE)
            GetFileSizeEx(h, &size);
        BRIDGE_MEM32(out_va +  0) = 0;                       /* StartingOffset */
        BRIDGE_MEM32(out_va +  4) = 0;
        BRIDGE_MEM32(out_va +  8) = (uint32_t)size.LowPart;  /* PartitionLength */
        BRIDGE_MEM32(out_va + 12) = (uint32_t)size.HighPart;
        BRIDGE_MEM32(out_va + 16) = 0;                       /* HiddenSectors  */
        BRIDGE_MEM32(out_va + 20) = 1;                       /* PartitionNumber*/
        BRIDGE_MEM32(out_va + 24) = 0x00010106u;             /* type/boot/recog */
        BRIDGE_MEM32(out_va + 28) = 0;
        bridge_write_iostatus(ios_va, 0, 32);
        g_eax = 0;
        return;
    }

    fprintf(stderr, "  [FILE] NtDeviceIoControlFile(0x%X) - unhandled\n", ioctl);
    bridge_write_iostatus(ios_va, 0xC00000BBu, 0);
    g_eax = 0xC00000BBu; /* STATUS_NOT_SUPPORTED */
}

/* ── NtFsControlFile (ordinal 200, 10 args = 40 bytes) ──── */
#define FSCTL_DISMOUNT_VOLUME 0x00090020u
static void bridge_NtFsControlFile(void)
{
    uint32_t fsctl = STACK_ARG(5);
    uint32_t ios_va = STACK_ARG(4);
    /* XAPI's cache-partition format writes a fresh FAT through the raw
     * partition handle, then dismounts it so the next open mounts the new
     * volume. The host directory behind Z: has nothing cached to drop, and
     * the title ignores the status anyway; the kernel returns success. */
    if (fsctl == FSCTL_DISMOUNT_VOLUME) {
        bridge_write_iostatus(ios_va, 0, 0);
        g_eax = 0;
        return;
    }
    fprintf(stderr, "  [FILE] NtFsControlFile(0x%X) - stub\n", fsctl);
    bridge_write_iostatus(ios_va, 0xC00000BBu, 0);
    g_eax = 0xC00000BBu;
}

/* ── NtCreateDirectoryObject (ordinal 188) ──────────────── */
static void bridge_NtCreateDirectoryObject(void)
{
    /* Return STATUS_SUCCESS with a fake handle */
    uint32_t handle_ptr = STACK_ARG(0);
    if (handle_ptr) BRIDGE_MEM32(handle_ptr) = 0xBEEF0010;
    g_eax = 0;  /* STATUS_SUCCESS */
}

/* IoCreateSymbolicLink (ordinal 67, 2 args)
 *
 * Was a bare "return STATUS_SUCCESS": the title was told its link existed and
 * nothing recorded it. Titles mount their own drive letters this way --
 * Wreckless links \??\Z: to \Device\Harddisk0\Partition1\ and then loads
 * every asset through z:\ -- so dropping the link left xbox_translate_path
 * applying the generic "Z: is the cache partition" rule, and every asset open
 * failed with ERROR_FILE_NOT_FOUND a whole boot later.
 *
 * Both arguments are guest ANSI_STRINGs whose Buffer field holds a guest VA,
 * so passing the structs straight through would have xbox_copy_ansi read a
 * 32-bit guest address as a 64-bit host pointer. Rebuild them by hand, the way
 * bridge_RtlEqualString does.
 */
static void bridge_IoCreateSymbolicLink(void)
{
    uint32_t link_va   = STACK_ARG(0);
    uint32_t target_va = STACK_ARG(1);
    XBOX_ANSI_STRING link, target;

    if (!link_va) {
        g_eax = 0xC000000Du;  /* STATUS_INVALID_PARAMETER */
        return;
    }
    link.Length        = BRIDGE_MEM16(link_va + 0);
    link.MaximumLength = BRIDGE_MEM16(link_va + 2);
    link.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(link_va + 4));

    if (target_va) {
        target.Length        = BRIDGE_MEM16(target_va + 0);
        target.MaximumLength = BRIDGE_MEM16(target_va + 2);
        target.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(target_va + 4));
    } else {
        target.Length = target.MaximumLength = 0;
        target.Buffer = NULL;
    }

    g_eax = (uint32_t)xbox_IoCreateSymbolicLink(&link,
                                                target_va ? &target : NULL);
}

/* ── ObReferenceObjectByHandle (ordinal 246) ─────────────── */
static void bridge_ObReferenceObjectByHandle(void)
{
    /* Xbox: NTSTATUS ObReferenceObjectByHandle(HANDLE Handle, PVOID ObjectType, PVOID* Object)
     * 3 args (not 6 like Windows NT) */
    uint32_t handle = STACK_ARG(0);
    uint32_t obj_type = STACK_ARG(1);
    uint32_t object_ptr = STACK_ARG(2);
    {
        uint32_t own = ob_from_handle(handle);   /* one the title created */
        if (own) {
            if (obj_type && OB_TYPE(own) != obj_type) {
                if (object_ptr) BRIDGE_MEM32(object_ptr) = 0;
                g_eax = 0xC0000024u;
                return;
            }
            InterlockedIncrement((volatile LONG *)XBOX_TO_NATIVE(own - 0x10));
            if (object_ptr) BRIDGE_MEM32(object_ptr) = own;
            g_eax = 0;
            return;
        }
    }
    /* Thread handles get a stand-in KTHREAD so XAPI SetThreadPriority's
     * KeSetBasePriorityThread reaches the right host thread. Everything else
     * keeps the old answer (object 0). */
uint32_t obj = bridge_thread_object(handle);
    /* The console's two failures. A thread's own handle gets the stand-in; other
     * valid handles keep object 0, there being no guest object behind them. */
    if (!obj && handle != 0xFFFFFFFEu && handle < 0x48000000u && !bridge_handle_lookup(handle)) {
        if (object_ptr) BRIDGE_MEM32(object_ptr) = 0;
        g_eax = 0xC0000008u;   /* STATUS_INVALID_HANDLE */
        return;
    }
    if (!obj && handle < 0x48000000u && obj_type == XBOX_KERNEL_DATA_BASE + KDATA_THREAD_OBJ_TYPE) {
        if (object_ptr) BRIDGE_MEM32(object_ptr) = 0;
        g_eax = 0xC0000024u;   /* STATUS_OBJECT_TYPE_MISMATCH */
        return;
    }
    if (object_ptr) BRIDGE_MEM32(object_ptr) = obj;
    g_eax = 0;  /* STATUS_SUCCESS */
}

/* ── Exception dispatch, behind RECOMP_SEH=1 ──────────────
 *
 * RtlDispatchException as the kernel's (0x8002970A): the title's handlers are called
 * newest first; one that says continue ends it, and a raise it never handled is bug
 * check 0x1E (0x800221BC). Off by default, since a game has never been run with it.
 *
 * What this cannot do yet: a __except block or C++ catch is entered by a jump into the
 * middle of the function that holds it, and generated C has no way in. The handler's
 * `jmp` then finds no function. That needs the recompiler to emit landing points; see
 * docs/technical/seh-handling.md. A filter that says "continue execution", and raises
 * no handler takes, do work.
 *
 * Handlers run on the raising thread's guest stack, below a record and a context in the
 * kernel's layout: +0 ContextFlags, +4 floating save (0x200), then Edi +0x204, Esi, Ebx,
 * Edx, Ecx, Eax +0x218, Ebp, Eip +0x220, SegCs, EFlags, Esp +0x22C, SegSs.
 */
#define XBOX_CONTEXT_SIZE 0x234u
#define XBOX_EXC_NONCONTINUABLE 0x01u
#define XBOX_EXC_STACK_INVALID  0x08u
#define XBOX_EXC_NESTED         0x10u

/* The frame whose handler is running on this thread, and the chain as that dispatch
 * found it: on a console a frame around each handler call answers "nested" (0x800288A8). */
static RECOMP_TLS uint32_t s_seh_active, s_seh_outer_head;
/* A handler that answers "continue" to a noncontinuable raise is raised at again, and
 * again. A console runs out of stack and stops; here that would take the host down first. */
#define XBOX_SEH_MAX_DEPTH 16
static RECOMP_TLS int s_seh_depth;

static int bridge_seh_enabled(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("RECOMP_SEH"); on = e && *e == '1'; }
    return on;
}

static void bridge_raise_noncontinuable(uint32_t code, uint32_t inner);

static int bridge_dispatch_exception(uint32_t record, uint32_t context)
{
    uint32_t reg = BRIDGE_MEM32(XBOX_FS_BASE), nested_end = 0;
    uint32_t outer_active = s_seh_active, outer_head = s_seh_outer_head;
    uint32_t map = g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram;
    uint32_t head = reg;
    int guard, handled = 0;

    for (guard = 0; guard < XBOX_SEH_MAX_FRAMES; guard++) {
        uint32_t handler, disp, slot;
        recomp_func_t fn;

        if (reg == XBOX_SEH_END_OF_CHAIN)
            break;
        if ((reg & 3u) || reg < 0x10000u || reg + 8 > map) {
            BRIDGE_MEM32(record + 4) |= XBOX_EXC_STACK_INVALID;
            break;
        }
        if (outer_active && reg == outer_head) {   /* reached what the outer dispatch saw */
            BRIDGE_MEM32(record + 4) |= XBOX_EXC_NESTED;
            nested_end = outer_active;
        }
        handler = BRIDGE_MEM32(reg + 4);
        fn = recomp_lookup(handler);
        if (!fn) fn = recomp_lookup_manual(handler);
        if (!fn) fn = recomp_lookup_kernel(handler);
        if (!fn) {
            fprintf(stderr, "  [SEH] frame 0x%08X: no code at handler 0x%08X\n", reg, handler);
            reg = BRIDGE_MEM32(reg);
            continue;
        }

        /* handler(record, frame, context, &dispatcher context): cdecl */
        slot = g_esp - 4;
        BRIDGE_MEM32(slot) = 0;
        g_esp = slot;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = slot;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = context;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = reg;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = record;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
        s_seh_active = reg;
        s_seh_outer_head = head;
        fn();
        disp = g_eax;
        s_seh_active = outer_active;
        s_seh_outer_head = outer_head;
        g_esp += 16;
        g_esp += 4;

        if (nested_end == reg) {
            BRIDGE_MEM32(record + 4) &= ~XBOX_EXC_NESTED;
            nested_end = 0;
        }
        if (disp == 0) {               /* continue execution */
            if (BRIDGE_MEM32(record + 4) & XBOX_EXC_NONCONTINUABLE) {
                bridge_raise_noncontinuable(0xC0000025u, record);
                return 0;
            }
            handled = 1;
            break;
        } else if (disp == 2) {        /* nested */
            BRIDGE_MEM32(record + 4) |= XBOX_EXC_NESTED;
        } else if (disp != 1) {        /* not an answer */
            bridge_raise_noncontinuable(0xC0000026u, record);
            return 0;
        }
        reg = BRIDGE_MEM32(reg);
    }
    return handled;
}

/* RtlRaiseException (0x80029648): the title's registers in a context, then the handlers.
 * Handled: on from where it was raised, with the registers a handler left. Not: bug check. */
static void bridge_raise(uint32_t record)
{
    uint32_t saved_esp = g_esp, context;
    uint32_t code = BRIDGE_MEM32(record), addr = BRIDGE_MEM32(saved_esp);

    if (s_seh_depth >= XBOX_SEH_MAX_DEPTH) {
        fprintf(stderr, "[SEH] 0x%08X at 0x%08X: raised %d deep, stopping\n", code, addr, s_seh_depth);
        fflush(stderr);
        bridge_bugcheck_end(0x1E);
        return;
    }
    s_seh_depth++;
    if (!BRIDGE_MEM32(record + 0xC))
        BRIDGE_MEM32(record + 0xC) = addr;
    fprintf(stderr, "[SEH] raise 0x%08X at 0x%08X flags 0x%X\n", code, addr, BRIDGE_MEM32(record + 4));

    g_esp -= XBOX_CONTEXT_SIZE;
    context = g_esp;
    memset(XBOX_TO_NATIVE(context), 0, XBOX_CONTEXT_SIZE);
    BRIDGE_MEM32(context) = 0x1002Bu;
    BRIDGE_MEM32(context + 0x210) = g_edx;
    BRIDGE_MEM32(context + 0x214) = g_ecx;
    BRIDGE_MEM32(context + 0x218) = g_eax;
    BRIDGE_MEM32(context + 0x21C) = g_seh_ebp;
    BRIDGE_MEM32(context + 0x220) = addr;
    BRIDGE_MEM32(context + 0x22C) = saved_esp;

    if (bridge_dispatch_exception(record, context)) {
        g_edx = BRIDGE_MEM32(context + 0x210);
        g_ecx = BRIDGE_MEM32(context + 0x214);
        g_eax = BRIDGE_MEM32(context + 0x218);
        g_esp = saved_esp;
        s_seh_depth--;
        return;
    }
    g_esp = saved_esp;
    s_seh_depth--;
    fprintf(stderr, "[SEH] 0x%08X at 0x%08X was not handled\n", code, addr);
    fflush(stderr);
    bridge_bugcheck_end(0x1E);   /* KMODE_EXCEPTION_NOT_HANDLED */
}

/* A record of its own, for what the kernel raises: ExRaiseStatus and the misuse checks */
static void bridge_raise_noncontinuable(uint32_t code, uint32_t inner)
{
    uint32_t saved_esp = g_esp, record;
    g_esp -= 0x50;
    record = g_esp;
    memset(XBOX_TO_NATIVE(record), 0, 0x50);
    BRIDGE_MEM32(record) = code;
    BRIDGE_MEM32(record + 4) = XBOX_EXC_NONCONTINUABLE;
    BRIDGE_MEM32(record + 8) = inner;
    BRIDGE_MEM32(record + 0xC) = BRIDGE_MEM32(saved_esp);
    bridge_raise(record);
    g_esp = saved_esp;
}

/* ── RtlRaiseException (ordinal 302) ─────────────────────
 * VOID RtlRaiseException(PEXCEPTION_RECORD ExceptionRecord)
 *
 * Called by CRT / SEH code to raise structured exceptions.
 * On Xbox this triggers the kernel exception dispatcher.
 * For recompilation, we log and continue (no real SEH dispatch yet).
 */
static void bridge_RtlRaiseException(void)
{
    uint32_t record_ptr = STACK_ARG(0);
    uint32_t code = record_ptr ? BRIDGE_MEM32(record_ptr) : 0;

    static int raise_count = 0;
    if (record_ptr && bridge_seh_enabled()) {
        bridge_raise(record_ptr);
        return;
    }
    raise_count++;
    if (raise_count <= 10) {
        fprintf(stderr, "  [KERNEL] RtlRaiseException: record=0x%08X code=0x%08X (#%d)\n",
                record_ptr, code, raise_count);
        fflush(stderr);
    }

    /* Handle float exceptions by clearing the FPU status.
     *
     * On the real Xbox, RtlRaiseException dispatches through the SEH chain.
     * For float exceptions (0xC0000090-0xC0000096), the CRT exception handler
     * clears the x87/SSE status word and continues execution. Without clearing,
     * the caller re-checks the FPU status, sees the exception still pending,
     * and re-raises in an infinite loop.
     *
     * _clearfp() clears both x87 and SSE exception flags on Windows x64.
     */
    if (code >= 0xC0000090u && code <= 0xC0000096u) {
        _clearfp();
    }

    g_eax = 0;
}

/* ── MmMapIoSpace (ordinal 177) ──────────────────────────
 * PVOID MmMapIoSpace(ULONG_PTR PhysicalAddress, ULONG NumberOfBytes, ULONG Protect)
 *
 * Maps physical I/O memory (GPU registers, etc.) into virtual address space.
 * Allocate from Xbox heap so the returned pointer is a valid Xbox VA.
 */
/* The kernel's check of an Mm* protection: read-only or read-write, optionally one of no-cache or write-combine */
static int mm_protect_ok(uint32_t p)
{
    return !(p & ~0x606u) && ((p & 6) == 2 || (p & 6) == 4) && (p & 0x600) != 0x600;
}

/* And of an Nt* one (NtProtectVirtualMemory). One of the access values, in the low nibble or the high, with
 * optionally PAGE_GUARD (0x100), no-cache (0x200) or write-combine (0x400), but no guard on PAGE_NOACCESS */
static int nt_protect_ok(uint32_t p)
{
    uint32_t lo = p & 0xF, hi = (p >> 4) & 0xF;
    static const signed char kAccess[16] = {-1, 0, 0, -1, 2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};  /* the kernel's */
    if (p & ~0x7FFu) return 0;
    if ((p & 1) && (p & 0x700)) return 0;
    if ((p & 0x200) && (p & 0x400)) return 0;
    if ((lo == 0) == (hi == 0)) return 0;
    return kAccess[lo | hi] != -1;
}

static void bridge_MmMapIoSpace(void)
{
    uint32_t phys_addr = STACK_ARG(0);
    uint32_t num_bytes = STACK_ARG(1);
    uint32_t protect = STACK_ARG(2);
    uint32_t offset = phys_addr & 0xFFF, xbox_va;

    if (!mm_protect_ok(protect)) {   /* refused, as the kernel */
        g_eax = 0;
        return;
    }
    /* Uncached device space is its own address on a console, and here where the device models answer */
    if (protect == 0x204 && phys_addr >= 0xFD000000u && phys_addr + num_bytes - 1 <= 0xFFBFFFFFu) {
        g_eax = phys_addr;
        return;
    }
    /* Otherwise pages of their own, with the offset into the page kept */
    xbox_va = xbox_HeapAlloc((offset + num_bytes + 0xFFF) & ~0xFFFu, 4096);
    if (xbox_va) xbox_va += offset;

    fprintf(stderr, "  [KERNEL] MmMapIoSpace: phys=0x%08X size=%u → Xbox VA 0x%08X\n",
            phys_addr, num_bytes, xbox_va);
    fflush(stderr);

    g_eax = xbox_va;
}

/* ── MmPersistContiguousMemory (ordinal 178) ─────────────
 * VOID MmPersistContiguousMemory(PVOID BaseAddress, ULONG NumberOfBytes, BOOLEAN Persist)
 *
 * Marks contiguous memory as persistent across reboots (for save data).
 * No-op for recompilation.
 */
static void bridge_MmPersistContiguousMemory(void)
{
    /* No-op stub */
    g_eax = 0;
}

/* ── Generic fallback for simple value-only functions ────── */
static void bridge_generic_stub(void)
{
    /* Success-returning stub for functions whose callers only check for 0.
     * Deliberately silent: the caller (kernel_thunk_dispatch) warns for
     * ordinals with no bridge at all, which is the case worth hearing about. */
    g_eax = 0;
}


/* ══════════════════════════════════════════════════════════════════════════
 * Wrappers for the ordinals Halo 2276's thunk table binds but the bridge did
 * not route. Every one of these already had a working xbox_* implementation in
 * src/kernel/*.c; only the wrapper that moves arguments off the simulated stack
 * was missing, so each call was silently a no-op returning 0.
 *
 * Guest pointers go through XBOX_TO_NATIVE, which maps NULL to NULL. Scalars
 * pass straight through. Handles are tokens, not host HANDLEs, so they go
 * through bridge_resolve_handle / bridge_write_handle.
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── AvGetSavedDataAddress (ordinal 1, void) */
static void bridge_AvGetSavedDataAddress(void)
{
    g_eax = (uint32_t)xbox_AvGetSavedDataAddress();
}

/* ── AvSendTVEncoderOption (ordinal 2, 4 args) */
static void bridge_AvSendTVEncoderOption(void)
{
    xbox_AvSendTVEncoderOption(XBOX_TO_NATIVE(STACK_ARG(0)),
                               STACK_ARG(1), STACK_ARG(2),
                               (PULONG)XBOX_TO_NATIVE(STACK_ARG(3)));
    g_eax = 0;
}

/* ── ExFreePool (ordinal 17, 1 arg)
 * Was resolving to a DATA address before the kernel_data_va_for_ordinal fix,
 * so the title was calling into kernel data. Even after that it was an
 * unbridged no-op, which leaks every pool block the title ever frees.
 *
 * Deliberately does NOT call xbox_ExFreePool: that one HeapFrees its argument
 * on the host heap, but every pool block reaches the title as a guest VA from
 * xbox_HeapAlloc, so only the guest heap can release it. xbox_HeapFree answers
 * through the block table and takes the raw 32-bit guest VA. */
static void bridge_ExFreePool(void)
{
    xbox_HeapFree(STACK_ARG(0));
    g_eax = 0;
}

/* ── IoCreateDevice (ordinal 65, 6 args) ────────────────
 *
 * NTSTATUS IoCreateDevice(PDRIVER_OBJECT DriverObject,
 *                         ULONG DeviceExtensionSize,
 *                         PANSI_STRING DeviceName,
 *                         DEVICE_TYPE DeviceType,
 *                         BOOLEAN Exclusive,
 *                         PDEVICE_OBJECT *DeviceObject);
 *
 * Deliberately does NOT call xbox_IoCreateDevice. That one allocates from the
 * host heap and writes a 64-bit native pointer through this 4-byte guest
 * out-parameter -- the memory-model mismatch the NOT ROUTED note below
 * describes. The object has to live in guest memory because the caller
 * immediately walks it, so allocate it there.
 *
 * Leaving the ordinal unbridged was not the safe option it looked like: the
 * unresolved thunk returns 0, which is STATUS_SUCCESS, so the title proceeds
 * with a NULL device. Wreckless reads DeviceExtension from it and `rep stosd`s
 * DeviceExtensionSize bytes through the NULL, erasing the fake TIB at guest
 * VA 0. The crash surfaced in SetLastError, thousands of calls later.
 *
 * Layout is the Xbox DEVICE_OBJECT: Flags at 0x14, DeviceExtension at 0x18,
 * DeviceType at 0x1C, StackSize at 0x1D, header 0x38 bytes. Only the fields a
 * title actually reads are filled; the rest is zero, which is what a freshly
 * created device object holds anyway.
 */


/* ── KeCancelTimer (ordinal 97, 1 arg)
 * BOOLEAN KeCancelTimer(PKTIMER Timer) -- returns whether it was set. */
static void bridge_KeCancelTimer(void)
{
/* Both halves: the shadow object this runtime keeps, and the firing
     * table above, or a cancelled timer keeps calling its DPC. */
    uint32_t timer_va = STACK_ARG(0);
    int armed = xbox_kernel_cancel_timer(timer_va);
    g_eax = (uint32_t)xbox_KeCancelTimer(XBOX_TO_NATIVE(timer_va)) || armed;
}

/* ── KeDisconnectInterrupt (ordinal 100, 1 arg) */
static void bridge_KeDisconnectInterrupt(void)
{
    /* Whether it was connected */
    uint32_t i = STACK_ARG(0), vector = BRIDGE_MEM32(i + 8);
    g_eax = BRIDGE_MEM8(i + 0x10);
    if (g_eax) {
        if (vector < XBOX_MAX_VECTORS && g_connected_isr[vector] == i)
            g_connected_isr[vector] = 0;
        BRIDGE_MEM8(i + 0x10) = 0;
    }
}

/* ── KeSetBasePriorityThread (ordinal 143, 2 args) */
/* KeQueryBasePriorityThread (ordinal 124, 1 arg). The implementation has
 * existed in kernel_thread.c all along; only the bridge wrapper was
 * missing, so the thunk fell through to the fallback and returned 0. */
static int kthread_increment(uint32_t t);

static void bridge_KeQueryBasePriorityThread(void)
{
    if (bridge_thread_host(STACK_ARG(0), NULL)) {
        g_eax = (uint32_t)kthread_increment(STACK_ARG(0));
        return;
    }
    g_eax = (uint32_t)xbox_KeQueryBasePriorityThread(
        XBOX_TO_NATIVE(STACK_ARG(0)));
}

/* The stand-in KTHREAD keeps what the kernel keeps there, for the results below: +0x31 saturation, +0x32 priority,
 * +0x70 base priority, +0x73 boost disabled. Host priorities stay as bridge_apply_priority sets them. The block starts
 * zeroed, so 0 means a thread nothing has set yet: normal, 8. */
#define KTHREAD_BASE(t)       (BRIDGE_MEM8((t) + 0x70) ? (int)(int8_t)BRIDGE_MEM8((t) + 0x70) : 8)
#define KTHREAD_PRIORITY(t)   (BRIDGE_MEM8((t) + 0x32) ? (int)(int8_t)BRIDGE_MEM8((t) + 0x32) : 8)

static int kthread_increment(uint32_t t)
{
    int sat = (int)(int8_t)BRIDGE_MEM8(t + 0x31);
    return sat ? sat * 16 : KTHREAD_BASE(t) - 8;
}

static void bridge_KeSetBasePriorityThread(void)
{
    uint32_t obj = STACK_ARG(0);
    if (bridge_thread_host(obj, NULL)) {
        /* As the kernel: the old increment comes back. 16 or more either way saturates, and the base stays in the
         * normal range, 1 to 15. */
        LONG inc = (LONG)STACK_ARG(1);
        int base = 8 + inc, old = kthread_increment(obj);
        BRIDGE_MEM8(obj + 0x31) = (uint8_t)(inc >= 16 ? 1 : inc <= -16 ? -1 : 0);
        if (base >= 16) base = 15;
        if (base <= 0) base = 1;
        BRIDGE_MEM8(obj + 0x70) = (uint8_t)base;
        BRIDGE_MEM8(obj + 0x32) = (uint8_t)(inc >= 16 ? 15 : base);
        bridge_apply_priority("KeSetBasePriorityThread", obj, inc);
        g_eax = (uint32_t)old;
        return;
    }
    g_eax = (uint32_t)xbox_KeSetBasePriorityThread(
        XBOX_TO_NATIVE(obj), (LONG)STACK_ARG(1));
}

/* ── KeStallExecutionProcessor (ordinal 151, 1 arg) */
static void bridge_KeStallExecutionProcessor(void)
{
    xbox_KeStallExecutionProcessor(STACK_ARG(0));
    g_eax = 0;
}

/* ── MmLockUnlockBufferPages (ordinal 175, 3 args) */
static void bridge_MmLockUnlockBufferPages(void)
{
    xbox_MmLockUnlockBufferPages(XBOX_TO_NATIVE(STACK_ARG(0)),
                                 STACK_ARG(1), (BOOLEAN)STACK_ARG(2));
    g_eax = 0;
}

/* ── MmQueryAllocationSize (ordinal 180, 1 arg)
 *
 * Answered from the guest heap's block table, NOT from xbox_MmQueryAllocationSize.
 * That one calls VirtualQuery, which on a translated guest address reports the
 * size of the whole 64 MB guest mapping -- a confidently wrong answer where the
 * title expects the size of the block it allocated. This is the memory-model
 * check the parked-bridge list below asks for, done: the question is about
 * guest memory, so only the guest allocator can answer it.
 */
static void bridge_MmQueryAllocationSize(void)
{
    extern uint32_t xbox_ContiguousBlockSize(uint32_t addr);
    uint32_t va = STACK_ARG(0);
    uint32_t n = xbox_ContiguousBlockSize(va);   /* XPhysicalSize asks this too */
    g_eax = n ? n : xbox_HeapBlockSize(va);
}

/* ── NtCreateMutant (ordinal 192, 3 args) */
static void bridge_NtCreateMutant(void)
{
    uint32_t handle_va = STACK_ARG(0);
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING name;
    HANDLE h = NULL;
    NTSTATUS st;

    bridge_build_oa(STACK_ARG(1), &oa, &name);
    {
        WCHAR wname[256];
        const WCHAR *named = bridge_object_name(STACK_ARG(1), wname, 256);
        if (named) {
            h = CreateMutexW(NULL, (BOOL)(uint8_t)STACK_ARG(2), named);
            st = bridge_named_status(h);
        } else {
            st = xbox_NtCreateMutant(&h, STACK_ARG(1) ? &oa : NULL, (BOOLEAN)STACK_ARG(2));
        }
    }
    if (st >= 0 && handle_va) {
        NTSTATUS written = bridge_write_handle(handle_va, h);
        if (!NT_SUCCESS(written)) st = written;
    }
    g_eax = (uint32_t)st;
}

/* ── NtReleaseMutant (ordinal 221, 2 args)
 *
 * NTSTATUS NtReleaseMutant(HANDLE MutantHandle, PLONG PreviousCount);
 *
 * The partner of NtCreateMutant above, and routing one without the other is a
 * deadlock generator: the create succeeds, the release silently does nothing,
 * and the mutex stays held forever by a thread that has already exited.
 *
 * That is what the Xbox Dashboard's audio streaming did. Each ambient WAV gets
 * five events, a worker thread and a mutant; the worker finished, failed to
 * release, and terminated. The next attempt could not take the mutex, so the
 * dashboard reopened the same file and spawned another worker with another
 * 512 KB stack, forever -- visible only as a heap that climbed and a tick that
 * never returned.
 *
 * Memory model: a handle token in, an optional 4-byte LONG out through a guest
 * address. Nothing allocates, frees, or hands back a host pointer.
 */
static void bridge_NtReleaseMutant(void)
{
    /* The previous count, as the kernel: 1 free, 0 held once, less for each further hold. Ours asks before releasing,
     * while this thread holds it, so it cannot change. */
    uint32_t count_va = STACK_ARG(1);
    struct { LONG count; BOOLEAN owned, abandoned; } info = {0, 0, 0};
    if (count_va) bridge_nt_query("NtQueryMutant", STACK_ARG(0), &info, sizeof info);

    g_eax = bridge_handle_status(bridge_resolve_handle(STACK_ARG(0)),
                                 (uint32_t)xbox_NtReleaseMutant(bridge_resolve_handle(STACK_ARG(0)), NULL));
    if (count_va && g_eax == 0) BRIDGE_MEM32(count_va) = (uint32_t)info.count;
}

/* -- NtSuspendThread (ordinal 231, 2 args) --------------------------------
 *
 * NTSTATUS NtSuspendThread(HANDLE ThreadHandle, PULONG PreviousSuspendCount);
 *
 * The implementation was already here and only the dispatch entry was missing,
 * which is worse than an outright stub: the call returned 0, so a thread that
 * parked itself believed it had stopped and carried straight on. Wreckless
 * does that on a worker, and the "suspended" thread spun through 289 million
 * kernel calls while the title thought it was idle.
 */
static void bridge_NtSuspendThread(void)
{
    uint32_t count_va = STACK_ARG(1);

    g_eax = (uint32_t)xbox_NtSuspendThread(
        bridge_resolve_handle(STACK_ARG(0)),
        count_va ? (PULONG)XBOX_TO_NATIVE(count_va) : NULL);
}

/* ── NtResumeThread (ordinal 224, 2 args) */
static void bridge_NtResumeThread(void)
{
    g_eax = (uint32_t)xbox_NtResumeThread(
        bridge_resolve_handle(STACK_ARG(0)),
        (PULONG)XBOX_TO_NATIVE(STACK_ARG(1)));
}

/* ── ObfDereferenceObject (ordinal 250, fastcall: object in ecx)
 * Not STACK_ARG(0). Xbox uses __fastcall here, so the argument never reaches
 * the stack and the arg-size entry is 0. Reading it off the stack would
 * dereference whatever the caller happened to leave there. */

/* ══════════════════════════════════════════════════════════════════════════
 * More wrappers for xbox_* implementations that had no route. Same rule as
 * the block above: each one was checked against the memory-model bar --
 * reads/writes only happen at caller-supplied guest addresses, XBOX_TO_NATIVE
 * maps guest NULL to host NULL, and none of them allocates, frees, or hands
 * back a host pointer. Any exception is noted in place.
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── ObfReferenceObject (ordinal 251, fastcall: object in ecx)
 * Mirror of 250. Xbox uses __fastcall here too, so the argument never reaches
 * the stack and the arg-size entry is 0; reading it off the stack would
 * dereference whatever the caller happened to leave there. */

/* ── HalReadSMBusValue / HalWriteSMBusValue (ordinals 45 / 50, 4 args)
 *
 * The SMC side of the AV-pack / temperature queries titles make at init.
 * Unrouted they read 0 -- "no AV pack connected" -- which made D3D pick the
 * lowest common denominator display mode and 40C sensors read as 0C.
 *
 * HalReadSMBusValue writes one ULONG through a caller-supplied DataValue and
 * nothing else; HalWriteSMBusValue is all scalars. Both are the safe kind the
 * note below names.
 */
static void bridge_HalReadSMBusValue(void)
{
    uint32_t data_va = STACK_ARG(3);

    g_eax = (uint32_t)xbox_HalReadSMBusValue(
        (UCHAR)STACK_ARG(0), (UCHAR)STACK_ARG(1),
        (BOOLEAN)STACK_ARG(2), (PULONG)XBOX_TO_NATIVE(data_va));
}

static void bridge_HalWriteSMBusValue(void)
{
    g_eax = (uint32_t)xbox_HalWriteSMBusValue(
        (UCHAR)STACK_ARG(0), (UCHAR)STACK_ARG(1),
        (BOOLEAN)STACK_ARG(2), STACK_ARG(3));
}

/* ── HalReadWritePCISpace (ordinal 46, 6 args)
 *
 * PCI config space for the NV2A/southbridge; on reads the kernel serves a
 * zeroed buffer, so only a memset at a caller-supplied address happens. */
static void bridge_HalReadWritePCISpace(void)
{
    xbox_HalReadWritePCISpace(
        STACK_ARG(0), STACK_ARG(1), STACK_ARG(2),
        XBOX_TO_NATIVE(STACK_ARG(3)), STACK_ARG(4), (BOOLEAN)STACK_ARG(5));
    g_eax = 0;
}

/* ── HalRequestSoftwareInterrupt / HalClearSoftwareInterrupt (48 / 38, 1 arg)
 * DPC/APC delivery hooks with no interrupt-driven machinery behind them here;
 * both xbox_* versions are documented no-ops. */
static void bridge_HalRequestSoftwareInterrupt(void)
{
    xbox_HalRequestSoftwareInterrupt((KIRQL)g_ecx);   /* fastcall, in CL */
    g_eax = 0;
}

static void bridge_HalClearSoftwareInterrupt(void)
{
    xbox_HalClearSoftwareInterrupt((KIRQL)g_ecx);   /* fastcall, in CL */
    g_eax = 0;
}

/* ── ExSaveNonVolatileSetting (ordinal 29, 4 args)
 *
 * The write half of the EEPROM pair whose read side is already routed.
 * xbox_ExSaveNonVolatileSetting logs and returns success without ever reading
 * the Value buffer, so marshalling it across is cosmetic but keeps the write
 * path out of the unknowable-stub category. */
static void bridge_ExSaveNonVolatileSetting(void)
{
    g_eax = (uint32_t)xbox_ExSaveNonVolatileSetting(
        STACK_ARG(0), STACK_ARG(1), XBOX_TO_NATIVE(STACK_ARG(2)),
        STACK_ARG(3));
}

/* ── MmUnmapIoSpace (ordinal 183, 2 args)
 *
 * Deliberately does NOT call xbox_MmUnmapIoSpace: that one VirtualFrees its
 * argument, but the mapping coming back out of bridge_MmMapIoSpace was carved
 * from the guest heap, so only the guest heap can release it. xbox_HeapFree
 * matches against the block table and takes the raw 32-bit guest VA. */
static void bridge_MmUnmapIoSpace(void)
{
    if (STACK_ARG(0) < 0xFD000000u)   /* device space was never allocated */
        xbox_HeapFree(STACK_ARG(0) & ~0xFFFu);
    g_eax = 0;
}

/* ── IoDeleteDevice (ordinal 68, 1 arg)
 *
 * Same memory model as MmUnmapIoSpace: bridge_IoCreateDevice allocates the
 * device object on the guest heap, so deletion has to answer there too --
 * HeapFree (what xbox_IoDeleteDevice calls) would free a pointer that heap
 * never saw. */

/* ── KeQueryInterruptTime (ordinal 125, void)
 * Returns a 64-bit tick count; the caller reads it as edx:eax, so the high
 * half goes to g_edx exactly like the performance-counter bridges. */
static void bridge_KeQueryInterruptTime(void)
{
    uint64_t t = xbox_KeQueryInterruptTime();

    g_eax = (uint32_t)t;
    g_edx = (uint32_t)(t >> 32);
}

/* ── KeSaveFloatingPointState / KeRestoreFloatingPointState (142 / 139, 1 arg)
 *
 * D3D brackets its fixed-function transform code with these on hardware. Both
 * xbox_* implementations are successful no-ops -- Windows user mode preserves
 * FP state across context switches -- so marshalling is a pass-through of one
 * address. */
static void bridge_KeSaveFloatingPointState(void)
{
    /* As the kernel: the record is marked valid and the SSE control word kept, and the section runs with the default
     * one. x87 control is not touched: recompiled x87 code does not use the host's. +0 flags (4 valid), +1 IRQL,
     * +8 x87 control, +0xC MXCSR, +0x14 thread. */
    uint32_t state = STACK_ARG(0);
    BRIDGE_MEM8(state) = 4;
    BRIDGE_MEM8(state + 1) = (uint8_t)xbox_CurrentIrql();
    BRIDGE_MEM8(state + 2) = 0;
    BRIDGE_MEM16(state + 8) = 0x27F;
    BRIDGE_MEM32(state + 0xC) = _mm_getcsr();
    BRIDGE_MEM32(state + 0x10) = 0;
    BRIDGE_MEM32(state + 0x14) = bridge_thread_object(0xFFFFFFFEu);
    _mm_setcsr(0x1F80);
    g_eax = 0;
}

static void bridge_KeRestoreFloatingPointState(void)
{
    uint32_t state = STACK_ARG(0);
    if ((BRIDGE_MEM8(state) & 0xFC) != 4) {
        fprintf(stderr, "  [KERNEL] KeRestoreFloatingPointState: 0x%08X not saved (bug check 0xE7 on a console)\n", state);
        bridge_bugcheck_end(0xE7);
        return;
    }
    _mm_setcsr(BRIDGE_MEM32(state + 0xC));
    BRIDGE_MEM8(state) = 0;
    g_eax = 0;
}

/* ── NtCreateSemaphore (ordinal 193, 4 args)
 *
 * Handle-shaped twin of NtCreateEvent/NtCreateMutant. The native HANDLE is
 * 8 bytes and the guest slot is 4, so the create goes through a local and the
 * result lands via bridge_write_handle. */
static void bridge_NtCreateSemaphore(void)
{
    uint32_t handle_va = STACK_ARG(0);
    HANDLE h = NULL;
    NTSTATUS st;

    {
        WCHAR wname[256];
        const WCHAR *named = bridge_object_name(STACK_ARG(1), wname, 256);
        LONG init = (LONG)STACK_ARG(2), max = (LONG)STACK_ARG(3);
        if (named) {
            if (max <= 0 || init < 0 || init > max) {
                g_eax = 0xC000000Du;
                return;
            }
            h = CreateSemaphoreW(NULL, init, max, named);
            st = bridge_named_status(h);
        } else {
            st = xbox_NtCreateSemaphore(&h, XBOX_TO_NATIVE(STACK_ARG(1)), init, max);
        }
    }
    if (st >= 0 && handle_va) {
        NTSTATUS written = bridge_write_handle(handle_va, h);
        if (!NT_SUCCESS(written)) st = written;
    }
    g_eax = (uint32_t)st;
}

/* ── NtReleaseSemaphore (ordinal 222, 3 args)
 * Handle token in, LONG by value, optional 4-byte PLONG out. */
static void bridge_NtReleaseSemaphore(void)
{
    uint32_t count_va = STACK_ARG(2);

    g_eax = bridge_handle_status(bridge_resolve_handle(STACK_ARG(0)), (uint32_t)xbox_NtReleaseSemaphore(
        bridge_resolve_handle(STACK_ARG(0)), (LONG)STACK_ARG(1),
        count_va ? (PLONG)XBOX_TO_NATIVE(count_va) : NULL));
}

/* ── KeAlertThread (ordinal 93, 2 args)
 * Thread passed as an object; resolves through the handle table the way the
 * Nt* thread calls do, degrading to a pass-through for synthetic handles. */
static void bridge_KeAlertThread(void)
{
    g_eax = (uint32_t)xbox_KeAlertThread(
        bridge_resolve_handle(STACK_ARG(0)), (KPROCESSOR_MODE)STACK_ARG(1));
}

/* ── KeWaitForMultipleObjects (ordinal 158, 8 args)
 *
 * KeWaitForSingleObject is routed; the multiple-object sibling was not. The
 * Objects[] array holds dispatcher-object pointers, the same thing the single
 * wait takes, so each is resolved the same way: an event the title built
 * itself, then a KeInitialize* shadow, then a handle token. Resolving only as
 * a token passed a title-built KEVENT through as a raw host pointer, which the
 * POSIX wait then locked as a mutex and faulted on. */
#define BRIDGE_MAXIMUM_WAIT_OBJECTS 64

static void bridge_KeWaitForMultipleObjects(void)
{
    uint32_t count      = STACK_ARG(0);
    uint32_t objects_va = STACK_ARG(1);
    uint32_t wait_type  = STACK_ARG(2);
    uint32_t alertable  = STACK_ARG(5);   /* 3=WaitReason, 4=WaitMode */
    uint32_t timeout_va = STACK_ARG(6);
    HANDLE handles[BRIDGE_MAXIMUM_WAIT_OBJECTS];
    uint32_t objects[BRIDGE_MAXIMUM_WAIT_OBJECTS];
    int types[BRIDGE_MAXIMUM_WAIT_OBJECTS];   /* -1: not a title event */
    uint32_t i;

    if (count == 0) {
        g_eax = (uint32_t)STATUS_INVALID_PARAMETER;
        return;
    }
    if (count > BRIDGE_MAXIMUM_WAIT_OBJECTS)
        count = BRIDGE_MAXIMUM_WAIT_OBJECTS;
    for (i = 0; i < count; i++) {
        uint32_t obj = objects_va ? BRIDGE_MEM32(objects_va + i * 4) : 0;
        int type;
        HANDLE ge = ke_guest_event(obj, &type);

        objects[i] = obj;
        types[i]   = -1;
        if (ge) {
            /* Guest SignalState is the truth, as in the single wait. */
            if (BRIDGE_MEM32(obj + 4) == 0) {
                ResetEvent(ge);
                if (BRIDGE_MEM32(obj + 4) != 0)
                    SetEvent(ge);
            } else {
                SetEvent(ge);
            }
            handles[i] = ge;
            types[i]   = type;
            continue;
        }
        handles[i] = ke_shadow_lookup(obj);
        if (handles[i] && ke_header_is_sync_event(obj))
            types[i] = 1;
        if (!handles[i])
            handles[i] = bridge_resolve_handle(obj);
    }

    g_eax = (uint32_t)xbox_KeWaitForMultipleObjects(
        count, (PVOID *)handles, wait_type,
        STACK_ARG(3), (KPROCESSOR_MODE)STACK_ARG(4),
        (BOOLEAN)alertable, XBOX_TO_NATIVE(timeout_va),
        XBOX_TO_NATIVE(STACK_ARG(7)));

    /* A synchronization event that satisfied the wait is consumed: WaitAny
     * (1) returns its index, WaitAll (0) consumes every one. */
    if (wait_type == 0 && g_eax == 0) {
        for (i = 0; i < count; i++)
            if (types[i] == 1)
                ke_wait_satisfied(objects[i]);
    } else if (wait_type != 0 && g_eax < count && types[g_eax] == 1) {
        ke_wait_satisfied(objects[g_eax]);
    }
}

#undef BRIDGE_MAXIMUM_WAIT_OBJECTS

/* ── NtSetSystemTime (ordinal 228, 2 args)
 * Accepts the new time, ignores it, reports the old one through the optional
 * PreviousTime out-parameter (8-byte FILETIME at a guest address). */
static void bridge_NtSetSystemTime(void)
{
    uint32_t prev_va = STACK_ARG(1);

    g_eax = (uint32_t)xbox_NtSetSystemTime(
        XBOX_TO_NATIVE(STACK_ARG(0)),
        prev_va ? (PLARGE_INTEGER)XBOX_TO_NATIVE(prev_va) : NULL);
}

/* ── ObReferenceObjectByName (ordinal 247, 5 args)
 *
 * The ANSI_STRING is rebuilt by hand (native struct has a 64-bit Buffer at
 * offset 8, the guest one a 32-bit Buffer at offset 4) and the Object
 * out-parameter is filled through a local so the 8-byte NULL store lands in a
 * local instead of spilling past a 4-byte guest slot. */
static void bridge_ObReferenceObjectByName(void)
{
    uint32_t name_va   = STACK_ARG(0);
    uint32_t object_va = STACK_ARG(4);
    XBOX_ANSI_STRING name;
    PVOID object_local = NULL;

    if (!name_va) {
        g_eax = (uint32_t)STATUS_INVALID_PARAMETER;
        return;
    }
    {   /* one the title created */
        uint16_t n = BRIDGE_MEM16(name_va);
        char *text = (char *)malloc(n + 1u);
        struct ob_managed *m = NULL;
        if (text) {
            char *last;
            memcpy(text, XBOX_TO_NATIVE(BRIDGE_MEM32(name_va + 4)), n);
            text[n] = 0;
            last = strrchr(text, '\\');
            m = ob_find_name(last ? last + 1 : text);
            free(text);
        }
        if (m) {
            InterlockedIncrement((volatile LONG *)XBOX_TO_NATIVE(m->body - 0x10));
            if (object_va) BRIDGE_MEM32(object_va) = m->body;
            g_eax = 0;
            return;
        }
    }
    name.Length        = BRIDGE_MEM16(name_va);
    name.MaximumLength = BRIDGE_MEM16(name_va + 2);
    name.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(name_va + 4));

    g_eax = (uint32_t)xbox_ObReferenceObjectByName(
        &name, STACK_ARG(1), XBOX_TO_NATIVE(STACK_ARG(2)),
        XBOX_TO_NATIVE(STACK_ARG(3)), &object_local);
    if (object_va)
        BRIDGE_MEM32(object_va) = (uint32_t)(uintptr_t)object_local;
}

/* ── RtlInitUnicodeString (ordinal 290, 2 args)
 *
 * Mirror of the RtlInitAnsiString bridge: the guest UNICODE_STRING is
 * { USHORT Length; USHORT MaximumLength; 32-bit Buffer; } and the Buffer field
 * must carry the guest VA of the source, so the fields are written by hand
 * rather than letting the native xbox_* write a 64-bit host pointer into a
 * 4-byte guest slot. Lengths are in bytes (wide chars x2). */
static void bridge_RtlInitUnicodeString(void)
{
    uint32_t dest_va = STACK_ARG(0);
    uint32_t src_va  = STACK_ARG(1);

    if (!dest_va) {
        g_eax = 0;
        return;
    }
    if (src_va) {
        const uint16_t *wp = (const uint16_t *)XBOX_TO_NATIVE(src_va);
        size_t bytes = 0;

        while (wp[bytes / 2])
            bytes += sizeof(uint16_t);
        if (bytes > 0xFFFE)
            bytes = 0xFFFE;
        BRIDGE_MEM16(dest_va + 0) = (uint16_t)bytes;
        BRIDGE_MEM16(dest_va + 2) = (uint16_t)(bytes + sizeof(uint16_t));
        BRIDGE_MEM32(dest_va + 4) = src_va;
    } else {
        BRIDGE_MEM16(dest_va + 0) = 0;
        BRIDGE_MEM16(dest_va + 2) = 0;
        BRIDGE_MEM32(dest_va + 4) = 0;
    }
    g_eax = 0;
}

/* ── RtlTimeFieldsToTime (ordinal 304, 2 args)
 * Counterpart of the routed RtlTimeToTimeFields: reads a XBOX_TIME_FIELDS and
 * writes a LARGE_INTEGER, both at caller-supplied guest addresses. */
static void bridge_RtlTimeFieldsToTime(void)
{
    g_eax = (uint32_t)xbox_RtlTimeFieldsToTime(
        (PXBOX_TIME_FIELDS)XBOX_TO_NATIVE(STACK_ARG(0)),
        (PLARGE_INTEGER)XBOX_TO_NATIVE(STACK_ARG(1)));
}

/* ── PhyGetLinkState (ordinal 252, 1 arg) */
static void bridge_PhyGetLinkState(void)
{
    g_eax = (uint32_t)xbox_PhyGetLinkState((BOOLEAN)STACK_ARG(0));
}

/* ── PhyInitialize (ordinal 253, 2 args) */
static void bridge_PhyInitialize(void)
{
    g_eax = (uint32_t)xbox_PhyInitialize((BOOLEAN)STACK_ARG(0),
                                         XBOX_TO_NATIVE(STACK_ARG(1)));
}

/* ── RtlTimeToTimeFields (ordinal 305, 2 args) */
static void bridge_RtlTimeToTimeFields(void)
{
    xbox_RtlTimeToTimeFields(
        (PLARGE_INTEGER)XBOX_TO_NATIVE(STACK_ARG(0)),
        (PXBOX_TIME_FIELDS)XBOX_TO_NATIVE(STACK_ARG(1)));
    g_eax = 0;
}

/* ── XcSHAInit / XcSHAUpdate / XcSHAFinal (ordinals 335-337) */
static void bridge_XcSHAInit(void)
{
    xbox_XcSHAInit((PXBOX_SHA_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)));
    g_eax = 0;
}

static void bridge_XcSHAUpdate(void)
{
    xbox_XcSHAUpdate((PXBOX_SHA_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)),
                     (const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(1)),
                     STACK_ARG(2));
    g_eax = 0;
}

static void bridge_XcSHAFinal(void)
{
    xbox_XcSHAFinal((PXBOX_SHA_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)),
                    (UCHAR*)XBOX_TO_NATIVE(STACK_ARG(1)));
    g_eax = 0;
}

/* ── XcRC4Key / XcRC4Crypt (ordinals 338-339) */
static void bridge_XcRC4Key(void)
{
    xbox_XcRC4Key((PXBOX_RC4_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)),
                  STACK_ARG(1),
                  (const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(2)));
    g_eax = 0;
}

static void bridge_XcRC4Crypt(void)
{
    xbox_XcRC4Crypt((PXBOX_RC4_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)),
                    STACK_ARG(1),
                    (UCHAR*)XBOX_TO_NATIVE(STACK_ARG(2)));
    g_eax = 0;
}

/* ── XcHMAC (ordinal 340, 7 args) */
static void bridge_XcHMAC(void)
{
    xbox_XcHMAC((const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(0)), STACK_ARG(1),
                (const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(2)), STACK_ARG(3),
                (const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(4)), STACK_ARG(5),
                (UCHAR*)XBOX_TO_NATIVE(STACK_ARG(6)));
    g_eax = 0;
}

/* ── XcDESKeyParity (ordinal 346, 2 args) */
static void bridge_XcDESKeyParity(void)
{
    xbox_XcDESKeyParity((PUCHAR)XBOX_TO_NATIVE(STACK_ARG(0)), STACK_ARG(1));
    g_eax = 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * The rest of the xbox_* implementations that had no route. Each was checked
 * against the memory-model bar: reads or writes only happen at
 * caller-supplied guest addresses, XBOX_TO_NATIVE maps guest NULL to host
 * NULL, and nothing allocates, frees, or hands back a host pointer unless the
 * exception is stated in place. I/O Manager and HAL entries are mostly
 * documented stubs whose whole contract is a return value; the Xc* crypto
 * entries are stubs and guest-buffer operations.
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── DbgBreakPoint (ordinal 5, void) */
static void bridge_DbgBreakPoint(void)
{
    xbox_DbgBreakPoint();
    g_eax = 0;
}

/* ── AvSetSavedDataAddress (ordinal 4, 1 arg) */
static void bridge_AvSetSavedDataAddress(void)
{
    xbox_AvSetSavedDataAddress(STACK_ARG(0));
    g_eax = 0;
}

/* ── HalDisableSystemInterrupt (ordinal 39, 2 args) */
static void bridge_HalDisableSystemInterrupt(void)
{
    xbox_HalDisableSystemInterrupt(STACK_ARG(0), 0);
    g_eax = 0;
}

/* ── HalInitiateShutdown (ordinal 360, void)
 * The implementation calls ExitProcess, so this never actually returns. */
static void bridge_HalInitiateShutdown(void)
{
    xbox_HalInitiateShutdown();
    g_eax = 0;
}

/* ── HalIsResetOrShutdownPending (ordinal 358, void) */
static void bridge_HalIsResetOrShutdownPending(void)
{
    g_eax = (uint32_t)xbox_HalIsResetOrShutdownPending();
}

/* ── WRITE_PORT_BUFFER_ULONG / WRITE_PORT_BUFFER_USHORT (334 / 333, 3 args)
 * Port I/O stubs that ignore every argument, so pointers pass through without
 * translation -- nothing dereferences them. */
static void bridge_WRITE_PORT_BUFFER_ULONG(void)
{
    xbox_WRITE_PORT_BUFFER_ULONG((PULONG)(uintptr_t)STACK_ARG(0),
        (PULONG)(uintptr_t)STACK_ARG(1), STACK_ARG(2));
    g_eax = 0;
}

static void bridge_WRITE_PORT_BUFFER_USHORT(void)
{
    xbox_WRITE_PORT_BUFFER_USHORT((PUSHORT)(uintptr_t)STACK_ARG(0),
        (PUSHORT)(uintptr_t)STACK_ARG(1), STACK_ARG(2));
    g_eax = 0;
}

/* ── I/O Manager stubs (ordinals 61, 62, 69, 73, 74, 79, 81-87, 359)
 *
 * The Xbox I/O manager is used internally by the XDK libraries we replace, so
 * these are all stubs whose contract is a return value plus occasional writes
 * to caller-supplied guest structures (IRP, IO_STATUS_BLOCK, symlink table).
 * None of them hands back a pointer the title dereferences.
 *
 * IofCallDriver / IofCompleteRequest are __fastcall on Xbox, so their
 * arguments live in ecx/edx, never on the stack -- the arg-size entries are 0
 * and reading STACK_ARG would dereference whatever the caller left there.
 */


static void bridge_IoDeleteSymbolicLink(void)
{
    uint32_t name_va = STACK_ARG(0);
    XBOX_ANSI_STRING name;

    if (!name_va) {
        g_eax = 0xC000000Du;   /* STATUS_INVALID_PARAMETER */
        return;
    }
    name.Length        = BRIDGE_MEM16(name_va + 0);
    name.MaximumLength = BRIDGE_MEM16(name_va + 2);
    name.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(name_va + 4));

    g_eax = (uint32_t)xbox_IoDeleteSymbolicLink(&name);
}




static void bridge_IoSetIoCompletion(void)
{
    g_eax = (uint32_t)xbox_IoSetIoCompletion(
        XBOX_TO_NATIVE(STACK_ARG(0)), XBOX_TO_NATIVE(STACK_ARG(1)),
        XBOX_TO_NATIVE(STACK_ARG(2)), (NTSTATUS)STACK_ARG(3),
        (ULONG_PTR)STACK_ARG(4));
}








/* ── MmLockUnlockPhysicalPage (ordinal 176, 2 args)
 * Page locking is a no-op in user mode; the physical address is a value,
 * never dereferenced. */
static void bridge_MmLockUnlockPhysicalPage(void)
{
    xbox_MmLockUnlockPhysicalPage(STACK_ARG(0), (BOOLEAN)STACK_ARG(1));
    g_eax = 0;
}

/* ── MmFreeSystemMemory (ordinal 172, 2 args)
 * Guest-heap twin of the allocator above; nothing on the host heap has ever
 * seen this address. */
static void bridge_MmFreeSystemMemory(void)
{
    xbox_HeapFree(STACK_ARG(0));
    g_eax = 0;
}

/* ── MmCreateKernelStack / MmDeleteKernelStack (169 / 170, 2 args)
 *
 * Same memory model as the pair above. The Xbox convention is that
 * MmCreateKernelStack returns the TOP of the stack and the delete call
 * receives (StackBase=top, StackLimit=base). Both halves of that come from the
 * guest heap here: create returns base+size, delete frees the base (arg 1). */
static void bridge_MmCreateKernelStack(void)
{
    /* The top of the stack; whole pages, as the kernel */
    uint32_t size = (STACK_ARG(0) + 0xFFF) & ~0xFFFu;
    uint32_t base_va = xbox_HeapAlloc(size, 4096);

    g_eax = base_va ? base_va + size : 0;
}

static void bridge_MmDeleteKernelStack(void)
{
    /* Top and size: this used to free the size, as if it were an address */
    xbox_HeapFree(STACK_ARG(0) - ((STACK_ARG(1) + 0xFFF) & ~0xFFFu));
    g_eax = 0;
}

/* ── Xc* remaining crypto (ordinals 341-345, 347-351)
 *
 * Split into two kinds, both of which clear the memory-model bar:
 *  - the public-key / DES / ModExp entries are documented stubs that ignore
 *    their arguments (no Xbox Live, no on-console key derivation), so the
 *    pointers pass through without being dereferenced;
 *  - XcBlockCrypt/XcKeyTable/XcCryptService/XcUpdateCrypto are the same shape.
 * XcVerifyPKCS1Signature is worth singling out: it returns TRUE so that
 * signature checks succeed instead of rebooting the dashboard. */
static void bridge_XcPKGetKeyLen(void)
{
    g_eax = (uint32_t)xbox_XcPKGetKeyLen(XBOX_TO_NATIVE(STACK_ARG(0)));
}

static void bridge_XcPKDecPrivate(void)
{
    g_eax = (uint32_t)xbox_XcPKDecPrivate(XBOX_TO_NATIVE(STACK_ARG(0)),
        XBOX_TO_NATIVE(STACK_ARG(1)), XBOX_TO_NATIVE(STACK_ARG(2)));
}

static void bridge_XcPKEncPublic(void)
{
    g_eax = (uint32_t)xbox_XcPKEncPublic(XBOX_TO_NATIVE(STACK_ARG(0)),
        XBOX_TO_NATIVE(STACK_ARG(1)), XBOX_TO_NATIVE(STACK_ARG(2)));
}

static void bridge_XcVerifyPKCS1Signature(void)
{
    /* A key that is not a real public key here means the kernel's own (XePublicKeyData), which we deliberately do not
     * hold: nothing can be checked against it, so it passes, as it always did. A title's own key is checked for real. */
    if (BRIDGE_MEM32(STACK_ARG(1)) != 0x31415352u) {
        g_eax = 1;
        return;
    }
    g_eax = (uint32_t)xbox_XcVerifyPKCS1Signature(
        XBOX_TO_NATIVE(STACK_ARG(0)), XBOX_TO_NATIVE(STACK_ARG(1)),
        XBOX_TO_NATIVE(STACK_ARG(2)));
}

static void bridge_XcModExp(void)
{
    g_eax = (uint32_t)xbox_XcModExp((PULONG)XBOX_TO_NATIVE(STACK_ARG(0)),
        (PULONG)XBOX_TO_NATIVE(STACK_ARG(1)),
        (PULONG)XBOX_TO_NATIVE(STACK_ARG(2)),
        (PULONG)XBOX_TO_NATIVE(STACK_ARG(3)), STACK_ARG(4));
}

static void bridge_XcKeyTable(void)
{
    xbox_XcKeyTable(STACK_ARG(0), XBOX_TO_NATIVE(STACK_ARG(1)),
        (const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(2)));
    g_eax = 0;
}

static void bridge_XcBlockCrypt(void)
{
    xbox_XcBlockCrypt(STACK_ARG(0), XBOX_TO_NATIVE(STACK_ARG(1)),
        XBOX_TO_NATIVE(STACK_ARG(2)), XBOX_TO_NATIVE(STACK_ARG(3)),
        STACK_ARG(4));
    g_eax = 0;
}

static void bridge_XcBlockCryptCBC(void)
{
    xbox_XcBlockCryptCBC(STACK_ARG(0), STACK_ARG(1),
        XBOX_TO_NATIVE(STACK_ARG(2)), XBOX_TO_NATIVE(STACK_ARG(3)),
        XBOX_TO_NATIVE(STACK_ARG(4)), STACK_ARG(5),
        XBOX_TO_NATIVE(STACK_ARG(6)));
    g_eax = 0;
}

static void bridge_XcCryptService(void)
{
    xbox_XcCryptService(STACK_ARG(0), XBOX_TO_NATIVE(STACK_ARG(1)));
    g_eax = 0;
}


/* ── RtlRip (ordinal 352, 3 args)
 * The CRT rip/assert path. All three pointers are guest C strings, so each is
 * translated; nothing is written. */
static void bridge_RtlRip(void)
{
    xbox_RtlRip((PCHAR)XBOX_TO_NATIVE(STACK_ARG(0)),
        (PCHAR)XBOX_TO_NATIVE(STACK_ARG(1)),
        (PCHAR)XBOX_TO_NATIVE(STACK_ARG(2)));
    g_eax = 0;
}

/* ── RtlAnsiStringToUnicodeString (ordinal 260, 3 args) ──────────────────
 *
 * NOT a call into xbox_RtlAnsiStringToUnicodeString. That one HeapAllocs on
 * the host heap and stores the resulting 64-bit host pointer into the guest
 * string's 4-byte Buffer field -- the memory-model mismatch. This bridge does
 * the whole conversion by hand:
 *
 *  - Source fields are read from the guest struct (Buffer at offset 4);
 *  - the destination stays wherever the title put it (non-allocating), or is
 *    carved from the guest heap (allocating) and its guest VA stored at
 *    offset 4;
 *  - conversion writes only at guest addresses.
 */
static void bridge_RtlAnsiStringToUnicodeString(void)
{
    uint32_t dst_va  = STACK_ARG(0);
    uint32_t src_va  = STACK_ARG(1);
    uint32_t do_alloc = STACK_ARG(2);
    uint32_t s_len, s_buf_va, d_max, d_buf_va = 0;
    uint32_t unicode_bytes;
    int result;

    if (!dst_va || !src_va) {
        g_eax = 0xC000000Du;   /* STATUS_INVALID_PARAMETER */
        return;
    }
    s_len   = BRIDGE_MEM16(src_va + 0);
    s_buf_va = BRIDGE_MEM32(src_va + 4);
    if (!s_buf_va) {
        g_eax = 0xC000000Du;
        return;
    }
    unicode_bytes = (uint32_t)(s_len + 1) * sizeof(WCHAR);

    d_max = BRIDGE_MEM16(dst_va + 2);
    if (do_alloc) {
        d_buf_va = xbox_HeapAlloc(unicode_bytes, 16);
        if (!d_buf_va) {
            g_eax = 0xC0000017u;   /* STATUS_NO_MEMORY */
            return;
        }
        BRIDGE_MEM16(dst_va + 2) = (uint16_t)unicode_bytes;
        BRIDGE_MEM32(dst_va + 4) = d_buf_va;
    } else {
        if (d_max < unicode_bytes) {
            g_eax = 0xC0000205u;   /* STATUS_BUFFER_OVERFLOW */
            return;
        }
        d_buf_va = BRIDGE_MEM32(dst_va + 4);
        if (!d_buf_va) {
            g_eax = 0xC000000Du;
            return;
        }
    }

    result = l1_mb2wc((const char*)XBOX_TO_NATIVE(s_buf_va), (int)s_len,
        (WCHAR*)XBOX_TO_NATIVE(d_buf_va), (int)(unicode_bytes / sizeof(WCHAR)));
    if (result > 0) {
        BRIDGE_MEM16(dst_va + 0) = (uint16_t)(result * sizeof(WCHAR));
        ((WCHAR*)XBOX_TO_NATIVE(d_buf_va))[result] = 0;
        g_eax = 0;
    } else {
        g_eax = 0xC0000001u;   /* STATUS_UNSUCCESSFUL */
    }
}

/* ── RtlUnicodeStringToAnsiString (ordinal 308, 3 args)
 * Mirror of the ANSI→Unicode bridge above. */
static void bridge_RtlUnicodeStringToAnsiString(void)
{
    uint32_t dst_va  = STACK_ARG(0);
    uint32_t src_va  = STACK_ARG(1);
    uint32_t do_alloc = STACK_ARG(2);
    uint32_t s_len, s_buf_va, d_max, d_buf_va = 0;
    uint32_t ansi_bytes;
    int result;

    if (!dst_va || !src_va) {
        g_eax = 0xC000000Du;
        return;
    }
    s_len   = BRIDGE_MEM16(src_va + 0);
    s_buf_va = BRIDGE_MEM32(src_va + 4);
    if (!s_buf_va) {
        g_eax = 0xC000000Du;
        return;
    }
    ansi_bytes = s_len / sizeof(WCHAR) + 1;

    d_max = BRIDGE_MEM16(dst_va + 2);
    if (do_alloc) {
        d_buf_va = xbox_HeapAlloc(ansi_bytes, 16);
        if (!d_buf_va) {
            g_eax = 0xC0000017u;
            return;
        }
        BRIDGE_MEM16(dst_va + 2) = (uint16_t)ansi_bytes;
        BRIDGE_MEM32(dst_va + 4) = d_buf_va;
    } else {
        if (d_max < ansi_bytes) {
            g_eax = 0xC0000205u;
            return;
        }
        d_buf_va = BRIDGE_MEM32(dst_va + 4);
        if (!d_buf_va) {
            g_eax = 0xC000000Du;
            return;
        }
    }

    result = l1_wc2mb((const WCHAR*)XBOX_TO_NATIVE(s_buf_va), (int)(s_len / sizeof(WCHAR)),
        (char*)XBOX_TO_NATIVE(d_buf_va), (int)ansi_bytes);
    if (result > 0) {
        BRIDGE_MEM16(dst_va + 0) = (uint16_t)result;
        if ((uint16_t)result < d_max)
            ((char*)XBOX_TO_NATIVE(d_buf_va))[result] = 0;
        g_eax = 0;
    } else {
        g_eax = 0xC0000001u;
    }
}

/* ── NtDuplicateObject (ordinal 197, 3 args)
 *
 * Xbox NtDuplicateObject has one process, and the handle table is ours: a
 * by-value source token in, a 4-byte guest slot for the duplicated target.
 * The source resolves through the table, DuplicateHandle produces a real
 * native handle, and the target lands via bridge_write_handle as a fresh
 * token -- same shape as NtCreateEvent. DUPLICATE_CLOSE_SOURCE also closes
 * the native handle behind the source token, which is why the source token is
 * NOT consumed here (the OS handle it wraps is gone after this call). */
static uint32_t ob_make_handle(uint32_t handle_va, uint32_t body);
static void ob_close_handle(uint32_t body);
static HANDLE bridge_take_handle(uint32_t token);

static void bridge_NtDuplicateObject(void)
{
    uint32_t target_va = STACK_ARG(1);
    HANDLE src = bridge_resolve_handle(STACK_ARG(0));
    HANDLE dup = NULL;
    DWORD opts = 0;

    if (!target_va) {
        g_eax = 0xC000000Du;   /* STATUS_INVALID_PARAMETER */
        return;
    }
    {   /* One the title created: another handle to the object, the old closed if asked */
        uint32_t own = ob_from_handle(STACK_ARG(0));
        if (own) {
            InterlockedIncrement((volatile LONG *)XBOX_TO_NATIVE(own - 0x10));
            if (STACK_ARG(2) & 1) {
                bridge_take_handle(STACK_ARG(0));
                ob_close_handle(own);
            }
            g_eax = ob_make_handle(target_va, own);
            return;
        }
    }
    if (STACK_ARG(2) & 0x1) opts |= DUPLICATE_CLOSE_SOURCE;
    if (STACK_ARG(2) & 0x2) opts |= DUPLICATE_SAME_ACCESS;

    if (!DuplicateHandle(GetCurrentProcess(), src, GetCurrentProcess(),
                         &dup, 0, FALSE, opts)) {
        static int logged = 0;
        if (logged++ < 8) {
            fprintf(stderr, "  [KERNEL] NtDuplicateObject: token=0x%08X "
                    "handle=%p failed (error %lu)\n",
                    STACK_ARG(0), src, (unsigned long)GetLastError());
            fflush(stderr);
        }
        BRIDGE_MEM32(target_va) = 0;   /* as the kernel */
        g_eax = 0xC0000008u;           /* STATUS_INVALID_HANDLE */
        return;
    }
    g_eax = (uint32_t)bridge_write_handle(target_va, dup);
}

/* ── Dispatch table: ordinal → bridge function + stack arg bytes ── */

typedef void (*bridge_func_t)(void);

/**
 * stdcall arg byte count for each kernel ordinal.
 * On x86 stdcall, the callee cleans (ret N). Our bridges must do the same
 * via g_esp += N after execution so the simulated stack stays balanced.
 *
 * Special cases:
 *   - KfRaiseIrql/KfLowerIrql: fastcall (arg in ecx), 0 stack bytes
 *   - KeSetTimer: DueTime is LARGE_INTEGER (8 bytes on stack) + Timer + Dpc
 */
/* ---- Section B: Rtl* bridge functions (ordinals 261-321) ----
 * Drawn from bridges_Rtl.c and spliced here at byte level. Macro and status
 * definitions inside remain #ifndef-guarded so the block still compiles
 * standalone. Routing lives in the two dispatch tables below. */
/* NTSTATUS values kernel.h does not define. */
#ifndef STATUS_BUFFER_TOO_SMALL
#define STATUS_BUFFER_TOO_SMALL    ((NTSTATUS)0xC0000023L)
#endif
#ifndef STATUS_INTEGER_OVERFLOW
#define STATUS_INTEGER_OVERFLOW    ((NTSTATUS)0xC0000095L)
#endif

/* ── Guest string helpers ─────────────────────────────────── */

/* Byte length (in UTF-16 bytes) of the guest C string at src_va, capped so the
 * caller can always add a terminator without overflowing USHORT Length. */
static uint32_t bridge_guest_wcslen_bytes(uint32_t src_va)
{
    const uint16_t *wp = (const uint16_t *)XBOX_TO_NATIVE(src_va);
    uint32_t bytes = 0;

    while (wp[bytes / 2]) {
        bytes += sizeof(uint16_t);
        if (bytes > 0xFFFE)
            return 0xFFFE;
    }
    return bytes;
}

/* ── Rtl Append (ordinals 261, 262, 263) ────────────────────
 * Each appends a source to a destination XBOX_*_STRING, honouring the
 * destination MaximumLength; the source bump and the destination length both
 * live in guest memory. Returns STATUS_SUCCESS or STATUS_BUFFER_TOO_SMALL. */

/* 261: NTSTATUS RtlAppendStringToString(PXBOX_ANSI_STRING, PXBOX_ANSI_STRING) */
static void bridge_RtlAppendStringToString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint16_t d_len = 0, d_max = 0, s_len = 0;
    uint32_t d_buf = 0, s_buf = 0;

    if (!dst_va || !src_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    d_len = BRIDGE_MEM16(dst_va + 0);
    d_max = BRIDGE_MEM16(dst_va + 2);
    d_buf = BRIDGE_MEM32(dst_va + 4);
    s_len = BRIDGE_MEM16(src_va + 0);
    s_buf = BRIDGE_MEM32(src_va + 4);
    if (!d_buf || !s_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if ((uint32_t)d_len + s_len > d_max) { g_eax = (uint32_t)STATUS_BUFFER_TOO_SMALL; return; }
    memcpy((char *)XBOX_TO_NATIVE(d_buf) + d_len, XBOX_TO_NATIVE(s_buf), s_len);
    BRIDGE_MEM16(dst_va + 0) = (uint16_t)(d_len + s_len);
    g_eax = 0;   /* STATUS_SUCCESS */
}

/* 262: NTSTATUS RtlAppendUnicodeStringToString(PXBOX_UNICODE_STRING, PXBOX_UNICODE_STRING) */
static void bridge_RtlAppendUnicodeStringToString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint16_t d_len = 0, d_max = 0, s_len = 0;
    uint32_t d_buf = 0, s_buf = 0;

    if (!dst_va || !src_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    d_len = BRIDGE_MEM16(dst_va + 0);
    d_max = BRIDGE_MEM16(dst_va + 2);
    d_buf = BRIDGE_MEM32(dst_va + 4);
    s_len = BRIDGE_MEM16(src_va + 0);
    s_buf = BRIDGE_MEM32(src_va + 4);
    if (!d_buf || !s_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if ((uint32_t)d_len + s_len > d_max) { g_eax = (uint32_t)STATUS_BUFFER_TOO_SMALL; return; }
    memcpy((char *)XBOX_TO_NATIVE(d_buf) + d_len, XBOX_TO_NATIVE(s_buf), s_len);
    BRIDGE_MEM16(dst_va + 0) = (uint16_t)(d_len + s_len);
    if ((uint32_t)d_len + s_len < d_max) BRIDGE_MEM16(d_buf + ((uint32_t)d_len & ~1u) + s_len) = 0;   /* as the kernel */
    g_eax = 0;   /* STATUS_SUCCESS */
}

/* 263: NTSTATUS RtlAppendUnicodeToString(PXBOX_UNICODE_STRING, PCWSTR) */
static void bridge_RtlAppendUnicodeToString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint16_t d_len = 0, d_max = 0;
    uint32_t d_buf = 0, add_bytes;

    if (!dst_va || !src_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    d_len = BRIDGE_MEM16(dst_va + 0);
    d_max = BRIDGE_MEM16(dst_va + 2);
    d_buf = BRIDGE_MEM32(dst_va + 4);
    if (!d_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    add_bytes = bridge_guest_wcslen_bytes(src_va);
    if ((uint32_t)d_len + add_bytes > d_max) { g_eax = (uint32_t)STATUS_BUFFER_TOO_SMALL; return; }
    memcpy((char *)XBOX_TO_NATIVE(d_buf) + d_len, XBOX_TO_NATIVE(src_va), add_bytes);
    BRIDGE_MEM16(dst_va + 0) = (uint16_t)(d_len + add_bytes);
    if ((uint32_t)d_len + add_bytes < d_max) BRIDGE_MEM16(d_buf + ((uint32_t)d_len & ~1u) + add_bytes) = 0;   /* as the kernel */
    g_eax = 0;   /* STATUS_SUCCESS */
}

/* ── Rtl Compare (ordinals 270, 271, 280) ───────────────────
 * <0 / 0 / >0 against the string bodies (not just the prefix), matching the
 * NT RtlCompare* contract: compare up to the common length, then lengths. */

/* 270: LONG RtlCompareString(PXBOX_ANSI_STRING, PXBOX_ANSI_STRING, BOOLEAN) */
static void bridge_RtlCompareString(void)
{
    uint32_t s1_va = STACK_ARG(0);
    uint32_t s2_va = STACK_ARG(1);
    uint32_t nocase = STACK_ARG(2);
    uint32_t l1, l2, common, i;
    const uint8_t *p1, *p2;
    int diff;

    if (!s1_va || !s2_va) { g_eax = 0; return; }
    l1 = BRIDGE_MEM16(s1_va + 0);
    l2 = BRIDGE_MEM16(s2_va + 0);
    p1 = (const uint8_t *)XBOX_TO_NATIVE(BRIDGE_MEM32(s1_va + 4));
    p2 = (const uint8_t *)XBOX_TO_NATIVE(BRIDGE_MEM32(s2_va + 4));
    if (!p1 || !p2) { g_eax = 0; return; }

    common = l1 < l2 ? l1 : l2;
    diff = 0;
    for (i = 0; i < common; i++) {
        int a = p1[i], b = p2[i];
        if (nocase) { a = l1_lower(a); b = l1_lower(b); }
        if (a != b) { diff = a - b; break; }
    }
    if (!diff)
        diff = (int)l1 - (int)l2;
    g_eax = (uint32_t)(int32_t)diff;
}

#include "kernel_case.h"

static uint16_t case_map(const KernelCaseRun *r, int n, uint16_t c)
{
    for (int i = 0; i < n; i++)
        if (c >= r[i].first && c <= r[i].last && ((c - r[i].first) % r[i].step) == 0)
            return (uint16_t)(c + r[i].delta);
    return c;
}
static uint16_t case_upper(uint16_t c)
{
    if (c < 0x80) return c >= 'a' && c <= 'z' ? (uint16_t)(c - 0x20) : c;
    return case_map(kCaseUpper, (int)(sizeof kCaseUpper / sizeof kCaseUpper[0]), c);
}
static uint16_t case_lower(uint16_t c)
{
    if (c < 0x80) return c >= 'A' && c <= 'Z' ? (uint16_t)(c + 0x20) : c;
    return case_map(kCaseLower, (int)(sizeof kCaseLower / sizeof kCaseLower[0]), c);
}

/* As the kernel: base 0 is 10 unless "0x", "0o" or "0b" follows (lower case, and no bare leading-zero octal); other
 * bases only 2, 8, 10, 16; anything at or below a space is skipped first; no overflow check, the value wraps. */
static int rtl_int_base(uint32_t base) { return base == 2 || base == 8 || base == 10 || base == 16; }

/* 271: LONG RtlCompareUnicodeString(PXBOX_UNICODE_STRING, PXBOX_UNICODE_STRING, BOOLEAN) */
static void bridge_RtlCompareUnicodeString(void)
{
    uint32_t s1_va = STACK_ARG(0);
    uint32_t s2_va = STACK_ARG(1);
    uint32_t nocase = STACK_ARG(2);
    uint32_t b1, b2, common, i;
    const uint16_t *p1, *p2;
    int diff;

    if (!s1_va || !s2_va) { g_eax = 0; return; }
    b1 = BRIDGE_MEM16(s1_va + 0);
    b2 = BRIDGE_MEM16(s2_va + 0);
    p1 = (const uint16_t *)XBOX_TO_NATIVE(BRIDGE_MEM32(s1_va + 4));
    p2 = (const uint16_t *)XBOX_TO_NATIVE(BRIDGE_MEM32(s2_va + 4));
    if (!p1 || !p2) { g_eax = 0; return; }

    common = b1 < b2 ? b1 : b2;
    diff = 0;
    for (i = 0; i < common / 2; i++) {
        int a = p1[i], b = p2[i];
        if (nocase) { a = case_lower((uint16_t)a); b = case_lower((uint16_t)b); }
        if (a != b) { diff = a - b; break; }
    }
    if (!diff)
        diff = (int)b1 - (int)b2;
    g_eax = (uint32_t)(int32_t)diff;
}

/* 280: BOOLEAN RtlEqualUnicodeString(PXBOX_UNICODE_STRING, PXBOX_UNICODE_STRING, BOOLEAN) */
static void bridge_RtlEqualUnicodeString(void)
{
    uint32_t s1_va = STACK_ARG(0);
    uint32_t s2_va = STACK_ARG(1);
    uint32_t nocase = STACK_ARG(2);
    uint32_t b1, b2, i;
    const uint16_t *p1, *p2;

    if (!s1_va || !s2_va) { g_eax = 0; return; }
    b1 = BRIDGE_MEM16(s1_va + 0);
    b2 = BRIDGE_MEM16(s2_va + 0);
    if (b1 != b2) { g_eax = 0; return; }
    p1 = (const uint16_t *)XBOX_TO_NATIVE(BRIDGE_MEM32(s1_va + 4));
    p2 = (const uint16_t *)XBOX_TO_NATIVE(BRIDGE_MEM32(s2_va + 4));
    if ((!p1 || !p2) && b1) { g_eax = 0; return; }

    for (i = 0; i < b1 / 2; i++) {
        int a = p1[i], b = p2[i];
        if (nocase) {
            a = case_lower((uint16_t)a);
            b = case_lower((uint16_t)b);
        }
        if (a != b) { g_eax = 0; return; }
    }
    g_eax = 1;
}

/* ── Rtl Copy (ordinals 272, 273, 317) ────────────────────── */

/* 272: VOID RtlCopyString(PXBOX_ANSI_STRING Dest, PXBOX_ANSI_STRING Src) */
static void bridge_RtlCopyString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint16_t d_max = 0, s_len = 0;
    uint32_t d_buf = 0, s_buf = 0, copy;

    g_eax = 0;
    if (!dst_va) return;
    if (!src_va) {
        BRIDGE_MEM16(dst_va) = 0;
        return;
    }
    d_max = BRIDGE_MEM16(dst_va + 2);
    d_buf = BRIDGE_MEM32(dst_va + 4);
    s_len = BRIDGE_MEM16(src_va + 0);
    s_buf = BRIDGE_MEM32(src_va + 4);
    copy = s_len < d_max ? s_len : d_max;
    if (copy && d_buf && s_buf)
        memcpy(XBOX_TO_NATIVE(d_buf), XBOX_TO_NATIVE(s_buf), copy);
    BRIDGE_MEM16(dst_va + 0) = (uint16_t)copy;
    g_eax = 0;
}

/* 273: VOID RtlCopyUnicodeString(PXBOX_UNICODE_STRING Dest, PXBOX_UNICODE_STRING Src) */
static void bridge_RtlCopyUnicodeString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint16_t d_max = 0, s_len = 0;
    uint32_t d_buf = 0, s_buf = 0, copy;

    if (!dst_va || !src_va) { g_eax = 0; return; }
    d_max = BRIDGE_MEM16(dst_va + 2);
    d_buf = BRIDGE_MEM32(dst_va + 4);
    s_len = BRIDGE_MEM16(src_va + 0);
    s_buf = BRIDGE_MEM32(src_va + 4);
    copy = s_len < d_max ? s_len : d_max;
    if (copy && d_buf && s_buf)
        memcpy(XBOX_TO_NATIVE(d_buf), XBOX_TO_NATIVE(s_buf), copy);
    BRIDGE_MEM16(dst_va + 0) = (uint16_t)copy;
    g_eax = 0;
}

/* 317: VOID RtlUpperString(PXBOX_ANSI_STRING Dest, PXBOX_ANSI_STRING Src) */
static void bridge_RtlUpperString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint16_t d_max = 0, s_len = 0;
    uint32_t d_buf = 0, s_buf = 0, copy, i;
    const uint8_t *sp;
    uint8_t *dp;

    if (!dst_va || !src_va) { g_eax = 0; return; }
    d_max = BRIDGE_MEM16(dst_va + 2);
    d_buf = BRIDGE_MEM32(dst_va + 4);
    s_len = BRIDGE_MEM16(src_va + 0);
    s_buf = BRIDGE_MEM32(src_va + 4);
    copy = s_len < d_max ? s_len : d_max;
    if (copy && d_buf && s_buf) {
        sp = (const uint8_t *)XBOX_TO_NATIVE(s_buf);
        dp = (uint8_t *)XBOX_TO_NATIVE(d_buf);
        for (i = 0; i < copy; i++)
            dp[i] = (uint8_t)l1_upper(sp[i]);
    }
    BRIDGE_MEM16(dst_va + 0) = (uint16_t)copy;
    g_eax = 0;
}

/* ── Rtl Create / Free string (ordinals 274, 287) ─────────── */

/* 274: BOOLEAN RtlCreateUnicodeString(PXBOX_UNICODE_STRING Dest, PCWSTR Src)
 * The buffer is carved from the guest heap so the Buffer field stays a guest
 * VA the title can dereference.  Returns TRUE on success. */
static void bridge_RtlCreateUnicodeString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint32_t bytes, buf_va;

    if (!dst_va || !src_va) { g_eax = 0; return; }
    bytes = bridge_guest_wcslen_bytes(src_va);
    buf_va = xbox_HeapAlloc(bytes + sizeof(uint16_t), 16);
    if (!buf_va) { g_eax = 0; return; }
    memcpy(XBOX_TO_NATIVE(buf_va), XBOX_TO_NATIVE(src_va), bytes);
    ((uint16_t *)XBOX_TO_NATIVE(buf_va))[bytes / 2] = 0;
    BRIDGE_MEM16(dst_va + 0) = (uint16_t)bytes;
    BRIDGE_MEM16(dst_va + 2) = (uint16_t)(bytes + sizeof(uint16_t));
    BRIDGE_MEM32(dst_va + 4) = buf_va;
    g_eax = 1;
}

/* 287: VOID RtlFreeUnicodeString(PXBOX_UNICODE_STRING Str) */
static void bridge_RtlFreeUnicodeString(void)
{
    uint32_t str_va = STACK_ARG(0);
    uint32_t buf_va;

    if (!str_va) { g_eax = 0; return; }
    buf_va = BRIDGE_MEM32(str_va + 4);
    if (buf_va)
        xbox_HeapFree(buf_va);
    BRIDGE_MEM16(str_va + 0) = 0;
    BRIDGE_MEM16(str_va + 2) = 0;
    BRIDGE_MEM32(str_va + 4) = 0;
    g_eax = 0;
}

/* ── Rtl Control characters (ordinals 275, 296, 313, 316) ── */

/* 275: WCHAR RtlDowncaseUnicodeChar(WCHAR Ch) */
static void bridge_RtlDowncaseUnicodeChar(void)
{
    uint32_t ch = STACK_ARG(0);
    g_eax = (uint32_t)(uint16_t)case_lower((uint16_t)ch);
}

/* 296: CHAR RtlLowerChar(CHAR Ch) */
static void bridge_RtlLowerChar(void)
{
    uint16_t u = case_lower((uint16_t)(STACK_ARG(0) & 0xFF));
    g_eax = u < 0x100 ? u : '?';
}

/* 313: WCHAR RtlUpcaseUnicodeChar(WCHAR Ch) */
static void bridge_RtlUpcaseUnicodeChar(void)
{
    uint32_t ch = STACK_ARG(0);
    g_eax = (uint32_t)(uint16_t)case_upper((uint16_t)ch);
}

/* 316: CHAR RtlUpperChar(CHAR Ch) */
static void bridge_RtlUpperChar(void)
{
    /* The Unicode mapping of the byte, '?' if it lands past Latin-1 (y-diaeresis does) */
    uint16_t u = case_upper((uint16_t)(STACK_ARG(0) & 0xFF));
    g_eax = u < 0x100 ? u : '?';
}

/* ── Rtl Upcase/Downcase string (ordinals 276, 314) ─────────
 * NT name suggests (2 args) but the real signature takes a third
 * BOOLEAN AllocateDestString (12 stack bytes).  If AllocateDest is set the
 * destination buffer is carved from the guest heap and its guest VA stored in
 * Dest->Buffer; otherwise the caller's existing buffer must fit. */

/* 276: NTSTATUS RtlDowncaseUnicodeString(Dest, Src, AllocDest) */
static void bridge_RtlDowncaseUnicodeString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint32_t do_alloc = STACK_ARG(2);
    uint16_t s_len, d_max = 0;
    uint32_t s_buf, d_buf = 0, i;
    const uint16_t *sp;
    uint16_t *dp;

    if (!dst_va || !src_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    s_len = BRIDGE_MEM16(src_va + 0);
    s_buf = BRIDGE_MEM32(src_va + 4);
    if (s_len && !s_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }

    if (do_alloc) {
        d_buf = xbox_HeapAlloc((uint32_t)s_len + sizeof(uint16_t), 16);
        if (!d_buf) { g_eax = (uint32_t)STATUS_NO_MEMORY; return; }
        BRIDGE_MEM16(dst_va + 2) = (uint16_t)(s_len + sizeof(uint16_t));
        BRIDGE_MEM32(dst_va + 4) = d_buf;
    } else {
        d_max = BRIDGE_MEM16(dst_va + 2);
        d_buf = BRIDGE_MEM32(dst_va + 4);
        if (d_max < s_len) { g_eax = (uint32_t)STATUS_BUFFER_TOO_SMALL; return; }
        if (s_len && !d_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    }

    sp = (const uint16_t *)XBOX_TO_NATIVE(s_buf);
    dp = (uint16_t *)XBOX_TO_NATIVE(d_buf);
    for (i = 0; i < (uint32_t)(s_len / 2); i++)
        dp[i] = (uint16_t)case_lower(sp[i]);
    dp[(uint32_t)(s_len / 2)] = 0;
    BRIDGE_MEM16(dst_va + 0) = s_len;
    g_eax = 0;   /* STATUS_SUCCESS */
}

/* 314: NTSTATUS RtlUpcaseUnicodeString(Dest, Src, AllocDest) */
static void bridge_RtlUpcaseUnicodeString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint32_t do_alloc = STACK_ARG(2);
    uint16_t s_len, d_max = 0;
    uint32_t s_buf, d_buf = 0, i;
    const uint16_t *sp;
    uint16_t *dp;

    if (!dst_va || !src_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    s_len = BRIDGE_MEM16(src_va + 0);
    s_buf = BRIDGE_MEM32(src_va + 4);
    if (s_len && !s_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }

    if (do_alloc) {
        d_buf = xbox_HeapAlloc((uint32_t)s_len + sizeof(uint16_t), 16);
        if (!d_buf) { g_eax = (uint32_t)STATUS_NO_MEMORY; return; }
        BRIDGE_MEM16(dst_va + 2) = (uint16_t)(s_len + sizeof(uint16_t));
        BRIDGE_MEM32(dst_va + 4) = d_buf;
    } else {
        d_max = BRIDGE_MEM16(dst_va + 2);
        d_buf = BRIDGE_MEM32(dst_va + 4);
        if (d_max < s_len) { g_eax = (uint32_t)STATUS_BUFFER_TOO_SMALL; return; }
        if (s_len && !d_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    }

    sp = (const uint16_t *)XBOX_TO_NATIVE(s_buf);
    dp = (uint16_t *)XBOX_TO_NATIVE(d_buf);
    for (i = 0; i < (uint32_t)(s_len / 2); i++)
        dp[i] = (uint16_t)case_upper(sp[i]);
    dp[(uint32_t)(s_len / 2)] = 0;
    BRIDGE_MEM16(dst_va + 0) = s_len;
    g_eax = 0;   /* STATUS_SUCCESS */
}

/* ── Rtl Integer conversion (ordinals 292, 293) ─────────────
 * NT pads RtlIntegerToChar's output to Length with leading zeros and expects
 * Length+1 bytes of buffer (the NUL goes at [Length]). */

/* 292: NTSTATUS RtlIntegerToChar(ULONG Value, ULONG Base, ULONG Length, PCHAR String) */
/* The kernel's RtlIntegerToChar: base 0 is 10, then only 2, 8, 10, 16. A negative length pads with zeros to that many
 * digits; a positive one is only room, and a digit count past it is STATUS_BUFFER_OVERFLOW. NUL if it fits. */
static uint32_t rtl_int_to_char(uint32_t value, uint32_t base, int32_t length, char *out)
{
    static const char digs[] = "0123456789ABCDEF";
    char tmp[40];
    int n = 0, want;

    if (!base) base = 10;
    if (!rtl_int_base(base)) return 0xC000000Du;
    do { tmp[n++] = digs[value % base]; value /= base; } while (value);
    want = length < 0 ? -length : length;
    if (length < 0) while (n < want && n < (int)sizeof tmp) tmp[n++] = '0';
    if (n > want) return 0x80000005u;
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    if (n < want) out[n] = 0;
    return 0;
}

static void bridge_RtlIntegerToChar(void)
{
    g_eax = rtl_int_to_char(STACK_ARG(0), STACK_ARG(1), (int32_t)STACK_ARG(2), (char *)XBOX_TO_NATIVE(STACK_ARG(3)));
}

/* 293: NTSTATUS RtlIntegerToUnicodeString(ULONG Value, ULONG Base, PXBOX_UNICODE_STRING String) */
static void bridge_RtlIntegerToUnicodeString(void)
{
    /* Through the text form, as the kernel: no sign, however large the value */
    char text[16];
    uint32_t str_va = STACK_ARG(2), n, max = BRIDGE_MEM16(str_va + 2), buf = BRIDGE_MEM32(str_va + 4), i;
    uint32_t st = rtl_int_to_char(STACK_ARG(0), STACK_ARG(1), 16, text);
    if ((int32_t)st < 0) { g_eax = st; return; }
    for (n = 0; n < sizeof text && text[n]; n++) {}
    if (n * 2 > max) { g_eax = 0x80000005u; return; }
    for (i = 0; i < n; i++) BRIDGE_MEM16(buf + i * 2) = (uint8_t)text[i];
    if (n * 2 + 2 <= max) BRIDGE_MEM16(buf + n * 2) = 0;
    BRIDGE_MEM16(str_va) = (uint16_t)(n * 2);
    g_eax = 0;
}

/* ── Rtl Memory (ordinals 284, 298, 320) ──────────────────── */

/* 284: VOID RtlFillMemory(PVOID Dest, SIZE_T Len, UCHAR Fill) */
static void bridge_RtlFillMemory(void)
{
    uint32_t dest_va = STACK_ARG(0);
    uint32_t len     = STACK_ARG(1);
    uint32_t fill    = STACK_ARG(2);

    if (dest_va && len)
        memset(XBOX_TO_NATIVE(dest_va), (int)(UCHAR)fill, (size_t)len);
    g_eax = 0;
}

/* 298: VOID RtlMoveMemory(PVOID Dest, PVOID Src, SIZE_T Len) */
static void bridge_RtlMoveMemory(void)
{
    uint32_t dest_va = STACK_ARG(0);
    uint32_t src_va  = STACK_ARG(1);
    uint32_t len     = STACK_ARG(2);

    if (dest_va && src_va && len)
        memmove(XBOX_TO_NATIVE(dest_va), XBOX_TO_NATIVE(src_va), (size_t)len);
    g_eax = 0;
}

/* 320: VOID RtlZeroMemory(PVOID Dest, SIZE_T Len) */
static void bridge_RtlZeroMemory(void)
{
    uint32_t dest_va = STACK_ARG(0);
    uint32_t len     = STACK_ARG(1);

    if (dest_va && len)
        memset(XBOX_TO_NATIVE(dest_va), 0, (size_t)len);
    g_eax = 0;
}

/* ── Rtl MultiByte <-> Unicode (ordinals 299, 300, 310, 311, 315) ──
 * All buffers are guest VAs.  MultiByteToWideChar / WideCharToMultiByte are
 * available on both host platforms.  UNICODE_STRING lengths are byte counts. */

/* 299: NTSTATUS RtlMultiByteToUnicodeN(PWCH UnicodeString, ULONG MaxBytesInUnicodeString,
 *       PULONG BytesInUnicodeString, PCCH MultiByteString, ULONG BytesInMultiByteString) */
static void bridge_RtlMultiByteToUnicodeN(void)
{
    uint32_t uni_va    = STACK_ARG(0);
    uint32_t max_bytes = STACK_ARG(1);
    uint32_t bytes_va  = STACK_ARG(2);
    uint32_t mb_va     = STACK_ARG(3);
    uint32_t mb_bytes  = STACK_ARG(4);
    int chars, result;

    if (!uni_va && max_bytes) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    chars = (int)(max_bytes / sizeof(WCHAR));
    result = l1_mb2wc((const char *)XBOX_TO_NATIVE(mb_va), (int)mb_bytes,
        uni_va ? (WCHAR *)XBOX_TO_NATIVE(uni_va) : NULL, chars);
    if (bytes_va)
        BRIDGE_MEM32(bytes_va) = result > 0 ? (uint32_t)(result * sizeof(WCHAR)) : 0;
    g_eax = 0  /* the kernel cannot fail: it converts what fits */;
}

/* 300: NTSTATUS RtlMultiByteToUnicodeSize(PULONG BytesInUnicodeString,
 *       PCCH MultiByteString, ULONG BytesInMultiByteString) */
static void bridge_RtlMultiByteToUnicodeSize(void)
{
    uint32_t bytes_va = STACK_ARG(0);
    uint32_t mb_va    = STACK_ARG(1);
    uint32_t mb_bytes = STACK_ARG(2);
    int result;

    result = l1_mb2wc((const char *)XBOX_TO_NATIVE(mb_va), (int)mb_bytes, NULL, 0);
    if (bytes_va)
        BRIDGE_MEM32(bytes_va) = result > 0 ? (uint32_t)(result * sizeof(WCHAR)) : 0;
    g_eax = result > 0 ? 0 : (uint32_t)STATUS_UNSUCCESSFUL;
}

/* 310: NTSTATUS RtlUnicodeToMultiByteN(PCHAR MultiByteString, ULONG MaxBytesInMultiByteString,
 *       PULONG BytesInMultiByteString, PCWCH UnicodeString, ULONG BytesInUnicodeString) */
static void bridge_RtlUnicodeToMultiByteN(void)
{
    uint32_t mb_va    = STACK_ARG(0);
    uint32_t max_bytes = STACK_ARG(1);
    uint32_t bytes_va = STACK_ARG(2);
    uint32_t uni_va   = STACK_ARG(3);
    uint32_t uni_bytes = STACK_ARG(4);
    int result;

    if (!mb_va && max_bytes) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    result = l1_wc2mb((const WCHAR *)XBOX_TO_NATIVE(uni_va), (int)(uni_bytes / sizeof(WCHAR)),
        mb_va ? (char *)XBOX_TO_NATIVE(mb_va) : NULL, (int)max_bytes);
    if (bytes_va)
        BRIDGE_MEM32(bytes_va) = result > 0 ? (uint32_t)result : 0;
    g_eax = 0  /* the kernel cannot fail: it converts what fits */;
}

/* 311: NTSTATUS RtlUnicodeToMultiByteSize(PULONG BytesInMultiByteString,
 *       PCWCH UnicodeString, ULONG BytesInUnicodeString) */
static void bridge_RtlUnicodeToMultiByteSize(void)
{
    uint32_t bytes_va = STACK_ARG(0);
    uint32_t uni_va   = STACK_ARG(1);
    uint32_t uni_bytes = STACK_ARG(2);
    int result;

    result = l1_wc2mb((const WCHAR *)XBOX_TO_NATIVE(uni_va), (int)(uni_bytes / sizeof(WCHAR)),
        NULL, 0);
    if (bytes_va)
        BRIDGE_MEM32(bytes_va) = result > 0 ? (uint32_t)result : 0;
    g_eax = result > 0 ? 0 : (uint32_t)STATUS_UNSUCCESSFUL;
}

/* 315: NTSTATUS RtlUpcaseUnicodeToMultiByteN(PCHAR MultiByteString,
 *       ULONG MaxBytesInUnicodeString, PULONG BytesInMultiByteString,
 *       PCWCH UnicodeString, ULONG BytesInUnicodeString)
 * WideCharToMultiByte has no case flag, so the source is upcased into a
 * temporary host buffer first. */
static void bridge_RtlUpcaseUnicodeToMultiByteN(void)
{
    uint32_t mb_va     = STACK_ARG(0);
    uint32_t max_bytes = STACK_ARG(1);
    uint32_t bytes_va  = STACK_ARG(2);
    uint32_t uni_va    = STACK_ARG(3);
    uint32_t uni_bytes = STACK_ARG(4);
    uint32_t wc = uni_bytes / sizeof(WCHAR);
    uint16_t *tmp = NULL;
    int result;
    uint32_t i;

    if (!mb_va && max_bytes) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if (wc) {
        tmp = (uint16_t *)malloc(((size_t)wc + 1) * sizeof(uint16_t));
        if (!tmp) { g_eax = (uint32_t)STATUS_NO_MEMORY; return; }
        {
            const uint16_t *sp = (const uint16_t *)XBOX_TO_NATIVE(uni_va);
            for (i = 0; i < wc; i++)
                tmp[i] = (uint16_t)case_upper(sp[i]);
            tmp[wc] = 0;
        }
    }
    result = l1_wc2mb(tmp, (int)wc,
        mb_va ? (char *)XBOX_TO_NATIVE(mb_va) : NULL, (int)max_bytes);
    free(tmp);
    if (bytes_va)
        BRIDGE_MEM32(bytes_va) = result > 0 ? (uint32_t)result : 0;
    g_eax = 0  /* the kernel cannot fail: it converts what fits */;
}

/* ── Rtl String -> Integer (ordinals 267, 309) ────────────── */

/* 267: NTSTATUS RtlCharToInteger(PCSTR Str, ULONG Base, PULONG Value)
 * Base 0 autodetects (0x -> 16, 0 -> 8, else 10), mirroring strtoul. */
static void bridge_RtlCharToInteger(void)
{
    const char *p = (const char *)XBOX_TO_NATIVE(STACK_ARG(0));
    uint32_t base = STACK_ARG(1), val_va = STACK_ARG(2), v = 0;
    char sign, c;

    if (base && !rtl_int_base(base)) { g_eax = 0xC000000Du; return; }
    while (*p && *p <= ' ') p++;
    sign = *p;
    c = *p ? *p++ : 0;
    if (c == '-' || c == '+') c = *p ? *p++ : 0;
    if (!base) {
        base = 10;
        if (c == '0') {
            c = *p ? *p++ : 0;
            if (c == 'x') base = 16, c = *p ? *p++ : 0;
            else if (c == 'o') base = 8, c = *p ? *p++ : 0;
            else if (c == 'b') base = 2, c = *p ? *p++ : 0;
        }
    }
    for (; c; c = *p ? *p++ : 0) {
        uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else break;
        if (d >= base) break;
        v = v * base + d;
    }
    BRIDGE_MEM32(val_va) = sign == '-' ? 0u - v : v;
    g_eax = 0;
}

/* 309: NTSTATUS RtlUnicodeStringToInteger(PXBOX_UNICODE_STRING Str, ULONG Base, PULONG Value) */
static void bridge_RtlUnicodeStringToInteger(void)
{
    uint32_t str_va = STACK_ARG(0), base = STACK_ARG(1), val_va = STACK_ARG(2), v = 0;
    uint32_t n = BRIDGE_MEM16(str_va) / 2u, i = 0;
    const uint16_t *w = (const uint16_t *)XBOX_TO_NATIVE(BRIDGE_MEM32(str_va + 4));
    uint16_t sign, c;
#define NEXTW() (i < n ? w[i++] : 0)

    if (base && !rtl_int_base(base)) { g_eax = 0xC000000Du; return; }
    while (i < n && w[i] <= ' ') i++;
    sign = NEXTW();
    c = sign;
    if (c == '-' || c == '+') c = NEXTW();
    if (!base) {
        base = 10;
        if (c == '0') {
            c = NEXTW();
            if (c == 'x') base = 16, c = NEXTW();
            else if (c == 'o') base = 8, c = NEXTW();
            else if (c == 'b') base = 2, c = NEXTW();
        }
    }
    for (; c; c = NEXTW()) {
        uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else break;
        if (d >= base) break;
        v = v * base + d;
    }
#undef NEXTW
    BRIDGE_MEM32(val_va) = sign == '-' ? 0u - v : v;
    g_eax = 0;
}

/* ── Rtl Byte swaps (ordinals 307, 318) ─────────────────────
 * Hand-rolled rather than MSVC _byteswap_* so the file stays portable. */

/* 307: ULONG RtlUlongByteSwap(ULONG Value) */
static void bridge_RtlUlongByteSwap(void)
{
    uint32_t v = g_ecx;   /* fastcall */
    g_eax = (v << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24);
}

/* 318: USHORT RtlUshortByteSwap(USHORT Value) */
static void bridge_RtlUshortByteSwap(void)
{
    uint32_t v = g_ecx;   /* fastcall */
    g_eax = (uint32_t)(uint16_t)(((v & 0xFFu) << 8) | ((v >> 8) & 0xFFu));
}

/* 278: VOID RtlEnterCriticalSectionAndRegion(PRTL_CRITICAL_SECTION) */
static void bridge_RtlEnterCriticalSectionAndRegion(void)
{
    uint32_t cs_va = STACK_ARG(0);
    InterlockedIncrement((volatile LONG *)XBOX_TO_NATIVE(cs_va + 0x10));
    xbox_RtlEnterCriticalSection((PRTL_CRITICAL_SECTION)XBOX_TO_NATIVE(cs_va));
    cs_entered(cs_va);
    g_eax = 0;
}

/* 295: VOID RtlLeaveCriticalSectionAndRegion(PRTL_CRITICAL_SECTION) */
static void bridge_RtlLeaveCriticalSectionAndRegion(void)
{
    uint32_t cs_va = STACK_ARG(0);
    cs_leaving(cs_va);
    xbox_RtlLeaveCriticalSection((PRTL_CRITICAL_SECTION)XBOX_TO_NATIVE(cs_va));
    g_eax = 0;
}

/* 306: BOOLEAN RtlTryEnterCriticalSection(PRTL_CRITICAL_SECTION) */
static void bridge_RtlTryEnterCriticalSection(void)
{
    extern BOOLEAN xbox_RtlTryEnterCriticalSection(PRTL_CRITICAL_SECTION);
    uint32_t cs_va = STACK_ARG(0);
    if (xbox_RtlTryEnterCriticalSection((PRTL_CRITICAL_SECTION)XBOX_TO_NATIVE(cs_va))) {
        InterlockedIncrement((volatile LONG *)XBOX_TO_NATIVE(cs_va + 0x10));
        cs_entered(cs_va);
        g_eax = 1;
    } else {
        g_eax = 0;
    }
}

/* ── Rtl Misc stubs (ordinals 288, 297, 319, 321) ─────────── */

/* 288: VOID RtlGetCallersAddress(PVOID *CallersAddress, PVOID *CallersCaller) */
static void bridge_RtlGetCallersAddress(void)
{
    uint32_t a_va = STACK_ARG(0);
    uint32_t c_va = STACK_ARG(1);

    if (a_va) BRIDGE_MEM32(a_va) = 0;
    if (c_va) BRIDGE_MEM32(c_va) = 0;
    g_eax = 0;
}

/* 297: VOID RtlMapGenericMask(PACCESS_MASK AccessMask, PRTL_GENERIC_MAPPING) */
static void bridge_RtlMapGenericMask(void)
{
    /* AccessMask, GenericMapping: GENERIC_READ, WRITE, EXECUTE and ALL replaced
     * by what the mapping says, those four bits cleared. */
    uint32_t m = STACK_ARG(0), g = STACK_ARG(1);
    uint32_t v = BRIDGE_MEM32(m);
    if (v & 0x80000000u) v |= BRIDGE_MEM32(g + 0);
    if (v & 0x40000000u) v |= BRIDGE_MEM32(g + 4);
    if (v & 0x20000000u) v |= BRIDGE_MEM32(g + 8);
    if (v & 0x10000000u) v |= BRIDGE_MEM32(g + 12);
    BRIDGE_MEM32(m) = v & 0x0FFFFFFFu;
    g_eax = 0;
}

/* 319: ULONG RtlWalkFrameChain(PVOID *Callers, ULONG Count, ULONG Flags) */
static void bridge_RtlWalkFrameChain(void)
{
    g_eax = 0;
}

/* 321: XboxEEPROMKey (data export, 0 args)
 * Not in the kernel_data_va_for_ordinal table, so it needs a bridge that hands
 * the caller a guest-addressable 16-byte key.  Carved from the guest heap once. */
static void bridge_XboxEEPROMKey(void)
{
    static uint32_t eeprom_va = 0;

    if (!eeprom_va) {
        eeprom_va = xbox_HeapAlloc(16, 16);
        if (eeprom_va)
            memset(XBOX_TO_NATIVE(eeprom_va), 0, 16);
    }
    g_eax = eeprom_va;
}

/* ── Rtl Exception (ordinals 264, 265, 266, 303) ──────────── */

/* 264: VOID RtlAssert(PVOID FailedAssertion, PVOID FileName, ULONG LineNumber) */
static void bridge_RtlAssert(void)
{
    uint32_t assert_va = STACK_ARG(0);
    uint32_t file_va   = STACK_ARG(1);
    uint32_t line      = STACK_ARG(2);
    const char *a = assert_va ? (const char *)XBOX_TO_NATIVE(assert_va) : NULL;
    const char *f = file_va   ? (const char *)XBOX_TO_NATIVE(file_va) : NULL;

    xbox_log(XBOX_LOG_ERROR, XBOX_LOG_RTL, "RtlAssert: %s:%u %s",
             f ? f : "?", line, a ? a : "?");
#ifdef _DEBUG
    DebugBreak();
#endif
    g_eax = 0;
}

/* 265: VOID RtlCaptureContext(PCONTEXT Context) - stub: zero the x86 CONTEXT. */
static void bridge_RtlCaptureContext(void)
{
    /* The Xbox CONTEXT is smaller than Windows' (the registers end at +0x238):
     * clearing 0x2CC bytes ran past it. Only what the real one writes. Registers a
     * recompiled function holds locally read as their last published value. */
    uint32_t c = STACK_ARG(0);
    if (c) {
        BRIDGE_MEM32(c + 0x21C) = g_eax;
        BRIDGE_MEM32(c + 0x218) = g_ecx;
        BRIDGE_MEM32(c + 0x214) = g_edx;
        BRIDGE_MEM32(c + 0x210) = g_ebx;
        BRIDGE_MEM32(c + 0x20C) = g_esi;
        BRIDGE_MEM32(c + 0x208) = g_edi;
        BRIDGE_MEM16(c + 0x228) = 0x08;
        BRIDGE_MEM16(c + 0x234) = 0x10;
        BRIDGE_MEM32(c + 0x22C) = 0x202;
        BRIDGE_MEM32(c + 0x224) = BRIDGE_MEM32(g_esp);   /* the return address */
        BRIDGE_MEM32(c + 0x220) = g_ebp;
        BRIDGE_MEM32(c + 0x230) = g_esp + 8;
    }
    g_eax = 0;
}

/* 266: ULONG RtlCaptureStackBackTrace(FramesToSkip, FramesToCapture,
 *       BackTrace, BackTraceHash) - stub, returns 0 frames. Stack is cleaned
 *       up per stdcall_args_for_ordinal (20 bytes in kernel_bridge.c). */
static void bridge_RtlCaptureStackBackTrace(void)
{
    g_eax = 0;
}


/* --- Ke Object Shadow Table --- */
#define KE_SHADOW_FIRST 4096
typedef struct { uint32_t guest_va; HANDLE host_handle; uint32_t in_use; } KE_SHADOW_SLOT;
static KE_SHADOW_SLOT *g_ke_shadow;   /* no limit on the console: grows, under g_ke_shadow_cs */
static size_t g_ke_shadow_size;

/* A free row, growing the table when there is none. Lock held. */
static int ke_shadow_free_slot(void)
{
    size_t i, n;
    KE_SHADOW_SLOT *grown;
    for (i = 0; i < g_ke_shadow_size; i++)
        if (!g_ke_shadow[i].in_use) return (int)i;
    n = g_ke_shadow_size ? g_ke_shadow_size * 2 : KE_SHADOW_FIRST;
    grown = (KE_SHADOW_SLOT *)realloc(g_ke_shadow, n * sizeof *grown);
    if (!grown) return -1;
    memset(grown + g_ke_shadow_size, 0, (n - g_ke_shadow_size) * sizeof *grown);
    i = g_ke_shadow_size;
    g_ke_shadow = grown;
    g_ke_shadow_size = n;
    return (int)i;
}
static CRITICAL_SECTION g_ke_shadow_cs;
static int g_ke_shadow_init;

static void ke_shadow_init(void)
{
    if (g_ke_shadow_init)
        return;
    InitializeCriticalSection(&g_ke_shadow_cs);
    g_ke_shadow_init = 1;
}

/* Insert a mapping guest_va -> host HANDLE. Replaces any prior mapping for
 * the same guest_va (closing the old handle and inserting the new one). */
static void ke_shadow_insert(uint32_t guest_va, HANDLE host)
{
    uint32_t i;

    if (!guest_va || !host)
        return;

    ke_shadow_init();

    EnterCriticalSection(&g_ke_shadow_cs);

    /* Reuse an existing slot for this VA, if present. */
    for (i = 0; i < g_ke_shadow_size; i++) {
        if (g_ke_shadow[i].in_use && g_ke_shadow[i].guest_va == guest_va) {
            if (g_ke_shadow[i].host_handle && g_ke_shadow[i].host_handle != host)
                CloseHandle(g_ke_shadow[i].host_handle);
            g_ke_shadow[i].host_handle = host;
            LeaveCriticalSection(&g_ke_shadow_cs);
            return;
        }
    }

    /* Otherwise a free slot. */
    {
        int free_row = ke_shadow_free_slot();
        if (free_row >= 0) {
            g_ke_shadow[free_row].in_use      = 1;
            g_ke_shadow[free_row].guest_va    = guest_va;
            g_ke_shadow[free_row].host_handle = host;
            LeaveCriticalSection(&g_ke_shadow_cs);
            return;
        }
    }

    /* Out of memory -- log and drop the new object. The caller's subsequent
     * waits will fall back to XBOX_TO_NATIVE and fail gracefully. */
    fprintf(stderr, "  [KERNEL] Ke shadow: out of memory; dropping VA 0x%08X\n",
            guest_va);
    LeaveCriticalSection(&g_ke_shadow_cs);
    CloseHandle(host);
}

/* Look up the host HANDLE for a guest object VA. Returns NULL if not found. */
static HANDLE ke_shadow_lookup(uint32_t guest_va)
{
    uint32_t i;
    HANDLE h = NULL;

    if (!guest_va)
        return NULL;

    ke_shadow_init();

    EnterCriticalSection(&g_ke_shadow_cs);
    for (i = 0; i < g_ke_shadow_size; i++) {
        if (g_ke_shadow[i].in_use && g_ke_shadow[i].guest_va == guest_va) {
            h = g_ke_shadow[i].host_handle;
            break;
        }
    }
    LeaveCriticalSection(&g_ke_shadow_cs);
    return h;
}

/* Remove (and close) the mapping for a guest object VA. */
static void ke_shadow_remove(uint32_t guest_va)
{
    uint32_t i;

    if (!guest_va)
        return;

    ke_shadow_init();

    EnterCriticalSection(&g_ke_shadow_cs);
    for (i = 0; i < g_ke_shadow_size; i++) {
        if (g_ke_shadow[i].in_use && g_ke_shadow[i].guest_va == guest_va) {
            if (g_ke_shadow[i].host_handle)
                CloseHandle(g_ke_shadow[i].host_handle);
            g_ke_shadow[i].in_use      = 0;
            g_ke_shadow[i].guest_va    = 0;
            g_ke_shadow[i].host_handle = NULL;
            break;
        }
    }
    LeaveCriticalSection(&g_ke_shadow_cs);
}

/* Events a title initialises itself (RECOMP_TITLE_KEVENTS=1, off by default).
 *
 * Shadows are made by KeInitializeEvent, but XDK code can build a KEVENT by
 * writing its header directly: one title (T()NY) does not even import
 * KeInitializeEvent, and its D3D waits on the event at miniport + 0x1A4, which
 * the graphics interrupt sets for software method 5. With no shadow, the guest
 * address itself went to Win32 as a HANDLE: SetEvent failed silently, and every
 * wait failed at once with STATUS_UNSUCCESSFUL, which BlockOnTime's
 * "while (KeWaitForSingleObject(...))" retried forever -- the menu froze.
 *
 * So on first use, an object whose header reads as an event (Type 0 notification
 * or 1 synchronization, Size 4 dwords) gets a host event with the same type and
 * state, marked in_use = 2. For those the bridges keep SignalState in guest
 * memory up to date, because XDK code resets it by writing 0 there.
 * Returns NULL for anything else (the old paths apply).
 *
 * Opt-in because it changes what every such wait does: a wait that failed at
 * once now blocks until the event is set, and on a title whose setter the
 * runtime does not model yet (a GPU interrupt, say) that turns a spin into a
 * hang. Turn it on for a title that waits on events it built itself. */
static HANDLE ke_guest_event(uint32_t guest_va, int *type)
{
    static int enabled = -1;
    uint32_t hdr, i;
    HANDLE h = NULL;

    if (enabled < 0) {
        const char *e = getenv("RECOMP_TITLE_KEVENTS");
        enabled = e && *e == '1';
    }
    if (!enabled)
        return NULL;
    if (guest_va < 0x10000u || (guest_va & 3u))
        return NULL;
    if (guest_va >= 0x04000000u && (guest_va < 0x80000000u || guest_va >= 0x84000000u))
        return NULL;                               /* RAM or the contiguous window */
    hdr = BRIDGE_MEM32(guest_va);
    if (((hdr & 0xFFu) != 0 && (hdr & 0xFFu) != 1) || ((hdr >> 16) & 0xFFu) != 4)
        return NULL;
    *type = (int)(hdr & 0xFFu);

    ke_shadow_init();
    EnterCriticalSection(&g_ke_shadow_cs);
    for (i = 0; i < g_ke_shadow_size; i++) {
        if (g_ke_shadow[i].in_use && g_ke_shadow[i].guest_va == guest_va) {
            h = g_ke_shadow[i].in_use == 2 ? g_ke_shadow[i].host_handle : NULL;
            LeaveCriticalSection(&g_ke_shadow_cs);
            return h;                              /* a KeInitializeEvent shadow: old path */
        }
    }
    {
        int free_row = ke_shadow_free_slot();
        if (free_row >= 0) {
            h = CreateEventA(NULL, *type == 0, BRIDGE_MEM32(guest_va + 4) != 0, NULL);
            if (h) {
                g_ke_shadow[free_row].in_use      = 2;
                g_ke_shadow[free_row].guest_va    = guest_va;
                g_ke_shadow[free_row].host_handle = h;
            }
        }
    }
    LeaveCriticalSection(&g_ke_shadow_cs);
    if (h) {
        static int shown;
        if (shown++ < 16)
            fprintf(stderr, "  [KERNEL] event 0x%08X built by the title: host event created "
                            "(%s)\n", guest_va, *type ? "synchronization" : "notification");
    }
    return h;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Data Exports (102, 120, 154, 240, 245, 249)
 *
 * These ordinals are DATA exports, not functions. The bridge returns a guest
 * VA inside the kernel data page so the title can dereference it. The offsets
 * below (0x510-0x558) are free in xbox_memory_layout.h's KDATA block (existing
 * slots end at 0x4B0+64, KiBugCheckData uses 0x500, page is 4 KB). Prefer
 * moving them into xbox_memory_layout.h and dropping the guarded defines here.
 * ═══════════════════════════════════════════════════════════════════════════
 */
#ifndef KDATA_MMGLOBAL
#define KDATA_MMGLOBAL          0x510  /* MmGlobalData (4 bytes) */
#endif
#ifndef KDATA_INTERRUPT_TIME
#define KDATA_INTERRUPT_TIME    0x520  /* KeInterruptTime (LARGE_INTEGER, 8 bytes) */
#endif
#ifndef KDATA_SYSTEM_TIME
#define KDATA_SYSTEM_TIME       0x530  /* KeSystemTime (LARGE_INTEGER, 8 bytes) */
#endif
#ifndef KDATA_OBJ_DIR_TYPE
#define KDATA_OBJ_DIR_TYPE      0x540  /* ObDirectoryObjectType (4 bytes) */
#endif
#ifndef KDATA_OBJ_HANDLE_TABLE
#define KDATA_OBJ_HANDLE_TABLE  0x548  /* ObpObjectHandleTable (4 bytes) */
#endif
#ifndef KDATA_OBJ_SYM_LINK_TYPE
#define KDATA_OBJ_SYM_LINK_TYPE 0x550  /* ObSymbolicLinkObjectType (4 bytes) */
#endif

/* --- MmGlobalData (ordinal 102, 0 args) --- */
static void bridge_MmGlobalData(void)
{
    /* Titles rarely dereference this; give it a stable, mapped address that
     * reads back the address itself (self-referential placeholder). */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_MMGLOBAL) =
        XBOX_KERNEL_DATA_BASE + KDATA_MMGLOBAL;
    g_eax = XBOX_KERNEL_DATA_BASE + KDATA_MMGLOBAL;
}

/* --- KeInterruptTime (ordinal 120, 0 args) --- */
static void bridge_KeInterruptTime(void)
{
    /* Refresh the 100ns-since-boot counter before handing out the pointer. */
    ULONGLONG t = (ULONGLONG)GetTickCount64() * 10000ull;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_INTERRUPT_TIME + 0) = (uint32_t)t;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_INTERRUPT_TIME + 4) = (uint32_t)(t >> 32);
    g_eax = XBOX_KERNEL_DATA_BASE + KDATA_INTERRUPT_TIME;
}

/* --- KeSystemTime (ordinal 154, 0 args) --- */
static void bridge_KeSystemTime(void)
{
    /* Refresh the 100ns-since-1601 clock before handing out the pointer. */
    LARGE_INTEGER now_time;
    xbox_KeQuerySystemTime(&now_time);
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_SYSTEM_TIME + 0) = now_time.LowPart;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_SYSTEM_TIME + 4) = (uint32_t)now_time.HighPart;
    g_eax = XBOX_KERNEL_DATA_BASE + KDATA_SYSTEM_TIME;
}

/* --- ObDirectoryObjectType (ordinal 240, 0 args) --- */
static void bridge_ObDirectoryObjectType(void)
{
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_OBJ_DIR_TYPE) =
        XBOX_KERNEL_DATA_BASE + KDATA_OBJ_DIR_TYPE;
    g_eax = XBOX_KERNEL_DATA_BASE + KDATA_OBJ_DIR_TYPE;
}

/* --- ObpObjectHandleTable (ordinal 245, 0 args) --- */
static void bridge_ObpObjectHandleTable(void)
{
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_OBJ_HANDLE_TABLE) =
        XBOX_KERNEL_DATA_BASE + KDATA_OBJ_HANDLE_TABLE;
    g_eax = XBOX_KERNEL_DATA_BASE + KDATA_OBJ_HANDLE_TABLE;
}

/* --- ObSymbolicLinkObjectType (ordinal 249, 0 args) --- */
static void bridge_ObSymbolicLinkObjectType(void)
{
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_OBJ_SYM_LINK_TYPE) =
        XBOX_KERNEL_DATA_BASE + KDATA_OBJ_SYM_LINK_TYPE;
    g_eax = XBOX_KERNEL_DATA_BASE + KDATA_OBJ_SYM_LINK_TYPE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Dbg (6, 7, 8, 10, 11)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- DbgBreakPointWithStatus (ordinal 6, 1 arg = 4 bytes) --- */
static void bridge_DbgBreakPointWithStatus(void)
{
    (void)STACK_ARG(0);
    DebugBreak();
    g_eax = 0;
}

/* --- DbgLoadImageSymbols (ordinal 7, 3 args = 12 bytes) --- */
static void bridge_DbgLoadImageSymbols(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    g_eax = 0;
}

/* --- DbgPrint (ordinal 8, __cdecl varargs, 0 fixed args) --- */
static void bridge_DbgPrint(void)
{
    /* First vararg is a guest format string. Log it verbatim (never treat it
     * as a format, it is guest-owned); caller cleans the stack (__cdecl). */
    const char *fmt = (const char *)XBOX_TO_NATIVE(STACK_ARG(0));
    if (fmt && KERNEL_LOG_ON()) {
        size_t i, n = 0;
        for (i = 0; i < 512 && fmt[i]; i++)
            n++;
        fwrite(fmt, 1, n, stderr);
        fputc('\n', stderr);
        fflush(stderr);
    }
    g_eax = 0;
}

/* --- DbgPrompt (ordinal 10, 2 args = 8 bytes) --- */
static void bridge_DbgPrompt(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

/* --- DbgUnLoadImageSymbols (ordinal 11, 3 args = 12 bytes) --- */
static void bridge_DbgUnLoadImageSymbols(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    g_eax = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Ex (12, 13, 18, 19, 20, 21, 25, 26, 27, 28)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- ExAcquireReadWriteLockExclusive (ordinal 12, 1 arg = 4 bytes) --- */
/* ExReadWriteLock, the same fields and steps as the kernel's. A critical section
 * stands in for its interrupts-off. */
static CRITICAL_SECTION g_rw_cs;
static INIT_ONCE g_rw_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK rw_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&g_rw_cs);
    return TRUE;
}
#define RW_LOCK()   (InitOnceExecuteOnce(&g_rw_once, rw_init, NULL, NULL), EnterCriticalSection(&g_rw_cs))
#define RW_UNLOCK() LeaveCriticalSection(&g_rw_cs)

static void rw_wait(uint32_t obj)
{
    HANDLE h = ke_shadow_lookup(obj);
    if (h) WaitForSingleObject(h, INFINITE);
}

static void bridge_ExAcquireReadWriteLockExclusive(void)
{
    uint32_t l = STACK_ARG(0);
    int wait;
    RW_LOCK();
    BRIDGE_MEM32(l) = BRIDGE_MEM32(l) + 1;
    wait = BRIDGE_MEM32(l) != 0;
    if (wait) BRIDGE_MEM32(l + 4) = BRIDGE_MEM32(l + 4) + 1;
    RW_UNLOCK();
    if (wait) rw_wait(l + 0x10);
    g_eax = 0;
}

/* --- ExAcquireReadWriteLockShared (ordinal 13, 1 arg = 4 bytes) --- */
static void bridge_ExAcquireReadWriteLockShared(void)
{
    uint32_t l = STACK_ARG(0);
    int wait = 0;
    RW_LOCK();
    BRIDGE_MEM32(l) = BRIDGE_MEM32(l) + 1;
    if (BRIDGE_MEM32(l) == 0 || (BRIDGE_MEM32(l + 0xC) != 0 && BRIDGE_MEM32(l + 4) == 0)) {
        BRIDGE_MEM32(l + 0xC) = BRIDGE_MEM32(l + 0xC) + 1;
    } else {
        BRIDGE_MEM32(l + 8) = BRIDGE_MEM32(l + 8) + 1;
        wait = 1;
    }
    RW_UNLOCK();
    if (wait) rw_wait(l + 0x20);
    g_eax = 0;
}

/* --- ExInitializeReadWriteLock (ordinal 18, 1 arg = 4 bytes) --- */
static void bridge_ExInitializeReadWriteLock(void)
{
    uint32_t l = STACK_ARG(0);
    g_eax = 0;
    if (!l) return;
    BRIDGE_MEM32(l) = 0xFFFFFFFFu;
    BRIDGE_MEM32(l + 4) = 0;
    BRIDGE_MEM32(l + 8) = 0;
    BRIDGE_MEM32(l + 0xC) = 0;
    /* WriterEvent: synchronization, not signalled. */
    BRIDGE_MEM32(l + 0x10) = 0x00040001u;
    BRIDGE_MEM32(l + 0x14) = 0;
    BRIDGE_MEM32(l + 0x18) = l + 0x18;
    BRIDGE_MEM32(l + 0x1C) = l + 0x18;
    ke_shadow_insert(l + 0x10, CreateEventW(NULL, FALSE, FALSE, NULL));
    /* ReaderSemaphore: count 0, limit 0x7FFFFFFF. */
    BRIDGE_MEM32(l + 0x20) = 0x00050005u;
    BRIDGE_MEM32(l + 0x24) = 0;
    BRIDGE_MEM32(l + 0x28) = l + 0x28;
    BRIDGE_MEM32(l + 0x2C) = l + 0x28;
    BRIDGE_MEM32(l + 0x30) = 0x7FFFFFFFu;
    ke_shadow_insert(l + 0x20, CreateSemaphoreW(NULL, 0, 0x7FFFFFFF, NULL));
}

/* --- ExInterlockedAddLargeInteger (ordinal 19, 3 args = 12 bytes)
 * LARGE_INTEGER ExInterlockedAddLargeInteger(PLARGE_INTEGER Addend,
 *                                            LARGE_INTEGER Increment);
 * Addend (4) + Increment.Low (4) + Increment.High (4). Returns the PREVIOUS
 * value of Addend in edx:eax. */
static void bridge_ExInterlockedAddLargeInteger(void)
{
    uint32_t addend_va = STACK_ARG(0);
    LONGLONG increment = ((LONGLONG)(uint32_t)STACK_ARG(2) << 32)
                       | (LONGLONG)(uint32_t)STACK_ARG(1);
    volatile LONG *p = (volatile LONG *)XBOX_TO_NATIVE(addend_va);
    LONGLONG old = 0;

    if (p) {
        if (((uintptr_t)p & 7) == 0) {
            volatile LONGLONG *qp = (volatile LONGLONG *)p;
            LONGLONG cmp = *qp;
            for (;;) {
                LONGLONG desired = cmp + increment;
                LONGLONG actual = InterlockedCompareExchange64(qp, desired, cmp);
                if (actual == cmp)
                    break;
                cmp = actual;
            }
            old = cmp;
        } else {
            /* Unaligned: single-threaded guest, plain RMW is good enough. */
            old = ((LONGLONG)(uint32_t)p[1] << 32) | (uint32_t)p[0];
            {
                LONGLONG newv = old + increment;
                p[0] = (LONG)(uint32_t)newv;
                p[1] = (LONG)(uint32_t)((uint64_t)newv >> 32);
            }
        }
    }

    g_eax = (uint32_t)old;
    g_edx = (uint32_t)((uint64_t)old >> 32);
}

/* --- ExInterlockedAddLargeStatistic (ordinal 20, 2 args = 8 bytes)
 * LARGE_INTEGER ExInterlockedAddLargeStatistic(PLARGE_INTEGER Addend,
 *                                              ULONG Increment);
 * Addend (4) + Increment (4). Atomic add; per task spec, returns 0. */
static void bridge_ExInterlockedAddLargeStatistic(void)
{
    /* fastcall: ecx Addend, edx Increment. 64-bit add of a 32-bit value. */
    uint32_t a = g_ecx, inc = g_edx;
    uint64_t v;
    ex_lock();
    v = ((uint64_t)BRIDGE_MEM32(a + 4) << 32) | BRIDGE_MEM32(a);
    v += inc;
    BRIDGE_MEM32(a) = (uint32_t)v;
    BRIDGE_MEM32(a + 4) = (uint32_t)(v >> 32);
    ex_unlock();
}

/* --- ExInterlockedCompareExchange64 (ordinal 21, 4 args = 16 bytes)
 * LONGLONG ExInterlockedCompareExchange64(PLONGLONG Destination,
 *                                         LONGLONG Exchange,
 *                                         PLONGLONG Comparand);
 * Arg layout on a 32-bit stack is Destination (4) + Exchange (8) +
 * Comparand.Low (4) -- the Comparand high dword is not passed, so the
 * comparison is against a zero-extended 32-bit value. Returns the old value
 * in edx:eax. */
static void bridge_ExInterlockedCompareExchange64(void)
{
    /* fastcall: ecx Destination, edx -> Exchange, the stack -> Comparand. */
    volatile LONGLONG *d = (volatile LONGLONG *)XBOX_TO_NATIVE(g_ecx);
    LONGLONG x = (LONGLONG)(((uint64_t)BRIDGE_MEM32(g_edx + 4) << 32) | BRIDGE_MEM32(g_edx));
    LONGLONG c = (LONGLONG)(((uint64_t)BRIDGE_MEM32(STACK_ARG(0) + 4) << 32) | BRIDGE_MEM32(STACK_ARG(0)));
    LONGLONG old = InterlockedCompareExchange64(d, x, c);
    g_eax = (uint32_t)old;
    g_edx = (uint32_t)((uint64_t)old >> 32);
}

/* --- ExReadWriteRefurbInfo (ordinal 25, 3 args = 12 bytes) --- */
/* ExReadWriteRefurbInfo(Info, Length, Write): 16 bytes the kernel keeps in a sector of the hard disk, its first DWORD
 * the signature 'BRFR' that it writes itself; a read with no signature there gives zeros. Length must be 16. Here for
 * "kernel_state.bin" (see xbox_state_save). */
static void bridge_ExReadWriteRefurbInfo(void)
{
    extern void xbox_refurb_access(uint32_t saved[4], int write);
    uint32_t saved[4];
    uint32_t info = STACK_ARG(0), length = STACK_ARG(1), write = STACK_ARG(2) & 0xFF;
    if (length != 16) { g_eax = 0xC000000Du; return; }
    if (write) {
        for (int i = 0; i < 4; i++) saved[i] = BRIDGE_MEM32(info + i * 4);
        saved[0] = 0x52465242u;
        xbox_refurb_access(saved, 1);
    } else {
        xbox_refurb_access(saved, 0);
        for (int i = 0; i < 4; i++) BRIDGE_MEM32(info + i * 4) = saved[0] == 0x52465242u ? saved[i] : 0;
    }
    g_eax = 0;
}

/* --- ExRaiseException (ordinal 26, 1 arg = 4 bytes) --- */
static void bridge_ExRaiseException(void)
{
    /* Not dispatched without RECOMP_SEH=1: see bridge_raise. At least it shows. */
    if (STACK_ARG(0) && bridge_seh_enabled()) { bridge_raise(STACK_ARG(0)); return; }
    fprintf(stderr, "  [KERNEL] ExRaiseException: record 0x%08X, not dispatched\n", STACK_ARG(0));
    fflush(stderr);
    g_eax = 0;
}

/* --- ExRaiseStatus (ordinal 27, 1 arg = 4 bytes) --- */
static void bridge_ExRaiseStatus(void)
{
    /* Not dispatched without RECOMP_SEH=1: see bridge_raise. At least it shows. */
    if (bridge_seh_enabled()) { bridge_raise_noncontinuable(STACK_ARG(0), 0); return; }
    fprintf(stderr, "  [KERNEL] ExRaiseStatus: status 0x%08X, not dispatched\n", STACK_ARG(0));
    fflush(stderr);
    g_eax = 0;
}

/* --- ExReleaseReadWriteLock (ordinal 28, 1 arg = 4 bytes) --- */
static void bridge_ExReleaseReadWriteLock(void)
{
    uint32_t l = STACK_ARG(0);
    uint32_t readers;
    g_eax = 0;
    RW_LOCK();
    BRIDGE_MEM32(l) = BRIDGE_MEM32(l) - 1;
    if ((int32_t)BRIDGE_MEM32(l) < 0) {           /* nobody holds or waits */
        BRIDGE_MEM32(l + 0xC) = 0;
        RW_UNLOCK();
        return;
    }
    if (BRIDGE_MEM32(l + 0xC) != 0) {             /* a reader left */
        BRIDGE_MEM32(l + 0xC) = BRIDGE_MEM32(l + 0xC) - 1;
        if (BRIDGE_MEM32(l + 0xC) != 0) {
            RW_UNLOCK();
            return;
        }
    } else if ((readers = BRIDGE_MEM32(l + 8)) != 0) {   /* a writer left, readers wait */
        HANDLE h = ke_shadow_lookup(l + 0x20);
        BRIDGE_MEM32(l + 0xC) = readers;
        BRIDGE_MEM32(l + 8) = 0;
        RW_UNLOCK();
        if (h) ReleaseSemaphore(h, (LONG)readers, NULL);
        return;
    }
    BRIDGE_MEM32(l + 4) = BRIDGE_MEM32(l + 4) - 1;   /* the next writer */
    RW_UNLOCK();
    {
        HANDLE h = ke_shadow_lookup(l + 0x10);
        if (h) SetEvent(h);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * ExfInterlocked linked-list operations (32, 33, 34)
 *
 * The Xbox ABI keeps a spin-lock around these; with a single-threaded guest
 * the two-pointer fix-up after the atomic head swap is sufficient.
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- ExfInterlockedInsertHeadList (ordinal 32, 2 args = 8 bytes)
 * PLIST_ENTRY ExfInterlockedInsertHeadList(ListHead, Entry);
 * Returns the entry's old Flink (the previous first entry, or ListHead
 * itself for an empty list). */
static void bridge_ExfInterlockedInsertHeadList(void)
{
    /* fastcall: ecx ListHead, edx Entry. The old first entry, NULL if it was empty. */
    uint32_t head = g_ecx, e = g_edx, first;
    ex_lock();
    first = BRIDGE_MEM32(head);
    BRIDGE_MEM32(e) = first;
    BRIDGE_MEM32(e + 4) = head;
    BRIDGE_MEM32(head) = e;
    BRIDGE_MEM32(first + 4) = e;
    ex_unlock();
    g_eax = first == head ? 0 : first;
}

/* --- ExfInterlockedInsertTailList (ordinal 33, 2 args = 8 bytes)
 * PLIST_ENTRY ExfInterlockedInsertTailList(ListHead, Entry);
 * Returns the previous tail. NT semantics return the old Blink (the entry
 * that was last, or ListHead for an empty list); note the task brief said
 * "old Flink" but the canonical Xbox/NT behaviour is the old Blink. */
static void bridge_ExfInterlockedInsertTailList(void)
{
    /* fastcall: ecx ListHead, edx Entry. The old last entry, NULL if it was empty. */
    uint32_t head = g_ecx, e = g_edx, last;
    ex_lock();
    last = BRIDGE_MEM32(head + 4);
    BRIDGE_MEM32(e) = head;
    BRIDGE_MEM32(e + 4) = last;
    BRIDGE_MEM32(head + 4) = e;
    BRIDGE_MEM32(last) = e;
    ex_unlock();
    g_eax = last == head ? 0 : last;
}

/* --- ExfInterlockedRemoveHeadList (ordinal 34, 1 arg = 4 bytes)
 * PLIST_ENTRY ExfInterlockedRemoveHeadList(ListHead);
 * Returns the removed first entry, or 0 when the list is empty. */
static void bridge_ExfInterlockedRemoveHeadList(void)
{
    /* fastcall: ecx ListHead. The first entry, NULL if empty. */
    uint32_t head = g_ecx, first;
    ex_lock();
    first = BRIDGE_MEM32(head);
    if (first == head) {
        first = 0;
    } else {
        uint32_t next = BRIDGE_MEM32(first);
        BRIDGE_MEM32(head) = next;
        BRIDGE_MEM32(next + 4) = head;
    }
    ex_unlock();
    g_eax = first;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Fsc (36)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- FscInvalidateIdleBlocks (ordinal 36, 0 args = 0 bytes) --- */
static void bridge_FscInvalidateIdleBlocks(void)
{
    g_eax = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Hal (43, 365, 366)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- HalEnableSystemInterrupt (ordinal 43, 2 args = 8 bytes) --- */
static void bridge_HalEnableSystemInterrupt(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

/* --- HalEnableSecureTrayEject (ordinal 365, 0 args = 0 bytes) --- */
static void bridge_HalEnableSecureTrayEject(void)
{
    g_eax = 0;
}

/* --- HalWriteSMCScratchRegister (ordinal 366, 1 arg = 4 bytes) --- */
static void bridge_HalWriteSMCScratchRegister(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Interlocked (51-58) -- __fastcall: args travel in ecx/edx/eax, never on
 * the guest stack, so the arg-size entries are all 0 for these. The guest VA
 * in ecx/edx must go through XBOX_TO_NATIVE, exactly like the existing
 * bridge_ObfDereferenceObject.
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- InterlockedCompareExchange (ordinal 51)
 * LONG InterlockedCompareExchange(PLONG Destination, LONG Exchange,
 *                                 LONG Comparand);
 * fastcall: ecx=Destination, edx=Exchange, eax=Comparand-in/old-out. */
static void bridge_InterlockedCompareExchange(void)
{
    /* ecx Destination, edx Exchange, the comparand on the stack. */
    LONG *dst = (LONG *)XBOX_TO_NATIVE(g_ecx);
    if (!dst) {
        g_eax = 0;
        return;
    }
    g_eax = (uint32_t)InterlockedCompareExchange(dst, (LONG)g_edx, (LONG)STACK_ARG(0));
}

/* --- InterlockedDecrement (ordinal 52)
 * fastcall: ecx=Destination. */
static void bridge_InterlockedDecrement(void)
{
    LONG *dst = (LONG *)XBOX_TO_NATIVE(g_ecx);
    if (!dst) {
        g_eax = 0;
        return;
    }
    g_eax = (uint32_t)InterlockedDecrement(dst);
}

/* --- InterlockedIncrement (ordinal 53)
 * fastcall: ecx=Destination. */
static void bridge_InterlockedIncrement(void)
{
    LONG *dst = (LONG *)XBOX_TO_NATIVE(g_ecx);
    if (!dst) {
        g_eax = 0;
        return;
    }
    g_eax = (uint32_t)InterlockedIncrement(dst);
}

/* --- InterlockedExchange (ordinal 54)
 * fastcall: ecx=Destination, edx=Value. */
static void bridge_InterlockedExchange(void)
{
    LONG *dst = (LONG *)XBOX_TO_NATIVE(g_ecx);
    if (!dst) {
        g_eax = 0;
        return;
    }
    g_eax = (uint32_t)InterlockedExchange(dst, (LONG)g_edx);
}

/* --- InterlockedExchangeAdd (ordinal 55)
 * fastcall: ecx=Destination, edx=Value. */
static void bridge_InterlockedExchangeAdd(void)
{
    LONG *dst = (LONG *)XBOX_TO_NATIVE(g_ecx);
    if (!dst) {
        g_eax = 0;
        return;
    }
    g_eax = (uint32_t)InterlockedExchangeAdd(dst, (LONG)g_edx);
}

/* --- InterlockedFlushSList (ordinal 56, fastcall) --- */
static void bridge_InterlockedFlushSList(void)
{
    uint32_t list = g_ecx;
    volatile LONGLONG *h = (volatile LONGLONG *)XBOX_TO_NATIVE(list);
    LONGLONG old;
    do {
        old = *h;
        if ((uint32_t)old == 0) break;
    } while (InterlockedCompareExchange64(h, 0, old) != old);
    g_eax = (uint32_t)old;
    g_edx = (uint32_t)((uint64_t)old >> 32);
}

/* --- InterlockedPopEntrySList (ordinal 57, fastcall) --- */
static void bridge_InterlockedPopEntrySList(void)
{
    uint32_t list = g_ecx;
    volatile LONGLONG *h = (volatile LONGLONG *)XBOX_TO_NATIVE(list);
    LONGLONG old;
    for (;;) {
        uint32_t next, hi;
        old = *h;
        if ((uint32_t)old == 0) break;
        next = BRIDGE_MEM32((uint32_t)old);
        hi = (uint32_t)((uint64_t)old >> 32) + 0xFFFFu;   /* Depth - 1, Sequence + 1 */
        if (InterlockedCompareExchange64(h, (LONGLONG)(((uint64_t)hi << 32) | next), old) == old)
            break;
    }
    g_eax = (uint32_t)old;
    g_edx = (uint32_t)((uint64_t)old >> 32);
}

/* --- InterlockedPushEntrySList (ordinal 58, fastcall) --- */
static void bridge_InterlockedPushEntrySList(void)
{
    uint32_t list = g_ecx, entry = g_edx;
    volatile LONGLONG *h = (volatile LONGLONG *)XBOX_TO_NATIVE(list);
    LONGLONG old;
    do {
        uint32_t hi;
        old = *h;
        BRIDGE_MEM32(entry) = (uint32_t)old;
        hi = (uint32_t)((uint64_t)old >> 32) + 0x10001u;   /* Depth + 1, Sequence + 1 */
        if (InterlockedCompareExchange64(h, (LONGLONG)(((uint64_t)hi << 32) | entry), old) == old)
            break;
    } while (1);
    g_eax = (uint32_t)old;
    g_edx = (uint32_t)((uint64_t)old >> 32);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Io (59, 60, 63, 72, 75, 76, 77, 78, 80, 90, 91)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- IoAllocateIrp (ordinal 59, 2 args = 8 bytes)
 * PIRP IoAllocateIrp(CCHAR StackSize, BOOLEAN ChargeQuota);
 * Allocates an IRP header plus StackSize IO_STACK_LOCATIONs from the guest
 * heap. Pair with IoFreeIrp (ordinal 72), which frees through the same heap. */

/* --- IoBuildAsynchronousFsdRequest (ordinal 60, 7 args = 28 bytes) --- */

/* --- IoCheckShareAccess (ordinal 63, 5 args = 20 bytes) --- */

/* --- IoFreeIrp (ordinal 72, 1 arg = 4 bytes) --- */

/* --- IoQueryFileInformation (ordinal 75, 5 args = 20 bytes) --- */
static void bridge_IoQueryFileInformation(void)
{
    g_eax = 0xC0000002u;   /* STATUS_NOT_IMPLEMENTED: driver-level, no file objects here */
}

/* --- IoQueryVolumeInformation (ordinal 76, 5 args = 20 bytes) --- */
static void bridge_IoQueryVolumeInformation(void)
{
    g_eax = 0xC0000002u;   /* STATUS_NOT_IMPLEMENTED: driver-level, no file objects here */
}

/* --- IoQueueThreadIrp (ordinal 77, 1 arg = 4 bytes) --- */
static void bridge_IoQueueThreadIrp(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

/* --- IoRemoveShareAccess (ordinal 78, 2 args = 8 bytes) --- */

/* --- IoSetShareAccess (ordinal 80, 5 args = 20 bytes) --- */

/* --- IoDismountVolume (ordinal 90, 1 arg = 4 bytes) --- */
static void bridge_IoDismountVolume(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

/* --- IoDismountVolumeByName (ordinal 91, 1 arg = 4 bytes) --- */
static void bridge_IoDismountVolumeByName(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Kd (88, 89)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- KdDebuggerEnabled (ordinal 88, 0 args = 0 bytes) --- */
static void bridge_KdDebuggerEnabled(void)
{
    g_eax = 0;  /* not connected */
}

/* --- KdDebuggerNotPresent (ordinal 89, 0 args = 0 bytes) --- */
static void bridge_KdDebuggerNotPresent(void)
{
    g_eax = 1;  /* true: no debugger present */
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Ke -- shadow-table object bridges (92-163)
 *
 * The KeInitialize* family creates a real Win32 object, records guest-VA ->
 * host-HANDLE in the shadow table, and zeroes/initialises the guest dispatcher
 * header. The KePulse/Release/Reset/Set family resolves back through the
 * shadow table (with XBOX_TO_NATIVE as a fallback for legacy callers).
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- KeInitializeEvent (ordinal 108, 3 args = 12 bytes) --- */
static void bridge_KeInitializeEvent(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t event_type = STACK_ARG(1); /* 0=Notification(manual-reset), 1=Synchronization(auto-reset) */
    uint32_t initial_state = STACK_ARG(2);

    BOOL manual_reset = (event_type == 0) ? TRUE : FALSE;
    HANDLE h = CreateEventW(NULL, manual_reset, initial_state ? TRUE : FALSE, NULL);
    if (!h) {
        fprintf(stderr, "  [KERNEL] KeInitializeEvent: CreateEventW failed (error %u)\n",
                GetLastError());
        g_eax = 0;
        return;
    }

    ke_shadow_insert(guest_va, h);

    /* Also zero the guest KEVENT struct (16 bytes) and set SignalState.
     * KEVENT layout: +0x00 Type(UCHAR), +0x01 Absolute(UCHAR),
     *                +0x02 Size(UCHAR), +0x03 Inserted(UCHAR)
     *                +0x04 SignalState(LONG), +0x08 WaitListHead.Flink,
     *                +0x0C WaitListHead.Blink */
    BRIDGE_MEM8(guest_va + 0)  = (uint8_t)(event_type ? 1 : 0);
    BRIDGE_MEM8(guest_va + 1)  = 0;
    BRIDGE_MEM8(guest_va + 2)  = 4;    /* Size, in dwords */
    BRIDGE_MEM8(guest_va + 3)  = 0;
    BRIDGE_MEM32(guest_va + 4) = initial_state ? 1 : 0; /* SignalState */
    BRIDGE_MEM32(guest_va + 8)  = guest_va + 8;  /* empty WaitListHead points at itself */
    BRIDGE_MEM32(guest_va + 12) = guest_va + 8;

    g_eax = 0;
}

/* --- KeInitializeMutant (ordinal 110, 2 args = 8 bytes) --- */
static void bridge_KeInitializeMutant(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t initial_owner = STACK_ARG(1);

    HANDLE h = CreateMutexW(NULL, initial_owner ? TRUE : FALSE, NULL);
    if (!h) {
        fprintf(stderr, "  [KERNEL] KeInitializeMutant: CreateMutexW failed (error %u)\n",
                GetLastError());
        g_eax = 0;
        return;
    }

    ke_shadow_insert(guest_va, h);

    /* As the real KeInitializeMutant: Type 2, Size 8 dwords. SignalState is 1
     * when free and 0 when owned; Owner (+0x18) and Abandoned (+0x1C). */
    BRIDGE_MEM8(guest_va + 0)  = 2;
    BRIDGE_MEM8(guest_va + 1)  = 0;
    BRIDGE_MEM8(guest_va + 2)  = 8;
    BRIDGE_MEM8(guest_va + 3)  = 0;
    BRIDGE_MEM32(guest_va + 4) = initial_owner ? 0 : 1;
    BRIDGE_MEM32(guest_va + 8)  = guest_va + 8;
    BRIDGE_MEM32(guest_va + 12) = guest_va + 8;
    BRIDGE_MEM32(guest_va + 0x18) = 0;
    BRIDGE_MEM8(guest_va + 0x1C) = 0;

    g_eax = 0;
}

/* --- KeInitializeSemaphore (ordinal 112, 3 args = 12 bytes) --- */
static void bridge_KeInitializeSemaphore(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t count = STACK_ARG(1);
    uint32_t limit = STACK_ARG(2);

    HANDLE h = CreateSemaphoreW(NULL, (LONG)count, (LONG)limit, NULL);
    if (!h) {
        fprintf(stderr, "  [KERNEL] KeInitializeSemaphore: CreateSemaphoreW failed (error %u)\n",
                GetLastError());
        g_eax = 0;
        return;
    }

    ke_shadow_insert(guest_va, h);

    /* As the real KeInitializeSemaphore: Type 5, Size 5 dwords, Limit at +0x10. */
    BRIDGE_MEM8(guest_va + 0)  = 5;
    BRIDGE_MEM8(guest_va + 1)  = 0;
    BRIDGE_MEM8(guest_va + 2)  = 5;
    BRIDGE_MEM8(guest_va + 3)  = 0;
    BRIDGE_MEM32(guest_va + 4) = (LONG)count;
    BRIDGE_MEM32(guest_va + 8)  = guest_va + 8;
    BRIDGE_MEM32(guest_va + 12) = guest_va + 8;
    BRIDGE_MEM32(guest_va + 0x10) = limit;

    g_eax = 0;
}

/* --- KePulseEvent (ordinal 123, 3 args = 12 bytes) --- */
static void bridge_KePulseEvent(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t increment = STACK_ARG(1);
    uint32_t wait = STACK_ARG(2);
    HANDLE h;

    (void)increment;
    (void)wait;

    {
        int type;
        HANDLE ge = ke_guest_event(guest_va, &type);
        if (ge) {
                    g_eax = BRIDGE_MEM32(guest_va + 4);   /* previous state */
        PulseEvent(ge);
        BRIDGE_MEM32(guest_va + 4) = 0;
            return;
        }
    }
    h = ke_shadow_lookup(guest_va);
    if (h)
        BRIDGE_MEM32(guest_va + 4) = 0;
    if (!h)
        h = XBOX_TO_NATIVE(guest_va);
    g_eax = h ? bridge_event_state(h) : 0;   /* previous state */
    if (h)
        PulseEvent(h);
}

/* --- KeReleaseMutant (ordinal 131, 4 args = 16 bytes) --- */
static void bridge_KeReleaseMutant(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t increment = STACK_ARG(1);
    uint32_t wait = STACK_ARG(2);
    uint32_t abandoned = STACK_ARG(3);
    HANDLE h;

    (void)increment;
    (void)wait;
    (void)abandoned;

    h = ke_shadow_lookup(guest_va);
    if (!h)
        h = XBOX_TO_NATIVE(guest_va);
    if (h)
        ReleaseMutex(h);

    g_eax = 0;
}

/* --- KeReleaseSemaphore (ordinal 132, 4 args = 16 bytes) --- */
static void bridge_KeReleaseSemaphore(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t increment = STACK_ARG(1);
    uint32_t adjustment = STACK_ARG(2);  /* LONG */
    uint32_t wait = STACK_ARG(3);
    HANDLE h;

    (void)increment;
    (void)wait;

    h = ke_shadow_lookup(guest_va);
    if (!h)
        h = XBOX_TO_NATIVE(guest_va);
    {
        LONG prev = 0;
        if (h && ReleaseSemaphore(h, (LONG)adjustment, &prev))
            InterlockedExchangeAdd((volatile LONG *)&BRIDGE_MEM32(guest_va + 4), (LONG)adjustment);
        else if (h && bridge_seh_enabled())
            bridge_raise_noncontinuable(0xC0000047u, 0);   /* past its limit: raised, as the kernel */
        g_eax = (uint32_t)prev;   /* previous count */
    }
}

/* --- KeResetEvent (ordinal 138, 1 arg = 4 bytes) --- */
static void bridge_KeResetEvent(void)
{
    uint32_t guest_va = STACK_ARG(0);
    HANDLE h;

    {
        int type;
        HANDLE ge = ke_guest_event(guest_va, &type);
        if (ge) {
                    g_eax = BRIDGE_MEM32(guest_va + 4);   /* previous state */
        BRIDGE_MEM32(guest_va + 4) = 0;
        ResetEvent(ge);
            return;
        }
    }
    h = ke_shadow_lookup(guest_va);
    if (h)
        BRIDGE_MEM32(guest_va + 4) = 0;
    if (!h)
        h = XBOX_TO_NATIVE(guest_va);
        g_eax = h ? bridge_event_state(h) : 0;   /* previous state */
    if (h)
        ResetEvent(h);
}

/* --- KeResumeThread (ordinal 140, 1 arg = 4 bytes) --- */
static void bridge_KeResumeThread(void)
{
    /* The real one takes a KTHREAD: our stand-in object first. */
    HANDLE hThread = bridge_thread_host(STACK_ARG(0), NULL);
    if (!hThread)
        hThread = bridge_resolve_handle(STACK_ARG(0));
    if (!hThread)
        hThread = XBOX_TO_NATIVE(STACK_ARG(0));

    if (hThread)
        g_eax = (uint32_t)ResumeThread(hThread);
    else
        g_eax = 0;
}

/* --- KeSuspendThread (ordinal 152, 1 arg = 4 bytes) --- */
static void bridge_KeSuspendThread(void)
{
    /* The real one takes a KTHREAD: our stand-in object first. */
    HANDLE hThread = bridge_thread_host(STACK_ARG(0), NULL);
    if (!hThread)
        hThread = bridge_resolve_handle(STACK_ARG(0));
    if (!hThread)
        hThread = XBOX_TO_NATIVE(STACK_ARG(0));

    if (hThread)
        g_eax = (uint32_t)SuspendThread(hThread);
    else
        g_eax = 0;
}

/* --- KeAlertResumeThread (ordinal 92, 1 arg = 4 bytes) --- */
static void bridge_KeAlertResumeThread(void)
{
    /* Alerts and resumes: the alert has no host equal for a suspended thread. */
    HANDLE h = bridge_thread_host(STACK_ARG(0), NULL);
    g_eax = h ? (uint32_t)ResumeThread(h) : 0;
}

/* --- KeBoostPriorityThread (ordinal 94, 2 args = 8 bytes) --- */
static void bridge_KeBoostPriorityThread(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

/* --- KeEnterCriticalRegion (ordinal 101, 0 args = 0 bytes) --- */
static void bridge_KeEnterCriticalRegion(void)
{
    g_eax = 0;
}

/* --- KeInitializeApc (ordinal 105, 6 args = 24 bytes) --- */
static void bridge_KeInitializeApc(void)
{
    uint32_t apc_va = STACK_ARG(0);

    if (apc_va) {
        BRIDGE_MEM8(apc_va + 0)  = 0;
        BRIDGE_MEM8(apc_va + 1)  = 0;
        BRIDGE_MEM8(apc_va + 2)  = 0;
        BRIDGE_MEM8(apc_va + 3)  = 0;
        BRIDGE_MEM32(apc_va + 4) = 0;
        BRIDGE_MEM32(apc_va + 8) = 0;
        BRIDGE_MEM32(apc_va + 12) = 0;
        BRIDGE_MEM32(apc_va + 16) = 0;
        BRIDGE_MEM32(apc_va + 20) = 0;
    }
    g_eax = 0;
}

/* --- KeInitializeDeviceQueue (ordinal 106, 1 arg = 4 bytes) --- */

/* --- KeInitializeQueue (ordinal 111, 2 args = 8 bytes) --- */
/* KQUEUE: Type 4, Size 10 dwords, SignalState +4 (entries waiting), WaitList +8,
 * EntryList +0x10, CurrentCount +0x18, MaximumCount +0x1C, ThreadList +0x20, as the
 * kernel lays it out. A host semaphore counts the entries for waiters. Not modelled:
 * the kernel holding back waiters once MaximumCount threads run. */
static CRITICAL_SECTION g_queue_cs;
static INIT_ONCE g_queue_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK queue_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&g_queue_cs);
    return TRUE;
}
#define QUEUE_LOCK()   (InitOnceExecuteOnce(&g_queue_once, queue_init, NULL, NULL), EnterCriticalSection(&g_queue_cs))
#define QUEUE_UNLOCK() LeaveCriticalSection(&g_queue_cs)

static void queue_insert(int at_head)
{
    uint32_t q = STACK_ARG(0), e = STACK_ARG(1), head = q + 0x10, prev;
    HANDLE h = ke_shadow_lookup(q);
    QUEUE_LOCK();
    prev = BRIDGE_MEM32(q + 4);
    BRIDGE_MEM32(q + 4) = prev + 1;
    if (at_head) {
        uint32_t next = BRIDGE_MEM32(head);
        BRIDGE_MEM32(e) = next;
        BRIDGE_MEM32(e + 4) = head;
        BRIDGE_MEM32(next + 4) = e;
        BRIDGE_MEM32(head) = e;
    } else {
        uint32_t last = BRIDGE_MEM32(head + 4);
        BRIDGE_MEM32(e) = head;
        BRIDGE_MEM32(e + 4) = last;
        BRIDGE_MEM32(last) = e;
        BRIDGE_MEM32(head + 4) = e;
    }
    QUEUE_UNLOCK();
    if (h) ReleaseSemaphore(h, 1, NULL);
    g_eax = prev;
}

static void bridge_KeInitializeQueue(void)
{
    uint32_t q = STACK_ARG(0);
    uint32_t count = STACK_ARG(1);

    g_eax = 0;
    if (!q) return;
    BRIDGE_MEM8(q + 0) = 4;
    BRIDGE_MEM8(q + 2) = 0x0A;
    BRIDGE_MEM32(q + 4) = 0;
    BRIDGE_MEM32(q + 8) = q + 8;
    BRIDGE_MEM32(q + 0xC) = q + 8;
    BRIDGE_MEM32(q + 0x10) = q + 0x10;
    BRIDGE_MEM32(q + 0x14) = q + 0x10;
    BRIDGE_MEM32(q + 0x18) = 0;
    BRIDGE_MEM32(q + 0x1C) = count ? count : 1;
    BRIDGE_MEM32(q + 0x20) = q + 0x20;
    BRIDGE_MEM32(q + 0x24) = q + 0x20;
    ke_shadow_insert(q, CreateSemaphoreW(NULL, 0, 0x7FFFFFFF, NULL));
}

/* --- KeInsertByKeyDeviceQueue (ordinal 114, 3 args = 12 bytes) --- */

/* --- KeInsertDeviceQueue (ordinal 115, 2 args = 8 bytes) --- */

/* --- KeInsertHeadQueue (ordinal 116, 2 args = 8 bytes) --- */
static void bridge_KeInsertHeadQueue(void)
{
    queue_insert(1);
}

/* --- KeInsertQueue (ordinal 117, 2 args = 8 bytes) --- */
static void bridge_KeInsertQueue(void)
{
    queue_insert(0);
}

/* --- KeInsertQueueApc (ordinal 118, 4 args = 16 bytes) --- */
static void bridge_KeInsertQueueApc(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    g_eax = 1;
}

/* --- KeInsertQueueDpc (ordinal 119) ---
 * BOOLEAN KeInsertQueueDpc(PKDPC Dpc, PVOID SystemArgument1,
 *                          PVOID SystemArgument2)
 *
 * Enqueues a DPC for the timer thread to drain. See the queue above. */

/* --- KeIsExecutingDpc (ordinal 121, 0 args = 0 bytes) --- */
static void bridge_KeIsExecutingDpc(void)
{
    g_eax = t_in_dpc;
}

/* --- KeLeaveCriticalRegion (ordinal 122, 0 args = 0 bytes) --- */
static void bridge_KeLeaveCriticalRegion(void)
{
    g_eax = 0;
}

/* --- KeRaiseIrqlToSynchLevel (ordinal 130, 0 args = 0 bytes) --- */
static void bridge_KeRaiseIrqlToSynchLevel(void)
{
    g_eax = (uint32_t)xbox_KfRaiseIrql(0x1C);   /* SYNCH_LEVEL, returns the old one */
}

/* --- KeRemoveByKeyDeviceQueue (ordinal 133, 2 args = 8 bytes) --- */

/* --- KeRemoveDeviceQueue (ordinal 134, 1 arg = 4 bytes) --- */

/* --- KeRemoveEntryDeviceQueue (ordinal 135, 2 args = 8 bytes) --- */
/* ── The I/O manager's requests ──────────────────────────────
 *
 * The kernel's IRPs, device objects and device queues, in guest memory and in the kernel's layout, so a driver in a
 * title sees what it would on a console. The host answers files itself, so no request of ours passes through here.
 *
 * Calling a title's driver routines is behind RECOMP_IO_REQUESTS=1: those functions did nothing until now, and titles
 * that import them may hold driver code that expects hardware. Off, they behave as before.
 *
 * IRP: +0 Type (6), +2 Size, +4 Flags, +8 ThreadListEntry, +0x10 IoStatus, +0x18 StackCount, +0x19 CurrentLocation,
 * +0x1A PendingReturned, +0x1C UserIosb, +0x20 UserEvent, +0x28 UserApc routine, +0x2C its context, +0x30 UserBuffer,
 * +0x38 locked length, +0x3C DeviceQueueEntry, +0x50 Thread, +0x5C CurrentStackLocation, +0x60 FileObject; stack
 * locations of 0x28 bytes follow from +0x68.
 * Stack location: +0 Major, +1 Minor, +3 Control, +4..+0x13 Parameters, +0x18 DeviceObject, +0x1C FileObject,
 * +0x20 CompletionRoutine, +0x24 its context.
 * Device: +4 ReferenceCount, +8 DriverObject, +0xC mounted volume, +0x10 CurrentIrp, +0x14 Flags, +0x18 Extension,
 * +0x1C DeviceType, +0x1D start-I/O flags, +0x1E StackSize, +0x1F DeletePending, +0x28 DeviceQueue, +0x34 lock event.
 * Driver: +0 StartIo, +4 DeleteDevice, +8 Dismount, +0xC MajorFunction[]. */
#define IRP_STACK_SIZE 0x28u

static int io_requests_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("RECOMP_IO_REQUESTS"); on = e && *e == '1'; }
    return on;
}

/* Calls stdcall guest code; its result */
static uint32_t bridge_call_guest(uint32_t routine, int argc, const uint32_t *argv)
{
    recomp_func_t fn = recomp_lookup(routine);
    uint32_t esp = g_esp, r;
    if (!fn) fn = recomp_lookup_manual(routine);
    if (!fn) fn = recomp_lookup_kernel(routine);
    if (!fn) {
        fprintf(stderr, "  [KERNEL] driver routine 0x%08X not in dispatch\n", routine);
        return 0xC0000001u;
    }
    for (int i = argc - 1; i >= 0; i--) { g_esp -= 4; BRIDGE_MEM32(g_esp) = argv[i]; }
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
    fn();
    r = g_eax;
    g_esp = esp;
    return r;
}

static void bridge_set_event_va(uint32_t va)
{
    int type;
    HANDLE h = ke_guest_event(va, &type);
    if (h) BRIDGE_MEM32(va + 4) = 1;
    else h = ke_shadow_lookup(va);
    if (h) SetEvent(h);
}

static void irp_init(uint32_t irp, uint32_t size, uint32_t stack)
{
    memset(XBOX_TO_NATIVE(irp), 0, size);
    BRIDGE_MEM16(irp) = 6;
    BRIDGE_MEM16(irp + 2) = (uint16_t)size;
    BRIDGE_MEM32(irp + 8) = irp + 8;
    BRIDGE_MEM32(irp + 0xC) = irp + 8;
    BRIDGE_MEM8(irp + 0x18) = (uint8_t)stack;
    BRIDGE_MEM8(irp + 0x19) = (uint8_t)(stack + 1);
    BRIDGE_MEM32(irp + 0x5C) = irp + 0x68 + (int32_t)(int8_t)stack * IRP_STACK_SIZE;
}

static uint32_t irp_alloc(uint32_t stack)
{
    uint32_t size = ((int32_t)(int8_t)stack * IRP_STACK_SIZE + 0x68) & 0xFFFF;
    uint32_t irp = pool_alloc(size);
    if (irp) irp_init(irp, size, stack);
    return irp;
}

static void irp_free(uint32_t irp)
{
    if (BRIDGE_MEM16(irp) != 6) {
        fprintf(stderr, "  [KERNEL] IoFreeIrp: 0x%08X is not an IRP (bug check 0x44 on a console)\n", irp);
        bridge_bugcheck_end(0x44);
        return;
    }
    BRIDGE_MEM16(irp) = 0;
    xbox_HeapFree(irp);
}

static void bridge_IoAllocateIrp(void) { g_eax = irp_alloc(STACK_ARG(0) & 0xFF); }

static void bridge_IoInitializeIrp(void) { irp_init(STACK_ARG(0), STACK_ARG(1) & 0xFFFF, STACK_ARG(2) & 0xFF); }

static void bridge_IoFreeIrp(void) { irp_free(STACK_ARG(0)); }

static void bridge_IoMarkIrpMustComplete(void)
{
    uint32_t stack = BRIDGE_MEM32(STACK_ARG(0) + 0x5C);
    BRIDGE_MEM8(stack + 3) |= 2;
}

/* The request's stack location for the next driver; calls its routine for this major function */
static uint32_t irp_call_driver(uint32_t device, uint32_t irp)
{
    uint32_t stack, argv[2];
    int8_t loc = (int8_t)(BRIDGE_MEM8(irp + 0x19) - 1);
    BRIDGE_MEM8(irp + 0x19) = (uint8_t)loc;
    if (loc <= 0) {
        fprintf(stderr, "  [KERNEL] IofCallDriver: IRP 0x%08X out of stack locations (bug check 0x35 on a console)\n", irp);
        bridge_bugcheck_end(0x35);
        return 0xC0000001u;
    }
    stack = BRIDGE_MEM32(irp + 0x5C) - IRP_STACK_SIZE;
    BRIDGE_MEM32(irp + 0x5C) = stack;
    BRIDGE_MEM32(stack + 0x18) = device;
    argv[0] = device, argv[1] = irp;
    return bridge_call_guest(BRIDGE_MEM32(BRIDGE_MEM32(device + 8) + 0xC + BRIDGE_MEM8(stack) * 4), 2, argv);
}

static void bridge_IofCallDriver(void)
{
    g_eax = io_requests_on() ? irp_call_driver(g_ecx, g_edx) : 0;
}

/* Unwinds the stack locations, calling completion routines, then finishes the request for its caller */
static void irp_complete(uint32_t irp)
{
    uint8_t count = BRIDGE_MEM8(irp + 0x18) + 1;
    int8_t loc = (int8_t)BRIDGE_MEM8(irp + 0x19);
    uint32_t stack, flags;

    if (loc > (int8_t)count || BRIDGE_MEM16(irp) != 6) {
        fprintf(stderr, "  [KERNEL] IofCompleteRequest: bad IRP 0x%08X (bug check 0x44 on a console)\n", irp);
        bridge_bugcheck_end(0x44);
        return;
    }
    stack = BRIDGE_MEM32(irp + 0x5C);
    loc++;
    BRIDGE_MEM8(irp + 0x19) = (uint8_t)loc;
    BRIDGE_MEM32(irp + 0x5C) = stack + IRP_STACK_SIZE;
    while (loc <= (int8_t)count) {
        uint8_t control = BRIDGE_MEM8(stack + 3);
        int status_bad = (int32_t)BRIDGE_MEM32(irp + 0x10) < 0;
        BRIDGE_MEM8(irp + 0x1A) = control & 1;
        if (status_bad ? (control & 0x80) : (control & 0x40)) {
            uint32_t routine = BRIDGE_MEM32(stack + 0x20), argv[3];
            argv[0] = loc == (int8_t)count ? 0 : BRIDGE_MEM32(BRIDGE_MEM32(irp + 0x5C) + 0x18);
            argv[1] = irp;
            argv[2] = BRIDGE_MEM32(stack + 0x24);
            memset(XBOX_TO_NATIVE(stack), 0, 0x14);
            BRIDGE_MEM32(stack + 0x1C) = 0;
            if (bridge_call_guest(routine, 3, argv) == 0xC0000016u)  /* STATUS_MORE_PROCESSING_REQUIRED */
                return;
        } else {
            if ((control & 1) && loc <= (int8_t)BRIDGE_MEM8(irp + 0x18))
                BRIDGE_MEM8(BRIDGE_MEM32(irp + 0x5C) + 3) |= 1;
            memset(XBOX_TO_NATIVE(stack), 0, 0x14);
            BRIDGE_MEM32(stack + 0x1C) = 0;
        }
        BRIDGE_MEM32(irp + 0x5C) += IRP_STACK_SIZE;
        stack += IRP_STACK_SIZE;
        loc++;
        BRIDGE_MEM8(irp + 0x19) = (uint8_t)loc;
    }
    flags = BRIDGE_MEM32(irp + 4);
    if (flags & 0x42) {  /* paging or associated: the status and event, freed if asked */
        uint32_t iosb = BRIDGE_MEM32(irp + 0x1C);
        BRIDGE_MEM32(iosb) = BRIDGE_MEM32(irp + 0x10);
        BRIDGE_MEM32(iosb + 4) = BRIDGE_MEM32(irp + 0x14);
        bridge_set_event_va(BRIDGE_MEM32(irp + 0x20));
        if (flags & 2) irp_free(irp);
        return;
    }
    if (flags & 0x200) BRIDGE_MEM8(irp + 5) &= 0xFD;  /* pages stay locked here: nothing to unlock */
    if ((flags & 0x80) && !BRIDGE_MEM8(irp + 0x1A))   /* deferred, not pending: the caller finishes it */
        return;
    /* What the kernel does in a kernel APC on the requesting thread. No request here has a file object. */
    {
        uint32_t iosb = BRIDGE_MEM32(irp + 0x1C), event = BRIDGE_MEM32(irp + 0x20), apc = BRIDGE_MEM32(irp + 0x28);
        if (iosb) {
            BRIDGE_MEM32(iosb) = BRIDGE_MEM32(irp + 0x10);
            BRIDGE_MEM32(iosb + 4) = BRIDGE_MEM32(irp + 0x14);
        }
        if (event) bridge_set_event_va(event);
        if (apc) bridge_call_user_apc(apc, BRIDGE_MEM32(irp + 0x2C), iosb, 0);
        irp_free(irp);
    }
}

static void bridge_IofCompleteRequest(void)
{
    if (io_requests_on()) irp_complete(g_ecx);
}

static void bridge_IoInvalidDeviceRequest(void)
{
    uint32_t irp = STACK_ARG(1);
    BRIDGE_MEM32(irp + 0x10) = 0xC0000010u;
    if (io_requests_on()) irp_complete(irp);
    g_eax = 0xC0000010u;
}

/* Share access: the access in the file object's flags (+2: 2 read, 4 write, 8 delete, 0x10.. shared), counts in the
 * record: OpenCount, Readers, Writers, Deleters, SharedRead, SharedWrite, SharedDelete */
static uint8_t share_flags(uint32_t access, uint32_t share, uint32_t file)
{
    uint8_t f = (uint8_t)(((access & 0x21) ? 2 : 0) | ((access & 6) ? 4 : 0) | ((access & 0x10000) ? 8 : 0));
    f |= BRIDGE_MEM8(file + 2) & 0xF1;
    BRIDGE_MEM8(file + 2) = f;
    if (f & 0xE) {
        f = (uint8_t)((f & 0x8F) | ((share & 7) << 4));
        BRIDGE_MEM8(file + 2) = f;
    }
    return f;
}

static void bridge_IoCheckShareAccess(void)
{
    uint32_t file = STACK_ARG(2), rec = STACK_ARG(3), update = STACK_ARG(4) & 0xFF;
    uint8_t f = share_flags(STACK_ARG(0), STACK_ARG(1), file), open;
    g_eax = 0;
    if (!(f & 0xE)) return;
    open = BRIDGE_MEM8(rec);
    if (open == 0xFF ||
        ((f & 2) && BRIDGE_MEM8(rec + 4) < open) || ((f & 4) && BRIDGE_MEM8(rec + 5) < open) ||
        ((f & 8) && BRIDGE_MEM8(rec + 6) < open) ||
        (BRIDGE_MEM8(rec + 1) && !(f & 0x10)) || (BRIDGE_MEM8(rec + 2) && !(f & 0x20)) ||
        (BRIDGE_MEM8(rec + 3) && !(f & 0x40))) {
        g_eax = 0xC0000043u;   /* STATUS_SHARING_VIOLATION */
        return;
    }
    if (update) {
        BRIDGE_MEM8(rec) = open + 1;
        for (int i = 0; i < 6; i++)
            BRIDGE_MEM8(rec + 1 + i) += (f >> (i + 1)) & 1;
    }
}

static void bridge_IoSetShareAccess(void)
{
    uint32_t rec = STACK_ARG(3);
    uint8_t f = share_flags(STACK_ARG(0), STACK_ARG(1), STACK_ARG(2));
    BRIDGE_MEM8(rec) = (f & 0xE) ? 1 : 0;
    for (int i = 0; i < 6; i++)
        BRIDGE_MEM8(rec + 1 + i) = (f & 0xE) ? (f >> (i + 1)) & 1 : 0;
}

static void bridge_IoRemoveShareAccess(void)
{
    uint8_t f = BRIDGE_MEM8(STACK_ARG(0) + 2);
    uint32_t rec = STACK_ARG(1);
    if (!(f & 0xE)) return;
    BRIDGE_MEM8(rec) -= 1;
    for (int i = 0; i < 6; i++)
        if ((f >> (i + 1)) & 1) BRIDGE_MEM8(rec + 1 + i) -= 1;
}

/* Device queue: +0 Type (0x14), +2 Size (0xC), +3 Busy, +4 list head. Entry: +0 list, +8 key, +0xC Inserted. */
static uint32_t devq_insert(uint32_t q, uint32_t e)
{
    uint32_t r = 0;
    ex_lock();
    if (BRIDGE_MEM8(q + 3) == 1) {
        uint32_t head = q + 4, last = BRIDGE_MEM32(head + 4);
        BRIDGE_MEM32(e) = head;
        BRIDGE_MEM32(e + 4) = last;
        BRIDGE_MEM32(last) = e;
        BRIDGE_MEM32(head + 4) = e;
        r = 1;
    } else {
        BRIDGE_MEM8(q + 3) = 1;
    }
    BRIDGE_MEM8(e + 0xC) = (uint8_t)r;
    ex_unlock();
    return r;
}

static uint32_t devq_insert_key(uint32_t q, uint32_t e, uint32_t key)
{
    uint32_t r = 0;
    ex_lock();
    BRIDGE_MEM32(e + 8) = key;
    if (BRIDGE_MEM8(q + 3) == 1) {
        uint32_t head = q + 4, at = BRIDGE_MEM32(head), prev;
        while (at != head && key >= BRIDGE_MEM32(at + 8)) at = BRIDGE_MEM32(at);
        prev = BRIDGE_MEM32(at + 4);
        BRIDGE_MEM32(e) = at;
        BRIDGE_MEM32(e + 4) = prev;
        BRIDGE_MEM32(prev) = e;
        BRIDGE_MEM32(at + 4) = e;
        r = 1;
    } else {
        BRIDGE_MEM8(q + 3) = 1;
    }
    BRIDGE_MEM8(e + 0xC) = (uint8_t)r;
    ex_unlock();
    return r;
}

static void devq_unlink(uint32_t e)
{
    uint32_t n = BRIDGE_MEM32(e), p = BRIDGE_MEM32(e + 4);
    BRIDGE_MEM32(p) = n;
    BRIDGE_MEM32(n + 4) = p;
    BRIDGE_MEM8(e + 0xC) = 0;
}

static uint32_t devq_remove(uint32_t q)
{
    uint32_t head = q + 4, e;
    ex_lock();
    e = BRIDGE_MEM32(head);
    if (e == head) { BRIDGE_MEM8(q + 3) = 0; e = 0; }
    else devq_unlink(e);
    ex_unlock();
    return e;
}

static uint32_t devq_remove_key(uint32_t q, uint32_t key)
{
    uint32_t head = q + 4, e, first;
    ex_lock();
    first = e = BRIDGE_MEM32(head);
    if (e == head) {
        BRIDGE_MEM8(q + 3) = 0;
        e = 0;
    } else {
        while (e != head && key > BRIDGE_MEM32(e + 8)) e = BRIDGE_MEM32(e);
        if (e == head) e = first;   /* none that far along: the first */
        devq_unlink(e);
    }
    ex_unlock();
    return e;
}

static void bridge_KeInitializeDeviceQueue(void)
{
    uint32_t q = STACK_ARG(0);
    BRIDGE_MEM16(q) = 0x14;
    BRIDGE_MEM8(q + 2) = 0xC;
    BRIDGE_MEM8(q + 3) = 0;
    BRIDGE_MEM32(q + 4) = q + 4;
    BRIDGE_MEM32(q + 8) = q + 4;
}

static void bridge_KeInsertDeviceQueue(void) { g_eax = devq_insert(STACK_ARG(0), STACK_ARG(1)); }
static void bridge_KeInsertByKeyDeviceQueue(void) { g_eax = devq_insert_key(STACK_ARG(0), STACK_ARG(1), STACK_ARG(2)); }
static void bridge_KeRemoveDeviceQueue(void) { g_eax = devq_remove(STACK_ARG(0)); }
static void bridge_KeRemoveByKeyDeviceQueue(void) { g_eax = devq_remove_key(STACK_ARG(0), STACK_ARG(1)); }

static void bridge_KeRemoveEntryDeviceQueue(void)
{
    uint32_t e = STACK_ARG(0);
    uint8_t was;
    ex_lock();
    was = BRIDGE_MEM8(e + 0xC);
    if (was == 1) devq_unlink(e);
    ex_unlock();
    g_eax = was;
}

/* Starting packets: the driver's StartIo runs for one request at a time; one finishing inside StartIo is held until
 * StartIo returns (+0x1D: 1 in StartIo, 2 next wanted, 4 by key, key at +0x44). */
static void device_start(uint32_t dev, uint32_t irp)
{
    uint32_t argv[2];
    BRIDGE_MEM32(dev + 0x10) = irp;
    if (!irp) return;
    BRIDGE_MEM8(dev + 0x1D) |= 1;
    argv[0] = dev, argv[1] = irp;
    bridge_call_guest(BRIDGE_MEM32(BRIDGE_MEM32(dev + 8)), 2, argv);
    BRIDGE_MEM8(dev + 0x1D) &= 0xFE;
}

static void device_start_next(uint32_t dev, int by_key, uint32_t key)
{
    uint32_t e = by_key ? devq_remove_key(dev + 0x28, key) : devq_remove(dev + 0x28);
    device_start(dev, e ? e - 0x3C : 0);
}

static void device_drain(uint32_t dev)
{
    do {
        BRIDGE_MEM8(dev + 0x1D) &= 0xFD;
        if (BRIDGE_MEM8(dev + 0x1D) & 4) device_start_next(dev, 1, BRIDGE_MEM32(dev + 0x44));
        else device_start_next(dev, 0, 0);
    } while (BRIDGE_MEM8(dev + 0x1D) & 2);
}

static void bridge_IoStartPacket(void)
{
    uint32_t dev = STACK_ARG(0), irp = STACK_ARG(1), key = STACK_ARG(2), queued;
    if (!io_requests_on()) return;
    queued = key ? devq_insert_key(dev + 0x28, irp + 0x3C, BRIDGE_MEM32(key)) : devq_insert(dev + 0x28, irp + 0x3C);
    if (!queued) {
        device_start(dev, irp);
        if (BRIDGE_MEM8(dev + 0x1D) & 2) device_drain(dev);
    }
}

static void bridge_IoStartNextPacket(void)
{
    uint32_t dev = STACK_ARG(0);
    if (!io_requests_on()) return;
    if (BRIDGE_MEM8(dev + 0x1D) & 1) {
        BRIDGE_MEM8(dev + 0x1D) = (uint8_t)((BRIDGE_MEM8(dev + 0x1D) & 0xFB) | 2);
    } else {
        device_start_next(dev, 0, 0);
        if (BRIDGE_MEM8(dev + 0x1D) & 2) device_drain(dev);
    }
}

static void bridge_IoStartNextPacketByKey(void)
{
    uint32_t dev = STACK_ARG(0), key = STACK_ARG(1);
    if (!io_requests_on()) return;
    if (BRIDGE_MEM8(dev + 0x1D) & 1) {
        BRIDGE_MEM8(dev + 0x1D) |= 6;
        BRIDGE_MEM32(dev + 0x44) = key;
    } else {
        device_start_next(dev, 1, key);
        if (BRIDGE_MEM8(dev + 0x1D) & 2) device_drain(dev);
    }
}

/* Devices. Named devices: the name is not kept (no object directory here). */
#define XBOX_DEVICE_OBJECT_SIZE 0x48u

static void bridge_IoCreateDevice(void)
{
    uint32_t ext = STACK_ARG(1), name = STACK_ARG(2), type = STACK_ARG(3), exclusive = STACK_ARG(4) & 0xFF;
    uint32_t out = STACK_ARG(5), total = ((ext + 7) & ~7u) + XBOX_DEVICE_OBJECT_SIZE, dev = pool_alloc(total), flags = 0x10;

    if (!dev) {
        if (out) BRIDGE_MEM32(out) = 0;
        g_eax = 0xC000009Au;
        return;
    }
    memset(XBOX_TO_NATIVE(dev), 0, total);
    BRIDGE_MEM16(dev) = 3;
    BRIDGE_MEM16(dev + 2) = (uint16_t)(ext + XBOX_DEVICE_OBJECT_SIZE);
    BRIDGE_MEM8(dev + 0x1C) = (uint8_t)type;
    if (type == 7 || type == 0x3A || type == 2 || type == 0x3B) {   /* a disk or file system: its lock */
        BRIDGE_MEM8(dev + 0x34) = 1;
        BRIDGE_MEM8(dev + 0x36) = 4;
        BRIDGE_MEM32(dev + 0x38) = 1;
        BRIDGE_MEM32(dev + 0x3C) = dev + 0x3C;
        BRIDGE_MEM32(dev + 0x40) = dev + 0x3C;
    } else {
        BRIDGE_MEM32(dev + 0xC) = dev;
    }
    if (exclusive) flags |= 2;
    if (name) flags |= 8;
    BRIDGE_MEM32(dev + 0x14) = flags;
    BRIDGE_MEM32(dev + 0x18) = ext ? dev + XBOX_DEVICE_OBJECT_SIZE : 0;
    BRIDGE_MEM8(dev + 0x1E) = 1;
    BRIDGE_MEM32(dev + 8) = STACK_ARG(0);
    BRIDGE_MEM16(dev + 0x28) = 0x14;
    BRIDGE_MEM8(dev + 0x2A) = 0xC;
    BRIDGE_MEM32(dev + 0x2C) = dev + 0x2C;
    BRIDGE_MEM32(dev + 0x30) = dev + 0x2C;
    if (out) BRIDGE_MEM32(out) = dev;
    g_eax = 0;
}

static void bridge_IoDeleteDevice(void)
{
    uint32_t dev = STACK_ARG(0);
    BRIDGE_MEM8(dev + 0x1F) = 1;
    if (!BRIDGE_MEM32(dev + 4)) {
        uint32_t driver = BRIDGE_MEM32(dev + 8), del = driver ? BRIDGE_MEM32(driver + 4) : 0;
        if (del && io_requests_on()) bridge_call_guest(del, 1, &dev);
        else xbox_HeapFree(dev);
    }
    g_eax = 0;
}

/* Building requests */
static uint32_t irp_build_fsd(uint32_t major, uint32_t dev, uint32_t buffer, uint32_t length, uint32_t offset_va, uint32_t iosb)
{
    uint32_t irp = irp_alloc(BRIDGE_MEM8(dev + 0x1E));
    if (irp) {
        uint32_t stack = BRIDGE_MEM32(irp + 0x5C) - IRP_STACK_SIZE;
        BRIDGE_MEM32(irp + 0x50) = bridge_thread_object(0xFFFFFFFEu);
        BRIDGE_MEM8(stack) = (uint8_t)major;
        if (major != 6 && major != 0xC) {
            BRIDGE_MEM32(irp + 0x30) = buffer;
            if ((BRIDGE_MEM32(dev + 0x14) & 4) && length) {  /* direct I/O: pages count as locked */
                BRIDGE_MEM8(irp + 5) |= 2;
                BRIDGE_MEM32(irp + 0x38) = length;
            }
            BRIDGE_MEM32(stack + 4) = length;
            BRIDGE_MEM32(stack + 0xC) = BRIDGE_MEM32(offset_va);
            BRIDGE_MEM32(stack + 0x10) = BRIDGE_MEM32(offset_va + 4);
        }
        BRIDGE_MEM32(irp + 0x1C) = iosb;
    }
    return irp;
}

static void bridge_IoBuildAsynchronousFsdRequest(void)
{
    g_eax = irp_build_fsd(STACK_ARG(0), STACK_ARG(1), STACK_ARG(2), STACK_ARG(3), STACK_ARG(4), STACK_ARG(5));
}

static void bridge_IoBuildSynchronousFsdRequest(void)
{
    uint32_t irp = irp_build_fsd(STACK_ARG(0), STACK_ARG(1), STACK_ARG(2), STACK_ARG(3), STACK_ARG(4), STACK_ARG(6));
    if (irp) BRIDGE_MEM32(irp + 0x20) = STACK_ARG(5);
    g_eax = irp;
}

static uint32_t irp_build_control(uint32_t ctl, uint32_t dev, uint32_t in, uint32_t in_len, uint32_t out, uint32_t out_len,
                                  uint32_t internal, uint32_t event, uint32_t iosb)
{
    uint32_t irp = irp_alloc(BRIDGE_MEM8(dev + 0x1E));
    if (irp) {
        uint32_t stack = BRIDGE_MEM32(irp + 0x5C) - IRP_STACK_SIZE;
        BRIDGE_MEM32(irp + 0x30) = out;
        BRIDGE_MEM8(stack) = (internal & 0xFF) ? 0xB : 0xA;
        BRIDGE_MEM32(stack + 4) = out_len;
        BRIDGE_MEM32(stack + 8) = in;
        BRIDGE_MEM32(stack + 0xC) = in_len;
        BRIDGE_MEM32(stack + 0x10) = ctl;
        if (out_len && ((ctl & 3) == 1 || (ctl & 3) == 2)) {
            BRIDGE_MEM8(irp + 5) |= 2;
            BRIDGE_MEM32(irp + 0x38) = out_len;
        }
        BRIDGE_MEM32(irp + 0x1C) = iosb;
        BRIDGE_MEM32(irp + 0x20) = event;
        BRIDGE_MEM32(irp + 0x50) = bridge_thread_object(0xFFFFFFFEu);
    }
    return irp;
}

static void bridge_IoBuildDeviceIoControlRequest(void)
{
    g_eax = irp_build_control(STACK_ARG(0), STACK_ARG(1), STACK_ARG(2), STACK_ARG(3), STACK_ARG(4), STACK_ARG(5),
                              STACK_ARG(6), STACK_ARG(7), STACK_ARG(8));
}

/* Synchronous requests wait on an event and an IO_STATUS_BLOCK in guest memory (on the kernel's stack, there). Blocks
 * are reused, never freed, since an event's address stays tied to its host event. */
struct io_wait_block { struct io_wait_block *next; uint32_t va; };
static struct io_wait_block *g_io_wait_free;

static struct io_wait_block *io_wait_get(void)
{
    struct io_wait_block *b;
    ex_lock();
    b = g_io_wait_free;
    if (b) g_io_wait_free = b->next;
    ex_unlock();
    if (!b) {
        uint32_t va = xbox_HeapAlloc(0x18, 16);
        b = va ? (struct io_wait_block *)malloc(sizeof *b) : NULL;
        if (!b) return NULL;
        b->va = va;
        memset(XBOX_TO_NATIVE(va), 0, 0x18);
        BRIDGE_MEM8(va + 2) = 4;
        BRIDGE_MEM32(va + 8) = va + 8;
        BRIDGE_MEM32(va + 0xC) = va + 8;
        ke_shadow_insert(va, CreateEventW(NULL, TRUE, FALSE, NULL));
    }
    ResetEvent(ke_shadow_lookup(b->va));
    BRIDGE_MEM32(b->va + 0x10) = 0;
    BRIDGE_MEM32(b->va + 0x14) = 0;
    return b;
}

static void io_wait_put(struct io_wait_block *b)
{
    ex_lock();
    b->next = g_io_wait_free;
    g_io_wait_free = b;
    ex_unlock();
}

static uint32_t irp_run_sync(uint32_t dev, uint32_t irp, struct io_wait_block *b)
{
    uint32_t st;
    if (!irp) return 0xC000009Au;
    st = irp_call_driver(dev, irp);
    if (st == 0x103u) {   /* STATUS_PENDING */
        WaitForSingleObject(ke_shadow_lookup(b->va), INFINITE);
        st = BRIDGE_MEM32(b->va + 0x10);
    }
    return st;
}

static void bridge_IoSynchronousFsdRequest(void)
{
    struct io_wait_block *b;
    uint32_t dev = STACK_ARG(1), irp;
    if (!io_requests_on()) { g_eax = 0; return; }
    b = io_wait_get();
    if (!b) { g_eax = 0xC000009Au; return; }
    irp = irp_build_fsd(STACK_ARG(0), dev, STACK_ARG(2), STACK_ARG(3), STACK_ARG(4), b->va + 0x10);
    if (irp) BRIDGE_MEM32(irp + 0x20) = b->va;
    g_eax = irp_run_sync(dev, irp, b);
    io_wait_put(b);
}

static void bridge_IoSynchronousDeviceIoControlRequest(void)
{
    /* Control, Device, In, InLen, Out, OutLen, Returned, Internal */
    struct io_wait_block *b;
    uint32_t dev = STACK_ARG(1), returned = STACK_ARG(6), irp, st;
    if (!io_requests_on()) { g_eax = 0; return; }
    b = io_wait_get();
    if (!b) { g_eax = 0xC000009Au; return; }
    irp = irp_build_control(STACK_ARG(0), dev, STACK_ARG(2), STACK_ARG(3), STACK_ARG(4), STACK_ARG(5), STACK_ARG(7),
                            b->va, b->va + 0x10);
    st = irp_run_sync(dev, irp, b);
    if (returned) BRIDGE_MEM32(returned) = BRIDGE_MEM32(b->va + 0x14);
    io_wait_put(b);
    g_eax = st;
}


/* --- KeRemoveQueue (ordinal 136, 1 arg = 4 bytes) --- */
static void bridge_KeRemoveQueue(void)
{
    /* Queue, WaitMode, Timeout: the entry, or a wait status in its place. */
    uint32_t q = STACK_ARG(0), head = q + 0x10;
    HANDLE h = ke_shadow_lookup(q);
    NTSTATUS st;
    uint32_t e;

    if (!h) { g_eax = 0x102u; return; }   /* STATUS_TIMEOUT */
    st = xbox_KeWaitForSingleObject(h, 0, (KPROCESSOR_MODE)STACK_ARG(1), FALSE,
                                    STACK_ARG(2) ? XBOX_TO_NATIVE(STACK_ARG(2)) : NULL);
    if (st != 0) { g_eax = (uint32_t)st; return; }
    QUEUE_LOCK();
    e = BRIDGE_MEM32(head);
    {
        uint32_t next = BRIDGE_MEM32(e);
        BRIDGE_MEM32(head) = next;
        BRIDGE_MEM32(next + 4) = head;
    }
    BRIDGE_MEM32(q + 4) = BRIDGE_MEM32(q + 4) - 1;
    QUEUE_UNLOCK();
    g_eax = e;
}

/* --- KeRemoveQueueDpc (ordinal 137, 1 arg = 4 bytes) ---
 * BOOLEAN KeRemoveQueueDpc(PKDPC Dpc)
 *
 * Removes a queued DPC. See the queue above. */

/* --- KeRundownQueue (ordinal 141, 1 arg = 4 bytes) --- */
static void bridge_KeRundownQueue(void)
{
    /* Hands back what is left: the entries still linked, without the list head. */
    uint32_t q = STACK_ARG(0), head = q + 0x10, first;
    HANDLE h = ke_shadow_lookup(q);
    QUEUE_LOCK();
    first = BRIDGE_MEM32(head);
    if (first == head) {
        first = 0;
    } else {
        uint32_t last = BRIDGE_MEM32(head + 4);
        BRIDGE_MEM32(last) = first;
        BRIDGE_MEM32(first + 4) = last;
    }
    QUEUE_UNLOCK();
    if (h) while (WaitForSingleObject(h, 0) == WAIT_OBJECT_0) { }
    g_eax = first;
}

/* --- KeSetEventBoostPriority (ordinal 146, 2 args = 8 bytes) --- */
static void bridge_KeSetEventBoostPriority(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t increment = STACK_ARG(1);
    HANDLE h;

    (void)increment;

    {   /* An event the title built itself, as in KeSetEvent */
        int type;
        HANDLE ge = ke_guest_event(guest_va, &type);
        if (ge) {
            BRIDGE_MEM32(guest_va + 4) = 1;
            SetEvent(ge);
            g_eax = 0;
            return;
        }
    }
    h = ke_shadow_lookup(guest_va);
    if (h)
        BRIDGE_MEM32(guest_va + 4) = 1;
    if (!h)
        h = XBOX_TO_NATIVE(guest_va);
    if (h)
        SetEvent(h);

    g_eax = 0;
}

/* --- KeSetPriorityProcess (ordinal 147, 2 args = 8 bytes) --- */
static void bridge_KeSetPriorityProcess(void)
{
    /* The previous base priority, as the kernel. Threads keep theirs: the host's are ours to choose. */
    static LONG base = 8;
    g_eax = (uint32_t)InterlockedExchange(&base, (LONG)(int8_t)STACK_ARG(1));
}

/* --- KeSetPriorityThread (ordinal 148, 2 args = 8 bytes) --- */
static void bridge_KeSetPriorityThread(void)
{
    /* Absolute priority: Xbox normal is 8, realtime starts at 16. */
    uint32_t obj = STACK_ARG(0);
    g_eax = 8;
    if (bridge_thread_host(obj, NULL)) {
        g_eax = (uint32_t)KTHREAD_PRIORITY(obj);   /* the old one, as the kernel */
        BRIDGE_MEM8(obj + 0x32) = (uint8_t)STACK_ARG(1);
        bridge_apply_priority("KeSetPriorityThread", obj, (LONG)STACK_ARG(1) - 8);
    }
}

/* --- KeTestAlertThread (ordinal 155, 1 arg = 4 bytes) --- */
static void bridge_KeTestAlertThread(void)
{
    /* Runs what is queued for this thread; TRUE if anything was. */
    g_eax = SleepEx(0, TRUE) == WAIT_IO_COMPLETION;
}

/* --- KiBugCheckData (ordinal 162, 0 args = 0 bytes) --- */
static void bridge_KiBugCheckData(void)
{
    g_eax = XBOX_KERNEL_DATA_BASE + 0x500;
}

/* --- KiUnlockDispatcherDatabase (ordinal 163, 1 arg = 4 bytes) --- */
static void bridge_KiUnlockDispatcherDatabase(void)
{
    xbox_KfLowerIrql((KIRQL)g_ecx);   /* fastcall: the IRQL to go back to, in CL */
    g_eax = 0;
}

/* --- KeGetCurrentIrql (ordinal 103, 0 args = 0 bytes)
 * Stack-based with 0 args (not the Kf* fastcall form). IRQL is unmounted, so
 * report PASSIVE_LEVEL. */
static void bridge_KeGetCurrentIrql(void)
{
    g_eax = (uint32_t)xbox_CurrentIrql();
}

/* --- KeGetCurrentThread (ordinal 104, 0 args = 0 bytes) --- */
static void bridge_KeGetCurrentThread(void)
{
    /* The stand-in KTHREAD, the same one ObReferenceObjectByHandle gives. */
    g_eax = bridge_thread_object(0xFFFFFFFEu);
}

/* --- KeSetDisableBoostThread (ordinal 144, 2 args = 8 bytes) --- */
static void bridge_KeSetDisableBoostThread(void)
{
    /* The old setting. There is no boost here to disable. */
    uint32_t obj = STACK_ARG(0);
    g_eax = 0;
    if (bridge_thread_host(obj, NULL)) {
        g_eax = BRIDGE_MEM8(obj + 0x73);
        BRIDGE_MEM8(obj + 0x73) = (uint8_t)STACK_ARG(1);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Mm (174, 374, 375, 376, 377, 378)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- MmIsAddressValid (ordinal 174, 1 arg = 4 bytes) --- */
static void bridge_MmIsAddressValid(void)
{
    /* The real one reads its page tables. Here: what is mapped for the guest. */
    uint32_t a = STACK_ARG(0);
    uint64_t ram = g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram;
    uint32_t vi[7];
    if (a >= 0x10000u && a < ram)
        g_eax = 1;
    else if ((a >= 0x80000000u && a < 0x84000000u) || a >= 0xFD000000u)
        g_eax = 1;
    else
        g_eax = guest_vmem_query(a, vi) && vi[4] == 0x1000u;
}

/* --- MmDbgAllocateMemory (ordinal 374, 2 args = 8 bytes)
 * PVOID MmDbgAllocateMemory(ULONG Size, ULONG AllocationTag);
 * Allocates zeroed guest memory. */
static void bridge_MmDbgAllocateMemory(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t tag = STACK_ARG(1);
    uint32_t va;

    (void)tag;
    if (!size) {
        g_eax = 0;
        return;
    }
    va = xbox_HeapAlloc(size, 16);
    if (va)
        memset((void *)((uintptr_t)va + g_xbox_mem_offset), 0, size);
    g_eax = va;
}

/* --- MmDbgFreeMemory (ordinal 375, 2 args = 8 bytes)
 * VOID MmDbgFreeMemory(PVOID BaseAddress, ULONG Size); */
static void bridge_MmDbgFreeMemory(void)
{
    (void)STACK_ARG(1);
    xbox_HeapFree(STACK_ARG(0));
    g_eax = 0;
}

/* --- MmDbgQueryAvailablePages (ordinal 376, 0 args = 0 bytes) --- */
static void bridge_MmDbgQueryAvailablePages(void)
{
    g_eax = 0x10000;  /* large made-up free-page count */
}

/* --- MmDbgReleaseAddress (ordinal 377, 2 args = 8 bytes) --- */
static void bridge_MmDbgReleaseAddress(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

/* --- MmDbgWriteCheck (ordinal 378, 2 args = 8 bytes)
 * Passthrough: returns the first argument (the guest VA). */
static void bridge_MmDbgWriteCheck(void)
{
    (void)STACK_ARG(1);
    g_eax = STACK_ARG(0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Nt (185, 191, 194, 201, 204, 206, 208, 209, 212, 213, 214, 216, 220, 223,
 *      227, 229, 230, 237)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- NtCancelTimer (ordinal 185, 2 args = 8 bytes)
 * NTSTATUS NtCancelTimer(HANDLE TimerHandle);
 * Resolves the handle token (the pair created by NtCreateTimer) and cancels
 * the waitable timer. (No xbox_NtCancelTimer() exists in kernel.h; done
 * inline with the Win32 API.) */
static void bridge_NtCancelTimer(void)
{
    HANDLE h = bridge_resolve_handle(STACK_ARG(0));
    uint32_t prev = STACK_ARG(1);
    struct { LONGLONG remaining; BOOLEAN state; } info = {0, 0};
    if (!h) { g_eax = 0xC0000008u; return; }
    CancelWaitableTimer(h);
    if (prev) {
        bridge_nt_query("NtQueryTimer", STACK_ARG(0), &info, sizeof info);
        BRIDGE_MEM8(prev) = info.state;
    }
    g_eax = 0;
}

/* --- NtCreateIoCompletion (ordinal 191, 4 args = 16 bytes) --- */
/* Asks the host's native query of that name about a handle: class 0, the basic
 * information, which is what the console's two-argument call returns. */
static uint32_t bridge_nt_query(const char *name, uint32_t token, void *buf, ULONG len)
{
    typedef LONG (WINAPI *query_fn)(HANDLE, ULONG, void *, ULONG, ULONG *);
    HANDLE h = bridge_resolve_handle(token);
    query_fn f = (query_fn)bridge_nt_fn(name);
    if (!h) return 0xC0000008u;   /* STATUS_INVALID_HANDLE */
    if (!f) return 0xC0000002u;   /* STATUS_NOT_IMPLEMENTED */
    return (uint32_t)f(h, 0, buf, len, NULL);
}

static void bridge_NtCreateIoCompletion(void)
{
    typedef LONG (WINAPI *create_fn)(HANDLE *, ULONG, void *, ULONG);
    create_fn f = (create_fn)bridge_nt_fn("NtCreateIoCompletion");
    HANDLE h = NULL;
    if (!f) { g_eax = 0xC0000002u; return; }
    g_eax = (uint32_t)f(&h, 0x001F0003u /* IO_COMPLETION_ALL_ACCESS */, NULL, STACK_ARG(3));
    if (g_eax == 0)
        g_eax = (uint32_t)bridge_write_handle(STACK_ARG(0), h);
}

/* --- NtCreateTimer (ordinal 194, 3 args = 12 bytes)
 * NTSTATUS NtCreateTimer(PHANDLE TimerHandle, POBJECT_ATTRIBUTES
 *                        ObjectAttributes, TIMER_TYPE TimerType);
 * Creates a manual-reset waitable timer and hands its token out through the
 * handle table, matching the NtCreateEvent/NtCreateMutant pattern. */
static void bridge_NtCreateTimer(void)
{
    HANDLE h;

    (void)STACK_ARG(1);

    if (STACK_ARG(2) > 1) {
        g_eax = 0xC000000Du;   /* STATUS_INVALID_PARAMETER */
        return;
    }
    /* 0 NotificationTimer stays signalled, 1 SynchronizationTimer resets. */
    {
        WCHAR wname[256];
        h = CreateWaitableTimerW(NULL, STACK_ARG(2) == 0, bridge_object_name(STACK_ARG(1), wname, 256));
    }
    if (!h) {
        fprintf(stderr, "  [KERNEL] NtCreateTimer: CreateWaitableTimerW failed (error %u)\n",
                GetLastError());
        g_eax = 0xC000009Au;  /* STATUS_INSUFFICIENT_RESOURCES */
        return;
    }
    g_eax = (uint32_t)bridge_write_handle(STACK_ARG(0), h);
}

/* --- NtOpenDirectoryObject (ordinal 201, 3 args = 12 bytes) --- */
static void bridge_NtOpenDirectoryObject(void)
{
    /* Handle, Attributes: no object directories here. */
    if (STACK_ARG(0)) BRIDGE_MEM32(STACK_ARG(0)) = 0;
    g_eax = 0xC0000034u;   /* STATUS_OBJECT_NAME_NOT_FOUND */
}

/* --- NtProtectVirtualMemory (ordinal 204, 4 args = 16 bytes)
 * NTSTATUS NtProtectVirtualMemory(PVOID* BaseAddress, PSIZE_T RegionSize,
 *                                 ULONG NewProtect, PULONG OldProtect);
 * Both pointers live in guest memory; translate and let VirtualProtect do the
 * work. (No xbox_NtProtectVirtualMemory() exists in kernel.h; done inline.) */
static void bridge_NtProtectVirtualMemory(void)
{
    uint32_t base_ptr = STACK_ARG(0);
    uint32_t size_ptr = STACK_ARG(1);
    uint32_t new_prot = STACK_ARG(2);
    uint32_t old_ptr  = STACK_ARG(3);
    uint32_t base_va = base_ptr ? BRIDGE_MEM32(base_ptr) : 0;
    uint32_t size    = size_ptr ? BRIDGE_MEM32(size_ptr) : 0;
    DWORD old = 0;

    /* As the kernel: a bad protection, a bad range, then the pages not committed. Base and size come back in whole
     * pages. */
    if (!nt_protect_ok(new_prot)) { g_eax = 0xC0000045u; return; }
    if (base_va > 0x7FFEFFFFu || size == 0 || size > 0x7FFEFFFFu - base_va) { g_eax = 0xC000000Du; return; }
    {
        uint32_t last = (base_va + size - 1) | 0xFFF, first = base_va & ~0xFFFu;
        if (!VirtualProtect(XBOX_TO_NATIVE(first), last - first + 1, (DWORD)new_prot, &old)) {
            g_eax = 0xC000002Du;   /* STATUS_NOT_COMMITTED */
            return;
        }
        BRIDGE_MEM32(base_ptr) = first;
        BRIDGE_MEM32(size_ptr) = last - first + 1;
        if (old_ptr)
            BRIDGE_MEM32(old_ptr) = (uint32_t)old;
        g_eax = 0;
    }
}

/* --- NtQueueApcThread (ordinal 206, 5 args = 20 bytes) --- */
static void bridge_NtQueueApcThread(void)
{
    HANDLE th = bridge_resolve_handle(STACK_ARG(0));
    struct bridge_user_apc *a;
    if (!th) { g_eax = 0xC0000008u; return; }
    a = (struct bridge_user_apc *)malloc(sizeof *a);
    if (!a) { g_eax = 0xC0000017u; return; }   /* STATUS_NO_MEMORY */
    a->routine = STACK_ARG(1);
    a->ctx = STACK_ARG(2);
    a->a1 = STACK_ARG(3);
    a->a2 = STACK_ARG(4);
    a->once = 1;
    if (!QueueUserAPC(bridge_user_apc_host, th, (ULONG_PTR)a)) {
        free(a);
        g_eax = 0xC0000001u;
        return;
    }
    g_eax = 0;
}

/* --- NtQueryDirectoryObject (ordinal 208, 7 args = 28 bytes) --- */
static void bridge_NtQueryDirectoryObject(void)
{
    /* Handle, Buffer, Length, ReturnSingleEntry, &Context, &Returned. The kernel wants room for a header; our only
     * directory handle is the empty one NtCreateDirectoryObject gives. */
    if (STACK_ARG(2) < 0xC) { g_eax = 0xC000000Du; return; }
    if (STACK_ARG(0) != 0xBEEF0010u) { g_eax = 0xC0000008u; return; }
    if (STACK_ARG(5)) BRIDGE_MEM32(STACK_ARG(5)) = 0;
    g_eax = 0x8000001Au;   /* STATUS_NO_MORE_ENTRIES */
}

/* --- NtQueryEvent (ordinal 209, 4 args = 16 bytes) --- */
static void bridge_NtQueryEvent(void)
{
    LONG info[2] = {0, 0};
    g_eax = bridge_nt_query("NtQueryEvent", STACK_ARG(0), info, sizeof info);
    if (g_eax == 0) {
        BRIDGE_MEM32(STACK_ARG(1)) = (uint32_t)info[0];   /* EventType */
        BRIDGE_MEM32(STACK_ARG(1) + 4) = (uint32_t)info[1];   /* EventState */
    }
}

/* --- NtQueryIoCompletion (ordinal 212, 5 args = 20 bytes) --- */
static void bridge_NtQueryIoCompletion(void)
{
    LONG depth = 0;
    g_eax = bridge_nt_query("NtQueryIoCompletion", STACK_ARG(0), &depth, sizeof depth);
    if (g_eax == 0)
        BRIDGE_MEM32(STACK_ARG(1)) = (uint32_t)depth;
}

/* --- NtQueryMutant (ordinal 213, 5 args = 20 bytes) --- */
static void bridge_NtQueryMutant(void)
{
    struct { LONG count; BOOLEAN owned, abandoned; } info = {0, 0, 0};
    g_eax = bridge_nt_query("NtQueryMutant", STACK_ARG(0), &info, sizeof info);
    if (g_eax == 0) {
        BRIDGE_MEM32(STACK_ARG(1)) = (uint32_t)info.count;
        BRIDGE_MEM8(STACK_ARG(1) + 4) = info.owned;
        BRIDGE_MEM8(STACK_ARG(1) + 5) = info.abandoned;
    }
}

/* --- NtQuerySemaphore (ordinal 214, 5 args = 20 bytes) --- */
static void bridge_NtQuerySemaphore(void)
{
    LONG info[2] = {0, 0};
    g_eax = bridge_nt_query("NtQuerySemaphore", STACK_ARG(0), info, sizeof info);
    if (g_eax == 0) {
        BRIDGE_MEM32(STACK_ARG(1)) = (uint32_t)info[0];   /* CurrentCount */
        BRIDGE_MEM32(STACK_ARG(1) + 4) = (uint32_t)info[1];   /* MaximumCount */
    }
}

/* --- NtQueryTimer (ordinal 216, 5 args = 20 bytes) --- */
static void bridge_NtQueryTimer(void)
{
    struct { LONGLONG remaining; BOOLEAN state; } info = {0, 0};
    g_eax = bridge_nt_query("NtQueryTimer", STACK_ARG(0), &info, sizeof info);
    if (g_eax == 0) {
        BRIDGE_MEM32(STACK_ARG(1)) = (uint32_t)info.remaining;
        BRIDGE_MEM32(STACK_ARG(1) + 4) = (uint32_t)((uint64_t)info.remaining >> 32);
        BRIDGE_MEM8(STACK_ARG(1) + 8) = info.state;
    }
}

/* --- NtReadFileScatter (ordinal 220, 8 args = 32 bytes) --- */
/* NtReadFileScatter / NtWriteFileGather: as NtReadFile / NtWriteFile, with the
 * buffer given as one address per 4 KB page (Length bytes in all). */
static void bridge_file_pages(int write)
{
    HANDLE   handle  = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va  = STACK_ARG(4), pages = STACK_ARG(5), length = STACK_ARG(6), off_va = STACK_ARG(7);
    XBOX_IO_STATUS_BLOCK ios;
    LARGE_INTEGER off = {0};
    PLARGE_INTEGER poff = NULL;
    uint32_t done = 0, i;
    NTSTATUS st = 0;

    if (!handle) { g_eax = 0xC0000008u; return; }
    if (!pages || !bridge_buf_ok(pages, ((length + 4095u) / 4096u) * 4u, "file pages")) {
        g_eax = 0xC000000Du;
        return;
    }
    if (off_va) {
        off.LowPart  = BRIDGE_MEM32(off_va);
        off.HighPart = (LONG)BRIDGE_MEM32(off_va + 4);
        if (off.QuadPart >= 0) poff = &off;
    }
    for (i = 0; done < length; i++) {
        uint32_t page = BRIDGE_MEM32(pages + i * 4u);
        uint32_t n = length - done < 4096u ? length - done : 4096u;
        memset(&ios, 0, sizeof ios);
        if (!bridge_buf_ok(page, n, "file pages")) { st = (NTSTATUS)0xC0000005L; break; }
        st = write ? xbox_NtWriteFile(handle, NULL, NULL, NULL, &ios, XBOX_TO_NATIVE(page), n, poff)
                   : xbox_NtReadFile(handle, NULL, NULL, NULL, &ios, XBOX_TO_NATIVE(page), n, poff);
        done += (uint32_t)ios.Information;
        if (poff) off.QuadPart += (LONGLONG)ios.Information;
        if (st != 0 || (uint32_t)ios.Information < n) break;
    }
    if (done && st == (NTSTATUS)0xC0000011L) st = 0;   /* a short read, not the end */
    bridge_write_iostatus(ios_va, st, done);
    bridge_complete_file_io(STACK_ARG(1), STACK_ARG(2), STACK_ARG(3), ios_va);
    g_eax = (uint32_t)st;
}

static void bridge_NtReadFileScatter(void)
{
    bridge_file_pages(0);
}

/* --- NtRemoveIoCompletion (ordinal 223, 5 args = 20 bytes) --- */
static void bridge_NtRemoveIoCompletion(void)
{
    typedef LONG (WINAPI *remove_fn)(HANDLE, ULONG_PTR *, ULONG_PTR *, void *, void *);
    remove_fn f = (remove_fn)bridge_nt_fn("NtRemoveIoCompletion");
    HANDLE h = bridge_resolve_handle(STACK_ARG(0));
    ULONG_PTR key = 0, ctx = 0;
    struct { LONG_PTR status; ULONG_PTR info; } ios = {0, 0};
    LONG st;
    if (!h) { g_eax = 0xC0000008u; return; }
    if (!f) { g_eax = 0xC0000002u; return; }
    st = f(h, &key, &ctx, &ios, STACK_ARG(4) ? XBOX_TO_NATIVE(STACK_ARG(4)) : NULL);
    g_eax = (uint32_t)st;
    if (st == 0) {
        BRIDGE_MEM32(STACK_ARG(1)) = (uint32_t)key;
        BRIDGE_MEM32(STACK_ARG(2)) = (uint32_t)ctx;
        BRIDGE_MEM32(STACK_ARG(3)) = (uint32_t)ios.status;
        BRIDGE_MEM32(STACK_ARG(3) + 4) = (uint32_t)ios.info;
    }
}

/* --- NtSetIoCompletion (ordinal 227, 5 args = 20 bytes) --- */
static void bridge_NtSetIoCompletion(void)
{
    typedef LONG (WINAPI *set_fn)(HANDLE, ULONG_PTR, ULONG_PTR, LONG, ULONG_PTR);
    set_fn f = (set_fn)bridge_nt_fn("NtSetIoCompletion");
    HANDLE h = bridge_resolve_handle(STACK_ARG(0));
    if (!h) { g_eax = 0xC0000008u; return; }
    if (!f) { g_eax = 0xC0000002u; return; }
    g_eax = (uint32_t)f(h, STACK_ARG(1), STACK_ARG(2), (LONG)STACK_ARG(3), STACK_ARG(4));
}

/* --- NtSetTimerEx (ordinal 229, 8 args = 32 bytes) --- */
static void bridge_NtSetTimerEx(void)
{
    /* Handle, DueTime, ApcRoutine, ApcMode, ApcContext, Resume, Period, PreviousState */
    HANDLE h = bridge_resolve_handle(STACK_ARG(0));
    uint32_t apc = STACK_ARG(2), prev = STACK_ARG(7);
    LONG period = (LONG)STACK_ARG(6);
    LARGE_INTEGER due;
    struct { LONGLONG remaining; BOOLEAN state; } info = {0, 0};
    struct bridge_user_apc *a = NULL;
    if (period < 0) { g_eax = 0xC000000Du; return; }
    if (!h) { g_eax = 0xC0000008u; return; }
    if (prev) bridge_nt_query("NtQueryTimer", STACK_ARG(0), &info, sizeof info);
    due.LowPart = BRIDGE_MEM32(STACK_ARG(1));
    due.HighPart = (LONG)BRIDGE_MEM32(STACK_ARG(1) + 4);
    if (apc) {
        a = (struct bridge_user_apc *)malloc(sizeof *a);
        if (!a) { g_eax = 0xC000009Au; return; }
        a->routine = apc;
        a->ctx = STACK_ARG(4);
        a->a1 = a->a2 = 0;
        a->once = period == 0;
    }
    if (!SetWaitableTimer(h, &due, period, a ? bridge_timer_apc_host : NULL, a, FALSE)) {
        free(a);
        g_eax = 0xC0000001u;
        return;
    }
    if (prev) BRIDGE_MEM8(prev) = info.state;
    g_eax = (uint8_t)STACK_ARG(5) ? 0x40000025u : 0;   /* STATUS_TIMER_RESUME_IGNORED */
}

/* --- NtSignalAndWaitForSingleObjectEx (ordinal 230, 5 args = 20 bytes) --- */
static void bridge_NtSignalAndWaitForSingleObjectEx(void)
{
    HANDLE sig = bridge_resolve_handle(STACK_ARG(0));
    HANDLE wait = bridge_resolve_handle(STACK_ARG(1));

    if (!sig || !wait) {
        g_eax = 0xC0000008u;   /* STATUS_INVALID_HANDLE */
        return;
    }
    g_eax = (uint32_t)xbox_NtSignalAndWaitForSingleObjectEx(sig, wait, (BOOLEAN)STACK_ARG(3),
                                                            XBOX_TO_NATIVE(STACK_ARG(4)));
}

/* --- NtWriteFileGather (ordinal 237, 8 args = 32 bytes) --- */
static void bridge_NtWriteFileGather(void)
{
    bridge_file_pages(1);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Ob (239, 241, 242, 243, 244, 248)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- ObCreateObject (ordinal 239, 7 args = 28 bytes) --- */

/* --- ObInsertObject (ordinal 241, 6 args = 24 bytes) --- */

/* --- ObMakeTemporaryObject (ordinal 242, 1 arg = 4 bytes) --- */

/* --- ObOpenObjectByName (ordinal 243, 4 args = 16 bytes) --- */

/* --- ObOpenObjectByPointer (ordinal 244, 5 args = 20 bytes) --- */

/* --- ObReferenceObjectByPointer (ordinal 248, 1 arg = 4 bytes) --- */
/* ── Objects a title creates itself ──────────────────────────
 *
 * The kernel's header in front of the object: +0 reference count, +4 handle count, +8 type, +0xC flags (1 named,
 * 2 permanent, 4 in a directory), 0x10 bytes before the body. A named one has 0x10 more before that, its ANSI name at
 * +8 and the text after the body. Types: +0 allocate, +4 free, +8 close, +0xC delete, +0x18 pool tag.
 *
 * Everything else called an object here is a stand-in or a host object with no header, so only addresses in this list
 * are touched. Names live in one flat list, not the kernel's directories: a name means the same with any root. */

static uint32_t bridge_call_guest(uint32_t routine, int argc, const uint32_t *argv);

static struct ob_managed *ob_find(uint32_t body)
{
    struct ob_managed *m;
    ex_lock();
    for (m = g_ob_list; m && m->body != body; m = m->next) {}
    ex_unlock();
    return m;
}

static struct ob_managed *ob_find_name(const char *name)
{
    struct ob_managed *m;
    ex_lock();
    for (m = g_ob_list; m && !(m->name && (OB_FLAGS(m->body) & 4) && !_stricmp(m->name, name)); m = m->next) {}
    ex_unlock();
    return m;
}

/* The object name in OBJECT_ATTRIBUTES, its last component; NULL with none */
static char *ob_attr_name(uint32_t oa)
{
    uint32_t str = oa ? BRIDGE_MEM32(oa + 4) : 0, text;
    uint16_t len;
    char *out, *last;
    if (!str) return NULL;
    len = BRIDGE_MEM16(str);
    text = BRIDGE_MEM32(str + 4);
    if (!len || !text) return NULL;
    out = (char *)malloc(len + 1u);
    if (!out) return NULL;
    memcpy(out, XBOX_TO_NATIVE(text), len);
    out[len] = 0;
    last = strrchr(out, '\\');
    if (last) memmove(out, last + 1, strlen(last));
    return out;
}

static void ob_deref(uint32_t body)
{
    struct ob_managed *m = ob_find(body), **pp;
    uint32_t type, block, del, freer;
    if (!m) return;
    if (InterlockedDecrement((volatile LONG *)XBOX_TO_NATIVE(body - 0x10)) != 0) return;
    type = OB_TYPE(body);
    del = BRIDGE_MEM32(type + 0xC);
    freer = BRIDGE_MEM32(type + 4);
    block = (OB_FLAGS(body) & 1) ? body - 0x20 : body - 0x10;
    if (del) bridge_call_guest(del, 1, &body);
    ex_lock();
    for (pp = &g_ob_list; *pp != m; pp = &(*pp)->next) {}
    *pp = m->next;
    ex_unlock();
    free(m->name);
    free(m);
    if (freer) bridge_call_guest(freer, 1, &block);
    else xbox_HeapFree(block);
}

/* Out of its name's place once nothing keeps it there */
static void ob_maybe_unname(uint32_t body)
{
    if (!OB_HANDLES(body) && (OB_FLAGS(body) & 4) && !(OB_FLAGS(body) & 2)) {
        OB_FLAGS(body) &= ~4u;
        ob_deref(body);
    }
}

/* From NtClose: a handle holds one reference and one handle count */
static void ob_close_handle(uint32_t body)
{
    uint32_t type = OB_TYPE(body), closer = BRIDGE_MEM32(type + 8), argv[2];
    uint32_t left = (uint32_t)InterlockedDecrement((volatile LONG *)XBOX_TO_NATIVE(body - 0xC));
    argv[0] = body, argv[1] = left + 1;
    if (closer) bridge_call_guest(closer, 2, argv);
    ob_maybe_unname(body);
    ob_deref(body);
}

static uint32_t ob_make_handle(uint32_t handle_va, uint32_t body)
{
    uint32_t st = (uint32_t)bridge_write_handle(handle_va, (HANDLE)(OB_HANDLE_TAG | body));
    if ((int32_t)st >= 0)
        InterlockedIncrement((volatile LONG *)XBOX_TO_NATIVE(body - 0xC));
    return st;
}

/* The object behind a handle, 0 if it is not one of these */
static uint32_t ob_from_handle(uint32_t handle)
{
    uintptr_t h = (uintptr_t)bridge_handle_lookup(handle);
    return (h & ~(uintptr_t)0xFFFFFFFFu) == OB_HANDLE_TAG ? (uint32_t)h : 0;
}

static void bridge_ObCreateObject(void)
{
    /* Type, Attributes, BodySize, &Object */
    uint32_t type = STACK_ARG(0), oa = STACK_ARG(1), size = STACK_ARG(2), out = STACK_ARG(3);
    char *name = NULL;
    uint32_t alloc = type ? BRIDGE_MEM32(type) : 0, tag = type ? BRIDGE_MEM32(type + 0x18) : 0;
    uint32_t total, block, body, argv[2];
    struct ob_managed *m;

    BRIDGE_MEM32(out) = 0;
    if (oa && BRIDGE_MEM32(oa + 4)) {
        name = ob_attr_name(oa);
        if (!name || !*name) {
            free(name);
            g_eax = 0xC0000033u;   /* STATUS_OBJECT_NAME_INVALID */
            return;
        }
    }
    size = name ? (size + 3) & ~3u : size;
    total = name ? size + 0x20 + (uint32_t)strlen(name) : size + 0x10;
    argv[0] = total, argv[1] = tag;
    /* The kernel's own types allocate from the pool (our copies of them hold no routine) */
    block = alloc ? bridge_call_guest(alloc, 2, argv) : pool_alloc(total);
    m = block ? (struct ob_managed *)malloc(sizeof *m) : NULL;
    if (!m) {
        free(name);
        g_eax = 0xC000009Au;
        return;
    }
    if (name) {
        uint32_t text = block + 0x20 + size, n = (uint32_t)strlen(name);
        BRIDGE_MEM32(block) = 0;
        BRIDGE_MEM32(block + 4) = 0;
        BRIDGE_MEM16(block + 8) = (uint16_t)n;
        BRIDGE_MEM16(block + 0xA) = (uint16_t)n;
        BRIDGE_MEM32(block + 0xC) = text;
        memcpy(XBOX_TO_NATIVE(text), name, n);
        body = block + 0x20;
    } else {
        body = block + 0x10;
    }
    OB_REFS(body) = 1;
    OB_HANDLES(body) = 0;
    OB_TYPE(body) = type;
    OB_FLAGS(body) = name ? 1 : 0;
    m->body = body;
    m->name = name;
    ex_lock();
    m->next = g_ob_list;
    g_ob_list = m;
    ex_unlock();
    BRIDGE_MEM32(out) = body;
    g_eax = 0;
}

static void bridge_ObInsertObject(void)
{
    /* Object, Attributes, Extra handle references (ignored by the kernel's count here), &Handle. The caller's reference
     * is used up, as on the console: on success it becomes the handle's or is dropped for an existing object. */
    uint32_t body = STACK_ARG(0), oa = STACK_ARG(1), handle_va = STACK_ARG(3), use = body, st = 0;
    struct ob_managed *m = ob_find(body);
    if (handle_va) BRIDGE_MEM32(handle_va) = 0;
    if (!m) {   /* not from ObCreateObject */
        g_eax = 0xC0000002u;
        return;
    }
    if (m->name) {
        struct ob_managed *other = ob_find_name(m->name);
        if (other) {
            if (!(oa && (BRIDGE_MEM32(oa + 8) & 0x80))) {   /* OBJ_OPENIF */
                ob_deref(body);
                g_eax = 0xC0000035u;   /* STATUS_OBJECT_NAME_COLLISION */
                return;
            }
            if (OB_TYPE(other->body) != OB_TYPE(body)) {
                ob_deref(body);
                g_eax = 0xC0000024u;
                return;
            }
            use = other->body;
            InterlockedIncrement((volatile LONG *)XBOX_TO_NATIVE(use - 0x10));
            ob_deref(body);
            st = 0x40000000u;          /* STATUS_OBJECT_NAME_EXISTS */
        } else {
            OB_FLAGS(body) |= 4;
            InterlockedIncrement((volatile LONG *)XBOX_TO_NATIVE(body - 0x10));   /* the name holds one */
        }
    }
    if (oa && (BRIDGE_MEM32(oa + 8) & 0x10))   /* OBJ_PERMANENT */
        OB_FLAGS(use) |= 2;
    {
        uint32_t w = ob_make_handle(handle_va, use);
        if ((int32_t)w < 0) {
            ob_deref(use);
            st = w;
        }
    }
    g_eax = st;
}

static void bridge_ObOpenObjectByPointer(void)
{
    /* Object, Type, &Handle */
    uint32_t body = STACK_ARG(0), type = STACK_ARG(1), handle_va = STACK_ARG(2);
    if (handle_va) BRIDGE_MEM32(handle_va) = 0;
    if (!ob_find(body)) {
        g_eax = 0xC0000002u;   /* a stand-in has no header */
        return;
    }
    if (OB_TYPE(body) != type) {
        g_eax = 0xC0000024u;
        return;
    }
    InterlockedIncrement((volatile LONG *)XBOX_TO_NATIVE(body - 0x10));
    g_eax = ob_make_handle(handle_va, body);
    if ((int32_t)g_eax < 0) ob_deref(body);
}

static void bridge_ObOpenObjectByName(void)
{
    /* Attributes, Type, ParseContext, &Handle */
    uint32_t oa = STACK_ARG(0), type = STACK_ARG(1), handle_va = STACK_ARG(3);
    char *name = ob_attr_name(oa);
    struct ob_managed *m = name ? ob_find_name(name) : NULL;
    free(name);
    if (handle_va) BRIDGE_MEM32(handle_va) = 0;
    if (!m) {
        g_eax = 0xC0000034u;
        return;
    }
    if (type && OB_TYPE(m->body) != type) {
        g_eax = 0xC0000024u;
        return;
    }
    InterlockedIncrement((volatile LONG *)XBOX_TO_NATIVE(m->body - 0x10));
    g_eax = ob_make_handle(handle_va, m->body);
    if ((int32_t)g_eax < 0) ob_deref(m->body);
}

static void bridge_ObReferenceObjectByPointer(void)
{
    uint32_t body = STACK_ARG(0), type = STACK_ARG(1);
    g_eax = 0;
    if (ob_find(body)) {
        if (OB_TYPE(body) != type) g_eax = 0xC0000024u;
        else InterlockedIncrement((volatile LONG *)XBOX_TO_NATIVE(body - 0x10));
    }
}

static void bridge_ObMakeTemporaryObject(void)
{
    uint32_t body = STACK_ARG(0);
    if (ob_find(body)) {
        OB_FLAGS(body) &= ~2u;
        ob_maybe_unname(body);
    }
    g_eax = 0;
}

static void bridge_ObfReferenceObject(void)
{
    uint32_t body = g_ecx;
    if (ob_find(body))
        g_eax = (uint32_t)InterlockedIncrement((volatile LONG *)XBOX_TO_NATIVE(body - 0x10));
    else
        g_eax = 0;
}

static void bridge_ObfDereferenceObject(void)
{
    ob_deref(g_ecx);
    g_eax = 0;
}

/* A handle that is not valid is C0000008; one of the wrong kind, which the host's calls also fail on, C0000024 */
static uint32_t bridge_handle_status(HANDLE h, uint32_t st)
{
    DWORD flags;
    return st == 0xC0000008u && h && GetHandleInformation(h, &flags) ? 0xC0000024u : st;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Ps (254, 256, 257)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- PsCreateSystemThread (ordinal 254, 7 args = 28 bytes)
 * NTSTATUS PsCreateSystemThread(PHANDLE ThreadHandle, ULONG KernelStackSize,
 *                               ULONG TlsDataSize, PULONG ThreadId,
 *                               PVOID StartContext1, PVOID StartContext2,
 *                               PXBOX_SYSTEM_ROUTINE StartRoutine);
 * Mirror of PsCreateSystemThreadEx (ordinal 255) with the ThreadExtraSize
 * argument dropped: same first-call-inline / worker-spawn behaviour.
 * Requires bridge_spawn_thread/bridge_run_thread_inline/g_thread_mode from
 * Section A of kernel_bridge.c. */
static void bridge_PsCreateSystemThread(void)
{
    uint32_t xbox_handle_ptr = STACK_ARG(0);
    uint32_t start_context1  = STACK_ARG(3);   /* the routine's one argument */
    uint32_t start_context2  = 0;
    uint32_t start_routine   = STACK_ARG(2);
    int is_first_call = (g_thread_mode == XBOX_THREAD_MODE_INLINE)
                        && (g_thread_call_count == 0);

    g_thread_call_count++;

    /* Fake handle until the worker path can write a real one. */
    if (xbox_handle_ptr)
        BRIDGE_MEM32(xbox_handle_ptr) = 0xBEEF0002;

    if (start_routine) {
        recomp_func_t fn = recomp_lookup(start_routine);
        if (!fn)
            fn = recomp_lookup_manual(start_routine);
        if (fn) {
            if (is_first_call) {
                g_esp -= 4; BRIDGE_MEM32(g_esp) = start_context2;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = start_context1;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
                fn();
                g_esp += 12;
            } else {
                const char *inline_workers = getenv("RECOMP_WORKERS");
                uint32_t stack_top;

                if (inline_workers && !strcmp(inline_workers, "inline")) {
                    bridge_run_thread_inline(fn, start_context1, start_context2);
                    g_eax = 0;
                    return;
                }
                stack_top = xbox_AllocThreadStack();
                if (!stack_top) {
                    bridge_run_thread_inline(fn, start_context1, start_context2);
                } else {
                    HANDLE th = bridge_spawn_thread(fn, start_context1,
                                                    start_context2, stack_top);
                    if (!th) {
                        g_eax = 0xC000009Au;   /* STATUS_INSUFFICIENT_RESOURCES */
                        return;
                    }
                    if (STACK_ARG(1)) BRIDGE_MEM32(STACK_ARG(1)) = (uint32_t)GetThreadId(th);
                    if (xbox_handle_ptr) bridge_write_handle(xbox_handle_ptr, th);
                    ResumeThread(th);
                }
            }
        } else {
            fprintf(stderr, "  [KERNEL] PsCreateSystemThread: start routine 0x%08X not found in dispatch!\n",
                    start_routine);
        }
    }
    g_eax = 0;  /* STATUS_SUCCESS */
}

/* --- PsQueryStatistics (ordinal 256, 1 arg = 4 bytes)
 * Zeroes the caller's statistics structure. */
static void bridge_PsQueryStatistics(void)
{
    /* A 12-byte record that says so: Length, ThreadCount, HandleCount. This used to clear 0x80 bytes of the caller. */
    uint32_t stats_va = STACK_ARG(0);
    if (BRIDGE_MEM32(stats_va) != 0xCu) {
        g_eax = 0xC000000Du;
        return;
    }
    BRIDGE_MEM32(stats_va + 4) = (uint32_t)s_thread_obj_count;
    BRIDGE_MEM32(stats_va + 8) = 0;   /* not counted here */
    g_eax = 0;
}

/* --- PsSetCreateThreadNotifyRoutine (ordinal 257, 2 args = 8 bytes) --- */
static void bridge_PsSetCreateThreadNotifyRoutine(void)
{
    uint32_t i;
    for (i = 0; i < 8; i++)
        if (!s_thread_notify[i]) {
            s_thread_notify[i] = STACK_ARG(0);
            g_eax = 0;
            return;
        }
    g_eax = 0xC000009Au;   /* STATUS_INSUFFICIENT_RESOURCES */
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Rtl (281, 282, 283, 285, 286, 303, 361, 362, 363, 364)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- RtlExtendedIntegerMultiply (ordinal 281, 3 args = 12 bytes)
 * LARGE_INTEGER RtlExtendedIntegerMultiply(LARGE_INTEGER Multiplicand,
 *                                          LONG Multiplier);
 * Multiplicand.Low (4) + Multiplicand.High (4) + Multiplier (4). C's int64
 * multiply wraps mod 2^64, which is exactly what the x86 imul pair produces
 * for the low 64 bits the API returns. */
static void bridge_RtlExtendedIntegerMultiply(void)
{
    int32_t mult = (int32_t)STACK_ARG(2);
    int64_t multi = ((int64_t)(int32_t)STACK_ARG(1) << 32)
                  | (uint32_t)STACK_ARG(0);
    int64_t result = multi * (int64_t)mult;

    g_eax = (uint32_t)result;
    g_edx = (uint32_t)((uint64_t)result >> 32);
}

/* --- RtlExtendedLargeIntegerDivide (ordinal 282, 4 args = 16 bytes)
 * LARGE_INTEGER RtlExtendedLargeIntegerDivide(LARGE_INTEGER Dividend,
 *                                             ULONG Divisor,
 *                                             PULONG Remainder);
 * Dividend.Low (4) + Dividend.High (4) + Divisor (4) + Remainder-ptr (4). */
static void bridge_RtlExtendedLargeIntegerDivide(void)
{
    uint64_t dividend = ((uint64_t)(uint32_t)STACK_ARG(1) << 32)
                      | (uint32_t)STACK_ARG(0);
    uint32_t divisor = STACK_ARG(2);
    uint32_t rem_ptr = STACK_ARG(3);
    uint32_t remainder;
    uint64_t quotient;

    if (divisor == 0) {
        /* Division by zero: report 0/*; Real hardware faults here. */
        if (rem_ptr)
            BRIDGE_MEM32(rem_ptr) = 0;
        g_eax = 0;
        g_edx = 0;
        return;
    }

    quotient  = dividend / divisor;
    remainder = (uint32_t)(dividend % divisor);

    if (rem_ptr)
        BRIDGE_MEM32(rem_ptr) = remainder;
    g_eax = (uint32_t)quotient;
    g_edx = (uint32_t)(quotient >> 32);
}

/* --- RtlExtendedMagicDivide (ordinal 283, 4 args = 16 bytes)
 * RtlExtendedMagicDivide(Dividend.Low, Dividend.High, Divisor, Remainder-ptr)
 * per the 16-byte stack layout. The precomputed magic multiplier is not
 * available, so a real division produces the identical quotient. */
static void bridge_RtlExtendedMagicDivide(void)
{
    /* Dividend[8], MagicDivisor[8], ShiftCount: the top 64 bits of the product,
     * shifted, the dividend's sign kept. */
    int64_t d = (int64_t)(((uint64_t)STACK_ARG(1) << 32) | STACK_ARG(0));
    uint64_t m = ((uint64_t)STACK_ARG(3) << 32) | STACK_ARG(2);
    uint32_t shift = STACK_ARG(4) & 0xFFu;
    uint64_t u = d < 0 ? (uint64_t)0 - (uint64_t)d : (uint64_t)d;
    uint64_t q = (uint64_t)(((unsigned __int128)u * m) >> 64);
    q = shift >= 64 ? 0 : q >> shift;
    if (d < 0) q = (uint64_t)0 - q;
    g_eax = (uint32_t)q;
    g_edx = (uint32_t)(q >> 32);
}

/* --- RtlFillMemoryUlong (ordinal 285, 3 args = 12 bytes)
 * VOID RtlFillMemoryUlong(PVOID Destination, ULONG Length, ULONG Pattern);
 * Length is multiples of 4 by contract. */
static void bridge_RtlFillMemoryUlong(void)
{
    volatile uint32_t *dst = (volatile uint32_t *)XBOX_TO_NATIVE(STACK_ARG(0));
    uint32_t length = STACK_ARG(1);
    uint32_t pattern = STACK_ARG(2);
    uint32_t i;

    if (!dst)
        return;
    for (i = 0; i < length / 4; i++)
        dst[i] = pattern;
    g_eax = 0;
}

/* --- RtlFreeAnsiString (ordinal 286, 1 arg = 4 bytes)
 * VOID RtlFreeAnsiString(PANSI_STRING AnsiString);
 * Frees AnsiString->Buffer (a guest-heap allocation) and clears the struct.
 * ANSI_STRING layout: +0 Length(USHORT), +2 MaximumLength(USHORT), +4 Buffer. */
static void bridge_RtlFreeAnsiString(void)
{
    uint32_t str_va = STACK_ARG(0);
    uint32_t buf;

    if (!str_va) {
        g_eax = 0;
        return;
    }
    buf = BRIDGE_MEM32(str_va + 4);
    if (buf)
        xbox_HeapFree(buf);
    BRIDGE_MEM16(str_va + 0) = 0;
    BRIDGE_MEM16(str_va + 2) = 0;
    BRIDGE_MEM32(str_va + 4) = 0;
    g_eax = 0;
}

/* --- RtlRaiseStatus (ordinal 303, 1 arg = 4 bytes) --- */
static void bridge_RtlRaiseStatus(void)
{
    uint32_t status = STACK_ARG(0);
    fprintf(stderr, "  [KERNEL] RtlRaiseStatus: 0x%08X (swallowed)\n", status);
    g_eax = 0;
}

/* --- Rtl* printf family (ordinals 361-364) ---
 *
 * int RtlSnprintf(char*, size_t count, const char* fmt, ...)
 * int RtlSprintf(char*, const char* fmt, ...)
 * int RtlVsnprintf(char*, size_t count, const char* fmt, va_list)
 * int RtlVsprintf(char*, const char* fmt, va_list)
 *
 * All four are __cdecl on the console, so the stdcall table pops nothing
 * ("caller cleans", like DbgPrint) and the varargs stay readable on the guest
 * stack while the bridge runs. The V* variants take an x86 va_list, which is
 * just a guest pointer to where the argument slots begin.
 *
 * The x64 CRT's vsnprintf cannot consume an x86 va_list, so these bridges used
 * to copy the format string verbatim ("%d" came out as the literal text) and
 * read the wrong fixed arguments for the two non-counted variants: RtlSprintf's
 * "count" was the format pointer, and the vararg list started one slot early.
 * This walks the format itself and formats each conversion with the host CRT,
 * reading 32-bit argument slots from guest memory (64-bit args 8-aligned, as
 * on x86).
 */

/* Guest cursor over the variadic argument slots. */
static uint32_t bridge_va_next32(uint32_t *pos)
{
    uint32_t v = BRIDGE_MEM32(*pos);
    *pos += 4;
    return v;
}

static uint64_t bridge_va_next64(uint32_t *pos)
{
    uint32_t p = (*pos + 7) & ~7u;
    uint32_t lo = BRIDGE_MEM32(p);
    uint32_t hi = BRIDGE_MEM32(p + 4);
    *pos = p + 8;
    return (uint64_t)lo | ((uint64_t)hi << 32);
}

/* Format fmt, pulling varargs from guest memory at `pos`, into out (at most
 * cap-1 chars, NUL-terminated). Returns the number of characters written. */
static int bridge_format_printf(char *out, size_t cap, const char *fmt,
                                uint32_t pos)
{
    /* As the kernel's printf. Conversions d i o u x X, p (eight upper-case hex digits), c C s S Z and n. e E f g G take
     * their 8 bytes and print nothing: the kernel has no floating point here. An unknown conversion prints nothing.
     * h, l, w, I32, I64 sizes; S and C are wide by default, s and c narrow, l and w make them wide, h narrow. Z is a
     * string record: narrow, or wide with w or l. Past the buffer the result is -1 with no NUL, as _vsnprintf. */
    size_t used = 0, i = 0;
    int overflow = 0;

#define PUT(c) do { if (used < cap) out[used++] = (char)(c); else overflow = 1; } while (0)

    if (!out)
        cap = 0;
    while (fmt[i] && !overflow) {
        if (fmt[i] != '%') {
            PUT(fmt[i]);
            i++;
            continue;
        }
        i++;
        if (fmt[i] == '%') {
            PUT('%');
            i++;
            continue;
        }
        {
            char flags[8], conv[32], num[96], *cp = conv;
            int nflags = 0, width = 0, have_width = 0, prec = -1;
            int size = 0;   /* 0 none, 1 h, 3 l, 4 I64, 5 w */
            char spec;
            int left;

            while (fmt[i] && strchr("-+ 0#", fmt[i]) && nflags < 7)
                flags[nflags++] = fmt[i++];
            flags[nflags] = 0;
            if (fmt[i] == '*') {
                width = (int32_t)bridge_va_next32(&pos);
                have_width = 1;
                i++;
            } else {
                while (fmt[i] >= '0' && fmt[i] <= '9') {
                    width = width * 10 + (fmt[i] - '0');
                    have_width = 1;
                    i++;
                }
            }
            left = strchr(flags, '-') != NULL;
            if (width < 0) { left = 1; width = -width; }
            if (fmt[i] == '.') {
                i++;
                prec = 0;
                if (fmt[i] == '*') {
                    prec = (int32_t)bridge_va_next32(&pos);
                    i++;
                } else {
                    while (fmt[i] >= '0' && fmt[i] <= '9') prec = prec * 10 + (fmt[i++] - '0');
                }
            }
            if (fmt[i] == 'h') { size = 1; i++; }
            else if (fmt[i] == 'l') { size = 3; i++; }
            else if (fmt[i] == 'w') { size = 5; i++; }
            else if (fmt[i] == 'I' && fmt[i + 1] == '6' && fmt[i + 2] == '4') { size = 4; i += 3; }
            else if (fmt[i] == 'I' && fmt[i + 1] == '3' && fmt[i + 2] == '2') { i += 3; }
            spec = fmt[i];
            if (!spec)
                break;
            i++;

            switch (spec) {
            case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'p': {
                unsigned long long v;
                char c = spec == 'i' ? 'd' : spec == 'p' ? 'X' : spec;
                if (spec == 'p') prec = 8;
                v = size == 4 ? bridge_va_next64(&pos) : bridge_va_next32(&pos);
                *cp++ = '%';
                memcpy(cp, flags, (size_t)nflags), cp += nflags;
                if (prec >= 0) cp += sprintf(cp, ".%d", prec);
                *cp++ = 'l', *cp++ = 'l', *cp++ = c, *cp = 0;
                if (c == 'd') {
                    long long sv = size == 4 ? (long long)v : size == 1 ? (long long)(int16_t)v : (long long)(int32_t)v;
                    snprintf(num, sizeof num, conv, sv);
                } else {
                    if (size == 1) v &= 0xFFFF;
                    snprintf(num, sizeof num, conv, v);
                }
                {
                    size_t n = strlen(num), k;
                    int pad = have_width && (size_t)width > n ? width - (int)n : 0;
                    int zero = strchr(flags, '0') && !left && prec < 0;
                    if (zero && pad) {   /* zeros go after any sign or prefix */
                        size_t lead = (num[0] == '-' || num[0] == '+' || num[0] == ' ') ? 1 : 0;
                        if (num[lead] == '0' && (num[lead + 1] == 'x' || num[lead + 1] == 'X')) lead += 2;
                        for (k = 0; k < lead; k++) PUT(num[k]);
                        while (pad-- > 0) PUT('0');
                        for (k = lead; k < n; k++) PUT(num[k]);
                    } else {
                        if (!left) while (pad-- > 0) PUT(' ');
                        for (k = 0; k < n; k++) PUT(num[k]);
                        if (left) while (pad-- > 0) PUT(' ');
                    }
                }
                break;
            }
            case 'c': case 'C': {
                uint32_t v = bridge_va_next32(&pos);
                int pad = have_width && width > 1 ? width - 1 : 0;
                int wide = spec == 'C' ? size != 1 : (size == 3 || size == 5);
                if (!left) while (pad-- > 0) PUT(' ');
                PUT(wide ? ((v & 0xFFFF) < 0x100 ? v : '?') : v);
                if (left) while (pad-- > 0) PUT(' ');
                break;
            }
            case 's': case 'S': case 'Z': {
                uint32_t va = bridge_va_next32(&pos);
                int wide = spec == 'S' ? size != 1 : (size == 3 || size == 5);
                int counted = -1, n = 0, k, pad;
                if (spec == 'Z') {
                    uint32_t text = va ? BRIDGE_MEM32(va + 4) : 0;
                    if (text) {
                        counted = (int)(int16_t)BRIDGE_MEM16(va);
                        if (wide) counted /= 2;
                    }
                    va = text;
                }
                if (!va) { wide = 0; counted = 6; }
                if (counted >= 0) {
                    n = counted;
                } else {
                    while ((prec < 0 || n < prec) &&
                           (wide ? BRIDGE_MEM16(va + (uint32_t)n * 2) : BRIDGE_MEM8(va + (uint32_t)n))) n++;
                }
                if (prec >= 0 && n > prec) n = prec;
                pad = have_width && width > n ? width - n : 0;
                if (!left) while (pad-- > 0) PUT(' ');
                for (k = 0; k < n; k++) {
                    uint32_t c;
                    if (!va) c = (uint8_t)"(null)"[k];
                    else if (wide) { c = BRIDGE_MEM16(va + (uint32_t)k * 2); if (c >= 0x100) c = '?'; }
                    else c = BRIDGE_MEM8(va + (uint32_t)k);
                    PUT(c);
                }
                if (left) while (pad-- > 0) PUT(' ');
                break;
            }
            case 'n': {
                uint32_t tva = bridge_va_next32(&pos);
                if (tva) {
                    if (size == 1) BRIDGE_MEM16(tva) = (uint16_t)used;
                    else BRIDGE_MEM32(tva) = (uint32_t)used;
                }
                break;
            }
            case 'e': case 'E': case 'f': case 'g': case 'G':
                bridge_va_next64(&pos);
                break;
            default:
                break;
            }
        }
    }
#undef PUT

    if (overflow) return -1;
    if (used < cap) out[used] = 0;
    return (int)used;
}

/* --- RtlSnprintf (ordinal 361, __cdecl, caller cleans) --- */
static void bridge_RtlSnprintf(void)
{
    char     *dst  = (char *)XBOX_TO_NATIVE(STACK_ARG(0));
    uint32_t  count = STACK_ARG(1);
    const char *fmt = (const char *)XBOX_TO_NATIVE(STACK_ARG(2));

    if (!dst || !fmt) {
        g_eax = 0;
        return;
    }
    /* Varargs start after buffer/count/format on the guest stack. */
    g_eax = bridge_format_printf(dst, count, fmt, g_esp + 12);
}

/* --- RtlSprintf (ordinal 362, __cdecl, caller cleans) --- */
static void bridge_RtlSprintf(void)
{
    char     *dst  = (char *)XBOX_TO_NATIVE(STACK_ARG(0));
    const char *fmt = (const char *)XBOX_TO_NATIVE(STACK_ARG(1));

    if (!dst || !fmt) {
        g_eax = 0;
        return;
    }
    g_eax = bridge_format_printf(dst, 0x7FFFFFFF, fmt, g_esp + 8);   /* unbounded, as the kernel */
}

/* --- RtlVsnprintf (ordinal 363, __cdecl, caller cleans) --- */
static void bridge_RtlVsnprintf(void)
{
    char     *dst  = (char *)XBOX_TO_NATIVE(STACK_ARG(0));
    uint32_t  count = STACK_ARG(1);
    const char *fmt = (const char *)XBOX_TO_NATIVE(STACK_ARG(2));
    uint32_t  list = STACK_ARG(3);   /* x86 va_list = guest pointer to args */

    if (!dst || !fmt || !list) {
        g_eax = 0;
        return;
    }
    g_eax = bridge_format_printf(dst, count, fmt, list);
}

/* --- RtlVsprintf (ordinal 364, __cdecl, caller cleans) --- */
static void bridge_RtlVsprintf(void)
{
    char     *dst  = (char *)XBOX_TO_NATIVE(STACK_ARG(0));
    const char *fmt = (const char *)XBOX_TO_NATIVE(STACK_ARG(1));
    uint32_t  list = STACK_ARG(2);   /* x86 va_list = guest pointer to args */

    if (!dst || !fmt || !list) {
        g_eax = 0;
        return;
    }
    g_eax = bridge_format_printf(dst, 0x7FFFFFFF, fmt, list);   /* unbounded, as the kernel */
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Port I/O (329, 330, 331, 332)
 *
 * The port argument is an I/O window the title mapped with MmMapIoSpace, so
 * it is an ordinary guest VA in the memory model. The READ variants copy the
 * requested element count from the mapped window into the caller's buffer;
 * the WRITE variants are no-ops (mirroring the existing 333/334 bridges).
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- READ_PORT_BUFFER_UCHAR (ordinal 329, 3 args = 12 bytes) --- */
static void bridge_READ_PORT_BUFFER_UCHAR(void)
{
    /* The first argument is an I/O port, not an address: no device answers here, so a floating bus's all ones. */
    void *dst = XBOX_TO_NATIVE(STACK_ARG(1));
    uint32_t count = STACK_ARG(2);

    if (dst)
        memset(dst, 0xFF, count);
    g_eax = 0;
}

/* --- READ_PORT_BUFFER_USHORT (ordinal 330, 3 args = 12 bytes) --- */
static void bridge_READ_PORT_BUFFER_USHORT(void)
{
    /* The first argument is an I/O port, not an address: no device answers here, so a floating bus's all ones. */
    void *dst = XBOX_TO_NATIVE(STACK_ARG(1));
    uint32_t count = STACK_ARG(2);

    if (dst)
        memset(dst, 0xFF, count * sizeof(uint16_t));
    g_eax = 0;
}

/* --- READ_PORT_BUFFER_ULONG (ordinal 331, 3 args = 12 bytes) --- */
static void bridge_READ_PORT_BUFFER_ULONG(void)
{
    /* The first argument is an I/O port, not an address: no device answers here, so a floating bus's all ones. */
    void *dst = XBOX_TO_NATIVE(STACK_ARG(1));
    uint32_t count = STACK_ARG(2);

    if (dst)
        memset(dst, 0xFF, count * sizeof(uint32_t));
    g_eax = 0;
}

/* --- WRITE_PORT_BUFFER_UCHAR (ordinal 332, 3 args = 12 bytes) --- */
static void bridge_WRITE_PORT_BUFFER_UCHAR(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    g_eax = 0;
}

static int stdcall_args_for_ordinal(ULONG ordinal)
{
    switch (ordinal) {
    /* ── Display / AV ── */
    case   1: return  0;  /* AvGetSavedDataAddress (void) */
    case   2: return 16;  /* AvSendTVEncoderOption (4) */
    case   3: return 24;  /* AvSetDisplayMode (6) */
    case   4: return  4;  /* AvSetSavedDataAddress (1) */
    case   5: return  0;  /* DbgBreakPoint (void) */
    case   8: return  0;  /* DbgPrint - __cdecl varargs, caller cleans */
    case   9: return  8;  /* HalReadSMCTrayState (2) */
    case  14: return  4;  /* ExAllocatePool (1) */
    case  15: return  8;  /* ExAllocatePoolWithTag (2) */
    case  17: return  4;  /* ExFreePool (1) */
    case  23: return  4;  /* ExQueryPoolBlockSize (1) */
    case  24: return 20;  /* ExQueryNonVolatileSetting (5) */
    case  29: return 16;  /* ExSaveNonVolatileSetting (4) */
    case  35: return  0;  /* FscGetCacheSize (void) */
    case  37: return  4;  /* FscSetCacheSize (1) */
    case  38: return  0;  /* HalClearSoftwareInterrupt (fastcall) */
    case  39: return  4;  /* HalDisableSystemInterrupt (1) */
    case  42: return  0;  /* HalDiskSerialNumber - data export */
    case  44: return  8;  /* HalGetInterruptVector (2) */
    case  47: return  8;  /* HalRegisterShutdownNotification (2) */
    case  46: return 24;  /* HalReadWritePCISpace (6) */
    case  45: return 16;  /* HalReadSMBusValue (4) */
    case  50: return 16;  /* HalWriteSMBusValue (4) */
    case  48: return  0;  /* HalRequestSoftwareInterrupt (fastcall) */
    case  49: return  4;  /* HalReturnToFirmware (1) */
    case  61: return 36;  /* IoBuildDeviceIoControlRequest (9) */
    case  62: return 28;  /* IoBuildSynchronousFsdRequest (7) */
    case  65: return 24;  /* IoCreateDevice (6) */
    case  66: return 40;  /* IoCreateFile (10) */
    case  67: return  8;  /* IoCreateSymbolicLink (2) */
    case  68: return  4;  /* IoDeleteDevice (1) */
    case  69: return  4;  /* IoDeleteSymbolicLink (1) */
    case  73: return 12;  /* IoInitializeIrp (3) */
    case  74: return  8;  /* IoInvalidDeviceRequest (2) */
    case  79: return 20;  /* IoSetIoCompletion (5) */
    case  81: return  4;  /* IoStartNextPacket (1) */
    case  82: return  8;  /* IoStartNextPacketByKey (2) */
    case  83: return 12;  /* IoStartPacket (3) */
    case  84: return 32;  /* IoSynchronousDeviceIoControlRequest (8) */
    case  85: return 20;  /* IoSynchronousFsdRequest (5) */
    case  86: return  0;  /* IofCallDriver (fastcall: args in ecx/edx) */
    case  87: return  0;  /* IofCompleteRequest (fastcall: args in ecx/edx) */
    /* Missing this entry cost the Xbox Dashboard its whole boot. Ordinal 91 has
     * no bridge, so the generic stub ran -- and with no arg count it left the
     * one pushed argument on the guest stack. The caller (sub_00032859) then
     * ran `pop edi; pop esi; pop ebx` four bytes low and came back with its
     * registers rotated, which destroyed the XApp `this` pointer two frames up.
     * Its scene manager was never created, its scene never loaded, and it
     * returned to firmware -- reported as nothing more than "returning 0". */
    case  90: return  4;  /* IoDismountVolume (1) */
    case  91: return  4;  /* IoDismountVolumeByName (1) */
    case  93: return  8;  /* KeAlertThread (2) */
    case  95: return  4;  /* KeBugCheck (1) */
    case  96: return 20;  /* KeBugCheckEx (5) */
    case  97: return  4;  /* KeCancelTimer (1) */
    case  98: return  4;  /* KeConnectInterrupt (1) */
    case  99: return 12;  /* KeDelayExecutionThread (3) */
    case 100: return  4;  /* KeDisconnectInterrupt (1) */
    case 107: return 12;  /* KeInitializeDpc (3) */
    case 109: return 28;  /* KeInitializeInterrupt (7) */
    case 113: return  8;  /* KeInitializeTimerEx (2) */
    case 119: return 12;  /* KeInsertQueueDpc (3) */
    case 124: return  4;  /* KeQueryBasePriorityThread (1) */
    case 125: return  0;  /* KeQueryInterruptTime (void) */
    case 126: return  0;  /* KeQueryPerformanceCounter (void) */
    case 127: return  0;  /* KeQueryPerformanceFrequency (void) */
    case 128: return  4;  /* KeQuerySystemTime (1) */
    case 129: return  0;  /* KeRaiseIrqlToDpcLevel (void) */
    case 137: return  4;  /* KeRemoveQueueDpc (1) */
    case 139: return  4;  /* KeRestoreFloatingPointState (1) */
    case 142: return  4;  /* KeSaveFloatingPointState (1) */
    case 143: return  8;  /* KeSetBasePriorityThread (2) */
    case 144: return  8;  /* KeSetDisableBoostThread (2) */
    case 145: return 12;  /* KeSetEvent (3) */
    case 149: return 16;  /* KeSetTimer (Timer+DueTime[8]+Dpc) */
    case 150: return 20;  /* KeSetTimerEx (Timer+DueTime[8]+Period+Dpc) */
    case 151: return  4;  /* KeStallExecutionProcessor (1) */
    case 153: return 12;  /* KeSynchronizeExecution (3) */
    case 158: return 32;  /* KeWaitForMultipleObjects (8) */
    case 159: return 20;  /* KeWaitForSingleObject (5) */
    case 160: return  0;  /* KfRaiseIrql (fastcall: arg in ecx) */
    case 161: return  0;  /* KfLowerIrql (fastcall: arg in ecx) */
    case 165: return  4;  /* MmAllocateContiguousMemory (1) */
    case 166: return 20;  /* MmAllocateContiguousMemoryEx (5) */
    case 167: return  8;  /* MmAllocateSystemMemory (2) */
    case 168: return  8;  /* MmClaimGpuInstanceMemory (2) */
    case 169: return  8;  /* MmCreateKernelStack (2) */
    case 170: return  8;  /* MmDeleteKernelStack (2) */
    case 171: return  4;  /* MmFreeContiguousMemory (1) */
    case 172: return  8;  /* MmFreeSystemMemory (2) */
    case 173: return  4;  /* MmGetPhysicalAddress (1) */
    case 175: return 12;  /* MmLockUnlockBufferPages (3) */
    case 176: return  8;  /* MmLockUnlockPhysicalPage (2) */
    case 177: return 12;  /* MmMapIoSpace (3) */
    case 178: return 12;  /* MmPersistContiguousMemory (3) */
    case 179: return  4;  /* MmQueryAddressProtect (1) */
    case 180: return  4;  /* MmQueryAllocationSize (1) */
    case 181: return  4;  /* MmQueryStatistics (1) */
    case 182: return 12;  /* MmSetAddressProtect (3) */
    case 183: return  8;  /* MmUnmapIoSpace (2) */
    case 184: return 20;  /* NtAllocateVirtualMemory (5) */
    case 185: return  8;  /* NtCancelTimer (2) */
    case 186: return  4;  /* NtClearEvent (1) */
    case 187: return  4;  /* NtClose (1) */
    case 188: return  8;  /* NtCreateDirectoryObject (2) */
    case 189: return 16;  /* NtCreateEvent (4) */
    case 190: return 36;  /* NtCreateFile (9) */
    case 191: return 16;  /* NtCreateIoCompletion (4) */
    case 192: return 12;  /* NtCreateMutant (3) */
    case 193: return 16;  /* NtCreateSemaphore (4) */
    case 194: return 12;  /* NtCreateTimer (3) */
    case 195: return  4;  /* NtDeleteFile (1) */
    case 196: return 40;  /* NtDeviceIoControlFile (10) */
    case 197: return 12;  /* NtDuplicateObject (3) */
    case 198: return  8;  /* NtFlushBuffersFile (2) */
    case 199: return 12;  /* NtFreeVirtualMemory (3) */
    case 200: return 40;  /* NtFsControlFile (10) */
    case 202: return 24;  /* NtOpenFile (6) */
    case 203: return  8;  /* NtOpenSymbolicLinkObject (2) */
    case 204: return 16;  /* NtProtectVirtualMemory (4) */
    case 205: return  8;  /* NtPulseEvent (2) */
    case 206: return 20;  /* NtQueueApcThread (5) */
    case 207: return 40;  /* NtQueryDirectoryFile (10) */
    case 210: return  8;  /* NtQueryFullAttributesFile (2) */
    case 211: return 20;  /* NtQueryInformationFile (5) */
    case 215: return 12;  /* NtQuerySymbolicLinkObject (3) */
    case 217: return 8;   /* NtQueryVirtualMemory (2) -- Xbox takes
                             BaseAddress and Info only, not NT's four */
    case 218: return 20;  /* NtQueryVolumeInformationFile (5) */
    case 219: return 32;  /* NtReadFile (8) */
    case 220: return 32;  /* NtReadFileScatter (8) */
    case 221: return  8;  /* NtReleaseMutant (2) */
    case 222: return 12;  /* NtReleaseSemaphore (3) */
    case 223: return 20;  /* NtRemoveIoCompletion (5) */
    case 224: return  8;  /* NtResumeThread (2) */
    case 225: return  8;  /* NtSetEvent (2) */
    case 226: return 20;  /* NtSetInformationFile (5) */
    case 227: return 20;  /* NtSetIoCompletion (5) */
    case 228: return  8;  /* NtSetSystemTime (2) */
    case 229: return 32;  /* NtSetTimerEx (8) */
    case 230: return 20;  /* NtSignalAndWaitForSingleObjectEx (5) */
    case 231: return  8;  /* NtSuspendThread (2) */
    case 232: return 12;  /* NtUserIoApcDispatcher (3) */
    case 233: return 12;  /* NtWaitForSingleObject (3) */
    case 234: return 16;  /* NtWaitForSingleObjectEx (4) */
    case 235: return 24;  /* NtWaitForMultipleObjectsEx (6) */
    case 236: return 32;  /* NtWriteFile (8) */
    case 237: return 32;  /* NtWriteFileGather (8) */
    case 238: return  0;  /* NtYieldExecution (void) */
    case 243: return 16;  /* ObOpenObjectByName (4) */
    case 247: return 20;  /* ObReferenceObjectByName (5) */
    case 250: return  0;  /* ObfDereferenceObject (fastcall: arg in ecx) */
    case 251: return  0;  /* ObfReferenceObject (fastcall: arg in ecx) */
    case 252: return  4;  /* PhyGetLinkState (1) */
    case 253: return  8;  /* PhyInitialize (2) */
    case 255: return 40;  /* PsCreateSystemThreadEx (10) */
    case 258: return  4;  /* PsTerminateSystemThread (1) */
    case 260: return 12;  /* RtlAnsiStringToUnicodeString (3) */
    case 268: return 12;  /* RtlCompareMemory (3) */
    case 269: return 12;  /* RtlCompareMemoryUlong (3) */
    case 270: return 12;  /* RtlCompareString (3) */
    case 261: return  8;  /* RtlAppendStringToString (2) */
    case 262: return  8;  /* RtlAppendUnicodeStringToString (2) */
    case 263: return  8;  /* RtlAppendUnicodeToString (2) */
    case 264: return 16;  /* RtlAssert (4) */
    case 265: return  4;  /* RtlCaptureContext (1) */
    case 266: return 16;  /* RtlCaptureStackBackTrace (4) */
    case 267: return 12;  /* RtlCharToInteger (3) */
    case 271: return 12;  /* RtlCompareUnicodeString (3) */
    case 272: return  8;  /* RtlCopyString (2) */
    case 273: return  8;  /* RtlCopyUnicodeString (2) */
    case 274: return  8;  /* RtlCreateUnicodeString (2) */
    case 275: return  4;  /* RtlDowncaseUnicodeChar (1) */
    case 276: return 12;  /* RtlDowncaseUnicodeString (3) */
    case 278: return  4;  /* RtlEnterCriticalSectionAndRegion (1) */
    case 280: return 12;  /* RtlEqualUnicodeString (3) */
    case 284: return 12;  /* RtlFillMemory (3) */
    case 287: return  4;  /* RtlFreeUnicodeString (1) */
    case 288: return  8;  /* RtlGetCallersAddress (2) */
    case 292: return 16;  /* RtlIntegerToChar (4) */
    case 293: return 12;  /* RtlIntegerToUnicodeString (3) */
    case 295: return  4;  /* RtlLeaveCriticalSectionAndRegion (1) */
    case 296: return  4;  /* RtlLowerChar (1) */
    case 297: return  8;  /* RtlMapGenericMask (2) */
    case 298: return 12;  /* RtlMoveMemory (3) */
    case 299: return 20;  /* RtlMultiByteToUnicodeN (5) */
    case 300: return 12;  /* RtlMultiByteToUnicodeSize (3) */
    case 306: return  4;  /* RtlTryEnterCriticalSection (1) */
    case 307: return  0;  /* RtlUlongByteSwap (fastcall) */
    case 309: return 12;  /* RtlUnicodeStringToInteger (3) */
    case 310: return 20;  /* RtlUnicodeToMultiByteN (5) */
    case 311: return 12;  /* RtlUnicodeToMultiByteSize (3) */
    case 313: return  4;  /* RtlUpcaseUnicodeChar (1) */
    case 314: return 12;  /* RtlUpcaseUnicodeString (3) */
    case 315: return 20;  /* RtlUpcaseUnicodeToMultiByteN (5) */
    case 316: return  4;  /* RtlUpperChar (1) */
    case 317: return  8;  /* RtlUpperString (2) */
    case 318: return  0;  /* RtlUshortByteSwap (fastcall) */
    case 319: return 12;  /* RtlWalkFrameChain (3) */
    case 320: return  8;  /* RtlZeroMemory (2) */
    case 321: return  0;  /* XboxEEPROMKey (void) */
    case 277: return  4;  /* RtlEnterCriticalSection (1) */
    case 279: return 12;  /* RtlEqualString (3) */
    case 285: return 12;  /* RtlFillMemoryUlong (3) */
    case 286: return  4;  /* RtlFreeAnsiString (1) */
    case 289: return  8;  /* RtlInitAnsiString (2) */
    case 290: return  8;  /* RtlInitUnicodeString (2) */
    case 291: return  4;  /* RtlInitializeCriticalSection (1) */
    case 294: return  4;  /* RtlLeaveCriticalSection (1) */
    case 301: return  4;  /* RtlNtStatusToDosError (1) */
    case 302: return  4;  /* RtlRaiseException (1) */
    case 304: return  8;  /* RtlTimeFieldsToTime (2) */
    case 305: return  8;  /* RtlTimeToTimeFields (2) */
    case 308: return 12;  /* RtlUnicodeStringToAnsiString (3) */
    case 312: return 16;  /* RtlUnwind (4) */
    case 327: return  4;  /* XeLoadSection (1) */
    case 328: return  4;  /* XeUnloadSection (1) */
    case 333: return 12;  /* WRITE_PORT_BUFFER_USHORT (3) */
    case 334: return 12;  /* WRITE_PORT_BUFFER_ULONG (3) */
    case 335: return  4;  /* XcSHAInit (1) */
    case 336: return 12;  /* XcSHAUpdate (3) */
    case 337: return  8;  /* XcSHAFinal (2) */
    case 338: return 12;  /* XcRC4Key (3) */
    case 339: return 12;  /* XcRC4Crypt (3) */
    case 340: return 28;  /* XcHMAC (7) */
    case 341: return 12;  /* XcPKEncPublic (3) */
    case 342: return 12;  /* XcPKDecPrivate (3) */
    case 343: return  4;  /* XcPKGetKeyLen (1) */
    case 344: return 12;  /* XcVerifyPKCS1Signature (3) */
    case 345: return 20;  /* XcModExp (5) */
    case 346: return  8;  /* XcDESKeyParity (2) */
    case 347: return 12;  /* XcKeyTable (3) */
    case 348: return 20;  /* XcBlockCrypt (5) */
    case 349: return 28;  /* XcBlockCryptCBC (7) */
    case 350: return  8;  /* XcCryptService (2) */
    case 351: return  8;  /* XcUpdateCrypto (2) */
    case 352: return 12;  /* RtlRip (3) */
    case 358: return  0;  /* HalIsResetOrShutdownPending (void) */
    case 359: return  4;  /* IoMarkIrpMustComplete (1) */

    /* ── Unknown stubs ── */

    /* ── Pool Allocator ── */
    /* ordinal 16 is the ExEventObjectType data export; see
     * kernel_thunks.c, which points its thunk at kernel data.
     * 17 is ExFreePool and is a real function. */

    /* ── HAL ── */

    /* ── I/O Manager ── */
    /* ordinal 64 is the IoCompletionObjectType data export; see
     * kernel_thunks.c. 65 is IoCreateDevice, a real function. */
    /* case  71: DATA export - IoDeviceObjectType */

    /* ── Kernel Synchronization ── */
    /* case 156: DATA export - KeTickCount */

    /* ── Launch Data ── */
    /* case 164: DATA export - LaunchDataPage */

    /* ── Memory Management ── */

    /* ── NT Virtual Memory ── */

    /* ── NT File I/O & Handle ── */

    /* ── Object Manager ── */
    case 246: return 12;  /* ObReferenceObjectByHandle(3) - Xbox: Handle,Type,Object* */
    case 360: return  0;  /* HalInitiateShutdown (void) */

    /* ── Network / PHY ── */

    /* ── Threading ── */
    /* case 259: DATA export - PsThreadObjectType */

    /* ── Runtime Library ── */

    /* ── Xbox Identity (data exports) ── */
    /* cases 322-328, 355-357: DATA exports */

    /* ── Port I/O ── */

    /* ── Crypto ── */

    /* Data exports */
    case 102: return 0;  /* MmGlobalData (void/data) */
    case 120: return 0;  /* KeInterruptTime (void/data) */
    case 154: return 0;  /* KeSystemTime (void/data) */
    case 240: return 0;  /* ObDirectoryObjectType (void/data) */
    case 245: return 0;  /* ObpObjectHandleTable (void/data) */
    case 249: return 0;  /* ObSymbolicLinkObjectType (void/data) */

    /* Dbg */
    case   6: return  4;  /* DbgBreakPointWithStatus (1) */
    case   7: return 12;  /* DbgLoadImageSymbols (3) */
    case  10: return 12;  /* DbgPrompt (3) */
    case  11: return 12;  /* DbgUnLoadImageSymbols (3) */

    /* Ex */
    case  12: return  4;  /* ExAcquireReadWriteLockExclusive (1) */
    case  13: return  4;  /* ExAcquireReadWriteLockShared (1) */
    case  18: return  4;  /* ExInitializeReadWriteLock (1) */
    case  19: return 16;  /* ExInterlockedAddLargeInteger (Addend, Increment[8], Lock) */
    case  20: return  0;  /* ExInterlockedAddLargeStatistic (fastcall) */
    case  21: return  4;  /* ExInterlockedCompareExchange64 (fastcall, 1 on the stack) */
    case  25: return 12;  /* ExReadWriteRefurbInfo (3) */
    case  26: return  4;  /* ExRaiseException (1) */
    case  27: return  4;  /* ExRaiseStatus (1) */
    case  28: return  4;  /* ExReleaseReadWriteLock (1) */

    /* ExfInterlocked */
    case  32: return  0;  /* ExfInterlockedInsertHeadList (fastcall) */
    case  33: return  0;  /* ExfInterlockedInsertTailList (fastcall) */
    case  34: return  0;  /* ExfInterlockedRemoveHeadList (fastcall) */

    /* Fsc */
    case  36: return  0;  /* FscInvalidateIdleBlocks (void) */

    /* Hal */
    case  43: return  8;  /* HalEnableSystemInterrupt (2) */
    case 365: return  0;  /* HalEnableSecureTrayEject (void) */
    case 366: return  4;  /* HalWriteSMCScratchRegister (1) */

    /* Interlocked (fastcall, args in ecx/edx) */
    case  51: return  4;  /* InterlockedCompareExchange (fastcall, 1 on the stack) */
    case  52: return  0;  /* InterlockedDecrement (fastcall) */
    case  53: return  0;  /* InterlockedIncrement (fastcall) */
    case  54: return  0;  /* InterlockedExchange (fastcall) */
    case  55: return  0;  /* InterlockedExchangeAdd (fastcall) */
    case  56: return  0;  /* InterlockedFlushSList (fastcall) */
    case  57: return  0;  /* InterlockedPopEntrySList (fastcall) */
    case  58: return  0;  /* InterlockedPushEntrySList (fastcall) */

    /* Io */
    case  59: return  4;  /* IoAllocateIrp (1) */
    case  60: return 24;  /* IoBuildAsynchronousFsdRequest (6) */
    case  63: return 20;  /* IoCheckShareAccess (5) */
    case  72: return  4;  /* IoFreeIrp (1) */
    case  75: return 20;  /* IoQueryFileInformation (5) */
    case  76: return 20;  /* IoQueryVolumeInformation (5) */
    case  77: return  4;  /* IoQueueThreadIrp (1) */
    case  78: return  8;  /* IoRemoveShareAccess (2) */
    case  80: return 16;  /* IoSetShareAccess (4) */

    /* Kd */
    case  88: return  0;  /* KdDebuggerEnabled (void) */
    case  89: return  0;  /* KdDebuggerNotPresent (void) */

    /* Ke (new) */
    case  92: return  4;  /* KeAlertResumeThread (1) */
    case  94: return  8;  /* KeBoostPriorityThread (2) */
    case 101: return  0;  /* KeEnterCriticalRegion (void) */
    case 103: return  0;  /* KeGetCurrentIrql (void) */
    case 104: return  0;  /* KeGetCurrentThread (void) */
    case 105: return 28;  /* KeInitializeApc (7) */
    case 106: return  4;  /* KeInitializeDeviceQueue (1) */
    case 108: return 12;  /* KeInitializeEvent (3) */
    case 110: return  8;  /* KeInitializeMutant (2) */
    case 111: return  8;  /* KeInitializeQueue (2) */
    case 112: return 12;  /* KeInitializeSemaphore (3) */
    case 114: return 12;  /* KeInsertByKeyDeviceQueue (3) */
    case 115: return  8;  /* KeInsertDeviceQueue (2) */
    case 116: return  8;  /* KeInsertHeadQueue (2) */
    case 117: return  8;  /* KeInsertQueue (2) */
    case 118: return 16;  /* KeInsertQueueApc (4) */
    case 121: return  0;  /* KeIsExecutingDpc (void) */
    case 122: return  0;  /* KeLeaveCriticalRegion (void) */
    case 123: return 12;  /* KePulseEvent (3) */
    case 130: return  0;  /* KeRaiseIrqlToSynchLevel (void) */
    case 131: return 16;  /* KeReleaseMutant (4) */
    case 132: return 16;  /* KeReleaseSemaphore (4) */
    case 133: return  8;  /* KeRemoveByKeyDeviceQueue (2) */
    case 134: return  4;  /* KeRemoveDeviceQueue (1) */
    case 135: return  8;  /* KeRemoveEntryDeviceQueue (2) */
    case 136: return 12;  /* KeRemoveQueue (3) */
    case 138: return  4;  /* KeResetEvent (1) */
    case 140: return  4;  /* KeResumeThread (1) */
    case 141: return  4;  /* KeRundownQueue (1) */
    case 146: return  8;  /* KeSetEventBoostPriority (2) */
    case 147: return  8;  /* KeSetPriorityProcess (2) */
    case 148: return  8;  /* KeSetPriorityThread (2) */
    case 152: return  4;  /* KeSuspendThread (1) */
    case 155: return  4;  /* KeTestAlertThread (1) */
    case 162: return  0;  /* KiBugCheckData (void/data) */
    case 163: return  0;  /* KiUnlockDispatcherDatabase (fastcall) */

    /* Mm */
    case 174: return  4;  /* MmIsAddressValid (1) */
    case 374: return  8;  /* MmDbgAllocateMemory (2) */
    case 375: return  8;  /* MmDbgFreeMemory (2) */
    case 376: return  0;  /* MmDbgQueryAvailablePages (void) */
    case 377: return  8;  /* MmDbgReleaseAddress (2) */
    case 378: return  8;  /* MmDbgWriteCheck (2) */

    /* Nt */
    case 201: return  8;  /* NtOpenDirectoryObject (2) */
    case 208: return 24;  /* NtQueryDirectoryObject (6) */
    case 209: return  8;  /* NtQueryEvent (2) */
    case 212: return  8;  /* NtQueryIoCompletion (2) */
    case 213: return  8;  /* NtQueryMutant (2) */
    case 214: return  8;  /* NtQuerySemaphore (2) */
    case 216: return  8;  /* NtQueryTimer (2) */

    /* Ob */
    case 239: return 16;  /* ObCreateObject (4) */
    case 241: return 16;  /* ObInsertObject (4) */
    case 242: return  4;  /* ObMakeTemporaryObject (1) */
    case 244: return 12;  /* ObOpenObjectByPointer (3) */
    case 248: return  8;  /* ObReferenceObjectByPointer (2) */

    /* Ps */
    case 254: return 20;  /* PsCreateSystemThread (5) */
    case 256: return  4;  /* PsQueryStatistics (1) */
    case 257: return  4;  /* PsSetCreateThreadNotifyRoutine (1) */

    /* Rtl */
    case 281: return 12;  /* RtlExtendedIntegerMultiply (3) */
    case 282: return 16;  /* RtlExtendedLargeIntegerDivide (4) */
    case 283: return 20;  /* RtlExtendedMagicDivide (Dividend[8], Divisor[8], Shift) */
    case 303: return  4;  /* RtlRaiseStatus (1) */
    case 361: return  0;  /* RtlSnprintf - __cdecl, caller cleans */
    case 362: return  0;  /* RtlSprintf - __cdecl, caller cleans */
    case 363: return  0;  /* RtlVsnprintf - __cdecl, caller cleans */
    case 364: return  0;  /* RtlVsprintf - __cdecl, caller cleans */

    /* Port I/O */
    case 329: return 12;  /* READ_PORT_BUFFER_UCHAR (3) */
    case 330: return 12;  /* READ_PORT_BUFFER_USHORT (3) */
    case 331: return 12;  /* READ_PORT_BUFFER_ULONG (3) */
    case 332: return 12;  /* WRITE_PORT_BUFFER_UCHAR (3) */


    /* Not 0: a genuine zero-argument function and an ordinal nobody has
     * written down are both "pop nothing", but only one of them is a
     * problem, and the warning below could not tell them apart -- it
     * accused FscGetCacheSize, which really does take no arguments. */
    default:  return -1;  /* DATA exports or truly unknown */
    }
}

/* XcUpdateCrypto(New, Old): sixteen routines in this order, each non-zero one replacing the kernel's. A title's own
 * then runs in place of ours. Old would get the kernel's routines to chain to: we have no guest addresses for ours, so
 * it gets zeros. */
static uint32_t g_xc_hook[16];
static void bridge_XcUpdateCrypto(void)
{
    uint32_t in = STACK_ARG(0), out = STACK_ARG(1);
    for (int i = 0; i < 16; i++) {
        uint32_t v = in ? BRIDGE_MEM32(in + i * 4u) : 0;
        if (v) g_xc_hook[i] = v;
    }
    if (out) memset(XBOX_TO_NATIVE(out), 0, 64);
    g_eax = 0;
}

#define XC_HOOKED(i, argc, name) \
    static void bridge_hooked_##name(void) { \
        if (g_xc_hook[i]) { \
            uint32_t a[8]; \
            for (int k = 0; k < (argc); k++) a[k] = STACK_ARG(k); \
            g_eax = bridge_call_guest(g_xc_hook[i], (argc), a); \
            return; \
        } \
        bridge_##name(); \
    }
XC_HOOKED(0, 1, XcSHAInit)
XC_HOOKED(1, 3, XcSHAUpdate)
XC_HOOKED(2, 2, XcSHAFinal)
XC_HOOKED(3, 3, XcRC4Key)
XC_HOOKED(4, 3, XcRC4Crypt)
XC_HOOKED(5, 7, XcHMAC)
XC_HOOKED(6, 3, XcPKEncPublic)
XC_HOOKED(7, 3, XcPKDecPrivate)
XC_HOOKED(8, 1, XcPKGetKeyLen)
XC_HOOKED(9, 3, XcVerifyPKCS1Signature)
XC_HOOKED(10, 5, XcModExp)
XC_HOOKED(11, 2, XcDESKeyParity)
XC_HOOKED(12, 3, XcKeyTable)
XC_HOOKED(13, 5, XcBlockCrypt)
XC_HOOKED(14, 7, XcBlockCryptCBC)
XC_HOOKED(15, 2, XcCryptService)

static bridge_func_t bridge_for_ordinal(ULONG ordinal)
{
    switch (ordinal) {
    /* Threading */
    case 255: return bridge_PsCreateSystemThreadEx;
    case 258: return bridge_PsTerminateSystemThread;

    /* File/Handle */
    case 187: return bridge_NtClose;
    case 190: return bridge_NtCreateFile;
    case 279: return bridge_RtlEqualString;
    case 289: return bridge_RtlInitAnsiString;
    case 195: return bridge_NtDeleteFile;
    case 196: return bridge_NtDeviceIoControlFile;
    case 198: return bridge_NtFlushBuffersFile;
    case 200: return bridge_NtFsControlFile;
    case 202: return bridge_NtOpenFile;
    case 203: return bridge_NtOpenSymbolicLinkObject;
    case 207: return bridge_NtQueryDirectoryFile;
    case 210: return bridge_NtQueryFullAttributesFile;
    case 211: return bridge_NtQueryInformationFile;
    case 218: return bridge_NtQueryVolumeInformationFile;
    case 219: return bridge_NtReadFile;
    case 312: return bridge_RtlUnwind;
    case 327: return bridge_XeLoadSection;
    case 328: return bridge_XeUnloadSection;
    case 226: return bridge_NtSetInformationFile;
    case 236: return bridge_NtWriteFile;

    /* Memory - contiguous */
    case 165: return bridge_MmAllocateContiguousMemory;
    case 166: return bridge_MmAllocateContiguousMemoryEx;
    case 171: return bridge_MmFreeContiguousMemory;
    case 173: return bridge_MmGetPhysicalAddress;
    case 182: return bridge_MmSetAddressProtect;
    case 181: return bridge_MmQueryStatistics;

    /* Memory - virtual */
    case 184: return bridge_NtAllocateVirtualMemory;
    case 199: return bridge_NtFreeVirtualMemory;
    case 215: return bridge_NtQuerySymbolicLinkObject;
    case 217: return bridge_NtQueryVirtualMemory;

    /* Pool */
    case  14: return bridge_ExAllocatePool;
    case  15: return bridge_ExAllocatePoolWithTag;
    case  23: return bridge_ExQueryPoolBlockSize;
    case 268: return bridge_RtlCompareMemory;
    case 269: return bridge_RtlCompareMemoryUlong;
    case  35: return bridge_FscGetCacheSize;
    case  37: return bridge_FscSetCacheSize;
    case  24: return bridge_ExQueryNonVolatileSetting;

    /* IRQL */
    case 160: return bridge_KfRaiseIrql;
    case 161: return bridge_KfLowerIrql;
    case 129: return bridge_KeRaiseIrqlToDpcLevel;

    /* Critical sections */
    case 291: return bridge_RtlInitializeCriticalSection;
    /* Wreckless asks for the AV pack during D3D device creation; unbridged it
     * read 0, which is "no pack connected". Writes one ULONG through a guest
     * pointer it is given and nothing else, and XBOX_TO_NATIVE turns a guest
     * NULL into a host NULL so xbox_AvSendTVEncoderOption's own !Result guard
     * still fires. */
    case   2: return bridge_AvSendTVEncoderOption;
    /* Reads a LARGE_INTEGER and fills a TIME_FIELDS, both at caller-supplied
     * guest addresses -- the case the NOT ROUTED note below names as fine. */
    case 305: return bridge_RtlTimeToTimeFields;
    case 277: return bridge_RtlEnterCriticalSection;
    case 294: return bridge_RtlLeaveCriticalSection;

    /* Timing */
    case 126: return bridge_KeQueryPerformanceCounter;
    case 127: return bridge_KeQueryPerformanceFrequency;
    case 128: return bridge_KeQuerySystemTime;
    case 149: return bridge_KeSetTimer;
    case 150: return bridge_KeSetTimerEx;

    /* DPC / Timer init */
    case 107: return bridge_KeInitializeDpc;
    case 119: return bridge_KeInsertQueueDpc;
    case 137: return bridge_KeRemoveQueueDpc;
    case 153: return bridge_KeSynchronizeExecution;
    case 113: return bridge_KeInitializeTimerEx;

    /* NV2A interrupt plumbing */
    case  44: return bridge_HalGetInterruptVector;
    case  98: return bridge_KeConnectInterrupt;
    case 109: return bridge_KeInitializeInterrupt;
    case  47: return bridge_HalRegisterShutdownNotification;
    case 168: return bridge_MmClaimGpuInstanceMemory;

    /* Synchronization */
    case 189: return bridge_NtCreateEvent;
    case 145: return bridge_KeSetEvent;
    case 159: return bridge_KeWaitForSingleObject;
    case  99: return bridge_KeDelayExecutionThread;
    case 179: return bridge_MmQueryAddressProtect;
    case 232: return bridge_NtUserIoApcDispatcher;
    case  95: return bridge_KeBugCheck;
    case  96: return bridge_KeBugCheckEx;
    case 186: return bridge_NtClearEvent;
    case 205: return bridge_NtPulseEvent;
    case 225: return bridge_NtSetEvent;
    case 233: return bridge_NtWaitForSingleObject;
    case 234: return bridge_NtWaitForSingleObjectEx;
    case 235: return bridge_NtWaitForMultipleObjectsEx;
    case 238: return bridge_NtYieldExecution;

    /* Hardware */
    case   9: return bridge_HalReadSMCTrayState;
    case  49: return bridge_HalReturnToFirmware;

    /* Display */
    case   3: return bridge_AvSetDisplayMode;

    /* I/O */
    case  66: return bridge_IoCreateFile;
    case  65: return bridge_IoCreateDevice;
    case  67: return bridge_IoCreateSymbolicLink;
    case 188: return bridge_NtCreateDirectoryObject;
    case 246: return bridge_ObReferenceObjectByHandle;

    /* Memory - I/O mapping */
    case 177: return bridge_MmMapIoSpace;
    case 178: return bridge_MmPersistContiguousMemory;

    /* RTL */
    case 301: return bridge_RtlNtStatusToDosError;
    case 302: return bridge_RtlRaiseException;

    /* Section B: per-ordinal bridges (261-321), defined in the splice above. */
    case 261: return bridge_RtlAppendStringToString;
    case 262: return bridge_RtlAppendUnicodeStringToString;
    case 263: return bridge_RtlAppendUnicodeToString;
    case 264: return bridge_RtlAssert;
    case 265: return bridge_RtlCaptureContext;
    case 266: return bridge_RtlCaptureStackBackTrace;
    case 267: return bridge_RtlCharToInteger;
    case 270: return bridge_RtlCompareString;
    case 271: return bridge_RtlCompareUnicodeString;
    case 272: return bridge_RtlCopyString;
    case 273: return bridge_RtlCopyUnicodeString;
    case 274: return bridge_RtlCreateUnicodeString;
    case 275: return bridge_RtlDowncaseUnicodeChar;
    case 276: return bridge_RtlDowncaseUnicodeString;
    case 278: return bridge_RtlEnterCriticalSectionAndRegion;
    case 280: return bridge_RtlEqualUnicodeString;
    case 284: return bridge_RtlFillMemory;
    case 287: return bridge_RtlFreeUnicodeString;
    case 288: return bridge_RtlGetCallersAddress;
    case 292: return bridge_RtlIntegerToChar;
    case 293: return bridge_RtlIntegerToUnicodeString;
    case 295: return bridge_RtlLeaveCriticalSectionAndRegion;
    case 296: return bridge_RtlLowerChar;
    case 297: return bridge_RtlMapGenericMask;
    case 298: return bridge_RtlMoveMemory;
    case 299: return bridge_RtlMultiByteToUnicodeN;
    case 300: return bridge_RtlMultiByteToUnicodeSize;
    case 306: return bridge_RtlTryEnterCriticalSection;
    case 307: return bridge_RtlUlongByteSwap;
    case 309: return bridge_RtlUnicodeStringToInteger;
    case 310: return bridge_RtlUnicodeToMultiByteN;
    case 311: return bridge_RtlUnicodeToMultiByteSize;
    case 313: return bridge_RtlUpcaseUnicodeChar;
    case 314: return bridge_RtlUpcaseUnicodeString;
    case 315: return bridge_RtlUpcaseUnicodeToMultiByteN;
    case 316: return bridge_RtlUpperChar;
    case 317: return bridge_RtlUpperString;
    case 318: return bridge_RtlUshortByteSwap;
    case 319: return bridge_RtlWalkFrameChain;
    case 320: return bridge_RtlZeroMemory;
    case 321: return bridge_XboxEEPROMKey;


    /* Routed. The XcRC4 pair was turned OFF mid-bisect and never turned back
     * on. Both pass the memory-model bar: a guest RC4_CONTEXT and a guest data
     * buffer, every access translated through XBOX_TO_NATIVE, no allocation
     * and no host pointer returned. Unrouted, savegame/EEPROM code that used
     * RC4 silently did nothing. */
    case 338: return bridge_hooked_XcRC4Key;
    case 339: return bridge_hooked_XcRC4Crypt;

    /* Routed. The lesson of the block that used to sit here was that an
     * xbox_* existence does not make a wrapper mechanical: each candidate has
     * to be checked for which side owns the allocation and whether an
     * out-pointer must carry a guest VA. IoCreateDevice was fixed by
     * allocating the device object in guest memory; ExFreePool below answers
     * through the guest heap; MmUnmapIoSpace/IoDeleteDevice do the same.
     *
     * The three beside them were parked, never broken: AvGetSavedDataAddress
     * returns a ULONG value, KeCancelTimer takes a guest KTIMER through
     * XBOX_TO_NATIVE, KeDisconnectInterrupt takes a guest KINTERRUPT the same
     * way. None of them allocates, frees, or hands back a host pointer. */
    case   1: return bridge_AvGetSavedDataAddress;
    case  17: return bridge_ExFreePool;
    case  97: return bridge_KeCancelTimer;
    case 100: return bridge_KeDisconnectInterrupt;
    /* Routed: thread base priority, queried and set. Neither allocates,
     * frees, nor returns a pointer -- each takes a thread handle and a
     * LONG. Half-Life 2 calls both while starting its worker threads,
     * and unbridged they returned 0, so every thread read its own base
     * priority as 0 and any priority the title set was discarded. */
    case 124: return bridge_KeQueryBasePriorityThread;
    case 143: return bridge_KeSetBasePriorityThread;
    /* Routed. Both clear the memory-model bar above: neither allocates,
     * frees, nor hands back a host pointer. KeStallExecutionProcessor takes a
     * microsecond count and busy-waits -- no pointers at all.
     * MmLockUnlockBufferPages takes (BaseAddress, Length, UnlockPages) and
     * pins physical pages, which is a no-op on the host; XBOX_TO_NATIVE is the
     * correct marshalling for its one address argument, and it writes nothing
     * through it.
     *
     * Half-Life 2 calls both during C++ static initialisation. Unbridged they
     * returned 0 without stalling or locking anything -- harmless in isolation,
     * but they are exactly the kind of silent no-op that makes a later failure
     * unattributable. */
    case 151: return bridge_KeStallExecutionProcessor;
    case 175: return bridge_MmLockUnlockBufferPages;
    /* Routed, both checked against the memory-model bar above.
     *
     * MmQueryAllocationSize now answers from the guest heap's block table
     * instead of the host's VirtualQuery, so nothing crosses the two worlds.
     *
     * NtCreateMutant creates a host mutex and hands it back through
     * bridge_write_handle, which is a guest token -- the same shape as
     * NtCreateEvent, which has been routed all along. It allocates no guest
     * memory and returns no host pointer.
     *
     * The Xbox Dashboard needs the mutant: its audio thread creates one during
     * the first tick, and unbridged the call returned STATUS_SUCCESS without
     * writing a handle, so the main thread waited on five events that nothing
     * would ever signal. */
    case 180: return bridge_MmQueryAllocationSize;
    case 192: return bridge_NtCreateMutant;
    case 221: return bridge_NtReleaseMutant;
    /* Routed. Checked against the memory-model warning above rather than
     * assumed mechanical: NtResumeThread takes a handle token and writes a
     * 4-byte suspend count through an optional out-parameter. Guest ULONG and
     * host ULONG are both 4 bytes, XBOX_TO_NATIVE already maps NULL to NULL,
     * and xbox_NtResumeThread checks the pointer before writing. Nothing
     * allocates, frees, or hands back a host pointer -- which is what
     * disqualified IoCreateDevice and ExFreePool.
     *
     * Halo 2276 calls this immediately before its first camera frustum build;
     * unbridged it returned 0 (STATUS_SUCCESS) without resuming anything, so a
     * thread the title had created suspended never started. */
    case 224: return bridge_NtResumeThread;
    case 231: return bridge_NtSuspendThread;
    /* Routed. ObfDereferenceObject is the fastcall partner of 251 (see its
     * wrapper) and only touches the object's reference count; PhyGetLinkState
     * reports link-up and PhyInitialize is a successful no-op. None of the
     * three allocates, frees, or hands back a host pointer; PhyGetLinkState is
     * what titles poll for Ethernet presence before selecting their network
     * path. */
    case 250: return bridge_ObfDereferenceObject;
    case 252: return bridge_PhyGetLinkState;
    case 253: return bridge_PhyInitialize;
    /* Routed. The memory-model note above already names this group as the
     * safe kind: each one reads or writes bytes at an address the caller
     * supplied, and none allocates, frees, or hands back a host pointer.
     *
     * Unbridged they returned 0 without hashing anything, which is invisible
     * until something checks a digest. The Xbox Dashboard verifies each XIP
     * archive it loads against a 20-byte digest in its own table
     * (sub_00034924) and calls HalReturnToFirmware(4) when the compare fails
     * -- so a no-op SHA does not corrupt anything, it reboots the console. */
    case 335: return bridge_hooked_XcSHAInit;
    case 336: return bridge_hooked_XcSHAUpdate;
    case 337: return bridge_hooked_XcSHAFinal;
    case 340: return bridge_hooked_XcHMAC;
    case 346: return bridge_hooked_XcDESKeyParity;

    /* Routed. Each of these was checked against the memory-model bar
     * described above before being added: reads or writes only happen at
     * caller-supplied guest addresses, and nothing allocates, frees, or hands
     * back a host pointer. Two of them (MmUnmapIoSpace, IoDeleteDevice) free
     * guest memory, so they deliberately answer through the guest heap rather
     * than calling the xbox_* host-heap version -- see the wrappers. */
    case  29: return bridge_ExSaveNonVolatileSetting;
    case  38: return bridge_HalClearSoftwareInterrupt;
    case  45: return bridge_HalReadSMBusValue;
    case  46: return bridge_HalReadWritePCISpace;
    case  48: return bridge_HalRequestSoftwareInterrupt;
    case  50: return bridge_HalWriteSMBusValue;
    case  68: return bridge_IoDeleteDevice;
    case  93: return bridge_KeAlertThread;
    case 125: return bridge_KeQueryInterruptTime;
    case 139: return bridge_KeRestoreFloatingPointState;
    case 142: return bridge_KeSaveFloatingPointState;
    case 158: return bridge_KeWaitForMultipleObjects;
    case 183: return bridge_MmUnmapIoSpace;
    case 193: return bridge_NtCreateSemaphore;
    case 222: return bridge_NtReleaseSemaphore;
    case 228: return bridge_NtSetSystemTime;
    case 247: return bridge_ObReferenceObjectByName;
    case 251: return bridge_ObfReferenceObject;
    case 290: return bridge_RtlInitUnicodeString;
    case 304: return bridge_RtlTimeFieldsToTime;

    /* Routed. The next batch is every remaining xbox_* implementation that
     * clears the memory-model bar, split by category:
     *
     *  - HAL/AV/port-I/O and the I/O Manager stubs all take or produce value
     *    arguments and caller-supplied guest structures; none of them returns
     *    a pointer the title dereferences. IofCallDriver/IofCompleteRequest
     *    are __fastcall (ns in ecx/edx).
     *  - The Xc* entries are documented stubs (no Xbox Live, no on-console key
     *    derivation); pointers are translated but never dereferenced.
     *  - MmAllocateSystemMemory/MmCreateKernelStack answer through the guest
     *    heap rather than VirtualAlloc so the title sees a 32-bit guest VA;
     *    their frees pair with them.
     *  - NtDuplicateObject dups a real native handle and hands it back as a
     *    fresh guest token; RtlRip logs guest strings; the ANSI<->Unicode
     *    conversion bridges do the whole conversion in guest memory (their
     *    xbox_* versions write host pointers into 4-byte guest slots).
     */
    case   4: return bridge_AvSetSavedDataAddress;
    case   5: return bridge_DbgBreakPoint;
    case  39: return bridge_HalDisableSystemInterrupt;
    case 360: return bridge_HalInitiateShutdown;
    case 358: return bridge_HalIsResetOrShutdownPending;
    case 333: return bridge_WRITE_PORT_BUFFER_USHORT;
    case 334: return bridge_WRITE_PORT_BUFFER_ULONG;

    case  61: return bridge_IoBuildDeviceIoControlRequest;
    case  62: return bridge_IoBuildSynchronousFsdRequest;
    case  69: return bridge_IoDeleteSymbolicLink;
    case  73: return bridge_IoInitializeIrp;
    case  74: return bridge_IoInvalidDeviceRequest;
    case  79: return bridge_IoSetIoCompletion;
    case  81: return bridge_IoStartNextPacket;
    case  82: return bridge_IoStartNextPacketByKey;
    case  83: return bridge_IoStartPacket;
    case  84: return bridge_IoSynchronousDeviceIoControlRequest;
    case  85: return bridge_IoSynchronousFsdRequest;
    case  86: return bridge_IofCallDriver;
    case  87: return bridge_IofCompleteRequest;
    case 359: return bridge_IoMarkIrpMustComplete;

    case 176: return bridge_MmLockUnlockPhysicalPage;
    /* Allocator and free together: both answer from the guest heap, and the
     * allocator page-aligns and zeroes because that is what the console's
     * page allocator returns and callers assume it. */
    case 167: return bridge_MmAllocateSystemMemory;
    case 172: return bridge_MmFreeSystemMemory;
    case 169: return bridge_MmCreateKernelStack;
    case 170: return bridge_MmDeleteKernelStack;

    case 341: return bridge_hooked_XcPKEncPublic;
    case 342: return bridge_hooked_XcPKDecPrivate;
    case 343: return bridge_hooked_XcPKGetKeyLen;
    case 344: return bridge_hooked_XcVerifyPKCS1Signature;
    case 345: return bridge_hooked_XcModExp;
    case 347: return bridge_hooked_XcKeyTable;
    case 348: return bridge_hooked_XcBlockCrypt;
    case 349: return bridge_hooked_XcBlockCryptCBC;
    case 350: return bridge_hooked_XcCryptService;
    case 351: return bridge_XcUpdateCrypto;

    case 352: return bridge_RtlRip;
    case 260: return bridge_RtlAnsiStringToUnicodeString;
    case 308: return bridge_RtlUnicodeStringToAnsiString;
    case 197: return bridge_NtDuplicateObject;

    /* Data exports */
    case 102: return bridge_MmGlobalData;
    case 120: return bridge_KeInterruptTime;
    case 154: return bridge_KeSystemTime;
    case 240: return bridge_ObDirectoryObjectType;
    case 245: return bridge_ObpObjectHandleTable;
    case 249: return bridge_ObSymbolicLinkObjectType;

    /* Dbg */
    case   6: return bridge_DbgBreakPointWithStatus;
    case   7: return bridge_DbgLoadImageSymbols;
    case   8: return bridge_DbgPrint;
    case  10: return bridge_DbgPrompt;
    case  11: return bridge_DbgUnLoadImageSymbols;

    /* Ex */
    case  12: return bridge_ExAcquireReadWriteLockExclusive;
    case  13: return bridge_ExAcquireReadWriteLockShared;
    case  18: return bridge_ExInitializeReadWriteLock;
    case  19: return bridge_ExInterlockedAddLargeInteger;
    case  20: return bridge_ExInterlockedAddLargeStatistic;
    case  21: return bridge_ExInterlockedCompareExchange64;
    case  25: return bridge_ExReadWriteRefurbInfo;
    case  26: return bridge_ExRaiseException;
    case  27: return bridge_ExRaiseStatus;
    case  28: return bridge_ExReleaseReadWriteLock;

    /* ExfInterlocked */
    case  32: return bridge_ExfInterlockedInsertHeadList;
    case  33: return bridge_ExfInterlockedInsertTailList;
    case  34: return bridge_ExfInterlockedRemoveHeadList;

    /* Fsc */
    case  36: return bridge_FscInvalidateIdleBlocks;

    /* Hal */
    case  43: return bridge_HalEnableSystemInterrupt;
    case 365: return bridge_HalEnableSecureTrayEject;
    case 366: return bridge_HalWriteSMCScratchRegister;

    /* Interlocked */
    case  51: return bridge_InterlockedCompareExchange;
    case  52: return bridge_InterlockedDecrement;
    case  53: return bridge_InterlockedIncrement;
    case  54: return bridge_InterlockedExchange;
    case  55: return bridge_InterlockedExchangeAdd;
    case  56: return bridge_InterlockedFlushSList;
    case  57: return bridge_InterlockedPopEntrySList;
    case  58: return bridge_InterlockedPushEntrySList;

    /* Io */
    case  59: return bridge_IoAllocateIrp;
    case  60: return bridge_IoBuildAsynchronousFsdRequest;
    case  63: return bridge_IoCheckShareAccess;
    case  72: return bridge_IoFreeIrp;
    case  75: return bridge_IoQueryFileInformation;
    case  76: return bridge_IoQueryVolumeInformation;
    case  77: return bridge_IoQueueThreadIrp;
    case  78: return bridge_IoRemoveShareAccess;
    case  80: return bridge_IoSetShareAccess;
    case  90: return bridge_IoDismountVolume;
    case  91: return bridge_IoDismountVolumeByName;

    /* Kd */
    case  88: return bridge_KdDebuggerEnabled;
    case  89: return bridge_KdDebuggerNotPresent;

    /* Ke (new) */
    case  92: return bridge_KeAlertResumeThread;
    case  94: return bridge_KeBoostPriorityThread;
    case 101: return bridge_KeEnterCriticalRegion;
    case 103: return bridge_KeGetCurrentIrql;
    case 104: return bridge_KeGetCurrentThread;
    case 105: return bridge_KeInitializeApc;
    case 106: return bridge_KeInitializeDeviceQueue;
    case 108: return bridge_KeInitializeEvent;
    case 110: return bridge_KeInitializeMutant;
    case 111: return bridge_KeInitializeQueue;
    case 112: return bridge_KeInitializeSemaphore;
    case 114: return bridge_KeInsertByKeyDeviceQueue;
    case 115: return bridge_KeInsertDeviceQueue;
    case 116: return bridge_KeInsertHeadQueue;
    case 117: return bridge_KeInsertQueue;
    case 118: return bridge_KeInsertQueueApc;
    case 121: return bridge_KeIsExecutingDpc;
    case 122: return bridge_KeLeaveCriticalRegion;
    case 123: return bridge_KePulseEvent;
    case 130: return bridge_KeRaiseIrqlToSynchLevel;
    case 131: return bridge_KeReleaseMutant;
    case 132: return bridge_KeReleaseSemaphore;
    case 133: return bridge_KeRemoveByKeyDeviceQueue;
    case 134: return bridge_KeRemoveDeviceQueue;
    case 135: return bridge_KeRemoveEntryDeviceQueue;
    case 136: return bridge_KeRemoveQueue;
    case 138: return bridge_KeResetEvent;
    case 140: return bridge_KeResumeThread;
    case 141: return bridge_KeRundownQueue;
    case 144: return bridge_KeSetDisableBoostThread;
    case 146: return bridge_KeSetEventBoostPriority;
    case 147: return bridge_KeSetPriorityProcess;
    case 148: return bridge_KeSetPriorityThread;
    case 152: return bridge_KeSuspendThread;
    case 155: return bridge_KeTestAlertThread;
    case 162: return bridge_KiBugCheckData;
    case 163: return bridge_KiUnlockDispatcherDatabase;

    /* Mm */
    case 174: return bridge_MmIsAddressValid;
    case 374: return bridge_MmDbgAllocateMemory;
    case 375: return bridge_MmDbgFreeMemory;
    case 376: return bridge_MmDbgQueryAvailablePages;
    case 377: return bridge_MmDbgReleaseAddress;
    case 378: return bridge_MmDbgWriteCheck;

    /* Nt */
    case 185: return bridge_NtCancelTimer;
    case 191: return bridge_NtCreateIoCompletion;
    case 194: return bridge_NtCreateTimer;
    case 201: return bridge_NtOpenDirectoryObject;
    case 204: return bridge_NtProtectVirtualMemory;
    case 206: return bridge_NtQueueApcThread;
    case 208: return bridge_NtQueryDirectoryObject;
    case 209: return bridge_NtQueryEvent;
    case 212: return bridge_NtQueryIoCompletion;
    case 213: return bridge_NtQueryMutant;
    case 214: return bridge_NtQuerySemaphore;
    case 216: return bridge_NtQueryTimer;
    case 220: return bridge_NtReadFileScatter;
    case 223: return bridge_NtRemoveIoCompletion;
    case 227: return bridge_NtSetIoCompletion;
    case 229: return bridge_NtSetTimerEx;
    case 230: return bridge_NtSignalAndWaitForSingleObjectEx;
    case 237: return bridge_NtWriteFileGather;

    /* Ob */
    case 239: return bridge_ObCreateObject;
    case 241: return bridge_ObInsertObject;
    case 242: return bridge_ObMakeTemporaryObject;
    case 243: return bridge_ObOpenObjectByName;
    case 244: return bridge_ObOpenObjectByPointer;
    case 248: return bridge_ObReferenceObjectByPointer;

    /* Ps */
    case 254: return bridge_PsCreateSystemThread;
    case 256: return bridge_PsQueryStatistics;
    case 257: return bridge_PsSetCreateThreadNotifyRoutine;

    /* Rtl */
    case 281: return bridge_RtlExtendedIntegerMultiply;
    case 282: return bridge_RtlExtendedLargeIntegerDivide;
    case 283: return bridge_RtlExtendedMagicDivide;
    case 285: return bridge_RtlFillMemoryUlong;
    case 286: return bridge_RtlFreeAnsiString;
    case 303: return bridge_RtlRaiseStatus;
    case 361: return bridge_RtlSnprintf;
    case 362: return bridge_RtlSprintf;
    case 363: return bridge_RtlVsnprintf;
    case 364: return bridge_RtlVsprintf;

    /* Port I/O */
    case 329: return bridge_READ_PORT_BUFFER_UCHAR;
    case 330: return bridge_READ_PORT_BUFFER_USHORT;
    case 331: return bridge_READ_PORT_BUFFER_ULONG;
    case 332: return bridge_WRITE_PORT_BUFFER_UCHAR;

    default:  return NULL;
    }
}

/* ── Per-slot bridge functions (resolved at init) ────────── */

static bridge_func_t g_slot_bridges[XBOX_KERNEL_THUNK_TABLE_SIZE];
static int g_slot_arg_bytes[XBOX_KERNEL_THUNK_TABLE_SIZE];
static uint8_t g_slot_arg_unknown[XBOX_KERNEL_THUNK_TABLE_SIZE];

/* Xbox VA to sample around each bridge call; 0 = off. See dispatch. */
uint32_t g_kernel_watch_va = 0;

/* Arm the watch from the environment.
 *
 * The facility existed but nothing set it, so it was unreachable.
 * RECOMP_KERNEL_WATCH=<guest addr> samples that dword either side of every
 * bridge call and names the ordinal that changed it -- which is the one fact
 * a hardware watchpoint cannot give, because a bridge corrupting Xbox memory
 * faults inside ntdll with no recompiled frame to blame.
 *
 * A change seen between the previous call's "after" and this call's "before"
 * is guest code, not a bridge, which is just as useful to know. */
static void kernel_watch_arm_once(void)
{
    static int done;
    const char *env;
    if (done)
        return;
    done = 1;
    env = getenv("RECOMP_KERNEL_WATCH");
    if (env)
        g_kernel_watch_va = (uint32_t)strtoul(env, NULL, 0);
}

/* Current dispatching slot */
static RECOMP_TLS int g_kernel_dispatch_slot = -1;

static void kernel_thunk_dispatch_body(void);

/* The guest CPU lock (RECOMP_GUEST_LOCK=1).
 *
 * The Xbox has one CPU: two guest threads never run guest code at the same
 * instant, and titles lean on that without knowing it -- a worker fills a
 * buffer and sets a flag with plain stores, and the reader trusts the order.
 * Here every guest thread runs on its own host core, and the lifted C gives
 * no ordering either. T()NY crashes somewhere different on every run
 * once its loader workers start.
 *
 * With the lock on, a guest thread holds it while it runs guest code and
 * lets go for the length of every kernel call -- the waits, sleeps and I/O
 * where the console's scheduler would switch threads anyway. Host threads
 * that only run a guest callback (vblank, interrupts) never take it.
 *
 * ponytail: a guest thread that spins in guest code on a flag another guest
 * thread sets, with no kernel call in the loop, deadlocks here. The console
 * would preempt it on its quantum; the upgrade is a periodic yield point in
 * the lifted code's backward branches. */
static CRITICAL_SECTION g_guest_cpu;
static int g_guest_cpu_on = -1;
static RECOMP_TLS int t_guest_thread;     /* this host thread runs a guest thread */
static RECOMP_TLS int t_guest_held;       /* and holds the guest CPU */
static RECOMP_TLS int t_dispatch_depth;

static int guest_cpu_enabled(void)
{
    if (g_guest_cpu_on < 0) {
        const char *e = getenv("RECOMP_GUEST_LOCK");
        InitializeCriticalSection(&g_guest_cpu);
        g_guest_cpu_on = e && *e == '1';
        if (g_guest_cpu_on)
            fprintf(stderr, "  [KERNEL] guest CPU lock on: one guest thread runs at a time\n");
    }
    return g_guest_cpu_on;
}

/* One host CPU for every thread that runs guest code (RECOMP_GUEST_CORES).
 *
 * Ported from DefJamFFNY-recomp (decision D28, toolkit patch 0018): the
 * console has one core, and titles rely on it without saying so. Def Jam's
 * movie streamer published a count, then a pointer, and its reader landed
 * between the two stores on a 24-core host in 7 of 7 runs; pinned, about 1 in
 * 15. Unlike RECOMP_GUEST_LOCK this keeps the host scheduler's preemption, so
 * a guest thread spinning on a flag cannot deadlock: only simultaneity goes.
 * Host-side threads (GPU, mixer, shader compiler) are never pinned.
 *
 *   unset   logical CPU 2 (CPU 0 when the host has fewer than 3)
 *   all     no pinning
 *   <hex>   this affinity mask */
void xbox_PinToGuestCore(void)
{
    static KAFFINITY mask = (KAFFINITY)-1;
    if (mask == (KAFFINITY)-1) {
        const char *e = getenv("RECOMP_GUEST_CORES");
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        if (e && !strcmp(e, "all"))
            mask = 0;
        else if (e && *e)
            mask = (KAFFINITY)strtoull(e, NULL, 16);
        else
            mask = (KAFFINITY)1 << (si.dwNumberOfProcessors > 2 ? 2 : 0);
        if (mask)
            fprintf(stderr, "  [KERNEL] guest threads share logical CPUs 0x%llX (RECOMP_GUEST_CORES=all: off)\n",
                    (unsigned long long)mask);
    }
    if (mask && !SetThreadAffinityMask(GetCurrentThread(), mask))
        fprintf(stderr, "  [KERNEL] guest CPU pin failed (error %lu)\n", GetLastError());
}

/* Time on the shared guest core (RECOMP_GUEST_BOOST=keep: none of this). Windows gives equal threads long turns
 * and runs a higher one first, where Wine's Linux scheduler slices finely: a guest worker busy on the core (HP:CoS's
 * file cache copier, whose reads finish at once where the console's DVD made it wait) held the main thread for 20 to
 * 400 ms. So, as under Wine, every guest thread short of time-critical runs at normal (bridge_win32_priority: the
 * title's +1 to +14), a worker that has run 2 ms on end steps aside at its next kernel call for whatever is ready on
 * the core (xbox_GuestSliceYield: the main thread, at most ~2 ms behind it), and a main thread polling a worker
 * through a lock yields to it (xbox_GuestPollYield). Not done (2026-10-25): the main thread above normal with the
 * timer and USB ISR threads highest (the music stopped after a pickup in the Basilisk fight and stayed off), the
 * workers below normal (the inventory's load took 1.3 to 2.2 s), the threads unpinned (HP:CoS's request queue lost a
 * wake-up and the last cutscene froze) */
int xbox_GuestCoreShared(void)
{
    static int shared = -1;
    if (shared < 0) {
        const char *e = getenv("RECOMP_GUEST_CORES"), *k = getenv("RECOMP_GUEST_BOOST");
        shared = !(e && !strcmp(e, "all")) && !(k && !strcmp(k, "keep"));
    }
    return shared;
}

extern RECOMP_TLS int g_xbox_guest_main;

/* a guest worker (not the main thread, not time-critical) that has run 2 ms since it last stepped aside */
static void xbox_GuestSliceYield(void)
{
    static LARGE_INTEGER freq;
    static RECOMP_TLS LARGE_INTEGER t_since;
    static RECOMP_TLS int t_critical = -1;
    LARGE_INTEGER now;
    if (g_xbox_guest_main || !xbox_GuestCoreShared()) return;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    if (!t_since.QuadPart) t_since = now;
    if (now.QuadPart - t_since.QuadPart < freq.QuadPart / 500) return;
    if (t_critical < 0 || (now.QuadPart & 0xF) == 0)  /* the title may set it later: looked at again now and then */
        t_critical = GetThreadPriority(GetCurrentThread()) >= THREAD_PRIORITY_HIGHEST;
    if (!t_critical) SwitchToThread();
    QueryPerformanceCounter(&t_since);
}

/* The main thread polling a worker through a lock (thousands of lock calls a frame, against ~70; the HP:CoS
 * inventory's load): every 64th call past 1000 within 16 ms gives the core to whatever else is ready on it, the
 * worker it waits for, which its higher priority would otherwise keep off */
RECOMP_TLS int g_xbox_guest_main;
void xbox_GuestPollYield(void)
{
    static ULONGLONG window;
    static uint32_t calls;
    if (!g_xbox_guest_main || !xbox_GuestCoreShared()) return;
    ULONGLONG now = GetTickCount64();
    if (now - window > 16) window = now, calls = 0;
    if (++calls > 1000 && !(calls & 63)) SwitchToThread();
}

/* Called once on each host thread that runs a guest thread, before its first
 * guest instruction. */
void guest_cpu_join(void)
{
    xbox_PinToGuestCore();
    t_guest_thread = 1;
    if (guest_cpu_enabled() && !t_guest_held) {
        EnterCriticalSection(&g_guest_cpu);
        t_guest_held = 1;
    }
}

void guest_cpu_part(void)
{
    if (t_guest_held) {
        t_guest_held = 0;
        LeaveCriticalSection(&g_guest_cpu);
    }
    t_guest_thread = 0;
}

/* A guest thread lets go of the guest CPU for the length of every kernel
 * call. */
static void kernel_thunk_dispatch(void)
{
    int held = t_guest_held;
    if (held) {                        /* let other guest threads run meanwhile */
        t_guest_held = 0;
        LeaveCriticalSection(&g_guest_cpu);
    }
    t_dispatch_depth++;
    kernel_thunk_dispatch_body();
    t_dispatch_depth--;
    /* Back to guest code: take the CPU again. Only at the outermost call --
     * a guest callback a bridge runs (an APC) returns into the bridge, not to
     * the thread's own code -- and only if the thread still exists as one. */
    if (t_guest_thread && t_dispatch_depth == 0 && guest_cpu_enabled()) {
        EnterCriticalSection(&g_guest_cpu);
        t_guest_held = 1;
    }
    (void)held;
}

/* [SLOWCALL]: a kernel call that took RECOMP_SLOWCALL_MS (default 20) or more.
 *
 * A freeze is the title's main thread not coming back from something, and a
 * stack sample only says where it was, not how long each call there took. Timing
 * the one entry every kernel call goes through answers that for all of them at
 * once: file opens and reads (from any thread, named by their file), and on the
 * main thread every wait, sleep and lock too -- other threads park in waits as a
 * matter of course, so theirs would only be noise. Bounded per run. 0 is off. */
#include "kernel_ordinal_names.h"

static DWORD    s_slowcall_main;    /* the first thread to call the kernel: the title's main thread */
static double   s_slowcall_ms = -1;
static LONG     s_slowcall_lines;

/* Per frame, for a title's slow-frame breakdown ([HITCH]): the main thread's time in file calls and in every other
 * kernel call (waits, sleeps, locks), the file calls made, and the slowest of them with its file. Read and cleared by
 * xbox_kernel_frame_stats() once a frame (the title's Swap); only kept while RECOMP_SLOWCALL_MS times the calls. */
static double s_frame_file_ms, s_frame_other_ms, s_frame_worst_ms;
static uint32_t s_frame_file_calls;
static char s_frame_worst[96];
static double s_frame_by_ordinal[XBOX_KERNEL_THUNK_TABLE_SIZE > 400 ? XBOX_KERNEL_THUNK_TABLE_SIZE : 400];

static void frame_stats_note(ULONG ordinal, double ms, const uint32_t args[4])
{
    /* per ordinal, once: 1 a file call, 2 not (the name test on every main-thread kernel call cost the locks) */
    static uint8_t kind[512];
    const char *name = NULL;
    int file_call;
    if (ordinal < 512 && kind[ordinal]) {
        file_call = kind[ordinal] == 1;
    } else {
        name = kernel_ordinal_name(ordinal);
        file_call = name && ((name[0] == 'N' && name[1] == 't' && strstr(name, "File")) || !strcmp(name, "NtClose"));
        if (ordinal < 512) kind[ordinal] = file_call ? 1 : 2;
    }
    if (!file_call) {
        s_frame_other_ms += ms;
        if (ordinal < sizeof s_frame_by_ordinal / sizeof s_frame_by_ordinal[0])
            s_frame_by_ordinal[ordinal] += ms;
        return;
    }
    s_frame_file_ms += ms;
    s_frame_file_calls++;
    if (ms > s_frame_worst_ms) {
        if (!name) name = kernel_ordinal_name(ordinal);
        int opens = !strcmp(name, "NtCreateFile") || !strcmp(name, "NtOpenFile");
        uint32_t h = opens ? (args[0] ? BRIDGE_MEM32(args[0]) : 0) : args[0];
        char got[72];
        s_frame_worst_ms = ms;
        if (opens || !bridge_file_name(h, got, sizeof got))
            snprintf(s_frame_worst, sizeof s_frame_worst, "%s", name);
        else
            snprintf(s_frame_worst, sizeof s_frame_worst, "%s %s", name, got);
    }
}

void xbox_kernel_frame_stats(double *file_ms, double *other_ms, uint32_t *file_calls, double *worst_ms,
                             char *worst, size_t worst_len, double *top_other_ms, char *top_other, size_t top_len)
{
    size_t top = 0;
    for (size_t i = 1; i < sizeof s_frame_by_ordinal / sizeof s_frame_by_ordinal[0]; i++)
        if (s_frame_by_ordinal[i] > s_frame_by_ordinal[top]) top = i;
    *top_other_ms = s_frame_by_ordinal[top];
    {
        const char *name = kernel_ordinal_name((unsigned)top);
        if (*top_other_ms <= 0) snprintf(top_other, top_len, "-");
        else if (name) snprintf(top_other, top_len, "%s", name);
        else snprintf(top_other, top_len, "ordinal%u", (unsigned)top);
    }
    memset(s_frame_by_ordinal, 0, sizeof s_frame_by_ordinal);
    *file_ms = s_frame_file_ms, *other_ms = s_frame_other_ms, *file_calls = s_frame_file_calls;
    *worst_ms = s_frame_worst_ms;
    snprintf(worst, worst_len, "%s", s_frame_worst);
    s_frame_file_ms = s_frame_other_ms = s_frame_worst_ms = 0;
    s_frame_file_calls = 0;
    s_frame_worst[0] = 0;
}

static void slowcall_report(ULONG ordinal, double ms, uint32_t caller, const uint32_t args[4])
{
    const char *name = kernel_ordinal_name(ordinal);
    int main_thread = GetCurrentThreadId() == s_slowcall_main;
    int file_call = name && ((name[0] == 'N' && name[1] == 't' && strstr(name, "File")) || !strcmp(name, "NtClose"));
    char file[80] = "";
    char num[16];
    if (!main_thread && !file_call)
        return;
    if (InterlockedIncrement(&s_slowcall_lines) > 3000)
        return;
    if (file_call) {
        /* NtCreateFile/NtOpenFile take the handle's address first; the others the handle */
        int opens = !strcmp(name, "NtCreateFile") || !strcmp(name, "NtOpenFile");
        uint32_t h = opens ? (args[0] ? BRIDGE_MEM32(args[0]) : 0) : args[0];
        char got[72];
        if (bridge_file_name(h, got, sizeof got))
            snprintf(file, sizeof file, " file \"%s\"", got);
    }
    if (!name) {
        snprintf(num, sizeof num, "ordinal%u", (unsigned)ordinal);
        name = num;
    }
    fprintf(stderr, "[SLOWCALL] %.1f ms %s (%u) thread %lu%s from 0x%08X args %08X %08X %08X %08X%s\n",
            ms, name, (unsigned)ordinal, (unsigned long)GetCurrentThreadId(), main_thread ? " main" : "",
            caller, args[0], args[1], args[2], args[3], file);
    fflush(stderr);
}

static void xbox_GuestSliceYield(void);

static void kernel_thunk_dispatch_body(void)
{
    int slot = g_kernel_dispatch_slot;
    xbox_GuestSliceYield();
    bridge_func_t bridge;
    ULONG ordinal;

    if (slot < 0 || slot >= XBOX_KERNEL_THUNK_TABLE_SIZE) {
        fprintf(stderr, "  [KERNEL] bad slot %d\n", slot);
        g_eax = 0;
        g_esp += 4;  /* pop dummy return address */
        return;
    }

    ordinal = g_slot_ordinals[slot];
    bridge = g_slot_bridges[slot];

    g_kernel_call_count++;
    if (ordinal < XBOX_KERNEL_THUNK_TABLE_SIZE)
        g_ordinal_calls[ordinal]++;

    if (KERNEL_LOG_ON()) {
        /* The guest return address sits at the top of the guest stack: the
         * caller pushed it before dispatching here. Logging it turns "some
         * function is calling this" into "this call site is", which is the
         * difference between guessing and knowing when a title recurses. */
        fprintf(stderr,
                "  [KERNEL] #%lld: ordinal %u (slot %d) esp=0x%08X ret=0x%08X\n",
                g_kernel_call_count, ordinal, slot, g_esp,
                g_esp ? BRIDGE_MEM32(g_esp) : 0);
        fflush(stderr);
    }

    {
        static DWORD last_summary_tick = 0;
        DWORD now = GetTickCount();
        if (last_summary_tick == 0) last_summary_tick = now;
        if (now - last_summary_tick >= 2000 && g_kernel_call_count > 200) {
            fprintf(stderr, "  [KERNEL] summary: %lld total calls, latest ordinal %u (slot %d) esp=0x%08X\n",
                    g_kernel_call_count, ordinal, slot, g_esp);
            /* And which ones, ranked. "Latest" names whatever the sample
             * happened to land on; the question behind this line is what a
             * title sitting still is actually asking the kernel for, and
             * that wants counting rather than sampling. */
            {
                static unsigned char shown_ord[XBOX_KERNEL_THUNK_TABLE_SIZE];
                int r, shown;

                memset(shown_ord, 0, sizeof shown_ord);
                for (shown = 0; shown < 6; shown++) {
                    int best = -1;
                    for (r = 0; r < XBOX_KERNEL_THUNK_TABLE_SIZE; r++)
                        if (g_ordinal_calls[r] && !shown_ord[r]
                            && (best < 0 || g_ordinal_calls[r] > g_ordinal_calls[best]))
                            best = r;
                    if (best < 0)
                        break;
                    shown_ord[best] = 1;
                    fprintf(stderr, "  [KERNEL]   ordinal %3d x%llu\n", best,
                            (unsigned long long)g_ordinal_calls[best]);
                }
            }
            fflush(stderr);
            last_summary_tick = now;
        }
    }

    /* Pop the dummy return address that PUSH32(esp, 0) pushed before RECOMP_ICALL.
     * On real x86, "call [thunk]" pushes a real return address and "ret" pops it.
     * In our model, the bridge is called directly (not via the simulated stack),
     * so we must manually consume the dummy return address. */
    /* Keep the caller before popping it. A bridge that blocks -- a critical
     * section above all -- can then say which guest call site is holding the
     * lock, which is the one fact a deadlock report otherwise lacks. */
    g_xbox_kernel_caller = g_esp ? BRIDGE_MEM32(g_esp) : 0;
    g_esp += 4;

    /* Name the bridge that corrupts a watched dword.
     *
     * A bridge hands Xbox pointers to real Win32 calls, so a bad one has
     * Windows write into Xbox memory -- the resulting wild write has a stack
     * inside ntdll with no recompiled frame to blame, and a watchpoint just
     * says "something changed". Sampling either side of the call names the
     * ordinal directly, which is the one fact those tools cannot give.
     *
     * Set g_kernel_watch_va to arm; zero (the default) costs one compare. */
    uint32_t _watch_before = 0;
    kernel_watch_arm_once();
    if (g_kernel_watch_va) {
        _watch_before = BRIDGE_MEM32(g_kernel_watch_va);
        /* Reporting only on change misses the case that matters most: a value
         * that was already wrong before the first call sampled it. Printing
         * every sample under RECOMP_KERNEL_WATCH_ALL shows when it changed
         * even if no single bridge did it. */
        if (getenv("RECOMP_KERNEL_WATCH_ALL")) {
            static uint32_t seen = 0xDEADBEEFu;
            if (_watch_before != seen) {
                seen = _watch_before;
                fprintf(stderr, "  [KWATCH] 0x%08X = %08X before ordinal %u"
                                " (call #%lld)\n",
                        g_kernel_watch_va, _watch_before, ordinal,
                        g_kernel_call_count);
                fflush(stderr);
            }
        }
    }

    if (s_slowcall_ms < 0) {
        const char *e = getenv("RECOMP_SLOWCALL_MS");
        s_slowcall_ms = e && *e ? atof(e) : 20.0;
    }
    if (!s_slowcall_main)
        InterlockedCompareExchange((LONG *)&s_slowcall_main, (LONG)GetCurrentThreadId(), 0);
    LARGE_INTEGER _slow_t0;
    uint32_t _slow_args[4], _slow_caller = g_xbox_kernel_caller;
    if (s_slowcall_ms > 0) {
        int a;
        for (a = 0; a < 4; a++)
            _slow_args[a] = BRIDGE_MEM32(g_esp + 4u * (uint32_t)a);
        QueryPerformanceCounter(&_slow_t0);
    }

    if (bridge) {
        bridge();
    } else {
        /* No specific bridge - return 0. Warn once per ordinal rather than
         * gating on g_kernel_call_count: a missing bridge is rare and is
         * usually the reason a game misbehaves, so it must not be swallowed
         * by the general call-trace throttle. Bounded to one line per slot. */
        static uint8_t warned[XBOX_KERNEL_THUNK_TABLE_SIZE];
        if (!warned[slot]) {
            warned[slot] = 1;
            fprintf(stderr, "  [KERNEL] WARNING: no bridge for ordinal %u (slot %d), returning 0\n",
                    ordinal, slot);
            /* "Returning 0" is the harmless half. The damaging half is the
             * stack: the Xbox kernel is stdcall, so the callee owes the caller
             * its arguments back, and an ordinal missing from
             * stdcall_args_for_ordinal() returns 0 bytes and leaves them
             * there. The caller's own `pop`s then run low by that much and it
             * returns with its callee-saved registers rotated -- silently,
             * frames away from here. Say so, because a title that dies of this
             * looks nothing like a title that is missing a kernel function. */
            if (g_slot_arg_unknown[slot])
                fprintf(stderr, "  [KERNEL]   ordinal %u has no entry in "
                        "stdcall_args_for_ordinal(). If it takes arguments, "
                        "this call is corrupting the caller's stack -- add its "
                        "argument size there before anything else.\n", ordinal);
            fflush(stderr);
        }
        g_eax = 0;
    }

    if (s_slowcall_ms > 0) {
        static LARGE_INTEGER freq;
        LARGE_INTEGER t1;
        double ms;
        QueryPerformanceCounter(&t1);
        if (!freq.QuadPart)
            QueryPerformanceFrequency(&freq);
        ms = (double)(t1.QuadPart - _slow_t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
        if (GetCurrentThreadId() == s_slowcall_main)
            frame_stats_note(ordinal, ms, _slow_args);
        if (ms >= s_slowcall_ms)
            slowcall_report(ordinal, ms, _slow_caller, _slow_args);
    }

    /* Clean stdcall args from the simulated stack.
     * On real x86, stdcall callee does "ret N" to pop the return address
     * and N bytes of arguments. We already popped the dummy return address
     * above; now pop the args. */
    g_esp += g_slot_arg_bytes[slot];

    if (g_kernel_watch_va) {
        uint32_t _after = BRIDGE_MEM32(g_kernel_watch_va);
        if (_after != _watch_before) {
            fprintf(stderr,
                    "  [KWATCH] ordinal %u changed Xbox VA 0x%08X: "
                    "%08X -> %08X\n",
                    ordinal, g_kernel_watch_va, _watch_before, _after);
            fflush(stderr);
        }
    }

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] → returned 0x%08X\n", g_eax);
        fflush(stderr);
    }
}

/* ── Dispatch lookup ────────────────────────────────────── */

/**
 * Look up a kernel thunk by synthetic VA.
 * Called as a fallback when recomp_lookup() returns NULL.
 */
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va)
{
    if (xbox_va >= KERNEL_VA_BASE && xbox_va < KERNEL_VA_END) {
        int slot = (xbox_va - KERNEL_VA_BASE) / 4;
        if (slot >= 0 && slot < XBOX_KERNEL_THUNK_TABLE_SIZE) {
            g_kernel_dispatch_slot = slot;
            return kernel_thunk_dispatch;
        }
    }
    return NULL;
}

/* ── Initialization ─────────────────────────────────────── */

/*
 * Where this title's kernel thunk table lives. Defaults to the compile-time
 * constant, but every XBE puts it somewhere different (it comes from the
 * header's KernelImageThunkAddress), so xbox_MemoryLayoutInit() parses the
 * real address out of the binary and overrides it here.
 *
 * Halo build 2276 puts it at 0x00253090 against the default's 0x0036B7C0 --
 * without the override the bridge patches ordinals into whatever happens to
 * live at the wrong address and every kernel call goes somewhere arbitrary.
 */
static uint32_t g_thunk_table_base  = XBOX_KERNEL_THUNK_TABLE_BASE;
static uint32_t g_thunk_table_count = XBOX_KERNEL_THUNK_TABLE_SIZE;

/**
 * Return the kernel thunk table address and entry count currently in effect.
 * The address is parsed from the XBE header during memory layout init, so the
 * values are per-title, not hardcoded. *base/*count are set to 0 if unavailable.
 */
void xbox_kernel_get_thunk_address(uint32_t *xbox_va, uint32_t *count)
{
    if (xbox_va) *xbox_va = g_thunk_table_base;
    if (count)  *count  = g_thunk_table_count;
}

void xbox_kernel_set_thunk_address(uint32_t xbox_va, uint32_t count)
{
    if (!xbox_va) {
        return;
    }

    g_thunk_table_base = xbox_va;

    /* count indexes g_slot_* arrays, which are sized by the macro. A title
     * importing more slots than the real kernel exports would run off them. */
    if (count && count <= XBOX_KERNEL_THUNK_TABLE_SIZE) {
        g_thunk_table_count = count;
    } else if (count > XBOX_KERNEL_THUNK_TABLE_SIZE) {
        fprintf(stderr,
                "  Kernel thunk bridge: XBE declares %u thunk slots, clamping to %d\n",
                count, XBOX_KERNEL_THUNK_TABLE_SIZE);
        g_thunk_table_count = XBOX_KERNEL_THUNK_TABLE_SIZE;
    }
}

/**
 * Resolve the kernel thunk table in Xbox memory.
 *
 * Must be called AFTER xbox_MemoryLayoutInit() so Xbox memory is mapped.
 *
 * Reads the actual ordinals from the XBE memory thunk table (0x80000000|ordinal),
 * resolves each to a per-ordinal bridge function, and replaces the entry
 * with a synthetic VA for dispatch.
 */
/* Per-title ordinal remap; NULL = identity (the kernel's own XDK). Set by
 * xbox_kernel_set_ordinal_remap before init. See kernel.h. */
static const unsigned short *g_ordinal_remap = NULL;
static int g_ordinal_remap_count = 0;

void xbox_kernel_set_ordinal_remap(const unsigned short *map, int count)
{
    g_ordinal_remap = map;
    g_ordinal_remap_count = count;
}

int xbox_bridge_has_ordinal(ULONG ordinal)
{
    return bridge_for_ordinal(ordinal) != NULL;
}

void xbox_kernel_bridge_init(void)
{
    {
        extern void xbox_state_load(void);
        xbox_state_load();   /* what the console keeps across runs */
    }
    int i;
    int resolved = 0;
    int bridged = 0;
    int unbridged = 0;
    DWORD old_protect;

    guest_cpu_join();          /* the caller goes on to run the title's entry point */
    g_xbox_guest_main = 1;
    fprintf(stderr, "  Kernel thunk bridge: resolving %d entries at 0x%08X\n",
            g_thunk_table_count, g_thunk_table_base);

    /* The thunk table lives in .rdata which is marked PAGE_READONLY.
     * Temporarily make it writable so we can patch the ordinals. */
    VirtualProtect(
        (LPVOID)((uintptr_t)g_thunk_table_base + g_xbox_mem_offset),
        g_thunk_table_count * 4,
        PAGE_READWRITE,
        &old_protect
    );

    /* Initialize kernel data export values first */
    kernel_data_init();

    for (i = 0; i < g_thunk_table_count; i++) {
        uint32_t va = g_thunk_table_base + i * 4;
        uint32_t current = BRIDGE_MEM32(va);

        if (current & 0x80000000) {
            /* Read the actual ordinal from Xbox memory, then translate it into
             * the kernel's canonical ordinal space. Identity unless the title
             * set a remap (a different XDK). Every routing decision below --
             * data export, bridge, arg size -- keys off the canonical ordinal,
             * so one translation here covers all three. */
            ULONG ordinal = current & 0x7FFFFFFF;
            if (g_ordinal_remap && ordinal < (ULONG)g_ordinal_remap_count
                && g_ordinal_remap[ordinal]) {
                ordinal = g_ordinal_remap[ordinal];
            }
            g_slot_ordinals[i] = ordinal;

            /* Check if this is a data export */
            uint32_t data_va = kernel_data_va_for_ordinal(ordinal);
            if (data_va) {
                /* DATA export: point thunk to actual data in mapped memory.
                 * This allows the game to dereference the thunk entry. */
                BRIDGE_MEM32(va) = data_va;
                resolved++;
                bridged++;
                continue;
            }

            /* FUNCTION export: use synthetic VA for dispatch */
            g_slot_bridges[i] = bridge_for_ordinal(ordinal);
            { int _n = stdcall_args_for_ordinal(ordinal);
              g_slot_arg_unknown[i] = (uint8_t)(_n < 0);
              g_slot_arg_bytes[i] = (_n < 0) ? 0 : _n; }
            if (g_slot_bridges[i]) {
                bridged++;
            } else {
                unbridged++;
                fprintf(stderr, "  [KERNEL] unbridged function thunk: ordinal %lu"
                        " (slot %u, VA 0x%08X)\n",
                        (unsigned long)ordinal, i, KERNEL_VA_BASE + i * 4);
            }

            /* Replace Xbox memory entry with synthetic VA */
            uint32_t synthetic = KERNEL_VA_BASE + i * 4;
            BRIDGE_MEM32(va) = synthetic;
            resolved++;
        }
    }

    /*
     * Thunk entries below the header-declared base.
     *
     * KernelImageThunkAddress points at the main import run, but the linker can
     * emit further runs just before it, separated by a NULL. Halo has three at
     * base-0x10 (ordinals 52, 51 and 5). Those stay unpatched, so game code
     * doing "mov ebx,[thunk]; call ebx" jumps to the raw 0x8000xxxx marker
     * instead of a kernel function. The indirect call cannot resolve it, yields
     * 0, and a caller looping until it sees an error code never sees one - in
     * Halo that hung main() in a file-enumeration loop before it reached any
     * initialisation.
     *
     * Only entries carrying the ordinal marker with an ordinal the kernel has
     * (1 to 366) are touched, so scanning back over unrelated .rdata is
     * harmless.
     */
    {
        const int LOOKBEHIND = 16;   /* entries, i.e. 64 bytes */
        DWORD scan_protect;
        uint32_t low = g_thunk_table_base - LOOKBEHIND * 4;

        VirtualProtect((LPVOID)((uintptr_t)low + g_xbox_mem_offset),
                       LOOKBEHIND * 4, PAGE_READWRITE, &scan_protect);

        for (i = 1; i <= LOOKBEHIND; i++) {
            uint32_t va = g_thunk_table_base - i * 4;
            uint32_t current = BRIDGE_MEM32(va);
            int slot;

            /* an import is the marker with a kernel ordinal (1 to 366); other words with the top bit set are data
             * (negative numbers before Philosopher's Stone's table were taken for imports and overwritten) */
            if (!(current & 0x80000000) || (current & 0x7FFFFFFF) < 1 || (current & 0x7FFFFFFF) > 366) {
                continue;            /* NULL separator or ordinary data */
            }
            slot = g_thunk_table_count + i;   /* park these above the main run */
            if (slot >= XBOX_KERNEL_THUNK_TABLE_SIZE) {
                break;
            }

            g_slot_ordinals[slot] = current & 0x7FFFFFFF;
            g_slot_bridges[slot] = bridge_for_ordinal(g_slot_ordinals[slot]);
            { int _n = stdcall_args_for_ordinal(g_slot_ordinals[slot]);
              g_slot_arg_unknown[slot] = (uint8_t)(_n < 0);
              g_slot_arg_bytes[slot] = (_n < 0) ? 0 : _n; }
            BRIDGE_MEM32(va) = KERNEL_VA_BASE + slot * 4;
            resolved++;
            if (g_slot_bridges[slot]) bridged++; else unbridged++;

            fprintf(stderr, "  [KERNEL] extra thunk at 0x%08X: ordinal %u (slot %d)\n",
                    va, g_slot_ordinals[slot], slot);
        }

        VirtualProtect((LPVOID)((uintptr_t)low + g_xbox_mem_offset),
                       LOOKBEHIND * 4, scan_protect, &scan_protect);
    }

    /* Restore original protection */
    VirtualProtect(
        (LPVOID)((uintptr_t)g_thunk_table_base + g_xbox_mem_offset),
        g_thunk_table_count * 4,
        old_protect,
        &old_protect
    );

    fprintf(stderr, "  Kernel thunk bridge: %d/%d resolved (%d bridged, %d stub)\n",
            resolved, g_thunk_table_count, bridged, unbridged);
    fprintf(stderr, "  Synthetic VA range: 0x%08X-0x%08X\n",
            KERNEL_VA_BASE, KERNEL_VA_BASE + (resolved - 1) * 4);

}
