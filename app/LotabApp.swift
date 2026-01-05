import Foundation
import SwiftUI

// NOTE: For ensuring that the application has focus on open.
// Might not be necessary -- seems the issues I am facing might just be
// a quark of executing the binary from the terminal.
class AppDelegate: NSObject, NSApplicationDelegate {
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

            let charValue = event.characters?.first?.asciiValue ?? 0

            var transition: LmModeTransition = LM_MODETS_UNKNOWN
            var old_mode: LmMode = LM_MODE_UNKNOWN
            var new_mode: LmMode = LM_MODE_UNKNOWN
            lm_process_key_event(
                self.modeContext,
                event.keyCode,
                charValue,
                event.modifierFlags.contains(.command) ? 1 : 0,
                event.modifierFlags.contains(.shift) ? 1 : 0,
                &transition,
                &old_mode,
                &new_mode)

            // If the transition was handled (returning nil in original code), we return nil here.
            // Currently handleTransition doesn't return anything.
            // We need to adhere to the return type of the monitor: NSEvent?
            // Most cases in the original switch returned nil (swallowing the event).
            // A few (unknown/default) returned the event.
            // I'll make handleTransition return NSEvent?.
            return self.handleTransition(
                transition: transition, oldMode: old_mode, newMode: new_mode, originalEvent: event)
        }

        self.modeContext = lm_alloc()
        startUDSServer()

        // Explicitly hide initially
        DispatchQueue.main.async {
            self.hideUI()
        }
    }

    func handleTransition(
        transition: LmModeTransition, oldMode: LmMode, newMode: LmMode,
        originalEvent: NSEvent? = nil
    ) -> NSEvent? {
        let tm = Lotab.shared

        switch transition {
        case LM_MODETS_UNKNOWN:
            break
        case LM_MODETS_HIDE_UI:
            self.hideUI()
            return nil

        case LM_MODETS_SELECT_TAB:
            tm.addTabToSelection()
            return nil

        case LM_MODETS_SELECT_ALL_TABS:
            tm.selectAllTabs()
            return nil

        case LM_MODETS_NAVIGATE_UP:
            tm.listNavigateUp()
            return nil

        case LM_MODETS_NAVIGATE_DOWN:
            tm.listNavigateDown()
            return nil

        case LM_MODETS_CLOSE_SELECTED_TABS:
            tm.closeSelectedTabs(udsClient: self.udsClient)
            return nil

        case LM_MODETS_ACTIVATE_TO_TAB:
            if !tm.multiSelection.isEmpty { return nil }
            if let selectedId = Lotab.shared.selection {
                if let client = self.udsClient {
                    lotab_client_send_tab_selected(client, Int32(selectedId))
                }
                self.hideUI()
                return nil
            }
            return nil

        case LM_MODETS_ADHERE_TO_MODE:
            if oldMode == LM_MODE_LIST_NORMAL && newMode == LM_MODE_LIST_FILTER_INFLIGHT {
                tm.isFiltering = true
                tm.filterText = ""
                return nil
            }
            // Ensure we disable filtering if returning to Normal from anything except Multiselect
            if newMode == LM_MODE_LIST_NORMAL && oldMode != LM_MODE_LIST_MULTISELECT {
                tm.isFiltering = false
                tm.filterText = ""
                return nil
            }
            if oldMode == LM_MODE_LIST_MULTISELECT
                && (newMode == LM_MODE_LIST_NORMAL
                    || newMode == LM_MODE_LIST_FILTER_COMMITTED)
            {
                tm.clearSelection()
                // IMPORTANT: If we are adhering to mode NORMAL from MULTISELECT,
                // we might have a filter text preserved.
                if let filter_txt = lm_get_filter_text(self.modeContext) {
                    tm.filterText = String(cString: filter_txt)
                    tm.isFiltering = !tm.filterText.isEmpty  // Or false if we consider it committed?
                    // In LIST_NORMAL with filter, usually isFiltering=false implies we are not TYPING, but filter is active.
                    // But Lotab's isFiltering usually means the INPUT box is active.
                    // If we just transitioned back with a filter, we probably want isFiltering=false (committed state).
                    // But we likely want to verify the filterText is reflected.
                }
                return nil
            }
            if newMode == LM_MODE_TASK_ASSOCIATION {
                tm.isAssociatingTask = true
                tm.isCreatingTask = false
                tm.taskAssociationSelection = Int(
                    lm_get_task_association_selection(self.modeContext))
                return nil
            }
            if newMode == LM_MODE_TASK_CREATION {
                tm.isCreatingTask = true
                tm.isAssociatingTask = false
                if let cStr = lm_get_task_creation_input(self.modeContext) {
                    tm.taskCreationInput = String(cString: cStr)
                }
                return nil
            }
            break

        case LM_MODETS_COMMIT_LIST_FILTER:
            tm.isFiltering = false
            break

        case LM_MODETS_UPDATE_LIST_FILTER:
            vlog_s(.trace, LotabApp.appClass, ".inside this...")
            if let filter_txt = lm_get_filter_text(self.modeContext) {
                tm.filterText = String(cString: filter_txt)
            } else {
                tm.filterText = ""
            }
            vlog_s(.trace, LotabApp.appClass, "updated filter text: \(tm.filterText)")

        case LM_MODETS_START_ASSOCIATION:
            tm.isAssociatingTask = true
            tm.isCreatingTask = false
            return nil

        case LM_MODETS_CANCEL_ASSOCIATION:
            tm.isAssociatingTask = false
            tm.isCreatingTask = false
            return nil

        case LM_MODETS_ASSOCIATE_TASK:
            let selection = tm.taskAssociationSelection
            // Index 0 is "Create New", so index 1 corresponds to task 0
            if selection > 0 && selection - 1 < tm.tasks.count {
                let taskId = tm.tasks[selection - 1].id
                let idsToAssoc = Array(tm.multiSelection)
                if !idsToAssoc.isEmpty, let client = self.udsClient {
                    let cIds = idsToAssoc.map { Int32($0) }
                    cIds.withUnsafeBufferPointer { buffer in
                        lotab_client_send_associate_tabs(
                            client, buffer.baseAddress, Int(buffer.count), Int32(taskId))
                    }
                }
            }
            tm.isAssociatingTask = false
            tm.isCreatingTask = false
            tm.clearSelection()
            return nil

        case LM_MODETS_CREATE_TASK:
            let name = tm.taskCreationInput
            let idsToAssoc = Array(tm.multiSelection)
            if !name.isEmpty, let client = self.udsClient {
                name.withCString { taskNamePtr in
                    if !idsToAssoc.isEmpty {
                        let cIds = idsToAssoc.map { Int32($0) }
                        cIds.withUnsafeBufferPointer { buffer in
                            lotab_client_send_create_task_and_associate(
                                client, taskNamePtr, buffer.baseAddress, Int(buffer.count))
                        }
                    } else {
                        lotab_client_send_create_task_and_associate(client, taskNamePtr, nil, 0)
                    }
                }
            }
            tm.isAssociatingTask = false
            tm.isCreatingTask = false
            tm.clearSelection()
            return nil

        default:
            vlog_s(.warn, LotabApp.appClass, "unknown transition id: \(transition)")
        }

        return originalEvent
    }

    func applicationDidResignActive(_ notification: Notification) {
        hideUI()
    }

    func applicationWillTerminate(_ notification: Notification) {
        if let client = udsClient {
            // This will stop the run loop and free resources.
            // Note: Since the loop is in a detached thread, there's a small race
            // but for app termination the OS will clean up soon anyway.
            lotab_client_destroy(client)
            udsClient = nil
        }
        if let mctx = modeContext {
            lm_destroy(mctx)
            modeContext = nil
        }
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
    private var modeContext: OpaquePointer?

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
                    newTabs.append(
                        BrowserTab(
                            id: Int(cTab.id), title: title, active: cTab.active,
                            taskId: Int(cTab.task_id)))
                }
            }

            DispatchQueue.main.async {
                Lotab.shared.tabs = newTabs
                vlog_s(.trace, LotabApp.appClass, "Updated tabs: \(newTabs.count)")

                // --- Notify State Machine of List Update (Auto-Exit Multiselect) ---
                if let appDelegate = AppDelegate.shared, let mctx = appDelegate.modeContext {
                    let len = Lotab.shared.displayedTabs.count
                    var transition: LmModeTransition = LM_MODETS_UNKNOWN
                    var old_mode: LmMode = LM_MODE_UNKNOWN
                    var new_mode: LmMode = LM_MODE_UNKNOWN

                    lm_on_list_len_update(mctx, Int32(len), &transition, &old_mode, &new_mode)

                    if transition != LM_MODETS_UNKNOWN {
                        _ = appDelegate.handleTransition(
                            transition: transition, oldMode: old_mode, newMode: new_mode)
                    }
                }
            }
        }

        let onTasksUpdate: lotab_on_tasks_update_cb = { userData, tasksList in
            vlog_s(.trace, LotabApp.appClass, "onTasksUpdate callback entered")
            guard let tasksList = tasksList else { return }
            var newTasks: [Task] = []
            let count = Int(tasksList.pointee.count)
            if count > 0 {
                let buffer = UnsafeBufferPointer(start: tasksList.pointee.tasks, count: count)
                for i in 0..<count {
                    let cTask = buffer[i]
                    let name = String(cString: cTask.name)
                    let color = String(cString: cTask.color)
                    newTasks.append(Task(id: Int(cTask.id), name: name, color: color))
                }
            }

            DispatchQueue.main.async {
                Lotab.shared.tasks = newTasks
                vlog_s(.info, LotabApp.appClass, "Updated tasks: \(newTasks.count)")
                for t in newTasks {
                    vlog_s(.trace, LotabApp.appClass, "Task[\(t.id)]: \(t.name)")
                }
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
    let taskId: Int
}

struct Task: Identifiable, Hashable {
    let id: Int
    let name: String
    let color: String
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

    // Task Association
    @Published var isAssociatingTask: Bool = false
    @Published var isCreatingTask: Bool = false
    @Published var taskAssociationSelection: Int = 0
    @Published var taskCreationInput: String = ""

    var displayedTabs: [BrowserTab] {
        let filtered =
            filterText.isEmpty
            ? tabs : tabs.filter { $0.title.localizedCaseInsensitiveContains(filterText) }
        let active = filtered.filter { $0.active }
        let other = filtered.filter { !$0.active }
        return active + other
    }

    public func listNavigateDown() {
        let tabs = self.displayedTabs
        if !tabs.isEmpty {
            if let sel = self.selection, let idx = tabs.firstIndex(where: { $0.id == sel }) {
                if idx == tabs.count - 1 {
                    self.selection = tabs.first?.id
                } else {
                    self.selection = tabs[idx + 1].id
                }
            } else {
                self.selection = tabs.first?.id
            }
        }
    }

    public func listNavigateUp() {
        let tabs = self.displayedTabs
        if !tabs.isEmpty {
            if let sel = self.selection, let idx = tabs.firstIndex(where: { $0.id == sel }) {
                if idx == 0 {
                    self.selection = tabs.last?.id
                } else {
                    self.selection = tabs[idx - 1].id
                }
            } else {
                self.selection = tabs.first?.id
            }
        }
    }

    public func addTabToSelection() {
        if let sel = self.selection {
            if self.multiSelection.contains(sel) {
                self.multiSelection.remove(sel)
            } else {
                self.multiSelection.insert(sel)
            }
        }
    }

    public func selectAllTabs() {
        self.multiSelection = Set(self.displayedTabs.map { $0.id })
    }

    public func clearSelection() {
        self.multiSelection.removeAll()
    }

    public func closeSelectedTabs(udsClient: OpaquePointer?) {
        let idsToClose: [Int]
        // TODO: Re-enable
        // if event.modifierFlags.contains(.shift) && !tm.multiSelection.isEmpty {
        //     // Close all but selected
        //     idsToClose = tm.tabs.filter { !tm.multiSelection.contains($0.id) }.map {
        //         $0.id
        //     }
        // } else {
        // Normal Close
        if !self.multiSelection.isEmpty {
            idsToClose = Array(self.multiSelection)
        } else if let sel = self.selection {
            idsToClose = [sel]
        } else {
            idsToClose = []
        }
        // }

        if !idsToClose.isEmpty {
            // Use C client to send close_tabs
            if let client = udsClient {
                let cIds = idsToClose.map { Int32($0) }
                cIds.withUnsafeBufferPointer { buffer in
                    lotab_client_send_close_tabs(
                        client, buffer.baseAddress, Int(buffer.count))
                }
            }
            self.multiSelection = []
        }

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
