#include "core/hw/tegra_x1/gpu/renderer/metal/spatial_upscaler.hpp"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wundef"
#include <MetalFX/MTLFXSpatialScaler.hpp>
#pragma GCC diagnostic pop

#include <algorithm>

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

SpatialUpscaler::SpatialUpscaler(MTL::Device* device_)
    : device{device_->retain()},
      supported{MTLFX::SpatialScalerDescriptor::supportsDevice(device_)} {
    if (!supported)
        LOG_INFO(MetalRenderer, "MetalFX spatial upscaling is not supported, "
                                "frames are scaled bilinearly");
}

SpatialUpscaler::~SpatialUpscaler() {
    for (auto& [key, entry] : entries) {
        if (entry.scaler)
            entry.scaler->release();
        if (entry.output)
            entry.output->release();
    }
    device->release();
}

SpatialUpscaler::Entry& SpatialUpscaler::getEntry(const Key& key) {
    if (auto it = entries.find(key); it != entries.end()) {
        it->second.used = ++clock;
        return it->second;
    }
    // The presented size changes rarely (window resizes): keep a few
    if (entries.size() >= MAX_ENTRIES) {
        auto oldest = std::min_element(
            entries.begin(), entries.end(),
            [](auto& a, auto& b) { return a.second.used < b.second.used; });
        if (oldest->second.scaler)
            oldest->second.scaler->release();
        if (oldest->second.output)
            oldest->second.output->release();
        entries.erase(oldest);
    }

    const auto [input_width, input_height, output_width, output_height,
                format] = key;
    Entry entry{nullptr, nullptr, ++clock};
    NS_STACK_SCOPED auto descriptor =
        MTLFX::SpatialScalerDescriptor::alloc()->init();
    descriptor->setInputWidth(input_width);
    descriptor->setInputHeight(input_height);
    descriptor->setOutputWidth(output_width);
    descriptor->setOutputHeight(output_height);
    descriptor->setColorTextureFormat(format);
    descriptor->setOutputTextureFormat(format);
    // Switch games present SDR frames
    descriptor->setColorProcessingMode(
        MTLFX::SpatialScalerColorProcessingModePerceptual);
    entry.scaler = descriptor->newSpatialScaler(device);
    if (entry.scaler) {
        NS_STACK_SCOPED auto texture_descriptor =
            MTL::TextureDescriptor::alloc()->init();
        texture_descriptor->setTextureType(MTL::TextureType2D);
        texture_descriptor->setPixelFormat(format);
        texture_descriptor->setWidth(output_width);
        texture_descriptor->setHeight(output_height);
        texture_descriptor->setStorageMode(MTL::StorageModePrivate);
        texture_descriptor->setUsage(entry.scaler->outputTextureUsage() |
                                     MTL::TextureUsageShaderRead);
        entry.output = device->newTexture(texture_descriptor);
        LOG_INFO(MetalRenderer, "MetalFX spatial upscaling {}x{} to {}x{} ({})",
                 input_width, input_height, output_width, output_height,
                 static_cast<u64>(format));
    } else {
        LOG_WARN(MetalRenderer,
                 "MetalFX cannot upscale {}x{} to {}x{} ({}), using bilinear",
                 input_width, input_height, output_width, output_height,
                 static_cast<u64>(format));
    }
    return entries.emplace(key, entry).first->second;
}

MTL::Texture* SpatialUpscaler::upscale(MTL::CommandBuffer* command_buffer,
                                       MTL::Texture* input, u32 output_width,
                                       u32 output_height) {
    if (!supported || input->textureType() != MTL::TextureType2D ||
        output_width == 0 || output_height == 0 ||
        output_width > MAX_OUTPUT_SIZE || output_height > MAX_OUTPUT_SIZE)
        return nullptr;
    const auto input_width = static_cast<u32>(input->width());
    const auto input_height = static_cast<u32>(input->height());
    auto& entry = getEntry({input_width, input_height, output_width,
                            output_height, input->pixelFormat()});
    if (!entry.scaler || !entry.output)
        return nullptr;
    const auto required = entry.scaler->colorTextureUsage();
    if ((input->usage() & required) != required)
        return nullptr;

    entry.scaler->setColorTexture(input);
    entry.scaler->setOutputTexture(entry.output);
    entry.scaler->setInputContentWidth(input_width);
    entry.scaler->setInputContentHeight(input_height);
    entry.scaler->encodeToCommandBuffer(command_buffer);
    return entry.output;
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
