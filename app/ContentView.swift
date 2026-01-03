import SwiftUI

struct ContentView: View {
    @ObservedObject var lotab: Lotab

    var body: some View {
        VStack(spacing: 0) {
            headerView

            if lotab.isMarking {
                labelSelectionView
            } else if lotab.isSelectingByLabel {
                labelMultiSelectionView
            } else if lotab.tabs.isEmpty {
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
            Text("Lotab")
                .font(.headline)
                .fontWeight(.bold)
            Spacer()
            Text("\(lotab.multiSelection.count) selected")
                .font(.caption)
                .foregroundColor(.secondary)
                .opacity((lotab.multiSelection.isEmpty || lotab.isMarking) ? 0 : 1)
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
            List {
                let displayed = lotab.displayedTabs
                let activeTabs = displayed.filter { $0.active }
                let otherTabs = displayed.filter { !$0.active }
                let sortedTabs = activeTabs + otherTabs

                ForEach(Array(sortedTabs.enumerated()), id: \.element.id) { index, tab in
                    tabRow(tab)
                        .padding(.top, index == 0 ? 8 : 0)
                }
            }
            .listStyle(.plain)
            .scrollContentBackground(.hidden)
            .onChange(of: lotab.tabs) { newTabs in
                if lotab.selection == nil || !newTabs.contains(where: { $0.id == lotab.selection })
                {
                    lotab.selection = newTabs.first(where: { $0.active })?.id ?? newTabs.first?.id
                }
            }
            .onChange(of: lotab.selection) { newSel in
                if let id = newSel {
                    proxy.scrollTo(id, anchor: .center)
                }
            }
            .onAppear {
                if lotab.selection == nil {
                    lotab.selection =
                        lotab.tabs.first(where: { $0.active })?.id ?? lotab.tabs.first?.id
                }
            }
        }
    }

    private var labelSelectionView: some View {
        VStack(spacing: 0) {
            if lotab.isCreatingLabel {
                labelCreationView
            } else {
                labelMenuView
            }
        }
    }

    private var labelMenuView: some View {
        ScrollViewReader { proxy in
            List {
                Section(header: Text("Select Label").font(.caption).foregroundColor(.secondary)) {
                    // Create New Option (Index 0)
                    labelRow(text: "Create New Label", index: 0, topPadding: 4)
                    // Existing Labels
                    ForEach(Array(lotab.allLabels.enumerated()), id: \.offset) { index, label in
                        labelRow(text: label, index: index + 1)
                    }
                }
            }
            .listStyle(.plain)
            .scrollContentBackground(.hidden)
            .onChange(of: lotab.labelListSelection) { newSel in
                proxy.scrollTo(newSel, anchor: .center)
            }
        }
    }

    private func labelRow(text: String, index: Int, topPadding: CGFloat = 0) -> some View {
        HStack {
            Text(text)
            Spacer()
            if index != 0 {
                RoundedRectangle(cornerRadius: 2)
                    .fill(Color.generate(from: text))
                    .frame(width: 12, height: 12)
            }
        }
        .padding(.vertical, 4)
        .padding(.horizontal, 8)
        .background(
            RoundedRectangle(cornerRadius: 6)
                .fill(lotab.labelListSelection == index ? Color.accentColor : Color.clear)
        )
        .padding(.top, topPadding)
        .listRowSeparator(.hidden)
        .listRowBackground(Color.clear)
        .listRowInsets(EdgeInsets(top: 0, leading: 0, bottom: 0, trailing: 0))
        .id(index)
    }

    private var labelCreationView: some View {
        VStack(spacing: 20) {
            Spacer()
            Text("Create New Label")
                .font(.headline)

            HStack {
                Text(lotab.markText.isEmpty ? "Label name..." : lotab.markText)
                    .font(.title2)
                    .foregroundColor(lotab.markText.isEmpty ? .secondary : .primary)
                Spacer()
            }
            .padding()
            .background(Color.black.opacity(0.2))
            .cornerRadius(8)
            .padding(.horizontal)

            Spacer()
        }
    }

    private var labelMultiSelectionView: some View {
        ScrollViewReader { proxy in
            List {
                Section(
                    header: Text("Select tabs by label(s)").font(.caption).foregroundColor(
                        .secondary)
                ) {
                    ForEach(Array(lotab.allLabels.enumerated()), id: \.offset) { index, label in
                        HStack {
                            Image(
                                systemName: lotab.labelSelectionTemp.contains(label)
                                    ? "checkmark.square.fill" : "square"
                            )
                            .foregroundColor(
                                lotab.labelSelectionCursor == index
                                    ? .white
                                    : (lotab.labelSelectionTemp.contains(label)
                                        ? .accentColor : .secondary))
                            Text(label)
                                .foregroundColor(
                                    lotab.labelSelectionCursor == index ? .white : .primary)
                            Spacer()
                            RoundedRectangle(cornerRadius: 2)
                                .fill(Color.generate(from: label))
                                .frame(width: 12, height: 12)
                        }
                        .padding(.vertical, 4)
                        .padding(.horizontal, 8)
                        .background(
                            RoundedRectangle(cornerRadius: 6)
                                .fill(
                                    lotab.labelSelectionCursor == index
                                        ? Color.accentColor : Color.clear)
                        )
                        .padding(.top, index == 0 ? 8 : 0)
                        .listRowSeparator(.hidden)
                        .listRowBackground(Color.clear)
                        .listRowInsets(EdgeInsets(top: 0, leading: 0, bottom: 0, trailing: 0))
                        .id(index)
                    }
                }
            }
            .listStyle(.plain)
            .scrollContentBackground(.hidden)
            .onChange(of: lotab.labelSelectionCursor) { newSel in
                proxy.scrollTo(newSel, anchor: .center)
            }
        }
    }

    private func tabRow(_ tab: BrowserTab) -> some View {
        let isSelected = lotab.selection == tab.id
        return HStack {
            Image(
                systemName: lotab.multiSelection.contains(tab.id)
                    ? "checkmark.square.fill" : "square"
            )
            .foregroundColor(
                isSelected
                    ? .white : (lotab.multiSelection.contains(tab.id) ? .accentColor : .secondary))
            Text(tab.title)
                .lineLimit(1)
                .truncationMode(.tail)
                .foregroundColor(isSelected ? .white : .primary)
            Spacer()
            ForEach(Array(lotab.tabLabels[tab.id] ?? []).sorted(), id: \.self) { label in
                Text(label)
                    .font(.system(size: 10, weight: .bold))
                    .padding(.horizontal, 4)
                    .padding(.vertical, 2)
                    .background(Color.generate(from: label).opacity(0.8))
                    .foregroundColor(.white)
                    .cornerRadius(4)
            }
            if tab.active {
                Text("Active")
                    .font(.caption2)
                    .fontWeight(.bold)
                    .foregroundColor(isSelected ? .white : .accentColor)
                    .padding(.horizontal, 6)
                    .padding(.vertical, 2)
                    .overlay(
                        RoundedRectangle(cornerRadius: 4)
                            .stroke(isSelected ? Color.white : Color.accentColor, lineWidth: 1)
                    )
            }
        }
        .padding(.vertical, 4)
        .padding(.horizontal, 8)
        .background(
            RoundedRectangle(cornerRadius: 6)
                .fill(isSelected ? Color.accentColor : Color.clear)
        )
        .contentShape(Rectangle())
        .onTapGesture {
            lotab.selection = tab.id
        }
        .listRowSeparator(.hidden)
        .listRowBackground(Color.clear)
        .listRowInsets(EdgeInsets(top: 0, leading: 0, bottom: 0, trailing: 0))
        .id(tab.id)
    }

    private struct FooterItem: Identifiable {
        let id = UUID()
        let components: [FooterComponent]
        let description: String
    }

    private enum FooterComponent: Hashable {
        case key(String)
        case text(String)
    }

    private var footerItems: [FooterItem] {
        var items: [FooterItem] = []

        if lotab.isCreatingLabel {
            items.append(FooterItem(components: [.key("return")], description: "to create"))
            items.append(FooterItem(components: [.key("esc")], description: "to cancel"))
        } else if lotab.isSelectingByLabel {
            items.append(FooterItem(components: [.key("space")], description: "to toggle"))
            items.append(FooterItem(components: [.key("return")], description: "to confirm"))
            items.append(FooterItem(components: [.key("esc")], description: "to cancel"))
        } else if lotab.isMarking {
            items.append(FooterItem(components: [.key("return")], description: "to select"))
            items.append(FooterItem(components: [.key("esc")], description: "to cancel"))
        } else {
            if lotab.isFiltering || !lotab.filterText.isEmpty {
                items.append(
                    FooterItem(components: [.text("search: \(lotab.filterText)")], description: ""))
            } else {
                items.append(
                    FooterItem(
                        components: [.key("↓"), .key("↑"), .text("or"), .key("j"), .key("k")],
                        description: "to navigate"))
                if lotab.multiSelection.isEmpty {
                    items.append(FooterItem(components: [.key("/")], description: "to search"))
                }
            }

            if lotab.multiSelection.isEmpty {
                let desc = lotab.isFiltering ? "to search" : "to open"
                items.append(FooterItem(components: [.key("return")], description: desc))
            }

            let escDesc =
                lotab.isFiltering || !lotab.multiSelection.isEmpty ? "to cancel" : "to close"
            items.append(FooterItem(components: [.key("esc")], description: escDesc))

            if !lotab.multiSelection.isEmpty {
                items.append(FooterItem(components: [.key("x")], description: "to close"))
                items.append(
                    FooterItem(
                        components: [.key("shift"), .key("x")], description: "to close others"))
            }

            items.append(
                FooterItem(components: [.key("cmd"), .key("a")], description: "to select all"))
            items.append(FooterItem(components: [.key("s")], description: "to select"))

            if !lotab.multiSelection.isEmpty {
                items.append(FooterItem(components: [.key("m")], description: "to mark"))
            }
        }
        return items
    }

    private var footerView: some View {
        let items = footerItems
        let columns = 3
        let count = items.count
        let itemsPerColumn = count > 0 ? Int(ceil(Double(count) / Double(columns))) : 0

        return HStack(alignment: .top, spacing: 32) {
            ForEach(0..<columns, id: \.self) { col in
                let start = col * itemsPerColumn
                let end = min(start + itemsPerColumn, count)

                if start < end {
                    VStack(alignment: .leading, spacing: 2) {
                        ForEach(items[start..<end]) { item in
                            HStack(spacing: 4) {
                                ForEach(item.components, id: \.self) { component in
                                    switch component {
                                    case .key(let k): KeyView(text: k)
                                    case .text(let t):
                                        Text(t).font(.caption).foregroundColor(
                                            item.description.isEmpty ? .primary : .secondary)
                                    }
                                }
                                if !item.description.isEmpty {
                                    Text(item.description)
                                        .font(.caption)
                                        .foregroundColor(.secondary)
                                }
                            }
                        }
                    }
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

struct KeyView: View {
    let text: String

    var body: some View {
        Text(text)
            .font(.system(size: 10, weight: .bold))
            .foregroundColor(.secondary)
            .padding(.horizontal, 4)
            .padding(.vertical, 1)
            .overlay(
                RoundedRectangle(cornerRadius: 3)
                    .stroke(Color.secondary.opacity(0.5), lineWidth: 1)
            )
    }
}

extension Color {
    static func generate(from string: String) -> Color {
        var h: Int = 0
        for char in string.utf8 {
            h = (Int(char) + ((h << 5) - h))
        }

        // Simple seeded random to get nice colors
        // Or manipulate hash to get HSB
        // This simple bit-shift might produce dark colors.
        // Let's try to ensure they are bright enough for dark mode or readable.
        // Actually, simple hash is requested. "unique color associated".

        let hue = Double(abs(h) % 360) / 360.0
        return Color(hue: hue, saturation: 0.7, brightness: 0.9)
    }
}
