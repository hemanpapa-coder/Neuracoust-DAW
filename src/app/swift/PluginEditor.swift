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

    /// -1 addresses the primary instrument, -2 the first rack layer, and so on.
    private func instrumentSlotIndex(for slot: Slot) -> Int { -1 - slot.insertIndex }

    func close(_ slot: Slot) {
        guard let session = sessions.removeValue(forKey: slot) else { return }
        openSlots.remove(slot)
        // Ask the host to leave through its own window-close path, which is where it
        // captures the plug-in's patch. A bare terminate() is a SIGTERM and would take
        // the patch with it — the editor's program selection would be lost exactly as if
        // the DAW had never asked for it.
        var askedToQuit = false
        do {
            try session.input.write(contentsOf: Data("QUIT\n".utf8))
            askedToQuit = true
        } catch {
            askedToQuit = false
        }
        try? session.input.close()
        if session.process.isRunning {
            if askedToQuit {
                // Backstop for an editor that hangs on the way out.
                let process = session.process
                DispatchQueue.main.asyncAfter(deadline: .now() + 2.0) {
                    if process.isRunning { process.terminate() }
                }
            } else {
                session.process.terminate()
            }
        }
        if slot.insertIndex == -1 {
            // Tear the reverse monitor ring down and give the live-MIDI path back to
            // the render instance. A no-op if a newer editor owns the ring by now.
            engine.instrumentEditorClosed(trackId: slot.trackId)
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

    /// Removing/clearing an instrument (its main slot -1, or a rack layer < -1) must take its
    /// editor window with it — otherwise a ghost TRITON window lingers after "악기 제거". All
    /// instrument-slot editors use a negative insertIndex, so close every negative slot for the
    /// track (layers renumber on removal, so closing them all is the safe choice).
    func closeInstrumentEditors(trackId: Int) {
        for slot in Array(sessions.keys) where slot.trackId == trackId && slot.insertIndex < 0 {
            close(slot)
        }
    }

    /// Called on quit. Editors are child processes; orphaning them leaves windows behind.
    func closeAll() {
        // Array() first: close() removes from `sessions`, and the other close-many helpers
        // already snapshot the keys for that reason.
        for slot in Array(sessions.keys) {
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
        if slot.insertIndex < 0 {
            // Instrument rack slot: the DAW mirrors the live MIDI stream over the pipe
            // ("MIDI <status> <d1> <d2>") so the editor's own instance animates its GUI
            // keyboard/wheels.
            arguments += ["--instrument"]
            // Patch handoff both ways through one file: the host restores what the project
            // stored, and overwrites it with the program the user picks before it exits.
            // A workstation instrument's program lives in this blob and in no parameter, so
            // without it the editor's browser selection dies with the editor window.
            let stateURL = engine.instrumentStateFileURL(trackId: slot.trackId,
                                                         slotIndex: instrumentSlotIndex(for: slot))
            engine.exportInstrumentState(trackId: slot.trackId,
                                         slotIndex: instrumentSlotIndex(for: slot),
                                         to: stateURL)
            arguments += ["--state-file", stateURL.path]
            // Primary slot only: the reverse monitor ring makes the editor instance the
            // track's live voice (GUI keyboard clicks become audible). A layer editor
            // (insertIndex < -1) renders just its own layer, so handing it the whole
            // track's live path would silence the other layers — layers stay GUI-only.
            if slot.insertIndex == -1,
               let monitor = engine.instrumentEditorOpened(trackId: slot.trackId) {
                arguments += ["--monitor-shm", monitor.shmName,
                              "--monitor-max-block", String(monitor.maxBlock),
                              "--monitor-sample-rate", String(monitor.sampleRate)]
            }
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
            // The helper may emit its final preset/program snapshot while handling
            // NSWindowWillClose. Drain the pipe before the main actor removes the
            // session; otherwise consume() sees no session and silently drops the
            // patch that must be transferred to the render instance.
            output.fileHandleForReading.readabilityHandler = nil
            let finalOutput = output.fileHandleForReading.readDataToEndOfFile()
            Task { @MainActor [weak self] in
                guard let self else { return }
                if !finalOutput.isEmpty {
                    self.consume(finalOutput, from: slot)
                }
                // A final line is normally newline-terminated. Be defensive about
                // abrupt vendor exits and consume any complete UTF-8 tail as well.
                if let session = self.sessions[slot], !session.stdoutTail.isEmpty,
                   let tail = String(data: session.stdoutTail, encoding: .utf8) {
                    session.stdoutTail.removeAll()
                    self.handle(line: tail.trimmingCharacters(in: .whitespacesAndNewlines),
                                slot: slot, session: session)
                }
                // A crash is normal for a bad plug-in; the DAW keeps running.
                if self.sessions[slot] != nil, finished.terminationReason == .uncaughtSignal {
                    self.lastError = "\(descriptor.name) 에디터가 예기치 않게 종료되었습니다."
                }
                self.sessions.removeValue(forKey: slot)
                self.openSlots.remove(slot)
                // Patch first, voice second — and in that order for a reason. Taking the
                // patch rebuilds the render instance; handing the live-MIDI path back makes
                // that instance audible. Done the other way round the render instance became
                // the voice while it still held the program it was loaded with, so the first
                // notes after closing the editor sounded the OLD instrument before the new
                // one arrived.
                if slot.insertIndex < 0 {
                    // Nothing was written if the editor died before it could capture, and
                    // the project keeps what it had.
                    let slotIndex = self.instrumentSlotIndex(for: slot)
                    let stateURL = self.engine.instrumentStateFileURL(trackId: slot.trackId,
                                                                     slotIndex: slotIndex)
                    self.engine.importInstrumentState(trackId: slot.trackId,
                                                      slotIndex: slotIndex,
                                                      from: stateURL)
                }
                if slot.insertIndex == -1 {
                    // Crash or window-close exit: give the live path back to the render
                    // instance so the keyboard keeps sounding without its editor.
                    self.engine.instrumentEditorClosed(trackId: slot.trackId)
                }
            }
        }

        do {
            try process.run()
        } catch {
            lastError = "에디터 호스트를 실행할 수 없습니다: \(error.localizedDescription)"
            if slot.insertIndex == -1 {
                engine.instrumentEditorClosed(trackId: slot.trackId)
            }
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

    /// Mirrors the live MIDI stream to the open instrument editors on `trackIds` (the tracks
    /// actually hearing the keyboard), so their GUI keyboards and wheels move with the input.
    func forwardLiveMidi(trackIds: Set<Int>, events: [NCMidiLiveEvent]) {
        guard !events.isEmpty, !openSlots.isEmpty else { return }
        var payload: String?
        // Collect the sessions whose pipe broke and close them AFTER the loop: close() removes
        // from `sessions`, and mutating the dictionary while iterating it traps. With SIGPIPE now
        // ignored, a write to a just-closed editor actually reaches this catch (before, the signal
        // killed the app first), so the in-loop close became a live crash.
        var dead: [Slot] = []
        for (slot, session) in sessions
        where slot.insertIndex < 0 && session.ready && trackIds.contains(slot.trackId) {
            if payload == nil {
                payload = events.map { "MIDI \($0.status) \($0.data1) \($0.data2)\n" }.joined()
            }
            guard let data = payload?.data(using: .utf8) else { return }
            do {
                try session.input.write(contentsOf: data)
            } catch {
                dead.append(slot)
            }
        }
        for slot in dead { close(slot) }
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
