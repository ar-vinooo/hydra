import SwiftUI

struct EmulationView: View {
    @EnvironmentObject var globalState: GlobalState

    @Binding var fps: Int
    @State private var showingStartupError = false
    @State private var startupError = ""

    var body: some View {
        ZStack {
            MetalView(fps: $fps)
                .onAppear {
                    do {
                        let system = try HydraSystem()
                        globalState.system = system
                        system.loadAndStart(loader: globalState.activeGame!.loader)
                    } catch {
                        startupError = error.localizedDescription
                        showingStartupError = true
                    }
                }
                .onDisappear {
                    fps = 0
                }

            #if os(macOS)
                KeyCatcherView()
            #endif

            if globalState.isStopping {
                GameStopView()
            }
        }
        .alert("Unable to start game", isPresented: $showingStartupError) {
            Button("OK") {
                globalState.activeGame = nil
            }
        } message: {
            Text(startupError)
        }
    }
}
