import AppKit
import Foundation
import SwiftUI

/// Owns the C++ engine and drives every live readout from a 30 Hz poll of its
/// status snapshot. The engine pushes nothing — there are no callbacks, no KVO,
/// no notifications (docs/legacy-ui-contract.md §1).
///
/// All engine calls happen on the main actor because the engine's public API is
/// main-thread-only.
@MainActor
final class EngineController: ObservableObject {
    enum ViewTab: String, CaseIterable, Identifiable {
        case edit = "Edit"
        case mix = "Mix"
        var id: String { rawValue }
    }

    // Engine-derived state, refreshed each tick.
    @Published private(set) var running = false
    @Published private(set) var transportRunning = false
    @Published private(set) var outputPeakLeft: Float = 0
    @Published private(set) var outputPeakRight: Float = 0
    @Published private(set) var sampleRate: Double = 0
    @Published private(set) var bufferSize: Int = 0
    @Published private(set) var delayCompensationMs: Double = 0
    @Published private(set) var maxRenderDurationUs: Double = 0
    @Published private(set) var deviceName = ""
    @Published private(set) var startupError: String?

    /// Smoothed playhead. Between polls it advances on the wall clock; it only
    /// snaps back to the engine when the two disagree by more than
    /// `resyncThreshold`. Reading `playbackSeconds` straight from the snapshot
    /// makes the playhead step at 30 Hz.
    @Published private(set) var playheadSeconds: Double = 0

    // UI state the engine knows nothing about.
    @Published var viewTab: ViewTab = .edit
    @Published var loopEnabled = false
    @Published var clickEnabled = false
    @Published var snapEnabled = true
    @Published var recording = false

    @Published private(set) var projectName = ""
    @Published private(set) var tempoBpm = 120

    // MARK: History
    @Published private(set) var canUndo = false
    @Published private(set) var canRedo = false
    @Published private(set) var undoStepName = ""
    @Published private(set) var redoStepName = ""
    @Published private(set) var projectDirty = false
    @Published private(set) var projectPath = ""
    @Published private(set) var lastError: String?
    @Published private(set) var timeSignature = (numerator: 4, denominator: 4)

    // MARK: Tracks

    struct Track: Identifiable {
        let id: Int
        let name: String
        let kind: TrackKind
        let colorHex: String
        let folder: String
        var inputBus: String
        var outputBus: String
        var volumeDb: Float
        var pan: Float
        var muted: Bool
        var solo: Bool
        var recordArmed: Bool
        var inputMonitoring: Bool
        var inserts: [InsertSlot]
        var sends: [String]

        var peakLeft: Float = 0
        var peakRight: Float = 0

        var panLabel: String {
            let value = Int((abs(pan) * 100).rounded())
            if value == 0 { return "C" }
            return pan < 0 ? "L\(value)" : "R\(value)"
        }
    }

    struct InsertSlot: Identifiable {
        let id: Int
        let name: String
        var bypassed: Bool
        /// "NAT", "INT", "RINT" or "EXT" — where this insert actually runs.
        let modeBadge: String

        var isEmpty: Bool { name.isEmpty || name == "No Insert" }
    }

    enum TrackKind: String {
        case audio, instrument, midi, aux, vca, folder, bus, master, monitor

        init(engineType: String) {
            self = TrackKind(rawValue: engineType) ?? .audio
        }

        var label: String {
            switch self {
            case .audio: return "AUDIO"
            case .instrument: return "INST"
            case .midi: return "MIDI"
            case .aux: return "AUX"
            case .vca: return "VCA"
            case .folder: return "FOLDER"
            case .bus: return "BUS"
            case .master: return "MASTER"
            case .monitor: return "MONITOR"
            }
        }

        /// Palette from docs/design-tokens.md.
        var accent: Color {
            switch self {
            case .audio, .folder: return Theme.Palette.accent
            case .instrument: return Theme.Palette.instrument
            case .midi: return Theme.Palette.instrument
            case .aux, .bus: return Theme.Palette.teal
            case .vca: return Theme.Palette.vca
            case .master: return Theme.Palette.amber
            case .monitor: return Theme.Palette.purple
            }
        }

        var isMasterish: Bool { self == .master || self == .monitor }
        var hasArm: Bool { self == .audio || self == .instrument || self == .midi }
        var hasSolo: Bool { !isMasterish }
        var showsInserts: Bool { self != .vca }
        var showsSends: Bool { self != .vca && self != .folder && !isMasterish }
    }

    @Published private(set) var tracks: [Track] = []

    // MARK: Plugin browser

    struct PluginCandidate: Identifiable {
        let id: Int
        let name: String
        let brand: String
        let category: String
        let format: String
        let path: String
    }

    struct Facet: Identifiable {
        let id: String
        let name: String
        let tally: Int
    }

    enum FacetKind: Int32 {
        case brand = 0, category = 1, format = 2, scope = 3
    }

    /// Which track the browser will insert into. nil while it is closed.
    @Published private(set) var pluginTargetTrack: Int?
    @Published private(set) var plugins: [PluginCandidate] = []
    @Published private(set) var brands: [Facet] = []
    @Published private(set) var categories: [Facet] = []
    @Published private(set) var formats: [Facet] = []
    @Published private(set) var totalPluginCount = 0

    @Published var pluginSearch = "" { didSet { applyPluginFilter() } }
    @Published var pluginBrand = "" { didSet { applyPluginFilter() } }
    @Published var pluginCategory = "" { didSet { applyPluginFilter() } }
    @Published var pluginFormat = "" { didSet { applyPluginFilter() } }

    var pluginBrowserOpen: Bool { pluginTargetTrack != nil }

    /// The scan costs ~90 ms over a thousand plug-ins, so it runs once and is cached
    /// in the engine. Reopening the browser reuses it.
    func openPluginBrowser(forTrack trackId: Int) {
        guard let handle else { return }
        if totalPluginCount == 0 {
            totalPluginCount = Int(nc_plugin_scan(handle))
            reloadFacets()
        }
        pluginTargetTrack = trackId
        applyPluginFilter()
    }

    func closePluginBrowser() {
        pluginTargetTrack = nil
    }

    private func reloadFacets() {
        guard let handle else { return }
        func facets(_ kind: FacetKind) -> [Facet] {
            (0..<Int(nc_plugin_facet_count(handle, kind.rawValue))).map { index in
                let i = Int32(index)
                return Facet(
                    id: readEngineString { nc_plugin_facet_name(handle, kind.rawValue, i, $0, $1) },
                    name: readEngineString { nc_plugin_facet_name(handle, kind.rawValue, i, $0, $1) },
                    tally: Int(nc_plugin_facet_tally(handle, kind.rawValue, i))
                )
            }
        }
        brands = facets(.brand)
        categories = facets(.category)
        formats = facets(.format)
    }

    private func applyPluginFilter() {
        guard let handle, totalPluginCount > 0 else { return }

        let count = Int(nc_plugin_apply_filter(handle, pluginSearch, pluginBrand, pluginCategory, pluginFormat))

        // A thousand rows of SwiftUI is fine in a LazyVStack, but building a thousand
        // structs on every keystroke is not. Cap what the browser materialises.
        let shown = min(count, 400)
        plugins = (0..<shown).map { index in
            let i = Int32(index)
            return PluginCandidate(
                id: index,
                name: readEngineString { nc_plugin_name(handle, i, $0, $1) },
                brand: readEngineString { nc_plugin_brand(handle, i, $0, $1) },
                category: readEngineString { nc_plugin_category(handle, i, $0, $1) },
                format: readEngineString { nc_plugin_format(handle, i, $0, $1) },
                path: readEngineString { nc_plugin_path(handle, i, $0, $1) }
            )
        }
        pluginMatchCount = count
    }

    @Published private(set) var pluginMatchCount = 0

    // MARK: Inserts

    func addInsert(_ pluginIndex: Int) {
        guard let handle, let trackId = pluginTargetTrack else { return }
        if nc_track_add_insert(handle, Int32(trackId), Int32(pluginIndex)) {
            reloadTracks()
            refreshHistory()
        }
    }

    func removeInsert(track trackId: Int, slot: Int) {
        guard let handle else { return }
        if nc_track_remove_insert(handle, Int32(trackId), Int32(slot)) {
            reloadTracks()
            refreshHistory()
        }
    }

    func moveInsert(track trackId: Int, slot: Int, direction: Int) {
        guard let handle else { return }
        if nc_track_move_insert(handle, Int32(trackId), Int32(slot), Int32(direction)) >= 0 {
            reloadTracks()
            refreshHistory()
        }
    }

    func toggleInsertBypass(track trackId: Int, slot: Int) {
        guard let handle,
              let track = tracks.first(where: { $0.id == trackId }),
              slot < track.inserts.count else { return }
        nc_track_set_insert_bypassed(handle, Int32(trackId), Int32(slot), !track.inserts[slot].bypassed)
        reloadTracks()
        refreshHistory()
    }

    // MARK: Monitor station

    struct MonitorModule: Identifiable {
        let id: Int
        let name: String
        let detail: String
        let stage: String
        var enabled: Bool

        var displayDetail: String { stripSlotPrefix(detail) }
    }

    struct SpeakerSet: Identifiable {
        let id: Int
        let letter: String
        let name: String
        var model: String
        var output: String
        var simWeight: Float
        var roomEq: Bool

        /// The engine stores models as "Speaker B: Yamaha NS-10M Studio (NF)".
        /// The slot letter is already on the tab, so drop the prefix.
        var displayModel: String { stripSlotPrefix(model) }
    }

    enum ListenMode: String, CaseIterable, Identifiable {
        case stereo = "LR"
        case mono = "MONO"
        case midSide = "MS"
        case polarity = "POL"

        var id: String { rawValue }
        var label: String {
            switch self {
            case .stereo: return "Stereo"
            case .mono: return "Mono"
            case .midSide: return "M/S"
            case .polarity: return "Ø"
            }
        }
    }

    enum OutputMode { case speaker, headphone }

    @Published private(set) var monitorModules: [MonitorModule] = []
    @Published private(set) var speakerSets: [SpeakerSet] = []
    @Published private(set) var activeSpeakerSlot = 0
    @Published private(set) var monitorVolumeDb: Float = -6
    @Published private(set) var listenMode: ListenMode = .stereo
    @Published private(set) var monitorDim = false
    @Published private(set) var monitorMono = false
    @Published private(set) var monitorTalkback = false
    @Published private(set) var monitorDspEnabled = true
    @Published private(set) var monitorPathMode = "internal"

    /// Output mode is a UI concept — the engine models speaker vs headphone as
    /// which simulation module is enabled.
    @Published var outputMode: OutputMode = .speaker

    // Live meters, refreshed each tick.
    @Published private(set) var phaseCorrelation: Float = 0
    @Published private(set) var spectrumLow: Float = 0
    @Published private(set) var spectrumMid: Float = 0
    @Published private(set) var spectrumHigh: Float = 0
    @Published private(set) var wakeJitterUs: Double = 0
    @Published private(set) var remoteDspActive = false
    @Published private(set) var remoteDspRoundTripMs: Double = 0

    /// Inserts the engine is actually running, summed across paths. This is the
    /// only honest signal that a plug-in loaded rather than merely being listed.
    @Published private(set) var activeInsertCount = 0

    /// Fraction of the buffer period consumed by the worst recent render pass.
    /// This is render headroom, which is what actually predicts dropouts — raw
    /// wake jitter reads ~1 buffer period even when idle on Waves SoundGrid.
    var dspLoadFraction: Double {
        guard sampleRate > 0, bufferSize > 0 else { return 0 }
        let bufferPeriodUs = Double(bufferSize) / sampleRate * 1_000_000
        return min(1.0, maxRenderDurationUs / bufferPeriodUs)
    }

    /// `NCEngine` is opaque in C, so Swift imports the handle as an OpaquePointer.
    /// Marked nonisolated so `deinit` can free it; nothing else ever holds it.
    private nonisolated(unsafe) var handle: OpaquePointer?
    private var timer: Timer?
    private var keyMonitor: Any?

    /// Listen Room drives its own relay process but reads engine state, so it
    /// borrows the handle and rides this controller's tick.
    weak var listenRoom: ListenRoomController?

    var rawHandle: OpaquePointer? { handle }

    func readEngineString(capacity: Int = Int(NC_TEXT_LEN),
                          _ fill: (UnsafeMutablePointer<CChar>, Int) -> Void) -> String {
        var buffer = [CChar](repeating: 0, count: capacity)
        fill(&buffer, buffer.count)
        return String(cString: buffer)
    }

    private var transportWallClockStart: CFTimeInterval = 0
    private var transportWallClockBase: Double = 0

    private let tickInterval = 1.0 / 30.0
    private let resyncThreshold = 0.18

    init() {
        handle = nc_engine_create()
    }

    deinit {
        if let handle {
            nc_engine_stop(handle)
            nc_engine_destroy(handle)
        }
    }

    func start() {
        guard let handle else { return }

        var errorBuffer = [CChar](repeating: 0, count: 256)
        let ok = nc_engine_start(handle, &errorBuffer, errorBuffer.count)
        if !ok {
            startupError = String(cString: errorBuffer)
        }

        projectName = readString { nc_project_name(handle, $0, $1) }
        tempoBpm = Int(nc_project_tempo_bpm(handle))
        timeSignature = (
            Int(nc_project_time_signature_numerator(handle)),
            Int(nc_project_time_signature_denominator(handle))
        )
        loopEnabled = nc_project_loop_enabled(handle)
        reloadTracks()
        reloadClips()
        reloadMonitorState()
        nc_history_reset(handle)
        refreshHistory()
        installKeyMonitor()

        let timer = Timer(timeInterval: tickInterval, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.tick() }
        }
        RunLoop.main.add(timer, forMode: .common)
        self.timer = timer
    }

    /// The old UI drove every shortcut from an NSEvent monitor rather than menu key
    /// equivalents — all 308 of its menu items carry an empty keyEquivalent. Menu
    /// shortcuts do not reach this window reliably, so do the same here.
    private func installKeyMonitor() {
        keyMonitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self] event in
            guard let self else { return event }
            return MainActor.assumeIsolated { self.handleKeyDown(event) }
        }
    }

    /// Hardware key codes on an ANSI layout. Matching on characters breaks the
    /// moment a Korean input source is active: `charactersIgnoringModifiers` then
    /// reports a jamo, not a Latin letter. Every shortcut goes through here, which
    /// is why the menu items' own key equivalents never have to fire.
    private enum KeyCode {
        static let z: UInt16 = 6
        static let s: UInt16 = 1
        static let o: UInt16 = 31
        static let n: UInt16 = 45
        static let i: UInt16 = 34
        static let b: UInt16 = 11
        static let c: UInt16 = 8
        static let x: UInt16 = 7
        static let v: UInt16 = 9
        static let d: UInt16 = 2
        static let delete: UInt16 = 51
        static let forwardDelete: UInt16 = 117
    }

    private func handleKeyDown(_ event: NSEvent) -> NSEvent? {
        // Never steal keys while the user is typing.
        if NSApp.keyWindow?.firstResponder is NSTextView {
            return event
        }
        // Timeline edits carry no modifier, the way every DAW does it.
        if !event.modifierFlags.contains(.command) {
            switch event.keyCode {
            case KeyCode.b where selectedClipId != nil:
                splitSelectedClipAtPlayhead()
                return nil
            case KeyCode.delete, KeyCode.forwardDelete:
                guard selectedClipId != nil else { return event }
                deleteSelectedClip()
                return nil
            default:
                return event
            }
        }

        let shift = event.modifierFlags.contains(.shift)

        switch event.keyCode {
        case KeyCode.z:
            shift ? redo() : undo()
        case KeyCode.s:
            shift ? saveProjectAs() : saveProject()
        case KeyCode.o:
            openProject()
        case KeyCode.n:
            newProject()
        case KeyCode.i:
            importAudio(intoTrack: 0)
        case KeyCode.c where selectedClipId != nil:
            copySelectedClip()
        case KeyCode.x where selectedClipId != nil:
            cutSelectedClip()
        case KeyCode.v where clipboardClipName != nil:
            pasteClipAtPlayhead()
        case KeyCode.d where selectedClipId != nil:
            duplicateSelectedClip()
        default:
            return event
        }
        return nil
    }

    func shutdown() {
        if let keyMonitor {
            NSEvent.removeMonitor(keyMonitor)
        }
        keyMonitor = nil
        timer?.invalidate()
        timer = nil
        if let handle {
            nc_engine_stop(handle)
        }
    }

    // MARK: - Transport

    func togglePlay() {
        setTransport(running: !transportRunning)
    }

    func stop() {
        setTransport(running: false)
        seek(0)
    }

    func rewind() {
        guard let handle else { return }
        nc_engine_rewind(handle)
        playheadSeconds = 0
        transportWallClockBase = 0
        transportWallClockStart = CACurrentMediaTime()
    }

    func seek(_ seconds: Double) {
        guard let handle else { return }
        let clamped = max(0, seconds)
        nc_engine_seek(handle, clamped)
        playheadSeconds = clamped
        transportWallClockBase = clamped
        transportWallClockStart = CACurrentMediaTime()
    }

    func toggleRecording() {
        guard let handle else { return }
        recording.toggle()
        nc_engine_set_recording(handle, recording)
    }

    func toggleLoop() {
        guard let handle else { return }
        loopEnabled.toggle()
        nc_project_set_loop_enabled(handle, loopEnabled)
    }

    func toggleClick() {
        guard let handle else { return }
        clickEnabled.toggle()
        nc_engine_set_metronome_enabled(handle, clickEnabled)
    }

    private func setTransport(running: Bool) {
        guard let handle else { return }
        nc_engine_set_transport_running(handle, running)
        transportRunning = running
        transportWallClockBase = playheadSeconds
        transportWallClockStart = CACurrentMediaTime()
    }

    // MARK: - Readouts

    var timecode: String {
        guard let handle else { return "00:00:00:00" }
        return readString { nc_project_timecode(handle, playheadSeconds, $0, $1) }
    }

    var barsBeats: (bar: Int, beat: Int, tick: Int) {
        guard let handle else { return (1, 1, 0) }
        var bar: Int32 = 1
        var beat: Int32 = 1
        var tick: Int32 = 0
        nc_project_bars_beats(handle, playheadSeconds, &bar, &beat, &tick)
        return (Int(bar), Int(beat), Int(tick))
    }

    // MARK: - Tracks

    /// Structure only. Peaks come from the tick; volume/pan/mute follow the setters.
    private func reloadTracks() {
        guard let handle else { return }

        tracks = (0..<Int(nc_track_count(handle))).map { index in
            let i = Int32(index)
            let insertCount = Int(nc_track_insert_count(handle, i))
            let sendCount = Int(nc_track_send_count(handle, i))

            return Track(
                id: index,
                name: readEngineString { nc_track_name(handle, i, $0, $1) },
                kind: TrackKind(engineType: readEngineString { nc_track_type(handle, i, $0, $1) }),
                colorHex: readEngineString { nc_track_color(handle, i, $0, $1) },
                folder: readEngineString { nc_track_folder(handle, i, $0, $1) },
                inputBus: readEngineString { nc_track_input_bus(handle, i, $0, $1) },
                outputBus: readEngineString { nc_track_output_bus(handle, i, $0, $1) },
                volumeDb: nc_track_volume_db(handle, i),
                pan: nc_track_pan(handle, i),
                muted: nc_track_muted(handle, i),
                solo: nc_track_solo(handle, i),
                recordArmed: nc_track_record_armed(handle, i),
                inputMonitoring: nc_track_input_monitoring(handle, i),
                inserts: (0..<insertCount).map { slot in
                    let s = Int32(slot)
                    return InsertSlot(
                        id: slot,
                        name: readEngineString { nc_track_insert_name(handle, i, s, $0, $1) },
                        bypassed: nc_track_insert_bypassed(handle, i, s),
                        modeBadge: readEngineString { nc_track_insert_mode_badge(handle, i, s, $0, $1) }
                    )
                },
                sends: (0..<sendCount).map { slot in
                    readEngineString { nc_track_send_bus(handle, i, Int32(slot), $0, $1) }
                }
            )
        }
    }

    /// Mixer strips show every track; the master meter panel handles the master bus.
    var mixerTracks: [Track] { tracks.filter { $0.kind != .monitor } }

    /// Volume and pan are continuous. The bridge records no history for them; the
    /// view calls recordGesture once the drag ends, so one drag is one undo step.
    func setTrackVolume(_ id: Int, _ db: Float) {
        guard let handle else { return }
        nc_track_set_volume_db(handle, Int32(id), db)
        syncTrack(id)
    }

    func setTrackPan(_ id: Int, _ pan: Float) {
        guard let handle else { return }
        nc_track_set_pan(handle, Int32(id), pan)
        syncTrack(id)
    }

    func recordGesture(_ stepName: String) {
        guard let handle else { return }
        if nc_history_record_gesture(handle, stepName) {
            refreshHistory()
        }
    }

    func undo() {
        guard let handle, nc_history_undo(handle) else { return }
        reloadTracks()
        reloadClips()
        reloadMonitorState()
        refreshHistory()
    }

    func redo() {
        guard let handle, nc_history_redo(handle) else { return }
        reloadTracks()
        reloadClips()
        reloadMonitorState()
        refreshHistory()
    }

    // MARK: - Project file I/O

    /// Asks before throwing away unsaved work. Returns false when the user cancels.
    /// A clean document never prompts.
    @discardableResult
    func confirmDiscardingChanges() -> Bool {
        guard projectDirty else { return true }

        let alert = NSAlert()
        alert.messageText = "저장하지 않은 변경 사항이 있습니다"
        alert.informativeText = projectPath.isEmpty
            ? "이 프로젝트는 아직 저장된 적이 없습니다."
            : (projectPath as NSString).lastPathComponent
        alert.addButton(withTitle: "저장")
        alert.addButton(withTitle: "저장하지 않음")
        alert.addButton(withTitle: "취소")

        switch alert.runModal() {
        case .alertFirstButtonReturn:
            return saveProject()
        case .alertSecondButtonReturn:
            return true
        default:
            return false
        }
    }

    /// Panels are user-initiated; nothing here writes without an explicit choice.
    func newProject() {
        guard let handle, confirmDiscardingChanges() else { return }
        nc_project_new(handle)
        afterProjectReplaced()
    }

    func openProject() {
        guard confirmDiscardingChanges() else { return }

        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.init(filenameExtension: "ndaw")].compactMap { $0 }
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        guard panel.runModal() == .OK, let url = panel.url else { return }
        openProject(at: url)
    }

    @discardableResult
    func saveProject() -> Bool {
        guard let handle else { return false }
        if projectPath.isEmpty {
            return saveProjectAs()
        }
        var errorBuffer = [CChar](repeating: 0, count: 256)
        guard nc_project_save(handle, &errorBuffer, errorBuffer.count) else {
            lastError = String(cString: errorBuffer)
            return false
        }
        refreshHistory()
        return true
    }

    @discardableResult
    func saveProjectAs() -> Bool {
        guard let handle else { return false }

        let panel = NSSavePanel()
        panel.allowedContentTypes = [.init(filenameExtension: "ndaw")].compactMap { $0 }
        panel.nameFieldStringValue = projectName.isEmpty ? "Untitled" : projectName
        guard panel.runModal() == .OK, let url = panel.url else { return false }

        var errorBuffer = [CChar](repeating: 0, count: 256)
        guard nc_project_save_as(handle, url.path, &errorBuffer, errorBuffer.count) else {
            lastError = String(cString: errorBuffer)
            return false
        }
        refreshHistory()
        return true
    }

    /// Opens a document or imports audio, chosen by extension. Used by the Finder
    /// hand-off (`application(_:open:)`) as well as the panels.
    func open(urls: [URL]) {
        for url in urls {
            if url.pathExtension.lowercased() == "ndaw" {
                openProject(at: url)
            } else if nc_audio_import_supported(url.path) {
                importAudio(intoTrack: 0, from: [url])
            } else {
                lastError = "지원하지 않는 파일: \(url.lastPathComponent)"
            }
        }
    }

    private func openProject(at url: URL) {
        guard let handle, confirmDiscardingChanges() else { return }

        var preferAutosave = false
        if nc_project_autosave_is_newer(url.path) {
            let alert = NSAlert()
            alert.messageText = "복구할 자동 저장본이 있습니다"
            alert.informativeText = "이 프로젝트보다 나중에 저장된 자동 저장본이 있습니다. 복구하시겠습니까?"
            alert.addButton(withTitle: "복구")
            alert.addButton(withTitle: "무시하고 열기")
            preferAutosave = alert.runModal() == .alertFirstButtonReturn
        }

        var errorBuffer = [CChar](repeating: 0, count: 256)
        if nc_project_open(handle, url.path, preferAutosave, &errorBuffer, errorBuffer.count) {
            afterProjectReplaced()
        } else {
            lastError = String(cString: errorBuffer)
        }
    }

    func importAudio(intoTrack trackId: Int) {
        guard let handle else { return }

        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = true
        panel.canChooseDirectories = false
        guard panel.runModal() == .OK else { return }
        importAudio(intoTrack: trackId, from: panel.urls)
    }

    func importAudio(intoTrack trackId: Int, from urls: [URL]) {
        guard let handle else { return }

        // Sequential placement: each file starts where the previous one ended.
        var start = playheadSeconds
        for url in urls {
            guard nc_audio_import_supported(url.path) else {
                lastError = "지원하지 않는 형식: \(url.lastPathComponent)"
                continue
            }
            var errorBuffer = [CChar](repeating: 0, count: 256)
            if nc_audio_import(handle, Int32(trackId), url.path, start, &errorBuffer, errorBuffer.count) {
                start += clipDuration(ofLast: trackId)
            } else {
                lastError = String(cString: errorBuffer)
            }
        }
        reloadTracks()
        reloadClips()
        refreshHistory()
    }

    private func clipDuration(ofLast trackId: Int) -> Double {
        guard let handle else { return 0 }
        let count = Int(nc_clip_count(handle))
        return count > 0 ? nc_clip_duration_seconds(handle, Int32(count - 1)) : 0
    }

    private func afterProjectReplaced() {
        guard let handle else { return }
        projectName = readEngineString { nc_project_name(handle, $0, $1) }
        tempoBpm = Int(nc_project_tempo_bpm(handle))
        timeSignature = (
            Int(nc_project_time_signature_numerator(handle)),
            Int(nc_project_time_signature_denominator(handle))
        )
        loopEnabled = nc_project_loop_enabled(handle)
        reloadTracks()
        reloadClips()
        reloadMonitorState()
        refreshHistory()
        lastError = nil
    }

    // MARK: - Clips

    struct Clip: Identifiable {
        let id: String
        let name: String
        let trackName: String
        let sourcePath: String
        let startSeconds: Double
        let durationSeconds: Double
    }

    @Published private(set) var clips: [Clip] = []

    /// Peak envelopes keyed by source path, fetched once per file from the engine.
    @Published private(set) var waveforms: [String: (mins: [Float], maxs: [Float])] = [:]

    /// Timeline selection. Purely a view concept; the engine has no notion of it.
    @Published var selectedClipId: String?

    // Timeline viewport, in seconds.
    @Published private(set) var visibleStart: Double = 0
    @Published private(set) var visibleDuration: Double = 30

    func setViewport(start: Double, duration: Double) {
        visibleStart = max(0, start)
        visibleDuration = min(600, max(0.25, duration))
    }

    /// Zooms about the middle of the view, which is what a button press implies.
    func zoomTimeline(by factor: Double) {
        let centre = visibleStart + visibleDuration / 2
        let duration = min(600, max(0.25, visibleDuration * factor))
        setViewport(start: centre - duration / 2, duration: duration)
    }

    /// Frames every clip, with a little air after the last one.
    func fitTimeline() {
        let end = clips.map { $0.startSeconds + $0.durationSeconds }.max() ?? 0
        setViewport(start: 0, duration: end > 0 ? end * 1.05 : 30)
    }

    /// Lanes come from the tracks the timeline can hold clips on; Master and
    /// Monitor are buses, not lanes.
    var timelineModel: TimelineModel {
        let lanes = tracks.filter { !$0.kind.isMasterish }
        let laneIndex = Dictionary(uniqueKeysWithValues: lanes.enumerated().map { ($1.name, $0) })

        return TimelineModel(
            lanes: lanes.map {
                TimelineModel.Lane(name: $0.name,
                                   accent: NSColor.from($0.kind.accent),
                                   muted: $0.muted)
            },
            clips: clips.compactMap { clip in
                guard let lane = laneIndex[clip.trackName] else { return nil }
                return TimelineModel.Clip(id: clip.id,
                                          name: clip.name,
                                          laneIndex: lane,
                                          startSeconds: clip.startSeconds,
                                          durationSeconds: clip.durationSeconds,
                                          sourcePath: clip.sourcePath,
                                          selected: clip.id == selectedClipId)
            },
            tempoBpm: tempoBpm,
            beatsPerBar: timeSignature.numerator,
            visibleStart: visibleStart,
            visibleDuration: visibleDuration
        )
    }

    // MARK: - Clip editing

    /// Snapping is the caller's choice: `nc_project_snap_time` always snaps.
    func snap(_ seconds: Double) -> Double {
        guard let handle, snapEnabled else { return seconds }
        return nc_project_snap_time(handle, seconds)
    }

    /// Continuous. Records nothing; call `commitClipGesture` when the drag ends.
    func moveClip(_ clipId: String, to startSeconds: Double) {
        guard let handle else { return }
        if nc_clip_move(handle, clipId, startSeconds) { reloadClips() }
    }

    func trimClipStart(_ clipId: String, to startSeconds: Double) {
        guard let handle else { return }
        if nc_clip_trim_start(handle, clipId, startSeconds) { reloadClips() }
    }

    func trimClipEnd(_ clipId: String, to endSeconds: Double) {
        guard let handle else { return }
        if nc_clip_trim_end(handle, clipId, endSeconds) { reloadClips() }
    }

    func commitClipGesture(_ stepName: String) {
        recordGesture(stepName)
    }

    // MARK: Clipboard

    @Published private(set) var clipboardClipName: String?

    func copySelectedClip() {
        guard let handle, let clipId = selectedClipId, nc_clip_copy(handle, clipId) else { return }
        refreshClipboard()
    }

    func cutSelectedClip() {
        guard let handle, let clipId = selectedClipId, nc_clip_cut(handle, clipId) else { return }
        selectedClipId = nil
        reloadClips()
        refreshClipboard()
        refreshHistory()
    }

    /// Pastes at the playhead, onto the clip's original track.
    func pasteClipAtPlayhead() {
        guard let handle else { return }
        var buffer = [CChar](repeating: 0, count: 128)
        guard nc_clip_paste(handle, playheadSeconds, &buffer, buffer.count) else { return }
        selectedClipId = String(cString: buffer)
        reloadClips()
        refreshHistory()
    }

    func duplicateSelectedClip() {
        guard let handle, let clipId = selectedClipId else { return }
        var buffer = [CChar](repeating: 0, count: 128)
        guard nc_clip_duplicate(handle, clipId, &buffer, buffer.count) else { return }
        selectedClipId = String(cString: buffer)
        reloadClips()
        refreshHistory()
    }

    private func refreshClipboard() {
        guard let handle else { return }
        clipboardClipName = nc_clipboard_has_clip(handle)
            ? readEngineString { nc_clipboard_clip_name(handle, $0, $1) }
            : nil
    }

    func splitSelectedClipAtPlayhead() {
        guard let handle, let clipId = selectedClipId else { return }
        if nc_clip_split(handle, clipId, playheadSeconds) {
            reloadClips()
            refreshHistory()
        }
    }

    func deleteSelectedClip() {
        guard let handle, let clipId = selectedClipId else { return }
        if nc_clip_delete(handle, clipId) {
            selectedClipId = nil
            reloadClips()
            refreshHistory()
        }
    }

    private func loadWaveforms() {
        guard let handle else { return }
        for clip in clips where waveforms[clip.sourcePath] == nil {
            var mins = [Float](repeating: 0, count: Int(NC_WAVEFORM_BUCKETS))
            var maxs = [Float](repeating: 0, count: Int(NC_WAVEFORM_BUCKETS))
            if nc_waveform_peaks(handle, clip.sourcePath, &mins, &maxs) {
                waveforms[clip.sourcePath] = (mins, maxs)
            }
        }
    }

    private func reloadClips() {
        guard let handle else { return }
        clips = (0..<Int(nc_clip_count(handle))).map { index in
            let i = Int32(index)
            return Clip(
                id: readEngineString { nc_clip_id(handle, i, $0, $1) },
                name: readEngineString(capacity: 512) { nc_clip_name(handle, i, $0, $1) },
                trackName: readEngineString { nc_clip_track(handle, i, $0, $1) },
                sourcePath: readEngineString(capacity: 1024) { nc_clip_source_path(handle, i, $0, $1) },
                startSeconds: nc_clip_start_seconds(handle, i),
                durationSeconds: nc_clip_duration_seconds(handle, i)
            )
        }
        loadWaveforms()
    }

    private func refreshHistory() {
        guard let handle else { return }
        canUndo = nc_history_can_undo(handle)
        canRedo = nc_history_can_redo(handle)
        projectDirty = nc_project_dirty(handle)
        projectPath = readEngineString(capacity: 1024) { nc_project_path(handle, $0, $1) }
        undoStepName = readEngineString { nc_history_undo_step_name(handle, $0, $1) }
        redoStepName = readEngineString { nc_history_redo_step_name(handle, $0, $1) }
    }

    func toggleTrackMute(_ id: Int) {
        guard let handle, let track = tracks.first(where: { $0.id == id }) else { return }
        nc_track_set_muted(handle, Int32(id), !track.muted)
        syncTrack(id)
        refreshHistory()
    }

    /// Solo is additive here, the way the engine models it — several tracks can be
    /// soloed at once, and Master/Monitor refuse it.
    func toggleTrackSolo(_ id: Int) {
        guard let handle, let track = tracks.first(where: { $0.id == id }) else { return }
        nc_track_set_solo(handle, Int32(id), !track.solo)
        syncTrack(id)
        refreshHistory()
    }

    func toggleTrackArm(_ id: Int) {
        guard let handle, let track = tracks.first(where: { $0.id == id }) else { return }
        nc_track_set_record_armed(handle, Int32(id), !track.recordArmed)
        syncTrack(id)
        refreshHistory()
    }

    func toggleTrackInputMonitoring(_ id: Int) {
        guard let handle, let track = tracks.first(where: { $0.id == id }) else { return }
        nc_track_set_input_monitoring(handle, Int32(id), !track.inputMonitoring)
        syncTrack(id)
        refreshHistory()
    }

    /// Re-read one track's mutable fields from the engine rather than assuming the
    /// write landed — the edit operations clamp and can refuse.
    private func syncTrack(_ id: Int) {
        guard let handle, let position = tracks.firstIndex(where: { $0.id == id }) else { return }
        let i = Int32(id)
        tracks[position].volumeDb = nc_track_volume_db(handle, i)
        tracks[position].pan = nc_track_pan(handle, i)
        tracks[position].muted = nc_track_muted(handle, i)
        tracks[position].solo = nc_track_solo(handle, i)
        tracks[position].recordArmed = nc_track_record_armed(handle, i)
        tracks[position].inputMonitoring = nc_track_input_monitoring(handle, i)
    }

    /// Meters arrive keyed by track name, so match on name, not position.
    private func applyTrackMeters(_ status: NCEngineStatus) {
        guard !tracks.isEmpty else { return }

        var peaks: [String: (Float, Float)] = [:]
        let count = min(Int(status.trackMeterCount), Int(NC_MAX_TRACK_METERS))
        withUnsafePointer(to: status.trackMeterNames) { namesPointer in
            namesPointer.withMemoryRebound(to: CChar.self,
                                           capacity: Int(NC_MAX_TRACK_METERS) * Int(NC_NAME_LEN)) { flat in
                for index in 0..<count {
                    let name = String(cString: flat.advanced(by: index * Int(NC_NAME_LEN)))
                    guard !name.isEmpty else { continue }
                    let left = withUnsafePointer(to: status.trackPeakLeft) {
                        $0.withMemoryRebound(to: Float.self, capacity: Int(NC_MAX_TRACK_METERS)) { $0[index] }
                    }
                    let right = withUnsafePointer(to: status.trackPeakRight) {
                        $0.withMemoryRebound(to: Float.self, capacity: Int(NC_MAX_TRACK_METERS)) { $0[index] }
                    }
                    peaks[name] = (left, right)
                }
            }
        }

        for index in tracks.indices {
            let (left, right) = peaks[tracks[index].name] ?? (0, 0)
            tracks[index].peakLeft = left
            tracks[index].peakRight = right
        }
    }

    // MARK: - Monitor station

    private func reloadMonitorState() {
        guard let handle else { return }

        monitorModules = (0..<Int(nc_monitor_module_count(handle))).map { index in
            let i = Int32(index)
            return MonitorModule(
                id: index,
                name: readString { nc_monitor_module_name(handle, i, $0, $1) },
                detail: readString { nc_monitor_module_detail(handle, i, $0, $1) },
                stage: readString { nc_monitor_module_stage(handle, i, $0, $1) },
                enabled: nc_monitor_module_enabled(handle, i)
            )
        }

        let names = ["Mains", "Nearfield", "Grot Box"]
        speakerSets = (0..<3).map { slot in
            let s = Int32(slot)
            return SpeakerSet(
                id: slot,
                letter: ["A", "B", "C"][slot],
                name: names[slot],
                model: readString { nc_monitor_speaker_model(handle, s, $0, $1) },
                output: readString { nc_monitor_speaker_output(handle, s, $0, $1) },
                simWeight: nc_monitor_speaker_sim_weight(handle, s),
                roomEq: nc_monitor_speaker_room_eq(handle, s)
            )
        }

        activeSpeakerSlot = Int(nc_monitor_active_speaker_slot(handle))
        monitorVolumeDb = nc_monitor_volume_db(handle)
        monitorDim = nc_monitor_dim(handle)
        monitorMono = nc_monitor_mono(handle)
        monitorTalkback = nc_monitor_talkback(handle)
        monitorDspEnabled = nc_monitor_dsp_enabled(handle)
        monitorPathMode = readString { nc_monitor_path_mode(handle, $0, $1) }
        listenMode = ListenMode(rawValue: readString { nc_monitor_listen_mode(handle, $0, $1) }) ?? .stereo
    }

    var activeSpeakerSet: SpeakerSet? {
        speakerSets.first { $0.id == activeSpeakerSlot }
    }

    func setMonitorVolume(_ db: Float) {
        guard let handle else { return }
        nc_monitor_set_volume_db(handle, db)
        monitorVolumeDb = nc_monitor_volume_db(handle)
    }

    func setListenMode(_ mode: ListenMode) {
        guard let handle else { return }
        // Mono is its own engine flag; the other three are listen-mode strings.
        nc_monitor_set_mono(handle, mode == .mono)
        nc_monitor_set_listen_mode(handle, mode.rawValue)
        listenMode = mode
        monitorMono = nc_monitor_mono(handle)
    }

    func toggleDim() {
        guard let handle else { return }
        nc_monitor_set_dim(handle, !monitorDim)
        monitorDim = nc_monitor_dim(handle)
    }

    func toggleMonitorMono() {
        guard let handle else { return }
        nc_monitor_set_mono(handle, !monitorMono)
        monitorMono = nc_monitor_mono(handle)
    }

    func toggleTalkback() {
        guard let handle else { return }
        nc_monitor_set_talkback(handle, !monitorTalkback)
        monitorTalkback = nc_monitor_talkback(handle)
    }

    func setSpeakerSlot(_ slot: Int) {
        guard let handle else { return }
        nc_monitor_set_active_speaker_slot(handle, Int32(slot))
        // The speaker-simulation module reports the model of whichever slot is
        // active, so its row text goes stale unless the list is re-read.
        reloadMonitorState()
    }

    func setModuleEnabled(_ index: Int, _ enabled: Bool) {
        guard let handle else { return }
        nc_monitor_set_module_enabled(handle, Int32(index), enabled)
        if let position = monitorModules.firstIndex(where: { $0.id == index }) {
            monitorModules[position].enabled = nc_monitor_module_enabled(handle, Int32(index))
        }
    }

    func bypassAllModules() {
        guard let handle else { return }
        nc_monitor_set_dsp_enabled(handle, !monitorDspEnabled)
        monitorDspEnabled = nc_monitor_dsp_enabled(handle)
    }

    func setMonitorPathMode(_ mode: String) {
        guard let handle else { return }
        nc_monitor_set_path_mode(handle, mode)
        monitorPathMode = readString { nc_monitor_path_mode(handle, $0, $1) }
    }

    // MARK: - Poll loop

    private func tick() {
        guard let handle else { return }

        var status = NCEngineStatus()
        nc_engine_status(handle, &status)

        phaseCorrelation = status.phaseCorrelation
        spectrumLow = status.spectrumLow
        spectrumMid = status.spectrumMid
        spectrumHigh = status.spectrumHigh
        wakeJitterUs = status.realtimeAverageWakeJitterUs
        remoteDspActive = status.remoteDspMonitorActive
        remoteDspRoundTripMs = status.remoteDspRoundTripMs
        activeInsertCount = Int(status.activeRealtimeVst3TrackInserts)
            + Int(status.activeRealtimeVst3MasterInserts)
            + Int(status.activeRemoteDspTrackInserts)

        running = status.running
        transportRunning = status.transportRunning
        outputPeakLeft = status.outputPeakLeft
        outputPeakRight = status.outputPeakRight
        sampleRate = status.sampleRate
        bufferSize = Int(status.requestedBufferSize)
        delayCompensationMs = status.delayCompensationMs
        maxRenderDurationUs = status.realtimeMaxRenderDurationUs
        deviceName = withUnsafePointer(to: status.deviceName) {
            $0.withMemoryRebound(to: CChar.self, capacity: Int(NC_TEXT_LEN)) { String(cString: $0) }
        }

        updatePlayhead(engineSeconds: status.playbackSeconds)
        applyTrackMeters(status)
        listenRoom?.refresh()
    }

    private func updatePlayhead(engineSeconds: Double) {
        guard transportRunning else {
            playheadSeconds = engineSeconds
            transportWallClockBase = engineSeconds
            transportWallClockStart = CACurrentMediaTime()
            return
        }

        let elapsed = CACurrentMediaTime() - transportWallClockStart
        let predicted = transportWallClockBase + elapsed

        if abs(predicted - engineSeconds) > resyncThreshold {
            playheadSeconds = engineSeconds
            transportWallClockBase = engineSeconds
            transportWallClockStart = CACurrentMediaTime()
        } else {
            playheadSeconds = predicted
        }
    }

    private func readString(_ fill: (UnsafeMutablePointer<CChar>, Int) -> Void) -> String {
        var buffer = [CChar](repeating: 0, count: Int(NC_TEXT_LEN))
        fill(&buffer, buffer.count)
        return String(cString: buffer)
    }
}

/// Drops a leading "Speaker A: " / "Headphone C: " label from an engine model string.
func stripSlotPrefix(_ text: String) -> String {
    guard let colon = text.firstIndex(of: ":") else { return text }
    let head = text[text.startIndex..<colon]
    guard head.hasPrefix("Speaker ") || head.hasPrefix("Headphone ") else { return text }
    return String(text[text.index(after: colon)...]).trimmingCharacters(in: .whitespaces)
}

/// dBFS from a linear peak, floored so silence doesn't read -inf.
func peakToDb(_ peak: Float) -> Double {
    peak <= 0.00001 ? -60.0 : max(-60.0, 20.0 * log10(Double(peak)))
}

/// Maps dBFS onto 0...1 across the meter's -60…0 dB span.
func meterFraction(_ peak: Float) -> Double {
    min(1.0, max(0.0, (peakToDb(peak) + 60.0) / 60.0))
}
