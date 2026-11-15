// xboxrecomp: AMD FSR 3.1 frame generation through the FidelityFX API (fsr_fg.h). The API is C: the loader DLL is
// opened at run time and its five entry points taken by name, so nothing links against AMD's (MSVC-built) libraries.
#include "gpu/fsr_fg.h"

#include <wrl/client.h>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include "ffx_api.h"
#include "ffx_framegeneration.h"
#include "dx12/ffx_api_dx12.h"
#include "dx12/ffx_api_framegeneration_dx12.h"

#include "app/app.h"
#include "core/log.h"

using Microsoft::WRL::ComPtr;

namespace gpu::fsr {
namespace {

struct Api {
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxConfigure configure = nullptr;
    PfnFfxDispatch dispatch = nullptr;
    PfnFfxQuery query = nullptr;
} api;
// FSR 3.1.4's single DLL (SDK v1.1.4, the one Proton games ship) by default: the SDK 2.3 loader's frame generation drew
// only black under vkd3d-proton (2026-11-14). The older API knows no version descriptions and no PrepareV2
bool legacy = true;

ffxContext swap_ctx = nullptr;  // owns the swap chain
ffxContext fg_ctx = nullptr;    // the generation itself, made again when the back buffer's size changes
bool on = false;
uint32_t fg_w = 0, fg_h = 0;
uint64_t frame_id = 0;  // must go up by exactly one a frame: anything else restarts the generation
ComPtr<ID3D12Resource> depth, motion;  // blank: the game has neither in a form FSR takes
std::chrono::steady_clock::time_point last;

// RECOMP_FSR_DLL=<name>: another FidelityFX DLL (amd_fidelityfx_loader_dx12.dll: the SDK 2.3 one)
bool Load() {
    const char* e = std::getenv("RECOMP_FSR_DLL");
    const char* name = e && *e ? e : "amd_fidelityfx_dx12.dll";
    HMODULE m = LoadLibraryA(name);
    if (!m && !(e && *e)) m = LoadLibraryA(name = "amd_fidelityfx_loader_dx12.dll");
    if (!m) {
        LOGE("[FSR] %s is not beside the exe: frame generation off", name);
        return false;
    }
    legacy = std::strstr(name, "loader") == nullptr;
    LOGR("[FSR] %s (%s API)", name, legacy ? "FSR 3.1.4" : "FSR SDK 2");
    api.create = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(m, "ffxCreateContext"));
    api.destroy = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(m, "ffxDestroyContext"));
    api.configure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(m, "ffxConfigure"));
    api.dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(m, "ffxDispatch"));
    api.query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(m, "ffxQuery"));
    if (!api.create || !api.destroy || !api.configure || !api.dispatch) {
        LOGE("[FSR] amd_fidelityfx_loader_dx12.dll lacks the FidelityFX API: frame generation off");
        return false;
    }
    return true;
}

// RECOMP_FSR_DEBUG: 1 FSR's API checks, its warnings and errors in the log ([FSR] lines); 2-4 as 1 and something on
// screen (Frame: the debug view, only generated frames, reset markers)
int DebugLevel() {
    static int level = -1;
    if (level < 0) {
        const char* e = std::getenv("RECOMP_FSR_DEBUG");
        level = e ? std::atoi(e) : 0;
    }
    return level;
}

void Message(uint32_t type, const wchar_t* message) {
    static uint32_t told;
    if (told++ < 200) LOGI("[FSR] %s: %ls", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", message);
}

// FSR calls this when it wants the generated frame made
ffxReturnCode_t Generate(ffxDispatchDescFrameGeneration* params, void*) { return api.dispatch(&fg_ctx, &params->header); }

// a texture of zeros (D3D12 hands committed memory out cleared), left in the state FSR reads it in
ComPtr<ID3D12Resource> Blank(ID3D12Device* dev, DXGI_FORMAT format, uint32_t w, uint32_t h) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    ComPtr<ID3D12Resource> r;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                 IID_PPV_ARGS(&r));
    return r;
}

bool MakeContext(ID3D12Device* dev, uint32_t w, uint32_t h) {
    if (fg_ctx) api.destroy(&fg_ctx, nullptr), fg_ctx = nullptr;
    ffxCreateContextDescFrameGenerationVersion ver{};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    ver.version = FFX_FRAMEGENERATION_VERSION;
    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.header.pNext = legacy ? nullptr : &ver.header;
    backend.device = dev;
    ffxCreateContextDescFrameGeneration d{};
    d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    d.header.pNext = &backend.header;
    d.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;  // the blank depth (0) reads as far away
    if (DebugLevel()) d.flags |= FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING;
    d.displaySize = {w, h};
    d.maxRenderSize = {w, h};
    d.backBufferFormat = FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
    const ffxReturnCode_t r = api.create(&fg_ctx, &d.header, nullptr);
    if (r != FFX_API_RETURN_OK) {
        LOGE("[FSR] frame generation context refused (%u) at %ux%u: frame generation off", r, w, h);
        fg_ctx = nullptr;
        return false;
    }
    if (DebugLevel()) {
        ffxConfigureDescGlobalDebug1 dbg{};
        dbg.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1;
        dbg.fpMessage = Message;
        dbg.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_VERBOSE;
        const ffxReturnCode_t dr = api.configure(&fg_ctx, &dbg.header);
        LOGR("[FSR] debug messages %s (%u)", dr == FFX_API_RETURN_OK ? "on" : "refused", dr);
    }
    if (api.query) {  // which frame generation the DLL picked
        ffxQueryGetProviderVersion pv{};
        pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        if (api.query(&fg_ctx, &pv.header) == FFX_API_RETURN_OK && pv.versionName)
            LOGR("[FSR] frame generation provider: %s", pv.versionName);
    }
    depth = Blank(dev, DXGI_FORMAT_R32_FLOAT, w, h);
    motion = Blank(dev, DXGI_FORMAT_R16G16_FLOAT, w, h);
    fg_w = w, fg_h = h;
    frame_id = 0;
    LOGR("[FSR] frame generation ready at %ux%u (no motion vectors: optical flow only)", w, h);
    return depth && motion;
}

}  // namespace

bool On() { return on; }

bool CreateSwapChain(IDXGIFactory* factory, ID3D12CommandQueue* queue, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1& desc,
                     IDXGISwapChain4** out) {
    if (!app::Get().fsr_frame_generation || !Load()) return false;
    DXGI_SWAP_CHAIN_DESC1 sd = desc;
    ffxCreateContextDescFrameGenerationSwapChainVersionDX12 ver{};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
    ver.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
    ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 d{};
    d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
    d.header.pNext = legacy ? nullptr : &ver.header;
    d.swapchain = out;
    d.hwnd = hwnd;
    d.desc = &sd;
    d.fullscreenDesc = nullptr;
    d.dxgiFactory = factory;
    d.gameQueue = queue;
    const ffxReturnCode_t r = api.create(&swap_ctx, &d.header, nullptr);
    if (r != FFX_API_RETURN_OK || !*out) {
        LOGE("[FSR] frame generation swap chain refused (%u): the usual swap chain, no frame generation", r);
        swap_ctx = nullptr;
        return false;
    }
    on = true;
    LOGR("[FSR] FSR 3.1 frame generation swap chain made: a generated frame between each two of the game's");
    return true;
}

void Frame(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, IDXGISwapChain* swap, uint32_t w, uint32_t h) {
    if (!on) return;
    if ((!fg_ctx || w != fg_w || h != fg_h) && !MakeContext(dev, w, h)) {
        // the swap chain stays FSR's (it presents the game's frames as they are), only nothing is generated
        on = false;
        app::g_fsr_generating = false;
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const float dt = frame_id ? std::chrono::duration<float, std::milli>(now - last).count() : 1000.0f / 59.94f;
    last = now;

    ffxConfigureDescFrameGeneration c{};
    c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    c.swapChain = swap;
    c.frameGenerationCallback = Generate;
    c.frameGenerationEnabled = true;
    c.allowAsyncWorkloads = false;
    c.generationRect = {0, 0, int32_t(w), int32_t(h)};
    c.frameID = frame_id;
    // RECOMP_FSR_DEBUG 2: FSR's debug view; 3: only the generated frames shown (are they all black, or some?);
    // 4: FSR's reset markers and tear lines over the picture (does a black frame come with a reset?)
    if (DebugLevel() == 2) c.flags = FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW;
    if (DebugLevel() == 3) c.onlyPresentGenerated = true;
    if (DebugLevel() == 4)
        c.flags = FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_RESET_INDICATORS | FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES;
    ffxReturnCode_t r = api.configure(&fg_ctx, &c.header);

    ffxDispatchDescFrameGenerationPrepareV2 p{};
    p.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    p.frameID = frame_id;
    p.commandList = cl;
    p.renderSize = {w, h};
    p.motionVectorScale = {1.0f, 1.0f};
    p.frameTimeDelta = dt;
    p.reset = frame_id == 0;
    p.cameraNear = 0.1f;
    p.cameraFar = 1000.0f;
    p.cameraFovAngleVertical = 1.0472f;  // 60 degrees; only used with real depth
    p.viewSpaceToMetersFactor = 1.0f;
    p.depth = ffxApiGetResourceDX12(depth.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    p.motionVectors = ffxApiGetResourceDX12(motion.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    p.cameraUp[1] = 1.0f;
    p.cameraRight[0] = 1.0f;
    p.cameraForward[2] = 1.0f;
    if (r == FFX_API_RETURN_OK && !legacy) {
        r = api.dispatch(&fg_ctx, &p.header);
    } else if (r == FFX_API_RETURN_OK) {  // FSR 3.1.4: the same fields, less the camera's vectors
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        ffxDispatchDescFrameGenerationPrepare o{};
#pragma GCC diagnostic pop
        o.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
        o.frameID = p.frameID;
        o.flags = p.flags;
        o.commandList = p.commandList;
        o.renderSize = p.renderSize;
        o.jitterOffset = p.jitterOffset;
        o.motionVectorScale = p.motionVectorScale;
        o.frameTimeDelta = p.frameTimeDelta;
        o.cameraNear = p.cameraNear;
        o.cameraFar = p.cameraFar;
        o.cameraFovAngleVertical = p.cameraFovAngleVertical;
        o.viewSpaceToMetersFactor = p.viewSpaceToMetersFactor;
        o.depth = p.depth;
        o.motionVectors = p.motionVectors;
        r = api.dispatch(&fg_ctx, &o.header);
    }
    app::g_fsr_generating = r == FFX_API_RETURN_OK;
    static uint32_t told;
    if (r != FFX_API_RETURN_OK && told++ < 5) LOGE("[FSR] frame %llu: configure/prepare failed (%u)",
                                                    (unsigned long long)frame_id, r);
    frame_id++;
}

}  // namespace gpu::fsr
