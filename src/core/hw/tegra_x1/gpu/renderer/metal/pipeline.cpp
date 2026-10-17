#include "core/hw/tegra_x1/gpu/renderer/metal/pipeline.hpp"
#include "core/hw/tegra_x1/gpu/visual_diagnostics.hpp"

#include "core/hw/tegra_x1/gpu/perf_stats.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/maxwell_to_mtl.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/shader.hpp"
#include <optional>

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

Pipeline::Pipeline(MTL::Device* /*device*/,
                   const PipelineDescriptor& descriptor,
                   BinaryArchiveCache& binary_archive_cache)
    : PipelineBase(descriptor) {
    MTL::RenderPipelineDescriptor* pipeline_descriptor =
        MTL::RenderPipelineDescriptor::alloc()->init();

    // Shaders
    const auto vertex_shader = static_cast<Shader*>(
        descriptor.shaders[static_cast<u32>(ShaderType::Vertex)]);
    const auto fragment_shader = static_cast<Shader*>(
        descriptor.shaders[static_cast<u32>(ShaderType::Fragment)]);
    // A shader that failed to compile has no function; Metal asserts on such
    // a descriptor, so leave the pipeline null and let draws using it be
    // skipped
    if (vertex_shader == nullptr || vertex_shader->getFunction() == nullptr ||
        (fragment_shader != nullptr && fragment_shader->getFunction() == nullptr)) {
        LOG_ERROR(MetalRenderer,
                  "Skipping pipeline with a shader that failed to compile");
        pipeline_descriptor->release();
        return;
    }
    pipeline_descriptor->setVertexFunction(vertex_shader->getFunction());
    // Layered rendering and viewport selection require the primitive class
    if (vertex_shader->getDescriptor().writes_layer) {
        switch (descriptor.topology_class) {
        case PrimitiveTopologyClass::Point:
            pipeline_descriptor->setInputPrimitiveTopology(
                MTL::PrimitiveTopologyClassPoint);
            break;
        case PrimitiveTopologyClass::Line:
            pipeline_descriptor->setInputPrimitiveTopology(
                MTL::PrimitiveTopologyClassLine);
            break;
        default:
            pipeline_descriptor->setInputPrimitiveTopology(
                MTL::PrimitiveTopologyClassTriangle);
            break;
        }
    }
    // Depth-only passes can intentionally disable the fragment stage.
    if (fragment_shader != nullptr)
        pipeline_descriptor->setFragmentFunction(fragment_shader->getFunction());

    // Vertex state

    // Vertex attribute states
    u32 min_vertex_array_strides[VERTEX_ARRAY_COUNT] = {0};
    for (u32 i = 0; i < VERTEX_ATTRIB_COUNT; i++) {
        const auto& vertex_attrib_state =
            descriptor.vertex_state.vertex_attrib_states[i];
        if (vertex_attrib_state.type == engines::VertexAttribType::None)
            continue;

        // HACK: how are attributes disabled?
        if (vertex_attrib_state.is_fixed ||
            vertex_attrib_state.buffer_id >= VERTEX_ARRAY_COUNT)
            continue;

        ASSERT_DEBUG(i != 31, MetalRenderer,
                     "Vertex attribute 31 is not available on Metal");

        // LOG_DEBUG(MetalRenderer,
        //           "Buffer ID: {}, is fixed: {}, offset: 0x{:08x}",
        //           vertex_attrib_state.buffer_id,
        //           vertex_attrib_state.is_fixed, vertex_attrib_state.offset);

        auto vertex_attrib_descriptor =
            pipeline_descriptor->vertexDescriptor()->attributes()->object(i);
        vertex_attrib_descriptor->setBufferIndex(
            getVertexBufferIndex(vertex_attrib_state.buffer_id));
        // TODO: is fixed
        vertex_attrib_descriptor->setOffset(vertex_attrib_state.offset);
        vertex_attrib_descriptor->setFormat(toMtlVertexFormat(
            vertex_attrib_state.type, vertex_attrib_state.size,
            vertex_attrib_state.bgra));

        auto& min_vertex_array_stride =
            min_vertex_array_strides[vertex_attrib_state.buffer_id];
        min_vertex_array_stride = std::max(
            min_vertex_array_stride,
            static_cast<u32>(vertex_attrib_state.offset +
                             getVertexFormatSize(vertex_attrib_state.size)));
    }

    // Vertex arrays
    for (u32 i = 0; i < VERTEX_ARRAY_COUNT; i++) {
        const auto& vertex_array = descriptor.vertex_state.vertex_arrays[i];
        if (!vertex_array.enable)
            continue;

        u32 stride = vertex_array.stride;
        MTL::VertexStepFunction step_function;
        u32 step_rate;
        const auto min_stride = min_vertex_array_strides[i];
        // Instances are drawn one at a time and the engine binds the element
        // of the current instance, so instanced streams are constant
        if (stride == 0 || vertex_array.is_per_instance) {
            stride = std::max(stride, min_stride);
            step_function = MTL::VertexStepFunctionConstant;
            step_rate = 0;
        } else {
            if (stride < min_stride)
                LOG_WARN(MetalRenderer,
                         "Vertex array stride ({}) is less than minimum "
                         "required stride ({})",
                         stride, min_stride);
            step_function = MTL::VertexStepFunctionPerVertex;
            step_rate = 1;
        }

        auto vertex_array_descriptor =
            pipeline_descriptor->vertexDescriptor()->layouts()->object(
                getVertexBufferIndex(i));
        vertex_array_descriptor->setStride(stride);
        vertex_array_descriptor->setStepFunction(step_function);
        vertex_array_descriptor->setStepRate(step_rate);

        LOG_DEBUG(MetalRenderer, "Vertex array {} enabled, stride: {}", i,
                  vertex_array.stride);
    }

    // Depth stencil target
    if (descriptor.depth_stencil_format != TextureFormat::Invalid)
        setDepthStencilPixelFormat(
            pipeline_descriptor,
            toMtlPixelFormat(descriptor.depth_stencil_format));

    // Color targets
    for (u32 i = 0; i < COLOR_TARGET_COUNT; i++) {
        const auto& color_target_state = descriptor.color_target_states[i];
        if (color_target_state.format == TextureFormat::Invalid)
            continue;

        auto color_attachment_descriptor =
            pipeline_descriptor->colorAttachments()->object(i);
        color_attachment_descriptor->setPixelFormat(
            toMtlPixelFormat(color_target_state.format));

        MTL::ColorWriteMask write_mask = MTL::ColorWriteMaskNone;
        if (any(color_target_state.write_mask & engines::ColorWriteMask::Red))
            write_mask |= MTL::ColorWriteMaskRed;
        if (any(color_target_state.write_mask & engines::ColorWriteMask::Green))
            write_mask |= MTL::ColorWriteMaskGreen;
        if (any(color_target_state.write_mask & engines::ColorWriteMask::Blue))
            write_mask |= MTL::ColorWriteMaskBlue;
        if (any(color_target_state.write_mask & engines::ColorWriteMask::Alpha))
            write_mask |= MTL::ColorWriteMaskAlpha;
        if (fragment_shader != nullptr) {
            const auto mask = (fragment_shader->getDescriptor().resource_mapping.fragment_output_mask >> (i * 4)) & 0xf;
            MTL::ColorWriteMask export_mask = MTL::ColorWriteMaskNone;
            if (mask & 1) export_mask |= MTL::ColorWriteMaskRed;
            if (mask & 2) export_mask |= MTL::ColorWriteMaskGreen;
            if (mask & 4) export_mask |= MTL::ColorWriteMaskBlue;
            if (mask & 8) export_mask |= MTL::ColorWriteMaskAlpha;
            write_mask &= export_mask;
        }
        color_attachment_descriptor->setWriteMask(write_mask);

        if (color_target_state.blend_enabled) {
            color_attachment_descriptor->setBlendingEnabled(true);
            color_attachment_descriptor->setRgbBlendOperation(
                toMtlBlendOperation(color_target_state.rgb_op));
            color_attachment_descriptor->setSourceRGBBlendFactor(
                toMtlBlendFactor(color_target_state.src_rgb_factor));
            color_attachment_descriptor->setDestinationRGBBlendFactor(
                toMtlBlendFactor(color_target_state.dst_rgb_factor));
            color_attachment_descriptor->setAlphaBlendOperation(
                toMtlBlendOperation(color_target_state.alpha_op));
            color_attachment_descriptor->setSourceAlphaBlendFactor(
                toMtlBlendFactor(color_target_state.src_alpha_factor));
            color_attachment_descriptor->setDestinationAlphaBlendFactor(
                toMtlBlendFactor(color_target_state.dst_alpha_factor));
        }
    }

    // Pipeline
    std::optional<ScopedPerfTimer> compile_timer;
    if (PerfStats::isEnabled()) {
        compile_timer.emplace(PerfStats::instance().pipeline_compile_ns);
        PerfStats::instance().pipeline_compiles++;
    }
    NS::Error* error = nullptr;
    pipeline = binary_archive_cache.createPipeline(
        pipeline_descriptor, &error,
        fmt::format("{}:{}", vertex_shader->getCacheKey(),
                    fragment_shader != nullptr ? fragment_shader->getCacheKey()
                                               : "no-fragment"));
    if (auto& visual = VisualDiagnostics::instance(); visual.selected())
        visual.event("metal_pipeline_result", {{"available", pipeline ? "1" : "0"},
            {"vertex_key", vertex_shader->getCacheKey()},
            {"fragment_key", fragment_shader ? fragment_shader->getCacheKey() : "no-fragment"},
            {"message", error ? error->localizedDescription()->utf8String() : ""}});
    pipeline_descriptor->release();
    if (error != nullptr) {
        LOG_ERROR(MetalRenderer, "Failed to create pipeline: {}",
                  error->localizedDescription()->utf8String());
        error->release(); // TODO: release?
        return;
    }
}

Pipeline::~Pipeline() {
    if (pipeline != nullptr)
        pipeline->release();
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
