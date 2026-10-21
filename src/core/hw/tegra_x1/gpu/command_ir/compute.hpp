#pragma once

#include "core/hw/tegra_x1/gpu/command_ir/resource_set.hpp"
#include "core/hw/tegra_x1/gpu/renderer/renderer.hpp"

namespace hydra::hw::tegra_x1::gpu::command_ir {

struct ComputeCommand {
    renderer::ComputeDispatch dispatch{};
    ResourceSet resources;
    // Exact binding/shape snapshot; validation compares pointers without
    // dereferencing objects that may already have been retired.
    std::vector<u64> sealed_layout;
};

bool sealCompute(renderer::IRenderer& renderer, ComputeCommand& command);
bool validateCompute(renderer::IRenderer& renderer,
                     const ComputeCommand& command);
bool executeCompute(renderer::IRenderer& renderer,
                    renderer::ICommandBuffer* commands,
                    const ComputeCommand& command);

} // namespace hydra::hw::tegra_x1::gpu::command_ir
