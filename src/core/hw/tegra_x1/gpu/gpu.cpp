#include "core/hw/tegra_x1/gpu/gpu.hpp"

#include "core/hw/tegra_x1/cpu/mmu.hpp"
#include "core/hw/tegra_x1/gpu/const.hpp"

#ifdef ZTD_PLATFORM_APPLE
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"
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

Gpu::Gpu() noexcept
    : pfifo(*this), three_d_engine(*this), compute_engine(*this),
      inline_engine(*this), two_d_engine(*this), copy_engine(*this),
      renderer{createRenderer()} {}

void Gpu::subchannelMethod(u32 subchannel, u32 method, u32 arg) {
    if (method == 0x0) { // SetEngine
        ASSERT_DEBUG(subchannel < SUBCHANNEL_COUNT, Gpu,
                     "Invalid subchannel {}", subchannel);

        const auto set_object_arg = std::bit_cast<SetObjectArg>(arg);
        // TODO: what is engine ID?
        engines::EngineBase* engine = nullptr;
        switch (set_object_arg.class_id) {
        case 0xb197:
            engine = &three_d_engine;
            break;
        case 0xb1c0:
            engine = &compute_engine;
            break;
        case 0xa140:
            engine = &inline_engine;
            break;
        case 0x902d:
            engine = &two_d_engine;
            break;
        case 0xb0b5:
            engine = &copy_engine;
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
            return;
        }

        subchannels[subchannel] = engine;

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

bool Gpu::reserveFence(Fence& out_fence) {
    static const bool enabled = std::getenv("HYDRA_GPU_FENCES") != nullptr;
    if (!enabled)
        return false;
    std::scoped_lock lock(fence_mutex);
    if (fence_syncpoint == 0) {
        const auto id = video_syncpoints->allocate();
        if (!id)
            return false;
        fence_syncpoint = *id;
    }
    out_fence = {fence_syncpoint, ++fence_target};
    return true;
}

void Gpu::pullerMethod(u32 method, u32 arg) {
    // Method numbers of the channel's own registers (Eden's puller)
    constexpr u32 SEMAPHORE_ADDRESS_HIGH = 0x4;
    constexpr u32 SEMAPHORE_ADDRESS_LOW = 0x5;
    constexpr u32 SEMAPHORE_SEQUENCE = 0x6;
    constexpr u32 SEMAPHORE_TRIGGER = 0x7;
    constexpr u32 SEMAPHORE_ACQUIRE = 0x1a;
    constexpr u32 SEMAPHORE_RELEASE = 0x1b;
    constexpr u32 SYNCPOINT_PAYLOAD = 0x1c;
    constexpr u32 SYNCPOINT_OPERATION = 0x1d;
    constexpr u32 WAIT_FOR_IDLE = 0x1e;
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
            semaphore_address() - base + size > as->size) {
            ONCE(LOG_WARN(Gpu, "Semaphore in unmapped memory {:#x}",
                          semaphore_address()));
            return nullptr;
        }
        return reinterpret_cast<u8*>(as->ptr + (semaphore_address() - base));
    };
    const auto invalidate = [&](u8* ptr, u64 size) {
        renderer->invalidateMemory(
            ztd::Range<uptr>::fromSize(reinterpret_cast<uptr>(ptr), size));
    };
    // The CPU writes acquired semaphores: wait for it, bounded so a missed
    // release cannot stall the GPU thread for good
    const auto acquire = [&](auto&& satisfied) {
        auto* ptr = resolve(sizeof(u32));
        if (ptr == nullptr)
            return;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(100);
        while (true) {
            u32 word;
            std::memcpy(&word, ptr, sizeof(word));
            if (satisfied(word))
                return;
            if (std::chrono::steady_clock::now() >= deadline) {
                ONCE(LOG_WARN(Gpu, "Semaphore acquire timed out ({:#x})",
                              semaphore_address()));
                return;
            }
            std::this_thread::yield();
        }
    };

    switch (method) {
    case SEMAPHORE_ADDRESS_HIGH:
    case SEMAPHORE_ADDRESS_LOW:
    case SEMAPHORE_SEQUENCE:
    case SYNCPOINT_PAYLOAD:
    case WAIT_FOR_IDLE: // The GPU executes in order
        break;
    case SEMAPHORE_TRIGGER: {
        const u32 payload = puller_regs[SEMAPHORE_SEQUENCE];
        switch (arg & 0xf) {
        case 0x1: // Acquire equal
            acquire([&](u32 word) { return word == payload; });
            break;
        case 0x2: { // Release: payload and timestamp
            if (auto* ptr = resolve(16)) {
                const u64 timestamp = engines::getGpuTimestamp();
                std::memcpy(ptr, &payload, sizeof(payload));
                std::memcpy(ptr + 8, &timestamp, sizeof(timestamp));
                invalidate(ptr, 16);
            }
            break;
        }
        case 0x4: // Acquire greater or equal
            acquire([&](u32 word) { return static_cast<i32>(word - payload) >= 0; });
            break;
        case 0x8: // Acquire mask
            acquire([&](u32 word) { return (word & payload) != 0; });
            break;
        default:
            ONCE(LOG_NOT_IMPLEMENTED(Gpu, "Semaphore operation {:#x}", arg));
            break;
        }
        break;
    }
    case SEMAPHORE_ACQUIRE:
        acquire([&](u32 word) { return word == arg; });
        break;
    case SEMAPHORE_RELEASE:
        if (auto* ptr = resolve(sizeof(u32))) {
            std::memcpy(ptr, &arg, sizeof(arg));
            invalidate(ptr, sizeof(u32));
        }
        break;
    case SYNCPOINT_OPERATION: {
        // Bit 0: 1 increments the syncpoint in bits 8-31 (0 waits for it, the
        // GPU executes in order)
        const u32 id = arg >> 8;
        if ((arg & 1) != 0 && isFenceSyncpoint(id))
            video_syncpoints->complete(id, 1);
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

    // TODO: why are there more planes?
    const renderer::TextureDescriptor descriptor(
        mmu->unmapAddr(getMap(static_cast<u32>(buff.nvmap_id)).value()->addr +
                       plane.offset),
        renderer::TextureType::_2D,
        renderer::toTextureFormat(plane.color_format), is_linear, plane.pitch,
        plane.width, plane.height, 1, 1, 1, 0x0, plane.block_height_gobs_log2,
        0x0);

    return renderer->getTextureCache().find(command_buffer, descriptor,
                                            renderer::TextureUsage::Present);
}

} // namespace hydra::hw::tegra_x1::gpu
