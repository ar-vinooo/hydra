#pragma once

#include "core/hw/tegra_x1/gpu/renderer/metal/const.hpp"

#include <mutex>
#include <vector>

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

// Objects shared by the Metal 4 command buffers of a renderer. Command buffers
// are encoded on several threads, so the pools are thread safe.
//
// Metal 4 neither tracks hazards, nor retains or makes resident what a command
// buffer uses. CommandBuffer does that: it inserts barriers, and retains every
// allocation it encodes in a residency set until the GPU is done.
class Mtl4Context {
  public:
    // Null if the device does not support Metal 4
    static Mtl4Context* create(MTL::Device* device);
    ~Mtl4Context();
    Mtl4Context(const Mtl4Context&) = delete;
    Mtl4Context& operator=(const Mtl4Context&) = delete;

    // Allocators store the commands of one command buffer until it completed
    MTL4::CommandAllocator* acquireAllocator();
    void recycleAllocator(MTL4::CommandAllocator* allocator);

    // Residency sets of the allocations of one command buffer
    MTL::ResidencySet* acquireResidencySet();
    void recycleResidencySet(MTL::ResidencySet* set);

    // Shared memory for inline bytes (setBytes has no Metal 4 equivalent)
    static constexpr usize TRANSIENT_CHUNK_SIZE = 256 * 1024;
    MTL::Buffer* acquireTransientChunk();
    void recycleTransientChunk(MTL::Buffer* chunk);

    // Bindings of one stage, for all the buffer, texture and sampler indices
    MTL4::ArgumentTable* acquireArgumentTable();
    void recycleArgumentTable(MTL4::ArgumentTable* table);

    // The drawables of the layer are made resident through its residency set
    void addLayer(CA::MetalLayer* layer);

    MTL::Device* getDevice() const { return device; }
    MTL4::CommandQueue* getQueue() const { return queue; }
    // For MetalFX, null if creating it failed
    MTL4::Compiler* getCompiler() const { return compiler; }
    // Orders work encoded outside the command buffer's encoders (MetalFX)
    MTL::Fence* getExternalFence() const { return external_fence; }
    // A few bytes for work that only orders other work
    MTL::Buffer* getScratchBuffer() const { return scratch_buffer; }

  private:
    Mtl4Context(MTL::Device* device, MTL4::CommandQueue* queue);

    MTL::Device* device;
    MTL4::CommandQueue* queue;
    MTL4::Compiler* compiler{nullptr};
    MTL::Fence* external_fence;
    MTL::Buffer* scratch_buffer;

    std::mutex mutex;
    std::vector<MTL4::CommandAllocator*> free_allocators;
    std::vector<MTL::ResidencySet*> free_residency_sets;
    std::vector<MTL::Buffer*> free_transient_chunks;
    std::vector<MTL4::ArgumentTable*> free_argument_tables;
    CA::MetalLayer* layer{nullptr};
};

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
