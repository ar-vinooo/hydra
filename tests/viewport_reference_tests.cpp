// ThreeD::getViewport / getScissor driven through the viewport, scissor and
// window origin registers; the Metal viewport and scissor they build are
// printed for tests/viewport_reference.py.
//
// Input: "<scale x y z hex float bits> <offset x y z> <swizzle word> <lower left>
//         <z clip> <surface height> <scissor enabled> <min x> <max x> <min y> <max y>"
// Output: "<originX> <originY> <width> <height> <znear> <zfar>
//          <scissor x> <scissor y> <scissor w> <scissor h>"
#include "core/hw/tegra_x1/gpu/engines/3d.hpp"
#include "core/hw/tegra_x1/gpu/gpu.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace hydra;
using namespace hydra::hw::tegra_x1::gpu;
using hydra::hw::tegra_x1::gpu::renderer::metal::stackAutoRelease;

namespace hydra::hw::tegra_x1::gpu::engines {
struct ThreeDTestAccess {
    static MTL::Viewport viewport(ThreeD& engine, u32 index) {
        return engine.getViewport(index);
    }
    static MTL::ScissorRect scissor(ThreeD& engine, u32 index) {
        return engine.getScissor(index);
    }
};
} // namespace hydra::hw::tegra_x1::gpu::engines

int main(int argc, char** argv) {
    using engines::ThreeDTestAccess;
    using engines::Regs3D;
    try {
        if (argc != 3)
            throw std::runtime_error("usage: cases.txt results.txt");
        setenv("HYDRA_HOT_RELOAD", "0", 1);
        NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
        Gpu gpu;
        auto& engine = gpu.get3DEngine();
        std::ifstream input(argv[1]);
        std::ofstream output(argv[2]);
        std::string line;
        const auto word = [](auto offset) { return static_cast<u32>(offset / 4); };
        while (std::getline(input, line)) {
            std::istringstream row(line);
            u32 v[6], swizzle, lower_left, z_clip, height, enabled, min_x, max_x, min_y, max_y;
            for (auto& value : v)
                row >> std::hex >> value;
            row >> std::hex >> swizzle >> lower_left >> z_clip >> height >> enabled >>
                min_x >> max_x >> min_y >> max_y;
            const u32 transform = word(offsetof(Regs3D, viewport_transforms));
            for (u32 i = 0; i < 6; ++i)
                engine.method(transform + i, v[i]);
            engine.method(transform + 6, swizzle);
            engine.method(word(offsetof(Regs3D, window_origin_flags)), lower_left);
            engine.method(word(offsetof(Regs3D, viewport_z_clip)), z_clip);
            engine.method(word(offsetof(Regs3D, screen_scissor)) + 1, height << 16);
            const u32 scissor = word(offsetof(Regs3D, scissors));
            engine.method(scissor, enabled);
            engine.method(scissor + 1, min_x | (max_x << 16));
            engine.method(scissor + 2, min_y | (max_y << 16));
            const auto viewport = ThreeDTestAccess::viewport(engine, 0);
            const auto rect = ThreeDTestAccess::scissor(engine, 0);
            char text[512];
            std::snprintf(text, sizeof(text), "%.9g %.9g %.9g %.9g %.9g %.9g %lu %lu %lu %lu\n",
                          viewport.originX, viewport.originY, viewport.width,
                          viewport.height, viewport.znear, viewport.zfar,
                          static_cast<unsigned long>(rect.x), static_cast<unsigned long>(rect.y),
                          static_cast<unsigned long>(rect.width), static_cast<unsigned long>(rect.height));
            output << text;
        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
