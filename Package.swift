// swift-tools-version: 6.0
import PackageDescription

let warnings: [CSetting] = [
    .unsafeFlags(["-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-unused-function",
                  "-Werror=int-conversion"]), // a host pointer passed as a guest address
    // glibc hides POSIX/BSD declarations (strdup, localtime_r, MAP_ANON) under strict C17.
    .define("_DEFAULT_SOURCE"),
]

let package = Package(
    name: "bluebox",
    platforms: [.macOS(.v13)],
    products: [
        .executable(name: "bluebox", targets: ["bluebox"]),
    ],
    targets: [
        // Guest memory arena, logging, generated low-memory and struct definitions.
        .target(name: "CCore", cSettings: warnings),
        // 68020 interpreter.
        .target(name: "CPU68K", dependencies: ["CCore"], cSettings: warnings),
        // PowerPC interpreter (32-bit user mode).
        .target(name: "CPUPPC", dependencies: ["CCore"], cSettings: warnings),
        // AppleDouble, MacBinary, resource fork parsing and decompression (host side).
        .target(name: "ForkStore", cSettings: warnings),
        // Trap dispatch and the Toolbox managers.
        // MACE 3:1 and 6:1 decoding, from FFmpeg: this target alone is LGPL 2.1+.
        .target(name: "MACE"),
        .target(name: "Toolbox", dependencies: ["CCore", "CPU68K", "CPUPPC", "ForkStore", "MACE"], cSettings: warnings),
        // Host-side helpers: Finder icons and document types, keyboard layouts, printing to PDF.
        .target(name: "BlueBoxHost", dependencies: ["Toolbox", "ForkStore", "CCore"]),
        // Command-line tool: run, import, install, apps, rsrc.
        .executableTarget(name: "bluebox", dependencies: ["BlueBoxHost", "Toolbox", "ForkStore", "CCore"]),
        // Runner for the SingleStepTests CPU vectors (test-only).
        .target(name: "CPUTestSupport", dependencies: ["CPU68K", "CCore"], path: "Tests/CPUTestSupport",
                cSettings: warnings),
        // Tests touch C globals (one guest arena per process) and run serialized.
        .testTarget(name: "ToolboxTests",
                    dependencies: ["Toolbox", "ForkStore", "CPU68K", "CPUPPC", "CCore", "CPUTestSupport", "BlueBoxHost", "MACE"],
                    swiftSettings: [.swiftLanguageMode(.v5)]),
    ],
    cLanguageStandard: .c17
)
