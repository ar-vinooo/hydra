#pragma once

#include "core/horizon/services/nvdrv/ioctl/fd_base.hpp"
#include "core/hw/tegra_x1/gpu/const.hpp"

#include <map>

namespace hydra::hw::tegra_x1::gpu { class GMmu; }

namespace hydra::horizon::services::nvdrv::ioctl {

enum class AllocSpaceFlags : u32 {
    None = 0,
    FixedOffset = ZTD_BIT(0),
    Sparse = ZTD_BIT(1),
};

ZTD_ENABLE_ENUM_BITWISE_OPERATORS(AllocSpaceFlags)

enum class MapBufferFlags : u32 {
    None = 0,
    FixedOffset = ZTD_BIT(0),
    IsCacheable = ZTD_BIT(2),
    Modify = ZTD_BIT(8),
};

ZTD_ENABLE_ENUM_BITWISE_OPERATORS(MapBufferFlags)

struct VaRegion {
    gpu_vaddr_t addr;
    u32 page_size;
    u32 reserved;
    u64 pages;
};

struct RemapOp {
    u16 flags;
    u16 kind;
    u32 mem_handle;
    u32 mem_offset_in_pages;
    u32 virt_offset_in_pages;
    u32 num_pages;
};

class NvHostAsGpu : public FdBase {
  public:
    NvResult ioctl([[maybe_unused]] IoctlContext& context, u32 type,
                   u32 nr) override;
    NvResult ioctl3([[maybe_unused]] IoctlContext& context, u32 type,
                    u32 nr) override;

  private:
    NvResult ioctlUnchecked(IoctlContext& context, u32 type, u32 nr);
    NvResult ensureAddressSpace(kernel::Process* process);
    // Ioctls
    NvResult bindChannel(IoctlContext& context, u32 fd_id);
    NvResult allocSpace(kernel::Process* process, u32 pages, u32 page_size,
                        Aligned<AllocSpaceFlags, 8> flags,
                        InOut<u64, gpu_vaddr_t> align_and_offset);
    NvResult freeSpace(vaddr_t offset, u32 pages, u32 page_size);
    NvResult unmapBuffer(System* system, kernel::Process* process,
                         gpu_vaddr_t addr);
    NvResult mapBufferEx(System* system, kernel::Process* process,
                         MapBufferFlags flags, hw::tegra_x1::gpu::NvKind kind,
                         Handle nvmap_handle, [[maybe_unused]] u32 reserved,
                         u64 buffer_offset, u64 mapping_size,
                         InOutSingle<gpu_vaddr_t> inout_addr);
    NvResult getVaRegions(gpu_vaddr_t buffer_addr,
                          InOutSingle<u32> inout_buffer_size,
                          [[maybe_unused]] u32 reserved,
                          std::array<VaRegion, 2>* out_va_regions);
    NvResult allocAsEx(kernel::Process* process, u32 flags, i32 as_fd,
                       u32 big_page_size, [[maybe_unused]] u32 reserved,
                       u64 va_range_start, u64 va_range_end,
                       u64 va_range_split);
    NvResult remap(IoctlContext* context, std::span<const RemapOp> entries);

  private:
    std::mutex address_space_mutex;
    kernel::Process* address_space_owner{nullptr};
    std::shared_ptr<hw::tegra_x1::gpu::GMmu> address_space;
    // Remap offsets are expressed in big pages
    u64 as_big_page_size{0x10000};
    // Guest range of each buffer mapping, by GPU address
    std::map<gpu_vaddr_t, std::pair<vaddr_t, u64>> buffer_mappings;
    struct Reservation {
        u64 size;
        bool sparse;
    };
    // AllocSpace reservations, by GPU address
    std::map<gpu_vaddr_t, Reservation> reservations;
    u64 as_va_begin{0};
    u64 as_va_end{0}; // 0: unbounded

    bool vaRangeUsed(gpu_vaddr_t begin, u64 size, gpu_vaddr_t& next) const;
    std::optional<gpu_vaddr_t> findFreeVa(u64 size, u64 alignment) const;
    // Forgets (splitting as needed) the buffer mappings overlapping a range
    void dropMappingRecords(gpu_vaddr_t begin, gpu_vaddr_t end);
    // Sparse zero backing for the part of a range inside sparse reservations
    void restoreSparse(gpu_vaddr_t begin, gpu_vaddr_t end);

  public:
    NvResult getVaRegions3(gpu_vaddr_t buffer_addr,
                           InOutSingle<u32> inout_buffer_size,
                           [[maybe_unused]] u32 reserved) {
        // TODO: does this just throw the out_va_regions away?
        std::array<VaRegion, 2> out_va_regions;
        return getVaRegions(buffer_addr, inout_buffer_size, reserved,
                            &out_va_regions);
    }
};

} // namespace hydra::horizon::services::nvdrv::ioctl

ENABLE_STRUCT_FORMATTING(hydra::horizon::services::nvdrv::ioctl::RemapOp, flags,
                         ":#x", "flags", kind, "", "kind", mem_handle, "",
                         "memory handle", mem_offset_in_pages, ":#x",
                         "memory offset (in pages)", virt_offset_in_pages,
                         ":#x", "virtual offset (in pages)", num_pages, ":#x",
                         "number of pages")
