import SwiftUI

#if HYDRA_DEBUG
    let debugLoggingEnabled = true
#else
    let debugLoggingEnabled = false
#endif

struct DebugSettingsView: View {
    @State private var logOutput = HydraLogOutput(rawValue: configGetLogOutput().pointee)
    @State private var logFsAccess = configGetLogFsAccess().pointee
    @State private var debugLogging = configGetDebugLogging().pointee

    // TODO: process args

    @State private var recoverFromSegfault = configGetRecoverFromSegfault().pointee

    @State private var gdbEnabled = configGetGdbEnabled().pointee
    @State private var gdbPort = configGetGdbPort().pointee
    @State private var gdbWaitForClient = configGetGdbWaitForClient().pointee

    @State private var displayResolution = HydraResolution(
        rawValue: configGetDisplayResolution().pointee)
    @State private var customDisplayResolution = configGetCustomDisplayResolution().pointee

    var body: some View {
        Spacer()
        HStack {
            Spacer()
            Form {
                Section("Logging") {
                    Picker("Log output", selection: self.$logOutput.rawValue) {
                        Text("none (not recommended)")
                            .tag(HYDRA_LOG_OUTPUT_NONE.rawValue)
                        Text("stdout")
                            .tag(HYDRA_LOG_OUTPUT_STD_OUT.rawValue)
                        Text("file (default)")
                            .tag(HYDRA_LOG_OUTPUT_FILE.rawValue)
                    }
                    .onChange(of: self.logOutput.rawValue) { _, newValue in
                        configGetLogOutput().pointee = newValue
                    }

                    Toggle("Log filesystem access", isOn: self.$logFsAccess)
                        .onChange(of: self.logFsAccess) { _, newValue in
                            configGetLogFsAccess().pointee = newValue
                        }

                    Toggle("Debug logging", isOn: self.$debugLogging)
                        .disabled(!debugLoggingEnabled)
                        .onChange(of: self.debugLogging) { _, newValue in
                            configGetDebugLogging().pointee = newValue
                        }
                }

                // TODO: process arguments

                Section {
                    Picker("Display resolution", selection: self.$displayResolution.rawValue) {
                        Text("Console (default)").tag(HYDRA_RESOLUTION_CONSOLE.rawValue)
                        Text("720p").tag(HYDRA_RESOLUTION_720P.rawValue)
                        Text("1080p").tag(HYDRA_RESOLUTION_1080P.rawValue)
                        Text("1440p").tag(HYDRA_RESOLUTION_1440P.rawValue)
                        Text("2160p").tag(HYDRA_RESOLUTION_2160P.rawValue)
                        Text("4320p").tag(HYDRA_RESOLUTION_4320P.rawValue)
                        Text("Window size").tag(HYDRA_RESOLUTION_AUTO.rawValue)
                        Text("Window size, exact").tag(HYDRA_RESOLUTION_AUTO_EXACT.rawValue)
                        Text("Custom").tag(HYDRA_RESOLUTION_CUSTOM.rawValue)
                    }
                    .onChange(of: self.displayResolution.rawValue) { _, newValue in
                        configGetDisplayResolution().pointee = newValue
                    }

                    if self.displayResolution == HYDRA_RESOLUTION_CUSTOM {
                        TextField(
                            "Width", value: self.$customDisplayResolution.x,
                            format: .number)
                        TextField(
                            "Height", value: self.$customDisplayResolution.y,
                            format: .number
                        )
                        .onChange(of: self.customDisplayResolution) { _, newValue in
                            configGetCustomDisplayResolution().pointee = newValue
                        }
                    }
                } header: {
                    Text("Display")
                } footer: {
                    Text(
                        "The display resolution reported to games when docked. A Switch reports 1080p; other values can make games misbehave. Handheld mode always reports 720p."
                    )
                }

                Section("Error handling") {
                    Toggle("Recover from segfault", isOn: self.$recoverFromSegfault)
                        .onChange(of: self.recoverFromSegfault) { _, newValue in
                            configGetRecoverFromSegfault().pointee = newValue
                        }
                }

                Section("GDB") {
                    Toggle("Enabled", isOn: self.$gdbEnabled)
                        .onChange(of: self.gdbEnabled) { _, newValue in
                            configGetGdbEnabled().pointee = newValue
                        }
                    if (self.gdbEnabled) {
                        TextField("Port", value: self.$gdbPort, formatter: NumberFormatter())
                            .onChange(of: self.gdbPort) { _, newValue in
                                configGetGdbPort().pointee = newValue
                            }
                        Toggle("Wait for client", isOn: self.$gdbWaitForClient)
                            .onChange(of: self.gdbWaitForClient) { _, newValue in
                                configGetGdbWaitForClient().pointee = newValue
                            }
                    }
                }
            }
            .formStyle(.grouped)
            .onAppear {
                // TODO: process args
            }
            Spacer()
        }
        Spacer()
    }
}
