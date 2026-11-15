#pragma once
#include <atomic>
#include <cstdint>
#include <memory>

#include "gpu/state.h"

namespace gpu {

// D3DCLEAR flags as the XDK defines them
enum : uint32_t {
    kClearTarget = 0xF0,  // ARGB channels
    kClearZ = 0x01,
    kClearStencil = 0x02,
};

constexpr int kOcclusionParams = 10;

class Backend {
public:
    virtual ~Backend() = default;
    // the title's back buffer size, the output window may differ
    virtual bool Init(uint32_t width, uint32_t height) = 0;
    // rects: count D3DRECTs (4 x int32), null for the whole viewport
    virtual void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                       uint32_t stencil) = 0;
    // indices: the title's 16 bit indices, null for non indexed draws, then start is the first vertex
    virtual void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) = 0;
    virtual void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects_va,
                      uint32_t dst_points_va) = 0;
    // pillarbox: the frame is a 4:3 picture
    virtual void Present(const Surface& front, bool pillarbox) = 0;
    // the CPU is about to touch this resource's memory: write GPU results back first
    virtual void Sync(uint32_t data_va, uint32_t size) = 0;
    // a lock that does not wait for the GPU (NOOVERWRITE): a result a frame or two old is what the console gives too
    virtual void SyncLate(uint32_t data_va) = 0;
    // SyncLate in two for the render thread: DeliverLate (any thread) writes the newest finished copy to guest memory,
    // false when there is none recent; RecordLate (in order with the draws) starts a copy for a later lock
    virtual bool DeliverLate(uint32_t) { return false; }
    virtual void RecordLate(uint32_t) {}
    virtual void Flush() = 0;
    virtual void SetGamma(const uint8_t* ramp /*3 x 256*/) = 0;
    // ambient occlusion from the depth of the scene so far, multiplied into its colour. p: 1 / the projection's x and y
    // scale, its x and y centre, then view z = p[4] / (ndc depth - p[5]), then the fog already drawn:
    // amount p[8] * saturate(p[6] * view z + p[7]), then p[9]: 1 for GTAO, 0 for SSAO
    virtual void Occlusion(const State&, const float /*p*/[kOcclusionParams]) {}

    // the XDK's fences and busy checks against the render thread; a backend that draws at once is never behind
    virtual uint32_t Fence() { return ++fence_; }
    virtual bool FencePending(uint32_t) { return false; }
    virtual void WaitFence(uint32_t) {}
    virtual bool Busy(uint32_t /*resource*/) { return false; }
    virtual void WaitResource(uint32_t /*resource*/) {}

private:
    uint32_t fence_ = 0;
};

std::unique_ptr<Backend> CreateBackend();
// the same backend driven from a thread of its own: calls return once recorded
std::unique_ptr<Backend> CreateThreadedBackend(std::unique_ptr<Backend> inner);

// the pipeline library to disk now: before a quick reboot starts the next process, and at quit
void SavePipelineCache();

// the frame buffer holds a 4:3 picture until a draw from outside it (in_splash: draws that belong to it)
inline std::atomic<bool> g_splash{false};
inline std::atomic<int> g_in_splash{0};
inline std::atomic<bool> g_shown_splash{false};  // the frame on screen was presented 4:3
// the game side drew a 4:3 picture this frame (versus' loading page): the next Swap shows it 4:3, whatever drew after
inline std::atomic<bool> g_splash_next{false};

// xboxrecomp: a loading screen kept 4:3 in 16:9 (hle_hud.c, Enhancements > Keep loading screens 4:3) was drawn: the
// frame's sides beside the squeezed picture are cleared black when it is shown (d3d12.cpp DrawSdrFrame)
inline std::atomic<bool> g_black_sides{false};

// F3: save the next frame, every draw and target, under captures
inline std::atomic<bool> g_capture{false};

// xboxrecomp: pipelines made ahead of need at startup from shadercache\pipelines_list.bin (F10's Debug tab, the
// corner note while it runs): how many, how many done, how many could not be made here
inline std::atomic<uint32_t> g_prebuild_total{0}, g_prebuild_done{0}, g_prebuild_failed{0};
inline std::atomic<uint32_t> g_pipelines_recorded{0};

// the game's shadow map, the colour surface bound beside it and the swizzled chain aliased over the map's memory
// that blurs shadow edges on screen, by surface data as the headers hold it; blur_hi is 0 until the game made them
struct ShadowTargets {
    uint32_t map = 0, color = 0, blur_lo = 0, blur_hi = 0;
};
inline ShadowTargets g_shadow;  // written under the D3D lock

// xboxrecomp debug: [SLOWGPU] lines, renderer work that took RECOMP_SLOWGPU_MS (default 20) or more: what it was, how
// long, on which thread (render = the frame waits on it; shader build = it does not). 0 turns them off. d3d12.cpp
double SlowGpuMs();
void SlowGpu(double ms, const char* fmt, ...);

}  // namespace gpu
