#pragma once

#include "core/hw/tegra_x1/gpu/renderer/metal/const.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <vector>

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

class ShaderLibraryCache;

// Library keys (see ShaderLibraryCache) of the shaders of a pipeline, which
// let a later session build it again
struct PipelineSources {
    std::string vertex;
    std::string fragment; // Empty without a fragment stage
};

// Each pipeline has its own archive so a Metal serializer failure cannot
// poison the other entries. The worker saves misses without skipping draws.
//
// With a recipe directory, the descriptor of every pipeline is recorded as
// well, and the recorded pipelines are built in the background when the
// title starts. A draw takes its pipeline from there, or waits for it if it
// is being built.
//
// With HYDRA_ASYNC_PIPELINES=1, pipelines missing from the caches are compiled
// on workers instead of on the thread that draws (see createPipelineAsync).
class BinaryArchiveCache {
  public:
    // Receives a pipeline compiled on a worker (null and an error on failure)
    using PipelineCallback =
        std::function<void(MTL::RenderPipelineState*, NS::Error*)>;

    explicit BinaryArchiveCache(MTL::Device* device,
                                std::filesystem::path path = {});
    ~BinaryArchiveCache();
    BinaryArchiveCache(const BinaryArchiveCache&) = delete;
    BinaryArchiveCache& operator=(const BinaryArchiveCache&) = delete;

    MTL::RenderPipelineState*
    createPipeline(MTL::RenderPipelineDescriptor* descriptor, NS::Error** error,
                   std::string_view shader_key,
                   const PipelineSources* sources = nullptr);
    // Like createPipeline, but in async mode a pipeline that is neither
    // prewarmed nor archived is compiled on a worker and passed to done.
    // Returns null in that case; done is not called otherwise.
    MTL::RenderPipelineState*
    createPipelineAsync(MTL::RenderPipelineDescriptor* descriptor,
                        NS::Error** error, std::string_view shader_key,
                        const PipelineSources* sources, PipelineCallback done);
    bool isAsync() const { return async; }
    // Record pipelines in recipe_directory and build the recorded ones with
    // shaders from libraries. Only the first call has an effect.
    void startPrewarm(std::filesystem::path recipe_directory,
                      ShaderLibraryCache& libraries);
    // Stop building pipelines (libraries must outlive the prewarm)
    void stopPrewarm();
    // Wait until queued saves and the prewarm finished
    void flush();
    u64 getHits() const { return hits; }
    u64 getMisses() const { return misses; }
    u64 getPrewarmHits() const { return prewarm_hits; }
    u64 getPrewarmed() const { return prewarmed; }
    static std::filesystem::path defaultPath(MTL::Device* device);
    // Archives and recipes unused for this long are deleted
    static constexpr auto MAX_UNUSED_AGE = std::chrono::hours(24 * 30);
    // Least recently used archives are deleted above this size
    static constexpr u64 MAX_ARCHIVE_BYTES = 1024ull * 1024 * 1024;

  private:
    struct Pending {
        // Pipeline to harvest into an archive (null if it has one)
        MTL::RenderPipelineDescriptor* descriptor;
        // Archive to save, or to mark as used without a descriptor
        std::filesystem::path path;
        // Recipe to store (empty if stored)
        std::string recipe;
        // Recipe file to save or to mark as used
        std::filesystem::path recipe_path;
    };
    struct Building {
        bool done{false};
        // Pipeline built for a draw, shared with draws that waited for it
        MTL::RenderPipelineState* pipeline{nullptr};
        ~Building() {
            if (pipeline)
                pipeline->release();
        }
    };
    struct AsyncJob {
        MTL::RenderPipelineDescriptor* descriptor{nullptr}; // Retained
        std::string shader_key;
        std::optional<PipelineSources> sources;
        PipelineCallback done;
    };

    void run();
    void runAsync();
    void trimArchives();
    void prewarm(std::vector<std::filesystem::path> recipes,
                 ShaderLibraryCache& libraries);
    MTL::RenderPipelineState* prewarmRecipe(const std::filesystem::path& path,
                                            ShaderLibraryCache& libraries);
    // Build from the archive at entry, or compile and queue the archive
    MTL::RenderPipelineState* build(MTL::RenderPipelineDescriptor* descriptor,
                                    NS::Error** error,
                                    const std::filesystem::path& entry,
                                    bool& archive_hit);
    // Take a prewarmed pipeline, wait for one being built, or build it
    MTL::RenderPipelineState* obtain(MTL::RenderPipelineDescriptor* descriptor,
                                     NS::Error** error, const std::string& key);
    void queueRecipe(const std::string& key, std::string_view shader_key,
                     const PipelineSources* sources,
                     const std::vector<u64>& fields);

    MTL::Device* device;
    std::filesystem::path path;
    bool enabled{false};
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<Pending> pending;
    std::thread worker;
    bool stopping{false};
    bool busy{false};
    std::atomic<u64> hits{0};
    std::atomic<u64> misses{0};

    // Async compilation
    bool async{false};
    bool async_stopping{false};
    std::deque<AsyncJob> async_jobs;
    std::condition_variable async_condition;
    std::vector<std::thread> async_workers;

    // Prewarm
    std::filesystem::path recipe_directory;
    std::set<std::string> recipe_keys; // Stored or queued
    std::set<std::string> used_recipes;
    std::map<std::string, MTL::RenderPipelineState*> prewarmed_pipelines;
    std::map<std::string, std::shared_ptr<Building>> building;
    std::set<std::string> created; // Built for draws
    std::condition_variable built;
    std::vector<std::thread> prewarmers;
    usize prewarm_pending{0};
    bool prewarm_stopping{false};
    std::atomic<u64> prewarm_hits{0};
    std::atomic<u64> prewarmed{0};
};

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
