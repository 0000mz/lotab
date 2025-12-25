import SwiftUI
import Foundation

// NOTE: For ensuring that the application has focus on open.
// Might not be necessary -- seems the issues I am facing might just be
// a quark of executing the binary from the terminal.
class AppDelegate: NSObject, NSApplicationDelegate {
    private var serverSocket: Int32 = -1
    private let socketPath = "/tmp/tabmanager.sock"

    func applicationDidFinishLaunching(_ notification: Notification) {
        // Start as a background/accessory app
        NSApp.setActivationPolicy(.accessory)

        // Setup ESC key monitor to hide the UI
        NSEvent.addLocalMonitorForEvents(matching: .keyDown) { event in
            if event.keyCode == 53 { // ESC key
                self.hideUI()
                return nil // Swallow the event
            }
            return event
        }

        startUDSServer()

        // Explicitly hide any windows that SwiftUI might have shown during startup
        DispatchQueue.main.async {
            self.hideUI()
        }
    }

    func applicationDidResignActive(_ notification: Notification) {
        hideUI()
    }

    private func showUI() {
        DispatchQueue.main.async {
            NSApp.activate(ignoringOtherApps: true)
            if let window = NSApp.windows.first {
                // Center the window
                if let screen = NSScreen.main {
                    let screenRect = screen.visibleFrame
                    let windowRect = window.frame

                    let x = screenRect.origin.x + (screenRect.width - windowRect.width) / 2
                    let y = screenRect.origin.y + (screenRect.height - windowRect.height) / 2

                    window.setFrameOrigin(NSPoint(x: x, y: y))
                }

                // Disable movement
                window.isMovable = false
                window.isMovableByWindowBackground = false

                // Hide window controls
                window.styleMask.remove([.titled, .closable, .miniaturizable, .resizable])
                window.titlebarAppearsTransparent = true
                window.titleVisibility = .hidden

                window.isOpaque = false
                window.backgroundColor = .clear

                window.level = .floating
                window.makeKeyAndOrderFront(nil)
            }
        }
    }

    private func hideUI() {
        print("App: Hiding UI")
        DispatchQueue.main.async {
            NSApp.hide(nil)
        }
    }

    private func startUDSServer() {
        unlink(socketPath)
        serverSocket = socket(AF_UNIX, SOCK_STREAM, 0)
        guard serverSocket >= 0 else {
            print("App: Failed to create socket")
            return
        }

        var addr = sockaddr_un()
        addr.sun_family = sa_family_t(AF_UNIX)
        let pathLen = socketPath.withCString { Int(strlen($0)) }
        _ = socketPath.withCString { src in
            withUnsafeMutablePointer(to: &addr.sun_path) { dest in
                let destPtr = UnsafeMutableRawPointer(dest).assumingMemoryBound(to: Int8.self)
                strncpy(destPtr, src, 104) // 104 is the max length for sun_path
            }
        }
        addr.sun_len = UInt8(MemoryLayout<sa_family_t>.size + MemoryLayout<UInt8>.size + pathLen + 1)

        let addrLen = socklen_t(addr.sun_len)
        let bindResult = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                Darwin.bind(serverSocket, $0, addrLen)
            }
        }

        guard bindResult == 0 else {
            print("App: Failed to bind socket. Error: \(String(cString: strerror(errno)))")
            return
        }

        guard Darwin.listen(serverSocket, 5) == 0 else {
            print("App: Failed to listen on socket")
            return
        }

        print("App: UDS Server started at \(socketPath)")

        Thread.detachNewThread {
            self.acceptConnections()
        }
    }

    private func acceptConnections() {
        while true {
            var clientAddr = sockaddr_un()
            var len = socklen_t(MemoryLayout<sockaddr_un>.size)
            let clientSocket = withUnsafeMutablePointer(to: &clientAddr) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    Darwin.accept(self.serverSocket, $0, &len)
                }
            }

            if clientSocket >= 0 {
                print("App: Accepted new UDS connection")
                Thread.detachNewThread {
                    self.handleClient(clientSocket)
                }
            }
        }
    }

    private func handleClient(_ clientSocket: Int32) {
        let bufferSize = 4096
        let buffer = UnsafeMutablePointer<UInt8>.allocate(capacity: bufferSize)
        var accumulator = Data()

        defer {
            buffer.deallocate()
            close(clientSocket)
        }

        while true {
            let bytesRead = read(clientSocket, buffer, bufferSize)
            if bytesRead > 0 {
                let data = Data(bytes: buffer, count: bytesRead)
                accumulator.append(data)

                while let range = accumulator.range(of: Data([0x0A])) { // Newline \n
                    let messageData = accumulator.subdata(in: 0..<range.lowerBound)
                    accumulator.removeSubrange(0..<range.upperBound)

                    if let message = String(data: messageData, encoding: .utf8) {
                        print("App: Received UDS message: \(message)")
                        if message.contains("tabs_update") {
                            // Decode tabs
                            if let jsonData = message.data(using: .utf8) {
                                do {
                                    let payload = try JSONDecoder().decode(TabListPayload.self, from: jsonData)
                                    print("App: Successfully decoded \(payload.data.tabs.count) tabs")
                                    DispatchQueue.main.async {
                                        TabManager.shared.tabs = payload.data.tabs
                                    }
                                } catch {
                                    print("App: JSON Decoding Error for tabs_update: \(error)")
                                }
                            }
                        } else if message.contains("ui_visibility_toggle") {
                            showUI()
                        }
                    }
                }
            } else if bytesRead == 0 {
                print("App: UDS connection closed by peer")
                break
            } else {
                print("App: UDS read error")
                break
            }
        }
    }
}

struct Tab: Identifiable, Decodable, Hashable {
    let id: Int
    let title: String
    let active: Bool
}

struct TabListPayload: Decodable {
    struct Data: Decodable {
        let tabs: [Tab]
    }
    let data: Data
}

class TabManager: ObservableObject {
    static let shared = TabManager()
    @Published var tabs: [Tab] = []
}

extension String {
    func appendLineTo(path: String) throws {
        let data = (self + "\n").data(using: .utf8)!
        if let fileHandle = FileHandle(forWritingAtPath: path) {
            fileHandle.seekToEndOfFile()
            fileHandle.write(data)
            fileHandle.closeFile()
        } else {
            try data.write(to: URL(fileURLWithPath: path), options: .atomic)
        }
    }
}

@main
struct TabManagerApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate

    var body: some Scene {
        WindowGroup {
            ContentView(tabManager: TabManager.shared)
        }
    }
}
