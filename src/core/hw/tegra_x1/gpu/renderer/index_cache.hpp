#pragma once

#include "core/hw/tegra_x1/gpu/renderer/buffer_view.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer {

class BufferBase;
class IRenderer;

struct IndexDescriptor {
    engines::IndexType type;
    engines::PrimitiveType primitive_type;
    u32 count;
    std::optional<ztd::Range<uptr>> mem_range{std::nullopt};
    bool restart_enabled{false};
    u32 restart_index{0};
};

class IndexCache {
  public:
    explicit IndexCache(IRenderer& renderer_) : renderer{renderer_} {}
    ~IndexCache();

    BufferView decode(ICommandBuffer* command_buffer,
                      const IndexDescriptor& descriptor,
                      engines::IndexType& out_type,
                      engines::PrimitiveType& out_primitive_type,
                      u32& out_count);
    static u32 hash(const IndexDescriptor& descriptor);

  private:
    IRenderer& renderer;

    struct Entry {
        IndexDescriptor descriptor;
        // Guest indices the buffer was decoded from: they can be rewritten
        // in place
        std::vector<u8> source;
        std::shared_ptr<BufferBase> buffer;
        u32 count{0};
    };
    std::map<u32, Entry> cache;

  public:
    // ABI-stable implementations selected by the live module.
    BufferView decodeHot0(ICommandBuffer* command_buffer,
                          const IndexDescriptor& descriptor,
                          engines::IndexType& out_type,
                          engines::PrimitiveType& out_primitive_type,
                          u32& out_count);
    static u32 hashHot1(const IndexDescriptor& descriptor);
};

} // namespace hydra::hw::tegra_x1::gpu::renderer
