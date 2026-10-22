import SwiftUI

struct GraphicsSettingsView: View {
    @State private var gpuRenderer = HydraGpuRenderer(rawValue: configGetGpuRenderer().pointee)
    @State private var shaderBackend = HydraShaderBackend(rawValue: configGetShaderBackend().pointee)
    @State private var metalApi = HydraMetalApi(rawValue: configGetMetalApi().pointee)
    @State private var upscalingFilter = HydraUpscalingFilter(
        rawValue: configGetUpscalingFilter().pointee)
    @State private var frameRateLimit = configGetFrameRateLimit().pointee
    @State private var frameGeneration = HydraFrameGeneration(
        rawValue: configGetFrameGeneration().pointee)
    @State private var sharpening = Double(configGetSharpening().pointee)
    @State private var metalHud = configGetMetalHud().pointee

    var body: some View {
        Spacer()
        HStack {
            Spacer()
            Form {
                Section {
                    Picker("Gpu renderer", selection: self.$gpuRenderer.rawValue) {
                        Text("Null").tag(HYDRA_GPU_RENDERER_NULL.rawValue)
                        Text("Metal (recommended)").tag(HYDRA_GPU_RENDERER_METAL.rawValue)
                    }
                    .onChange(of: self.gpuRenderer.rawValue) { _, newValue in
                        configGetGpuRenderer().pointee = newValue
                    }

                    Picker("Shader backend", selection: self.$shaderBackend.rawValue) {
                        Text("MSL (recommended)").tag(HYDRA_SHADER_BACKEND_MSL.rawValue)
                        Text("AIR (broken)").tag(HYDRA_SHADER_BACKEND_AIR.rawValue)
                    }
                    .onChange(of: self.shaderBackend.rawValue) { _, newValue in
                        configGetShaderBackend().pointee = newValue
                    }

                    Picker("Metal API", selection: self.$metalApi.rawValue) {
                        Text("Metal 3").tag(HYDRA_METAL_API_METAL3.rawValue)
                        Text("Metal 4 (experimental)").tag(HYDRA_METAL_API_METAL4.rawValue)
                    }
                    .onChange(of: self.metalApi.rawValue) { _, newValue in
                        configGetMetalApi().pointee = newValue
                    }
                } footer: {
                    Text(
                        "Metal 4 encodes with explicit barriers, residency sets and argument tables, for less CPU time per draw. It needs macOS 26 or iOS 26 and falls back to Metal 3. Applies when a game boots."
                    )
                }

                Section {
                    Picker("Upscaling filter", selection: self.$upscalingFilter.rawValue) {
                        Text("Bilinear").tag(HYDRA_UPSCALING_FILTER_BILINEAR.rawValue)
                        Text("Nearest (pixel-sharp)").tag(
                            HYDRA_UPSCALING_FILTER_NEAREST.rawValue)
                        Text("MetalFX Spatial").tag(HYDRA_UPSCALING_FILTER_METALFX.rawValue)
                    }
                    .onChange(of: self.upscalingFilter.rawValue) { _, newValue in
                        configGetUpscalingFilter().pointee = newValue
                    }
                    VStack(alignment: .leading) {
                        Slider(value: self.$sharpening, in: 0...100, step: 5) {
                            Text("Sharpening")
                        }
                        .onChange(of: self.sharpening) { _, newValue in
                            configGetSharpening().pointee = UInt32(newValue)
                        }
                        Text(
                            self.sharpening == 0
                                ? "Off" : "\(Int(self.sharpening))%"
                        )
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    }
                } header: {
                    Text("Upscaling")
                } footer: {
                    Text(
                        "Only changes how the final image is scaled to the window, not the emulation. Games choose their own rendering resolution, as on a Switch: 1080p display when docked, 720p in Handheld mode (System), which renders fewer pixels."
                    )
                }

                Section {
                    Picker("Frame rate limit", selection: self.$frameRateLimit) {
                        Text("60 (console)").tag(UInt32(60))
                        Text("90").tag(UInt32(90))
                        Text("120").tag(UInt32(120))
                        Text("Unlimited").tag(UInt32(0))
                    }
                    .onChange(of: self.frameRateLimit) { _, newValue in
                        configGetFrameRateLimit().pointee = newValue
                    }
                } header: {
                    Text("Frame rate")
                } footer: {
                    Text(
                        "Above 60, the game's display refreshes faster than a Switch's and the window shows the newest frame, for lower input latency. Games that time themselves by display refreshes run too fast. Applies when a game boots."
                    )
                }

                Section {
                    Picker("Frame generation", selection: self.$frameGeneration.rawValue) {
                        Text("Off").tag(HYDRA_FRAME_GENERATION_OFF.rawValue)
                        Text("Interpolate").tag(HYDRA_FRAME_GENERATION_INTERPOLATE.rawValue)
                        Text("MetalFX Frame Interpolation (experimental)").tag(
                            HYDRA_FRAME_GENERATION_METALFX.rawValue)
                    }
                    .onChange(of: self.frameGeneration.rawValue) { _, newValue in
                        configGetFrameGeneration().pointee = newValue
                    }
                } header: {
                    Text("Frame generation")
                } footer: {
                    Text(
                        "Fills display refreshes the game has no frame for with frames interpolated from the motion between its last two frames, e.g. a 30 FPS game shown at 60, or a 60 FPS game at 120 on ProMotion displays. Each game frame is shown one frame later. MetalFX needs macOS 26 or iOS 26 and falls back to Interpolate."
                    )
                }

                Section {
                    Toggle("Metal performance HUD", isOn: self.$metalHud)
                        .onChange(of: self.metalHud) { _, newValue in
                            configGetMetalHud().pointee = newValue
                        }
                } header: {
                    Text("Diagnostics")
                }
            }
            .formStyle(.grouped)
            Spacer()
        }
        Spacer()
    }
}
