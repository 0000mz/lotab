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
                                Spacer()
                                Image(systemName: "checkmark.circle.fill")
                                    .foregroundColor(.blue)
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
        .frame(width: 400, height: 500)
    }
}
