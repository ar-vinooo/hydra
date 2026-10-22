import MetalKit
import SwiftUI

// The frame rate in the window title
struct FrameRate: Equatable {
    // New images per second on screen: game frames and generated ones
    var shown = 0
    // The game's own frames per second
    var game = 0
}

class MetalLayerCoordinator: NSObject {
    @Binding var system: HydraSystem?
    @Binding var fps: FrameRate

    private var layer: CAMetalLayer? = nil
    private var displayLink: CADisplayLink? = nil
    private var surfaceSet = false
    private var hudShown: Bool? = nil

    init(system: Binding<HydraSystem?>, fps: Binding<FrameRate>) {
        self._system = system
        self._fps = fps
        super.init()
    }

    deinit {
        self.displayLink?.invalidate()
    }

    #if os(macOS)
        func setView(_ view: NSView) {
            self.displayLink?.invalidate()
            self.layer = view.layer as? CAMetalLayer
            self.surfaceSet = false
            self.hudShown = nil

            // Display link
            self.displayLink = view.displayLink(
                target: self, selector: #selector(handleDisplayLink))
            self.displayLink?.add(to: .main, forMode: .common)
        }
    #else
        func setView(_ view: UIView) {
            self.displayLink?.invalidate()
            self.layer = view.layer as? CAMetalLayer
            self.surfaceSet = false
            self.hudShown = nil

            // Display link
            self.displayLink = CADisplayLink(target: self, selector: #selector(handleDisplayLink))
            // Refresh as fast as the display can (ProMotion), for frame
            // generation and frame rate limits above 60
            self.displayLink?.preferredFrameRateRange = CAFrameRateRange(
                minimum: 60, maximum: 120, preferred: 120)
            self.displayLink?.add(to: .main, forMode: .commonModes)
        }
    #endif

    @objc func handleDisplayLink() {
        // Metal performance HUD
        let showHud = configGetMetalHud().pointee
        if showHud != self.hudShown, let layer = self.layer {
            layer.developerHUDProperties = [
                "mode": showHud ? "default" : "disabled"
            ]
            self.hudShown = showHud
        }

        if let system = system {
            // Set the surface if its not already set
            if !self.surfaceSet {
                guard let layer = self.layer else {
                    return
                }
                system.surface = Unmanaged.passUnretained(layer).toOpaque()
                self.surfaceSet = true
            }

            // Present
            var dtAverageUpdated = false
            system.progressFrame(
                width: UInt32(self.layer!.drawableSize.width),
                height: UInt32(self.layer!.drawableSize.height),
                dtAverageUpdated: &dtAverageUpdated)

            // Update
            if dtAverageUpdated {
                let dt = system.getLastDeltaTimeAverage()
                let game = dt != 0.0 ? Int((1.0 / dt).rounded()) : 0
                // Shown counts what the display really showed; before the
                // display reports anything, the game's rate stands in
                let shown = Int(system.getLastShownFps().rounded())
                fps = FrameRate(shown: shown != 0 ? shown : game, game: game)
            }
        }
    }
}

#if os(macOS)
    struct MetalView: NSViewRepresentable {
        @EnvironmentObject var globalState: GlobalState

        @Binding var fps: FrameRate

        func makeNSView(context: Context) -> NSView {
            let view = NSView(frame: .zero)
            let layer = CAMetalLayer()

            layer.displaySyncEnabled = true
            layer.contentsScale = NSScreen.main?.backingScaleFactor ?? 2.0
            view.layer = layer
            view.wantsLayer = true
            context.coordinator.setView(view)

            return view
        }

        func updateNSView(_ view: NSView, context: Context) {
            guard let layer = view.layer as? CAMetalLayer else { return }
            layer.frame = view.bounds
            context.coordinator.setView(view)
        }

        func makeCoordinator() -> MetalLayerCoordinator {
            return MetalLayerCoordinator(
                system: $globalState.system, fps: self.$fps)
        }
    }
#else
    class MetalBackedView: UIView {
        override class var layerClass: AnyClass {
            return CAMetalLayer.self
        }

        override func didMoveToWindow() {
            super.didMoveToWindow()
            if let metalLayer = layer as? CAMetalLayer {
                metalLayer.contentsScale = window?.screen.scale ?? UIScreen.main.scale
            }
        }
    }

    struct MetalView: UIViewRepresentable {
        @EnvironmentObject var globalState: GlobalState

        @Binding var fps: FrameRate

        func makeUIView(context: Context) -> UIView {
            let view = MetalBackedView(frame: .zero)
            context.coordinator.setView(view)
            return view
        }

        func updateUIView(_ view: UIView, context: Context) {
            if let layer = view.layer.sublayers?.first as? CAMetalLayer {
                layer.frame = view.bounds
                layer.contentsScale = view.window?.screen.scale ?? UIScreen.main.scale
            }
            context.coordinator.setView(view)
        }

        func makeCoordinator() -> MetalLayerCoordinator {
            return MetalLayerCoordinator(system: $globalState.system, fps: $fps)
        }
    }
#endif
