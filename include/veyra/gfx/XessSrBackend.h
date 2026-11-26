// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Intel XeSS Super Resolution upscaling backend (DirectX 12, dynamic libxess.dll).
//
// Exploits hardware XMX matrix engines on Intel Arc GPUs (such as Arc A730M)
// and automatically falls back to DP4a/SM6.4 shaders on non-XMX DX12 GPUs.
// Consumes Veyra's linear working color, render-extent pixel motion vectors
// (current -> previous convention matching XeSS velocity), and render-extent
// constant depth, writing the upscaled image directly into the caller's UAV.

#include <d3d12.h>

#include <cstdint>
#include <filesystem>
#include <memory>

namespace veyra::gfx {

class XessSrBackend {
public:
    XessSrBackend();
    ~XessSrBackend();

    XessSrBackend(const XessSrBackend&) = delete;
    XessSrBackend& operator=(const XessSrBackend&) = delete;

    // Resolves libxess.dll from runtime_local/intel/{experimental,xess} or runtime/intel.
    static std::filesystem::path runtimePath();
    static bool runtimeInstalled();

    bool initialize(ID3D12Device* device, uint32_t maxRenderWidth, uint32_t maxRenderHeight,
                    uint32_t maxUpscaleWidth, uint32_t maxUpscaleHeight, bool hdr);

    bool evaluate(ID3D12GraphicsCommandList* list, ID3D12Resource* color, ID3D12Resource* motion,
                  ID3D12Resource* depth, ID3D12Resource* output, uint32_t renderWidth, uint32_t renderHeight,
                  uint32_t upscaleWidth, uint32_t upscaleHeight, bool reset);

    void release();
    bool created() const;
    bool usesXmx() const;
    uint64_t evaluateCount() const;
    uint64_t failureCount() const;
    const char* providerVersion() const;

    struct Impl;

private:
    std::unique_ptr<Impl> p_;
};

} // namespace veyra::gfx
