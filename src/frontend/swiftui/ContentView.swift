import SwiftUI

#if os(iOS)
    import GameController
#endif

struct ContentView: View {
    @EnvironmentObject var globalState: GlobalState

    @AppStorage("viewMode") private var viewMode: ViewMode = .list
    @State private var fps = FrameRate()

    #if os(iOS)
        @State private var virtualController: GCVirtualController? = nil
    #endif

    private var appVersion: String {
        Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?"
    }

    private var gitVersion: String {
        Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "?"
    }

    private var navigationTitle: String {
        let gameTitle = globalState.activeGame?.name ?? ""
        var titleInsert = ""
        if globalState.activeGame?.name != nil { titleInsert = "  |  \(gameTitle)" }
        var gitInsert = ""
        if gitVersion != "" { gitInsert = " (\(gitVersion))" }
        var fpsInsert = ""
        if globalState.system != nil {
            // With generated frames on screen, the game's own rate too
            fpsInsert =
                fps.shown > fps.game + 1
                ? "  |  \(fps.shown) FPS (\(fps.game) game)" : "  |  \(fps.shown) FPS"
        }
        return "Hydra v\(appVersion)\(gitInsert)\(titleInsert)\(fpsInsert)"
    }

    var body: some View {
        NavigationStack {
            GameListView(viewMode: $viewMode)
                .navigationDestination(item: $globalState.activeGame) { activeGame in
                    EmulationView(fps: $fps)
                        .aspectRatio(CGSize(width: 16, height: 9), contentMode: .fit)
                        // Letterbox in black, like a TV, when the window is
                        // not 16:9 (e.g. full screen on a 16:10 display)
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                        .background(Color.black.ignoresSafeArea())
                        #if os(iOS)
                            .onAppear {
                                // TODO: if virtual controller enabled
                                if true {
                                    let config = GCVirtualController.Configuration()
                                    config.elements = [
                                        GCInputButtonA, GCInputButtonB, GCInputButtonX,
                                        GCInputButtonY,
                                        GCInputLeftThumbstick, GCInputRightThumbstick,
                                        GCInputLeftShoulder,
                                        GCInputRightShoulder, GCInputLeftTrigger,
                                        GCInputRightTrigger,
                                    ]

                                    let virtualController = GCVirtualController(
                                        configuration: config)
                                    virtualController.connect()

                                    self.virtualController = virtualController
                                }
                            }
                            .onDisappear {
                                virtualController?.disconnect()
                                virtualController = nil
                            }
                        #endif
                        .toolbar {
                            EmulationToolbarItems()
                        }
                }
                .toolbar {
                    GameListToolbarItems(viewMode: $viewMode)
                }
                .navigationBarBackButtonHidden()
        }
        #if os(macOS)
            .windowToolbarFullScreenVisibility(.onHover)
            .background(FillScreenOnAppear())
        #endif
        .navigationTitle(navigationTitle)
    }
}

#if os(macOS)
    /// Sizes the window to the screen (between the menu bar and the Dock) when
    /// it opens, without entering full screen
    struct FillScreenOnAppear: NSViewRepresentable {
        final class FillScreenView: NSView {
            private var filled = false

            override func viewDidMoveToWindow() {
                super.viewDidMoveToWindow()
                guard !filled, let window = window else { return }
                filled = true
                // After AppKit restored the frame of the previous session
                DispatchQueue.main.async {
                    guard let screen = window.screen ?? NSScreen.main else { return }
                    window.setFrame(screen.visibleFrame, display: true)
                }
            }
        }

        func makeNSView(context: Context) -> NSView { FillScreenView() }

        func updateNSView(_ nsView: NSView, context: Context) {}
    }
#endif
