// Runs guest Maxwell instruction cases through decoder -> MSL -> Metal.
// Driven by tests/maxwell_alu_reference.py and tests/maxwell_texture_reference.py:
//   hydra-maxwell-alu-reference-tests [--texture|--sampler|--texture-types|--texel-fetch|--gather] <cases> <results>
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"
#include <iostream>
#include <string>

using hydra::hw::tegra_x1::gpu::renderer::metal::stackAutoRelease;
void runMaxwellAluCaseFile(MTL::Device* device, const char* in_path, const char* out_path);
void runMaxwellTextureCaseFile(MTL::Device* device, const char* in_path, const char* out_path);
void runMaxwellSamplerCaseFile(MTL::Device* device, const char* in_path, const char* out_path);
void runMaxwellTextureTypeCaseFile(MTL::Device* device, const char* in_path, const char* out_path);
void runMaxwellTexelFetchCaseFile(MTL::Device* device, const char* in_path, const char* out_path);
void runMaxwellGatherCaseFile(MTL::Device* device, const char* in_path, const char* out_path);

int main(int argc, char** argv) {
    const bool texture = argc == 4 && std::string(argv[1]) == "--texture";
    const bool sampler = argc == 4 && std::string(argv[1]) == "--sampler";
    const bool types = argc == 4 && std::string(argv[1]) == "--texture-types";
    const bool texel = argc == 4 && std::string(argv[1]) == "--texel-fetch";
    const bool gather = argc == 4 && std::string(argv[1]) == "--gather";
    if (argc != 3 && !texture && !sampler && !types && !texel && !gather) {
        std::cerr << "usage: [--texture|--sampler|--texture-types|--texel-fetch|--gather] cases.txt results.txt\n";
        return 2;
    }
    argv += (texture || sampler || types || texel || gather) ? 1 : 0;
    try {
        NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
        NS_STACK_SCOPED auto device = MTL::CreateSystemDefaultDevice();
        if (gather) runMaxwellGatherCaseFile(device, argv[1], argv[2]);
        else if (texel) runMaxwellTexelFetchCaseFile(device, argv[1], argv[2]);
        else if (types) runMaxwellTextureTypeCaseFile(device, argv[1], argv[2]);
        else if (sampler) runMaxwellSamplerCaseFile(device, argv[1], argv[2]);
        else if (texture) runMaxwellTextureCaseFile(device, argv[1], argv[2]);
        else runMaxwellAluCaseFile(device, argv[1], argv[2]);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
