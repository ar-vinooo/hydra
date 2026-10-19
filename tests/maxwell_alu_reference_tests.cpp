// Runs guest Maxwell instruction cases through decoder -> MSL -> Metal.
// Driven by tests/maxwell_alu_reference.py and tests/maxwell_texture_reference.py:
//   hydra-maxwell-alu-reference-tests [--texture|--sampler] <cases> <results>
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"
#include <iostream>
#include <string>

using hydra::hw::tegra_x1::gpu::renderer::metal::stackAutoRelease;
void runMaxwellAluCaseFile(MTL::Device* device, const char* in_path, const char* out_path);
void runMaxwellTextureCaseFile(MTL::Device* device, const char* in_path, const char* out_path);
void runMaxwellSamplerCaseFile(MTL::Device* device, const char* in_path, const char* out_path);

int main(int argc, char** argv) {
    const bool texture = argc == 4 && std::string(argv[1]) == "--texture";
    const bool sampler = argc == 4 && std::string(argv[1]) == "--sampler";
    if (argc != 3 && !texture && !sampler) {
        std::cerr << "usage: [--texture|--sampler] cases.txt results.txt\n";
        return 2;
    }
    argv += (texture || sampler) ? 1 : 0;
    try {
        NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
        NS_STACK_SCOPED auto device = MTL::CreateSystemDefaultDevice();
        if (sampler) runMaxwellSamplerCaseFile(device, argv[1], argv[2]);
        else if (texture) runMaxwellTextureCaseFile(device, argv[1], argv[2]);
        else runMaxwellAluCaseFile(device, argv[1], argv[2]);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
