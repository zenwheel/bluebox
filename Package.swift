// swift-tools-version: 6.0
import PackageDescription

let warnings: [CSetting] = [
    .unsafeFlags(["-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-unused-function"]),
    // glibc hides POSIX/BSD declarations (strdup, localtime_r, MAP_ANON) under strict C17.
    .define("_DEFAULT_SOURCE"),
]

let package = Package(
    name: "classic",
    platforms: [.macOS(.v13)],
    products: [
        .executable(name: "classic", targets: ["classic"]),
    ],
    targets: [
        // Guest memory arena, logging, generated low-memory and struct definitions.
        .target(name: "CCore", cSettings: warnings),
        // 68020 interpreter.
        .target(name: "CPU68K", dependencies: ["CCore"], cSettings: warnings),
        // AppleDouble, MacBinary, resource fork parsing and decompression (host side).
        .target(name: "ForkStore", cSettings: warnings),
        // Trap dispatch and the Toolbox managers.
        .target(name: "Toolbox", dependencies: ["CCore", "CPU68K", "ForkStore"], cSettings: warnings),
        // Command-line tool: run, import, rsrc.
        .executableTarget(name: "classic", dependencies: ["Toolbox", "ForkStore", "CCore"]),
        // Runner for the SingleStepTests CPU vectors (test-only).
        .target(name: "CPUTestSupport", dependencies: ["CPU68K", "CCore"], path: "Tests/CPUTestSupport",
                cSettings: warnings),
        // Tests touch C globals (one guest arena per process) and run serialized.
        .testTarget(name: "ToolboxTests",
                    dependencies: ["Toolbox", "ForkStore", "CPU68K", "CCore", "CPUTestSupport"],
                    swiftSettings: [.swiftLanguageMode(.v5)]),
    ],
    cLanguageStandard: .c17
)
