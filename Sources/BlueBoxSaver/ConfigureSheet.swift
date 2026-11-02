// The screen saver's Options sheet: which module, its controls (as After
// Dark's control panel showed them, from the module's resources), and the
// screen it gets.

#if canImport(ScreenSaver)
import AppKit
import Toolbox

@MainActor
final class ConfigureSheet: NSObject {
    let window: NSWindow
    private var settings = SaverSettings.load()
    private var modules: [String] = []
    private let onSave: (SaverSettings) -> Void

    private let modulePopup = NSPopUpButton()
    private let credit = NSTextField(wrappingLabelWithString: "")
    private let controlsStack = NSStackView()
    private let scalePopup = NSPopUpButton()
    private let depthPopup = NSPopUpButton()
    private let soundBox = NSButton(checkboxWithTitle: "Sound", target: nil, action: nil)
    private let backdropBox = NSButton(checkboxWithTitle: "Start from the desktop picture", target: nil, action: nil)
    private var controlViews: [(slot: Int, view: NSControl, info: ModuleControl)] = []
    private var info: ModuleInfo?

    init(onSave: @escaping (SaverSettings) -> Void) {
        self.onSave = onSave
        machine_screensaver_set_settings_dir(SaverSettings.settingsDir)
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 520, height: 430), styleMask: [.titled], backing: .buffered,
                          defer: true)
        super.init()
        build()
        reloadModules()
    }

    private func label(_ s: String, bold: Bool = false) -> NSTextField {
        let l = NSTextField(labelWithString: s)
        if bold { l.font = .boldSystemFont(ofSize: NSFont.systemFontSize) }
        return l
    }

    private func build() {
        let title = label("After Dark modules in Blue Box", bold: true)
        modulePopup.target = self
        modulePopup.action = #selector(moduleChosen)
        let addFolder = NSButton(title: "Add Folder…", target: self, action: #selector(addFolder))
        let moduleRow = NSStackView(views: [label("Module:"), modulePopup, addFolder])
        moduleRow.orientation = .horizontal
        credit.textColor = .secondaryLabelColor
        credit.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        credit.preferredMaxLayoutWidth = 480
        controlsStack.orientation = .vertical
        controlsStack.alignment = .leading
        controlsStack.spacing = 6

        for (t, v) in [("1 point a pixel", 1), ("2 points a pixel (classic size)", 2), ("3 points a pixel", 3)] {
            scalePopup.addItem(withTitle: t)
            scalePopup.lastItem?.tag = v
        }
        scalePopup.selectItem(withTag: settings.scale)
        for (t, v) in [("256 colours", 8), ("Thousands of colours", 16), ("Millions of colours", 32), ("16 colours", 4),
                       ("Black and white", 1)] {
            depthPopup.addItem(withTitle: t)
            depthPopup.lastItem?.tag = v
        }
        depthPopup.selectItem(withTag: settings.depth)
        depthPopup.target = self
        depthPopup.action = #selector(moduleChosen) // the controls a module shows can depend on the depth
        soundBox.state = settings.sound ? .on : .off
        backdropBox.state = settings.backdrop ? .on : .off
        let screenRow = NSStackView(views: [label("Screen:"), scalePopup, depthPopup])
        screenRow.orientation = .horizontal

        let cancel = NSButton(title: "Cancel", target: self, action: #selector(cancel))
        cancel.keyEquivalent = "\u{1b}"
        let ok = NSButton(title: "OK", target: self, action: #selector(save))
        ok.keyEquivalent = "\r"
        let buttons = NSStackView(views: [NSView(), cancel, ok])
        buttons.orientation = .horizontal

        let separator = NSBox()
        separator.boxType = .separator
        let stack = NSStackView(views: [title, moduleRow, credit, controlsStack, separator, screenRow, soundBox, backdropBox,
                                        buttons])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 10
        stack.edgeInsets = NSEdgeInsets(top: 16, left: 20, bottom: 16, right: 20)
        stack.translatesAutoresizingMaskIntoConstraints = false
        let content = NSView()
        content.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: content.leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: content.trailingAnchor),
            stack.topAnchor.constraint(equalTo: content.topAnchor),
            stack.bottomAnchor.constraint(lessThanOrEqualTo: content.bottomAnchor),
            buttons.widthAnchor.constraint(equalTo: stack.widthAnchor, constant: -40),
            separator.widthAnchor.constraint(equalTo: stack.widthAnchor, constant: -40),
            modulePopup.widthAnchor.constraint(greaterThanOrEqualToConstant: 260),
        ])
        window.contentView = content
    }

    private func reloadModules() {
        modules = ModuleCatalog.scan(settings.allFolders)
        modulePopup.removeAllItems()
        if modules.isEmpty {
            modulePopup.addItem(withTitle: "No modules: import some into Blue Box")
            modulePopup.isEnabled = false
        } else {
            modulePopup.isEnabled = true
            var seen: [String: Int] = [:]
            for m in modules {
                let name = (m as NSString).lastPathComponent
                seen[name, default: 0] += 1
                // Names repeat (several copies): the folder tells them apart.
                let title = seen[name]! > 1 ? "\(name) (\(((m as NSString).deletingLastPathComponent as NSString).lastPathComponent))" : name
                modulePopup.addItem(withTitle: title)
                modulePopup.lastItem?.representedObject = m
            }
            let chosen = settings.chosenModule()
            if let i = modules.firstIndex(where: { $0 == chosen }) { modulePopup.selectItem(at: i) }
        }
        moduleChosen()
    }

    @objc private func moduleChosen() {
        for v in controlsStack.arrangedSubviews { controlsStack.removeArrangedSubview(v); v.removeFromSuperview() }
        controlViews = []
        guard let path = modulePopup.selectedItem?.representedObject as? String else {
            credit.stringValue = ""
            info = nil
            return
        }
        let depth = depthPopup.selectedTag()
        let mi = ModuleCatalog.info(path, config: ModuleCatalog.config(depth: depth, sound: soundBox.state == .on))
        info = mi
        credit.stringValue = mi.credit + (mi.isPowerPC ? "  (PowerPC)" : "")
        let saved = settings.controls[path] ?? [] // the first version kept them here
        for (slot, c) in mi.controls.enumerated() {
            guard let c else { continue }
            let value = slot < saved.count && saved[slot] >= 0 ? saved[slot] : c.value
            let title = label(c.title.isEmpty ? "Control \(slot + 1)" : c.title)
            title.widthAnchor.constraint(equalToConstant: 150).isActive = true
            var row: [NSView] = [title]
            switch c.kind {
            case .slider:
                let s = NSSlider(value: Double(value), minValue: 0, maxValue: 100, target: self, action: #selector(sliderMoved(_:)))
                s.widthAnchor.constraint(equalToConstant: 200).isActive = true
                let unit = label(c.unitLabel(value))
                s.tag = slot
                row += [s, unit]
                controlViews.append((slot, s, c))
            case .menu:
                let p = NSPopUpButton()
                p.addItems(withTitles: c.items.isEmpty ? ["1"] : c.items)
                p.selectItem(at: max(0, min(p.numberOfItems - 1, value - 1)))
                row.append(p)
                controlViews.append((slot, p, c))
            case .checkbox:
                title.stringValue = ""
                let b = NSButton(checkboxWithTitle: c.title, target: nil, action: nil)
                b.state = value != 0 ? .on : .off
                row.append(b)
                controlViews.append((slot, b, c))
            case .text:
                if value > 0 && value <= c.items.count { row.append(label(c.items[value - 1])) } else { continue }
            case .button:
                continue // a setup dialog of the module's own: not shown here
            }
            let r = NSStackView(views: row)
            r.orientation = .horizontal
            controlsStack.addArrangedSubview(r)
        }
        if controlViews.isEmpty { controlsStack.addArrangedSubview(label("This module has no settings.")) }
        if let stack = window.contentView?.subviews.first {
            window.setContentSize(NSSize(width: 520, height: stack.fittingSize.height))
        }
    }

    @objc private func sliderMoved(_ s: NSSlider) {
        guard let row = s.superview as? NSStackView, let unit = row.arrangedSubviews.last as? NSTextField,
              let c = controlViews.first(where: { $0.view === s })?.info else { return }
        unit.stringValue = c.unitLabel(Int(s.doubleValue.rounded()))
    }

    @objc private func addFolder() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.message = "Choose a folder holding After Dark modules"
        panel.beginSheetModal(for: window) { [weak self] response in
            guard let self, response == .OK, let url = panel.url else { return }
            if !self.settings.allFolders.contains(url.path) { self.settings.folders.append(url.path) }
            self.reloadModules()
        }
    }

    @objc private func save() {
        let path = modulePopup.selectedItem?.representedObject as? String
        settings.module = path
        settings.scale = scalePopup.selectedTag()
        settings.depth = depthPopup.selectedTag()
        settings.sound = soundBox.state == .on
        settings.backdrop = backdropBox.state == .on
        if let path {
            // Into the module's settings, as After Dark's panel wrote them
            // into the module: its resources hold its controls' values.
            for (_, view, c) in controlViews {
                var value: Int
                switch c.kind {
                case .slider: value = Int((view as? NSSlider)?.doubleValue.rounded() ?? 0)
                case .menu: value = ((view as? NSPopUpButton)?.indexOfSelectedItem ?? 0) + 1
                case .checkbox: value = (view as? NSButton)?.state == .on ? 1 : 0
                default: continue
                }
                let bytes = [UInt8(truncatingIfNeeded: value >> 8), UInt8(truncatingIfNeeded: value)]
                _ = machine_screensaver_store_resource(path, ModuleCatalog.typeCode(c.type), Int16(c.id), bytes, 2)
            }
            settings.controls[path] = nil
        }
        settings.save()
        onSave(settings)
        close()
    }

    @objc private func cancel() { close() }

    private func close() {
        if let parent = window.sheetParent { parent.endSheet(window) } else { window.orderOut(nil) }
    }
}
#endif
