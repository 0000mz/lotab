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
                        // Use C client to send close_tabs
                        if let client = self.udsClient {
                            let cIds = idsToClose.map { Int32($0) }
                            cIds.withUnsafeBufferPointer { buffer in
                                lotab_client_send_close_tabs(
                                    client, buffer.baseAddress, Int(buffer.count))
                            }
                        }
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
                    // Use C client to send tab_selected
                    if let client = self.udsClient {
                        lotab_client_send_tab_selected(client, Int32(selectedId))
                    }
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

    private var udsClient: OpaquePointer?

    private func startUDSServer() {
        let socketPath = "/tmp/lotab.sock"

        // Define callbacks
        let onTabsUpdate: lotab_on_tabs_update_cb = { userData, tabsList in
            vlog_s(.info, LotabApp.appClass, "onTabsUpdate callback entered")
            guard let tabsList = tabsList else { return }
            var newTabs: [BrowserTab] = []
            let count = Int(tabsList.pointee.count)
            if count > 0 {
                let buffer = UnsafeBufferPointer(start: tabsList.pointee.tabs, count: count)
                for i in 0..<count {
                    let cTab = buffer[i]
                    let title = String(cString: cTab.title)
                    newTabs.append(BrowserTab(id: Int(cTab.id), title: title, active: cTab.active))
                }
            }

            DispatchQueue.main.async {
                Lotab.shared.tabs = newTabs
                vlog_s(.info, LotabApp.appClass, "Updated tabs: \(newTabs.count)")
            }
        }

        let onTasksUpdate: lotab_on_tasks_update_cb = { userData, tasksList in
            vlog_s(.info, LotabApp.appClass, "onTasksUpdate callback entered")
            guard let tasksList = tasksList else { return }
            var newTasks: [Task] = []
            let count = Int(tasksList.pointee.count)
            if count > 0 {
                let buffer = UnsafeBufferPointer(start: tasksList.pointee.tasks, count: count)
                for i in 0..<count {
                    let cTask = buffer[i]
                    let name = String(cString: cTask.name)
                    newTasks.append(Task(id: Int(cTask.id), name: name))
                }
            }

            DispatchQueue.main.async {
                Lotab.shared.tasks = newTasks
                vlog_s(.info, LotabApp.appClass, "Updated tasks: \(newTasks.count)")
            }
        }

        let onUIToggle: lotab_on_ui_toggle_cb = { userData in
            vlog_s(.info, LotabApp.appClass, "onUIToggle callback entered")
            DispatchQueue.main.async {
                vlog_s(.info, LotabApp.appClass, "DispatchQueue.main.async entered")
                if let appDelegate = AppDelegate.shared {
                    appDelegate.showUI()
                    NSApp.activate(ignoringOtherApps: true)
                } else {
                    vlog_s(.error, LotabApp.appClass, "AppDelegate.shared is nil")
                }
            }
        }

        let callbacks = ClientCallbacks(
            on_tabs_update: onTabsUpdate,
            on_tasks_update: onTasksUpdate,
            on_ui_toggle: onUIToggle
        )

        self.udsClient = lotab_client_new(socketPath, callbacks, nil)

        if self.udsClient != nil {
            Thread.detachNewThread {
                vlog_s(.info, LotabApp.appClass, "Starting UDS Client Loop")
                lotab_client_run_loop(self.udsClient)
            }
        } else {
            vlog_s(.error, LotabApp.appClass, "Failed to create UDS client")
        }
    }
}

struct BrowserTab: Identifiable, Hashable {
    let id: Int
    let title: String
    let active: Bool
}

struct Task: Identifiable, Hashable {
    let id: Int
    let name: String
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
