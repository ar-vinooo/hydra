#pragma once

#include <cstdlib>

#include <fmt/ranges.h>

#include "common/fmt_helper.hpp"
#include "common/log.hpp"
#include "common/types.hpp"

#define CONFIG_INSTANCE Config::getInstance()

namespace hydra {

enum class InputBackend : u32 {
    AppleGameController,
    Sdl,
};

enum class CpuBackend : u32 {
    AppleHypervisor,
    Dynarmic,
};

enum class GpuRenderer : u32 {
    Null,
    Metal,
};

// The Metal API the renderer encodes with
enum class MetalApi : u32 {
    Metal3,
    Metal4, // Explicit barriers, residency and bindings (macOS 26, iOS 26)
};

enum class ShaderBackend : u32 {
    Msl,
    Air,
};

// The display resolution reported to the guest. A Switch reports 1080p when
// docked and 720p in handheld mode (Console); the others are debug overrides
// that no hardware reports.
enum class Resolution : u32 {
    Auto, // The window size rounded up to a standard resolution
    _720p,
    _1080p,
    _1440p,
    _2160p,
    _4320p,
    AutoExact,
    Custom,
    Console,
};

STRONG_TYPEDEF(CustomResolution, uint2);

// How the presented frame is scaled to the window
enum class UpscalingFilter : u32 {
    Bilinear,
    Nearest,
    MetalFx, // MetalFX spatial upscaling when the window is larger
};

// Frames generated between two guest frames
enum class FrameGeneration : u32 {
    Off,
    Interpolate, // Optical flow interpolation
    MetalFx,     // As Interpolate, with MetalFX for the frames halfway
};

enum class FrameGenerationRateMode : u32 { Variable, Multiple };

enum class AudioBackend : u32 {
    Null,
    Cubeb,
};

enum class SystemLanguage : u32 {
    AmericanEnglish = 0,
    BritishEnglish = 1,
    Japanese = 2,
    French = 3,
    German = 4,
    LatinAmericanSpanish = 5,
    Spanish = 6,
    Italian = 7,
    Dutch = 8,
    CanadianFrench = 9,
    Portuguese = 10,
    Russian = 11,
    Korean = 12,
    TraditionalChinese = 13,
    SimplifiedChinese = 14,
    BrazilianPortuguese = 15,
    Polish = 16,
    Thai = 17,
};

struct LoaderPlugin {
    std::string path;
    std::map<std::string, std::string> options;
};

class Config {
  public:
    static Config& getInstance() {
        static Config g_config;
        return g_config;
    }

    Config();

    void loadDefaults();

    void serialize();
    void deserialize();

    void log();

    // Paths
    std::string_view getAppDataPath() const { return app_data_path; }
    std::string_view getLogsPath() const { return logs_path; }
    std::string_view getPicturesPath() const { return pictures_path; }

    std::string getConfigPath() const {
        return fmt::format("{}/config.toml", app_data_path);
    }

    // Default values
    static std::vector<std::string> getDefaultGamePaths() { return {}; }
    static std::vector<LoaderPlugin> getDefaultLoaderPlugins() { return {}; }
    static std::vector<std::string> getDefaultPatchPaths() { return {}; }
    static InputBackend getDefaultInputBackend() {
#ifdef ZTD_PLATFORM_APPLE
        return InputBackend::AppleGameController;
#else
        return InputBackend::Sdl;
#endif
    }
    static std::vector<std::string> getDefaultInputProfiles() {
        return {"Default", "", "", "", "", "", "", "", "", ""};
    }
    static CpuBackend getDefaultCpuBackend() {
#ifdef HYDRA_HYPERVISOR_ENABLED
        return CpuBackend::AppleHypervisor;
#else
        return CpuBackend::Dynarmic;
#endif
    }
    static GpuRenderer getDefaultGpuRenderer() {
#ifdef ZTD_PLATFORM_APPLE
        return GpuRenderer::Metal;
#else
        return GpuRenderer::Null;
#endif
    }
    static MetalApi getDefaultMetalApi() { return MetalApi::Metal3; }
    static ShaderBackend getDefaultShaderBackend() {
        return ShaderBackend::Msl;
    }
    static Resolution getDefaultDisplayResolution() {
        return Resolution::Console;
    }
    static uint2 getDefaultCustomDisplayResolution() { return {1920, 1080}; }
    static UpscalingFilter getDefaultUpscalingFilter() {
        return UpscalingFilter::Bilinear;
    }
    // 60 as on a Switch; 0 for unlimited
    static u32 getDefaultFrameRateLimit() { return 60; }
    static FrameGeneration getDefaultFrameGeneration() {
        return FrameGeneration::Off;
    }
    static FrameGenerationRateMode getDefaultFrameGenerationRateMode() {
        return FrameGenerationRateMode::Variable;
    }
    static u32 getDefaultFrameGenerationTargetFps() { return 120; }
    static u32 getDefaultFrameGenerationMultiplier() { return 2; }
    // Contrast adaptive sharpening of presented frames, 0 (off) to 100
    static u32 getDefaultSharpening() { return 0; }
    static bool getDefaultMetalHud() { return false; }
    static AudioBackend getDefaultAudioBackend() {
#ifdef HYDRA_CUBEB_ENABLED
        return AudioBackend::Cubeb;
#else
        return AudioBackend::Null;
#endif
    }
    static uuid_t getDefaultUserId() {
        return 0x0; // TODO: INVALID_USER_ID
    }
    static constexpr std::string_view getDefaultDeviceNickname() {
        return "Hydra's Switch";
    }
    static SystemLanguage getDefaultSystemLanguage() {
        return SystemLanguage::AmericanEnglish;
    }
    static constexpr std::string_view getDefaultSystemLocation() {
        return "auto";
    }
    static constexpr std::string_view getDefaultFirmwarePath() { return ""; }
    std::string getDefaultSdCardPath() const {
        return fmt::format("{}/sdmc", app_data_path);
    }
    std::string getDefaultSavePath() const {
        return fmt::format("{}/save", app_data_path);
    }
    std::string getDefaultSysmodulesPath() const {
        return fmt::format("{}/sysmodules", app_data_path);
    }
    static bool getDefaultHandheldMode() { return true; }
    static LogOutput getDefaultLogOutput() { return LogOutput::File; }
    static bool getDefaultLogFsAccess() { return false; }
    static bool getDefaultDebugLogging() { return false; }
    static std::vector<std::string> getDefaultProcessArgs() { return {}; }
    static bool getDefaultRecoverFromSegfault() { return false; }
    static bool getDefaultGdbEnabled() { return false; }
    static u16 getDefaultGdbPort() { return 1234; }
    static bool getDefaultGdbWaitForClient() { return false; }

  private:
    std::string app_data_path;
    std::string logs_path;
    std::string pictures_path; // TODO: remove this

    // Config
    std::vector<std::string> game_paths;
    std::vector<LoaderPlugin> loader_plugins;
    std::vector<std::string> patch_paths;
    InputBackend input_backend;
    std::vector<std::string> input_profiles;
    CpuBackend cpu_backend;
    GpuRenderer gpu_renderer;
    MetalApi metal_api;
    ShaderBackend shader_backend;
    Resolution display_resolution;
    uint2 custom_display_resolution;
    UpscalingFilter upscaling_filter;
    u32 frame_rate_limit;
    FrameGeneration frame_generation;
    FrameGenerationRateMode frame_generation_rate_mode;
    u32 frame_generation_target_fps;
    u32 frame_generation_multiplier;
    u32 sharpening;
    bool metal_hud;
    AudioBackend audio_backend;
    uuid_t user_id;
    std::string device_nickname;
    SystemLanguage system_language;
    std::string system_location;
    std::string firmware_path;
    std::string sd_card_path;
    std::string save_path;
    std::string sysmodules_path;
    bool handheld_mode;
    LogOutput log_output;
    bool log_fs_access;
    bool debug_logging;
    std::vector<std::string> process_args;
    bool recover_from_segfault;
    bool gdb_enabled;
    u16 gdb_port;
    bool gdb_wait_for_client;

  public:
    REF_GETTER(game_paths, getGamePaths);
    REF_GETTER(loader_plugins, getLoaderPlugins);
    REF_GETTER(patch_paths, getPatchPaths);
    REF_GETTER(input_backend, getInputBackend);
    REF_GETTER(input_profiles, getInputProfiles);
    REF_GETTER(cpu_backend, getCpuBackend);
    REF_GETTER(gpu_renderer, getGpuRenderer);
    REF_GETTER(metal_api, getMetalApi);
    REF_GETTER(shader_backend, getShaderBackend);
    REF_GETTER(display_resolution, getDisplayResolution);
    REF_GETTER(custom_display_resolution, getCustomDisplayResolution);
    REF_GETTER(upscaling_filter, getUpscalingFilter);
    REF_GETTER(frame_rate_limit, getFrameRateLimit);
    REF_GETTER(frame_generation, getFrameGeneration);
    REF_GETTER(frame_generation_rate_mode, getFrameGenerationRateMode);
    REF_GETTER(frame_generation_target_fps, getFrameGenerationTargetFps);
    REF_GETTER(frame_generation_multiplier, getFrameGenerationMultiplier);
    REF_GETTER(sharpening, getSharpening);
    REF_GETTER(metal_hud, getMetalHud);
    REF_GETTER(audio_backend, getAudioBackend);
    REF_GETTER(user_id, getUserId);
    REF_GETTER(device_nickname, getDeviceNickname);
    REF_GETTER(system_language, getSystemLanguage);
    REF_GETTER(system_location, getSystemLocation);
    REF_GETTER(firmware_path, getFirmwarePath);
    REF_GETTER(sd_card_path, getSdCardPath);
    REF_GETTER(save_path, getSavePath);
    REF_GETTER(sysmodules_path, getSysmodulesPath);
    REF_GETTER(handheld_mode, getHandheldMode);
    REF_GETTER(log_output, getLogOutput);
    REF_GETTER(log_fs_access, getLogFsAccess);
    REF_GETTER(debug_logging, getDebugLogging);
    REF_GETTER(process_args, getProcessArgs);
    REF_GETTER(recover_from_segfault, getRecoverFromSegfault);
    REF_GETTER(gdb_enabled, getGdbEnabled);
    REF_GETTER(gdb_port, getGdbPort);
    REF_GETTER(gdb_wait_for_client, getGdbWaitForClient);
};

} // namespace hydra

ENABLE_ENUM_FORMATTING_AND_CASTING(hydra, InputBackend, AppleGameController,
                                   "Apple GameController", Sdl, "SDL")
ENABLE_ENUM_FORMATTING_AND_CASTING(hydra, CpuBackend, AppleHypervisor,
                                   "Apple Hypervisor", Dynarmic, "dynarmic")
ENABLE_ENUM_FORMATTING_AND_CASTING(hydra, GpuRenderer, Null, "Null", Metal,
                                   "Metal")
ENABLE_ENUM_FORMATTING_AND_CASTING(hydra, MetalApi, Metal3, "Metal 3", Metal4,
                                   "Metal 4")
ENABLE_ENUM_FORMATTING_AND_CASTING(hydra, ShaderBackend, Msl, "MSL", Air, "AIR")
ENABLE_ENUM_FORMATTING_AND_CASTING(hydra, Resolution, Auto, "window", _720p,
                                   "720p", _1080p, "1080p", _1440p, "1440p",
                                   _2160p, "2160p", _4320p, "4320p", AutoExact,
                                   "Auto exact", Custom, "custom", Console,
                                   "console")
ENABLE_ENUM_FORMATTING_AND_CASTING(hydra, UpscalingFilter, Bilinear,
                                   "Bilinear", Nearest, "Nearest", MetalFx,
                                   "MetalFX")
ENABLE_ENUM_FORMATTING_AND_CASTING(hydra, FrameGeneration, Off, "Off",
                                   Interpolate, "Interpolate", MetalFx,
                                   "MetalFX")
ENABLE_ENUM_FORMATTING_AND_CASTING(hydra, FrameGenerationRateMode, Variable,
                                   "Variable", Multiple, "Multiple")
ENABLE_ENUM_FORMATTING_AND_CASTING(hydra, AudioBackend, Null, "Null", Cubeb,
                                   "Cubeb")
ENABLE_ENUM_FORMATTING_AND_CASTING(
    hydra, SystemLanguage, AmericanEnglish, "American English", BritishEnglish,
    "British English", Japanese, "Japanese", French, "French", German, "German",
    LatinAmericanSpanish, "Latin American Spanish", Spanish, "Spanish", Italian,
    "Italian", Dutch, "Dutch", CanadianFrench, "Canadian French", Portuguese,
    "Portuguese", Russian, "Russian", Korean, "Korean", TraditionalChinese,
    "Traditional Chinese", SimplifiedChinese, "Simplified Chinese",
    BrazilianPortuguese, "Brazilian Portuguese", Polish, "Polish", Thai, "Thai")
ENABLE_ENUM_FORMATTING_AND_CASTING(hydra, LogOutput, None, "none", StdOut,
                                   "stdout", File, "file")
