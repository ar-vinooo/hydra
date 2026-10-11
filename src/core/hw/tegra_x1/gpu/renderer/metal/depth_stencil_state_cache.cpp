#include "core/hw/tegra_x1/gpu/renderer/metal/depth_stencil_state_cache.hpp"

#include "core/hw/tegra_x1/gpu/renderer/metal/maxwell_to_mtl.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

namespace {

MTL::StencilOperation toMtlStencilOp(StencilOp op) {
    switch (op) {
    case StencilOp::Keep:
        return MTL::StencilOperationKeep;
    case StencilOp::Zero:
        return MTL::StencilOperationZero;
    case StencilOp::Replace:
        return MTL::StencilOperationReplace;
    case StencilOp::IncrSat:
        return MTL::StencilOperationIncrementClamp;
    case StencilOp::DecrSat:
        return MTL::StencilOperationDecrementClamp;
    case StencilOp::Invert:
        return MTL::StencilOperationInvert;
    case StencilOp::Incr:
        return MTL::StencilOperationIncrementWrap;
    case StencilOp::Decr:
        return MTL::StencilOperationDecrementWrap;
    }
    return MTL::StencilOperationKeep;
}

MTL::StencilDescriptor* createStencilDescriptor(const StencilFaceState& face) {
    auto desc = MTL::StencilDescriptor::alloc()->init();
    desc->setStencilCompareFunction(toMtlCompareFunc(face.func));
    desc->setStencilFailureOperation(toMtlStencilOp(face.fail_op));
    desc->setDepthFailureOperation(toMtlStencilOp(face.depth_fail_op));
    desc->setDepthStencilPassOperation(toMtlStencilOp(face.pass_op));
    desc->setReadMask(face.func_mask & 0xff);
    desc->setWriteMask(face.write_mask & 0xff);
    return desc;
}

} // namespace

MTL::DepthStencilState*
DepthStencilStateCache::create(const DepthStencilStateDescriptor& descriptor) {
    auto desc = MTL::DepthStencilDescriptor::alloc()->init();

    // DEBUG: HYDRA_DEBUG_DEPTH_ALWAYS=1 makes every enabled depth test pass
    static const bool debug_depth_always =
        std::getenv("HYDRA_DEBUG_DEPTH_ALWAYS") != nullptr;
    if (descriptor.depth_test_enabled)
        desc->setDepthCompareFunction(
            debug_depth_always
                ? MTL::CompareFunctionAlways
                : toMtlCompareFunc(descriptor.depth_compare_op));

    // HACK: Minecraft: Story Mode overrides the depth buffer
    if (descriptor.depth_write_enabled && descriptor.depth_test_enabled &&
        descriptor.depth_compare_op != engines::CompareOp::Always)
        desc->setDepthWriteEnabled(true);

    if (descriptor.stencil_enabled) {
        auto front = createStencilDescriptor(descriptor.stencil_front);
        auto back = createStencilDescriptor(descriptor.stencil_back);
        desc->setFrontFaceStencil(front);
        desc->setBackFaceStencil(back);
        front->release();
        back->release();
    }

    auto state = device->newDepthStencilState(desc);
    desc->release();
    return state;
}

u32 DepthStencilStateCache::hash(
    const DepthStencilStateDescriptor& descriptor) {
    ztd::hash::XxHash32 hash;
    hash.add(descriptor.depth_test_enabled);
    hash.add(descriptor.depth_write_enabled);
    hash.add(descriptor.depth_compare_op);
    hash.add(descriptor.stencil_enabled);
    if (descriptor.stencil_enabled) {
        for (const auto& face :
             {descriptor.stencil_front, descriptor.stencil_back}) {
            hash.add(face.func);
            hash.add(face.fail_op);
            hash.add(face.depth_fail_op);
            hash.add(face.pass_op);
            hash.add(face.func_mask & 0xff);
            hash.add(face.write_mask & 0xff);
        }
    }
    return hash.toHashCode();
}

void DepthStencilStateCache::destroyElement(
    MTL::DepthStencilState* depth_stencil_state) {
    depth_stencil_state->release();
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
