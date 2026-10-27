import AppKit
import Foundation

class AppDelegate: NSObject, NSApplicationDelegate {
    weak var globalState: GlobalState?

    func applicationWillTerminate(_ notification: Notification) {
        // Stop workers before C++ globals (debuggers, MMU state) are finalized.
        globalState?.system?.forceStop()
        globalState?.system = nil
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        return true
    }
}
