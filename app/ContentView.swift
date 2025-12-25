import SwiftUI

struct ContentView: View {
    @ObservedObject var tabManager: TabManager

    var body: some View {
        VStack {
            if tabManager.tabs.isEmpty {
                Text("No tabs found")
                    .foregroundColor(.secondary)
            } else {
                List {
                    if let activeTab = tabManager.tabs.first(where: { $0.active }) {
                        Section(header: Text("Active Tab")) {
                            HStack {
                                Text(activeTab.title)
                                    .lineLimit(1)
                                    .truncationMode(.tail)
                            }
                        }
                    }

                    Section(header: Text("Other Tabs")) {
                        ForEach(tabManager.tabs.filter { !$0.active }) { tab in
                            Text(tab.title)
                                .lineLimit(1)
                                .truncationMode(.tail)
                        }
                    }
                }
            }
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
