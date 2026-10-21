import SwiftUI

struct GraphicsSettingsView: View {
    @State private var gpuRenderer = HydraGpuRenderer(rawValue: configGetGpuRenderer().pointee)
    @State private var shaderBackend = HydraShaderBackend(rawValue: configGetShaderBackend().pointee)
    @State private var upscalingFilter = HydraUpscalingFilter(
        rawValue: configGetUpscalingFilter().pointee)

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
                } header: {
                    Text("Upscaling")
                } footer: {
                    Text(
                        "Only changes how the final image is scaled to the window, not the emulation. Games choose their own rendering resolution, as on a Switch: 1080p display when docked, 720p in Handheld mode (System), which renders fewer pixels."
                    )
                }
            }
            .formStyle(.grouped)
            Spacer()
        }
        Spacer()
    }
}
