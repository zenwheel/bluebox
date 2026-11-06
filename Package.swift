// swift-tools-version: 6.0
import PackageDescription

/// The package's own folder: the state header is passed by its full path.
let packageDir = String(#filePath.dropLast("/Package.swift".count))

let warnings: [CSetting] = [
    .unsafeFlags(["-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-unused-function",
                  "-Wno-missing-field-initializers", // tables leave trailing fields zero on purpose
                  "-Werror=int-conversion", // a host pointer passed as a guest address
                  // Every mutable static in a state section, and the tracked
                  // allocator declared (Sources/CCore/state/bbstate.h).
                  "-include", packageDir + "/Sources/CCore/state/bbstate.h"]),
    // glibc hides POSIX/BSD declarations (strdup, localtime_r, MAP_ANON) under strict C17.
    .define("_DEFAULT_SOURCE"),
]

let package = Package(
    name: "bluebox",
    platforms: [.macOS(.v13)],
    products: [
        .executable(name: "bluebox", targets: ["bluebox"]),
        // The macOS screen saver's code (scripts/build-saver.sh makes the .saver).
        .library(name: "BlueBoxSaver", type: .dynamic, targets: ["BlueBoxSaver"]),
    ],
    dependencies: [
        // Software updates for Blue Box.app's shell (macOS only).
        .package(url: "https://github.com/sparkle-project/Sparkle", exact: "2.10.0"),
    ],
    targets: [
        // Guest memory arena, logging, generated low-memory and struct definitions.
        .target(name: "CCore", cSettings: warnings),
        // 68020 interpreter.
        .target(name: "CPU68K", dependencies: ["CCore"], cSettings: warnings),
        // PowerPC interpreter (32-bit user mode).
        .target(name: "CPUPPC", dependencies: ["CCore"], cSettings: warnings),
        // AppleDouble, MacBinary, resource fork parsing and decompression (host side).
        .target(name: "ForkStore", dependencies: ["CCore"], cSettings: warnings),
        // Trap dispatch and the Toolbox managers.
        // MACE 3:1 and 6:1 decoding, from FFmpeg: this target alone is LGPL 2.1+.
        .target(name: "MACE"),
        .target(name: "Toolbox", dependencies: ["CCore", "CPU68K", "CPUPPC", "ForkStore", "MACE"], cSettings: warnings,
                linkerSettings: [.linkedLibrary("resolv"), // the resolver: Open Transport's DNS queries
                                 // Unicode character properties (os9left_text.c): the host's ICU
                                 .linkedLibrary("icucore", .when(platforms: [.macOS])),
                                 .linkedLibrary("icuuc", .when(platforms: [.linux])),
                                .linkedLibrary("z"), // zlib: QuickTime 3's compressed movie headers
                                .linkedLibrary("sqlite3"), // the catalog (catalog.c)
                                 // EtherTalk: an interface's own Ethernet address
                                 .linkedFramework("IOKit", .when(platforms: [.macOS])),
                                 .linkedFramework("CoreFoundation", .when(platforms: [.macOS])),
                                 // Apple's OpenGL for Mac OS 9, on the host's (opengl.c)
                                 .linkedFramework("OpenGL", .when(platforms: [.macOS])),
                                 // QuickDraw 3D and RAVE, drawn with Metal (r3d_metal.m)
                                 .linkedFramework("Metal", .when(platforms: [.macOS])),
                                 .linkedFramework("Foundation", .when(platforms: [.macOS])),
                                 // QuickTime's codecs Blue Box has none of its own for (hostvideo_apple.m)
                                 .linkedFramework("VideoToolbox", .when(platforms: [.macOS])),
                                 .linkedFramework("CoreMedia", .when(platforms: [.macOS])),
                                 .linkedFramework("CoreVideo", .when(platforms: [.macOS])),
                                 .linkedFramework("AudioToolbox", .when(platforms: [.macOS])),
                                 .linkedFramework("ImageIO", .when(platforms: [.macOS])),
                                 .linkedFramework("CoreGraphics", .when(platforms: [.macOS]))]),
        // Host-side helpers: Finder icons and document types, keyboard layouts, printing to PDF.
        .target(name: "BlueBoxHost", dependencies: ["Toolbox", "ForkStore", "CCore"]),
        // Command-line tool: run, import, install, apps, rsrc, inspect.
        // Sparkle is weakly linked: the program also runs on its own, outside
        // Blue Box.app (copied onto the PATH), where there is no framework;
        // in the app it is found in Contents/Frameworks.
        .executableTarget(name: "bluebox",
                          dependencies: ["BlueBoxHost", "Toolbox", "ForkStore", "CCore",
                                         .product(name: "Sparkle", package: "Sparkle",
                                                  condition: .when(platforms: [.macOS]))],
                          linkerSettings: [.unsafeFlags(["-Xlinker", "-weak_framework", "-Xlinker", "Sparkle",
                                                         "-Xlinker", "-rpath", "-Xlinker", "@executable_path/../Frameworks"],
                                                        .when(platforms: [.macOS]))]),
        // Blue Box After Dark.saver: After Dark modules as a macOS screen saver
        // (macOS only; empty elsewhere).
        .target(name: "BlueBoxSaver", dependencies: ["Toolbox", "ForkStore", "CCore", "BlueBoxHost"],
                swiftSettings: [.swiftLanguageMode(.v5)],
                linkerSettings: [.linkedFramework("ScreenSaver", .when(platforms: [.macOS]))]),
        // Runner for the SingleStepTests CPU vectors (test-only).
        .target(name: "CPUTestSupport", dependencies: ["CPU68K", "CCore"], path: "Tests/CPUTestSupport",
                cSettings: warnings),
        // Tests touch C globals (one guest arena per process) and run serialized.
        .testTarget(name: "ToolboxTests",
                    dependencies: ["Toolbox", "ForkStore", "CPU68K", "CPUPPC", "CCore", "CPUTestSupport", "BlueBoxHost", "MACE",
                                   "BlueBoxSaver"],
                    swiftSettings: [.swiftLanguageMode(.v5)]),
    ],
    cLanguageStandard: .c17
)
