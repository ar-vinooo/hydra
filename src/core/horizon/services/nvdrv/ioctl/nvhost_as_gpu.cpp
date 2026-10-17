#include "core/horizon/services/nvdrv/ioctl/nvhost_as_gpu.hpp"

#include "core/horizon/kernel/process.hpp"
#include "core/hw/tegra_x1/cpu/mmu.hpp"
#include "core/system.hpp"

namespace hydra::horizon::services::nvdrv::ioctl {

DEFINE_IOCTL_TABLE(NvHostAsGpu,
                   DEFINE_IOCTL_TABLE_ENTRY(NvHostAsGpu, 0x41, 0x01,
                                            bindChannel, 0x02, allocSpace, 0x03,
                                            freeSpace, 0x05, unmapBuffer, 0x06,
                                            mapBufferEx, 0x08, getVaRegions,
                                            0x09, allocAsEx, 0x14, remap))

DEFINE_IOCTL3_TABLE(NvHostAsGpu, DEFINE_IOCTL3_TABLE_ENTRY(NvHostAsGpu, 0x41,
                                                           0x08, getVaRegions))

NvResult NvHostAsGpu::bindChannel(u32 fd_id) {
    LOG_FUNC_WITH_ARGS_STUBBED(Services, "FD: {}", fd_id);
    return NvResult::Success;
}

NvResult NvHostAsGpu::allocSpace(kernel::Process* process, u32 pages,
                                 u32 page_size,
                                 Aligned<AllocSpaceFlags, 8> flags,
                                 InOut<u64, gpu_vaddr_t> align_and_offset) {
    uptr gpu_addr = invalid<uptr>();
    if (any(flags & AllocSpaceFlags::FixedOffset))
        gpu_addr = align_and_offset; // TODO: is it really align?

    align_and_offset = process->getGMmu().allocatePrivateAddressSpace(
        static_cast<u64>(pages) * static_cast<u64>(page_size), gpu_addr);
    return NvResult::Success;
}

NvResult NvHostAsGpu::freeSpace(vaddr_t offset, u32 pages, u32 page_size) {
    LOG_FUNC_WITH_ARGS_STUBBED(Services,
                               "offset: {:#x}, pages: {}, page_size: {:#x}",
                               offset, pages, page_size);
    return NvResult::Success;
}

namespace {

// Cached GPU resources of guest memory that is (re)mapped or unmapped may be
// stale: the guest can reuse the memory for other data, and CPU writes made
// while it was not GPU mapped are not tracked. Re-read it from guest memory.
void invalidateGuestRange(System* system, kernel::Process* process,
                          vaddr_t begin, u64 size) {
    if (size == 0)
        return;
    auto* mmu = process->getMmu();
    auto& renderer = system->getGpu().getRenderer();
    const vaddr_t end = begin + size;
    vaddr_t run_begin = begin;
    uptr run_ptr = mmu->unmapAddr(begin);
    vaddr_t cursor = begin;
    while (cursor < end) {
        const vaddr_t next =
            std::min(align(cursor + 1, hw::tegra_x1::cpu::GUEST_PAGE_SIZE), end);
        const uptr ptr = next < end ? mmu->unmapAddr(next) : 0;
        if (next == end || ptr != run_ptr + (next - run_begin)) {
            if (run_ptr != 0)
                renderer.invalidateMemory(
                    ztd::Range<uptr>::fromSize(run_ptr, next - run_begin));
            run_begin = next;
            run_ptr = ptr;
        }
        cursor = next;
    }
}

} // namespace

NvResult NvHostAsGpu::unmapBuffer(System* system, kernel::Process* process,
                                  gpu_vaddr_t addr) {
    // TODO: remove the GPU mapping (sparse reservations read as zero)
    const auto it = buffer_mappings.find(addr);
    if (it != buffer_mappings.end()) {
        invalidateGuestRange(system, process, it->second.first,
                             it->second.second);
        buffer_mappings.erase(it);
    }
    return NvResult::Success;
}

// TODO: kind
NvResult NvHostAsGpu::mapBufferEx(System* system, kernel::Process* process,
                                  MapBufferFlags flags,
                                  hw::tegra_x1::gpu::NvKind kind,
                                  Handle nvmap_handle,
                                  [[maybe_unused]] u32 reserved,
                                  u64 buffer_offset, u64 mapping_size,
                                  InOutSingle<gpu_vaddr_t> inout_addr) {
    (void)kind;

    if (any(flags & MapBufferFlags::Modify)) {
        // Changes the PTE kind (compression) of an existing mapping, which the
        // host does not model: the mapping stays as it is
        LOG_DEBUG(Services,
                  "Address space modifying (Gpu addr: {:#x}, size: {:#x})",
                  *inout_addr.data, mapping_size);
        return NvResult::Success;
    }

    ZTD_ASSIGN_OR(
        const auto map, system->getGpu().getMap(nvmap_handle), ZTD_PASS({
            LOG_WARN(Services, "Invalid nvmap handle {}", nvmap_handle);
            return NvResult::BadParameter; /* TODO: correct? */
        }));

    u64 size = mapping_size;
    if (size == 0x0)
        size = map->size; // TODO: correct?

    gpu_vaddr_t addr = invalid<uptr>();
    if (any(flags & MapBufferFlags::FixedOffset))
        addr = inout_addr;

    inout_addr = process->getGMmu().mapBufferToAddressSpace(
        ztd::Range<vaddr_t>::fromSize(map->addr + buffer_offset, size), addr);
    buffer_mappings[*inout_addr.data] = {map->addr + buffer_offset, size};
    invalidateGuestRange(system, process, map->addr + buffer_offset, size);
    return NvResult::Success;
}

NvResult NvHostAsGpu::getVaRegions(gpu_vaddr_t buffer_addr,
                                   InOutSingle<u32> inout_buffer_size,
                                   [[maybe_unused]] u32 reserved,
                                   std::array<VaRegion, 2>* out_va_regions) {
    LOG_FUNC_WITH_ARGS_STUBBED(Services,
                               "buffer address: {:#x}, buffer size: {}",
                               buffer_addr, inout_buffer_size);

    inout_buffer_size = 2 * sizeof(VaRegion);
    // HACK
    (*out_va_regions)[0] =
        VaRegion{.addr = 0x0, .page_size = hw::tegra_x1::gpu::GPU_PAGE_SIZE};
    (*out_va_regions)[1] = (*out_va_regions)[0];
    return NvResult::Success;
}

// Arguments in the order of the guest structure: flags, as_fd, big_page_size
NvResult NvHostAsGpu::allocAsEx(kernel::Process* process, u32 flags, i32 as_fd,
                                u32 big_page_size,
                                [[maybe_unused]] u32 reserved,
                                u64 va_range_start, u64 va_range_end,
                                u64 va_range_split) {
    LOG_FUNC_WITH_ARGS_STUBBED(Services,
                               "big page size: {:#x}, as fd: {}, flags: {:#x}, "
                               "start: {:#x}, end: {:#x}, split: {:#x}",
                               big_page_size, as_fd, flags, va_range_start,
                               va_range_end, va_range_split);

    if (big_page_size != 0) {
        if (!std::has_single_bit(big_page_size)) {
            LOG_ERROR(Services, "Big page size {:#x} is not a power of two",
                      big_page_size);
            return NvResult::BadValue;
        }
        as_big_page_size = big_page_size;
    }

    // TODO: why does nouveau pass 0x0 for all of these?

    // TODO: what is split for?
    process->getGMmu().allocatePrivateAddressSpace(
        va_range_end - va_range_start, va_range_start);
    return NvResult::Success;
}

// Binds (or unbinds) nvmap memory to sparse GPU address space reserved with
// allocSpace
NvResult NvHostAsGpu::remap(IoctlContext* context, const RemapOp* entries) {
    const usize count = context->in_stream->getSize() / sizeof(RemapOp);
    auto& gmmu = context->process->getGMmu();
    for (const auto& entry : std::span<const RemapOp>(entries, count)) {
        LOG_DEBUG(Services, "Remap: {}", entry);

        const gpu_vaddr_t gpu_addr =
            static_cast<u64>(entry.virt_offset_in_pages) * as_big_page_size;
        const u64 size = static_cast<u64>(entry.num_pages) * as_big_page_size;
        if (size == 0)
            continue;

        if (entry.mem_handle == 0) {
            gmmu.unbindToSparse(gpu_addr, size);
            continue;
        }

        ZTD_ASSIGN_OR(
            const auto map, context->system.getGpu().getMap(entry.mem_handle),
            ZTD_PASS({
                LOG_WARN(Services, "Invalid nvmap handle {}", entry.mem_handle);
                return NvResult::BadParameter;
            }));
        const u64 offset =
            static_cast<u64>(entry.mem_offset_in_pages) * as_big_page_size;
        gmmu.mapBufferToAddressSpace(
            ztd::Range<vaddr_t>::fromSize(map->addr + offset, size), gpu_addr);
    }

    return NvResult::Success;
}

} // namespace hydra::horizon::services::nvdrv::ioctl
