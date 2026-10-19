#include "core/hw/tegra_x1/gpu/renderer/metal/occlusion.hpp"

#include <cstring>

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

OcclusionPagePool::~OcclusionPagePool() {
    for (auto* buffer : free_buffers)
        buffer->release();
}

std::shared_ptr<OcclusionPage> OcclusionPagePool::acquire() {
    MTL::Buffer* buffer = nullptr;
    {
        std::scoped_lock lock(mutex);
        if (!free_buffers.empty()) {
            buffer = free_buffers.back();
            free_buffers.pop_back();
        }
    }
    if (buffer == nullptr) {
        buffer = device->newBuffer(OcclusionPage::SLOTS * sizeof(u64),
                                   MTL::ResourceStorageModeShared);
        ASSERT(buffer != nullptr, MetalRenderer,
               "Cannot allocate visibility buffer");
        std::memset(buffer->contents(), 0, OcclusionPage::SLOTS * sizeof(u64));
    }
    return std::shared_ptr<OcclusionPage>(
        new OcclusionPage{buffer, 0},
        [pool = shared_from_this()](OcclusionPage* page) {
            pool->release(page);
        });
}

void OcclusionPagePool::release(OcclusionPage* page) {
    // The GPU only wrote the slots that were handed out
    std::memset(page->buffer->contents(), 0, page->used * sizeof(u64));
    {
        std::scoped_lock lock(mutex);
        if (free_buffers.size() < MAX_FREE_PAGES) {
            free_buffers.push_back(page->buffer);
            page->buffer = nullptr;
        }
    }
    if (page->buffer != nullptr)
        page->buffer->release();
    delete page;
}

OcclusionCounter::OcclusionCounter(MTL::Device* device,
                                   std::shared_ptr<CompletionQueue> completions_)
    : completions{std::move(completions_)},
      pool{std::make_shared<OcclusionPagePool>(device)} {}

std::optional<u64>
OcclusionCounter::prepareDraw(CommandBuffer& command_buffer,
                              MTL::RenderPassDescriptor* render_pass) {
    if (!enabled)
        return std::nullopt;
    // Every page belongs to one encoder of one Metal command buffer
    if (open.page == nullptr || open.page->used == OcclusionPage::SLOTS ||
        open.token.get() != command_buffer.getCompletionToken() ||
        !command_buffer.hasRenderPass(render_pass) ||
        command_buffer.getVisibilityResultBuffer() != open.page->buffer) {
        closeSegment();
        open.page = pool->acquire();
        open.token = command_buffer.shareCompletionToken();
        open.begin = 0;
    }
    command_buffer.setVisibilityResultBuffer(open.page->buffer);
    return static_cast<u64>(open.page->used++) * sizeof(u64);
}

void OcclusionCounter::closeSegment() {
    if (open.page == nullptr || open.page->used == open.begin)
        return;
    completions->push(
        open.token, [totals = totals, page = open.page, begin = open.begin,
                     end = open.page->used](bool succeeded) {
            if (!succeeded) {
                totals->valid = false;
                return;
            }
            const auto* slots = static_cast<const u64*>(page->buffer->contents());
            for (u32 i = begin; i < end; i++)
                totals->value += slots[i];
        });
    open.begin = open.page->used;
}

void OcclusionCounter::reset() {
    // Earlier draws still have to finish before they can be forgotten, their
    // pages stay in use until then
    closeSegment();
    completions->push(nullptr, [totals = totals](bool) {
        totals->value = 0;
        totals->valid = true;
    });
}

u64 OcclusionCounter::queueReport(CommandBuffer& command_buffer,
                                  ReportPublisher publish,
                                  std::vector<ztd::Range<uptr>> writes) {
    closeSegment();
    // The callbacks of the submission follow the report
    command_buffer.noteQueuedCompletionWork();
    return completions->push(
        command_buffer.shareCompletionToken(),
        [totals = totals, publish = std::move(publish)](bool succeeded) {
            publish(succeeded && totals->valid
                        ? std::optional<u64>(totals->value)
                        : std::nullopt);
        },
        std::move(writes), true);
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
