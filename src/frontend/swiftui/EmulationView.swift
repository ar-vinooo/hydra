import SwiftUI

struct EmulationView: View {
    @EnvironmentObject var globalState: GlobalState

    @Binding var fps: FrameRate
    @State private var showingStartupError = false
    @State private var startupError = ""

    var body: some View {
        ZStack {
            MetalView(fps: $fps)
                .onAppear {
                    do {
                        let system = try HydraSystem()
                        globalState.system = system
                        try system.loadAndStart(loader: globalState.activeGame!.loader)
                    } catch {
                        globalState.system = nil
                        startupError = error.localizedDescription
                        showingStartupError = true
                    }
                }
                .onDisappear {
                    fps = FrameRate()
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
