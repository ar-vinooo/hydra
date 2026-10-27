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
    @State private var frameGenerationRateMode = HydraFrameGenerationRateMode(
        rawValue: configGetFrameGenerationRateMode().pointee)
    @State private var frameGenerationTargetFps = configGetFrameGenerationTargetFps().pointee
    @State private var frameGenerationMultiplier = configGetFrameGenerationMultiplier().pointee
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
                    Picker("Native render FPS limit", selection: self.$frameRateLimit) {
                        Text("60 (console)").tag(UInt32(60))
                        Text("90").tag(UInt32(90))
                        Text("120").tag(UInt32(120))
                        Text("Unlimited").tag(UInt32(0))
                    }
                    .onChange(of: self.frameRateLimit) { _, newValue in
                        configGetFrameRateLimit().pointee = newValue
                    }
                } header: {
                    Text("Native rendering")
                } footer: {
                    Text(
                        "Limits the game's own frames, independently of frame generation. For example, 60 native FPS with Multiple 2× targets 120 output FPS. Above 60, games that time themselves by console display refreshes can run too fast. Applies when a game boots."
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
                    Picker("Output mode", selection: self.$frameGenerationRateMode.rawValue) {
                        Text("Variable (target FPS)").tag(HYDRA_FRAME_GENERATION_RATE_VARIABLE.rawValue)
                        Text("Multiple (native FPS × multiplier)").tag(HYDRA_FRAME_GENERATION_RATE_MULTIPLE.rawValue)
                    }
                    .onChange(of: self.frameGenerationRateMode.rawValue) { _, newValue in
                        configGetFrameGenerationRateMode().pointee = newValue
                    }
                    .disabled(self.frameGeneration.rawValue == HYDRA_FRAME_GENERATION_OFF.rawValue)

                    if self.frameGenerationRateMode.rawValue == HYDRA_FRAME_GENERATION_RATE_VARIABLE.rawValue {
                        TextField("Target output FPS", value: self.$frameGenerationTargetFps, format: .number)
                            .onChange(of: self.frameGenerationTargetFps) { _, newValue in
                                self.frameGenerationTargetFps = max(newValue, 1)
                                configGetFrameGenerationTargetFps().pointee = self.frameGenerationTargetFps
                            }
                            .disabled(self.frameGeneration.rawValue == HYDRA_FRAME_GENERATION_OFF.rawValue)
                    } else {
                        TextField("Multiplier", value: self.$frameGenerationMultiplier, format: .number)
                            .onChange(of: self.frameGenerationMultiplier) { _, newValue in
                                self.frameGenerationMultiplier = max(newValue, 1)
                                configGetFrameGenerationMultiplier().pointee = self.frameGenerationMultiplier
                            }
                            .disabled(self.frameGeneration.rawValue == HYDRA_FRAME_GENERATION_OFF.rawValue)
                    }
                } header: {
                    Text("Frame generation")
                } footer: {
                    Text(
                        "Variable targets the output FPS you enter. Multiple targets the current native FPS multiplied by your value: 60 FPS × 2 = 120 FPS. Neither target is capped to the screen refresh rate. A target below native FPS adds no frames. Actual throughput depends on rendering performance and the platform's presentation system. Each game frame is shown one frame later. MetalFX needs macOS 26 or iOS 26 and falls back to Interpolate. Applies immediately."
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
