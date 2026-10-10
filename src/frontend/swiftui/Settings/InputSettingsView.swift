import SwiftUI

#if HYDRA_SDL_ENABLED
    let sdlEnabled = true
#else
    let sdlEnabled = false
#endif

struct InputSettingsView: View {
    @EnvironmentObject var globalState: GlobalState
    @State private var inputBackend = HydraInputBackend(rawValue: configGetInputBackend().pointee)
    @State private var connectedDevices: [String] = []
    @State private var playerDevices = Array(repeating: [String](), count: 8)
    @State private var errorMessage = ""
    @State private var showingError = false

    private var gameRunning: Bool { globalState.system?.isRunning() == true }

    var body: some View {
        Form {
            Section {
                Picker("Input backend", selection: self.$inputBackend.rawValue) {
                    Text("Apple GameController (recommended)")
                        .tag(HYDRA_INPUT_BACKEND_APPLE_GAME_CONTROLLER.rawValue)
                    Text("SDL")
                        .tag(HYDRA_INPUT_BACKEND_SDL.rawValue)
                        .selectionDisabled(!sdlEnabled)
                }
                .disabled(gameRunning)
                .onChange(of: self.inputBackend.rawValue) { _, newValue in
                    configGetInputBackend().pointee = newValue
                    configSerialize()
                    refreshDevices()
                }
            }

            Section("Players") {
                ForEach(0..<8, id: \.self) { player in
                    HStack {
                        Text("Player \(player + 1)")
                        Spacer()
                        Menu {
                            Button("None") { assign([], to: player) }
                            ForEach(deviceOptions(for: player), id: \.self) { device in
                                Button {
                                    var selected = playerDevices[player]
                                    if selected.contains(device) {
                                        selected.removeAll { $0 == device }
                                    } else {
                                        selected.append(device)
                                    }
                                    assign(selected, to: player)
                                } label: {
                                    if playerDevices[player].contains(device) {
                                        Label(deviceLabel(device), systemImage: "checkmark")
                                    } else {
                                        Text(deviceLabel(device))
                                    }
                                }
                            }
                        } label: {
                            Text(playerDevices[player].isEmpty
                                 ? "None" : playerDevices[player].map(deviceLabel).joined(separator: ", "))
                                .lineLimit(2)
                                .multilineTextAlignment(.trailing)
                        }
                        .disabled(gameRunning)
                        .accessibilityLabel("Player \(player + 1) devices")
                    }
                }
            }

            Section {
                Text(gameRunning
                     ? "Stop the game to change player assignments."
                     : "Select a controller or keyboard for each player. Assigning a device moves it from its previous player. Changes are saved automatically.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                if connectedDevices.isEmpty {
                    Text("No input devices connected. Connect a controller, then refresh.")
                        .foregroundStyle(.secondary)
                }
                Button("Refresh devices", systemImage: "arrow.clockwise") { refreshDevices() }
            }
        }
        .formStyle(.grouped)
        .onAppear {
            refreshPlayers()
            refreshDevices()
        }
        .task {
            while !Task.isCancelled {
                do { try await Task.sleep(for: .seconds(2)) }
                catch { break }
                refreshDevices()
            }
        }
        .alert("Unable to update controller settings", isPresented: $showingError) {
            Button("OK", role: .cancel) {}
        } message: { Text(errorMessage) }
    }

    private func deviceLabel(_ device: String) -> String {
        connectedDevices.contains(device) ? device : "\(device) (disconnected)"
    }

    private func deviceOptions(for player: Int) -> [String] {
        Array(Set(connectedDevices + playerDevices[player])).sorted()
    }

    private func refreshDevices() {
        do { connectedDevices = try inputDeviceNames(system: globalState.system) }
        catch { report(error) }
    }

    private func refreshPlayers() {
        do { playerDevices = try (0..<8).map { try inputPlayerDevices($0) } }
        catch { report(error) }
    }

    private func assign(_ devices: [String], to player: Int) {
        do {
            try inputSetPlayerDevices(player, devices: devices, system: globalState.system)
            refreshPlayers()
        } catch { report(error) }
    }

    private func report(_ error: Error) {
        errorMessage = error.localizedDescription
        showingError = true
    }
}
