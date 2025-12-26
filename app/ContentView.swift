import SwiftUI

struct ContentView: View {
    @ObservedObject var tabManager: TabManager

    @State private var selection: Int?

    var body: some View {
        VStack(spacing: 0) {
            if tabManager.tabs.isEmpty {
                Spacer()
                Text("No tabs found")
                    .foregroundColor(.secondary)
                Spacer()
            } else {
                List(selection: $selection) {
                    let activeTabs = tabManager.tabs.filter { $0.active }
                    if !activeTabs.isEmpty {
                        Section(header: Text("Active Tabs")) {
                            ForEach(activeTabs) { tab in
                                HStack {
                                    Text(tab.title)
                                        .lineLimit(1)
                                        .truncationMode(.tail)
                                    Spacer()
                                }
                                .tag(tab.id)
                            }
                        }
                    }

                    Section(header: Text("Other Tabs")) {
                        ForEach(tabManager.tabs.filter { !$0.active }) { tab in
                            Text(tab.title)
                                .lineLimit(1)
                                .truncationMode(.tail)
                                .tag(tab.id)
                        }
                    }
                }
                .onChange(of: tabManager.tabs) { newTabs in
                    // Preserve selection if possible, or select first active
                    if selection == nil || !newTabs.contains(where: { $0.id == selection }) {
                         selection = newTabs.first(where: { $0.active })?.id ?? newTabs.first?.id
                    }
                }
                .onAppear {
                    // Initial selection
                    if selection == nil {
                        selection = tabManager.tabs.first(where: { $0.active })?.id ?? tabManager.tabs.first?.id
                    }
                }
                // Hidden button to capture Enter key
                Button("") {
                    if let selectedId = selection {
                        AppDelegate.shared?.sendUDSMessage(event: "tab_selected", data: ["tabId": selectedId])
                    }
                }
                .keyboardShortcut(.defaultAction)
                .hidden()
            }
            HStack {
                Spacer()
                Text("ESC to close")
                    .font(.caption)
                    .foregroundColor(.secondary)
                    .padding([.trailing], 12)
                    .frame(height: .infinity, alignment: .center)
            }
            .frame(height: 20)
        }
        .scrollContentBackground(.hidden)
        .frame(width: 600, height: 500)
        .background(VisualEffectView(material: .hudWindow, blendingMode: .behindWindow))
        .clipShape(RoundedRectangle(cornerRadius: 12))
    }
}

struct VisualEffectView: NSViewRepresentable {
    let material: NSVisualEffectView.Material
    let blendingMode: NSVisualEffectView.BlendingMode

    func makeNSView(context: Context) -> NSVisualEffectView {
        let visualEffectView = NSVisualEffectView()
        visualEffectView.material = material
        visualEffectView.blendingMode = blendingMode
        visualEffectView.state = .active
        return visualEffectView
    }

    func updateNSView(_ nsView: NSVisualEffectView, context: Context) {
        nsView.material = material
        nsView.blendingMode = blendingMode
    }
}
