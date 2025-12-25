import SwiftUI

struct ContentView: View {
    @ObservedObject var tabManager: TabManager

    var body: some View {
        VStack {
            if tabManager.tabs.isEmpty {
                Text("No tabs found")
                    .foregroundColor(.secondary)
            } else {
                List(tabManager.tabs) { tab in
                    HStack {
                        Text(tab.title)
                            .lineLimit(1)
                            .truncationMode(.tail)
                        Spacer()
                        if tab.active {
                            Image(systemName: "checkmark.circle.fill")
                                .foregroundColor(.blue)
                        }
                    }
                }
            }
        }
        .frame(width: 400, height: 500)
    }
}
