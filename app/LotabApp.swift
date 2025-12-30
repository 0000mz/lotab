import Foundation
import SwiftUI

// NOTE: For ensuring that the application has focus on open.
// Might not be necessary -- seems the issues I am facing might just be
// a quark of executing the binary from the terminal.
class AppDelegate: NSObject, NSApplicationDelegate {
    private var serverSocket: Int32 = -1
    private var activeClientSocket: Int32 = -1
    private let socketPath = "/tmp/lotab.sock"

    static var shared: AppDelegate?
    var window: FocusableWindow!

    func applicationDidFinishLaunching(_ notification: Notification) {
        AppDelegate.shared = self
        // Start as a background/accessory app
        NSApp.setActivationPolicy(.accessory)

        // Create the window manually
        let contentView = ContentView(lotab: Lotab.shared)

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
            let tm = Lotab.shared

            // Marking Mode
            // Marking Mode
            if tm.isMarking {
                if tm.isCreatingLabel {
                    // --- CREATION INPUT MODE ---
                    if event.keyCode == 53 {  // ESC: Cancel creation, back to list
                        tm.isCreatingLabel = false
                        tm.markText = ""
                        return nil
                    }
                    if event.keyCode == 36 {  // Enter: Create Label
                        let label = tm.markText.trimmingCharacters(in: .whitespacesAndNewlines)
                        if !label.isEmpty {
                            if !tm.allLabels.contains(label) {
                                tm.allLabels.append(label)
                            }
                            // Apply to selected tabs
                            let targets =
                                tm.multiSelection.isEmpty
                                ? (tm.selection != nil ? [tm.selection!] : [])
                                : Array(tm.multiSelection)
                            for id in targets {
                                var labels = tm.tabLabels[id] ?? []
                                labels.insert(label)
                                tm.tabLabels[id] = labels
                            }
                        }
                        tm.isMarking = false
                        tm.isCreatingLabel = false
                        tm.markText = ""
                        return nil
                    }
                    if event.keyCode == 51 {  // Backspace
                        if !tm.markText.isEmpty {
                            tm.markText.removeLast()
                        }
                        return nil
                    }
                    if let chars = event.characters, !chars.isEmpty,
                        !event.modifierFlags.contains(.command) && event.keyCode != 51
                    {
                        tm.markText += chars
                        return nil
                    }
                } else {
                    // --- MENU SELECTION MODE ---
                    if event.keyCode == 53 {  // ESC: Exit Marking
                        tm.isMarking = false
                        return nil
                    }
                    let totalItems = 1 + tm.allLabels.count  // 0=Create, 1..N=Labels

                    if event.keyCode == 125 || event.keyCode == 38 {  // Down or 'j'
                        tm.labelListSelection = (tm.labelListSelection + 1) % totalItems
                        return nil
                    }
                    if event.keyCode == 126 || event.keyCode == 40 {  // Up or 'k'
                        tm.labelListSelection =
                            (tm.labelListSelection - 1 + totalItems) % totalItems
                        return nil
                    }

                    if event.keyCode == 36 {  // Enter
                        if tm.labelListSelection == 0 {
                            // "Create New" selected
                            tm.isCreatingLabel = true
                            tm.markText = ""
                        } else {
                            // Label selected
                            let index = tm.labelListSelection - 1
                            if index >= 0 && index < tm.allLabels.count {
                                let label = tm.allLabels[index]
                                // Apply
                                let targets =
                                    tm.multiSelection.isEmpty
                                    ? (tm.selection != nil ? [tm.selection!] : [])
                                    : Array(tm.multiSelection)
                                for id in targets {
                                    var labels = tm.tabLabels[id] ?? []
                                    labels.insert(label)
                                    tm.tabLabels[id] = labels
                                }
                                tm.isMarking = false
                            }
                        }
                        return nil
                    }
                }
                return nil
            }

            // Select by Label Mode
            if tm.isSelectingByLabel {
                if event.keyCode == 53 {  // ESC
                    tm.isSelectingByLabel = false
                    return nil
                }
                let total = tm.allLabels.count
                if total > 0 {
                    if event.keyCode == 125 || event.keyCode == 38 {  // Down/j
                        tm.labelSelectionCursor = (tm.labelSelectionCursor + 1) % total
                        return nil
                    }
                    if event.keyCode == 126 || event.keyCode == 40 {  // Up/k
                        tm.labelSelectionCursor = (tm.labelSelectionCursor - 1 + total) % total
                        return nil
                    }
                    if event.keyCode == 49 {  // Space
                        let label = tm.allLabels[tm.labelSelectionCursor]
                        if tm.labelSelectionTemp.contains(label) {
                            tm.labelSelectionTemp.remove(label)
                        } else {
                            tm.labelSelectionTemp.insert(label)
                        }
                        return nil
                    }
                    if event.keyCode == 36 {  // Enter
                        if !tm.labelSelectionTemp.isEmpty {
                            let matching = tm.tabs.filter { tab in
                                let labels = tm.tabLabels[tab.id] ?? []
                                return !labels.isDisjoint(with: tm.labelSelectionTemp)
                            }.map { $0.id }
                            tm.multiSelection.formUnion(matching)
                        }
                        tm.isSelectingByLabel = false
                        return nil
                    }
                }
                return nil
            }

            // Toggle Filter Mode OR Handle characters
            if tm.isFiltering {
                if event.keyCode == 53 {  // ESC: Cancel filter
                    tm.isFiltering = false
                    tm.filterText = ""
                    return nil
                }
                if event.keyCode == 36 {  // Enter: Confirm filter
                    tm.isFiltering = false
                    return nil
                }
                if event.keyCode == 51 {  // Backspace
                    if !tm.filterText.isEmpty {
                        tm.filterText.removeLast()
                    }
                    return nil
                }

                // Handle typing AND swallow navigation keys
                if event.keyCode == 126 || event.keyCode == 125 {
                    return nil  // Swallow arrows to prevent list navigation
                }

                if let chars = event.characters, !chars.isEmpty,
                    !event.modifierFlags.contains(.command)
                {
                    tm.filterText += chars
                    return nil
                }
            } else {
                // Normal Mode
                if event.keyCode == 44 {  // Slash: Start filter
                    if !tm.multiSelection.isEmpty { return nil }
                    tm.isFiltering = true
                    tm.filterText = ""
                    return nil
                }
                if event.keyCode == 49 {  // Space: Toggle Multi-Selection
                    if let sel = tm.selection {
                        if tm.multiSelection.contains(sel) {
                            tm.multiSelection.remove(sel)
                        } else {
                            tm.multiSelection.insert(sel)
                        }
                    }
                }
                if event.keyCode == 46 {  // m: Mark tabs
                    if !tm.multiSelection.isEmpty {
                        tm.isMarking = true
                        tm.isCreatingLabel = false
                        tm.labelListSelection = 0
                        tm.markText = ""
                    }
                    return nil
                }
                if event.keyCode == 0 && event.modifierFlags.contains(.shift) {  // A with Shift: Select All
                    tm.multiSelection = Set(tm.displayedTabs.map { $0.id })
                    return nil
                }
                if event.keyCode == 1 {  // s: Select by Label
                    tm.isSelectingByLabel = true
                    tm.labelSelectionCursor = 0
                    tm.labelSelectionTemp = []
                    return nil
                }
                if event.keyCode == 7 {  // x: Close Selected Tabs
                    let idsToClose: [Int]
                    if event.modifierFlags.contains(.shift) && !tm.multiSelection.isEmpty {
                        // Close all but selected
                        idsToClose = tm.tabs.filter { !tm.multiSelection.contains($0.id) }.map {
                            $0.id
                        }
                    } else {
                        // Normal Close
                        if !tm.multiSelection.isEmpty {
                            idsToClose = Array(tm.multiSelection)
                        } else if let sel = tm.selection {
                            idsToClose = [sel]
                        } else {
                            idsToClose = []
                        }
                    }

                    if !idsToClose.isEmpty {
                        self.sendUDSMessage(event: "close_tabs", data: ["tabIds": idsToClose])
                        tm.multiSelection = []
                    }
                    return nil
                }
                if event.keyCode == 53 {  // ESC
                    if !tm.filterText.isEmpty {
                        tm.filterText = ""
                        return nil
                    }
                    if !tm.multiSelection.isEmpty {
                        tm.multiSelection = []
                        return nil
                    }
                    self.hideUI()
                    return nil
                }
            }

            // Navigation Logic
            // UP: Arrow (126) or K (40)
            let isK = event.keyCode == 40
            if (event.keyCode == 126 || isK) && !tm.isFiltering {
                let tabs = tm.displayedTabs
                if !tabs.isEmpty {
                    if let sel = tm.selection, let idx = tabs.firstIndex(where: { $0.id == sel }) {
                        if idx == 0 {
                            tm.selection = tabs.last?.id
                        } else {
                            tm.selection = tabs[idx - 1].id
                        }
                    } else {
                        tm.selection = tabs.first?.id
                    }
                    return nil
                }
            }

            // DOWN: Arrow (125) or J (38)
            let isJ = event.keyCode == 38
            if (event.keyCode == 125 || isJ) && !tm.isFiltering {
                let tabs = tm.displayedTabs
                if !tabs.isEmpty {
                    if let sel = tm.selection, let idx = tabs.firstIndex(where: { $0.id == sel }) {
                        if idx == tabs.count - 1 {
                            tm.selection = tabs.first?.id
                        } else {
                            tm.selection = tabs[idx + 1].id
                        }
                    } else {
                        tm.selection = tabs.first?.id
                    }
                    return nil
                }
            }

            // Confirm Selection (Entered Normal Mode)
            if !tm.isFiltering && event.keyCode == 36 {
                if !tm.multiSelection.isEmpty { return nil }
                if let selectedId = Lotab.shared.selection {
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
        vlog_s(.trace, LotabApp.appClass, "hiding ui")
        DispatchQueue.main.async {
            self.window.orderOut(nil)
            NSApp.hide(nil)
        }

        // Reset State on Open
        Lotab.shared.isFiltering = false
        Lotab.shared.filterText = ""
        Lotab.shared.selection = Lotab.shared.displayedTabs.first?.id
        Lotab.shared.multiSelection = []

        // Reset Marking State
        Lotab.shared.isMarking = false
        Lotab.shared.isCreatingLabel = false
        Lotab.shared.markText = ""
    }

    private func startUDSServer() {
        unlink(socketPath)
        serverSocket = socket(AF_UNIX, SOCK_STREAM, 0)
        vlog_s(.info, LotabApp.appClass, "Attempting to connect to UDS server")
        guard serverSocket >= 0 else {
            vlog_s(.error, LotabApp.appClass, "Failed to create socket")
            return
        }

        var addr = sockaddr_un()
        addr.sun_family = sa_family_t(AF_UNIX)
        let pathLen = socketPath.withCString { Int(strlen($0)) }
        socketPath.withCString { src in
            withUnsafeMutablePointer(to: &addr.sun_path) { dest in
                let destPtr = UnsafeMutableRawPointer(dest).assumingMemoryBound(to: Int8.self)
                strncpy(destPtr, src, 104)  // 104 is the max length for sun_path
            }
        }
        addr.sun_len = UInt8(
            MemoryLayout<sa_family_t>.size + MemoryLayout<UInt8>.size + pathLen + 1)

        let addrLen = socklen_t(addr.sun_len)
        let bindResult = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                Darwin.bind(serverSocket, $0, addrLen)
            }
        }

        guard bindResult == 0 else {
            vlog_s(
                .error, LotabApp.appClass,
                "Failed to bind socket. Error: \(String(cString: strerror(errno)))")
            return
        }

        guard Darwin.listen(serverSocket, 5) == 0 else {
            vlog_s(.error, LotabApp.appClass, "Failed to listen on socket")
            return
        }

        vlog_s(.info, LotabApp.appClass, "UDS server started at \(socketPath)")
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
                vlog_s(.info, LotabApp.appClass, "Accepted new UDS connection")
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
                vlog_s(.info, LotabApp.appClass, "UDS connection closed by peer")
                break
            } else if headerBytesRead < 0 {
                vlog_s(
                    .error, LotabApp.appClass,
                    "UDS header read error: \(String(cString: strerror(errno)))")
                break
            } else if headerBytesRead < 4 {
                vlog_s(.error, LotabApp.appClass, "UDS partial header read")
                break
            }

            let msgLen = headerData.withUnsafeBytes { $0.load(as: UInt32.self).littleEndian }

            // 2. Read Payload
            var payloadData = Data(count: Int(msgLen))
            var totalRead = 0
            var readError = false

            payloadData.withUnsafeMutableBytes { buffer in
                while totalRead < Int(msgLen) {
                    let n = read(
                        clientSocket, buffer.baseAddress! + totalRead, Int(msgLen) - totalRead)
                    if n <= 0 {
                        readError = true
                        break
                    }
                    totalRead += n
                }
            }

            if readError {
                vlog_s(.error, LotabApp.appClass, "UDS payload read error or closed prematurely")
                break
            }

            // 3. Process Message
            if let message = String(data: payloadData, encoding: .utf8) {
                vlog_s(.trace, LotabApp.appClass, "uds-read: \(message)")
                if message.contains("tabs_update") {
                    // Decode tabs
                    if let jsonData = message.data(using: .utf8) {
                        do {
                            let payload = try JSONDecoder().decode(
                                TabListPayload.self, from: jsonData)
                            vlog_s(
                                .info, LotabApp.appClass,
                                "Successfully decoded \(payload.data.tabs.count) tabs")
                            DispatchQueue.main.async {
                                Lotab.shared.tabs = payload.data.tabs
                            }
                        } catch {
                            vlog_s(
                                .error, LotabApp.appClass,
                                "JSON Decoding Error for tabs_update: \(error)")
                        }
                    }
                } else if message.contains("tasks_update") {
                    // Decode tasks
                    if let jsonData = message.data(using: .utf8) {
                        do {
                            let payload = try JSONDecoder().decode(
                                TaskListPayload.self, from: jsonData)
                            vlog_s(
                                .info, LotabApp.appClass,
                                "Successfully decoded \(payload.data.tasks.count) tasks")
                            DispatchQueue.main.async {
                                Lotab.shared.tasks = payload.data.tasks
                            }
                        } catch {
                            vlog_s(
                                .error, LotabApp.appClass,
                                "JSON Decoding Error for tasks_update: \(error)")
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
            vlog_s(.error, LotabApp.appClass, "Cannot send message: No active UDS connection")
            return
        }

        let messageParams: [String: Any] = [
            "event": event,
            "data": data,
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
                vlog_s(
                    .error, LotabApp.appClass,
                    "Failed to send UDS message: \(String(cString: strerror(errno)))")
            } else {
                vlog_s(.info, LotabApp.appClass, "uds-send: \(event) (len: \(jsonData.count))")
            }
        } catch {
            vlog_s(.error, LotabApp.appClass, "Failed to serialize UDS message: \(error)")
        }
    }
}

struct BrowserTab: Identifiable, Decodable, Hashable {
    let id: Int
    let title: String
    let active: Bool
}

struct TabListPayload: Decodable {
    struct Data: Decodable {
        let tabs: [BrowserTab]
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

class Lotab: ObservableObject {
    static let shared = Lotab()
    @Published var tabs: [BrowserTab] = []
    @Published var tasks: [Task] = []
    @Published var selection: Int?

    @Published var filterText: String = ""
    @Published var isFiltering: Bool = false
    @Published var isMarking: Bool = false  // Marking Mode
    @Published var isCreatingLabel: Bool = false  // Sub-mode: Creating Label Input
    @Published var labelListSelection: Int = 0  // 0 = Create New, 1+ = Labels
    @Published var markText: String = ""
    @Published var tabLabels: [Int: Set<String>] = [:]
    @Published var allLabels: [String] = ["Work", "Personal", "Read Later"]
    @Published var multiSelection: Set<Int> = []

    // Select by Label Mode
    @Published var isSelectingByLabel: Bool = false
    @Published var labelSelectionCursor: Int = 0
    @Published var labelSelectionTemp: Set<String> = []

    var displayedTabs: [BrowserTab] {
        let filtered =
            filterText.isEmpty
            ? tabs : tabs.filter { $0.title.localizedCaseInsensitiveContains(filterText) }
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
struct LotabApp: App {
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
