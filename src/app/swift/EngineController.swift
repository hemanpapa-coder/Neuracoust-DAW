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
    /// The loop range doubles as the edit range, the way the old UI used it.
    @Published private(set) var loopStartSeconds: Double = 0
    @Published private(set) var loopEndSeconds: Double = 4
    @Published var clickEnabled = false
    @Published var snapEnabled = true
    @Published var recording = false

    /// The Pro-Tools-style record mode chosen from the record button's context menu.
    /// It is the configuration the recording engine will use — the engine does not yet
    /// capture input to disk, so choosing a mode stages it rather than arming a take.
    enum RecordMode: String, CaseIterable, Identifiable {
        case newTake, loop, punch
        var id: String { rawValue }
        var label: String {
            switch self {
            case .newTake: return "새 테이크"
            case .loop: return "루프 레코딩"
            case .punch: return "펀치 레코딩"
            }
        }
    }
    @Published var recordMode: RecordMode = .newTake

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
        /// What turns this track's MIDI notes into sound. Empty on every other kind.
        var instrumentName: String
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

    // MARK: Master inserts

    /// The master chain is not the Master track's inserts; the engine keeps it apart.
    /// The browser addresses it with this sentinel in place of a track id.
    static let masterInsertTargetId = -1

    @Published private(set) var masterInserts: [InsertSlot] = []

    private func reloadMasterInserts() {
        guard let handle else { return }
        masterInserts = (0..<Int(nc_master_insert_count(handle))).map { slot in
            let s = Int32(slot)
            return InsertSlot(id: slot,
                              name: readEngineString { nc_master_insert_name(handle, s, $0, $1) },
                              bypassed: nc_master_insert_bypassed(handle, s),
                              modeBadge: "")
        }
    }

    func removeMasterInsert(slot: Int) {
        guard let handle, nc_master_remove_insert(handle, Int32(slot)) else { return }
        reloadMasterInserts()
        refreshHistory()
    }

    func toggleMasterInsertBypass(slot: Int) {
        guard let handle, slot < masterInserts.count else { return }
        guard nc_master_set_insert_bypassed(handle, Int32(slot), !masterInserts[slot].bypassed) else { return }
        reloadMasterInserts()
        refreshHistory()
    }

    func moveMasterInsert(slot: Int, direction: Int) {
        guard let handle, nc_master_move_insert(handle, Int32(slot), Int32(direction)) >= 0 else { return }
        reloadMasterInserts()
        refreshHistory()
    }

    // MARK: Inserts

    /// An instrument dropped on an instrument track fills its instrument slot rather
    /// than an insert — an insert cannot turn MIDI notes into sound.
    func addInsert(_ pluginIndex: Int) {
        guard let handle, let trackId = pluginTargetTrack else { return }

        if trackId == Self.masterInsertTargetId {
            if nc_master_add_insert(handle, Int32(pluginIndex)) {
                reloadMasterInserts()
                refreshHistory()
            }
            return
        }
        guard let track = tracks.first(where: { $0.id == trackId }) else { return }

        let plugin = plugins.first { $0.id == pluginIndex }
        let loadsAsInstrument = track.kind == .instrument && plugin?.category == "Instrument"

        let changed = loadsAsInstrument
            ? nc_track_set_instrument(handle, Int32(trackId), Int32(pluginIndex))
            : nc_track_add_insert(handle, Int32(trackId), Int32(pluginIndex))
        if changed {
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

    /// Master and Monitor refuse to be renamed, as does a name already in use.
    @discardableResult
    func renameTrack(_ trackId: Int, to newName: String) -> Bool {
        guard let handle else { return false }
        let trimmed = newName.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty, nc_track_rename(handle, Int32(trackId), trimmed) else { return false }
        reloadTracks()
        reloadClips()
        refreshHistory()
        return true
    }

    // MARK: Plug-in editor

    struct InsertDescriptor {
        let trackName: String
        let name: String
        let pluginPath: String
        let format: String
        let classId: String
        let className: String
        /// Set only when the plug-in runs in the sandboxed bridge, so its editor can
        /// observe the real audio and light up its own meters.
        let observerShmName: String
        let observerMaxBlock: Int
        let observerSampleRate: Double
    }

    /// A slot index of -1 addresses the track's instrument rather than an insert.
    static let instrumentSlotIndex = -1

    /// Everything the out-of-process editor host needs to load the plug-in. nil for an empty slot.
    func insertDescriptor(trackId: Int, insertIndex: Int) -> InsertDescriptor? {
        guard let handle else { return nil }

        if trackId == Self.masterInsertTargetId {
            let s = Int32(insertIndex)
            let path = readEngineString { nc_master_insert_plugin_path(handle, s, $0, $1) }
            guard !path.isEmpty, insertIndex < masterInserts.count else { return nil }
            return InsertDescriptor(
                trackName: "Master",
                name: masterInserts[insertIndex].name,
                pluginPath: path,
                format: readEngineString { nc_master_insert_plugin_format(handle, s, $0, $1) },
                classId: readEngineString { nc_master_insert_class_id(handle, s, $0, $1) },
                className: readEngineString { nc_master_insert_class_name(handle, s, $0, $1) },
                observerShmName: "", observerMaxBlock: 0, observerSampleRate: 0)
        }

        guard let track = tracks.first(where: { $0.id == trackId }) else { return nil }

        if insertIndex == Self.instrumentSlotIndex {
            let i = Int32(trackId)
            let path = readEngineString { nc_track_instrument_plugin_path(handle, i, $0, $1) }
            guard !path.isEmpty else { return nil }
            // An instrument runs in the render plan, never in the sandbox bridge, so it
            // has no shared-memory observer to point its editor at.
            return InsertDescriptor(
                trackName: track.name,
                name: track.instrumentName,
                pluginPath: path,
                format: readEngineString { nc_track_instrument_plugin_format(handle, i, $0, $1) },
                classId: readEngineString { nc_track_instrument_class_id(handle, i, $0, $1) },
                className: readEngineString { nc_track_instrument_class_name(handle, i, $0, $1) },
                observerShmName: "", observerMaxBlock: 0, observerSampleRate: 0)
        }

        guard insertIndex >= 0, insertIndex < track.inserts.count,
              !track.inserts[insertIndex].isEmpty else { return nil }
        let i = Int32(trackId), s = Int32(insertIndex)
        let path = readEngineString { nc_track_insert_plugin_path(handle, i, s, $0, $1) }
        guard !path.isEmpty else { return nil }

        var maxBlock: Int32 = 0
        var sampleRate = 0.0
        var shmName = [CChar](repeating: 0, count: Int(NC_TEXT_LEN))
        let outOfProcess = nc_track_insert_observer(handle, i, s, &shmName, shmName.count,
                                                    &maxBlock, &sampleRate)
        let observer = (name: outOfProcess ? String(cString: shmName) : "",
                        maxBlock: Int(maxBlock),
                        sampleRate: sampleRate)
        return InsertDescriptor(
            trackName: track.name,
            name: track.inserts[insertIndex].name,
            pluginPath: path,
            format: readEngineString { nc_track_insert_plugin_format(handle, i, s, $0, $1) },
            classId: readEngineString { nc_track_insert_class_id(handle, i, s, $0, $1) },
            className: readEngineString { nc_track_insert_class_name(handle, i, s, $0, $1) },
            observerShmName: observer.name,
            observerMaxBlock: observer.maxBlock,
            observerSampleRate: observer.sampleRate)
    }

    func storedVst3Parameters(trackId: Int, insertIndex: Int) -> [(id: UInt32, value: Double)] {
        guard let handle else { return [] }
        if trackId == Self.masterInsertTargetId {
            let s = Int32(insertIndex)
            let count = Int(nc_master_insert_param_count(handle, s))
            return (0..<count).map { p in
                (id: nc_master_insert_param_id(handle, s, Int32(p)),
                 value: nc_master_insert_param_value(handle, s, Int32(p)))
            }
        }
        let i = Int32(trackId), s = Int32(insertIndex)
        if insertIndex == Self.instrumentSlotIndex {
            let count = Int(nc_track_instrument_param_count(handle, i))
            return (0..<count).map { p in
                (id: nc_track_instrument_param_id(handle, i, Int32(p)),
                 value: nc_track_instrument_param_value(handle, i, Int32(p)))
            }
        }
        let count = Int(nc_track_insert_param_count(handle, i, s))
        return (0..<count).map { p in
            (id: nc_track_insert_param_id(handle, i, s, Int32(p)),
             value: nc_track_insert_param_value(handle, i, s, Int32(p)))
        }
    }

    /// One knob turn is a stream of these, so it neither reloads tracks nor records undo.
    func setVst3Parameter(trackId: Int, insertIndex: Int, parameterId: UInt32, normalizedValue: Double) {
        guard let handle else { return }
        let changed: Bool
        if trackId == Self.masterInsertTargetId {
            changed = nc_master_set_vst3_parameter(handle, Int32(insertIndex), parameterId, nil, normalizedValue)
        } else if insertIndex == Self.instrumentSlotIndex {
            changed = nc_track_set_instrument_vst3_parameter(handle, Int32(trackId), parameterId, nil, normalizedValue)
        } else {
            changed = nc_track_set_vst3_parameter(handle, Int32(trackId), Int32(insertIndex),
                                                  parameterId, nil, normalizedValue)
        }
        if changed {
            projectDirty = nc_project_dirty(handle)
        }
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

    enum OutputMode { case speaker, headphone }

    /// The monitor listen state, mirroring the engine's `monitorStationListenMode`
    /// ("LR"/"L"/"R"/"M"/"S") plus mono and phase inverts. Not four exclusive modes:
    /// the buttons cycle, the way the old UI's monitor station did.
    struct MonitorListen: Equatable {
        var listenMode = "LR"
        var mono = false
        var midSide = false
        var invertLeft = false
        var invertRight = false

        /// What the Stereo button reads: Mid in M/S, Left/Right when soloed, else Stereo.
        var stereoTitle: String {
            if midSide { return "Mid" }
            if !mono && listenMode == "L" { return "Left" }
            if !mono && listenMode == "R" { return "Right" }
            return "Stereo"
        }
        var stereoActive: Bool { midSide ? listenMode == "M" : !mono }

        /// What the Mono button reads: Side in M/S, Left/Right when mono-soloed, else Mono.
        var monoTitle: String {
            if midSide { return "Side" }
            if mono && listenMode == "L" { return "Left" }
            if mono && listenMode == "R" { return "Right" }
            return "Mono"
        }
        var monoActive: Bool { midSide ? listenMode == "S" : mono }

        var phaseTitle: String {
            switch (invertLeft, invertRight) {
            case (true, true): return "ØLR"
            case (true, false): return "ØL"
            case (false, true): return "ØR"
            default: return "Ø"
            }
        }
        var phaseActive: Bool { invertLeft || invertRight }
    }

    @Published private(set) var monitorModules: [MonitorModule] = []
    @Published private(set) var speakerSets: [SpeakerSet] = []
    @Published private(set) var activeSpeakerSlot = 0
    @Published private(set) var monitorVolumeDb: Float = -6
    @Published private(set) var monitorListen = MonitorListen()
    @Published private(set) var monitorDim = false
    @Published private(set) var monitorMono = false
    @Published private(set) var monitorMute = false
    @Published private(set) var monitorTalkback = false
    @Published private(set) var monitorDspEnabled = true
    @Published private(set) var monitorPathMode = "internal"

    /// Output mode is a UI concept — the engine models speaker vs headphone as
    /// which simulation module is enabled.
    @Published var outputMode: OutputMode = .speaker

    struct OutputDevice: Identifiable, Hashable { let id: String; let name: String }
    @Published private(set) var outputDevices: [OutputDevice] = []
    @Published private(set) var currentOutputDeviceId = ""   // empty = system default
    @Published private(set) var activeOutputDeviceName = ""

    /// Rescans CoreAudio and refreshes the device list — called when a menu opens.
    func refreshOutputDevices() {
        guard let handle else { return }
        let count = Int(nc_output_device_count(handle))
        outputDevices = (0..<count).map { i in
            OutputDevice(id: readEngineString { nc_output_device_id(handle, Int32(i), $0, $1) },
                         name: readEngineString(capacity: 256) { nc_output_device_name(handle, Int32(i), $0, $1) })
        }
        currentOutputDeviceId = readEngineString { nc_current_output_device_id(handle, $0, $1) }
        activeOutputDeviceName = readEngineString(capacity: 256) { nc_active_output_device_name(handle, $0, $1) }
    }

    /// An empty id selects the system default. Changing the device restarts the engine.
    func setOutputDevice(_ id: String) {
        guard let handle else { return }
        nc_set_output_device(handle, id.isEmpty ? nil : id)
        currentOutputDeviceId = readEngineString { nc_current_output_device_id(handle, $0, $1) }
        activeOutputDeviceName = readEngineString(capacity: 256) { nc_active_output_device_name(handle, $0, $1) }
    }

    // Live meters, refreshed each tick.
    @Published private(set) var phaseCorrelation: Float = 0
    @Published private(set) var spectrumLow: Float = 0
    @Published private(set) var spectrumMid: Float = 0
    @Published private(set) var spectrumHigh: Float = 0
    @Published private(set) var wakeJitterUs: Double = 0
    @Published private(set) var remoteDspActive = false

    // DSP core allocation (a QoS hint the engine applies to its realtime thread).
    @Published private(set) var coreIsolationEnabled = true
    @Published private(set) var dspCoreCount = 4
    // Cores DW asks the external DSP Manager to reserve; a connected node's own report wins.
    @Published private(set) var externalDspCoreCount = 4
    // The remote DSP node address the engine streams to (External/NDS target).
    @Published var remoteDspHost = "studio.local"
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

    /// Lazy so it can hold an unowned reference back to a fully-formed controller.
    lazy var pluginEditors = PluginEditorHost(engine: self)

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
        loopStartSeconds = nc_project_loop_start(handle)
        loopEndSeconds = nc_project_loop_end(handle)
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
        static let e: UInt16 = 14
        static let b: UInt16 = 11
        static let c: UInt16 = 8
        static let x: UInt16 = 7
        static let v: UInt16 = 9
        static let d: UInt16 = 2
        static let m: UInt16 = 46
        static let t: UInt16 = 17
        static let space: UInt16 = 49
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
            case KeyCode.space:
                // The transport toggle, the way the spacebar works in every DAW. It has
                // to be consumed here (return nil) or it also clicks whatever button
                // holds focus.
                togglePlay()
                return nil
            case KeyCode.b where selectedRegionId != nil:
                splitRegionAtPlayhead(selectedRegionId!)
                return nil
            case KeyCode.b where !selectedClipIds.isEmpty:
                splitSelectedClipsAtPlayhead()
                return nil
            case KeyCode.delete, KeyCode.forwardDelete:
                if let regionId = selectedRegionId {
                    deleteMidiRegion(regionId)
                    return nil
                }
                guard !selectedClipIds.isEmpty else { return event }
                deleteSelectedClips()
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
        case KeyCode.e:
            bounceProject()
        case KeyCode.c where !selectedClipIds.isEmpty:
            copySelectedClips()
        case KeyCode.x where !selectedClipIds.isEmpty:
            cutSelectedClips()
        case KeyCode.v where clipboardClipName != nil:
            pasteClipsAtPlayhead()
        case KeyCode.d where selectedRegionId != nil:
            duplicateRegion(selectedRegionId!)
        case KeyCode.d where !selectedClipIds.isEmpty:
            duplicateSelectedClips()
        case KeyCode.m:
            addMarkerAtPlayhead()
        case KeyCode.t:
            // ⌘T adds an audio track, ⌘⇧T an instrument track — the way most DAWs do it.
            shift ? addInstrumentTrack() : addAudioTrack()
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

    /// This does **not** capture audio. `nc_engine_set_recording` switches the
    /// monitor path for record-armed tracks — the engine has no input capture and
    /// never writes a take to disk. Nothing here should suggest otherwise.
    func toggleRecording() {
        guard let handle else { return }
        recording.toggle()
        nc_engine_set_recording(handle, recording)
        lastError = recording
            ? "입력 모니터 경로만 전환합니다. 이 엔진은 아직 녹음을 디스크에 기록하지 않습니다."
            : nil
    }

    func toggleLoop() {
        setLoop(!loopEnabled)
    }

    func setLoop(_ enabled: Bool) {
        guard let handle, enabled != loopEnabled else { return }
        loopEnabled = enabled
        nc_project_set_loop_enabled(handle, loopEnabled)
    }

    /// Loop record needs the loop on; punch reads its range from the loop/edit range.
    /// Selecting loop record turns the loop on so the two agree.
    func setRecordMode(_ mode: RecordMode) {
        recordMode = mode
        if mode == .loop { setLoop(true) }
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
                instrumentName: {
                    let loaded = readEngineString { nc_track_instrument_name(handle, i, $0, $1) }
                    return loaded == "No Instrument" ? "" : loaded
                }(),
                sends: (0..<sendCount).map { slot in
                    readEngineString { nc_track_send_bus(handle, i, Int32(slot), $0, $1) }
                }
            )
        }

        reloadMasterInserts()
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
        reloadMarkers()
        reloadMidiRegions()
        reloadMonitorState()
        refreshHistory()
    }

    func redo() {
        guard let handle, nc_history_redo(handle) else { return }
        reloadTracks()
        reloadClips()
        reloadMarkers()
        reloadMidiRegions()
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
        importAudio(intoTrack: trackId, at: playheadSeconds, from: urls)
    }

    /// Dropping files places the first at `startSeconds`; the rest follow end-to-end.
    /// Track and time come from where the drop landed, not the playhead.
    func importAudio(intoTrack trackId: Int, at startSeconds: Double, from urls: [URL]) {
        guard let handle else { return }

        var start = max(0, startSeconds)
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
        loopStartSeconds = nc_project_loop_start(handle)
        loopEndSeconds = nc_project_loop_end(handle)
        reloadTracks()
        reloadClips()
        reloadMarkers()
        reloadMidiRegions()
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
        /// Where the clip's audio begins inside its source file.
        let sourceOffsetSeconds: Double
        let fadeInSeconds: Double
        let fadeOutSeconds: Double
        let gainDb: Float
    }

    @Published private(set) var clips: [Clip] = []

    /// Peak envelopes keyed by source path, fetched once per file from the engine.
    @Published private(set) var waveforms: [String: (mins: [Float], maxs: [Float], durationSeconds: Double)] = [:]

    /// Timeline selection. Purely a view concept; the engine has no notion of it.
    @Published var selectedClipIds: Set<String> = []
    @Published var selectedTrackId: Int?

    /// Fades and clip gain edit one clip at a time; they hide on a multi-selection.
    var selectedClipId: String? { selectedClipIds.count == 1 ? selectedClipIds.first : nil }

    /// Also clears any region selection — clicking a clip, or empty space, means
    /// the Delete key no longer points at a region.
    func selectClip(_ clipId: String?) {
        selectedClipIds = clipId.map { [$0] } ?? []
        selectedRegionId = nil
    }

    /// Shift-click: add a clip to the selection, or take it back out.
    func toggleClipSelection(_ clipId: String) {
        if selectedClipIds.contains(clipId) {
            selectedClipIds.remove(clipId)
        } else {
            selectedClipIds.insert(clipId)
        }
    }

    func selectClips(_ clipIds: [String]) {
        selectedClipIds = Set(clipIds)
    }

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
    var laneTracks: [Track] { tracks.filter { !$0.kind.isMasterish } }

    var timelineModel: TimelineModel {
        let lanes = laneTracks
        let laneIndex = Dictionary(uniqueKeysWithValues: lanes.enumerated().map { ($1.name, $0) })

        return TimelineModel(
            lanes: lanes.map { track in
                var automation: TimelineModel.Automation?
                if let parameter = automationLanes[track.id] {
                    automation = TimelineModel.Automation(
                        parameterId: parameter.rawValue,
                        displayName: parameter.displayName,
                        range: parameter.range,
                        fallback: parameter == .volume ? track.volumeDb : track.pan,
                        points: automationPoints(trackId: track.id, parameter))
                }
                return TimelineModel.Lane(name: track.name,
                                          accent: NSColor.from(track.kind.accent),
                                          muted: track.muted,
                                          selected: track.id == selectedTrackId,
                                          automation: automation)
            },
            clips: clips.compactMap { clip in
                guard let lane = laneIndex[clip.trackName] else { return nil }
                return TimelineModel.Clip(id: clip.id,
                                          name: clip.name,
                                          laneIndex: lane,
                                          startSeconds: clip.startSeconds,
                                          durationSeconds: clip.durationSeconds,
                                          sourcePath: clip.sourcePath,
                                          sourceOffsetSeconds: clip.sourceOffsetSeconds,
                                          selected: selectedClipIds.contains(clip.id),
                                          fadeInSeconds: clip.fadeInSeconds,
                                          fadeOutSeconds: clip.fadeOutSeconds,
                                          gainDb: clip.gainDb)
            },
            tempoBpm: tempoBpm,
            beatsPerBar: timeSignature.numerator,
            visibleStart: visibleStart,
            visibleDuration: visibleDuration,
            markers: markers.map { TimelineModel.Marker(name: $0.name, timeSeconds: $0.timeSeconds) },
            midiRegions: midiRegions.compactMap { region in
                guard let lane = laneIndex[region.trackName] else { return nil }
                // Notes are in beats from the region start; the sketch wants seconds.
                let secondsPerBeat = 60.0 / Double(tempoBpm)
                return TimelineModel.MidiRegion(
                    id: region.id,
                    name: region.name,
                    laneIndex: lane,
                    startSeconds: region.startSeconds,
                    durationSeconds: region.durationSeconds,
                    muted: region.muted,
                    editing: region.id == editingRegionId,
                    selected: region.id == selectedRegionId,
                    noteSketch: notes(inRegion: region.id).map { note in
                        TimelineModel.MidiRegion.Sketch(
                            startSeconds: region.startSeconds + note.startBeats * secondsPerBeat,
                            durationSeconds: note.durationBeats * secondsPerBeat,
                            pitch: note.pitch)
                    })
            },
            rangeStart: loopStartSeconds,
            rangeEnd: loopEndSeconds,
            loopEnabled: loopEnabled
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

    /// Option-drag copy: duplicate the clip, drop the copy onto the original's start so
    /// the drag begins in place, and hand back the new id for the drag to move. The
    /// original is left untouched. Returns nil if duplication failed.
    func duplicateClipForDrag(_ clipId: String, at startSeconds: Double) -> String? {
        guard let handle else { return nil }
        var buffer = [CChar](repeating: 0, count: 128)
        guard nc_clip_duplicate(handle, clipId, &buffer, buffer.count) else { return nil }
        let newId = String(cString: buffer)
        _ = nc_clip_move(handle, newId, startSeconds)
        selectClip(newId)
        reloadClips()
        refreshHistory()
        return newId
    }

    func trimClipStart(_ clipId: String, to startSeconds: Double) {
        guard let handle else { return }
        if nc_clip_trim_start(handle, clipId, startSeconds) { reloadClips() }
    }

    func trimClipEnd(_ clipId: String, to endSeconds: Double) {
        guard let handle else { return }
        if nc_clip_trim_end(handle, clipId, endSeconds) { reloadClips() }
    }

    func setClipFades(_ clipId: String, fadeIn: Double, fadeOut: Double) {
        guard let handle else { return }
        if nc_clip_set_fades(handle, clipId, fadeIn, fadeOut) { reloadClips() }
    }

    func setClipGain(_ clipId: String, _ gainDb: Float) {
        guard let handle else { return }
        if nc_clip_set_gain_db(handle, clipId, gainDb) { reloadClips() }
    }

    /// Lane indices address `laneTracks`, not `tracks` — Master and Monitor are not lanes.
    func selectLane(_ laneIndex: Int) {
        guard laneIndex < laneTracks.count else { return }
        selectedTrackId = laneTracks[laneIndex].id
    }

    /// The engine track id for a timeline lane, or nil past the last lane.
    func trackId(forLane laneIndex: Int) -> Int? {
        laneIndex >= 0 && laneIndex < laneTracks.count ? laneTracks[laneIndex].id : nil
    }

    /// Files dropped onto a lane at a point in time. A drop past the last lane (onto
    /// empty space) makes a new audio track and lands the clip there.
    func dropAudio(onLane laneIndex: Int, atSeconds seconds: Double, urls: [URL]) {
        let audio = urls.filter { nc_audio_import_supported($0.path) }
        guard !audio.isEmpty else {
            lastError = "가져올 수 있는 오디오 파일이 없습니다."
            return
        }
        var targetLane = laneIndex
        if trackId(forLane: targetLane) == nil {
            guard let handle, nc_track_add_audio(handle) >= 0 else { return }
            reloadTracks()
            targetLane = laneTracks.count - 1   // the freshly-added track is the last lane
        }
        guard let trackId = trackId(forLane: targetLane) else { return }
        importAudio(intoTrack: trackId, at: snap(seconds), from: audio)
    }

    func moveClipToLane(_ clipId: String, laneIndex: Int, startSeconds: Double) {
        guard let handle, laneIndex < laneTracks.count else { return }
        let trackId = laneTracks[laneIndex].id

        var buffer = [CChar](repeating: 0, count: 128)
        guard nc_clip_move_to_track(handle, clipId, Int32(trackId), startSeconds,
                                    &buffer, buffer.count) else { return }
        selectClip(String(cString: buffer))
        reloadClips()
        refreshHistory()
    }

    // MARK: Bounce

    struct BounceSummary {
        let path: String
        let durationSeconds: Double
        let peakDbfs: Double
        let clipped: Bool
        let nearSilent: Bool
    }

    @Published private(set) var bouncing = false
    @Published private(set) var bounceSummary: BounceSummary?

    /// Renders off the main thread from a serialized snapshot, so editing stays
    /// live and nothing races the engine. Roughly 25x realtime without plug-ins.
    func bounceProject() {
        guard let handle, !bouncing else { return }
        guard !clips.isEmpty else {
            lastError = "내보낼 클립이 없습니다"
            return
        }

        let panel = NSSavePanel()
        panel.allowedContentTypes = [.init(filenameExtension: "wav")].compactMap { $0 }
        panel.nameFieldStringValue = (projectName.isEmpty ? "Bounce" : projectName) + ".wav"
        guard panel.runModal() == .OK, let url = panel.url else { return }

        let needed = Int(nc_project_serialize(handle, nil, 0))
        guard needed > 0 else {
            lastError = "프로젝트를 직렬화할 수 없습니다"
            return
        }
        var buffer = [CChar](repeating: 0, count: needed + 1)
        _ = nc_project_serialize(handle, &buffer, buffer.count)
        let snapshot = String(cString: buffer)

        bouncing = true
        bounceSummary = nil
        let outputPath = url.path

        Task.detached(priority: .userInitiated) {
            var result = NCBounceResult()
            let ok = nc_bounce_snapshot_to_wav(snapshot, outputPath, &result)

            let message = withUnsafePointer(to: result.message) {
                $0.withMemoryRebound(to: CChar.self, capacity: Int(NC_TEXT_LEN)) { String(cString: $0) }
            }
            let peak = max(result.peakLeft, result.peakRight)

            await MainActor.run { [weak self] in
                guard let self else { return }
                self.bouncing = false
                guard ok else {
                    self.lastError = message.isEmpty ? "바운스에 실패했습니다" : message
                    return
                }
                self.bounceSummary = BounceSummary(
                    path: outputPath,
                    durationSeconds: result.durationSeconds,
                    peakDbfs: peakToDb(peak),
                    clipped: result.clippingDetected,
                    nearSilent: result.nearSilent
                )
            }
        }
    }

    func dismissBounceSummary() {
        bounceSummary = nil
    }

    // MARK: Tracks

    func addAudioTrack() {
        guard let handle, nc_track_add_audio(handle) >= 0 else { return }
        reloadTracks()
        refreshHistory()
    }

    func addInstrumentTrack() {
        guard let handle, nc_track_add_instrument(handle) >= 0 else { return }
        reloadTracks()
        refreshHistory()
    }

    /// Deleting takes the clips with it, so ask first when the track has any.
    func deleteSelectedTrack() {
        guard let handle, let trackId = selectedTrackId,
              let track = tracks.first(where: { $0.id == trackId }) else { return }

        let clipsOnTrack = clips.filter { $0.trackName == track.name }
        if !clipsOnTrack.isEmpty {
            let alert = NSAlert()
            alert.messageText = "\(track.name) 트랙을 삭제할까요?"
            alert.informativeText = "이 트랙의 클립 \(clipsOnTrack.count)개도 함께 삭제됩니다."
            alert.addButton(withTitle: "삭제")
            alert.addButton(withTitle: "취소")
            guard alert.runModal() == .alertFirstButtonReturn else { return }
        }

        guard nc_track_delete(handle, Int32(trackId), true) else {
            lastError = "이 트랙은 삭제할 수 없습니다"
            return
        }
        selectedTrackId = nil
        selectedClipIds = []
        reloadTracks()
        reloadClips()
        refreshHistory()
    }

    /// Master and Monitor refuse deletion in the engine; do not offer it.
    var canDeleteSelectedTrack: Bool {
        guard let trackId = selectedTrackId,
              let track = tracks.first(where: { $0.id == trackId }) else { return false }
        return !track.kind.isMasterish
    }

    func commitClipGesture(_ stepName: String) {
        recordGesture(stepName)
    }

    // MARK: Clipboard

    @Published private(set) var clipboardClipName: String?
    @Published private(set) var clipboardClipCount = 0

    /// Bridges a Swift string array to `const char* const*` for the batch edits.
    private func withClipIds<R>(_ ids: [String], _ body: (UnsafeMutablePointer<UnsafePointer<CChar>?>, Int32) -> R) -> R? {
        guard !ids.isEmpty else { return nil }
        let copies = ids.map { strdup($0) }
        defer { copies.forEach { free($0) } }
        var pointers: [UnsafePointer<CChar>?] = copies.map { UnsafePointer($0) }
        return pointers.withUnsafeMutableBufferPointer { body($0.baseAddress!, Int32(ids.count)) }
    }

    /// Ids the last batch edit created — the clips the user should now be holding.
    private func selectBatchResult() {
        guard let handle else { return }
        let count = Int(nc_result_count(handle))
        selectedClipIds = Set((0..<count).map { index in
            readEngineString { nc_result_id(handle, Int32(index), $0, $1) }
        })
    }

    /// Ordered so a batch edit sees the same clips the user sees selected.
    private var selection: [String] { clips.map(\.id).filter { selectedClipIds.contains($0) } }

    func copySelectedClips() {
        guard let handle else { return }
        guard withClipIds(selection, { nc_clip_copy_many(handle, $0, $1) }) == true else { return }
        refreshClipboard()
    }

    func cutSelectedClips() {
        guard let handle else { return }
        guard let cut = withClipIds(selection, { nc_clip_cut_many(handle, $0, $1) }), cut > 0 else { return }
        selectedClipIds = []
        reloadClips()
        refreshClipboard()
        refreshHistory()
    }

    /// Pastes at the playhead: the earliest clip lands there, the rest keep their offsets.
    func pasteClipsAtPlayhead() {
        guard let handle, nc_clip_paste_all(handle, playheadSeconds) > 0 else { return }
        selectBatchResult()
        reloadClips()
        refreshHistory()
    }

    func duplicateSelectedClips() {
        guard let handle else { return }
        guard let made = withClipIds(selection, { nc_clip_duplicate_many(handle, $0, $1) }), made > 0 else { return }
        selectBatchResult()
        reloadClips()
        refreshHistory()
    }

    private func refreshClipboard() {
        guard let handle else { return }
        clipboardClipCount = Int(nc_clipboard_clip_count(handle))
        clipboardClipName = nc_clipboard_has_clip(handle)
            ? readEngineString { nc_clipboard_clip_name(handle, $0, $1) }
            : nil
    }

    /// Clips the playhead misses are left alone.
    func splitSelectedClipsAtPlayhead() {
        guard let handle else { return }
        let playhead = playheadSeconds
        guard let split = withClipIds(selection, { nc_clip_split_many(handle, $0, $1, playhead) }),
              split > 0 else { return }
        reloadClips()
        refreshHistory()
    }

    func deleteSelectedClips() {
        guard let handle else { return }
        guard let deleted = withClipIds(selection, { nc_clip_delete_many(handle, $0, $1) }), deleted > 0 else { return }
        selectedClipIds = []
        reloadClips()
        refreshHistory()
    }

    // MARK: MIDI

    struct MidiRegion: Equatable {
        let id: String
        let name: String
        let trackName: String
        let startSeconds: Double
        let durationSeconds: Double
        let muted: Bool
    }

    struct MidiNote: Equatable {
        let id: String
        let pitch: Int
        let startBeats: Double
        let durationBeats: Double
        let velocity: Int
    }

    @Published private(set) var midiRegions: [MidiRegion] = []
    /// The region open in the piano roll, or nil while it is closed.
    @Published var editingRegionId: String?
    /// Selecting a region is exclusive with selecting clips: one Delete key, one target.
    @Published private(set) var selectedRegionId: String?

    func selectRegion(_ regionId: String?) {
        selectedRegionId = regionId
        if regionId != nil { selectedClipIds = [] }
    }

    var editingRegion: MidiRegion? {
        midiRegions.first { $0.id == editingRegionId }
    }

    private func reloadMidiRegions() {
        guard let handle else { return }
        midiRegions = (0..<Int(nc_midi_region_count(handle))).map { index in
            let i = Int32(index)
            return MidiRegion(id: readEngineString { nc_midi_region_id(handle, i, $0, $1) },
                              name: readEngineString { nc_midi_region_name(handle, i, $0, $1) },
                              trackName: readEngineString { nc_midi_region_track(handle, i, $0, $1) },
                              startSeconds: nc_midi_region_start_seconds(handle, i),
                              durationSeconds: nc_midi_region_duration_seconds(handle, i),
                              muted: nc_midi_region_muted(handle, i))
        }
        if let editing = editingRegionId, !midiRegions.contains(where: { $0.id == editing }) {
            editingRegionId = nil
        }
        if let selected = selectedRegionId, !midiRegions.contains(where: { $0.id == selected }) {
            selectedRegionId = nil
        }
    }

    func notes(inRegion regionId: String) -> [MidiNote] {
        guard let handle else { return [] }
        let count = Int(nc_midi_note_count(handle, regionId))
        return (0..<count).map { index in
            let i = Int32(index)
            return MidiNote(id: readEngineString { nc_midi_note_id(handle, regionId, i, $0, $1) },
                            pitch: Int(nc_midi_note_pitch(handle, regionId, i)),
                            startBeats: nc_midi_note_start_beats(handle, regionId, i),
                            durationBeats: nc_midi_note_duration_beats(handle, regionId, i),
                            velocity: Int(nc_midi_note_velocity(handle, regionId, i)))
        }
    }

    /// Only instrument and midi tracks can hold a region.
    func canHoldMidi(_ track: Track) -> Bool { track.kind == .instrument || track.kind == .midi }

    func addMidiRegion(laneIndex: Int, startSeconds: Double, durationSeconds: Double = 4) {
        guard let handle, laneIndex < laneTracks.count,
              canHoldMidi(laneTracks[laneIndex]) else { return }
        var buffer = [CChar](repeating: 0, count: 128)
        guard nc_midi_region_add(handle, Int32(laneTracks[laneIndex].id), startSeconds,
                                 durationSeconds, &buffer, buffer.count) else { return }
        reloadMidiRegions()
        refreshHistory()
    }

    /// Continuous; the view commits the gesture when the drag ends.
    func moveMidiRegion(_ regionId: String, laneIndex: Int?, startSeconds: Double) {
        guard let handle else { return }
        let trackIndex = laneIndex.map { $0 < laneTracks.count ? Int32(laneTracks[$0].id) : -1 } ?? -1
        guard nc_midi_region_move(handle, regionId, trackIndex, startSeconds) else { return }
        reloadMidiRegions()
    }

    func resizeMidiRegion(_ regionId: String, durationSeconds: Double) {
        guard let handle, nc_midi_region_resize(handle, regionId, durationSeconds) else { return }
        reloadMidiRegions()
    }

    func deleteMidiRegion(_ regionId: String) {
        guard let handle, nc_midi_region_delete(handle, regionId) else { return }
        if selectedRegionId == regionId { selectedRegionId = nil }
        reloadMidiRegions()
        refreshHistory()
    }

    // MARK: Piano roll

    func addNote(pitch: Int, startBeats: Double, durationBeats: Double, velocity: Int = 96) {
        guard let handle, let regionId = editingRegionId else { return }
        // A click that lands a pixel short of an existing note must not stack a second
        // one on top of it — silently doubled notes are hard to see and easy to hear.
        let occupied = notes(inRegion: regionId).contains {
            $0.pitch == pitch && startBeats < $0.startBeats + $0.durationBeats
                && $0.startBeats < startBeats + durationBeats
        }
        guard !occupied else { return }
        var buffer = [CChar](repeating: 0, count: 128)
        guard nc_midi_note_add(handle, regionId, Int32(pitch), startBeats, durationBeats,
                               Int32(velocity), &buffer, buffer.count) else { return }
        reloadMidiRegions()
        refreshHistory()
    }

    /// Continuous.
    func moveNote(_ noteId: String, pitch: Int, startBeats: Double) {
        guard let handle, let regionId = editingRegionId,
              nc_midi_note_move(handle, regionId, noteId, Int32(pitch), startBeats) else { return }
        objectWillChange.send()
    }

    /// Continuous.
    func resizeNote(_ noteId: String, durationBeats: Double) {
        guard let handle, let regionId = editingRegionId,
              nc_midi_note_resize(handle, regionId, noteId, durationBeats) else { return }
        objectWillChange.send()
    }

    /// Continuous: dragging the velocity lane streams these.
    func setNoteVelocity(_ noteId: String, _ velocity: Int) {
        guard let handle, let regionId = editingRegionId else { return }
        let clamped = max(1, min(127, velocity))
        guard nc_midi_note_set_velocity(handle, regionId, noteId, Int32(clamped)) else { return }
        objectWillChange.send()
    }

    func deleteNote(_ noteId: String) {
        guard let handle, let regionId = editingRegionId,
              nc_midi_note_delete(handle, regionId, noteId) else { return }
        refreshHistory()
        objectWillChange.send()
    }

    // MARK: MIDI region tools

    /// Beats. A sixteenth is 0.25.
    func quantizeRegion(_ regionId: String, beatQuantum: Double) {
        guard let handle, nc_midi_region_quantize(handle, regionId, beatQuantum) > 0 else { return }
        reloadMidiRegions()
        refreshHistory()
    }

    func transposeRegion(_ regionId: String, semitones: Int) {
        guard let handle, nc_midi_region_transpose(handle, regionId, Int32(semitones)) > 0 else { return }
        reloadMidiRegions()
        refreshHistory()
    }

    /// The seed makes it repeatable, which is the only way to test it.
    func humanizeRegion(_ regionId: String, seed: UInt32 = 12345) {
        guard let handle, nc_midi_region_humanize(handle, regionId, 0.03, 12, seed) > 0 else { return }
        reloadMidiRegions()
        refreshHistory()
    }

    func duplicateRegion(_ regionId: String) {
        guard let handle else { return }
        var buffer = [CChar](repeating: 0, count: 128)
        guard nc_midi_region_duplicate(handle, regionId, &buffer, buffer.count) else { return }
        reloadMidiRegions()
        refreshHistory()
    }

    /// Splits at the playhead. A playhead outside the region does nothing.
    func splitRegionAtPlayhead(_ regionId: String) {
        guard let handle else { return }
        var buffer = [CChar](repeating: 0, count: 128)
        guard nc_midi_region_split(handle, regionId, playheadSeconds, &buffer, buffer.count) else { return }
        reloadMidiRegions()
        refreshHistory()
    }

    // MARK: Markers

    struct Marker: Equatable {
        let name: String
        let timeSeconds: Double
    }

    @Published private(set) var markers: [Marker] = []

    /// How near a click has to land. The engine addresses markers by time, so this
    /// is the grab radius, in seconds at the current zoom.
    private var markerTolerance: Double { visibleDuration * 0.006 }

    private func reloadMarkers() {
        guard let handle else { return }
        markers = (0..<Int(nc_marker_count(handle))).map { index in
            Marker(name: readEngineString { nc_marker_name(handle, Int32(index), $0, $1) },
                   timeSeconds: nc_marker_time(handle, Int32(index)))
        }
    }

    func addMarkerAtPlayhead() {
        guard let handle else { return }
        var buffer = [CChar](repeating: 0, count: 128)
        guard nc_marker_add(handle, playheadSeconds, &buffer, buffer.count) else { return }
        reloadMarkers()
        refreshHistory()
    }

    func renameMarker(at timeSeconds: Double, to name: String) {
        guard let handle else { return }
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty,
              nc_marker_rename(handle, timeSeconds, markerTolerance, trimmed) else { return }
        reloadMarkers()
        refreshHistory()
    }

    /// Continuous. `fromSeconds` is where the marker sits now, not where the drag began.
    func moveMarker(from fromSeconds: Double, to toSeconds: Double) {
        guard let handle, nc_marker_move(handle, fromSeconds, markerTolerance, toSeconds) else { return }
        reloadMarkers()
    }

    func deleteMarker(at timeSeconds: Double) {
        guard let handle, nc_marker_delete(handle, timeSeconds, markerTolerance) else { return }
        reloadMarkers()
        refreshHistory()
    }

    /// Sets the edit range to the stretch between the markers around `seconds`.
    func selectBetweenMarkers(around seconds: Double) {
        guard let handle else { return }
        var start = 0.0
        var end = 0.0
        guard nc_marker_surrounding_range(handle, seconds, &start, &end) else { return }
        setLoopRange(start: start, end: end)
    }

    // MARK: Automation

    /// Only what the renderer actually reads. Storing anything else would draw a
    /// curve that does nothing to the sound.
    enum AutomationParameter: String, CaseIterable {
        case volume = "track.volume"
        case pan = "track.pan"

        var displayName: String { self == .volume ? "볼륨 (dB)" : "팬" }
        var range: ClosedRange<Float> { self == .volume ? -60...12 : -1...1 }
    }

    /// Which lanes have their automation folded out, and on which parameter.
    @Published private(set) var automationLanes: [Int: AutomationParameter] = [:]

    func toggleAutomation(laneIndex: Int) {
        guard laneIndex < laneTracks.count else { return }
        let trackId = laneTracks[laneIndex].id
        if automationLanes[trackId] == nil {
            automationLanes[trackId] = .volume
        } else {
            automationLanes[trackId] = nil
        }
    }

    func cycleAutomationParameter(laneIndex: Int) {
        guard laneIndex < laneTracks.count,
              let current = automationLanes[laneTracks[laneIndex].id] else { return }
        let all = AutomationParameter.allCases
        let next = all[(all.firstIndex(of: current)! + 1) % all.count]
        automationLanes[laneTracks[laneIndex].id] = next
    }

    private func automationPoints(trackId: Int, _ parameter: AutomationParameter)
        -> [TimelineModel.Automation.Point] {
        guard let handle else { return [] }
        let count = Int(nc_track_automation_count(handle, Int32(trackId), parameter.rawValue))
        return (0..<count).map { index in
            TimelineModel.Automation.Point(
                timeSeconds: nc_track_automation_time(handle, Int32(trackId), parameter.rawValue, Int32(index)),
                value: nc_track_automation_value(handle, Int32(trackId), parameter.rawValue, Int32(index)))
        }
    }

    func addAutomationPoint(laneIndex: Int, timeSeconds: Double, value: Float) {
        guard let handle, laneIndex < laneTracks.count else { return }
        let trackId = laneTracks[laneIndex].id
        guard let parameter = automationLanes[trackId],
              nc_track_automation_add(handle, Int32(trackId), parameter.rawValue, timeSeconds, value)
        else { return }
        reloadTracks()
        refreshHistory()
    }

    /// Continuous; the view commits the gesture when the drag ends.
    func moveAutomationPoint(laneIndex: Int, pointIndex: Int, timeSeconds: Double, value: Float) {
        guard let handle, laneIndex < laneTracks.count else { return }
        let trackId = laneTracks[laneIndex].id
        guard let parameter = automationLanes[trackId],
              nc_track_automation_move(handle, Int32(trackId), parameter.rawValue,
                                       Int32(pointIndex), timeSeconds, value)
        else { return }
        reloadTracks()
    }

    func deleteAutomationPoint(laneIndex: Int, pointIndex: Int) {
        guard let handle, laneIndex < laneTracks.count else { return }
        let trackId = laneTracks[laneIndex].id
        guard let parameter = automationLanes[trackId],
              nc_track_automation_delete(handle, Int32(trackId), parameter.rawValue, Int32(pointIndex))
        else { return }
        reloadTracks()
        refreshHistory()
    }

    // MARK: Range editing

    func setLoopRange(start: Double, end: Double) {
        guard let handle, nc_project_set_loop_range(handle, start, end) else { return }
        loopStartSeconds = nc_project_loop_start(handle)
        loopEndSeconds = nc_project_loop_end(handle)
    }

    /// True when the range covers any time at all — every range edit needs that.
    var hasEditRange: Bool { loopEndSeconds > loopStartSeconds }

    func copyRange() {
        guard let handle, nc_range_copy(handle, loopStartSeconds, loopEndSeconds) > 0 else { return }
        refreshClipboard()
    }

    func cutRange() {
        guard let handle, nc_range_cut(handle, loopStartSeconds, loopEndSeconds) > 0 else { return }
        selectedClipIds = []
        reloadClips()
        refreshClipboard()
        refreshHistory()
    }

    func clearRange() {
        guard let handle, nc_range_clear(handle, loopStartSeconds, loopEndSeconds) else { return }
        selectedClipIds = []
        reloadClips()
        refreshHistory()
    }

    /// Leaves the range standing as clips of its own, ready to be dragged.
    func separateRange() {
        guard let handle, nc_range_separate(handle, loopStartSeconds, loopEndSeconds) > 0 else { return }
        reloadClips()
        refreshHistory()
    }

    func duplicateRange() {
        guard let handle, nc_range_duplicate(handle, loopStartSeconds, loopEndSeconds) > 0 else { return }
        selectBatchResult()
        reloadClips()
        refreshHistory()
    }

    /// Continuous, for dragging a whole selection. Records nothing; the view calls
    /// `commitClipGesture` when the drag ends.
    func moveSelection(by deltaSeconds: Double) {
        guard let handle, deltaSeconds != 0 else { return }
        guard let moved = withClipIds(selection, { nc_clip_move_many(handle, $0, $1, deltaSeconds) }),
              moved > 0 else { return }
        reloadClips()
    }

    private func loadWaveforms() {
        guard let handle else { return }
        for clip in clips where waveforms[clip.sourcePath] == nil {
            // The peak count scales with the file's length, so read however many it has.
            let count = Int(nc_waveform_peak_count(handle, clip.sourcePath))
            guard count > 0 else { continue }
            var mins = [Float](repeating: 0, count: count)
            var maxs = [Float](repeating: 0, count: count)
            if nc_waveform_peaks(handle, clip.sourcePath, &mins, &maxs, Int32(count)) {
                waveforms[clip.sourcePath] = (mins, maxs,
                                              nc_waveform_duration_seconds(handle, clip.sourcePath))
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
                durationSeconds: nc_clip_duration_seconds(handle, i),
                sourceOffsetSeconds: nc_clip_source_offset_seconds(handle, i),
                fadeInSeconds: nc_clip_fade_in(handle, i),
                fadeOutSeconds: nc_clip_fade_out(handle, i),
                gainDb: nc_clip_gain_db(handle, i)
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

    /// True while any track is soloed, so the others can show they are being held down.
    var anyTrackSoloed: Bool { tracks.contains { $0.solo } }

    /// A slow on/off phase for the solo-implied blink on silenced tracks, toggled from
    /// the poll tick so every strip pulses in sync without its own animation timer.
    @Published private(set) var soloBlinkOn = false

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
        monitorMute = nc_monitor_mute(handle)
        monitorTalkback = nc_monitor_talkback(handle)
        monitorDspEnabled = nc_monitor_dsp_enabled(handle)
        monitorPathMode = readString { nc_monitor_path_mode(handle, $0, $1) }
        // Only re-derive the selection when the engine mode no longer matches it — so a
        // user's {internal, nds} choice (both mapping to "auto") is not flattened.
        if modeFromDspSources(dspSources) != monitorPathMode {
            dspSources = dspSourcesFromMode(monitorPathMode)
        }
        coreIsolationEnabled = nc_dsp_core_isolation(handle)
        dspCoreCount = Int(nc_dsp_core_count(handle))
        externalDspCoreCount = Int(nc_dsp_external_core_count(handle))
        remoteDspHost = readString { nc_dsp_remote_host(handle, $0, $1) }
        reloadMonitorListen()
    }

    private func reloadMonitorListen() {
        guard let handle else { return }
        monitorListen = MonitorListen(
            listenMode: readString { nc_monitor_listen_mode(handle, $0, $1) },
            mono: nc_monitor_mono(handle),
            midSide: nc_monitor_mid_side(handle),
            invertLeft: nc_monitor_invert_left(handle),
            invertRight: nc_monitor_invert_right(handle))
        monitorMono = monitorListen.mono
    }

    var activeSpeakerSet: SpeakerSet? {
        speakerSets.first { $0.id == activeSpeakerSlot }
    }

    func setMonitorVolume(_ db: Float) {
        guard let handle else { return }
        nc_monitor_set_volume_db(handle, db)
        monitorVolumeDb = nc_monitor_volume_db(handle)
    }

    /// Changing the core allocation restarts the audio engine to apply the QoS hint.
    func setCoreIsolation(_ enabled: Bool) {
        guard let handle else { return }
        nc_dsp_set_core_isolation(handle, enabled)
        coreIsolationEnabled = nc_dsp_core_isolation(handle)
        dspCoreCount = Int(nc_dsp_core_count(handle))
        refreshHistory()
    }

    func setDspCoreCount(_ count: Int) {
        guard let handle else { return }
        nc_dsp_set_core_count(handle, Int32(count))
        dspCoreCount = Int(nc_dsp_core_count(handle))
        refreshHistory()
    }

    /// The external DSP Manager reserve applies live through the monitor path — no restart.
    func setExternalDspCoreCount(_ count: Int) {
        guard let handle else { return }
        nc_dsp_set_external_core_count(handle, Int32(count))
        externalDspCoreCount = Int(nc_dsp_external_core_count(handle))
        refreshHistory()
    }

    /// Point the engine's remote DSP stream at a node address. Applies live.
    func setRemoteDspHost(_ host: String) {
        guard let handle else { return }
        nc_dsp_set_remote_host(handle, host)
        remoteDspHost = readString { nc_dsp_remote_host(handle, $0, $1) }
        refreshHistory()
    }

    /// Broadcast-probe the LAN for a node and adopt its address if one answers.
    func discoverRemoteDspHost() {
        guard let handle else { return }
        let found = readString { nc_dsp_discover_remote_host(handle, $0, $1) }
        if !found.isEmpty { setRemoteDspHost(found) }
    }

    /// Isolation keeps a floor of 4 cores, matching the engine.
    var minDspCoreCount: Int { coreIsolationEnabled ? 4 : 1 }

    func cycleStereo() { guard let handle else { return }; nc_monitor_cycle_stereo(handle); reloadMonitorListen() }
    func cycleMono() { guard let handle else { return }; nc_monitor_cycle_mono(handle); reloadMonitorListen() }
    func toggleMidSide() { guard let handle else { return }; nc_monitor_toggle_mid_side(handle); reloadMonitorListen() }
    func cyclePhase() { guard let handle else { return }; nc_monitor_cycle_phase(handle); reloadMonitorListen() }

    func toggleDim() {
        guard let handle else { return }
        nc_monitor_set_dim(handle, !monitorDim)
        monitorDim = nc_monitor_dim(handle)
    }

    /// Mute: the old UI's monitor station mute, distinct from the mono listen button.
    func toggleMonitorMute() {
        guard let handle else { return }
        nc_monitor_set_mute(handle, !monitorMute)
        monitorMute = nc_monitor_mute(handle)
    }

    func toggleTalkback() {
        guard let handle else { return }
        nc_monitor_set_talkback(handle, !monitorTalkback)
        monitorTalkback = nc_monitor_talkback(handle)
    }

    /// Talkback is momentary and pulls Dim in with it, like a console talkback key.
    /// Engaging remembers Dim's prior state so releasing restores it rather than
    /// forcing it off — the user may have Dim on independently.
    private var dimBeforeTalkback = false
    func setTalkbackEngaged(_ on: Bool) {
        guard let handle else { return }
        if on {
            if !monitorTalkback {
                dimBeforeTalkback = monitorDim
            }
            nc_monitor_set_talkback(handle, true)
            monitorTalkback = nc_monitor_talkback(handle)
            if !monitorDim {
                nc_monitor_set_dim(handle, true)
                monitorDim = nc_monitor_dim(handle)
            }
        } else {
            nc_monitor_set_talkback(handle, false)
            monitorTalkback = nc_monitor_talkback(handle)
            if monitorDim != dimBeforeTalkback {
                nc_monitor_set_dim(handle, dimBeforeTalkback)
                monitorDim = nc_monitor_dim(handle)
            }
        }
    }

    func setSpeakerSlot(_ slot: Int) {
        guard let handle else { return }
        nc_monitor_set_active_speaker_slot(handle, Int32(slot))
        // The speaker-simulation module reports the model of whichever slot is
        // active, so its row text goes stale unless the list is re-read.
        reloadMonitorState()
    }

    /// The speaker-model catalog and physical-output routes, read once from the engine.
    lazy var speakerModelCatalog: [String] = {
        guard let handle else { return [] }
        return (0..<Int(nc_speaker_model_count())).map { i in
            readString { nc_speaker_model_name(Int32(i), $0, $1) }
        }
    }()
    lazy var speakerOutputRoutes: [String] = {
        guard let handle else { return [] }
        return (0..<Int(nc_speaker_output_route_count())).map { i in
            readString { nc_speaker_output_route(Int32(i), $0, $1) }
        }
    }()

    /// Assign a speaker MODEL to a slot (spec-based virtual monitoring). Pass a bare
    /// catalog name; the engine stores the slotted form and clears any physical route.
    func setSpeakerModel(_ slot: Int, _ model: String) {
        guard let handle else { return }
        nc_monitor_set_speaker_model(handle, Int32(slot), model)
        reloadMonitorState()
        refreshHistory()
    }

    /// Route a slot straight to a physical output pair (no simulation), or "None" to
    /// return to the modelled path.
    func setSpeakerOutput(_ slot: Int, _ route: String) {
        guard let handle else { return }
        nc_monitor_set_speaker_output(handle, Int32(slot), route)
        reloadMonitorState()
        refreshHistory()
    }

    func setSpeakerRoomEq(_ slot: Int, _ enabled: Bool) {
        guard let handle else { return }
        nc_monitor_set_speaker_room_eq(handle, Int32(slot), enabled)
        reloadMonitorState()
        refreshHistory()
    }

    // MARK: Live MIDI input

    /// True while a keyboard is open and feeding armed instrument tracks.
    @Published private(set) var midiLiveActive = false
    /// So auto-start is attempted once per set of connected inputs, not every tick.
    private var midiLiveAutoStarted = false

    /// Opens the first MIDI source when one appears, then drains its notes into every
    /// armed / input-monitoring instrument track. Notes sound stopped or playing.
    private func pumpLiveMidi(_ handle: OpaquePointer) {
        if !nc_midi_live_active(handle) {
            let count = Int(nc_midi_input_count(handle))
            if count > 0, !midiLiveAutoStarted {
                let id = readString { nc_midi_input_id(handle, 0, $0, $1) }
                _ = id.withCString { nc_midi_live_start(handle, $0) }
                midiLiveAutoStarted = true
            } else if count == 0 {
                midiLiveAutoStarted = false
            }
        }
        nc_midi_pump_live_input(handle)
        let active = nc_midi_live_active(handle)
        if active != midiLiveActive { midiLiveActive = active }
    }

    /// The available MIDI input sources (id, name), for a source picker.
    func midiInputs() -> [(id: String, name: String)] {
        guard let handle else { return [] }
        return (0..<Int(nc_midi_input_count(handle))).map { i in
            (readString { nc_midi_input_id(handle, Int32(i), $0, $1) },
             readString { nc_midi_input_name(handle, Int32(i), $0, $1) })
        }
    }

    func startLiveMidi(_ sourceId: String) {
        guard let handle else { return }
        _ = sourceId.withCString { nc_midi_live_start(handle, $0) }
        midiLiveAutoStarted = true
        midiLiveActive = nc_midi_live_active(handle)
    }

    func stopLiveMidi() {
        guard let handle else { return }
        nc_midi_live_stop(handle)
        midiLiveAutoStarted = true   // don't immediately re-auto-start what the user stopped
        midiLiveActive = false
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

    // The three DSP sources are multi-select. The UI tracks exactly which the user
    // picked; the engine has one routing string, so a lone source maps to its own mode
    // and any combination maps to "auto" — the engine's use-everything-available mode.
    // Tracking the selection separately means clicking NDS lights NDS, not all three.
    enum DspSource: Hashable, CaseIterable { case internalDsp, external, nds }

    /// Derived from the engine mode on load, then owned by the UI as the user toggles.
    @Published private(set) var dspSources: Set<DspSource> = [.internalDsp]

    func usesDspSource(_ source: DspSource) -> Bool { dspSources.contains(source) }

    private func dspSourcesFromMode(_ mode: String) -> Set<DspSource> {
        switch mode {
        case "auto": return [.internalDsp, .external, .nds]
        case "remote_external": return [.external]
        case "nds", "external": return [.nds]
        default: return [.internalDsp]
        }
    }

    private func modeFromDspSources(_ sources: Set<DspSource>) -> String {
        if sources.count >= 2 { return "auto" }
        if sources.contains(.external) { return "remote_external" }
        if sources.contains(.nds) { return "nds" }
        return "internal"
    }

    /// Toggles a source in or out; the last one cannot be turned off.
    func toggleDspSource(_ source: DspSource) {
        var selected = dspSources
        if selected.contains(source) {
            guard selected.count > 1 else { return }
            selected.remove(source)
        } else {
            selected.insert(source)
        }
        dspSources = selected
        setMonitorPathMode(modeFromDspSources(selected))
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

        // Pulse the solo-implied blink ~2 Hz while any track is soloed; hold it off
        // otherwise so idle strips do not repaint. Only publish on a change.
        let blink = anyTrackSoloed && Int(CACurrentMediaTime() * 2.2).isMultiple(of: 2)
        if blink != soloBlinkOn { soloBlinkOn = blink }

        // Live MIDI: keep a keyboard open and drain its notes into armed instruments.
        pumpLiveMidi(handle)

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
