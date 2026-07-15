import AppKit
import Foundation

/// Opens a plug-in's own editor GUI, which runs in a separate process.
///
/// A plug-in editor can hang, leak, or crash, and it must not take the DAW down
/// with it. So the bundled "Neuracoust VST3 Editor Host" helper loads the plug-in
/// and shows its window; we speak to it over a pipe.
///
///   host → app   READY
///                PARAM <id> <normalized>      whenever the user moves a control
///                HOST_STAGE <stage> cwd=…     progress while loading
///                HOST_ERROR <message>
///   app  → host  PARAM_SET <id> <normalized>  restores what the project stored
///
/// Parameter edits arrive as a stream while a knob is turned, so they go straight
/// into the project and the running graph without touching the undo history.
@MainActor
final class PluginEditorHost: ObservableObject {
    struct Slot: Hashable {
        let trackId: Int
        let insertIndex: Int
    }

    /// Slots whose editor is open, so the UI can show them lit.
    @Published private(set) var openSlots: Set<Slot> = []
    /// Last failure, for the status strip.
    @Published private(set) var lastError: String?

    private final class Session {
        let process: Process
        let input: FileHandle
        var stdoutTail = Data()
        var ready = false

        init(process: Process, input: FileHandle) {
            self.process = process
            self.input = input
        }
    }

    private var sessions: [Slot: Session] = [:]
    private unowned let engine: EngineController

    init(engine: EngineController) {
        self.engine = engine
    }

    func isOpen(_ slot: Slot) -> Bool { openSlots.contains(slot) }

    /// Brings an already-open editor forward instead of spawning a second one.
    func toggle(trackId: Int, insertIndex: Int) {
        let slot = Slot(trackId: trackId, insertIndex: insertIndex)
        if sessions[slot] != nil {
            close(slot)
        } else {
            open(slot)
        }
    }

    /// Open the editor if it isn't already up — for auto-opening a freshly added insert, where a
    /// toggle could accidentally CLOSE an editor that happens to occupy the same slot index.
    func openIfNeeded(trackId: Int, insertIndex: Int) {
        let slot = Slot(trackId: trackId, insertIndex: insertIndex)
        if sessions[slot] == nil { open(slot) }
    }

    func close(_ slot: Slot) {
        guard let session = sessions.removeValue(forKey: slot) else { return }
        openSlots.remove(slot)
        // The host closes its window and exits on EOF; terminate is the backstop.
        try? session.input.close()
        if session.process.isRunning {
            session.process.terminate()
        }
    }

    /// Removing a track insert shifts the higher slots down, so any editor at or above the
    /// removed slot is now stale (ghost window / wrong plugin). Close those; lower slots and
    /// instrument editors (negative index) are untouched.
    func closeInsertAtOrAbove(trackId: Int, insertIndex: Int) {
        for slot in Array(sessions.keys) where slot.trackId == trackId && slot.insertIndex >= insertIndex {
            close(slot)
        }
    }

    /// Called on quit. Editors are child processes; orphaning them leaves windows behind.
    func closeAll() {
        for slot in sessions.keys {
            close(slot)
        }
    }

    // MARK: - Launch

    private func open(_ slot: Slot) {
        guard let descriptor = engine.insertDescriptor(trackId: slot.trackId, insertIndex: slot.insertIndex) else {
            lastError = "빈 인서트 슬롯입니다."
            return
        }
        guard let helper = helperURL(forFormat: descriptor.format) else {
            lastError = "\(descriptor.format) 에디터 호스트를 찾을 수 없습니다."
            return
        }

        var arguments = [
            "--plugin", descriptor.pluginPath,
            "--name", descriptor.name,
            "--title", "\(descriptor.trackName) — \(descriptor.name)",
        ]
        if !descriptor.classId.isEmpty {
            arguments += ["--class-id", descriptor.classId]
        }
        if !descriptor.className.isEmpty {
            arguments += ["--class-name", descriptor.className]
        }
        if !descriptor.observerShmName.isEmpty {
            // Read-only view of the realtime bridge: the plug-in's meters follow the
            // track's real audio instead of sitting dead.
            arguments += ["--observe-shm", descriptor.observerShmName,
                          "--observe-max-block", String(descriptor.observerMaxBlock),
                          "--observe-sample-rate", String(descriptor.observerSampleRate)]
        }

        let process = Process()
        process.executableURL = helper
        process.arguments = arguments
        // The helper resolves relative paths of its own; a plug-in's working
        // directory must not be wherever the DAW happened to be launched from.
        process.currentDirectoryURL = URL(fileURLWithPath: NSTemporaryDirectory())

        let output = Pipe()
        let input = Pipe()
        // The helper merges its diagnostics into stdout; HOST_ERROR carries them.
        process.standardOutput = output
        process.standardError = output
        process.standardInput = input

        let session = Session(process: process, input: input.fileHandleForWriting)

        output.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let chunk = handle.availableData
            guard !chunk.isEmpty else {
                handle.readabilityHandler = nil
                return
            }
            Task { @MainActor [weak self] in
                self?.consume(chunk, from: slot)
            }
        }

        process.terminationHandler = { [weak self] finished in
            Task { @MainActor [weak self] in
                guard let self else { return }
                output.fileHandleForReading.readabilityHandler = nil
                // A crash is normal for a bad plug-in; the DAW keeps running.
                if self.sessions[slot] != nil, finished.terminationReason == .uncaughtSignal {
                    self.lastError = "\(descriptor.name) 에디터가 예기치 않게 종료되었습니다."
                }
                self.sessions.removeValue(forKey: slot)
                self.openSlots.remove(slot)
            }
        }

        do {
            try process.run()
        } catch {
            lastError = "에디터 호스트를 실행할 수 없습니다: \(error.localizedDescription)"
            return
        }

        sessions[slot] = session
        openSlots.insert(slot)
        lastError = nil
    }

    private func helperURL(forFormat format: String) -> URL? {
        let helpers = Bundle.main.bundleURL.appendingPathComponent("Contents/MacOS")
        let name = format.lowercased().hasPrefix("au") ? "Neuracoust AU Editor Host"
                                                       : "Neuracoust VST3 Editor Host"
        let url = helpers.appendingPathComponent(name)
        return FileManager.default.isExecutableFile(atPath: url.path) ? url : nil
    }

    // MARK: - Protocol

    private func consume(_ chunk: Data, from slot: Slot) {
        guard let session = sessions[slot] else { return }
        session.stdoutTail.append(chunk)

        // The host writes whole lines, but a pipe read can split one in half.
        while let newline = session.stdoutTail.firstIndex(of: UInt8(ascii: "\n")) {
            let lineData = session.stdoutTail[session.stdoutTail.startIndex..<newline]
            session.stdoutTail.removeSubrange(session.stdoutTail.startIndex...newline)
            guard let line = String(data: lineData, encoding: .utf8) else { continue }
            handle(line: line.trimmingCharacters(in: .whitespaces), slot: slot, session: session)
        }
    }

    private func handle(line: String, slot: Slot, session: Session) {
        if line == "READY" {
            guard !session.ready else { return }
            session.ready = true
            sendStoredParameters(to: slot, session: session)
            return
        }
        if line.hasPrefix("HOST_ERROR ") {
            lastError = String(line.dropFirst("HOST_ERROR ".count))
            return
        }
        if line.hasPrefix("PARAM ") {
            let parts = line.split(separator: " ")
            guard parts.count >= 3,
                  let parameterId = UInt32(parts[1]),
                  let value = Double(parts[2]) else { return }
            engine.setVst3Parameter(trackId: slot.trackId,
                                    insertIndex: slot.insertIndex,
                                    parameterId: parameterId,
                                    normalizedValue: value)
            return
        }
        if line == "TRANSPORT_TOGGLE" {
            // The plug-in editor window had keyboard focus, so its host process caught the
            // spacebar and forwarded it here over the pipe. Without acting on it the
            // transport would not respond while a plug-in GUI is focused.
            engine.togglePlay()
            return
        }
        // HOST_STAGE, INSPECT, PARAMINFO: nothing to do yet.
    }

    /// Restores what the project saved, so reopening an editor shows the mix, not the defaults.
    private func sendStoredParameters(to slot: Slot, session: Session) {
        let stored = engine.storedVst3Parameters(trackId: slot.trackId, insertIndex: slot.insertIndex)
        guard !stored.isEmpty else { return }
        let payload = stored.map { "PARAM_SET \($0.id) \($0.value)\n" }.joined()
        guard let data = payload.data(using: .utf8) else { return }
        // A dead editor's pipe raises rather than returning an error.
        do {
            try session.input.write(contentsOf: data)
        } catch {
            close(slot)
        }
    }
}
