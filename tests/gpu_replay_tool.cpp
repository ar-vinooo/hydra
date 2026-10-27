// Replays a GPU capture (HYDRA_GPU_CAPTURE, see capture/draw_capture.hpp) on a
// fresh renderer, without the guest. Exit status 0 means every texture the
// capture wrote has the captured pixels.
//   hydra-gpu-replay <capture.hcap> [--dump <directory>]
// --dump writes the replayed contents of each written texture as
// texture_<n>_<layer*levels+level>.bin (tightly packed host rows).
// --commands N replays only the first N commands (compare N values to find the
// first command whose result is wrong) and --png <directory> writes every
// written colour texture as PNG (8-bit as is; float and packed formats are
// tone mapped for viewing). --png-every K writes them after every K commands as
// step_<commands>_tex<n>_<format>.png (images that did not change are skipped)
// and index.txt lists which commands ran in each step.
// --watch <texture>,<x0>,<y0>,<x1>,<y1> reports every command that changes that
// region of a written texture (give --commands to stop early).
// --inspect <command> prints a draw (pipeline, shaders, bound textures and
// samplers) and writes the textures it samples as inspect_<command>_tex<n>.png
// (8-bit colour and BC1-BC7 when bcdec is available).
#include "core/hw/tegra_x1/gpu/capture/draw_capture.hpp"
#include "core/hw/tegra_x1/gpu/gpu.hpp"
#include "core/hw/tegra_x1/gpu/memory_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// Block-compressed textures can be shown when bcdec (downloaded by
// tests/run_python_references.sh) is present
#if __has_include("/private/tmp/hydra-bcdec/bcdec.h")
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#define BCDEC_IMPLEMENTATION
#include "/private/tmp/hydra-bcdec/bcdec.h"
#pragma clang diagnostic pop
#define HAVE_BCDEC 1
#endif

using namespace hydra::hw::tegra_x1::gpu;

namespace {

float halfToFloat(uint16_t h) {
    const uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 31,
                   mantissa = h & 1023;
    float value;
    if (exponent == 0)
        value = std::ldexp(static_cast<float>(mantissa), -24);
    else if (exponent == 31)
        value = mantissa ? 0.0f : 65504.0f;
    else
        value = std::ldexp(static_cast<float>(mantissa | 1024),
                           static_cast<int>(exponent) - 25);
    return sign ? -value : value;
}

// An unsigned 11 or 10 bit float (5 exponent bits)
float smallFloat(uint32_t bits, int mantissa_bits) {
    const uint32_t exponent = bits >> mantissa_bits;
    const uint32_t mantissa = bits & ((1u << mantissa_bits) - 1);
    if (exponent == 0)
        return std::ldexp(static_cast<float>(mantissa), -14 - mantissa_bits);
    if (exponent == 31)
        return 65504.0f;
    return std::ldexp(1.0f + static_cast<float>(mantissa) /
                                 static_cast<float>(1u << mantissa_bits),
                      static_cast<int>(exponent) - 15);
}

unsigned char display(float value) {
    if (!(value > 0.0f))
        return 0;
    value = value / (1.0f + value); // Reinhard: HDR values stay visible
    return static_cast<unsigned char>(
        std::lround(std::clamp(std::pow(value * 2.0f, 1.0f / 2.2f), 0.0f, 1.0f) * 255.0f));
}

// RGBA8 for viewing, false if the format is not a colour format handled here
bool toRgba8(const std::string& format, const std::vector<unsigned char>& in,
             unsigned width, unsigned height, std::vector<unsigned char>& out) {
    const size_t pixels = size_t(width) * height;
    out.assign(pixels * 4, 255);
    const auto word = [&](size_t i, size_t size) {
        uint32_t value = 0;
        std::memcpy(&value, in.data() + i * size, size);
        return value;
    };
    if (format.starts_with("rgba8") || format.starts_with("bgra8")) {
        if (in.size() < pixels * 4)
            return false;
        std::memcpy(out.data(), in.data(), pixels * 4);
        if (format.starts_with("bgra8"))
            for (size_t p = 0; p < pixels; p++)
                std::swap(out[p * 4], out[p * 4 + 2]);
    } else if (format == "rgba16float") {
        if (in.size() < pixels * 8)
            return false;
        for (size_t p = 0; p < pixels; p++)
            for (size_t c = 0; c < 3; c++) {
                uint16_t h;
                std::memcpy(&h, in.data() + p * 8 + c * 2, 2);
                out[p * 4 + c] = display(halfToFloat(h));
            }
    } else if (format == "rg11b10float") {
        if (in.size() < pixels * 4)
            return false;
        for (size_t p = 0; p < pixels; p++) {
            const auto v = word(p, 4);
            out[p * 4] = display(smallFloat(v & 0x7ff, 6));
            out[p * 4 + 1] = display(smallFloat((v >> 11) & 0x7ff, 6));
            out[p * 4 + 2] = display(smallFloat((v >> 22) & 0x3ff, 5));
        }
    } else if (format == "rgb10a2unorm") {
        if (in.size() < pixels * 4)
            return false;
        for (size_t p = 0; p < pixels; p++) {
            const auto v = word(p, 4);
            for (size_t c = 0; c < 3; c++)
                out[p * 4 + c] = static_cast<unsigned char>(((v >> (c * 10)) & 1023) * 255 / 1023);
        }
    } else if (format == "r8unorm") {
        if (in.size() < pixels)
            return false;
        for (size_t p = 0; p < pixels; p++)
            out[p * 4] = out[p * 4 + 1] = out[p * 4 + 2] = in[p];
    } else if (format == "r16float" || format == "r32float" || format == "z32float") {
        const size_t size = format == "r16float" ? 2 : 4;
        if (in.size() < pixels * size)
            return false;
        for (size_t p = 0; p < pixels; p++) {
            float v;
            if (size == 2) {
                uint16_t h;
                std::memcpy(&h, in.data() + p * 2, 2);
                v = halfToFloat(h);
            } else {
                std::memcpy(&v, in.data() + p * 4, 4);
            }
            const auto g = format == "z32float"
                               ? static_cast<unsigned char>(std::clamp(std::pow(std::clamp(v, 0.0f, 1.0f), 0.25f), 0.0f, 1.0f) * 255.0f)
                               : display(v);
            out[p * 4] = out[p * 4 + 1] = out[p * 4 + 2] = g;
        }
    } else {
        return false;
    }
    return true;
}

// Decodes the first mip of a block-compressed texture to RGBA8
bool decodeCompressed(const std::string& format, const std::vector<unsigned char>& in,
                      unsigned width, unsigned height, std::vector<unsigned char>& out) {
#ifdef HAVE_BCDEC
    int kind = 0;
    size_t block_bytes = 16;
    if (format.starts_with("bc1")) { kind = 1; block_bytes = 8; }
    else if (format.starts_with("bc2")) kind = 2;
    else if (format.starts_with("bc3")) kind = 3;
    else if (format.starts_with("bc4")) { kind = 4; block_bytes = 8; }
    else if (format.starts_with("bc5")) kind = 5;
    else if (format.starts_with("bc7")) kind = 7;
    else return false;
    const unsigned bw = (width + 3) / 4, bh = (height + 3) / 4;
    if (in.size() < size_t(bw) * bh * block_bytes)
        return false;
    out.assign(size_t(width) * height * 4, 255);
    unsigned char block[4 * 4 * 4];
    for (unsigned by = 0; by < bh; by++)
        for (unsigned bx = 0; bx < bw; bx++) {
            const unsigned char* src = in.data() + (size_t(by) * bw + bx) * block_bytes;
            switch (kind) {
            case 1: bcdec_bc1(src, block, 16); break;
            case 2: bcdec_bc2(src, block, 16); break;
            case 3: bcdec_bc3(src, block, 16); break;
            case 4: {
                unsigned char r[16];
                bcdec_bc4(src, r, 4);
                for (int i = 0; i < 16; i++) {
                    block[i * 4] = block[i * 4 + 1] = block[i * 4 + 2] = r[i];
                    block[i * 4 + 3] = 255;
                }
                break;
            }
            case 5: {
                unsigned char rg[32];
                bcdec_bc5(src, rg, 8);
                for (int i = 0; i < 16; i++) {
                    block[i * 4] = rg[(i / 4) * 8 + (i % 4) * 2];
                    block[i * 4 + 1] = rg[(i / 4) * 8 + (i % 4) * 2 + 1];
                    block[i * 4 + 2] = 0;
                    block[i * 4 + 3] = 255;
                }
                break;
            }
            default: bcdec_bc7(src, block, 16); break;
            }
            for (unsigned y = 0; y < 4 && by * 4 + y < height; y++)
                for (unsigned x = 0; x < 4 && bx * 4 + x < width; x++)
                    std::memcpy(&out[(size_t(by * 4 + y) * width + bx * 4 + x) * 4],
                                &block[(y * 4 + x) * 4], 4);
        }
    return true;
#else
    (void)format; (void)in; (void)width; (void)height; (void)out;
    return false;
#endif
}

// One mip of a block-linear texture from its guest bytes, packed like the host
// upload (the same conversion as TextureCache::decodeTexture)
std::vector<unsigned char> decodeGuestLevel(const renderer::TextureDescriptor& d,
                                            const std::vector<unsigned char>& guest,
                                            unsigned level) {
    const auto dims = d.getLevelDimensions(level);
    const auto bl = d.getLevelBlockSizeLog2(level);
    const uint32_t stride = renderer::getTextureFormatStride(d.format, static_cast<uint32_t>(dims.width));
    const uint32_t rows = renderer::getTextureFormatRows(d.format, static_cast<uint32_t>(dims.height));
    std::vector<unsigned char> out(size_t(stride) * rows * static_cast<uint32_t>(dims.depth));
    const uint32_t offset = d.getLevelOffset(level);
    if (offset + d.getLevelSize(level) > guest.size())
        return {};
    convertBlockLinearToLinear(
        ((stride + (64u << static_cast<uint32_t>(bl.width)) - 1) / (64u << static_cast<uint32_t>(bl.width))) *
            (64u << static_cast<uint32_t>(bl.width)),
        rows, static_cast<uint32_t>(dims.depth), static_cast<uint32_t>(bl.height),
        static_cast<uint32_t>(bl.depth), guest.data() + offset,
        [&](const uint8_t* in_gob, uint32_t gob_x, uint32_t gob_y, uint32_t gob_z) {
            const uint32_t x = gob_x * 64;
            if (x >= stride)
                return;
            for (uint32_t local_y = 0; local_y < 8; local_y++) {
                const uint32_t y = gob_y * 8 + local_y;
                if (y >= rows)
                    break;
                std::memcpy(out.data() + size_t(gob_z) * rows * stride + size_t(y) * stride + x,
                            in_gob + size_t(local_y) * 64, std::min(64u, stride - x));
            }
        });
    return out;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: hydra-gpu-replay <capture.hcap> [--dump <dir>] "
                     "[--commands N] [--png <dir>]\n";
        return 2;
    }
    std::string dump_dir, png_dir;
    long watch[5] = {-1, 0, 0, 0, 0};
    long inspect = -1;
    size_t first_step = 0;
    size_t command_limit = std::numeric_limits<size_t>::max(), png_every = 0;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string option = argv[i];
        if (option == "--dump")
            dump_dir = argv[i + 1];
        else if (option == "--png")
            png_dir = argv[i + 1];
        else if (option == "--commands")
            command_limit = std::stoull(argv[i + 1]);
        else if (option == "--png-every")
            png_every = std::stoull(argv[i + 1]);
        else if (option == "--from")
            first_step = std::stoull(argv[i + 1]);
        else if (option == "--inspect")
            inspect = std::stol(argv[i + 1]);
        else if (option == "--watch")
            std::sscanf(argv[i + 1], "%ld,%ld,%ld,%ld,%ld", &watch[0], &watch[1],
                        &watch[2], &watch[3], &watch[4]);
    }
    try {
        const auto file = capture::CaptureFile::load(argv[1]);
        if (inspect >= 0) {
            if (size_t(inspect) >= file.commands.size() ||
                file.commands[size_t(inspect)].kind != capture::CommandRecord::Kind::Draw) {
                std::cerr << "command " << inspect << " is not a draw\n";
                return 1;
            }
            const auto& d = file.commands[size_t(inspect)].draw;
            const auto& pipeline = file.pipelines[size_t(d.pipeline)];
            std::cout << "draw " << inspect << ": pipeline " << d.pipeline << " pass "
                      << d.pass << " primitive " << d.primitive << " count " << d.count
                      << " first " << d.first << " base_vertex " << d.base_vertex
                      << " instances " << d.instance_count << "\n";
            for (int stage_type = 0; stage_type < 2; stage_type++) {
                const auto id = pipeline.shaders[stage_type];
                if (id < 0)
                    continue;
                const auto& sh = file.shaders[size_t(id)].descriptor;
                std::cout << (stage_type == 0 ? "vertex" : "fragment") << " shader " << id
                          << ": " << sh.code.size() << " bytes MSL";
                for (const auto& u : sh.resource_mapping.unsupported)
                    std::cout << " [unsupported: " << u << "]";
                std::cout << '\n';
                if (!png_dir.empty()) {
                    std::ofstream msl(fmt::format("{}/inspect_{}_{}.metal", png_dir, inspect,
                                                  stage_type == 0 ? "vertex" : "fragment"));
                    msl.write(reinterpret_cast<const char*>(sh.code.data()),
                              static_cast<std::streamsize>(sh.code.size()));
                }
            }
            for (size_t st = 0; st < d.stages.size(); st++)
                for (const auto& binding : d.stages[st].textures) {
                    if (binding.view < 0) {
                        std::cout << "  stage " << st << " slot " << binding.index << ": none\n";
                        continue;
                    }
                    const auto& view = file.views[size_t(binding.view)];
                    const auto& t = file.textures[size_t(view.texture)];
                    const auto name = fmt::format("{}", static_cast<renderer::TextureFormat>(t.format));
                    std::cout << "  stage " << st << " slot " << binding.index << ": texture "
                              << view.texture << " " << name << " " << t.width << "x" << t.height
                              << " levels " << t.levels << " layers " << t.layers << " type "
                              << t.type << " view levels " << view.level_begin << ".."
                              << view.level_end << " layers " << view.layer_begin << ".."
                              << view.layer_end << " sampler " << binding.sampler;
                    if (binding.sampler >= 0 &&
                        file.samplers[size_t(binding.sampler)].raw.size() ==
                            sizeof(renderer::SamplerDescriptor)) {
                        renderer::SamplerDescriptor sd;
                        std::memcpy(&sd, file.samplers[size_t(binding.sampler)].raw.data(), sizeof(sd));
                        std::cout << " [lod " << sd.min_lod << ".." << sd.max_lod << " bias "
                                  << sd.lod_bias << " aniso " << sd.max_anisotropy << " mip "
                                  << static_cast<int>(sd.mip_filter) << "]";
                    }
                    std::cout << (t.initial.empty() ? " (no contents)" : "") << '\n';
                    if (!t.guest.empty() && t.type == 2 && t.layers == 1 && !t.is_linear) {
                        const renderer::TextureDescriptor desc(
                            0, MTL::TextureType2D, static_cast<renderer::TextureFormat>(t.format),
                            false, 0, t.width, t.height, t.depth, t.levels, t.layers,
                            t.block_width_gobs_log2, t.block_height_gobs_log2,
                            t.block_depth_gobs_log2, t.layer_size);
                        std::cout << "    guest memory vs host, per mip:";
                        for (unsigned level = 0; level < t.levels && level < t.initial.size(); level++) {
                            const auto decoded = decodeGuestLevel(desc, t.guest, level);
                            if (decoded.empty()) {
                                std::cout << " " << level << ":outside";
                                continue;
                            }
                            const bool same = decoded == t.initial[level];
                            std::cout << " " << level << (same ? ":same" : ":DIFFERENT");
                            if (!same && !png_dir.empty()) {
                                const unsigned w = std::max(1u, t.width >> level);
                                const unsigned h = std::max(1u, t.height >> level);
                                std::vector<unsigned char> rgba;
                                if (toRgba8(name, decoded, w, h, rgba) ||
                                    decodeCompressed(name, decoded, w, h, rgba)) {
                                    const auto path = fmt::format(
                                        "{}/inspect_{}_tex{}_slot{}_GUEST_mip{}.png", png_dir,
                                        inspect, view.texture, binding.index, level);
                                    stbi_write_png(path.c_str(), static_cast<int>(w),
                                                   static_cast<int>(h), 4, rgba.data(),
                                                   static_cast<int>(w) * 4);
                                }
                            }
                        }
                        std::cout << '\n';
                    }
                    if (t.type == 9 && !t.initial.empty()) {
                        // Buffer texture: show whether its values are plausible
                        const auto& bytes = t.initial[0];
                        std::cout << "    " << bytes.size() << " bytes:";
                        if (name == "rg16float") {
                            size_t bad = 0, zero = 0, total = bytes.size() / 2;
                            float lo = 1e30f, hi = -1e30f;
                            for (size_t k = 0; k + 1 < bytes.size(); k += 2) {
                                uint16_t h;
                                std::memcpy(&h, &bytes[k], 2);
                                const float v = halfToFloat(h);
                                const bool nan = ((h >> 10) & 31) == 31;
                                if (nan || std::fabs(v) > 64.0f) bad++;
                                if (h == 0) zero++;
                                if (!nan) { lo = std::min(lo, v); hi = std::max(hi, v); }
                            }
                            std::cout << " halves " << total << ", zero " << zero
                                      << ", |x|>64 or NaN " << bad << ", range [" << lo
                                      << ", " << hi << "]; first:";
                            for (size_t k = 0; k < 16 && k * 2 + 1 < bytes.size(); k++) {
                                uint16_t h;
                                std::memcpy(&h, &bytes[k * 2], 2);
                                std::cout << ' ' << halfToFloat(h);
                            }
                        } else {
                            size_t zero = 0;
                            for (const auto byte : bytes) zero += byte == 0;
                            std::cout << " zero bytes " << zero << ", first:";
                            for (size_t k = 0; k < 16 && k < bytes.size(); k++)
                                std::cout << ' ' << int(bytes[k]);
                        }
                        std::cout << '\n';
                    }
                    if (png_dir.empty() || t.initial.empty() || t.type != 2)
                        continue;
                    // Every mip level the capture holds
                    for (size_t level = 0; level < t.initial.size() && level < t.levels; level++) {
                        const unsigned w = std::max(1u, t.width >> level);
                        const unsigned h = std::max(1u, t.height >> level);
                        std::vector<unsigned char> rgba;
                        if (!toRgba8(name, t.initial[level], w, h, rgba) &&
                            !decodeCompressed(name, t.initial[level], w, h, rgba))
                            break;
                        const auto path = fmt::format(
                            "{}/inspect_{}_tex{}_slot{}_{}_mip{}.png", png_dir, inspect,
                            view.texture, binding.index, name, level);
                        stbi_write_png(path.c_str(), static_cast<int>(w), static_cast<int>(h), 4,
                                       rgba.data(), static_cast<int>(w) * 4);
                    }
                }
            return 0;
        }
        Gpu gpu;
        std::ofstream index;
        std::map<size_t, size_t> last_hash;
        size_t previous_step = 0;
        const auto on_step = [&](size_t executed,
                                 const std::vector<capture::Contents>& contents) {
            if (!index.is_open() && !png_dir.empty())
                index.open(png_dir + "/index.txt");
            index << "step " << executed << ": commands " << previous_step
                  << ".." << executed - 1 << '\n';
            for (size_t c = previous_step; c < executed && c < file.commands.size(); c++) {
                const auto& command = file.commands[c];
                if (command.kind == capture::CommandRecord::Kind::Clear)
                    index << "  command " << c << ": clear pass " << command.clear.pass << '\n';
                else if (command.kind == capture::CommandRecord::Kind::Draw)
                    index << "  command " << c << ": draw pipeline " << command.draw.pipeline
                          << " pass " << command.draw.pass << " count " << command.draw.count
                          << " instances " << command.draw.instance_count << '\n';
                else if (command.kind == capture::CommandRecord::Kind::Copy)
                    index << "  command " << c << ": copy view "
                          << command.copy.src << " -> " << command.copy.dst
                          << '\n';
                else if (command.kind == capture::CommandRecord::Kind::Compute)
                    index << "  command " << c << ": compute shader "
                          << command.compute.shader << '\n';
            }
            if (watch[0] >= 0 && size_t(watch[0]) < contents.size() &&
                !contents[size_t(watch[0])].empty()) {
                const auto& blob = contents[size_t(watch[0])][0];
                const auto& t = file.textures[size_t(watch[0])];
                const size_t bpp = blob.size() / (size_t(t.width) * t.height);
                size_t hash = 1469598103934665603ull;
                for (long y = watch[2]; y < watch[4]; y++)
                    for (long x = watch[1]; x < watch[3]; x++)
                        for (size_t b = 0; b < bpp; b++)
                            hash = (hash ^ blob[(size_t(y) * t.width + size_t(x)) * bpp + b]) * 1099511628211ull;
                static size_t last_watch = 0;
                static bool first = true;
                if (!first && hash != last_watch)
                    std::cout << "region changed by command(s) " << previous_step << ".."
                              << executed - 1 << '\n';
                first = false;
                last_watch = hash;
            }
            previous_step = executed;
            for (size_t i = 0; i < contents.size() && !png_dir.empty(); i++) {
                const auto& t = file.textures[i];
                if (contents[i].empty() || t.type != 2)
                    continue;
                const auto name = fmt::format(
                    "{}", static_cast<renderer::TextureFormat>(t.format));
                std::vector<unsigned char> rgba;
                if (!toRgba8(name, contents[i][0], t.width, t.height, rgba))
                    continue;
                const size_t hash = std::hash<std::string_view>{}(std::string_view(
                    reinterpret_cast<const char*>(rgba.data()), rgba.size()));
                if (last_hash.count(i) && last_hash[i] == hash)
                    continue;
                last_hash[i] = hash;
                const auto path = fmt::format("{}/step_{:05}_tex{}_{}.png", png_dir,
                                              executed, i, name);
                stbi_write_png(path.c_str(), static_cast<int>(t.width),
                               static_cast<int>(t.height), 4, rgba.data(),
                               static_cast<int>(t.width) * 4);
                index << "  image " << path.substr(path.rfind('/') + 1) << " ("
                      << t.width << "x" << t.height << ")\n";
            }
        };
        const auto result = capture::replay(
            gpu.getRenderer(), file, command_limit,
            png_every != 0 ? png_every : 0, on_step, first_step);
        if (!result.ok) {
            std::cerr << "replay failed: " << result.error << '\n';
            return 1;
        }
        for (const auto& note : file.notes)
            std::cout << "note: " << note << '\n';
        for (size_t i = 0; i < file.textures.size(); i++) {
            const auto& t = file.textures[i];
            std::cout << "texture " << i << ": type " << t.type << " format "
                      << t.format << " " << t.width << "x" << t.height << "x"
                      << t.depth << " levels " << t.levels << " layers "
                      << t.layers << (t.final_contents.empty() ? "" : " (written)")
                      << '\n';
        }
        for (size_t i = 0; i < file.commands.size(); i++) {
            const auto& c = file.commands[i];
            if (c.kind == capture::CommandRecord::Kind::Clear) {
                std::cout << "command " << i << ": clear pass " << c.clear.pass
                          << (c.clear.has_color ? " color" : "")
                          << (c.clear.has_depth ? " depth" : "")
                          << (c.clear.has_stencil ? " stencil" : "") << '\n';
                continue;
            }
            if (c.kind == capture::CommandRecord::Kind::Copy) {
                std::cout << "command " << i << ": copy view " << c.copy.src
                          << " -> " << c.copy.dst << '\n';
                continue;
            }
            if (c.kind == capture::CommandRecord::Kind::Compute) {
                std::cout << "command " << i << ": compute shader "
                          << c.compute.shader << " storage "
                          << c.compute.storage.size() << " images "
                          << c.compute.images.size() << '\n';
                continue;
            }
            const auto& d = c.draw;
            std::cout << "command " << i << ": draw pipeline " << d.pipeline
                      << " pass " << d.pass << " primitive " << d.primitive
                      << " count " << d.count << " first " << d.first
                      << " instances " << d.instance_count
                      << (d.index_type != 0 ? " indexed" : "");
            size_t textures = 0;
            for (const auto& stage : d.stages)
                textures += stage.textures.size();
            std::cout << " textures " << textures << '\n';
        }
        for (size_t i = 0; i < file.shaders.size(); i++) {
            const auto& sh = file.shaders[i].descriptor;
            std::cout << "shader " << i << ": type " << static_cast<int>(sh.type)
                      << " " << sh.code.size() << " bytes MSL";
            for (const auto& u : sh.resource_mapping.unsupported)
                std::cout << " [unsupported: " << u << "]";
            std::cout << '\n';
        }
        std::cout << file.commands.size() << " commands, " << file.textures.size()
                  << " textures, " << file.buffers.size() << " buffers\n";
        for (size_t i = 0; i < file.textures.size(); i++) {
            if (file.textures[i].final_contents.empty())
                continue;
            const bool same =
                result.contents[i] == file.textures[i].final_contents;
            std::cout << "texture " << i << ": " << (same ? "same" : "DIFFERENT")
                      << " (" << result.contents[i].size() << " subresources)\n";
        }
        if (!png_dir.empty() && png_every == 0) {
            for (size_t i = 0; i < result.contents.size(); i++) {
                const auto& t = file.textures[i];
                if (result.contents[i].empty() || t.type != 2)
                    continue;
                const auto name = fmt::format(
                    "{}", static_cast<renderer::TextureFormat>(t.format));
                std::vector<unsigned char> rgba;
                if (!toRgba8(name, result.contents[i][0], t.width, t.height, rgba))
                    continue;
                const auto path = png_dir + "/texture_" + std::to_string(i) + ".png";
                stbi_write_png(path.c_str(), static_cast<int>(t.width),
                               static_cast<int>(t.height), 4, rgba.data(),
                               static_cast<int>(t.width) * 4);
                std::cout << "wrote " << path << " (" << name << ")\n";
            }
        }
        if (!dump_dir.empty()) {
            for (size_t i = 0; i < result.contents.size(); i++)
                for (size_t sub = 0; sub < result.contents[i].size(); sub++) {
                    std::ofstream out(dump_dir + "/texture_" +
                                          std::to_string(i) + "_" +
                                          std::to_string(sub) + ".bin",
                                      std::ios::binary);
                    out.write(reinterpret_cast<const char*>(result.contents[i][sub].data()),
                              static_cast<std::streamsize>(result.contents[i][sub].size()));
                }
        }
        std::cout << (result.matches ? "MATCH" : "MISMATCH: " + result.mismatch)
                  << '\n';
        return result.matches ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
