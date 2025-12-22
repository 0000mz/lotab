import SwiftUI

// NOTE: For ensuring that the application has focus on open.
// Might not be necessary -- seems the issues I am facing might just be
// a quark of executing the binary from the terminal.
class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)
        DispatchQueue.main.async {
            NSRunningApplication.current.activate(options: .activateIgnoringOtherApps)
            NSApp.windows.first?.makeKeyAndOrderFront(nil)
        }
    }
}

@main
struct TabManagerApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate

    var body: some Scene {
        WindowGroup {
            ContentView()
        }
    }
}
