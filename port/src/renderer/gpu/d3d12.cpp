// The title's Direct3D 8 calls drawn with Direct3D 12
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" {
#include "video/video_player.h"
}

#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include "app/app.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/pipeline_cache.h"
#include "gpu/shaders.h"
#include "gpu/smaa_shader.h"
#include "gpu/smaa_textures.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"

using Microsoft::WRL::ComPtr;

namespace gpu {

namespace {

// xboxrecomp debug: [SLOWGPU] timings
double MsSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

constexpr int kFrames = 3;
constexpr uint64_t kStaleFrames = 8;  // frames a target may go unused and still be read back

// xboxrecomp debug (RECOMP_PERF, read by hle_d3d8.cpp's [PERF] line through hpcos_GpuStats): GPU time of the game's
// drawing and of the shown frame's passes, frames timed, and the targets copied or resolved for reading (and samples)
std::atomic<uint64_t> g_gpu_scene_ns, g_gpu_post_ns, g_gpu_frames, g_feedback_copies, g_feedback_px;
// and the bytes the draws copied into the frame's upload ring, and the times a full ring made the frame wait for the GPU
std::atomic<uint64_t> g_upload_bytes, g_ring_waits;
// xboxrecomp debug: one frame's texture and pipeline work for hle_d3d8.cpp's [HITCH] line (hpcos_HitchGpuStats):
// textures made and the time making their resources, uploaded (decoded and copied up) with their bytes and time, pipelines from the library or the driver
// and their time, draws left out for a pipeline still being made
std::atomic<uint64_t> g_hitch_tex_new, g_hitch_texnew_ns, g_hitch_tex_up, g_hitch_tex_bytes, g_hitch_tex_ns, g_hitch_pso_n, g_hitch_pso_ns,
    g_hitch_pso_drops;
// read on first use, not at load: debug.env sets RECOMP_PERF after the static initialisers have run
struct PerfFlag {
    operator bool() const {
        static const bool on = [] {
            const char* e = std::getenv("RECOMP_PERF");
            return e && *e && *e != '0';
        }();
        return on;
    }
};
const PerfFlag g_perf_on;
// the frame's upload ring: every draw copies its vertices and indices in. 64 MB may fill within a frame on the Hogwarts
// grounds drawn whole, and a full ring waits for the GPU (Submit(true) in Draw); 3 frames x 192 MB of upload memory
constexpr uint64_t kUploadSize = 192ull << 20;
// a draw starting with less than this left in the frame's ring waits for the GPU and starts the ring over
constexpr uint64_t kUploadReserve = 8ull << 20;
constexpr uint32_t kSrvCount = 3 * 32768;
constexpr uint32_t kSamplerCount = 2048;
constexpr uint32_t kRtvCount = 1024;
constexpr uint32_t kDsvCount = 256;
// post pass views after the back buffers': bloom pair, exposure, ambient occlusion pair, SMAA's three, HDR10
// output's frame and panel
constexpr uint32_t kBloomRtv = 0;
constexpr uint32_t kExposureRtv = 2;
constexpr uint32_t kAoRtv = 3;
constexpr uint32_t kSmaaRtv = 5;
constexpr uint32_t kOutRtv = 8;
constexpr uint32_t kPostRtvs = 10;
constexpr uint32_t kConsoleShadows = 512;  // the shadow resolution setting that keeps the console's look
constexpr DXGI_FORMAT kBgra = DXGI_FORMAT_B8G8R8A8_UNORM;
// Shader model 3.0 HDR: the scene target holds colour over kHeadroom so lights add past 1 while alpha, which the
// game's blends use as a factor, keeps its 0..1 clamp; tone mapping brings it back when the frame is read
constexpr DXGI_FORMAT kHdrFormat = DXGI_FORMAT_R16G16B16A16_UNORM;
constexpr float kHeadroom = 8.0f;
// HDR10 output: back buffers in PQ over BT.2020; SDR white and peak in nits when Windows can't tell
constexpr DXGI_FORMAT kHdr10Format = DXGI_FORMAT_R10G10B10A2_UNORM;
constexpr float kDefaultPaperNits = 200.0f;
constexpr float kDefaultPeakNits = 1000.0f;
constexpr float kSdrWhiteUnit = 0.08f;  // Windows' SDR white level: 1000 is 80 nits
// longest a frame missing draws still on the build threads keeps the last whole frame on screen instead
// xboxrecomp: was 50 ms; a level's first frames on a new PC miss many pipelines, and past the limit they showed the
// world as its clear colour alone (blue) until the builds came back, so the loading picture stays up instead (as HP:PS)
constexpr auto kHoldMax = std::chrono::milliseconds(3000);

const char* kBlitHlsl = R"(
cbuffer B : register(b1) { float4 uvrect; float4 opts; float4 cb0; float4 cb1; float4 cb2; float4 grade; float4 sharp; };
Texture2D img : register(t0);
Texture2D lut : register(t1);
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uvrect.xy + t * uvrect.zw;
    return o;
}
float4 psmain(O i) : SV_Target {
    float4 c = 0;
    int n = max((int)opts.z, 1);
    float2 px = fwidth(i.uv);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) c += img.SampleLevel(ls, i.uv + px * ((float2(x, y) + 0.5) / n - 0.5), 0);
    c /= n * n;
    // xboxrecomp: contrast adaptive sharpening (after AMD's CAS): the four neighbours a source texel or an output
    // pixel away, whichever is wider, weighted by how much room the local contrast leaves; sharp.x the amount
    if (sharp.x > 0) {
        float w, h;
        img.GetDimensions(w, h);
        float2 d = max(px, 1 / float2(w, h));
        float3 a = img.SampleLevel(ls, i.uv - float2(0, d.y), 0).rgb, b = img.SampleLevel(ls, i.uv - float2(d.x, 0), 0).rgb;
        float3 e = img.SampleLevel(ls, i.uv + float2(d.x, 0), 0).rgb, f = img.SampleLevel(ls, i.uv + float2(0, d.y), 0).rgb;
        float3 mn = min(c.rgb, min(min(a, b), min(e, f))), mx = max(c.rgb, max(max(a, b), max(e, f)));
        float3 amp = sqrt(saturate(min(mn, 2 - mx) / max(mx, 1e-4)));
        float3 k = amp * -lerp(0.125, 0.2, sharp.x);
        c.rgb = saturate((c.rgb + (a + b + e + f) * k) / (1 + 4 * k));
    }
    // xboxrecomp: colour grading: grade.x saturation, grade.y vibrance (dull colours more), grade.z warmth (-1..1)
    if (grade.w > 0) {
        float l = dot(c.rgb, float3(0.2126, 0.7152, 0.0722));
        float chroma = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
        float s = grade.x * (1 + grade.y * (1 - saturate(chroma * 2)));
        c.rgb = saturate(lerp(l.xxx, c.rgb, s));
        c.rgb = saturate(c.rgb * (1 + grade.z * float3(0.08, 0, -0.08)));
    }
    // xboxrecomp: vignette (sharp.y) over the shown picture's corners, film grain (sharp.z) moving each frame (sharp.w)
    if (sharp.y > 0 || sharp.z > 0) {
        float2 t = (i.uv - uvrect.xy) / uvrect.zw - 0.5;
        t.x *= uvrect.z / uvrect.w * 1.333;
        c.rgb *= 1 - sharp.y * 0.6 * smoothstep(0.35, 0.95, length(t) * 1.25);
        float2 g = floor(i.p.xy) + frac(sharp.w * float2(0.618034, 0.414214)) * 1000;
        float noise = frac(sin(dot(g, float2(12.9898, 78.233))) * 43758.5453) - 0.5;
        c.rgb = saturate(c.rgb + noise * sharp.z * 0.12 * (1 - abs(dot(c.rgb, 1.0 / 3) * 2 - 1) * 0.5));
    }
    c.rgb *= opts.w;
    // xboxrecomp: a colour-blind correction (cb0..2 the rows of a linear RGB matrix, cb0.w on), before the ramp
    if (cb0.w > 0) {
        float3 l = pow(saturate(c.rgb), 2.2);
        l = float3(dot(cb0.xyz, l), dot(cb1.xyz, l), dot(cb2.xyz, l));
        c.rgb = pow(saturate(l), 1 / 2.2);
    }
    if (opts.x > 0) c.rgb = float3(lut.Sample(ps, float2(c.r, 0.5)).r, lut.Sample(ps, float2(c.g, 0.5)).g, lut.Sample(ps, float2(c.b, 0.5)).b);
    return float4(c.rgb, opts.y > 0 ? c.a : 1);
}
)";

// the HDR resolve: exposure, bloom of what passes white, tone mapping
const char* kPostHlsl = R"(
cbuffer P : register(b1) { float4 p0; float4 p1; };
Texture2D src : register(t0);
Texture2D bloom : register(t1);
Texture2D<float> expo : register(t2);
SamplerState ls : register(s0);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = t;
    return o;
}
static const float3 kLum = float3(0.2126, 0.7152, 0.0722);
// p0: headroom, key, lowest and highest exposure; the log average luminance over a grid
float exposure(O i) : SV_Target {
    float s = 0;
    for (int y = 0; y < 18; y++)
        for (int x = 0; x < 32; x++)
            s += log(max(dot(src.SampleLevel(ls, (float2(x, y) + 0.5) / float2(32, 18), 0).rgb * p0.x, kLum), 1e-3));
    return clamp(p0.y / exp(s / 576), p0.z, p0.w);
}
// p1: source texel; a quarter size copy of what passes 1 after exposure
float4 bright(O i) : SV_Target {
    float3 c = 0;
    for (int k = 0; k < 4; k++) c += src.SampleLevel(ls, i.uv + p1.xy * float2(k & 1 ? 1 : -1, k & 2 ? 1 : -1), 0).rgb;
    return float4(max(c * 0.25 * p0.x * expo.Load(int3(0, 0, 0)) - 1, 0), 1);
}
// xboxrecomp: the scene bloom's (no headroom: the console's colours end at white). p0.x the knee; what passes it,
// rising softly from it, at a quarter size
float4 lbright(O i) : SV_Target {
    float3 c = 0;
    for (int k = 0; k < 4; k++) c += src.SampleLevel(ls, i.uv + p1.xy * float2(k & 1 ? 1 : -1, k & 2 ? 1 : -1), 0).rgb;
    c *= 0.25;
    float l = max(c.r, max(c.g, c.b)), t = saturate((l - p0.x) / (1 - p0.x));
    return float4(c * t * t, 1);
}
// p1: step; 9 gaussian taps in 5 bilinear reads
float4 blur(O i) : SV_Target {
    float3 c = src.SampleLevel(ls, i.uv, 0).rgb * 0.227027;
    c += (src.SampleLevel(ls, i.uv + p1.xy * 1.384615, 0).rgb + src.SampleLevel(ls, i.uv - p1.xy * 1.384615, 0).rgb) * 0.316216;
    c += (src.SampleLevel(ls, i.uv + p1.xy * 3.230769, 0).rgb + src.SampleLevel(ls, i.uv - p1.xy * 3.230769, 0).rgb) * 0.070270;
    return float4(c, 1);
}
// p0: headroom, tone mapping, bloom strength; linear up to the knee, highlights roll off towards white
float4 tone(O i) : SV_Target {
    float4 s = src.Load(int3(i.p.xy, 0));
    float3 c = s.rgb * p0.x, b = bloom.SampleLevel(ls, i.uv, 0).rgb * p0.z;
    if (p0.y > 0) {
        const float a = 0.75;
        c = c * expo.Load(int3(0, 0, 0)) + b;
        c = min(c, a) + (1 - a) * (1 - exp(min(a - c, 0) / (1 - a)));
    } else {
        c += b;
    }
    return float4(saturate(c), s.a);
}
)";

// HDR10 output: the frame with the panel over it, its highlights stretched towards the display's peak
const char* kHdrHlsl = R"(
cbuffer P : register(b1) { float4 p0; float4 p1; };
Texture2D frame : register(t0);
Texture2D ui : register(t1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = t;
    return o;
}
static const float3 kLum = float3(0.2126, 0.7152, 0.0722);
static const float3x3 k709To2020 = {0.6274, 0.3293, 0.0433, 0.0691, 0.9195, 0.0114, 0.0164, 0.0880, 0.8956};
// gamma 2.2 as SDR monitors show it; sRGB's linear toe lifts the darks
float3 to_linear(float3 c) {
    return pow(c, 2.2);
}
// inverse Reinhard on luminance so hues keep: the brighter, the more it gains, white reaches most
float3 expand(float3 c, float most) {
    return c / (1 - dot(c, kLum) * (1 - 1 / most));
}
float3 pq(float3 nits) {
    float3 y = pow(saturate(nits / 10000), 0.1593017578125);
    return pow((0.8359375 + 18.8515625 * y) / (1 + 18.6875 * y), 78.84375);
}
// p0: SDR white nits, peak nits
float4 hdr10(O i) : SV_Target {
    int3 px = int3(i.p.xy, 0);
    float3 f = min(expand(to_linear(frame.Load(px).rgb), max(p0.y / p0.x, 1)) * p0.x, p0.y);
    float4 u = ui.Load(px);
    float3 panel = to_linear(u.a > 0 ? u.rgb / u.a : 0) * p0.x;
    return float4(pq(mul(k709To2020, lerp(f, panel, u.a))), 1);
}
)";

// the output showing monitor mon, false when no adapter has it
bool FindOutput(IDXGIFactory4* factory, HMONITOR mon, DXGI_OUTPUT_DESC1& desc) {
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; a++) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; o++) {
            ComPtr<IDXGIOutput6> output6;
            if (FAILED(output.As(&output6)) || FAILED(output6->GetDesc1(&desc))) continue;
            if (desc.Monitor == mon) return true;
        }
    }
    return false;
}

// Windows' SDR content brightness on monitor mon, in nits
float SdrWhiteNits(HMONITOR mon) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    UINT32 path_count = 0;
    UINT32 mode_count = 0;
    if (!GetMonitorInfoW(mon, &info)) return kDefaultPaperNits;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) {
        return kDefaultPaperNits;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    LONG queried = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(),
                                      nullptr);
    if (queried != ERROR_SUCCESS) return kDefaultPaperNits;
    for (UINT32 i = 0; i < path_count; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = paths[i].sourceInfo.adapterId;
        source.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;
        if (std::wcscmp(source.viewGdiDeviceName, info.szDevice) != 0) continue;
        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId = paths[i].targetInfo.adapterId;
        white.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&white.header) != ERROR_SUCCESS) break;
        return float(white.SDRWhiteLevel) * kSdrWhiteUnit;
    }
    return kDefaultPaperNits;
}

const char* kSmaaConfig = R"(
cbuffer P : register(b1) { float4 metrics; };
#define SMAA_RT_METRICS metrics
#define SMAA_CUSTOM_SL
#define SMAA_THRESHOLD 0.05
#define SMAA_MAX_SEARCH_STEPS 16
#define SMAA_MAX_SEARCH_STEPS_DIAG 8
#define SMAA_CORNER_ROUNDING 25
SamplerState LinearSampler : register(s0);
SamplerState PointSampler : register(s1);
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(LinearSampler, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(PointSampler, coord)
#define SMAASampleOffset(tex, coord, offset) tex.Sample(LinearSampler, coord, offset)
#define SMAAGather(tex, coord) tex.Gather(LinearSampler, coord, 0)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
)";

// after kSmaaHlsl: edges from the frame's luma, blend weights from edges, area and search, then the frame blended
const char* kSmaaPasses = R"(
Texture2D tex0 : register(t0);
Texture2D tex1 : register(t1);
Texture2D tex2 : register(t2);
struct SmaaEdgeV { float4 p : SV_Position; float2 uv : TEXCOORD0; float4 o[3] : TEXCOORD1; };
struct SmaaWeightV { float4 p : SV_Position; float2 uv : TEXCOORD0; float2 pix : TEXCOORD1; float4 o[3] : TEXCOORD2; };
struct SmaaBlendV { float4 p : SV_Position; float2 uv : TEXCOORD0; float4 o : TEXCOORD1; };
float4 corner(uint id, out float2 uv) {
    uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
SmaaEdgeV vs_edges(uint id : SV_VertexID) {
    SmaaEdgeV o;
    o.p = corner(id, o.uv);
    SMAAEdgeDetectionVS(o.uv, o.o);
    return o;
}
SmaaWeightV vs_weights(uint id : SV_VertexID) {
    SmaaWeightV o;
    o.p = corner(id, o.uv);
    SMAABlendingWeightCalculationVS(o.uv, o.pix, o.o);
    return o;
}
SmaaBlendV vs_blend(uint id : SV_VertexID) {
    SmaaBlendV o;
    o.p = corner(id, o.uv);
    SMAANeighborhoodBlendingVS(o.uv, o.o);
    return o;
}
float4 smaa_edges(SmaaEdgeV i) : SV_Target { return float4(SMAALumaEdgeDetectionPS(i.uv, i.o, tex0), 0, 0); }
float4 smaa_weights(SmaaWeightV i) : SV_Target { return SMAABlendingWeightCalculationPS(i.uv, i.pix, i.o, tex0, tex1, tex2, 0); }
float4 smaa_blend(SmaaBlendV i) : SV_Target { return SMAANeighborhoodBlendingPS(i.uv, i.o, tex0, tex1); }
)";

// from the scene's depth: occlusion pass, then a blur across and one down that multiplies it into the colour
// stencil bit 0 is the world, the sky is drawn where it is clear and counts as the far plane
const char* kAoHlsl = R"(
cbuffer A : register(b1) { float4 proj; float4 zp; float4 vp; float4 opt; float4 fog; };
// xboxrecomp (hppoa): NOSTENCIL, every pixel's depth as it is (this title marks nothing in the stencil; a sky drawn
// without depth leaves the clear, the far plane)
#ifdef MS
Texture2DMS<float> depth : register(t0);
Texture2DMS<uint2> stencil : register(t2);
#ifdef NOSTENCIL
float Raw(int2 p) { return depth.Load(p, 0); }
#else
float Raw(int2 p) { return stencil.Load(p, 0).g & 1 ? depth.Load(p, 0) : 1; }
#endif
#else
Texture2D<float> depth : register(t0);
Texture2D<uint2> stencil : register(t2);
#ifdef NOSTENCIL
float Raw(int2 p) { return depth.Load(int3(p, 0)); }
#else
float Raw(int2 p) { return stencil.Load(int3(p, 0)).g & 1 ? depth.Load(int3(p, 0)) : 1; }
#endif
#endif
Texture2D<float2> ao : register(t1);
Texture2D<float4> scene : register(t3);  // xboxrecomp: the frame's colour, for dof
float4 vs(uint id : SV_VertexID) : SV_Position { return float4(float2((id << 1) & 2, id & 2) * float2(2, -2) + float2(-1, 1), 0, 1); }
// view space position of a pixel. proj: 1 / scale xy, centre xy; zp: z = x / (depth - y); vp: viewport in pixels
float3 View(float2 px) {
    int2 p = clamp(int2(px), int2(vp.xy), int2(vp.xy + vp.zw) - 1);
    float z = zp.x / (Raw(p) - zp.y);
    float2 ndc = (p + 0.5 - vp.xy) / vp.zw * float2(2, -2) + float2(-1, 1);
    return float3((ndc - proj.zw) * proj.xy * z, z);
}
static const float kDither[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
// normal from the nearer neighbour each way: no bend over depth edges
float3 Normal(float2 px, float3 c) {
    float3 l = View(px - float2(1, 0)), r = View(px + float2(1, 0));
    float3 u = View(px - float2(0, 1)), d = View(px + float2(0, 1));
    float3 dx = abs(r.z - c.z) < abs(c.z - l.z) ? r - c : c - l;
    float3 dy = abs(d.z - c.z) < abs(c.z - u.z) ? d - c : c - u;
    return normalize(cross(dx, dy));
}
// opt: world radius, most pixels it may span; out: occlusion, view z
float2 occlusion(float4 pos : SV_Position) : SV_Target {
    float3 c = View(pos.xy);
    float2 rad = opt.x * 0.5 * vp.zw / (proj.xy * c.z);
    float span = max(rad.x, rad.y);
    if (span < 1) return float2(1, c.z);
    float3 n = Normal(pos.xy, c);
    float k = min(1, opt.y / span), R = opt.x * k;
    int2 cell = int2(pos.xy) & 3;
    float turn = kDither[cell.y * 4 + cell.x] * (6.2831853 / 16), a = 0;
    for (int i = 0; i < 16; i++) {
        float f = (i + 0.5) / 16, t = i * 2.3999632 + turn;
        float3 v = (View(pos.xy + float2(cos(t), sin(t)) * rad * (k * f)) - c) / R;
        float vv = dot(v, v), w = max(1 - vv, 0);
        a += w * w * w * max((dot(v, n) - 0.05) / (vv + 0.01), 0);
    }
    return float2(max(1 - a * 0.25, 0), c.z);
}
static const float kHalfPi = 1.5707963;
// GTAO (Jimenez 2016) at XeGTAO's high preset: 3 slices, 3 steps a side, squared step spread, falloff over the outer
// 61.5% of the radius, power 2.2. Per slice the highest horizon each side, the visible arc between integrated against
// the normal projected into the slice
float2 gtao(float4 pos : SV_Position) : SV_Target {
    float3 c = View(pos.xy);
    float2 rad = opt.x * 0.5 * vp.zw / (proj.xy * c.z);
    float span = max(rad.x, rad.y);
    if (span < 1) return float2(1, c.z);
    float3 n = Normal(pos.xy, c);
    float3 v = normalize(-c);
    float k = min(1, opt.y / span), R = opt.x * k;
    int2 cell = int2(pos.xy) & 3;
    float slice_noise = kDither[cell.y * 4 + cell.x] / 16;
    float step_noise = kDither[cell.x * 4 + cell.y] / 16;
    float vis = 0;
    for (int s = 0; s < 3; s++) {
        float phi = (s + slice_noise) * (kHalfPi * 2 / 3);
        float3 dir = float3(cos(phi), sin(phi), 0);
        // in pixels; screen y runs down, view y up
        float2 omega = float2(dir.x, -dir.y) * rad * k;
        float3 ortho = dir - dot(dir, v) * v;
        float3 axis = normalize(cross(ortho, v));
        float3 pn = n - axis * dot(n, axis);
        float pn_len = max(length(pn), 1e-4);
        float cos_n = saturate(dot(pn, v) / pn_len);
        float an = sign(dot(ortho, pn)) * acos(cos_n);
        // x: the +omega side, y: the -omega side; lowest is flat along the slice's tangent
        float2 low = float2(cos(an + kHalfPi), cos(an - kHalfPi));
        float2 hc = low;
        for (int j = 0; j < 3; j++) {
            float f = (j + step_noise) / 3;
            // at least 1.3 pixels out, off the centre pixel
            float2 off = omega * (f * f + 1.3 / length(omega));
            float3 d0 = View(pos.xy + off) - c;
            float3 d1 = View(pos.xy - off) - c;
            float2 len = max(float2(length(d0), length(d1)), 1e-4);
            float2 w = saturate((R - len) / (R * 0.615));
            float2 sc = float2(dot(d0, v), dot(d1, v)) / len;
            hc = max(hc, lerp(low, sc, w));
        }
        float h0 = an + clamp(-acos(hc.y) - an, -kHalfPi, kHalfPi);
        float h1 = an + clamp(acos(hc.x) - an, -kHalfPi, kHalfPi);
        float sin_n = sin(an);
        float arc0 = (cos_n + 2 * h0 * sin_n - cos(2 * h0 - an)) / 4;
        float arc1 = (cos_n + 2 * h1 * sin_n - cos(2 * h1 - an)) / 4;
        vis += pn_len * (arc0 + arc1);
    }
    return float2(pow(saturate(vis / 3), 2.2), c.z);
}
// opt.zw: step; 5 taps average the 4 pixel dither, depth edges left out
float Smooth(float2 pos, out float z) {
    int2 p = int2(pos), lo = int2(vp.xy), hi = int2(vp.xy + vp.zw) - 1;
    z = ao.Load(int3(p, 0)).y;
    float sum = 0, n = 0;
    for (int i = -2; i <= 2; i++) {
        float2 s = ao.Load(int3(clamp(p + i * int2(opt.zw), lo, hi), 0));
        float w = (abs(i) == 2 ? 0.5 : 1) * saturate(2 - abs(s.y - z) / (z * 0.02));
        sum += s.x * w;
        n += w;
    }
    return sum / n;
}
float2 occlusion_blur(float4 pos : SV_Position) : SV_Target {
    float z, a = Smooth(pos.xy, z);
    return float2(a, z);
}
// xboxrecomp (hppoa): cutscene depth of field. The focus is the nearest of 9 points round the middle (the figure
// being shown); what lies behind it blurs with its distance past it, up to opt.x pixels across. 16 taps over a
// disc, each counted only as far as its own blur reaches, so a sharp figure does not smear into the background
float Coc(float z, float focus) { return saturate((z - focus) / (focus * 1.5)); }
float4 dof(float4 pos : SV_Position) : SV_Target {
    float focus = 1e30;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) focus = min(focus, View(vp.xy + vp.zw * (0.5 + float2(x, y) * 0.12)).z);
    float4 c = scene.Load(int3(pos.xy, 0));
    float r = Coc(View(pos.xy).z, focus) * opt.x;
    if (r < 0.5) return c;
    float4 sum = c;
    float n = 1;
    for (int i = 0; i < 16; i++) {
        float f = sqrt((i + 0.5) / 16), t = i * 2.3999632;
        float2 o = float2(cos(t), sin(t)) * r * f;
        int2 q = clamp(int2(pos.xy + o), int2(vp.xy), int2(vp.xy + vp.zw) - 1);
        float w = saturate(Coc(View(q).z, focus) * opt.x / max(length(o), 1));
        sum += scene.Load(int3(q, 0)) * w;
        n += w;
    }
    return sum / n;
}
// fog: the game's, already blended over the scene, fades the occlusion out as it thickens
float4 occlusion_apply(float4 pos : SV_Position) : SV_Target {
    float z, a = Smooth(pos.xy, z);
    float fogged = fog.z * saturate(fog.x * z + fog.y);
    return float4(lerp(a, 1, fogged).xxx, 1);
}
)";

float Bits(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

void Color(uint32_t argb, float* o) {
    o[0] = ((argb >> 16) & 255) / 255.0f;
    o[1] = ((argb >> 8) & 255) / 255.0f;
    o[2] = (argb & 255) / 255.0f;
    o[3] = (argb >> 24) / 255.0f;
}

constexpr uint32_t kBlendDestColor = 0x306;  // D3DBLEND_DESTCOLOR

D3D12_BLEND Blend(uint32_t v, bool alpha) {
    switch (v) {
        case 0: return D3D12_BLEND_ZERO;
        case 1: return D3D12_BLEND_ONE;
        case 0x300: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
        case 0x301: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
        case 0x302: return D3D12_BLEND_SRC_ALPHA;
        case 0x303: return D3D12_BLEND_INV_SRC_ALPHA;
        case 0x304: return D3D12_BLEND_DEST_ALPHA;
        case 0x305: return D3D12_BLEND_INV_DEST_ALPHA;
        case kBlendDestColor: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
        case 0x307: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
        case 0x308: return D3D12_BLEND_SRC_ALPHA_SAT;
        case 0x8001: case 0x8003: return D3D12_BLEND_BLEND_FACTOR;
        case 0x8002: case 0x8004: return D3D12_BLEND_INV_BLEND_FACTOR;
        default: return D3D12_BLEND_ONE;
    }
}

D3D12_BLEND_OP BlendOp(uint32_t v) {
    switch (v) {
        case 0x800A: return D3D12_BLEND_OP_SUBTRACT;
        case 0x800B: return D3D12_BLEND_OP_REV_SUBTRACT;
        case 0x8007: return D3D12_BLEND_OP_MIN;
        case 0x8008: return D3D12_BLEND_OP_MAX;
        default: return D3D12_BLEND_OP_ADD;
    }
}

D3D12_COMPARISON_FUNC Cmp(uint32_t v) {
    uint32_t n = v >= 0x200 && v <= 0x207 ? v - 0x200 : 7;
    return D3D12_COMPARISON_FUNC(n + 1);
}

D3D12_STENCIL_OP StencilOp(uint32_t v) {
    switch (v) {
        case 0: return D3D12_STENCIL_OP_ZERO;
        case 0x1E01: return D3D12_STENCIL_OP_REPLACE;
        case 0x1E02: return D3D12_STENCIL_OP_INCR_SAT;
        case 0x1E03: return D3D12_STENCIL_OP_DECR_SAT;
        case 0x150A: return D3D12_STENCIL_OP_INVERT;
        case 0x8507: return D3D12_STENCIL_OP_INCR;
        case 0x8508: return D3D12_STENCIL_OP_DECR;
        default: return D3D12_STENCIL_OP_KEEP;
    }
}

D3D12_TEXTURE_ADDRESS_MODE Address(uint32_t v) {
    switch (v) {
        case 2: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        case 3: case 5: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        case 4: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        default: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    }
}

DXGI_FORMAT HostFormat(Host h) {
    switch (h) {
        case Host::BC1: return DXGI_FORMAT_BC1_UNORM;
        case Host::BC2: return DXGI_FORMAT_BC2_UNORM;
        case Host::BC3: return DXGI_FORMAT_BC3_UNORM;
        case Host::R32F: return DXGI_FORMAT_R32_FLOAT;
        default: return DXGI_FORMAT_B8G8R8A8_UNORM;
    }
}

constexpr uint32_t kFmtD16 = 0x2C;     // D3DFMT_D16
constexpr uint32_t kFmtLinD16 = 0x30;  // D3DFMT_LIN_D16

// TexInfo::shadow of a depth texture
uint8_t ShadowDepthKind(uint32_t format) { return format == kFmtD16 || format == kFmtLinD16 ? 2 : 1; }

// bytes per row of a 32 bit surface in guest memory, rounded to 64
uint32_t GuestPitch(uint32_t width) { return (width * 4 + 63) & ~63u; }

// rows of a texture upload start D3D12_TEXTURE_DATA_PITCH_ALIGNMENT apart
uint32_t UploadPitch(uint32_t row_bytes) { return (row_bytes + 255) & ~255u; }

D3D12_RESOURCE_DESC BufferDesc(uint64_t bytes) {
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = bytes;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return bd;
}

// one level, one sample: callers set Flags and SampleDesc where they differ
D3D12_RESOURCE_DESC Texture2DDesc(DXGI_FORMAT fmt, uint64_t w, uint32_t h) {
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w;
    td.Height = h;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    return td;
}

struct Target {
    ComPtr<ID3D12Resource> res;
    bool depth = false;
    uint32_t gw = 0;
    uint32_t gh = 0;
    uint32_t w = 0;
    uint32_t h = 0;
    uint32_t view = 0;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    uint32_t data = 0;
    uint32_t bytes = 0;
    uint64_t epoch = 0;  // last lock seen
    uint64_t sig = 0;    // guest memory as last matched
    bool drawn = false;           // the GPU wrote it since the last readback
    std::vector<uint8_t> read;    // what Sync left in guest memory
    // single sample copy draws read: of the target being drawn to, or a multisampled one resolved
    ComPtr<ID3D12Resource> fb;
    D3D12_RESOURCE_STATES fb_state = D3D12_RESOURCE_STATE_COPY_DEST;
    uint32_t samples = 1;         // above 1 on the back buffer sized targets under MSAA
    DXGI_FORMAT fmt = kBgra;
    bool hdr = false;              // colour held over kHeadroom until resolved
    uint64_t hdr_frame = ~0ull;    // the present count it last went HDR, once a frame
    uint64_t scene_frame = ~0ull;  // xboxrecomp: the present count it last had 3D drawn in it (SceneBloom)
    uint64_t bloom_frame = ~0ull;  // xboxrecomp: the present count its scene was last bloomed
};

// MemSig hashes a surface up to kSigWholeMax bytes whole, a bigger one as evenly spaced samples
constexpr uint32_t kSigWholeMax = 65536;
constexpr uint32_t kSigSamples = 64;
constexpr uint32_t kSigSampleBytes = 64;

uint64_t MemSig(uint32_t data, uint32_t total) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(data));
    uint64_t sig = Hash(&total, sizeof(total));
    if (total <= kSigWholeMax) return Hash(p, total, sig);
    for (uint32_t i = 0; i < kSigSamples; i++) {
        const uint8_t* sample = p + uint64_t(total - kSigSampleBytes) * i / (kSigSamples - 1);
        sig = Hash(sample, kSigSampleBytes, sig);
    }
    return sig;
}

// MemSig samples a big surface and misses small writes (XFONT text): after a Sync every byte is compared
bool Changed(const Target& t) {
    if (t.drawn || t.read.empty()) return false;
    uint32_t row = t.gw * 4;
    uint32_t pitch = GuestPitch(t.gw);
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        if (std::memcmp(p + size_t(y) * pitch, &t.read[size_t(y) * row], row)) return true;
    }
    return false;
}

// a lock alone proves nothing, reads lock too: only changed bytes mean the CPU wrote it
bool LatchCpuWrite(Target& t) {
    uint64_t e = DirtyEpoch(t.data);
    if (e <= t.epoch) return false;
    t.epoch = e;
    uint64_t sig = MemSig(t.data, t.bytes);
    if (sig == t.sig && !Changed(t)) return false;
    t.sig = sig;
    return true;
}

struct Tex {
    ComPtr<ID3D12Resource> res;
    TextureDesc d;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    uint32_t levels = 1;
    uint64_t sig = 0;
    uint64_t frame = 0;
    uint64_t palette = 0;  // hash of the colours a P8 texture was decoded with
    bool norm = false;  // a normalization cube
    bool prompt_pic = false;  // xboxrecomp: the HUD's A B X Y picture (PromptPicture), by its contents
};

struct PsoKey {
    uint64_t vs;
    uint64_t ps;
    uint32_t regs;
    uint32_t topo;
    uint32_t has_rt;
    uint32_t has_ds;
    uint32_t elements[16];  // per register: DXGI format, buffer << 8, offset << 16
    uint32_t blend;
    uint32_t src;
    uint32_t dst;
    uint32_t op;
    uint32_t wmask;
    uint32_t zen;
    uint32_t zwrite;
    uint32_t zfunc;
    uint32_t sten;
    uint32_t sfunc;
    uint32_t smask;
    uint32_t swmask;
    uint32_t sfail;
    uint32_t szfail;
    uint32_t spass;
    uint32_t cull;
    uint32_t ccw;
    uint32_t fill;
    int32_t zbias;
    float zslope;
    uint32_t rtfmt;
    // sample count, 0 without: the bytes the key's padding held, so pipelines cached before it still match
    uint32_t msaa;
};
static_assert(sizeof(PsoKey) == 184);

// xboxrecomp: a pipeline as pipelines_list.bin keeps it: the key, and the source checks its shaders are found by in
// the shader cache (FindShader), so a PC that never ran the game makes it before the title asks
struct PipelineRecord {
    PsoKey k;
    uint64_t vs_check;
    uint64_t ps_check;
};
static_assert(sizeof(PipelineRecord) == 200);

// Blit's settings past the source and its rectangle
struct BlitOptions {
    bool gamma = false;       // through the gamma lookup
    bool alpha = false;       // the source's alpha, else 1
    uint32_t taps = 1;        // n x n box filter per pixel
    float mul = 1;            // colour scale
    DXGI_FORMAT rtv = kBgra;  // the target's format
    bool over = false;        // premultiplied over the target, which keeps its alpha
    uint32_t samples = 1;     // the target's sample count
};

// a full screen pass's target
struct PostTarget {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    DXGI_FORMAT fmt;
    uint32_t w;
    uint32_t h;
    uint32_t samples = 1;
};

// what a full screen pass reads at t0 to t2
struct PostInputs {
    ID3D12Resource* res[3];
    DXGI_FORMAT fmt[3];
};

// DrawPostPass's settings past its target, inputs and constants
struct PostOptions {
    float blend = -1;  // blend factor over the target, below 0 the pass overwrites it
    const char* hlsl = kPostHlsl;
    const char* vs_entry = "vs";
};

// Shrink's settings past the source and the size
struct ShrinkOptions {
    uint32_t taps = 1;
    // into: a copy from an earlier call to draw over again, in state into_state
    ID3D12Resource* into = nullptr;
    D3D12_RESOURCE_STATES into_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    float mul = 1;
};

// a draw's four texture stages: BindStageTexturesAndTargets fills them, the constants, views and samplers read them
struct DrawTextures {
    TexInfo info[4];
    D3D12_SHADER_RESOURCE_VIEW_DESC sv[4];
    ID3D12Resource* res[4];
    bool rt[4] = {};     // stage samples a render target
    float wide[4] = {};  // host texels per console texel of a target read into a smaller one, 0 otherwise
};

// a draw's shaders: what they are made from, their cache keys and the hashes Register returned
struct DrawShaders {
    bool prog = false;  // a vertex program, else the fixed function pipeline
    bool quad = false;
    bool fixed_transform = false;  // fixed function on untransformed vertices: the shader reads matrices and lights
    FixedKey fk;
    uint64_t vkey = 0;
    uint64_t vh = 0;
    uint64_t pkey = 0;
    uint64_t ph = 0;
    bool wdepth = false;  // xboxrecomp: the pixel shader writes log2(1 + w) as depth (UseLogDepth)
    bool ppl = false;     // xboxrecomp: per pixel lighting, the program run again per pixel (shaders.cpp PerPixelFields)
};

// xboxrecomp: which draws write depth from w. HP:CoS draws most of the world W buffered (D3DZB_USEW) and other
// passes (the spell book's light beams, effects) with plain Z in the same frame and depth buffer. Our W draws store
// log2(1 + w) / 20 and Z draws z / w, two scales that do not compare: a Z pass passed against W depth only close to
// the camera. RECOMP_LOGDEPTH_ALL (default on) puts every 3D draw with depth on the log2(1 + w) scale;
// =0 keeps it to ZEnable == 2. Pretransformed (screen space) draws keep their z.
// xboxrecomp debug keys, read once a frame in Present: F8 flips RECOMP_LOGDEPTH_ALL, F9 turns the alpha test off,
// F10 passes every depth test
int g_dbg_logall = -1;
bool g_dbg_noatest = false;
bool g_dbg_zalways = false;

bool UseLogDepth(const State& s, const Assembled& a) {
    int& all = g_dbg_logall;
    if (all < 0) {
        const char* e = std::getenv("RECOMP_LOGDEPTH_ALL");
        all = !(e && *e == '0');
    }
    if (a.pretransformed) return false;
    return all ? s.rs[kRsZEnable] != 0 : WBuffered(s);
}

// the render target's console size in pixels, or the depth buffer's without one
struct GuestSize {
    uint32_t w;
    uint32_t h;
};

struct MouthHead;  // xboxrecomp: DrawMouthShrunk's table, defined with it

class D3D12Backend final : public Backend {
public:
    bool Init(uint32_t width, uint32_t height) override;
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override;
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) override;
    void Present(const Surface& front, bool pillarbox) override;
    void Sync(uint32_t data, uint32_t size) override;
    void SyncLate(uint32_t data) override;
    bool DeliverLate(uint32_t data) override;
    void RecordLate(uint32_t data) override;
    void Flush() override;
    void SetGamma(const uint8_t* ramp) override;
    void Occlusion(const State& s, const float p[kOcclusionParams]) override;
    void OcclusionOn(const Surface& rts, const Surface& zss, const Viewport& v, const float p[kOcclusionParams]);

private:
    struct Frame {
        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<ID3D12Resource> upload;
        uint8_t* map = nullptr;
        uint64_t used = 0;
        uint64_t fence = 0;
        std::vector<ComPtr<ID3D12Resource>> garbage;
        uint32_t srv = 0;
    };
    struct Alloc {
        ID3D12Resource* res;
        uint64_t off;
        uint8_t* cpu;
        D3D12_GPU_VIRTUAL_ADDRESS gpu;
    };

    Alloc Upload(uint64_t size, uint64_t align = 256);
    Frame& CurrentFrame() { return frames_[frame_ % kFrames]; }
    uint32_t AllocSrvSlots(uint32_t n);
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(ID3D12DescriptorHeap* h, uint32_t i, uint32_t inc) {
        auto c = h->GetCPUDescriptorHandleForHeapStart();
        c.ptr += size_t(i) * inc;
        return c;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(ID3D12DescriptorHeap* h, uint32_t i, uint32_t inc) {
        auto g = h->GetGPUDescriptorHandleForHeapStart();
        g.ptr += uint64_t(i) * inc;
        return g;
    }
    void Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES& cur, D3D12_RESOURCE_STATES want);
    void Begin();
    void Submit(bool wait);
    void WaitFence(uint64_t value, const char* why);
    [[noreturn]] void GpuLost(const char* where, HRESULT hr);
    using TimePoint = std::chrono::steady_clock::time_point;
    void LogPresentParts(TimePoint t0, TimePoint t1, TimePoint t2, TimePoint t3, TimePoint t4);
    double perf_parts_[4] = {};
    uint32_t perf_frames_ = 0;
    void SetFullViewport(uint32_t w, uint32_t h);
    void SetBlitTarget(D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t w, uint32_t h);
    void Blit(ID3D12Resource* src, DXGI_FORMAT fmt, const float uvrect[4], const BlitOptions& o = {});
    uint32_t EnsureBlitSamplers();
    void DrawFullScreen(ID3D12PipelineState* pso, D3D12_GPU_VIRTUAL_ADDRESS cb, uint32_t srv_slot,
                        std::optional<uint32_t> sampler_slot = {});
    ID3D12Resource* EnsureFeedbackCopy(Target& t, bool feedback = false);
    // xboxrecomp debug (RECOMP_PERF): copies by target since the last [PERF-COPY] line: data -> feedback, resolve
    struct CopyCount { uint32_t gw, gh, w, h, samples, feedback, resolve; };
    std::unordered_map<uint32_t, CopyCount> copy_counts_;
    std::chrono::steady_clock::time_point copy_t0_{};
    uint32_t copy_frames_ = 0;
    int feedback_stage_ = -1;  // the stage of the draw being set up that reads the target it draws to
    void LogCopies();
    // what a read of a target takes, and the state it is in
    struct Readable {
        ID3D12Resource* res;
        D3D12_RESOURCE_STATES* state;
    };
    Readable ReadableResource(Target& t);
    uint32_t Samples(uint32_t want);
    void EnterHdr(Target& t);
    bool DrawPostPass(const char* entry, const PostTarget& out, const PostInputs& in, const float p[8],
                      const PostOptions& o = {});
    ID3D12PipelineState* EnsurePostPipeline(const char* entry, const PostTarget& out, const PostOptions& o);
    D3D12_CPU_DESCRIPTOR_HANDLE PostRtv(uint32_t i);
    bool CreatePostTarget(ComPtr<ID3D12Resource>& r, D3D12_RESOURCE_STATES& st, DXGI_FORMAT f, uint32_t w, uint32_t h,
                          uint32_t view);
    // Resolve, then its steps in call order
    void Resolve(Target& t);
    bool EnsureBloomTargets(uint32_t bw, uint32_t bh);
    bool EnsureExposureTarget();
    float UpdateExposureBlend();
    void DrawBloom(const PostInputs& in, const Target& t, uint32_t bw, uint32_t bh);
    void SceneBloom(Target& t);
    bool BloomForScene(const State& s, const Assembled& a);
    // xboxrecomp (hppoa): the end of a frame's 3D scene: occlusion, cutscene depth of field, bloom
    void SceneEnd(Target& t);
    bool SceneCamera(float p[kOcclusionParams]) const;
    void SceneDof(Target& t);
    float scene_proj_[16] = {};          // the scene's last fixed function 3D draw: its projection,
    Viewport scene_vp_;                  // viewport,
    Surface scene_rt_, scene_zs_;        // target and depth,
    uint32_t scene_fog_[5] = {};         // fog: enable, table mode, start, end, density
    uint64_t scene_cam_frame_ = ~0ull;   // and the present count it was drawn in
    // Smaa, then its steps in call order, each callee right after its caller
    struct SmaaLookup {
        DXGI_FORMAT fmt;
        uint32_t w;
        uint32_t h;
        uint32_t bpp;  // bytes per texel
        const uint8_t* data;
    };
    ID3D12Resource* Smaa(ID3D12Resource* src, DXGI_FORMAT fmt, uint32_t w, uint32_t h);
    bool EnsureSmaaLookups();
    bool CreateLookupTexture(ComPtr<ID3D12Resource>& r, const SmaaLookup& l);
    bool EnsureSmaaTargets(DXGI_FORMAT fmt, uint32_t w, uint32_t h);
    bool DrawSmaaPasses(ID3D12Resource* src, DXGI_FORMAT fmt, uint32_t w, uint32_t h, const char* hlsl);
    // a pass's shader entry, target and what it reads
    // depth: the scene's, read by the first pass; src: the pass before; multiply: onto the scene
    struct AoPass {
        const char* entry;
        DXGI_FORMAT out;
        uint32_t samples = 1;
        bool multiply = false;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv;
        ID3D12Resource* depth = nullptr;
        ID3D12Resource* src = nullptr;
        ID3D12Resource* color = nullptr;  // xboxrecomp: kAoHlsl's scene (dof)
        float step_x = 0;
        float step_y = 0;
    };
    // what the passes share: kAoHlsl's constants, the scene rectangle and viewport, the depth's sample count
    struct AoFrame {
        float c[20];
        D3D12_RECT rect;
        D3D12_VIEWPORT vp;
        uint32_t depth_samples;
        bool gtao;
    };
    // Occlusion, in order, each callee right after its caller
    bool EnsureAoTargets(uint32_t w, uint32_t h);
    void DrawOcclusionPasses(Target& rt, Target& ds, const AoFrame& f);
    bool DrawAoPass(const AoFrame& f, const AoPass& pass);
    ID3D12PipelineState* EnsureAoPipeline(const AoPass& pass, bool ms);
    void CreateAoViews(uint32_t slot, const AoPass& pass, bool ms);
    // Draw, in call order, each callee right after its caller
    bool BindTargets(const State& s);
    bool EnterHdrForScene(const State& s, const Assembled& a);
    uint64_t EnsureVertexShader(const State& s, const Assembled& a, const DrawShaders& sh);
    bool BindStageTexturesAndTargets(const State& s, DrawTextures& stages);
    ID3D12Resource* BindTexture(const State& s, int stage, DrawTextures& stages);
    ID3D12Resource* BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, DrawTextures& stages);
    ID3D12Resource* BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                     DrawTextures& stages);
    bool CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key);
    void UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p, const uint32_t* palette,
                       bool same);
    void UploadTextureLevel(Tex& tx, uint32_t f, uint32_t l, const std::vector<uint8_t>& px, uint32_t pitch);
    uint64_t EnsurePixelShader(const PixelKey& pk, uint64_t pkey, const PerPixelSource* ppl = nullptr);
    bool PerPixel(const State& s, const Assembled& a, const DrawShaders& sh);
    std::unordered_map<uint64_t, bool> ppl_ok_;
    std::string prompt_sig_;  // xboxrecomp: the key names the keycap picture was drawn with  // xboxrecomp: per program and inputs, PerPixelLightable
    void BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k);
    void KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k);
    ID3D12PipelineState* EnsurePipeline(const PsoKey& k, bool wait = false);
    void TakeBuilt();
    void RecordPipelineKey(const PsoKey& k);
    void StartPrebuild();
    bool prebuild_started_ = false;
    ComPtr<ID3D12PipelineState> CreatePipeline(const PsoKey& k, uint64_t h, ID3DBlob* vs, ID3DBlob* ps, ID3DBlob* gs,
                                               bool compile, bool& pending);
    void FillPipelineDesc(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps, D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) const;
    void UploadConstants(const State& s, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages,
                         GuestSize size, DrawConstants& c);
    void FillHdrConstants(const PsoKey& k, DrawConstants& c);
    void FillTextureConstants(const State& s, const DrawTextures& stages, DrawConstants& c);
    void FillTexelConstants(int i, const TextureDesc& d, const DrawTextures& stages, DrawConstants& c);
    void CaptureDraw(const State& s, uint32_t prim, uint32_t count, const PsoKey& k, const DrawConstants& c,
                     const DrawTextures& stages);
    void CaptureTextures(const State& s, const DrawTextures& stages);
    void EndDrawCapture(const State& s, const Assembled& a, uint32_t count, bool prog, const DrawConstants& c);
    void CaptureInputs(const State& s, const Assembled& a, const DrawConstants& c);
    void CaptureShaders(const State& s, const Assembled& a, const DrawShaders& sh, const PixelKey& pk);
    void CaptureShader(const char* kind, uint64_t key, const std::string& src);
    void SetTextureViewsAndSamplers(const State& s, const Assembled& a, DrawTextures& stages);
    // xboxrecomp: the last draw's texture views, reused while a draw's are the same (SetTextureViewsAndSamplers)
    struct SrvTable {
        ID3D12Resource* res[4] = {};
        D3D12_SHADER_RESOURCE_VIEW_DESC sv[4] = {};
        uint64_t frame = 0;
        uint32_t slot = 0;
        bool valid = false;
    } last_srv_;
    uint32_t EnsureStageSamplers(const State& s, const DrawTextures& stages, bool clamp);
    void SetViewportScissor(const State& s, uint32_t gw);
    void SetBlendFactorStencilRef(const State& s);
    void DrawGeometry(const Assembled& a, int vb_slot = -1, const uint8_t* vb_src = nullptr);
    bool DrawFacePulled(const State& s, const Assembled& a, const DrawConstants& c);
    bool DrawMouthShrunk(const State& s, const Assembled& a);
    void NoteHead(const State& s, const Assembled& a);
    bool DrawEyesShrunk(const State& s, const Assembled& a);
    const MouthHead* eyes_of_ = nullptr;  // xboxrecomp: the head just drawn, for its eyeballs after it
    void UploadVertexBuffers(const Assembled& a, int vb_slot = -1, const uint8_t* vb_src = nullptr);
    // shaders by source hash: Register keeps the source, EnsureShaderCode finds the bytecode on disk or compiles it;
    // with compile false it only looks, and sets pending where the compiler is needed
    uint64_t Register(std::string src, const char* entry, const char* profile);
    ID3DBlob* EnsureShaderCode(uint64_t hash, bool compile, bool& pending);
    ID3DBlob* Compile(const std::string& src, const char* entry, const char* profile);
    ID3D12PipelineState* EnsureBlitPipeline(DXGI_FORMAT rtv, bool over, uint32_t samples);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC FullScreenPipelineDesc(ID3DBlob* vs, ID3DBlob* ps, DXGI_FORMAT rtv,
                                                              uint32_t samples) const;
    // Init, in order, each callee right after its caller
    bool CreateDevice();
    bool CreateSwapChain(HWND hwnd);
    void CreateHeaps();
    uint32_t CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE t, uint32_t n, bool visible, ComPtr<ID3D12DescriptorHeap>& out);
    void CreateBackBufferViews();
    bool CreateRootSignature();
    void CreateFrames();
    void CreateGammaLut();
    void LoadSettings();
    void LatchFeatureSettings(const app::Settings& set);
    // Present, in call order, each callee right after its caller; DrawFrame runs the ones after it to RenderUi
    void FinishCapture(const Surface& front);
    void StartCapture();
    void DetectHdr();
    void ResizeSwap();
    bool ResizeBackBuffers(uint32_t w, uint32_t h, DXGI_FORMAT fmt, DXGI_COLOR_SPACE_TYPE space);
    void UploadGammaRamp(uint32_t brightness, uint32_t contrast);
    void DrawFrame(Target* t, bool pillarbox, uint32_t bi);
    ID3D12Resource* PrepareShownFrame(Target* t);
    void DrawSdrFrame(Target* t, ID3D12Resource* r, bool pillarbox, D3D12_CPU_DESCRIPTOR_HANDLE bb);
    bool EnsureOutTargets();
    void DrawHdrFrame(Target* t, ID3D12Resource* r, bool pillarbox, D3D12_CPU_DESCRIPTOR_HANDLE bb);
    void BlitFrame(const Target& t, ID3D12Resource* r, bool pillarbox, DXGI_FORMAT rtv = kBgra);
    void RenderUi();
    void RenderOverlay();
    void RenderPanel();
    void ReleaseOldTextures();
    void ResetFrame();
    void CountFps();
    void ApplySettings();
    ComPtr<ID3D12Resource> Shrink(ID3D12Resource* src, D3D12_RESOURCE_STATES& st, DXGI_FORMAT fmt, uint32_t w,
                                  uint32_t h, const ShrinkOptions& o = {});
    bool ReadBack(ID3D12Resource* res, uint32_t w, uint32_t h, std::vector<uint8_t>& out);
    void CapSave(Target& t, const char* name);
    void CapStep(const Surface& rt);
    // a resource CapSave shrinks to w x h and writes, in state *state, colour times mul
    struct CapImage {
        ID3D12Resource* res;
        D3D12_RESOURCE_STATES* state;
        DXGI_FORMAT fmt;
        uint32_t w;
        uint32_t h;
        float mul = 1;
    };
    void CapSave(const CapImage& img, const char* name);
    // a surface's place in targets_: its key, console size and entry, end when it has none
    struct TargetSlot {
        uint32_t data;
        uint64_t key;
        uint32_t gw;
        uint32_t gh;
        std::unordered_map<uint64_t, Target>::iterator it;
        bool sized;  // the entry is there at the surface's size
    };
    Target* FindTarget(const Surface& s, bool depth);
    Target* EnsureTarget(const Surface& s, bool depth);
    // EnsureTarget's steps, in call order
    TargetSlot FindTargetSlot(const Surface& s, bool depth);
    void ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it);
    bool CreateTargetResource(Target& t, bool shadow, bool blur);
    bool CreateTargetView(Target& t);
    // UploadTarget, then its step
    void UploadTarget(Target& t, const Surface& s);
    ComPtr<ID3D12Resource> CreatePixelTexture(const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w, uint32_t h);
    // Clear's arguments past the state
    struct ClearArgs {
        uint32_t count;
        const uint8_t* rects;
        uint32_t flags;
        uint32_t color;
        float z;
        uint32_t stencil;
    };
    void ClearTarget(Target& t, const State& s, const ClearArgs& c, std::vector<D3D12_RECT>& rs);
    // Copy's guest lists: count src_rects, dst_points 0 or one per rect
    struct CopyList {
        uint32_t count;
        uint32_t src_rects;
        uint32_t dst_points;
    };
    void CopyByDraw(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state, Target& dst_t,
                    const CopyList& l);
    void CopyByRegion(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state, Target& dst_t,
                      const CopyList& l);
    // the target as a guest sized BGRA copy source, and the shrunk copy that holds it when it is not one already
    struct CopySource {
        ID3D12Resource* res;
        ComPtr<ID3D12Resource> down;
    };
    CopySource PrepareCopySource(Target& t, const ShrinkOptions& o);
    // RecordLate's steps, under late_lock_
    struct Late;
    void ResetLate(Late& l, const Target& t);
    ID3D12Resource* PrepareLateSource(Target& t, Late& l);
    void RecordLateCopy(Late& l, uint32_t slot, ID3D12Resource* src);

    ComPtr<IDXGIFactory4> factory_;
    ComPtr<ID3D12Device> dev_;
    ComPtr<ID3D12CommandQueue> queue_;
    // xboxrecomp debug (RECOMP_PERF): GPU timestamps a frame -- its start, the game's drawing done, the frame shown
    // (SMAA, HDR and the blit done) -- resolved into ts_buf_ per frame slot and read once that frame's fence passed
    ComPtr<ID3D12QueryHeap> ts_heap_;
    ComPtr<ID3D12Resource> ts_buf_;
    uint64_t* ts_map_ = nullptr;
    uint64_t ts_freq_ = 0;
    bool ts_open_[kFrames] = {};
    void GpuTimestamp(uint32_t k);
    ComPtr<IDXGISwapChain3> swap_;
    ComPtr<ID3D12Resource> bb_[kFrames];
    ComPtr<ID3D12Fence> fence_;
    HANDLE event_ = nullptr;
    uint64_t fence_value_ = 0;
    Frame frames_[kFrames];
    uint64_t frame_ = 0;
    ComPtr<ID3D12GraphicsCommandList> cl_;
    ComPtr<ID3D12DescriptorHeap> srv_heap_;
    ComPtr<ID3D12DescriptorHeap> smp_heap_;
    ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    ComPtr<ID3D12DescriptorHeap> dsv_heap_;
    ComPtr<ID3D12DescriptorHeap> ui_heap_;
    // xboxrecomp: the movie overlay (XMV frames from hle_video.c), drawn over the frame where the ImGui menu was
    ComPtr<ID3D12Resource> ov_tex_;
    D3D12_RESOURCE_STATES ov_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
    uint32_t ov_w_ = 0, ov_h_ = 0, ov_version_ = 0xFFFFFFFFu;
    bool ov_shown_ = false;
    float ov_rect_[4] = {};  // display pixels: x, y, w, h of fb_w_ x fb_h_
    uint32_t ov_fb_w_ = 640, ov_fb_h_ = 480;
    void OverlayLatch(const XboxPresentInfo& pi);
    uint32_t srv_inc_ = 0;
    uint32_t smp_inc_ = 0;
    uint32_t rtv_inc_ = 0;
    uint32_t dsv_inc_ = 0;
    std::vector<uint32_t> free_rtv_;
    std::vector<uint32_t> free_dsv_;
    ComPtr<ID3D12RootSignature> root_;
    struct Source {
        std::string text;
        std::string entry;
        std::string profile;
        ComPtr<ID3DBlob> code;
        uint64_t check = 0;  // xboxrecomp: the source's second hash, kept for pipelines_list.bin
        uint8_t state = 0;  // 0 source only, 1 compiling, 2 done (no code: it failed)
        // xboxrecomp: code from base_shaders.bin (built on another PC); the text stays for when the driver refuses it
        bool base = false;
        bool no_base = false;  // refused once: compiled here from now on
    };
    // a base shader the driver refused: compiled here from its source next time; true when hash was one
    bool DropBaseCode(uint64_t hash);
    std::vector<ComPtr<ID3DBlob>> retired_code_;  // refused base bytecode, kept: other threads may still read it
    std::unordered_map<uint64_t, Source> shaders_;  // build threads too, under shader_lock_
    std::mutex shader_lock_;
    std::condition_variable shader_done_;
    std::unordered_map<uint64_t, uint64_t> shader_src_;  // vertex and pixel program keys to source hash
    std::unordered_map<uint64_t, uint64_t> line_gs_;     // a vertex shader's LineGsHlsl, for line draws
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> psos_;
    // async shaders: a pipeline that needs a compiler is built on the build threads, its draws skipped until it is back
    bool async_ = false;
    bool skipped_frame_ = false;
    bool shown_ = false;
    std::chrono::steady_clock::time_point shown_t_;
    std::unordered_set<uint64_t> building_;
    std::vector<std::pair<uint64_t, ComPtr<ID3D12PipelineState>>> built_;
    std::mutex built_lock_;
    std::atomic<bool> built_ready_{false};
    std::unordered_map<uint64_t, uint32_t> samplers_;
    uint32_t smp_used_ = 0;
    std::unordered_map<uint64_t, Target> targets_;
    std::unordered_map<uint32_t, uint64_t> target_used_;  // target address to the frame it was last used
    bool TargetStale(uint32_t data) const;
    std::unordered_map<uint64_t, Tex> textures_;  // by data address, a P8 texture's with its palette's above
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> blit_psos_;
    // shader model 3.0 features, from the settings at each present
    bool sm3_ = false;
    bool hdr_ = false;
    bool tone_ = false;
    // HDR10 output: the window's display, the output asked and in use, the frame and panel targets it draws first
    HMONITOR hdr_mon_ = nullptr;
    bool hdr_display_ = false;
    bool hdr_want_ = false;
    bool hdr_out_ = false;
    float paper_nits_ = kDefaultPaperNits;
    float peak_nits_ = kDefaultPeakNits;
    ComPtr<ID3D12Resource> out_[2];
    D3D12_RESOURCE_STATES out_state_[2] = {};
    uint32_t out_w_ = 0;
    uint32_t out_h_ = 0;
    uint32_t msaa_ = 1;      // samples in use
    uint32_t msaa_set_ = 0;  // app::Settings::msaa they came from
    // resolve: quarter size bloom pair, the exposure pixel kept across frames
    ComPtr<ID3D12Resource> bloom_[2];
    ComPtr<ID3D12Resource> expo_;
    D3D12_RESOURCE_STATES bloom_state_[2] = {};
    D3D12_RESOURCE_STATES expo_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
    uint32_t post_rtv_ = 0;
    uint32_t bloom_w_ = 0;
    uint32_t bloom_h_ = 0;
    bool expo_fresh_ = true;
    // ambient occlusion: the raw pass and its first blur, each with view z beside it
    ComPtr<ID3D12Resource> ao_[2];
    D3D12_RESOURCE_STATES ao_state_[2] = {};
    uint32_t ao_w_ = 0;
    uint32_t ao_h_ = 0;
    // SMAA: edges, blend weights and the blended frame, at the frame's size; its two lookups
    ComPtr<ID3D12Resource> smaa_[3];
    ComPtr<ID3D12Resource> smaa_lut_[2];
    D3D12_RESOURCE_STATES smaa_state_[3] = {};
    uint32_t smaa_w_ = 0;
    uint32_t smaa_h_ = 0;
    DXGI_FORMAT smaa_fmt_ = DXGI_FORMAT_UNKNOWN;
    std::chrono::steady_clock::time_point expo_t_ = std::chrono::steady_clock::now();
    // one frame capture: every draw in a text file, each target as the frame leaves it, the textures it read
    FILE* cap_ = nullptr;
    std::filesystem::path cap_dir_;
    uint32_t cap_i_ = 0;
    uint32_t cap_rt_ = 0;
    bool cap_draws_ = false;  // xboxrecomp: RECOMP_CAP_DRAWS, the target before every draw
    std::unordered_set<uint32_t> cap_seen_;
    std::unordered_set<uint64_t> cap_sh_;
    struct CapTex {
        ComPtr<ID3D12Resource> res;
        DXGI_FORMAT fmt;
        uint32_t data;
        uint32_t w;
        uint32_t h;
    };
    std::vector<CapTex> cap_tex_;
    ComPtr<ID3D12Resource> lut_;
    D3D12_RESOURCE_STATES lut_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
    uint32_t ramp_set_ = ~0u;  // brightness << 8 | contrast the lookup holds
    // xboxrecomp: the title's own ramp (SetGammaRamp), red, green and blue, 256 entries each; the settings go on top
    uint8_t ramp_[768] = {};  // identity from Init
    bool ramp_dirty_ = false;
    bool gamma_ = false;
    Target* bound_rt_ = nullptr;
    Target* bound_ds_ = nullptr;
    Target* draw_rt_ = nullptr;  // the draw's targets, the bound ones go when a texture's copy is drawn
    Target* draw_ds_ = nullptr;
    uint32_t guest_w_ = 640;
    uint32_t guest_h_ = 480;
    uint32_t bb_w_ = 0;
    uint32_t bb_h_ = 0;
    float scale_ = 1.0f;
    uint32_t shadow_res_ = 0;  // app::Settings::shadow_resolution the targets were made for
    // surfaces locked without a wait: copies in flight, the newest finished one goes to guest memory
    struct Late {
        ComPtr<ID3D12Resource> rb[kFrames];
        ComPtr<ID3D12Resource> down;
        D3D12_RESOURCE_STATES down_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
        uint64_t fence[kFrames] = {};
        uint64_t frame[kFrames] = {};
        uint64_t delivered = 0;
        uint32_t gw = 0;
        uint32_t gh = 0;
        uint32_t row_pitch = 0;
        uint32_t next = 0;
    };
    std::unordered_map<uint32_t, Late> late_;
    std::mutex late_lock_;                      // DeliverLate runs on the title's thread
    std::atomic<uint64_t> presents_seen_{0};    // presents_ for DeliverLate
    Assembled asm_;  // kept between draws for its buffers
    bool tearing_ = false;
    uint64_t presents_ = 0;
    std::chrono::steady_clock::time_point fps_t_ = std::chrono::steady_clock::now();
    uint64_t fps_n_ = 0;
};

bool D3D12Backend::Init(uint32_t width, uint32_t height) {
    guest_w_ = width;
    guest_h_ = height;
    for (int i = 0; i < 768; i++) ramp_[i] = uint8_t(i);  // identity until the title sets its ramp
    HWND hwnd = static_cast<HWND>(app::Window());
    if (!CreateDevice()) return false;
    if (!CreateSwapChain(hwnd)) return false;
    CreateHeaps();
    CreateBackBufferViews();
    if (!CreateRootSignature()) return false;
    CreateFrames();
    CreateGammaLut();
    app::InitUi();
    ImGui_ImplDX12_Init(dev_.Get(), kFrames, DXGI_FORMAT_B8G8R8A8_UNORM, ui_heap_.Get(),
                        ui_heap_->GetCPUDescriptorHandleForHeapStart(), ui_heap_->GetGPUDescriptorHandleForHeapStart());
    LoadSettings();
    Begin();
    LOGI("d3d12: %ux%u guest, %ux%u window, render scale %.3f", width, height, bb_w_, bb_h_, scale_);
    return true;
}

bool D3D12Backend::CreateDevice() {
    if (GetEnvironmentVariableW(L"RECOMP_D3D12_DEBUG", nullptr, 0)) {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer();
    }
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_)))) return false;
    // dedicated GPU first on hybrid laptops
    ComPtr<IDXGIFactory6> f6;
    ComPtr<IDXGIAdapter1> adapter;
    if (SUCCEEDED(factory_.As(&f6))) {
        f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    }
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev_))) &&
        FAILED(D3D12CreateDevice((adapter = nullptr).Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev_)))) {
        LOGE("d3d12: no device");
        return false;
    }
    DXGI_ADAPTER_DESC1 ad{};
    if (adapter) adapter->GetDesc1(&ad);
    LOGR("D3D12 adapter: %ls", adapter ? ad.Description : L"default");
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    dev_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_));
    ComPtr<IDXGIFactory5> f5;
    BOOL tear = FALSE;
    if (SUCCEEDED(factory_.As(&f5))) f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tear, sizeof(tear));
    tearing_ = tear;
    return true;
}

bool D3D12Backend::CreateSwapChain(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    bb_w_ = std::max<LONG>(rc.right - rc.left, 1);
    bb_h_ = std::max<LONG>(rc.bottom - rc.top, 1);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = bb_w_;
    sd.Height = bb_h_;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kFrames;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    ComPtr<IDXGISwapChain1> sc1;
    HRESULT hr = factory_->CreateSwapChainForHwnd(queue_.Get(), hwnd, &sd, nullptr, nullptr, &sc1);
    if (FAILED(hr)) {
        LOGE("d3d12: no swap chain (hr %08lX, window %p %ldx%ld, tearing %d)", (unsigned long)hr, (void*)hwnd,
             (long)bb_w_, (long)bb_h_, (int)tearing_);
        return false;
    }
    factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    sc1.As(&swap_);
    return true;
}

uint32_t D3D12Backend::CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE t, uint32_t n, bool visible,
                                  ComPtr<ID3D12DescriptorHeap>& out) {
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = t;
    hd.NumDescriptors = n;
    hd.Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    dev_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&out));
    return dev_->GetDescriptorHandleIncrementSize(t);
}

void D3D12Backend::CreateBackBufferViews() {
    for (int i = 0; i < kFrames; i++) {
        swap_->GetBuffer(i, IID_PPV_ARGS(&bb_[i]));
        dev_->CreateRenderTargetView(bb_[i].Get(), nullptr, Cpu(rtv_heap_.Get(), kRtvCount + i, rtv_inc_));
    }
}

void D3D12Backend::CreateHeaps() {
    srv_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvCount, true, srv_heap_);
    smp_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerCount, true, smp_heap_);
    rtv_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvCount + kFrames + kPostRtvs, false, rtv_heap_);
    post_rtv_ = kRtvCount + kFrames;
    dsv_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvCount, false, dsv_heap_);
    CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, true, ui_heap_);
    for (uint32_t i = kRtvCount; i-- > 0;) free_rtv_.push_back(i);
    for (uint32_t i = kDsvCount; i-- > 0;) free_dsv_.push_back(i);
}

bool D3D12Backend::CreateRootSignature() {
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 4;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    ranges[1].NumDescriptors = 4;
    D3D12_ROOT_PARAMETER params[4] = {};

    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1;
    for (int i = 0; i < 2; i++) {
        params[2 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2 + i].DescriptorTable.NumDescriptorRanges = 1;
        params[2 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
        params[2 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 4;

    rsd.pParameters = params;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> rs;
    ComPtr<ID3DBlob> err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs, &err))) return false;
    dev_->CreateRootSignature(0, rs->GetBufferPointer(), rs->GetBufferSize(), IID_PPV_ARGS(&root_));
    cache::Open(dev_.Get(), Hash(rs->GetBufferPointer(), rs->GetBufferSize()));
    return true;
}

void D3D12Backend::CreateFrames() {
    for (auto& f : frames_) {
        dev_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.alloc));
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC bd = BufferDesc(kUploadSize);
        dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                      IID_PPV_ARGS(&f.upload));
        f.upload->Map(0, nullptr, reinterpret_cast<void**>(&f.map));
    }
    dev_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    dev_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames_[0].alloc.Get(), nullptr, IID_PPV_ARGS(&cl_));
}

// gamma ramp as a 256 x 1 lookup
void D3D12Backend::CreateGammaLut() {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_B8G8R8A8_UNORM, 256, 1);
    dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                  IID_PPV_ARGS(&lut_));
}

void D3D12Backend::LoadSettings() {
    scale_ = app::RenderScale(guest_h_);
    shadow_res_ = app::Get().shadow_resolution;
    const app::Settings& set = app::Get();
    sm3_ = set.shader_model >= 3;
    hdr_ = sm3_ && set.hdr_rendering;
    LatchFeatureSettings(set);
    msaa_set_ = set.msaa;
    msaa_ = Samples(msaa_set_);
}

// tone mapping and async shaders from the settings; sm3_ is set before
void D3D12Backend::LatchFeatureSettings(const app::Settings& set) {
    tone_ = sm3_ && set.tone_mapping;
    async_ = set.async_shaders;
}

void D3D12Backend::Begin() {
    ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get(), smp_heap_.Get()};
    cl_->SetDescriptorHeaps(2, heaps);
    cl_->SetGraphicsRootSignature(root_.Get());
    bound_rt_ = bound_ds_ = nullptr;
}

void D3D12Backend::Submit(bool wait) {
    cl_->Close();
    ID3D12CommandList* lists[] = {cl_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    queue_->Signal(fence_.Get(), ++fence_value_);
    Frame& f = CurrentFrame();
    f.fence = fence_value_;
    if (wait) {
        WaitFence(fence_value_, "a submit that waits");
        f.alloc->Reset();
        f.garbage.clear();
        f.used = 0;
        f.srv = 0;
        last_srv_.valid = false;
        cl_->Reset(f.alloc.Get(), nullptr);
        Begin();
    }
}

void D3D12Backend::Flush() { Submit(true); }

// xboxrecomp: no frame keeps the GPU this long; Windows resets it after 2 s, amdgpu after 10 s
constexpr DWORD kGpuHangMs = 15000;

const char* RemovedReason(HRESULT hr) {
    switch (hr) {
        case S_OK: return "none reported";
        case DXGI_ERROR_DEVICE_HUNG: return "DEVICE_HUNG";
        case DXGI_ERROR_DEVICE_REMOVED: return "DEVICE_REMOVED";
        case DXGI_ERROR_DEVICE_RESET: return "DEVICE_RESET";
        case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "DRIVER_INTERNAL_ERROR";
        case DXGI_ERROR_INVALID_CALL: return "INVALID_CALL";
        default: return "other";
    }
}

// xboxrecomp: a lost device ends the run with a message: Windows wakes the waits and fails every call after,
// Wine's vkd3d leaves a wait on the hung frame asleep for good
[[noreturn]] void D3D12Backend::GpuLost(const char* where, HRESULT hr) {
    HRESULT why = dev_->GetDeviceRemovedReason();
    LOGE("GPU lost in %s (%08lX): device removed reason %08lX %s, fence done %llu of %llu, frame %llu", where,
         (unsigned long)hr, (unsigned long)why, RemovedReason(why), (unsigned long long)fence_->GetCompletedValue(),
         (unsigned long long)fence_value_, (unsigned long long)frame_);
    std::fflush(stdout);
    std::fflush(stderr);
    wchar_t text[512];
    std::swprintf(text, 512,
                  L"The graphics card stopped responding (%hs, %hs).\n\nThe game has to close. Progress since the last "
                  L"save is lost. Details are in hpcos_run.log.",
                  where, RemovedReason(why));
    MessageBoxW(nullptr, text, L"Harry Potter and the Chamber of Secrets", MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
    // not ExitProcess: unloading the driver over a lost device can hang as well
    TerminateProcess(GetCurrentProcess(), 3);
    for (;;) Sleep(INFINITE);
}

void D3D12Backend::WaitFence(uint64_t value, const char* why) {
    auto t0 = std::chrono::steady_clock::now();
    uint64_t done = fence_->GetCompletedValue();
    struct Report {
        std::chrono::steady_clock::time_point t0;
        uint64_t value, done;
        const char* why;
        ~Report() { SlowGpu(MsSince(t0), "fence wait for %llu (GPU at %llu) for %s", (unsigned long long)value,
                            (unsigned long long)done, why); }
    } report{t0, value, done, why};
    HRESULT hr = fence_->SetEventOnCompletion(value, event_);
    if (FAILED(hr)) GpuLost("fence wait", hr);
    DWORD waited = 0;
    while (WaitForSingleObject(event_, 1000) == WAIT_TIMEOUT) {
        waited += 1000;
        if (HRESULT why = dev_->GetDeviceRemovedReason(); FAILED(why)) GpuLost("fence wait", why);
        if (waited >= kGpuHangMs) GpuLost("fence wait timeout", HRESULT_FROM_WIN32(WAIT_TIMEOUT));
    }
    // a removed device completes every fence at UINT64_MAX
    if (fence_->GetCompletedValue() == UINT64_MAX) GpuLost("fence wait", dev_->GetDeviceRemovedReason());
}

void D3D12Backend::SetFullViewport(uint32_t w, uint32_t h) {
    D3D12_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    D3D12_RECT scissor{0, 0, LONG(w), LONG(h)};
    cl_->RSSetViewports(1, &vp);
    cl_->RSSetScissorRects(1, &scissor);
}

// one w x h colour target for a Blit, the draw's bound targets forgotten
void D3D12Backend::SetBlitTarget(D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t w, uint32_t h) {
    cl_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    bound_rt_ = bound_ds_ = nullptr;
    SetFullViewport(w, h);
}

D3D12Backend::Alloc D3D12Backend::Upload(uint64_t size, uint64_t align) {
    Frame& f = CurrentFrame();
    uint64_t o = (f.used + align - 1) & ~(align - 1);
    if (g_perf_on) g_upload_bytes += size;
    if (o + size <= kUploadSize) {
        f.used = o + size;
        return {f.upload.Get(), o, f.map + o, f.upload->GetGPUVirtualAddress() + o};
    }
    // too big for the ring: a buffer of its own, released with the frame
    ComPtr<ID3D12Resource> r;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
    constexpr uint64_t kOwnAlign = 0x10000;  // a committed buffer's size granularity
    D3D12_RESOURCE_DESC bd = BufferDesc((size + kOwnAlign - 1) & ~(kOwnAlign - 1));
    dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                  IID_PPV_ARGS(&r));
    uint8_t* p = nullptr;
    r->Map(0, nullptr, reinterpret_cast<void**>(&p));
    f.garbage.push_back(r);
    return {r.Get(), 0, p, r->GetGPUVirtualAddress()};
}

uint32_t D3D12Backend::AllocSrvSlots(uint32_t n) {
    Frame& f = CurrentFrame();
    const uint32_t per = kSrvCount / kFrames;
    if (f.srv + n > per) {
        LOG_FIRST(4, "d3d12: descriptor ring wrapped within a frame");
        f.srv = 0;
        last_srv_.valid = false;
    }
    uint32_t i = uint32_t(frame_ % kFrames) * per + f.srv;
    f.srv += n;
    return i;
}

void D3D12Backend::Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES& cur, D3D12_RESOURCE_STATES want) {
    if (cur == want) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = cur;
    b.Transition.StateAfter = want;
    cl_->ResourceBarrier(1, &b);
    cur = want;
}

// a resized target gives back its view and its resources
void D3D12Backend::ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it) {
    if (bound_rt_ == &it->second || bound_ds_ == &it->second) bound_rt_ = bound_ds_ = nullptr;
    (it->second.depth ? free_dsv_ : free_rtv_).push_back(it->second.view);
    CurrentFrame().garbage.push_back(it->second.res);
    if (it->second.fb) CurrentFrame().garbage.push_back(it->second.fb);
    targets_.erase(it);
}

// the host texture of a new target, at render scale or the shadow resolution
// xboxrecomp: the render scale for an off-screen page bigger than the screen at most (RECOMP_PAGE_SCALE, default 2).
// HP:CoS draws each character into a 1024x1024 page (82D84000) reading the page as it draws, so every such draw copies
// it whole first: at the screen's 4.5x (4K) the page was 4608x4608 and 75 copies a frame beside Percy, Fred and George
// took 21 ms of GPU. At 2x (2048x2048, twice the console's detail) a copy is a fifth of that.
float PageScale() {
    static const float k = [] {
        const char* e = std::getenv("RECOMP_PAGE_SCALE");
        float v = e && *e ? float(std::atof(e)) : 2.0f;
        return v > 0.0f ? v : 2.0f;
    }();
    return k;
}

bool D3D12Backend::CreateTargetResource(Target& t, bool shadow, bool blur) {
    float sc = shadow && shadow_res_ ? float(shadow_res_) / t.gw : scale_;
    if (!shadow && (t.gw > guest_w_ || t.gh > guest_h_)) sc = std::min(sc, PageScale());
    t.w = std::max(1u, uint32_t(std::lround(t.gw * sc)));
    t.h = std::max(1u, uint32_t(std::lround(t.gh * sc)));
    if (shadow) LOGI("d3d12: shadow map %ux%u", t.w, t.h);
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_UNKNOWN, t.w, t.h);
    // the scene, its depth and the colour surfaces drawn with that depth; the game's own maps and chains stay as
    // they are
    bool screen = t.gw == guest_w_ && t.gh == guest_h_;
    t.samples = screen ? msaa_ : 1;
    td.SampleDesc.Count = t.samples;
    D3D12_CLEAR_VALUE cv{};
    if (t.depth) {
        td.Format = DXGI_FORMAT_R24G8_TYPELESS;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        cv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        cv.DepthStencil.Depth = 1;
        t.state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    } else {
        // the back buffer sized colour targets: the scene and the game's copies of it
        t.fmt = hdr_ && !blur && screen ? kHdrFormat : kBgra;
        td.Format = t.fmt;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        cv.Format = td.Format;
        t.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    return SUCCEEDED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, t.state, &cv,
                                                   IID_PPV_ARGS(&t.res)));
}

// a view from the free pool, cleared; false when the pool is empty
bool D3D12Backend::CreateTargetView(Target& t) {
    auto& pool = t.depth ? free_dsv_ : free_rtv_;
    if (pool.empty()) return false;
    t.view = pool.back();
    pool.pop_back();
    if (t.depth) {
        D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dv.ViewDimension = t.samples > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
        dev_->CreateDepthStencilView(t.res.Get(), &dv, Cpu(dsv_heap_.Get(), t.view, dsv_inc_));
        float one = 1;
        cl_->ClearDepthStencilView(Cpu(dsv_heap_.Get(), t.view, dsv_inc_),
                                   D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, one, 0, 0, nullptr);
    } else {
        dev_->CreateRenderTargetView(t.res.Get(), nullptr, Cpu(rtv_heap_.Get(), t.view, rtv_inc_));
        float black[4] = {0, 0, 0, 0};
        cl_->ClearRenderTargetView(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), black, 0, nullptr);
    }
    return true;
}

// targets_ key of a surface's colour or depth target
uint64_t TargetKey(uint32_t data, bool depth) { return uint64_t(data) << 1 | (depth ? 1 : 0); }

// the shadow map's depth surface, or with depth false its colour surface
bool IsShadowSurface(const Surface& s, bool depth) {
    const ShadowTargets& sh = g_shadow;
    return sh.blur_hi && s.data == (depth ? sh.map : sh.color);
}

D3D12Backend::TargetSlot D3D12Backend::FindTargetSlot(const Surface& s, bool depth) {
    TargetSlot slot;
    slot.data = mem::VirtualFromPhysical(s.data);
    slot.key = TargetKey(slot.data, depth);
    SurfaceSize(s, slot.gw, slot.gh);
    slot.it = targets_.find(slot.key);
    slot.sized = slot.it != targets_.end() && slot.it->second.gw == slot.gw && slot.it->second.gh == slot.gh;
    return slot;
}

// the surface's target at its size, null when there is none
Target* D3D12Backend::FindTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    // the shadow map's colour surface has no memory and colour writes off: depth alone is bound
    if (IsShadowSurface(s, depth) && !depth) return nullptr;
    TargetSlot slot = FindTargetSlot(s, depth);
    target_used_[s.data] = frame_;
    return slot.sized ? &slot.it->second : nullptr;
}

// the surface's target, made again at its size when that changed
Target* D3D12Backend::EnsureTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    // the shadow map's colour surface has no memory and colour writes off: depth alone is bound
    const ShadowTargets& sh = g_shadow;
    bool shadow = IsShadowSurface(s, depth);
    if (shadow && !depth) return nullptr;
    // the edge blur reads the scene: it follows the screen, unless the console's shadows are asked for
    bool blur = sh.blur_hi && !depth && shadow_res_ != kConsoleShadows && s.data >= sh.blur_lo && s.data < sh.blur_hi;
    TargetSlot slot = FindTargetSlot(s, depth);
    target_used_[s.data] = frame_;
    if (slot.sized) return &slot.it->second;
    if (slot.it != targets_.end()) ReleaseTarget(slot.it);
    Target t;
    t.depth = depth;
    t.gw = slot.gw;
    t.gh = slot.gh;
    if (!CreateTargetResource(t, shadow, blur)) return nullptr;
    // an empty pool drops the new resource with t
    if (!CreateTargetView(t)) return nullptr;
    t.data = slot.data;
    t.bytes = GuestPitch(slot.gw) * slot.gh;
    if (depth) t.bytes = slot.gw * slot.gh * 4;
    t.epoch = DirtyEpoch(slot.data);
    t.sig = MemSig(slot.data, t.bytes);
    auto& ref = targets_[slot.key] = std::move(t);
    return &ref;
}

// the sample count the adapter has for the scene's formats, the one asked for or the next under it
uint32_t D3D12Backend::Samples(uint32_t want) {
    uint32_t n = std::clamp(want, 1u, 8u);
    auto has = [&](DXGI_FORMAT f) {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q{f, n};
        return SUCCEEDED(dev_->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof(q))) &&
               q.NumQualityLevels;
    };
    while (n > 1 && !(has(kBgra) && has(kHdrFormat) && has(DXGI_FORMAT_D24_UNORM_S8_UINT))) n--;
    LOGI("d3d12: MSAA %ux (asked %u)", n, want);
    return n;
}

bool D3D12Backend::BindTargets(const State& s) {
    Target* rt = EnsureTarget(s.rt, false);
    Target* ds = s.rs[kRsZEnable] || s.rs[kRsStencilEnable] ? EnsureTarget(s.zs, true) : nullptr;
    if (rt && ds && (rt->w != ds->w || rt->h != ds->h || rt->samples != ds->samples)) ds = nullptr;
    if (!rt && !ds) return false;
    if (rt) {
        if (LatchCpuWrite(*rt)) UploadTarget(*rt, s.rt);
        Transition(rt->res.Get(), rt->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        rt->drawn = true;
    }
    if (ds) {
        Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        ds->drawn = true;
    }
    if (rt != bound_rt_ || ds != bound_ds_) {
        D3D12_CPU_DESCRIPTOR_HANDLE rh = rt ? Cpu(rtv_heap_.Get(), rt->view, rtv_inc_) : D3D12_CPU_DESCRIPTOR_HANDLE{};
        D3D12_CPU_DESCRIPTOR_HANDLE dh = ds ? Cpu(dsv_heap_.Get(), ds->view, dsv_inc_) : D3D12_CPU_DESCRIPTOR_HANDLE{};
        cl_->OMSetRenderTargets(rt ? 1 : 0, rt ? &rh : nullptr, FALSE, ds ? &dh : nullptr);
        bound_rt_ = rt;
        bound_ds_ = ds;
    }
    return true;
}

// the pixels the CPU changed since the last Sync, opaque, the rest 0; t.read takes them
std::vector<uint8_t> LatchCpuChanges(Target& t) {
    std::vector<uint8_t> px(t.read.size(), 0);
    uint32_t guest_pitch = GuestPitch(t.gw);
    const uint8_t* guest = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        for (uint32_t x = 0; x < t.gw; x++) {
            const uint8_t* c = guest + size_t(y) * guest_pitch + x * 4;
            size_t i = (size_t(y) * t.gw + x) * 4;
            if (!std::memcmp(c, &t.read[i], 4)) continue;
            std::memcpy(&t.read[i], c, 4);
            std::memcpy(&px[i], c, 3);
            px[i + 3] = 255;
        }
    }
    return px;
}

// BGRA pixels into a new w x h texture, left as a shader resource
ComPtr<ID3D12Resource> D3D12Backend::CreatePixelTexture(const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w,
                                                        uint32_t h) {
    ComPtr<ID3D12Resource> tex;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_B8G8R8A8_UNORM, w, h);
    dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                  IID_PPV_ARGS(&tex));
    uint32_t row = UploadPitch(w * 4);
    Alloc a = Upload(uint64_t(row) * h, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    for (uint32_t y = 0; y < h; y++) std::memcpy(a.cpu + size_t(y) * row, &px[size_t(y) * pitch], w * 4);
    D3D12_TEXTURE_COPY_LOCATION dst{tex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION src{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.PlacedFootprint.Offset = a.off;
    src.PlacedFootprint.Footprint = {DXGI_FORMAT_B8G8R8A8_UNORM, w, h, 1, row};
    cl_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COPY_DEST;
    Transition(tex.Get(), st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return tex;
}

// CPU pixels of a color surface into its host target, scaled
void D3D12Backend::UploadTarget(Target& t, const Surface& s) {
    TextureDesc d;
    if (!Describe(s.header, d) || Format(d.format).depth) return;
    std::vector<uint8_t> px;
    uint32_t pitch;
    // after a Sync only the pixels the CPU changed go over the picture, which stays at render scale
    bool over = !t.drawn && d.width == t.gw && d.height == t.gh && t.read.size() == size_t(t.gw) * t.gh * 4;
    if (over) {
        LOG_FIRST(4, "d3d12: CPU pixels drawn over target %08X", t.data);
        pitch = t.gw * 4;
        px = LatchCpuChanges(t);
    } else {
        DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, px, pitch);
    }
    ComPtr<ID3D12Resource> tex = CreatePixelTexture(px, pitch, d.width, d.height);
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    SetBlitTarget(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), t.w, t.h);
    float uv[4] = {0, 0, 1, 1};
    float mul = t.hdr ? 1 / kHeadroom : 1;
    Blit(tex.Get(), kBgra, uv, {.alpha = true, .mul = mul, .rtv = t.fmt, .over = over, .samples = t.samples});
    CurrentFrame().garbage.push_back(tex);
    t.epoch = DirtyEpoch(t.data);
    t.sig = MemSig(t.data, t.bytes);
}

// samplers_ key of the full screen passes' samplers, apart from the stage samplers' hashed keys
constexpr uint64_t kBlitSamplerKey = 0xB117;

// four clamp samplers: linear at slot 0, point at slots 1 to 3
uint32_t D3D12Backend::EnsureBlitSamplers() {
    uint64_t key = kBlitSamplerKey;
    auto it = samplers_.find(key);
    uint32_t si;
    if (it == samplers_.end()) {
        if (smp_used_ + 4 > kSamplerCount) {
            samplers_.clear();
            smp_used_ = 0;
        }
        si = smp_used_;
        smp_used_ += 4;
        for (int i = 0; i < 4; i++) {
            D3D12_SAMPLER_DESC sd{};
            sd.Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sd.MaxLOD = D3D12_FLOAT32_MAX;
            dev_->CreateSampler(&sd, Cpu(smp_heap_.Get(), si + i, smp_inc_));
        }
        samplers_[key] = si;
    } else {
        si = it->second;
    }
    return si;
}

// the full screen triangle: cb at b0 and b1, views at srv_slot; without a sampler slot EnsureBlitSamplers gives it
void D3D12Backend::DrawFullScreen(ID3D12PipelineState* pso, D3D12_GPU_VIRTUAL_ADDRESS cb, uint32_t srv_slot,
                                  std::optional<uint32_t> sampler_slot) {
    cl_->SetPipelineState(pso);
    cl_->SetGraphicsRootConstantBufferView(0, cb);
    cl_->SetGraphicsRootConstantBufferView(1, cb);
    cl_->SetGraphicsRootDescriptorTable(2, Gpu(srv_heap_.Get(), srv_slot, srv_inc_));
    // without a slot the passes take the blit samplers, looked up at this point as they always were
    uint32_t si = sampler_slot ? *sampler_slot : EnsureBlitSamplers();
    cl_->SetGraphicsRootDescriptorTable(3, Gpu(smp_heap_.Get(), si, smp_inc_));
    cl_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl_->DrawInstanced(3, 1, 0, 0);
}

// xboxrecomp: colour-blind correction as one linear RGB matrix (daltonizing): what a viewer with the deficiency loses,
// c - S c (S Machado, Oliveira and Fernandes' simulation at full severity), moved by E into channels they still tell
// apart and added back, scaled by the strength: M = I + k E (I - S). Rows into out[0..11], out[3] 1 when on.
void ColorBlindMatrix(const app::Settings& set, float out[12]) {
    for (int i = 0; i < 12; i++) out[i] = 0;
    if (!set.colorblind || set.colorblind > 3 || !set.colorblind_strength) return;
    static const float kSim[3][9] = {
        {0.152286f, 1.052583f, -0.204868f, 0.114503f, 0.786281f, 0.099216f, -0.003882f, -0.048116f, 1.051998f},
        {0.367322f, 0.860646f, -0.227968f, 0.280085f, 0.672501f, 0.047413f, -0.011820f, 0.042940f, 0.968881f},
        {1.255528f, -0.076749f, -0.178779f, -0.078411f, 0.930809f, 0.147602f, 0.004733f, 0.691367f, 0.303900f}};
    // red-green losses go to green and blue; a blue-yellow loss to red and green
    static const float kErrRG[9] = {0, 0, 0, 0.7f, 1, 0, 0.7f, 0, 1};
    static const float kErrBY[9] = {1, 0, 0.7f, 0, 1, 0.7f, 0, 0, 0};
    const float* S = kSim[set.colorblind - 1];
    const float* E = set.colorblind == app::kCbTritanopia ? kErrBY : kErrRG;
    float k = std::min(set.colorblind_strength, 100u) / 100.0f, d[9];
    for (int i = 0; i < 9; i++) d[i] = (i % 4 == 0 ? 1.0f : 0.0f) - S[i];  // I - S
    for (int r = 0; r < 3; r++)
        for (int col = 0; col < 3; col++) {
            float e = 0;
            for (int j = 0; j < 3; j++) e += E[r * 3 + j] * d[j * 3 + col];
            out[r * 4 + col] = (r == col ? 1.0f : 0.0f) + k * e;
        }
    out[3] = 1;
}

// xboxrecomp: the grading sliders off their neutral places
bool GradingOn(const app::Settings& s) { return s.saturation != 100 || s.vibrance != 0 || s.warmth != 50; }

void D3D12Backend::Blit(ID3D12Resource* src, DXGI_FORMAT fmt, const float uvrect[4], const BlitOptions& o) {
    uint32_t slot = AllocSrvSlots(2);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = fmt;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(src, &sv, Cpu(srv_heap_.Get(), slot, srv_inc_));
    sv.Format = kBgra;
    dev_->CreateShaderResourceView(lut_.Get(), &sv, Cpu(srv_heap_.Get(), slot + 1, srv_inc_));
    uint32_t si = EnsureBlitSamplers();
    Alloc cb = Upload(256, 256);
    float c[28] = {uvrect[0], uvrect[1], uvrect[2], uvrect[3],
                   o.gamma ? 1.0f : 0.0f, o.alpha ? 1.0f : 0.0f, float(o.taps), o.mul};
    // xboxrecomp: the colour-blind correction, sharpening and grading on the finished frame (the pass through the
    // gamma lookup)
    if (o.gamma) {
        const app::Settings& set = app::Get();
        ColorBlindMatrix(set, c + 8);
        if (GradingOn(set)) {
            c[20] = float(std::min(set.saturation, 200u)) / 100.0f;
            c[21] = float(std::min(set.vibrance, 100u)) / 100.0f;
            c[22] = (float(std::min(set.warmth, 100u)) - 50.0f) / 50.0f;
            c[23] = 1;
        }
        c[24] = float(std::min(set.sharpen, 100u)) / 100.0f;
        c[25] = float(std::min(set.vignette, 100u)) / 100.0f;
        c[26] = float(std::min(set.grain, 100u)) / 100.0f;
        c[27] = float(presents_ % 997);
    }
    std::memcpy(cb.cpu, c, sizeof(c));
    DrawFullScreen(EnsureBlitPipeline(o.rtv, o.over, o.samples), cb.gpu, slot, si);
}

constexpr uint64_t kSourceCheck = 0x5CDA0002C0FFEE11ull;  // seed of the second source hash the disk cache keeps


ComPtr<ID3DBlob> CompileHlsl(const std::string& src, const std::string& entry, const std::string& profile,
                             uint64_t hash) {
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> err;
    auto t0 = std::chrono::steady_clock::now();
    HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, entry.c_str(), profile.c_str(),
                            D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &err);
    SlowGpu(MsSince(t0), "D3DCompile %s %016llX (%zu bytes of HLSL)", profile.c_str(), (unsigned long long)hash,
            src.size());
    if (FAILED(hr)) {
        LOGE("d3d12: %s compile failed: %s", profile.c_str(),
             err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        LOGE("%s", src.c_str());
        return nullptr;
    }
    cache::AddShader(hash, Hash(src.data(), src.size(), kSourceCheck), code.Get());
    return code;
}

uint64_t D3D12Backend::Register(std::string src, const char* entry, const char* profile) {
    // one source can hold several entry points
    uint64_t profile_seed = Hash(profile, std::strlen(profile));
    uint64_t entry_seed = Hash(entry, std::strlen(entry), profile_seed);
    uint64_t hash = Hash(src.data(), src.size(), entry_seed);
    std::lock_guard lock(shader_lock_);
    auto [it, fresh] = shaders_.try_emplace(hash);
    if (fresh) {
        it->second.check = Hash(src.data(), src.size(), kSourceCheck);
        it->second.text = std::move(src);
        it->second.entry = entry;
        it->second.profile = profile;
    }
    return hash;
}

ID3DBlob* D3D12Backend::EnsureShaderCode(uint64_t hash, bool compile, bool& pending) {
    std::unique_lock lock(shader_lock_);
    Source& s = shaders_[hash];
    if (s.state == 0 && !s.no_base) {
        s.code = cache::FindShader(hash, Hash(s.text.data(), s.text.size(), kSourceCheck), &s.base);
        if (s.code) {
            s.state = 2;
            if (!s.base) s.text = {};
        }
    }
    while (s.state == 1) {
        if (!compile) {
            pending = true;
            return nullptr;
        }
        shader_done_.wait(lock);
    }
    if (s.state == 2) return s.code.Get();
    if (!compile) {
        pending = true;
        return nullptr;
    }
    s.state = 1;
    std::string text = std::move(s.text);
    lock.unlock();
    ComPtr<ID3DBlob> code = CompileHlsl(text, s.entry, s.profile, hash);
    lock.lock();
    s.code = std::move(code);
    s.state = 2;
    shader_done_.notify_all();
    return s.code.Get();
}

// xboxrecomp (from HP:PS): shipped bytecode the driver will not take goes, and this PC's compiler makes it, once
bool D3D12Backend::DropBaseCode(uint64_t hash) {
    std::lock_guard lock(shader_lock_);
    auto it = shaders_.find(hash);
    if (it == shaders_.end() || !it->second.base || it->second.state != 2) return false;
    Source& s = it->second;
    LOGW("shader cache: driver refused base shader %016llx (%s), compiling it here", (unsigned long long)hash,
         s.profile.c_str());
    retired_code_.push_back(std::move(s.code));
    s.base = false;
    s.no_base = true;
    s.state = 0;
    return true;
}

ID3DBlob* D3D12Backend::Compile(const std::string& src, const char* entry, const char* profile) {
    uint64_t hash = Register(src, entry, profile);
    bool pending = false;
    return EnsureShaderCode(hash, true, pending);
}

// a full screen pass's pipeline: solid, no culling, one target, writing all channels; callers set the blend
D3D12_GRAPHICS_PIPELINE_STATE_DESC D3D12Backend::FullScreenPipelineDesc(ID3DBlob* vs, ID3DBlob* ps, DXGI_FORMAT rtv,
                                                                        uint32_t samples) const {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = root_.Get();
    d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = rtv;
    d.SampleDesc.Count = samples;
    return d;
}

ID3D12PipelineState* D3D12Backend::EnsureBlitPipeline(DXGI_FORMAT rtv, bool over, uint32_t samples) {
    auto& pso = blit_psos_[uint64_t(rtv) | uint64_t(over) << 32 | uint64_t(samples) << 40];
    if (pso) return pso.Get();
    ID3DBlob* vs = Compile(kBlitHlsl, "vs", "vs_5_0");
    ID3DBlob* ps = Compile(kBlitHlsl, "psmain", "ps_5_0");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = FullScreenPipelineDesc(vs, ps, rtv, samples);
    if (over) {
        auto& blend = d.BlendState.RenderTarget[0];
        // premultiplied colour, the target keeps its alpha
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_ONE;
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
        blend.DestBlendAlpha = D3D12_BLEND_ONE;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
    return pso.Get();
}

// DrawPostPass's pipeline for the pass, kept in blit_psos_; null when it can't be made
ID3D12PipelineState* D3D12Backend::EnsurePostPipeline(const char* entry, const PostTarget& out,
                                                      const PostOptions& o) {
    uint64_t seed = uint64_t(out.fmt) << 1 | (o.blend >= 0 ? 1 : 0) | uint64_t(out.samples) << 40;
    auto& pso = blit_psos_[Hash(entry, std::strlen(entry), seed)];
    if (pso) return pso.Get();
    ID3DBlob* vs = Compile(o.hlsl, o.vs_entry, "vs_5_0");
    ID3DBlob* ps = Compile(o.hlsl, entry, "ps_5_0");
    if (!vs || !ps) return nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = FullScreenPipelineDesc(vs, ps, out.fmt, out.samples);
    if (o.blend >= 0) {
        auto& b = d.BlendState.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = b.SrcBlendAlpha = D3D12_BLEND_BLEND_FACTOR;
        b.DestBlend = b.DestBlendAlpha = D3D12_BLEND_INV_BLEND_FACTOR;
        b.BlendOp = b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    if (FAILED(dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) return nullptr;
    return pso.Get();
}

bool D3D12Backend::DrawPostPass(const char* entry, const PostTarget& out, const PostInputs& in, const float p[8],
                                const PostOptions& o) {
    ID3D12PipelineState* pso = EnsurePostPipeline(entry, out, o);
    if (!pso) return false;
    uint32_t slot = AllocSrvSlots(4);
    for (int i = 0; i < 4; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = i < 3 && in.res[i] ? in.fmt[i] : kBgra;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(i < 3 ? in.res[i] : nullptr, &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
    Alloc cb = Upload(256, 256);
    std::memcpy(cb.cpu, p, 8 * sizeof(float));
    cl_->OMSetRenderTargets(1, &out.rtv, FALSE, nullptr);
    SetFullViewport(out.w, out.h);
    if (o.blend >= 0) {
        float f[4] = {o.blend, o.blend, o.blend, o.blend};
        cl_->OMSetBlendFactor(f);
    }
    DrawFullScreen(pso, cb.gpu, slot);
    bound_rt_ = bound_ds_ = nullptr;
    return true;
}

// a single sample copy of a colour target to read: while drawing to it, or resolved when it is multisampled
ID3D12Resource* D3D12Backend::EnsureFeedbackCopy(Target& t, bool feedback) {
    bool ms = t.samples > 1;
    if (!t.fb) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td = t.res->GetDesc();
        td.Flags = D3D12_RESOURCE_FLAG_NONE;
        td.SampleDesc.Count = 1;
        td.Alignment = 0;
        t.fb_state = D3D12_RESOURCE_STATE_COPY_DEST;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, t.fb_state, nullptr,
                                                 IID_PPV_ARGS(&t.fb)))) {
            return nullptr;
        }
    } else if (ms && t.state == D3D12_RESOURCE_STATE_RESOLVE_SOURCE) {
        // every write takes the target out of this state: the last resolve still holds
        Transition(t.fb.Get(), t.fb_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        return t.fb.Get();
    }
    Transition(t.res.Get(), t.state, ms ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE : D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(t.fb.Get(), t.fb_state, ms ? D3D12_RESOURCE_STATE_RESOLVE_DEST : D3D12_RESOURCE_STATE_COPY_DEST);
    if (ms) cl_->ResolveSubresource(t.fb.Get(), 0, t.res.Get(), 0, t.fmt);
    else cl_->CopyResource(t.fb.Get(), t.res.Get());
    g_feedback_copies++;  // xboxrecomp debug (RECOMP_PERF)
    g_feedback_px += uint64_t(t.w) * t.h * std::max<uint32_t>(t.samples, 1);
    if (g_perf_on) {
        CopyCount& c = copy_counts_[t.data];
        c.gw = t.gw, c.gh = t.gh, c.w = t.w, c.h = t.h, c.samples = t.samples;
        (feedback ? c.feedback : c.resolve)++;
    }
    Transition(t.fb.Get(), t.fb_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return t.fb.Get();
}

// what a read of the target takes, and its state: the target itself, or a multisampled one's resolved copy
D3D12Backend::Readable D3D12Backend::ReadableResource(Target& t) {
    D3D12_RESOURCE_STATES* state = t.samples > 1 ? &t.fb_state : &t.state;
    if (t.samples == 1) return {t.res.Get(), state};
    if (t.depth) LOG_FIRST(4, "d3d12: multisampled depth %08X read, left out", t.data);
    return {t.depth ? nullptr : EnsureFeedbackCopy(t), state};
}

// the first 3D draw of a frame: what the target holds goes over the headroom
void D3D12Backend::EnterHdr(Target& t) {
    t.hdr_frame = presents_;
    ID3D12Resource* fb = EnsureFeedbackCopy(t);
    if (!fb) return;
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    SetBlitTarget(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), t.w, t.h);
    float uv[4] = {0, 0, 1, 1};
    Blit(fb, t.fmt, uv, {.alpha = true, .mul = 1 / kHeadroom, .rtv = t.fmt, .samples = t.samples});
    t.hdr = true;
}

// HDR resolve: bloom at a quarter of the target, exposure adapting over about a second
constexpr uint32_t kBloomShrink = 4;
constexpr DXGI_FORMAT kBloomFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT kExposureFormat = DXGI_FORMAT_R16_FLOAT;
constexpr float kExposureRate = 1.5f;     // per second
constexpr float kExposureMaxStep = 0.25f;  // longest frame time one step follows, in seconds
constexpr float kExposureKey = 0.4f;       // the average luminance exposure aims for
constexpr float kLowestExposure = 0.7f;    // under tone mapping; without it exposure stays at the highest
constexpr float kHighestExposure = 1.0f;
constexpr float kBloomStrength = 0.6f;

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Backend::PostRtv(uint32_t i) {
    return Cpu(rtv_heap_.Get(), post_rtv_ + i, rtv_inc_);
}

// a post pass target in the render target state with its view at PostRtv(view); the one it replaces goes to garbage
bool D3D12Backend::CreatePostTarget(ComPtr<ID3D12Resource>& r, D3D12_RESOURCE_STATES& st, DXGI_FORMAT f, uint32_t w,
                                    uint32_t h, uint32_t view) {
    if (r) CurrentFrame().garbage.push_back(r);
    r.Reset();
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(f, w, h);
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    st = D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, st, nullptr, IID_PPV_ARGS(&r)))) {
        return false;
    }
    dev_->CreateRenderTargetView(r.Get(), nullptr, PostRtv(view));
    return true;
}

bool D3D12Backend::EnsureBloomTargets(uint32_t bw, uint32_t bh) {
    if (bloom_[0] && bloom_w_ == bw && bloom_h_ == bh) return true;
    bloom_w_ = bw;
    bloom_h_ = bh;
    for (int i = 0; i < 2; i++) {
        if (!CreatePostTarget(bloom_[i], bloom_state_[i], kBloomFormat, bw, bh, kBloomRtv + i)) return false;
    }
    return true;
}

bool D3D12Backend::EnsureExposureTarget() {
    if (expo_) return true;
    if (!CreatePostTarget(expo_, expo_state_, kExposureFormat, 1, 1, kExposureRtv)) return false;
    // new memory can hold NaN, which the blend would keep forever
    float one[4] = {1, 1, 1, 1};
    cl_->ClearRenderTargetView(PostRtv(kExposureRtv), one, 0, nullptr);
    expo_fresh_ = true;
    return true;
}

// how far this frame's exposure moves towards the scene's, as a blend factor
float D3D12Backend::UpdateExposureBlend() {
    auto now = std::chrono::steady_clock::now();
    float dt = std::clamp(std::chrono::duration<float>(now - expo_t_).count(), 0.0f, kExposureMaxStep);
    expo_t_ = now;
    float a = expo_fresh_ ? 1.0f : 1.0f - std::exp(-dt * kExposureRate);
    expo_fresh_ = false;
    return a;
}

// bright parts of in's source into bloom_[0], blurred across then down, left readable
void D3D12Backend::DrawBloom(const PostInputs& in, const Target& t, uint32_t bw, uint32_t bh) {
    Transition(bloom_[0].Get(), bloom_state_[0], D3D12_RESOURCE_STATE_RENDER_TARGET);
    float pb[8] = {kHeadroom, 0, 0, 0, 1.0f / t.w, 1.0f / t.h};
    DrawPostPass("bright", {PostRtv(kBloomRtv), kBloomFormat, bw, bh}, in, pb);
    for (int pass = 0; pass < 2; pass++) {
        int from = pass;
        int to = 1 - pass;
        Transition(bloom_[from].Get(), bloom_state_[from], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(bloom_[to].Get(), bloom_state_[to], D3D12_RESOURCE_STATE_RENDER_TARGET);
        PostInputs b{{bloom_[from].Get(), nullptr, nullptr}, {kBloomFormat, kBloomFormat, kBloomFormat}};
        float pp[8] = {0, 0, 0, 0, pass ? 0 : 1.0f / bw, pass ? 1.0f / bh : 0};
        DrawPostPass("blur", {PostRtv(kBloomRtv + to), kBloomFormat, bw, bh}, b, pp);
    }
    Transition(bloom_[0].Get(), bloom_state_[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

// xboxrecomp (hppoa): bloom for a console picture (0..1, no HDR target): what passes the knee in the 3D scene,
// blurred at a quarter size twice over, added back before the HUD goes on top. Once a frame per target
constexpr float kSceneBloomKnee = 0.7f;
void D3D12Backend::SceneBloom(Target& t) {
    t.bloom_frame = presents_;
    const app::Settings& set = app::Get();
    if (!set.bloom || !set.bloom_strength) return;
    ID3D12Resource* fb = EnsureFeedbackCopy(t);
    if (!fb) return;
    uint32_t bw = std::max(1u, t.w / kBloomShrink);
    uint32_t bh = std::max(1u, t.h / kBloomShrink);
    if (!EnsureBloomTargets(bw, bh)) return;
    PostInputs in{{fb, nullptr, nullptr}, {t.fmt, kBloomFormat, kExposureFormat}};
    Transition(bloom_[0].Get(), bloom_state_[0], D3D12_RESOURCE_STATE_RENDER_TARGET);
    float pb[8] = {kSceneBloomKnee, 0, 0, 0, 1.0f / t.w, 1.0f / t.h};
    DrawPostPass("lbright", {PostRtv(kBloomRtv), kBloomFormat, bw, bh}, in, pb);
    for (int pass = 0; pass < 4; pass++) {  // across, down, across, down: a wider glow
        int from = pass & 1, to = 1 - from;
        Transition(bloom_[from].Get(), bloom_state_[from], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(bloom_[to].Get(), bloom_state_[to], D3D12_RESOURCE_STATE_RENDER_TARGET);
        PostInputs b{{bloom_[from].Get(), nullptr, nullptr}, {kBloomFormat, kBloomFormat, kBloomFormat}};
        float pp[8] = {0, 0, 0, 0, pass & 1 ? 0 : 1.0f / bw, pass & 1 ? 1.0f / bh : 0};
        DrawPostPass("blur", {PostRtv(kBloomRtv + to), kBloomFormat, bw, bh}, b, pp);
    }
    Transition(bloom_[0].Get(), bloom_state_[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    in.res[1] = bloom_[0].Get();
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    // the HDR resolve's last pass with no headroom and no tone mapping: the frame plus the glow
    float pt[8] = {1, 0, float(std::min(set.bloom_strength, 100u)) / 100.0f * 1.5f, 0};
    DrawPostPass("tone", {Cpu(rtv_heap_.Get(), t.view, rtv_inc_), t.fmt, t.w, t.h, t.samples}, in, pt);
}

// the frame is read: exposure, bloom and tone mapping take the target back to 0..1
void D3D12Backend::Resolve(Target& t) {
    if (cap_) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_hdr_%08X", cap_i_, t.data);
        CapSave(t, name);
    }
    t.hdr = false;
    ID3D12Resource* fb = EnsureFeedbackCopy(t);
    if (!fb) return;
    uint32_t bw = std::max(1u, t.w / kBloomShrink);
    uint32_t bh = std::max(1u, t.h / kBloomShrink);
    if (!EnsureBloomTargets(bw, bh)) return;
    if (!EnsureExposureTarget()) return;
    float a = UpdateExposureBlend();
    PostInputs in{{fb, nullptr, nullptr}, {t.fmt, kBloomFormat, kExposureFormat}};
    Transition(expo_.Get(), expo_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
    float lowest = tone_ ? kLowestExposure : kHighestExposure;
    float pe[8] = {kHeadroom, kExposureKey, lowest, kHighestExposure};
    DrawPostPass("exposure", {PostRtv(kExposureRtv), kExposureFormat, 1, 1}, in, pe, {.blend = a});
    Transition(expo_.Get(), expo_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    in.res[2] = expo_.Get();
    DrawBloom(in, t, bw, bh);
    in.res[1] = bloom_[0].Get();
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    float pt[8] = {kHeadroom, tone_ ? 1.0f : 0.0f, kBloomStrength, 0};
    DrawPostPass("tone", {Cpu(rtv_heap_.Get(), t.view, rtv_inc_), t.fmt, t.w, t.h, t.samples}, in, pt);
}

// SMAA's own targets: edges, then blend weights, then the blended frame in the frame's format
constexpr DXGI_FORMAT kSmaaEdgeFormat = DXGI_FORMAT_R8G8_UNORM;
constexpr DXGI_FORMAT kSmaaWeightFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

// a lookup texture filled from l's data, left in the pixel shader resource state
bool D3D12Backend::CreateLookupTexture(ComPtr<ID3D12Resource>& r, const SmaaLookup& l) {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(l.fmt, l.w, l.h);
    D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COPY_DEST;
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, st, nullptr, IID_PPV_ARGS(&r)))) {
        return false;
    }
    uint32_t row = l.w * l.bpp;
    uint32_t pitch = UploadPitch(row);
    Alloc a = Upload(uint64_t(pitch) * l.h, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    for (uint32_t y = 0; y < l.h; y++) std::memcpy(a.cpu + size_t(y) * pitch, l.data + size_t(y) * row, row);
    D3D12_TEXTURE_COPY_LOCATION dl{r.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION sl{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    sl.PlacedFootprint.Offset = a.off;
    sl.PlacedFootprint.Footprint = {l.fmt, l.w, l.h, 1, pitch};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    Transition(r.Get(), st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
}

bool D3D12Backend::EnsureSmaaLookups() {
    // smaa_textures.h: area 160 x 560 in two channels, search 64 x 16 in one
    constexpr SmaaLookup area{DXGI_FORMAT_R8G8_UNORM, 160, 560, 2, kSmaaArea};
    constexpr SmaaLookup search{DXGI_FORMAT_R8_UNORM, 64, 16, 1, kSmaaSearch};
    static_assert(sizeof(kSmaaArea) == area.w * area.h * area.bpp);
    static_assert(sizeof(kSmaaSearch) == search.w * search.h * search.bpp);
    if (smaa_lut_[1]) return true;
    if (!CreateLookupTexture(smaa_lut_[0], area) || !CreateLookupTexture(smaa_lut_[1], search)) {
        smaa_lut_[0].Reset();
        smaa_lut_[1].Reset();
        return false;
    }
    return true;
}

bool D3D12Backend::EnsureSmaaTargets(DXGI_FORMAT fmt, uint32_t w, uint32_t h) {
    if (smaa_[2] && smaa_w_ == w && smaa_h_ == h && smaa_fmt_ == fmt) return true;
    smaa_w_ = smaa_h_ = 0;
    for (int i = 0; i < 3; i++) {
        if (smaa_[i]) CurrentFrame().garbage.push_back(smaa_[i]);
        smaa_[i].Reset();
    }
    const DXGI_FORMAT f[3] = {kSmaaEdgeFormat, kSmaaWeightFormat, fmt};
    for (int i = 0; i < 3; i++) {
        if (!CreatePostTarget(smaa_[i], smaa_state_[i], f[i], w, h, kSmaaRtv + i)) {
            for (auto& r : smaa_) r.Reset();
            return false;
        }
    }
    smaa_w_ = w;
    smaa_h_ = h;
    smaa_fmt_ = fmt;
    LOGI("d3d12: SMAA %ux%u", w, h);
    return true;
}

// edges, blend weights, then the blend of src into smaa_[2], left readable
bool D3D12Backend::DrawSmaaPasses(ID3D12Resource* src, DXGI_FORMAT fmt, uint32_t w, uint32_t h, const char* hlsl) {
    // edge detection discards where there is no edge, so the edges start clear each frame
    const float clear[4] = {};
    float p[8] = {1.0f / w, 1.0f / h, float(w), float(h)};
    Transition(smaa_[0].Get(), smaa_state_[0], D3D12_RESOURCE_STATE_RENDER_TARGET);
    cl_->ClearRenderTargetView(PostRtv(kSmaaRtv), clear, 0, nullptr);
    PostInputs edges{{src, nullptr, nullptr}, {fmt, fmt, fmt}};
    PostTarget edges_out{PostRtv(kSmaaRtv), kSmaaEdgeFormat, w, h};
    if (!DrawPostPass("smaa_edges", edges_out, edges, p, {.hlsl = hlsl, .vs_entry = "vs_edges"})) return false;
    Transition(smaa_[0].Get(), smaa_state_[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(smaa_[1].Get(), smaa_state_[1], D3D12_RESOURCE_STATE_RENDER_TARGET);
    cl_->ClearRenderTargetView(PostRtv(kSmaaRtv + 1), clear, 0, nullptr);
    PostInputs weights{{smaa_[0].Get(), smaa_lut_[0].Get(), smaa_lut_[1].Get()},
                       {kSmaaEdgeFormat, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8_UNORM}};
    PostTarget weights_out{PostRtv(kSmaaRtv + 1), kSmaaWeightFormat, w, h};
    if (!DrawPostPass("smaa_weights", weights_out, weights, p, {.hlsl = hlsl, .vs_entry = "vs_weights"})) return false;
    Transition(smaa_[1].Get(), smaa_state_[1], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(smaa_[2].Get(), smaa_state_[2], D3D12_RESOURCE_STATE_RENDER_TARGET);
    PostInputs blend{{src, smaa_[1].Get(), nullptr}, {fmt, kSmaaWeightFormat, fmt}};
    PostTarget blend_out{PostRtv(kSmaaRtv + 2), fmt, w, h};
    if (!DrawPostPass("smaa_blend", blend_out, blend, p, {.hlsl = hlsl, .vs_entry = "vs_blend"})) return false;
    Transition(smaa_[2].Get(), smaa_state_[2], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
}

// the frame as it is shown, src in the pixel shader resource state: SMAA's blend of it, null when it can't run
ID3D12Resource* D3D12Backend::Smaa(ID3D12Resource* src, DXGI_FORMAT fmt, uint32_t w, uint32_t h) {
    static const std::string hlsl = std::string(kSmaaConfig) + kSmaaHlsl + kSmaaPasses;
    if (!EnsureSmaaLookups()) return nullptr;
    if (!EnsureSmaaTargets(fmt, w, h)) return nullptr;
    if (!DrawSmaaPasses(src, fmt, w, h, hlsl.c_str())) return nullptr;
    return smaa_[2].Get();
}

// ambient occlusion's target format, and the formats its passes read at t0 to t3: depth, the pass before, stencil,
// then a fourth slot no pass reads
constexpr DXGI_FORMAT kAoFormat = DXGI_FORMAT_R16G16_FLOAT;
constexpr DXGI_FORMAT kAoViewFormats[4] = {DXGI_FORMAT_R24_UNORM_X8_TYPELESS, kAoFormat,
                                           DXGI_FORMAT_X24_TYPELESS_G8_UINT, kBgra};
constexpr float kAoMaxSpan = 0.15f;  // most pixels the radius may span, as a share of the viewport height
constexpr float kAoRadius = 80.0f;   // world units, the engine's are cm

// the viewport in the target's pixels, clamped to it
D3D12_RECT ScaledViewportRect(const Viewport& v, const Target& rt) {
    float sc = float(rt.w) / rt.gw;
    return {std::clamp<LONG>(LONG(v.x * sc), 0, rt.w), std::clamp<LONG>(LONG(v.y * sc), 0, rt.h),
            std::clamp<LONG>(LONG((v.x + v.w) * sc), 0, rt.w), std::clamp<LONG>(LONG((v.y + v.h) * sc), 0, rt.h)};
}

bool D3D12Backend::EnsureAoTargets(uint32_t w, uint32_t h) {
    if (ao_[1] && ao_w_ == w && ao_h_ == h) return true;
    ao_w_ = w;
    ao_h_ = h;
    for (int i = 0; i < 2; i++) {
        if (!CreatePostTarget(ao_[i], ao_state_[i], kAoFormat, ao_w_, ao_h_, kAoRtv + i)) return false;
    }
    return true;
}

// the pass's pipeline, kept in blit_psos_; ms: the depth it reads is multisampled
ID3D12PipelineState* D3D12Backend::EnsureAoPipeline(const AoPass& pass, bool ms) {
    uint64_t seed = uint64_t(pass.out) | uint64_t(ms) << 32 | uint64_t(pass.samples) << 40;
    auto& pso = blit_psos_[Hash(pass.entry, std::strlen(pass.entry), seed)];
    if (pso) return pso.Get();
    std::string text = std::string(ms ? "#define MS 1\n" : "") + "#define NOSTENCIL 1\n" + kAoHlsl;
    ID3DBlob* vs = Compile(text, "vs", "vs_5_0");
    ID3DBlob* ps = Compile(text, pass.entry, "ps_5_0");
    if (!vs || !ps) return nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = FullScreenPipelineDesc(vs, ps, pass.out, pass.samples);
    if (pass.multiply) {
        // the alpha the game's blends read stays
        auto& b = d.BlendState.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = b.SrcBlendAlpha = D3D12_BLEND_ZERO;
        b.DestBlend = D3D12_BLEND_SRC_COLOR;
        b.DestBlendAlpha = D3D12_BLEND_ONE;
        b.BlendOp = b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    if (FAILED(dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) return nullptr;
    return pso.Get();
}

void D3D12Backend::CreateAoViews(uint32_t slot, const AoPass& pass, bool ms) {
    ID3D12Resource* reads[4] = {pass.depth, pass.src, pass.depth, pass.color};
    for (int i = 0; i < 4; i++) {
        ID3D12Resource* r = reads[i];
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = r ? kAoViewFormats[i] : kBgra;
        // multisampled: the depth and its stencil; the colour dof reads is the resolved copy
        sv.ViewDimension = r && ms && (i == 0 || i == 2) ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (sv.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D) {
            sv.Texture2D.MipLevels = 1;
            sv.Texture2D.PlaneSlice = r && i == 2;  // the stencil plane
        }
        dev_->CreateShaderResourceView(r, &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
}

bool D3D12Backend::DrawAoPass(const AoFrame& f, const AoPass& pass) {
    bool ms = pass.depth && f.depth_samples > 1;
    ID3D12PipelineState* pso = EnsureAoPipeline(pass, ms);
    if (!pso) return false;
    uint32_t slot = AllocSrvSlots(4);
    CreateAoViews(slot, pass, ms);
    decltype(f.c) c;
    std::memcpy(c, f.c, sizeof(c));
    // kAoHlsl's opt.zw: this pass's blur step
    c[14] = pass.step_x;
    c[15] = pass.step_y;
    Alloc cb = Upload(256, 256);
    std::memcpy(cb.cpu, c, sizeof(c));
    cl_->OMSetRenderTargets(1, &pass.rtv, FALSE, nullptr);
    cl_->RSSetViewports(1, &f.vp);
    cl_->RSSetScissorRects(1, &f.rect);
    DrawFullScreen(pso, cb.gpu, slot);
    return true;
}

// occlusion from ds's depth, blurred across, then blurred down and multiplied into rt
void D3D12Backend::DrawOcclusionPasses(Target& rt, Target& ds, const AoFrame& f) {
    bound_rt_ = bound_ds_ = nullptr;
    Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(ao_[0].Get(), ao_state_[0], D3D12_RESOURCE_STATE_RENDER_TARGET);
    const char* entry = f.gtao ? "gtao" : "occlusion";
    AoPass occlusion{.entry = entry, .out = kAoFormat, .rtv = PostRtv(kAoRtv), .depth = ds.res.Get()};
    if (!DrawAoPass(f, occlusion)) return;
    Transition(ao_[0].Get(), ao_state_[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(ao_[1].Get(), ao_state_[1], D3D12_RESOURCE_STATE_RENDER_TARGET);
    AoPass blur{.entry = "occlusion_blur", .out = kAoFormat, .rtv = PostRtv(kAoRtv + 1), .src = ao_[0].Get(),
                .step_x = 1};
    if (!DrawAoPass(f, blur)) return;
    Transition(ao_[1].Get(), ao_state_[1], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(rt.res.Get(), rt.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE scene = Cpu(rtv_heap_.Get(), rt.view, rtv_inc_);
    AoPass apply{.entry = "occlusion_apply", .out = rt.fmt, .samples = rt.samples, .multiply = true, .rtv = scene,
                 .src = ao_[1].Get(), .step_y = 1};
    DrawAoPass(f, apply);
    rt.drawn = true;
}

// p: 1 / the projection's x and y scale, its x and y centre, then view z = p[4] / (ndc depth - p[5])
void D3D12Backend::Occlusion(const State& s, const float p[kOcclusionParams]) { OcclusionOn(s.rt, s.zs, s.viewport, p); }

// xboxrecomp: Occlusion for a target and depth named directly (the scene's end, SceneEnd)
void D3D12Backend::OcclusionOn(const Surface& rts, const Surface& zss, const Viewport& v, const float p[kOcclusionParams]) {
    Target* rt = FindTarget(rts, false);
    Target* ds = FindTarget(zss, true);
    // the frame's own scene only, views drawn to smaller targets are left
    if (!rt || !ds || rt->gw != guest_w_ || rt->gh != guest_h_ || ds->w != rt->w || ds->h != rt->h ||
        ds->samples != rt->samples) {
        return;
    }
    float zr = v.max_z - v.min_z;
    D3D12_RECT rect = ScaledViewportRect(v, *rt);
    if (rect.right <= rect.left || rect.bottom <= rect.top || zr <= 0) return;
    if (!EnsureAoTargets(rt->w, rt->h)) return;
    if (cap_) {
        CapStep(rts);
        fprintf(cap_, "%04u occlusion rt %08X ds %08X\n", cap_i_++, rts.data, zss.data);
    }
    LOG_FIRST(1, "d3d12: ambient occlusion %s %ux%u, %u samples, projection %.4f %.4f, view z %.6f / (depth - %.8f)",
              p[9] ? "GTAO" : "SSAO", rt->w, rt->h, ds->samples, 1 / p[0], 1 / p[1], p[4], p[5]);
    float rect_w = float(rect.right - rect.left);
    float rect_h = float(rect.bottom - rect.top);
    // kAoHlsl's proj, zp, vp, opt and fog; each pass sets its blur step in opt.zw
    AoFrame f{.c = {p[0], p[1], p[2], p[3], p[4] * zr, v.min_z + p[5] * zr, 0, 0,
                    float(rect.left), float(rect.top), rect_w, rect_h, kAoRadius, kAoMaxSpan * rect_h, 0, 0,
                    p[6], p[7], p[8], 0},
              .rect = rect,
              .vp = {0, 0, float(rt->w), float(rt->h), 0, 1},
              .depth_samples = ds->samples,
              .gtao = p[9] != 0};
    DrawOcclusionPasses(*rt, *ds, f);
}

void D3D12Backend::TakeBuilt() {
    std::lock_guard lock(built_lock_);
    built_ready_ = false;
    for (auto& [h, pso] : built_) {
        // a pipeline made meanwhile without waiting stays: command lists hold it
        psos_.emplace(h, std::move(pso));
        building_.erase(h);
    }
    built_.clear();
}

ID3D12PipelineState* D3D12Backend::EnsurePipeline(const PsoKey& k, bool wait) {
    if (built_ready_.load(std::memory_order_acquire)) TakeBuilt();
    uint64_t h = Hash(&k, sizeof(k));
    auto it = psos_.find(h);
    if (it != psos_.end()) return it->second.Get();
    bool async = async_ && !cap_ && !wait;
    if (async && building_.count(h)) {
        skipped_frame_ = true;
        return nullptr;
    }
    bool pending = false;
    ID3DBlob* vs = EnsureShaderCode(k.vs, !async, pending);
    ID3DBlob* ps = EnsureShaderCode(k.ps, !async, pending);
    auto git = k.topo == uint32_t(Topology::Lines) ? line_gs_.find(k.vs) : line_gs_.end();
    uint64_t gh = git != line_gs_.end() ? git->second : 0;
    ID3DBlob* gs = gh ? EnsureShaderCode(gh, !async, pending) : nullptr;
    ComPtr<ID3D12PipelineState> pso;
    if (!pending && vs && ps && (gs || !gh)) pso = CreatePipeline(k, h, vs, ps, gs, !async, pending);
    if (pending) {
        building_.insert(h);
        skipped_frame_ = true;
        cache::Post([this, k, h, gh] {
            bool unused = false;
            ID3DBlob* vs = EnsureShaderCode(k.vs, true, unused);
            ID3DBlob* ps = EnsureShaderCode(k.ps, true, unused);
            ID3DBlob* gs = gh ? EnsureShaderCode(gh, true, unused) : nullptr;
            ComPtr<ID3D12PipelineState> pso;
            if (vs && ps && (gs || !gh)) pso = CreatePipeline(k, h, vs, ps, gs, true, unused);
            std::lock_guard lock(built_lock_);
            built_.emplace_back(h, std::move(pso));
            built_ready_.store(true, std::memory_order_release);
        });
        return nullptr;
    }
    return (psos_[h] = std::move(pso)).Get();
}

// the key's vertex attributes, ATTR0 to ATTR15 in register order; returns how many
uint32_t FillInputLayout(const PsoKey& k, D3D12_INPUT_ELEMENT_DESC el[16]) {
    uint32_t n = 0;
    for (uint32_t r = 0; r < 16; r++) {
        if (!((k.regs >> r) & 1)) continue;
        uint32_t e = k.elements[r];
        el[n++] = {"ATTR", r, DXGI_FORMAT(e & 0xFF), (e >> 8) & 0xFF, e >> 16,
                   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    }
    return n;
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE TopologyType(uint32_t topo) {
    switch (topo) {
        case 0: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        case 1: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        default: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
}

constexpr uint32_t kFillWireframe = 0x1B01;  // D3DFILL_WIREFRAME
constexpr uint32_t kCullCw = 0x900;          // D3DCULL_CW

void FillDepthStencilDesc(const PsoKey& k, D3D12_DEPTH_STENCIL_DESC& ds) {
    ds.DepthEnable = k.zen;
    ds.DepthWriteMask = k.zwrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = Cmp(k.zfunc);
    ds.StencilEnable = k.sten;
    ds.StencilReadMask = UINT8(k.smask);
    ds.StencilWriteMask = UINT8(k.swmask);
    D3D12_DEPTH_STENCILOP_DESC so{StencilOp(k.sfail), StencilOp(k.szfail), StencilOp(k.spass), Cmp(k.sfunc)};
    ds.FrontFace = ds.BackFace = so;
}

// everything of the pipeline but its input layout
void D3D12Backend::FillPipelineDesc(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps,
                                    D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) const {
    d.pRootSignature = root_.Get();
    d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    auto& b = d.BlendState.RenderTarget[0];
    b.BlendEnable = k.blend;
    b.SrcBlend = Blend(k.src, false);
    b.DestBlend = Blend(k.dst, false);
    b.BlendOp = BlendOp(k.op);
    b.SrcBlendAlpha = Blend(k.src, true);
    b.DestBlendAlpha = Blend(k.dst, true);
    b.BlendOpAlpha = BlendOp(k.op);
    b.RenderTargetWriteMask = UINT8(k.wmask);
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = k.fill == kFillWireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = k.cull ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
    d.RasterizerState.FrontCounterClockwise = k.ccw;
    d.RasterizerState.DepthBias = k.zbias;
    d.RasterizerState.SlopeScaledDepthBias = k.zslope;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.RasterizerState.MultisampleEnable = k.msaa != 0;
    FillDepthStencilDesc(k, d.DepthStencilState);
    d.PrimitiveTopologyType = TopologyType(k.topo);
    d.NumRenderTargets = k.has_rt ? 1 : 0;
    d.RTVFormats[0] = k.has_rt ? DXGI_FORMAT(k.rtfmt) : DXGI_FORMAT_UNKNOWN;
    d.DSVFormat = k.has_ds ? DXGI_FORMAT_D24_UNORM_S8_UINT : DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = std::max(k.msaa, 1u);
}

// from the library, else from the driver when compile is set (any thread)
ComPtr<ID3D12PipelineState> D3D12Backend::CreatePipeline(const PsoKey& k, uint64_t h, ID3DBlob* vs, ID3DBlob* ps,
                                                         ID3DBlob* gs, bool compile, bool& pending) {
    D3D12_INPUT_ELEMENT_DESC el[16];
    uint32_t n = FillInputLayout(k, el);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.InputLayout = {el, n};
    FillPipelineDesc(k, vs, ps, d);
    if (gs) d.GS = {gs->GetBufferPointer(), gs->GetBufferSize()};
    // the bytecode's own checksum: another compiler's output for the same source is another pipeline
    auto sum = [](ID3DBlob* b) { return Hash(static_cast<const uint8_t*>(b->GetBufferPointer()) + 4, 16); };
    uint64_t name = Hash(&h, sizeof(h), sum(vs) ^ sum(ps) * 3 ^ (gs ? sum(gs) * 5 : 0));
    auto t0 = std::chrono::steady_clock::now();
    ComPtr<ID3D12PipelineState> pso = cache::LoadPipeline(name, d);
    SlowGpu(MsSince(t0), "pipeline %016llX from the library (%s)", (unsigned long long)h, pso ? "found" : "not there");
    g_hitch_pso_n++;
    g_hitch_pso_ns += uint64_t(MsSince(t0) * 1e6);
    if (pso) {
        RecordPipelineKey(k);
        return pso;
    }
    if (!compile) {
        pending = true;
        return nullptr;
    }
    t0 = std::chrono::steady_clock::now();
    HRESULT made = dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
    SlowGpu(MsSince(t0), "pipeline %016llX made by the driver", (unsigned long long)h);
    g_hitch_pso_ns += uint64_t(MsSince(t0) * 1e6);
    if (FAILED(made)) {
        // xboxrecomp: bytecode from base_shaders.bin this driver won't take: this PC's compiler makes it, once
        bool vs_base = DropBaseCode(k.vs);
        bool ps_base = DropBaseCode(k.ps);
        if (vs_base || ps_base) {
            bool unused = false;
            ID3DBlob* vs2 = EnsureShaderCode(k.vs, true, unused);
            ID3DBlob* ps2 = EnsureShaderCode(k.ps, true, unused);
            if (vs2 && ps2) return CreatePipeline(k, h, vs2, ps2, gs, compile, pending);
        }
        // xboxrecomp: which pipeline and why, as its draws are skipped from now on (psos_ keeps the null): 32 failed
        // at every start on an RTX 3060 under vkd3d-proton (2026-11-01), unnamed. VKD3D_DEBUG=warn says more there
        LOGE("d3d12: pipeline %016llX creation failed (0x%08lX), its draws are skipped: vs %016llX ps %016llX%s, %u "
             "target(s) format %d, depth format %d, %u sample(s), topology type %d", (unsigned long long)h,
             (unsigned long)made, (unsigned long long)k.vs, (unsigned long long)k.ps, gs ? " with a line gs" : "",
             d.NumRenderTargets, int(d.RTVFormats[0]), int(d.DSVFormat), d.SampleDesc.Count, int(d.PrimitiveTopologyType));
        return nullptr;
    }
    cache::StorePipeline(name, pso.Get());
    RecordPipelineKey(k);
    return pso;
}

// xboxrecomp: the pipeline into pipelines_list.bin once (any thread)
void D3D12Backend::RecordPipelineKey(const PsoKey& k) {
    PipelineRecord r{k, 0, 0};
    {
        std::lock_guard lock(shader_lock_);
        auto v = shaders_.find(k.vs), p = shaders_.find(k.ps);
        if (v == shaders_.end() || p == shaders_.end()) return;
        r.vs_check = v->second.check;
        r.ps_check = p->second.check;
    }
    cache::RecordPipeline(Hash(&k, sizeof(k)), &r);
    gpu::g_pipelines_recorded = uint32_t(cache::RecordedPipelineCount());
}

// RECOMP_LINE_GS=0: lines drawn by D3D12 itself (one host pixel, centres at .5), for comparison
static bool LineGsOn() {
    static const bool on = [] { const char* e = std::getenv("RECOMP_LINE_GS"); return !(e && *e == '0'); }();
    return on;
}

// xboxrecomp: every recorded pipeline made on the build threads behind whatever the title asks for, from the shader
// cache's bytecode (shaders.bin, base_shaders.bin), and kept in the pipeline library: the first time a level shows an
// effect it is ready instead of compiled then (the stutter, the held frames). Records for another anti-aliasing
// sample count than this one are left; a record whose shaders are not in the cache is skipped. RECOMP_PREBUILD=0: off
void D3D12Backend::StartPrebuild() {
    cache::OpenPipelineList(sizeof(PipelineRecord));
    gpu::g_pipelines_recorded = uint32_t(cache::RecordedPipelineCount());
    if (const char* e = std::getenv("RECOMP_PREBUILD"); e && *e == '0') return;
    uint32_t msaa = app::Get().msaa;
    std::vector<PipelineRecord> list;
    for (const auto& bytes : cache::RecordedPipelines()) {
        PipelineRecord r;
        std::memcpy(&r, bytes.data(), sizeof(r));
        if (r.k.msaa && r.k.msaa != msaa) continue;
        // a line pipeline carries LineGsHlsl, which the record has no code for: made when first drawn (and left out
        // of the count, or the startup note waited for it forever)
        if (LineGsOn() && r.k.topo == uint32_t(Topology::Lines)) continue;
        list.push_back(r);
    }
    if (list.empty()) return;
    gpu::g_prebuild_total = uint32_t(list.size());
    auto t0 = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
    LOGI("[PIPELINES] making %zu recorded pipelines ahead of need", list.size());
    for (const PipelineRecord& r : list) {
        cache::PostLow([this, r, t0] {
            uint64_t h = Hash(&r.k, sizeof(r.k));
            ComPtr<ID3DBlob> vs = cache::FindShader(r.k.vs, r.vs_check), ps = cache::FindShader(r.k.ps, r.ps_check);
            ComPtr<ID3D12PipelineState> pso;
            bool unused = false;
            if (vs && ps) pso = CreatePipeline(r.k, h, vs.Get(), ps.Get(), nullptr, true, unused);
            if (pso) {
                std::lock_guard lock(built_lock_);
                built_.emplace_back(h, std::move(pso));
                built_ready_.store(true, std::memory_order_release);
            } else {
                gpu::g_prebuild_failed++;  // shaders not in this cache, or the driver refused it: made on demand
            }
            if (++gpu::g_prebuild_done == gpu::g_prebuild_total)
                LOGI("[PIPELINES] %u made ahead in %.1f s, %u could not be (left to be made on demand)",
                     gpu::g_prebuild_total.load() - gpu::g_prebuild_failed.load(),
                     std::chrono::duration<double>(std::chrono::steady_clock::now() - *t0).count(),
                     gpu::g_prebuild_failed.load());
        });
    }
}

TexKind TextureKind(const TextureDesc& d) {
    if (d.cube) return kTexCube;
    if (d.depth > 1) return kTex3D;
    return kTex2D;
}

// console formats without an alpha channel: R5G6B5, X1R5G5B5, X8R8G8B8 and their linear forms
bool NoAlpha(uint32_t format) {
    return format == 0x05 || format == 0x03 || format == 0x07 || format == 0x11 || format == 0x1C || format == 0x1E;
}

constexpr float kUpscaledMin = 1.01f;  // host texels per console texel past rounding: the target is upscaled

ID3D12Resource* D3D12Backend::BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t,
                                                DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    D3D12_SHADER_RESOURCE_VIEW_DESC& sv = stages.sv[stage];
    if (&t == draw_ds_) return nullptr;
    bool feedback = &t == draw_rt_;
    stages.rt[stage] = true;
    if (!t.depth && LatchCpuWrite(t)) {
        Surface su;
        su.header = s.textures[size_t(stage)];
        UploadTarget(t, su);
    }
    info.kind = kTex2D;
    // a pass shrinking it averages each tap over a console texel, else upscaled taps skip texels and ghost
    Target* rt = FindTarget(s.rt, false);
    float k = t.gw ? float(t.w) / t.gw : 1;
    if (!t.depth && k > kUpscaledMin && rt && rt != &t && (t.gw > rt->gw || t.gh > rt->gh)) {
        stages.wide[stage] = k;
        info.flags |= kTexWide;
    }
    if (t.hdr) info.flags |= kTexHdr;
    sv.Format = t.depth ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : t.fmt;
    // xboxrecomp: a target read as a format without alpha reads alpha 1 on the console, whatever the host target
    // holds there. HP:CoS draws its static scenery with a pixel shader that multiplies alpha by its R5G6B5 shadow
    // target's and alpha tests the product: the clear left 0 there and every ground and wall pixel was discarded.
    // (in the shader, kTexOpaque: a swizzled view made vkd3d's sampler border colour black)
    if (!t.depth && NoAlpha(d.format)) info.flags |= kTexOpaque;
    // xboxrecomp: a target left bound to a stage the shader never samples needs no copy. HP:CoS draws its falling
    // house cards into page 82D84000 with that page still on stage 1: ~700 whole page copies a frame, 30 fps
    static const bool copy_all = std::getenv("RECOMP_FEEDBACK_ALL") && *std::getenv("RECOMP_FEEDBACK_ALL") == '1';
    if (feedback && !copy_all && !StageSampled(s, stage)) return nullptr;
    // the console reads the pixels it is about to overwrite: give the draw a copy
    if (feedback) {
        feedback_stage_ = stage;
        return EnsureFeedbackCopy(t, true);
    }
    if (t.depth) info.shadow = ShadowDepthKind(d.format);
    Readable r = ReadableResource(t);
    if (r.res) Transition(r.res, *r.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return r.res;
}

bool SameTexture(const Tex& tx, const TextureDesc& d) {
    return tx.res && tx.d.format == d.format && tx.d.width == d.width && tx.d.height == d.height &&
           tx.d.depth == d.depth && tx.d.levels == d.levels && tx.d.cube == d.cube && tx.d.pitch == d.pitch;
}

// false when the texture could not be made, its entry dropped
bool D3D12Backend::CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key) {
    g_hitch_tex_new++;
    struct HitchTime {
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        ~HitchTime() { g_hitch_texnew_ns += uint64_t(MsSince(t0) * 1e6); }
    } hitch_time;
    if (tx.res) CurrentFrame().garbage.push_back(tx.res);
    tx = Tex{};
    tx.d = d;
    tx.fmt = HostFormat(fi.host);
    bool bc = fi.compressed;
    uint32_t w = d.width;
    uint32_t h = d.height;
    tx.levels = d.levels;
    if (bc && ((w & 3) || (h & 3))) {
        w = (w + 3) & ~3u;
        h = (h + 3) & ~3u;
        tx.levels = 1;
    }
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td{};
    td.Dimension = d.depth > 1 ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w;
    td.Height = h;
    td.DepthOrArraySize = UINT16(d.cube ? 6 : d.depth);
    td.MipLevels = UINT16(tx.levels);
    td.Format = tx.fmt;
    td.SampleDesc.Count = 1;
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&tx.res)))) {
        textures_.erase(key);
        return false;
    }
    return true;
}

void UpdateNormCube(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint32_t f, const std::vector<uint8_t>& px,
                   uint32_t pitch) {
    // a normalization cube: each face's centre points along its axis, 255 or 0 there, 128 across
    if (!f) tx.norm = fi.host == Host::BGRA8 && d.width >= 8 && px.size() >= size_t(d.width / 2 + 1) * pitch;
    const uint8_t* c = tx.norm ? &px[size_t(d.width / 2) * pitch + d.width / 2 * 4] : nullptr;
    for (uint32_t ch = 0; c && ch < 3; ch++) {
        int want = 128;
        if (ch == f / 2) want = f & 1 ? 0 : 255;
        tx.norm &= std::abs(c[2 - ch] - want) <= 16;
    }
}

void CopyLevelRows(uint8_t* out, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp, UINT rows, UINT64 row_size,
                   const std::vector<uint8_t>& px, uint32_t pitch) {
    uint32_t slices = fp.Footprint.Depth;
    size_t src_rows = pitch ? px.size() / pitch / std::max(slices, 1u) : 0;
    for (uint32_t z = 0; z < slices; z++) {
        for (uint32_t y = 0; y < rows; y++) {
            uint8_t* dst = out + (size_t(z) * rows + y) * fp.Footprint.RowPitch;
            if (y < src_rows) {
                std::memcpy(dst, &px[(size_t(z) * src_rows + y) * pitch], std::min<size_t>(pitch, row_size));
            } else {
                std::memset(dst, 0, size_t(row_size));
            }
        }
    }
}

void D3D12Backend::UploadTextureLevel(Tex& tx, uint32_t f, uint32_t l, const std::vector<uint8_t>& px,
                                      uint32_t pitch) {
    D3D12_RESOURCE_DESC rd = tx.res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 row_size;
    UINT64 bytes;
    uint32_t sub = f * tx.levels + l;
    dev_->GetCopyableFootprints(&rd, sub, 1, 0, &fp, &rows, &row_size, &bytes);
    Alloc a = Upload(bytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    CopyLevelRows(a.cpu, fp, rows, row_size, px, pitch);
    fp.Offset = a.off;
    D3D12_TEXTURE_COPY_LOCATION dl{tx.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dl.SubresourceIndex = sub;
    D3D12_TEXTURE_COPY_LOCATION sl{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    sl.PlacedFootprint = fp;
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
}

void D3D12Backend::UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p,
                                 const uint32_t* palette, bool same) {
    auto hitch_t0 = std::chrono::steady_clock::now();
    D3D12_RESOURCE_STATES st = same ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COPY_DEST;
    Transition(tx.res.Get(), st, D3D12_RESOURCE_STATE_COPY_DEST);
    uint32_t faces = d.cube ? 6 : 1;
    std::vector<uint8_t> px;
    for (uint32_t f = 0; f < faces; f++) {
        for (uint32_t l = 0; l < tx.levels; l++) {
            uint32_t pitch;
            DecodeLevel(d, l, f, p, palette, px, pitch);
            if (d.cube && !l) UpdateNormCube(tx, d, fi, f, px, pitch);
            UploadTextureLevel(tx, f, l, px, pitch);
        }
    }
    Transition(tx.res.Get(), st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    g_hitch_tex_up++;
    g_hitch_tex_bytes += TotalSize(d);
    g_hitch_tex_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                                      hitch_t0).count());
}

void FillTextureView(const Tex& tx, const TextureDesc& d, D3D12_SHADER_RESOURCE_VIEW_DESC& sv) {
    sv.Format = tx.fmt;
    if (d.cube) {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        sv.TextureCube.MipLevels = tx.levels;
    } else if (d.depth > 1) {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        sv.Texture3D.MipLevels = tx.levels;
    } else {
        sv.Texture2D.MipLevels = tx.levels;
    }
}

// xboxrecomp (from XboxXBERecomp): a P8 texture's palette: its colours, their address (part of the texture's key)
// and their hash, all 0 without one
struct StagePalette {
    uint32_t colours[256] = {};
    uint32_t va = 0;
    uint64_t hash = 0;
};

// a P8 texture is decoded with the stage's palette: 256 colours, fewer by the size code in the top two bits
void ReadStagePalette(const State& s, int stage, const TextureDesc& d, StagePalette& palette) {
    constexpr uint32_t kFmtP8 = 0x0B;          // D3DFMT_P8
    constexpr uint32_t kPaletteCommon = 0;     // D3DPalette: Common, its top two bits the size code
    constexpr uint32_t kPaletteData = 4;       // D3DPalette: Data, physical address of the colours
    constexpr uint32_t kPaletteSizeShift = 30;
    if (d.format != kFmtP8 || !s.palettes[size_t(stage)]) return;
    uint32_t header = s.palettes[size_t(stage)];
    palette.va = mem::VirtualFromPhysical(kernel::R32(header + kPaletteData));
    uint32_t size_code = kernel::R32(header + kPaletteCommon) >> kPaletteSizeShift;
    size_t bytes = (std::size(palette.colours) >> size_code) * sizeof(uint32_t);
    std::memcpy(palette.colours, kernel::HostPtr(palette.va), bytes);
    palette.hash = Hash(palette.colours, sizeof(palette.colours));
}

// a texture in guest memory: made again when its layout changed, uploaded again when its bytes did
// xboxrecomp: key prompts (F10 Controls, Button prompts; app_KeyboardPrompts). The HUD's button cluster at the top
// right is one 128x128 picture of the pad's Y X B A (panel.geo), known here by a hash of its texels; with key prompts on
// it is drawn as four keycaps with the bound keys' names, where the buttons were (Y top, X left, B right, A bottom).
extern "C" int app_KeyboardPrompts(void);
extern "C" const char* app_ButtonKeyLabel(int pad_input);
constexpr uint64_t kPromptPicHash = 0;  // RECOMP_PROMPT_LOG prints the candidates' hashes

uint64_t FullHash(uint32_t data, uint32_t n) {
    return Hash(kernel::HostPtr(data), n);
}

// a label short enough for a keycap
std::string CapLabel(const char* name) {
    std::string n = name;
    if (n == "Left Click") return "LMB";
    if (n == "Right Click") return "RMB";
    if (n == "Middle Click") return "MMB";
    if (n == "Mouse 4") return "M4";
    if (n == "Mouse 5") return "M5";
    if (n == "Space") return "SPACE";
    if (n == "Escape") return "ESC";
    if (n == "Enter") return "ENTER";
    if (n == "Backspace") return "BKSP";
    return n;
}

// 128x128 BGRA, keycaps over transparency
std::vector<uint8_t> PromptPicture(const std::string labels[4]) {
    constexpr int W = 128;
    std::vector<uint8_t> px(W * W * 4, 0);
    struct Cap { int cx, cy; } caps[4] = {{64, 24}, {22, 64}, {106, 64}, {64, 104}};  // Y X B A
    // the labels, white on black, through GDI: its coverage is the text's alpha
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -W;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bm || !bits) {
        if (bm) DeleteObject(bm);
        DeleteDC(dc);
        return px;
    }
    HGDIOBJ old = SelectObject(dc, bm);
    std::memset(bits, 0, W * W * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    for (int i = 0; i < 4; i++) {
        if (labels[i].empty()) continue;
        int h = labels[i].size() <= 1 ? 26 : labels[i].size() <= 3 ? 18 : 13;
        HFONT f = CreateFontA(-h, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Arial");
        HGDIOBJ of = SelectObject(dc, f);
        RECT r{caps[i].cx - 30, caps[i].cy - 16, caps[i].cx + 30, caps[i].cy + 16};
        DrawTextA(dc, labels[i].c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, of);
        DeleteObject(f);
    }
    GdiFlush();
    const uint8_t* text = static_cast<const uint8_t*>(bits);
    for (int y = 0; y < W; y++) {
        for (int x = 0; x < W; x++) {
            float a = 0, edge = 0;
            for (int i = 0; i < 4; i++) {
                if (labels[i].empty()) continue;
                int hw = labels[i].size() <= 1 ? 19 : 28, hh = 17;  // a single letter's cap is near square
                // rounded box distance, radius 7
                float dx = std::max(std::fabs(float(x) + 0.5f - caps[i].cx) - (hw - 7), 0.0f);
                float dy = std::max(std::fabs(float(y) + 0.5f - caps[i].cy) - (hh - 7), 0.0f);
                float d = std::sqrt(dx * dx + dy * dy) - 7;
                a = std::max(a, std::clamp(0.5f - d, 0.0f, 1.0f));
                edge = std::max(edge, std::clamp(1.0f - std::fabs(d + 1.5f) / 1.5f, 0.0f, 1.0f));
            }
            uint8_t* o = &px[(y * W + x) * 4];
            float t = text[(y * W + x) * 4 + 1] / 255.0f;  // green: GDI's grey coverage
            float c = 0.18f + 0.62f * edge;                // dark cap, light rim
            c = c * (1 - t) + 1.0f * t;
            uint8_t v = uint8_t(std::clamp(c, 0.0f, 1.0f) * 255);
            o[0] = o[1] = o[2] = v;
            o[3] = uint8_t(std::clamp(a * 0.92f + t * 0.08f, 0.0f, 1.0f) * 255);
        }
    }
    SelectObject(dc, old);
    DeleteObject(bm);
    DeleteDC(dc);
    return px;
}

ID3D12Resource* D3D12Backend::BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                               DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    StagePalette palette;
    ReadStagePalette(s, stage, d, palette);
    const uint64_t key = uint64_t(palette.va) << 32 | d.data;
    Tex& tx = textures_[key];
    uint32_t total = TotalSize(d);
    bool same = SameTexture(tx, d);
    if (!same && !CreateTexture(tx, d, fi, key)) return nullptr;
    // cheap change check once per frame: lock epochs plus a sample of the bytes
    if (tx.frame != presents_ + 1 || !same || tx.palette != palette.hash) {
        tx.frame = presents_ + 1;
        tx.palette = palette.hash;
        const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
        uint64_t sig = MemSig(d.data, total) ^ DirtyEpoch(d.data) ^ palette.hash;
        if (sig != tx.sig || !same) {
            tx.sig = sig;
            UploadTexture(tx, d, fi, p, palette.va ? palette.colours : nullptr, same);
            // xboxrecomp: the HUD's button picture, by its texels (key prompts)
            tx.prompt_pic = false;
            if (d.width == 128 && d.height == 128 && !d.cube && d.depth == 1) {
                uint64_t h = FullHash(d.data, LevelSize(d, 0));
                tx.prompt_pic = h == kPromptPicHash;
                static const bool log = std::getenv("RECOMP_PROMPT_LOG") != nullptr;
                if (log) fprintf(stderr, "[PROMPTS] 128x128 texture %08X format %02X hash %016llX\n", d.data, d.format,
                                 (unsigned long long)h);
            }
        }
    }
    if (tx.prompt_pic && app_KeyboardPrompts()) {
        // the keycaps instead, remade when a key name changes
        std::string labels[4] = {CapLabel(app_ButtonKeyLabel(3)), CapLabel(app_ButtonKeyLabel(2)),
                                 CapLabel(app_ButtonKeyLabel(1)), CapLabel(app_ButtonKeyLabel(0))};
        std::string sig = labels[0] + "|" + labels[1] + "|" + labels[2] + "|" + labels[3];
        TextureDesc kd;
        kd.data = 0xFFFF0000u;
        kd.format = 0x12;  // LIN_A8R8G8B8
        kd.width = kd.height = 128;
        kd.pitch = 128 * 4;
        kd.linear = true;
        const FormatInfo& kfi = Format(kd.format);
        Tex& kt = textures_[0xFFFFFFFFFFFF0000ull];
        bool ksame = SameTexture(kt, kd);
        if (!ksame && !CreateTexture(kt, kd, kfi, 0xFFFFFFFFFFFF0000ull)) return nullptr;
        if (!ksame || prompt_sig_ != sig) {
            prompt_sig_ = sig;
            std::vector<uint8_t> pic = PromptPicture(labels);
            UploadTexture(kt, kd, kfi, pic.data(), nullptr, ksame);
        }
        FillTextureView(kt, kd, stages.sv[stage]);
        return kt.res.Get();
    }
    FillTextureView(tx, d, stages.sv[stage]);
    if (fi.depth) info.shadow = ShadowDepthKind(d.format);
    if (tx.norm && d.cube && hdr_) info.flags |= kTexNormCube;
    return tx.res.Get();
}

constexpr uint32_t kTaddressBorderTex = 4;  // D3DTADDRESS_BORDER

ID3D12Resource* D3D12Backend::BindTexture(const State& s, int stage, DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    D3D12_SHADER_RESOURCE_VIEW_DESC& sv = stages.sv[stage];
    info = {};
    sv = {};
    stages.rt[stage] = false;
    stages.wide[stage] = 0;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sv.Texture2D.MipLevels = 1;
    TextureDesc d;
    if (!s.textures[size_t(stage)] || !Describe(s.textures[size_t(stage)], d)) return nullptr;
    const FormatInfo& fi = Format(d.format);
    info.kind = TextureKind(d);
    info.rect = d.linear;
    // xboxrecomp: border addressing in the shader (kTexBorderU), the sampler clamps
    if (info.kind == kTex2D && !fi.depth) {
        const auto& ts = s.tss[size_t(stage)];
        if (ts[kTssAddressU] == kTaddressBorderTex) info.flags |= kTexBorderU;
        if (ts[kTssAddressV] == kTaddressBorderTex) info.flags |= kTexBorderV;
    }

    // rendered by the GPU: sample the target itself; DXT data loaded over a target's memory is not it
    auto tit = targets_.find(TargetKey(d.data, fi.depth));
    if (tit != targets_.end() && !d.cube && d.depth == 1 && !fi.compressed) {
        return BindTargetTexture(s, stage, d, tit->second, stages);
    }
    return BindGuestTexture(s, stage, d, fi, stages);
}

// D3DTEXTUREFILTERTYPE and D3DTEXTUREADDRESS values the samplers test
constexpr uint32_t kTexfLinear = 2;
constexpr uint32_t kTexfAnisotropic = 3;
constexpr uint32_t kTaddressWrap = 1;
constexpr uint32_t kTaddressClamp = 3;

D3D12_SAMPLER_DESC StageSampler(const State& s, int i, const DrawTextures& stages, bool clamp, uint32_t aniso) {
    D3D12_SAMPLER_DESC sd = {};
    const auto& t = s.tss[size_t(i)];
    uint32_t mag = t[kTssMagFilter];
    uint32_t min = t[kTssMinFilter];
    uint32_t mip = t[kTssMipFilter];
    auto lin = [](uint32_t f) { return f >= kTexfLinear; };
    D3D12_FILTER_TYPE fmin = lin(min) ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE fmag = lin(mag) ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE fmip = mip == kTexfLinear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    sd.Filter = D3D12_FILTER(fmin << 4 | fmag << 2 | fmip);
    bool anisotropic = min == kTexfAnisotropic || mag == kTexfAnisotropic;
    uint32_t an = anisotropic ? std::max<uint32_t>(t[kTssMaxAnisotropy], 1) : 1;
    // forced only where the title filters across mip levels and the texture has them
    if (aniso && lin(min) && mip && stages.sv[i].Texture2D.MipLevels > 1 && !stages.info[i].shadow) an = aniso;
    if (an > 1) {
        sd.Filter = D3D12_FILTER_ANISOTROPIC;
        sd.MaxAnisotropy = std::min<uint32_t>(an, 16);
    }
    // clamp: the draw's coordinates all lie in 0..1 (UnitTexcoords), so wrap is read as clamp
    sd.AddressU = Address(clamp && t[kTssAddressU] == kTaddressWrap ? kTaddressClamp : t[kTssAddressU]);
    sd.AddressV = Address(clamp && t[kTssAddressV] == kTaddressWrap ? kTaddressClamp : t[kTssAddressV]);
    // xboxrecomp: the shader does the border (kTexBorderU/V)
    if (stages.info[i].flags & kTexBorderU) sd.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    if (stages.info[i].flags & kTexBorderV) sd.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sd.AddressW = Address(t[kTssAddressW]);
    sd.MipLODBias = Bits(t[kTssMipLodBias]);
    sd.MinLOD = float(t[kTssMaxMipLevel]);
    sd.MaxLOD = mip ? D3D12_FLOAT32_MAX : sd.MinLOD;
    Color(t[kTssBorderColor], sd.BorderColor);
    if (stages.info[i].shadow) {
        // the console tests texel OP r, D3D12 tests r OP texel
        static const uint8_t swap[8] = {0, 4, 2, 6, 1, 5, 3, 7};
        sd.Filter = lin(min) || lin(mag) ? D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT
                                         : D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
        sd.ComparisonFunc = D3D12_COMPARISON_FUNC(swap[Cmp(s.rs[kRsShadowFunc]) - 1] + 1);
        sd.MaxAnisotropy = 0;
    }
    return sd;
}

uint32_t D3D12Backend::EnsureStageSamplers(const State& s, const DrawTextures& stages, bool clamp) {
    D3D12_SAMPLER_DESC sd[4];
    uint32_t aniso = app::Get().aniso;
    for (int i = 0; i < 4; i++) sd[i] = StageSampler(s, i, stages, clamp, aniso);
    uint64_t key = Hash(sd, sizeof(sd));
    auto it = samplers_.find(key);
    if (it != samplers_.end()) return it->second;
    if (smp_used_ + 4 > kSamplerCount) {
        samplers_.clear();
        smp_used_ = 0;
    }
    uint32_t base = smp_used_;
    smp_used_ += 4;
    for (int i = 0; i < 4; i++) dev_->CreateSampler(&sd[i], Cpu(smp_heap_.Get(), base + i, smp_inc_));
    samplers_[key] = base;
    return base;
}

uint32_t SimpleState(const State& s, uint32_t method) {
    return s.simple[method >> 2];
}

// HDR starts at a frame's first 3D draw into the HDR scene target; screen space quads never start it: a loading
// screen's fade redraws over the last tone mapped picture every frame
bool D3D12Backend::EnterHdrForScene(const State& s, const Assembled& a) {
    bool scene_target = bound_rt_ && bound_ds_ && bound_rt_->fmt == kHdrFormat;
    bool not_started = scene_target && !bound_rt_->hdr && bound_rt_->hdr_frame != presents_;
    bool depth_written_3d = !a.pretransformed && s.rs[kRsZEnable] && SimpleState(s, kMZWriteEnable);
    if (not_started && depth_written_3d) {
        EnterHdr(*bound_rt_);
        if (!BindTargets(s)) return false;
    }
    return true;
}

// D3DTRANSFORMSTATETYPE indices into s.transforms
constexpr size_t kTsView = 2;
constexpr size_t kTsProjection = 3;
constexpr size_t kTsTexture0 = 16;
constexpr size_t kTsWorld = 256;

// xboxrecomp (hppoa): a frame's 3D scene goes into the guest sized target with depth written; the first 2D draw after
// it there (pretransformed, or no depth test, sampling no render target) is the HUD's: the scene is bloomed first
bool D3D12Backend::BloomForScene(const State& s, const Assembled& a) {
    Target* t = bound_rt_;
    if (!t || t->gw != guest_w_ || t->gh != guest_h_ || t->hdr) return true;
    bool flat = a.pretransformed || !s.rs[kRsZEnable];
    if (!flat) {
        if (bound_ds_ && SimpleState(s, kMZWriteEnable)) {
            t->scene_frame = presents_;
            // the camera, from a fixed function draw (a vertex program keeps its projection in its constants)
            bool prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
            if (!prog) {
                std::memcpy(scene_proj_, s.transforms[kTsProjection].data(), sizeof(scene_proj_));
                scene_vp_ = s.viewport;
                scene_rt_ = s.rt;
                scene_zs_ = s.zs;
                scene_fog_[0] = s.rs[kRsFogEnable];
                scene_fog_[1] = s.rs[kRsFogTableMode];
                scene_fog_[2] = s.rs[kRsFogStart];
                scene_fog_[3] = s.rs[kRsFogEnd];
                scene_fog_[4] = s.rs[kRsFogDensity];
                scene_cam_frame_ = presents_;
            }
        }
        return true;
    }
    if (t->scene_frame != presents_ || t->bloom_frame == presents_) return true;
    // a view drawn into a texture laid over the scene is not the HUD: the train's window shows the landscape drawn
    // first, then the compartment after it (the scene ends at the depth clear before the HUD, Clear)
    for (uint32_t tex : s.textures) {
        TextureDesc d;
        if (tex && Describe(tex, d) && targets_.count(TargetKey(d.data, Format(d.format).depth))) return true;
    }
    SceneEnd(*t);
    return bound_rt_ == t || BindTargets(s);
}

// xboxrecomp (hppoa): kAoHlsl's camera from the scene's projection (row vectors: clip = view * P): 1 / x and y scale
// and centre as the shader takes them, view z = p4 / (ndc depth - p5), the fog as view z's line (p6, p7, on p8),
// p9 GTAO. False without a perspective projection this frame
bool D3D12Backend::SceneCamera(float p[kOcclusionParams]) const {
    if (scene_cam_frame_ != presents_) return false;
    const float* m = scene_proj_;
    float w = m[11];
    if (std::fabs(w) < 1e-6f || std::fabs(m[0]) < 1e-6f || std::fabs(m[5]) < 1e-6f || std::fabs(m[14]) < 1e-9f) return false;
    p[0] = w / m[0];
    p[1] = w / m[5];
    p[2] = m[8] / w;
    p[3] = m[9] / w;
    p[4] = m[14] / w;
    p[5] = m[10] / w;
    p[6] = p[7] = p[8] = 0;
    if (scene_fog_[0]) {
        float start = Bits(scene_fog_[2]), end = Bits(scene_fog_[3]), dens = Bits(scene_fog_[4]);
        if (scene_fog_[1] == 3 && end > start) p[6] = 1 / (end - start), p[7] = -start / (end - start), p[8] = 1;
        else if ((scene_fog_[1] == 1 || scene_fog_[1] == 2) && dens > 0) p[6] = dens / 3, p[8] = 1;
    }
    p[9] = app::Get().ambient_occlusion >= 2 ? 1.0f : 0.0f;
    return true;
}

extern "C" int hpcos_Cutscene(void);  // hle_shots.c: no cutscene test in this title yet, so 0

void D3D12Backend::SceneEnd(Target& t) {
    const app::Settings& set = app::Get();
    float p[kOcclusionParams];
    bool cam = SceneCamera(p) && scene_rt_.data == t.data;
    if (cam && set.ambient_occlusion) OcclusionOn(scene_rt_, scene_zs_, scene_vp_, p);
    if (cam && set.dof && set.dof_strength && hpcos_Cutscene()) SceneDof(t);
    SceneBloom(t);
}

// the scene through kAoHlsl's dof into t, read from a copy of it, with the scene's depth
void D3D12Backend::SceneDof(Target& t) {
    float p[kOcclusionParams];
    Target* ds = FindTarget(scene_zs_, true);
    if (!SceneCamera(p) || !ds || ds->w != t.w || ds->h != t.h || ds->samples != t.samples) return;
    const auto& v = scene_vp_;
    float zr = v.max_z - v.min_z;
    D3D12_RECT rect = ScaledViewportRect(v, t);
    if (rect.right <= rect.left || rect.bottom <= rect.top || zr <= 0) return;
    ID3D12Resource* fb = EnsureFeedbackCopy(t);
    if (!fb) return;
    float rect_h = float(rect.bottom - rect.top);
    // opt.x: the widest blur, 1.2 % of the picture's height at full strength
    float radius = rect_h * 0.012f * float(std::min(app::Get().dof_strength, 100u)) / 100.0f;
    AoFrame f{.c = {p[0], p[1], p[2], p[3], p[4] * zr, v.min_z + p[5] * zr, 0, 0, float(rect.left), float(rect.top),
                    float(rect.right - rect.left), rect_h, radius, 0, 0, 0, 0, 0, 0, 0},
              .rect = rect,
              .vp = {0, 0, float(t.w), float(t.h), 0, 1},
              .depth_samples = ds->samples,
              .gtao = false};
    bound_rt_ = bound_ds_ = nullptr;
    Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    AoPass pass{.entry = "dof", .out = t.fmt, .samples = t.samples, .rtv = Cpu(rtv_heap_.Get(), t.view, rtv_inc_),
                .depth = ds->res.Get(), .color = fb};
    DrawAoPass(f, pass);
    t.drawn = true;
}

constexpr uint32_t kQuadVertices = 6;  // a quad drawn as two triangles

DrawShaders ClassifyShaders(const State& s, const Assembled& a, uint32_t count) {
    DrawShaders sh;
    // a shader made from a declaration alone (no program) is the fixed function pipeline on those inputs
    sh.prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    // up to a quad with no depth test: it may be a pass over the whole target, see QuadHlsl
    sh.quad = count <= kQuadVertices && !s.rs[kRsZEnable];
    sh.fixed_transform = !sh.prog && !a.pretransformed;
    if (!sh.prog) sh.fk = MakeFixedKey(s);
    return sh;
}

// the program's hash is cached in the guest shader object
void EnsureProgramHash(const State& s) {
    if (!s.vs->hash) s.vs->hash = Hash(s.vs->program.data(), s.vs->program.size() * 4) | 1;
}

// vertex shader cache key salts
constexpr uint64_t kDeclarationSalt = 0x10000;    // fixed function seed, above the 16 bit register mask
constexpr uint64_t kPretransformedSalt = 0x5A5A;  // screen space fixed function vertices, apart from untransformed
constexpr uint64_t kQuadSalt = 0xC3C30000;        // the QuadHlsl variant, apart from the plain shader
constexpr uint64_t kPerPixelSalt = 0x5050000000000000ull;  // xboxrecomp: the per pixel lighting variant

uint64_t VertexKey(const State& s, const Assembled& a, const DrawShaders& sh) {
    uint64_t seed = sh.prog ? s.vs->hash ^ a.regs : Hash(&s.vs_handle, 4, a.regs | kDeclarationSalt);
    uint64_t vkey = Hash(&a.decode, 8, seed);
    if (!sh.prog && a.pretransformed) vkey ^= kPretransformedSalt;
    if (sh.quad) vkey ^= kQuadSalt;
    if (!sh.prog) vkey = Hash(&sh.fk, sizeof(sh.fk), vkey);
    if (sh.ppl) vkey ^= kPerPixelSalt;
    return vkey;
}

std::string VertexHlsl(const State& s, const Assembled& a, const DrawShaders& sh) {
    if (sh.prog) return VertexProgramHlsl(s.vs->program, a.regs, a.decode, sh.ppl);
    return FixedVertexHlsl(a.regs, a.pretransformed, a.decode, sh.fk);
}

uint64_t D3D12Backend::EnsureVertexShader(const State& s, const Assembled& a, const DrawShaders& sh) {
    auto vit = shader_src_.find(sh.vkey);
    uint64_t vh;
    if (vit != shader_src_.end()) {
        vh = vit->second;
    } else {
        std::string src = VertexHlsl(s, a, sh);
        vh = Register(sh.quad ? QuadHlsl(std::move(src)) : src, "main", "vs_5_0");
        shader_src_[sh.vkey] = vh;
    }
    // the vertex shader's text is gone once compiled: the line shader is made from a fresh copy
    if (a.topology == Topology::Lines && LineGsOn() && !line_gs_.count(vh)) {
        std::string src = VertexHlsl(s, a, sh);
        line_gs_[vh] = Register(LineGsHlsl(sh.quad ? QuadHlsl(std::move(src)) : src), "gsline", "gs_5_0");
    }
    return vh;
}

// xboxrecomp: a vertex program's lighting done per pixel (F10 Graphics, per_pixel_lighting): programs that light
// without a0 (no matrix palette), drawn as triangles, not a screen quad
bool D3D12Backend::PerPixel(const State& s, const Assembled& a, const DrawShaders& sh) {
    if (!sh.prog || sh.quad || a.topology != Topology::Triangles || !app::Get().per_pixel_lighting) return false;
    uint64_t key = s.vs->hash ^ a.regs;
    auto it = ppl_ok_.find(key);
    if (it != ppl_ok_.end()) return it->second;
    bool ok = PerPixelLightable(s.vs->program, a.regs);
    ppl_ok_[key] = ok;
    return ok;
}

bool D3D12Backend::BindStageTexturesAndTargets(const State& s, DrawTextures& stages) {
    draw_rt_ = bound_rt_;
    draw_ds_ = bound_ds_;
    for (int i = 0; i < 4; i++) stages.res[i] = BindTexture(s, i, stages);
    // textures may have recorded copies that unbound the targets
    return BindTargets(s);
}

constexpr uint64_t kPixelKeySalt = 0x9E3779B97F4A7C15ull;

uint64_t D3D12Backend::EnsurePixelShader(const PixelKey& pk, uint64_t pkey, const PerPixelSource* ppl) {
    // pixel shaders share the map with vertex shaders, the salt keeps their keys apart
    auto pit = shader_src_.find(pkey ^ kPixelKeySalt);
    if (pit != shader_src_.end()) return pit->second;
    uint64_t ph = Register(PixelHlsl(pk, ppl), "main", "ps_5_0");
    shader_src_[pkey ^ kPixelKeySalt] = ph;
    return ph;
}

void KeyBlend(const State& s, PsoKey& k) {
    k.blend = SimpleState(s, kMBlendEnable) != 0;
    if (k.blend) {
        k.src = SimpleState(s, kMSrcBlend);
        k.dst = SimpleState(s, kMDestBlend);
        k.op = SimpleState(s, kMBlendOp);
    }
    uint32_t cw = SimpleState(s, kMColorWriteEnable);
    // the console's enables are one per byte, A R G B; D3D12 wants bits R G B A
    k.wmask = ((cw >> 16) & 1) | ((cw >> 8) & 1) << 1 | (cw & 1) << 2 | ((cw >> 24) & 1) << 3;
}

constexpr float kDepth24Max = 16777215.0f;  // 2^24 minus 1: a 24 bit depth buffer's units per 1.0

void D3D12Backend::KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k) {
    k.zen = s.rs[kRsZEnable] != 0;
    if (k.zen) {
        k.zwrite = SimpleState(s, kMZWriteEnable) != 0;
        k.zfunc = g_dbg_zalways ? 0x207u : SimpleState(s, kMZFunc);  // GL_ALWAYS
    }
    k.sten = s.rs[kRsStencilEnable] != 0;
    if (k.sten) {
        k.sfunc = SimpleState(s, kMStencilFunc);
        k.smask = SimpleState(s, kMStencilMask) & 0xFF;
        k.swmask = SimpleState(s, kMStencilWriteMask) & 0xFF;
        k.sfail = s.rs[kRsStencilFail];
        k.szfail = SimpleState(s, kMStencilZFail);
        k.spass = SimpleState(s, kMStencilPass);
    }
    // polygon offset: bias in depth buffer units, the target is always 24 bit; the slope is per console pixel,
    // and an upscaled target's pixels are smaller
    if (a.topology == Topology::Triangles && SimpleState(s, kMSolidOffsetEnable) && !UseLogDepth(s, a)) {
        k.zbias = int32_t(std::lround(Bits(SimpleState(s, kMDepthOffset)) * kDepth24Max / s.zscale));
        k.zslope = Bits(SimpleState(s, kMSlopeScale)) * (bound_ds_ ? float(bound_ds_->w) / bound_ds_->gw : 1.0f);
    }
}

void KeyRasterizer(const State& s, const Assembled& a, PsoKey& k) {
    uint32_t cull = s.rs[kRsCullMode];
    k.cull = cull != 0 && a.topology == Topology::Triangles;
    k.ccw = cull == kCullCw;  // culling clockwise leaves counter clockwise as the front
    k.fill = s.rs[kRsFillMode];
}

void D3D12Backend::BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k) {
    std::memset(&k, 0, sizeof(k));
    k.vs = vh;
    k.ps = ph;
    k.regs = a.regs;
    for (int r = 0; r < 16; r++) {
        if ((a.regs >> r) & 1) {
            const VertexElement& e = a.elements[size_t(r)];
            k.elements[r] = e.format | uint32_t(e.buffer) << 8 | uint32_t(e.offset) << 16;
        }
    }
    k.topo = uint32_t(a.topology);
    k.has_rt = bound_rt_ != nullptr;
    k.has_ds = bound_ds_ != nullptr;
    k.rtfmt = bound_rt_ ? bound_rt_->fmt : 0;
    uint32_t samples = bound_rt_ ? bound_rt_->samples : bound_ds_->samples;
    k.msaa = samples > 1 ? samples : 0;
    KeyBlend(s, k);
    if (k.has_ds) KeyDepthStencil(s, a, k);
    KeyRasterizer(s, a, k);
}

// alpha test off: compare always, in Cmp() - 1 terms like the enabled test
constexpr float kAlphaTestAlways = float(D3D12_COMPARISON_FUNC_ALWAYS - 1);

void FillSurfaceConstants(const State& s, GuestSize size, DrawConstants& c) {
    c.surf[0] = float(size.w);
    c.surf[1] = float(size.h);
    c.surf[2] = 1.0f / s.zscale;
    if (s.rs[kRsFogEnable]) {
        c.fog[0] = Bits(s.rs[kRsFogStart]);
        c.fog[1] = Bits(s.rs[kRsFogEnd]);
        c.fog[2] = Bits(s.rs[kRsFogDensity]);
        c.fog[3] = float(s.rs[kRsFogTableMode]);
    }
    Color(s.rs[kRsFogColor], c.fog_color);
    c.misc[0] = float(SimpleState(s, kMAlphaRef) & 0xFF);
    c.misc[1] = SimpleState(s, kMAlphaTestEnable) && !g_dbg_noatest ? float(Cmp(SimpleState(s, kMAlphaFunc)) - 1) : kAlphaTestAlways;
    c.misc[2] = float(Cmp(s.rs[kRsShadowFunc]) - 1);
    c.misc[3] = s.rs[kRsSpecularEnable] ? 1.0f : 0.0f;
}

void FillCombinerConstants(const State& s, DrawConstants& c) {
    for (int i = 0; i < 8; i++) {
        Color(SimpleState(s, kMPsConstant0 + i * 4), c.pc[i * 2]);
        Color(SimpleState(s, kMPsConstant1 + i * 4), c.pc[i * 2 + 1]);
    }
    Color(SimpleState(s, kMPsFinalConstant0), c.pc[16]);
    Color(SimpleState(s, kMPsFinalConstant1), c.pc[17]);
    Color(s.rs[kRsTextureFactor], c.tfactor);
}

void D3D12Backend::FillHdrConstants(const PsoKey& k, DrawConstants& c) {
    // colour lands over the headroom unless the blend multiplies it with what the target holds
    uint32_t src_factor = k.blend ? k.src : 1;
    c.hdr[0] = bound_rt_ && bound_rt_->hdr && src_factor != 0 && src_factor != kBlendDestColor ? 1 / kHeadroom : 1;
    c.hdr[1] = kHeadroom;
}

constexpr float kGlowBoxTexels = 3.0f;  // console texels across the box each wide tap averages

void D3D12Backend::FillTexelConstants(int i, const TextureDesc& d, const DrawTextures& stages, DrawConstants& c) {
    c.texsize[i][0] = float(d.width);
    c.texsize[i][1] = float(d.height);
    // the console samples pixels at integer positions, D3D12 half a pixel on: targets are read half a texel back
    if (!stages.rt[i]) return;
    c.texsize[i][2] = -0.5f / float(d.width);
    c.texsize[i][3] = -0.5f / float(d.height);
    // a 3 console texel box per tap keeps the console chain's glow width and brightness
    if (stages.wide[i] > 0) {
        float n = std::ceil(kGlowBoxTexels * stages.wide[i] * 0.5f);
        // xboxrecomp: the shader clamps the same; a width out of this range would loop it for seconds
        if (!(n >= 1 && n <= 12)) {
            LOG_FIRST(8, "d3d12: glow taps %g (scale %g) for target %08X %ux%u, clamped", n, stages.wide[i], d.data,
                      d.width, d.height);
            n = std::clamp(n, 1.0f, 12.0f);
        }
        c.spread[i][0] = kGlowBoxTexels / (n * float(d.width));
        c.spread[i][1] = kGlowBoxTexels / (n * float(d.height));
        c.spread[i][2] = n;
    }
}

void D3D12Backend::FillTextureConstants(const State& s, const DrawTextures& stages, DrawConstants& c) {
    for (int i = 0; i < 4; i++) {
        TextureDesc d;
        if (s.textures[size_t(i)] && Describe(s.textures[size_t(i)], d)) FillTexelConstants(i, d, stages, c);
        const auto& t = s.tss[size_t(i)];
        c.bump[i][0] = Bits(t[kTssBumpEnvMat00]);
        c.bump[i][1] = Bits(t[kTssBumpEnvMat01]);
        c.bump[i][2] = Bits(t[kTssBumpEnvMat10]);
        c.bump[i][3] = Bits(t[kTssBumpEnvMat11]);
        c.bumpl[i][0] = Bits(t[kTssBumpEnvLScale]);
        c.bumpl[i][1] = Bits(t[kTssBumpEnvLOffset]);
        Color(t[kTssBorderColor], c.border[i]);
    }
}

void MatMul(const float* x, const float* y, float* o) {
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) {
            o[r * 4 + col] = x[r * 4] * y[col] + x[r * 4 + 1] * y[4 + col] + x[r * 4 + 2] * y[8 + col] +
                             x[r * 4 + 3] * y[12 + col];
        }
    }
}

// HLSL mul(row vector, matrix) with column major packing wants the transpose
void Transpose(const float* m, float* o) {
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) o[col * 4 + r] = m[r * 4 + col];
    }
}

void InverseTranspose3x3(const float* wv, float* n) {
    float det = wv[0] * (wv[5] * wv[10] - wv[6] * wv[9]) - wv[1] * (wv[4] * wv[10] - wv[6] * wv[8]) +
                wv[2] * (wv[4] * wv[9] - wv[5] * wv[8]);
    float id = det != 0 ? 1.0f / det : 0.0f;
    for (int r = 0; r < 3; r++) {
        for (int col = 0; col < 3; col++) {
            int r1 = (r + 1) % 3;
            int r2 = (r + 2) % 3;
            int c1 = (col + 1) % 3;
            int c2 = (col + 2) % 3;
            n[r * 4 + col] = (wv[r1 * 4 + c1] * wv[r2 * 4 + c2] - wv[r1 * 4 + c2] * wv[r2 * 4 + c1]) * id;
        }
    }
}

void FillTransformConstants(const State& s, GuestSize size, DrawConstants& c) {
    uint32_t gw = size.w;
    uint32_t gh = size.h;
    // world, view, projection, then the viewport inside the target
    float wv[16];
    float wvp[16];
    MatMul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
    MatMul(wv, s.transforms[kTsProjection].data(), wvp);
    const auto& v = s.viewport;
    float sx = float(v.w) / gw;
    float sy = float(v.h) / gh;
    float ox = (2.0f * v.x + v.w) / gw - 1;
    float oy = 1 - (2.0f * v.y + v.h) / gh;
    float vpm[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, v.max_z - v.min_z, 0, ox, oy, v.min_z, 1};
    float full[16];
    MatMul(wvp, vpm, full);
    Transpose(full, c.wvp);
    Transpose(wv, c.wv);
    // normals take the inverse transpose of the upper 3x3
    float n[16] = {};
    InverseTranspose3x3(wv, n);
    Transpose(n, c.wvn);
    for (int t = 0; t < 4; t++) Transpose(s.transforms[kTsTexture0 + size_t(t)].data(), c.tm[t]);
}

// D3DLIGHT8 as floats: Type, Diffuse[4], Specular[4], Ambient[4], Position[3], Direction[3], Range, Falloff,
// Attenuation0 to 2, Theta, Phi
constexpr int kLightFloats = 26;
constexpr int kLightDiffuse = 1;
constexpr int kLightSpecular = 5;
constexpr int kLightAmbient = 9;
constexpr int kLightPosition = 13;
constexpr int kLightDirection = 16;
constexpr int kLightRange = 19;
constexpr int kLightFalloff = 20;
constexpr int kLightAttenuation0 = 21;
constexpr int kLightAttenuation1 = 22;
constexpr int kLightAttenuation2 = 23;
constexpr int kLightTheta = 24;
constexpr int kLightPhi = 25;

void LightToView(const std::array<uint32_t, kLightFloats>& l, const float* vm, float (&o)[7][4]) {
    float f[kLightFloats];
    std::memcpy(f, l.data(), sizeof(f));
    for (int j = 0; j < 4; j++) {
        o[0][j] = f[kLightDiffuse + j];
        o[1][j] = f[kLightSpecular + j];
        o[2][j] = f[kLightAmbient + j];
    }
    const float* pos = f + kLightPosition;
    const float* dir = f + kLightDirection;
    for (int j = 0; j < 3; j++) {
        o[3][j] = pos[0] * vm[j] + pos[1] * vm[4 + j] + pos[2] * vm[8 + j] + vm[12 + j];
        o[4][j] = dir[0] * vm[j] + dir[1] * vm[4 + j] + dir[2] * vm[8 + j];
    }
    o[3][3] = f[kLightRange];
    o[4][3] = f[kLightFalloff];
    o[5][0] = f[kLightAttenuation0];
    o[5][1] = f[kLightAttenuation1];
    o[5][2] = f[kLightAttenuation2];
    o[6][0] = std::cos(f[kLightTheta] * 0.5f);
    o[6][1] = std::cos(f[kLightPhi] * 0.5f);
}

void FillLightConstants(const State& s, const FixedKey& fk, DrawConstants& c) {
    std::memcpy(c.mat, s.material.data(), sizeof(float) * 16);
    c.mat[4][0] = s.material[16];
    Color(s.rs[kRsAmbient], c.amb);
    // lights to view space
    const float* vm = s.transforms[kTsView].data();
    for (int i = 0; i < 8; i++) {
        if (!((fk.types >> (i * 2)) & 3)) continue;
        LightToView(s.lights[size_t(i)], vm, c.lt[i]);
    }
}

void D3D12Backend::UploadConstants(const State& s, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages,
                                   GuestSize size, DrawConstants& c) {
    Alloc vc = Upload(sizeof(s.vsc), 256);
    std::memcpy(vc.cpu, s.vsc.data(), sizeof(s.vsc));
    Alloc dc = Upload(sizeof(DrawConstants), 256);
    FillSurfaceConstants(s, size, c);
    if (sh.wdepth) {
        c.wbuf[0] = 1;
        if (SimpleState(s, kMSolidOffsetEnable)) {
            c.wbuf[1] = Bits(SimpleState(s, kMDepthOffset)) / s.zscale;
            // per console pixel, and an upscaled target's pixels are smaller
            c.wbuf[2] = Bits(SimpleState(s, kMSlopeScale)) * (bound_ds_ ? float(bound_ds_->w) / bound_ds_->gw : 1.0f);
        }
    }
    FillCombinerConstants(s, c);
    FillHdrConstants(k, c);
    FillTextureConstants(s, stages, c);
    if (sh.fixed_transform) {
        FillTransformConstants(s, size, c);
        FillLightConstants(s, sh.fk, c);
    }
    std::memcpy(dc.cpu, &c, sizeof(c));
    cl_->SetGraphicsRootConstantBufferView(0, vc.gpu);
    cl_->SetGraphicsRootConstantBufferView(1, dc.gpu);
}

void D3D12Backend::CaptureTextures(const State& s, const DrawTextures& stages) {
    for (int i = 0; i < 4; i++) {
        ID3D12Resource* res = stages.res[i];
        TextureDesc d;
        if (!s.textures[size_t(i)] || !Describe(s.textures[size_t(i)], d)) continue;
        bool tg = targets_.count(TargetKey(d.data, Format(d.format).depth));
        fprintf(cap_, " | t%d %08X fmt %X %ux%u%s%s", i, d.data, d.format, d.width, d.height, tg ? " target" : "",
                res ? "" : " unbound");
        // xboxrecomp: addressing and border colour, what a sample past the edge returns
        const auto& t = s.tss[size_t(i)];
        fprintf(cap_, " addr %u %u border %08X", t[kTssAddressU], t[kTssAddressV], t[kTssBorderColor]);
        if (res && !tg && stages.info[i].kind == kTex2D && cap_seen_.insert(d.data).second) {
            cap_tex_.push_back({res, stages.sv[i].Format, d.data, d.width, d.height});
        }
    }
}

// s.vsc holds the 192 vertex shader constants in hardware order: D3D register + 96
constexpr int kVsConstants = 192;
constexpr int kVsConstantBias = 96;

void D3D12Backend::CaptureInputs(const State& s, const Assembled& a, const DrawConstants& c) {
    fprintf(cap_, "\n     verts");
    uint32_t n = 0;
    for (int r = 0; r < 16; r++) n += (a.regs >> r) & 1;
    for (size_t v = 0; v < std::min<size_t>(4, n ? a.vertices.size() / (n * 4) : 0); v++) {
        fprintf(cap_, " [");
        for (uint32_t k = 0; k < n * 4; k++) fprintf(cap_, " %g", a.vertices[v * n * 4 + k]);
        fprintf(cap_, " ]");
    }
    // xboxrecomp: raw float registers (a quad's corners) and the texture rectangles QuadHlsl clamps to
    if (a.vertices.empty() && a.vertex_count <= 6) {
        for (int r = 0; r < 16; r++) {
            const VertexElement& e = a.elements[size_t(r)];
            const VertexBuffer& vb = a.buffers[e.buffer];
            if (!(a.regs >> r & 1) || !vb.src || !vb.stride || uint64_t(a.vertex_count) * vb.stride > vb.bytes) continue;
            if (e.format != DXGI_FORMAT_R32G32_FLOAT && e.format != DXGI_FORMAT_R32G32B32_FLOAT &&
                e.format != DXGI_FORMAT_R32G32B32A32_FLOAT) {
                continue;
            }
            fprintf(cap_, "\n     v%d", r);
            for (uint32_t v = 0; v < a.vertex_count; v++) {
                float f[2];
                std::memcpy(f, vb.src + size_t(v) * vb.stride + e.offset, sizeof(f));
                fprintf(cap_, " (%g %g)", f[0], f[1]);
            }
        }
    }
    if (c.qrect[2] > c.qrect[0]) fprintf(cap_, "\n     qrect %g %g %g %g", c.qrect[0], c.qrect[1], c.qrect[2], c.qrect[3]);
    for (int t = 0; t < 4; t++) {
        if (c.uvrect[t][2] > c.uvrect[t][0]) {
            fprintf(cap_, "\n     uvrect%d %g %g %g %g", t, c.uvrect[t][0], c.uvrect[t][1], c.uvrect[t][2], c.uvrect[t][3]);
        }
    }
    for (int r = 0; r < kVsConstants; r++) {
        const auto& v = s.vsc[r];
        if (v[0] || v[1] || v[2] || v[3]) {
            fprintf(cap_, "\n     c%d %g %g %g %g", r - kVsConstantBias, v[0], v[1], v[2], v[3]);
        }
    }
    fprintf(cap_, "\n     pc");
    for (size_t r = 0; r < std::size(c.pc); r++) {
        fprintf(cap_, " (%.3f %.3f %.3f %.3f)", c.pc[r][0], c.pc[r][1], c.pc[r][2], c.pc[r][3]);
    }
}

// the draw's line: its state, then its textures
void D3D12Backend::CaptureDraw(const State& s, uint32_t prim, uint32_t count, const PsoKey& k, const DrawConstants& c,
                               const DrawTextures& stages) {
    fprintf(cap_,
            "%04u draw rt %08X ds %08X prim %u count %u vs %08X ps %08X blend %u %X %X zen %u %X zwrite %u "
            "zbias %d %.2f cull %u cw %X c0 %.3f %.3f %.3f %.3f tf %08X atest %g %g fog %g %g %g %g fc %.3f %.3f %.3f",
            cap_i_++, s.rt.data, bound_ds_ ? s.zs.data : 0, prim, count, s.vs_handle, s.ps ? s.ps->handle : 0, k.blend,
            k.src, k.dst, k.zen, k.zfunc, k.zwrite, k.zbias, k.zslope, k.cull, k.wmask, c.pc[0][0], c.pc[0][1],
            c.pc[0][2], c.pc[0][3], s.rs[kRsTextureFactor], c.misc[1], c.misc[0], c.fog[0], c.fog[1], c.fog[2],
            c.fog[3], c.fog_color[0], c.fog_color[1], c.fog_color[2]);
    if (s.callers[0]) {
        fprintf(cap_, " | from");
        for (uint32_t r : s.callers)
            if (r) fprintf(cap_, " %08X", r);
    }
    CaptureTextures(s, stages);
}

// a cap_sh_ entry apart from the shader keys: this vertex program's inputs were captured
constexpr uint64_t kInputsCapturedTag = 0xC0;

// small draws, and a vertex program's first draw: its inputs and constants; then ends the draw's line
void D3D12Backend::EndDrawCapture(const State& s, const Assembled& a, uint32_t count, bool prog,
                                  const DrawConstants& c) {
    uint64_t inputs_key = uint64_t(s.vs_handle) << 32 | kInputsCapturedTag;
    if (count <= kQuadVertices || (prog && cap_sh_.insert(inputs_key).second)) CaptureInputs(s, a, c);
    // xboxrecomp: the raw inputs of a programmable draw (the converted path above is empty when the input
    // assembler reads the title's buffers as they are): per register its format, buffer and offset, and the
    // first three vertices as floats
    if (prog && a.vertices.empty()) {
        fprintf(cap_, "\n     raw draw_count %u index_count %u base %d", a.draw_count, a.index_count, a.base_vertex);
        // xboxrecomp: the draw's bytes, to run its vertex program offline: each vertex buffer as read
        // (vb_<draw>_<buffer>.bin), the 16-bit indices (ib_<draw>.bin) and the 192 constants in hardware order
        // (vc_<draw>.bin). Every capture keeps them for draws of up to 64 KB (the stair's window beams: their fog
        // comes from the program and its constants); RECOMP_CAP_DRAWS keeps them all
        size_t bytes = size_t(a.index_count) * 2;
        for (uint32_t b = 0; b < a.buffer_count; b++) bytes += a.buffers[b].bytes;
        if (cap_draws_ || bytes <= 64 * 1024) {
            auto dump = [&](const char* name, const void* p, size_t n) {
                if (!p || !n) return;
                if (FILE* f = _wfopen((cap_dir_ / name).c_str(), L"wb")) {
                    fwrite(p, 1, n, f);
                    fclose(f);
                }
            };
            char name[48];
            for (uint32_t b = 0; b < a.buffer_count; b++) {
                snprintf(name, sizeof(name), "vb_%04u_%u.bin", cap_i_ - 1, b);
                dump(name, a.buffers[b].src, a.buffers[b].bytes);
            }
            snprintf(name, sizeof(name), "ib_%04u.bin", cap_i_ - 1);
            dump(name, a.index_src, size_t(a.index_count) * 2);
            snprintf(name, sizeof(name), "vc_%04u.bin", cap_i_ - 1);
            dump(name, s.vsc.data(), sizeof(s.vsc));
        }
        for (int r = 0; r < 16; r++) {
            if (!((a.regs >> r) & 1)) continue;
            const VertexElement& e = a.elements[size_t(r)];
            const VertexBuffer& b = a.buffers[e.buffer];
            fprintf(cap_, "\n     v%d fmt %u buf %u off %u stride %u bytes %u decode %X:", r, e.format, e.buffer,
                    e.offset, b.stride, b.bytes, unsigned(a.decode >> (r * 4)) & 0xF);
            for (uint32_t v = 0; v < 3 && b.src && (v + 1) * b.stride <= b.bytes; v++) {
                float f[4];
                std::memcpy(f, b.src + v * b.stride + e.offset, sizeof(f));
                fprintf(cap_, " [%g %g %g %g]", f[0], f[1], f[2], f[3]);
            }
        }
    }
    fputc('\n', cap_);
}

// each shader once, as translated
void D3D12Backend::CaptureShader(const char* kind, uint64_t key, const std::string& src) {
    if (!cap_sh_.insert(key).second) return;
    char name[48];
    snprintf(name, sizeof(name), "%s_%016llX.hlsl", kind, (unsigned long long)key);
    if (FILE* f = _wfopen((cap_dir_ / name).c_str(), L"w")) {
        fwrite(src.data(), 1, src.size(), f);
        fclose(f);
    }
    fprintf(cap_, "     %s\n", name);
}

void D3D12Backend::CaptureShaders(const State& s, const Assembled& a, const DrawShaders& sh, const PixelKey& pk) {
    std::string src = VertexHlsl(s, a, sh);
    CaptureShader("vs", sh.vkey, sh.quad ? QuadHlsl(std::move(src)) : src);
    PerPixelSource pps{sh.prog ? &s.vs->program : nullptr, a.regs};
    CaptureShader("ps", sh.pkey, PixelHlsl(pk, sh.ppl ? &pps : nullptr));
}

D3D12_SRV_DIMENSION NullViewDimension(uint8_t kind) {
    if (kind == kTexCube) return D3D12_SRV_DIMENSION_TEXTURECUBE;
    if (kind == kTex3D) return D3D12_SRV_DIMENSION_TEXTURE3D;
    return D3D12_SRV_DIMENSION_TEXTURE2D;
}

// A small draw whose every float2 input lies in 0..1: sprites, HUD pieces, text, full screen passes. The console's
// pixel centres sample such a quad from its first texel to half a texel short of its far edge; upscaled, the far
// edge's last half texel is drawn too, and wrapped it blends in the opposite edge (the crosshair ticks' dark lines).
// Clamped, it takes the edge texel instead, which a wrapped texture used once over 0..1 has there anyway.
// xboxrecomp: also draws converted on the CPU (their elements are float4) and larger ones: a sky's panels each map their
// texture once over 0..1, and wrapped, the filter (16x anisotropic upscaled) drew the far edge in as a line along every
// panel's seam.
bool UnitTexcoords(const Assembled& a) {
    constexpr float kLo = -1e-4f;
    constexpr float kHi = 1.0001f;
    constexpr uint32_t kSmallDrawVertices = 16384;  // most vertices a draw has for this
    if (!a.vertex_count || a.vertex_count > kSmallDrawVertices) return false;
    bool any = false;
    for (int r = 0; r < 16; r++) {
        const VertexElement& e = a.elements[size_t(r)];
        if (!(a.regs >> r & 1)) continue;
        // float2, or the converted path's float4 (any element but the position in r 0 that leaves 0..1, a normal
        // say, rules the draw out)
        if (r == 0 || (e.format != DXGI_FORMAT_R32G32_FLOAT && e.format != DXGI_FORMAT_R32G32B32A32_FLOAT)) continue;
        const VertexBuffer& vb = a.buffers[e.buffer];
        if (!vb.src || !vb.stride || uint64_t(a.vertex_count) * vb.stride > vb.bytes) return false;
        for (uint32_t v = 0; v < a.vertex_count; v++) {
            float uv[2];
            std::memcpy(uv, vb.src + size_t(v) * vb.stride + e.offset, sizeof(uv));
            if (!(uv[0] >= kLo && uv[0] <= kHi && uv[1] >= kLo && uv[1] <= kHi)) return false;
        }
        any = true;
    }
    return any;
}

// xboxrecomp: the texture rectangle (u0 v0 u1 v1) of input register r over a draw's vertices, for QuadHlsl's qclamp
bool RegisterRect(const Assembled& a, int r, float out[4]) {
    if (!(a.regs >> r & 1) || !a.vertex_count) return false;
    const VertexElement& e = a.elements[size_t(r)];
    if (e.format != DXGI_FORMAT_R32G32_FLOAT && e.format != DXGI_FORMAT_R32G32B32_FLOAT &&
        e.format != DXGI_FORMAT_R32G32B32A32_FLOAT) {
        return false;
    }
    const VertexBuffer& vb = a.buffers[e.buffer];
    if (!vb.src || !vb.stride || uint64_t(a.vertex_count) * vb.stride > vb.bytes) return false;
    out[0] = out[1] = 1e30f;
    out[2] = out[3] = -1e30f;
    for (uint32_t v = 0; v < a.vertex_count; v++) {
        float uv[2];
        std::memcpy(uv, vb.src + size_t(v) * vb.stride + e.offset, sizeof(uv));
        if (!(std::isfinite(uv[0]) && std::isfinite(uv[1]))) return false;
        out[0] = std::min(out[0], uv[0]);
        out[1] = std::min(out[1], uv[1]);
        out[2] = std::max(out[2], uv[0]);
        out[3] = std::max(out[3], uv[1]);
    }
    return out[2] > out[0] && out[3] > out[1];
}

// xboxrecomp: for a vertex program, the input register each oT is a plain copy of (the title's 2D programs:
// mov oT0, v4), -1 where it is computed; looked at once per program
std::array<int, 5> CopiedTexcoords(const State& s, const Assembled& a, const DrawShaders& sh) {
    static std::unordered_map<uint64_t, std::array<int, 5>> known;
    auto it = known.find(sh.vkey);
    if (it != known.end()) return it->second;
    std::array<int, 5> r{-1, -1, -1, -1, -1};
    std::string src = VertexHlsl(s, a, sh);
    for (int k = 0; k < 5; k++) {  // oT0 to oT3, then oPos
        std::string write = k < 4 ? "        oT" + std::to_string(k) + "." : std::string("        oPos."),
                    whole = write + "xyzw = mr.xyzw;\n";
        size_t at = src.find(write);
        if (at == std::string::npos || src.find(write, at + 1) != std::string::npos) continue;  // none, or several
        if (src.compare(at, whole.size(), whole) != 0) continue;
        size_t line = src.rfind("precise float4 mr = ((i.v", at);
        if (line == std::string::npos) continue;
        int reg = -1;
        char tail[16] = {};
        if (std::sscanf(src.c_str() + line, "precise float4 mr = ((i.v%d).%15[^)]", &reg, tail) == 2 &&
            std::strcmp(tail, "xyzw") == 0 && src.find('\n', line) + 1 == at) {
            r[size_t(k)] = reg;
        }
    }
    known.emplace(sh.vkey, r);
    if (r[0] >= 0 || r[1] >= 0 || r[4] >= 0) {
        LOG_FIRST(16, "d3d12: quad program %016llX copies texcoords from v%d v%d v%d v%d, position from v%d",
                  (unsigned long long)sh.vkey, r[0], r[1], r[2], r[3], r[4]);
    }
    return r;
}

// xboxrecomp: a single quad whose coordinates reach the pixel shader unchanged (fixed function, or a program that
// copies them): each stage's texture rectangle, so the pixel shader keeps to the part the console samples
// (QuadHlsl's qclamp)
void QuadTextureRects(const State& s, const Assembled& a, const DrawShaders& sh, DrawConstants& c) {
    if (!sh.quad) return;
    if (sh.prog) {
        std::array<int, 5> reg = CopiedTexcoords(s, a, sh);
        for (int t = 0; t < 4; t++) {
            if (reg[size_t(t)] >= 0 && !RegisterRect(a, reg[size_t(t)], c.uvrect[t]))
                std::memset(c.uvrect[t], 0, sizeof(c.uvrect[t]));
        }
        // a program's oPos is in screen space on the console: copied, it is the quad's pixel rectangle
        if (reg[4] >= 0 && !RegisterRect(a, reg[4], c.qrect)) std::memset(c.qrect, 0, sizeof(c.qrect));
    } else if (a.pretransformed && !RegisterRect(a, 0, c.qrect)) {
        std::memset(c.qrect, 0, sizeof(c.qrect));
    }
    if (sh.prog) return;
    for (int t = 0; t < 4; t++) {
        uint32_t tci = s.tss[size_t(t)][kTssTexCoordIndex], ttf = s.tss[size_t(t)][kTssTextureTransformFlags];
        if (!a.pretransformed && ((tci >> 16) || ttf)) continue;  // generated or transformed coordinates
        if (!RegisterRect(a, 9 + int(tci & 3), c.uvrect[t])) std::memset(c.uvrect[t], 0, sizeof(c.uvrect[t]));
    }
}

// xboxrecomp: true 16:9. The title's widescreen camera (hle_video.c IsWidescreen) draws the scene anamorphically into
// its 640x480 frame, which is shown 16:9 wide; 2D laid out for 4:3 (HUD, text, menus) is drawn 3/4 as wide about the
// centre so it keeps its shape. A 2D draw is a pretransformed one or a program that copies its position (screen
// space on the console); one spanning the frame's width (backdrops, fades, full screen passes) still fills it.
void Squeeze2D(const State& s, const Assembled& a, const DrawShaders& sh, uint32_t frame_w, DrawConstants& c) {
    if (app::BootAspect() != app::AspectMode::Wide) return;
    int reg = -1;
    if (sh.prog) reg = CopiedTexcoords(s, a, sh)[4];
    else if (a.pretransformed) reg = 0;
    float r[4];
    bool flat = reg >= 0 && RegisterRect(a, reg, r);  // a 2D draw, r its rectangle
    // a backdrop across the frame fills the 16:9 width, on a 4:3 screen too (the inventory's blue mist)
    if (flat && r[0] <= 1.0f && r[2] >= float(frame_w) - 1.0f) return;
    // a 4:3 screen (hle_d3d8.cpp's Swap) narrows everything else in the frame, 3D too; otherwise only the 2D
    if (!s.pillar && !flat) return;
    c.squeeze[0] = 0.75f;
    c.squeeze[1] = float(frame_w) * 0.5f;
}

// a stage without a texture gets a null view of its kind; the samplers read the views after
void D3D12Backend::SetTextureViewsAndSamplers(const State& s, const Assembled& a, DrawTextures& stages) {
    for (int i = 0; i < 4; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC& sv = stages.sv[i];
        if (!stages.res[i]) {
            sv.ViewDimension = NullViewDimension(stages.info[i].kind);
            sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        }
    }
    // xboxrecomp: the previous draw's views when they are the same: each view is an image view vkd3d creates, and
    // runs of draws sharing their textures (HP:CoS's falling cards, ~1300 a frame) made thousands a frame
    SrvTable& last = last_srv_;
    static const bool reuse = !(std::getenv("RECOMP_SRV_REUSE") && *std::getenv("RECOMP_SRV_REUSE") == '0');
    bool same = reuse && last.valid && last.frame == frame_ && std::memcmp(last.res, stages.res, sizeof(last.res)) == 0 &&
                std::memcmp(last.sv, stages.sv, sizeof(last.sv)) == 0;
    if (!same) {
        last.slot = AllocSrvSlots(4);
        for (int i = 0; i < 4; i++)
            dev_->CreateShaderResourceView(stages.res[i], &stages.sv[i], Cpu(srv_heap_.Get(), last.slot + i, srv_inc_));
        std::memcpy(last.res, stages.res, sizeof(last.res));
        std::memcpy(last.sv, stages.sv, sizeof(last.sv));
        last.frame = frame_;
        last.valid = true;
    }
    cl_->SetGraphicsRootDescriptorTable(2, Gpu(srv_heap_.Get(), last.slot, srv_inc_));
    uint32_t samplers = EnsureStageSamplers(s, stages, UnitTexcoords(a));
    cl_->SetGraphicsRootDescriptorTable(3, Gpu(smp_heap_.Get(), samplers, smp_inc_));
}

// viewport covers the target, the guest viewport is in the shader; scissor clips to it
void D3D12Backend::SetViewportScissor(const State& s, uint32_t gw) {
    float W = float(bound_rt_ ? bound_rt_->w : bound_ds_->w);
    float H = float(bound_rt_ ? bound_rt_->h : bound_ds_->h);
    D3D12_VIEWPORT vp{0, 0, W, H, 0, 1};
    cl_->RSSetViewports(1, &vp);
    float sc = W / gw;
    LONG x0 = LONG(s.viewport.x * sc);
    LONG y0 = LONG(s.viewport.y * sc);
    LONG x1 = LONG((s.viewport.x + s.viewport.w) * sc);
    LONG y1 = LONG((s.viewport.y + s.viewport.h) * sc);
    if (s.scissor_count) {
        x0 = std::max(x0, LONG(s.scissors[0] * sc));
        y0 = std::max(y0, LONG(s.scissors[1] * sc));
        x1 = std::min(x1, LONG(s.scissors[2] * sc));
        y1 = std::min(y1, LONG(s.scissors[3] * sc));
    }
    D3D12_RECT rect{std::clamp<LONG>(x0, 0, LONG(W)), std::clamp<LONG>(y0, 0, LONG(H)),
                    std::clamp<LONG>(x1, 0, LONG(W)), std::clamp<LONG>(y1, 0, LONG(H))};
    cl_->RSSetScissorRects(1, &rect);
}

void D3D12Backend::SetBlendFactorStencilRef(const State& s) {
    float bf[4];
    Color(SimpleState(s, kMBlendColor), bf);
    cl_->OMSetBlendFactor(bf);
    cl_->OMSetStencilRef(SimpleState(s, kMStencilRef) & 0xFF);
}

D3D_PRIMITIVE_TOPOLOGY PrimitiveTopology(const Assembled& a) {
    if (a.topology == Topology::Points) return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    if (a.topology == Topology::Lines) {
        return a.strip ? D3D_PRIMITIVE_TOPOLOGY_LINESTRIP : D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    }
    return a.strip ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
}

// vb_slot / vb_src: that buffer's bytes taken from vb_src instead (DrawMouthShrunk's changed copy)
void D3D12Backend::UploadVertexBuffers(const Assembled& a, int vb_slot, const uint8_t* vb_src) {
    D3D12_VERTEX_BUFFER_VIEW vbv[16];
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        const VertexBuffer& b = a.buffers[i];
        Alloc vb = Upload(b.bytes, 16);
        std::memcpy(vb.cpu, int(i) == vb_slot ? vb_src : b.src, b.bytes);
        vbv[i] = {vb.gpu, b.bytes, b.stride};
    }
    cl_->IASetVertexBuffers(0, a.buffer_count, vbv);
}

void D3D12Backend::DrawGeometry(const Assembled& a, int vb_slot, const uint8_t* vb_src) {
    UploadVertexBuffers(a, vb_slot, vb_src);
    cl_->IASetPrimitiveTopology(PrimitiveTopology(a));
    if (a.index_count) {
        uint32_t ibytes = a.index_count * (a.index16 ? 2 : 4);
        Alloc ib = Upload(ibytes, 4);
        std::memcpy(ib.cpu, a.index_src, ibytes);
        D3D12_INDEX_BUFFER_VIEW ibv{ib.gpu, ibytes, a.index16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT};
        cl_->IASetIndexBuffer(&ibv);
        cl_->DrawIndexedInstanced(a.index_count, 1, 0, a.base_vertex, 0);
    } else {
        cl_->DrawInstanced(a.draw_count, 1, 0, 0);
    }
}

// xboxrecomp: the characters' heads (the skinning program 01181221) hang the chin and the corners of the lower lip on
// the jaw bones and everything inside the mouth, the lower teeth too, on the head bone. When the lip sync closes the
// jaw the chin moves up and back through the lower teeth, and gums, teeth and the dark of the mouth show through the
// skin below the lip (Hermione in the cutscenes). Both renderers draw it so; the bones match the Xbox code.
// A head draw is split here: triangles with a vertex on a bone other than the draw's main one (the jaw) are drawn
// RECOMP_FACE_PULL percent nearer in depth (default 0 = as the Xbox, off), so they win over what sits just behind them.
// Only draws whose main bone carries most vertices (a head, not a body) and that use the log depth scale.
bool D3D12Backend::DrawFacePulled(const State& s, const Assembled& a, const DrawConstants& c) {
    static float pull = -1;
    if (pull < 0) {
        const char* e = std::getenv("RECOMP_FACE_PULL");
        pull = e && *e ? std::max(0.0f, float(std::atof(e))) : 0.0f;
    }
    if (pull <= 0 || s.vs_handle != 0x01181221 || c.wbuf[0] == 0 || a.topology != Topology::Triangles ||
        a.index_count < 3 || !a.index_src)
        return false;
    const VertexElement &w1 = a.elements[5], &w2 = a.elements[6], &bi = a.elements[7];
    if (!(a.regs & (1 << 5)) || !(a.regs & (1 << 6)) || !(a.regs & (1 << 7)) || w1.format != DXGI_FORMAT_R32_FLOAT ||
        w2.format != DXGI_FORMAT_R32_FLOAT || bi.format != DXGI_FORMAT_B8G8R8A8_UNORM || w1.buffer != bi.buffer ||
        w2.buffer != bi.buffer)
        return false;
    const VertexBuffer& vb = a.buffers[bi.buffer];
    auto index = [&](uint32_t i) -> uint32_t {
        return a.index16 ? static_cast<const uint16_t*>(a.index_src)[i] : static_cast<const uint32_t*>(a.index_src)[i];
    };
    auto vertex = [&](uint32_t i, const VertexElement& e) -> const uint8_t* {
        uint64_t off = uint64_t(int64_t(index(i)) + a.base_vertex) * vb.stride + e.offset;
        return off + 4 <= vb.bytes ? vb.src + off : nullptr;
    };
    // the main bone: the first index byte of most vertices
    uint32_t counts[256] = {};
    for (uint32_t i = 0; i < a.index_count; i++)
        if (const uint8_t* b = vertex(i, bi)) counts[b[0]]++;
    uint32_t main = uint32_t(std::max_element(counts, counts + 256) - counts);
    if (counts[main] * 10 < a.index_count * 7) return false;
    auto jaw = [&](uint32_t i) {
        const uint8_t* b = vertex(i, bi);
        const uint8_t* x = vertex(i, w1);
        const uint8_t* y = vertex(i, w2);
        if (!b || !x || !y) return false;
        float f1, f2;
        std::memcpy(&f1, x, 4), std::memcpy(&f2, y, 4);
        return (b[0] != main && f1 > 0.01f) || (b[1] != main && f2 > 0.01f);
    };
    // the strip as a list, winding kept, split in two
    std::vector<uint32_t> rest, pulled;
    uint32_t n = a.strip ? a.index_count - 2 : a.index_count / 3;
    for (uint32_t t = 0; t < n; t++) {
        uint32_t i0 = a.strip ? t : t * 3, i1 = i0 + 1, i2 = i0 + 2;
        if (a.strip && (t & 1)) std::swap(i0, i1);
        uint32_t v0 = index(i0), v1 = index(i1), v2 = index(i2);
        if (a.strip && (v0 == v1 || v1 == v2 || v0 == v2)) continue;
        auto& out = jaw(i0) || jaw(i1) || jaw(i2) ? pulled : rest;
        out.insert(out.end(), {v0, v1, v2});
    }
    if (pulled.empty()) return false;
    LOG_FIRST(4, "d3d12: head draw split, %zu of %u triangles on the jaw drawn %.1f%% nearer", pulled.size() / 3,
              n, pull);

    UploadVertexBuffers(a);
    cl_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    auto draw = [&](const std::vector<uint32_t>& list) {
        if (list.empty()) return;
        uint32_t ibytes = uint32_t(list.size() * 4);
        Alloc ib = Upload(ibytes, 4);
        std::memcpy(ib.cpu, list.data(), ibytes);
        D3D12_INDEX_BUFFER_VIEW ibv{ib.gpu, ibytes, DXGI_FORMAT_R32_UINT};
        cl_->IASetIndexBuffer(&ibv);
        cl_->DrawIndexedInstanced(uint32_t(list.size()), 1, 0, a.base_vertex, 0);
    };
    draw(rest);
    // log2(1 + w) / 32 depth: a fraction nearer in w is a constant step
    DrawConstants nearer = c;
    nearer.wbuf[1] -= std::log2(1 + pull / 100) / 32;
    Alloc dc = Upload(sizeof(DrawConstants), 256);
    std::memcpy(dc.cpu, &nearer, sizeof(nearer));
    cl_->SetGraphicsRootConstantBufferView(1, dc.gpu);
    draw(pulled);
    return true;
}

// xboxrecomp: the cutscene heads of Hermione and Ron (the skinning program 01181221 on a 128 x 128 face picture, each
// drawn as two parts whose index counts no other head shares) hold their teeth, the roof of the mouth and the tongue on
// the head bone, textured from patches in the picture's corners (teeth and roof top left, tongue top right). The chin
// and lower lip ride the jaw, and closing it moves them through those. The triangles made only of vertices from those
// patches are drawn RECOMP_MOUTH_SCALE percent of their size (default 95, 100 = as the Xbox) about the middle of their
// box: deeper in the mouth and smaller. Skin and lips are untouched.
struct MouthPatch { float x0, x1, y0, y1; };  // texels of the 128 x 128 face picture
struct MouthHead {
    const char* name;
    uint32_t counts[2];
    MouthPatch patches[3];
    uint32_t eyes[2] = {};  // the eyeball meshes' index counts, drawn after the head: shrunk whole (DrawEyesShrunk)
};
constexpr MouthHead kMouthHeads[] = {
    {"Hermione", {1003, 713}, {{0, 31, 0, 31}, {100, 128, 0, 25}, {0, 0, -1, -1}}},
    // teeth, the roof below them, tongue
    {"Ron", {691, 933}, {{0, 32, 0, 17}, {0, 19, 0, 39}, {104, 128, 0, 30}}},
    // the mouth's inside is one dark red patch, no teeth of their own (face 81B70C80 in the Chamber, capture 140303)
    // his eyeballs (41 and 43 indices, two meshes an eye, y 1.62..1.65) poke through the lids when he looks down in the
    // last cutscene (capture 154507, draws 0082-0085)
    {"Tom Riddle", {935, 615}, {{100, 128, 0, 30}, {0, 0, -1, -1}, {0, 0, -1, -1}}, {41, 43}},
};

// xboxrecomp debug: a head mesh (the skinning program, indexed, a square face texture) not in kMouthHeads, logged
// once per face texture and mesh; RECOMP_HEAD_CAPTURE=1 also captures the frame (up to 8 a run), so a character's
// mouth can be found without anyone pressing F12
void D3D12Backend::NoteHead(const State& s, const Assembled& a) {
    TextureDesc d;
    if (s.vs_handle != 0x01181221 || !a.index_src || a.topology != Topology::Triangles || !s.textures[0] ||
        !Describe(s.textures[0], d) || d.width != d.height || (d.width != 128 && d.width != 256))
        return;
    static std::unordered_set<uint64_t> seen;
    static int captures = 0;
    static const bool capture = GetEnvironmentVariableW(L"RECOMP_HEAD_CAPTURE", nullptr, 0) != 0;
    if (seen.size() > 256 || !seen.insert(uint64_t(d.data) << 32 | a.index_count).second) return;
    bool cap = capture && captures < 8 && !cap_;
    LOGI("d3d12: head mesh %u indices, face %08X %ux%u%s", a.index_count, d.data, d.width, d.height,
         cap ? ", frame captured" : "");
    if (cap) {
        captures++;
        g_capture = true;
    }
}

extern "C" volatile int hpcos_chamber_finale;  // hle_debug.c: the Chamber's last cutscene is playing

// xboxrecomp: Tom Riddle's face meshes (935 and 615 indices) drawn double-sided in the Chamber's last cutscene. Bending
// his head down folds face triangles over (the skinning turns them away from the camera) and the back-face cull left
// holes there, through which his eyeballs and the inside of his mouth showed; the Xbox does the same.
// RECOMP_FACE_TWO_SIDED=1 turns it on (an experiment that did not help; kept for the next look)
bool FaceTwoSided(const State& s, const Assembled& a) {
    static int on = -1;
    if (on < 0) {
        const char* e = std::getenv("RECOMP_FACE_TWO_SIDED");
        on = e && *e == '1';  // off: tried 2026-10-22, the holes stayed (not the cull)
    }
    if (!on || !hpcos_chamber_finale || s.vs_handle != 0x01181221 || (a.index_count != 935 && a.index_count != 615))
        return false;
    LOG_FIRST(4, "d3d12: Tom Riddle's face (%u indices) drawn double-sided", a.index_count);
    return true;
}

// xboxrecomp: an eyeball mesh of the head drawn just before (MouthHead::eyes), every vertex it uses drawn at
// RECOMP_EYE_SCALE % (default 100: off, the face drawn double-sided covers them) about its box centre: the lids keep it covered when the head bends down. Only in
// the Chamber's last cutscene (hpcos_chamber_finale), where Riddle looks down
bool D3D12Backend::DrawEyesShrunk(const State& s, const Assembled& a) {
    if (!hpcos_chamber_finale) return false;
    static float scale = -1;
    if (scale < 0) {
        const char* e = std::getenv("RECOMP_EYE_SCALE");
        scale = (e && *e ? std::clamp(float(std::atof(e)), 0.0f, 100.0f) : 100.0f) / 100;
    }
    if (scale >= 1 || !a.index_src || a.topology != Topology::Triangles || !(a.regs & 1)) return false;
    const VertexElement& p = a.elements[0];
    if (p.format != DXGI_FORMAT_R32G32B32_FLOAT) return false;
    const VertexBuffer& vb = a.buffers[p.buffer];
    std::vector<uint64_t> offs;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (uint32_t i = 0; i < a.index_count; i++) {
        uint32_t ix = a.index16 ? static_cast<const uint16_t*>(a.index_src)[i] : static_cast<const uint32_t*>(a.index_src)[i];
        uint64_t off = uint64_t(int64_t(ix) + a.base_vertex) * vb.stride + p.offset;
        if (off + 12 > vb.bytes || std::find(offs.begin(), offs.end(), off) != offs.end()) continue;
        offs.push_back(off);
        float f[3];
        std::memcpy(f, vb.src + off, 12);
        for (int j = 0; j < 3; j++) lo[j] = std::min(lo[j], f[j]), hi[j] = std::max(hi[j], f[j]);
    }
    if (offs.empty()) return false;
    std::vector<uint8_t> copy(vb.src, vb.src + vb.bytes);
    for (uint64_t off : offs) {
        float f[3];
        std::memcpy(f, copy.data() + off, 12);
        for (int j = 0; j < 3; j++) {
            float mid = (lo[j] + hi[j]) / 2;
            f[j] = mid + (f[j] - mid) * scale;
        }
        std::memcpy(copy.data() + off, f, 12);
    }
    LOG_FIRST(8, "d3d12: %s's eye (%u indices), %zu vertices drawn at %.0f%%, box x %.3f..%.3f y %.3f..%.3f z %.3f..%.3f",
              eyes_of_->name, a.index_count, offs.size(), scale * 100, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
    DrawGeometry(a, p.buffer, copy.data());
    return true;
}

bool D3D12Backend::DrawMouthShrunk(const State& s, const Assembled& a) {
    static float scale = -1;
    if (scale < 0) {
        const char* e = std::getenv("RECOMP_MOUTH_SCALE");
        scale = (e && *e ? std::clamp(float(std::atof(e)), 0.0f, 100.0f) : 95.0f) / 100;
    }
    TextureDesc d;
    const MouthHead* head = nullptr;
    for (const MouthHead& h : kMouthHeads)
        if (a.index_count == h.counts[0] || a.index_count == h.counts[1]) head = &h;
    if (s.vs_handle != 0x01181221) eyes_of_ = nullptr;
    if (head) eyes_of_ = head;
    if (!head && eyes_of_ && (a.index_count == eyes_of_->eyes[0] || a.index_count == eyes_of_->eyes[1]))
        return DrawEyesShrunk(s, a);
    if (!head) NoteHead(s, a);
    if (scale >= 1 || !head || s.vs_handle != 0x01181221 || !a.index_src ||
        a.topology != Topology::Triangles || !(a.regs & (1 << 0)) || !(a.regs & (1 << 4)) || !s.textures[0] ||
        !Describe(s.textures[0], d) || d.width != 128 || d.height != 128)
        return false;
    const VertexElement &p = a.elements[0], &t = a.elements[4];
    if (p.format != DXGI_FORMAT_R32G32B32_FLOAT || t.format != DXGI_FORMAT_R32G32_FLOAT || p.buffer != t.buffer)
        return false;
    const VertexBuffer& vb = a.buffers[p.buffer];
    auto index = [&](uint32_t i) -> uint32_t {
        return a.index16 ? static_cast<const uint16_t*>(a.index_src)[i] : static_cast<const uint32_t*>(a.index_src)[i];
    };
    auto base = [&](uint32_t i) { return uint64_t(int64_t(index(i)) + a.base_vertex) * vb.stride; };
    auto inside = [&](uint32_t i) {
        uint64_t off = base(i) + t.offset;
        if (off + 8 > vb.bytes || base(i) + p.offset + 12 > vb.bytes) return false;
        float uv[2];
        std::memcpy(uv, vb.src + off, 8);
        float x = (uv[0] - std::floor(uv[0])) * 128, y = (uv[1] - std::floor(uv[1])) * 128;
        for (const MouthPatch& m : head->patches)
            if (x >= m.x0 && x <= m.x1 && y >= m.y0 && y <= m.y1) return true;
        return false;
    };
    // the vertices of the triangles wholly inside the mouth, each once, and their box
    std::vector<uint64_t> offs;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    uint32_t n = a.strip ? a.index_count - 2 : a.index_count / 3;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t i0 = a.strip ? k : k * 3;
        if (!inside(i0) || !inside(i0 + 1) || !inside(i0 + 2)) continue;
        for (uint32_t i = i0; i < i0 + 3; i++) {
            uint64_t off = base(i) + p.offset;
            if (std::find(offs.begin(), offs.end(), off) != offs.end()) continue;
            offs.push_back(off);
            float f[3];
            std::memcpy(f, vb.src + off, 12);
            for (int j = 0; j < 3; j++) lo[j] = std::min(lo[j], f[j]), hi[j] = std::max(hi[j], f[j]);
        }
    }
    if (offs.empty()) return false;
    std::vector<uint8_t> copy(vb.src, vb.src + vb.bytes);
    for (uint64_t off : offs) {
        float f[3];
        std::memcpy(f, copy.data() + off, 12);
        for (int j = 0; j < 3; j++) {
            float mid = (lo[j] + hi[j]) / 2;
            f[j] = mid + (f[j] - mid) * scale;
        }
        std::memcpy(copy.data() + off, f, 12);
    }
    LOG_FIRST(8, "d3d12: %s's mouth (%u indices), %zu vertices inside it drawn at %.0f%%, box x %.3f..%.3f y %.3f..%.3f "
              "z %.3f..%.3f", head->name, a.index_count, offs.size(), scale * 100, lo[0], hi[0], lo[1], hi[1], lo[2],
              hi[2]);
    DrawGeometry(a, p.buffer, copy.data());
    return true;
}

// xboxrecomp: why draws were left out, logged every 10 s while any are (a missing floor or wall is one of these)
namespace {
enum DropReason { kDropAssemble, kDropTargets, kDropHdr, kDropTextures, kDropPipeline, kDropReasons };
uint64_t g_drops[kDropReasons], g_draws;
// xboxrecomp: [LEVELSTART] per frame for 3 s after a loading pause: draws asked, dropped, the last colour clear
uint32_t g_ls_draws, g_ls_drops, g_ls_clears, g_ls_clear_color, g_ls_clear_rt;

void CountDrop(int why) {
    static auto t0 = std::chrono::steady_clock::now();
    if (why >= 0) g_drops[why]++;
    if (why >= 0) g_ls_drops++;
    if (why == kDropPipeline) g_hitch_pso_drops++;
    g_draws++;
    auto now = std::chrono::steady_clock::now();
    if (now - t0 < std::chrono::seconds(10)) return;
    t0 = now;
    uint64_t any = 0;
    for (uint64_t d : g_drops) any += d;
    if (any)
        LOGI("d3d12: %llu draws, left out: %llu assembly, %llu targets, %llu hdr, %llu textures, %llu pipeline",
             (unsigned long long)g_draws, (unsigned long long)g_drops[0], (unsigned long long)g_drops[1],
             (unsigned long long)g_drops[2], (unsigned long long)g_drops[3], (unsigned long long)g_drops[4]);
    std::memset(g_drops, 0, sizeof(g_drops));
    g_draws = 0;
}
}  // namespace

void D3D12Backend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    if (cap_) CapStep(s.rt);
    g_ls_draws++;
    Assembled& a = asm_;
    bool assembled = Assemble(s, prim, start, count, indices, a);
    if (!assembled) return CountDrop(kDropAssemble);
    // a versus level's first frame draws everything once (thousands of draws, over 64 MB): past the ring every upload
    // was a buffer of its own, seconds of creating and then freeing them
    if (CurrentFrame().used > kUploadSize - kUploadReserve) {
        if (g_perf_on) g_ring_waits++;
        Submit(true);
    }
    if (!BindTargets(s)) return CountDrop(kDropTargets);
    if (!EnterHdrForScene(s, a)) return CountDrop(kDropHdr);
    if (!BloomForScene(s, a)) return CountDrop(kDropTargets);

    // vertex shader
    DrawShaders sh = ClassifyShaders(s, a, count);
    if (sh.prog) EnsureProgramHash(s);
    sh.ppl = PerPixel(s, a, sh);
    sh.vkey = VertexKey(s, a, sh);
    sh.vh = EnsureVertexShader(s, a, sh);

    // stage textures, then the pixel shader made for them
    DrawTextures stages;
    feedback_stage_ = -1;
    if (!BindStageTexturesAndTargets(s, stages)) return CountDrop(kDropTextures);
    PixelKey pk = MakePixelKey(s, stages.info);
    sh.wdepth = UseLogDepth(s, a);
    pk.wbuf = sh.wdepth;
    pk.quad = sh.quad;
    sh.pkey = Hash(&pk, sizeof(pk));
    PerPixelSource pps{&s.vs->program, a.regs};
    if (sh.ppl) sh.pkey = Hash(&sh.vkey, sizeof(sh.vkey), sh.pkey);  // the pixel shader carries the program
    sh.ph = EnsurePixelShader(pk, sh.pkey, sh.ppl ? &pps : nullptr);

    // pipeline
    PsoKey k;
    BuildPipelineKey(s, a, sh.vh, sh.ph, k);
    if (FaceTwoSided(s, a)) k.cull = false;
    // a quad is a picture or a pass over the screen: left out it shows what lies under it, so it waits for its pipeline
    ID3D12PipelineState* pso = EnsurePipeline(k, count <= kQuadVertices);
    if (!pso) return CountDrop(kDropPipeline);
    CountDrop(-1);
    cl_->SetPipelineState(pso);


    // constants
    GuestSize size;
    SurfaceSize(bound_rt_ ? s.rt : s.zs, size.w, size.h);
    DrawConstants c{};
    QuadTextureRects(s, a, sh, c);
    if (bound_rt_ && bound_rt_->gw == guest_w_ && bound_rt_->gh == guest_h_) Squeeze2D(s, a, sh, guest_w_, c);
    if (feedback_stage_ >= 0 && g_perf_on) {
        // xboxrecomp debug (RECOMP_PERF): a draw reading the target it draws to, for copying only what it needs
        float pos[4] = {}, uv[4] = {};
        bool has_pos = a.pretransformed && RegisterRect(a, 0, pos);
        int tci = int(s.tss[size_t(feedback_stage_)][kTssTexCoordIndex] & 0xFFFF);
        int uv_reg = sh.prog ? CopiedTexcoords(s, a, sh)[size_t(feedback_stage_)] : 9 + (tci & 3);
        bool has_uv = uv_reg >= 0 && RegisterRect(a, uv_reg, uv);
        LOG_FIRST(60, "[FEEDBACK] rt %08X %ux%u stage %d prim %u count %u verts %u prog %d pretrans %d quad %d | vp %u %u "
                      "%u %u scissors %u (%u %u %u %u) | pos %s %.1f %.1f %.1f %.1f | uv %s %.4f %.4f %.4f %.4f | "
                      "quad uvrect %.4f %.4f %.4f %.4f texgen %X ttf %X",
                  bound_rt_ ? bound_rt_->data : 0, bound_rt_ ? bound_rt_->gw : 0, bound_rt_ ? bound_rt_->gh : 0,
                  feedback_stage_, prim, count, a.vertex_count, int(sh.prog), int(a.pretransformed), int(sh.quad),
                  s.viewport.x, s.viewport.y, s.viewport.w, s.viewport.h, s.scissor_count, s.scissors[0],
                  s.scissors[1], s.scissors[2], s.scissors[3], has_pos ? "yes" : "no", pos[0], pos[1], pos[2],
                  pos[3], has_uv ? "yes" : "no", uv[0], uv[1], uv[2], uv[3], c.uvrect[feedback_stage_][0],
                  c.uvrect[feedback_stage_][1], c.uvrect[feedback_stage_][2], c.uvrect[feedback_stage_][3],
                  s.tss[size_t(feedback_stage_)][kTssTexCoordIndex] >> 16,
                  s.tss[size_t(feedback_stage_)][kTssTextureTransformFlags]);
    }
    UploadConstants(s, sh, k, stages, size, c);

    if (cap_) {
        CaptureDraw(s, prim, count, k, c, stages);
        EndDrawCapture(s, a, count, sh.prog, c);
        CaptureShaders(s, a, sh, pk);
    }

    // texture views and samplers, viewport and scissor, blend factor and stencil reference
    SetTextureViewsAndSamplers(s, a, stages);
    SetViewportScissor(s, size.w);
    SetBlendFactorStencilRef(s);

    // geometry
    if (!DrawFacePulled(s, a, c) && !DrawMouthShrunk(s, a)) DrawGeometry(a);
}

// a guest rect scaled to the target and clamped to it
D3D12_RECT ScaledRect(const Target& t, int x0, int y0, int x1, int y1) {
    float sc = float(t.w) / t.gw;
    return {std::clamp<LONG>(LONG(x0 * sc), 0, t.w), std::clamp<LONG>(LONG(y0 * sc), 0, t.h),
            std::clamp<LONG>(LONG(x1 * sc), 0, t.w), std::clamp<LONG>(LONG(y1 * sc), 0, t.h)};
}

// rs: the rects buffer, reused across targets
void D3D12Backend::ClearTarget(Target& t, const State& s, const ClearArgs& c, std::vector<D3D12_RECT>& rs) {
    rs.clear();
    if (c.count && c.rects) {
        for (uint32_t i = 0; i < c.count; i++) {
            int32_t r[4];
            std::memcpy(r, c.rects + size_t(i) * sizeof(r), sizeof(r));
            rs.push_back(ScaledRect(t, r[0], r[1], r[2], r[3]));
        }
    } else {
        const auto& v = s.viewport;
        rs.push_back(ScaledRect(t, int(v.x), int(v.y), int(v.x + v.w), int(v.y + v.h)));
    }
    if (t.depth) {
        Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        D3D12_CLEAR_FLAGS cf = D3D12_CLEAR_FLAGS(((c.flags & kClearZ) ? D3D12_CLEAR_FLAG_DEPTH : 0) |
                                                 ((c.flags & kClearStencil) ? D3D12_CLEAR_FLAG_STENCIL : 0));
        cl_->ClearDepthStencilView(Cpu(dsv_heap_.Get(), t.view, dsv_inc_), cf, std::clamp(c.z, 0.0f, 1.0f),
                                   UINT8(c.stencil), UINT(rs.size()), rs.data());
    } else {
        // pixels the CPU wrote before this clear go in first, a later upload would paint over the clear
        if (LatchCpuWrite(t)) UploadTarget(t, s.rt);
        Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        float col[4];
        Color(c.color, col);
        for (int i = 0; t.hdr && i < 3; i++) col[i] /= kHeadroom;
        cl_->ClearRenderTargetView(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), col, UINT(rs.size()), rs.data());
    }
    t.drawn = true;
}

void D3D12Backend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                         uint32_t stencil) {
    if (cap_) {
        CapStep(s.rt);
        fprintf(cap_, "%04u clear rt %08X ds %08X flags %X color %08X z %.3f\n", cap_i_++, s.rt.data, s.zs.data, flags,
                color, z);
    }
    if (flags & kClearTarget) {
        g_ls_clears++;
        g_ls_clear_color = color;
        g_ls_clear_rt = s.rt.data;
    }
    // xboxrecomp (hppoa): clearing the scene's depth (before the HUD) ends the scene: occlusion needs that depth
    if ((flags & kClearZ) && s.zs.data == scene_zs_.data && scene_cam_frame_ == presents_) {
        Target* t = FindTarget(scene_rt_, false);
        if (t && t->scene_frame == presents_ && t->bloom_frame != presents_) SceneEnd(*t);
        bound_rt_ = bound_ds_ = nullptr;
    }
    Target* rt = (flags & kClearTarget) ? EnsureTarget(s.rt, false) : nullptr;
    Target* ds = (flags & (kClearZ | kClearStencil)) ? EnsureTarget(s.zs, true) : nullptr;
    uint32_t gw;
    uint32_t gh;
    SurfaceSize(rt ? s.rt : s.zs, gw, gh);
    std::vector<D3D12_RECT> rs;
    ClearArgs c{.count = count, .rects = rects, .flags = flags, .color = color, .z = z, .stencil = stencil};
    for (Target* t : {rt, ds}) {
        if (t) ClearTarget(*t, s, c, rs);
    }
}

// CopyRects' guest lists: src_rects RECTs, dst_points POINTs, all int32
constexpr uint32_t kGuestRectBytes = 16;
constexpr uint32_t kRectLeft = 0;
constexpr uint32_t kRectTop = 4;
constexpr uint32_t kRectRight = 8;
constexpr uint32_t kRectBottom = 12;
constexpr uint32_t kGuestPointBytes = 8;
constexpr uint32_t kPointX = 0;
constexpr uint32_t kPointY = 4;

struct GuestRect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
};

struct GuestPoint {
    int32_t x;
    int32_t y;
};

GuestRect ReadGuestRect(uint32_t rects, uint32_t i) {
    uint32_t r = rects + i * kGuestRectBytes;
    GuestRect g;
    g.left = int32_t(kernel::R32(r + kRectLeft));
    g.top = int32_t(kernel::R32(r + kRectTop));
    g.right = int32_t(kernel::R32(r + kRectRight));
    g.bottom = int32_t(kernel::R32(r + kRectBottom));
    return g;
}

GuestPoint ReadGuestPoint(uint32_t points, uint32_t i) {
    uint32_t p = points + i * kGuestPointBytes;
    GuestPoint g;
    g.x = int32_t(kernel::R32(p + kPointX));
    g.y = int32_t(kernel::R32(p + kPointY));
    return g;
}

// a drawn copy's rect in guest pixels: the source corners and where it lands
struct DrawnRect {
    float x0 = 0;
    float y0 = 0;
    float x1 = 0;
    float y1 = 0;
    float dx = 0;
    float dy = 0;
};

// without dst_points the rect lands where it is
DrawnRect ReadDrawnRect(uint32_t src_rects, uint32_t dst_points, uint32_t i) {
    GuestRect g = ReadGuestRect(src_rects, i);
    DrawnRect r;
    r.x0 = float(g.left);
    r.y0 = float(g.top);
    r.x1 = float(g.right);
    r.y1 = float(g.bottom);
    r.dx = r.x0;
    r.dy = r.y0;
    if (dst_points) {
        GuestPoint p = ReadGuestPoint(dst_points, i);
        r.dx = float(p.x);
        r.dy = float(p.y);
    }
    return r;
}

// a guest coordinate read as unsigned, times the host scale
UINT ScaleUnsigned(int32_t v, float sc) { return UINT(uint32_t(v) * sc); }

// a drawn copy's taps round down a ratio a hair over a whole number
constexpr float kTapsSlack = 1e-3f;

// one Blit per rect: for formats that differ or a multisampled destination
void D3D12Backend::CopyByDraw(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state,
                              Target& dst_t, const CopyList& l) {
    Transition(src_res, src_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(dst_t.res.Get(), dst_t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), dst_t.view, rtv_inc_);
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    float src_scale = float(src_t.w) / src_t.gw;
    float dst_scale = float(dst_t.w) / dst_t.gw;
    bool listed = l.count && l.src_rects;
    uint32_t n = listed ? l.count : 1;
    for (uint32_t i = 0; i < n; i++) {
        float whole_w = float(std::min(src_t.gw, dst_t.gw));
        float whole_h = float(std::min(src_t.gh, dst_t.gh));
        DrawnRect r{.x0 = 0, .y0 = 0, .x1 = whole_w, .y1 = whole_h};
        if (listed) r = ReadDrawnRect(l.src_rects, l.dst_points, i);
        if (r.x1 <= r.x0 || r.y1 <= r.y0) continue;
        D3D12_VIEWPORT vp{r.dx * dst_scale, r.dy * dst_scale, (r.x1 - r.x0) * dst_scale, (r.y1 - r.y0) * dst_scale,
                          0, 1};
        LONG right = std::min(LONG(vp.TopLeftX + vp.Width), LONG(dst_t.w));
        LONG bottom = std::min(LONG(vp.TopLeftY + vp.Height), LONG(dst_t.h));
        D3D12_RECT scissor{LONG(vp.TopLeftX), LONG(vp.TopLeftY), right, bottom};
        if (scissor.right <= scissor.left || scissor.bottom <= scissor.top) continue;
        cl_->RSSetViewports(1, &vp);
        cl_->RSSetScissorRects(1, &scissor);
        float uv[4] = {r.x0 / src_t.gw, r.y0 / src_t.gh, (r.x1 - r.x0) / src_t.gw, (r.y1 - r.y0) / src_t.gh};
        uint32_t taps = uint32_t(std::ceil(std::max(src_scale / dst_scale, 1.0f) - kTapsSlack));
        float mul = src_t.fmt != dst_t.fmt && dst_t.hdr ? 1 / kHeadroom : 1;
        BlitOptions o{.alpha = true, .taps = taps, .mul = mul, .rtv = dst_t.fmt, .samples = dst_t.samples};
        Blit(src_res, src_t.fmt, uv, o);
    }
    dst_t.drawn = true;
    bound_rt_ = bound_ds_ = nullptr;
}

// one CopyTextureRegion per rect, clamped to both targets
void D3D12Backend::CopyByRegion(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state,
                                Target& dst_t, const CopyList& l) {
    Transition(src_res, src_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(dst_t.res.Get(), dst_t.state, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dl{dst_t.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION sl{src_res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    float dst_scale = float(dst_t.w) / dst_t.gw;
    bool listed = l.count && l.src_rects;
    uint32_t n = listed ? l.count : 1;
    for (uint32_t i = 0; i < n; i++) {
        D3D12_BOX b{0, 0, 0, std::min(src_t.w, dst_t.w), std::min(src_t.h, dst_t.h), 1};
        uint32_t dx = 0;
        uint32_t dy = 0;
        if (listed) {
            GuestRect r = ReadGuestRect(l.src_rects, i);
            b.left = ScaleUnsigned(r.left, dst_scale);
            b.top = ScaleUnsigned(r.top, dst_scale);
            b.right = std::min(ScaleUnsigned(r.right, dst_scale), src_t.w);
            b.bottom = std::min(ScaleUnsigned(r.bottom, dst_scale), src_t.h);
            dx = b.left;
            dy = b.top;
            if (l.dst_points) {
                GuestPoint p = ReadGuestPoint(l.dst_points, i);
                dx = ScaleUnsigned(p.x, dst_scale);
                dy = ScaleUnsigned(p.y, dst_scale);
            }
        }
        if (b.right <= b.left || b.bottom <= b.top || dx >= dst_t.w || dy >= dst_t.h) continue;
        b.right = std::min<UINT>(b.right, b.left + dst_t.w - dx);
        b.bottom = std::min<UINT>(b.bottom, b.top + dst_t.h - dy);
        cl_->CopyTextureRegion(&dl, dx, dy, 0, &sl, &b);
    }
    dst_t.drawn = true;
    bound_rt_ = bound_ds_ = nullptr;
}

void D3D12Backend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects,
                        uint32_t dst_points) {
    Target* src_t = FindTarget(src, false);
    if (cap_) {
        const char* from = "memory";
        if (src_t) from = src_t->drawn ? "target" : "idle target";
        fprintf(cap_, "%04u copy %08X %s to %08X\n", cap_i_++, src.data, from, dst.data);
    }
    // pixels the CPU made: the copy happens in memory, textures and targets pick it up from there
    if (!src_t || !src_t->drawn) {
        if (!CopySurfaceMemory(src, dst, count, src_rects, dst_points)) {
            LOG_FIRST(8, "d3d12: CopyRects %08X to %08X skipped", src.data, dst.data);
        } else if (Target* dst_t = FindTarget(dst, false)) {
            UploadTarget(*dst_t, dst);
        }
        return;
    }
    if (src_t->hdr) Resolve(*src_t);
    Target* dst_t = EnsureTarget(dst, false);
    if (!dst_t || src_t == dst_t) return;
    Readable src_r = ReadableResource(*src_t);
    if (!src_r.res) return;
    CopyList l{.count = count, .src_rects = src_rects, .dst_points = dst_points};
    if (src_t->fmt != dst_t->fmt || dst_t->samples > 1) {
        // formats differ, or the samples of a multisampled target have to be written: drawn across
        CopyByDraw(*src_t, src_r.res, *src_r.state, *dst_t, l);
        return;
    }
    CopyByRegion(*src_t, src_r.res, *src_r.state, *dst_t, l);
    if (cap_) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_copy_%08X", cap_i_, dst_t->data);
        CapSave(*dst_t, name);
    }
}

// the output the window's display has: a factory made before Windows HDR was switched no longer tells
void D3D12Backend::DetectHdr() {
    HMONITOR mon = MonitorFromWindow(static_cast<HWND>(app::Window()), MONITOR_DEFAULTTONEAREST);
    if (mon == hdr_mon_ && factory_->IsCurrent()) return;
    hdr_mon_ = mon;
    ComPtr<IDXGIFactory4> fresh;
    if (!factory_->IsCurrent() && SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&fresh)))) factory_ = fresh;
    DXGI_OUTPUT_DESC1 desc{};
    bool found = FindOutput(factory_.Get(), mon, desc);
    hdr_display_ = found && desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    peak_nits_ = found && desc.MaxLuminance > 0 ? desc.MaxLuminance : kDefaultPeakNits;
    paper_nits_ = SdrWhiteNits(mon);
    app::g_hdr_display = hdr_display_;
    LOGR("Display HDR: %s, peak %.0f nits, SDR white %.0f nits", hdr_display_ ? "on" : "off", peak_nits_,
         paper_nits_);
}

void D3D12Backend::ResizeSwap() {
    HWND hwnd = static_cast<HWND>(app::Window());
    RECT rc;
    GetClientRect(hwnd, &rc);
    uint32_t w = std::max<LONG>(rc.right - rc.left, 1);
    uint32_t h = std::max<LONG>(rc.bottom - rc.top, 1);
    // xboxrecomp: HDR10 output removed (2026-10-23): always SDR, also on a display with Windows HDR on
    bool hdr = false;
    if (w == bb_w_ && h == bb_h_ && hdr == hdr_want_) return;
    Flush();
    for (auto& f : frames_) {
        if (f.fence) WaitFence(f.fence, "the swap chain resize");
    }
    for (auto& b : bb_) b.Reset();
    hdr_want_ = hdr;
    hdr_out_ = hdr && ResizeBackBuffers(w, h, kHdr10Format, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
    if (!hdr_out_) ResizeBackBuffers(w, h, kBgra, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    if (hdr && !hdr_out_) LOGR("Display HDR: the swap chain can't show HDR10, SDR it is");
    CreateBackBufferViews();
    bb_w_ = w;
    bb_h_ = h;
}

// false when the swap chain can't present in that colour space
bool D3D12Backend::ResizeBackBuffers(uint32_t w, uint32_t h, DXGI_FORMAT fmt, DXGI_COLOR_SPACE_TYPE space) {
    UINT flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    if (FAILED(swap_->ResizeBuffers(kFrames, w, h, fmt, flags))) return false;
    UINT support = 0;
    swap_->CheckColorSpaceSupport(space, &support);
    if (!(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) return false;
    return SUCCEEDED(swap_->SetColorSpace1(space));
}

void D3D12Backend::FinishCapture(const Surface& front) {
    CapStep(Surface{});
    if (Target* f = FindTarget(front, false)) CapSave(*f, "final");
    for (auto& t : cap_tex_) {
        char name[32];
        snprintf(name, sizeof(name), "tex_%08X", t.data);
        D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        CapSave({.res = t.res.Get(), .state = &st, .fmt = t.fmt, .w = t.w, .h = t.h}, name);
    }
    fclose(cap_);
    cap_ = nullptr;
    cap_tex_.clear();
    LOGI("d3d12: frame captured to %s", cap_dir_.string().c_str());
}

void D3D12Backend::StartCapture() {
    SYSTEMTIME tm;
    GetLocalTime(&tm);
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "%02u%02u%02u", tm.wHour, tm.wMinute, tm.wSecond);
    std::error_code ec;
    cap_dir_ = app::ExeDir() / "captures" / stamp;
    std::filesystem::create_directories(cap_dir_, ec);
    cap_ = _wfopen((cap_dir_ / "frame.txt").c_str(), L"w");
    cap_i_ = cap_rt_ = 0;
    cap_draws_ = GetEnvironmentVariableW(L"RECOMP_CAP_DRAWS", nullptr, 0) != 0;
    cap_seen_.clear();
    cap_sh_.clear();
}

// Brightness and contrast range from 0 to 100, 50/50 leaves the image unchanged.
// Unreal's PC ramp offsets brightness and scales contrast linearly, graying blacks
// and clipping both ends. Here, black and white stay fixed: contrast uses an S-curve
// around mid-gray (slope 0.5–2), while brightness adjusts gamma from 2 to 0.5.
// Both match Unreal's ramp at quarter gray.
// xboxrecomp: the curve applies after the title's own ramp, which HP:CoS sets (the original renderer replaced its game's)
void D3D12Backend::UploadGammaRamp(uint32_t brightness, uint32_t contrast) {
    constexpr uint32_t kRampBytes = 256 * 4;  // one BGRA texel per ramp entry
    ramp_set_ = brightness << 8 | contrast;
    ramp_dirty_ = false;
    float k = std::exp2((float(contrast) - 50.0f) / 50.0f);
    float g = std::exp2((50.0f - float(brightness)) / 50.0f);
    Alloc a = Upload(kRampBytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    bool identity = true;
    auto curve = [&](uint8_t in) {
        float x = in / 255.0f;
        float y = x < 0.5f ? 0.5f * std::pow(2.0f * x, k) : 1.0f - 0.5f * std::pow(2.0f - 2.0f * x, k);
        return uint8_t(std::lround(std::clamp(std::pow(y, g), 0.0f, 1.0f) * 255.0f));
    };
    for (int i = 0; i < 256; i++) {
        uint8_t* p = a.cpu + i * 4;
        p[0] = curve(ramp_[512 + i]);
        p[1] = curve(ramp_[256 + i]);
        p[2] = curve(ramp_[i]);
        p[3] = 255;
        identity &= p[0] == i && p[1] == i && p[2] == i;
    }
    gamma_ = !identity;
    Transition(lut_.Get(), lut_state_, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dl{lut_.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION sl{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    sl.PlacedFootprint.Offset = a.off;
    sl.PlacedFootprint.Footprint = {DXGI_FORMAT_B8G8R8A8_UNORM, 256, 1, 1, kRampBytes};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
}

// what the frame shows: the target made readable, SMAA's blend of it when that is on
ID3D12Resource* D3D12Backend::PrepareShownFrame(Target* t) {
    Readable shown = t ? ReadableResource(*t) : Readable{nullptr, nullptr};
    ID3D12Resource* r = shown.res;
    if (r) {
        Transition(r, *shown.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (app::Get().smaa) {
            if (ID3D12Resource* aa = Smaa(r, t->fmt, t->w, t->h)) r = aa;
        }
    }
    return r;
}

void D3D12Backend::BlitFrame(const Target& t, ID3D12Resource* r, bool pillarbox, DXGI_FORMAT rtv) {
    float aspect = app::Aspect();
    float w = float(bb_w_);
    float h = float(bb_h_);
    float vw = std::min(w, h * aspect);
    float vh = vw / aspect;
    if (pillarbox) vw = vh * 4.0f / 3.0f;
    float uv[4] = {0, 0, 1, 1};
    D3D12_VIEWPORT vp{(w - vw) / 2, (h - vh) / 2, vw, vh, 0, 1};
    D3D12_RECT scissor{0, 0, LONG(bb_w_), LONG(bb_h_)};
    cl_->RSSetViewports(1, &vp);
    cl_->RSSetScissorRects(1, &scissor);
    // xboxrecomp: the colour-blind filter is in the gamma pass, which a neutral ramp (CoS's own at default brightness)
    // would skip; the lookup is identity then, so only the filter changes the picture
    // (and the sharpening and grading, xboxrecomp)
    const app::Settings& set = app::Get();
    if (r) Blit(r, t.fmt, uv, {.gamma = gamma_ || set.colorblind != 0 || set.sharpen != 0 || GradingOn(set) ||
                                          set.vignette != 0 || set.grain != 0, .rtv = rtv});
}

// the overlay as the window holds it: a new frame goes up to ov_tex_, under the window's overlay lock
void D3D12Backend::OverlayLatch(const XboxPresentInfo& pi) {
    ov_shown_ = pi.ov_on && pi.ov && pi.ov_w && pi.ov_h;
    if (!ov_shown_) return;
    ov_fb_w_ = pi.fb_w ? pi.fb_w : 640;
    ov_fb_h_ = pi.fb_h ? pi.fb_h : 480;
    ov_rect_[0] = float(pi.ov_x);
    ov_rect_[1] = float(pi.ov_y);
    ov_rect_[2] = float(pi.ov_dw);
    ov_rect_[3] = float(pi.ov_dh);
    if (!ov_tex_ || ov_w_ != pi.ov_w || ov_h_ != pi.ov_h) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td = Texture2DDesc(kBgra, pi.ov_w, pi.ov_h);
        if (ov_tex_) CurrentFrame().garbage.push_back(ov_tex_);
        ov_tex_ = nullptr;
        ov_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, ov_state_, nullptr,
                                                 IID_PPV_ARGS(&ov_tex_)))) {
            ov_shown_ = false;
            return;
        }
        ov_w_ = pi.ov_w;
        ov_h_ = pi.ov_h;
        ov_version_ = pi.ov_version - 1;
    }
    if (pi.ov_version == ov_version_) return;
    ov_version_ = pi.ov_version;
    uint32_t pitch = (ov_w_ * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    Alloc a = Upload(uint64_t(pitch) * ov_h_, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    for (uint32_t y = 0; y < ov_h_; y++) {
        uint8_t* row = a.cpu + size_t(y) * pitch;
        std::memcpy(row, pi.ov + size_t(y) * ov_w_, ov_w_ * 4);
        for (uint32_t x = 0; x < ov_w_; x++) row[x * 4 + 3] = 0xFF;
    }
    Transition(ov_tex_.Get(), ov_state_, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dl{ov_tex_.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION sl{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    sl.PlacedFootprint.Offset = a.off;
    sl.PlacedFootprint.Footprint = {kBgra, ov_w_, ov_h_, 1, pitch};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
}

// xboxrecomp: the movie overlay scaled into the 4:3 picture, then the options panel
void D3D12Backend::RenderUi() {
    RenderOverlay();
    RenderPanel();
}

void D3D12Backend::RenderOverlay() {
    xbox_FramebufferWindowOverlayRead(
        [](const XboxPresentInfo* pi, void* self) { static_cast<D3D12Backend*>(self)->OverlayLatch(*pi); }, this);
    if (!ov_shown_ || !ov_tex_) return;
    Transition(ov_tex_.Get(), ov_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    float w = float(bb_w_), h = float(bb_h_);
    float vh = std::min(h, w * 3.0f / 4.0f), vw = vh * 4.0f / 3.0f;
    float x0 = (w - vw) / 2, y0 = (h - vh) / 2;
    float sx = vw / float(ov_fb_w_), sy = vh / float(ov_fb_h_);
    D3D12_VIEWPORT vp{x0 + ov_rect_[0] * sx, y0 + ov_rect_[1] * sy, ov_rect_[2] * sx, ov_rect_[3] * sy, 0, 1};
    D3D12_RECT scissor{0, 0, LONG(bb_w_), LONG(bb_h_)};
    cl_->RSSetViewports(1, &vp);
    cl_->RSSetScissorRects(1, &scissor);
    float uv[4] = {0, 0, 1, 1};
    Blit(ov_tex_.Get(), kBgra, uv, {});
}

// the options panel and fps counter, over everything
void D3D12Backend::RenderPanel() {
    std::lock_guard lock(app::UiLock());
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    app::DrawUi();
    ImGui::Render();
    ID3D12DescriptorHeap* uh[] = {ui_heap_.Get()};
    cl_->SetDescriptorHeaps(1, uh);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cl_.Get());
}

void D3D12Backend::DrawFrame(Target* t, bool pillarbox, uint32_t bi) {
    D3D12_RESOURCE_STATES bs = D3D12_RESOURCE_STATE_PRESENT;
    ID3D12Resource* r = PrepareShownFrame(t);
    Transition(bb_[bi].Get(), bs, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), kRtvCount + bi, rtv_inc_);
    // without its targets an HDR10 back buffer stays black: the SDR pipelines don't match its format
    if (!hdr_out_) DrawSdrFrame(t, r, pillarbox, rh);
    else if (EnsureOutTargets()) DrawHdrFrame(t, r, pillarbox, rh);
    Transition(bb_[bi].Get(), bs, D3D12_RESOURCE_STATE_PRESENT);
}

void D3D12Backend::DrawSdrFrame(Target* t, ID3D12Resource* r, bool pillarbox, D3D12_CPU_DESCRIPTOR_HANDLE bb) {
    float black[4] = {0, 0, 0, 1};
    cl_->ClearRenderTargetView(bb, black, 0, nullptr);
    cl_->OMSetRenderTargets(1, &bb, FALSE, nullptr);
    bound_rt_ = bound_ds_ = nullptr;
    if (t) BlitFrame(*t, r, pillarbox);
    RenderUi();
}

bool D3D12Backend::EnsureOutTargets() {
    if (out_[0] && out_[1] && out_w_ == bb_w_ && out_h_ == bb_h_) return true;
    out_w_ = bb_w_;
    out_h_ = bb_h_;
    return CreatePostTarget(out_[0], out_state_[0], kHdrFormat, bb_w_, bb_h_, kOutRtv) &&
           CreatePostTarget(out_[1], out_state_[1], kBgra, bb_w_, bb_h_, kOutRtv + 1);
}

// the frame and the panel each into a target of their own, then both into the back buffer as HDR10; the panel
// stays at SDR white
void D3D12Backend::DrawHdrFrame(Target* t, ID3D12Resource* r, bool pillarbox, D3D12_CPU_DESCRIPTOR_HANDLE bb) {
    D3D12_CPU_DESCRIPTOR_HANDLE frame = PostRtv(kOutRtv);
    D3D12_CPU_DESCRIPTOR_HANDLE panel = PostRtv(kOutRtv + 1);
    float clear[4] = {0, 0, 0, 0};
    for (int i = 0; i < 2; i++) Transition(out_[i].Get(), out_state_[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
    cl_->ClearRenderTargetView(frame, clear, 0, nullptr);
    cl_->ClearRenderTargetView(panel, clear, 0, nullptr);
    cl_->OMSetRenderTargets(1, &frame, FALSE, nullptr);
    bound_rt_ = bound_ds_ = nullptr;
    if (t) BlitFrame(*t, r, pillarbox, kHdrFormat);
    cl_->OMSetRenderTargets(1, &panel, FALSE, nullptr);
    RenderUi();
    Begin();  // RenderUi left ImGui's heap and root signature bound
    for (int i = 0; i < 2; i++) Transition(out_[i].Get(), out_state_[i], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    PostInputs in{{out_[0].Get(), out_[1].Get(), nullptr}, {kHdrFormat, kBgra, kBgra}};
    float p[8] = {paper_nits_, peak_nits_, 0, 0};
    DrawPostPass("hdr10", {bb, kHdr10Format, bb_w_, bb_h_}, in, p, {.hlsl = kHdrHlsl});
}

void D3D12Backend::ReleaseOldTextures() {
    constexpr uint64_t kSweepEvery = 60;    // presents between sweeps
    constexpr size_t kTexturesKept = 2048;  // no sweep below this many
    constexpr uint64_t kIdleFrames = 600;
    // textures unused for ten seconds go, levels stream theirs in and out
    if (presents_ % kSweepEvery != 0 || textures_.size() <= kTexturesKept) return;
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (it->second.frame + kIdleFrames < presents_) {
            CurrentFrame().garbage.push_back(it->second.res);
            it = textures_.erase(it);
        } else {
            ++it;
        }
    }
}

void D3D12Backend::ResetFrame() {
    Frame& f = CurrentFrame();
    if (f.fence && fence_->GetCompletedValue() < f.fence) WaitFence(f.fence, "the frame slot's last use (GPU behind)");
    f.alloc->Reset();
    f.garbage.clear();
    f.used = 0;
    f.srv = 0;
    last_srv_.valid = false;
    cl_->Reset(f.alloc.Get(), nullptr);
    Begin();
    // xboxrecomp debug: this slot's last frame is done on the GPU: its times, then the new frame's start
    uint32_t slot = uint32_t(frame_ % kFrames);
    if (ts_open_[slot] && ts_map_ && ts_freq_) {
        const uint64_t* t = ts_map_ + slot * 3;
        if (t[1] >= t[0] && t[2] >= t[1]) {
            g_gpu_scene_ns += (t[1] - t[0]) * 1000000000ull / ts_freq_;
            g_gpu_post_ns += (t[2] - t[1]) * 1000000000ull / ts_freq_;
            g_gpu_frames++;
        }
        ts_open_[slot] = false;
    }
    GpuTimestamp(0);
}

// xboxrecomp debug: every 2 s, the targets copied for reading most ([PERF-COPY]): guest address and size, host size and
// samples, copies a frame because a draw read the target it drew to (feedback) or read a multisampled one (resolve)
void D3D12Backend::LogCopies() {
    if (!g_perf_on) return;
    auto now = std::chrono::steady_clock::now();
    if (copy_t0_ == std::chrono::steady_clock::time_point{}) copy_t0_ = now;
    copy_frames_++;
    double span = std::chrono::duration<double>(now - copy_t0_).count();
    if (span < 2.0) return;
    std::vector<std::pair<uint32_t, CopyCount>> v(copy_counts_.begin(), copy_counts_.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
        return a.second.feedback + a.second.resolve > b.second.feedback + b.second.resolve;
    });
    double fps_frames = std::max(1u, copy_frames_);
    std::string line;
    for (size_t i = 0; i < v.size() && i < 6; i++) {
        const CopyCount& c = v[i].second;
        char b[160];
        std::snprintf(b, sizeof b, " | %08X %ux%u (%ux%u x%u) feedback %.1f resolve %.1f", v[i].first, c.gw, c.gh, c.w,
                      c.h, c.samples, c.feedback / fps_frames, c.resolve / fps_frames);
        line += b;
    }
    if (!line.empty()) LOGI("[PERF-COPY] per frame%s", line.c_str());
    copy_counts_.clear();
    copy_t0_ = now;
    copy_frames_ = 0;
}

// xboxrecomp debug: timestamp k (0 start, 1 drawing done, 2 shown) of the current frame slot; resolved after the last
void D3D12Backend::GpuTimestamp(uint32_t k) {
    static const bool on = [] {
        const char* e = std::getenv("RECOMP_PERF");
        return e && *e && *e != '0';
    }();
    if (!on) return;
    if (!ts_heap_) {
        D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 3 * kFrames, 0};
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 3 * kFrames * sizeof(uint64_t);
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dev_->CreateQueryHeap(&qd, IID_PPV_ARGS(&ts_heap_))) ||
            FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&ts_buf_))) ||
            FAILED(queue_->GetTimestampFrequency(&ts_freq_)) ||
            FAILED(ts_buf_->Map(0, nullptr, reinterpret_cast<void**>(&ts_map_)))) {
            ts_heap_.Reset();
            ts_map_ = nullptr;
            return;
        }
    }
    uint32_t slot = uint32_t(frame_ % kFrames);
    cl_->EndQuery(ts_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 3 + k);
    if (k == 2) {
        cl_->ResolveQueryData(ts_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 3, 3, ts_buf_.Get(),
                              slot * 3 * sizeof(uint64_t));
        ts_open_[slot] = true;
    }
}

void D3D12Backend::CountFps() {
    fps_n_++;
    auto now = std::chrono::steady_clock::now();
    double el = std::chrono::duration<double>(now - fps_t_).count();
    if (el >= 0.5) {
        app::g_fps = float(fps_n_ / el);
        fps_n_ = 0;
        fps_t_ = now;
    }
}

void D3D12Backend::Present(const Surface& front, bool pillarbox) {
    if (!prebuild_started_) {  // xboxrecomp: the settings are read by now (the sample count)
        prebuild_started_ = true;
        StartPrebuild();
    }
    static const bool dbg_keys_env = GetEnvironmentVariableW(L"RECOMP_DEBUG_KEYS", nullptr, 0) != 0;
    const bool dbg_keys = dbg_keys_env || app::Get().debug_keys;  // xboxrecomp: or F10's Debug tab
    if (dbg_keys && (GetAsyncKeyState(VK_F8) & 1)) {
        g_dbg_logall = g_dbg_logall == 1 ? 0 : 1;
        LOGI("debug: log depth for all draws %s", g_dbg_logall ? "on" : "off");
    }
    if (dbg_keys && (GetAsyncKeyState(VK_F9) & 1)) {
        g_dbg_noatest = !g_dbg_noatest;
        LOGI("debug: alpha test %s", g_dbg_noatest ? "off" : "on");
    }
    if (dbg_keys && (GetAsyncKeyState(VK_F7) & 1)) {  // F10 is the options panel
        g_dbg_zalways = !g_dbg_zalways;
        LOGI("debug: depth test %s", g_dbg_zalways ? "always passes" : "on");
    }
    if (cap_) {
        FinishCapture(front);
    } else if ((GetAsyncKeyState(VK_F12) & 1) || g_capture.exchange(false)) {  // xboxrecomp: F12 captures a frame
        StartCapture();
    }
    auto perf_t0 = std::chrono::steady_clock::now();  // xboxrecomp debug (RECOMP_PERF): [PERF-PRESENT]
    DetectHdr();
    ResizeSwap();
    Target* t = EnsureTarget(front, false);
    if (t && LatchCpuWrite(*t)) UploadTarget(*t, front);
    if (t && t->hdr) Resolve(*t);
    // xboxrecomp: a frame with a scene and no HUD after it is bloomed here
    if (t && t->scene_frame == presents_ && t->bloom_frame != presents_) SceneEnd(*t);
    if (const app::Settings& s = app::Get(); ramp_dirty_ || (s.brightness << 8 | s.contrast) != ramp_set_)
        UploadGammaRamp(s.brightness, s.contrast);
    Transition(lut_.Get(), lut_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // a frame missing draws still on the build threads is not shown, the last whole one stays up: otherwise a new
    // effect's intermediate passes flash on screen
    auto now_t = std::chrono::steady_clock::now();
    // xboxrecomp: the level-start window, 3 s after a pause over 500 ms (a level load) or 8 s after the level index
    // changed
    static std::chrono::steady_clock::time_point ls_last, ls_until;
    static uint32_t ls_n, ls_level = 0xFFFFFFFFu;
    {
        uint32_t lv = kernel::R32(0x527B78);  // HP:CoS level index (hle_trainer / debug report)
        if (ls_last.time_since_epoch().count() && now_t - ls_last > std::chrono::milliseconds(500)) {
            ls_until = now_t + std::chrono::seconds(3);
            ls_n = 0;
            LOGI("[LEVELSTART] pause of %.0f ms before this frame",
                 std::chrono::duration<double, std::milli>(now_t - ls_last).count());
        }
        if (lv != ls_level) {
            ls_until = now_t + std::chrono::seconds(8);
            ls_n = 0;
            LOGI("[LEVELSTART] level %d -> %d", int(ls_level), int(lv));
            ls_level = lv;
        }
    }
    // xboxrecomp: the first frame of a level draws nothing (the world is not culled in yet), only the clears: the sky
    // colour (blue). On the console it is up for one vblank; here the next frame usually waits on new pipelines and
    // is held, so that clear stayed on screen ~0.1 s. In the level-start window a frame with no draws at all (no
    // movie on the overlay) keeps the last whole frame - the loading picture - up instead.
    bool movie = false;  // read now: ov_shown_ is only latched when a frame is drawn
    xbox_FramebufferWindowOverlayRead(
        [](const XboxPresentInfo* pi, void* on) { *static_cast<bool*>(on) = pi->ov_on != 0; }, &movie);
    bool empty_frame = g_ls_draws == 0 && !movie && now_t < ls_until;
    bool hold = (skipped_frame_ || empty_frame) && shown_ && now_t - shown_t_ < kHoldMax;
    {   // xboxrecomp: [LEVELSTART] what each frame held in the level-start window: a frame showing only the clear
        // colour has few draws, one with draws skipped for pipelines says so
        static uint32_t last_draws = 0xFFFFFFFFu;
        // one line per frame whose draw count moved by more than a tenth (a frame like the last is not repeated)
        bool changed = last_draws == 0xFFFFFFFFu || g_ls_draws * 10 > last_draws * 11 || g_ls_draws * 11 < last_draws * 10;
        if (now_t < ls_until && ls_n++ < 2000 && (changed || skipped_frame_ || hold))
            LOGI("[LEVELSTART] frame %u: front %08X, draws %u (dropped %u), colour clears %u, last %08X on %08X, "
                 "skipped for pipelines %s, %s", ls_n, front.data, g_ls_draws, g_ls_drops, g_ls_clears, g_ls_clear_color,
                 g_ls_clear_rt, skipped_frame_ ? "yes" : empty_frame ? "no (empty frame)" : "no",
                 hold ? "held (last whole frame shown)" : "shown");
        last_draws = g_ls_draws;
        g_ls_draws = g_ls_drops = g_ls_clears = 0;
        ls_last = now_t;
    }
    skipped_frame_ = false;
    {   // xboxrecomp: each hold in the log as it ends (the title animates on meanwhile: a long one looks like a jump)
        static uint32_t held;
        if (hold) {
            held++;
        } else if (held) {
            LOGI("d3d12: last whole frame held %u frames (%.0f ms) while pipelines were built / the level's first "
                 "frames were empty", held,
                 std::chrono::duration<double, std::milli>(now_t - shown_t_).count());
            held = 0;
        }
    }
    uint32_t bi = swap_->GetCurrentBackBufferIndex();
    LogCopies();
    GpuTimestamp(1);
    if (!hold) DrawFrame(t, pillarbox, bi);
    GpuTimestamp(2);
    auto perf_t1 = std::chrono::steady_clock::now();
    Submit(false);
    auto perf_t2 = std::chrono::steady_clock::now();
    if (!hold) {
        bool vsync = app::Get().vsync;
        auto pt0 = std::chrono::steady_clock::now();
        HRESULT hr = swap_->Present(vsync ? 1 : 0, !vsync && tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
        SlowGpu(MsSince(pt0), "swap chain Present (vsync %s)", vsync ? "on" : "off");
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) GpuLost("present", hr);
        shown_ = true;
        shown_t_ = now_t;
    }
    auto perf_t3 = std::chrono::steady_clock::now();

    presents_++;
    presents_seen_.store(presents_);
    frame_++;
    ReleaseOldTextures();
    ResetFrame();
    if (g_perf_on) LogPresentParts(perf_t0, perf_t1, perf_t2, perf_t3, std::chrono::steady_clock::now());
    CountFps();
    ApplySettings();
}

// xboxrecomp debug (RECOMP_PERF): Present's parts every 2 s, per frame: before the frame's passes and those passes
// recorded, the frame's command list executed (vkd3d translates it here), the swap chain's present (the vsync wait),
// the next frame's slot waiting for the GPU
void D3D12Backend::LogPresentParts(TimePoint t0, TimePoint t1, TimePoint t2, TimePoint t3, TimePoint t4) {
    auto ms = [](TimePoint a, TimePoint b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    perf_parts_[0] += ms(t0, t1);
    perf_parts_[1] += ms(t1, t2);
    perf_parts_[2] += ms(t2, t3);
    perf_parts_[3] += ms(t3, t4);
    if (++perf_frames_ < 120) return;
    double n = perf_frames_;
    LOGI("[PERF-PRESENT] per frame ms: frame passes %.2f, execute %.2f, swap present %.2f, slot wait %.2f (vsync %d)",
         perf_parts_[0] / n, perf_parts_[1] / n, perf_parts_[2] / n, perf_parts_[3] / n, int(app::Get().vsync));
    perf_frames_ = 0;
    for (double& p : perf_parts_) p = 0;
}

void D3D12Backend::ApplySettings() {
    app::Poll();
    float want = app::RenderScale(guest_h_);
    const app::Settings& set = app::Get();
    uint32_t shadows = set.shadow_resolution;
    sm3_ = set.shader_model >= 3;
    LatchFeatureSettings(set);
    cache::SaveWhenIdle();
    bool hdr = sm3_ && set.hdr_rendering;
    uint32_t msaa = msaa_;
    if (set.msaa != msaa_set_) {
        msaa_set_ = set.msaa;
        msaa = Samples(msaa_set_);
    }
    if (std::fabs(want - scale_) > 1e-4f || shadows != shadow_res_ || hdr != hdr_ || msaa != msaa_) {
        hdr_ = hdr;
        msaa_ = msaa;
        expo_fresh_ = true;
        Flush();
        for (auto& [key, tg] : targets_) (tg.depth ? free_dsv_ : free_rtv_).push_back(tg.view);
        targets_.clear();
        scale_ = want;
        shadow_res_ = shadows;
        bound_rt_ = bound_ds_ = nullptr;
    }
}

// the target resolved and made readable, then shrunk with o when it is not guest sized BGRA; res null on failure
D3D12Backend::CopySource D3D12Backend::PrepareCopySource(Target& t, const ShrinkOptions& o) {
    if (t.hdr) Resolve(t);
    Readable r = ReadableResource(t);
    CopySource out{r.res, nullptr};
    if (!r.res) return out;
    if (t.w != t.gw || t.h != t.gh || t.fmt != kBgra) {
        out.down = Shrink(r.res, *r.state, t.fmt, t.gw, t.gh, o);
        out.res = out.down.Get();
    } else {
        Transition(r.res, *r.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    return out;
}

// the CPU reads a surface the GPU drew: bring the pixels back
// xboxrecomp (from XboxXBERecomp): long unused, the title freed it and other data is at this address now: reading
// the old target back would overwrite that data
bool D3D12Backend::TargetStale(uint32_t data) const {
    auto it = target_used_.find(data);
    uint64_t used = it == target_used_.end() ? 0 : it->second;
    return frame_ - used > kStaleFrames;
}

void D3D12Backend::Sync(uint32_t data, uint32_t) {
    auto it = targets_.find(TargetKey(data, false));
    if (it == targets_.end() || !it->second.drawn || TargetStale(data)) return;
    Target& t = it->second;
    // src.down, when there is one, stays alive until ReadBack is done
    CopySource src = PrepareCopySource(t, {});
    if (!src.res) return;
    std::vector<uint8_t> px;
    if (!ReadBack(src.res, t.gw, t.gh, px)) return;
    // color targets are 32 bit linear in guest memory, the pitch rounds to 64 bytes
    uint32_t pitch = GuestPitch(t.gw);
    uint8_t* guest = static_cast<uint8_t*>(kernel::HostPtr(data));
    for (uint32_t y = 0; y < t.gh; y++) std::memcpy(guest + size_t(y) * pitch, &px[size_t(y) * t.gw * 4], t.gw * 4);
    t.read = std::move(px);
    t.drawn = false;
    t.sig = MemSig(t.data, t.bytes);
}

// A NOOVERWRITE lock: the console reads whatever the GPU has finished, so a surface locked every frame gets the newest
// finished copy (a frame or two old) and the CPU never waits on the GPU; one locked now and then still waits in Sync
void D3D12Backend::SyncLate(uint32_t data) {
    if (!DeliverLate(data)) Sync(data, 0);
    RecordLate(data);
}

bool D3D12Backend::DeliverLate(uint32_t data) {
    std::lock_guard lock(late_lock_);
    auto it = late_.find(data);
    if (it == late_.end()) return false;
    Late& l = it->second;
    uint64_t done = fence_->GetCompletedValue();
    uint64_t now = presents_seen_.load();
    int best = -1;
    for (int i = 0; i < kFrames; i++) {
        bool finished = l.fence[i] && l.fence[i] <= done;
        bool recent = now - l.frame[i] <= uint64_t(kFrames) + 1;
        bool newest = best < 0 || l.fence[i] > l.fence[best];
        if (finished && recent && newest) best = i;
    }
    if (best < 0) {
        return false;
    }
    if (l.fence[best] > l.delivered) {
        uint8_t* p = nullptr;
        // the last row is not padded to the pitch: a range to the end of the pitch fails to map
        D3D12_RANGE rr{0, size_t(l.row_pitch) * (l.gh - 1) + l.gw * 4};
        if (FAILED(l.rb[best]->Map(0, &rr, reinterpret_cast<void**>(&p)))) return false;
        uint32_t pitch = GuestPitch(l.gw);
        uint8_t* dst = static_cast<uint8_t*>(kernel::HostPtr(data));
        for (uint32_t y = 0; y < l.gh; y++) std::memcpy(dst + size_t(y) * pitch, p + size_t(y) * l.row_pitch, l.gw * 4);
        D3D12_RANGE none{0, 0};
        l.rb[best]->Unmap(0, &none);
        l.delivered = l.fence[best];
    }
    return true;
}

// a slot set for another size: its buffers go with the frame
void D3D12Backend::ResetLate(Late& l, const Target& t) {
    auto& garbage = CurrentFrame().garbage;
    for (auto& r : l.rb) {
        if (r) garbage.push_back(r);
    }
    if (l.down) garbage.push_back(l.down);
    l = Late{};
    l.gw = t.gw;
    l.gh = t.gh;
}

// the target as a guest sized BGRA copy source, shrunk into l.down when it is not one already
ID3D12Resource* D3D12Backend::PrepareLateSource(Target& t, Late& l) {
    ShrinkOptions reuse{.into = l.down.Get(), .into_state = l.down_state};
    CopySource src = PrepareCopySource(t, reuse);
    if (src.down) {
        l.down = src.down;
        l.down_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    return src.res;
}

// src into the readback buffer of the slot, made on first use
void D3D12Backend::RecordLateCopy(Late& l, uint32_t slot, ID3D12Resource* src) {
    D3D12_RESOURCE_DESC rd = src->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 row_size;
    UINT64 bytes;
    dev_->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &row_size, &bytes);
    if (!l.rb[slot]) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bd = BufferDesc(bytes);
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&l.rb[slot])))) {
            return;
        }
    }
    D3D12_TEXTURE_COPY_LOCATION dl{l.rb[slot].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dl.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION sl{src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    l.row_pitch = fp.Footprint.RowPitch;
    l.fence[slot] = fence_value_ + 1;  // the next Submit carries it
    l.frame[slot] = presents_;
    l.next = (slot + 1) % kFrames;
}

void D3D12Backend::RecordLate(uint32_t data) {
    auto it = targets_.find(TargetKey(data, false));
    if (it == targets_.end() || TargetStale(data)) return;
    Target& t = it->second;
    // what DeliverLate or Sync left in guest memory is no CPU write to upload back
    t.sig = MemSig(t.data, t.bytes);
    t.read.clear();
    std::lock_guard lock(late_lock_);
    Late& l = late_[data];
    if (l.gw != t.gw || l.gh != t.gh) ResetLate(l, t);
    // a copy for a later lock, unless the GPU is a whole ring behind
    uint32_t slot = l.next;
    if (l.fence[slot] > fence_->GetCompletedValue()) return;
    ID3D12Resource* src = PrepareLateSource(t, l);
    if (!src) return;
    RecordLateCopy(l, slot, src);
}

// filters src into a new w x h target, left as a copy source
ComPtr<ID3D12Resource> D3D12Backend::Shrink(ID3D12Resource* src, D3D12_RESOURCE_STATES& st, DXGI_FORMAT fmt,
                                            uint32_t w, uint32_t h, const ShrinkOptions& o) {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_B8G8R8A8_UNORM, w, h);
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv{};
    cv.Format = td.Format;
    ComPtr<ID3D12Resource> down = o.into;
    D3D12_RESOURCE_STATES down_state = o.into ? o.into_state : D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (free_rtv_.empty()) return nullptr;
    if (down) {
        Transition(down.Get(), down_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    } else if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                                    D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&down)))) {
        return nullptr;
    }
    // borrows the next free view without taking it, only for this recording
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), free_rtv_.back(), rtv_inc_);
    dev_->CreateRenderTargetView(down.Get(), nullptr, rh);
    Transition(src, st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    SetBlitTarget(rh, w, h);
    float uv[4] = {0, 0, 1, 1};
    Blit(src, fmt, uv, {.alpha = true, .taps = o.taps, .mul = o.mul});
    Transition(down.Get(), down_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    return down;
}

// a BGRA copy source to the CPU, rows packed; waits for the GPU
bool D3D12Backend::ReadBack(ID3D12Resource* res, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    D3D12_RESOURCE_DESC rd = res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 row_size;
    UINT64 bytes;
    dev_->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &row_size, &bytes);
    ComPtr<ID3D12Resource> rb;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd = BufferDesc(bytes);
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&rb)))) {
        return false;
    }
    D3D12_TEXTURE_COPY_LOCATION dl{rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dl.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION sl{res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    Flush();
    uint8_t* p = nullptr;
    rb->Map(0, nullptr, reinterpret_cast<void**>(&p));
    out.resize(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; y++) std::memcpy(&out[size_t(y) * w * 4], p + size_t(y) * fp.Footprint.RowPitch, w * 4);
    rb->Unmap(0, nullptr);
    return true;
}

// BMP file and info header: byte offsets of the fields CapSave writes
constexpr int kBmpHeaderBytes = 54;
constexpr int kBmpFileSize = 2;
constexpr int kBmpPixelOffset = 10;
constexpr int kBmpInfoSize = 14;
constexpr int kBmpWidth = 18;
constexpr int kBmpHeight = 22;  // negative: rows top down
constexpr int kBmpPlanes = 26;
constexpr int kBmpBitCount = 28;
constexpr int kBmpImageSize = 34;
constexpr uint32_t kBmpInfoBytes = 40;

// xboxrecomp debug: RECOMP_CAP_SCALE=n saves a 640 x 480 (the frame's) image at n times the console size, and
// RECOMP_CAP_CROP=x,y,w,h (console pixels) only that part of it: a face's mouth before every draw (RECOMP_CAP_DRAWS)
// at the resolution it was drawn, not 10 console pixels
struct CapView {
    float scale = 1;
    int x = 0, y = 0, w = 0, h = 0;
};
static const CapView& CapViewSettings() {
    static CapView v = [] {
        CapView c;
        if (const char* e = std::getenv("RECOMP_CAP_SCALE")) c.scale = std::clamp(float(std::atof(e)), 1.0f, 8.0f);
        if (const char* e = std::getenv("RECOMP_CAP_CROP")) std::sscanf(e, "%d,%d,%d,%d", &c.x, &c.y, &c.w, &c.h);
        return c;
    }();
    return v;
}

void D3D12Backend::CapSave(const CapImage& img, const char* name) {
    const CapView& view = CapViewSettings();
    bool frame = img.w == 640 && img.h == 480 && (view.scale != 1 || view.w > 0);
    uint32_t sw = frame ? uint32_t(img.w * view.scale + 0.5f) : img.w, sh = frame ? uint32_t(img.h * view.scale + 0.5f) : img.h;
    ComPtr<ID3D12Resource> down = Shrink(img.res, *img.state, img.fmt, sw, sh, {.mul = img.mul});
    std::vector<uint8_t> px;
    if (!down || !ReadBack(down.Get(), sw, sh, px)) return;
    CapImage out = img;
    out.w = sw, out.h = sh;
    if (frame && view.w > 0 && view.h > 0) {
        uint32_t cx = std::min(uint32_t(std::max(view.x, 0) * view.scale), sw - 1), cy = std::min(uint32_t(std::max(view.y, 0) * view.scale), sh - 1);
        uint32_t cw = std::min(uint32_t(view.w * view.scale), sw - cx), ch = std::min(uint32_t(view.h * view.scale), sh - cy);
        std::vector<uint8_t> c(size_t(cw) * ch * 4);
        for (uint32_t r = 0; r < ch; r++) std::memcpy(&c[size_t(r) * cw * 4], &px[(size_t(cy + r) * sw + cx) * 4], size_t(cw) * 4);
        px.swap(c);
        out.w = cw, out.h = ch;
    }
    FILE* f = _wfopen((cap_dir_ / (std::string(name) + ".bmp")).c_str(), L"wb");
    if (!f) return;
    uint8_t hd[kBmpHeaderBytes] = {'B', 'M'};
    auto put = [&](int o, uint32_t v) { std::memcpy(hd + o, &v, 4); };
    put(kBmpFileSize, kBmpHeaderBytes + uint32_t(px.size()));
    put(kBmpPixelOffset, kBmpHeaderBytes);
    put(kBmpInfoSize, kBmpInfoBytes);
    put(kBmpWidth, out.w);
    put(kBmpHeight, uint32_t(-int32_t(out.h)));
    hd[kBmpPlanes] = 1;
    hd[kBmpBitCount] = 32;
    put(kBmpImageSize, uint32_t(px.size()));
    fwrite(hd, 1, kBmpHeaderBytes, f);
    fwrite(px.data(), 1, px.size(), f);
    fclose(f);
}

void D3D12Backend::CapSave(Target& t, const char* name) {
    Readable r = ReadableResource(t);
    if (!r.res) return;
    DXGI_FORMAT fmt = t.depth ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : t.fmt;
    float mul = t.hdr ? kHeadroom : 1;
    CapSave({.res = r.res, .state = r.state, .fmt = fmt, .w = t.gw, .h = t.gh, .mul = mul}, name);
}

// the frame moves to another target: save the one it leaves
void D3D12Backend::CapStep(const Surface& rt) {
    uint32_t d = rt.header ? mem::VirtualFromPhysical(rt.data) : 0;
    bool same = d == cap_rt_;
    if (same && !(cap_draws_ && d)) return;
    auto it = targets_.find(TargetKey(cap_rt_, false));
    if (cap_rt_ && it != targets_.end()) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_%08X", cap_i_, cap_rt_);
        CapSave(it->second, name);
    }
    auto z = targets_.find(TargetKey(cap_rt_, true));
    if (!same && cap_rt_ && z != targets_.end()) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_%08X_z", cap_i_, cap_rt_);
        CapSave(z->second, name);
    }
    cap_rt_ = d;
}

void D3D12Backend::SetGamma(const uint8_t* ramp) {
    std::memcpy(ramp_, ramp, sizeof(ramp_));
    ramp_dirty_ = true;
}

}  // namespace

std::unique_ptr<Backend> CreateBackend() { return std::make_unique<D3D12Backend>(); }

// xboxrecomp debug: [SLOWGPU] (backend.h)
double SlowGpuMs() {
    static double ms = [] {
        const char* e = std::getenv("RECOMP_SLOWGPU_MS");
        return e && *e ? std::atof(e) : 20.0;
    }();
    return ms;
}

void SlowGpu(double ms, const char* fmt, ...) {
    double limit = SlowGpuMs();
    if (limit <= 0 || ms < limit) return;
    static std::atomic<int> lines{0};
    if (lines++ >= 3000) return;
    char what[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(what, sizeof what, fmt, ap);
    va_end(ap);
    // which thread: the render thread holds the frame up, a shader build thread does not
    char name[48] = "";
    using GetDescription = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static auto get = reinterpret_cast<GetDescription>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));
    PWSTR desc = nullptr;
    if (get && SUCCEEDED(get(GetCurrentThread(), &desc)) && desc) {
        std::snprintf(name, sizeof name, " \"%ls\"", desc);
        LocalFree(desc);
    }
    std::fprintf(stderr, "[SLOWGPU] %.1f ms %s, thread %lu%s\n", ms, what, (unsigned long)GetCurrentThreadId(), name);
    std::fflush(stderr);
}

}  // namespace gpu

// xboxrecomp debug: GPU ms a frame for the game's drawing and the shown frame's passes, and target copies / resolves a
// frame with their megasamples, since the last call (cleared); frames the per frame figures are over
// xboxrecomp debug: [HITCH]: textures made, uploaded, MB and ms uploading, ms making textures (out[7]); pipelines asked of the library or driver,
// ms making them, draws left out for a pipeline; since the last call, cleared
extern "C" void hpcos_HitchGpuStats(double out[8]) {
    out[0] = double(gpu::g_hitch_tex_new.exchange(0));
    out[1] = double(gpu::g_hitch_tex_up.exchange(0));
    out[2] = double(gpu::g_hitch_tex_bytes.exchange(0)) / 1048576.0;
    out[3] = double(gpu::g_hitch_tex_ns.exchange(0)) / 1e6;
    out[4] = double(gpu::g_hitch_pso_n.exchange(0));
    out[5] = double(gpu::g_hitch_pso_ns.exchange(0)) / 1e6;
    out[6] = double(gpu::g_hitch_pso_drops.exchange(0));
    out[7] = double(gpu::g_hitch_texnew_ns.exchange(0)) / 1e6;
}

extern "C" void hpcos_GpuStats(double out[6], uint32_t frames) {
    uint64_t n = gpu::g_gpu_frames.exchange(0);
    double scene = double(gpu::g_gpu_scene_ns.exchange(0)), post = double(gpu::g_gpu_post_ns.exchange(0));
    out[0] = n ? scene / n / 1e6 : 0;
    out[1] = n ? post / n / 1e6 : 0;
    double f = frames ? double(frames) : 1;
    out[2] = double(gpu::g_feedback_copies.exchange(0)) / f;
    out[3] = double(gpu::g_feedback_px.exchange(0)) / f / 1e6;
    out[4] = double(gpu::g_upload_bytes.exchange(0)) / f / 1048576.0;
    out[5] = double(gpu::g_ring_waits.exchange(0)) / f;
}

