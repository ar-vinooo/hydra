#include "core/horizon/services/nvdrv/ioctl/nvhost_as_gpu.hpp"

#include "core/horizon/kernel/process.hpp"
#include "core/hw/tegra_x1/cpu/mmu.hpp"
#include "core/system.hpp"

namespace hydra::horizon::services::nvdrv::ioctl {

DEFINE_IOCTL_TABLE_IMPL(NvHostAsGpu, Unchecked,
                   DEFINE_IOCTL_TABLE_ENTRY(NvHostAsGpu, 0x41, 0x02, allocSpace, 0x03,
                                            freeSpace, 0x05, unmapBuffer, 0x06,
                                            mapBufferEx, 0x08, getVaRegions,
                                            0x09, allocAsEx))

NvResult NvHostAsGpu::ioctl(IoctlContext& context, u32 type, u32 nr) {
    std::scoped_lock lock(address_space_mutex);
    if (type != 0x41)
        return ioctlUnchecked(context, type, nr);
    const auto initialized = ensureAddressSpace(context.process);
    if (initialized != NvResult::Success)
        return initialized;
    if (nr != 0x01 && nr != 0x06 && nr != 0x14)
        return ioctlUnchecked(context, type, nr);
    if (!context.process || !context.in_stream)
        return NvResult::BadParameter;
    auto& input = *context.in_stream;
    if (input.getSeek() > input.getSize())
        return NvResult::InvalidSize;
    const auto size = input.getSize() - input.getSeek();
    if (nr == 0x01) {
        if (size < sizeof(u32))
            return NvResult::InvalidSize;
        u32 fd;
        std::memcpy(&fd, input.getPtr() + input.getSeek(), sizeof(fd));
        return bindChannel(context, fd);
    }
    if (nr == 0x14) {
        if (size % sizeof(RemapOp) != 0)
            return NvResult::InvalidSize;
        std::vector<RemapOp> entries;
        try {
            entries.resize(size / sizeof(RemapOp));
        } catch (const std::bad_alloc&) {
            return NvResult::InsufficientMemory;
        }
        if (size != 0)
            std::memcpy(entries.data(), input.getPtr() + input.getSeek(), size);
        return remap(&context, entries);
    }
    constexpr usize MAP_BUFFER_SIZE = 40;
    if (!context.out_stream)
        return NvResult::BadParameter;
    auto& output = *context.out_stream;
    if (size < MAP_BUFFER_SIZE || output.getSeek() > output.getSize() ||
        output.getSize() - output.getSeek() < MAP_BUFFER_SIZE)
        return NvResult::InvalidSize;
    // The generic ioctl argument reader requires aligned buffers.
    alignas(8) std::array<u8, MAP_BUFFER_SIZE> request{}, response{};
    std::memcpy(request.data(), input.getPtr() + input.getSeek(), request.size());
    IoctlContext checked{
        .system = context.system, .process = context.process,
        .in_stream = ztd::io::MemoryStream(request),
        .out_stream = ztd::io::MemoryStream(response)};
    std::memcpy(response.data(), request.data(), response.size());
    const auto result = ioctlUnchecked(checked, type, nr);
    if (result == NvResult::Success)
        std::memcpy(output.getPtr() + output.getSeek(), response.data(), response.size());
    return result;
}

DEFINE_IOCTL3_TABLE(NvHostAsGpu, DEFINE_IOCTL3_TABLE_ENTRY(NvHostAsGpu, 0x41,
                                                           0x08, getVaRegions))

NvResult NvHostAsGpu::ensureAddressSpace(kernel::Process* process) {
    if (!process)
        return NvResult::BadParameter;
    if (address_space_owner && address_space_owner != process)
        return NvResult::BadParameter;
    if (address_space)
        return address_space->gpuWorkCancelled() ? NvResult::InvalidState : NvResult::Success;
    try {
        address_space = process->createGpuAddressSpace();
    } catch (const std::bad_alloc&) {
        return NvResult::InsufficientMemory;
    }
    if (!address_space)
        return NvResult::InvalidState;
    address_space_owner = process;
    return NvResult::Success;
}

NvResult NvHostAsGpu::bindChannel(IoctlContext& context, u32 fd_id) {
    if (!context.resolve_fd)
        return NvResult::BadParameter;
    auto* channel = context.resolve_fd(fd_id);
    if (!channel)
        return NvResult::BadParameter;
    return channel->bindAddressSpace(*context.process, address_space);
}

namespace {

constexpr u64 MIN_PAGE_SIZE = 0x1000;

bool validPageSize(u32 page_size) {
    return page_size >= MIN_PAGE_SIZE && std::has_single_bit(page_size);
}

u64 occupiedSize(u64 size) { return align(size, hw::tegra_x1::gpu::GPU_PAGE_SIZE); }

} // namespace

// Whether the range intersects a reservation or buffer mapping; `next` is
// the end of the first intersecting one
bool NvHostAsGpu::vaRangeUsed(gpu_vaddr_t begin, u64 size,
                              gpu_vaddr_t& next) const {
    const auto end = begin + size;
    const auto overlaps = [&](const auto& map, auto extent) {
        auto it = map.upper_bound(begin);
        if (it != map.begin()) {
            const auto prev = std::prev(it);
            if (prev->first + extent(prev->second) > begin) {
                next = prev->first + extent(prev->second);
                return true;
            }
        }
        if (it != map.end() && it->first < end) {
            next = it->first + extent(it->second);
            return true;
        }
        return false;
    };
    return overlaps(reservations, [](const Reservation& r) { return r.size; }) ||
           overlaps(buffer_mappings, [](const auto& m) { return occupiedSize(m.second); });
}

std::optional<gpu_vaddr_t> NvHostAsGpu::findFreeVa(u64 size,
                                                   u64 alignment) const {
    alignment = std::max<u64>(alignment, hw::tegra_x1::gpu::GPU_PAGE_SIZE);
    const u64 limit = as_va_end != 0 ? as_va_end : invalid<u64>();
    gpu_vaddr_t candidate = align(
        std::max<u64>(as_va_begin, hw::tegra_x1::gpu::GPU_PAGE_SIZE), alignment);
    while (candidate <= limit && size <= limit - candidate) {
        gpu_vaddr_t next;
        if (!vaRangeUsed(candidate, size, next))
            return candidate;
        if (next > invalid<u64>() - alignment)
            break;
        candidate = align(next, alignment);
    }
    return std::nullopt;
}

void NvHostAsGpu::dropMappingRecords(gpu_vaddr_t begin, gpu_vaddr_t end) {
    auto it = buffer_mappings.upper_bound(begin);
    if (it != buffer_mappings.begin())
        --it;
    while (it != buffer_mappings.end() && it->first < end) {
        const auto base = it->first;
        const auto [guest, size] = it->second;
        const auto record_end = base + size;
        if (record_end <= begin) {
            ++it;
            continue;
        }
        it = buffer_mappings.erase(it);
        if (base < begin)
            buffer_mappings[base] = {guest, begin - base};
        if (record_end > end)
            it = buffer_mappings
                     .emplace(end, std::pair{guest + (end - base), record_end - end})
                     .first;
        if (it != buffer_mappings.end() && it->first < end)
            ++it;
    }
}

void NvHostAsGpu::restoreSparse(gpu_vaddr_t begin, gpu_vaddr_t end) {
    for (const auto& [base, reservation] : reservations) {
        if (!reservation.sparse)
            continue;
        const auto from = std::max(begin, base);
        const auto to = std::min(end, base + reservation.size);
        if (from < to)
            address_space->reserveSparse(from, to - from);
    }
}

NvResult NvHostAsGpu::allocSpace(kernel::Process* process, u32 pages,
                                 u32 page_size,
                                 Aligned<AllocSpaceFlags, 8> flags,
                                 InOut<u64, gpu_vaddr_t> align_and_offset) {
    (void)process;
    constexpr auto KNOWN = AllocSpaceFlags::FixedOffset | AllocSpaceFlags::Sparse;
    if (any(flags & ~KNOWN) || !validPageSize(page_size) || pages == 0)
        return NvResult::BadValue;
    const u64 size = static_cast<u64>(pages) * page_size;

    gpu_vaddr_t gpu_addr;
    if (any(flags & AllocSpaceFlags::FixedOffset)) {
        gpu_addr = align_and_offset;
        if (gpu_addr % page_size != 0 || size > invalid<u64>() - gpu_addr ||
            gpu_addr + size > (as_va_end != 0 ? as_va_end : invalid<u64>()))
            return NvResult::BadValue;
        // Reservations never overlap each other. Buffers already mapped
        // there stay mapped.
        const auto overlaps_reservation = [&] {
            auto it = reservations.upper_bound(gpu_addr);
            if (it != reservations.begin() &&
                std::prev(it)->first + std::prev(it)->second.size > gpu_addr)
                return true;
            return it != reservations.end() && it->first < gpu_addr + size;
        }();
        if (overlaps_reservation)
            return NvResult::BadValue;
    } else {
        // The alignment is the flag union's input; the VA is page aligned
        const auto found = findFreeVa(size, std::max<u64>(page_size, align_and_offset));
        if (!found)
            return NvResult::InsufficientMemory;
        gpu_addr = *found;
    }

    const bool sparse = any(flags & AllocSpaceFlags::Sparse);
    if (sparse)
        address_space->reserveSparse(gpu_addr, size);
    reservations[gpu_addr] = {size, sparse};
    align_and_offset = gpu_addr;
    return NvResult::Success;
}

// Mapped buffers inside the reservation stay mapped (they become ordinary
// mappings); only the zero backing of the sparse holes goes away.
NvResult NvHostAsGpu::freeSpace(vaddr_t offset, u32 pages, u32 page_size) {
    if (!validPageSize(page_size))
        return NvResult::BadValue;
    const auto it = reservations.find(offset);
    if (it == reservations.end() ||
        it->second.size != static_cast<u64>(pages) * page_size)
        return NvResult::BadValue;
    if (it->second.sparse)
        address_space->releaseSparse(it->first, it->second.size);
    reservations.erase(it);
    return NvResult::Success;
}

namespace {

// NVN maps whole big pages of an nvmap whose size is not a big page multiple,
// so the mapped size may reach the nvmap size rounded up to `granularity`; the
// backing check below still requires every mapped page to be readable guest
// memory.
NvResult resolveNvmapRange(System& system, kernel::Process& process,
                           Handle handle, u64 offset, u64 size,
                           ztd::Range<vaddr_t>& range, u64 granularity,
                           ztd::Range<vaddr_t>* backed = nullptr) {
    const auto map = system.getGpu().getMap(handle);
    if (!map)
        return NvResult::BadParameter;
    // The guest address only resolves in the process that created the handle
    if ((*map)->owner != nullptr && (*map)->owner != &process)
        return NvResult::BadParameter;
    if (size == 0)
        size = (*map)->size;
    const u64 limit = align((*map)->size, std::max<u64>(granularity, 1));
    if (size == 0 || offset > limit || size > limit - offset) {
        LOG_WARN(Services,
                 "nvmap {} range out of bounds: nvmap size {:#x}, offset {:#x}, "
                 "size {:#x}",
                 handle, (*map)->size, offset, size);
        return NvResult::InvalidSize;
    }
    const auto begin = (*map)->addr;
    if (!kernel::ADDRESS_SPACE.contains(begin) ||
        offset > kernel::ADDRESS_SPACE.getEnd() - begin ||
        size > kernel::ADDRESS_SPACE.getEnd() - begin - offset) {
        LOG_WARN(Services,
                 "nvmap {} range outside the guest address space: addr {:#x}, "
                 "offset {:#x}, size {:#x}",
                 handle, begin, offset, size);
        return NvResult::InvalidAddress;
    }
    const auto start = begin + offset;
    const auto end = start + size;
    // Sizes are rounded up to the big page size, but only the nvmap allocation
    // itself has guest memory behind it (the rest is alignment padding that
    // the guest never accesses), e.g. a 0x1e9000 byte NVN pool mapped as 0x1f0000
    const auto backed_end = std::clamp(
        align(begin + (*map)->size, hw::tegra_x1::cpu::GUEST_PAGE_SIZE), start,
        end);
    auto* mmu = process.getMmu();
    for (auto cursor = start; cursor < backed_end;) {
        const auto region = mmu->queryRegion(cursor);
        if (region.state.type == kernel::MemoryType::Free ||
            !any(region.state.perm & kernel::MemoryPermission::Read) ||
            mmu->unmapAddr(cursor) == 0) {
            LOG_WARN(Services,
                     "nvmap {} page {:#x} not usable (range {:#x}-{:#x}): "
                     "type {}, perm {:#x}, backing {:#x}",
                     handle, cursor, start, end,
                     static_cast<u32>(region.state.type),
                     static_cast<u32>(region.state.perm),
                     static_cast<u64>(mmu->unmapAddr(cursor)));
            return NvResult::InvalidAddress;
        }
        cursor = std::min(align(cursor + 1, hw::tegra_x1::cpu::GUEST_PAGE_SIZE), backed_end);
    }
    if (backed != nullptr)
        *backed = ztd::Range<vaddr_t>(start, backed_end);
    range = ztd::Range<vaddr_t>::fromSize(start, size);
    return NvResult::Success;
}

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

// Unknown addresses succeed, like the Linux driver, which only logs them
NvResult NvHostAsGpu::unmapBuffer(System* system, kernel::Process* process,
                                  gpu_vaddr_t addr) {
    const auto it = buffer_mappings.find(addr);
    if (it == buffer_mappings.end()) {
        LOG_DEBUG(Services, "Unmap of unmapped GPU address {:#x}", addr);
        return NvResult::Success;
    }
    const auto [guest, size] = it->second;
    buffer_mappings.erase(it);
    address_space->unmapRange(addr, size, false);
    restoreSparse(addr, addr + size);
    invalidateGuestRange(system, process, guest, size);
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

    ztd::Range<vaddr_t> range, backed;
    const auto result = resolveNvmapRange(*system, *process, nvmap_handle,
                                         buffer_offset, mapping_size, range,
                                         as_big_page_size, &backed);
    if (result != NvResult::Success) {
        LOG_WARN(Services,
                 "MapBufferEx failed ({}): handle {}, offset {:#x}, size {:#x}, "
                 "flags {:#x}",
                 static_cast<u32>(result), nvmap_handle, buffer_offset,
                 mapping_size, static_cast<u32>(flags));
        return result;
    }

    gpu_vaddr_t addr;
    if (any(flags & MapBufferFlags::FixedOffset)) {
        addr = inout_addr;
        if (range.getSize() > invalid<u64>() - addr)
            return NvResult::InvalidAddress;
    } else {
        const auto found = findFreeVa(range.getSize(), 0);
        if (!found)
            return NvResult::InsufficientMemory;
        addr = *found;
    }

    // A fixed mapping replaces whatever it overlaps
    dropMappingRecords(addr, addr + range.getSize());
    // The whole requested range of GPU addresses stays reserved, only the
    // part with guest memory behind it is mapped
    if (backed.getSize() != 0)
        inout_addr = address_space->mapBufferToAddressSpace(backed, addr);
    else
        inout_addr = addr;
    buffer_mappings[*inout_addr.data] = {range.getBegin(), range.getSize()};
    invalidateGuestRange(system, process, backed.getBegin(), backed.getSize());
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

    // Callers (libnx, nouveau) usually pass zero, meaning the default range.
    // Nothing is backed: unmapped addresses fault like on hardware.
    (void)process;
    (void)va_range_split;
    if (va_range_end != 0 && va_range_end <= va_range_start)
        return NvResult::BadValue;
    as_va_begin = va_range_start;
    as_va_end = va_range_end;
    return NvResult::Success;
}

// Binds (or unbinds) nvmap memory to sparse GPU address space reserved with
// allocSpace
NvResult NvHostAsGpu::remap(IoctlContext* context, std::span<const RemapOp> entries) {
    // Validate every backing range before replacing any mapping in the batch.
    for (const auto& entry : entries) {
        if (entry.num_pages == 0 || entry.mem_handle == 0)
            continue;
        ztd::Range<vaddr_t> range, backed;
        const auto result = resolveNvmapRange(context->system, *context->process,
            Handle(entry.mem_handle), u64(entry.mem_offset_in_pages) * as_big_page_size,
            u64(entry.num_pages) * as_big_page_size, range, as_big_page_size, &backed);
        if (result != NvResult::Success)
            return result;
    }
    auto& gmmu = *address_space;
    for (const auto& entry : entries) {
        LOG_DEBUG(Services, "Remap: {}", entry);

        const gpu_vaddr_t gpu_addr =
            static_cast<u64>(entry.virt_offset_in_pages) * as_big_page_size;
        const u64 size = static_cast<u64>(entry.num_pages) * as_big_page_size;
        if (size == 0)
            continue;

        dropMappingRecords(gpu_addr, gpu_addr + size);
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
        // Alignment padding past the nvmap allocation has no guest memory
        const u64 backed_size =
            offset >= map->size ? 0 : std::min<u64>(size, map->size - offset);
        if (backed_size != 0)
            gmmu.mapBufferToAddressSpace(
                ztd::Range<vaddr_t>::fromSize(map->addr + offset, backed_size),
                gpu_addr);
    }

    return NvResult::Success;
}

} // namespace hydra::horizon::services::nvdrv::ioctl
