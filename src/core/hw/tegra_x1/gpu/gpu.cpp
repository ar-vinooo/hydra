#include "core/hw/tegra_x1/gpu/gpu.hpp"
#include "core/hw/tegra_x1/gpu/capture/guest_events.hpp"
#include "core/hw/tegra_x1/gpu/visual_diagnostics.hpp"

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

GpuChannelContext::GpuChannelContext(Gpu& gpu)
    : three_d(gpu), compute(gpu), inline_engine(gpu), two_d(gpu), copy(gpu) {}

Gpu::Gpu() noexcept
    : pfifo(*this), default_channel(*this), renderer{createRenderer()} {}

void Gpu::subchannelMethod(u32 subchannel, u32 method, u32 arg) {
    auto& context = channelContext();
    // Constant buffer words of the 3D engine batch their invalidation, any
    // other method may read the buffer
    if (!engines::ThreeD::isConstBufferData(method)) {
        context.three_d.flushConstBufferInvalidation();
    } else if (const auto bound = getEngineAtSubchannel(subchannel);
               !bound || *bound != &context.three_d) {
        context.three_d.flushConstBufferInvalidation();
    }
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
        default:
            // Keep the current binding: dropping the engine would discard
            // every later method sent to this subchannel
            LOG_ERROR(Gpu,
                      "Unknown engine class ID 0x{:08x} for subchannel {}, "
                      "keeping the current engine",
                      set_object_arg.class_id, subchannel);
            if (tls_crnt_decode_state) {
                auto& diagnostics = VisualDiagnostics::instance();
                if (diagnostics.enabled()) {
                    LOG_ERROR(Gpu, "Fault source: GPU header at {:#x}, word {:#010x}, "
                                   "method {:#x}, subchannel {}, argument {:#010x}",
                              diagnostics.commandAddress(), diagnostics.commandHeader(),
                              tls_crnt_decode_state->method, subchannel, arg);
                    diagnostics.commandFault(tls_crnt_decode_state->method, subchannel, arg);
                }
                tls_crnt_decode_state->fail(25);
            }
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
        if (tls_crnt_decode_state)
            tls_crnt_decode_state->fail(25);
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
        // A reused counter keeps its value; targets continue from it
        target = video_syncpoints->read(*id).value_or(0);
        {
            std::scoped_lock registry_lock(fence_registry->mutex);
            fence_registry->ids.insert(*id);
        }
        if (channel)
            channel->release_fence = [syncpoints = video_syncpoints,
                                      registry = fence_registry, id = *id] {
                {
                    std::scoped_lock registry_lock(registry->mutex);
                    registry->ids.erase(id);
                }
                syncpoints->release(id);
            };
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
    if (method != 0x14)
        puller_regs[method] = arg;

    const auto semaphore_address = [&] {
        return (static_cast<u64>(puller_regs[SEMAPHORE_ADDRESS_HIGH] & 0xff)
                << 32) |
               puller_regs[SEMAPHORE_ADDRESS_LOW];
    };
    // Host pointer of the semaphore (plus a byte offset) if the memory is
    // mapped. A 16-byte report can straddle host mappings, so its words are
    // resolved separately. Without `settle`, reports queued earlier are not
    // waited for (a queued release lands after them anyway).
    const auto resolve = [&](u64 size, u64 offset = 0,
                             bool settle = true) -> u8* {
        const u64 address = semaphore_address() + offset;
        uptr base;
        const auto as = tls_crnt_gmmu != nullptr
                            ? tls_crnt_gmmu->findAddr(address, base)
                            : std::nullopt;
        if (!as || as->ptr == 0 || address - base + size > as->size ||
            (as->ptr + address - base) %
                std::atomic_ref<u32>::required_alignment != 0) {
            ONCE(LOG_WARN(Gpu, "Semaphore in unmapped memory {:#x}", address));
            if (tls_crnt_decode_state)
                tls_crnt_decode_state->fail(31);
            return nullptr;
        }
        auto* ptr = reinterpret_cast<u8*>(as->ptr + (address - base));
        if (!settle)
            return ptr;
        // A report queued earlier in the stream may still be on its way here
        renderer::ScopedCheckpointReason reason(
            tls_crnt_command_buffer, CheckpointReason::PullerSemaphore);
        renderer->flushReports(
            tls_crnt_command_buffer,
            ztd::Range<uptr>::fromSize(reinterpret_cast<uptr>(ptr), size));
        return ptr;
    };
    const auto invalidate = [&](u8* ptr, u64 size) {
        renderer->invalidateMemory(
            ztd::Range<uptr>::fromSize(reinterpret_cast<uptr>(ptr), size));
    };
    auto& channel = channelContext();
    const auto wait_for_gpu = [&](CheckpointReason why) {
        if (tls_crnt_command_buffer == nullptr)
            return true;
        renderer::ScopedCheckpointReason reason(tls_crnt_command_buffer, why);
        const bool succeeded = renderer->synchronize(tls_crnt_command_buffer);
        if (succeeded)
            channel.idle_pending = false;
        if (!succeeded && tls_crnt_decode_state)
            tls_crnt_decode_state->fail(25);
        // What the guest is told next (semaphore, reference) includes the
        // reports of the work it waited for
        if (succeeded)
            renderer->drainReports();
        return succeeded;
    };
    // WAIT_FOR_IDLE and MEMBAR order GPU work, which the command buffer
    // already does. Only the CPU depends on them: the DMA engines access guest
    // memory on the CPU, where shader storage writes land at completion, so
    // only those are waited for.
    const auto wait_for_guest_writes = [&] {
        if (tls_crnt_command_buffer == nullptr)
            return;
        renderer::ScopedCheckpointReason reason(
            tls_crnt_command_buffer, CheckpointReason::PullerOther);
        if (!renderer->synchronizeGuestWrites(tls_crnt_command_buffer) &&
            tls_crnt_decode_state)
            tls_crnt_decode_state->fail(25);
    };
    // A release that waits for idle (RELEASE_WFI) lands once the GPU finished
    // the work before it, without the GPU thread waiting: the completion queue
    // orders it after the reports and writes queued earlier. The memory is
    // looked up again then, it may have been remapped. `update` publishes the
    // word, after the timestamp of a 16-byte release.
    const auto queue_release = [&](u64 size, auto update) {
        auto* ptr = resolve(sizeof(u32), 0, false);
        auto* timestamp_ptr =
            ptr != nullptr && size == 16 ? resolve(sizeof(u64), 8, false) : nullptr;
        if (ptr == nullptr || (size == 16 && timestamp_ptr == nullptr))
            return false;
        std::vector<ztd::Range<uptr>> writes{ztd::Range<uptr>::fromSize(
            reinterpret_cast<uptr>(ptr), sizeof(u32))};
        if (timestamp_ptr != nullptr)
            writes.push_back(ztd::Range<uptr>::fromSize(
                reinterpret_cast<uptr>(timestamp_ptr), sizeof(u64)));
        renderer->queueGuestWrite(
            tls_crnt_command_buffer,
            [target = renderer.get(), gmmu = tls_crnt_gmmu,
             owner = tls_crnt_decode_state
                         ? tls_crnt_decode_state->address_space
                         : std::shared_ptr<GMmu>{},
             address = semaphore_address(), size, update](bool succeeded) {
                if (!succeeded || gmmu->gpuWorkCancelled())
                    return;
                auto* word = resolveSemaphoreWord(*gmmu, address);
                if (word == nullptr) {
                    ONCE(LOG_WARN(Gpu,
                                  "Semaphore {:#x} was unmapped before its "
                                  "release",
                                  address));
                    return;
                }
                if (size == 16) {
                    // Little endian halves, a mapping may split them
                    const u64 timestamp = engines::getGpuTimestamp();
                    for (u32 half = 0; half < 2; half++) {
                        auto* timestamp_word =
                            resolveSemaphoreWord(*gmmu, address + 8 + half * 4);
                        if (timestamp_word == nullptr)
                            continue;
                        std::atomic_ref<u32>(*timestamp_word)
                            .store(static_cast<u32>(timestamp >> (half * 32)),
                                   std::memory_order_relaxed);
                        target->queueMemoryInvalidation(
                            ztd::Range<uptr>::fromSize(
                                reinterpret_cast<uptr>(timestamp_word),
                                sizeof(u32)));
                    }
                }
                update(std::atomic_ref<u32>(*word));
                target->queueMemoryInvalidation(ztd::Range<uptr>::fromSize(
                    reinterpret_cast<uptr>(word), sizeof(u32)));
            },
            std::move(writes));
        return true;
    };
    // Submit prior work before polling: it may produce the semaphore itself.
    // A pending acquire blocks later methods until satisfied or shutdown.
    const auto acquire = [&](u32 payload, u32 operation, auto&& satisfied) {
        capture::GuestEvents::instance().event(
            "semaphore_acquire", capture::currentGuestEventOrigin(),
            semaphore_address(), 4, 0, payload, operation);
        auto* ptr = resolve(sizeof(u32));
        if (ptr == nullptr || !wait_for_gpu(CheckpointReason::PullerAcquire))
            return;
        while (!pfifo.isStopping()) {
            const u32 word = std::atomic_ref<u32>(
                *reinterpret_cast<u32*>(ptr)).load(std::memory_order_acquire);
            if (satisfied(word)) {
                capture::GuestEvents::instance().event(
                    "semaphore_acquired", capture::currentGuestEventOrigin(),
                    semaphore_address(), 4, payload, word, operation);
                return;
            }
            if (tls_crnt_decode_state) {
                tls_crnt_decode_state->semaphore_wait = PfifoSemaphoreWait{
                    semaphore_address(), payload, operation};
                return;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    };

    switch (method) {
    case 0x14: // NVB06F_SET_REFERENCE (byte offset 0x50)
        puller_regs[method] = arg;
        channel.idle_pending = true;
        break;
    case 0x2: // NVB06F_NOP
        break;
    case SEMAPHORE_ADDRESS_HIGH:
    case SEMAPHORE_ADDRESS_LOW:
    case SEMAPHORE_SEQUENCE:
    case SYNCPOINT_PAYLOAD:
        break;
    // clb06f.h: interrupts and framebuffer flushes have no host consumer; the
    // caches they would flush are coherent here (GPU writes reach guest memory
    // when the CPU touches them).
    case 0x8: // NON_STALL_INTERRUPT
    case 0x9: // FB_FLUSH
        break;
    case 0xa: // MEM_OP_A and MEM_OP_B only carry operands (removed from the
    case 0xb: // published header); MEM_OP_C holds the TLB operand
    case 0xc:
        break;
    case 0xd: { // MEM_OP_D: OPERATION[31:27]
        switch (arg >> 27) {
        case 0x5: // MEMBAR
            wait_for_gpu(CheckpointReason::PullerMembar);
            break;
        case 0x9:  // MMU_TLB_INVALIDATE: mappings are looked up directly
        case 0xd:  // L2_PEERMEM_INVALIDATE
        case 0xe:  // L2_SYSMEM_INVALIDATE
        case 0xf:  // L2_CLEAN_COMPTAGS
        case 0x10: // L2_FLUSH_DIRTY
            break;
        default:
            ONCE(LOG_NOT_IMPLEMENTED(Gpu, "Memory operation {:#x}", arg >> 27));
            if (tls_crnt_decode_state)
                tls_crnt_decode_state->fail(25);
            break;
        }
        break;
    }
    case WAIT_FOR_IDLE:
        wait_for_gpu(CheckpointReason::PullerWaitForIdle);
        break;
    case SEMAPHORE_TRIGGER: {
        const u32 payload = puller_regs[SEMAPHORE_SEQUENCE];
        switch (arg & 0x1f) {
        case 0x1: // Acquire equal
            acquire(payload, 1, [&](u32 word) { return word == payload; });
            break;
        case 0x2: { // RELEASE_SIZE: 0 is 16 bytes, 1 is four bytes.
            const u64 size = (arg & (1u << 24)) != 0 ? 4 : 16;
            if ((arg & (1u << 20)) == 0 && !wait_for_gpu(CheckpointReason::PullerRelease))
                break;
            if (channel.idle_pending && !wait_for_gpu(CheckpointReason::PullerReference))
                break;
            auto* ptr = resolve(sizeof(u32));
            auto* timestamp_ptr = ptr != nullptr && size == 16 ? resolve(sizeof(u64), 8) : nullptr;
            if (ptr == nullptr || (size == 16 && timestamp_ptr == nullptr))
                break;
            if (timestamp_ptr != nullptr) {
                const u64 timestamp = engines::getGpuTimestamp();
                std::memcpy(timestamp_ptr, &timestamp, sizeof(timestamp));
                invalidate(timestamp_ptr, sizeof(timestamp));
            }
            std::atomic_ref<u32>(*reinterpret_cast<u32*>(ptr))
                .store(payload, std::memory_order_release);
            invalidate(ptr, sizeof(u32));
            capture::GuestEvents::instance().event(
                "semaphore_release", capture::currentGuestEventOrigin(),
                semaphore_address(), size, 0, payload, arg);
            break;
        }
        case 0x10: { // REDUCTION: atomic read-modify-write of the 4-byte word
            const u32 reduction = (arg >> 27) & 0xf;
            const bool is_signed = (arg & (1u << 31)) == 0; // FORMAT_SIGNED = 0
            // Maxwell defines INC/DEC encodings; NVIDIA's Turing dev_pbdma
            // table supplies their unsigned-only equations (see test README).
            if (reduction > 7 || (reduction >= 6 && is_signed)) {
                ONCE(LOG_NOT_IMPLEMENTED(Gpu, "Semaphore reduction {} (signed={})", reduction, is_signed));
                if (tls_crnt_decode_state)
                    tls_crnt_decode_state->fail(25);
                break;
            }
            if ((arg & (1u << 20)) == 0 && !wait_for_gpu(CheckpointReason::PullerReduction))
                break;
            if (channel.idle_pending && !wait_for_gpu(CheckpointReason::PullerReference))
                break;
            auto* ptr = resolve(sizeof(u32));
            if (ptr == nullptr)
                break;
            reduce(std::atomic_ref<u32>(*reinterpret_cast<u32*>(ptr)));
            invalidate(ptr, sizeof(u32));
            // The trigger argument (auxiliary) tells a reduction from a release
            capture::GuestEvents::instance().event(
                "semaphore_release", capture::currentGuestEventOrigin(),
                semaphore_address(), 4, 0, payload, arg);
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
        if (!wait_for_gpu(CheckpointReason::PullerRelease))
            break;
        if (auto* ptr = resolve(sizeof(u32))) {
            std::atomic_ref<u32>(*reinterpret_cast<u32*>(ptr))
                .store(arg, std::memory_order_release);
            invalidate(ptr, sizeof(u32));
            capture::GuestEvents::instance().event(
                "semaphore_release", capture::currentGuestEventOrigin(),
                semaphore_address(), 4, 0, arg);
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
        if (!wait_for_gpu(CheckpointReason::PullerSyncpoint))
            break;
        if (operation == 1) {
            // The increment tells that the preceding work completed
            if (!wait_for_gpu())
                break;
            video_syncpoints->complete(id, 1);
        } else {
            const u32 threshold = puller_regs[SYNCPOINT_PAYLOAD];
            if (tls_crnt_decode_state) {
                // A wait does not need this channel's earlier work to finish:
                // a yielding channel commits its batch before PFIFO polls
                const auto value = video_syncpoints->read(id);
                if (!video::Syncpoints::reached(*value, threshold))
                    tls_crnt_decode_state->syncpoint_wait = Fence{id, threshold};
            } else {
                // Direct callers block here, the work they queued (which may
                // reach the threshold) has to be submitted first
                if (!wait_for_gpu())
                    break;
                while (!pfifo.isStopping() && !video_syncpoints->waitFor(
                    id, threshold, std::chrono::milliseconds(20))) {}
            }
        }
        break;
    }
    default:
        ONCE(LOG_NOT_IMPLEMENTED(Gpu, "Channel method {:#x}", method));
        if (tls_crnt_decode_state)
            tls_crnt_decode_state->fail(25);
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
    if ((*map)->owner_identity && (!mmu || (*map)->owner_identity != mmu->getDeviceMappings()))
        return nullptr;
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
