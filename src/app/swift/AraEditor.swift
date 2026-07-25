import AppKit

/// The window that hosts an ARA plug-in's own editor — Melodyne's blob view, drawn by Melodyne.
///
/// Unlike a VST3 insert editor, this is **in-process**. ARA hands the plug-in function pointers it
/// calls to pull our audio, so the plug-in has to live in our address space; there is no version of
/// this that runs in the editor-host process. The cost is that a crash in the plug-in is a crash in
/// the DAW, which is why the session is opened only on an explicit menu pick and torn down as soon
/// as the window closes.
///
/// The window's bottom bar is the whole contract with the project: **적용** archives the plug-in's
/// edits onto the clip and prints them into its audio, **취소** throws the session away. Nothing the
/// user does inside the plug-in reaches the project until 적용.
///
/// This class owns no engine state — `EngineController` opened the session before presenting, and
/// gets it back through `onApply` / `onCancel`.
final class AraEditorWindowController: NSObject, NSWindowDelegate {
    private let barHeight: CGFloat = 44
    private var window: NSWindow?
    private var onApply: (() -> Void)?
    private var onCancel: (() -> Void)?
    private var closing = false

    /// Attaches the already-open ARA session's editor into a new window.
    /// `attach` is handed the NSView to draw into and returns the size the plug-in asked for, or nil
    /// if the plug-in refused — in which case no window is shown.
    func present(title: String,
                 attach: (NSView) -> NSSize?,
                 onApply: @escaping () -> Void,
                 onCancel: @escaping () -> Void) -> Bool {
        self.onApply = onApply
        self.onCancel = onCancel

        // The window comes FIRST, at a provisional size. A plug-in creating its editor expects the
        // view it is handed to already live in a window — attaching to a detached NSView is how you
        // get a plug-in that reports a size and then draws nothing.
        let provisional = NSSize(width: 900, height: 600)
        let window = NSWindow(
            contentRect: NSRect(origin: .zero, size: NSSize(width: provisional.width,
                                                            height: provisional.height + barHeight)),
            styleMask: [.titled, .closable],
            backing: .buffered,
            defer: false)
        window.title = title
        window.delegate = self
        window.isReleasedWhenClosed = false

        let content = NSView(frame: NSRect(origin: .zero, size: NSSize(width: provisional.width,
                                                                       height: provisional.height + barHeight)))
        // Bottom-up: the bar owns the bottom strip, the plug-in gets everything above it. The window
        // is not resizable — an ARA editor reports one size and Melodyne does not like being fought.
        let bar = NSView(frame: NSRect(x: 0, y: 0, width: provisional.width, height: barHeight))
        let host = NSView(frame: NSRect(x: 0, y: barHeight, width: provisional.width, height: provisional.height))
        content.addSubview(bar)
        content.addSubview(host)
        window.contentView = content

        guard let requested = attach(host) else {
            window.delegate = nil
            return false
        }

        let editorWidth = max(480, requested.width)
        let editorHeight = max(320, requested.height)
        window.setContentSize(NSSize(width: editorWidth, height: editorHeight + barHeight))
        bar.frame = NSRect(x: 0, y: 0, width: editorWidth, height: barHeight)
        host.frame = NSRect(x: 0, y: barHeight, width: editorWidth, height: editorHeight)
        buildBar(bar, width: editorWidth)

        window.center()
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        self.window = window
        return true
    }

    private func buildBar(_ bar: NSView, width: CGFloat) {
        let label = NSTextField(labelWithString: "편집한 뒤 적용을 누르면 클립에 반영됩니다. 취소하면 버려집니다.")
        label.font = .systemFont(ofSize: 11)
        label.textColor = .secondaryLabelColor
        label.frame = NSRect(x: 14, y: 13, width: max(120, width - 220), height: 18)
        bar.addSubview(label)

        let cancel = NSButton(title: "취소", target: self, action: #selector(cancelPressed))
        cancel.bezelStyle = .rounded
        cancel.frame = NSRect(x: width - 190, y: 8, width: 80, height: 28)
        bar.addSubview(cancel)

        let apply = NSButton(title: "적용", target: self, action: #selector(applyPressed))
        apply.bezelStyle = .rounded
        apply.keyEquivalent = "\r"
        apply.frame = NSRect(x: width - 100, y: 8, width: 86, height: 28)
        bar.addSubview(apply)
    }

    @objc private func applyPressed() { finish(apply: true) }
    @objc private func cancelPressed() { finish(apply: false) }

    /// Runs one of the two callbacks and then takes the window down — in that order, and never the
    /// other way round. The callback is what detaches the plug-in's editor from our host view; if
    /// the window went first, releasing it would deallocate the NSView the plug-in is still drawing
    /// into. `withExtendedLifetime` is needed because the callback clears EngineController's only
    /// strong reference to this object while this method is still running.
    private func finish(apply: Bool) {
        guard !closing else { return }
        closing = true
        let action = apply ? onApply : onCancel
        onApply = nil
        onCancel = nil
        withExtendedLifetime(self) {
            action?()
            window?.delegate = nil
            window?.orderOut(nil)
            window = nil
        }
    }

    func windowWillClose(_ notification: Notification) {
        // The red button is a cancel. The buttons above have already set `closing`, so this only
        // fires for a genuine window close.
        finish(apply: false)
    }
}
