// Direct3D 8 (XDK 5028) entry points the title calls, implemented on a host
// D3D12 renderer instead of the emulated NV2A pushbuffer.
//
// Ported from the MIT-licensed renderer's src/xdk/d3d8.cpp (MIT, Copyright (c) 2026
// TGP482, Joshhhuaaa; see ../LICENSE), which targets XDK 5849. The
// addresses, CDevice offsets and argument counts below are this XBE's: the
// entry points from XbSymbolDatabase, the rest read from the functions' own
// code (the comment by each constant says where).
//
// Resource creation, locking and the XG helpers stay recompiled: they only
// manage memory. Everything that would push hardware commands is replaced, and
// what still pushes (anything not caught here) writes into a scratch buffer
// that MakeSpace rewinds, so the pushbuffer executor sees nothing.
//
// Each entry point keeps the generated XDK body as sub_X_gen, so
// RECOMP_HLE_D3D=0 runs the original Direct3D on the emulated NV2A.
// Built into the recompile with --exclude-manual pointing at this file.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "app/app.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/texture.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

extern "C" {
#include "video/video_player.h"

// the recompiled code's registers (recomp_types.h), one set per guest thread
extern __thread uint32_t g_eax, g_ecx, g_edx, g_esp;
extern __thread uint32_t g_ebx, g_esi, g_edi;
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
}

using namespace kernel;

namespace d3d {

namespace {

// ── This XBE's D3D globals ──────────────────────────────────────────────────

constexpr uint32_t kDevice = 0x00140890;       // the static CDevice: Direct3D_CreateDevice stores it to g_pDevice
constexpr uint32_t kDeviceBytes = 0x80C * 4;   // CDevice::~CDevice clears this many dwords
constexpr uint32_t kDevicePtr = 0x00140888;    // D3D__pDevice
constexpr uint32_t kIndexData = 0x001403E4;    // D3D__IndexData, SetIndices
constexpr uint32_t kDirtyFlags = 0x001403E8;   // D3D__DirtyFlags
constexpr uint32_t kTextureState = 0x001403F0; // D3D__TextureState [4][32]
constexpr uint32_t kRenderState = 0x001405F0;  // D3D__RenderState [166]
constexpr uint32_t kStreams = 0x001428C0;      // g_Stream [16] stride, offset, buffer
constexpr uint32_t kSimpleEncode = 0x0017DD58; // D3DSIMPLERENDERSTATEENCODE, SetRenderStateNotInline
constexpr uint32_t kVsConstants = 0x0013F7D0;  // the 192 vertex shader constants SetVertexShaderConstant1 mirrors

// recompiled D3D helpers kept as guest code
constexpr uint32_t kDestroyResource = 0x00134000;  // D3D_DestroyResource
constexpr uint32_t kInitDefaultState = 0x001368E0; // called by CDevice::Init after SetRenderTarget

// CDevice fields mirrored so the recompiled getters and resource code see what the XDK would
constexpr uint32_t kDevPush = 0x00, kDevThreshold = 0x04;
constexpr uint32_t kDevFence = 0x2C;             // stored into a resource as it is released (SetTexture)
constexpr uint32_t kDevBaseVertex = 0x1C;        // SetIndices
constexpr uint32_t kDevPsTextureModes = 0x378;   // SetRenderState_PSTextureModes
constexpr uint32_t kDevIndexBuffer = 0x388;      // SetIndices
constexpr uint32_t kDevZScale = 0x50C;           // GetViewportOffsetAndScale
constexpr uint32_t kDevSuperX = 0x518, kDevSuperY = 0x51C;
constexpr uint32_t kDevTransforms = 0x810;       // SetTransform: 10 matrices
constexpr uint32_t kDevTextures = 0xB38;         // SetTexture
constexpr uint32_t kDevRenderTarget = 0x15B4;    // GetRenderTarget2
constexpr uint32_t kDevDepth = 0x15B8;           // GetDepthStencilSurface2
constexpr uint32_t kDevFrameBuffers = 0x15C0;    // CDevice::Init: back buffer, front buffer, ..., +0xC auto depth
constexpr uint32_t kDevSwapCount = 0x2010;       // Swap's return

gpu::State g_state;
std::unique_ptr<gpu::Backend> g_backend;
std::recursive_mutex g_lock;

std::unordered_map<uint32_t, std::shared_ptr<gpu::VertexShader>> g_vs;
std::unordered_map<uint32_t, std::shared_ptr<gpu::PixelShader>> g_ps;
std::array<uint32_t, 3> g_buffers{};  // front, back surfaces
uint32_t g_buffer_count = 2;
uint32_t g_depth = 0;
uint32_t g_scratch_push = 0;
uint32_t g_interval = 1;  // vertical blanks a frame is shown for, from the presentation parameters

// ── Guest calls ─────────────────────────────────────────────────────────────

// runs a recompiled function from here on the current guest thread: the
// arguments pushed right to left, then a return address, as `call` leaves it
uint32_t CallGuest(uint32_t fn, std::initializer_list<uint32_t> args) {
    uint32_t esp = g_esp, ebx = g_ebx, esi = g_esi, edi = g_edi;
    const uint32_t* a = args.begin();
    for (size_t i = args.size(); i-- > 0;) {
        g_esp -= 4;
        W32(g_esp, a[i]);
    }
    g_esp -= 4;
    W32(g_esp, 0);
    recomp_func_t f = recomp_lookup(fn);
    if (f) f();
    else LOG_FIRST(8, "d3d: no recompiled function at %08X", fn);
    g_esp = esp;  // stdcall or cdecl alike
    g_ebx = ebx;
    g_esi = esi;
    g_edi = edi;
    return g_eax;
}

// the XDK's internal reference count lives in bits 19-22 of Common; the last one
// going (with no external references either) destroys the resource
void AddRef(uint32_t res) {
    if (res) W32(res, R32(res) + 0x80000);
}
void Release(uint32_t res) {
    if (!res) return;
    uint32_t c = R32(res) - 0x80000;
    W32(res, c);
    if (!(c & 0x78FFFF)) CallGuest(kDestroyResource, {res});
}

uint32_t Dev(uint32_t off) { return R32(kDevice + off); }
void SetDev(uint32_t off, uint32_t v) { W32(kDevice + off, v); }
float ArgF(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
float GF(uint32_t va) {
    float f;
    std::memcpy(&f, HostPtr(va), 4);
    return f;
}

gpu::Surface SurfaceOf(uint32_t header) {
    gpu::Surface s;
    s.header = header;
    if (header) {
        s.data = R32(header + 4);
        s.format = R32(header + 12);
        s.size = R32(header + 16);
    }
    return s;
}

uint32_t NewSurface(uint32_t w, uint32_t h, uint32_t fmt, uint32_t bpp) {
    uint32_t pitch = (w * bpp + 63) & ~63u;
    uint32_t data = mem::AllocContiguous(pitch * h, 0, 0xFFFFFFFF, 0x1000);
    uint32_t hdr = mem::AllocSystem(24);
    W32(hdr + 0, 0x000D0001);  // surface, one reference, one internal reference held by the device
    W32(hdr + 4, mem::PhysicalFromVirtual(data));
    W32(hdr + 8, 0);
    W32(hdr + 12, 0x00010021 | (fmt << 8));  // dma A, 2D, one level
    W32(hdr + 16, (w - 1) | (h - 1) << 12 | (pitch / 64 - 1) << 24);
    W32(hdr + 20, 0);
    return hdr;
}

void SyncDeferred() {
    // deferred states live in the title's memory, the inline SetRenderState and SetTextureStageState write them
    for (uint32_t i = gpu::kRsDeferredFirst; i < gpu::kRsComplexFirst; i++) g_state.rs[i] = R32(kRenderState + i * 4);
    for (int s = 0; s < 4; s++)
        for (uint32_t i = 0; i < gpu::kTssBumpEnvMat00; i++) g_state.tss[s][i] = R32(kTextureState + (s * 32 + i) * 4);
}

float ZScale(const gpu::Surface& zs) {
    uint32_t f = zs.header ? (zs.format >> 8) & 0xFF : 0x2E;
    return f == 0x2C || f == 0x30 ? 65535.0f : 16777215.0f;
}

// c-38 and c-37 hold the viewport scale and offset, as D3D_GetViewportOffsetAndScale computes them
void UpdateViewportConstants() {
    auto& v = g_state.viewport;
    g_state.zscale = ZScale(g_state.zs);
    float hw = v.w * 0.5f, hh = v.h * 0.5f;
    g_state.vsc[58] = {hw, -hh, (v.max_z - v.min_z) * g_state.zscale, 0};
    g_state.vsc[59] = {v.x + g_state.ss_offset_x + hw, v.y + g_state.ss_offset_y + hh, v.min_z * g_state.zscale, 0};
}

void SetComplex(uint32_t rs, uint32_t v) {
    g_state.rs[rs] = v;
    W32(kRenderState + rs * 4, v);
}

// frame pacing: Swap returns on the 59.94 Hz grid, every g_interval-th tick
// xboxrecomp: g_pace_late_ms, how far past its tick the latest Swap left, for [FRAME] (2026-11-02). A high resolution
// waitable timer in place of Sleep was tried and measured worse under Wine (worst 0.32 ms off the grid against 0.004:
// the final Sleep(0) loop already lands on the tick), so the wait is unchanged; frames slower than 16.7 ms are the
// frame's own work, which this shows
double g_pace_late_ms = 0;

void Pace() {
    static LARGE_INTEGER freq, next;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    // xboxrecomp: the title's own present interval (the 30 fps cap that doubled it was removed)
    LONGLONG period = freq.QuadPart * 1001 / 60000 * std::max<uint32_t>(g_interval, 1u);
    QueryPerformanceCounter(&now);
    if (!next.QuadPart || now.QuadPart - next.QuadPart > 4 * period) next.QuadPart = now.QuadPart;
    while (now.QuadPart < next.QuadPart) {
        LONGLONG left = (next.QuadPart - now.QuadPart) * 1000 / freq.QuadPart;
        Sleep(left > 1 ? DWORD(left - 1) : 0);
        QueryPerformanceCounter(&now);
    }
    g_pace_late_ms = double(now.QuadPart - next.QuadPart) * 1000.0 / double(freq.QuadPart);
    next.QuadPart += period;
}

int g_on = -1, g_trace = 0;

}  // namespace

bool On() {
    if (g_on < 0) {
        const char* e = std::getenv("RECOMP_HLE_D3D");
        const char* t = std::getenv("RECOMP_D3D_TRACE");
        g_on = !(e && *e == '0');
        g_trace = t && *t && *t != '0';
        std::fprintf(stderr, "[D3D] %s\n", g_on ? "host D3D12 renderer (HLE)" : "XDK Direct3D on the emulated NV2A");
    }
    return g_on != 0;
}

}  // namespace d3d

// the generated XDK bodies this file wraps (tools/recomp/manual_scan.py reads this list)
extern "C" {
extern void sub_00130640_gen(void);
extern void sub_001306F0_gen(void);
extern void sub_00130720_gen(void);
extern void sub_00130750_gen(void);
extern void sub_00130A80_gen(void);
extern void sub_00130AC0_gen(void);
extern void sub_00130B00_gen(void);
extern void sub_00130B50_gen(void);
extern void sub_00130BC0_gen(void);
extern void sub_00130C00_gen(void);
extern void sub_00130C40_gen(void);
extern void sub_00130CA0_gen(void);
extern void sub_00130D10_gen(void);
extern void sub_00130D90_gen(void);
extern void sub_00130E10_gen(void);
extern void sub_00130E70_gen(void);
extern void sub_00130EC0_gen(void);
extern void sub_00130F20_gen(void);
extern void sub_00130F90_gen(void);
extern void sub_00130FE0_gen(void);
extern void sub_001310F0_gen(void);
extern void sub_00131160_gen(void);
extern void sub_001311A0_gen(void);
extern void sub_001311E0_gen(void);
extern void sub_00131BD0_gen(void);
extern void sub_00131C60_gen(void);
extern void sub_00131CF0_gen(void);
extern void sub_00131D60_gen(void);
extern void sub_00131D90_gen(void);
extern void sub_00131E00_gen(void);
extern void sub_00131E70_gen(void);
extern void sub_00131E90_gen(void);
extern void sub_00131EB0_gen(void);
extern void sub_00131F10_gen(void);
extern void sub_00131F50_gen(void);
extern void sub_00131FD0_gen(void);
extern void sub_00132020_gen(void);
extern void sub_00132570_gen(void);
extern void sub_001326B0_gen(void);
extern void sub_00132710_gen(void);
extern void sub_001327C0_gen(void);
extern void sub_001328E0_gen(void);
extern void sub_00132970_gen(void);
extern void sub_001329D0_gen(void);
extern void sub_00132A50_gen(void);
extern void sub_00132CA0_gen(void);
extern void sub_00132EA0_gen(void);
extern void sub_00132EC0_gen(void);
extern void sub_00132FD0_gen(void);
extern void sub_00133010_gen(void);
extern void sub_00133040_gen(void);
extern void sub_00133090_gen(void);
extern void sub_00133260_gen(void);
extern void sub_00133330_gen(void);
extern void sub_00133380_gen(void);
extern void sub_001333A0_gen(void);
extern void sub_001333C0_gen(void);
extern void sub_00133520_gen(void);
extern void sub_001336D0_gen(void);
extern void sub_00133760_gen(void);
extern void sub_001337D0_gen(void);
extern void sub_00133890_gen(void);
extern void sub_00133A30_gen(void);
extern void sub_00133A80_gen(void);
extern void sub_00133AD0_gen(void);
extern void sub_00133CD0_gen(void);
extern void sub_00134270_gen(void);
extern void sub_00134D70_gen(void);
extern void sub_00135050_gen(void);
extern void sub_00135760_gen(void);
extern void sub_001357D0_gen(void);
extern void sub_00135920_gen(void);
extern void sub_001359D0_gen(void);
extern void sub_00135B10_gen(void);
extern void sub_00135C90_gen(void);
extern void sub_00135CA0_gen(void);
extern void sub_00135CF0_gen(void);
extern void sub_00135D10_gen(void);
extern void sub_00135E60_gen(void);
extern void sub_00135FC0_gen(void);
extern void sub_00136100_gen(void);
extern void sub_001361A0_gen(void);
extern void sub_00136400_gen(void);
extern void sub_00136440_gen(void);
extern void sub_001364A0_gen(void);
extern void sub_001364E0_gen(void);
extern void sub_001387A0_gen(void);
}

using namespace d3d;

// ── Entry point glue ────────────────────────────────────────────────────────

#define A(i) R32(g_esp + 4 + 4 * (i))
#define TRACE(...)                                                      \
    do {                                                                \
        if (g_trace) {                                                  \
            std::fprintf(stderr, "[D3D] " __VA_ARGS__);                 \
            std::fputc('\n', stderr);                                   \
        }                                                               \
    } while (0)

// a host body for the guest function at addr: __stdcall with nargs stack
// arguments (also __fastcall, whose first two are ecx and edx and the rest on
// the stack), or the XDK's own body when the HLE is off
#define HLE(addr, nargs)                                         \
    extern "C" void sub_##addr##_gen(void);                      \
    static uint32_t hle_##addr();                                \
    extern "C" void sub_##addr(void) {                           \
        if (!On()) {                                             \
            sub_##addr##_gen();                                  \
            return;                                              \
        }                                                        \
        uint32_t r = hle_##addr();                               \
        g_eax = r;                                               \
        g_esp += 4 + 4 * (nargs);                                \
    }                                                            \
    static uint32_t hle_##addr()

// device

// Direct3D_CreateDevice(adapter, type, window, flags, params, device**)
HLE(00130640, 6) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t pp = A(4);
    uint32_t w = R32(pp), h = R32(pp + 4), fmt = R32(pp + 8), count = std::max<uint32_t>(R32(pp + 12), 1);
    uint32_t auto_depth = R32(pp + 0x20), depth_fmt = R32(pp + 0x24), interval = R32(pp + 0x30);
    if (!w) w = 640;
    if (!h) h = 480;
    LOGI("d3d: CreateDevice %ux%u format %X buffers %u depth %u format %X interval %X flags %X", w, h, fmt, count,
         auto_depth, depth_fmt, interval, R32(pp + 0x28));
    g_interval = interval == 2 ? 2 : interval == 3 ? 3 : interval == 4 ? 4 : 1;
    if (interval == 0x80000000u) g_interval = 1;
    std::memset(HostPtr(kDevice), 0, kDeviceBytes);
    W32(kDevicePtr, kDevice);
    // anything still pushing hardware commands writes into a scratch buffer that MakeSpace rewinds
    g_scratch_push = mem::AllocContiguous(0x10000, 0, 0xFFFFFFFF, 0x1000);
    SetDev(kDevPush, g_scratch_push);
    SetDev(kDevThreshold, g_scratch_push + 0xF000);

    uint32_t color_fmt = gpu::LinearColorFormat(fmt);
    g_buffer_count = std::min<uint32_t>(count + 1, 3);
    for (uint32_t i = 0; i < g_buffer_count; i++) g_buffers[i] = NewSurface(w, h, color_fmt, 4);
    g_depth = 0;
    if (auto_depth) {
        uint32_t zf = gpu::LinearDepthFormat(depth_fmt);
        g_depth = NewSurface(w, h, zf, zf == 0x30 || zf == 0x31 ? 2 : 4);  // 0x30 LIN_D16, 0x31 LIN_F16
    }
    if (!g_backend) {
        g_backend = gpu::CreateBackend();
        if (!g_backend->Init(w, h)) LOGE("d3d: renderer init failed");
        g_backend = gpu::CreateThreadedBackend(std::move(g_backend));
        // the window thread stops painting its own picture: the swap chain owns the window now
        xbox_FramebufferWindowSetPresenter([](void*, const XboxPresentInfo*) { return 1; });
        // xboxrecomp: the start menu (app/menu.cpp): the title waits here, before its first movie, while the page is
        // drawn over a black frame; its aspect ratio is the run's (below, and hle_video.c's widescreen check)
        if (app::Get().start_menu) {
            app::OpenStartMenu();
            LOGI("d3d: start menu up, the title waits for Start");
            while (app::StartMenuOpen()) {
                {
                    std::lock_guard<std::recursive_mutex> lock(g_lock);
                    g_backend->Present(SurfaceOf(g_buffers[1]), false);
                }
                Sleep(16);
            }
        }
    }
    // xboxrecomp: true 16:9. Every camera mode copies the title's aspect constant (1.3333 at 0x1E2FC0, beside its 55
    // degree vertical field of view at 0x1E2FBC) into the projection; 16:9 widens the view at the sides (Hor+).
    // The frame stays 640x480, shown 16:9 wide; d3d12.cpp's Squeeze2D keeps the 2D at its 4:3 shape.
    if (app::BootAspect() == app::AspectMode::Wide) {
        float wide = 16.0f / 9.0f;
        std::memcpy(HostPtr(0x001E2FC0), &wide, 4);
        LOGI("d3d: 16:9 camera aspect");
    }
    g_state = gpu::State{};
    g_state.viewport = {0, 0, w, h, 0.0f, 1.0f};
    SetDev(kDevRenderTarget, g_buffers[1]);
    SetDev(kDevDepth, g_depth);
    SetDev(kDevFrameBuffers + 0, g_buffers[1]);
    SetDev(kDevFrameBuffers + 4, g_buffers[0]);
    SetDev(kDevFrameBuffers + 0xC, g_depth);
    float one = 1.0f, zs = g_depth && (R32(g_depth + 12) >> 8 & 0xFF) == 0x30 ? 65535.0f : 16777215.0f;
    std::memcpy(HostPtr(kDevice + kDevSuperX), &one, 4);
    std::memcpy(HostPtr(kDevice + kDevSuperY), &one, 4);
    std::memcpy(HostPtr(kDevice + kDevZScale), &zs, 4);
    g_state.rt = SurfaceOf(g_buffers[1]);
    g_state.zs = SurfaceOf(g_depth);
    UpdateViewportConstants();
    // the XDK's own default state setup, through the entry points below
    CallGuest(kInitDefaultState, {});
    if (A(5)) W32(A(5), kDevice);
    return 0;
}

// synchronisation, the renderer never runs behind

// D3D_BlockOnResource(resource)
HLE(00135D10, 1) {
    uint32_t res = A(0);
    if (res && g_backend) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        uint32_t type = R32(res) & 0x70000;  // 0x40000 texture, 0x50000 surface
        static const bool no_yuv = std::getenv("RECOMP_LOCK_NO_YUV_SYNC") != nullptr;
        uint32_t fmt = (R32(res + 0xC) >> 8) & 0xFF;
        if (no_yuv && (fmt == 0x24 || fmt == 0x25)) {
        } else if (type == 0x40000 || type == 0x50000) {
            uint32_t data = mem::VirtualFromPhysical(R32(res + 4));
            g_backend->Sync(data, 0);
        } else {
            g_backend->WaitResource(res);
        }
    }
    gpu::MarkDirty(res);
    return 0;
}

// Lock2DSurface(resource, face, level, locked, rect, flags): D3DLOCK_NOOVERWRITE skips the block above, yet a
// title may read what the GPU drew
extern "C" void sub_001387A0_gen(void);
extern "C" void sub_001387A0(void) {
    if (On()) {
        uint32_t res = A(0);
        static const bool trace = std::getenv("RECOMP_LOCK_TRACE") != nullptr;
        static const bool no_yuv = std::getenv("RECOMP_LOCK_NO_YUV_SYNC") != nullptr;
        uint32_t fmt = res ? (R32(res + 0xC) >> 8) & 0xFF : 0;
        if (trace && res) {
            static int n;
            if (n++ < 400) fprintf(stderr, "[LOCK] %08X data %08X fmt %02X flags %X from %08X\n", res, R32(res + 4), fmt, A(5),
                                   R32(g_esp));
        }
        if (no_yuv && (fmt == 0x24 || fmt == 0x25)) {
        } else if ((A(5) & 0x20) && res && g_backend) {
            std::lock_guard<std::recursive_mutex> lock(g_lock);
            g_backend->SyncLate(mem::VirtualFromPhysical(R32(res + 4)));
        }
    }
    sub_001387A0_gen();
}

// D3DResource_BlockUntilNotBusy(resource)
HLE(00134270, 1) {
    if (A(0) && g_backend) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_backend->WaitResource(A(0));
    }
    gpu::MarkDirty(A(0));
    return 0;
}
HLE(00133760, 0) { return 0; }  // D3DDevice_IsBusy
HLE(001359D0, 2) { return 0; }  // D3D_BlockOnTime(time, ...)
// D3D_SetFence(flags)
HLE(00135920, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_backend ? g_backend->Fence() : 0;
}
// D3D_KickOffAndWaitForIdle
HLE(00135CF0, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_backend) g_backend->Flush();
    return 0;
}
HLE(001357D0, 0) { return 0; }  // CDevice_KickOff (__thiscall, no stack arguments)

// D3DDevice_MakeSpace
HLE(00135C90, 0) {
    LOG_FIRST(16, "d3d: MakeSpace from %08X, some caller still pushes hardware commands", R32(g_esp));
    SetDev(kDevPush, g_scratch_push);
    return g_scratch_push;
}
// D3D_MakeRequestedSpace(minimum, requested)
HLE(00135B10, 2) {
    LOG_FIRST(16, "d3d: MakeRequestedSpace from %08X", R32(g_esp));
    SetDev(kDevPush, g_scratch_push);
    return g_scratch_push;
}
// D3DDevice_BeginStateBig(count): room for a long run of pushes
HLE(00135CA0, 1) {
    LOG_FIRST(16, "d3d: BeginStateBig from %08X", R32(g_esp));
    SetDev(kDevPush, g_scratch_push);
    return g_scratch_push;
}

// D3DDevice_InsertCallback(type, callback, context): the GPU would call it as it passes this point; the renderer
// is never behind, so now
HLE(00135760, 3) {
    uint32_t cb = A(1), context = A(2);
    TRACE("InsertCallback type %u fn %08X ctx %08X", A(0), cb, context);
    if (cb) CallGuest(cb, {context});
    return 0;
}

// D3DDevice_BlockUntilVerticalBlank
HLE(00133010, 0) {
    Sleep(16);
    return 0;
}
HLE(00133040, 1) { return 0; }  // D3DDevice_SetFlickerFilter
HLE(00133090, 1) { return 0; }  // D3DDevice_SetSoftDisplayFilter
HLE(001337D0, 2) { return 0; }  // D3D_SetTileNoWait: the console's memory controller's business

// D3DDevice_GetDisplayMode(mode): Width, Height, RefreshRate, Flags, Format of the device's buffers
HLE(00133260, 1) {
    uint32_t m = A(0);
    uint32_t w = 640, h = 480;
    if (g_buffers[1]) gpu::SurfaceSize(SurfaceOf(g_buffers[1]), w, h);
    W32(m + 0, w);
    W32(m + 4, h);
    W32(m + 8, 60);
    W32(m + 12, 0);
    W32(m + 16, 0x1E);  // D3DFMT_LIN_X8R8G8B8
    return 0;
}

// targets and presentation

// D3DDevice_SetRenderTarget(target, depth)
HLE(00132EA0, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t rt = A(0) ? A(0) : Dev(kDevRenderTarget), zs = A(1);
    AddRef(rt);
    AddRef(zs);
    uint32_t old_rt = Dev(kDevRenderTarget), old_zs = Dev(kDevDepth);
    SetDev(kDevRenderTarget, rt);
    SetDev(kDevDepth, zs);
    Release(old_rt);
    Release(old_zs);
    g_state.rt = SurfaceOf(rt);
    g_state.zs = SurfaceOf(zs);
    // SetRenderTarget resets the viewport to the whole target
    uint32_t w, h;
    gpu::SurfaceSize(g_state.rt, w, h);
    g_state.viewport = {0, 0, w, h, 0.0f, 1.0f};
    float zscale = ZScale(g_state.zs);
    std::memcpy(HostPtr(kDevice + kDevZScale), &zscale, 4);
    UpdateViewportConstants();
    return 0;
}

// D3DDevice_GetBackBuffer2(index): -1 the front buffer
HLE(00133330, 1) {
    int32_t i = int32_t(A(0));
    uint32_t s = i < 0 ? g_buffers[0] : g_buffers[std::min<uint32_t>(uint32_t(i) + 1, g_buffer_count - 1)];
    W32(s, R32(s) + 1);
    return s;
}
// D3DDevice_GetRenderTarget2
HLE(00133380, 0) {
    uint32_t s = Dev(kDevRenderTarget);
    if (s) W32(s, R32(s) + 1);
    return s;
}
// D3DDevice_GetDepthStencilSurface2
HLE(001333A0, 0) {
    uint32_t s = Dev(kDevDepth);
    if (s) W32(s, R32(s) + 1);
    return s;
}

// xboxrecomp debug: the time between the last two Swaps, for main.c's RECOMP_PROFILE_AUTO (slow frames apart)
extern "C" volatile double hpcos_last_frame_ms = 0;
// xboxrecomp debug: when the last Swap happened (QueryPerformanceCounter), for main.c's [STALL] watcher
extern "C" volatile int64_t hpcos_last_swap_qpc = 0;
extern "C" void hpcos_RenderStats(double out[8], uint64_t* draws);  // gpu/render_thread.cpp
extern "C" void hpcos_GpuStats(double out[6], uint32_t frames);     // gpu/d3d12.cpp

// xboxrecomp debug: RECOMP_PERF=1 logs where the frames' time went every 2 s ([PERF]): frames and fps, the slowest,
// then per frame on average: the game thread's own work (the frame less its pacing sleep and its waits on the render
// thread), its waits on the render thread (for the frame before, read-backs and the rest), the render thread
// executing the frame's commands and inside Present (the swap chain: vsync and the GPU), and draws
static void PerfFrame(double frame_ms, double pace_ms) {
    static int on = -1;
    if (on < 0) {
        const char* e = std::getenv("RECOMP_PERF");
        on = e && *e && *e != '0';
    }
    if (!on) return;
    static LARGE_INTEGER freq, t0;
    static double sum_frame, sum_pace, worst;
    static uint32_t frames;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq), t0 = now;
    sum_frame += frame_ms, sum_pace += pace_ms, frames++;
    if (frame_ms > worst) worst = frame_ms;
    double span = double(now.QuadPart - t0.QuadPart) / double(freq.QuadPart);
    if (span < 2.0) return;
    double r[8], g[6];
    uint64_t draws;
    hpcos_RenderStats(r, &draws);
    hpcos_GpuStats(g, frames);
    double n = frames, waits = r[0] + r[1] + r[2] + r[3] + r[4] + r[5];
    std::fprintf(stderr, "[PERF] %u frames %.1f fps, slowest %.1f ms | per frame ms: game work %.2f, waits on render "
                 "thread %.2f (frame %.2f read-back %.2f other %.2f), pace sleep %.2f | render thread exec %.2f, "
                 "present %.2f | %.0f draws | GPU ms: drawing %.2f, shown frame passes %.2f | target copies %.1f "
                 "(%.0f Msamples) | uploads %.1f MB, ring waits %.2f\n",
                 frames, n / span, worst, (sum_frame - sum_pace - waits) / n, waits / n, r[5] / n, r[0] / n,
                 (r[1] + r[2] + r[3] + r[4]) / n, sum_pace / n, r[6] / n, r[7] / n, double(draws) / n, g[0], g[1],
                 g[2], g[3], g[4], g[5]);
    sum_frame = sum_pace = worst = 0;
    frames = 0;
    t0 = now;
}

extern "C" void hpcos_HitchRenderStats(double out[3]);  // gpu/render_thread.cpp
extern "C" void hpcos_HitchGpuStats(double out[8]);     // gpu/d3d12.cpp
extern "C" void xbox_kernel_frame_stats(double* file_ms, double* other_ms, uint32_t* file_calls, double* worst_ms,
                                        char* worst, size_t worst_len, double* top_other_ms, char* top_other,
                                        size_t top_len);  // xboxrecomp kernel_bridge.c

// xboxrecomp debug: [HITCH], one line for every frame over 25 ms (a dropped 60 Hz frame), saying where its time went:
// the game thread in file calls (how many, the slowest and its file) and in other kernel calls (waits, sleeps; the one
// that took most of it), its
// waits on the render thread, the game's own code (the rest); the render thread executing and presenting; textures
// made and uploaded (MB, ms), pipelines loaded or made (ms) and draws left out for one. The render thread's figures are
// the work done since the last Swap, which is mostly but not exactly this frame's. The counters are cleared each frame
static LARGE_INTEGER g_swap_entry;  // this Swap's start: the end of the title's frame

// frame_ms: the time between two frames leaving the pacing (one shown each 60 Hz tick when on time: 16.7 ms; over 25
// a tick was missed, which is what a player sees). work_ms: the title's own time for the frame, from the last Swap's
// return to this one's start. Measured Swap to Swap instead (as before 2026-10-25), a frame with ~8 ms more work than
// the one before it read as over 25 ms though it was shown on time
static void HitchFrame(double frame_ms, double work_ms) {
    double r[3], g[8], file_ms, other_ms, worst_ms, top_ms;
    uint32_t file_calls;
    char worst[96], top[48];
    hpcos_HitchRenderStats(r);
    hpcos_HitchGpuStats(g);
    xbox_kernel_frame_stats(&file_ms, &other_ms, &file_calls, &worst_ms, worst, sizeof worst, &top_ms, top,
                            sizeof top);
    static uint32_t lines;
    if (frame_ms <= 25.0 || lines >= 3000) return;
    lines++;
    double game = frame_ms - file_ms - other_ms - r[0];
    std::fprintf(stderr, "[HITCH] %.1f ms (work %.1f) | game thread: code %.1f, file calls %.1f (%u, slowest %.1f %s), other "
                 "kernel %.1f (most %.1f %s), waits on render thread %.1f | render thread: exec %.1f, present %.1f | textures %.0f "
                 "made %.1f ms, %.0f uploaded %.2f MB %.1f ms | pipelines %.0f, %.1f ms, %.0f draws left out\n",
                 frame_ms, work_ms, game < 0 ? 0 : game, file_ms, file_calls, worst_ms, worst[0] ? worst : "-", other_ms, top_ms, top, r[0],
                 r[1], r[2], g[0], g[7], g[1], g[2], g[3], g[4], g[5], g[6]);
}

// D3DDevice_Swap(flags)
static std::atomic<uint64_t> g_last_swap_ms{0};
static std::atomic<int> g_hud_trace{0};  // xboxrecomp debug: HudTrace, F12
static uint32_t g_hud_n = 0;
extern "C" int hpcos_HudTracing(void) { return g_hud_trace.load(); }                         // hle_hud.c
extern "C" int hpcos_Widescreen(void) { return app::BootAspect() == app::AspectMode::Wide; }  // hle_hud.c
// xboxrecomp: the debug trainer's report asks for an F12 capture of the next frame (hle_trainer.c)
extern "C" void hpcos_RequestCapture(void) { gpu::g_capture = true; }
// xboxrecomp debug: clears and draws since the last Swap, numbered as a capture's frame.txt (hle_debug.c's F8 trace)
extern "C" volatile uint32_t hpcos_draw_seq = 0;
// xboxrecomp debug: Swaps so far, the frame number MorphTrace compares vertex hashes by
static std::atomic<uint32_t> g_swaps{0};

// A movie frame went up (hle_video.c's UpdateOverlay). The NV2A scans the overlay out by itself, so a title playing
// an XMV need not Swap; when it has not swapped lately, present the last frame for the overlay to be drawn over.
extern "C" void hle_d3d8_overlay_present(void) {
    if (!On() || GetTickCount64() - g_last_swap_ms.load() < 50) return;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_backend && g_buffers[1]) g_backend->Present(SurfaceOf(g_buffers[1]), false);
}

HLE(00134D70, 1) {
    g_last_swap_ms.store(GetTickCount64());
    hpcos_draw_seq = 0;
    g_swaps++;
    if (app::BootAspect() == app::AspectMode::Wide) {
        // xboxrecomp: 16:9. The inventory's camera (sub_00028380) projects 4 / 3 into the frame's aspect (0x50C2B8 /
        // 0x50C2BC) and the title places its labels, counters and map icons by that projection: such a screen is
        // shown 4:3 in the middle, 3D and 2D narrowed alike. The gameplay cameras' 16:9 (CreateDevice) is not 4:3.
        float num, den;
        std::memcpy(&num, HostPtr(0x0050C2B8), 4);
        std::memcpy(&den, HostPtr(0x0050C2BC), 4);
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_state.pillar = den != 0 && std::fabs(num / den - 4.0f / 3.0f) < 0.01f;
    }
    {   // xboxrecomp debug: F12 down starts a [HUD] trace of the next frame, this Swap ends one
        static bool was_down;
        bool down = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
        if (g_hud_trace.exchange(0)) fprintf(stderr, "[HUD] end\n");
        if (down && !was_down) {
            gpu::g_capture = true;  // this GetAsyncKeyState takes F12's pressed bit the renderer's own check reads
            g_hud_n = 0;
            g_hud_trace.store(1);
            fprintf(stderr, "[HUD] begin\n");
        }
        was_down = down;
    }
    {   // the time between Swaps (the title's frame and the pacing before it), for main.c's watchers
        static LARGE_INTEGER freq, last;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq), last = now;
        hpcos_last_frame_ms = double(now.QuadPart - last.QuadPart) * 1000.0 / double(freq.QuadPart);
        last = now;
        hpcos_last_swap_qpc = now.QuadPart;
        g_swap_entry = now;
    }
    {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        if (g_backend) g_backend->Present(SurfaceOf(g_buffers[1]), false);
        // like the XDK, the surfaces stay put and their memory rotates: titles keep back buffer pointers
        uint32_t last = g_buffers[g_buffer_count - 1], data = R32(last + 4), lk = R32(last + 8);
        for (uint32_t i = g_buffer_count - 1; i > 0; i--) {
            W32(g_buffers[i] + 4, R32(g_buffers[i - 1] + 4));
            W32(g_buffers[i] + 8, R32(g_buffers[i - 1] + 8));
        }
        W32(g_buffers[0] + 4, data);
        W32(g_buffers[0] + 8, lk);
        g_state.rt = SurfaceOf(Dev(kDevRenderTarget));
    }
    xbox_FramebufferWindowFlipped();
    xbox_FramebufferWindowFrameStats(0);
    {   // xboxrecomp debug: RECOMP_PERF, the pacing sleep timed
        LARGE_INTEGER a, b, f;
        QueryPerformanceCounter(&a);
        Pace();
        QueryPerformanceCounter(&b);
        // [FRAME] every 5 s: frames, the slowest and how many over 25 ms, between frames leaving the pacing
        static LARGE_INTEGER prev, t0;
        static uint32_t frames, slow;
        static double worst, late_worst;
        if (g_pace_late_ms > late_worst) late_worst = g_pace_late_ms;
        QueryPerformanceFrequency(&f);
        if (prev.QuadPart) {
            double shown = double(b.QuadPart - prev.QuadPart) * 1000.0 / double(f.QuadPart);
            double work = double(g_swap_entry.QuadPart - prev.QuadPart) * 1000.0 / double(f.QuadPart);
            frames++;
            if (shown > worst) worst = shown;
            if (shown > 25.0) slow++;
            HitchFrame(shown, work);
        } else {
            t0 = b;
        }
        prev = b;
        double span = double(b.QuadPart - t0.QuadPart) / double(f.QuadPart);
        if (span >= 5.0) {
            fprintf(stderr, "[FRAME] %.1f fps, slowest %.1f ms, %u frames over 25 ms, Swap up to %.2f ms after its tick\n",
                    frames / span, worst, slow, late_worst);
            frames = slow = 0;
            worst = late_worst = 0;
            t0 = b;
        }
        QueryPerformanceFrequency(&f);
        PerfFrame(hpcos_last_frame_ms, double(b.QuadPart - a.QuadPart) * 1000.0 / double(f.QuadPart));
    }
    uint32_t n = Dev(kDevSwapCount) + 1;
    SetDev(kDevSwapCount, n);
    return n;
}

// D3DDevice_Clear(count, rects, flags, color, z, stencil)
HLE(00135050, 6) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    const uint8_t* rects = A(0) && A(1) ? static_cast<const uint8_t*>(HostPtr(A(1))) : nullptr;
    if (g_backend) hpcos_draw_seq++, g_backend->Clear(g_state, A(0), rects, A(2), A(3), ArgF(A(4)), A(5));
    return 0;
}

// D3DDevice_SetViewport(viewport)
HLE(001333C0, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t v = A(0);
    g_state.viewport = {R32(v), R32(v + 4), R32(v + 8), R32(v + 12), GF(v + 16), GF(v + 20)};
    g_state.scissor_count = 0;
    // the XDK's copy, which GetViewport and GetViewportOffsetAndScale read
    std::memcpy(HostPtr(kDevice + 0xA90), HostPtr(v), 24);
    UpdateViewportConstants();
    return 0;
}

// D3DDevice_SetScissors(count, exclusive, rects)
HLE(00133890, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_state.scissor_count = std::min<uint32_t>(A(0), 8);
    for (uint32_t i = 0; i < g_state.scissor_count * 4; i++) g_state.scissors[i] = R32(A(2) + i * 4);
    return 0;
}

// D3DDevice_SetScreenSpaceOffset(x, y)
HLE(00133A30, 2) {
    g_state.ss_offset_x = ArgF(A(0));
    g_state.ss_offset_y = ArgF(A(1));
    W32(kDevice + 0xAA8, A(0));
    W32(kDevice + 0xAAC, A(1));
    UpdateViewportConstants();
    return 0;
}

// render states

// D3DDevice_SetRenderState_Simple(method, value), __fastcall
HLE(00130720, 0) {
    uint32_t method = g_ecx & 0x1FFC, value = g_edx;
    g_state.simple[method >> 2] = value;
    return 0;
}

// D3DDevice_SetRenderStateNotInline(state, value)
HLE(00130750, 2) {
    uint32_t rs = A(0), v = A(1);
    if (rs < gpu::kRsDeferredFirst) {
        uint32_t method = R32(kSimpleEncode + rs * 4) & 0x1FFC;
        g_state.simple[method >> 2] = v;
        W32(kRenderState + rs * 4, v);
    } else if (rs < gpu::kRsComplexFirst) {
        W32(kRenderState + rs * 4, v);
    } else if (rs < gpu::kRsMax) {
        SetComplex(rs, v);
    }
    return 0;
}

// D3DDevice_SetRenderState_ZBias(value). Not a plain complex state: the XDK turns it into the polygon offset simple
// states, which are what the renderer keys depth bias on (decals such as windows z-fight without it). As 5028 does:
// f = -(float)(DWORD)value; PolygonOffsetZSlopeScale (0x4D) = f / 4, PolygonOffsetZOffset (0x4E) = f, and the
// point/wireframe/solid offset enables (0x4F-0x51) = value != 0.
HLE(00130D90, 1) {
    uint32_t v = A(0);
    float f = -float(v);  // the XDK reads the DWORD unsigned
    float slope = f * 0.25f;
    uint32_t bits_slope, bits_f, on = v != 0;
    std::memcpy(&bits_slope, &slope, 4);
    std::memcpy(&bits_f, &f, 4);
    const uint32_t states[5][2] = {{0x4D, bits_slope}, {0x4E, bits_f}, {0x4F, on}, {0x50, on}, {0x51, on}};
    for (const auto& sv : states) {
        uint32_t method = R32(kSimpleEncode + sv[0] * 4) & 0x1FFC;
        g_state.simple[method >> 2] = sv[1];
        W32(kRenderState + sv[0] * 4, sv[1]);
    }
    SetComplex(gpu::kRsZBias, v);
    return 0;
}

#define COMPLEX_STATE(addr, rs) \
    HLE(addr, 1) {              \
        SetComplex(rs, A(0));   \
        return 0;               \
    }
// D3DDevice_SetRenderState_<name>(value), at this XBE's addresses
COMPLEX_STATE(001306F0, gpu::kRsPsTextureModes)
COMPLEX_STATE(00130F90, gpu::kRsVertexBlend)
COMPLEX_STATE(00130B00, gpu::kRsFogColor)
COMPLEX_STATE(00130E70, gpu::kRsFillMode)
COMPLEX_STATE(00130EC0, gpu::kRsBackFillMode)
COMPLEX_STATE(00130F20, gpu::kRsTwoSidedLighting)
COMPLEX_STATE(00130C00, gpu::kRsNormalizeNormals)
COMPLEX_STATE(00131BD0, gpu::kRsZEnable)
COMPLEX_STATE(00131C60, gpu::kRsStencilEnable)
COMPLEX_STATE(00131CF0, gpu::kRsStencilFail)
COMPLEX_STATE(00130BC0, gpu::kRsFrontFace)
COMPLEX_STATE(00130B50, gpu::kRsCullMode)
COMPLEX_STATE(00130C40, gpu::kRsTextureFactor)
COMPLEX_STATE(00130E10, gpu::kRsLogicOp)
COMPLEX_STATE(00130A80, gpu::kRsEdgeAntiAlias)
COMPLEX_STATE(00131F50, gpu::kRsMultiSampleAntiAlias)
COMPLEX_STATE(00131FD0, gpu::kRsMultiSampleMask)
// MultiSampleMode (00131ED0) is never called by this title, so the recompiler
// emits no body for it to fall back to.
COMPLEX_STATE(00131F10, gpu::kRsMultiSampleRenderTargetMode)
COMPLEX_STATE(00130AC0, gpu::kRsShadowFunc)
COMPLEX_STATE(00130CA0, gpu::kRsLineWidth)
COMPLEX_STATE(00132020, gpu::kRsSampleAlpha)
COMPLEX_STATE(00130D10, gpu::kRsDxt1NoiseEnable)
COMPLEX_STATE(00131D60, gpu::kRsYuvEnable)
COMPLEX_STATE(00131D90, gpu::kRsOcclusionCullEnable)
COMPLEX_STATE(00131E00, gpu::kRsStencilCullEnable)
COMPLEX_STATE(00131E70, gpu::kRsRopZCmpAlwaysRead)
COMPLEX_STATE(00131E90, gpu::kRsRopZRead)
COMPLEX_STATE(00131EB0, gpu::kRsDoNotCullUncompressed)

// D3DDevice_SetTextureState_TexCoordIndex(stage, value)
HLE(00130FE0, 2) {
    g_state.tss[A(0) & 3][gpu::kTssTexCoordIndex] = A(1);
    W32(kTextureState + ((A(0) & 3) * 32 + gpu::kTssTexCoordIndex) * 4, A(1));
    return 0;
}
// D3DDevice_SetTextureState_BumpEnv(stage, type, value)
HLE(001310F0, 3) {
    if (A(1) >= gpu::kTssBumpEnvMat00 && A(1) <= gpu::kTssBumpEnvLOffset) g_state.tss[A(0) & 3][A(1)] = A(2);
    return 0;
}
// D3DDevice_SetTextureState_BorderColor(stage, value)
HLE(00131160, 2) {
    g_state.tss[A(0) & 3][gpu::kTssBorderColor] = A(1);
    return 0;
}
// D3DDevice_SetTextureState_ColorKeyColor(stage, value)
HLE(001311A0, 2) {
    g_state.tss[A(0) & 3][gpu::kTssColorKeyColor] = A(1);
    return 0;
}
// D3DDevice_SetTextureStageStateNotInline(stage, type, value)
HLE(001311E0, 3) {
    uint32_t stage = A(0) & 3, type = A(1), v = A(2);
    if (type < gpu::kTssCount) {
        g_state.tss[stage][type] = v;
        W32(kTextureState + (stage * 32 + type) * 4, v);
    }
    return 0;
}

// D3DDevice_SetTransform(type, matrix): the XDK's types are view 0, projection 1, texture 2-5, world 6-9; the
// renderer keeps Direct3D 8's numbers (view 2, projection 3, texture 16, world 256). The device copy is what
// GetTransform reads.
HLE(00132EC0, 2) {
    uint32_t t = A(0);
    if (t >= 10) return 0;
    std::memcpy(HostPtr(kDevice + kDevTransforms + t * 64), HostPtr(A(1)), 64);
    uint32_t pc = t < 2 ? t + 2 : t < 6 ? t + 14 : t + 250;
    std::memcpy(g_state.transforms[pc].data(), HostPtr(A(1)), 64);
    return 0;
}

// D3DDevice_SetMaterial(material)
HLE(00132FD0, 1) {
    std::memcpy(g_state.material.data(), HostPtr(A(0)), 17 * 4);
    return 0;
}

// resources in use

// D3DDevice_SetTexture(stage, texture)
HLE(00133520, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t stage = A(0) & 3, tex = A(1);
    uint32_t old = Dev(kDevTextures + stage * 4);
    AddRef(tex);
    SetDev(kDevTextures + stage * 4, tex);
    if (old) W32(old + 8, Dev(kDevFence));
    Release(old);
    g_state.textures[stage] = tex;
    W32(kDirtyFlags, R32(kDirtyFlags) | 0x4800);
    return 0;
}

// D3DDevice_SetStreamSource(stream, buffer, stride)
HLE(001328E0, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t n = A(0) & 15, vb = A(1), stride = A(2);
    uint32_t old = R32(kStreams + n * 12 + 8);
    AddRef(vb);
    W32(kStreams + n * 12, stride);
    W32(kStreams + n * 12 + 8, vb);
    Release(old);
    g_state.streams[n] = {vb, stride};
    return 0;
}

// D3DDevice_SetIndices(buffer, base vertex)
HLE(001336D0, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t ib = A(0);
    uint32_t old = Dev(kDevIndexBuffer);
    AddRef(ib);
    W32(kIndexData, ib ? R32(ib + 4) : 0);
    SetDev(kDevIndexBuffer, ib);
    SetDev(kDevBaseVertex, A(1));
    Release(old);
    g_state.index_buffer = ib;
    g_state.index_data = ib ? R32(ib + 4) : 0;
    g_state.base_vertex = A(1);
    return 0;
}

// shaders

// D3DDevice_CreateVertexShader(declaration, function, handle*, usage)
HLE(00132570, 4) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t decl = A(0), func = A(1), out = A(2);
    auto vs = std::make_shared<gpu::VertexShader>();
    if (decl) {
        for (uint32_t i = 0; i < 1024; i++) {
            uint32_t t = R32(decl + i * 4);
            vs->decl.push_back(t);
            if (t == 0xFFFFFFFF) break;
        }
    }
    if (func) {
        uint32_t hdr = R32(func);
        vs->program_type = hdr & 0xFFFF;
        uint32_t n = hdr >> 16;
        for (uint32_t i = 0; i < n * 4; i++) vs->program.push_back(R32(func + 4 + i * 4));
    }
    gpu::ParseDeclaration(*vs);
    uint32_t rec = mem::AllocSystem(16, 16);
    vs->handle = rec | 1;
    g_vs[vs->handle] = vs;
    W32(out, vs->handle);
    LOG_FIRST(64, "d3d: vertex shader %08X, %zu decl tokens, %zu instructions, type %X", vs->handle, vs->decl.size(),
              vs->program.size() / 4, vs->program_type);
    return 0;
}

// D3DDevice_SetVertexShader(handle)
HLE(00132CA0, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t h = A(0);
    g_state.vs_handle = h;
    g_state.vs = nullptr;
    SetDev(0x37C, h);  // m_VertexShader, by XbSymbolDatabase
    if (h & 1) {
        auto it = g_vs.find(h);
        if (it != g_vs.end()) {
            g_state.vs = it->second;
            // constants declared with the shader load when it is selected
            for (auto& [reg, v] : it->second->constants)
                if (reg < 192) g_state.vsc[reg] = v;
        } else {
            LOG_FIRST(8, "d3d: SetVertexShader with unknown handle %08X", h);
        }
    }
    return 0;
}

namespace {
// constants by hardware register (D3D register + 96), also into the XDK's mirror that GetVertexShaderConstant reads
void VsConstants(uint32_t reg, uint32_t src, uint32_t count) {
    // xboxrecomp debug: RECOMP_VSC_TRACE=<hw register, e.g. 109> logs each change of that constant and its caller
    static int trace = [] { const char* e = std::getenv("RECOMP_VSC_TRACE"); return e && *e ? std::atoi(e) : -1; }();
    if (trace >= 0 && uint32_t(trace) >= reg && uint32_t(trace) < reg + count) {
        static float last[4];
        const float* v = reinterpret_cast<const float*>(HostPtr(src + (trace - reg) * 16));
        if (std::memcmp(v, last, 8)) {  // x and y: the fog constant's z and w are stack garbage
            std::memcpy(last, v, 16);
            std::fprintf(stderr, "[VSC] c%d = %g %g %g %g from %08X (stack %08X %08X %08X %08X)\n", trace, v[0], v[1], v[2],
                         v[3], R32(g_esp), R32(g_esp + 4), R32(g_esp + 8), R32(g_esp + 12), R32(g_esp + 16));
            if (R32(g_esp) == 0x00017F55u) {  // the game's fog setter 0x17EA0: edi = its fog record (+4 near, +8 far)
                float rn, rf, k;
                std::memcpy(&rn, HostPtr(g_edi + 4), 4);
                std::memcpy(&rf, HostPtr(g_edi + 8), 4);
                std::memcpy(&k, HostPtr(0x214010), 4);
                std::fprintf(stderr, "[VSC]   fog record %08X: %08X near %g far %g color %08X; K %g\n", g_edi, R32(g_edi), rn, rf,
                             R32(g_edi + 12), k);
            }
        }
    }
    for (uint32_t i = 0; i < count && reg + i < 192; i++) {
        std::memcpy(g_state.vsc[reg + i].data(), HostPtr(src + i * 16), 16);
        std::memcpy(HostPtr(kVsConstants + (reg + i) * 16), HostPtr(src + i * 16), 16);
    }
}
}  // namespace

// D3DDevice_SetVertexShaderConstant1(register, data), __fastcall
HLE(001326B0, 0) {
    VsConstants(g_ecx, g_edx, 1);
    return 0;
}
// D3DDevice_SetVertexShaderConstant4(register, data), __fastcall
HLE(00132710, 0) {
    VsConstants(g_ecx, g_edx, 4);
    return 0;
}
// D3DDevice_SetVertexShaderConstantNotInlineFast(register, data, dwords), __fastcall
HLE(001327C0, 1) {
    VsConstants(g_ecx, g_edx, A(0) / 4);
    return 0;
}

// D3DDevice_LoadVertexShader(handle, address)
HLE(00132970, 2) {
    LOG_FIRST(4, "d3d: LoadVertexShader %08X at %u is not supported", A(0), A(1));
    return 0;
}
// D3DDevice_SelectVertexShader(handle, address)
HLE(001329D0, 2) {
    LOG_FIRST(4, "d3d: SelectVertexShader %08X at %u is not supported", A(0), A(1));
    return 0;
}
HLE(00132A50, 1) { return 0; }  // D3DDevice_SetShaderConstantMode

// D3DDevice_CreatePixelShader(definition, handle*)
HLE(00133A80, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto ps = std::make_shared<gpu::PixelShader>();
    std::memcpy(ps->def, HostPtr(A(0)), sizeof(ps->def));
    uint32_t rec = mem::AllocSystem(16, 16);
    ps->handle = rec;
    g_ps[rec] = ps;
    W32(A(1), rec);
    return 0;
}

// D3DDevice_SetPixelShader(handle)
HLE(00133AD0, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_state.ps = nullptr;
    if (!A(0)) return 0;
    auto it = g_ps.find(A(0));
    if (it == g_ps.end()) {
        LOG_FIRST(8, "d3d: SetPixelShader with unknown handle %08X", A(0));
        return 0;
    }
    g_state.ps = it->second;
    // the definition loads the combiner state like the XDK pushes it
    const uint32_t* d = it->second->def;
    auto put = [&](uint32_t method, uint32_t v) { g_state.simple[method >> 2] = v; };
    for (int i = 0; i < 8; i++) {
        put(gpu::kMPsAlphaInputs + i * 4, d[0 + i]);
        put(gpu::kMPsConstant0 + i * 4, d[10 + i]);
        put(gpu::kMPsConstant1 + i * 4, d[18 + i]);
        put(gpu::kMPsAlphaOutputs + i * 4, d[26 + i]);
        put(gpu::kMPsRgbInputs + i * 4, d[34 + i]);
        put(gpu::kMPsRgbOutputs + i * 4, d[45 + i]);
    }
    put(gpu::kMPsFinalAbcd, d[8]);
    put(gpu::kMPsFinalEfgh, d[9]);
    put(gpu::kMPsCompareMode, d[42]);
    put(gpu::kMPsFinalConstant0, d[43]);
    put(gpu::kMPsFinalConstant1, d[44]);
    put(gpu::kMPsCombinerCount, d[53]);
    put(gpu::kMPsDotMapping, d[55]);
    put(gpu::kMPsInputTexture, d[56]);
    SetComplex(gpu::kRsPsTextureModes, d[54]);
    return 0;
}

// D3DDevice_SetPixelShaderConstant(register, data, count)
HLE(00133CD0, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t reg = A(0), src = A(1), n = A(2);
    for (uint32_t i = 0; i < n && reg + i < 16; i++) std::memcpy(g_state.psc[reg + i].data(), HostPtr(src + i * 16), 16);
    gpu::ApplyPixelConstants(g_state, reg, n);
    return 0;
}

// drawing

// xboxrecomp debug: pressing F12 also logs the next frame's small undepth-tested draws from the title's side ([HUD]
// lines): their screen rectangle, texture, and the title's return addresses found on the stack, to find what draws
// each HUD element
namespace {
void HudTrace(uint32_t prim, uint32_t count, uint32_t verts, uint32_t stride) {
    if (!g_hud_trace.load() || count > 32 || g_state.rs[gpu::kRsZEnable]) return;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    if (verts && stride >= 8) {
        for (uint32_t v = 0; v < count; v++) {
            float xy[2];
            std::memcpy(xy, HostPtr(verts + v * stride), 8);
            x0 = std::min(x0, xy[0]), y0 = std::min(y0, xy[1]), x1 = std::max(x1, xy[0]), y1 = std::max(y1, xy[1]);
        }
    }
    fprintf(stderr, "[HUD] %04u prim %u count %u vs %08X tex %08X rect %.1f %.1f %.1f %.1f rets", g_hud_n++, prim, count,
            g_state.vs_handle, g_state.textures[0], x0, y0, x1, y1);
    int found = 0;
    for (uint32_t i = 0; i < 256 && found < 10; i++) {
        uint32_t v = R32(g_esp + 4 * i);
        if (v < 0x11005 || v >= 0x130620) continue;  // the title's own code
        if (R8(v - 5) == 0xE8 || R8(v - 2) == 0xFF || R8(v - 3) == 0xFF || R8(v - 6) == 0xFF) {
            fprintf(stderr, " %08X", v);
            found++;
        }
    }
    fprintf(stderr, "\n");
}

// the draw's vertices for HudTrace: stream 0 from start
uint32_t StreamVertices(uint32_t start) {
    const auto& st = g_state.streams[0];
    if (!st.vb || !st.stride) return 0;
    return mem::VirtualFromPhysical(R32(st.vb + 4)) + start * st.stride;
}
}  // namespace

// xboxrecomp debug (RECOMP_CAP_CALLERS=1): the title's return addresses above a draw, a call before each, for the frame
// capture's draw lines ("| from ..."): which of the title's code drew what
void NoteCallers() {
    static const bool on = std::getenv("RECOMP_CAP_CALLERS") && *std::getenv("RECOMP_CAP_CALLERS") != '0';
    if (!on) return;
    auto byte = [](uint32_t a) { return *static_cast<const uint8_t*>(HostPtr(a)); };
    size_t found = 0;
    g_state.callers.fill(0);
    for (uint32_t i = 0; i < 96 && found < g_state.callers.size(); i++) {
        uint32_t v = R32(g_esp + 4 * i);
        if (v < 0x11005 || v >= 0x130620) continue;
        if (byte(v - 5) == 0xE8 || byte(v - 2) == 0xFF || byte(v - 3) == 0xFF || byte(v - 6) == 0xFF)
            g_state.callers[found++] = v;
    }
}

// D3DDevice_DrawVertices(primitive, start, count)
HLE(00136100, 3) {
    NoteCallers();
    HudTrace(A(0), A(2), StreamVertices(A(1)), g_state.streams[0].stride);
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    SyncDeferred();
    if (g_backend) hpcos_draw_seq++, g_backend->Draw(g_state, A(0), A(1), A(2), nullptr);
    return 0;
}

// xboxrecomp debug: RECOMP_MORPH_TRACE=1 follows every indexed draw of 16+ indices made with vertex shader
// RECOMP_MORPH_VS (default 01181221, the characters' skinning shader), to find how the characters' mouths move: their
// bones barely turn and their vertices never change, so the shape has to come from which triangles a frame draws.
// Per vertex buffer it hashes the vertices once a frame (changed: the CPU reshaping them) and the index list of each
// draw; every 5 s it logs each buffer's frames, vertex changes and distinct index lists ([MORPH]), and each index list
// not seen before is written once to morph/<vb>_<hash>.bin beside the exe (header: magic, vb, index ptr, count, prim,
// stride, vertex count, texture, frame; then the indices, then the vertices up to the largest index). The first buffer
// rewritten most frames gets the hardware write watch (main.c) on its vertex 100, whose [WATCH] lines name the writer.
extern "C" void hpcos_WatchArm(uint32_t va);  // main.c
namespace {
void MorphTrace(uint32_t prim, uint32_t count, uint32_t ip, const uint8_t* indices) {
    static int on = -1;
    static uint32_t vs = 0x01181221;
    if (on < 0) {
        const char* e = std::getenv("RECOMP_MORPH_TRACE");
        const char* v = std::getenv("RECOMP_MORPH_VS");
        on = e && *e && *e != '0';
        if (v && *v) vs = uint32_t(std::strtoul(v, nullptr, 16));
        if (on) CreateDirectoryA("morph", nullptr);
    }
    if (!on || !indices || count < 16 || g_state.vs_handle != vs) return;
    const auto& st = g_state.streams[0];
    if (!st.vb || !st.stride) return;
    const uint16_t* ix = reinterpret_cast<const uint16_t*>(indices);
    uint32_t top = 0;
    uint64_t ih = 1469598103934665603ull;
    for (uint32_t i = 0; i < count; i++) top = std::max<uint32_t>(top, ix[i]), ih = (ih ^ ix[i]) * 1099511628211ull;
    uint32_t data = mem::VirtualFromPhysical(R32(st.vb + 4)) + g_state.base_vertex * st.stride;
    uint32_t bytes = (top + 1) * st.stride;
    uint64_t h = 1469598103934665603ull;
    const uint8_t* p = static_cast<const uint8_t*>(HostPtr(data));
    for (uint32_t i = 0; i < bytes; i++) h = (h ^ p[i]) * 1099511628211ull;

    struct Entry {
        uint64_t hash = 0;
        uint32_t frame = 0, obs = 0, changes = 0, stride = 0, verts = 0, tex = 0, draws = 0;
        std::unordered_map<uint64_t, uint32_t> lists;  // index list hash: draws
    };
    static std::unordered_map<uint32_t, Entry> seen;
    static std::unordered_map<uint64_t, bool> dumped;
    static bool armed;
    static ULONGLONG last_log;
    Entry& e = seen[data];
    uint32_t frame = g_swaps.load();
    if (e.frame != frame || !e.obs) {
        if (e.obs && e.hash != h) e.changes++;
        e.obs++;
        e.hash = h;
    }
    e.frame = frame, e.stride = st.stride, e.verts = std::max(e.verts, top + 1), e.tex = g_state.textures[0];
    e.draws++;
    e.lists[ih]++;
    uint64_t dk = ih ^ (uint64_t(data) << 20);
    if (!dumped[dk] && dumped.size() < 2000) {
        dumped[dk] = true;
        char name[64];
        snprintf(name, sizeof name, "morph/%08X_%016llX.bin", data, (unsigned long long)ih);
        if (FILE* f = fopen(name, "wb")) {
            uint32_t hdr[9] = {0x4850524Du, data, ip, count, prim, st.stride, top + 1, g_state.textures[0], frame};
            fwrite(hdr, sizeof hdr, 1, f);
            fwrite(ix, 2, count, f);
            fwrite(p, 1, bytes, f);
            fclose(f);
        }
    }
    if (!armed && e.obs >= 8 && e.changes * 2 >= e.obs) {
        armed = true;
        uint32_t va = data + std::min<uint32_t>(100, top) * st.stride;
        fprintf(stderr, "[MORPH] watching vb %08X vertex %u (%08X), %u verts stride %u tex %08X: changed %u of %u "
                        "frames\n", data, std::min<uint32_t>(100, top), va, top + 1, st.stride, e.tex, e.changes, e.obs);
        hpcos_WatchArm(va);
    }
    ULONGLONG now = GetTickCount64();
    if (now - last_log < 5000) return;
    last_log = now;
    for (const auto& [vb, v] : seen) {
        if (v.obs < 2) continue;
        fprintf(stderr, "[MORPH] vb %08X %u verts stride %u tex %08X: %u frames, vertices changed %u, %u draws, %zu "
                        "index lists:", vb, v.verts, v.stride, v.tex, v.obs, v.changes, v.draws, v.lists.size());
        int n = 0;
        for (const auto& [lh, c] : v.lists)
            if (n++ < 12) fprintf(stderr, " %016llX:%u", (unsigned long long)lh, c);
        fprintf(stderr, "\n");
    }
    seen.clear();
}
}  // namespace

// D3DDevice_DrawIndexedVertices(primitive, count, indices)
HLE(001361A0, 3) {
    NoteCallers();
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    SyncDeferred();
    const uint8_t* indices = A(2) ? static_cast<const uint8_t*>(HostPtr(A(2))) : nullptr;
    MorphTrace(A(0), A(1), A(2), indices);
    if (g_backend) hpcos_draw_seq++, g_backend->Draw(g_state, A(0), 0, A(1), indices);
    return 0;
}

// DrawVerticesUP(prim, count, vertices, stride): the vertices are copied to a ring the render thread reads, stream 0
// points there through a vertex buffer header for the draw. A half of the ring is written again once the draws
// that read it are done.
namespace {
constexpr uint32_t kUpRing = 4u << 20, kUpHalf = kUpRing / 2;
uint32_t g_up_ring = 0, g_up_pos = 0, g_up_header = 0, g_up_fence[2] = {};

uint32_t UpVertices(uint32_t src, uint32_t bytes) {
    if (!g_up_ring) {
        g_up_ring = mem::AllocContiguous(kUpRing, 0, 0xFFFFFFFF, 0x1000);
        g_up_header = mem::AllocSystem(20);
        std::memset(HostPtr(g_up_header), 0, 20);
        W32(g_up_header, 1);
    }
    bytes = std::min(bytes, kUpHalf);
    uint32_t room = (bytes + 63) & ~63u;
    uint32_t pos = g_up_pos + room > kUpRing ? 0 : g_up_pos;
    uint32_t from = g_up_pos ? (g_up_pos - 1) / kUpHalf : 1, to = (pos + room - 1) / kUpHalf;
    if (to != from) {
        g_up_fence[from] = g_backend->Fence();
        g_backend->WaitFence(g_up_fence[to]);
    }
    std::memcpy(HostPtr(g_up_ring + pos), HostPtr(src), bytes);
    g_up_pos = pos + room;
    W32(g_up_header + 4, mem::PhysicalFromVirtual(g_up_ring + pos));
    return g_up_header;
}
}  // namespace

// D3DDevice_DrawVerticesUP(primitive, count, vertices, stride)
HLE(00135E60, 4) {
    NoteCallers();
    HudTrace(A(0), A(1), A(2), A(3));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!g_backend || !A(1) || !A(3)) return 0;
    SyncDeferred();
    auto stream = g_state.streams[0];
    g_state.streams[0] = {UpVertices(A(2), A(1) * A(3)), A(3)};
    hpcos_draw_seq++;
    g_backend->Draw(g_state, A(0), 0, A(1), nullptr);
    g_state.streams[0] = stream;
    return 0;
}

// D3DDevice_DrawIndexedVerticesUP(primitive, count, indices, vertices, stride): the vertices up to the largest
// index go through the same ring
HLE(00135FC0, 5) {
    NoteCallers();
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t count = A(1), stride = A(4);
    if (!g_backend || !count || !stride || !A(2)) return 0;
    SyncDeferred();
    const uint16_t* idx = static_cast<const uint16_t*>(HostPtr(A(2)));
    uint32_t top = 0;
    for (uint32_t i = 0; i < count; i++) top = std::max<uint32_t>(top, idx[i]);
    auto stream = g_state.streams[0];
    g_state.streams[0] = {UpVertices(A(3), (top + 1) * stride), stride};
    hpcos_draw_seq++;
    g_backend->Draw(g_state, A(0), 0, count, reinterpret_cast<const uint8_t*>(idx));
    g_state.streams[0] = stream;
    return 0;
}

// immediate mode: not drawn, said once
HLE(001364A0, 1) {  // D3DDevice_Begin(primitive)
    LOG_FIRST(4, "d3d: Begin/End immediate mode is not supported (from %08X)", R32(g_esp));
    return 0;
}
HLE(001364E0, 0) { return 0; }  // D3DDevice_End
HLE(00136400, 3) { return 0; }  // D3DDevice_SetVertexData2f(register, a, b)
HLE(00136440, 5) { return 0; }  // D3DDevice_SetVertexData4f(register, a, b, c, d)
