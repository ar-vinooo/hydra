#pragma once

#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/ir/intrinsic.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::codegen::lang::
    msl {

// MSL helper function implementing the intrinsic
std::string_view getIntrinsicSource(ir::Intrinsic intrinsic);
// Name of the helper function
std::string_view getIntrinsicFunctionName(ir::Intrinsic intrinsic);

} // namespace
  // hydra::hw::tegra_x1::gpu::renderer::shader_decomp::codegen::lang::msl
