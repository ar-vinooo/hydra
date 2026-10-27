#include "core/hw/tegra_x1/gpu/renderer/metal/pipeline.hpp"
#include "core/hw/tegra_x1/gpu/renderer/geometry.hpp"
#include "core/hw/tegra_x1/gpu/renderer/tessellation.hpp"
#include "core/hw/tegra_x1/gpu/visual_diagnostics.hpp"

#include "core/hw/tegra_x1/gpu/perf_stats.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/maxwell_to_mtl.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/shader.hpp"
#include <optional>

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

Pipeline::Pipeline(MTL::Device* device,
                   const PipelineDescriptor& descriptor,
                   BinaryArchiveCache& binary_archive_cache)
    : PipelineBase(descriptor) {
    ScopedGpuWork work(GpuWork::Shader);
    MTL::RenderPipelineDescriptor* pipeline_descriptor =
        MTL::RenderPipelineDescriptor::alloc()->init();

    // Shaders
    const auto vertex_shader = static_cast<Shader*>(
        descriptor.shaders[static_cast<u32>(ShaderType::Vertex)]);
    const auto fragment_shader = static_cast<Shader*>(
        descriptor.shaders[static_cast<u32>(ShaderType::Fragment)]);
    // Geometry pipelines (geometry.hpp): the vertex shader captures, the
    // passthrough draws
    const auto geometry_shader = static_cast<Shader*>(
        descriptor.shaders[static_cast<u32>(ShaderType::Geometry)]);
    const auto passthrough_shader =
        static_cast<Shader*>(descriptor.geometry_passthrough);
    const bool geometry =
        geometry_shader != nullptr && passthrough_shader != nullptr;
    // Tessellation pipelines (tessellation.hpp): the vertex shader captures,
    // the evaluation shader draws
    const auto control_shader = static_cast<Shader*>(
        descriptor.shaders[static_cast<u32>(ShaderType::TessControl)]);
    const auto evaluation_shader = static_cast<Shader*>(
        descriptor.shaders[static_cast<u32>(ShaderType::TessEvaluation)]);
    const auto factor_shader =
        static_cast<Shader*>(descriptor.tessellation_factors);
    const bool tessellation = control_shader != nullptr &&
                              evaluation_shader != nullptr &&
                              factor_shader != nullptr;
    if (tessellation && (control_shader->getFunction() == nullptr ||
                         evaluation_shader->getFunction() == nullptr ||
                         factor_shader->getFunction() == nullptr)) {
        LOG_ERROR(MetalRenderer, "Skipping pipeline with a tessellation "
                                 "shader that failed to compile");
        pipeline_descriptor->release();
        return;
    }
    if (geometry && (geometry_shader->getFunction() == nullptr ||
                     passthrough_shader->getFunction() == nullptr)) {
        LOG_ERROR(MetalRenderer, "Skipping pipeline with a geometry shader "
                                 "that failed to compile");
        pipeline_descriptor->release();
        return;
    }
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
    // (the drawn one, which a geometry shader decides)
    if (geometry       ? passthrough_shader->getDescriptor().writes_layer
        : tessellation ? evaluation_shader->getDescriptor().writes_layer
                       : vertex_shader->getDescriptor().writes_layer) {
        const auto output = static_cast<geometry::Output>(
            geometry_shader != nullptr ? geometry_shader->getDescriptor()
                                             .resource_mapping.geometry_output
                                       : 0);
        switch (geometry ? (output == geometry::Output::Points
                                ? MTL::PrimitiveTopologyClassPoint
                            : output == geometry::Output::LineStrip
                                ? MTL::PrimitiveTopologyClassLine
                                : MTL::PrimitiveTopologyClassTriangle)
                         : descriptor.topology_class) {
        case MTL::PrimitiveTopologyClassPoint:
        case MTL::PrimitiveTopologyClassLine:
            pipeline_descriptor->setInputPrimitiveTopology(
                descriptor.topology_class);
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
    // With one sample, alpha-to-coverage keeps or drops the whole pixel
    pipeline_descriptor->setAlphaToCoverageEnabled(descriptor.alpha_to_coverage);
    pipeline_descriptor->setAlphaToOneEnabled(descriptor.alpha_to_one);
    // As many samples as the render targets have on the host
    pipeline_descriptor->setRasterSampleCount(
        getHostSampleCount(device, descriptor.sample_count));

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
                color_target_state.rgb_op);
            color_attachment_descriptor->setSourceRGBBlendFactor(
                color_target_state.src_rgb_factor);
            color_attachment_descriptor->setDestinationRGBBlendFactor(
                color_target_state.dst_rgb_factor);
            color_attachment_descriptor->setAlphaBlendOperation(
                color_target_state.alpha_op);
            color_attachment_descriptor->setSourceAlphaBlendFactor(
                color_target_state.src_alpha_factor);
            color_attachment_descriptor->setDestinationAlphaBlendFactor(
                color_target_state.dst_alpha_factor);
        }
    }

    // Geometry pipelines: the capture keeps the vertex fetch, the others
    // read buffers
    if (geometry) {
        const auto create = [&](MTL::RenderPipelineDescriptor* state_descriptor,
                                const char* what) -> MTL::RenderPipelineState* {
            NS::Error* state_error = nullptr;
            auto* state = device->newRenderPipelineState(state_descriptor,
                                                         &state_error);
            if (state == nullptr)
                LOG_ERROR(MetalRenderer, "Failed to create the {} pipeline: {}",
                          what,
                          state_error != nullptr
                              ? state_error->localizedDescription()->utf8String()
                              : "unknown error");
            return state;
        };
        auto* capture = static_cast<MTL::RenderPipelineDescriptor*>(
            pipeline_descriptor->copy());
        capture->setFragmentFunction(nullptr);
        capture->setRasterizationEnabled(false);
        capture->setInputPrimitiveTopology(
            MTL::PrimitiveTopologyClassUnspecified);
        capture_pipeline = create(capture, "vertex capture");
        capture->release();

        pipeline_descriptor->setVertexDescriptor(nullptr);
        auto* emit = static_cast<MTL::RenderPipelineDescriptor*>(
            pipeline_descriptor->copy());
        emit->setVertexFunction(geometry_shader->getFunction());
        emit->setFragmentFunction(nullptr);
        emit->setRasterizationEnabled(false);
        emit->setInputPrimitiveTopology(MTL::PrimitiveTopologyClassUnspecified);
        geometry_pipeline = create(emit, "geometry shader");
        emit->release();

        pipeline_descriptor->setVertexFunction(passthrough_shader->getFunction());
        if (capture_pipeline == nullptr || geometry_pipeline == nullptr) {
            if (capture_pipeline != nullptr)
                capture_pipeline->release();
            if (geometry_pipeline != nullptr)
                geometry_pipeline->release();
            capture_pipeline = nullptr;
            geometry_pipeline = nullptr;
            pipeline_descriptor->release();
            return;
        }
    }

    if (tessellation) {
        const auto create = [&](MTL::RenderPipelineDescriptor* state_descriptor,
                                const char* what) -> MTL::RenderPipelineState* {
            NS::Error* state_error = nullptr;
            auto* state = device->newRenderPipelineState(state_descriptor,
                                                         &state_error);
            if (state == nullptr)
                LOG_ERROR(MetalRenderer, "Failed to create the {} pipeline: {}",
                          what,
                          state_error != nullptr
                              ? state_error->localizedDescription()->utf8String()
                              : "unknown error");
            return state;
        };
        const auto raster_off = [&](Shader* shader, const char* what) {
            auto* state_descriptor = static_cast<MTL::RenderPipelineDescriptor*>(
                pipeline_descriptor->copy());
            state_descriptor->setVertexFunction(shader->getFunction());
            state_descriptor->setFragmentFunction(nullptr);
            state_descriptor->setRasterizationEnabled(false);
            state_descriptor->setInputPrimitiveTopology(
                MTL::PrimitiveTopologyClassUnspecified);
            auto* state = create(state_descriptor, what);
            state_descriptor->release();
            return state;
        };
        capture_pipeline = raster_off(vertex_shader, "vertex capture");
        pipeline_descriptor->setVertexDescriptor(nullptr);
        tess_control_pipeline =
            raster_off(control_shader, "tessellation control shader");
        tess_factor_pipeline =
            raster_off(factor_shader, "tessellation factor");

        // The evaluation shader runs per tessellated vertex
        pipeline_descriptor->setVertexFunction(evaluation_shader->getFunction());
        switch (tessellation::getSpacing(descriptor.tessellation_parameters)) {
        case tessellation::Spacing::FractionalOdd:
            pipeline_descriptor->setTessellationPartitionMode(
                MTL::TessellationPartitionModeFractionalOdd);
            break;
        case tessellation::Spacing::FractionalEven:
            pipeline_descriptor->setTessellationPartitionMode(
                MTL::TessellationPartitionModeFractionalEven);
            break;
        default:
            pipeline_descriptor->setTessellationPartitionMode(
                MTL::TessellationPartitionModeInteger);
            break;
        }
        pipeline_descriptor->setTessellationOutputWindingOrder(
            tessellation::getOutputPrimitives(descriptor.tessellation_parameters) ==
                    tessellation::OutputPrimitives::TrianglesCw
                ? MTL::WindingClockwise
                : MTL::WindingCounterClockwise);
        pipeline_descriptor->setTessellationFactorStepFunction(
            MTL::TessellationFactorStepFunctionPerPatch);
        pipeline_descriptor->setTessellationFactorFormat(
            MTL::TessellationFactorFormatHalf);
        pipeline_descriptor->setTessellationControlPointIndexType(
            MTL::TessellationControlPointIndexTypeNone);
        pipeline_descriptor->setMaxTessellationFactor(64);
        if (capture_pipeline == nullptr || tess_control_pipeline == nullptr ||
            tess_factor_pipeline == nullptr) {
            for (auto** state : {&capture_pipeline, &tess_control_pipeline,
                                 &tess_factor_pipeline}) {
                if (*state != nullptr)
                    (*state)->release();
                *state = nullptr;
            }
            pipeline_descriptor->release();
            return;
        }
    }

    // Pipeline
    std::optional<ScopedPerfTimer> compile_timer;
    if (PerfStats::isEnabled()) {
        compile_timer.emplace(PerfStats::instance().pipeline_compile_ns);
        PerfStats::instance().pipeline_compiles++;
    }
    NS::Error* error = nullptr;
    const auto* drawing_shader = geometry       ? passthrough_shader
                                 : tessellation ? evaluation_shader
                                                : vertex_shader;
    const PipelineSources sources{
        drawing_shader->getLibraryKey(),
        fragment_shader != nullptr ? fragment_shader->getLibraryKey() : ""};
    // A fragment shader without a stored source cannot be rebuilt
    const bool rebuildable =
        fragment_shader == nullptr || !sources.fragment.empty();
    pipeline = binary_archive_cache.createPipeline(
        pipeline_descriptor, &error,
        fmt::format("{}:{}", drawing_shader->getCacheKey(),
                    fragment_shader != nullptr ? fragment_shader->getCacheKey()
                                               : "no-fragment"),
        rebuildable ? &sources : nullptr);
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
    if (capture_pipeline != nullptr)
        capture_pipeline->release();
    if (geometry_pipeline != nullptr)
        geometry_pipeline->release();
    if (tess_control_pipeline != nullptr)
        tess_control_pipeline->release();
    if (tess_factor_pipeline != nullptr)
        tess_factor_pipeline->release();
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
