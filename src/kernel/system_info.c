/*
 * [SYSTEM] lines for the top of a run log: the OS (and Wine / Proton under it), the CPU, the RAM, each GPU with its
 * memory and driver version, and the desktop's mode, so a player's log says what it ran on without asking. Called
 * once by the port's main() after its [VERSION] line. Hardware and OS facts only: no user, computer or folder names.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void xbox_LogSystemInfo(void);

#if defined(_WIN32)
#define COBJMACROS
#include <windows.h>
#include <dxgi.h>
#if defined(__GNUC__)
#include <cpuid.h>
#else
#include <intrin.h>
#endif

typedef LONG(WINAPI *rtl_get_version_fn)(OSVERSIONINFOW *);
typedef const char *(CDECL *wine_str_fn)(void);
typedef void(CDECL *wine_host_fn)(const char **, const char **);

/* a REG_SZ under HKLM, or "" */
static void reg_str(const char *key, const char *name, char *out, DWORD n)
{
    DWORD size = n;
    out[0] = 0;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, key, name, RRF_RT_REG_SZ, NULL, out, &size) != ERROR_SUCCESS)
        out[0] = 0;
}

static void log_os(void)
{
    /* RtlGetVersion: the real version; GetVersionEx answers with what the exe's manifest asks for */
    OSVERSIONINFOW v = {sizeof v};
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    rtl_get_version_fn rgv = ntdll ? (rtl_get_version_fn)(void *)GetProcAddress(ntdll, "RtlGetVersion") : NULL;
    if (rgv) rgv(&v);
    char product[128], display[64], ubr_s[16] = "";
    static const char *kNt = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    reg_str(kNt, "ProductName", product, sizeof product);
    reg_str(kNt, "DisplayVersion", display, sizeof display);
    DWORD ubr = 0, size = sizeof ubr;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, kNt, "UBR", RRF_RT_REG_DWORD, NULL, &ubr, &size) == ERROR_SUCCESS && ubr)
        snprintf(ubr_s, sizeof ubr_s, ".%lu", (unsigned long)ubr);
    printf("[SYSTEM] OS: Windows %lu.%lu.%lu%s%s%s%s%s", (unsigned long)v.dwMajorVersion,
           (unsigned long)v.dwMinorVersion, (unsigned long)v.dwBuildNumber, ubr_s, product[0] ? " (" : "", product,
           display[0] ? " " : "", display);
    if (product[0]) printf(")");
    /* Wine (and Proton, which names its build) answers through ntdll's own exports */
    wine_str_fn wver = ntdll ? (wine_str_fn)(void *)GetProcAddress(ntdll, "wine_get_version") : NULL;
    if (wver) {
        wine_str_fn wbuild = (wine_str_fn)(void *)GetProcAddress(ntdll, "wine_get_build_id");
        wine_host_fn whost = (wine_host_fn)(void *)GetProcAddress(ntdll, "wine_get_host_version");
        const char *sys = NULL, *rel = NULL;
        if (whost) whost(&sys, &rel);
        printf(" | Wine %s", wver());
        if (wbuild) printf(" (%s)", wbuild());
        if (sys) printf(" on %s %s", sys, rel ? rel : "");
    }
    printf("\n");
}

static void log_cpu(void)
{
    char brand[49] = "";
    unsigned int r[4], max_ext = 0;
#if defined(__GNUC__)
    max_ext = __get_cpuid_max(0x80000000u, NULL);
    if (max_ext >= 0x80000004u)
        for (unsigned int i = 0; i < 3; i++) {
            __get_cpuid(0x80000002u + i, &r[0], &r[1], &r[2], &r[3]);
            memcpy(brand + 16 * i, r, 16);
        }
#else
    int ri[4];
    __cpuid(ri, 0x80000000);
    max_ext = (unsigned int)ri[0];
    if (max_ext >= 0x80000004u)
        for (int i = 0; i < 3; i++) {
            __cpuid(ri, 0x80000002 + i);
            memcpy(brand + 16 * i, ri, 16);
        }
#endif
    (void)r;
    brand[48] = 0;
    const char *b = brand;
    while (*b == ' ') b++;  /* some brand strings are padded at the front */
    DWORD logical = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    /* physical cores: one RelationProcessorCore record each */
    DWORD len = 0, cores = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len);
    if (len) {
        char *buf = (char *)malloc(len);
        if (buf && GetLogicalProcessorInformationEx(RelationProcessorCore,
                                                    (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)buf, &len)) {
            for (DWORD off = 0; off < len;) {
                SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *e = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)(buf + off);
                if (e->Relationship == RelationProcessorCore) cores++;
                if (!e->Size) break;
                off += e->Size;
            }
        }
        free(buf);
    }
    DWORD mhz = 0, size = sizeof mhz;
    RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "~MHz", RRF_RT_REG_DWORD,
                 NULL, &mhz, &size);
    printf("[SYSTEM] CPU: %s", *b ? b : "unknown");
    if (cores) printf(", %lu cores", (unsigned long)cores);
    printf(", %lu threads", (unsigned long)logical);
    if (mhz) printf(", %lu MHz", (unsigned long)mhz);
    printf("\n");
}

static void log_ram(void)
{
    MEMORYSTATUSEX m = {sizeof m};
    if (!GlobalMemoryStatusEx(&m)) return;
    printf("[SYSTEM] RAM: %.1f GB, %.1f GB free\n", m.ullTotalPhys / 1073741824.0, m.ullAvailPhys / 1073741824.0);
}

static void log_gpus(void)
{
    IDXGIFactory1 *f = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f)) || !f) {
        printf("[SYSTEM] GPU: (DXGI unavailable)\n");
        return;
    }
    IDXGIAdapter1 *a = NULL;
    for (UINT i = 0; IDXGIFactory1_EnumAdapters1(f, i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(a, &d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            printf("[SYSTEM] GPU %u: %ls (%04X:%04X), %.1f GB VRAM", i, d.Description, d.VendorId, d.DeviceId,
                   d.DedicatedVideoMemory / 1073741824.0);
            /* the user mode driver's version, as Device Manager shows it */
            LARGE_INTEGER umd;
            if (SUCCEEDED(IDXGIAdapter1_CheckInterfaceSupport(a, &IID_IDXGIDevice, &umd)))
                printf(", driver %u.%u.%u.%u", (unsigned)(umd.HighPart >> 16), (unsigned)(umd.HighPart & 0xFFFF),
                       (unsigned)((DWORD)umd.LowPart >> 16), (unsigned)(umd.LowPart & 0xFFFF));
            printf("\n");
        }
        IDXGIAdapter1_Release(a);
    }
    IDXGIFactory1_Release(f);
}

static void log_display(void)
{
    DEVMODEW dm = {0};
    dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm))
        printf("[SYSTEM] Display: %lux%lu @ %lu Hz\n", (unsigned long)dm.dmPelsWidth, (unsigned long)dm.dmPelsHeight,
               (unsigned long)dm.dmDisplayFrequency);
}

void xbox_LogSystemInfo(void)
{
    log_os();
    log_cpu();
    log_ram();
    log_gpus();
    log_display();
    fflush(stdout);
}

#else
#include <sys/utsname.h>
#include <unistd.h>

void xbox_LogSystemInfo(void)
{
    struct utsname u;
    if (uname(&u) == 0) printf("[SYSTEM] OS: %s %s (%s)\n", u.sysname, u.release, u.machine);
    long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGESIZE);
    printf("[SYSTEM] CPU: %ld threads\n", sysconf(_SC_NPROCESSORS_ONLN));
    if (pages > 0 && page > 0) printf("[SYSTEM] RAM: %.1f GB\n", (double)pages * page / 1073741824.0);
    fflush(stdout);
}
#endif
