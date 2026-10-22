#include "core/hw/tegra_x1/gpu/renderer/metal/frame_presenter.hpp"

#include "common/config.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/mtl4_context.hpp"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wundef"
#include <MetalFX/MTL4FXFrameInterpolator.hpp>
#include <MetalFX/MTLFXFrameInterpolator.hpp>
#pragma GCC diagnostic pop

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

namespace {

constexpr auto SHADER_SOURCE = R"(
#include <metal_stdlib>
using namespace metal;

constexpr sampler linear_clamp(coord::normalized, filter::linear,
                               mip_filter::nearest, address::clamp_to_edge);

kernel void fg_luma(texture2d<float> color [[texture(0)]],
                    texture2d<float, access::write> luma [[texture(1)]],
                    uint2 gid [[thread_position_in_grid]]) {
    const uint2 size = uint2(luma.get_width(), luma.get_height());
    if (any(gid >= size))
        return;
    const float2 uv = (float2(gid) + 0.5) / float2(size);
    const float3 c = color.sample(linear_clamp, uv, level(0.0)).rgb;
    luma.write(float4(dot(c, float3(0.299, 0.587, 0.114))), gid);
}

struct SearchParams {
    float lod;
    int radius;
    uint has_coarse;
    float smoothness;
};

// Mean absolute difference of a 5x5 patch
static float patch_cost(thread const float* patch, texture2d<float> frame,
                        float2 uv, float2 texel, float lod) {
    float sum = 0.0;
    for (int y = -2; y <= 2; y++) {
        for (int x = -2; x <= 2; x++) {
            const float value = frame.sample(linear_clamp,
                                             uv + float2(x, y) * texel,
                                             level(lod)).r;
            sum += abs(patch[(y + 2) * 5 + x + 2] - value);
        }
    }
    return sum * (1.0 / 25.0);
}

// Block matching at one level of the luma pyramid, around the motion found at
// the coarser level. Writes where each texel of `from` is in `to`.
kernel void fg_search(texture2d<float> from [[texture(0)]],
                      texture2d<float> to [[texture(1)]],
                      texture2d<float> coarse [[texture(2)]],
                      texture2d<float, access::write> flow [[texture(3)]],
                      constant SearchParams& params [[buffer(0)]],
                      uint2 gid [[thread_position_in_grid]]) {
    const uint2 size = uint2(flow.get_width(), flow.get_height());
    if (any(gid >= size))
        return;
    const float2 texel = 1.0 / float2(size);
    const float2 uv = (float2(gid) + 0.5) * texel;

    float patch[25];
    for (int y = -2; y <= 2; y++) {
        for (int x = -2; x <= 2; x++) {
            patch[(y + 2) * 5 + x + 2] =
                from.sample(linear_clamp, uv + float2(x, y) * texel,
                            level(params.lod)).r;
        }
    }

    const float2 predicted =
        params.has_coarse != 0
            ? coarse.sample(linear_clamp, uv, level(0.0)).xy
            : float2(0.0);
    // Static content (HUDs, still scenes) is common: no motion wins ties
    float2 best = float2(0.0);
    float best_cost = patch_cost(patch, to, uv, texel, params.lod) * 0.95;
    for (int dy = -params.radius; dy <= params.radius; dy++) {
        for (int dx = -params.radius; dx <= params.radius; dx++) {
            const float2 motion = predicted + float2(dx, dy) * texel;
            const float cost =
                patch_cost(patch, to, uv + motion, texel, params.lod) +
                params.smoothness * length(float2(dx, dy));
            if (cost < best_cost) {
                best_cost = cost;
                best = motion;
            }
        }
    }
    flow.write(float4(best, best_cost, 0.0), gid);
}

// Vector median of the 3x3 neighbourhood, removes isolated wrong matches
kernel void fg_filter(texture2d<float> src [[texture(0)]],
                      texture2d<float, access::write> dst [[texture(1)]],
                      uint2 gid [[thread_position_in_grid]]) {
    const int2 size = int2(src.get_width(), src.get_height());
    if (any(int2(gid) >= size))
        return;
    float4 neighbours[9];
    for (int i = 0; i < 9; i++) {
        const int2 pos = clamp(int2(gid) + int2(i % 3 - 1, i / 3 - 1),
                               int2(0), size - 1);
        neighbours[i] = src.read(uint2(pos));
    }
    int best = 4;
    float best_sum = INFINITY;
    for (int i = 0; i < 9; i++) {
        float sum = 0.0;
        for (int j = 0; j < 9; j++)
            sum += distance(neighbours[i].xy, neighbours[j].xy);
        if (sum < best_sum) {
            best_sum = sum;
            best = i;
        }
    }
    dst.write(float4(neighbours[best].xy, neighbours[4].z, 0.0), gid);
}

struct InterpolateParams {
    float t;
};

// Each frame is warped to time t along its motion, the warps are blended by
// how close and how well matched their frame is. Where the two motions
// disagree (occlusions), the better match is used alone.
kernel void fg_interpolate(texture2d<float> prev [[texture(0)]],
                           texture2d<float> cur [[texture(1)]],
                           texture2d<float> forward [[texture(2)]],
                           texture2d<float> backward [[texture(3)]],
                           texture2d<float, access::write> out [[texture(4)]],
                           constant InterpolateParams& params [[buffer(0)]],
                           uint2 gid [[thread_position_in_grid]]) {
    const uint2 size = uint2(out.get_width(), out.get_height());
    if (any(gid >= size))
        return;
    const float2 uv = (float2(gid) + 0.5) / float2(size);
    const float t = params.t;

    const float4 f = forward.sample(linear_clamp, uv, level(0.0));
    const float4 b = backward.sample(linear_clamp, uv, level(0.0));
    const float3 from_prev =
        prev.sample(linear_clamp, uv - t * f.xy, level(0.0)).rgb;
    const float3 from_cur =
        cur.sample(linear_clamp, uv - (1.0 - t) * b.xy, level(0.0)).rgb;

    float w_prev = (1.0 - t) * exp(-f.z * 20.0);
    float w_cur = t * exp(-b.z * 20.0);
    const float disagreement = length((f.xy + b.xy) * float2(size));
    const float exclusive = saturate((disagreement - 2.0) / 6.0);
    if (w_prev < w_cur)
        w_prev *= 1.0 - exclusive;
    else
        w_cur *= 1.0 - exclusive;

    float3 color;
    const float total = w_prev + w_cur;
    if (total < 1e-4)
        color = t < 0.5 ? prev.sample(linear_clamp, uv, level(0.0)).rgb
                        : cur.sample(linear_clamp, uv, level(0.0)).rgb;
    else
        color = (from_prev * w_prev + from_cur * w_cur) / total;
    out.write(float4(color, 1.0), gid);
}

// Full size motion for MetalFX, in normalized coordinates
kernel void fg_motion(texture2d<float> backward [[texture(0)]],
                      texture2d<float, access::write> motion [[texture(1)]],
                      constant float& sign [[buffer(0)]],
                      uint2 gid [[thread_position_in_grid]]) {
    const uint2 size = uint2(motion.get_width(), motion.get_height());
    if (any(gid >= size))
        return;
    const float2 uv = (float2(gid) + 0.5) / float2(size);
    const float2 value = backward.sample(linear_clamp, uv, level(0.0)).xy;
    motion.write(float4(value * sign, 0.0, 0.0), gid);
}

struct PresentOut {
    float4 position [[position]];
};

constant float2 present_vertices[3] = {
    float2(-1.0, -3.0),
    float2(-1.0,  1.0),
    float2( 3.0,  1.0)
};

vertex PresentOut vertex_present(ushort vid [[vertex_id]]) {
    PresentOut out;
    out.position = float4(present_vertices[vid], 0.0, 1.0);
    return out;
}

struct PresentParams {
    float sharpness;
};

// The source has the size of the drawable. Sharpening follows AMD FidelityFX
// CAS: neighbours are subtracted less where the contrast is already high.
fragment float4 fragment_present(PresentOut in [[stage_in]],
                                 texture2d<float> src [[texture(0)]],
                                 constant PresentParams& params [[buffer(0)]]) {
    const int2 size = int2(src.get_width(), src.get_height());
    const int2 pos = clamp(int2(in.position.xy), int2(0), size - 1);
    float3 c = src.read(uint2(pos)).rgb;
    if (params.sharpness > 0.0) {
        const float3 up = src.read(uint2(clamp(pos + int2(0, -1), int2(0), size - 1))).rgb;
        const float3 down = src.read(uint2(clamp(pos + int2(0, 1), int2(0), size - 1))).rgb;
        const float3 left = src.read(uint2(clamp(pos + int2(-1, 0), int2(0), size - 1))).rgb;
        const float3 right = src.read(uint2(clamp(pos + int2(1, 0), int2(0), size - 1))).rgb;
        const float3 lo = min(c, min(min(up, down), min(left, right)));
        const float3 hi = max(c, max(max(up, down), max(left, right)));
        const float3 amount = sqrt(saturate(min(lo, 1.0 - hi) / max(hi, float3(1e-4))));
        const float3 weight = amount * (-1.0 / mix(8.0, 5.0, params.sharpness));
        c = saturate((c + (up + down + left + right) * weight) /
                     (1.0 + 4.0 * weight));
    }
    return float4(c, 1.0);
}
)";

struct SearchParams {
    f32 lod;
    i32 radius;
    u32 has_coarse;
    f32 smoothness;
};

constexpr usize THREADGROUP_SIZE = 8;

void dispatch2D(ComputeEncoder* encoder, u32 width, u32 height) {
    encoder->dispatchThreads(MTL::Size(width, height, 1),
                             MTL::Size(THREADGROUP_SIZE, THREADGROUP_SIZE, 1));
}

u32 mipCount(u32 width, u32 height) {
    return static_cast<u32>(std::floor(std::log2(std::max(width, height)))) +
           1;
}

} // namespace

FramePresenter::FramePresenter(MTL::Device* device_,
                               Mtl4Context* mtl4_context_)
    : device{device_->retain()}, mtl4_context{mtl4_context_} {
    present_pass = MTL::RenderPassDescriptor::alloc()->init();
    auto color_attachment = present_pass->colorAttachments()->object(0);
    color_attachment->setLoadAction(MTL::LoadActionDontCare);
    color_attachment->setStoreAction(MTL::StoreActionStore);
    pipelines_valid = createPipelines();
    if (!pipelines_valid)
        LOG_ERROR(MetalRenderer, "Frame presenter shaders failed to compile, "
                                 "frame generation and sharpening are off");
}

FramePresenter::~FramePresenter() {
    releaseResources();
    for (auto* pipeline : {luma_pipeline, search_pipeline, filter_pipeline,
                           interpolate_pipeline, motion_pipeline})
        if (pipeline)
            pipeline->release();
    if (present_pipeline)
        present_pipeline->release();
    if (present_vertex)
        present_vertex->release();
    if (present_fragment)
        present_fragment->release();
    present_pass->release();
    device->release();
}

bool FramePresenter::isEnabled() {
    return CONFIG_INSTANCE.getFrameGeneration() != FrameGeneration::Off ||
           CONFIG_INSTANCE.getSharpening() != 0;
}

bool FramePresenter::createPipelines() {
    auto library = createLibraryFromSource(device, SHADER_SOURCE);
    if (!library)
        return false;

    bool valid = true;
    const auto compute = [&](const char* name) -> MTL::ComputePipelineState* {
        auto function = library->newFunction(toNsString(std::string(name)));
        if (!function) {
            valid = false;
            return nullptr;
        }
        NS::Error* error = nullptr;
        auto pipeline = device->newComputePipelineState(function, &error);
        function->release();
        if (!pipeline) {
            LOG_ERROR(MetalRenderer, "Failed to create {} pipeline: {}", name,
                      error ? error->localizedDescription()->utf8String()
                            : "unknown error");
            valid = false;
        }
        return pipeline;
    };
    luma_pipeline = compute("fg_luma");
    search_pipeline = compute("fg_search");
    filter_pipeline = compute("fg_filter");
    interpolate_pipeline = compute("fg_interpolate");
    motion_pipeline = compute("fg_motion");
    present_vertex = library->newFunction(toNsString("vertex_present"));
    present_fragment = library->newFunction(toNsString("fragment_present"));
    library->release();
    return valid && present_vertex && present_fragment;
}

MTL::Texture* FramePresenter::newTexture(u32 texture_width,
                                         u32 texture_height,
                                         MTL::PixelFormat texture_format,
                                         MTL::TextureUsage usage,
                                         u32 mip_count) {
    NS_STACK_SCOPED auto descriptor = MTL::TextureDescriptor::alloc()->init();
    descriptor->setTextureType(MTL::TextureType2D);
    descriptor->setPixelFormat(texture_format);
    descriptor->setWidth(std::max(texture_width, 1u));
    descriptor->setHeight(std::max(texture_height, 1u));
    descriptor->setMipmapLevelCount(mip_count);
    descriptor->setStorageMode(MTL::StorageModePrivate);
    descriptor->setUsage(usage);
    return device->newTexture(descriptor);
}

void FramePresenter::releaseResources() {
    const auto release = [](auto*& object) {
        if (object)
            object->release();
        object = nullptr;
    };
    for (auto& slot : slots) {
        release(slot.color);
        release(slot.luma);
    }
    for (auto& flow : flows) {
        for (auto*& level : flow.levels)
            release(level);
        flow.levels.clear();
        release(flow.filtered);
    }
    release(zero_flow);
    release(generated);
    release(metalfx_mtl3);
    release(metalfx_mtl4);
    metalfx = nullptr;
    release(metalfx_motion);
    release(metalfx_depth);
    release(metalfx_output);
    metalfx_depth_cleared = false;
    width = 0;
    height = 0;
    resources_valid = false;
    format = MTL::PixelFormatInvalid;
    history = 0;
}

bool FramePresenter::resize(u32 width_, u32 height_,
                            MTL::PixelFormat format_) {
    if (width_ == width && height_ == height && format_ == format)
        return resources_valid;
    releaseResources();
    width = width_;
    height = height_;
    format = format_;

    // MetalFX first, the frames it reads need its usages
    MTL::TextureUsage color_usage =
        MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead;
    const bool metalfx_supported =
        mtl4_context != nullptr
            ? mtl4_context->getCompiler() != nullptr &&
                  MTLFX::FrameInterpolatorDescriptor::supportsMetal4FX(device)
            : MTLFX::FrameInterpolatorDescriptor::supportsDevice(device);
    if (metalfx_supported) {
        NS_STACK_SCOPED auto descriptor =
            MTLFX::FrameInterpolatorDescriptor::alloc()->init();
        descriptor->setInputWidth(width);
        descriptor->setInputHeight(height);
        descriptor->setOutputWidth(width);
        descriptor->setOutputHeight(height);
        descriptor->setColorTextureFormat(format);
        descriptor->setOutputTextureFormat(format);
        descriptor->setDepthTextureFormat(MTL::PixelFormatDepth32Float);
        descriptor->setMotionTextureFormat(MTL::PixelFormatRG16Float);
        if (mtl4_context != nullptr) {
            metalfx_mtl4 = descriptor->newFrameInterpolator(
                device, mtl4_context->getCompiler());
            metalfx = metalfx_mtl4;
        } else {
            metalfx_mtl3 = descriptor->newFrameInterpolator(device);
            metalfx = metalfx_mtl3;
        }
        if (metalfx) {
            color_usage |= metalfx->colorTextureUsage();
            metalfx_motion = newTexture(
                width, height, MTL::PixelFormatRG16Float,
                metalfx->motionTextureUsage() | MTL::TextureUsageShaderWrite);
            metalfx_depth = newTexture(width, height,
                                       MTL::PixelFormatDepth32Float,
                                       metalfx->depthTextureUsage() |
                                           MTL::TextureUsageRenderTarget);
            metalfx_output =
                newTexture(width, height, format,
                           metalfx->outputTextureUsage() |
                               MTL::TextureUsageShaderRead);
        } else {
            LOG_WARN(MetalRenderer,
                     "MetalFX cannot interpolate {}x{} frames ({})", width,
                     height, static_cast<u64>(format));
        }
    }

    const u32 luma_width = std::max(width / 2, 1u);
    const u32 luma_height = std::max(height / 2, 1u);
    const u32 luma_mips = mipCount(luma_width, luma_height);
    for (auto& slot : slots) {
        slot.color = newTexture(width, height, format, color_usage);
        slot.luma = newTexture(
            luma_width, luma_height, MTL::PixelFormatR16Float,
            MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite |
                MTL::TextureUsageRenderTarget,
            luma_mips);
    }

    // Levels from 1/8 of the frame, while at least 8 texels across
    flow_level_count = 0;
    for (u32 i = 0; i < FLOW_LEVEL_COUNT; i++) {
        const u32 level = FINEST_FLOW_LEVEL + i;
        if (level >= luma_mips ||
            std::min(luma_width >> level, luma_height >> level) < 8)
            break;
        flow_level_count++;
    }
    const auto flow_usage =
        MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite;
    for (auto& flow : flows) {
        for (u32 i = 0; i < flow_level_count; i++) {
            const u32 level = FINEST_FLOW_LEVEL + i;
            flow.levels.push_back(newTexture(luma_width >> level,
                                             luma_height >> level,
                                             MTL::PixelFormatRGBA32Float,
                                             flow_usage));
        }
        flow.filtered = newTexture(luma_width >> FINEST_FLOW_LEVEL,
                                   luma_height >> FINEST_FLOW_LEVEL,
                                   MTL::PixelFormatRGBA32Float, flow_usage);
    }
    zero_flow = newTexture(1, 1, MTL::PixelFormatRGBA32Float, flow_usage);
    generated = newTexture(width, height, MTL::PixelFormatRGBA8Unorm,
                           MTL::TextureUsageShaderRead |
                               MTL::TextureUsageShaderWrite);

    LOG_INFO(MetalRenderer,
             "Frame presenter {}x{}: {} motion levels, MetalFX frame "
             "interpolation {}",
             width, height, flow_level_count,
             metalfx ? "available" : "unavailable");
    resources_valid = slots[0].color != nullptr && generated != nullptr &&
                      flow_level_count != 0;
    return resources_valid;
}

MTL::Texture* FramePresenter::getCanvas(u32 canvas_width, u32 canvas_height,
                                        MTL::PixelFormat canvas_format) {
    if (!pipelines_valid || canvas_width < MIN_SIZE ||
        canvas_height < MIN_SIZE)
        return nullptr;
    if (!resize(canvas_width, canvas_height, canvas_format))
        return nullptr;
    return slots[canvas_slot].color;
}

void FramePresenter::present(CommandBuffer* command_buffer,
                             MTL::Texture* drawable, bool new_frame) {
    const auto mode = CONFIG_INSTANCE.getFrameGeneration();
    MTL::Texture* source = slots[canvas_slot].color;
    if (mode != FrameGeneration::Off) {
        if (new_frame) {
            pushFrame(command_buffer, mode == FrameGeneration::MetalFx);
        } else if (history != 0) {
            refreshes_since_frame++;
            // A long gap (loading, pause): interpolating from the stale frame
            // would show it again
            if (refreshes_since_frame > MAX_REFRESHES_PER_FRAME)
                history = std::min(history, 1u);
        }
        if (history != 0)
            source = getFrameForRefresh(command_buffer, mode);
    } else {
        history = 0;
    }
    encodePresent(command_buffer, source, drawable);
}

void FramePresenter::pushFrame(CommandBuffer* command_buffer,
                               bool metalfx_enabled) {
    const auto now = std::chrono::steady_clock::now();
    if (history != 0 && refreshes_since_frame > MAX_REFRESHES_PER_FRAME)
        history = 0;
    if (history != 0) {
        const auto refreshes = static_cast<f32>(refreshes_since_frame);
        refreshes_per_frame =
            history >= 2
                ? refreshes_per_frame + (refreshes - refreshes_per_frame) * 0.25f
                : refreshes;
        frame_delta_time =
            std::chrono::duration<f32>(now - last_frame_time).count();
    }
    last_frame_time = now;

    // The canvas becomes the current frame, the previous one the next canvas
    const u32 free_slot = previous_slot;
    previous_slot = current_slot;
    current_slot = canvas_slot;
    canvas_slot = free_slot;
    history = std::min(history + 1, 2u);
    refreshes_since_frame = 1;
    frame_index++;

    encodeLuma(command_buffer, slots[current_slot]);
    if (history >= 2)
        encodeFlow(command_buffer, metalfx_enabled && metalfx != nullptr);
}

MTL::Texture* FramePresenter::getFrameForRefresh(CommandBuffer* command_buffer,
                                                 FrameGeneration mode) {
    auto* current = slots[current_slot].color;
    if (history < 2)
        return current;
    const auto refreshes = std::clamp(
        static_cast<u32>(std::lround(refreshes_per_frame)), 1u,
        MAX_REFRESHES_PER_FRAME);
    if (refreshes_since_frame >= refreshes)
        return current;

    const f32 t =
        static_cast<f32>(refreshes_since_frame) / static_cast<f32>(refreshes);
    if (mode == FrameGeneration::MetalFx && metalfx &&
        std::abs(t - 0.5f) < 1e-3f && encodeMetalFx(command_buffer))
        return metalfx_output;
    encodeInterpolation(command_buffer, t);
    return generated;
}

void FramePresenter::encodeLuma(CommandBuffer* command_buffer,
                                const Slot& slot) {
    auto encoder = command_buffer->getComputeCommandEncoder();
    encoder->setComputePipelineState(luma_pipeline);
    encoder->setTexture(slot.color, 0);
    encoder->setTexture(slot.luma, 1);
    dispatch2D(encoder, static_cast<u32>(slot.luma->width()),
               static_cast<u32>(slot.luma->height()));
    command_buffer->getBlitCommandEncoder()->generateMipmaps(slot.luma);
}

void FramePresenter::encodeFlow(CommandBuffer* command_buffer,
                                bool metalfx_enabled) {
    auto* previous_luma = slots[previous_slot].luma;
    auto* current_luma = slots[current_slot].luma;
    auto encoder = command_buffer->getComputeCommandEncoder();
    for (u32 direction = 0; direction < 2; direction++) {
        auto& flow = flows[direction];
        auto* from = direction == 0 ? previous_luma : current_luma;
        auto* to = direction == 0 ? current_luma : previous_luma;

        // Coarse to fine: the coarsest level searches widest
        encoder->setComputePipelineState(search_pipeline);
        encoder->setTexture(from, 0);
        encoder->setTexture(to, 1);
        for (u32 i = flow_level_count; i-- > 0;) {
            const bool coarsest = i + 1 == flow_level_count;
            const SearchParams params{
                .lod = static_cast<f32>(FINEST_FLOW_LEVEL + i),
                .radius = coarsest ? 4 : 2,
                .has_coarse = coarsest ? 0u : 1u,
                .smoothness = 0.004f,
            };
            encoder->setTexture(coarsest ? zero_flow : flow.levels[i + 1], 2);
            encoder->setTexture(flow.levels[i], 3);
            encoder->setBytes(&params, sizeof(params), 0);
            dispatch2D(encoder, static_cast<u32>(flow.levels[i]->width()),
                       static_cast<u32>(flow.levels[i]->height()));
        }

        encoder->setComputePipelineState(filter_pipeline);
        encoder->setTexture(flow.levels[0], 0);
        encoder->setTexture(flow.filtered, 1);
        dispatch2D(encoder, static_cast<u32>(flow.filtered->width()),
                   static_cast<u32>(flow.filtered->height()));
    }

    if (metalfx_enabled) {
        // DEBUG: for checking the motion direction MetalFX expects
        static const f32 sign =
            std::getenv("HYDRA_METALFX_FLIP_MOTION") ? -1.0f : 1.0f;
        encoder->setComputePipelineState(motion_pipeline);
        encoder->setTexture(flows[1].filtered, 0);
        encoder->setTexture(metalfx_motion, 1);
        encoder->setBytes(&sign, sizeof(sign), 0);
        dispatch2D(encoder, width, height);
    }
}

void FramePresenter::encodeInterpolation(CommandBuffer* command_buffer,
                                         f32 t) {
    auto encoder = command_buffer->getComputeCommandEncoder();
    encoder->setComputePipelineState(interpolate_pipeline);
    encoder->setTexture(slots[previous_slot].color, 0);
    encoder->setTexture(slots[current_slot].color, 1);
    encoder->setTexture(flows[0].filtered, 2);
    encoder->setTexture(flows[1].filtered, 3);
    encoder->setTexture(generated, 4);
    encoder->setBytes(&t, sizeof(t), 0);
    dispatch2D(encoder, width, height);
}

bool FramePresenter::encodeMetalFx(CommandBuffer* command_buffer) {
    // Games give no depth: a flat scene, the motion carries everything
    if (!metalfx_depth_cleared) {
        NS_STACK_SCOPED auto pass = MTL::RenderPassDescriptor::alloc()->init();
        auto depth_attachment = pass->depthAttachment();
        depth_attachment->setTexture(metalfx_depth);
        depth_attachment->setLoadAction(MTL::LoadActionClear);
        depth_attachment->setStoreAction(MTL::StoreActionStore);
        depth_attachment->setClearDepth(0.5);
        command_buffer->getRenderCommandEncoder(pass);
        command_buffer->endEncoding();
        metalfx_depth_cleared = true;
    }

    // MetalFX encodes its own passes
    auto* fence = command_buffer->beginExternalEncoding();

    metalfx->setColorTexture(slots[current_slot].color);
    metalfx->setPrevColorTexture(slots[previous_slot].color);
    metalfx->setDepthTexture(metalfx_depth);
    metalfx->setMotionTexture(metalfx_motion);
    metalfx->setMotionVectorScaleX(static_cast<f32>(width));
    metalfx->setMotionVectorScaleY(static_cast<f32>(height));
    metalfx->setDeltaTime(frame_delta_time);
    metalfx->setNearPlane(0.1f);
    metalfx->setFarPlane(1000.0f);
    metalfx->setFieldOfView(60.0f);
    metalfx->setAspectRatio(static_cast<f32>(width) /
                            static_cast<f32>(height));
    metalfx->setJitterOffsetX(0.0f);
    metalfx->setJitterOffsetY(0.0f);
    metalfx->setDepthReversed(false);
    // Its history only follows consecutive frames
    metalfx->setShouldResetHistory(metalfx_last_frame + 1 != frame_index);
    metalfx->setOutputTexture(metalfx_output);
    if (metalfx_mtl4 != nullptr) {
        // Metal 4 does not track what MetalFX reads and writes
        for (auto* texture :
             {slots[current_slot].color, slots[previous_slot].color,
              metalfx_depth, metalfx_motion, metalfx_output})
            command_buffer->track(texture);
        metalfx->setFence(fence);
        metalfx_mtl4->encodeToCommandBuffer(
            command_buffer->getMtl4CommandBuffer());
    } else {
        metalfx_mtl3->encodeToCommandBuffer(command_buffer->getCommandBuffer());
    }
    metalfx_last_frame = frame_index;
    return true;
}

void FramePresenter::encodePresent(CommandBuffer* command_buffer,
                                   MTL::Texture* source,
                                   MTL::Texture* drawable) {
    if (present_format != drawable->pixelFormat()) {
        if (present_pipeline)
            present_pipeline->release();
        present_pipeline = nullptr;
        present_format = drawable->pixelFormat();
        NS_STACK_SCOPED auto descriptor =
            MTL::RenderPipelineDescriptor::alloc()->init();
        descriptor->setVertexFunction(present_vertex);
        descriptor->setFragmentFunction(present_fragment);
        descriptor->colorAttachments()->object(0)->setPixelFormat(
            present_format);
        NS::Error* error = nullptr;
        present_pipeline = device->newRenderPipelineState(descriptor, &error);
        if (!present_pipeline)
            LOG_ERROR(MetalRenderer, "Failed to create present pipeline: {}",
                      error ? error->localizedDescription()->utf8String()
                            : "unknown error");
    }
    if (!present_pipeline)
        return;

    present_pass->colorAttachments()->object(0)->setTexture(drawable);
    auto encoder = command_buffer->getRenderCommandEncoder(present_pass);
    encoder->setRenderPipelineState(present_pipeline);
    encoder->setViewport(MTL::Viewport{
        .originX = 0.0,
        .originY = 0.0,
        .width = static_cast<f64>(drawable->width()),
        .height = static_cast<f64>(drawable->height()),
        .znear = 0.0,
        .zfar = 1.0});
    const f32 sharpness =
        static_cast<f32>(std::min(CONFIG_INSTANCE.getSharpening(), 100u)) /
        100.0f;
    encoder->setFragmentBytes(&sharpness, sizeof(sharpness), 0);
    encoder->setFragmentTexture(source, 0);
    encoder->drawPrimitives(MTL::PrimitiveTypeTriangle,
                            static_cast<NS::UInteger>(0),
                            static_cast<NS::UInteger>(3));
    command_buffer->endEncoding();
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
