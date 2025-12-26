import SwiftUI
import Foundation

// NOTE: For ensuring that the application has focus on open.
// Might not be necessary -- seems the issues I am facing might just be
// a quark of executing the binary from the terminal.
class AppDelegate: NSObject, NSApplicationDelegate {
    private var serverSocket: Int32 = -1
    private var activeClientSocket: Int32 = -1
    private let socketPath = "/tmp/tabmanager.sock"

    static var shared: AppDelegate?
    var window: FocusableWindow!

    func applicationDidFinishLaunching(_ notification: Notification) {
        AppDelegate.shared = self
        // Start as a background/accessory app
        NSApp.setActivationPolicy(.accessory)

        // Create the window manually
        let contentView = ContentView(tabManager: TabManager.shared)

        // Create a borderless window with no style mask initially
        window = FocusableWindow(
            contentRect: NSRect(x: 0, y: 0, width: 600, height: 500),
            styleMask: [.borderless],
            backing: .buffered,
            defer: false
        )

        window.center()
        window.contentView = NSHostingView(rootView: contentView)

        // Configure Window Properties
        window.isMovable = false
        window.isMovableByWindowBackground = false
        window.titlebarAppearsTransparent = true
        window.titleVisibility = .hidden
        window.isOpaque = false
        window.backgroundColor = .clear
        window.level = .floating
        window.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]

        // Setup Key monitors
        NSEvent.addLocalMonitorForEvents(matching: .keyDown) { (event: NSEvent) -> NSEvent? in
            let tm = TabManager.shared

            // Toggle Filter Mode OR Handle characters
            if tm.isFiltering {
                if event.keyCode == 53 { // ESC: Cancel filter
                    tm.isFiltering = false
                    tm.filterText = ""
                    return nil
                }
                if event.keyCode == 36 { // Enter: Confirm filter
                    tm.isFiltering = false
                    return nil
                }
                if event.keyCode == 51 { // Backspace
                    if !tm.filterText.isEmpty {
                        tm.filterText.removeLast()
                    }
                    return nil
                }

                // Handle typing (allow navigation keys to pass through)
                if event.keyCode != 126 && event.keyCode != 125 { // Not arrows
                    if let chars = event.characters, !chars.isEmpty, !event.modifierFlags.contains(.command) {
                        tm.filterText += chars
                        return nil
                    }
                }
            } else {
                // Normal Mode
                if event.keyCode == 44 { // Slash: Start filter
                    tm.isFiltering = true
                    tm.filterText = ""
                    return nil
                }
                if event.keyCode == 53 { // ESC: Close UI
                    self.hideUI()
                    return nil
                }
            }

            // Navigation Logic
            // UP: Arrow (126) or (K/40 if not filtering)
            let isK = event.keyCode == 40
            if event.keyCode == 126 || (isK && !tm.isFiltering) {
                let tabs = tm.displayedTabs
                if !tabs.isEmpty {
                    if let sel = tm.selection, let idx = tabs.firstIndex(where: { $0.id == sel }) {
                        if idx == 0 {
                            tm.selection = tabs.last?.id
                            return nil
                        } else if isK {
                            tm.selection = tabs[idx - 1].id
                            return nil
                        }
                    } else {
                        tm.selection = tabs.first?.id
                        return nil
                    }
                }
            }

            // DOWN: Arrow (125) or (J/38 if not filtering)
            let isJ = event.keyCode == 38
            if event.keyCode == 125 || (isJ && !tm.isFiltering) {
                let tabs = tm.displayedTabs
                if !tabs.isEmpty {
                    if let sel = tm.selection, let idx = tabs.firstIndex(where: { $0.id == sel }) {
                        if idx == tabs.count - 1 {
                            tm.selection = tabs.first?.id
                            return nil
                        } else if isJ {
                            tm.selection = tabs[idx + 1].id
                            return nil
                        }
                    } else {
                        tm.selection = tabs.first?.id
                        return nil
                    }
                }
            }

            // Confirm Selection (Entered Normal Mode)
            if !tm.isFiltering && event.keyCode == 36 {
                if let selectedId = TabManager.shared.selection {
                     self.sendUDSMessage(event: "tab_selected", data: ["tabId": selectedId])
                     self.hideUI()
                     return nil
                }
            }
            return event
        }

        startUDSServer()

        // Explicitly hide initially
        DispatchQueue.main.async {
            self.hideUI()
        }
    }

    func applicationDidResignActive(_ notification: Notification) {
        hideUI()
    }

    private func showUI() {
        DispatchQueue.main.async {
            // Center the window
            if let screen = NSScreen.main {
                let screenRect = screen.visibleFrame
                let windowRect = self.window.frame

                let x = screenRect.origin.x + (screenRect.width - windowRect.width) / 2
                let y = screenRect.origin.y + (screenRect.height - windowRect.height) / 2

                self.window.setFrameOrigin(NSPoint(x: x, y: y))
            }

            // Show and focus
            self.window.makeKeyAndOrderFront(nil)
            self.window.orderFrontRegardless()
            NSApp.activate(ignoringOtherApps: true)
        }
    }

    func hideUI() {
        vlog_s(.trace, TabManagerApp.appClass, "hiding ui")
        DispatchQueue.main.async {
            self.window.orderOut(nil)
            NSApp.hide(nil)
        }

        // Reset State on Open
        TabManager.shared.isFiltering = false
        TabManager.shared.filterText = ""
        TabManager.shared.selection = TabManager.shared.displayedTabs.first?.id
    }

    private func startUDSServer() {
        unlink(socketPath)
        serverSocket = socket(AF_UNIX, SOCK_STREAM, 0)
        vlog_s(.info, TabManagerApp.appClass, "Attempting to connect to UDS server")
        guard serverSocket >= 0 else {
            vlog_s(.error, TabManagerApp.appClass, "Failed to create socket")
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
            vlog_s(.error, TabManagerApp.appClass, "Failed to bind socket. Error: \(String(cString: strerror(errno)))");
            return
        }

        guard Darwin.listen(serverSocket, 5) == 0 else {
            vlog_s(.error, TabManagerApp.appClass, "Failed to listen on socket");
            return
        }

        vlog_s(.info, TabManagerApp.appClass, "UDS server started at \(socketPath)");
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
                self.activeClientSocket = clientSocket
                vlog_s(.info, TabManagerApp.appClass, "Accepted new UDS connection")
                Thread.detachNewThread {
                    self.handleClient(clientSocket)
                }
            }
        }
    }

    private func handleClient(_ clientSocket: Int32) {
        defer {
            close(clientSocket)
        }

        while true {
            // 1. Read Header (4 bytes)
            var headerData = Data(count: 4)
            let headerBytesRead = headerData.withUnsafeMutableBytes { buffer in
                 return read(clientSocket, buffer.baseAddress, 4)
            }

            if headerBytesRead == 0 {
                vlog_s(.info, TabManagerApp.appClass, "UDS connection closed by peer")
                break
            } else if headerBytesRead < 0 {
                vlog_s(.error, TabManagerApp.appClass, "UDS header read error: \(String(cString: strerror(errno)))")
                break
            } else if headerBytesRead < 4 {
                 vlog_s(.error, TabManagerApp.appClass, "UDS partial header read")
                 break
            }

            let msgLen = headerData.withUnsafeBytes { $0.load(as: UInt32.self).littleEndian }

            // 2. Read Payload
            var payloadData = Data(count: Int(msgLen))
            var totalRead = 0
            var readError = false

            payloadData.withUnsafeMutableBytes { buffer in
                while totalRead < Int(msgLen) {
                    let n = read(clientSocket, buffer.baseAddress! + totalRead, Int(msgLen) - totalRead)
                    if n <= 0 {
                        readError = true
                        break
                    }
                    totalRead += n
                }
            }

            if readError {
                 vlog_s(.error, TabManagerApp.appClass, "UDS payload read error or closed prematurely")
                 break
            }

            // 3. Process Message
            if let message = String(data: payloadData, encoding: .utf8) {
                vlog_s(.trace, TabManagerApp.appClass, "uds-read: \(message)")
                if message.contains("tabs_update") {
                    // Decode tabs
                    if let jsonData = message.data(using: .utf8) {
                        do {
                            let payload = try JSONDecoder().decode(TabListPayload.self, from: jsonData)
                            vlog_s(.info, TabManagerApp.appClass, "Successfully decoded \(payload.data.tabs.count) tabs")
                            DispatchQueue.main.async {
                                TabManager.shared.tabs = payload.data.tabs
                            }
                        } catch {
                            vlog_s(.error, TabManagerApp.appClass, "JSON Decoding Error for tabs_update: \(error)")
                        }
                    }
                } else if message.contains("tasks_update") {
                    // Decode tasks
                    if let jsonData = message.data(using: .utf8) {
                        do {
                            let payload = try JSONDecoder().decode(TaskListPayload.self, from: jsonData)
                            vlog_s(.info, TabManagerApp.appClass, "Successfully decoded \(payload.data.tasks.count) tasks")
                            DispatchQueue.main.async {
                                TabManager.shared.tasks = payload.data.tasks
                            }
                        } catch {
                            vlog_s(.error, TabManagerApp.appClass, "JSON Decoding Error for tasks_update: \(error)")
                        }
                    }
                } else if message.contains("ui_visibility_toggle") {
                    showUI()
                }
            }
        }
    }

    func sendUDSMessage(event: String, data: Any) {
        guard activeClientSocket >= 0 else {
            vlog_s(.error, TabManagerApp.appClass, "Cannot send message: No active UDS connection")
            return
        }

        let messageParams: [String: Any] = [
            "event": event,
            "data": data
        ]

        do {
            let jsonData = try JSONSerialization.data(withJSONObject: messageParams, options: [])
            var length = UInt32(jsonData.count).littleEndian
            let lengthData = Data(bytes: &length, count: MemoryLayout<UInt32>.size)

            let combinedData = lengthData + jsonData

            let result = combinedData.withUnsafeBytes { buffer -> Int in
                guard let baseAddress = buffer.baseAddress else { return -1 }
                return Int(write(activeClientSocket, baseAddress, buffer.count))
            }

            if result < 0 {
                vlog_s(.error, TabManagerApp.appClass, "Failed to send UDS message: \(String(cString: strerror(errno)))")
            } else {
                vlog_s(.info, TabManagerApp.appClass, "uds-send: \(event) (len: \(jsonData.count))")
            }
        } catch {
            vlog_s(.error, TabManagerApp.appClass, "Failed to serialize UDS message: \(error)")
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

struct Task: Identifiable, Decodable, Hashable {
    let id: Int
    let name: String
}

struct TaskListPayload: Decodable {
    struct Data: Decodable {
        let tasks: [Task]
    }
    let data: Data
}

class TabManager: ObservableObject {
    static let shared = TabManager()
    @Published var tabs: [Tab] = []
    @Published var tasks: [Task] = []
    @Published var selection: Int?

    @Published var filterText: String = ""
    @Published var isFiltering: Bool = false

    var displayedTabs: [Tab] {
        let filtered = filterText.isEmpty ? tabs : tabs.filter { $0.title.localizedCaseInsensitiveContains(filterText) }
        let active = filtered.filter { $0.active }
        let other = filtered.filter { !$0.active }
        return active + other
    }
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

class FocusableWindow: NSWindow {
    override var canBecomeKey: Bool { return true }
    override var canBecomeMain: Bool { return true }
}

@main
struct TabManagerApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate
    // Create a persistent EngClass for the App context
    static let appClass: UnsafeMutablePointer<EngClass> = {
        let ptr = UnsafeMutablePointer<EngClass>.allocate(capacity: 1)
        // Manually duplicate string for C memory management consistency
        ptr.pointee.name = strdup("app")
        return ptr
    }()

    init() {
        let args = ProcessInfo.processInfo.arguments
        if let index = args.firstIndex(of: "--log-level"), index + 1 < args.count {
            if let level = Int(args[index + 1]) {
                engine_set_log_level(Int32(level))
            }
        }
    }

    var body: some Scene {
        Settings {
            EmptyView()
        }
    }
}
