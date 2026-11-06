// The shell's Export as App…: the options of `bluebox export` in a sheet
// (the application's name, the background, how it is signed and whether
// it is notarized), then a save panel for where it goes.  The export runs
// as its own `bluebox export` process, as the shell's launches do, and says
// how it went in the status bar.

#if canImport(AppKit)
import AppKit
import BlueBoxHost
import SwiftUI
import UniformTypeIdentifiers

extension ShellModel {
    /// Make the exported application: `bluebox export`, in the background.
    func export(_ app: ShellApp, to bundle: URL, background: Bool, identity: String, notarize profile: String?) {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: Bundle.main.executablePath ?? CommandLine.arguments[0])
        let name = bundle.deletingPathExtension().lastPathComponent
        var args = ["export", app.path, "--to", bundle.deletingLastPathComponent().path, "--name", name,
                    "--sign", identity, "--force"] // the save panel has asked about replacing it
        if background { args.append("--background") }
        if let profile { args += ["--notarize", profile] }
        p.arguments = args
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = pipe
        p.standardInput = FileHandle.nullDevice
        messages.append(profile == nil ? "Exporting \(name)…" : "Exporting and notarizing \(name) (this waits for Apple)…")
        p.terminationHandler = { [weak self] p in
            let out = String(decoding: pipe.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
            let ok = p.terminationStatus == 0
            Task { @MainActor in
                guard let self else { return }
                if ok {
                    self.messages.append("Exported \(name) to \((bundle.path as NSString).abbreviatingWithTildeInPath)")
                    NSWorkspace.shared.activateFileViewerSelecting([bundle])
                } else {
                    let why = out.split(separator: "\n").last { $0.hasPrefix("bluebox:") }
                        .map { String($0.dropFirst("bluebox: ".count)) } ?? out.trimmingCharacters(in: .whitespacesAndNewlines)
                    self.messages.append("Could not export \(name): \(why)")
                }
            }
        }
        do { try p.run() } catch { messages.append("Could not export \(name): \(error.localizedDescription)") }
    }
}

/// How the sheet's signing choice is shown.
private enum SigningPick: Hashable {
    case adHoc
    case identity(String)
}

struct ExportSheet: View {
    @ObservedObject var model: ShellModel
    let app: ShellApp
    @Environment(\.dismiss) private var dismiss
    @State private var name = ""
    @State private var background = false
    @State private var signing = SigningPick.adHoc
    @State private var notarize = false
    @State private var profile = UserDefaults.standard.string(forKey: "notaryProfile") ?? ""
    /// What this Mac can sign and notarize with, once looked up.
    @State private var choices: SigningChoices?

    private var developerID: String? {
        if case .identity(let id) = signing { return id }
        return nil
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(spacing: 10) {
                Image(nsImage: model.icon(app)).resizable().interpolation(.none).frame(width: 32, height: 32)
                VStack(alignment: .leading) {
                    Text("Export \(app.name) as an Application").font(.headline)
                    Text("A Mac application that runs it on its own, with its own copy of Blue Box: it can be "
                         + "copied to another Mac. What the program saves goes to its own folder in "
                         + "~/Library/Application Support.")
                        .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                }
            }
            Form {
                TextField("Name", text: $name)
                Toggle("Run in the background (menus in the menu bar)", isOn: $background)
                    .help("As the Talking Moose runs: no Dock icon, and its windows float over the others")
                Picker("Signature", selection: $signing) {
                    Text("Ad hoc").tag(SigningPick.adHoc)
                    ForEach(choices?.developerIDs ?? [], id: \.self) { id in
                        Text(id).tag(SigningPick.identity(id))
                    }
                }
                signingNote
                Toggle("Notarize and staple", isOn: $notarize)
                    .disabled(!(choices?.canNotarize ?? false) || developerID == nil)
                if notarize && developerID != nil {
                    TextField("Keychain profile", text: $profile, prompt: Text("notarytool store-credentials name"))
                }
                if let note = choices?.notaryNote {
                    Text(note).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                } else if developerID == nil && choices != nil {
                    Text("Choose a Developer ID signature to notarize.").font(.caption).foregroundStyle(.secondary)
                }
            }
            .formStyle(.grouped)
            Text("Whether you may share someone else's program is up to you and its licence.")
                .font(.caption).foregroundStyle(.secondary)
            HStack {
                if choices == nil {
                    ProgressView().controlSize(.small)
                    Text("Looking for signing identities…").font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
                Button("Cancel", role: .cancel) { dismiss() }.keyboardShortcut(.cancelAction)
                Button("Export…") { choose() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(name.trimmingCharacters(in: .whitespaces).isEmpty
                              || (notarize && profile.trimmingCharacters(in: .whitespaces).isEmpty))
            }
        }
        .padding(20)
        .frame(width: 520)
        .onAppear {
            name = app.name
            background = isDeskAccessory
        }
        .task {
            // security and xcode-select take a moment; never xcrun's installer stub.
            let found = await Task.detached { ExportSigning.detect() }.value
            choices = found
        }
        .onChange(of: signing) { _ in if developerID == nil { notarize = false } }
    }

    @ViewBuilder private var signingNote: some View {
        if let choices, choices.developerIDs.isEmpty {
            Text("Ad hoc: it opens on this Mac; on another, macOS asks first (Open in System Settings › Privacy & Security). "
                 + "A Developer ID Application certificate in the keychain signs it for every Mac.")
                .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        } else if developerID == nil {
            Text("Ad hoc: it opens on this Mac; on another, macOS asks first.")
                .font(.caption).foregroundStyle(.secondary)
        }
    }

    /// A desk accessory file runs in the background, as install does it.
    private var isDeskAccessory: Bool { app.creator == 0x444D_4F56 /* DMOV, the Font/DA Mover's */ }

    private func choose() {
        let panel = NSSavePanel()
        panel.title = "Export \(app.name)"
        panel.nameFieldStringValue = name.trimmingCharacters(in: .whitespaces) + ".app"
        panel.allowedContentTypes = [.applicationBundle]
        panel.canCreateDirectories = true
        guard panel.runModal() == .OK, let url = panel.url else { return }
        let identity = developerID ?? "-"
        let notary = notarize && developerID != nil ? profile.trimmingCharacters(in: .whitespaces) : nil
        if let notary { UserDefaults.standard.set(notary, forKey: "notaryProfile") }
        dismiss()
        model.export(app, to: url, background: background, identity: identity, notarize: notary)
    }
}
#endif
