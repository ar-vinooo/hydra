#include "core/horizon/services/nvdrv/ioctl/nvmap.hpp"

#include "core/system.hpp"
#include "core/horizon/kernel/process.hpp"
#include "core/hw/tegra_x1/cpu/mmu.hpp"

namespace hydra::horizon::services::nvdrv::ioctl {

DEFINE_IOCTL_TABLE_IMPL(NvMap, Unchecked,
                   DEFINE_IOCTL_TABLE_ENTRY(NvMap, 0x01, 0x01, create, 0x03,
                                            fromId, 0x04, alloc, 0x05, free,
                                            0x09, param, 0x0e, getId))

NvMap::~NvMap() {
    if (bound_system)
        for (const auto& [handle, count] : references)
            for (u32 i = 0; i < count; ++i)
                bound_system->getGpu().freeMap(handle);
}

NvResult NvMap::ioctl(IoctlContext& context, u32 type, u32 nr) {
    std::scoped_lock lock(mutex);
    if (type != 1)
        return NvResult::NotImplemented;
    const usize size = nr == 4 ? 32 : nr == 5 ? 24 : nr == 9 ? 12 :
        (nr == 1 || nr == 3 || nr == 0xe) ? 8 : 0;
    if (!size)
        return NvResult::NotImplemented;
    if (!context.process || !context.in_stream || !context.out_stream)
        return NvResult::BadParameter;
    if ((bound_system && bound_system != &context.system) ||
        (bound_process && bound_process != context.process) ||
        (bound_identity && bound_identity != context.process->getMmu()->getDeviceMappings()))
        return NvResult::BadParameter;
    auto& input = *context.in_stream;
    auto& output = *context.out_stream;
    if (input.getSeek() > input.getSize() || size > input.getSize() - input.getSeek() ||
        output.getSeek() > output.getSize() || size > output.getSize() - output.getSeek())
        return NvResult::InvalidSize;
    alignas(8) std::array<u8, 32> request{}, response{};
    std::memcpy(request.data(), input.getPtr() + input.getSeek(), size);
    std::memcpy(response.data(), request.data(), size);
    if (nr == 4 || nr == 5 || nr == 9 || nr == 0xe) {
        u32 handle;
        std::memcpy(&handle, request.data() + (nr == 0xe ? 4 : 0), 4);
        if (!references.contains(handle))
            return NvResult::BadParameter;
    }
    bound_system = &context.system;
    bound_process = context.process;
    bound_identity = context.process->getMmu()->getDeviceMappings();
    IoctlContext checked{.system = context.system, .process = context.process,
        .in_stream = ztd::io::MemoryStream(request),
        .out_stream = ztd::io::MemoryStream(response)};
    try {
        const auto result = ioctlUnchecked(checked, type, nr);
        if (result == NvResult::Success)
            std::memcpy(output.getPtr() + output.getSeek(), response.data(), size);
        return result;
    } catch (const std::bad_alloc&) {
        return NvResult::InsufficientMemory;
    }
}

NvResult NvMap::create(System* system, kernel::Process* process, u32 size, Handle* out_handle) {
    if (size == 0)
        return NvResult::InvalidSize;
    const auto handle = system->getGpu().createMap(size, process, process->getMmu()->getDeviceMappings());
    if (!handle)
        return NvResult::InsufficientMemory;
    try {
        references.emplace(handle->getRaw(), 1);
    } catch (...) {
        system->getGpu().freeMap(*handle);
        throw;
    }
    *out_handle = handle->getRaw();
    return NvResult::Success;
}

NvResult NvMap::fromId(System* system, kernel::Process* process, u32 id, Handle* out_handle) {
    const auto map = system->getGpu().getMap(id);
    if (!map || !(*map)->addr)
        return NvResult::BadParameter;
    // Guest VA backing cannot yet be imported from a foreign CPU address space.
    if (((*map)->owner_identity && (*map)->owner_identity != process->getMmu()->getDeviceMappings()) ||
        ((*map)->owner && (*map)->owner != process))
        return NvResult::NotSupported;
    auto [entry, inserted] = references.try_emplace(id, 0);
    if (entry->second == invalid<u32>() || !system->getGpu().retainMap(id)) {
        if (inserted)
            references.erase(entry);
        return NvResult::BadParameter;
    }
    ++entry->second;
    *out_handle = id;
    return NvResult::Success;
}

// TODO: heap mask, kind
NvResult NvMap::alloc(System* system, Handle handle, u32 heap_mask, u32 flags,
                      InOutSingle<u32> inout_alignment, Aligned<u8, 8> kind,
                      gpu_vaddr_t addr) {
    (void)heap_mask;
    if (*inout_alignment.data != 0 && !std::has_single_bit(*inout_alignment.data))
        return NvResult::BadValue;
    const u32 alignment = std::max<u32>(inout_alignment, 0x1000);
    if (!system->getGpu().allocateMap(handle, addr, (flags & 1u) != 0, alignment, kind, flags))
        return NvResult::BadParameter;
    inout_alignment = alignment;
    return NvResult::Success;
}

NvResult NvMap::free(System* system, Aligned<Handle, 8> handle,
                     u64* out_addr, u32* out_size, u32* out_flags) {
    const auto found = system->getGpu().getMap(handle);
    if (!found)
        return NvResult::BadParameter;
    const auto map = **found;
    if (!system->getGpu().freeMap(handle))
        return NvResult::BadParameter;
    const auto local = references.find(static_cast<Handle>(handle).getRaw());
    if (--local->second == 0)
        references.erase(local);
    // Switchbrew describes an address (zero while referenced); libnx labels
    // this ignored field refcount. Preserve the address ABI pending traces.
    *out_addr = map.references->load() == 0 ? map.addr : 0;
    *out_size = static_cast<u32>(map.size);
    *out_flags = (map.allocation_flags >> 1) & 1u;
    return NvResult::Success;
}

NvResult NvMap::param(System* system, Handle handle, NvMapParamType type,
                      u32* out_value) {
    const auto found = system->getGpu().getMap(handle);
    if (!found)
        return NvResult::BadParameter;
    const auto map = *found;
    switch (type) {
    case NvMapParamType::Size:
        *out_value = static_cast<u32>(map->size);
        break;
    case NvMapParamType::Alignment:
        *out_value = map->alignment;
        break;
    case NvMapParamType::Heap:
        *out_value = 0x40000000;
        break;
    case NvMapParamType::Kind:
        *out_value = map->kind;
        break;
    case NvMapParamType::Compr:
        LOG_NOT_IMPLEMENTED(Services, "NvMapParamType::Compr");
        *out_value = 0;
        break;
    default: // Also includes Base
        // TODO: return NvInternalResult::InvalidInput?
        return NvResult::NotSupported;
    }
    return NvResult::Success;
}

NvResult NvMap::getId(u32* out_id, Handle handle) {
    // Handle and ID are the same
    *out_id = handle.getRaw();
    return NvResult::Success;
}

} // namespace hydra::horizon::services::nvdrv::ioctl
