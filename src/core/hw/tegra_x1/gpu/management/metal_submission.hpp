#pragma once

#include "core/hw/tegra_x1/gpu/renderer/metal/const.hpp"
#include <memory>
#include <unordered_map>

namespace hydra::hw::tegra_x1::gpu::renderer::metal {
class CommandBuffer;
class CompletionQueue;
void commitManagedSubmission(
    CommandBuffer& commands, const std::shared_ptr<CompletionQueue>& completions,
    std::unordered_map<const void*, std::shared_ptr<void>> resources = {});
// A standalone Metal 3 command buffer
void commitManagedSubmission(
    MTL::CommandBuffer* commands, const std::shared_ptr<CompletionQueue>& completions,
    std::unordered_map<const void*, std::shared_ptr<void>> resources = {});
} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
