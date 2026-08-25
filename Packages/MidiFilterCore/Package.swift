// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "MidiFilterCore",
    platforms: [.macOS(.v13), .iOS(.v16)],
    products: [
        .library(name: "MidiFilterCore", targets: ["MidiFilterCore"])
    ],
    targets: [
        .target(name: "MidiFilterCore", swiftSettings: [.swiftLanguageMode(.v5)]),
        .testTarget(
            name: "MidiFilterCoreTests",
            dependencies: ["MidiFilterCore"],
            swiftSettings: [.swiftLanguageMode(.v5)]
        ),
    ]
)
