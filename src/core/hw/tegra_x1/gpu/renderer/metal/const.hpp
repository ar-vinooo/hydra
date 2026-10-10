#pragma once

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wold-style-cast"

#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#pragma GCC diagnostic pop

#include "core/hw/tegra_x1/gpu/renderer/const.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

__attribute__((unused)) static inline void stackAutoRelease(void* object) {
    (*reinterpret_cast<NS::Object**>(object))->release();
}

#define NS_STACK_SCOPED                                                        \
    __attribute__((cleanup(stackAutoRelease))) __attribute__((unused))

#define TMP_AUTORELEASE_POOL_BEGIN()                                           \
    NS::AutoreleasePool* tmp_pool_ = NS::AutoreleasePool::alloc()->init()
#define TMP_AUTORELEASE_POOL_END() tmp_pool_->release()

// Cast from const char* to NS::String*
inline NS::String* toNsString(const char* str) {
    return NS::String::string(str, NS::ASCIIStringEncoding);
}

// Cast from std::string to NS::String*
inline NS::String* toNsString(const std::string& str) {
    return toNsString(str.c_str());
}

// Cast from const char* to NS::URL*
inline NS::URL* toNsurl(const char* str) {
    return NS::URL::fileURLWithPath(toNsString(str));
}

// Cast from std::string to NS::URL*
inline NS::URL* toNsurl(const std::string& str) { return toNsurl(str.c_str()); }

inline NS::String* getLabel(const std::string_view label,
                            const void* identifier) {
    return toNsString(
        fmt::format("{} ({})", label,
                    std::to_string(reinterpret_cast<uintptr_t>(identifier))));
}

constexpr usize BUFFER_COUNT = 31;
constexpr usize TEXTURE_COUNT = 31;
constexpr usize SAMPLER_COUNT = 16;

inline u32 getVertexBufferIndex(u32 index) { return BUFFER_COUNT - index - 1; }

inline void
setDepthStencilPixelFormat(MTL::RenderPipelineDescriptor* pipeline_descriptor,
                           MTL::PixelFormat pixel_format) {
    bool has_depth = false;
    bool has_stencil = false;
    switch (pixel_format) {
    case MTL::PixelFormatDepth16Unorm:
    case MTL::PixelFormatDepth32Float:
        has_depth = true;
        break;
    case MTL::PixelFormatDepth24Unorm_Stencil8:
    case MTL::PixelFormatDepth32Float_Stencil8:
        has_depth = true;
        has_stencil = true;
        break;
    case MTL::PixelFormatStencil8:
    case MTL::PixelFormatX24_Stencil8:
    case MTL::PixelFormatX32_Stencil8:
        has_stencil = true;
        break;
    default:
        break;
    }
    pipeline_descriptor->setDepthAttachmentPixelFormat(
        has_depth ? pixel_format : MTL::PixelFormatInvalid);
    pipeline_descriptor->setStencilAttachmentPixelFormat(
        has_stencil ? pixel_format : MTL::PixelFormatInvalid);
}

inline u32 getRenderTargetArrayLength(MTL::Texture* texture) {
    switch (texture->textureType()) {
    case MTL::TextureType3D:
        return static_cast<u32>(texture->depth());
    case MTL::TextureTypeCube:
    case MTL::TextureTypeCubeArray:
        return static_cast<u32>(texture->arrayLength()) * 6;
    default:
        return static_cast<u32>(texture->arrayLength());
    }
}

inline MTL::Library* createLibraryFromSource(MTL::Device* device,
                                             const std::string_view source) {
    NS::Error* error;
    // TODO: don't construct a new string?
    MTL::Library* library =
        device->newLibrary(toNsString(std::string(source)), nullptr, &error);
    if (error != nullptr) {
        LOG_ERROR(Gpu, "Failed to create library: {}",
                  error->localizedDescription()->utf8String());
        // TODO: don't log?
        LOG_INFO(MetalRenderer, "Shader source: {}", source);
        error->release(); // TODO: release?
        return nullptr;
    }

    return library;
}

inline MTL::Function* createFunctionFromSource(MTL::Device* device,
                                               const std::string_view source,
                                               const std::string_view name) {
    auto library = createLibraryFromSource(device, source);
    // TODO: don't construct a new string?
    auto function = library->newFunction(toNsString(std::string(name)));
    library->release();

    return function;
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
