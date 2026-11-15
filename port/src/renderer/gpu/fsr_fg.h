#pragma once
// xboxrecomp: AMD FSR 3.1 frame generation (app fsr_frame_generation, off by default; for testing). The game still
// draws its own 59.94 frames; FSR's swap chain shows a generated frame between each two of them, made by its optical
// flow from the two pictures (the game gives no motion vectors: blank ones and a blank depth go in), and paces them.
// Needs amd_fidelityfx_loader_dx12.dll and amd_fidelityfx_framegeneration_dx12.dll beside the exe (third_party/
// fidelityfx, AMD's binary licence there); without them, or when FSR refuses, the plain DXGI swap chain is used.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdint>

namespace gpu::fsr {

// The swap chain through FSR, when the setting is on and FSR is there: true and *out set; false: make the usual one
bool CreateSwapChain(IDXGIFactory* factory, ID3D12CommandQueue* queue, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1& desc,
                     IDXGISwapChain4** out);
// a frame about to be presented, w x h (the back buffer): its frame generation configured and prepared on cl, which
// runs before the present
void Frame(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, IDXGISwapChain* swap, uint32_t w, uint32_t h);
bool On();  // the swap chain is FSR's

}  // namespace gpu::fsr
