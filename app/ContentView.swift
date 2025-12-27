import SwiftUI

struct ContentView: View {
    @ObservedObject var tabManager: TabManager

    var body: some View {
        VStack(spacing: 0) {
            headerView

            if tabManager.tabs.isEmpty {
                emptyStateView
            } else {
                tabListView
            }

            footerView
        }
        .scrollContentBackground(.hidden)
        .frame(width: 600, height: 500)
        .background(VisualEffectView(material: .hudWindow, blendingMode: .behindWindow))
        .clipShape(RoundedRectangle(cornerRadius: 12))
    }

    // MARK: - Subviews
    private var headerView: some View {
        HStack {
            Text("lotab")
                .font(.headline)
                .fontWeight(.bold)
            Spacer()
            Text("\(tabManager.multiSelection.count) selected")
                .font(.caption)
                .foregroundColor(.secondary)
                .opacity(tabManager.multiSelection.isEmpty ? 0 : 1)
        }
        .frame(height: 30)
        .padding(.horizontal, 16)
        .background(Color.black.opacity(0.3))
    }
    private var emptyStateView: some View {
        VStack {
            Spacer()
            Text("No tabs found")
                .foregroundColor(.secondary)
            Spacer()
        }
    }
    private var tabListView: some View {
        ScrollViewReader { proxy in
            List(selection: $tabManager.selection) {
                let displayed = tabManager.displayedTabs
                let activeTabs = displayed.filter { $0.active }

                if !activeTabs.isEmpty {
                    Section(header: Text("Active Tabs")) {
                        ForEach(activeTabs) { tab in
                            tabRow(tab)
                        }
                    }
                }

                let otherTabs = displayed.filter { !$0.active }
                if !otherTabs.isEmpty {
                    Section(header: Text("Other Tabs")) {
                        ForEach(otherTabs) { tab in
                            tabRow(tab)
                        }
                    }
                }
            }
            .onChange(of: tabManager.tabs) { _, newTabs in
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
                if tabManager.selection == nil {
                    tabManager.selection = tabManager.tabs.first(where: { $0.active })?.id ?? tabManager.tabs.first?.id
                }
            }
        }
    }

    private func tabRow(_ tab: Tab) -> some View {
        HStack {
            Image(systemName: tabManager.multiSelection.contains(tab.id) ? "checkmark.square.fill" : "square")
                .foregroundColor(tabManager.multiSelection.contains(tab.id) ? .accentColor : .secondary)
            Text(tab.title)
                .lineLimit(1)
                .truncationMode(.tail)
            Spacer()
        }
        .tag(tab.id)
        .id(tab.id)
    }

    private var footerView: some View {
        HStack(alignment: .top, spacing: 32) {
            VStack(alignment: .leading, spacing: 2) {
                if tabManager.isFiltering || !tabManager.filterText.isEmpty {
                    Text("search: \(tabManager.filterText)")
                        .font(.caption)
                        .fontWeight(.medium)
                        .foregroundColor(.primary)
                } else {
                    Text("Navigate: Arrows or j/k")
                        .font(.caption)
                        .foregroundColor(.secondary)
                    if tabManager.multiSelection.isEmpty {
                        Text("/ to search")
                            .font(.caption)
                            .foregroundColor(.secondary)
                    }
                }
                if tabManager.multiSelection.isEmpty {
                    Text(tabManager.isFiltering ? "ENTER to search" : "ENTER to open")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
                Text(tabManager.isFiltering || !tabManager.multiSelection.isEmpty ? "ESC to cancel" : "ESC to close")
                    .font(.caption)
                    .foregroundColor(.secondary)
            }

            if !tabManager.multiSelection.isEmpty {
                VStack(alignment: .leading, spacing: 2) {
                    Text("x to close")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
            }
            Spacer()
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .frame(height: 70, alignment: .top)
        .padding(.vertical, 8)
        .padding(.horizontal, 16)
        .background(Color.black.opacity(0.3))
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
