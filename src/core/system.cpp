#include "core/system.hpp"
#include "core/hw/tegra_x1/gpu/renderer/draw_queue.hpp"

#include "common/frame_generation_timing.hpp"

#include "core/hw/tegra_x1/gpu/display_stats.hpp"

#include "core/hw/tegra_x1/gpu/capture/draw_capture.hpp"
#include "core/hw/tegra_x1/gpu/frame_tracer.hpp"
#include "core/hw/tegra_x1/gpu/visual_diagnostics.hpp"
#include "core/hw/tegra_x1/gpu/perf_stats.hpp"

#include <execinfo.h>
#include <csignal>
#ifdef __APPLE__
#include <sys/ucontext.h>
#endif
#include <fmt/chrono.h>
#include <unistd.h>
#include <stb_image_write.h>
#ifdef ZTD_PLATFORM_APPLE
#include <mach/mach.h>
#endif

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wold-style-cast"

#include <cstddef>
#include <hatch/hatch.hpp>

#pragma GCC diagnostic pop

#include "core/audio/null/core.hpp"
#include "core/horizon/applets/album/const.hpp"
#include "core/horizon/applets/const.hpp"
#include "core/horizon/applets/controller/const.hpp"
#include "core/horizon/applets/error/const.hpp"
#include "core/horizon/applets/mii_edit/const.hpp"
#include "core/horizon/applets/software_keyboard/const.hpp"
#include "core/horizon/loader/nca_loader.hpp"
#include "core/horizon/loader/nro_loader.hpp"
#include "core/horizon/loader/nso_loader.hpp"
#include "core/horizon/services/am/internal/library_applet_controller.hpp"
#include "core/hw/tegra_x1/cpu/dynarmic/cpu.hpp"
#include "core/hw/tegra_x1/cpu/mmu.hpp"
#include "core/hw/tegra_x1/cpu/thread.hpp"
#include "core/hw/tegra_x1/gpu/renderer/buffer_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/surface_compositor.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture_view.hpp"
#include "core/input/device_manager.hpp"

#ifdef HYDRA_HYPERVISOR_ENABLED
#include "core/hw/tegra_x1/cpu/hypervisor/cpu.hpp"
#endif

#ifdef HYDRA_CUBEB_ENABLED
#include "core/audio/cubeb/core.hpp"
#endif

namespace hydra {

namespace {

constexpr auto STARTUP_MOVIE_FADE_IN_DURATION = 100ms;
constexpr auto STARTUP_MOVIE_BREAK_AFTER_FADE_IN_DURATION = 200ms;

hw::tegra_x1::cpu::ICpu* createCpu() {
    switch (CONFIG_INSTANCE.getCpuBackend()) {
    case CpuBackend::AppleHypervisor:
#ifdef HYDRA_HYPERVISOR_ENABLED
        return new hw::tegra_x1::cpu::hypervisor::Cpu();
#else
        LOG_FATAL(Other, "Apple Hypervisor not supported");
#endif
    case CpuBackend::Dynarmic:
        return new hw::tegra_x1::cpu::dynarmic::Cpu();
    default:
        // TODO: return an error instead
        LOG_FATAL(Other, "Unknown CPU backend {}",
                  CONFIG_INSTANCE.getCpuBackend());
    }
}

audio::ICore* createAudioCore() {
    switch (CONFIG_INSTANCE.getAudioBackend()) {
    case AudioBackend::Null:
        return new audio::null::Core();
    case AudioBackend::Cubeb:
#ifdef HYDRA_CUBEB_ENABLED
        return new audio::cubeb::Core();
#else
        LOG_FATAL(Other, "cubeb not supported");
#endif
    default:
        // TODO: return an error instead
        LOG_FATAL(Other, "Unknown audio backend {}",
                  CONFIG_INSTANCE.getAudioBackend());
    }
}

} // namespace

System::System(horizon::ui::IHandler& ui_handler_)
    : ui_handler{ui_handler_}, cpu{createCpu()}, audio_core{createAudioCore()},
      os(*this) {
    // TODO: set this elsewhere
    LOGGER_INSTANCE.setOutput(CONFIG_INSTANCE.getLogOutput());

    // DEBUG: print where an uncaught exception ends the process. Uses only
    // async-signal-safe calls, as locks may be broken at this point.
    static const bool terminate_hook_installed = [] {
        std::set_terminate([] {
            constexpr char header[] = "\n[hydra] std::terminate backtrace:\n";
            write(STDERR_FILENO, header, sizeof(header) - 1);
            void* frames[64];
            const int count = backtrace(frames, 64);
            backtrace_symbols_fd(frames, count, STDERR_FILENO);
            abort();
        });
        return true;
    }();
    (void)terminate_hook_installed;
    // Opt-in, best-effort diagnostics for native faults when macOS cannot
    // produce a crash report. Normal launches retain the OS signal handlers.
    static const bool fault_hook_installed = [] {
        if (std::getenv("HYDRA_TRACE_FAULTS") == nullptr)
            return false;
        void* warmup[1];
        backtrace(warmup, 1);
        struct sigaction action{};
        action.sa_flags = SA_SIGINFO | SA_RESETHAND;
        action.sa_sigaction = [](int signal, siginfo_t* info, void* context) {
            constexpr char header[] = "\n[hydra] native fault backtrace:\n";
            write(STDERR_FILENO, header, sizeof(header) - 1);
            auto write_address = [](uptr value) {
                char buffer[19] = {'0', 'x'};
                constexpr char digits[] = "0123456789abcdef";
                for (usize i = 0; i < 16; ++i)
                    buffer[2 + i] = digits[(value >> ((15 - i) * 4)) & 0xf];
                buffer[18] = '\n';
                write(STDERR_FILENO, buffer, sizeof(buffer));
            };
            write_address(reinterpret_cast<uptr>(info->si_addr));
#if defined(__APPLE__) && defined(__aarch64__)
            const auto* state = static_cast<const ucontext_t*>(context);
            write_address(state->uc_mcontext->__ss.__pc);
#else
            (void)context;
#endif
            void* frames[64];
            const int count = backtrace(frames, 64);
            backtrace_symbols_fd(frames, count, STDERR_FILENO);
            kill(getpid(), signal);
        };
        sigemptyset(&action.sa_mask);
        sigaction(SIGSEGV, &action, nullptr);
        sigaction(SIGBUS, &action, nullptr);
        return true;
    }();
    (void)fault_hook_installed;
}

System::~System() {
    forceStop();
    // TODO: set this elsewhere
    LOGGER_INSTANCE.setOutput(LogOutput::StdOut);
}

void System::loadAndStart(horizon::loader::ILoader* loader) {
    if (const auto reason = loader->getUnsupportedReason(); !reason.empty())
        throw std::runtime_error(std::string(reason));
    // Process
    ASSERT(main_process == nullptr, Other, "Process already exists");
    main_process =
        os.getKernel().getProcessManager().createProcess("Guest process");
    loader->loadProcess(*this, main_process);
    // Services read the version and save data sizes from it
    main_process->setApplicationControlProperty(
        std::unique_ptr<horizon::services::ns::ApplicationControlProperty>(
            loader->loadNacp()));

    // Check for firmware applets
    horizon::services::am::internal::LibraryAppletController controller(
        horizon::LibraryAppletMode::AllForeground);
    // TODO: correct?
    u64 system_tick;
    os.getKernel().getSystemTick(system_tick);
    switch (loader->getTitleId()) {
    case 0x0100000000001003: { // controller
        // Common args
        horizon::applets::CommonArguments common_args{
            .version = 1,
            .size = sizeof(horizon::applets::CommonArguments),
            .library_applet_api_version = 1, // TODO: correct?
            .theme_color = 0,                // HACK
            .play_startup_sound = false,     // HACK
            .system_tick = system_tick,
        };
        controller.pushInData(new horizon::services::am::IStorage(common_args));

        // Arg
        horizon::applets::controller::SupportArg<4> arg{
            .player_count_min = 0,
            .player_count_max = 2,
            .enable_take_over_connection = true,
            .enable_left_justify = false,
            .enable_permit_joy_dual = true,
            .enable_single_mode = true,
            .enable_identification_color = false,
            .enable_explain_text = false,
        };

        // Private arg
        horizon::applets::controller::ArgPrivate private_arg{
            .size = sizeof(horizon::applets::controller::ArgPrivate),
            .controller_support_arg_size = sizeof(arg),
            .flag0 = 0,
            .flag1 = 0,
            .mode = horizon::applets::controller::Mode::ShowControllerSupport,
            .caller = horizon::applets::controller::Caller::Application,
            .npad_style_set = horizon::services::hid::NpadStyleSet::JoyDual,
            .npad_joy_hold_type =
                horizon::services::hid::NpadJoyHoldType::Vertical,
        };
        controller.pushInData(new horizon::services::am::IStorage(private_arg));

        controller.pushInData(new horizon::services::am::IStorage(arg));

        break;
    }
    case 0x0100000000001005: { // error
        // Common args
        horizon::applets::CommonArguments common_args{
            .version = 1,
            .size = sizeof(horizon::applets::CommonArguments),
            .library_applet_api_version = 1, // TODO: correct?
            .theme_color = 0,                // HACK
            .play_startup_sound = false,     // HACK
            .system_tick = system_tick,
        };
        controller.pushInData(new horizon::services::am::IStorage(common_args));

        // Param common
        horizon::applets::error::ParamCommon param_common{
            .type = horizon::applets::error::ErrorType::ApplicationError,
            .is_jump_enabled = false,
        };
        controller.pushInData(
            new horizon::services::am::IStorage(param_common));

        // Param for application error
        horizon::applets::error::ParamForApplicationError
            param_for_application_error{
                .version = 1,
                .error_code_number = MAKE_RESULT(Svc, 0),
                .language_code = horizon::toLanguageCode(
                    CONFIG_INSTANCE.getSystemLanguage()),
                .dialog_message = "Dialog message",
                .fullscreen_message = "Fullscreen message",
        };
        controller.pushInData(
            new horizon::services::am::IStorage(param_for_application_error));

        break;
    }
    case 0x0100000000001008: { // swkbd
        // Common args
        horizon::applets::CommonArguments common_args{
            .version = 1,
            .size = sizeof(horizon::applets::CommonArguments),
            .library_applet_api_version = 1, // TODO: correct?
            .theme_color = 0,                // HACK
            .play_startup_sound = false,     // HACK
            .system_tick = system_tick,
        };
        controller.pushInData(new horizon::services::am::IStorage(common_args));

        // Config
        horizon::applets::software_keyboard::KeyboardConfigCommon config{
            .mode = horizon::applets::software_keyboard::KeyboardMode::Full,
            // TODO: more
        };
        controller.pushInData(new horizon::services::am::IStorage(config));

        break;
    }
    case 0x0100000000001009: { // miiEdit
        // Args
        horizon::applets::mii_edit::AppletInput args{
            ._unknown_x0 = 0x3,
            .mode = horizon::applets::mii_edit::AppletMode::ShowMiiEdit,
        };
        controller.pushInData(new horizon::services::am::IStorage(args));

        break;
    }
    case 0x010000000000100d: { // photoViewer
        // Common args
        horizon::applets::CommonArguments common_args{
            .version = 1,
            .size = sizeof(horizon::applets::CommonArguments),
            .library_applet_api_version = 1, // TODO: correct?
            .theme_color = 0,                // HACK
            .play_startup_sound = false,     // HACK
            .system_tick = system_tick,
        };
        controller.pushInData(new horizon::services::am::IStorage(common_args));

        // Arg
        auto arg = new horizon::applets::album::Arg{
            horizon::applets::album::Arg::ShowAllAlbumFilesForHomeMenu};
        controller.pushInData(new horizon::services::am::IStorage(arg));

        break;
    }
    default:
        break;
    }

    os.setLibraryAppletSelfController(std::move(controller));

    // Loading screen assets
    {
        std::unique_ptr<hw::tegra_x1::gpu::renderer::ICommandBuffer>
            command_buffer = nullptr;

        {
            u32 width;
            u32 height;
            if (auto data = loader->loadNintendoLogo(width, height)) {
                // Create texture
                const u32 stride = width * 4;
                const u32 size = height * stride;
                const hw::tegra_x1::gpu::renderer::TextureDescriptor descriptor(
                    0x0, MTL::TextureType2D,
                    hw::tegra_x1::gpu::renderer::TextureFormat::RGBA8Unorm,
                    true, stride, width, height, 1, 1, 1, 0x0, 0x0, 0x0);
                const auto texture =
                    gpu.getRenderer().createTexture(descriptor);

                const auto view_descriptor =
                    hw::tegra_x1::gpu::renderer::TextureViewDescriptor(
                        descriptor.type, descriptor.format,
                        ztd::Range<u32>(0, 1), ztd::Range<u32>(0, 1));
                const auto texture_view = texture->createView(view_descriptor);
                nintendo_logo = {.base = texture, .view = texture_view};

                // Command buffer
                command_buffer.reset(gpu.getRenderer().createCommandBuffer());

                // Copy data
                auto tmp_buffer =
                    gpu.getRenderer().allocateTemporaryBuffer(size);
                std::memcpy(reinterpret_cast<void*>(tmp_buffer->getPtr()), data,
                            size);
                // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
                free(data);
                texture->copyFrom(command_buffer.get(), tmp_buffer);
                gpu.getRenderer().freeTemporaryBuffer(tmp_buffer);
            }
        }
        {
            u32 width;
            u32 height;
            u32 frame_count;
            if (auto data = loader->loadStartupMovie(
                    startup_movie_delays, width, height, frame_count)) {
                const u32 stride = width * 4;
                const u32 size = height * stride;
                hw::tegra_x1::gpu::renderer::TextureDescriptor descriptor(
                    0x0, MTL::TextureType2D,
                    hw::tegra_x1::gpu::renderer::TextureFormat::RGBA8Unorm,
                    true, stride, width, height, 1, 1, 1, 0x0, 0x0, 0x0);
                const auto view_descriptor =
                    hw::tegra_x1::gpu::renderer::TextureViewDescriptor(
                        descriptor.type, descriptor.format,
                        ztd::Range<u32>(0, 1), ztd::Range<u32>(0, 1));
                startup_movie.reserve(frame_count);

                // Command buffer
                if (command_buffer == nullptr)
                    command_buffer.reset(
                        gpu.getRenderer().createCommandBuffer());

                for (u32 i = 0; i < frame_count; i++) {
                    // Create texture
                    const auto texture =
                        gpu.getRenderer().createTexture(descriptor);
                    const auto texture_view =
                        texture->createView(view_descriptor);

                    // Copy data
                    auto tmp_buffer =
                        gpu.getRenderer().allocateTemporaryBuffer(size);
                    std::memcpy(reinterpret_cast<void*>(tmp_buffer->getPtr()),
                                data + static_cast<usize>(i) * height * width,
                                size);
                    texture->copyFrom(command_buffer.get(), tmp_buffer);
                    gpu.getRenderer().freeTemporaryBuffer(tmp_buffer);
                    startup_movie.push_back(
                        {.base = texture, .view = texture_view});
                }
                // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
                free(data);

                // Extend the last frame's time
                startup_movie_delays.back() = 5s;
            }
        }
    }

    LOG_INFO(Other, "-------- Title info --------");
    LOG_INFO(Other, "Title ID: {:016x}", loader->getTitleId());
    // Start compiling the shaders the title used before while it boots
    gpu.getRenderer().setTitleId(loader->getTitleId());

    // Patch
    const auto target_patch_filename =
        fmt::format("{:016x}.hatch", loader->getTitleId());
    // TODO: iterate recursively
    for (const auto& patch_path : CONFIG_INSTANCE.getPatchPaths()) {
        if (!std::filesystem::exists(patch_path)) {
            LOG_ERROR(Other, "Patch path does not exist: {}", patch_path);
            continue;
        }

        if (!std::filesystem::is_directory(patch_path)) {
            // File
            tryApplyPatch(main_process, target_patch_filename, patch_path);
        } else {
            // Directory
            // TODO: iterate recursively
            for (const auto& dir_entry :
                 std::filesystem::directory_iterator{patch_path}) {
                tryApplyPatch(main_process, target_patch_filename,
                              dir_entry.path().string());
            }
        }
    }

    LOG_INFO(Other, "-------- Config --------");
    CONFIG_INSTANCE.log();

    LOG_INFO(Other, "-------- Run --------");

    // Enter focus
    // HACK: games expect focus change to be the second message?
    main_process->getAppletState().sendMessage(
        horizon::kernel::AppletMessage::Resume);
    main_process->getAppletState().setFocusState(
        horizon::kernel::AppletFocusState::InFocus);

    // Preselected user
    auto user_id = CONFIG_INSTANCE.getUserId();
    if (user_id == horizon::services::account::internal::INVALID_USER_ID) {
        // If there is just a single user, use that
        if (os.getUserManager().getUserCount() == 1) {
            user_id = os.getUserManager().getUserIDs()[0];
        } else {
            // TODO: launch a select user applet in case the game requires it
            LOG_FATAL(Other, "Multiple user accounts");
        }
    }

    if (user_id != horizon::services::account::internal::INVALID_USER_ID) {
        main_process->getAppletState().pushPreselectedUser(user_id);
        LOG_INFO(Other, "Preselected user with ID {:032x}", user_id);
    }

    main_process->start();

    // DEBUG: HYDRA_THREAD_DUMP=<file> dumps the guest threads when the file
    // appears, also before the first frame (progressFrame would never run)
    if (const char* trigger = std::getenv("HYDRA_THREAD_DUMP")) {
        std::thread([this, path = std::string(trigger)] {
            std::error_code ec;
            while (true) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                if (!std::filesystem::exists(path, ec))
                    continue;
                std::filesystem::remove(path, ec);
                if (main_process == nullptr)
                    LOG_INFO(Other, "THREADDUMP requested but no process exists");
                else
                    main_process->debugDumpThreads();
                std::fflush(nullptr);
            }
        }).detach();
    }

    // Activate GDB server
    if (CONFIG_INSTANCE.getGdbEnabled()) {
        if (CONFIG_INSTANCE.getGdbWaitForClient())
            main_process->getMainThread()->supervisorPause();

        // HACK: spinlock until the main thread is running
        while (!main_process->isRunning())
            std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        DEBUGGER_MANAGER_INSTANCE.getDebugger(main_process)
            .activateGdbServer(*this);
    }

    // Loading screen
    loading = true;

    const auto crnt_time = clock_t::now();
    next_startup_movie_frame_time = crnt_time + STARTUP_MOVIE_FADE_IN_DURATION +
                                    STARTUP_MOVIE_BREAK_AFTER_FADE_IN_DURATION;
    startup_movie_fade_in_time = crnt_time + STARTUP_MOVIE_FADE_IN_DURATION;
}

void System::requestStop() {
    // We don't request the processes to stop yet, instead we send a message to
    // all of them and give them some time to react
    for (auto& it : os.getKernel().getProcessManager())
        it->getAppletState().sendMessage(horizon::kernel::AppletMessage::Exit);
}

void System::forceStop() {
    // Request all processes to stop immediately
    for (auto& it : os.getKernel().getProcessManager())
        it->stop();

    // Service workers must stop while their managers and client MMUs are alive.
    os.shutdownServices();
    for (auto& process : os.getKernel().getProcessManager())
        process->joinThreads();
    gpu.getPfifo().shutdown();
}

void System::pause() {
    for (auto& it : os.getKernel().getProcessManager())
        it->supervisorPause();
}

void System::resume() {
    for (auto& it : os.getKernel().getProcessManager())
        it->supervisorResume();
}

void System::progressFrame(u32 width, u32 height,
                           bool& out_dt_average_updated) {
    out_dt_average_updated = false;

    // DEBUG: dump guest threads when the trigger file appears
    static const char* thread_dump_trigger = std::getenv("HYDRA_THREAD_DUMP");
    if (thread_dump_trigger != nullptr && main_process != nullptr) {
        std::error_code ec;
        if (std::filesystem::remove(thread_dump_trigger, ec))
            main_process->debugDumpThreads();
    }

    // Pump input events
    input_device_manager.pumpEvents();

    // Set the resolution for OS
    os.setSurfaceResolution({width, height});

    // Input
    os.getHidResourceManager().update();

    // Present

    // Acquire surface
    auto compositor = gpu.getRenderer().acquireNextSurface();
    if (compositor == nullptr)
        return;

    // Delta time
    {
        auto layer =
            os.getDisplayDriver().getFirstLayerForProcess(main_process);
        if (layer != nullptr)
            accumulated_dt += layer->getAccumulatedDt();
    }

    // Command buffer
    auto command_buffer = gpu.getRenderer().createCommandBuffer();

    // Acquire present textures
    bool acquired =
        os.getDisplayDriver().acquirePresentTextures(command_buffer);
    if (acquired) {
        hw::tegra_x1::gpu::FrameTracer::instance().onPresent();
        hw::tegra_x1::gpu::capture::LiveCapture::instance().onPresent(
            gpu.getRenderer());
        hw::tegra_x1::gpu::VisualDiagnostics::instance().onPresent();
    }

    // DEBUG: performance statistics
    if (acquired && hw::tegra_x1::gpu::PerfStats::isEnabled()) {
        auto& stats = hw::tegra_x1::gpu::PerfStats::instance();
        stats.frames++;
        static auto last = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration<f64>(now - last).count();
        if (elapsed >= 2.0) {
            last = now;
            const auto frames = static_cast<f64>(stats.frames.exchange(0));
            const auto per_frame_ms = [&](std::atomic<u64>& counter) {
                return static_cast<f64>(counter.exchange(0)) / 1e6 /
                       std::max(frames, 1.0);
            };
            const auto pfifo_ms = per_frame_ms(stats.pfifo_busy_ns);
            const auto wait_ms = per_frame_ms(stats.present_wait_ns);
            const auto gpu_ms = per_frame_ms(stats.gpu_ns);
            const auto lists = stats.pfifo_lists.exchange(0);
            const auto cbs = stats.command_buffers.exchange(0);
            const auto draws = stats.draws.exchange(0);
            const auto upload_ms = per_frame_ms(stats.texture_upload_ns);
            const auto upload_bytes = stats.texture_upload_bytes.exchange(0);
            const auto buffers_created =
                stats.upload_buffers_created.exchange(0);
            const auto buffers_reused = stats.upload_buffers_reused.exchange(0);
            const auto draw_state_ms = per_frame_ms(stats.draw_state_ns);
            const auto draw_prepare_ms = per_frame_ms(stats.draw_prepare_ns);
            const auto draw_command_ms = per_frame_ms(stats.draw_command_ns);
            const auto draw_pipeline_ms = per_frame_ms(stats.draw_pipeline_ns);
            const auto draw_targets_ms = per_frame_ms(stats.draw_targets_ns);
            const auto draw_snapshot_ms = per_frame_ms(stats.draw_snapshot_ns);
            const auto draw_apply_ms = per_frame_ms(stats.draw_apply_ns);
            const auto draw_resources_ms = per_frame_ms(stats.draw_resources_ns);
            const auto shader_ns = stats.shader_compile_ns.exchange(0);
            const auto shaders = stats.shader_compiles.exchange(0);
            const auto pipeline_ns = stats.pipeline_compile_ns.exchange(0);
            const auto pipelines = stats.pipeline_compiles.exchange(0);
            LOG_INFO(
                Other,
                "DRAW: {:.2f} ms/frame binding | raster {} updates, {} skipped "
                "| compilation shader {} ({:.2f} ms), pipeline {} ({:.2f} ms)",
                draw_state_ms, stats.raster_updates.exchange(0),
                stats.raster_skips.exchange(0), shaders,
                static_cast<f64>(shader_ns) / 1e6, pipelines,
                static_cast<f64>(pipeline_ns) / 1e6);
            LOG_INFO(Other,
                     "DRAW PREP: {:.2f} ms/frame total | pipeline {:.2f}, "
                     "targets/coherence {:.2f}, raster snapshot {:.2f}, "
                     "vertex/stage resources {:.2f} (subsets of prepare)",
                     draw_prepare_ms, draw_pipeline_ms, draw_targets_ms,
                     draw_snapshot_ms, draw_resources_ms);
            LOG_INFO(Other,
                     "DRAW COMMAND: {:.2f} ms/frame encode/storage completion | "
                     "raster apply {:.2f} (subset of command)",
                     draw_command_ms, draw_apply_ms);
            LOG_INFO(Other,
                     "UPLOAD: {:.2f} ms/frame | {:.1f} MiB/s | buffers {} new, "
                     "{} reused",
                     upload_ms,
                     static_cast<f64>(upload_bytes) / (1024.0 * 1024.0) /
                         elapsed,
                     buffers_created, buffers_reused);
            LOG_INFO(Other, "TEXTURE: {} synchronization scans, {} skipped",
                     stats.texture_sync_scans.exchange(0),
                     stats.texture_sync_skips.exchange(0));
            LOG_INFO(Other, "CPU registers: {} writes, {} skipped",
                     stats.register_writes.exchange(0),
                     stats.register_write_skips.exchange(0));
            u64 cached_buffer_bytes;
            {
                auto& cache = gpu.getRenderer().getBufferCache();
                std::scoped_lock lock(cache.getMutex());
                cached_buffer_bytes = cache.getCachedBytes();
            }
            LOG_INFO(
                Other,
                "BUFFERS: {:.1f} MiB cached, {} snapshots ({:.1f} MiB/s) | "
                "encoders {:.1f} render, {:.1f} blit/frame",
                static_cast<f64>(cached_buffer_bytes) / (1024.0 * 1024.0),
                stats.buffer_snapshots.exchange(0),
                static_cast<f64>(stats.buffer_snapshot_bytes.exchange(0)) /
                    (1024.0 * 1024.0) / elapsed,
                static_cast<f64>(stats.render_encoders.exchange(0)) /
                    std::max(frames, 1.0),
                static_cast<f64>(stats.blit_encoders.exchange(0)) /
                    std::max(frames, 1.0));
            {
                // Waits of the GPU thread for the host GPU, by what asked
                // for them
                const auto per_frame = [&](u64 value) {
                    return static_cast<f64>(value) / std::max(frames, 1.0);
                };
                u64 waits = 0;
                u64 wait_ns = 0;
                std::string reasons;
                for (usize i = 0; i < hw::tegra_x1::gpu::CHECKPOINT_REASON_COUNT;
                     i++) {
                    const auto count = stats.checkpoint_count[i].exchange(0);
                    const auto ns = stats.checkpoint_ns[i].exchange(0);
                    if (count == 0)
                        continue;
                    waits += count;
                    wait_ns += ns;
                    reasons += fmt::format(
                        " | {} {:.1f} ({:.2f} ms)",
                        hw::tegra_x1::gpu::getCheckpointReasonName(
                            static_cast<hw::tegra_x1::gpu::CheckpointReason>(i)),
                        per_frame(count), static_cast<f64>(ns) / 1e6 /
                                              std::max(frames, 1.0));
                }
                LOG_INFO(Other,
                         "CHECKPOINTS waited {:.1f}/frame ({:.2f} ms/frame){} | "
                         "skipped (nothing in flight) {:.1f}/frame",
                         per_frame(waits),
                         static_cast<f64>(wait_ns) / 1e6 / std::max(frames, 1.0),
                         reasons, per_frame(stats.checkpoint_skips.exchange(0)));
                // Guest CPU: how long the guest threads executed, why they
                // left the vCPU and which SVCs took the most wall time
                std::string exits;
                u64 exit_total = 0;
                for (usize i = 0; i < hw::tegra_x1::gpu::CPU_EXIT_COUNT; i++) {
                    const auto count = stats.cpu_exits[i].exchange(0);
                    exit_total += count;
                    exits += fmt::format(
                        " | {} {:.0f}",
                        hw::tegra_x1::gpu::getCpuExitName(
                            static_cast<hw::tegra_x1::gpu::CpuExit>(i)),
                        per_frame(count));
                }
                std::array<std::pair<u64, u32>, 0x80> svcs;
                for (u32 i = 0; i < svcs.size(); i++)
                    svcs[i] = {stats.svc_ns[i].exchange(0), i};
                std::sort(svcs.begin(), svcs.end(), std::greater<>());
                std::string top_svcs;
                for (usize i = 0; i < 8 && svcs[i].first != 0; i++) {
                    const auto id = svcs[i].second;
                    top_svcs += fmt::format(
                        " | {:#x} {:.0f}x {:.2f} ms", id,
                        per_frame(stats.svc_count[id].load()),
                        static_cast<f64>(svcs[i].first) / 1e6 /
                            std::max(frames, 1.0));
                }
                for (auto& count : stats.svc_count)
                    count = 0;
                LOG_INFO(Other,
                         "GUEST CPU: run {:.2f} ms/frame (all threads), "
                         "{:.0f} exits/frame{}",
                         static_cast<f64>(stats.guest_run_ns.exchange(0)) /
                             1e6 / std::max(frames, 1.0),
                         per_frame(exit_total), exits);
                LOG_INFO(Other, "GUEST SVC wall time per frame:{}", top_svcs);
                LOG_INFO(Other, "DRAW QUEUE waits: {}",
                         hw::tegra_x1::gpu::renderer::takeDrawQueueReport(
                             frames));
                LOG_INFO(Other,
                         "REPORTS: {:.1f} queued/frame, {} forced a wait",
                         per_frame(stats.reports_queued.exchange(0)),
                         stats.report_waits.exchange(0));
            }
            LOG_INFO(Other,
                     "UNIMPLEMENTED draw state: {} MSAA, {} alpha-to-coverage, {} "
                     "alpha-to-one draws",
                     stats.msaa_draws.exchange(0),
                     stats.alpha_to_coverage_draws.exchange(0),
                     stats.alpha_to_one_draws.exchange(0));
            LOG_INFO(Other,
                     "RENDER PASSES: {:.1f}/frame | load {:.1f} MiB/frame, "
                     "store {:.1f} MiB/frame | continuations {:.1f}/frame "
                     "(open pass {:.1f}, blit {:.1f}, compute {:.1f}, "
                     "submission {:.1f}, visibility buffer {:.1f}, ended "
                     "elsewhere {:.1f}) | merged into the open pass "
                     "{:.1f}/frame",
                     static_cast<f64>(stats.render_passes.exchange(0)) /
                         std::max(frames, 1.0),
                     static_cast<f64>(stats.render_pass_load_bytes.exchange(0)) /
                         (1024.0 * 1024.0) / std::max(frames, 1.0),
                     static_cast<f64>(stats.render_pass_store_bytes.exchange(0)) /
                         (1024.0 * 1024.0) / std::max(frames, 1.0),
                     static_cast<f64>(stats.render_pass_continuations.exchange(0)) /
                         std::max(frames, 1.0),
                     static_cast<f64>(stats.render_pass_continuation_causes[0].exchange(0)) /
                         std::max(frames, 1.0),
                     static_cast<f64>(stats.render_pass_continuation_causes[1].exchange(0)) /
                         std::max(frames, 1.0),
                     static_cast<f64>(stats.render_pass_continuation_causes[2].exchange(0)) /
                         std::max(frames, 1.0),
                     static_cast<f64>(stats.render_pass_continuation_causes[3].exchange(0)) /
                         std::max(frames, 1.0),
                     static_cast<f64>(stats.render_pass_continuation_causes[4].exchange(0)) /
                         std::max(frames, 1.0),
                     static_cast<f64>(stats.render_pass_continuation_causes[5].exchange(0)) /
                         std::max(frames, 1.0),
                     static_cast<f64>(stats.render_pass_merges.exchange(0)) /
                         std::max(frames, 1.0));
            gpu.getRenderer().logPassProfile(frames);
            {
                const auto presents = stats.presents.exchange(0);
                const auto gpu_count = stats.present_gpu_count.exchange(0);
                LOG_INFO(Other,
                         "PRESENT: filter {}, sharpening {}, frame generation "
                         "{} | {}x{} -> {}x{} | GPU {:.3f} ms, CPU encode "
                         "{:.3f} ms per present | {:.1f} presents/s",
                         CONFIG_INSTANCE.getUpscalingFilter(),
                         CONFIG_INSTANCE.getSharpening(),
                         CONFIG_INSTANCE.getFrameGeneration(),
                         stats.present_input_width.load(),
                         stats.present_input_height.load(),
                         stats.present_output_width.load(),
                         stats.present_output_height.load(),
                         static_cast<f64>(stats.present_gpu_ns.exchange(0)) /
                             1e6 / std::max<f64>(static_cast<f64>(gpu_count), 1.0),
                         static_cast<f64>(stats.present_encode_ns.exchange(0)) /
                             1e6 / std::max<f64>(static_cast<f64>(presents), 1.0),
                         static_cast<f64>(presents) / elapsed);
            }
            {
                // Displayed since the last statistics
                const auto& display = hw::tegra_x1::gpu::DisplayStats::instance();
                static u64 last_counts[4] = {};
                const u64 now_counts[4] = {display.game_frames.load(),
                                           display.generated_frames.load(),
                                           display.repeated_frames.load(),
                                           display.dropped.load()};
                f64 per_second[4];
                for (usize i = 0; i < 4; i++) {
                    per_second[i] =
                        static_cast<f64>(now_counts[i] - last_counts[i]) / elapsed;
                    last_counts[i] = now_counts[i];
                }
                LOG_INFO(Other,
                         "DISPLAYED: {:.1f} images/s on screen ({:.1f} new "
                         "game frames, {:.1f} generated, {:.1f} repeated) | "
                         "{:.1f} dropped/s",
                         per_second[0] + per_second[1] + per_second[2],
                         per_second[0], per_second[1], per_second[2],
                         per_second[3]);
            }
            LOG_INFO(Other, "COMPUTE: {:.1f} dispatches, {:.0f} threads/frame",
                     static_cast<f64>(stats.compute_dispatches.exchange(0)) /
                         std::max(frames, 1.0),
                     static_cast<f64>(stats.compute_threads.exchange(0)) /
                         std::max(frames, 1.0));
            LOG_INFO(Other,
                     "SHADERS: decompiled {} ({:.2f} ms) | library hits {} | "
                     "prewarmed pipelines used {}",
                     stats.shader_decompiles.exchange(0),
                     static_cast<f64>(stats.shader_decompile_ns.exchange(0)) /
                         1e6,
                     stats.shader_library_hits.exchange(0),
                     stats.pipeline_prewarm_hits.exchange(0));
            {
                // Where the GPU thread's time goes: sync work has to stay on
                // it, async-capable work could move to other cores
                using hw::tegra_x1::gpu::GpuWork;
                f64 work_ms[hw::tegra_x1::gpu::GPU_WORK_COUNT];
                f64 sync_ms = 0.0, async_ms = 0.0;
                for (usize i = 0; i < hw::tegra_x1::gpu::GPU_WORK_COUNT; i++) {
                    work_ms[i] = per_frame_ms(stats.gpu_work_ns[i]);
                    const auto work = static_cast<GpuWork>(i);
                    if (hw::tegra_x1::gpu::isGpuWorkAsyncCapable(work))
                        async_ms += work_ms[i];
                    else if (work != GpuWork::Wait)
                        sync_ms += work_ms[i];
                }
                const auto ms = [&](GpuWork work) {
                    return work_ms[static_cast<usize>(work)];
                };
                const auto total_ms =
                    std::max(sync_ms + async_ms + ms(GpuWork::Wait), 1e-9);
                LOG_INFO(Other,
                         "GPU THREAD: sync {:.2f} ms ({:.0f}%: decode {:.2f}, "
                         "state {:.2f}, lookup {:.2f}) | async-capable {:.2f} "
                         "ms ({:.0f}%: encode {:.2f}, shader {:.2f}, texture "
                         "{:.2f}, buffer {:.2f}, copy {:.2f}) | waiting for "
                         "GPU {:.2f} ms ({:.0f}%) per frame",
                         sync_ms, 100.0 * sync_ms / total_ms,
                         ms(GpuWork::Decode), ms(GpuWork::State),
                         ms(GpuWork::Lookup), async_ms,
                         100.0 * async_ms / total_ms, ms(GpuWork::Encode),
                         ms(GpuWork::Shader), ms(GpuWork::Texture),
                         ms(GpuWork::Buffer), ms(GpuWork::Copy),
                         ms(GpuWork::Wait),
                         100.0 * ms(GpuWork::Wait) / total_ms);
            }
#ifdef ZTD_PLATFORM_APPLE
            task_vm_info_data_t memory_info{};
            mach_msg_type_number_t memory_count = TASK_VM_INFO_COUNT;
            if (task_info(mach_task_self(), TASK_VM_INFO,
                          reinterpret_cast<task_info_t>(&memory_info),
                          &memory_count) == KERN_SUCCESS)
                LOG_INFO(Other, "Process memory footprint: {:.1f} MiB",
                         static_cast<f64>(memory_info.phys_footprint) / (1024.0 * 1024.0));
#endif
            LOG_INFO(Other, "Metal resources: {:.1f} MiB allocated",
                     static_cast<f64>(gpu.getRenderer().getAllocatedResourceBytes()) / (1024.0 * 1024.0));
            LOG_INFO(Other,
                     "PERF: {:.1f} fps | pfifo (CPU encode) {:.2f} ms/frame, "
                     "{:.1f} lists/frame | present wait {:.2f} ms/frame | "
                     "Metal GPU {:.2f} ms/frame, {:.1f} cmdbufs/frame | {:.0f} "
                     "draws/frame",
                     frames / elapsed, pfifo_ms,
                     static_cast<f64>(lists) / std::max(frames, 1.0), wait_ms,
                     gpu_ms, static_cast<f64>(cbs) / std::max(frames, 1.0),
                     static_cast<f64>(draws) / std::max(frames, 1.0));
        }
    }

    // Render pass
    os.getDisplayDriver().present(command_buffer, compositor, width, height);

    if (loading) {
        if (acquired) {
            // TODO: till when should the loading screen be shown?
            // Stop the loading screen on the first present
            loading = false;

            // Free loading assets
            if (nintendo_logo)
                nintendo_logo = std::nullopt;
            if (!startup_movie.empty()) {
                startup_movie.clear();
                startup_movie.shrink_to_fit();
                startup_movie_delays.clear();
                startup_movie_delays.shrink_to_fit();
            }
        } else {
            const auto crnt_time = clock_t::now();

            // Display loading screen

            // Fade in
            f32 opacity = 1.0f;
            if (crnt_time < startup_movie_fade_in_time)
                opacity =
                    1.0f -
                    std::chrono::duration_cast<std::chrono::duration<f32>>(
                        startup_movie_fade_in_time - crnt_time) /
                        std::chrono::duration_cast<std::chrono::duration<f32>>(
                            STARTUP_MOVIE_FADE_IN_DURATION);

            // Nintendo logo
            if (nintendo_logo) {
                const auto tex = *nintendo_logo;
                int2 size = {
                    static_cast<i32>(tex.base->getDescriptor().width),
                    static_cast<i32>(tex.base->getDescriptor().height)};
                int2 dst_offset = {32, 32};
                compositor->drawTexture(
                    command_buffer, tex.view, IntRect2D({0, 0}, size),
                    IntRect2D(dst_offset, size), true, opacity);
            }

            // Startup movie
            if (!startup_movie.empty()) {
                // Progress frame
                while (crnt_time > next_startup_movie_frame_time) {
                    startup_movie_frame =
                        (startup_movie_frame + 1) % startup_movie.size();
                    next_startup_movie_frame_time +=
                        startup_movie_delays[startup_movie_frame];
                }

                auto frame = startup_movie[startup_movie_frame];
                int2 size = {
                    static_cast<i32>(frame.base->getDescriptor().width),
                    static_cast<i32>(frame.base->getDescriptor().height)};
                int2 dst_offset = {static_cast<i32>(width) - size.x() - 32,
                                   static_cast<i32>(height) - size.y() - 32};
                compositor->drawTexture(
                    command_buffer, frame.view, IntRect2D({0, 0}, size),
                    IntRect2D(dst_offset, size), true, opacity);
            }
        }
    } else {
        // Delta time
        const auto now = clock_t::now();
        const auto time_since_last_dt_averaging = now - last_dt_averaging_time;
        if (time_since_last_dt_averaging > 1s) {
            if (static_cast<bool>(accumulated_dt))
                last_dt_average = static_cast<f32>(accumulated_dt);
            else
                last_dt_average = 0.f;
            accumulated_dt = {};
            // New images the display showed: game frames and generated ones
            const auto& display = hw::tegra_x1::gpu::DisplayStats::instance();
            const u64 game = display.game_frames.load();
            const u64 generated = display.generated_frames.load();
            const auto seconds =
                std::chrono::duration<f32>(time_since_last_dt_averaging).count();
            last_shown_fps =
                static_cast<f32>((game - last_display_game) +
                                 (generated - last_display_generated)) /
                seconds;
            last_generated_fps =
                static_cast<f32>(generated - last_display_generated) / seconds;
            last_display_game = game;
            last_display_generated = generated;
            last_dt_averaging_time = now;

            out_dt_average_updated = true;
        } else {
            out_dt_average_updated = false;
        }
    }

    compositor->present(command_buffer, acquired);

    delete command_buffer;
    delete compositor;

}

f64 System::getPresentationFrameRate() const {
    return frameGenerationOutputRate(
        CONFIG_INSTANCE.getFrameGenerationRateMode() == FrameGenerationRateMode::Multiple,
        CONFIG_INSTANCE.getFrameGenerationTargetFps(),
        CONFIG_INSTANCE.getFrameGenerationMultiplier(), static_cast<f64>(last_dt_average));
}

bool System::isRunning() const {
    if (main_process == nullptr)
        return false;

    switch (main_process->getState()) {
    case horizon::kernel::ProcessState::Started:
    case horizon::kernel::ProcessState::Exiting:
        return true;
    default:
        return false;
    };
}

void System::takeScreenshot() {
    auto layer = os.getDisplayDriver().getFirstLayerForProcess(main_process);
    if (layer == nullptr)
        return;

    ZTD_ASSIGN_OR_RETURN(auto texture, layer->getPresentTexture());

    std::thread thread([layer, texture, this] {
        // Get the image data
        auto rect = layer->getSrcRect();

        // Check if the image is flipped
        ASSERT(rect.size.x() > 0, Other, "Invalid width {}", rect.size.x());
        bool flip_y = false;
        if (rect.size.y() < 0) {
            rect.origin.y() += rect.size.y();
            rect.size.y() = -rect.size.y();
            flip_y = true;
        }

        // Copy to a buffer
        auto command_buffer = gpu.getRenderer().createCommandBuffer();
        auto buffer = gpu.getRenderer().allocateTemporaryBuffer(
            static_cast<u32>(rect.size.y() * rect.size.x() * 4));
        buffer->copyFrom(command_buffer, texture,
                         MTL::Origin(static_cast<NS::UInteger>(rect.origin.x()),
                                     static_cast<NS::UInteger>(rect.origin.y()),
                                     0),
                         MTL::Size(static_cast<NS::UInteger>(rect.size.x()),
                                   static_cast<NS::UInteger>(rect.size.y()), 1),
                         ztd::Range<u32>(0, 1), ztd::Range<u32>(0, 1));
        // Wait for the copy to finish
        gpu.getRenderer().debugSyncCommandBuffer(command_buffer);
        delete command_buffer;

        // Save the image to file
        auto now = std::chrono::system_clock::now();
        // TODO: use title name in the filename
        std::string filename =
            fmt::format("{}/screenshot_{:%Y-%m-%d_%H-%M-%S}.jpg",
                        CONFIG_INSTANCE.getPicturesPath(), now);

        stbi_flip_vertically_on_write(flip_y);
        if (!stbi_write_jpg(filename.c_str(), rect.size.x(), rect.size.y(), 4,
                            reinterpret_cast<void*>(buffer->getPtr()), 100))
            LOG_ERROR(Other, "Failed to save screenshot to {}", filename);
        stbi_flip_vertically_on_write(false);

        // Free the buffer
        gpu.getRenderer().freeTemporaryBuffer(buffer);
    });
    thread.detach();
}

void System::captureGpuFrame() {
    // With HYDRA_GPU_CAPTURE, a replayable capture of what follows (more than
    // a frame, see LiveCapture) instead of a Metal GPU trace
    if (const char* directory = std::getenv("HYDRA_GPU_CAPTURE")) {
        std::ofstream(std::filesystem::path(directory) / "trigger")
            << "commands=3000 limit_mb=1500";
        LOG_INFO(Gpu, "GPU capture requested");
        return;
    }
    // TODO: allow multiple frames
    gpu.getRenderer().captureFrames(1);
}

void System::tryApplyPatch(horizon::kernel::Process* process,
                           const std::string_view target_filename,
                           const std::filesystem::path& path) {
    if (toLower(path.filename().string()) != target_filename)
        return;

    LOG_INFO(Other, "Applying patch \"{}\"", path.string());

    std::ifstream ifs(path);

    // Deserialize
    Hatch::Deserializer deserializer;
    deserializer.Deserialize(ifs);

    const auto& hatch = deserializer.GetHatch();

    // Memory patch
    for (const auto& entry : hatch.GetMemoryPatch())
        process->getMmu()->write<u32>(entry.addr, entry.value);

    ifs.close();
}

} // namespace hydra
