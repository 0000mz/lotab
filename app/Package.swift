// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "TabManagerApp",
    platforms: [
        .macOS(.v11)
    ],
    products: [
        .executable(name: "TabManager", targets: ["TabManager"])
    ],
    targets: [
        .executableTarget(
            name: "TabManager",
            path: "Sources/TabManager"
        )
    ]
)
