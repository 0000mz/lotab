import SwiftUI

struct ContentView: View {
    @ObservedObject var tabManager: TabManager

    var body: some View {
        VStack(spacing: 0) {
            if tabManager.tabs.isEmpty {
                Spacer()
                Text("No tabs found")
                    .foregroundColor(.secondary)
                Spacer()
            } else {
                ScrollViewReader { proxy in
                    List(selection: $tabManager.selection) {
                        let displayed = tabManager.displayedTabs
                        let activeTabs = displayed.filter { $0.active }

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
                                    .id(tab.id)
                                }
                            }
                        }

                        let otherTabs = displayed.filter { !$0.active }
                        if !otherTabs.isEmpty {
                            Section(header: Text("Other Tabs")) {
                                ForEach(otherTabs) { tab in
                                    Text(tab.title)
                                        .lineLimit(1)
                                        .truncationMode(.tail)
                                        .tag(tab.id)
                                        .id(tab.id)
                                }
                            }
                        }
                    }
                    .onChange(of: tabManager.tabs) { _, newTabs in
                        // Preserve selection if possible, or select first active
                        if tabManager.selection == nil || !newTabs.contains(where: { $0.id == tabManager.selection }) {
                             tabManager.selection = newTabs.first(where: { $0.active })?.id ?? newTabs.first?.id
                        }
                    }
                    .onChange(of: tabManager.selection) { _, newSel in
                        if let id = newSel {
                            proxy.scrollTo(id, anchor: .center)
                        }
                    }
                    .onAppear {
                        // Initial selection
                        if tabManager.selection == nil {
                            tabManager.selection = tabManager.tabs.first(where: { $0.active })?.id ?? tabManager.tabs.first?.id
                        }
                    }
                }
            }
            HStack {
                if tabManager.isFiltering || !tabManager.filterText.isEmpty {
                    Text("search: \(tabManager.filterText)")
                        .font(.caption)
                        .padding([.leading], 12)
                        .foregroundColor(.secondary)
                    Spacer()
                } else {
                    Text("/ to search")
                        .font(.caption)
                        .padding([.leading], 12)
                        .foregroundColor(.secondary)
                    Spacer()
                    Text("up/down or j/k")
                        .font(.caption)
                        .foregroundColor(.secondary)
                    Divider()
                        .frame(height: 12)
                        .padding(.horizontal, 4)
                }
                Text(tabManager.isFiltering ? "ENTER to search" : "ENTER to navigate")
                    .font(.caption)
                    .foregroundColor(.secondary)
                Divider()
                    .frame(height: 12)
                    .padding(.horizontal, 4)
                Text(tabManager.isFiltering ? "ESC to cancel search" : "ESC to close")
                    .font(.caption)
                    .foregroundColor(.secondary)
                    .padding([.trailing], 12)
                    .frame(height: .infinity, alignment: .center)
            }.frame(height: 30)
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
