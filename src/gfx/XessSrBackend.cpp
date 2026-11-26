// SPDX-License-Identifier: GPL-3.0-only
#include "veyra/gfx/XessSrBackend.h"

#include "veyra/Log.h"
#include "veyra/RuntimePaths.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <string>

#ifdef VEYRA_HAS_XESS_SR
#include <xess/xess_d3d12.h>
#endif

namespace veyra::gfx {

namespace {

std::filesystem::path findXessSrDll() {
    std::error_code ec;
    const auto data = runtime::localDataDirectory();
    const auto app = runtime::applicationRoot();
    const std::filesystem::path candidates[] = {
        data / L"intel" / L"experimental" / L"libxess.dll",
        data / L"intel" / L"xess" / L"libxess.dll",
        app / L"runtime" / L"intel" / L"libxess.dll",
    };
    for (const auto& candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return std::filesystem::absolute(candidate, ec).lexically_normal();
        }
    }
    return {};
}

#ifdef VEYRA_HAS_XESS_SR
const char* resultName(xess_result_t code) {
    switch (code) {
    case XESS_RESULT_SUCCESS: return "SUCCESS";
    case XESS_RESULT_WARNING_NONEXISTING_FOLDER: return "WARNING_NONEXISTING_FOLDER";
    case XESS_RESULT_WARNING_OLD_DRIVER: return "WARNING_OLD_DRIVER";
    case XESS_RESULT_ERROR_UNSUPPORTED_DEVICE: return "ERROR_UNSUPPORTED_DEVICE";
    case XESS_RESULT_ERROR_UNSUPPORTED_DRIVER: return "ERROR_UNSUPPORTED_DRIVER";
    case XESS_RESULT_ERROR_UNINITIALIZED: return "ERROR_UNINITIALIZED";
    case XESS_RESULT_ERROR_INVALID_ARGUMENT: return "ERROR_INVALID_ARGUMENT";
    case XESS_RESULT_ERROR_DEVICE_OUT_OF_MEMORY: return "ERROR_DEVICE_OUT_OF_MEMORY";
    case XESS_RESULT_ERROR_DEVICE: return "ERROR_DEVICE";
    case XESS_RESULT_ERROR_NOT_IMPLEMENTED: return "ERROR_NOT_IMPLEMENTED";
    case XESS_RESULT_ERROR_INVALID_CONTEXT: return "ERROR_INVALID_CONTEXT";
    case XESS_RESULT_ERROR_OPERATION_IN_PROGRESS: return "ERROR_OPERATION_IN_PROGRESS";
    case XESS_RESULT_ERROR_UNSUPPORTED: return "ERROR_UNSUPPORTED";
    case XESS_RESULT_ERROR_CANT_LOAD_LIBRARY: return "ERROR_CANT_LOAD_LIBRARY";
    case XESS_RESULT_ERROR_WRONG_CALL_ORDER: return "ERROR_WRONG_CALL_ORDER";
    default: return "ERROR_UNKNOWN";
    }
}

void xessLogCallback(const char* message, xess_logging_level_t level) {
    if (!message || !*message) return;
    if (level >= XESS_LOGGING_LEVEL_ERROR) log::error("xess-sr", message);
    else if (level >= XESS_LOGGING_LEVEL_WARNING) log::warn("xess-sr", message);
    else log::info("xess-sr", message);
}

xess_quality_settings_t selectQualityForScale(uint32_t renderW, uint32_t renderH,
                                              uint32_t upscaleW, uint32_t upscaleH) {
    const double sx = renderW > 0 ? double(upscaleW) / double(renderW) : 1.0;
    const double sy = renderH > 0 ? double(upscaleH) / double(renderH) : 1.0;
    const double scale = std::max(sx, sy);
    if (scale <= 1.05) return XESS_QUALITY_SETTING_AA;
    if (scale <= 1.35) return XESS_QUALITY_SETTING_ULTRA_QUALITY;
    if (scale <= 1.60) return XESS_QUALITY_SETTING_QUALITY;
    if (scale <= 1.85) return XESS_QUALITY_SETTING_BALANCED;
    if (scale <= 2.15) return XESS_QUALITY_SETTING_PERFORMANCE;
    return XESS_QUALITY_SETTING_ULTRA_PERFORMANCE;
}
#endif

} // namespace

struct XessSrBackend::Impl {
    bool available = false;
    bool xmx = false;
    uint64_t evaluateCount = 0;
    uint64_t failureCount = 0;
    std::string providerVersion;
#ifdef VEYRA_HAS_XESS_SR
    HMODULE dll = nullptr;
    xess_context_handle_t context = nullptr;
    uint32_t maxRenderWidth = 0, maxRenderHeight = 0;
    uint32_t maxUpscaleWidth = 0, maxUpscaleHeight = 0;
#define XESS_SR_PROC(name) decltype(&name) name##Fn = nullptr
    XESS_SR_PROC(xessD3D12CreateContext);
    XESS_SR_PROC(xessD3D12Init);
    XESS_SR_PROC(xessD3D12Execute);
    XESS_SR_PROC(xessDestroyContext);
    XESS_SR_PROC(xessGetVersion);
    XESS_SR_PROC(xessGetIntelXeFXVersion);
    XESS_SR_PROC(xessSetLoggingCallback);
    XESS_SR_PROC(xessIsOptimalDriver);
    XESS_SR_PROC(xessSetVelocityScale);
#undef XESS_SR_PROC
#endif
};

XessSrBackend::XessSrBackend() : p_(std::make_unique<Impl>()) {}

XessSrBackend::~XessSrBackend() { release(); }

std::filesystem::path XessSrBackend::runtimePath() { return findXessSrDll(); }

bool XessSrBackend::runtimeInstalled() { return !findXessSrDll().empty(); }

bool XessSrBackend::initialize(ID3D12Device* device, uint32_t maxRenderWidth, uint32_t maxRenderHeight,
                               uint32_t maxUpscaleWidth, uint32_t maxUpscaleHeight, bool hdr) {
#ifdef VEYRA_HAS_XESS_SR
    auto& p = *p_;
    if (p.available) return true;
    if (!device || !maxRenderWidth || !maxRenderHeight || !maxUpscaleWidth || !maxUpscaleHeight) {
        log::error("xess-sr", "invalid initialization dimensions or device");
        return false;
    }
    const auto dllPath = findXessSrDll();
    if (dllPath.empty()) {
        log::warn("xess-sr", "libxess.dll not found in runtime_local/intel/experimental, runtime_local/intel/xess or runtime/intel");
        return false;
    }
    p.dll = LoadLibraryExW(dllPath.c_str(), nullptr,
                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!p.dll) {
        log::error("xess-sr", std::format("LoadLibrary {} failed win32={}", dllPath.string(), GetLastError()));
        return false;
    }
#define LOAD_REQ(name)                                                                            \
    p.name##Fn = reinterpret_cast<decltype(p.name##Fn)>(GetProcAddress(p.dll, #name));            \
    if (!p.name##Fn) {                                                                            \
        log::error("xess-sr", "missing required export " #name);                                  \
        release();                                                                                \
        return false;                                                                             \
    }
#define LOAD_OPT(name) \
    p.name##Fn = reinterpret_cast<decltype(p.name##Fn)>(GetProcAddress(p.dll, #name))
    LOAD_REQ(xessD3D12CreateContext)
    LOAD_REQ(xessD3D12Init)
    LOAD_REQ(xessD3D12Execute)
    LOAD_REQ(xessDestroyContext)
    LOAD_OPT(xessGetVersion);
    LOAD_OPT(xessGetIntelXeFXVersion);
    LOAD_OPT(xessSetLoggingCallback);
    LOAD_OPT(xessIsOptimalDriver);
    LOAD_OPT(xessSetVelocityScale);
#undef LOAD_REQ
#undef LOAD_OPT

    xess_result_t result = p.xessD3D12CreateContextFn(device, &p.context);
    if (result != XESS_RESULT_SUCCESS || !p.context) {
        log::error("xess-sr", std::format("xessD3D12CreateContext failed result={} ({})", int(result), resultName(result)));
        release();
        return false;
    }

    if (p.xessSetLoggingCallbackFn) {
        p.xessSetLoggingCallbackFn(p.context, XESS_LOGGING_LEVEL_WARNING, xessLogCallback);
    }

    if (p.xessIsOptimalDriverFn) {
        const auto driverStatus = p.xessIsOptimalDriverFn(p.context);
        if (driverStatus == XESS_RESULT_WARNING_OLD_DRIVER) {
            log::warn("xess-sr", "Intel/GPU driver is older than recommended for optimal XeSS performance");
        }
    }

    xess_version_t sdkVer{};
    xess_version_t xefxVer{};
    if (p.xessGetVersionFn) {
        p.xessGetVersionFn(&sdkVer);
    }
    if (p.xessGetIntelXeFXVersionFn && p.xessGetIntelXeFXVersionFn(p.context, &xefxVer) == XESS_RESULT_SUCCESS) {
        p.xmx = (xefxVer.major > 0 || xefxVer.minor > 0 || xefxVer.patch > 0);
    }
    p.providerVersion = p.xmx
        ? std::format("XeSS {}.{}.{} (XMX XeFX {}.{}.{})", sdkVer.major, sdkVer.minor, sdkVer.patch, xefxVer.major, xefxVer.minor, xefxVer.patch)
        : std::format("XeSS {}.{}.{} (DP4a)", sdkVer.major, sdkVer.minor, sdkVer.patch);

    xess_d3d12_init_params_t init{};
    init.outputResolution = {maxUpscaleWidth, maxUpscaleHeight};
    init.qualitySetting = selectQualityForScale(maxRenderWidth, maxRenderHeight, maxUpscaleWidth, maxUpscaleHeight);
    init.initFlags = hdr ? uint32_t(XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE) : uint32_t(XESS_INIT_FLAG_LDR_INPUT_COLOR);
    result = p.xessD3D12InitFn(p.context, &init);
    if (result != XESS_RESULT_SUCCESS) {
        log::error("xess-sr", std::format("xessD3D12Init failed result={} ({}) render={}x{} output={}x{} quality={} hdr={}",
            int(result), resultName(result), maxRenderWidth, maxRenderHeight, maxUpscaleWidth, maxUpscaleHeight,
            int(init.qualitySetting), hdr));
        release();
        return false;
    }

    if (p.xessSetVelocityScaleFn) {
        p.xessSetVelocityScaleFn(p.context, 1.0f, 1.0f);
    }

    p.maxRenderWidth = maxRenderWidth;
    p.maxRenderHeight = maxRenderHeight;
    p.maxUpscaleWidth = maxUpscaleWidth;
    p.maxUpscaleHeight = maxUpscaleHeight;
    p.available = true;
    log::info("xess-sr", std::format("initialized provider={} xmx={} render={}x{} output={}x{} quality={} hdr={}",
        p.providerVersion, p.xmx, maxRenderWidth, maxRenderHeight, maxUpscaleWidth, maxUpscaleHeight,
        int(init.qualitySetting), hdr));
    return true;
#else
    (void)device; (void)maxRenderWidth; (void)maxRenderHeight;
    (void)maxUpscaleWidth; (void)maxUpscaleHeight; (void)hdr;
    log::error("xess-sr", "Intel XeSS SR SDK unavailable at build time");
    return false;
#endif
}

bool XessSrBackend::evaluate(ID3D12GraphicsCommandList* list, ID3D12Resource* color, ID3D12Resource* motion,
                             ID3D12Resource* depth, ID3D12Resource* output, uint32_t renderWidth, uint32_t renderHeight,
                             uint32_t upscaleWidth, uint32_t upscaleHeight, bool reset) {
#ifdef VEYRA_HAS_XESS_SR
    auto& p = *p_;
    if (!p.available || !p.context || !list || !color || !motion || !depth || !output) {
        ++p.failureCount;
        return false;
    }
    if (upscaleWidth != p.maxUpscaleWidth || upscaleHeight != p.maxUpscaleHeight) {
        log::warn("xess-sr", std::format("output extent mismatch got={}x{} init={}x{}",
            upscaleWidth, upscaleHeight, p.maxUpscaleWidth, p.maxUpscaleHeight));
    }
    xess_d3d12_execute_params_t exec{};
    exec.pColorTexture = color;
    exec.pVelocityTexture = motion;
    exec.pDepthTexture = depth;
    exec.pOutputTexture = output;
    exec.jitterOffsetX = 0.0f;
    exec.jitterOffsetY = 0.0f;
    exec.exposureScale = 1.0f;
    exec.resetHistory = reset ? 1u : 0u;
    exec.inputWidth = renderWidth;
    exec.inputHeight = renderHeight;

    const auto result = p.xessD3D12ExecuteFn(p.context, list, &exec);
    if (result != XESS_RESULT_SUCCESS) {
        ++p.failureCount;
        log::error("xess-sr", std::format("xessD3D12Execute failed result={} ({}) render={}x{} reset={}",
            int(result), resultName(result), renderWidth, renderHeight, reset));
        return false;
    }
    ++p.evaluateCount;
    return true;
#else
    (void)list; (void)color; (void)motion; (void)depth; (void)output;
    (void)renderWidth; (void)renderHeight; (void)upscaleWidth; (void)upscaleHeight; (void)reset;
    return false;
#endif
}

void XessSrBackend::release() {
#ifdef VEYRA_HAS_XESS_SR
    auto& p = *p_;
    if (p.context && p.xessDestroyContextFn) {
        const auto result = p.xessDestroyContextFn(p.context);
        if (result != XESS_RESULT_SUCCESS) {
            log::warn("xess-sr", std::format("xessDestroyContext result={} ({})", int(result), resultName(result)));
        }
        p.context = nullptr;
    }
    if (p.dll) {
        FreeLibrary(p.dll);
        p.dll = nullptr;
    }
    p.available = false;
    p.xmx = false;
#endif
}

bool XessSrBackend::created() const { return p_->available; }
bool XessSrBackend::usesXmx() const { return p_->xmx; }
uint64_t XessSrBackend::evaluateCount() const { return p_->evaluateCount; }
uint64_t XessSrBackend::failureCount() const { return p_->failureCount; }
const char* XessSrBackend::providerVersion() const { return p_->providerVersion.c_str(); }

} // namespace veyra::gfx
