#include "core/hw/tegra_x1/gpu/renderer/metal/depth_stencil_state_cache.hpp"

#include "core/hw/tegra_x1/gpu/renderer/metal/maxwell_to_mtl.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

namespace {

MTL::StencilDescriptor* createStencilDescriptor(const StencilFaceState& face) {
    auto desc = MTL::StencilDescriptor::alloc()->init();
    desc->setStencilCompareFunction(toMtlCompareFunc(face.func));
    desc->setStencilFailureOperation(face.fail_op);
    desc->setDepthFailureOperation(face.depth_fail_op);
    desc->setDepthStencilPassOperation(face.pass_op);
    desc->setReadMask(face.func_mask & 0xff);
    desc->setWriteMask(face.write_mask & 0xff);
    return desc;
}

} // namespace

MTL::DepthStencilState*
DepthStencilStateCache::createHot0(const DepthStencilStateDescriptor& descriptor) {
    auto desc = MTL::DepthStencilDescriptor::alloc()->init();

    // DEBUG: HYDRA_DEBUG_DEPTH_ALWAYS=1 makes every enabled depth test pass
    static const bool debug_depth_always =
        std::getenv("HYDRA_DEBUG_DEPTH_ALWAYS") != nullptr;
    if (descriptor.depth_test_enabled)
        desc->setDepthCompareFunction(
            debug_depth_always
                ? MTL::CompareFunctionAlways
                : toMtlCompareFunc(descriptor.depth_compare_op));

    // Always still writes depth when requested. Suppressing it drops guest
    // depth-copy/resolve passes, including fragment shader depth exports.
    if (descriptor.depth_write_enabled && descriptor.depth_test_enabled)
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

u32 DepthStencilStateCache::hashHot1(
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

// The fields of the hash
bool DepthStencilStateCache::equals(const DepthStencilStateDescriptor& a,
                                    const DepthStencilStateDescriptor& b) {
    if (a.depth_test_enabled != b.depth_test_enabled ||
        a.depth_write_enabled != b.depth_write_enabled ||
        a.depth_compare_op != b.depth_compare_op ||
        a.stencil_enabled != b.stencil_enabled)
        return false;
    if (!a.stencil_enabled)
        return true;

    const auto same_face = [](const StencilFaceState& face_a,
                              const StencilFaceState& face_b) {
        return face_a.func == face_b.func && face_a.fail_op == face_b.fail_op &&
               face_a.depth_fail_op == face_b.depth_fail_op &&
               face_a.pass_op == face_b.pass_op &&
               (face_a.func_mask & 0xff) == (face_b.func_mask & 0xff) &&
               (face_a.write_mask & 0xff) == (face_b.write_mask & 0xff);
    };
    return same_face(a.stencil_front, b.stencil_front) &&
           same_face(a.stencil_back, b.stencil_back);
}

void DepthStencilStateCache::destroyElement(
    MTL::DepthStencilState* depth_stencil_state) {
    depth_stencil_state->release();
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
