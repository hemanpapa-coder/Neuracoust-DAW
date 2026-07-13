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
    /// Full FFT spectrum magnitude bins (0..1, low→high frequency) for the analyzer.
    @Published private(set) var spectrumBins: [Float] = []
    /// Recent L/R sample pairs (interleaved) for the goniometer / vectorscope.
    @Published private(set) var goniometerSamples: [Float] = []
    // ITU-R BS.1770 loudness. True-peak is 4× oversampled (inter-sample peaks).
    @Published private(set) var momentaryLufs: Float = -70
    @Published private(set) var shortTermLufs: Float = -70
    @Published private(set) var integratedLufs: Float = -70
    @Published private(set) var loudnessRange: Float = 0
    @Published private(set) var truePeakDb: Float = -120
    /// Incoming audio-interface input peak (0..1) and MIDI-input activity (0..1), both
    /// smoothed with a decay so the meters fall back gently.
    @Published private(set) var inputPeak: Float = 0
    @Published private(set) var midiActivity: Float = 0
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
    /// Help mode: while on, controls carry hover tooltips describing what they do. Off by
    /// default so the tooltips don't nag during normal work.
    @Published var helpMode = false
    /// The control currently hovered in help mode, plus its window-space frame, so a single
    /// top-level overlay can draw the tooltip beside it (native .help was unreliable).
    struct HelpHover: Equatable { let text: String; let frame: CGRect }
    @Published var helpHover: HelpHover?
    @Published var loopEnabled = false
    /// The loop range doubles as the edit range, the way the old UI used it.
    @Published private(set) var loopStartSeconds: Double = 0
    @Published private(set) var loopEndSeconds: Double = 4
    @Published var clickEnabled = false
    @Published var snapEnabled = true
    @Published var recording = false
    /// Which timebases the timeline ruler shows (bars / time / samples). Any subset.
    @Published var rulerBars = true
    @Published var rulerTime = true
    @Published var rulerSamples = false
    /// Shared lane height, dragged from any lane's bottom edge; persisted.
    /// Default lane height, and per-track overrides (so a multi-selection resizes together).
    @Published var laneHeight: CGFloat = 100
    @Published var laneHeights: [Int: CGFloat] = [:]

    func laneHeightFor(_ trackId: Int) -> CGFloat { laneHeights[trackId] ?? laneHeight }

    func setLaneHeight(trackIds: [Int], height: CGFloat) {
        let h = min(320, max(40, height))
        for id in trackIds { laneHeights[id] = h }
    }
    func commitLaneHeight() {
        let encoded = laneHeights.map { "\($0.key):\(Int($0.value))" }.joined(separator: "|")
        UserDefaults.standard.set(encoded, forKey: SettingsKey.laneHeight)
    }

    /// Default mixer channel-strip width, and per-track overrides — the same idea as
    /// lane height: drag a strip's side edge to widen/narrow it (a multi-selection moves
    /// together), snapped to a step, persisted.
    static let channelWidthDefault: CGFloat = 110
    static let channelWidthStep: CGFloat = 12
    // Width narrows left-right only (height is unchanged); the strip keeps every element,
    // the scales just tuck behind the fader cap as it tightens.
    static let channelWidthMin: CGFloat = 58
    // No free widening past the default — the width toggle is small ↔ large only.
    static let channelWidthMax: CGFloat = channelWidthDefault
    // Per-track width so narrowing a channel touches only that channel (or the current
    // multi-selection), never the whole mixer. Strips default to the full width, so a new
    // aux — or the master — is never narrower than the channels.
    @Published var channelWidths: [Int: CGFloat] = [:]

    func channelWidthFor(_ trackId: Int) -> CGFloat { channelWidths[trackId] ?? Self.channelWidthDefault }

    func setChannelWidth(trackIds: [Int], width: CGFloat) {
        // No step-snapping: the toggle sets exactly min or default, and snapping to a
        // 12 pt grid rounded the narrow width (58) up to 60, which then read as "wide"
        // and left the strip stuck narrow.
        let w = min(Self.channelWidthMax, max(Self.channelWidthMin, width))
        for id in trackIds { channelWidths[id] = w }
    }
    func commitChannelWidth() {
        let encoded = channelWidths.map { "\($0.key):\(Int($0.value))" }.joined(separator: "|")
        UserDefaults.standard.set(encoded, forKey: SettingsKey.channelWidth)
    }

    /// Live width-drag preview. While a strip's right edge is dragged, every targeted
    /// strip (the whole mixer selection when the dragged strip is part of it) renders at
    /// `width` until the gesture commits. nil when no drag is in flight.
    struct ChannelWidthDrag { var targets: Set<Int>; var anchorWidth: CGFloat; var width: CGFloat }
    @Published var channelWidthDrag: ChannelWidthDrag?

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

    struct TrackSend: Hashable {
        let bus: String
        let gainDb: Float
        var pan: Float = 0
        var preFader: Bool = false
    }

    struct Track: Identifiable {
        let id: Int
        let name: String
        let kind: TrackKind
        let colorHex: String
        let folder: String
        var notes: String = ""
        var inputBus: String
        var outputBus: String
        var volumeDb: Float
        var pan: Float
        var muted: Bool
        var solo: Bool
        var recordArmed: Bool
        var inputMonitoring: Bool
        var automationMode: String = "read"
        var inserts: [InsertSlot]
        /// What turns this track's MIDI notes into sound. Empty on every other kind.
        var instrumentName: String
        var sends: [TrackSend]

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

    /// Remove the instrument from an instrument track's slot.
    func clearInstrument(track trackId: Int) {
        guard let handle else { return }
        if nc_track_clear_instrument(handle, Int32(trackId)) {
            reloadTracks()
            refreshHistory()
        }
    }

    /// Drag a plugin to an exact slot (A–E), Pro Tools style — it stays where you drop it,
    /// leaving earlier slots empty. Dropping onto an occupied slot swaps the two.
    func moveInsert(track trackId: Int, from: Int, to: Int) {
        guard let handle, from != to else { return }
        if nc_track_move_insert_to_slot(handle, Int32(trackId), Int32(from), Int32(to)) >= 0 {
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

    /// The active edit tool. Smart behaves as before (grab decides from the hit zone);
    /// the others force one behavior, the way Pro Tools / Logic tools do.
    enum EditTool: String, CaseIterable, Identifiable {
        case smart, grabber, selector, trim, split, fade, zoom
        var id: String { rawValue }
        var symbol: String {
            switch self {
            case .smart: return "wand.and.stars"
            case .grabber: return "hand.raised"
            case .selector: return "rectangle.dashed"
            case .trim: return "arrow.left.and.right.square"
            case .split: return "scissors"
            case .fade: return "line.diagonal"
            case .zoom: return "magnifyingglass"
            }
        }
        var label: String {
            switch self {
            case .smart: return "스마트"
            case .grabber: return "이동"
            case .selector: return "선택"
            case .trim: return "트림"
            case .split: return "분할"
            case .fade: return "페이드"
            case .zoom: return "줌"
            }
        }
    }
    @Published var editTool: EditTool = .smart

    /// Pro Tools edit modes. Grid snaps to the timeline quantum, Slip is free, Shuffle
    /// ripples neighbours on move/delete, Spot places a clip by typed time. The engine
    /// has no edit-mode state — the UI decides which move/delete path to take.
    enum EditMode: String, CaseIterable, Identifiable {
        case shuffle, slip, spot, grid
        var id: String { rawValue }
        var symbol: String {
            switch self {
            case .shuffle: return "arrow.left.arrow.right"
            case .slip: return "arrow.left.and.right"
            case .spot: return "scope"
            case .grid: return "grid"
            }
        }
        var label: String {
            switch self {
            case .shuffle: return "셔플"
            case .slip: return "슬립"
            case .spot: return "스팟"
            case .grid: return "그리드"
            }
        }
    }
    @Published var editMode: EditMode = .grid {
        didSet {
            snapEnabled = (editMode == .grid)
            if let handle { nc_project_set_edit_mode(handle, editMode.rawValue.capitalized) }
        }
    }

    /// Grid resolution (snap quantum) used while in Grid mode.
    enum GridUnit: String, CaseIterable, Identifiable {
        case bar = "1 bar", beat = "1 beat", quarter = "1/4 beat"
        case eighth = "1/8 beat", sixteenth = "1/16 beat", tenth = "0.1s", frame = "1 frame"
        var id: String { rawValue }
        /// Bar/beat units follow the tempo map; time units are fixed wall-clock.
        static var musicalCases: [GridUnit] { [.bar, .beat, .quarter, .eighth, .sixteenth] }
        static var timeCases: [GridUnit] { [.tenth, .frame] }
        var label: String {
            switch self {
            case .bar: return "1 마디"; case .beat: return "1 비트"; case .quarter: return "1/4"
            case .eighth: return "1/8"; case .sixteenth: return "1/16"; case .tenth: return "0.1초"; case .frame: return "1 프레임"
            }
        }
    }
    @Published var gridUnit: GridUnit = .beat

    func setGridUnit(_ unit: GridUnit) {
        gridUnit = unit
        guard let handle else { return }
        _ = unit.rawValue.withCString { nc_project_set_grid_unit(handle, $0) }
        UserDefaults.standard.set(unit.rawValue, forKey: SettingsKey.gridUnit)
    }

    /// Spot mode surfaces this sheet: the clip awaiting an exact placement time.
    @Published var spotTargetClipId: String?

    /// Split a specific clip at a timeline second (the 분할 tool clicking a clip).
    func splitClipAt(_ clipId: String, seconds: Double) {
        guard let handle else { return }
        if nc_clip_split(handle, clipId, snap(seconds)) {
            reloadClips()
            refreshHistory()
        }
    }

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

    /// True while the Speaker Simulation monitor-DSP module is enabled. Routing a speaker
    /// set to a physical output is a raw passthrough that bypasses the simulation, so the
    /// speaker-set menu hides 물리 출력 while this is on.
    var speakerSimulationActive: Bool {
        monitorModules.contains { $0.enabled && $0.name.localizedCaseInsensitiveContains("Speaker Simulation") }
    }
    /// Headphone-specific correction modules — shown in the headphone panel so the user
    /// can see whether the virtual-speaker A/B/C is paired with crossfeed / correction.
    var headphoneSimulationActive: Bool {
        monitorModules.contains { $0.enabled && $0.name.localizedCaseInsensitiveContains("Headphone Simulation") }
    }
    var crossfeedActive: Bool {
        monitorModules.contains { $0.enabled && $0.name.localizedCaseInsensitiveContains("Crossfeed") }
    }
    @Published private(set) var speakerSets: [SpeakerSet] = []
    @Published private(set) var activeSpeakerSlot = 0
    @Published private(set) var monitorVolumeDb: Float = -6
    @Published private(set) var monitorListen = MonitorListen()
    @Published private(set) var monitorDim = false
    @Published private(set) var monitorMono = false
    @Published private(set) var monitorMute = false
    @Published private(set) var monitorTalkback = false
    /// Monitor the DAW master (false, default) or the computer's input source (true).
    @Published private(set) var monitorListenSource = false
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

    // Reference/monitor input — pick BlackHole here to A/B reference music into the mix.
    @Published private(set) var inputDevices: [OutputDevice] = []
    @Published private(set) var currentInputDeviceId = ""   // empty = system default

    func refreshInputDevices() {
        guard let handle else { return }
        let count = Int(nc_input_device_count(handle))
        inputDevices = (0..<count).map { i in
            OutputDevice(id: readEngineString { nc_input_device_id(handle, Int32(i), $0, $1) },
                         name: readEngineString(capacity: 256) { nc_input_device_name(handle, Int32(i), $0, $1) })
        }
        currentInputDeviceId = readEngineString { nc_current_input_device_id(handle, $0, $1) }
    }

    /// An empty id selects the system default. Changing the device restarts the engine.
    func setInputDevice(_ id: String) {
        guard let handle else { return }
        nc_set_input_device(handle, id.isEmpty ? nil : id)
        currentInputDeviceId = readEngineString { nc_current_input_device_id(handle, $0, $1) }
    }

    // Live meters, refreshed each tick.
    @Published private(set) var phaseCorrelation: Float = 0
    /// Plugin delay compensation: the enable flag and the engine's reported alignment.
    /// (`delayCompensationMs` is declared with the other status fields above.)
    @Published var delayCompensationEnabled: Bool = true
    @Published private(set) var delayCompensationSamples: Int = 0
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
    /// A held key must not wait a 33 ms UI tick to sound. This second timer only drains the
    /// live-MIDI queue into the instruments, ~240 Hz, so monitoring latency drops toward one
    /// audio buffer instead of one UI frame — the "레이턴시" the user felt against Logic.
    private var midiPumpTimer: Timer?
    private let midiPumpInterval = 1.0 / 240.0
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

    /// Localized UI string for `key` in the OS language, English fallback, then the key.
    func tr(_ key: String) -> String {
        var buffer = [CChar](repeating: 0, count: 512)
        key.withCString { nc_tr($0, &buffer, buffer.count) }
        return String(cString: buffer)
    }

    func start() {
        guard let handle else { return }

        // Localize to the user's preferred language (Korean / English / Japanese / Chinese),
        // English for anything a table doesn't cover. preferredLanguages reflects the OS
        // setting even though the app ships no .lproj (Locale.current would report en).
        nc_set_ui_language(Locale.preferredLanguages.first ?? Locale.current.identifier)

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
        installPluginEditorTransportObserver()

        let timer = Timer(timeInterval: tickInterval, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.tick() }
        }
        RunLoop.main.add(timer, forMode: .common)
        self.timer = timer

        // Drain live MIDI far more often than the 30 Hz tick so a played note reaches the
        // instrument within ~one audio buffer. The tick still owns source auto-open and the
        // activity meter; this only pumps the queue (a no-op when nothing is pending).
        let midiTimer = Timer(timeInterval: midiPumpInterval, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self, let handle = self.handle else { return }
                nc_midi_pump_live_input(handle)
            }
        }
        midiTimer.tolerance = 0
        RunLoop.main.add(midiTimer, forMode: .common)
        self.midiPumpTimer = midiTimer

        restorePersistedSettings()
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

    /// An out-of-process plugin editor's window steals key events, so its host process
    /// catches the spacebar itself and broadcasts this notification. DW listens for it
    /// and toggles the transport, so space works with a plugin GUI focused.
    private func installPluginEditorTransportObserver() {
        DistributedNotificationCenter.default().addObserver(
            forName: Notification.Name("com.neuracoust.daw.pluginEditor.transport.toggle"),
            object: nil,
            queue: .main
        ) { [weak self] _ in
            MainActor.assumeIsolated { self?.togglePlay() }
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
        static let h: UInt16 = 4
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
            case KeyCode.h where !selectedClipIds.isEmpty || hasEditRange:
                healSelectedClips()
                return nil
            case KeyCode.delete, KeyCode.forwardDelete:
                if selectedConductor != nil {
                    deleteSelectedConductor()
                    return nil
                }
                if let regionId = selectedRegionId {
                    deleteMidiRegion(regionId)
                    return nil
                }
                if !selectedClipIds.isEmpty {
                    deleteSelectedClips()
                    return nil
                }
                // Nothing time-based selected: a lone track/channel selection is the target.
                // deleteSelectedTrack asks first when the track carries clips, and the delete
                // is undoable, so this is safe.
                if selectedTrackId != nil, canDeleteSelectedTrack {
                    deleteSelectedTrack()
                    return nil
                }
                return event
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
        midiPumpTimer?.invalidate()
        midiPumpTimer = nil
        if let handle {
            nc_engine_stop(handle)
        }
    }

    // MARK: - Transport

    /// What the Stop button does with the playhead. Pro Tools calls the second one
    /// "Timeline Insertion Follows Playback" off — return to where you started.
    enum StopBehavior: String, CaseIterable, Identifiable {
        case inPlace, returnToStart
        var id: String { rawValue }
        var label: String { self == .inPlace ? "그 자리에 멈춤" : "출발한 곳으로" }
    }
    @Published var stopBehavior: StopBehavior = .inPlace
    /// Where playback last started from, for `returnToStart`.
    private var playStartSeconds: Double = 0

    func togglePlay() {
        setTransport(running: !transportRunning)
    }

    func stop() {
        let wasRunning = transportRunning
        setTransport(running: false)
        // Stop no longer jumps to the very start. Stay put, or return to where playback
        // began (never bar 1 unless that is where you pressed play), per the chosen mode.
        if stopBehavior == .returnToStart, wasRunning {
            seek(playStartSeconds)
        }
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
        if running, !transportRunning { playStartSeconds = playheadSeconds }   // remember for stop
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
    /// Pan law for mono tracks (project setting): "-3dB" / "-4.5dB" / "-6dB" / "legacy".
    @Published private(set) var panLaw: String = "legacy"

    func setPanLaw(_ law: String) {
        guard let handle else { return }
        _ = law.withCString { nc_project_set_pan_law(handle, $0) }
        panLaw = readEngineString { nc_project_pan_law(handle, $0, $1) }
        refreshHistory()
    }

    private func reloadTracks() {
        guard let handle else { return }
        panLaw = readEngineString { nc_project_pan_law(handle, $0, $1) }

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
                notes: readEngineString { nc_track_notes(handle, i, $0, $1) },
                inputBus: readEngineString { nc_track_input_bus(handle, i, $0, $1) },
                outputBus: readEngineString { nc_track_output_bus(handle, i, $0, $1) },
                volumeDb: nc_track_volume_db(handle, i),
                pan: nc_track_pan(handle, i),
                muted: nc_track_muted(handle, i),
                solo: nc_track_solo(handle, i),
                recordArmed: nc_track_record_armed(handle, i),
                inputMonitoring: nc_track_input_monitoring(handle, i),
                automationMode: readEngineString { nc_track_automation_mode(handle, i, $0, $1) },
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
                    TrackSend(bus: readEngineString { nc_track_send_bus(handle, i, Int32(slot), $0, $1) },
                              gainDb: nc_track_send_gain_db(handle, i, Int32(slot)),
                              pan: nc_track_send_pan(handle, i, Int32(slot)),
                              preFader: nc_track_send_pre_fader(handle, i, Int32(slot)))
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

    // MARK: Automation modes (Off / Read / Touch / Latch / Write)

    static let automationModes: [(id: String, label: String)] = [
        ("read", "Read"), ("touch", "Touch"), ("latch", "Latch"), ("write", "Write"), ("off", "Off"),
    ]

    func automationMode(_ trackId: Int) -> String {
        tracks.first { $0.id == trackId }?.automationMode ?? "read"
    }
    func setAutomationMode(_ trackId: Int, _ mode: String) {
        guard let handle else { return }
        _ = mode.withCString { nc_track_set_automation_mode(handle, Int32(trackId), $0) }
        reloadTracks()
        refreshHistory()
    }

    /// Click-cycle Read → Touch → Latch → Write → Off → Read.
    func cycleAutomationMode(_ trackId: Int) {
        let ids = Self.automationModes.map(\.id)
        let current = automationMode(trackId)
        let next = ids[((ids.firstIndex(of: current) ?? 0) + 1) % ids.count]
        setAutomationMode(trackId, next)
    }

    /// True once any live automation was written this playback pass — records one undo step
    /// and reloads the drawn lanes when the transport stops.
    private var automationPassDirty = false
    /// Params ("trackId:param") the user is currently holding (mouse down on the control).
    private var touchingAuto: Set<String> = []
    /// Latch params still writing their last value after the touch was released.
    private var latchedAuto: Set<String> = []
    /// Per-key previous playhead written, so a sweep erases (prev, now] each tick.
    private var autoWritePrev: [String: Double] = [:]

    /// The mixer control tells us when a fader/pan is grabbed and released, so Touch/Latch
    /// can write the held value (even motionless) and not be tugged by fader-follow.
    func beginAutomationTouch(_ trackId: Int, _ param: String) {
        let key = "\(trackId):\(param)"
        touchingAuto.insert(key)
        latchedAuto.remove(key)
        autoWritePrev[key] = playheadSeconds
    }
    func endAutomationTouch(_ trackId: Int, _ param: String) {
        let key = "\(trackId):\(param)"
        touchingAuto.remove(key)
        if automationMode(trackId) == "latch" { latchedAuto.insert(key) }   // latch holds until stop
    }

    private func sweepWrite(_ trackId: Int, _ param: String, _ value: Float) {
        guard let handle else { return }
        let key = "\(trackId):\(param)"
        let now = playheadSeconds
        let prev = autoWritePrev[key] ?? now
        if abs(now - prev) < 0.03 && autoWritePrev[key] != nil { return }    // thin to ~30 Hz
        _ = param.withCString { nc_track_automation_write_sweep(handle, Int32(trackId), $0, prev, now, value) }
        autoWritePrev[key] = now
        automationPassDirty = true
    }

    /// Each tick while playing. Per track/param: Write always overwrites the current value;
    /// Touch/Latch overwrite while the control is held (or, for Latch, still latched); every
    /// non-writing state follows the played-back automation so the control visibly moves.
    private func serviceAutomation() {
        guard let handle, transportRunning else { return }
        let now = playheadSeconds
        for track in tracks where track.kind.hasSolo {
            let mode = track.automationMode
            for (param, value) in [("track.volume", track.volumeDb), ("track.pan", track.pan)] {
                let key = "\(track.id):\(param)"
                let touching = touchingAuto.contains(key)
                let latched = latchedAuto.contains(key)
                let writing = mode == "write"
                    || ((mode == "touch" || mode == "latch") && touching)
                    || (mode == "latch" && latched)
                if writing {
                    sweepWrite(track.id, param, value)                 // held control stays; old curve erased
                } else if mode == "read" || ((mode == "touch" || mode == "latch") && !touching && !latched) {
                    let auto = nc_track_automation_value_at(handle, Int32(track.id), param, now, value)
                    if let idx = tracks.firstIndex(where: { $0.id == track.id }) {
                        if param == "track.volume" { if abs(tracks[idx].volumeDb - auto) > 0.05 { tracks[idx].volumeDb = auto } }
                        else { if abs(tracks[idx].pan - auto) > 0.002 { tracks[idx].pan = auto } }
                    }
                }
            }
        }
    }

    /// On stop, commit the live-written automation as one undo step and refresh the lanes.
    private func finishAutomationPass() {
        touchingAuto.removeAll(); latchedAuto.removeAll(); autoWritePrev.removeAll()
        guard automationPassDirty else { return }
        automationPassDirty = false
        reloadTracks()
        recordGesture("Automation")
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

        // Pro Tools-style: a project is a folder made up front. Prompt for name/location and
        // create it now so imports and recordings land inside it from the first drop — there
        // is no "Untitled" temp limbo. Cancelling the prompt leaves the current project intact
        // (nothing has been torn down yet).
        guard let (folderURL, ndawURL) = promptForProjectFolder(defaultName: "Untitled") else { return }

        nc_project_new(handle)
        keyEventStore = []          // conductor key state is per-project; reset it
        musicalKey = "C"
        afterProjectReplaced()
        // A fresh project inherits the saved app-level monitor / DSP / edit settings, so a
        // new session starts from "전체 설정 저장" instead of the bare engine defaults.
        reapplyPersistedSettingsToNewProject()

        // Give the fresh project its folder home immediately (document + Audio Files + icon).
        var errorBuffer = [CChar](repeating: 0, count: 256)
        if nc_project_save_as(handle, ndawURL.path, &errorBuffer, errorBuffer.count) {
            applyProjectFolderIcon(to: folderURL)
            rememberRecentProject(ndawURL)
            refreshHistory()
        } else {
            lastError = String(cString: errorBuffer)
        }
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

    /// Recently opened/saved documents, newest first, capped. Persisted across launches
    /// so the File ▸ 최근 항목 submenu can reopen the last sessions.
    @Published private(set) var recentProjects: [URL] = []
    private static let maxRecentProjects = 12

    private func loadRecentProjects() {
        let paths = UserDefaults.standard.stringArray(forKey: SettingsKey.recentProjects) ?? []
        recentProjects = paths.map { URL(fileURLWithPath: $0) }
    }

    private func rememberRecentProject(_ url: URL) {
        let standardized = url.standardizedFileURL
        var list = recentProjects.filter { $0.standardizedFileURL != standardized }
        list.insert(standardized, at: 0)
        if list.count > Self.maxRecentProjects { list = Array(list.prefix(Self.maxRecentProjects)) }
        recentProjects = list
        UserDefaults.standard.set(list.map { $0.path }, forKey: SettingsKey.recentProjects)
    }

    /// Reopen a document from the recent list. A path that has since moved or been
    /// deleted drops out of the list rather than erroring.
    func openRecentProject(_ url: URL) {
        guard FileManager.default.fileExists(atPath: url.path) else {
            recentProjects.removeAll { $0.standardizedFileURL == url.standardizedFileURL }
            UserDefaults.standard.set(recentProjects.map { $0.path }, forKey: SettingsKey.recentProjects)
            lastError = "파일을 찾을 수 없습니다: \(url.lastPathComponent)"
            return
        }
        openProject(at: url)
    }

    func clearRecentProjects() {
        recentProjects = []
        UserDefaults.standard.removeObject(forKey: SettingsKey.recentProjects)
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

    /// Ask for a name/location and create the project folder: <chosen>/<name>/<name>.ndaw,
    /// with the Audio Files folder alongside the document inside it. Shared by Save As and
    /// New Project. If the user already picked a folder whose name matches (re-saving in
    /// place), don't nest another level. Returns nil if cancelled or the folder can't be made.
    private func promptForProjectFolder(defaultName: String) -> (folder: URL, ndaw: URL)? {
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.init(filenameExtension: "ndaw")].compactMap { $0 }
        panel.nameFieldStringValue = defaultName
        panel.message = "프로젝트는 폴더로 만들어집니다 — 오디오 파일이 그 안의 Audio Files 폴더에 모입니다."
        guard panel.runModal() == .OK, let url = panel.url else { return nil }

        let baseName = url.deletingPathExtension().lastPathComponent
        let parent = url.deletingLastPathComponent()
        let folderURL = parent.lastPathComponent == baseName ? parent : url.deletingPathExtension()
        let ndawURL = folderURL.appendingPathComponent(baseName).appendingPathExtension("ndaw")
        do {
            try FileManager.default.createDirectory(at: folderURL, withIntermediateDirectories: true)
        } catch {
            lastError = "프로젝트 폴더를 만들 수 없습니다: \(error.localizedDescription)"
            return nil
        }
        return (folderURL, ndawURL)
    }

    @discardableResult
    func saveProjectAs() -> Bool {
        guard let handle else { return false }

        guard let (folderURL, ndawURL) = promptForProjectFolder(defaultName: projectName.isEmpty ? "Untitled" : projectName) else {
            return false
        }

        var errorBuffer = [CChar](repeating: 0, count: 256)
        guard nc_project_save_as(handle, ndawURL.path, &errorBuffer, errorBuffer.count) else {
            lastError = String(cString: errorBuffer)
            return false
        }
        // Pull any temp / external media into this folder's Audio Files so it travels
        // with the project. Harmless (returns 0) when everything is already inside. Only
        // clip source paths change, so refresh the clips (waveforms re-read from the new
        // paths) rather than reloading the whole document.
        if nc_project_consolidate_media(handle, &errorBuffer, errorBuffer.count) > 0 {
            reloadClips()
        }
        applyProjectFolderIcon(to: folderURL)
        refreshHistory()
        rememberRecentProject(ndawURL)
        return true
    }

    /// Brand the project folder with the Neuracoust logo (the bundled app icon), the way
    /// Logic marks its project folders. Best-effort — a failure just leaves the plain
    /// Finder folder icon.
    private func applyProjectFolderIcon(to folderURL: URL) {
        let icon = Bundle.main.url(forResource: "NeuracoustDAW", withExtension: "icns")
            .flatMap { NSImage(contentsOf: $0) } ?? NSApp.applicationIconImage
        guard let icon else { return }
        NSWorkspace.shared.setIcon(icon, forFile: folderURL.path, options: [])
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
            rememberRecentProject(url)
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

    /// How to treat the engine's musical analysis (tempo / key / chord / section markers)
    /// for an import.
    enum ImportAnalysisChoice { case apply, analyzeOnly, skip }

    /// Ask once per import whether to analyse and whether to commit the analysis to the
    /// timeline. A modal, so it must run on the main thread (imports already do).
    private func askImportAnalysisChoice() -> ImportAnalysisChoice {
        let alert = NSAlert()
        alert.messageText = "음원 분석"
        alert.informativeText = "임포트한 오디오에서 템포·조성·코드·섹션 마커를 분석할 수 있습니다.\n분석하고 그 결과를 타임라인(템포·마커·조성·코드)에 적용할지 선택하세요."
        alert.addButton(withTitle: "분석 + 타임라인 적용")
        alert.addButton(withTitle: "분석만 (적용 안 함)")
        alert.addButton(withTitle: "원본 그대로 (분석 안 함)")
        switch alert.runModal() {
        case .alertFirstButtonReturn: return .apply
        case .alertSecondButtonReturn: return .analyzeOnly
        default: return .skip
        }
    }

    /// Dropping files places the first at `startSeconds`; the rest follow end-to-end.
    /// Track and time come from where the drop landed, not the playhead.
    func importAudio(intoTrack trackId: Int, at startSeconds: Double, from urls: [URL]) {
        guard let handle, !urls.isEmpty else { return }

        // Ask once, up front, how to treat the analysis for this batch.
        let choice = askImportAnalysisChoice()
        let analyze = choice != .skip
        let applyToTimeline = choice == .apply

        var start = max(0, startSeconds)
        var lastSummary = ""
        for url in urls {
            guard nc_audio_import_supported(url.path) else {
                lastError = "지원하지 않는 형식: \(url.lastPathComponent)"
                continue
            }
            var buffer = [CChar](repeating: 0, count: 512)
            if nc_audio_import_analyzed(handle, Int32(trackId), url.path, start, analyze, applyToTimeline,
                                        &buffer, buffer.count) {
                lastSummary = String(cString: buffer)
                start += clipDuration(ofLast: trackId)
            } else {
                lastError = String(cString: buffer)
            }
        }
        reloadTracks()
        reloadClips()
        if applyToTimeline {
            // The analysis rewrote tempo / meter / markers — refresh those so they show now.
            reloadMarkers()
            tempoBpm = Int(nc_project_tempo_bpm(handle))
            timeSignature = (Int(nc_project_time_signature_numerator(handle)),
                             Int(nc_project_time_signature_denominator(handle)))
        }
        refreshHistory()
        // "분석만" leaves the timeline untouched, so its only output is the detected info —
        // show it, otherwise the mode would look like it did nothing.
        if choice == .analyzeOnly, !lastSummary.isEmpty {
            let info = NSAlert()
            info.messageText = "음원 분석 결과 (타임라인 미적용)"
            info.informativeText = lastSummary
            info.addButton(withTitle: "확인")
            info.runModal()
        } else if !lastSummary.isEmpty {
            lastError = lastSummary
        }
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
        // A new/opened engine project resets to its own default edit mode, so re-apply the
        // UI's mode and grid unit — otherwise snapping silently falls back to the 0.1 s
        // quantum instead of the musical (beat) grid the toolbar is showing.
        nc_project_set_edit_mode(handle, editMode.rawValue.capitalized)
        _ = gridUnit.rawValue.withCString { nc_project_set_grid_unit(handle, $0) }
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
        var fadeInCurve: String = "equal_power"
        var fadeOutCurve: String = "equal_power"
        let gainDb: Float
    }

    @Published private(set) var clips: [Clip] = []

    /// Fade shapes the renderer honours (label, engine curve id).
    static let fadeCurves: [(label: String, id: String)] = [
        ("등파워", "equal_power"), ("리니어", "linear"), ("지수(느리게)", "slow"), ("로그(빠르게)", "fast"),
    ]

    func setClipFadeInCurve(_ clipId: String, _ curve: String) {
        guard let handle, let clip = clips.first(where: { $0.id == clipId }) else { return }
        _ = curve.withCString { c in clip.fadeOutCurve.withCString { o in nc_clip_set_fade_curves(handle, clipId, c, o) } }
        reloadClips(); refreshHistory()
    }
    func setClipFadeOutCurve(_ clipId: String, _ curve: String) {
        guard let handle, let clip = clips.first(where: { $0.id == clipId }) else { return }
        _ = clip.fadeInCurve.withCString { i in curve.withCString { c in nc_clip_set_fade_curves(handle, clipId, i, c) } }
        reloadClips(); refreshHistory()
    }

    /// Peak envelopes keyed by source path, fetched once per file from the engine.
    /// Cached peaks per source path. `channels` is 1 (draw one envelope from L) or 2
    /// (draw L on top, R below). For mono, L holds the single channel and R is empty.
    struct WaveformData: Equatable {
        let channels: Int
        let minsL: [Float]
        let maxsL: [Float]
        let minsR: [Float]
        let maxsR: [Float]
        let durationSeconds: Double
    }
    @Published private(set) var waveforms: [String: WaveformData] = [:]

    /// Timeline selection. Purely a view concept; the engine has no notion of it.
    @Published var selectedClipIds: Set<String> = []
    @Published var selectedTrackId: Int?

    /// Mixer strip selection — a set, distinct from the timeline's single `selectedTrackId`
    /// (which it keeps in sync with the last-clicked strip so the lane highlights too).
    /// Click selects one; ⌘/⇧-click toggles. A width drag on any selected strip resizes
    /// the whole set together.
    @Published var selectedMixerTrackIds: Set<Int> = []

    func selectMixerTrack(_ trackId: Int, additive: Bool) {
        if additive {
            if selectedMixerTrackIds.contains(trackId) {
                selectedMixerTrackIds.remove(trackId)
            } else {
                selectedMixerTrackIds.insert(trackId)
            }
        } else {
            selectedMixerTrackIds = [trackId]
        }
        selectedTrackId = trackId
        focusTrackForDeletion()   // last click wins: the track is now the Delete target
    }

    /// Selecting a track/channel makes it the Delete-key target, so drop the clip / region /
    /// conductor selections (the reverse — selecting a clip keeps the track for the Inspector
    /// but takes the Delete key, because clips are checked before the track).
    private func focusTrackForDeletion() {
        selectedClipIds = []
        selectedRegionId = nil
        selectedConductor = nil
    }

    /// The track shown in the Channel column and the Inspector — the last-clicked
    /// selection, falling back to the first mixer track so the panels are never empty.
    var inspectedTrack: Track? {
        if let id = selectedTrackId, let t = tracks.first(where: { $0.id == id }) { return t }
        return mixerTracks.first
    }

    /// Nuendo-style Edit-view side panels, toggled from the transport bar. The Channel
    /// column reuses the mixer's `ChannelStrip`; the Inspector is its own view.
    @Published var showChannelColumn = true
    @Published var showInspector = true
    @Published var showMonitorDock = true

    /// Fades and clip gain edit one clip at a time; they hide on a multi-selection.
    var selectedClipId: String? { selectedClipIds.count == 1 ? selectedClipIds.first : nil }

    /// Also clears any region selection — clicking a clip, or empty space, means
    /// the Delete key no longer points at a region.
    func selectClip(_ clipId: String?) {
        selectedClipIds = clipId.map { [$0] } ?? []
        selectedRegionId = nil
        if clipId != nil { selectedConductor = nil }
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
                    let fallback: Float = parameter.isVolume ? track.volumeDb
                                        : parameter.isPan ? track.pan
                                        : (parameter.pluginFallback ?? 0.5)
                    automation = TimelineModel.Automation(
                        parameterId: parameter.id,
                        displayName: parameter.displayName,
                        range: parameter.range,
                        fallback: fallback,
                        points: automationPoints(trackId: track.id, parameter))
                }
                return TimelineModel.Lane(name: track.name,
                                          accent: NSColor.from(track.kind.accent),
                                          muted: track.muted,
                                          selected: selectedMixerTrackIds.contains(track.id),
                                          trackId: track.id,
                                          soloed: track.solo,
                                          armed: track.recordArmed,
                                          inputMonitor: track.inputMonitoring,
                                          soloSilencedBlink: anyTrackSoloed && !track.solo
                                              && track.kind.hasSolo && soloBlinkOn,
                                          volumeDb: track.volumeDb,
                                          pan: track.pan,
                                          peakLeft: track.peakLeft,
                                          peakRight: track.peakRight,
                                          automationMode: track.automationMode,
                                          automation: automation,
                                          inserts: track.inserts.prefix(4).map {
                                              TimelineModel.InsertChip(name: $0.name, bypassed: $0.bypassed, isEmpty: $0.isEmpty)
                                          },
                                          sends: track.sends.map {
                                              TimelineModel.SendChip(label: "\($0.bus) \(Int($0.gainDb)) \(sendPanLabel($0.pan))",
                                                                     preFader: $0.preFader)
                                          },
                                          height: laneHeightFor(track.id))
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
            sampleRate: sampleRate > 0 ? sampleRate : 48000,
            laneHeight: laneHeight,
            rulerBars: rulerBars,
            rulerTime: rulerTime,
            rulerSamples: rulerSamples,
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

    /// Toggle one of the ruler's timebases; at least one stays on.
    func toggleRulerTimebase(_ which: RulerTimebase) {
        switch which {
        case .bars: rulerBars.toggle()
        case .time: rulerTime.toggle()
        case .samples: rulerSamples.toggle()
        }
        if !rulerBars && !rulerTime && !rulerSamples { rulerBars = true }   // never all off
        let d = UserDefaults.standard
        d.set(rulerBars, forKey: SettingsKey.rulerBars)
        d.set(rulerTime, forKey: SettingsKey.rulerTime)
        d.set(rulerSamples, forKey: SettingsKey.rulerSamples)
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
    /// Option-drag copy, committed on release: duplicate the original and place the copy
    /// at the drop lane + time. Nothing is created until the drop, so no clip ever sums
    /// in place while dragging. `laneIndex` addresses `laneTracks`; -1 falls back to the
    /// original's own track.
    func dropClipCopy(_ clipId: String, laneIndex: Int, startSeconds: Double) {
        guard let handle else { return }
        var buffer = [CChar](repeating: 0, count: 128)
        guard nc_clip_duplicate(handle, clipId, &buffer, buffer.count) else { return }
        let newId = String(cString: buffer)

        var targetTrackId = -1
        if laneIndex >= 0 && laneIndex < laneTracks.count {
            targetTrackId = laneTracks[laneIndex].id
        } else if let clip = clips.first(where: { $0.id == clipId }),
                  let track = laneTracks.first(where: { $0.name == clip.trackName }) {
            targetTrackId = track.id
        }

        if targetTrackId >= 0 {
            var out = [CChar](repeating: 0, count: 128)
            if nc_clip_move_to_track(handle, newId, Int32(targetTrackId), startSeconds, &out, out.count) {
                selectClip(String(cString: out))
            }
        } else {
            _ = nc_clip_move(handle, newId, startSeconds)
            selectClip(newId)
        }
        reloadClips()
        refreshHistory()
    }

    /// Option-drag copy dropped past the last lane: duplicate onto a fresh audio track.
    func dropClipCopyToNewTrack(_ clipId: String, startSeconds: Double) {
        guard let handle else { return }
        var buffer = [CChar](repeating: 0, count: 128)
        guard nc_clip_duplicate(handle, clipId, &buffer, buffer.count) else { return }
        let newId = String(cString: buffer)
        let newIndex = nc_track_add_audio(handle)
        guard newIndex >= 0 else { reloadClips(); refreshHistory(); return }
        reloadTracks()
        var out = [CChar](repeating: 0, count: 128)
        if nc_clip_move_to_track(handle, newId, newIndex, startSeconds, &out, out.count) {
            selectClip(String(cString: out))
        }
        reloadClips()
        refreshHistory()
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

    /// Continuous, for dragging clip gain. Sets the field without a graph rebuild so the
    /// drag stays smooth; the waveform redraws from the new gain. Commit reconciles.
    func setClipGain(_ clipId: String, _ gainDb: Float) {
        guard let handle else { return }
        if nc_clip_set_gain_db_preview(handle, clipId, gainDb) { reloadClips() }
    }

    /// Drag-end: reconcile the previewed clip gain into the engine and record one step.
    func commitClipGain(_ clipId: String) {
        guard let handle, let clip = clips.first(where: { $0.id == clipId }) else { return }
        if nc_clip_set_gain_db(handle, clipId, clip.gainDb) {
            reloadClips()
            recordGesture("Clip gain")
        }
    }

    /// Lane indices address `laneTracks`, not `tracks` — Master and Monitor are not lanes.
    /// Click a lane to select its track; ⇧-click adds/removes it from the selection so a
    /// bottom-edge drag resizes every selected lane's height together.
    func selectLane(_ laneIndex: Int, additive: Bool = false) {
        guard laneIndex < laneTracks.count else { return }
        let id = laneTracks[laneIndex].id
        if additive {
            if selectedMixerTrackIds.contains(id) { selectedMixerTrackIds.remove(id) }
            else { selectedMixerTrackIds.insert(id) }
        } else {
            selectedMixerTrackIds = [id]
        }
        selectedTrackId = id
        focusTrackForDeletion()   // last click wins: the track is now the Delete target
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
        // Overlapping the destination lane's clips crossfades them, same as a same-lane move.
        applyCrossfadesForSelection()
        reloadClips()
        refreshHistory()
    }

    /// Dropping a clip (or its option-drag copy) past the last lane makes a fresh audio
    /// track and lands the clip there.
    func dropClipToNewTrack(_ clipId: String, startSeconds: Double) {
        guard let handle else { return }
        let newIndex = nc_track_add_audio(handle)
        guard newIndex >= 0 else { return }
        reloadTracks()
        var buffer = [CChar](repeating: 0, count: 128)
        guard nc_clip_move_to_track(handle, clipId, newIndex, startSeconds,
                                    &buffer, buffer.count) else {
            reloadClips()
            refreshHistory()
            return
        }
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

    /// The track the duplicate-options sheet is open for (an engine track index).
    @Published var duplicateTrackTarget: Int?

    /// Duplicates a track with all its settings; the sheet lets the user drop clips,
    /// inserts or sends from the copy.
    func duplicateTrack(_ trackId: Int, includeClips: Bool, includeInserts: Bool, includeSends: Bool) {
        guard let handle else { return }
        let newIndex = nc_track_duplicate(handle, Int32(trackId), includeClips, includeInserts, includeSends)
        duplicateTrackTarget = nil
        guard newIndex >= 0 else { return }
        reloadTracks()
        reloadClips()
        refreshHistory()
        selectedTrackId = Int(newIndex)
        selectedMixerTrackIds = [Int(newIndex)]
    }

    /// Reorder a mixer channel: drop `sourceId` before/after `targetId`.
    func moveTrackNear(_ sourceId: Int, targetId: Int, after: Bool) {
        guard let handle, sourceId != targetId,
              let source = tracks.first(where: { $0.id == sourceId }),
              let target = tracks.first(where: { $0.id == targetId }),
              source.kind != .master, target.kind != .master else { return }
        let moved = source.name.withCString { s in
            target.name.withCString { t in nc_track_move_near(handle, s, t, after) }
        }
        guard moved else { return }
        reloadTracks()
        reloadClips()
        refreshHistory()
        selectedTrackId = tracks.first(where: { $0.name == source.name })?.id ?? selectedTrackId
        if let id = selectedTrackId { selectedMixerTrackIds = [id] }
    }

    func addInstrumentTrack() {
        guard let handle, nc_track_add_instrument(handle) >= 0 else { return }
        reloadTracks()
        refreshHistory()
    }

    /// A pure MIDI track — MIDI regions with no instrument slot (a part is silent until
    /// routed to an instrument track). The engine already had nc_track_add_midi.
    func addMidiTrack() {
        guard let handle, nc_track_add_midi(handle) >= 0 else { return }
        reloadTracks()
        refreshHistory()
    }

    /// Deleting takes the clips with it, so ask first when the track has any.
    func deleteSelectedTrack() {
        guard let handle, let trackId = selectedTrackId,
              let track = tracks.first(where: { $0.id == trackId }) else { return }

        // Ask first when the track carries anything — clips, MIDI, an instrument, inserts or
        // sends — so a Delete-key press never silently discards work. An empty track deletes
        // straight away.
        let clipsOnTrack = clips.filter { $0.trackName == track.name }
        let regionsOnTrack = midiRegions.filter { $0.trackName == track.name }
        let hasInstrument = !track.instrumentName.isEmpty && track.instrumentName != "No Instrument"
        let realInserts = track.inserts.filter { !$0.isEmpty }
        var parts: [String] = []
        if !clipsOnTrack.isEmpty { parts.append("클립 \(clipsOnTrack.count)개") }
        if !regionsOnTrack.isEmpty { parts.append("MIDI 리전 \(regionsOnTrack.count)개") }
        if hasInstrument { parts.append("악기 \(track.instrumentName)") }
        if !realInserts.isEmpty { parts.append("인서트 \(realInserts.count)개") }
        if !track.sends.isEmpty { parts.append("센드 \(track.sends.count)개") }
        if !parts.isEmpty {
            let alert = NSAlert()
            alert.messageText = "\(track.name) 트랙을 삭제할까요?"
            alert.informativeText = "이 트랙의 " + parts.joined(separator: ", ") + "도 함께 삭제됩니다."
            alert.addButton(withTitle: "삭제")
            alert.addButton(withTitle: "취소")
            guard alert.runModal() == .alertFirstButtonReturn else { return }
        }

        guard nc_track_delete(handle, Int32(trackId), true) else {
            lastError = "이 트랙은 삭제할 수 없습니다"
            return
        }
        selectedTrackId = nil
        selectedMixerTrackIds = []
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
        // A single-clip move honours the edit mode: Shuffle ripples the neighbours,
        // Spot opens the exact-time sheet instead of committing the free position.
        if stepName == "Move clip", let clip = clips.first(where: { selectedClipIds.contains($0.id) }) {
            switch editMode {
            case .shuffle:
                commitShuffleMove()
                return
            case .spot:
                spotTargetClipId = clip.id
                return
            case .grid, .slip:
                break
            }
        }
        // Overlapping a clip onto a same-track neighbour becomes a crossfade, folded into
        // this one move step (Pro Tools' auto-crossfade on overlap). Applies to a multi-clip
        // drag ("Move clips") too, not just a single clip, so dragging two selected clips to
        // overlap crossfades them.
        if stepName == "Move clip" || stepName == "Move clips" { applyCrossfadesForSelection() }
        recordGesture(stepName)
    }

    /// Auto-crossfade any same-track overlaps created by the moved selection. Reloads once;
    /// records no step of its own so it merges into the caller's gesture.
    private func applyCrossfadesForSelection() {
        guard let handle else { return }
        var changed = false
        for id in selectedClipIds where nc_clip_apply_crossfades(handle, id) { changed = true }
        if changed { reloadClips() }
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

    /// Heal (re-join) split clips: glue adjacent same-source clips across the selected
    /// clips' span, or the edit range if nothing is selected.
    func healSelectedClips() {
        guard let handle else { return }
        let selected = clips.filter { selectedClipIds.contains($0.id) }
        let start: Double
        let end: Double
        if selected.isEmpty {
            guard hasEditRange else { return }
            start = loopStartSeconds
            end = loopEndSeconds
        } else {
            start = selected.map { $0.startSeconds }.min() ?? 0
            end = selected.map { $0.startSeconds + $0.durationSeconds }.max() ?? 0
        }
        if nc_clip_glue_range(handle, start, end) > 0 {
            reloadClips()
            refreshHistory()
        }
    }

    func deleteSelectedClips() {
        guard let handle else { return }
        // Shuffle mode ripples: deleting closes the gap and pulls later clips left.
        if editMode == .shuffle, !selection.isEmpty {
            let sel = clips.filter { selectedClipIds.contains($0.id) }
            let start = sel.map { $0.startSeconds }.min() ?? 0
            let end = sel.map { $0.startSeconds + $0.durationSeconds }.max() ?? 0
            if nc_clip_shuffle_delete_range(handle, start, end) > 0 {
                selectedClipIds = []
                reloadClips()
                refreshHistory()
                return
            }
        }
        guard let deleted = withClipIds(selection, { nc_clip_delete_many(handle, $0, $1) }), deleted > 0 else { return }
        selectedClipIds = []
        reloadClips()
        refreshHistory()
    }

    /// Commit a Shuffle-mode drag: after the free preview move, re-lay the clip against
    /// its neighbours so they ripple. Single-clip only — matches Pro Tools shuffle drag.
    func commitShuffleMove() {
        guard let handle, editMode == .shuffle, selectedClipIds.count == 1,
              let clip = clips.first(where: { selectedClipIds.contains($0.id) }) else { return }
        _ = nc_clip_shuffle_move(handle, clip.id, clip.startSeconds)
        reloadClips()
        refreshHistory()
    }

    /// Spot mode: place a clip at an exact typed time. Records its own step.
    func spotPlaceClip(_ clipId: String, at seconds: Double) {
        guard let handle else { return }
        if nc_clip_move(handle, clipId, max(0, seconds)) {
            reloadClips()
            recordGesture("Spot clip")
        }
        spotTargetClipId = nil
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
        if regionId != nil { selectedClipIds = []; selectedConductor = nil }
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
        reloadConductor()
    }

    // MARK: - Conductor / global track (tempo, chords, lyrics)

    struct ConductorEvent: Equatable, Identifiable {
        let id: Int
        let timeSeconds: Double
        let label: String
    }

    /// A single selected conductor event, keyed by its time (every delete is time+tolerance).
    /// Mutually exclusive with the clip and MIDI-region selections so one Delete key has one
    /// target.
    enum ConductorSelection: Equatable {
        case songform(Double), marker(Double), tempo(Double)
        case meter(Double), key(Double), chord(Double), lyric(Double)
    }
    @Published var selectedConductor: ConductorSelection?

    func selectConductor(_ selection: ConductorSelection?) {
        selectedConductor = selection
        if selection != nil {
            selectedClipIds = []
            selectedRegionId = nil
        }
    }

    /// Delete the selected conductor event via its type's existing time-keyed delete.
    func deleteSelectedConductor() {
        guard let selection = selectedConductor else { return }
        switch selection {
        case .songform(let t): deleteSongSection(at: t)
        case .marker(let t):   deleteMarker(at: t)
        case .tempo(let t):    deleteTempoMarker(at: t)
        case .meter(let t):    deleteMeter(at: t)
        case .key(let t):      deleteKey(at: t)
        case .chord(let t):    deleteChord(at: t)
        case .lyric(let t):    deleteLyric(at: t)
        }
        selectedConductor = nil
    }

    @Published private(set) var chords: [ConductorEvent] = []
    @Published private(set) var lyrics: [ConductorEvent] = []
    @Published private(set) var tempoMarkers: [ConductorEvent] = []
    /// Positional time-signature (meter) changes, backed by the project's timeSignatureMap.
    @Published private(set) var meterEvents: [ConductorEvent] = []
    /// Positional key changes. The engine has no key field, so — like `musicalKey` — these
    /// live app-side and are persisted with the settings (label is e.g. "Am", "F#").
    @Published private(set) var keyEvents: [ConductorEvent] = []
    /// Musical key shown in the conductor bar. Engine has no key field yet, so this is
    /// app-level (saved with the settings), display context for now.
    @Published var musicalKey: String = "C"

    func addMarker(at seconds: Double, name: String) {
        guard let handle else { return }
        var buffer = [CChar](repeating: 0, count: 128)
        if nc_marker_add(handle, seconds, &buffer, buffer.count) {
            if !name.isEmpty { nc_marker_rename(handle, seconds, markerTolerance, name) }
            reloadMarkers()
            refreshHistory()
        }
    }

    private func reloadConductor() {
        guard let handle else { return }
        chords = (0..<Int(nc_chord_count(handle))).map { i in
            ConductorEvent(id: i, timeSeconds: nc_chord_time(handle, Int32(i)),
                           label: readEngineString { nc_chord_name(handle, Int32(i), $0, $1) })
        }
        lyrics = (0..<Int(nc_lyric_count(handle))).map { i in
            ConductorEvent(id: i, timeSeconds: nc_lyric_time(handle, Int32(i)),
                           label: readEngineString { nc_lyric_text(handle, Int32(i), $0, $1) })
        }
        tempoMarkers = (0..<Int(nc_tempo_marker_count(handle))).map { i in
            ConductorEvent(id: i, timeSeconds: nc_tempo_marker_time(handle, Int32(i)),
                           label: String(format: "%.0f", nc_tempo_marker_bpm(handle, Int32(i))))
        }
        meterEvents = (0..<Int(nc_time_sig_count(handle))).map { i in
            ConductorEvent(id: i, timeSeconds: nc_time_sig_time(handle, Int32(i)),
                           label: "\(nc_time_sig_numerator(handle, Int32(i)))/\(nc_time_sig_denominator(handle, Int32(i)))")
        }
    }

    func addChord(at seconds: Double, name: String) {
        guard let handle else { return }
        if nc_chord_add(handle, seconds, Self.normalizeChordName(name)) { reloadConductor(); refreshHistory() }
    }

    /// Normalise a typed chord so the root reads as an uppercase note — "dm7" → "Dm7",
    /// "f#maj7" → "F#maj7", "bb" → "Bb". Only the leading note letter is capitalised; the
    /// quality (m, maj, sus, add…) is left as typed, so "Dm7" and "DM7" both survive.
    static func normalizeChordName(_ raw: String) -> String {
        let s = raw.trimmingCharacters(in: .whitespaces)
        guard let first = s.first, ("a"..."g").contains(Character(first.lowercased())) else { return s }
        return first.uppercased() + s.dropFirst()
    }
    func deleteChord(at seconds: Double) {
        guard let handle else { return }
        if nc_chord_delete(handle, seconds, markerTolerance) { reloadConductor(); refreshHistory() }
    }
    func addLyric(at seconds: Double, text: String) {
        guard let handle else { return }
        if nc_lyric_add(handle, seconds, text) { reloadConductor(); refreshHistory() }
    }
    func deleteLyric(at seconds: Double) {
        guard let handle else { return }
        if nc_lyric_delete(handle, seconds, markerTolerance) { reloadConductor(); refreshHistory() }
    }
    func addTempoMarker(at seconds: Double, bpm: Double) {
        guard let handle else { return }
        if nc_tempo_marker_add(handle, seconds, bpm) { reloadConductor(); refreshHistory() }
    }
    func deleteTempoMarker(at seconds: Double) {
        guard let handle else { return }
        if nc_tempo_marker_delete(handle, seconds, markerTolerance) { reloadConductor(); refreshHistory() }
    }
    func moveChord(from: Double, to: Double) {
        guard let handle else { return }
        if nc_chord_move(handle, from, markerTolerance, snap(to)) { reloadConductor() }
    }
    func moveLyric(from: Double, to: Double) {
        guard let handle else { return }
        if nc_lyric_move(handle, from, markerTolerance, snap(to)) { reloadConductor() }
    }

    /// Add / change a time-signature. `text` is "num/den" (e.g. "3/4"); a bare number is
    /// taken as the numerator over 4.
    func addMeter(at seconds: Double, text: String) {
        guard let handle else { return }
        let parts = text.split(separator: "/")
        let num = Int(parts.first.map(String.init) ?? "") ?? timeSignature.numerator
        let den = parts.count > 1 ? (Int(parts[1]) ?? timeSignature.denominator) : timeSignature.denominator
        if nc_time_sig_add(handle, seconds, Int32(max(1, num)), Int32(max(1, den))) {
            reloadConductor(); refreshHistory()
            // Keep the summary field in sync when the change lands at the very start.
            if seconds < 0.01 { timeSignature = (num, den) }
        }
    }
    func deleteMeter(at seconds: Double) {
        guard let handle else { return }
        if nc_time_sig_delete(handle, seconds, markerTolerance) { reloadConductor(); refreshHistory() }
    }
    func moveMeter(from: Double, to: Double) {
        guard let handle else { return }
        if nc_time_sig_move(handle, from, markerTolerance, snap(to)) { reloadConductor() }
    }

    func moveTempoMarker(from: Double, to: Double) {
        guard let handle else { return }
        if nc_tempo_marker_move(handle, from, markerTolerance, snap(to)) { reloadConductor() }
    }

    // Positional key changes (app-side, persisted with settings).
    private var keyEventStore: [(seconds: Double, label: String)] = [] { didSet { publishKeyEvents() }}
    private func publishKeyEvents() {
        keyEvents = keyEventStore.sorted { $0.seconds < $1.seconds }.enumerated().map {
            ConductorEvent(id: $0.offset, timeSeconds: $0.element.seconds, label: $0.element.label)
        }
    }
    func addKey(at seconds: Double, name: String) {
        let label = name.isEmpty ? musicalKey : name
        keyEventStore.removeAll { abs($0.seconds - seconds) < 0.05 }
        keyEventStore.append((seconds, label))
        if seconds < 0.01 { musicalKey = label }
        persistKeyEvents()
    }
    func deleteKey(at seconds: Double) {
        keyEventStore.removeAll { abs($0.seconds - seconds) <= markerTolerance }
        persistKeyEvents()
    }
    func moveKey(from: Double, to: Double) {
        guard let i = keyEventStore.firstIndex(where: { abs($0.seconds - from) <= markerTolerance }) else { return }
        keyEventStore[i].seconds = max(0, snap(to))
        persistKeyEvents()
    }
    private func persistKeyEvents() {
        let encoded = keyEventStore.map { "\($0.seconds):\($0.label)" }.joined(separator: "|")
        UserDefaults.standard.set(encoded, forKey: SettingsKey.keyEvents)
    }
    private func restoreKeyEvents() {
        guard let s = UserDefaults.standard.string(forKey: SettingsKey.keyEvents), !s.isEmpty else { return }
        keyEventStore = s.split(separator: "|").compactMap { pair in
            let kv = pair.split(separator: ":", maxSplits: 1)
            guard kv.count == 2, let t = Double(kv[0]) else { return nil }
            return (t, String(kv[1]))
        }
    }

    // Song-form / arrangement sections (Intro, Verse, Chorus…). Each event starts a section
    // that runs until the next one; app-side and persisted with the settings.
    @Published private(set) var songForm: [ConductorEvent] = []
    private var songFormStore: [(seconds: Double, label: String)] = [] { didSet { publishSongForm() }}
    private func publishSongForm() {
        songForm = songFormStore.sorted { $0.seconds < $1.seconds }.enumerated().map {
            ConductorEvent(id: $0.offset, timeSeconds: $0.element.seconds, label: $0.element.label)
        }
    }
    func addSongSection(at seconds: Double, name: String) {
        let label = name.trimmingCharacters(in: .whitespaces)
        songFormStore.removeAll { abs($0.seconds - seconds) < 0.05 }
        songFormStore.append((seconds, label.isEmpty ? "Section" : label))
        persistSongForm()
    }
    func deleteSongSection(at seconds: Double) {
        songFormStore.removeAll { abs($0.seconds - seconds) <= markerTolerance }
        persistSongForm()
    }
    func moveSongSection(from: Double, to: Double) {
        guard let i = songFormStore.firstIndex(where: { abs($0.seconds - from) <= markerTolerance }) else { return }
        songFormStore[i].seconds = max(0, snap(to))
        persistSongForm()
    }
    private func persistSongForm() {
        let encoded = songFormStore.map { "\($0.seconds):\($0.label)" }.joined(separator: "|")
        UserDefaults.standard.set(encoded, forKey: SettingsKey.songForm)
    }
    private func restoreSongForm() {
        guard let s = UserDefaults.standard.string(forKey: SettingsKey.songForm), !s.isEmpty else { return }
        songFormStore = s.split(separator: "|").compactMap { pair in
            let kv = pair.split(separator: ":", maxSplits: 1)
            guard kv.count == 2, let t = Double(kv[0]) else { return nil }
            return (t, String(kv[1]))
        }
    }

    /// A stable colour for a song-form section, keyed off common section names.
    static func songSectionColor(_ label: String) -> Color {
        switch label.lowercased() {
        case let l where l.contains("intro"):   return Color(hex: 0x5f9fd6)
        case let l where l.contains("verse"):   return Color(hex: 0x5fb85f)
        case let l where l.contains("pre"):      return Color(hex: 0x8fbf5f)
        case let l where l.contains("chorus") || l.contains("훅") || l.contains("hook"): return Color(hex: 0xe6a23c)
        case let l where l.contains("bridge"):   return Color(hex: 0xb79cf0)
        case let l where l.contains("solo"):     return Color(hex: 0xe0607a)
        case let l where l.contains("outro") || l.contains("end"): return Color(hex: 0x8a8f98)
        case let l where l.contains("drop"):     return Color(hex: 0xe0517a)
        default:
            // Deterministic hue from the label so custom names still get a stable colour.
            let h = Double(abs(label.hashValue) % 360) / 360.0
            return Color(hue: h, saturation: 0.55, brightness: 0.78)
        }
    }

    /// Seconds per bar at the project tempo/meter, for the conductor bar's grid.
    var secondsPerBar: Double {
        let bpm = Double(max(1, tempoBpm))
        let beats = Double(max(1, timeSignature.0))
        return 60.0 / bpm * beats
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
    /// A target the timeline can automate: the track's volume or pan, or one of its
    /// insert plug-in's parameters (id "insert.<slot>.<paramId>").
    struct AutomationParameter: Equatable, Hashable {
        let id: String
        let displayName: String
        let range: ClosedRange<Float>
        var pluginFallback: Float? = nil

        var isVolume: Bool { id == "track.volume" }
        var isPan: Bool { id == "track.pan" }
        var isPlugin: Bool { id.hasPrefix("insert.") }

        static let volume = AutomationParameter(id: "track.volume", displayName: "볼륨 (dB)", range: -60...12)
        static let pan = AutomationParameter(id: "track.pan", displayName: "팬", range: -1...1)
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

    /// Volume, pan, then every parameter of the track's loaded inserts — the picker list.
    func automationParameterOptions(laneIndex: Int) -> [AutomationParameter] {
        guard laneIndex < laneTracks.count else { return [.volume, .pan] }
        return [.volume, .pan] + insertAutomationParameters(trackId: laneTracks[laneIndex].id)
    }

    func insertAutomationParameters(trackId: Int) -> [AutomationParameter] {
        guard let handle else { return [] }
        var out: [AutomationParameter] = []
        for slot in 0..<5 {
            let count = Int(nc_track_insert_param_count(handle, Int32(trackId), Int32(slot)))
            guard count > 0 else { continue }
            for p in 0..<count {
                let pid = nc_track_insert_param_id(handle, Int32(trackId), Int32(slot), Int32(p))
                let name = readEngineString { nc_track_insert_param_name(handle, Int32(trackId), Int32(slot), Int32(p), $0, $1) }
                let val = Float(nc_track_insert_param_value(handle, Int32(trackId), Int32(slot), Int32(p)))
                out.append(AutomationParameter(id: "insert.\(slot).\(pid)",
                                               displayName: "\(["A","B","C","D","E"][slot]): \(name)",
                                               range: 0...1, pluginFallback: val))
            }
        }
        return out
    }

    func setAutomationParameter(laneIndex: Int, id: String) {
        guard laneIndex < laneTracks.count else { return }
        let options = automationParameterOptions(laneIndex: laneIndex)
        guard let picked = options.first(where: { $0.id == id }) else { return }
        automationLanes[laneTracks[laneIndex].id] = picked
    }

    func cycleAutomationParameter(laneIndex: Int) {
        guard laneIndex < laneTracks.count,
              let current = automationLanes[laneTracks[laneIndex].id] else { return }
        let all = automationParameterOptions(laneIndex: laneIndex)
        guard let i = all.firstIndex(where: { $0.id == current.id }) else { return }
        automationLanes[laneTracks[laneIndex].id] = all[(i + 1) % all.count]
    }

    private func automationPoints(trackId: Int, _ parameter: AutomationParameter)
        -> [TimelineModel.Automation.Point] {
        guard let handle else { return [] }
        let count = Int(nc_track_automation_count(handle, Int32(trackId), parameter.id))
        return (0..<count).map { index in
            TimelineModel.Automation.Point(
                timeSeconds: nc_track_automation_time(handle, Int32(trackId), parameter.id, Int32(index)),
                value: nc_track_automation_value(handle, Int32(trackId), parameter.id, Int32(index)))
        }
    }

    func addAutomationPoint(laneIndex: Int, timeSeconds: Double, value: Float) {
        guard let handle, laneIndex < laneTracks.count else { return }
        let trackId = laneTracks[laneIndex].id
        guard let parameter = automationLanes[trackId],
              nc_track_automation_add(handle, Int32(trackId), parameter.id, timeSeconds, value)
        else { return }
        reloadTracks()
        refreshHistory()
    }

    /// Continuous; the view commits the gesture when the drag ends.
    func moveAutomationPoint(laneIndex: Int, pointIndex: Int, timeSeconds: Double, value: Float) {
        guard let handle, laneIndex < laneTracks.count else { return }
        let trackId = laneTracks[laneIndex].id
        guard let parameter = automationLanes[trackId],
              nc_track_automation_move(handle, Int32(trackId), parameter.id,
                                       Int32(pointIndex), timeSeconds, value)
        else { return }
        reloadTracks()
    }

    func deleteAutomationPoint(laneIndex: Int, pointIndex: Int) {
        guard let handle, laneIndex < laneTracks.count else { return }
        let trackId = laneTracks[laneIndex].id
        guard let parameter = automationLanes[trackId],
              nc_track_automation_delete(handle, Int32(trackId), parameter.id, Int32(pointIndex))
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
            let channels = max(1, Int(nc_waveform_channel_count(handle, clip.sourcePath)))

            var minsL = [Float](repeating: 0, count: count)
            var maxsL = [Float](repeating: 0, count: count)
            guard nc_waveform_channel_peaks(handle, clip.sourcePath, 0, &minsL, &maxsL, Int32(count)) else { continue }

            var minsR: [Float] = []
            var maxsR: [Float] = []
            if channels > 1 {
                var r0 = [Float](repeating: 0, count: count)
                var r1 = [Float](repeating: 0, count: count)
                if nc_waveform_channel_peaks(handle, clip.sourcePath, 1, &r0, &r1, Int32(count)) {
                    minsR = r0
                    maxsR = r1
                }
            }
            waveforms[clip.sourcePath] = WaveformData(
                channels: minsR.isEmpty ? 1 : channels,
                minsL: minsL, maxsL: maxsL, minsR: minsR, maxsR: maxsR,
                durationSeconds: nc_waveform_duration_seconds(handle, clip.sourcePath))
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
                fadeInCurve: readEngineString { nc_clip_fade_in_curve(handle, i, $0, $1) },
                fadeOutCurve: readEngineString { nc_clip_fade_out_curve(handle, i, $0, $1) },
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

    // MARK: Track I/O routing

    /// Where a track can send its output: Master plus any aux/bus tracks.
    func outputBusOptions(_ id: Int) -> [String] {
        guard let handle else { return ["Master"] }
        return (0..<Int(nc_track_output_option_count(handle, Int32(id)))).map { i in
            readString { nc_track_output_option(handle, Int32(id), Int32(i), $0, $1) }
        }
    }

    func setTrackOutputBus(_ id: Int, _ bus: String) {
        guard let handle else { return }
        _ = bus.withCString { nc_track_set_output_bus(handle, Int32(id), $0) }
        reloadTracks()
        refreshHistory()
    }

    /// The channel memo. Records one undo step per commit (call on end-editing, not per
    /// keystroke); the memo has no audio effect, so no reconcile.
    func setTrackNotes(_ id: Int, _ notes: String) {
        guard let handle else { return }
        _ = notes.withCString { nc_track_set_notes(handle, Int32(id), $0) }
        reloadTracks()
        refreshHistory()
    }

    /// Audio-track input options — a few hardware input pairs.
    func audioInputOptions() -> [String] {
        ["Input 1-2", "Input 3-4", "Input 5-6", "Input 7-8"]
    }

    func setTrackInputBus(_ id: Int, _ bus: String) {
        guard let handle else { return }
        _ = bus.withCString { nc_track_set_input_bus(handle, Int32(id), $0) }
        reloadTracks()
        refreshHistory()
    }

    /// Point an instrument/MIDI track's input at a MIDI source: open it for live input
    /// and label the strip with it, so "MIDI Input" actually selects the keyboard.
    func setTrackMidiSource(_ id: Int, sourceId: String, label: String) {
        startLiveMidi(sourceId)
        setTrackInputBus(id, label)
    }

    // MARK: Track sends

    /// Make a new aux/bus track that sends can target.
    @discardableResult
    func addAuxTrack() -> Bool {
        guard let handle, nc_track_add_aux(handle) >= 0 else { return false }
        reloadTracks()
        refreshHistory()
        return true
    }

    /// The aux/bus tracks this track may send to.
    func sendBusOptions(_ id: Int) -> [String] {
        guard let handle else { return [] }
        return (0..<Int(nc_track_send_option_count(handle, Int32(id)))).map { i in
            readString { nc_track_send_option(handle, Int32(id), Int32(i), $0, $1) }
        }
    }

    func addSend(_ id: Int, to bus: String) {
        guard let handle else { return }
        _ = bus.withCString { nc_track_add_send(handle, Int32(id), $0) }
        reloadTracks()
        refreshHistory()
    }

    func setSendGain(_ id: Int, slot: Int, gainDb: Float) {
        guard let handle else { return }
        nc_track_set_send_gain_db(handle, Int32(id), Int32(slot), gainDb)
        reloadTracks()
        refreshHistory()
    }

    func setSendPan(_ id: Int, slot: Int, pan: Float) {
        guard let handle else { return }
        nc_track_set_send_pan(handle, Int32(id), Int32(slot), pan)
        reloadTracks()
        refreshHistory()
    }

    func setSendPreFader(_ id: Int, slot: Int, pre: Bool) {
        guard let handle else { return }
        nc_track_set_send_pre_fader(handle, Int32(id), Int32(slot), pre)
        reloadTracks()
        refreshHistory()
    }

    func removeSend(_ id: Int, slot: Int) {
        guard let handle else { return }
        nc_track_remove_send(handle, Int32(id), Int32(slot))
        reloadTracks()
        refreshHistory()
    }

    /// True while any track is soloed, so the others can show they are being held down.
    var anyTrackSoloed: Bool { tracks.contains { $0.solo } }

    /// A slow on/off phase for the solo-implied blink on silenced tracks, toggled from
    /// the poll tick so every strip pulses in sync without its own animation timer.
    @Published private(set) var soloBlinkOn = false

    /// A faster on/off phase for the clip warning: a channel value in the red pulses so a
    /// momentary overload is caught. Toggled from the poll tick only while something is
    /// actually clipping, so idle strips do not repaint.
    @Published private(set) var clipBlinkOn = false

    /// Solo is additive here, the way the engine models it — several tracks can be
    /// soloed at once, and Master/Monitor refuse it.
    /// Solo select behaviour set from the Solo button's right-click menu. Additive lets
    /// solos stack; Exclusive clears the others when you solo a track (Pro Tools X-OR).
    enum SoloSelectMode: String { case additive, exclusive }
    @Published var soloSelectMode: SoloSelectMode = .additive
    /// Solo monitor mode. SIP (solo-in-place) is the engine's behaviour today; AFL/PFL
    /// need a solo bus and are stored until that engine work lands.
    enum SoloMonitorMode: String { case sip, afl, pfl }
    @Published var soloMonitorMode: SoloMonitorMode = .sip

    func toggleTrackSolo(_ id: Int) {
        guard let handle, let track = tracks.first(where: { $0.id == id }) else { return }
        let willSolo = !track.solo
        // Exclusive: soloing a track clears every other track's solo first.
        if soloSelectMode == .exclusive && willSolo {
            for other in tracks where other.id != id && other.solo {
                nc_track_set_solo(handle, Int32(other.id), false)
            }
            reloadTracks()
        }
        nc_track_set_solo(handle, Int32(id), willSolo)
        syncTrack(id)
        refreshHistory()
    }

    func clearAllSolos() {
        guard let handle else { return }
        var changed = false
        for track in tracks where track.solo {
            nc_track_set_solo(handle, Int32(track.id), false)
            changed = true
        }
        if changed { reloadTracks(); refreshHistory() }
    }

    func setSoloMonitorMode(_ mode: SoloMonitorMode) { soloMonitorMode = mode }

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
            // Ballistic, so a track meter falls to silence on stop instead of freezing.
            tracks[index].peakLeft = max(left, tracks[index].peakLeft * Self.meterDecay)
            tracks[index].peakRight = max(right, tracks[index].peakRight * Self.meterDecay)
        }
    }

    /// Per-tick meter release. At ~30 Hz this falls a held peak to silence in ~0.2 s.
    private static let meterDecay: Float = 0.80

    /// The analyzer type the small dock widget opens on click (right-click changes it).
    @Published var dockAnalyzerKind: AnalyzerKind = .spectrum

    /// One-click on the small spectrum widget opens a large, movable analyzer window.
    func openAnalyzerWindow() {
        AnalyzerWindowManager.shared.open(kind: dockAnalyzerKind, engine: self)
    }

    private var spectrumScratch = [Float](repeating: 0, count: 1024)
    /// Reads the FFT bins (cached by the status poll) and smooths them so the analyzer
    /// rises fast and falls gently, the way a hardware analyzer does.
    private func updateSpectrumBins(_ handle: OpaquePointer) {
        let count = Int(nc_spectrum_bin_count(handle))
        guard count > 0 else {
            if !spectrumBins.isEmpty { spectrumBins = [] }
            return
        }
        if spectrumScratch.count < count { spectrumScratch = [Float](repeating: 0, count: count) }
        _ = spectrumScratch.withUnsafeMutableBufferPointer {
            nc_spectrum_bins(handle, $0.baseAddress, Int32(count))
        }
        if spectrumBins.count != count {
            spectrumBins = Array(spectrumScratch[0..<count])
            return
        }
        for i in 0..<count {
            let incoming = spectrumScratch[i]
            // Fast attack, slow release.
            spectrumBins[i] = incoming > spectrumBins[i]
                ? incoming
                : spectrumBins[i] * 0.72 + incoming * 0.28
        }
    }

    private var goniometerScratch = [Float](repeating: 0, count: 1024)
    private func updateGoniometer(_ handle: OpaquePointer) {
        let count = Int(nc_goniometer_sample_count(handle))
        guard count > 0 else {
            if !goniometerSamples.isEmpty { goniometerSamples = [] }
            return
        }
        if goniometerScratch.count < count { goniometerScratch = [Float](repeating: 0, count: count) }
        _ = goniometerScratch.withUnsafeMutableBufferPointer {
            nc_goniometer_samples(handle, $0.baseAddress, Int32(count))
        }
        goniometerSamples = Array(goniometerScratch[0..<count])
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

        let names = ["니어필드", "미드필드", "라지필드"]
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
        physicalSpeakerModel = readString { nc_monitor_physical_speaker_model(handle, $0, $1) }
        physicalHeadphoneModel = readString { nc_monitor_physical_headphone_model(handle, $0, $1) }
        monitorOutputExclusive = nc_monitor_output_exclusive(handle)
        autoFadeOutSeconds = nc_master_auto_fade_seconds(handle)
        autoFadeOutCurve = readString { nc_master_auto_fade_curve(handle, $0, $1) }
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

    /// Toggle plugin delay compensation. The engine reads the flag at start(), so this
    /// restarts the audio engine — a brief dropout, like a buffer-size change.
    func setDelayCompensation(_ enabled: Bool) {
        guard let handle else { return }
        nc_delay_compensation_set(handle, enabled)
        delayCompensationEnabled = nc_delay_compensation_enabled(handle)
    }

    func setDspCoreCount(_ count: Int) {
        guard let handle else { return }
        nc_dsp_set_core_count(handle, Int32(count))
        dspCoreCount = Int(nc_dsp_core_count(handle))
        refreshHistory()
    }

    /// I/O buffer choices, smallest (lowest latency) first. The device clamps to what it
    /// supports; `bufferSize` reflects what was actually granted after the restart.
    static let bufferSizeChoices = [32, 64, 128, 256, 512, 1024]

    /// The requested buffer size (the project value), which may differ from the granted
    /// `bufferSize` if the device clamped it.
    var requestedBufferSize: Int {
        guard let handle else { return bufferSize }
        return Int(nc_buffer_size(handle))
    }

    /// Change the audio I/O buffer size. Restarts the engine (a brief dropout) to apply, then
    /// reads back what the device actually granted. Smaller = lower latency, more CPU load.
    func setBufferSize(_ frames: Int) {
        guard let handle else { return }
        nc_set_buffer_size(handle, Int32(frames))
        // The restart rebuilds status; the granted size lands in `bufferSize` on the next tick.
        UserDefaults.standard.set(Int(nc_buffer_size(handle)), forKey: SettingsKey.bufferSize)
    }

    /// Round-trip-ish latency label for a buffer size at the current sample rate: one buffer
    /// period. (True round-trip is roughly twice this plus device/driver offsets.)
    func bufferLatencyMs(_ frames: Int) -> Double {
        let rate = sampleRate > 0 ? sampleRate : 48000
        return Double(frames) / rate * 1000.0
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
    /// Insert tail on stop: <0 always-on (default), 0 cut, >0 ring out N seconds.
    @Published private(set) var insertTailOnStopSeconds: Double = -1

    func setInsertTailOnStopSeconds(_ seconds: Double) {
        guard let handle else { return }
        nc_set_insert_tail_on_stop_seconds(handle, seconds)
        insertTailOnStopSeconds = nc_insert_tail_on_stop_seconds(handle)
    }

    // MARK: - App settings persistence
    //
    // Project files store the document; these are the app-level choices that otherwise
    // reset every launch (devices, monitor input, DSP path, edit modes, …). Saved to
    // UserDefaults and restored on start.
    private enum SettingsKey {
        static let outputDevice = "nc.outputDeviceId"
        static let inputDevice = "nc.inputDeviceId"
        static let listenSource = "nc.monitorListenSource"
        static let insertTail = "nc.insertTailOnStopSeconds"
        static let monitorPathMode = "nc.monitorPathMode"
        static let outputMode = "nc.outputMode"
        static let editMode = "nc.editMode"
        static let gridUnit = "nc.gridUnit"
        static let soloSelect = "nc.soloSelectMode"
        static let dockAnalyzer = "nc.dockAnalyzerKind"
        static let musicalKey = "nc.musicalKey"
        static let keyEvents = "nc.keyEvents"
        static let songForm = "nc.songForm"
        static let rulerBars = "nc.rulerBars"
        static let rulerTime = "nc.rulerTime"
        static let rulerSamples = "nc.rulerSamples"
        static let laneHeight = "nc.laneHeight"
        static let channelWidth = "nc.channelWidth"
        static let editTool = "nc.editTool"
        static let soloMonitor = "nc.soloMonitorMode"
        static let click = "nc.clickEnabled"
        static let coreIsolation = "nc.coreIsolation"
        static let dspCores = "nc.dspCoreCount"
        static let extDspCores = "nc.externalDspCoreCount"
        static let remoteHost = "nc.remoteDspHost"
        static let monitorExclusive = "nc.monitorOutputExclusive"
        static let physSpeaker = "nc.physicalSpeakerModel"
        static let physHeadphone = "nc.physicalHeadphoneModel"
        static let monitorVol = "nc.monitorVolumeDb"
        static let delayComp = "nc.delayCompensation"
        static let recentProjects = "nc.recentProjects"
        static let stopBehavior = "nc.stopBehavior"
        static let bufferSize = "nc.bufferSize"
        static let saved = "nc.settingsSaved"
    }

    /// Saves every app-level setting as the default for future launches.
    func saveAllSettings() {
        let d = UserDefaults.standard
        d.set(currentOutputDeviceId, forKey: SettingsKey.outputDevice)
        d.set(currentInputDeviceId, forKey: SettingsKey.inputDevice)
        d.set(monitorListenSource, forKey: SettingsKey.listenSource)
        d.set(insertTailOnStopSeconds, forKey: SettingsKey.insertTail)
        d.set(monitorPathMode, forKey: SettingsKey.monitorPathMode)
        d.set(outputMode == .speaker ? "speaker" : "headphone", forKey: SettingsKey.outputMode)
        d.set(editMode.rawValue, forKey: SettingsKey.editMode)
        d.set(gridUnit.rawValue, forKey: SettingsKey.gridUnit)
        d.set(soloSelectMode.rawValue, forKey: SettingsKey.soloSelect)
        d.set(dockAnalyzerKind.rawValue, forKey: SettingsKey.dockAnalyzer)
        d.set(musicalKey, forKey: SettingsKey.musicalKey)
        d.set(editTool.rawValue, forKey: SettingsKey.editTool)
        d.set(soloMonitorMode.rawValue, forKey: SettingsKey.soloMonitor)
        d.set(clickEnabled, forKey: SettingsKey.click)
        d.set(coreIsolationEnabled, forKey: SettingsKey.coreIsolation)
        d.set(dspCoreCount, forKey: SettingsKey.dspCores)
        d.set(externalDspCoreCount, forKey: SettingsKey.extDspCores)
        d.set(remoteDspHost, forKey: SettingsKey.remoteHost)
        d.set(monitorOutputExclusive, forKey: SettingsKey.monitorExclusive)
        d.set(physicalSpeakerModel, forKey: SettingsKey.physSpeaker)
        d.set(physicalHeadphoneModel, forKey: SettingsKey.physHeadphone)
        d.set(Double(monitorVolumeDb), forKey: SettingsKey.monitorVol)
        d.set(delayCompensationEnabled, forKey: SettingsKey.delayComp)
        d.set(stopBehavior.rawValue, forKey: SettingsKey.stopBehavior)
        d.set(requestedBufferSize, forKey: SettingsKey.bufferSize)
        d.set(true, forKey: SettingsKey.saved)
        lastError = "전체 설정을 저장했습니다."
    }

    /// Restores saved app-level settings. Called once at start.
    func restorePersistedSettings() {
        let d = UserDefaults.standard
        loadRecentProjects()   // independent of the saved-settings gate below
        guard d.bool(forKey: SettingsKey.saved) else { return }
        if let out = d.string(forKey: SettingsKey.outputDevice), !out.isEmpty { setOutputDevice(out) }
        if let inp = d.string(forKey: SettingsKey.inputDevice), !inp.isEmpty { setInputDevice(inp) }
        if d.object(forKey: SettingsKey.insertTail) != nil {
            setInsertTailOnStopSeconds(d.double(forKey: SettingsKey.insertTail))
        }
        if let mode = d.string(forKey: SettingsKey.monitorPathMode), !mode.isEmpty {
            setMonitorPathMode(mode)
            dspSources = dspSourcesFromMode(mode)
        }
        if d.object(forKey: SettingsKey.listenSource) != nil {
            setMonitorListenSource(d.bool(forKey: SettingsKey.listenSource))
        }
        if let om = d.string(forKey: SettingsKey.outputMode) {
            outputMode = (om == "headphone") ? .headphone : .speaker
        }
        if let em = d.string(forKey: SettingsKey.editMode), let mode = EditMode(rawValue: em) { editMode = mode }
        if let gu = d.string(forKey: SettingsKey.gridUnit), let unit = GridUnit(rawValue: gu) { gridUnit = unit }
        // Push the snap configuration into the engine so its quantum matches the UI.
        if let handle {
            nc_project_set_edit_mode(handle, editMode.rawValue.capitalized)
            _ = gridUnit.rawValue.withCString { nc_project_set_grid_unit(handle, $0) }
        }
        if let ss = d.string(forKey: SettingsKey.soloSelect), let mode = SoloSelectMode(rawValue: ss) { soloSelectMode = mode }
        if let da = d.string(forKey: SettingsKey.dockAnalyzer), let kind = AnalyzerKind(rawValue: da) { dockAnalyzerKind = kind }
        if let sb = d.string(forKey: SettingsKey.stopBehavior), let mode = StopBehavior(rawValue: sb) { stopBehavior = mode }
        if d.object(forKey: SettingsKey.bufferSize) != nil {
            let bs = d.integer(forKey: SettingsKey.bufferSize)
            if bs > 0, bs != requestedBufferSize { setBufferSize(bs) }
        }
        // Key and its positional changes are per-project, not global — restoring them from
        // settings leaked a stray key event (e.g. a "D") into every new/other project. Start
        // clean: the key row shows the default C at the top until the user adds one.
        musicalKey = "C"
        restoreSongForm()
        if d.object(forKey: SettingsKey.rulerBars) != nil { rulerBars = d.bool(forKey: SettingsKey.rulerBars) }
        if d.object(forKey: SettingsKey.rulerTime) != nil { rulerTime = d.bool(forKey: SettingsKey.rulerTime) }
        if d.object(forKey: SettingsKey.rulerSamples) != nil { rulerSamples = d.bool(forKey: SettingsKey.rulerSamples) }
        if let enc = d.string(forKey: SettingsKey.laneHeight), !enc.isEmpty {
            for pair in enc.split(separator: "|") {
                let kv = pair.split(separator: ":")
                if kv.count == 2, let id = Int(kv[0]), let h = Double(kv[1]) { laneHeights[id] = CGFloat(h) }
            }
        }
        if let enc = d.string(forKey: SettingsKey.channelWidth), !enc.isEmpty {
            for pair in enc.split(separator: "|") {
                let kv = pair.split(separator: ":")
                if kv.count == 2, let id = Int(kv[0]), let w = Double(kv[1]) { channelWidths[id] = CGFloat(w) }
            }
        }
        if let et = d.string(forKey: SettingsKey.editTool), let tool = EditTool(rawValue: et) { editTool = tool }
        if let sm = d.string(forKey: SettingsKey.soloMonitor), let mode = SoloMonitorMode(rawValue: sm) { soloMonitorMode = mode }
        if d.object(forKey: SettingsKey.click) != nil, d.bool(forKey: SettingsKey.click) != clickEnabled { toggleClick() }
        if d.object(forKey: SettingsKey.coreIsolation) != nil { setCoreIsolation(d.bool(forKey: SettingsKey.coreIsolation)) }
        if d.object(forKey: SettingsKey.dspCores) != nil { setDspCoreCount(d.integer(forKey: SettingsKey.dspCores)) }
        if d.object(forKey: SettingsKey.extDspCores) != nil { setExternalDspCoreCount(d.integer(forKey: SettingsKey.extDspCores)) }
        if let host = d.string(forKey: SettingsKey.remoteHost), !host.isEmpty { setRemoteDspHost(host) }
        if d.object(forKey: SettingsKey.monitorExclusive) != nil { setMonitorOutputExclusive(d.bool(forKey: SettingsKey.monitorExclusive)) }
        if let sp = d.string(forKey: SettingsKey.physSpeaker), !sp.isEmpty { setPhysicalSpeakerModel(sp) }
        if let hp = d.string(forKey: SettingsKey.physHeadphone), !hp.isEmpty { setPhysicalHeadphoneModel(hp) }
        if d.object(forKey: SettingsKey.monitorVol) != nil { setMonitorVolume(Float(d.double(forKey: SettingsKey.monitorVol))) }
        if d.object(forKey: SettingsKey.delayComp) != nil { setDelayCompensation(d.bool(forKey: SettingsKey.delayComp)) }
    }

    /// `nc_project_new` resets the project model, so the app-level monitor / DSP / edit
    /// settings the user saved with "전체 설정 저장" revert to the fresh project's defaults.
    /// Re-push the saved ones onto the new project so a new session inherits them (Pro Tools
    /// starts a new session from your I/O + preferences the same way). The output device is
    /// engine-level and survives; conductor/layout UI state lives on the controller and is
    /// left as-is. Restart-causing knobs (core isolation / count) are only re-applied when
    /// they actually differ, so a plain new project does not trigger a needless audio restart.
    func reapplyPersistedSettingsToNewProject() {
        let d = UserDefaults.standard
        guard d.bool(forKey: SettingsKey.saved), handle != nil else { return }
        if d.object(forKey: SettingsKey.insertTail) != nil { setInsertTailOnStopSeconds(d.double(forKey: SettingsKey.insertTail)) }
        if let mode = d.string(forKey: SettingsKey.monitorPathMode), !mode.isEmpty {
            setMonitorPathMode(mode)
            dspSources = dspSourcesFromMode(mode)
        }
        if d.object(forKey: SettingsKey.listenSource) != nil { setMonitorListenSource(d.bool(forKey: SettingsKey.listenSource)) }
        if let om = d.string(forKey: SettingsKey.outputMode) { outputMode = (om == "headphone") ? .headphone : .speaker }
        if let em = d.string(forKey: SettingsKey.editMode), let mode = EditMode(rawValue: em) { editMode = mode }
        if let gu = d.string(forKey: SettingsKey.gridUnit), let unit = GridUnit(rawValue: gu) { gridUnit = unit }
        if let handle {
            nc_project_set_edit_mode(handle, editMode.rawValue.capitalized)
            _ = gridUnit.rawValue.withCString { nc_project_set_grid_unit(handle, $0) }
        }
        if d.object(forKey: SettingsKey.extDspCores) != nil { setExternalDspCoreCount(d.integer(forKey: SettingsKey.extDspCores)) }
        if let host = d.string(forKey: SettingsKey.remoteHost), !host.isEmpty { setRemoteDspHost(host) }
        if d.object(forKey: SettingsKey.monitorExclusive) != nil { setMonitorOutputExclusive(d.bool(forKey: SettingsKey.monitorExclusive)) }
        if let sp = d.string(forKey: SettingsKey.physSpeaker), !sp.isEmpty { setPhysicalSpeakerModel(sp) }
        if let hp = d.string(forKey: SettingsKey.physHeadphone), !hp.isEmpty { setPhysicalHeadphoneModel(hp) }
        if d.object(forKey: SettingsKey.monitorVol) != nil { setMonitorVolume(Float(d.double(forKey: SettingsKey.monitorVol))) }
        if d.object(forKey: SettingsKey.delayComp) != nil { setDelayCompensation(d.bool(forKey: SettingsKey.delayComp)) }
        // These restart the audio engine, so only re-apply when the saved value differs.
        if d.object(forKey: SettingsKey.coreIsolation) != nil {
            let saved = d.bool(forKey: SettingsKey.coreIsolation)
            if saved != coreIsolationEnabled { setCoreIsolation(saved) }
        }
        if d.object(forKey: SettingsKey.dspCores) != nil {
            let saved = d.integer(forKey: SettingsKey.dspCores)
            if saved > 0 && saved != dspCoreCount { setDspCoreCount(saved) }
        }
        if d.object(forKey: SettingsKey.bufferSize) != nil {
            let saved = d.integer(forKey: SettingsKey.bufferSize)
            if saved > 0 && saved != requestedBufferSize { setBufferSize(saved) }
        }
    }

    /// The monitor station's input: the DAW Master, or the BlackHole loopback (the
    /// computer's audio). Two mutually-exclusive choices, like the Monitor DSP app.
    func selectMonitorInput(blackHole: Bool) {
        if blackHole {
            refreshInputDevices()
            if let bh = inputDevices.first(where: { $0.name.lowercased().contains("blackhole") }) {
                setInputDevice(bh.id)
            }
            setMonitorListenSource(true)
        } else {
            setMonitorListenSource(false)
        }
    }

    /// True when a BlackHole loopback device is present to select as the monitor source.
    var hasBlackHoleInput: Bool {
        inputDevices.contains { $0.name.lowercased().contains("blackhole") }
    }

    /// Master vs source monitoring. Source routes the input device (e.g. BlackHole)
    /// through the monitor bus so you hear the computer's audio.
    func setMonitorListenSource(_ on: Bool) {
        guard let handle else { return }
        nc_monitor_set_listen_source(handle, on)
        monitorListenSource = nc_monitor_listen_source(handle)
    }

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
    // These catalogs are static engine data (no handle needed). Computed, not lazy, so a
    // one-time access before the engine existed can never cache an empty list.
    var speakerModelCatalog: [String] {
        (0..<Int(nc_speaker_model_count())).map { i in
            readString { nc_speaker_model_name(Int32(i), $0, $1) }
        }
    }
    var speakerOutputRoutes: [String] {
        (0..<Int(nc_speaker_output_route_count())).map { i in
            readString { nc_speaker_output_route(Int32(i), $0, $1) }
        }
    }
    var headphoneModelCatalog: [String] {
        (0..<Int(nc_headphone_model_count())).map { i in
            readString { nc_headphone_model_name(Int32(i), $0, $1) }
        }
    }

    // The real speaker/headphone the user monitors on (definition, not a simulation),
    // and whether speaker and headphone are mutually exclusive.
    @Published var physicalSpeakerModel = ""
    @Published var physicalHeadphoneModel = ""
    @Published var monitorOutputExclusive = true

    func setPhysicalSpeakerModel(_ model: String) {
        guard let handle else { return }
        _ = model.withCString { nc_monitor_set_physical_speaker_model(handle, $0) }
        physicalSpeakerModel = readString { nc_monitor_physical_speaker_model(handle, $0, $1) }
        refreshHistory()
    }
    func setPhysicalHeadphoneModel(_ model: String) {
        guard let handle else { return }
        _ = model.withCString { nc_monitor_set_physical_headphone_model(handle, $0) }
        physicalHeadphoneModel = readString { nc_monitor_physical_headphone_model(handle, $0, $1) }
        refreshHistory()
    }
    func setMonitorOutputExclusive(_ exclusive: Bool) {
        guard let handle else { return }
        nc_monitor_set_output_exclusive(handle, exclusive)
        monitorOutputExclusive = nc_monitor_output_exclusive(handle)
        refreshHistory()
    }

    // Master auto fade-out (a fade in the master volume automation over the last N sec).
    @Published var autoFadeOutSeconds: Double = 0
    @Published var autoFadeOutCurve = "equal_power"
    static let autoFadeCurves = ["equal_power", "s_curve", "linear", "exponential", "logarithmic"]
    /// The length used when auto fade-out is first turned on.
    static let defaultAutoFadeSeconds = 4.0

    func setAutoFadeSeconds(_ seconds: Double) {
        guard let handle else { return }
        nc_master_set_auto_fade_seconds(handle, seconds)
        autoFadeOutSeconds = nc_master_auto_fade_seconds(handle)
        reloadTracks()
        refreshHistory()
    }
    func setAutoFadeCurve(_ curve: String) {
        guard let handle else { return }
        _ = curve.withCString { nc_master_set_auto_fade_curve(handle, $0) }
        autoFadeOutCurve = readString { nc_master_auto_fade_curve(handle, $0, $1) }
        reloadTracks()
        refreshHistory()
    }
    /// 0..1 amplitude of a curve at normalized position, for drawing the fade preview.
    func autoFadeAmplitude(_ curve: String, _ t: Double) -> Float {
        curve.withCString { nc_auto_fade_amplitude($0, t) }
    }

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
                // Prefer a real MIDI interface over the Waves SoundGrid virtual ports,
                // which sit first in the list but are not the user's keyboard.
                let sources = midiInputs()
                let keyboard = sources.first { !$0.name.localizedCaseInsensitiveContains("SoundGrid")
                                            && !$0.name.localizedCaseInsensitiveContains("Waves") }
                let chosen = keyboard?.id ?? sources.first?.id ?? ""
                _ = chosen.withCString { nc_midi_live_start(handle, $0) }
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
        let dcSamples = Int(nc_delay_compensation_samples(handle))
        if dcSamples != delayCompensationSamples { delayCompensationSamples = dcSamples }
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
        let wasTransportRunning = transportRunning
        transportRunning = status.transportRunning
        if wasTransportRunning && !transportRunning { finishAutomationPass() }
        // Ballistic meters: snap up to a new peak, decay down. Without the decay a held
        // engine peak stays lit after stop; with it the meter always falls to silence.
        outputPeakLeft = max(status.outputPeakLeft, outputPeakLeft * Self.meterDecay)
        outputPeakRight = max(status.outputPeakRight, outputPeakRight * Self.meterDecay)
        updateSpectrumBins(handle)
        updateGoniometer(handle)
        momentaryLufs = status.momentaryLufs
        shortTermLufs = status.shortTermLufs
        integratedLufs = status.integratedLufs
        loudnessRange = status.loudnessRange
        truePeakDb = status.truePeakDb
        // Input meter follows the peak immediately on the way up and decays on the way
        // down, so a transient stays readable for a moment.
        inputPeak = max(status.inputPeak, inputPeak * 0.82)
        sampleRate = status.sampleRate
        bufferSize = Int(status.requestedBufferSize)
        delayCompensationMs = status.delayCompensationMs
        maxRenderDurationUs = status.realtimeMaxRenderDurationUs
        deviceName = withUnsafePointer(to: status.deviceName) {
            $0.withMemoryRebound(to: CChar.self, capacity: Int(NC_TEXT_LEN)) { String(cString: $0) }
        }

        updatePlayhead(engineSeconds: status.playbackSeconds)
        // Drive any plugin-parameter automation lanes to the playhead, so a drawn plug-in
        // curve is heard live. (Volume/pan are baked by the renderer; this covers inserts.)
        if transportRunning { nc_apply_plugin_automation(handle, playheadSeconds) }
        serviceAutomation()
        applyTrackMeters(status)

        // Pulse the solo-implied blink ~2 Hz while any track is soloed; hold it off
        // otherwise so idle strips do not repaint. Only publish on a change.
        let blink = anyTrackSoloed && Int(CACurrentMediaTime() * 2.2).isMultiple(of: 2)
        if blink != soloBlinkOn { soloBlinkOn = blink }

        // Clip warning: pulse ~2.6 Hz while any channel (or the master out) is at/over
        // 0 dBFS. Linear 0.98 ≈ -0.18 dB, just under the strip's red threshold, so a red
        // readout always has the blink running behind it.
        let clipping = outputPeakLeft >= 0.98 || outputPeakRight >= 0.98
            || tracks.contains { max($0.peakLeft, $0.peakRight) >= 0.98 }
        let cb = clipping && Int(CACurrentMediaTime() * 2.6).isMultiple(of: 2)
        if cb != clipBlinkOn { clipBlinkOn = cb }

        // Live MIDI: keep a keyboard open and drain its notes into armed instruments.
        pumpLiveMidi(handle)
        // The pump bumps the activity; read (which resets it) and decay for the meter.
        midiActivity = max(nc_midi_input_activity(handle), midiActivity - 0.07)

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
