#include "core/hw/tegra_x1/gpu/renderer/metal/mtl4_context.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

Mtl4Context* Mtl4Context::create(MTL::Device* device) {
    if (!device->supportsFamily(MTL::GPUFamilyMetal4))
        return nullptr;
    auto* queue = device->newMTL4CommandQueue();
    if (queue == nullptr)
        return nullptr;
    return new Mtl4Context(device, queue);
}

Mtl4Context::Mtl4Context(MTL::Device* device_, MTL4::CommandQueue* queue_)
    : device{device_->retain()}, queue{queue_},
      external_fence{device_->newFence()},
      scratch_buffer{device_->newBuffer(16, MTL::ResourceStorageModePrivate)} {
    NS_STACK_SCOPED auto descriptor = MTL4::CompilerDescriptor::alloc()->init();
    NS::Error* error = nullptr;
    compiler = device->newCompiler(descriptor, &error);
    if (compiler == nullptr)
        LOG_WARN(MetalRenderer, "Failed to create a Metal 4 compiler: {}",
                 error ? error->localizedDescription()->utf8String()
                       : "unknown error");
}

Mtl4Context::~Mtl4Context() {
    for (auto* allocator : free_allocators)
        allocator->release();
    for (auto* set : free_residency_sets)
        set->release();
    for (auto* chunk : free_transient_chunks)
        chunk->release();
    for (auto* table : free_argument_tables)
        table->release();
    if (layer != nullptr) {
        queue->removeResidencySet(layer->residencySet());
        layer->release();
    }
    if (compiler != nullptr)
        compiler->release();
    external_fence->release();
    scratch_buffer->release();
    queue->release();
    device->release();
}

MTL4::CommandAllocator* Mtl4Context::acquireAllocator() {
    {
        std::scoped_lock lock(mutex);
        if (!free_allocators.empty()) {
            auto* allocator = free_allocators.back();
            free_allocators.pop_back();
            return allocator;
        }
    }
    return device->newCommandAllocator();
}

void Mtl4Context::recycleAllocator(MTL4::CommandAllocator* allocator) {
    // The GPU finished the commands it holds
    allocator->reset();
    std::scoped_lock lock(mutex);
    free_allocators.push_back(allocator);
}

MTL::ResidencySet* Mtl4Context::acquireResidencySet() {
    {
        std::scoped_lock lock(mutex);
        if (!free_residency_sets.empty()) {
            auto* set = free_residency_sets.back();
            free_residency_sets.pop_back();
            return set;
        }
    }
    NS_STACK_SCOPED auto descriptor =
        MTL::ResidencySetDescriptor::alloc()->init();
    descriptor->setInitialCapacity(1024);
    NS::Error* error = nullptr;
    auto* set = device->newResidencySet(descriptor, &error);
    if (set == nullptr)
        LOG_ERROR(MetalRenderer, "Failed to create a residency set: {}",
                  error ? error->localizedDescription()->utf8String()
                        : "unknown error");
    return set;
}

void Mtl4Context::recycleResidencySet(MTL::ResidencySet* set) {
    set->removeAllAllocations();
    set->commit();
    std::scoped_lock lock(mutex);
    free_residency_sets.push_back(set);
}

MTL::Buffer* Mtl4Context::acquireTransientChunk() {
    {
        std::scoped_lock lock(mutex);
        if (!free_transient_chunks.empty()) {
            auto* chunk = free_transient_chunks.back();
            free_transient_chunks.pop_back();
            return chunk;
        }
    }
    return device->newBuffer(TRANSIENT_CHUNK_SIZE,
                             MTL::ResourceStorageModeShared |
                                 MTL::ResourceHazardTrackingModeUntracked);
}

void Mtl4Context::recycleTransientChunk(MTL::Buffer* chunk) {
    std::scoped_lock lock(mutex);
    free_transient_chunks.push_back(chunk);
}

MTL4::ArgumentTable* Mtl4Context::acquireArgumentTable() {
    {
        std::scoped_lock lock(mutex);
        if (!free_argument_tables.empty()) {
            auto* table = free_argument_tables.back();
            free_argument_tables.pop_back();
            return table;
        }
    }
    NS_STACK_SCOPED auto descriptor =
        MTL4::ArgumentTableDescriptor::alloc()->init();
    descriptor->setMaxBufferBindCount(BUFFER_COUNT);
    descriptor->setMaxTextureBindCount(TEXTURE_COUNT);
    descriptor->setMaxSamplerStateBindCount(SAMPLER_COUNT);
    // Unbound slots read as null, as with a new Metal 3 encoder
    descriptor->setInitializeBindings(true);
    NS::Error* error = nullptr;
    auto* table = device->newArgumentTable(descriptor, &error);
    ASSERT(table != nullptr, MetalRenderer,
           "Failed to create an argument table: {}",
           error ? error->localizedDescription()->utf8String()
                 : "unknown error");
    return table;
}

void Mtl4Context::recycleArgumentTable(MTL4::ArgumentTable* table) {
    // Encoders copied the bindings they used
    std::scoped_lock lock(mutex);
    free_argument_tables.push_back(table);
}

void Mtl4Context::addLayer(CA::MetalLayer* layer_) {
    std::scoped_lock lock(mutex);
    if (layer_ == layer)
        return;
    if (layer != nullptr) {
        queue->removeResidencySet(layer->residencySet());
        layer->release();
    }
    layer = layer_->retain();
    queue->addResidencySet(layer->residencySet());
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
