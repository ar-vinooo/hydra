#include "core/hw/tegra_x1/gpu/gpu.hpp"

#include "core/hw/tegra_x1/cpu/mmu.hpp"
#include "core/hw/tegra_x1/gpu/const.hpp"

#ifdef ZTD_PLATFORM_APPLE
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"
#endif
#include "core/hw/tegra_x1/gpu/renderer/null/renderer.hpp"

namespace hydra::hw::tegra_x1::gpu {

namespace {

renderer::IRenderer* createRenderer() {
    const auto renderer_type = CONFIG_INSTANCE.getGpuRenderer();
    switch (renderer_type) {
    case GpuRenderer::Metal:
#ifdef ZTD_PLATFORM_APPLE
        return new renderer::metal::Renderer();
#else
        LOG_FATAL(Gpu, "Metal renderer not supported");
#endif
    case GpuRenderer::Null:
        return new renderer::null::Renderer();
    default:
        LOG_FATAL(Gpu, "Unknown Gpu renderer {}", renderer_type);
    }
}

struct SetObjectArg {
    u32 class_id : 16;
    u32 engine_id : 5;
};

} // namespace

GpuChannelContext::GpuChannelContext(Gpu& gpu)
    : three_d(gpu), compute(gpu), inline_engine(gpu), two_d(gpu), copy(gpu) {}

Gpu::Gpu() noexcept
    : pfifo(*this), default_channel(*this), renderer{createRenderer()} {}

void Gpu::subchannelMethod(u32 subchannel, u32 method, u32 arg) {
    auto& context = channelContext();
    if (method == 0x0) { // SetEngine
        ASSERT_DEBUG(subchannel < SUBCHANNEL_COUNT, Gpu,
                     "Invalid subchannel {}", subchannel);

        const auto set_object_arg = std::bit_cast<SetObjectArg>(arg);
        // TODO: what is engine ID?
        engines::EngineBase* engine = nullptr;
        switch (set_object_arg.class_id) {
        case 0xb197:
            engine = &context.three_d;
            break;
        case 0xb1c0:
            engine = &context.compute;
            break;
        case 0xa140:
            engine = &context.inline_engine;
            break;
        case 0x902d:
            engine = &context.two_d;
            break;
        case 0xb0b5:
            engine = &context.copy;
            break;
        case 0xb06f:
            // TODO: implement
            LOG_NOT_IMPLEMENTED(Gpu, "GPFIFO engine");
            break;
        default:
            // Keep the current binding: dropping the engine would discard
            // every later method sent to this subchannel
            LOG_ERROR(Gpu,
                      "Unknown engine class ID 0x{:08x} for subchannel {}, "
                      "keeping the current engine",
                      set_object_arg.class_id, subchannel);
            if (tls_crnt_decode_state)
                tls_crnt_decode_state->fail(25);
            return;
        }

        context.subchannels[subchannel] = engine;

        return;
    }

    // Methods below 0x40 belong to the channel, not to the bound engine
    if (method < 0x40) {
        pullerMethod(method, arg);
        return;
    }

    const auto engine = getEngineAtSubchannel(subchannel);
    if (!engine) {
        ONCE(LOG_WARN(Gpu, "No engine bound to subchannel {} (method: {:#x})",
                      subchannel, method));
        return;
    }

    (*engine)->method(method, arg);
}

bool Gpu::reserveFence(Fence& out_fence, PfifoDecodeState* channel, u32 increments) {
    // On unless HYDRA_GPU_FENCES=0: PFIFO runs the submissions later on its own
    // thread, so without fences the guest believes its work is done at once and
    // reuses push buffers, uniform and vertex rings and textures that the host
    // has not read yet
    static const bool enabled = [] {
        const char* env = std::getenv("HYDRA_GPU_FENCES");
        return env == nullptr || std::string_view(env) != "0";
    }();
    if (!enabled)
        return false;
    std::scoped_lock lock(fence_mutex);
    auto& channel_id = channel ? channel->fence_id : fence_syncpoint;
    auto& target = channel ? channel->fence_target : fence_target;
    if (channel_id == 0) {
        const auto id = video_syncpoints->allocate();
        if (!id)
            return false;
        channel_id = *id;
        fence_syncpoints.insert(*id);
    }
    target += increments;
    out_fence = {channel_id, target};
    return true;
}

void Gpu::pullerMethod(u32 method, u32 arg) {
    // Channel puller methods. SEMAPHORED fields follow NVIDIA clb06f.h.
    constexpr u32 SEMAPHORE_ADDRESS_HIGH = 0x4;
    constexpr u32 SEMAPHORE_ADDRESS_LOW = 0x5;
    constexpr u32 SEMAPHORE_SEQUENCE = 0x6;
    constexpr u32 SEMAPHORE_TRIGGER = 0x7;
    constexpr u32 SEMAPHORE_ACQUIRE = 0x1a;
    constexpr u32 SEMAPHORE_RELEASE = 0x1b;
    constexpr u32 SYNCPOINT_PAYLOAD = 0x1c;
    constexpr u32 SYNCPOINT_OPERATION = 0x1d;
    constexpr u32 WAIT_FOR_IDLE = 0x1e;
    auto& puller_regs = channelContext().puller_regs;
    puller_regs[method] = arg;

    const auto semaphore_address = [&] {
        return (static_cast<u64>(puller_regs[SEMAPHORE_ADDRESS_HIGH] & 0xff)
                << 32) |
               puller_regs[SEMAPHORE_ADDRESS_LOW];
    };
    // Host pointer of the semaphore if the memory is mapped
    const auto resolve = [&](u64 size) -> u8* {
        uptr base;
        const auto as = tls_crnt_gmmu != nullptr
                            ? tls_crnt_gmmu->findAddr(semaphore_address(), base)
                            : std::nullopt;
        if (!as || as->ptr == 0 ||
            semaphore_address() - base + size > as->size ||
            (as->ptr + semaphore_address() - base) %
                std::atomic_ref<u32>::required_alignment != 0) {
            ONCE(LOG_WARN(Gpu, "Semaphore in unmapped memory {:#x}",
                          semaphore_address()));
            if (tls_crnt_decode_state)
                tls_crnt_decode_state->fail(31);
            return nullptr;
        }
        return reinterpret_cast<u8*>(as->ptr + (semaphore_address() - base));
    };
    const auto invalidate = [&](u8* ptr, u64 size) {
        renderer->invalidateMemory(
            ztd::Range<uptr>::fromSize(reinterpret_cast<uptr>(ptr), size));
    };
    const auto wait_for_gpu = [&] {
        if (tls_crnt_command_buffer == nullptr)
            return true;
        renderer->debugSyncCommandBuffer(tls_crnt_command_buffer);
#ifdef ZTD_PLATFORM_APPLE
        if (CONFIG_INSTANCE.getGpuRenderer() == GpuRenderer::Metal) {
            const bool succeeded = static_cast<renderer::metal::CommandBuffer*>(
                tls_crnt_command_buffer)->lastSubmissionSucceeded();
            if (!succeeded && tls_crnt_decode_state)
                tls_crnt_decode_state->fail(25);
            return succeeded;
        }
#endif
        return true;
    };
    // Submit prior work before polling: it may produce the semaphore itself.
    // A pending acquire blocks later methods until satisfied or shutdown.
    const auto acquire = [&](u32 payload, u32 operation, auto&& satisfied) {
        auto* ptr = resolve(sizeof(u32));
        if (ptr == nullptr || !wait_for_gpu())
            return;
        while (!pfifo.isStopping()) {
            const u32 word = std::atomic_ref<u32>(
                *reinterpret_cast<u32*>(ptr)).load(std::memory_order_acquire);
            if (satisfied(word))
                return;
            if (tls_crnt_decode_state) {
                tls_crnt_decode_state->semaphore_wait = PfifoSemaphoreWait{
                    semaphore_address(), payload, operation};
                return;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    };

    switch (method) {
    case 0x2: // NVB06F_NOP
        break;
    case SEMAPHORE_ADDRESS_HIGH:
    case SEMAPHORE_ADDRESS_LOW:
    case SEMAPHORE_SEQUENCE:
    case SYNCPOINT_PAYLOAD:
        break;
    case WAIT_FOR_IDLE:
        wait_for_gpu();
        break;
    case SEMAPHORE_TRIGGER: {
        const u32 payload = puller_regs[SEMAPHORE_SEQUENCE];
        switch (arg & 0x1f) {
        case 0x1: // Acquire equal
            acquire(payload, 1, [&](u32 word) { return word == payload; });
            break;
        case 0x2: { // RELEASE_SIZE: 0 is 16 bytes, 1 is four bytes.
            const u64 size = (arg & (1u << 24)) != 0 ? 4 : 16;
            if ((arg & (1u << 20)) == 0 && !wait_for_gpu())
                break;
            if (auto* ptr = resolve(size)) {
                if (size == 16) {
                    const u64 timestamp = engines::getGpuTimestamp();
                    std::memcpy(ptr + 8, &timestamp, sizeof(timestamp));
                }
                std::atomic_ref<u32>(*reinterpret_cast<u32*>(ptr))
                    .store(payload, std::memory_order_release);
                invalidate(ptr, size);
            }
            break;
        }
        case 0x4: // Acquire greater or equal
            acquire(payload, 4, [&](u32 word) { return static_cast<i32>(word - payload) >= 0; });
            break;
        case 0x8: // Acquire mask
            acquire(payload, 8, [&](u32 word) { return (word & payload) != 0; });
            break;
        default:
            ONCE(LOG_NOT_IMPLEMENTED(Gpu, "Semaphore operation {:#x}", arg));
            if (tls_crnt_decode_state)
                tls_crnt_decode_state->fail(25);
            break;
        }
        break;
    }
    case SEMAPHORE_ACQUIRE:
        acquire(arg, 1, [&](u32 word) { return word == arg; });
        break;
    case SEMAPHORE_RELEASE:
        if (!wait_for_gpu())
            break;
        if (auto* ptr = resolve(sizeof(u32))) {
            std::atomic_ref<u32>(*reinterpret_cast<u32*>(ptr))
                .store(arg, std::memory_order_release);
            invalidate(ptr, sizeof(u32));
        }
        break;
    case SYNCPOINT_OPERATION: {
        // NVIDIA's channel_sync_gk20a.c and hw_pbdma_gk20a.h define
        // OP[1:0], WAIT_SWITCH[4] and SYNCPT_INDEX[15:8].
        const u32 id = (arg >> 8) & 0xff;
        const u32 operation = arg & 3;
        if (!video_syncpoints->read(id) || operation > 1) {
            if (tls_crnt_decode_state)
                tls_crnt_decode_state->fail(25);
            break;
        }
        if (!wait_for_gpu())
            break;
        if (operation == 1) {
            video_syncpoints->complete(id, 1);
        } else {
            const u32 threshold = puller_regs[SYNCPOINT_PAYLOAD];
            if (tls_crnt_decode_state) {
                const auto value = video_syncpoints->read(id);
                if (!video::Syncpoints::reached(*value, threshold))
                    tls_crnt_decode_state->syncpoint_wait = Fence{id, threshold};
            } else {
                while (!pfifo.isStopping() && !video_syncpoints->waitFor(
                    id, threshold, std::chrono::milliseconds(20))) {}
            }
        }
        break;
    }
    default:
        ONCE(LOG_NOT_IMPLEMENTED(Gpu, "Channel method {:#x}", method));
        break;
    }
}

renderer::ITextureView*
Gpu::getTexture(renderer::ICommandBuffer* command_buffer, cpu::IMmu* mmu,
                const NvGraphicsBuffer& buff) {
    std::scoped_lock texture_cache_lock(renderer->getTextureCache().getMutex());

    const auto& plane = buff.planes[0];

    LOG_DEBUG(Gpu,
              "Map id: {}, width: {}, "
              "height: {}",
              buff.nvmap_id, plane.width, plane.height);

    const bool is_linear =
        (plane.kind == NvKind::Pitch || plane.kind == NvKind::PitchNoSwizzle);

    const auto map = getMap(static_cast<u32>(buff.nvmap_id));
    if (!map || (*map)->addr == 0 || plane.offset >= (*map)->size) {
        ONCE(LOG_WARN(Gpu, "Present buffer refers to a missing nvmap"));
        return nullptr;
    }
    // TODO: pin the nvmap across queued/acquired buffer ownership.
    const auto guest_address = (*map)->addr + plane.offset;
    if (!mmu || guest_address < (*map)->addr)
        return nullptr;

    // TODO: why are there more planes?
    const renderer::TextureDescriptor descriptor(
        mmu->unmapAddr(guest_address),
        MTL::TextureType2D,
        renderer::toTextureFormat(plane.color_format), is_linear, plane.pitch,
        plane.width, plane.height, 1, 1, 1, 0x0, plane.block_height_gobs_log2,
        0x0);

    return renderer->getTextureCache().find(command_buffer, descriptor,
                                            renderer::TextureUsage::Present);
}

} // namespace hydra::hw::tegra_x1::gpu
