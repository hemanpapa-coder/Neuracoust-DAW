import AppKit
import AVFoundation
import Foundation
import ServiceManagement
import SwiftUI

/// The moving playhead, split off the main EngineController so its 30 Hz updates re-render ONLY the
/// views that draw it (timeline, transport, piano roll) instead of the whole engine-observing UI.
@MainActor
final class PlayheadClock: ObservableObject {
    @Published var seconds: Double = 0
}

/// Owns the C++ engine and drives every live readout from a 30 Hz poll of its
/// status snapshot. The engine pushes nothing — there are no callbacks, no KVO,
/// no notifications (docs/legacy-ui-contract.md §1).
///
/// All engine calls happen on the main actor because the engine's public API is
/// main-thread-only.
@MainActor
final class EngineController: ObservableObject {
    struct MidiSurfaceEndpoint: Identifiable, Hashable {
        let id: String
        let name: String
    }
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
    // Cumulative render-deadline misses (late wakes) — each is a potential audible dropout even when
    // playback keeps rolling. A safe session should hold this at (or near) 0.
    @Published private(set) var lateWakeCount: Int = 0
    // The count at the last user reset. The dock shows `lateWakeCount - baseline`, so the reset button
    // clears the visible tally without touching the realtime engine's atomic. Auto-corrects on an
    // engine restart (the raw count drops back to 0).
    @Published private(set) var lateWakeBaseline: Int = 0
    /// Dropouts since the last reset (or engine start). What the monitor dock displays.
    var dropoutCount: Int { max(0, lateWakeCount - lateWakeBaseline) }
    func resetDropoutCount() { lateWakeBaseline = lateWakeCount }
    @Published private(set) var deviceName = ""
    @Published private(set) var startupError: String?
    @Published private(set) var huiInputs: [MidiSurfaceEndpoint] = []
    @Published private(set) var huiOutputs: [MidiSurfaceEndpoint] = []
    @Published private(set) var huiConnected = false
    @Published private(set) var huiStatus = "연결 안 됨"
    @Published var huiInputId = ""
    @Published var huiOutputId = ""
    private var huiReconnectTicks = 0

    /// Smoothed playhead. Between polls it advances on the wall clock; it only
    /// snaps back to the engine when the two disagree by more than
    /// `resyncThreshold`. Reading `playbackSeconds` straight from the snapshot
    /// makes the playhead step at 30 Hz.
    ///
    /// NOT @Published: it advances every poll during playback, and if it lived on this (god)
    /// ObservableObject, every SwiftUI view observing the engine — the heavy MonitorDock, the mixer,
    /// the inspector, none of which show the playhead — would re-layout 30×/s, a measured ~70 % main-
    /// thread storm that stutters the timeline. Instead the 30 Hz value is mirrored onto the tiny
    /// `playheadClock`, which ONLY the timeline / transport / piano-roll observe. Internal reads use
    /// this plain field unchanged; writes go through `setPlayhead` so both stay in sync.
    private(set) var playheadSeconds: Double = 0
    /// The 30 Hz playhead, isolated so it re-renders only the views that draw it (see `playheadSeconds`).
    let playheadClock = PlayheadClock()
    private func setPlayhead(_ value: Double) {
        playheadSeconds = value
        if playheadClock.seconds != value { playheadClock.seconds = value }
    }

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
    @Published private(set) var preRollSeconds: Double = 0
    @Published private(set) var postRollSeconds: Double = 0
    /// The track lane a lane-dragged edit range belongs to (Pro Tools selector), for the highlight.
    /// nil = a ruler range that spans all lanes.
    @Published var editRangeLane: Int? = nil
    @Published var clickEnabled = false
    /// The click resolution: "auto" / "quarter" / "eighth" / "sixteenth".
    @Published var metronomeSubdivision = "auto"
    /// Click level (linear, 0..2) and timbre ("beep"/"wood"/"rim"/"cowbell").
    @Published var metronomeGain: Double = 1.0
    @Published var metronomeSound = "beep"
    /// Groove: "straight" / "shuffle" / "triplet", plus swing amount (0.5..0.9 meaningful).
    @Published var grooveFeel = "straight"
    @Published var grooveSwingAmount: Double = 0.6
    /// Accent the bar's first beat (off = an even, flat click).
    @Published var metronomeAccentFirst = true
    /// The selected groove genre (a preset that sets subdivision + swing + accent pattern).
    @Published var metronomeGenre = "straight"

    /// One groove preset. What matters for a *click* is the rhythmic FEEL (subdivision + swing)
    /// and the ACCENT pattern, so the catalog is organised by feel category, not by exhaustive
    /// genre names. `pattern` is per-step gains (0..1) over one 4/4 bar at `subdivision`; empty =
    /// the default bar/beat hierarchy. A 0 gain is a rest (no click on that step).
    struct MetronomeGenre {
        let id: String
        let title: String
        let category: String
        let subdivision: String
        let feel: String
        let swing: Double
        let pattern: [Float]
    }

    /// Feel categories, in menu order.
    static let metronomeGenreCategories = ["기본", "스트레이트", "스윙·셔플", "라틴"]

    static let metronomeGenres: [MetronomeGenre] = [
        // 기본 — the plain accent hierarchy, no genre pattern.
        .init(id: "straight", title: "기본 (강약 계층)", category: "기본",
              subdivision: "auto", feel: "straight", swing: 0.6, pattern: []),

        // 스트레이트 — even subdivisions, backbeat-driven (pop / rock / funk / ballad).
        .init(id: "poprock", title: "팝·락 (백비트)", category: "스트레이트",
              subdivision: "eighth", feel: "straight", swing: 0.6,
              pattern: [1.0, 0.25, 0.75, 0.25, 0.6, 0.25, 0.75, 0.3]),
        .init(id: "sixteen", title: "16비트 그루브", category: "스트레이트",
              subdivision: "sixteenth", feel: "straight", swing: 0.6,
              pattern: [1.0, 0.3, 0.45, 0.3, 0.7, 0.3, 0.45, 0.35, 0.6, 0.3, 0.45, 0.3, 0.75, 0.35, 0.5, 0.4]),
        .init(id: "ballad", title: "발라드 (부드럽게)", category: "스트레이트",
              subdivision: "eighth", feel: "straight", swing: 0.6,
              pattern: [0.9, 0.2, 0.55, 0.2, 0.5, 0.2, 0.55, 0.25]),

        // 스윙·셔플 — swung eighths (jazz / blues) and a triplet feel (ballad / gospel).
        .init(id: "jazz", title: "재즈 스윙", category: "스윙·셔플",
              subdivision: "eighth", feel: "shuffle", swing: 0.66,
              pattern: [0.6, 0.3, 0.9, 0.35, 0.55, 0.3, 0.9, 0.4]),
        .init(id: "blues", title: "블루스 셔플", category: "스윙·셔플",
              subdivision: "eighth", feel: "shuffle", swing: 0.62,
              pattern: [1.0, 0.35, 0.55, 0.3, 0.85, 0.4, 0.6, 0.35]),
        .init(id: "triplet", title: "트리플렛 (12/8)", category: "스윙·셔플",
              subdivision: "eighth", feel: "triplet", swing: 0.66,
              pattern: [0.95, 0.25, 0.5, 0.25, 0.55, 0.25, 0.5, 0.3]),

        // 라틴 — clave-based (bossa / samba / son clave).
        .init(id: "bossa", title: "보사노바", category: "라틴",
              subdivision: "sixteenth", feel: "straight", swing: 0.6,
              pattern: [1.0, 0, 0, 0.7, 0.5, 0, 0.7, 0, 0.5, 0, 0.7, 0, 0.6, 0, 0, 0]),
        .init(id: "samba", title: "삼바", category: "라틴",
              subdivision: "sixteenth", feel: "straight", swing: 0.6,
              pattern: [0.9, 0.3, 0.5, 0.35, 0.75, 0.35, 0.5, 0.4, 0.6, 0.3, 0.5, 0.35, 0.8, 0.35, 0.55, 0.5]),
        .init(id: "clave", title: "손 클라베 (살사)", category: "라틴",
              subdivision: "sixteenth", feel: "straight", swing: 0.6,
              pattern: [1.0, 0, 0, 0.85, 0, 0, 0.85, 0, 0, 0, 0.85, 0, 0.85, 0, 0, 0]),
    ]
    /// Bars of click before a record take actually begins (0 = off). The transport pre-rolls
    /// from this many bars back and the take opens when the playhead reaches the record point.
    @Published var countInBars = 0
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
    @Published private(set) var lastError: String? {
        didSet {
            if let msg = lastError, !msg.isEmpty { Diagnostics.shared.log("status: \(msg)") }
        }
    }
    @Published private(set) var timeSignature = (numerator: 4, denominator: 4)

    // MARK: Tracks

    struct TrackSend: Hashable {
        let bus: String
        let gainDb: Float
        var pan: Float = 0
        var preFader: Bool = false
    }

    /// One instrument in the rack, with its per-layer mute (bypass) and solo state.
    struct InstrumentLayer: Hashable {
        let name: String
        var muted: Bool = false
        var soloed: Bool = false
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
        /// Channel format — a mono track sums to one channel panned into the field, a stereo
        /// track keeps L/R.
        var isStereo: Bool = true
        var automationMode: String = "read"
        var inserts: [InsertSlot]
        /// What turns this track's MIDI notes into sound. Empty on every other kind.
        var instrumentName: String
        /// The instrument rack: every loaded instrument on the track (slot 0 = primary, the
        /// rest are layers), all fed the same MIDI and summed. Empty when there's no instrument.
        var instrumentLayers: [InstrumentLayer] = []
        var sends: [TrackSend]
        var consoleModel: String = "4000e"
        var consoleModuleOrder: String = "filter,eq,gate,comp,saturator"
        var consoleFilterEnabled = false
        var consoleFilterCircuitMode = false
        var consoleHighPassEnabled = false
        var consoleLowPassEnabled = false
        var consoleEqEnabled = false
        var consoleEqCircuitMode = false
        var consoleEqHfBell = false
        var consoleEqLfBell = false
        var consoleEqEMode = true
        var consoleCompEnabled = false
        var consoleCompCircuitMode = false
        var consoleCompFastAttack = false
        var consoleCompPeakMode = false
        var consoleCompGainReductionDb: Float = 0
        var consoleGateEnabled = false
        var consoleGateCircuitMode = false
        var consoleSaturatorEnabled = false
        var consoleDualMono = false
        var consoleSaturatorCircuitMode = false
        var consoleExpanderMode = true
        var consoleGateFastAttack = false
        var consoleGateGainReductionDb: Float = 0

        /// Plugin delay compensation applied to this track/bus, in samples (0 = none). Shown per
        /// strip when the mixer's PDC readout is on.
        var delayCompSamples: Int = 0

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
        /// An ARA plug-in (Melodyne and the like). Not a realtime effect — badged in the browser
        /// and refused by the insert path until ARA is hosted.
        var isAra: Bool = false
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
    /// Set when the user tried to add an instrument as an insert (FX only) — the browser hints.
    @Published var instrumentOnInsertRejected = false
    /// Why an insert-add was refused, shown in the plug-in browser next to the click. Cleared by
    /// the browser after it has been read.
    @Published var insertRejectedReason: String?
    /// When set, the plugin browser loads the picked instrument into this rack slot (a layer)
    /// instead of the primary slot 0. Cleared after the pick.
    private var pluginTargetInstrumentSlot: Int?
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

    /// The drum MIDI library (browse + drag/insert). Indexed once, cached to disk.
    let midiLibrary = MidiLibrary()
    @Published var midiLibraryOpen = false
    func openMidiLibrary() { midiLibrary.loadCacheIfAvailable(); midiLibraryOpen = true }
    func closeMidiLibrary() { midiLibraryOpen = false }
    func toggleMidiLibrary() { if midiLibraryOpen { midiLibraryOpen = false } else { openMidiLibrary() } }

    /// The scan costs ~90 ms over a thousand plug-ins, so it runs once and is cached
    /// in the engine. Reopening the browser reuses it.
    func openPluginBrowser(forTrack trackId: Int) {
        guard let handle else { return }
        if totalPluginCount == 0 {
            totalPluginCount = Int(nc_plugin_scan(handle))
            araPluginCache = nil   // a rescan can change what is ARA-capable
            reloadFacets()
        } else if nc_plugin_locations_changed(handle) {
            // A plug-in was installed/updated/removed since the last scan — refresh so it
            // shows up without the user reaching for the ↻ button.
            totalPluginCount = Int(nc_plugin_rescan(handle))
            reloadFacets()
        }
        pluginTargetTrack = trackId
        // A leftover Instrument facet from an earlier browse would combine with the
        // FX-insert exclusion into a guaranteed-empty list.
        if !browseTargetIsInstrumentSlot && pluginCategory == "Instrument" {
            pluginCategory = ""
        }
        applyPluginFilter()
    }

    /// Force a fresh scan from disk — picks up a plug-in installed after the app launched.
    func rescanPlugins() {
        guard let handle else { return }
        totalPluginCount = Int(nc_plugin_rescan(handle))
        reloadFacets()
        applyPluginFilter()
    }

    func closePluginBrowser() {
        pluginTargetTrack = nil
        pluginTargetInstrumentSlot = nil
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

    /// The browser serves two different pickers and each shows ONLY what its target
    /// can take: an instrument-slot target (빈 칩 / 악기 교체 / 레이어 추가) lists
    /// instruments only, every other target — an FX insert chain on ANY track kind,
    /// or the master chain — lists everything BUT instruments.
    var browseTargetIsInstrumentSlot: Bool { pluginTargetInstrumentSlot != nil }

    private func applyPluginFilter() {
        guard let handle, totalPluginCount > 0 else { return }

        let category = browseTargetIsInstrumentSlot ? "Instrument" : pluginCategory
        let excludeCategory = browseTargetIsInstrumentSlot ? "" : "Instrument"
        let count = Int(nc_plugin_apply_filter(handle, pluginSearch, pluginBrand, category,
                                               pluginFormat, excludeCategory))

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
                path: readEngineString { nc_plugin_path(handle, i, $0, $1) },
                isAra: nc_plugin_is_ara(handle, i)
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

    /// Add a plug-in straight onto a track's insert chain without opening the browser — for the
    /// insert slot's right-click "플러그인 선택" submenu. Routes through addInsert so the browser
    /// dismiss + editor auto-open + instrument guard all apply.
    func addInsertDirect(track trackId: Int, pluginIndex: Int) {
        pluginTargetTrack = trackId
        pluginTargetInstrumentSlot = nil
        addInsert(pluginIndex)
    }

    /// An instrument dropped on an instrument track fills its instrument slot rather
    /// than an insert — an insert cannot turn MIDI notes into sound.
    func addInsert(_ pluginIndex: Int) {
        guard let handle, let trackId = pluginTargetTrack else { return }

        if trackId == Self.masterInsertTargetId {
            if nc_master_add_insert(handle, Int32(pluginIndex)) {
                reloadMasterInserts()
                refreshHistory()
                // Keep the browser open so more master inserts can be stacked in one session; open
                // each from its chain chip after closing (닫기).
            }
            return
        }
        guard let track = tracks.first(where: { $0.id == trackId }) else { return }

        let plugin = plugins.first { $0.id == pluginIndex }
        // The plugin category is a name heuristic — a synth whose name carries no
        // "synth/piano/drum" keyword (Serum, Kontakt, Vital) is not tagged "Instrument".
        // So an EXPLICIT instrument-slot target (empty chip, 악기 교체, 레이어 추가) must win
        // over the heuristic: the user asked for the instrument slot, not an insert.
        let layerSlot = pluginTargetInstrumentSlot
        pluginTargetInstrumentSlot = nil
        let changed: Bool
        var addedFxInsert = false
        if track.kind == .instrument, let slot = layerSlot {
            changed = slot == 0
                ? nc_track_set_instrument(handle, Int32(trackId), Int32(pluginIndex))
                : nc_track_set_instrument_slot(handle, Int32(trackId), Int32(slot), Int32(pluginIndex))
        } else if track.kind == .instrument && plugin?.category == "Instrument" {
            // No explicit target, but the plugin looks like an instrument → primary slot.
            changed = nc_track_set_instrument(handle, Int32(trackId), Int32(pluginIndex))
        } else if plugin?.category == "Instrument" {
            // An instrument on a non-instrument track / FX insert target — not allowed. Instruments
            // live in an instrument track's instrument slot only. Flag it so the browser can hint.
            changed = false
            instrumentOnInsertRejected = true
        } else {
            changed = nc_track_add_insert(handle, Int32(trackId), Int32(pluginIndex))
            addedFxInsert = changed
            if !changed {
                // A refusal worth explaining — e.g. an ARA plug-in kept out of the realtime chain.
                // Shown in the browser itself, where the click happened: a status-strip line alone
                // reads as "nothing happened".
                let reason = readEngineString { nc_last_plugin_message(handle, $0, $1) }
                if !reason.isEmpty {
                    lastError = reason
                    insertRejectedReason = reason
                }
            }
        }
        if changed {
            reloadTracks()
            refreshHistory()
            // Keep the browser OPEN so several plug-ins can be stacked into the chain in one session
            // (the right-hand 인서트 체인 updates live as each is added — the "load a bunch at once"
            // flow the user wanted). We no longer auto-open the editor here: that buried the browser.
            // Open an editor from its chain chip after closing (닫기).
            _ = addedFxInsert
        }
    }

    /// Load (or replace) the track's primary instrument. Opens the browser targeting the
    /// instrument slot explicitly, so any picked plugin fills the slot rather than an insert.
    func loadInstrument(track trackId: Int) {
        pluginTargetInstrumentSlot = 0
        openPluginBrowser(forTrack: trackId)
    }

    /// Add another instrument to the track's rack (a layer), all fed the same MIDI and summed.
    /// Opens the plugin browser targeting the next free rack slot.
    func addInstrumentLayer(track trackId: Int) {
        guard let track = tracks.first(where: { $0.id == trackId }) else { return }
        // Empty rack → slot 0 (the primary instrument); otherwise the next free layer slot.
        let nextSlot = track.instrumentLayers.count
        guard nextSlot < 8 else { lastError = "악기 레이어는 최대 8개입니다."; return }
        pluginTargetInstrumentSlot = nextSlot
        openPluginBrowser(forTrack: trackId)
    }

    func removeInstrumentLayer(track trackId: Int, slot: Int) {
        guard let handle else { return }
        // Close the track's instrument editor windows first — the layers renumber, so an open
        // editor would otherwise point at the wrong (or a gone) layer.
        pluginEditors.closeInstrumentEditors(trackId: trackId)
        if nc_track_remove_instrument_slot(handle, Int32(trackId), Int32(slot)) {
            reloadTracks()
            refreshHistory()
        }
    }

    /// Per-layer mute (bypass): the layer stops sounding, the rest keep playing.
    func toggleInstrumentLayerMute(track trackId: Int, slot: Int) {
        guard let handle else { return }
        if nc_track_toggle_instrument_slot_bypass(handle, Int32(trackId), Int32(slot)) {
            reloadTracks()
            refreshHistory()
        }
    }

    /// Per-layer solo: while any layer is soloed, only soloed layers sound.
    func toggleInstrumentLayerSolo(track trackId: Int, slot: Int) {
        guard let handle else { return }
        if nc_track_toggle_instrument_slot_solo(handle, Int32(trackId), Int32(slot)) {
            reloadTracks()
            refreshHistory()
        }
    }

    func removeInsert(track trackId: Int, slot: Int) {
        guard let handle else { return }
        // Close the removed slot's editor (and higher, now-shifted ones) so no ghost window
        // lingers and no editor points at the wrong plugin after the slots renumber.
        pluginEditors.closeInsertAtOrAbove(trackId: trackId, insertIndex: slot)
        if nc_track_remove_insert(handle, Int32(trackId), Int32(slot)) {
            reloadTracks()
            refreshHistory()
        }
    }

    /// Switch an insert between "native" (in-process, on the audio thread) and "internal"
    /// (out-of-process on the isolated performance core) so the two can be compared per channel.
    /// Internal only actually runs off-thread while core isolation is on (dock → Remote Core/DSP).
    func setInsertDspMode(track trackId: Int, slot: Int, mode: String) {
        guard let handle else { return }
        let changed = mode.withCString { nc_track_insert_set_dsp_mode(handle, Int32(trackId), Int32(slot), $0) }
        if changed { reloadTracks(); refreshHistory() }
    }

    func setMasterInsertDspMode(slot: Int, mode: String) {
        guard let handle else { return }
        let changed = mode.withCString { nc_master_insert_set_dsp_mode(handle, Int32(slot), $0) }
        if changed { reloadTracks(); refreshHistory() }
    }

    /// Remove the instrument from an instrument track's slot.
    func clearInstrument(track trackId: Int) {
        guard let handle else { return }
        // Take the instrument's editor window(s) down with it — otherwise a ghost TRITON window
        // lingers after "악기 제거" (and the reverse monitor ring keeps feeding a gone instrument).
        pluginEditors.closeInstrumentEditors(trackId: trackId)
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

    /// Option-drag in the mixer copies an insert (with its parameters) to another slot,
    /// same track or across channels.
    func copyInsert(srcTrack: Int, srcSlot: Int, dstTrack: Int, dstSlot: Int) {
        guard let handle else { return }
        if nc_track_copy_insert(handle, Int32(srcTrack), Int32(srcSlot), Int32(dstTrack), Int32(dstSlot)) {
            reloadTracks()
            refreshHistory()
        }
    }

    /// Plain drag of an insert onto a different channel moves it there.
    func moveInsertAcross(srcTrack: Int, srcSlot: Int, dstTrack: Int, dstSlot: Int) {
        guard let handle else { return }
        if nc_track_move_insert_across(handle, Int32(srcTrack), Int32(srcSlot), Int32(dstTrack), Int32(dstSlot)) {
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

        // insertIndex <= -1 addresses an instrument rack slot: -1 = slot 0 (primary),
        // -2 = layer 1, … so a layer's editor opens and is addressed on its own.
        if insertIndex <= Self.instrumentSlotIndex {
            let i = Int32(trackId), sl = Int32(Self.instrumentSlotIndex - insertIndex)
            let path = readEngineString { nc_track_instrument_slot_plugin_path(handle, i, sl, $0, $1) }
            guard !path.isEmpty else { return nil }
            let name = readEngineString { nc_track_instrument_slot_name(handle, i, sl, $0, $1) }
            // An instrument runs in the render plan, never in the sandbox bridge, so it
            // has no shared-memory observer to point its editor at.
            return InsertDescriptor(
                trackName: track.name,
                name: name == "No Instrument" ? "" : name,
                pluginPath: path,
                format: readEngineString { nc_track_instrument_slot_plugin_format(handle, i, sl, $0, $1) },
                classId: readEngineString { nc_track_instrument_slot_class_id(handle, i, sl, $0, $1) },
                className: readEngineString { nc_track_instrument_slot_class_name(handle, i, sl, $0, $1) },
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
        if insertIndex <= Self.instrumentSlotIndex {
            let sl = Int32(Self.instrumentSlotIndex - insertIndex)
            let count = Int(nc_track_instrument_slot_param_count(handle, i, sl))
            return (0..<count).map { p in
                (id: nc_track_instrument_slot_param_id(handle, i, sl, Int32(p)),
                 value: nc_track_instrument_slot_param_value(handle, i, sl, Int32(p)))
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
        } else if insertIndex <= Self.instrumentSlotIndex {
            changed = nc_track_set_instrument_slot_vst3_parameter(
                handle, Int32(trackId), Int32(Self.instrumentSlotIndex - insertIndex),
                parameterId, nil, normalizedValue)
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
        // Passive modeled speaker's amp + cable (heuristic tone), and whether the model is passive.
        var amp: String = ""
        var cable: String = ""
        var modelIsPassive: Bool = false

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

    /// Non-destructive clip processing (Logic/Cubase style) — the renderer honours the flags, so
    /// nothing writes a new file and each undoes cleanly.
    func reverseClip(_ clipId: String) {
        guard let handle else { return }
        if nc_clip_toggle_reversed(handle, clipId) { reloadClips(); refreshHistory() }
    }
    func toggleClipMute(_ clipId: String) {
        guard let handle else { return }
        if nc_clip_toggle_muted(handle, clipId) { reloadClips(); refreshHistory() }
    }
    func toggleClipPolarity(_ clipId: String) {
        guard let handle else { return }
        if nc_clip_toggle_polarity(handle, clipId) { reloadClips(); refreshHistory() }
    }
    func normalizeClip(_ clipId: String) {
        guard let handle else { return }
        if nc_clip_normalize(handle, clipId) { reloadClips(); refreshHistory() }
    }

    /// Offline time-stretch + pitch-shift PRINT (Serato phase vocoder) — renders the clip to a new
    /// WAV and repoints it. timeRatio changes length, semitones change pitch, independently.
    func applyClipTimePitch(_ clipId: String, timeRatio: Double, semitones: Double) {
        guard let handle else { return }
        var err = [CChar](repeating: 0, count: 256)
        if nc_clip_apply_time_pitch(handle, clipId, timeRatio, semitones, formantPreserve ? 1 : 0, &err, err.count) {
            reloadClips(); refreshHistory()
        } else {
            let message = String(cString: err)
            if !message.isEmpty { Diagnostics.shared.log("time/pitch: \(message)") }
        }
    }

    /// Separate a clip into 4 stems (Drums/Bass/Other/Vocals) with the bundled Demucs helper, spawned
    /// as a SUBPROCESS so LibTorch (and any crash) stays out of the audio process. On success each stem
    /// becomes a new audio track at the clip's start. Runs off the main thread; progress is published.
    /// True when the optional 6-source model (guitar/piano too) has been bundled.
    var stem6sAvailable: Bool {
        FileManager.default.fileExists(atPath: Bundle.main.bundlePath + "/Contents/Resources/htdemucs_6s.pt")
    }
    /// True when the drum-split model (kick/snare/toms/cymbals) has been bundled.
    var drumSplitAvailable: Bool {
        FileManager.default.fileExists(atPath: Bundle.main.bundlePath + "/Contents/Resources/drumsep.pt")
    }
    /// True when the experimental orchestra-family model (strings/brass/woodwinds/…) has been bundled.
    var orchestraSeparationAvailable: Bool {
        FileManager.default.fileExists(atPath: Bundle.main.bundlePath + "/Contents/Resources/orchestra.pt")
    }

    /// Dispatch a stem-separation preset. Keeps the plain "all 4 parts" as the default; the rest let the
    /// user pick which stems (Stem Magic style) or auto-keep only the parts that are actually present.
    func separateClipStemsPreset(_ clipId: String, _ preset: String) {
        switch preset {
        case "configure": presentStemSeparationSettings(clipId)
        case "karaoke": separateClipStems(clipId, stems: ["vocals", "accompaniment"])       // voice / instrumental
        case "vocals":  separateClipStems(clipId, stems: ["vocals"])
        case "drums":   separateClipStems(clipId, stems: ["drums"])
        case "bass":    separateClipStems(clipId, stems: ["bass"])
        case "other":   separateClipStems(clipId, stems: ["other"])
        case "6s":      separateClipStems(clipId, skipSilent: true, modelName: "htdemucs_6s.pt")
        // Drum split (kick/snare/토 ...). Run the drumsep model with --kind drum; the toms variant also
        // fans the Toms stem out into pitch bands so different-pitch toms land on different tracks.
        case "drums-split":      separateClipStems(clipId, skipSilent: true, modelName: "drumsep.pt", kind: "drum")
        case "drums-split-toms": separateClipStems(clipId, skipSilent: true, modelName: "drumsep.pt", kind: "drum", splitToms: 3)
        case "orchestra":        separateClipStems(clipId, skipSilent: true, modelName: "orchestra.pt", kind: "orchestra")
        default:        separateClipStems(clipId)                                           // all native stems
        }
    }

    /// Stem Magic-style progressive selection. The four main families are operational now; finer
    /// families stay visible but disabled until their dedicated model is actually connected.
    func presentStemSeparationSettings(_ clipId: String) {
        let alert = NSAlert()
        alert.messageText = "스템 분리"
        alert.informativeText = "분리할 파트만 선택하세요. 선택한 클립 구간만 Metal로 처리합니다."
        alert.addButton(withTitle: "Metal로 분리")
        alert.addButton(withTitle: "취소")

        let mode = NSPopUpButton()
        mode.addItems(withTitles: ["선택한 파트", "보컬 / 반주 (2파트)"])
        let vocals = NSButton(checkboxWithTitle: "보컬", target: nil, action: nil)
        let drums = NSButton(checkboxWithTitle: "드럼", target: nil, action: nil)
        let bass = NSButton(checkboxWithTitle: "베이스", target: nil, action: nil)
        let other = NSButton(checkboxWithTitle: "그 외 / 악기", target: nil, action: nil)
        [vocals, drums, bass, other].forEach { $0.state = .on }

        let kick = NSButton(checkboxWithTitle: "    Kick", target: nil, action: nil)
        let snare = NSButton(checkboxWithTitle: "Snare", target: nil, action: nil)
        let toms = NSButton(checkboxWithTitle: "Tom", target: nil, action: nil)
        let cymbals = NSButton(checkboxWithTitle: "Cymbal", target: nil, action: nil)
        let drumDetail = NSStackView(views: [kick, snare, toms, cymbals])
        drumDetail.orientation = .horizontal; drumDetail.spacing = 10
        let hiHat = NSTextField(labelWithString: "    Hi-Hat은 현재 Cymbal에 포함")
        hiHat.textColor = .tertiaryLabelColor
        let guitar = NSButton(checkboxWithTitle: "    Guitar", target: nil, action: nil)
        let piano = NSButton(checkboxWithTitle: "Piano", target: nil, action: nil)
        let otherDetail = NSStackView(views: [guitar, piano])
        otherDetail.orientation = .horizontal; otherDetail.spacing = 10
        let futureOther = NSTextField(labelWithString: "    Organ · Strings · Brass · Winds · Synth: 전용 모델 연결 예정")
        futureOther.textColor = .tertiaryLabelColor
        let note = NSTextField(wrappingLabelWithString:
            "세부 파트는 전용 모델이 연결되는 항목부터 활성화됩니다. 현재 빌드는 메인 4파트와 2파트를 실제 출력합니다.")
        note.textColor = .secondaryLabelColor
        note.font = .systemFont(ofSize: 10)

        let stack = NSStackView(views: [
            NSTextField(labelWithString: "출력 구성"), mode,
            vocals, drums, drumDetail, hiHat, bass, other, otherDetail, futureOther, note
        ])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 6
        stack.frame = NSRect(x: 0, y: 0, width: 430, height: 285)
        note.preferredMaxLayoutWidth = 380
        alert.accessoryView = stack
        guard alert.runModal() == .alertFirstButtonReturn else { return }

        if mode.indexOfSelectedItem == 1 {
            separateClipStems(clipId, stems: ["vocals", "accompaniment"])
            return
        }
        var selected: [String] = []
        if drums.state == .on { selected.append("drums") }
        if bass.state == .on { selected.append("bass") }
        if other.state == .on { selected.append("other") }
        if vocals.state == .on { selected.append("vocals") }
        guard !selected.isEmpty else {
            stemSeparationStatus = "스템 분리: 하나 이상의 파트를 선택하세요"
            return
        }
        let selectedDrums = [
            kick.state == .on ? "kick" : nil,
            snare.state == .on ? "snare" : nil,
            toms.state == .on ? "toms" : nil,
            cymbals.state == .on ? "cymbals" : nil
        ].compactMap { $0 }
        let selectedOther = [
            guitar.state == .on ? "guitar" : nil,
            piano.state == .on ? "piano" : nil
        ].compactMap { $0 }
        separateClipStems(clipId, stems: selected, drumDetails: selectedDrums, otherDetails: selectedOther)
    }

    func separateClipStems(_ clipId: String, stems: [String] = [], skipSilent: Bool = false,
                           modelName: String? = nil, kind: String = "music", splitToms: Int = 0,
                           drumDetails: [String] = [], otherDetails: [String] = []) {
        guard let handle, stemSeparationProgress == nil else { return }
        // Resolve the clip's source file + start time by id.
        var idx = -1
        let count = Int(nc_clip_count(handle))
        for i in 0..<count {
            var id = [CChar](repeating: 0, count: 128)
            nc_clip_id(handle, Int32(i), &id, id.count)
            if String(cString: id) == clipId { idx = i; break }
        }
        guard idx >= 0 else { return }
        var srcBuf = [CChar](repeating: 0, count: 1024)
        nc_clip_source_path(handle, Int32(idx), &srcBuf, srcBuf.count)
        let originalSourcePath = String(cString: srcBuf)
        let startSeconds = nc_clip_start_seconds(handle, Int32(idx))
        guard !originalSourcePath.isEmpty else { return }

        let helper = Bundle.main.bundlePath + "/Contents/Resources/neuracoust_stem_mps.py"
        let metalPythonCandidates = [
            "/Volumes/Program Dev/Neuracoust Stem Magic (Metal)/.venv/bin/python",
            "/Volumes/Program Dev/Neuracoust Stem Magic/.venv/bin/python"
        ]
        guard let metalPython = metalPythonCandidates.first(where: {
            FileManager.default.isExecutableFile(atPath: $0)
        }), FileManager.default.fileExists(atPath: helper) else {
            stemSeparationStatus = "Stem Magic Metal 런타임을 찾을 수 없습니다"
            Diagnostics.shared.log("stem separation: Metal runtime or script missing")
            return
        }
        let prefix = URL(fileURLWithPath: originalSourcePath).deletingPathExtension().lastPathComponent
        let jobDir = (NSTemporaryDirectory() as NSString).appendingPathComponent("neuracoust-stems-\(UUID().uuidString)")
        let sourcePath = (jobDir as NSString).appendingPathComponent("selected-clip.wav")
        let outDir = (jobDir as NSString).appendingPathComponent("output")
        try? FileManager.default.createDirectory(atPath: jobDir, withIntermediateDirectories: true)
        var exportError = [CChar](repeating: 0, count: 512)
        guard nc_clip_export_raw_window(handle, clipId, sourcePath, &exportError, exportError.count) else {
            stemSeparationStatus = "선택한 클립 구간을 준비하지 못했습니다: \(String(cString: exportError))"
            return
        }

        stemSeparationProgress = 0
        stemSeparationStatus = "스템 분리 준비 중…"

        DispatchQueue.global(qos: .userInitiated).async {
            let proc = Process()
            proc.executableURL = URL(fileURLWithPath: metalPython)
            var args = [helper, sourcePath, outDir, "--stem-prefix", prefix]
            if !stems.isEmpty { args += ["--stems", stems.joined(separator: ",")] }
            if !drumDetails.isEmpty { args += ["--drum-detail", drumDetails.joined(separator: ",")] }
            if !otherDetails.isEmpty { args += ["--other-detail", otherDetails.joined(separator: ",")] }
            _ = skipSilent; _ = modelName; _ = kind; _ = splitToms
            proc.arguments = args
            let pipe = Pipe()
            proc.standardOutput = pipe
            var stems: [(String, String)] = []
            var errMsg = ""
            var carry = ""
            pipe.fileHandleForReading.readabilityHandler = { fh in
                let data = fh.availableData
                guard !data.isEmpty, let chunk = String(data: data, encoding: .utf8) else { return }
                carry += chunk
                while let nl = carry.firstIndex(of: "\n") {
                    let line = String(carry[carry.startIndex..<nl])
                    carry = String(carry[carry.index(after: nl)...])
                    let parts = line.split(separator: " ", maxSplits: 2).map(String.init)
                    switch parts.first {
                    case "PROGRESS":
                        if parts.count > 1, let p = Double(parts[1]) {
                            DispatchQueue.main.async {
                                self.stemSeparationProgress = p
                                self.stemSeparationStatus = "스템 분리 중… \(Int(p * 100))%"
                            }
                        }
                    case "STEM": if parts.count >= 3 { stems.append((parts[1], parts[2])) }
                    case "ERROR": errMsg = parts.count > 1 ? parts[1] : "알 수 없는 오류"
                    default: break
                    }
                }
            }
            do { try proc.run() } catch {
                DispatchQueue.main.async { self.finishStemSeparation(nil, "실행 실패: \(error.localizedDescription)", startSeconds) }
                return
            }
            proc.waitUntilExit()
            pipe.fileHandleForReading.readabilityHandler = nil
            let code = proc.terminationStatus
            DispatchQueue.main.async {
                if code == 0 && !stems.isEmpty { self.finishStemSeparation(stems, "", startSeconds) }
                else { self.finishStemSeparation(nil, errMsg.isEmpty ? "분리 실패 (코드 \(code))" : errMsg, startSeconds) }
            }
        }
    }

    private func finishStemSeparation(_ stems: [(String, String)]?, _ error: String, _ startSeconds: Double) {
        stemSeparationProgress = nil
        guard let stems, let handle else {
            if !error.isEmpty { stemSeparationStatus = "스템 분리 실패: \(error)"; Diagnostics.shared.log("stem separation: \(error)") }
            return
        }
        // Word-level map so compound names ("Toms Low", "HiHat") localize too — each space-separated
        // token is translated and rejoined; an unknown token (e.g. a raw model name) passes through.
        let koWord = ["Drums": "드럼", "Bass": "베이스", "Other": "그외", "Vocals": "보컬",
                      "Other-Remainder": "그 외 나머지",
                      "Guitar": "기타", "Piano": "피아노", "Accompaniment": "반주",
                      "Kick": "킥", "Snare": "스네어", "Toms": "텀", "Tom": "텀",
                      "Cymbals": "심벌", "HiHat": "하이햇", "Hihat": "하이햇",
                      "Strings": "스트링", "Brass": "브라스", "Woodwinds": "목관", "Percussion": "퍼커션",
                      "Low": "로우", "Mid": "미드", "High": "하이"]
        func localize(_ n: String) -> String {
            n.split(separator: " ").map { koWord[String($0)] ?? String($0) }.joined(separator: " ")
        }
        var made = 0
        for (name, path) in stems {
            let trackIndex = nc_track_add_audio(handle)
            guard trackIndex >= 0 else { continue }
            _ = renameTrack(Int(trackIndex), to: localize(name))
            var err = [CChar](repeating: 0, count: 512)
            if nc_audio_import(handle, trackIndex, path, startSeconds, &err, err.count) { made += 1 }
        }
        reloadTracks(); reloadClips(); refreshHistory()
        stemSeparationStatus = "스템 분리 완료 — \(made)개 트랙 생성"
    }

    // MARK: Audio → MIDI

    /// Convert a (usually stem-separated) audio clip into MIDI on a NEW instrument track: detect the
    /// clip's notes and lay them into a MIDI region at the clip's start, so a bass / vocal / wind line
    /// becomes playable, editable MIDI. Monophonic — CREPE if bundled (best on stem artifacts), else the
    /// engine's YIN. Dense polyphony (piano/guitar chords) is approximated by its dominant line until a
    /// polyphonic transcription model is added. Runs off the main thread; progress is published.
    func convertClipToMidi(_ clipId: String) {
        guard let handle, stemSeparationProgress == nil else { return }
        // Detection fills engine->pitchEditNotes, which the pitch editor also owns — clobbering it while
        // an editor session is open would corrupt its apply. Ask the user to close it first.
        if pitchEditorClipId != nil {
            stemSeparationStatus = "MIDI 변환: 먼저 피치 에디터를 닫아주세요"; return
        }
        var idx = -1
        let count = Int(nc_clip_count(handle))
        for i in 0..<count {
            var id = [CChar](repeating: 0, count: 128)
            nc_clip_id(handle, Int32(i), &id, id.count)
            if String(cString: id) == clipId { idx = i; break }
        }
        guard idx >= 0 else { return }
        let startSeconds = nc_clip_start_seconds(handle, Int32(idx))
        var nameBuf = [CChar](repeating: 0, count: 256)
        nc_clip_name(handle, Int32(idx), &nameBuf, nameBuf.count)
        let clipName = String(cString: nameBuf)

        let tmpDir = (NSTemporaryDirectory() as NSString).appendingPathComponent("neuracoust-midi-\(UUID().uuidString)")
        try? FileManager.default.createDirectory(atPath: tmpDir, withIntermediateDirectories: true)
        let rawPath = (tmpDir as NSString).appendingPathComponent("clip.wav")
        var err = [CChar](repeating: 0, count: 512)
        guard nc_clip_export_raw_window(handle, clipId, rawPath, &err, err.count) else {
            stemSeparationStatus = "MIDI 변환 실패: 클립을 내보낼 수 없습니다"; return
        }

        stemSeparationProgress = 0
        stemSeparationStatus = "MIDI 변환 준비 중…"

        // Engine-YIN path (synchronous, no model): detect straight from the exported window.
        guard crepeAvailable else {
            nc_detect_notes_reset(handle)
            _ = nc_detect_notes_add_from_file(handle, rawPath, 0)   // 0 = melodic (monophonic)
            finishConvertToMidi(startSeconds: startSeconds, name: clipName)
            return
        }

        // CREPE path: run the neural detector subprocess, segment its pitch track into the note cache.
        let helper = Bundle.main.bundlePath + "/Contents/MacOS/neuracoust_pitch_detector"
        let modelPath = crepeModelPath
        stemSeparationStatus = "MIDI 변환: 정밀 검출 중…"
        DispatchQueue.global(qos: .userInitiated).async {
            let proc = Process()
            proc.executableURL = URL(fileURLWithPath: helper)
            proc.arguments = [rawPath, "--model", modelPath]
            let pipe = Pipe(); proc.standardOutput = pipe
            var times: [Double] = [], hzs: [Double] = [], confs: [Double] = []
            var carry = ""
            pipe.fileHandleForReading.readabilityHandler = { fh in
                let d = fh.availableData
                guard !d.isEmpty, let s = String(data: d, encoding: .utf8) else { return }
                carry += s
                while let nl = carry.firstIndex(of: "\n") {
                    let line = String(carry[carry.startIndex..<nl]); carry = String(carry[carry.index(after: nl)...])
                    let p = line.split(separator: " ").map(String.init)
                    if p.first == "PITCH", p.count >= 4, let t = Double(p[1]), let hz = Double(p[2]), let c = Double(p[3]) {
                        times.append(t); hzs.append(hz); confs.append(c)
                    }
                }
            }
            do { try proc.run() } catch {
                DispatchQueue.main.async {
                    guard let h = self.handle else { self.stemSeparationProgress = nil; return }
                    nc_detect_notes_reset(h); _ = nc_detect_notes_add_from_file(h, rawPath, 0)   // fall back to YIN
                    self.finishConvertToMidi(startSeconds: startSeconds, name: clipName)
                }
                return
            }
            proc.waitUntilExit(); pipe.fileHandleForReading.readabilityHandler = nil
            DispatchQueue.main.async {
                guard let h = self.handle else { self.stemSeparationProgress = nil; return }
                if times.isEmpty {
                    nc_detect_notes_reset(h); _ = nc_detect_notes_add_from_file(h, rawPath, 0)
                } else {
                    _ = times.withUnsafeBufferPointer { tp in hzs.withUnsafeBufferPointer { hp in confs.withUnsafeBufferPointer { cp in
                        nc_segment_pitch_track(h, tp.baseAddress, hp.baseAddress, cp.baseAddress, Int32(times.count))
                    }}}
                }
                self.finishConvertToMidi(startSeconds: startSeconds, name: clipName)
            }
        }
    }

    struct DetectedMidiNote { let startSec: Double; let durSec: Double; let midi: Int; let vel: Int }

    /// Build the instrument track + MIDI region from the note cache the detector just filled. Main thread.
    private func finishConvertToMidi(startSeconds: Double, name: String) {
        stemSeparationProgress = nil
        guard let handle else { return }
        let noteCount = Int(nc_clip_note_count(handle))
        guard noteCount > 0 else { stemSeparationStatus = "MIDI 변환: 검출된 노트가 없습니다"; return }

        // Read notes (start/duration in seconds relative to the clip window, detected MIDI, confidence).
        var detected: [DetectedMidiNote] = []
        for i in 0..<noteCount {
            let s = nc_clip_note_start_seconds(handle, Int32(i))
            let d = max(0.02, nc_clip_note_duration_seconds(handle, Int32(i)))
            let m = Int(nc_clip_note_detected_midi(handle, Int32(i)).rounded())
            let conf = nc_clip_note_confidence(handle, Int32(i))
            guard m >= 0 && m <= 127 else { continue }
            let vel = min(120, max(40, Int(64 + conf * 56)))   // confidence → a musical velocity range
            detected.append(DetectedMidiNote(startSec: s, durSec: d, midi: m, vel: vel))
        }
        buildMidiTrack(from: detected, startSeconds: startSeconds, name: name)
    }

    /// Lay a set of detected notes onto a NEW instrument track as one MIDI region. Shared by the
    /// monophonic (cache) and polyphonic (basic-pitch) paths. Main thread.
    private func buildMidiTrack(from detected: [DetectedMidiNote], startSeconds: Double, name: String) {
        stemSeparationProgress = nil
        guard let handle else { return }
        guard !detected.isEmpty else { stemSeparationStatus = "MIDI 변환: 유효한 노트가 없습니다"; return }

        let trackIndex = nc_track_add_instrument(handle)
        guard trackIndex >= 0 else { stemSeparationStatus = "MIDI 변환 실패: 트랙 생성 불가"; return }
        _ = renameTrack(Int(trackIndex), to: name.isEmpty ? "MIDI" : "\(name) MIDI")

        let spanSeconds = max(1.0, (detected.map { $0.startSec + $0.durSec }.max() ?? 4.0))
        var regionBuf = [CChar](repeating: 0, count: 128)
        guard nc_midi_region_add(handle, trackIndex, startSeconds, spanSeconds, &regionBuf, regionBuf.count) else {
            stemSeparationStatus = "MIDI 변환 실패: 리전 생성 불가"; reloadTracks(); return
        }
        let regionId = String(cString: regionBuf)

        // Seconds → beats at the project tempo (region note times are beats from the region start).
        let bps = Double(max(1, Int(nc_project_tempo_bpm(handle)))) / 60.0
        var added = 0
        for n in detected {
            var nb = [CChar](repeating: 0, count: 128)
            if nc_midi_note_add(handle, regionId, Int32(n.midi), n.startSec * bps, n.durSec * bps,
                                Int32(n.vel), &nb, nb.count) { added += 1 }
        }
        reloadTracks(); reloadClips(); reloadMidiRegions(); refreshHistory()
        stemSeparationStatus = "MIDI 변환 완료 — \(added)개 노트 (악기 트랙 생성, 악기를 로드하세요)"
    }

    /// True when the polyphonic transcriber (basic-pitch, a Python helper) can run: the script is bundled
    /// and a python3 is on PATH. Whether `basic_pitch` itself is importable is checked at run time.
    var basicPitchAvailable: Bool {
        let script = Bundle.main.bundlePath + "/Contents/Resources/neuracoust_basic_pitch.py"
        return FileManager.default.fileExists(atPath: script)
            && (FileManager.default.isExecutableFile(atPath: "/usr/bin/python3")
                || FileManager.default.isExecutableFile(atPath: "/opt/homebrew/bin/python3"))
    }

    private var python3Path: String {
        FileManager.default.isExecutableFile(atPath: "/opt/homebrew/bin/python3")
            ? "/opt/homebrew/bin/python3" : "/usr/bin/python3"
    }

    /// Polyphonic audio → MIDI: transcribe chords/piano/guitar with basic-pitch (Spotify's lightweight
    /// note transcription), then lay every detected note onto a new instrument track. Off the main thread.
    func convertClipToMidiPolyphonic(_ clipId: String) {
        guard let handle, stemSeparationProgress == nil else { return }
        if pitchEditorClipId != nil { stemSeparationStatus = "MIDI 변환: 먼저 피치 에디터를 닫아주세요"; return }
        var idx = -1
        let count = Int(nc_clip_count(handle))
        for i in 0..<count {
            var id = [CChar](repeating: 0, count: 128)
            nc_clip_id(handle, Int32(i), &id, id.count)
            if String(cString: id) == clipId { idx = i; break }
        }
        guard idx >= 0 else { return }
        let startSeconds = nc_clip_start_seconds(handle, Int32(idx))
        var nameBuf = [CChar](repeating: 0, count: 256)
        nc_clip_name(handle, Int32(idx), &nameBuf, nameBuf.count)
        let clipName = String(cString: nameBuf)

        let tmpDir = (NSTemporaryDirectory() as NSString).appendingPathComponent("neuracoust-poly-midi-\(UUID().uuidString)")
        try? FileManager.default.createDirectory(atPath: tmpDir, withIntermediateDirectories: true)
        let rawPath = (tmpDir as NSString).appendingPathComponent("clip.wav")
        var err = [CChar](repeating: 0, count: 512)
        guard nc_clip_export_raw_window(handle, clipId, rawPath, &err, err.count) else {
            stemSeparationStatus = "MIDI 변환 실패: 클립을 내보낼 수 없습니다"; return
        }

        let script = Bundle.main.bundlePath + "/Contents/Resources/neuracoust_basic_pitch.py"
        let py = python3Path
        stemSeparationProgress = 0
        stemSeparationStatus = "폴리포닉 MIDI 변환 중… (basic-pitch)"

        DispatchQueue.global(qos: .userInitiated).async {
            let proc = Process()
            proc.executableURL = URL(fileURLWithPath: py)
            proc.arguments = [script, rawPath]
            let pipe = Pipe(); proc.standardOutput = pipe
            let errPipe = Pipe(); proc.standardError = errPipe
            var notes: [DetectedMidiNote] = []
            var errMsg = ""
            var carry = ""
            pipe.fileHandleForReading.readabilityHandler = { fh in
                let d = fh.availableData
                guard !d.isEmpty, let s = String(data: d, encoding: .utf8) else { return }
                carry += s
                while let nl = carry.firstIndex(of: "\n") {
                    let line = String(carry[carry.startIndex..<nl]); carry = String(carry[carry.index(after: nl)...])
                    let p = line.split(separator: " ").map(String.init)
                    if p.first == "NOTE", p.count >= 5, let pitch = Int(p[1]), let st = Double(p[2]),
                       let du = Double(p[3]), let vel = Int(p[4]), pitch >= 0, pitch <= 127 {
                        notes.append(DetectedMidiNote(startSec: st, durSec: max(0.02, du), midi: pitch,
                                                      vel: min(127, max(1, vel))))
                    } else if p.first == "ERROR" {
                        errMsg = line.replacingOccurrences(of: "ERROR ", with: "")
                    }
                }
            }
            do { try proc.run() } catch {
                DispatchQueue.main.async {
                    self.stemSeparationProgress = nil
                    self.stemSeparationStatus = "폴리포닉 변환 실행 실패: \(error.localizedDescription)"
                }
                return
            }
            proc.waitUntilExit()
            pipe.fileHandleForReading.readabilityHandler = nil
            if errMsg.isEmpty {
                let e = errPipe.fileHandleForReading.readDataToEndOfFile()
                if proc.terminationStatus != 0, let s = String(data: e, encoding: .utf8), !s.isEmpty {
                    errMsg = s.contains("basic_pitch") ? "basic-pitch 미설치 (pip install basic-pitch)" : String(s.prefix(120))
                }
            }
            DispatchQueue.main.async {
                self.stemSeparationProgress = nil
                if notes.isEmpty {
                    self.stemSeparationStatus = errMsg.isEmpty ? "폴리포닉 변환: 검출된 노트가 없습니다"
                        : "폴리포닉 변환 실패: \(errMsg)"
                    return
                }
                self.buildMidiTrack(from: notes, startSeconds: startSeconds, name: clipName)
            }
        }
    }

    /// Neural noise-floor removal (Facebook DNS denoiser, bundled helper). Exports the clip's played
    /// window, denoises it off the main thread, then repoints the clip at the cleaned WAV (length
    /// preserved). Runs BEFORE clip gain in intent: clean first, then boost, so the boost lifts signal
    /// not hiss. `mix` 0…1 is the denoise strength (1 = full). Reuses the background-DSP status fields.
    func denoiseClip(_ clipId: String, mix: Double = 1.0) {
        guard let handle, stemSeparationProgress == nil else { return }
        let helper = Bundle.main.bundlePath + "/Contents/MacOS/neuracoust_denoiser"
        guard FileManager.default.isExecutableFile(atPath: helper),
              FileManager.default.fileExists(atPath: Bundle.main.bundlePath + "/Contents/Resources/denoiser.pt") else {
            stemSeparationStatus = "디노이저가 번들에 없습니다 (tools/export_denoiser_torchscript.py 실행 필요)"
            Diagnostics.shared.log("denoise: helper/model not bundled")
            return
        }
        let tmpDir = (NSTemporaryDirectory() as NSString).appendingPathComponent("neuracoust-denoise-\(UUID().uuidString)")
        try? FileManager.default.createDirectory(atPath: tmpDir, withIntermediateDirectories: true)
        let inPath = (tmpDir as NSString).appendingPathComponent("in.wav")
        let outPath = (tmpDir as NSString).appendingPathComponent("out.wav")
        var err = [CChar](repeating: 0, count: 512)
        guard nc_clip_export_raw_window(handle, clipId, inPath, &err, err.count) else {
            stemSeparationStatus = "디노이즈 준비 실패: \(String(cString: err))"; return
        }
        stemSeparationProgress = 0
        stemSeparationStatus = "노이즈 제거 중…"
        DispatchQueue.global(qos: .userInitiated).async {
            let proc = Process()
            proc.executableURL = URL(fileURLWithPath: helper)
            proc.arguments = [inPath, outPath, "--mix", String(format: "%.3f", max(0.0, min(1.0, mix)))]
            let pipe = Pipe(); proc.standardOutput = pipe
            var errMsg = "", carry = ""
            pipe.fileHandleForReading.readabilityHandler = { fh in
                let d = fh.availableData
                guard !d.isEmpty, let s = String(data: d, encoding: .utf8) else { return }
                carry += s
                while let nl = carry.firstIndex(of: "\n") {
                    let line = String(carry[carry.startIndex..<nl]); carry = String(carry[carry.index(after: nl)...])
                    let p = line.split(separator: " ", maxSplits: 1).map(String.init)
                    if p.first == "PROGRESS", p.count > 1, let v = Double(p[1]) {
                        DispatchQueue.main.async { self.stemSeparationProgress = v; self.stemSeparationStatus = "노이즈 제거 중… \(Int(v * 100))%" }
                    } else if p.first == "ERROR" { errMsg = p.count > 1 ? p[1] : "알 수 없는 오류" }
                }
            }
            do { try proc.run() } catch {
                DispatchQueue.main.async { self.finishDenoise(clipId, false, "실행 실패: \(error.localizedDescription)", outPath) }; return
            }
            proc.waitUntilExit(); pipe.fileHandleForReading.readabilityHandler = nil
            let ok = proc.terminationStatus == 0 && FileManager.default.fileExists(atPath: outPath)
            DispatchQueue.main.async { self.finishDenoise(clipId, ok, errMsg, outPath) }
        }
    }

    /// VocAlign: time-warp a dub clip onto a reference (lead) clip's timing (MFCC-DTW), formant-preserving,
    /// offline print + repoint. Synchronous — bounded DSP, fast for phrase-length clips.
    func alignClipToReference(_ dubId: String, referenceClipId refId: String) {
        guard let handle else { return }
        var err = [CChar](repeating: 0, count: 256)
        if nc_clip_align_to_reference(handle, dubId, refId, alignStrength, formantPreserve ? 1 : 0, &err, err.count) {
            reloadClips(); refreshHistory()
            stemSeparationStatus = "리드에 정렬 완료"
        } else {
            let m = String(cString: err)
            stemSeparationStatus = "정렬 실패: \(m)"
            if !m.isEmpty { Diagnostics.shared.log("vocal align: \(m)") }
        }
    }

    private func finishDenoise(_ clipId: String, _ ok: Bool, _ error: String, _ outPath: String) {
        stemSeparationProgress = nil
        guard ok, let handle else {
            stemSeparationStatus = "노이즈 제거 실패: \(error.isEmpty ? "알 수 없는 오류" : error)"
            Diagnostics.shared.log("denoise: \(error)"); return
        }
        var err = [CChar](repeating: 0, count: 512)
        if nc_clip_repoint_to_window_wav(handle, clipId, outPath, "노이즈 제거", &err, err.count) {
            reloadClips(); refreshHistory()
            stemSeparationStatus = "노이즈 제거 완료"
        } else {
            stemSeparationStatus = "노이즈 제거 반영 실패: \(String(cString: err))"
        }
    }

    // MARK: - ARA (Melodyne 등)

    /// The installed ARA-capable plug-ins, as (name, path). Read from the full catalog, so it does
    /// not disturb whatever filter the plug-in browser is showing.
    ///
    /// CACHED, and that is not an optimisation detail: the timeline asks for this inside its view
    /// body, which SwiftUI re-evaluates on every poll tick, and answering means walking all ~1,000
    /// scanned plug-ins and marshalling strings out of the engine 30 times a second. The answer only
    /// changes when the plug-in scan does.
    private var araPluginCache: [(name: String, path: String)]? = nil

    func araPlugins() -> [(name: String, path: String)] {
        if let araPluginCache { return araPluginCache }
        guard let handle else { return [] }
        let count = Int(nc_ara_plugin_count(handle))
        let list = (0..<count).map { index in
            let i = Int32(index)
            return (name: readEngineString { nc_ara_plugin_name(handle, i, $0, $1) },
                    path: readEngineString { nc_ara_plugin_path(handle, i, $0, $1) })
        }.filter { !$0.name.isEmpty && !$0.path.isEmpty }
        araPluginCache = list
        return list
    }

    /// True when this clip already carries committed ARA edits — the menu says 편집 rather than 열기,
    /// and offers to drop them.
    func clipHasAraEdits(_ clipId: String) -> Bool {
        guard let handle else { return false }
        return nc_clip_has_ara_edits(handle, clipId)
    }

    var araEditorOpen: Bool { araEditor != nil }

    /// Opens the plug-in's own editor over a clip. In-process — see AraEditorWindowController.
    func openAraEditor(clipId: String, pluginName: String, pluginPath: String) {
        guard let handle else { return }
        guard araEditor == nil else {
            stemSeparationStatus = "ARA 편집 창이 이미 열려 있습니다"
            return
        }
        var error = [CChar](repeating: 0, count: 512)
        guard nc_ara_open(handle, clipId, pluginName, pluginPath, &error, error.count) else {
            let message = String(cString: error)
            stemSeparationStatus = "ARA 열기 실패: \(message.isEmpty ? "알 수 없는 오류" : message)"
            Diagnostics.shared.log("ara open: \(message)")
            return
        }

        let editor = AraEditorWindowController()
        let clipName = clips.first(where: { $0.id == clipId })?.name ?? clipId
        let shown = editor.present(
            title: "\(pluginName) — \(clipName)",
            attach: { [weak self] view in
                guard let self, let handle = self.handle else { return nil }
                var width: Int32 = 0
                var height: Int32 = 0
                var attachError = [CChar](repeating: 0, count: 512)
                guard nc_ara_attach_editor(handle, Unmanaged.passUnretained(view).toOpaque(),
                                           &width, &height, &attachError, attachError.count) else {
                    self.stemSeparationStatus = "ARA 에디터를 열지 못했습니다: \(String(cString: attachError))"
                    return nil
                }
                return NSSize(width: CGFloat(width), height: CGFloat(height))
            },
            onApply: { [weak self] in self?.commitAraEdits() },
            onCancel: { [weak self] in self?.cancelAraEdits() })

        if shown {
            araEditor = editor
        } else {
            nc_ara_close(handle)
        }
    }

    /// Archives the plug-in's edits onto the clip and prints them into its audio. Blocking — the
    /// render runs the plug-in over the whole clip — so the status line is set before it starts.
    private func commitAraEdits() {
        araEditor = nil
        guard let handle else { return }
        stemSeparationStatus = "ARA 편집을 적용하는 중…"
        // Detach the editor NOW, synchronously: the caller tears the window down as soon as this
        // returns, and the plug-in must be off that NSView before it goes.
        nc_ara_detach_editor(handle)
        // The render runs the plug-in over the whole clip and ARA is main-thread-only, so this
        // blocks — there is no background version of it. Hand the run loop one turn first so the
        // status line above is actually on screen before everything stops.
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            var error = [CChar](repeating: 0, count: 512)
            if nc_ara_commit(handle, &error, error.count) {
                self.reloadClips(); self.refreshHistory()
                self.stemSeparationStatus = "ARA 편집을 적용했습니다"
            } else {
                let message = String(cString: error)
                self.stemSeparationStatus = "ARA 적용 실패: \(message.isEmpty ? "알 수 없는 오류" : message)"
            }
            nc_ara_close(handle)
        }
    }

    private func cancelAraEdits() {
        araEditor = nil
        guard let handle else { return }
        nc_ara_close(handle)
        stemSeparationStatus = ""
    }

    /// Points the clip back at the audio it had before any ARA edit, and forgets the archive.
    func clearAraEdits(_ clipId: String) {
        guard let handle else { return }
        var error = [CChar](repeating: 0, count: 512)
        if nc_clip_clear_ara_edits(handle, clipId, &error, error.count) {
            reloadClips(); refreshHistory()
            stemSeparationStatus = "ARA 편집을 제거했습니다"
        } else {
            stemSeparationStatus = "ARA 편집 제거 실패: \(String(cString: error))"
        }
    }

    // MARK: - Pitch editor (Melodyne note mode + Serato anchor time-map mode)

    enum PitchEditorMode: String, CaseIterable {
        case melodyne = "멜로다인", anchor = "타임 워프"
        // rawValue stays as the internal key (persisted); `title` is what the UI shows. The old
        // labels named other companies' products — a note-based pitch editor and an anchor-based
        // time-warp are what these actually are.
        var title: String {
            switch self {
            case .melodyne: return "노트 편집"
            case .anchor: return "타임 워프"
            }
        }
    }
    /// Melodyne's tool palette, reproduced. Melodyne groups these into six buttons where a
    /// click-and-hold opens a fly-out of related tools; the same grouping is kept here (`group`),
    /// so the toolbar shows six buttons and the sub-tools live in each button's menu.
    ///
    /// Every tool here is one the offline print actually performs — nothing is a placeholder.
    enum PitchEditTool: String, CaseIterable {
        case main = "메인"
        case hand = "손"
        case zoom = "돋보기"
        case pitch = "피치"
        case modulation = "모듈레이션"
        case drift = "드리프트"
        case formant = "포먼트"
        case amplitude = "레벨"
        case mute = "음소거"
        case time = "타임"
        case attack = "어택"
        case separate = "분리"

        /// Which toolbar button this tool lives under.
        enum Group: String, CaseIterable {
            case main = "메인", hand = "손", zoom = "돋보기", pitch = "피치", formant = "포먼트"
            case amplitude = "레벨", time = "타임", separate = "분리"
        }

        var group: Group {
            switch self {
            case .main: return .main
            case .hand: return .hand
            case .zoom: return .zoom
            case .pitch, .modulation, .drift: return .pitch
            case .formant: return .formant
            case .amplitude, .mute: return .amplitude
            case .time, .attack: return .time
            case .separate: return .separate
            }
        }

        var symbol: String {
            switch self {
            case .main: return "arrow.up.and.down.and.arrow.left.and.right"
            case .hand: return "hand.raised"
            case .zoom: return "plus.magnifyingglass"
            case .pitch: return "music.note"
            case .modulation: return "waveform"
            case .drift: return "arrow.up.forward"
            case .formant: return "waveform.path.ecg"
            case .amplitude: return "speaker.wave.2"
            case .mute: return "speaker.slash"
            case .time: return "arrow.left.and.right"
            case .attack: return "bolt.horizontal"
            case .separate: return "scissors"
            }
        }

        /// What dragging does with this tool held — the toolbar's tooltip.
        var hint: String {
            switch self {
            case .main: return "잡은 위치가 결정: 가운데=이동, 위아래=음정, 가장자리=길이"
            case .hand: return "손 — 편집 없이 화면을 잡고 이동합니다. 위/아래로 끌면 음정 축, 좌우로 끌면 시간 축이 스크롤됩니다. ‘좌우’ 이동은 돋보기로 **가로 확대**한 상태에서만 움직일 여지가 생깁니다(확대 안 하면 클립 전체가 이미 보여 움직일 곳이 없음)."
            case .zoom: return "돋보기 — 드래그로 상자를 그리면 그 영역이 화면을 가득 채우게 확대됩니다. 짧게 **클릭**하면 그 지점 기준 단계 확대, **⌥클릭**은 축소."
            case .pitch: return "피치 — 노트를 위/아래로 끌어 음정을 바꿉니다. 스냅 모드가 ‘반음’이면 반음 격자, ‘스케일’이면 조(調)의 음, ‘끄기’면 연속으로 움직입니다. **여러 노트를 선택**하면 함께 이동합니다."
            case .modulation: return "모듈레이션(비브라토) — 위로 끌면 그 음의 흔들림을 깊게, 아래로 끌면 얕게. 끝까지 내리면 완전히 곧은 음이 됩니다(원래 비브라토를 배율로 조절)."
            case .drift: return "드리프트 — 음 안에서 느리게 미끄러지는 피치(스쿱·처짐)를 위/아래로 키우거나 줄입니다. 끝까지 내리면 드리프트 제거."
            case .formant: return "포먼트 — 위/아래로 끌어 음색(성별·톤)만 옮깁니다. **음정은 그대로** 두어 음정을 바꿔도 치프멍크가 되지 않게 합니다."
            case .amplitude: return "레벨 — 노트를 위/아래로 끌어 그 음만 크게/작게 합니다(±24 dB). **여러 노트를 선택**하면 함께 조절됩니다."
            case .mute: return "음소거 — 노트를 클릭해 껐다 켭니다. 꺼도 위치는 그대로 보여 다시 켤 수 있습니다."
            case .time: return "타임 — 노트 **가운데**를 좌우로 끌면 위치가, **가장자리**를 끌면 길이가 바뀝니다. 여러 노트를 선택하면 함께 이동합니다."
            case .attack: return "어택 — 좌우로 끌어 음의 시작을 날카롭게/부드럽게. 1.0이 원본이며, 오른쪽이 더 날카롭습니다."
            case .separate: return "가위(분리) — 노트를 **한 번 클릭**한 자리에서 둘로 자릅니다. 커서가 십자(절단점)로 바뀝니다."
            }
        }
    }

    // Melodyne-style detection modes. rawValue matches nc_clip_detect_notes's `mode` argument.
    enum DetectionMode: Int, CaseIterable { case melodic = 0, polyphonic = 1, percussive = 2
        var label: String { switch self { case .melodic: return "멜로딕"; case .polyphonic: return "폴리포닉"; case .percussive: return "퍼커시브" } }
    }

    struct PitchNote: Identifiable, Equatable {
        let id: Int             // index into the engine's detected-note cache
        var startSeconds: Double
        var durationSeconds: Double
        var detectedMidi: Double
        var offsetSemitones: Double
        var timeOffsetSeconds: Double
        var durationScale: Double
        var confidence: Double
        var gainDb: Double = 0
        var muted: Bool = false
        var formantSemitones: Double = 0
        var attackSpeed: Double = 1
        var modulationScale: Double = 1
        var driftScale: Double = 1
        /// True when anything on this note has been touched — the view dims untouched blobs.
        var edited: Bool {
            abs(offsetSemitones) > 0.001 || abs(timeOffsetSeconds) > 0.0001 ||
            abs(durationScale - 1) > 0.001 || abs(gainDb) > 0.01 || muted ||
            abs(formantSemitones) > 0.01 || abs(attackSpeed - 1) > 0.01 ||
            abs(modulationScale - 1) > 0.01 || abs(driftScale - 1) > 0.01
        }
        var editedMidi: Double { detectedMidi + offsetSemitones }
        var editedStartSeconds: Double { startSeconds + timeOffsetSeconds }
        var editedDurationSeconds: Double { durationSeconds * durationScale }
    }

    @Published var pitchEditorClipId: String? = nil
    /// Waveform amplitude in the pitch editor. Scales the DRAWN signal only — the view zoom scales
    /// the screen; this is for reading a quiet passage's shape while editing.
    @Published var pitchWaveformGain: Double = 1.0
    @Published var pitchEditorMode: PitchEditorMode = .melodyne
    @Published var pitchEditTool: PitchEditTool = .main
    @Published var detectionMode: DetectionMode = .melodic
    @Published var useCrepe: Bool = false   // CREPE neural detection (precise, async) vs built-in YIN
    @Published var useCrepeTiny: Bool = false  // tiny model: ~40x smaller, much faster, slightly less accurate
    @Published var formantPreserve: Bool = true   // keep timbre through a pitch shift (no chipmunk); off = raw shift
    @Published var alignStrength: Double = 1.0   // VocAlign amount: 1 = fully snap to the lead, 0 = no change

    /// The CREPE helper + full model are bundled (export via tools/export_crepe_torchscript.py).
    var crepeAvailable: Bool {
        let base = Bundle.main.bundlePath
        return FileManager.default.isExecutableFile(atPath: base + "/Contents/MacOS/neuracoust_pitch_detector")
            && FileManager.default.fileExists(atPath: base + "/Contents/Resources/crepe_full.pt")
    }
    /// The tiny model is optional — the fast toggle only offers it when it was bundled.
    var crepeTinyAvailable: Bool {
        FileManager.default.fileExists(atPath: Bundle.main.bundlePath + "/Contents/Resources/crepe_tiny.pt")
    }
    /// Which CREPE model file to run — tiny when the user picked fast AND it's present, else full.
    private var crepeModelPath: String {
        let res = Bundle.main.bundlePath + "/Contents/Resources/"
        return (useCrepeTiny && crepeTinyAvailable) ? res + "crepe_tiny.pt" : res + "crepe_full.pt"
    }
    @Published private(set) var pitchEditorClipDuration: Double = 1.0
    @Published private(set) var pitchEditorClipStartSeconds: Double = 0.0
    @Published var pitchEditorTimelineSync: Bool = true
    let pitchEditorClock = PlayheadClock()
    @Published private(set) var pitchNotes: [PitchNote] = []
    /// Selected note indices in the pitch editor. Empty = act on the whole clip; non-empty = the
    /// macros and batch drags act only on these (Melodyne-style multi-note editing).
    @Published var selectedPitchNotes: Set<Int> = []
    /// The note indices a macro should touch: the selection if any, else every note.
    private var targetPitchNoteIndices: [Int] {
        let all = Array(pitchNotes.indices)
        if selectedPitchNotes.isEmpty { return all }
        return selectedPitchNotes.filter { $0 >= 0 && $0 < pitchNotes.count }.sorted()
    }
    private var pitchDetectionGeneration = 0
    @Published private(set) var pitchClipPeaks: [SIMD2<Float>] = []   // (min,max) per column, clip window
    // Percussive mode: transient index → its new (dragged) start time in seconds.
    @Published var percussiveTimeEdits: [Int: Double] = [:]
    // Anchor mode: matched normalized (source, dest) positions in 0…1.
    @Published var timeMapAnchors: [SIMD2<Double>] = []
    @Published var pitchEditTimeRatio: Double = 1.0
    @Published var pitchEditSemitones: Double = 0.0

    /// Open the editor for a clip: cache its detected notes (Melodyne) and clip length (both modes).
    func openPitchEditor(_ clipId: String) {
        guard let handle else { return }
        // Opening from a clip's context menu establishes that clip as the editor
        // owner. A previous track's asynchronous analysis must never remain visible.
        selectedClipIds = [clipId]
        pitchEditorClipId = clipId
        timeMapAnchors = []
        pitchEditTimeRatio = 1.0
        pitchEditSemitones = 0.0
        // Clip length + waveform for the editor's time axis.
        pitchClipPeaks = []
        for i in 0..<Int(nc_clip_count(handle)) {
            var id = [CChar](repeating: 0, count: 128)
            nc_clip_id(handle, Int32(i), &id, id.count)
            if String(cString: id) == clipId {
                pitchEditorClipDuration = max(0.1, nc_clip_duration_seconds(handle, Int32(i)))
                pitchEditorClipStartSeconds = nc_clip_start_seconds(handle, Int32(i))
                loadClipWaveform(clipIndex: i)
                break
            }
        }
        reloadDetectedNotes(clipId)
    }

    /// Load the clip WINDOW's waveform as ~600 (min,max) columns, for drawing behind the note blobs.
    private func loadClipWaveform(clipIndex ci: Int) {
        guard let handle else { return }
        var pathBuf = [CChar](repeating: 0, count: 1024)
        nc_clip_source_path(handle, Int32(ci), &pathBuf, pathBuf.count)
        let path = String(cString: pathBuf)
        guard !path.isEmpty else { return }
        let total = nc_waveform_duration_seconds(handle, path)
        let count = Int(nc_waveform_peak_count(handle, path))
        guard count > 0, total > 0 else { return }
        var mins = [Float](repeating: 0, count: count)
        var maxs = [Float](repeating: 0, count: count)
        guard nc_waveform_peaks(handle, path, &mins, &maxs, Int32(count)) else { return }
        let offset = nc_clip_source_offset_seconds(handle, Int32(ci))
        let dur = nc_clip_duration_seconds(handle, Int32(ci))
        let lo = max(0, min(count, Int((offset / total) * Double(count))))
        let hi = max(lo + 1, min(count, Int(((offset + dur) / total) * Double(count))))
        let cols = 600
        var out: [SIMD2<Float>] = []
        out.reserveCapacity(cols)
        for c in 0..<cols {
            let s = lo + (hi - lo) * c / cols
            let e = max(s + 1, lo + (hi - lo) * (c + 1) / cols)
            var mn: Float = 0, mx: Float = 0
            for k in s..<min(e, count) { mn = min(mn, mins[k]); mx = max(mx, maxs[k]) }
            out.append(SIMD2(mn, mx))
        }
        pitchClipPeaks = out
    }

    /// Re-run detection for the open clip with the current `detectionMode` (called when the sub-mode
    /// picker changes). Percussive returns rhythmic markers (no pitch); melodic/polyphonic return notes.
    func redetectNotes() {
        guard let clipId = pitchEditorClipId else { return }
        reloadDetectedNotes(clipId)
    }

    private func reloadDetectedNotes(_ clipId: String) {
        guard let handle else { return }
        pitchDetectionGeneration &+= 1
        nc_detect_notes_reset(handle)
        nc_detect_notes_bind_clip(handle, clipId)
        pitchNotes = []
        percussiveTimeEdits = [:]
        // Polyphonic = separate the clip (Demucs) then detect each part — async, off the main thread.
        if detectionMode == .polyphonic { redetectPolyphonic(clipId); return }
        // Melodic with the CREPE neural detector (precise, async subprocess) if the user opted in.
        if useCrepe && detectionMode == .melodic && crepeAvailable { redetectWithCrepe(clipId); return }
        _ = nc_clip_detect_notes(handle, clipId, Int32(detectionMode.rawValue))
        loadNotesFromCache()
    }

    /// Melodic detection via the CREPE neural helper: export the clip window, run the detector, parse
    /// its pitch track, and segment it (same Viterbi + segmenter as YIN). Falls back to YIN on failure.
    private func redetectWithCrepe(_ clipId: String) {
        guard let handle else { return }
        let generation = pitchDetectionGeneration
        let helper = Bundle.main.bundlePath + "/Contents/MacOS/neuracoust_pitch_detector"
        let tmpDir = (NSTemporaryDirectory() as NSString).appendingPathComponent("neuracoust-crepe-\(UUID().uuidString)")
        try? FileManager.default.createDirectory(atPath: tmpDir, withIntermediateDirectories: true)
        let rawPath = (tmpDir as NSString).appendingPathComponent("clip.wav")
        var err = [CChar](repeating: 0, count: 512)
        guard nc_clip_export_raw_window(handle, clipId, rawPath, &err, err.count) else {
            _ = nc_clip_detect_notes(handle, clipId, 0); loadNotesFromCache(); return
        }
        let modelPath = crepeModelPath
        let tiny = useCrepeTiny && crepeTinyAvailable
        stemSeparationProgress = 0
        stemSeparationStatus = tiny ? "빠른 검출(CREPE tiny) 중…" : "정밀 검출(CREPE) 중…"
        DispatchQueue.global(qos: .userInitiated).async {
            let proc = Process()
            proc.executableURL = URL(fileURLWithPath: helper)
            proc.arguments = [rawPath, "--model", modelPath]
            let pipe = Pipe(); proc.standardOutput = pipe
            var times: [Double] = [], hzs: [Double] = [], confs: [Double] = []
            var carry = ""
            pipe.fileHandleForReading.readabilityHandler = { fh in
                let d = fh.availableData
                guard !d.isEmpty, let s = String(data: d, encoding: .utf8) else { return }
                carry += s
                while let nl = carry.firstIndex(of: "\n") {
                    let line = String(carry[carry.startIndex..<nl]); carry = String(carry[carry.index(after: nl)...])
                    let p = line.split(separator: " ").map(String.init)
                    if p.first == "PITCH", p.count >= 4, let t = Double(p[1]), let hz = Double(p[2]), let c = Double(p[3]) {
                        times.append(t); hzs.append(hz); confs.append(c)
                    }
                }
            }
            do { try proc.run() } catch {
                DispatchQueue.main.async {
                    guard self.pitchEditorClipId == clipId, self.pitchDetectionGeneration == generation else { return }
                    self.stemSeparationProgress = nil; _ = nc_clip_detect_notes(handle, clipId, 0); self.loadNotesFromCache()
                }
                return
            }
            proc.waitUntilExit(); pipe.fileHandleForReading.readabilityHandler = nil
            DispatchQueue.main.async {
                guard self.pitchEditorClipId == clipId, self.pitchDetectionGeneration == generation else { return }
                self.stemSeparationProgress = nil
                guard let h = self.handle, !times.isEmpty else {
                    if let h = self.handle { _ = nc_clip_detect_notes(h, clipId, 0); self.loadNotesFromCache() }
                    return
                }
                _ = times.withUnsafeBufferPointer { tp in hzs.withUnsafeBufferPointer { hp in confs.withUnsafeBufferPointer { cp in
                    nc_segment_pitch_track(h, tp.baseAddress, hp.baseAddress, cp.baseAddress, Int32(times.count))
                }}}
                nc_detect_notes_bind_clip(h, clipId)
                self.loadNotesFromCache()
                self.stemSeparationStatus = "정밀 검출 완료 — \(self.pitchNotes.count)개 노트"
            }
        }
    }

    private func loadNotesFromCache() {
        guard let handle else { return }
        // Note indices change on every (re)detection, so a stale selection would point at the wrong
        // blobs — clear it whenever the note list is rebuilt.
        selectedPitchNotes = []
        let count = Int(nc_clip_note_count(handle))
        var notes: [PitchNote] = []
        for i in 0..<count {
            notes.append(PitchNote(id: i,
                                   startSeconds: nc_clip_note_start_seconds(handle, Int32(i)),
                                   durationSeconds: nc_clip_note_duration_seconds(handle, Int32(i)),
                                   detectedMidi: nc_clip_note_detected_midi(handle, Int32(i)),
                                   offsetSemitones: nc_clip_note_offset_semitones(handle, Int32(i)),
                                   timeOffsetSeconds: nc_clip_note_time_offset_seconds(handle, Int32(i)),
                                   durationScale: nc_clip_note_duration_scale(handle, Int32(i)),
                                   confidence: nc_clip_note_confidence(handle, Int32(i)),
                                   gainDb: nc_clip_note_gain_db(handle, Int32(i)),
                                   muted: nc_clip_note_muted(handle, Int32(i)),
                                   formantSemitones: nc_clip_note_formant_semitones(handle, Int32(i)),
                                   attackSpeed: nc_clip_note_attack_speed(handle, Int32(i)),
                                   modulationScale: nc_clip_note_modulation_scale(handle, Int32(i)),
                                   driftScale: nc_clip_note_drift_scale(handle, Int32(i))))
        }
        pitchNotes = notes
    }

    /// True polyphonic detection: export the clip window, run the Demucs helper on it, then detect
    /// simultaneous pitches with Basic Pitch. If that optional model is unavailable, fall back to
    /// Demucs-assisted per-part detection (better than mix-level YIN, but not chord-complete).
    private func redetectPolyphonic(_ clipId: String) {
        guard let handle else { return }
        let generation = pitchDetectionGeneration
        let script = Bundle.main.bundlePath + "/Contents/Resources/neuracoust_basic_pitch.py"
        guard FileManager.default.fileExists(atPath: script) else {
            redetectPolyphonicByStems(clipId); return
        }
        let tmpDir = (NSTemporaryDirectory() as NSString).appendingPathComponent("neuracoust-poly-pitch-\(UUID().uuidString)")
        try? FileManager.default.createDirectory(atPath: tmpDir, withIntermediateDirectories: true)
        let rawPath = (tmpDir as NSString).appendingPathComponent("clip.wav")
        var err = [CChar](repeating: 0, count: 512)
        guard nc_clip_export_raw_window(handle, clipId, rawPath, &err, err.count) else {
            redetectPolyphonicByStems(clipId); return
        }
        stemSeparationProgress = 0
        stemSeparationStatus = "폴리포닉 화음 분석 중…"
        let py = python3Path
        DispatchQueue.global(qos: .userInitiated).async {
            let proc = Process()
            proc.executableURL = URL(fileURLWithPath: py)
            proc.arguments = [script, rawPath]
            let pipe = Pipe(); proc.standardOutput = pipe
            var found: [DetectedMidiNote] = []
            var carry = ""
            pipe.fileHandleForReading.readabilityHandler = { fh in
                let data = fh.availableData
                guard !data.isEmpty, let text = String(data: data, encoding: .utf8) else { return }
                carry += text
                while let newline = carry.firstIndex(of: "\n") {
                    let line = String(carry[..<newline])
                    carry = String(carry[carry.index(after: newline)...])
                    let p = line.split(separator: " ").map(String.init)
                    if p.first == "NOTE", p.count >= 5,
                       let midi = Int(p[1]), let start = Double(p[2]),
                       let duration = Double(p[3]), let velocity = Int(p[4]) {
                        found.append(DetectedMidiNote(startSec: start, durSec: duration,
                                                      midi: midi, vel: velocity))
                    }
                }
            }
            do { try proc.run() } catch {
                DispatchQueue.main.async {
                    guard self.pitchEditorClipId == clipId, self.pitchDetectionGeneration == generation else { return }
                    self.redetectPolyphonicByStems(clipId)
                }
                return
            }
            proc.waitUntilExit()
            pipe.fileHandleForReading.readabilityHandler = nil
            DispatchQueue.main.async {
                guard self.pitchEditorClipId == clipId, self.pitchDetectionGeneration == generation else { return }
                guard proc.terminationStatus == 0, !found.isEmpty, let h = self.handle else {
                    self.redetectPolyphonicByStems(clipId); return
                }
                nc_detect_notes_reset(h)
                for note in found {
                    nc_detect_notes_add_note(h, note.startSec, note.durSec, Double(note.midi),
                                             Double(note.vel) / 127.0)
                }
                nc_detect_notes_bind_clip(h, clipId)
                self.stemSeparationProgress = nil
                self.loadNotesFromCache()
                self.stemSeparationStatus = "폴리포닉 화음 분석 완료 — \(self.pitchNotes.count)개 노트"
            }
        }
    }

    private func redetectPolyphonicByStems(_ clipId: String) {
        guard let handle else { return }
        let generation = pitchDetectionGeneration
        let helper = Bundle.main.bundlePath + "/Contents/MacOS/neuracoust_stem_separator"
        guard FileManager.default.isExecutableFile(atPath: helper) else {
            stemSeparationProgress = nil
            _ = nc_clip_detect_notes(handle, clipId, 1)
            loadNotesFromCache()
            stemSeparationStatus = "내장 폴리포닉 분석 완료 — \(pitchNotes.count)개 노트"
            return
        }
        let tmpDir = (NSTemporaryDirectory() as NSString).appendingPathComponent("neuracoust-poly-\(UUID().uuidString)")
        try? FileManager.default.createDirectory(atPath: tmpDir, withIntermediateDirectories: true)
        let rawPath = (tmpDir as NSString).appendingPathComponent("clip.wav")
        var err = [CChar](repeating: 0, count: 512)
        guard nc_clip_export_raw_window(handle, clipId, rawPath, &err, err.count) else {
            _ = nc_clip_detect_notes(handle, clipId, 0); loadNotesFromCache(); return
        }
        let outDir = (tmpDir as NSString).appendingPathComponent("stems")
        stemSeparationProgress = 0
        stemSeparationStatus = "폴리포닉 분석 준비 중…"

        DispatchQueue.global(qos: .userInitiated).async {
            let proc = Process()
            proc.executableURL = URL(fileURLWithPath: helper)
            proc.arguments = [rawPath, outDir, "--stem-prefix", "poly"]
            let pipe = Pipe(); proc.standardOutput = pipe
            var stems: [(String, String)] = []; var carry = ""
            pipe.fileHandleForReading.readabilityHandler = { fh in
                let d = fh.availableData
                guard !d.isEmpty, let s = String(data: d, encoding: .utf8) else { return }
                carry += s
                while let nl = carry.firstIndex(of: "\n") {
                    let line = String(carry[carry.startIndex..<nl]); carry = String(carry[carry.index(after: nl)...])
                    let parts = line.split(separator: " ", maxSplits: 2).map(String.init)
                    if parts.first == "PROGRESS", parts.count > 1, let p = Double(parts[1]) {
                        DispatchQueue.main.async {
                            guard self.pitchEditorClipId == clipId, self.pitchDetectionGeneration == generation else { return }
                            self.stemSeparationProgress = p; self.stemSeparationStatus = "폴리포닉 분석 중… \(Int(p * 100))%"
                        }
                    } else if parts.first == "STEM", parts.count >= 3 { stems.append((parts[1], parts[2])) }
                }
            }
            do { try proc.run() } catch {
                DispatchQueue.main.async {
                    guard self.pitchEditorClipId == clipId, self.pitchDetectionGeneration == generation else { return }
                    self.stemSeparationProgress = nil; _ = nc_clip_detect_notes(handle, clipId, 0); self.loadNotesFromCache()
                }
                return
            }
            proc.waitUntilExit(); pipe.fileHandleForReading.readabilityHandler = nil
            DispatchQueue.main.async {
                guard self.pitchEditorClipId == clipId, self.pitchDetectionGeneration == generation else { return }
                self.stemSeparationProgress = nil
                guard let h = self.handle else { return }
                nc_detect_notes_reset(h)
                for (name, path) in stems where name != "Drums" {
                    _ = nc_detect_notes_add_from_file(h, path, 1)
                }
                nc_detect_notes_bind_clip(h, clipId)
                self.loadNotesFromCache()
                self.stemSeparationStatus = "폴리포닉 보조 분석 완료 — \(self.pitchNotes.count)개 노트"
            }
        }
    }

    /// How a dragged note's pitch snaps. Off = continuous (draw any pitch), Chromatic = nearest
    /// semitone, Scale = nearest degree of `musicalKey` (Melodyne's scale snap).
    enum PitchSnapMode: String, CaseIterable, Identifiable {
        case off = "끄기", chromatic = "반음", scale = "스케일"
        var id: String { rawValue }
    }
    @Published var pitchSnapMode: PitchSnapMode = .chromatic

    /// Pitch classes (0–11) allowed by `musicalKey`, e.g. "C" → C major, "Am" → A natural minor.
    /// Falls back to the full chromatic set for anything unparseable.
    var scalePitchClasses: Set<Int> {
        let names = ["C": 0, "C#": 1, "DB": 1, "D": 2, "D#": 3, "EB": 3, "E": 4, "FB": 4,
                     "F": 5, "F#": 6, "GB": 6, "G": 7, "G#": 8, "AB": 8, "A": 9, "A#": 10,
                     "BB": 10, "B": 11, "CB": 11]
        let raw = musicalKey.trimmingCharacters(in: .whitespaces)
        guard !raw.isEmpty else { return Set(0..<12) }
        let isMinor = raw.lowercased().contains("m") && !raw.lowercased().contains("maj")
        // Root = leading letter + optional accidental.
        var root = String(raw.prefix(1)).uppercased()
        if raw.count > 1, "#b♯♭".contains(raw[raw.index(raw.startIndex, offsetBy: 1)]) {
            root += raw[raw.index(raw.startIndex, offsetBy: 1)] == "♯" ? "#"
                  : raw[raw.index(raw.startIndex, offsetBy: 1)] == "♭" ? "b"
                  : String(raw[raw.index(raw.startIndex, offsetBy: 1)])
        }
        guard let rootPc = names[root.uppercased()] else { return Set(0..<12) }
        let major = [0, 2, 4, 5, 7, 9, 11], minor = [0, 2, 3, 5, 7, 8, 10]
        return Set((isMinor ? minor : major).map { ($0 + rootPc) % 12 })
    }

    /// Snap an absolute MIDI pitch to the nearest member of `pitchClasses` (searching outward).
    private func snapMidi(_ midi: Double, to pitchClasses: Set<Int>) -> Double {
        if pitchClasses.count >= 12 { return midi.rounded() }
        let base = Int(midi.rounded())
        for d in 0...12 {
            for cand in [base - d, base + d] where pitchClasses.contains(((cand % 12) + 12) % 12) {
                return Double(cand)
            }
        }
        return midi.rounded()
    }

    /// Apply the active snap mode to a raw dragged offset for a given note.
    func snappedPitchOffset(noteIndex index: Int, rawSemitones raw: Double) -> Double {
        guard index >= 0, index < pitchNotes.count else { return raw }
        let detected = pitchNotes[index].detectedMidi
        switch pitchSnapMode {
        case .off:       return raw
        case .chromatic: return (detected + raw).rounded() - detected
        case .scale:     return snapMidi(detected + raw, to: scalePitchClasses) - detected
        }
    }

    /// Set a note's pitch offset, snapped per `pitchSnapMode` (Melodyne grid / scale).
    func setPitchNoteOffset(_ index: Int, semitones: Double) {
        guard let handle, index >= 0, index < pitchNotes.count else { return }
        let snapped = snappedPitchOffset(noteIndex: index, rawSemitones: semitones)
        nc_clip_note_set_offset(handle, Int32(index), snapped)
        pitchNotes[index].offsetSemitones = snapped
    }

    /// Melodyne "Correct Pitch" macro: move every note's centre toward the nearest scale degree
    /// (or semitone, if no scale) by `intensity` (0 = no change, 1 = fully on the grid). One undo step.
    func correctPitch(intensity: Double) {
        guard let handle else { return }
        let pcs = scalePitchClasses
        let amount = max(0, min(1, intensity))
        for i in targetPitchNoteIndices {
            let target = snapMidi(pitchNotes[i].detectedMidi, to: pcs)
            let corrected = (target - pitchNotes[i].detectedMidi) * amount
            nc_clip_note_set_offset(handle, Int32(i), corrected)
            pitchNotes[i].offsetSemitones = corrected
        }
        nc_history_record_gesture(handle, "피치 보정")
    }

    /// Melodyne "Quantize Time" macro: nudge every note's onset toward the nearest beat-grid line
    /// (`subdivisionsPerBeat`: 1 = 1/4, 2 = 1/8, 4 = 1/16) by `intensity` (0…1). One undo step.
    func quantizeNoteTime(intensity: Double, subdivisionsPerBeat: Int = 4) {
        guard let handle else { return }
        let step = (60.0 / Double(max(1, tempoBpm))) / Double(max(1, subdivisionsPerBeat))
        let amount = max(0, min(1, intensity))
        for i in targetPitchNoteIndices {
            let n = pitchNotes[i]
            let absStart = pitchEditorClipStartSeconds + n.startSeconds + n.timeOffsetSeconds
            let target = (absStart / step).rounded() * step - pitchEditorClipStartSeconds - n.startSeconds
            let moved = n.timeOffsetSeconds + (target - n.timeOffsetSeconds) * amount
            let clamped = min(pitchEditorClipDuration - n.startSeconds - n.durationSeconds,
                              max(-n.startSeconds, moved))
            nc_clip_note_set_time_offset(handle, Int32(i), clamped)
            pitchNotes[i].timeOffsetSeconds = clamped
        }
        nc_history_record_gesture(handle, "타임 퀀타이즈")
    }

    /// Shift the selected notes (or all, if none selected) by whole octaves — the common fix when
    /// detection lands an octave off. One undo step.
    func shiftPitchOctave(_ octaves: Int) {
        guard let handle else { return }
        for i in targetPitchNoteIndices {
            let next = max(-24, min(24, pitchNotes[i].offsetSemitones + Double(octaves) * 12))
            nc_clip_note_set_offset(handle, Int32(i), next)
            pitchNotes[i].offsetSemitones = next
        }
        nc_history_record_gesture(handle, "옥타브 이동")
    }

    func setPitchNoteTimeOffset(_ index: Int, seconds: Double) {
        guard let handle, index >= 0, index < pitchNotes.count else { return }
        let note = pitchNotes[index]
        let clamped = min(pitchEditorClipDuration - note.startSeconds - note.durationSeconds,
                          max(-note.startSeconds, seconds))
        nc_clip_note_set_time_offset(handle, Int32(index), clamped)
        pitchNotes[index].timeOffsetSeconds = clamped
    }

    func setPitchNoteDurationScale(_ index: Int, scale: Double) {
        guard let handle, index >= 0, index < pitchNotes.count else { return }
        let clamped = min(4.0, max(0.25, scale))
        nc_clip_note_set_duration_scale(handle, Int32(index), clamped)
        pitchNotes[index].durationScale = clamped
    }

    func setPitchNoteGainDb(_ index: Int, gainDb: Double) {
        guard let handle, index >= 0, index < pitchNotes.count else { return }
        let clamped = min(24.0, max(-24.0, gainDb))
        nc_clip_note_set_gain_db(handle, Int32(index), clamped)
        pitchNotes[index].gainDb = clamped
    }

    func setPitchNoteMuted(_ index: Int, muted: Bool) {
        guard let handle, index >= 0, index < pitchNotes.count else { return }
        nc_clip_note_set_muted(handle, Int32(index), muted)
        pitchNotes[index].muted = muted
    }

    func setPitchNoteFormant(_ index: Int, semitones: Double) {
        guard let handle, index >= 0, index < pitchNotes.count else { return }
        let clamped = min(24.0, max(-24.0, semitones))
        nc_clip_note_set_formant_semitones(handle, Int32(index), clamped)
        pitchNotes[index].formantSemitones = clamped
    }

    func setPitchNoteAttackSpeed(_ index: Int, speed: Double) {
        guard let handle, index >= 0, index < pitchNotes.count else { return }
        let clamped = min(4.0, max(0.25, speed))
        nc_clip_note_set_attack_speed(handle, Int32(index), clamped)
        pitchNotes[index].attackSpeed = clamped
    }

    func setPitchNoteModulation(_ index: Int, scale: Double) {
        guard let handle, index >= 0, index < pitchNotes.count else { return }
        let clamped = min(4.0, max(0.0, scale))
        nc_clip_note_set_modulation_scale(handle, Int32(index), clamped)
        pitchNotes[index].modulationScale = clamped
    }

    func setPitchNoteDrift(_ index: Int, scale: Double) {
        guard let handle, index >= 0, index < pitchNotes.count else { return }
        let clamped = min(4.0, max(0.0, scale))
        nc_clip_note_set_drift_scale(handle, Int32(index), clamped)
        pitchNotes[index].driftScale = clamped
    }

    /// Melodyne's per-blob reset: everything about this note back to as detected.
    func resetPitchNote(_ index: Int) {
        guard let handle, index >= 0, index < pitchNotes.count else { return }
        nc_clip_note_reset(handle, Int32(index))
        pitchNotes[index].offsetSemitones = 0
        pitchNotes[index].timeOffsetSeconds = 0
        pitchNotes[index].durationScale = 1
        pitchNotes[index].gainDb = 0
        pitchNotes[index].muted = false
        pitchNotes[index].formantSemitones = 0
        pitchNotes[index].attackSpeed = 1
        pitchNotes[index].modulationScale = 1
        pitchNotes[index].driftScale = 1
    }

    /// Every note back to as detected — the "다시 처음부터" the toolbar offers.
    func resetAllPitchNotes() {
        guard let handle else { return }
        for index in pitchNotes.indices { nc_clip_note_reset(handle, Int32(index)) }
        loadNotesFromCache()
    }

    func splitPitchNote(_ index: Int, at localSeconds: Double) {
        guard let handle, nc_clip_note_split(handle, Int32(index), localSeconds) else { return }
        loadNotesFromCache()
    }

    /// Apply the Melodyne per-note edits — renders a new WAV and repoints the clip.
    func applyPitchEdits() {
        guard let handle, let clipId = pitchEditorClipId else { return }
        var err = [CChar](repeating: 0, count: 256)
        if nc_clip_apply_note_edits(handle, clipId, &err, err.count) {
            reloadClips(); refreshHistory()
            // Keep the editor open on the freshly-baked clip instead of closing it, so the user can
            // keep working. Re-detecting on the new audio shows the notes at their applied pitches
            // (offsets folded in, back to 0) — the committed state, ready for the next edit.
            openPitchEditor(clipId)
        } else {
            let m = String(cString: err); if !m.isEmpty { Diagnostics.shared.log("pitch edit: \(m)") }
        }
    }

    /// Apply the Serato-mode anchor time-remap (+ global ratio/pitch) — renders a new WAV.
    func applyTimeMapEdit() {
        guard let handle, let clipId = pitchEditorClipId else { return }
        let src = timeMapAnchors.map { $0.x }
        let dst = timeMapAnchors.map { $0.y }
        var err = [CChar](repeating: 0, count: 256)
        let ok = src.withUnsafeBufferPointer { sp in
            dst.withUnsafeBufferPointer { dp in
                nc_clip_apply_time_map(handle, clipId, pitchEditTimeRatio, pitchEditSemitones,
                                       sp.baseAddress, dp.baseAddress, Int32(src.count),
                                       formantPreserve ? 1 : 0, &err, err.count)
            }
        }
        if ok { reloadClips(); refreshHistory(); closePitchEditor() }
        else { let m = String(cString: err); if !m.isEmpty { Diagnostics.shared.log("time map: \(m)") } }
    }

    /// Export the current pitch-editor result to a standalone WAV the user picks — the clip and
    /// project are left unchanged (unlike 적용). Works for both Melodyne and Serato-anchor modes.
    func exportPitchEditToFile() {
        guard let handle, let clipId = pitchEditorClipId else { return }
        let panel = NSSavePanel()
        panel.nameFieldStringValue = (pitchEditorMode == .melodyne ? "피치편집" : "타임리맵") + ".wav"
        panel.title = "파일로 내보내기"
        panel.begin { [weak self] response in
            guard let self, response == .OK, let url = panel.url else { return }
            var err = [CChar](repeating: 0, count: 512)
            let ok: Bool
            if self.pitchEditorMode == .melodyne {
                ok = nc_clip_export_note_edits(handle, clipId, url.path, &err, err.count)
            } else {
                let src = self.timeMapAnchors.map { $0.x }
                let dst = self.timeMapAnchors.map { $0.y }
                ok = src.withUnsafeBufferPointer { sp in
                    dst.withUnsafeBufferPointer { dp in
                        nc_clip_export_time_map(handle, clipId, self.pitchEditTimeRatio, self.pitchEditSemitones,
                                                sp.baseAddress, dp.baseAddress, Int32(src.count), url.path, &err, err.count)
                    }
                }
            }
            if !ok { let m = String(cString: err); if !m.isEmpty { Diagnostics.shared.log("export: \(m)") } }
        }
    }

    private var pitchPreviewSound: NSSound?
    private var pitchPreviewEngine: AVAudioEngine?
    private var pitchPreviewPlayer: AVAudioPlayerNode?
    private var pitchPreviewTimer: Timer?

    /// Play one region through the same physical CoreAudio device selected by the DAW.
    /// NSSound always follows the macOS default output, which made note clicks appear
    /// silent whenever the DAW was using an interface such as UNiTE-2.
    /// The level a pitch-editor preview should play at, as a linear gain.
    ///
    /// Previews render to a temp WAV and play through their OWN AVAudioEngine, which bypasses the
    /// mixer entirely — so at unity they came out far louder than the same audio during playback
    /// (the monitor alone sits at -12 dB here). Re-apply what the signal would have passed through:
    /// the clip's gain, its track's fader, and the monitor volume.
    private var pitchPreviewGain: Float {
        var db = monitorVolumeDb
        if let clipId = pitchEditorClipId, let clip = clips.first(where: { $0.id == clipId }) {
            db += clip.gainDb
            if let track = tracks.first(where: { $0.name == clip.trackName }) {
                db += track.volumeDb
                if track.muted { return 0 }
            }
        }
        // Post-fader: the summed output also passes the master fader, which this
        // out-of-mixer preview would otherwise skip (leaving it louder than playback).
        if let master = tracks.first(where: { $0.kind == .master }) {
            db += master.volumeDb
            if master.muted { return 0 }
        }
        return max(0, min(1, powf(10, db / 20)))
    }

    private func playPitchPreviewFile(_ path: String, start: Double, duration: Double) -> Bool {
        do {
            let file = try AVAudioFile(forReading: URL(fileURLWithPath: path))
            let format = file.processingFormat
            let startFrame = max(0, min(file.length, AVAudioFramePosition(start * format.sampleRate)))
            let available = max(0, file.length - startFrame)
            let requested = AVAudioFrameCount(max(1, duration * format.sampleRate))
            let frameCount = min(AVAudioFrameCount(clamping: available), requested)
            guard frameCount > 0 else { return false }

            let audioEngine = AVAudioEngine()
            let player = AVAudioPlayerNode()
            audioEngine.attach(player)
            audioEngine.connect(player, to: audioEngine.mainMixerNode, format: format)

            // The project stores CoreAudio's device UID. Translate it to AudioDeviceID
            // and bind this short-lived preview engine before starting it.
            if !currentOutputDeviceId.isEmpty, let unit = audioEngine.outputNode.audioUnit {
                var uid: CFString = currentOutputDeviceId as CFString
                var device: AudioDeviceID = 0
                var address = AudioObjectPropertyAddress(
                    mSelector: kAudioHardwarePropertyDeviceForUID,
                    mScope: kAudioObjectPropertyScopeGlobal,
                    mElement: kAudioObjectPropertyElementMain)
                var size = UInt32(MemoryLayout<AudioValueTranslation>.size)
                let lookupStatus = withUnsafeMutablePointer(to: &uid) { uidPointer in
                    withUnsafeMutablePointer(to: &device) { devicePointer in
                        var translation = AudioValueTranslation(
                            mInputData: UnsafeMutableRawPointer(uidPointer),
                            mInputDataSize: UInt32(MemoryLayout<CFString>.size),
                            mOutputData: UnsafeMutableRawPointer(devicePointer),
                            mOutputDataSize: UInt32(MemoryLayout<AudioDeviceID>.size))
                        return AudioObjectGetPropertyData(
                            AudioObjectID(kAudioObjectSystemObject), &address,
                            0, nil, &size, &translation)
                    }
                }
                if lookupStatus == noErr,
                   device != 0 {
                    var selected = device
                    AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice,
                                         kAudioUnitScope_Global, 0, &selected,
                                         UInt32(MemoryLayout<AudioDeviceID>.size))
                }
            }

            // Match playback level: this engine is outside the mixer, so the gain the signal would
            // have seen there is applied here instead.
            audioEngine.mainMixerNode.outputVolume = pitchPreviewGain
            player.scheduleSegment(file, startingFrame: startFrame, frameCount: frameCount, at: nil)
            try audioEngine.start()
            player.play()
            pitchPreviewEngine = audioEngine
            pitchPreviewPlayer = player
            return true
        } catch {
            Diagnostics.shared.log("pitch audition output: \(error.localizedDescription)")
            return false
        }
    }

    /// Render the current pitch-editor result to a temp WAV and play it (system playback, not through
    /// the engine) so the user can hear the edit before committing. Works for both modes.
    func previewPitchEdit() {
        guard let handle, let clipId = pitchEditorClipId else { return }
        pitchPreviewSound?.stop()
        // Same rule as auditionPitchEvent: silence the engine's own playback so the
        // untouched original doesn't sound over (and mask) the rendered preview.
        setTransport(running: false)
        let tmp = (NSTemporaryDirectory() as NSString).appendingPathComponent("neuracoust-preview-\(UUID().uuidString).wav")
        var err = [CChar](repeating: 0, count: 512)
        let ok: Bool
        if pitchEditorMode == .melodyne {
            ok = nc_clip_export_note_edits(handle, clipId, tmp, &err, err.count)
        } else {
            let src = timeMapAnchors.map { $0.x }, dst = timeMapAnchors.map { $0.y }
            ok = src.withUnsafeBufferPointer { sp in dst.withUnsafeBufferPointer { dp in
                nc_clip_export_time_map(handle, clipId, pitchEditTimeRatio, pitchEditSemitones,
                                        sp.baseAddress, dp.baseAddress, Int32(src.count), tmp, &err, err.count)
            }}
        }
        guard ok else { let m = String(cString: err); if !m.isEmpty { Diagnostics.shared.log("preview: \(m)") }; return }
        pitchPreviewSound = NSSound(contentsOfFile: tmp, byReference: true)
        pitchPreviewSound?.volume = pitchPreviewGain
        pitchPreviewSound?.play()
    }

    func stopPitchPreview() { pitchPreviewSound?.stop() }

    /// Audition one detected event from the RENDERED edit result. In synced mode the main
    /// timeline follows the preview, but the engine's original clip must not be played:
    /// doing that made a moved blob still sound at its old pitch.
    func auditionPitchEvent(localStart: Double, duration: Double) {
        let start = min(pitchEditorClipDuration, max(0, localStart))
        let length = max(0.08, min(max(0.08, duration), pitchEditorClipDuration - start))
        pitchPreviewTimer?.invalidate(); pitchPreviewTimer = nil
        pitchPreviewPlayer?.stop(); pitchPreviewEngine?.stop()
        pitchPreviewPlayer = nil; pitchPreviewEngine = nil
        pitchPreviewSound?.stop()
        auditionStopWork?.cancel()
        // Always silence the engine's own playback before the rendered edit preview.
        // If the transport is running, the engine keeps sounding the UNTOUCHED clip
        // through the full mixer while this offline preview plays on top — the two
        // sources sum (roughly +6 dB, "twice as loud") and the dominant original
        // masks the edit, so a pitch move sounds like it did nothing. Stopping the
        // transport unconditionally makes the preview the only thing sounding.
        setTransport(running: false)
        if pitchEditorTimelineSync {
            seek(pitchEditorClipStartSeconds + start)
        }
        guard let handle, let clipId = pitchEditorClipId else { return }
        let tmp = (NSTemporaryDirectory() as NSString).appendingPathComponent("neuracoust-event-preview-\(UUID().uuidString).wav")
        var err = [CChar](repeating: 0, count: 512)
        let ok = nc_clip_export_note_edits(handle, clipId, tmp, &err, err.count)
        guard ok else {
            Diagnostics.shared.log("pitch audition render: \(String(cString: err))")
            return
        }
        let editedCount = pitchNotes.filter { abs($0.offsetSemitones) > 0.001 }.count
        Diagnostics.shared.log(String(format: "pitch audition: gain=%.3f, notes w/ pitch offset=%d/%d",
                                       pitchPreviewGain, editedCount, pitchNotes.count))
        if !playPitchPreviewFile(tmp, start: start, duration: length) {
            // Last-resort fallback for machines where a second CoreAudio client cannot
            // open the selected interface.
            guard let sound = NSSound(contentsOfFile: tmp, byReference: true) else { return }
            pitchPreviewSound = sound
            sound.volume = pitchPreviewGain
            sound.currentTime = start
            sound.play()
        }
        let began = CACurrentMediaTime()
        pitchEditorClock.seconds = start
        pitchPreviewTimer = Timer.scheduledTimer(withTimeInterval: 1.0 / 30.0, repeats: true) { [weak self] timer in
            MainActor.assumeIsolated {
                guard let self else { timer.invalidate(); return }
                let elapsed = CACurrentMediaTime() - began
                let local = min(self.pitchEditorClipDuration, start + elapsed)
                self.pitchEditorClock.seconds = local
                if self.pitchEditorTimelineSync {
                    // Keep the engine's stopped playhead (and therefore the main timeline)
                    // aligned with the independently rendered preview.
                    self.seek(self.pitchEditorClipStartSeconds + local)
                }
                if elapsed >= length {
                    self.pitchPreviewPlayer?.stop(); self.pitchPreviewEngine?.stop()
                    self.pitchPreviewPlayer = nil; self.pitchPreviewEngine = nil
                    self.pitchPreviewSound?.stop()
                    timer.invalidate(); self.pitchPreviewTimer = nil
                }
            }
        }
    }

    func setPitchEditorPosition(_ localSeconds: Double) {
        let local = min(pitchEditorClipDuration, max(0, localSeconds))
        pitchEditorClock.seconds = local
        if pitchEditorTimelineSync { seek(pitchEditorClipStartSeconds + local) }
    }

    /// Percussive timing: record a transient's new position (dragged horizontally).
    func setTransientTime(_ index: Int, _ seconds: Double) {
        guard index >= 0, index < pitchNotes.count else { return }
        percussiveTimeEdits[index] = min(pitchEditorClipDuration, max(0, seconds))
    }

    /// Apply percussive timing edits as a time-remap: each moved transient becomes a (source→dest)
    /// anchor, so the audio slides its hits to the new positions. One render, like the other modes.
    func applyPercussiveTiming() {
        guard let handle, let clipId = pitchEditorClipId, !percussiveTimeEdits.isEmpty else { return }
        var pairs: [(Double, Double)] = []
        for (idx, newT) in percussiveTimeEdits where idx < pitchNotes.count {
            let src = pitchNotes[idx].startSeconds / pitchEditorClipDuration
            let dst = newT / pitchEditorClipDuration
            pairs.append((min(0.98, max(0.02, src)), min(0.98, max(0.02, dst))))
        }
        guard !pairs.isEmpty else { return }
        pairs.sort { $0.0 < $1.0 }
        let s = pairs.map { $0.0 }, d = pairs.map { $0.1 }
        var err = [CChar](repeating: 0, count: 256)
        let ok = s.withUnsafeBufferPointer { sp in d.withUnsafeBufferPointer { dp in
            nc_clip_apply_time_map(handle, clipId, 1.0, 0.0, sp.baseAddress, dp.baseAddress, Int32(s.count), 0, &err, err.count)
        }}
        if ok { reloadClips(); refreshHistory(); closePitchEditor() }
        else { let m = String(cString: err); if !m.isEmpty { Diagnostics.shared.log("percussive timing: \(m)") } }
    }

    func closePitchEditor() {
        pitchDetectionGeneration &+= 1
        pitchPreviewTimer?.invalidate(); pitchPreviewTimer = nil
        pitchPreviewSound?.stop()
        pitchEditorClipId = nil; pitchNotes = []; percussiveTimeEdits = [:]
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
    @Published private(set) var monitorVolumeDb: Float = -12
    @Published private(set) var monitorListen = MonitorListen()
    @Published private(set) var monitorDim = false
    @Published private(set) var monitorMono = false
    @Published private(set) var monitorMute = false
    @Published private(set) var monitorTalkback = false
    /// Talkback destination: "listen_room" (default), "monitor_bus", or "all".
    @Published private(set) var talkbackRoute = "listen_room"
    /// Which input channel (1-based) the talkback mic is on (e.g. ch2 when ch1 is the singer).
    @Published private(set) var talkbackChannel = 1
    /// System-wide keypad capture: drive monitor volume / transport from the numeric keypad even
    /// when another app is frontmost. Needs the Accessibility permission. Persisted per machine.
    @Published private(set) var keypadCaptureEnabled = false
    /// The user's requested state is kept separately from the live event-tap state. A rebuilt app
    /// can temporarily lose Accessibility permission; keeping this true lets the DAW re-arm the
    /// tap automatically as soon as the user returns from System Settings.
    @Published private(set) var keypadCaptureRequested = false
    private let keypadCapture = GlobalKeypadCapture()
    /// A/B *listening* state: false = hearing the DAW master, true = hearing the reference tap
    /// (other apps). Only meaningful while `referenceArmed` is on.
    @Published private(set) var monitorListenSource = false
    /// Reference-hold: the process tap is armed and the tapped apps are muted at their own output,
    /// so switching to the master never leaks their sound out of the computer. Left-clicking "다른 앱"
    /// arms it and A/Bs; right-clicking → 레퍼런스 종료 disarms and unmutes the apps.
    @Published private(set) var referenceArmed = false
    @Published private(set) var monitorDspEnabled = true
    @Published private(set) var monitorPathMode = "internal"

    /// Output mode is a UI concept — the engine models speaker vs headphone as
    /// which simulation module is enabled. Switching it re-derives the single monitor EQ
    /// (headphone drops the speaker model curve; speaker restores it).
    @Published var outputMode: OutputMode = .speaker {
        didSet { if oldValue != outputMode { syncMonitorEqToContext() } }
    }

    struct OutputDevice: Identifiable, Hashable { let id: String; let name: String }
    @Published private(set) var outputDevices: [OutputDevice] = []
    @Published private(set) var currentOutputDeviceId = ""   // empty = system default
    @Published private(set) var activeOutputDeviceName = ""

    /// Rescans CoreAudio and refreshes the device list. Called on the dock's appearance and
    /// periodically from the poll, so a hot-plugged interface (e.g. a UNiTE-2 connected mid
    /// session) shows up without a restart. Republishes only on change, so the periodic scan
    /// doesn't flicker open menus.
    func refreshOutputDevices() {
        guard let handle else { return }
        let count = Int(nc_output_device_count(handle))
        let devices = (0..<count).map { i in
            OutputDevice(id: readEngineString { nc_output_device_id(handle, Int32(i), $0, $1) },
                         name: readEngineString(capacity: 256) { nc_output_device_name(handle, Int32(i), $0, $1) })
        }
        if devices != outputDevices { outputDevices = devices }
        setIfChanged(\.currentOutputDeviceId, readEngineString { nc_current_output_device_id(handle, $0, $1) })
        setIfChanged(\.activeOutputDeviceName, readEngineString(capacity: 256) { nc_active_output_device_name(handle, $0, $1) })
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
        let devices = (0..<count).map { i in
            OutputDevice(id: readEngineString { nc_input_device_id(handle, Int32(i), $0, $1) },
                         name: readEngineString(capacity: 256) { nc_input_device_name(handle, Int32(i), $0, $1) })
        }
        if devices != inputDevices { inputDevices = devices }
        setIfChanged(\.currentInputDeviceId, readEngineString { nc_current_input_device_id(handle, $0, $1) })
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
    /// Reference-tap ("다른 앱") FIFO faults. The jitter figure above is the OUTPUT render thread
    /// only — a tap capture that starves or overflows crackles while jitter reads clean. This is
    /// that blind spot made visible.
    @Published private(set) var referenceTapFaults: Int = 0
    @Published private(set) var remoteDspActive = false

    // DSP core allocation (a QoS hint the engine applies to its realtime thread).
    @Published private(set) var coreIsolationEnabled = true
    @Published private(set) var dspCoreCount = 4
    // Cores DW asks the external DSP Manager to reserve; a connected node's own report wins.
    @Published private(set) var externalDspCoreCount = 4
    // The remote DSP node address the engine streams to (External/NDS target).
    @Published var remoteDspHost = "studio.local"
    @Published private(set) var remoteDspRoundTripMs: Double = 0
    // The "use this node" master switch (whether the external node participates at all).
    @Published private(set) var externalDspEnabled = true
    // Discovered/queried node hardware, shown in the Remote Core panel. Populated on 검색/refresh.
    struct RemoteNodeSpecs: Equatable {
        var model = ""
        var cpuModel = ""
        var cpuMhz: Double = 0
        var memoryMb = 0
        var coreCount = 0
        var roundTripMs: Double = 0
    }
    @Published private(set) var remoteNodeSpecs: RemoteNodeSpecs? = nil

    // Stem separation (Demucs helper subprocess). progress nil = idle, 0…1 = running.
    @Published private(set) var stemSeparationProgress: Double? = nil
    @Published private(set) var stemSeparationStatus: String = ""

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
    /// The live ARA editor window, if one is open. Held so a second menu pick cannot open a rival
    /// session over the same clip, and so the window survives the call that created it.
    fileprivate var araEditor: AraEditorWindowController?
    private var timer: Timer?
    /// A held key must not wait a 33 ms UI tick to sound. This second timer only drains the
    /// live-MIDI queue into the instruments, ~240 Hz, so monitoring latency drops toward one
    /// audio buffer instead of one UI frame — the "레이턴시" the user felt against Logic.
    private var midiPumpTimer: Timer?
    private let midiPumpInterval = 1.0 / 240.0
    private var keyMonitor: Any?
    /// >0 while one or more NSMenus (context menus, `Menu`s, the menu bar) are tracking. While a
    /// menu is open the 30 Hz telemetry publish is paused so the heavy engine-observing views
    /// (esp. MonitorDock) don't re-render and tear the open NSMenu down — which made submenus
    /// flicker and refuse to expand. Cosmetic meters resume the instant the menu closes.
    private var menuTrackingDepth = 0
    private var menuTrackingObservers: [Any] = []
    private var menuTrackingActive: Bool { menuTrackingDepth > 0 }
    // Talkback keypad key = console key: a quick TAP latches it on (tap again = off), a HOLD is
    // momentary (talk while held). These track the press so key-up can tell tap from hold.
    private var talkbackKeyDownTime: Double = 0
    private var talkbackKeyLatched = false
    private var talkbackKeyConsumed = false      // a key-down that turned a latch off; key-up ignores it
    private let talkbackTapSeconds = 0.30

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
            Diagnostics.shared.reportError("engine start failed: \(startupError ?? "")")
        } else {
            // The device that actually opened — logged deterministically (not via stderr piping) so
            // silence reports always name the interface the engine reached.
            let dev = readString { nc_active_output_device_name(handle, $0, $1) }
            Diagnostics.shared.log("engine started — output device: \(dev.isEmpty ? "(system default)" : dev)")
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
        preRollSeconds = nc_project_pre_roll(handle)
        postRollSeconds = nc_project_post_roll(handle)
        reloadTracks()
        reloadClips()
        reloadMonitorState()
        reloadRecordControllerState()
        nc_history_reset(handle)
        refreshHistory()
        installKeyMonitor()
        installMenuTrackingObserver()
        installKeypadPermissionObserver()
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
                self.drainLiveMidi(handle)
            }
        }
        midiTimer.tolerance = 0
        RunLoop.main.add(midiTimer, forMode: .common)
        self.midiPumpTimer = midiTimer

        restorePersistedSettings()
        refreshHuiDevices()
        restoreHuiConnection()
    }

    func refreshHuiDevices() {
        guard let handle else { return }
        huiInputs = (0..<Int(nc_hui_input_count(handle))).map { index in
            MidiSurfaceEndpoint(
                id: readEngineString { nc_hui_input_id(handle, Int32(index), $0, $1) },
                name: readEngineString { nc_hui_input_name(handle, Int32(index), $0, $1) })
        }
        huiOutputs = (0..<Int(nc_hui_output_count(handle))).map { index in
            MidiSurfaceEndpoint(
                id: readEngineString { nc_hui_output_id(handle, Int32(index), $0, $1) },
                name: readEngineString { nc_hui_output_name(handle, Int32(index), $0, $1) })
        }
    }

    func selectHuiInput(_ id: String) {
        huiInputId = id
        UserDefaults.standard.set(id, forKey: "nc.hui.input")
        reconnectHuiIfReady()
    }

    func selectHuiOutput(_ id: String) {
        huiOutputId = id
        UserDefaults.standard.set(id, forKey: "nc.hui.output")
        reconnectHuiIfReady()
    }

    func connectHui() {
        guard let handle, !huiInputId.isEmpty, !huiOutputId.isEmpty else {
            huiStatus = "HUI 입력과 출력을 모두 선택하세요."
            return
        }
        UserDefaults.standard.set(true, forKey: "nc.hui.enabled")
        huiConnected = huiInputId.withCString { input in
            huiOutputId.withCString { output in nc_hui_connect(handle, input, output) }
        }
        huiStatus = readEngineString { nc_hui_status(handle, $0, $1) }
    }

    func disconnectHui() {
        guard let handle else { return }
        nc_hui_disconnect(handle)
        huiConnected = false
        huiStatus = "연결 안 됨"
        UserDefaults.standard.set(false, forKey: "nc.hui.enabled")
    }

    private func reconnectHuiIfReady() {
        if !huiInputId.isEmpty, !huiOutputId.isEmpty { connectHui() }
    }

    private func restoreHuiConnection() {
        let defaults = UserDefaults.standard
        huiInputId = defaults.string(forKey: "nc.hui.input") ?? ""
        huiOutputId = defaults.string(forKey: "nc.hui.output") ?? ""
        if defaults.bool(forKey: "nc.hui.enabled") { connectHui() }
    }

    private func serviceHui(_ handle: OpaquePointer) {
        let connected = nc_hui_connected(handle)
        if connected != huiConnected {
            huiConnected = connected
            huiStatus = readEngineString { nc_hui_status(handle, $0, $1) }
        }
        guard connected else {
            huiReconnectTicks += 1
            if huiReconnectTicks >= 60, UserDefaults.standard.bool(forKey: "nc.hui.enabled") {
                huiReconnectTicks = 0
                refreshHuiDevices()
                if huiInputs.contains(where: { $0.id == huiInputId }),
                   huiOutputs.contains(where: { $0.id == huiOutputId }) {
                    connectHui()
                }
            }
            return
        }
        huiReconnectTicks = 0
        var event = NCHuiEvent()
        var changedMix = false
        while nc_hui_next_event(handle, &event) {
            let id = Int(event.trackIndex)
            switch event.type {
            case 1 where id >= 0:
                setTrackVolume(id, max(-60, min(12, event.value * 72 - 60)))
                changedMix = true
            case 2 where id >= 0:
                let current = tracks.first(where: { $0.id == id })?.pan ?? 0
                setTrackPan(id, max(-1, min(1, current + event.value * 0.02)))
                changedMix = true
            case 3 where event.pressed && id >= 0:
                selectedTrackId = id
                selectedMixerTrackIds = [id]
            case 4 where event.pressed && id >= 0:
                if let track = tracks.first(where: { $0.id == id }) {
                    applyTrackFlag([id], flag: 0, value: !track.muted)
                }
            case 5 where event.pressed && id >= 0: toggleTrackSolo(id)
            case 6 where event.pressed && id >= 0: toggleTrackArm(id)
            case 7 where event.pressed: setTransport(running: true)
            case 8 where event.pressed: stop()
            case 9 where event.pressed: toggleRecording()
            case 10 where event.pressed: rewind()
            case 11 where event.pressed: seek(playheadSeconds + 5)
            default: break
            }
        }
        if changedMix { recordGesture("Mackie HUI") }
        nc_hui_sync(handle, transportRunning, recording)
    }

    /// The old UI drove every shortcut from an NSEvent monitor rather than menu key
    /// equivalents — all 308 of its menu items carry an empty keyEquivalent. Menu
    /// shortcuts do not reach this window reliably, so do the same here.
    private func installKeyMonitor() {
        keyMonitor = NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .keyUp]) { [weak self] event in
            guard let self else { return event }
            return MainActor.assumeIsolated {
                event.type == .keyUp ? self.handleKeyUp(event) : self.handleKeyDown(event)
            }
        }
    }

    /// Key release for the Talk shortcut: a quick TAP latches talkback on (stays lit); a HOLD is
    /// momentary, so releasing after holding drops it. (A press that turned a latch off is consumed.)
    private func handleKeyUp(_ event: NSEvent) -> NSEvent? {
        guard monitorShortcutsEnabled,
              NSApp.keyWindow?.firstResponder is NSTextView == false,
              monitorShortcutAction(forKeyCode: event.keyCode) == .talk else { return event }
        if talkbackKeyConsumed { talkbackKeyConsumed = false; return nil }
        if CACurrentMediaTime() - talkbackKeyDownTime < talkbackTapSeconds {
            talkbackKeyLatched = true            // tap → latch on (stays engaged)
        } else {
            talkbackKeyLatched = false
            if monitorTalkback { setTalkbackEngaged(false) }   // hold → release
        }
        return nil
    }

    /// Track NSMenu open/close so `tick()` can pause telemetry while a menu is up. A counter
    /// (not a bool) because a context menu and its submenu each post their own begin/end.
    private func installMenuTrackingObserver() {
        guard menuTrackingObservers.isEmpty else { return }
        let nc = NotificationCenter.default
        let begin = nc.addObserver(forName: NSMenu.didBeginTrackingNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.menuTrackingDepth += 1 }
        }
        let end = nc.addObserver(forName: NSMenu.didEndTrackingNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self else { return }
                self.menuTrackingDepth = max(0, self.menuTrackingDepth - 1)
            }
        }
        menuTrackingObservers = [begin, end]
    }

    /// A development rebuild changes the app's code identity and macOS may ask for Accessibility
    /// again. When the user comes back from System Settings, retry immediately instead of requiring
    /// another click on the monitor dock.
    private func installKeypadPermissionObserver() {
        let observer = NotificationCenter.default.addObserver(
            forName: NSApplication.didBecomeActiveNotification,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self,
                      self.keypadCaptureRequested,
                      !self.keypadCaptureEnabled,
                      GlobalKeypadCapture.accessibilityTrusted(prompt: false) else { return }
                self.setKeypadCapture(true)
            }
        }
        menuTrackingObservers.append(observer)
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
        static let r: UInt16 = 15               // Pro Tools single-key zoom out (horizontal)
        static let h: UInt16 = 4
        static let space: UInt16 = 49
        static let delete: UInt16 = 51
        static let forwardDelete: UInt16 = 117
        static let leftBracket: UInt16 = 33    // [  — Pro Tools zoom out
        static let rightBracket: UInt16 = 30   // ]  — Pro Tools zoom in
        static let enter: UInt16 = 36          // Return — Pro Tools: return the playhead to start
        static let f: UInt16 = 3               // Pro Tools zoom-to-fit
        static let tab: UInt16 = 48            // Pro Tools: Tab → next clip/region/marker boundary
    }

    private func handleKeyDown(_ event: NSEvent) -> NSEvent? {
        // Never steal keys while the user is typing.
        if NSApp.keyWindow?.firstResponder is NSTextView {
            return event
        }
        // Timeline edits carry no modifier, the way every DAW does it.
        if !event.modifierFlags.contains(.command) {
            // The monitor-shortcut layer (numeric keypad) wins over the normal no-modifier keys
            // while it is engaged — e.g. keypad Enter drives Talk instead of dropping a marker.
            if monitorShortcutsEnabled, let action = monitorShortcutAction(forKeyCode: event.keyCode) {
                // Swallow auto-repeat: a held key must not fire the action over and over (that
                // machine-gunned talkback on/off — Dim in/out — and pumped the monitor level).
                if event.isARepeat { return nil }
                if action == .talk {
                    if talkbackKeyLatched && monitorTalkback {
                        talkbackKeyLatched = false        // a press on a latched key turns it off
                        setTalkbackEngaged(false)
                        talkbackKeyConsumed = true        // key-up does nothing for this press
                    } else {
                        talkbackKeyLatched = false
                        setTalkbackEngaged(true)          // engage; key-up decides tap(latch) vs hold(release)
                        talkbackKeyDownTime = CACurrentMediaTime()
                        talkbackKeyConsumed = false
                    }
                } else {
                    performMonitorShortcut(action)
                }
                return nil
            }
            switch event.keyCode {
            case KeyCode.space:
                // The transport toggle, the way the spacebar works in every DAW. It has
                // to be consumed here (return nil) or it also clicks whatever button
                // holds focus.
                togglePlay()
                return nil
            case KeyCode.t:
                // Modern Pro Tools single-key horizontal zoom: T in, R out (no modifier).
                // ⌘] / ⌘[ still do the same (handled in the command block below).
                zoomTimeline(by: 0.5)
                return nil
            case KeyCode.r:
                zoomTimeline(by: 2.0)
                return nil
            case KeyCode.enter:
                // Pro Tools: Return returns the playhead to the session start. (Markers are ⌘M.)
                seek(0)
                return nil
            case KeyCode.tab:
                // Pro Tools Tab-to-boundary: jump the playhead to the next edit point (clip/region
                // edge or marker); ⇧Tab to the previous one. Consumed so Tab does not shift focus.
                let boundaries = editBoundaries()
                if event.modifierFlags.contains(.shift) {
                    seek(boundaries.last(where: { $0 < playheadSeconds - 0.0005 }) ?? 0)
                } else if let next = boundaries.first(where: { $0 > playheadSeconds + 0.0005 }) {
                    seek(next)
                }
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
                // With the piano roll open and notes picked, Delete removes those notes
                // (Logic/Cubase) — checked before the region so it never drops the whole part.
                if editingRegionId != nil, !selectedNoteIds.isEmpty {
                    deleteSelectedNotes()
                    return nil
                }
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
        let option = event.modifierFlags.contains(.option)

        switch event.keyCode {
        // Pro Tools horizontal zoom: ⌘] in, ⌘[ out. ⌘⌥] / ⌘⌥[ frames the session (fit).
        case KeyCode.rightBracket:
            option ? fitTimeline() : zoomTimeline(by: 0.5)
        case KeyCode.leftBracket:
            option ? fitTimeline() : zoomTimeline(by: 2.0)
        case KeyCode.z:
            shift ? redo() : undo()
        case KeyCode.s:
            shift ? saveProjectAs() : saveProject()
        case KeyCode.o:
            openProject()
        case KeyCode.n where shift:
            // Pro Tools: ⌘⇧N = New Track (audio). ⌘⇧⌥N adds an instrument track.
            option ? addInstrumentTrack() : addAudioTrack()
        case KeyCode.n:
            newProject()                        // ⌘N = New Session
        case KeyCode.i:
            // Pro Tools binds Import Audio to ⌘⇧I; plain ⌘I was unbound and users reach
            // for it, so both import (⌘I is free here — no Get Info / Italic to shadow).
            importAudio(intoTrack: 0)
        case KeyCode.b where option:
            bounceProject()                     // Pro Tools: ⌘⌥B = Bounce to Disk
        case KeyCode.e:
            // Pro Tools: ⌘E = Separate Clip (split at the playhead / selection).
            if let regionId = selectedRegionId { splitRegionAtPlayhead(regionId) }
            else { splitSelectedClipsAtPlayhead() }
        case KeyCode.h:
            healSelectedClips()                 // Pro Tools: ⌘H = Heal Separation
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
            addMarkerAtPlayhead()               // kept alongside Pro Tools' Return
        default:
            return event
        }
        return nil
    }

    func shutdown() {
        releaseKeypadCapture()   // return the keypad to the system
        if let keyMonitor {
            NSEvent.removeMonitor(keyMonitor)
        }
        keyMonitor = nil
        menuTrackingObservers.forEach { NotificationCenter.default.removeObserver($0) }
        menuTrackingObservers.removeAll()
        menuTrackingDepth = 0
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
        // A fade/range audition temporarily hijacks the loop range (loopEnabled + a short window)
        // and only restores it in stopAudition(). If the user hits the spacebar to end or resume
        // playback instead, that restore never runs and normal play loops the short audition
        // window ("plays a tiny section over and over"). Restore the real loop first, using the
        // pre-toggle running state so a spacebar press to STOP doesn't bounce back into play.
        let wasRunning = transportRunning
        if savedLoopForAudition != nil {
            stopAudition()
        }
        // Melodyne-style auto-apply: starting playback commits any pending pitch edits to the clip
        // first (the bake is synchronous, so the timeline then plays exactly what you just drew —
        // no manual Apply needed). No edits pending → nothing happens, no wasted undo step.
        if !wasRunning, pitchEditorClipId != nil, pitchEditorMode == .melodyne,
           pitchNotes.contains(where: { $0.edited }) {
            applyPitchEdits()
        }
        setTransport(running: !wasRunning)
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
        setPlayhead(0)
        transportWallClockBase = 0
        transportWallClockStart = CACurrentMediaTime()
    }

    func seek(_ seconds: Double) {
        guard let handle else { return }
        let clamped = max(0, seconds)
        nc_engine_seek(handle, clamped)
        setPlayhead(clamped)
        transportWallClockBase = clamped
        transportWallClockStart = CACurrentMediaTime()
    }

    private var auditionStopWork: DispatchWorkItem?
    private var savedLoopForAudition: (enabled: Bool, start: Double, end: Double)?
    /// Pre-/post-roll around an audition (seconds of lead-in and tail). User-adjustable in the editor.
    @Published var auditionRollSeconds: Double = 1.5

    /// Audition a fade/crossfade: play from `roll` before it to `roll` after it. With `loop`, it uses
    /// the ENGINE's loop range (rock-solid — the old self-rescheduling timer stopped at content end
    /// before looping) and restores the user's loop on stop. Non-loop plays once and stops.
    /// End of the last audio clip / MIDI region — the engine stops here, so a loop end past it never
    /// wraps (it just plays once and stops). The audition loop clamps to this.
    private var projectContentEnd: Double {
        var e = 0.0
        for c in clips { e = max(e, c.startSeconds + c.durationSeconds) }
        for r in midiRegions { e = max(e, r.startSeconds + r.durationSeconds) }
        return e
    }

    func auditionRegion(from start: Double, to end: Double, loop: Bool) {
        guard let handle, end > start else { return }
        let roll = max(0, auditionRollSeconds)
        let from = max(0, start - roll)
        // Clamp the tail to the content end, else the engine reaches the end of audio and STOPS
        // instead of wrapping — so the loop only ever plays once (the reported bug).
        let contentEnd = projectContentEnd
        let to = contentEnd > end ? min(end + roll, contentEnd) : end + roll
        auditionStopWork?.cancel(); auditionStopWork = nil

        if loop {
            if savedLoopForAudition == nil {
                savedLoopForAudition = (loopEnabled, loopStartSeconds, loopEndSeconds)
            }
            nc_project_set_loop_range(handle, from, to)
            nc_project_set_loop_enabled(handle, true)
            loopEnabled = true; loopStartSeconds = from; loopEndSeconds = to
            seek(from)
            setTransport(running: true)
        } else {
            seek(from)
            setTransport(running: true)
            let work = DispatchWorkItem { [weak self] in self?.stopAudition() }
            auditionStopWork = work
            DispatchQueue.main.asyncAfter(deadline: .now() + (to - from), execute: work)
        }
    }

    func stopAudition() {
        auditionStopWork?.cancel(); auditionStopWork = nil
        setTransport(running: false)
        if let handle, let saved = savedLoopForAudition {
            nc_project_set_loop_range(handle, saved.start, saved.end)
            nc_project_set_loop_enabled(handle, saved.enabled)
            loopEnabled = saved.enabled; loopStartSeconds = saved.start; loopEndSeconds = saved.end
            savedLoopForAudition = nil
        }
    }

    /// Sorted timeline edit points — every clip and MIDI-region start/end plus every marker, and 0 —
    /// for Pro Tools Tab-to-boundary navigation.
    private func editBoundaries() -> [Double] {
        var pts: Set<Double> = [0]
        for c in clips { pts.insert(c.startSeconds); pts.insert(c.startSeconds + c.durationSeconds) }
        for r in midiRegions { pts.insert(r.startSeconds); pts.insert(r.startSeconds + r.durationSeconds) }
        for m in markers { pts.insert(m.timeSeconds) }
        return pts.sorted()
    }

    /// Is a background audio capture pass running (transport rolling with an armed audio track)?
    private var audioRecordingActive = false
    /// Was Record pressed at least once during this pass? Only then is the take kept at Stop.
    private var recordCommitted = false
    /// Where the background capture began (transport roll). The file holds the whole pass from here;
    /// each punch region becomes its own clip that references a window of the file.
    private var recordCaptureStartSeconds: Double = 0
    private var recordTrackId: Int? = nil               // the armed audio track punches commit to
    /// A punch region: timeline seconds + peak-bucket range (for the visible waveform slice).
    private struct PunchRegion { var inSec: Double; var outSec: Double; var inBucket: Int; var outBucket: Int }
    private var recordRegions: [PunchRegion] = []       // completed (punched in AND out)
    private var currentPunchInSeconds: Double? = nil    // the in-progress region (punched in, not out)
    private var currentPunchInBucket = 0
    /// Full capture peaks (from captureStart) accumulated incrementally; each clip slices its window.
    private var captureLivePeaksL: [Float] = []
    private var captureLivePeaksR: [Float] = []
    private var recordingPeaksConsumed = 0

    /// A live recording clip drawn on the timeline (one per punch region + the growing one).
    struct RecordingClip: Equatable {
        let trackId: Int
        let startSeconds: Double
        let durationSeconds: Double
        let peaksL: [Float]
        let peaksR: [Float]
    }
    @Published private(set) var recordingClips: [RecordingClip] = []
    @Published private(set) var recordingChannels: Int = 2

    /// Record. A record-armed instrument/MIDI track captures the live keyboard into a MIDI region;
    /// a record-armed AUDIO track captures its input (a hardware pair, or the "다른 앱" reference
    /// tap) to a WAV and drops a clip at the playhead when stopped. Both can run at once.
    func toggleRecording() {
        guard let handle else { return }
        recording.toggle()
        nc_engine_set_recording(handle, recording)
        if recording {
            // Audio (Pro Tools style): rolling the transport already started an INVISIBLE background
            // capture; pressing Record PUNCHES IN — the visible clip starts here, while the file
            // already holds the pre-roll captured since playback began.
            if !transportRunning { setTransport(running: true) }   // begins the background capture
            if !audioRecordingActive { beginAudioCapture() }        // if transport was already rolling
            if audioRecordingActive, currentPunchInSeconds == nil {
                recordCommitted = true
                currentPunchInSeconds = playheadSeconds            // open a new punch region
                currentPunchInBucket = captureLivePeaksL.count
                // Auto-input: hear the live tap source ONLY while punched in.
                if recordingTapSource { nc_monitor_set_tap_input_monitor(handle, true) }
                lastError = "녹음 (펀치 인)…"
            }
            // Begin a MIDI take on the first RECORD-ARMED instrument/MIDI track, and roll the
            // transport so the playhead advances under the recorded notes. Recording requires the
            // track's Record-arm (R), deliberately NOT falling back to the merely-selected track:
            // selecting an instrument track lets you PLAY it (the live-MIDI monitor routes to the
            // selected track), but capturing to a take is a separate, explicit choice — arming it.
            // Recording onto a track the user only clicked to audition was a surprise capture.
            let target = tracks.first { $0.recordArmed && ($0.kind == .instrument || $0.kind == .midi) }
            if let target {
                let recordAt = playheadSeconds
                if countInBars > 0 {
                    // Count-in: roll from N bars back with the click, and open the take only when
                    // the playhead reaches the record point (serviceCountIn watches for it).
                    countInRestoreClick = clickEnabled
                    if !clickEnabled {
                        clickEnabled = true
                        nc_engine_set_metronome_enabled(handle, true)
                        countInForcedClick = true
                    }
                    pendingRecord = (trackId: target.id, atSeconds: recordAt)
                    let secondsPerBar = (60.0 / Double(max(1, tempoBpm))) * Double(max(1, timeSignature.numerator))
                    seek(max(0, recordAt - Double(countInBars) * secondsPerBar))
                    setTransport(running: true)
                    lastError = "카운트인 \(countInBars)마디…"
                } else {
                    if !transportRunning { setTransport(running: true) }
                    nc_midi_record_begin(handle, Int32(target.id), recordAt)
                    reloadMidiRegions()   // show the (empty) take region the moment recording starts
                    lastError = "\(target.name)에 MIDI 녹음 중…"
                }
            } else {
                lastError = "MIDI 녹음하려면 악기/MIDI 트랙을 레코드-암 하세요. (오디오 입력 녹음은 미구현)"
            }
        } else {
            // Record OFF is a punch-out: close the current region as a fixed clip, but keep capturing
            // in the background so a later punch-in starts a NEW clip (each region is kept).
            if audioRecordingActive, let pin = currentPunchInSeconds {
                recordRegions.append(PunchRegion(inSec: pin, outSec: playheadSeconds,
                                                 inBucket: currentPunchInBucket, outBucket: captureLivePeaksL.count))
                currentPunchInSeconds = nil
                nc_monitor_set_tap_input_monitor(handle, false)   // punch-out: back to hearing the tape
            }
            commitMidiRecording()
        }
    }

    private func slicePeaks(_ arr: [Float], _ from: Int, _ to: Int) -> [Float] {
        let a = max(0, min(from, arr.count))
        let b = max(a, min(to, arr.count))
        return Array(arr[a..<b])
    }

    /// Start a background audio capture on the first record-armed audio track for this play pass.
    /// Runs whenever the transport rolls with a track armed — even if Record was never pressed — so
    /// the audio around a punch-in is retained. Discarded at Stop unless Record committed it.
    func beginAudioCapture() {
        guard let handle, !audioRecordingActive,
              let armed = tracks.first(where: { $0.recordArmed && $0.kind == .audio }) else { return }
        recordCaptureStartSeconds = playheadSeconds
        audioRecordingActive = nc_engine_begin_audio_record(handle)
        if audioRecordingActive {
            recordCommitted = false
            recordTrackId = armed.id
            recordRegions = []
            currentPunchInSeconds = nil
            recordingClips = []                    // invisible until Record punches in
            captureLivePeaksL = []
            captureLivePeaksR = []
            recordingPeaksConsumed = 0
            recordingChannels = max(1, min(2, Int(nc_recording_channels(handle))))
        }
    }

    /// The track a punch-in commits to (the first record-armed audio track).
    private var armedAudioTrackId: Int? { tracks.first(where: { $0.recordArmed && $0.kind == .audio })?.id }

    /// End a background capture pass at Stop: if Record was pressed at least once, save the WAV and
    /// drop the clip (the whole captured pass is retained on disk); otherwise discard it silently.
    func endAudioCapturePass() {
        guard let handle, audioRecordingActive else { return }
        audioRecordingActive = false
        // Close any region still open at Stop (punched in, never punched out).
        if let pin = currentPunchInSeconds {
            recordRegions.append(PunchRegion(inSec: pin, outSec: playheadSeconds,
                                             inBucket: currentPunchInBucket, outBucket: captureLivePeaksL.count))
            currentPunchInSeconds = nil
        }
        let committed = recordCommitted
        let regions = recordRegions
        let captureStart = recordCaptureStartSeconds
        // Reset all live state.
        recordCommitted = false
        recordRegions = []
        recordingClips = []
        captureLivePeaksL = []
        captureLivePeaksR = []
        recordingPeaksConsumed = 0

        guard committed, !regions.isEmpty else {
            nc_engine_discard_audio_record(handle)   // played over an armed track but never recorded
            return
        }
        // Save the whole-pass WAV once, then drop one clip per punch region (each references a window
        // of the same file, so extending any clip reveals the surrounding background audio).
        var pathBuf = [CChar](repeating: 0, count: 1024)
        var errBuf = [CChar](repeating: 0, count: 256)
        guard nc_engine_finish_audio_record(handle, &pathBuf, 1024, &errBuf, 256) else {
            let msg = String(cString: errBuf)
            lastError = msg.isEmpty ? "오디오 녹음 저장 실패" : msg
            return
        }
        let path = String(cString: pathBuf)
        for r in regions {
            let offset = max(0, r.inSec - captureStart)
            var clipId = [CChar](repeating: 0, count: 128)
            var e2 = [CChar](repeating: 0, count: 256)
            _ = path.withCString { cpath in
                nc_engine_add_take_clip(handle, cpath, r.inSec, offset, r.outSec - r.inSec, &clipId, 128, &e2, 256)
            }
        }
        reloadClips()
        reloadTracks()
        refreshHistory()
        lastError = "오디오 녹음 완료 (\(regions.count)개 구간)"
    }

    /// While an audio take is capturing, pull its growing duration + coarse live peaks so the
    /// timeline can draw the waveform streaming out under the playhead (like the MIDI take region).
    private func serviceAudioRecordingWaveform(_ handle: OpaquePointer) {
        guard audioRecordingActive else { return }
        // Accumulate the WHOLE-pass peaks (from captureStart) incrementally — a small, constant lock.
        let total = Int(nc_recording_live_peak_count(handle))
        let newBuckets = total - recordingPeaksConsumed
        if newBuckets > 0 {
            var buf = [Float](repeating: 0, count: newBuckets * 2)
            let got = Int(nc_recording_live_peaks_since(handle, Int32(recordingPeaksConsumed), &buf, Int32(newBuckets)))
            for i in 0..<got {
                captureLivePeaksL.append(buf[i * 2])
                captureLivePeaksR.append(buf[i * 2 + 1])
            }
            recordingPeaksConsumed += got
        }
        // Build the visible clips: one per completed punch region + the growing current one. Nothing
        // shows during a plain background pass (no committed regions).
        guard recordCommitted, let tid = recordTrackId else {
            if !recordingClips.isEmpty { recordingClips = [] }
            return
        }
        var clips: [RecordingClip] = []
        for r in recordRegions {
            clips.append(RecordingClip(trackId: tid, startSeconds: r.inSec,
                                       durationSeconds: max(0.01, r.outSec - r.inSec),
                                       peaksL: slicePeaks(captureLivePeaksL, r.inBucket, r.outBucket),
                                       peaksR: slicePeaks(captureLivePeaksR, r.inBucket, r.outBucket)))
        }
        if let pin = currentPunchInSeconds {
            // Draw the growing clip exactly to the PLAYHEAD, never to the captured-seconds count —
            // the tap captures on its own (input) clock, which runs slightly ahead of the output
            // playhead, so using it made the clip overshoot the timeline (worse the longer you punch).
            let end = max(pin, playheadSeconds)
            clips.append(RecordingClip(trackId: tid, startSeconds: pin,
                                       durationSeconds: max(0.01, end - pin),
                                       peaksL: slicePeaks(captureLivePeaksL, currentPunchInBucket, captureLivePeaksL.count),
                                       peaksR: slicePeaks(captureLivePeaksR, currentPunchInBucket, captureLivePeaksR.count)))
        }
        recordingClips = clips
    }

    /// Count-in state: the take to open, and whether/what click to restore afterward.
    private var pendingRecord: (trackId: Int, atSeconds: Double)?
    private var countInRestoreClick = false
    private var countInForcedClick = false

    /// During a count-in the transport rolls but no take is open yet. Once the playhead reaches
    /// the record point, open the take there — the first played note lands exactly on the beat.
    private func serviceCountIn(_ handle: OpaquePointer) {
        guard let pending = pendingRecord else { return }
        guard playheadSeconds >= pending.atSeconds - 0.001 else { return }
        pendingRecord = nil
        nc_midi_record_begin(handle, Int32(pending.trackId), pending.atSeconds)
        reloadMidiRegions()
        lastError = "MIDI 녹음 중…"
    }

    /// Restore the click to the user's setting after a count-in that forced it on (whether the
    /// take recorded or was stopped early).
    private func endForcedClick() {
        guard let handle, countInForcedClick else { return }
        countInForcedClick = false
        if clickEnabled != countInRestoreClick {
            clickEnabled = countInRestoreClick
            nc_engine_set_metronome_enabled(handle, countInRestoreClick)
        }
    }

    /// Clear a pending count-in (stop pressed before the record point) and restore the click.
    private func cancelCountIn() {
        pendingRecord = nil
        endForcedClick()
    }

    /// Finish a MIDI take (on stop or record-off) and drop the recorded region on its track.
    private func commitMidiRecording() {
        cancelCountIn()   // stop during the count-in leaves a pending take + forced click to undo
        guard let handle, nc_midi_record_active(handle) else { return }
        let regionId = readString { nc_midi_record_commit(handle, $0, $1) }
        reloadMidiRegions()
        reloadTracks()
        refreshHistory()
        lastError = regionId.isEmpty ? nil : "MIDI 녹음 완료."
    }

    func toggleLoop() {
        setLoop(!loopEnabled)
    }

    func setLoop(_ enabled: Bool) {
        guard let handle, enabled != loopEnabled else { return }
        loopEnabled = enabled
        nc_project_set_loop_enabled(handle, loopEnabled)
    }

    func setLoopRoll(pre: Double, post: Double) {
        guard let handle else { return }
        nc_project_set_pre_post_roll(handle, pre, post)
        preRollSeconds = nc_project_pre_roll(handle)
        postRollSeconds = nc_project_post_roll(handle)
        refreshHistory()
    }

    func presentLoopRollSettings() {
        let alert = NSAlert()
        alert.messageText = "루프 프리/포스트롤"
        alert.informativeText = "루프 구간 앞뒤에 함께 반복할 시간을 초 단위로 입력하세요."
        alert.addButton(withTitle: "적용")
        alert.addButton(withTitle: "취소")
        let form = NSGridView(views: [
            [NSTextField(labelWithString: "프리롤"), NSTextField(string: String(format: "%.3f", preRollSeconds)),
             NSTextField(labelWithString: "초")],
            [NSTextField(labelWithString: "포스트롤"), NSTextField(string: String(format: "%.3f", postRollSeconds)),
             NSTextField(labelWithString: "초")]
        ])
        form.column(at: 1).width = 100
        form.rowSpacing = 8
        alert.accessoryView = form
        guard alert.runModal() == .alertFirstButtonReturn,
              let preField = form.cell(atColumnIndex: 1, rowIndex: 0).contentView as? NSTextField,
              let postField = form.cell(atColumnIndex: 1, rowIndex: 1).contentView as? NSTextField else { return }
        setLoopRoll(pre: max(0, preField.doubleValue), post: max(0, postField.doubleValue))
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

    /// Commits the current click timbre, accents, tempo/signature maps and groove to a
    /// real mono WAV clip on a newly-created Metronome audio track.
    func printMetronomeToTrack(loopRangeOnly: Bool) {
        guard let handle else { return }
        var message = [CChar](repeating: 0, count: 512)
        let ok = message.withUnsafeMutableBufferPointer {
            nc_metronome_print_to_track(handle, loopRangeOnly, $0.baseAddress, $0.count)
        }
        let text = String(cString: message)
        guard ok else {
            lastError = text.isEmpty ? "메트로놈 트랙을 만들지 못했습니다." : text
            return
        }
        reloadTracks()
        reloadClips()
        refreshHistory()
        if let printed = tracks.last(where: { $0.name.hasPrefix("Metronome") }) {
            selectedTrackId = printed.id
            selectedMixerTrackIds = [printed.id]
        }
        lastError = text.isEmpty ? "메트로놈 오디오 트랙을 만들었습니다." : text
    }

    /// The click resolution (자동/♩/♪/♬). Re-asserts the enabled state so it takes effect live.
    /// A manual subdivision change drops out of any genre groove (its pattern no longer fits).
    func setMetronomeSubdivision(_ subdivision: String) {
        guard let handle else { return }
        metronomeSubdivision = subdivision
        subdivision.withCString { nc_engine_set_metronome_subdivision(handle, $0) }
        clearMetronomeGenre()
        nc_engine_set_metronome_enabled(handle, clickEnabled)
    }

    /// Applies a groove genre: subdivision + swing + accent pattern in one go (Boss Dr. Beat /
    /// drum-machine style). "기본" clears the pattern back to the default hierarchy.
    func setMetronomeGenre(_ id: String) {
        guard let handle, let genre = Self.metronomeGenres.first(where: { $0.id == id }) else { return }
        metronomeGenre = id
        id.withCString { nc_engine_set_metronome_genre(handle, $0) }   // persist the picker's choice
        metronomeSubdivision = genre.subdivision
        grooveFeel = genre.feel
        grooveSwingAmount = genre.swing
        genre.subdivision.withCString { nc_engine_set_metronome_subdivision(handle, $0) }
        genre.feel.withCString { nc_engine_set_groove(handle, $0, Float(genre.swing)) }
        if genre.pattern.isEmpty {
            nc_engine_set_metronome_pattern(handle, nil, 0)
        } else {
            genre.pattern.withUnsafeBufferPointer {
                nc_engine_set_metronome_pattern(handle, $0.baseAddress, Int32($0.count))
            }
        }
        nc_engine_set_metronome_enabled(handle, clickEnabled)
    }

    /// Clears the genre accent pattern (a manual subdivision/swing tweak leaves the preset).
    private func clearMetronomeGenre() {
        guard let handle else { return }
        metronomeGenre = "straight"
        "straight".withCString { nc_engine_set_metronome_genre(handle, $0) }
        nc_engine_set_metronome_pattern(handle, nil, 0)
    }

    /// Reload the metronome UI + live click from an opened project (these persist in the .ndaw). The
    /// genre id fully determines subdivision / swing / accent pattern, so re-applying it restores
    /// those live; gain / sound / accent-first are re-asserted directly.
    private func reloadMetronomeState() {
        guard let handle else { return }
        metronomeGain = Double(nc_metronome_gain(handle))
        metronomeSound = readEngineString { nc_metronome_sound(handle, $0, $1) }
        metronomeAccentFirst = nc_metronome_accent_first(handle)
        nc_engine_set_metronome_gain(handle, Float(metronomeGain))
        metronomeSound.withCString { nc_engine_set_metronome_sound(handle, $0) }
        nc_engine_set_metronome_accent_first(handle, metronomeAccentFirst)
        let genre = readEngineString { nc_metronome_genre(handle, $0, $1) }
        if genre != "straight", Self.metronomeGenres.contains(where: { $0.id == genre }) {
            setMetronomeGenre(genre)   // restores subdivision / swing / pattern live + in the UI
        } else {
            metronomeGenre = genre.isEmpty ? "straight" : genre
            nc_engine_set_metronome_enabled(handle, clickEnabled)
        }
    }

    /// Click level (linear 0..2). Re-asserts enabled so it applies live.
    func setMetronomeGain(_ gain: Double) {
        guard let handle else { return }
        metronomeGain = max(0, min(2, gain))
        nc_engine_set_metronome_gain(handle, Float(metronomeGain))
        nc_engine_set_metronome_enabled(handle, clickEnabled)
    }

    /// Click timbre (beep/wood/rim/cowbell).
    func setMetronomeSound(_ sound: String) {
        guard let handle else { return }
        metronomeSound = sound
        sound.withCString { nc_engine_set_metronome_sound(handle, $0) }
        nc_engine_set_metronome_enabled(handle, clickEnabled)
    }

    /// Groove feel + swing amount. Feel "straight" ignores the amount; "shuffle" uses it.
    /// A manual swing change drops out of any genre groove.
    func setGroove(feel: String, swingAmount: Double? = nil) {
        guard let handle else { return }
        grooveFeel = feel
        if let swingAmount { grooveSwingAmount = max(0.5, min(0.9, swingAmount)) }
        feel.withCString { nc_engine_set_groove(handle, $0, Float(grooveSwingAmount)) }
        clearMetronomeGenre()
        nc_engine_set_metronome_enabled(handle, clickEnabled)
    }

    /// Toggle the downbeat accent (off = an even click, easier to play against).
    func setMetronomeAccentFirst(_ accent: Bool) {
        guard let handle else { return }
        metronomeAccentFirst = accent
        nc_engine_set_metronome_accent_first(handle, accent)
        nc_engine_set_metronome_enabled(handle, clickEnabled)
    }

    func setCountInBars(_ bars: Int) {
        countInBars = max(0, min(2, bars))
    }

    private func setTransport(running: Bool) {
        guard let handle else { return }
        // Stopping the transport ends any MIDI take (space/stop, not just the record button).
        if !running {
            if recording { recording = false; nc_engine_set_recording(handle, false) }
            commitMidiRecording()
            endAudioCapturePass()   // commit the take if Record was pressed, else discard the scratch
        }
        if running, !transportRunning { playStartSeconds = playheadSeconds }   // remember for stop
        nc_engine_set_transport_running(handle, running)
        transportRunning = running
        transportWallClockBase = playheadSeconds
        transportWallClockStart = CACurrentMediaTime()
        // Pro Tools style: once the transport rolls, a record-armed audio track captures in the
        // background even without Record pressed (kept only if Record commits it before Stop).
        // Deferred to the next runloop so opening the capture device never stutters the transport
        // start; when Record triggers this, toggleRecording begins the capture synchronously first.
        if running {
            DispatchQueue.main.async { [weak self] in self?.beginAudioCapture() }
        }
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

    /// Refresh the UI after an edit made outside the normal view path (e.g. the AI
    /// assistant applying a command through the bridge). Reloads tracks and undo state.
    func reloadAfterExternalEdit() {
        reloadTracks()
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
                isStereo: readEngineString { nc_track_channel_format(handle, i, $0, $1) } != "mono",
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
                instrumentLayers: {
                    let count = Int(nc_track_instrument_slot_count(handle, i))
                    return (0..<count).compactMap { s -> InstrumentLayer? in
                        let name = readEngineString { nc_track_instrument_slot_name(handle, i, Int32(s), $0, $1) }
                        guard !name.isEmpty, name != "No Instrument" else { return nil }
                        return InstrumentLayer(name: name,
                                               muted: nc_track_instrument_slot_bypassed(handle, i, Int32(s)),
                                               soloed: nc_track_instrument_slot_soloed(handle, i, Int32(s)))
                    }
                }(),
                sends: (0..<sendCount).map { slot in
                    TrackSend(bus: readEngineString { nc_track_send_bus(handle, i, Int32(slot), $0, $1) },
                              gainDb: nc_track_send_gain_db(handle, i, Int32(slot)),
                              pan: nc_track_send_pan(handle, i, Int32(slot)),
                              preFader: nc_track_send_pre_fader(handle, i, Int32(slot)))
                },
                consoleModel: readEngineString { nc_track_console_model(handle, i, $0, $1) },
                consoleModuleOrder: readEngineString { nc_track_console_module_order(handle, i, $0, $1) },
                consoleFilterEnabled: "filterEnabled".withCString { nc_track_console_bool(handle, i, $0) },
                consoleFilterCircuitMode: "filterCircuitMode".withCString { nc_track_console_bool(handle, i, $0) },
                consoleHighPassEnabled: "highPassEnabled".withCString { nc_track_console_bool(handle, i, $0) },
                consoleLowPassEnabled: "lowPassEnabled".withCString { nc_track_console_bool(handle, i, $0) },
                consoleEqEnabled: "eqEnabled".withCString { nc_track_console_bool(handle, i, $0) },
                consoleEqCircuitMode: "eqCircuitMode".withCString { nc_track_console_bool(handle, i, $0) },
                consoleEqHfBell: "eqHfBell".withCString { nc_track_console_bool(handle, i, $0) },
                consoleEqLfBell: "eqLfBell".withCString { nc_track_console_bool(handle, i, $0) },
                consoleEqEMode: "eqEMode".withCString { nc_track_console_bool(handle, i, $0) },
                consoleCompEnabled: "compEnabled".withCString { nc_track_console_bool(handle, i, $0) },
                consoleCompCircuitMode: "compCircuitMode".withCString { nc_track_console_bool(handle, i, $0) },
                consoleCompFastAttack: "compFastAttack".withCString { nc_track_console_bool(handle, i, $0) },
                consoleCompPeakMode: "compPeakMode".withCString { nc_track_console_bool(handle, i, $0) },
                consoleGateEnabled: "gateEnabled".withCString { nc_track_console_bool(handle, i, $0) },
                consoleGateCircuitMode: "gateCircuitMode".withCString { nc_track_console_bool(handle, i, $0) },
                consoleSaturatorEnabled: "saturatorEnabled".withCString { nc_track_console_bool(handle, i, $0) },
                consoleDualMono: "dualMono".withCString { nc_track_console_bool(handle, i, $0) },
                consoleSaturatorCircuitMode: "saturatorCircuitMode".withCString { nc_track_console_bool(handle, i, $0) },
                consoleExpanderMode: "expanderMode".withCString { nc_track_console_bool(handle, i, $0) },
                consoleGateFastAttack: "gateFastAttack".withCString { nc_track_console_bool(handle, i, $0) }
            )
        }
        // Per-track PDC (samples), so each strip can show how much it's delayed to stay aligned.
        for i in tracks.indices { tracks[i].delayCompSamples = Int(nc_track_delay_compensation_samples(handle, Int32(tracks[i].id))) }

        reloadMasterInserts()
    }

    /// Show each mixer strip's plugin-delay-compensation readout at its base. Off by default.
    @Published var showChannelDelayComp = false
    func setShowChannelDelayComp(_ on: Bool) {
        showChannelDelayComp = on
        UserDefaults.standard.set(on, forKey: "nc.showChannelDelayComp")
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

    // Console model shown on each module's name plate. UI/label only for now — the per-model
    // DSP character is future work; this is the switch that will drive it.
    static let consoleModels = ["SSL 4000E", "SSL 4000G", "SSL 9000K", "Neve 8078", "API Vision", "Neuracoust NC"]
    @Published var consoleModel: String = "SSL 4000E"
    func setConsoleModel(_ name: String) { consoleModel = name }

    func consoleValue(_ id: Int, _ parameter: String) -> Float {
        guard let handle else { return 0 }
        return parameter.withCString { nc_track_console_value(handle, Int32(id), $0) }
    }

    func setConsoleBool(_ id: Int, _ parameter: String, _ value: Bool) {
        guard let handle else { return }
        parameter.withCString { nc_track_set_console_bool(handle, Int32(id), $0, value) }
        // A console switch changes one strip only. Reloading the entire mixer here
        // invalidated every channel, recomputed aligned heights, and could make the
        // mixer appear frozen while switching FLT/EQ/GAT/CMP/SAT.
        syncTrack(id)
        refreshHistory()
    }

    func setConsoleModuleOrder(_ id: Int, _ order: [String]) {
        guard let handle else { return }
        order.joined(separator: ",").withCString { nc_track_set_console_module_order(handle, Int32(id), $0) }
        reloadTracks(); refreshHistory()
    }

    func setConsoleValue(_ id: Int, _ parameter: String, _ value: Float) {
        guard let handle else { return }
        parameter.withCString { nc_track_set_console_value(handle, Int32(id), $0, value) }
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
        reloadRecordControllerState()
        refreshHistory()
    }

    func redo() {
        guard let handle, nc_history_redo(handle) else { return }
        reloadTracks()
        reloadClips()
        reloadMarkers()
        reloadMidiRegions()
        reloadMonitorState()
        reloadRecordControllerState()
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

    /// Save a self-contained COPY (collecting all external media — audio + video) to a chosen
    /// folder, while continuing to work on the CURRENT document. For archiving or handing a session
    /// off with everything it references, even media that lives outside the project folder.
    func saveProjectCopy() {
        guard let handle else { return }
        let base = (projectName.isEmpty ? "Untitled" : projectName) + " Copy"
        guard let (folderURL, ndawURL) = promptForProjectFolder(defaultName: base) else { return }
        var errorBuffer = [CChar](repeating: 0, count: 256)
        let copied = nc_project_save_copy(handle, ndawURL.path, &errorBuffer, errorBuffer.count)
        if copied < 0 {
            lastError = String(cString: errorBuffer)
            return
        }
        applyProjectFolderIcon(to: folderURL)
        rememberRecentProject(ndawURL)
        let info = NSAlert()
        info.messageText = "복사본으로 저장 완료"
        info.informativeText = copied > 0
            ? "자체 완결형 복사본을 저장했습니다 — 작업 폴더 밖의 미디어 \(copied)개를 함께 모았습니다. 현재 작업 문서는 그대로입니다."
            : "자체 완결형 복사본을 저장했습니다(모든 미디어가 이미 안에 있었습니다). 현재 작업 문서는 그대로입니다."
        info.runModal()
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
        preRollSeconds = nc_project_pre_roll(handle)
        postRollSeconds = nc_project_post_roll(handle)
        reloadTracks()
        reloadClips()
        reloadMarkers()
        reloadMidiRegions()
        reloadMonitorState()
        reloadRecordControllerState()
        reloadMetronomeState()
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
        var fadeInCurvature: Double = 0.0
        var fadeOutCurvature: Double = 0.0
        let gainDb: Float
        // Non-destructive processing state, mirrored into the waveform drawing.
        var muted: Bool = false
        var reversed: Bool = false
        var polarityInverted: Bool = false
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

    /// Continuous fade shape bend from the editor's middle handle. `edge` picks which side; the other
    /// side's curvature is preserved.
    func setClipFadeCurvature(_ clipId: String, fadeIn: Bool, _ curvature: Double) {
        guard let handle, let clip = clips.first(where: { $0.id == clipId }) else { return }
        let inC = fadeIn ? curvature : clip.fadeInCurvature
        let outC = fadeIn ? clip.fadeOutCurvature : curvature
        if nc_clip_set_fade_curvature(handle, clipId, inC, outC) { reloadClips(); refreshHistory() }
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
    @Published var selectedTrackId: Int? {
        didSet {
            // The selected instrument track hears the keyboard (Logic/Live style) so playing
            // just works after loading an instrument — no need to arm the track first.
            guard let handle else { return }
            nc_set_live_midi_target(handle, Int32(selectedTrackId ?? -1))
        }
    }

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

    /// The app is a monitor-station SHELL that hosts the DAW. In compact mode only the monitor
    /// station shows (a small always-available controller); expanding reveals the full DAW. The
    /// engine keeps running across the switch, so monitoring/playback never drops. Persisted.
    @Published var compactMonitorMode: Bool = UserDefaults.standard.bool(forKey: "nc.compactMonitorMode") {
        didSet { UserDefaults.standard.set(compactMonitorMode, forKey: "nc.compactMonitorMode") }
    }
    /// Start in compact monitor mode when launched (used by the login-item auto-launch).
    @Published var launchInMonitorMode: Bool = UserDefaults.standard.bool(forKey: "nc.launchInMonitorMode") {
        didSet { UserDefaults.standard.set(launchInMonitorMode, forKey: "nc.launchInMonitorMode") }
    }
    func expandToDaw() { compactMonitorMode = false }
    func collapseToMonitor() { compactMonitorMode = true }

    /// Launch at login (a login item) so the monitor station is always up. Reflects the real
    /// SMAppService registration state; toggling registers/unregisters the app.
    @Published var launchAtLogin: Bool = {
        if #available(macOS 13.0, *) { return SMAppService.mainApp.status == .enabled }
        return false
    }() {
        didSet { applyLoginItem() }
    }
    private func applyLoginItem() {
        guard #available(macOS 13.0, *) else {
            lastError = "로그인 항목은 macOS 13 이상에서 지원됩니다."
            return
        }
        do {
            if launchAtLogin { try SMAppService.mainApp.register() }
            else { try SMAppService.mainApp.unregister() }
        } catch {
            lastError = "로그인 항목 설정 실패: \(error.localizedDescription)"
        }
    }

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
        // Cap the zoomed-out span at 1 h (was 10 min, too small for long songs to fully zoom out).
        let duration = min(3600, max(0.25, visibleDuration * factor))
        // Anchor on the SELECTED clip(s) if any, else the playhead — so zoom keeps what you care
        // about centred instead of the geometric middle of the (possibly far-off) visible span.
        let sel = clips.filter { selectedClipIds.contains($0.id) }
        let anchor: Double
        if !sel.isEmpty {
            let lo = sel.map { $0.startSeconds }.min() ?? 0
            let hi = sel.map { $0.startSeconds + $0.durationSeconds }.max() ?? 0
            anchor = (lo + hi) / 2
        } else {
            anchor = playheadSeconds
        }
        setViewport(start: max(0, anchor - duration / 2), duration: duration)
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
                                        : parameter.id == "track.mute" ? (track.muted ? 1 : 0)
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
                                          fadeInCurve: clip.fadeInCurve,
                                          fadeOutCurve: clip.fadeOutCurve,
                                          fadeInCurvature: clip.fadeInCurvature,
                                          fadeOutCurvature: clip.fadeOutCurvature,
                                          gainDb: clip.gainDb,
                                          muted: clip.muted,
                                          reversed: clip.reversed,
                                          polarityInverted: clip.polarityInverted)
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
            loopEnabled: loopEnabled,
            editRangeLane: editRangeLane
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

    /// LIVE-DRAG move (not the commit): slide the clip in place without a plan rebuild, so the music
    /// keeps playing while you drag. commitClipGesture reconciles once on drop.
    func previewMoveClip(_ clipId: String, to startSeconds: Double) {
        guard let handle else { return }
        if nc_clip_update_start(handle, clipId, startSeconds) { reloadClips() }
    }

    /// LIVE-DRAG multi-clip move (delta), lightweight like previewMoveClip.
    func previewMoveSelection(by deltaSeconds: Double) {
        guard let handle, deltaSeconds != 0 else { return }
        if (withClipIds(selection, { nc_clip_update_start_many(handle, $0, $1, deltaSeconds) }) ?? 0) > 0 {
            reloadClips()
        }
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

    /// Option-drag copy of a multi-selection: duplicate every selected clip in place, select the
    /// copies, and return the copy of `anchorId` so the timeline keeps dragging the copies while the
    /// originals stay put. One undo step for the whole duplication.
    func beginCopySelection(anchorId: String) -> String? {
        guard let handle else { return nil }
        let originals = selection   // stable id order over the current selection
        guard !originals.isEmpty else { return nil }
        var newIds: [String] = []
        var newAnchor: String?
        for id in originals {
            guard let clip = clips.first(where: { $0.id == id }) else { continue }
            var buffer = [CChar](repeating: 0, count: 128)
            guard nc_clip_duplicate(handle, id, &buffer, buffer.count) else { continue }
            let newId = String(cString: buffer)
            // Pin the copy exactly onto its original (same track, same start) so the selection-move
            // delta that follows is measured from the right place.
            _ = nc_clip_move(handle, newId, clip.startSeconds)
            newIds.append(newId)
            if id == anchorId { newAnchor = newId }
        }
        guard !newIds.isEmpty else { return nil }
        selectedClipIds = Set(newIds)
        reloadClips()
        refreshHistory()
        return newAnchor ?? newIds.first
    }

    // LIVE-DRAG trim: lightweight in-place bounds patch, like previewMoveClip. The heavy rebuild used
    // to run per frame and stopped the music at the first drag frame; the drop reconciles once
    // (commitClipGesture "Trim clip").
    func trimClipStart(_ clipId: String, to startSeconds: Double) {
        guard let handle else { return }
        if nc_clip_update_trim_start(handle, clipId, startSeconds) { reloadClips() }
    }

    func trimClipEnd(_ clipId: String, to endSeconds: Double) {
        guard let handle else { return }
        if nc_clip_update_trim_end(handle, clipId, endSeconds) { reloadClips() }
    }

    /// Roll edit: drag the shared boundary of two abutting clips. Live in-place (seamless during
    /// playback); commit once on drop (commitClipGesture "Roll edit").
    func rollBoundary(_ leftId: String, _ rightId: String, to boundarySeconds: Double) {
        guard let handle else { return }
        if nc_clip_roll_boundary(handle, leftId, rightId, boundarySeconds) { reloadClips() }
    }

    func setClipFades(_ clipId: String, fadeIn: Double, fadeOut: Double) {
        guard let handle else { return }
        if nc_clip_set_fades(handle, clipId, fadeIn, fadeOut) { reloadClips() }
    }

    /// Crossfade length = the overlap of two clips. Adjusting it TRIMS THE FRONT CLIP'S END (never
    /// moves the back clip), so the back clip's audio stays sync-locked in place — extending the front
    /// clip's tail deeper into the overlap lengthens the crossfade, shortening it retracts. The render
    /// derives the fades from the overlap, so this alone reshapes the crossfade. One undo step; the
    /// front clip's source tail bounds how long the crossfade can grow (trim clamps to it).
    func setCrossfadeLength(_ leftId: String, _ rightId: String, to seconds: Double) {
        guard let handle,
              let right = clips.first(where: { $0.id == rightId }) else { return }
        let maxOverlap = right.durationSeconds   // can't overlap more than the back clip's length
        let overlap = max(0, min(maxOverlap, seconds))
        let newLeftEnd = right.startSeconds + overlap
        if nc_clip_trim_end(handle, leftId, newLeftEnd) {
            reloadClips(); refreshHistory()
        }
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
    enum MultiFileLayout { case multitrack, sequential, cancel }
    /// Dropping several files at once: ask whether to spread them across new tracks or lay them
    /// end-to-end on one track (the Logic/Pro Tools import prompt).
    private func askMultiFileLayout(_ count: Int) -> MultiFileLayout {
        let alert = NSAlert()
        alert.messageText = "여러 오디오 파일 가져오기 (\(count)개)"
        alert.informativeText = "선택한 파일들을 어떻게 배치할까요?"
        alert.addButton(withTitle: "멀티트랙 (파일별 새 트랙)")
        alert.addButton(withTitle: "한 트랙에 연이어")
        alert.addButton(withTitle: "취소")
        switch alert.runModal() {
        case .alertFirstButtonReturn: return .multitrack
        case .alertSecondButtonReturn: return .sequential
        default: return .cancel
        }
    }

    func dropAudio(onLane laneIndex: Int, atSeconds seconds: Double, urls: [URL]) {
        let audio = urls.filter { nc_audio_import_supported($0.path) }
        guard !audio.isEmpty else {
            lastError = "가져올 수 있는 오디오 파일이 없습니다."
            return
        }
        let start = snap(seconds)
        if audio.count > 1 {
            switch askMultiFileLayout(audio.count) {
            case .cancel: return
            case .multitrack: importAudioMultitrack(at: start, from: audio); return
            case .sequential: break   // fall through to the single-track sequential import
            }
        }
        var targetLane = laneIndex
        if trackId(forLane: targetLane) == nil {
            guard let handle, nc_track_add_audio(handle) >= 0 else { return }
            reloadTracks()
            targetLane = laneTracks.count - 1   // the freshly-added track is the last lane
        }
        guard let trackId = trackId(forLane: targetLane) else { return }
        importAudio(intoTrack: trackId, at: start, from: audio)
    }

    /// A .mid file (or several) dropped onto the timeline — from the MIDI library or Finder. Lands each
    /// as a MIDI region on a MIDI-capable track (reuses the target lane if it's instrument/midi, else
    /// makes a new instrument track). Load an instrument on the track to hear it.
    func dropMidi(onLane laneIndex: Int, atSeconds seconds: Double, urls: [URL]) {
        guard let handle else { return }
        let midis = urls.filter { ["mid", "midi"].contains($0.pathExtension.lowercased()) }
        guard !midis.isEmpty else { return }
        let start = snap(seconds)
        // A single-track loop lands on the target lane (if it can hold MIDI); a multi-track song splits
        // into one instrument track per part. The bridge decides and creates tracks as needed, so pass
        // the preferred lane only when it is MIDI-capable, else -1 (auto-create).
        var preferred: Int32 = -1
        if laneIndex < laneTracks.count, canHoldMidi(laneTracks[laneIndex]),
           let tid = trackId(forLane: laneIndex) { preferred = Int32(tid) }
        var made = 0
        for url in midis {
            var err = [CChar](repeating: 0, count: 256)
            let n = Int(nc_midi_import_file_auto(handle, url.path, preferred, start, &err, err.count))
            if n > 0 { made += n; preferred = -1 }   // later files auto-create their own tracks
            else if made == 0 {
                let m = String(cString: err); if !m.isEmpty { Diagnostics.shared.log("midi import: \(m)") }
            }
        }
        reloadTracks(); reloadClips(); reloadMidiRegions(); refreshHistory()
        if made == 0 { lastError = "MIDI 파일을 가져올 수 없습니다." }
    }

    /// Insert a MIDI file onto the currently-selected (or a new) instrument track at the playhead — the
    /// MIDI library's click-to-insert, an alternative to dragging onto a lane.
    func insertMidiFileAtPlayhead(_ path: String) {
        let url = URL(fileURLWithPath: path)
        let lane = selectedLaneForMidiInsert()
        dropMidi(onLane: lane, atSeconds: playheadSeconds, urls: [url])
    }

    /// Prefer a selected MIDI-capable lane; else the last instrument/midi lane; else one-past (new track).
    private func selectedLaneForMidiInsert() -> Int {
        if let selId = selectedTrackId,
           let lane = laneTracks.firstIndex(where: { $0.id == selId }), canHoldMidi(laneTracks[lane]) {
            return lane
        }
        if let last = laneTracks.lastIndex(where: { canHoldMidi($0) }) { return last }
        return laneTracks.count
    }

    /// One new audio track per file, all starting at the same time — the multitrack import layout.
    private func importAudioMultitrack(at start: Double, from urls: [URL]) {
        guard let handle else { return }
        let choice = askImportAnalysisChoice()          // one analysis decision for the whole batch
        let analyze = choice != .skip
        let applyToTimeline = choice == .apply
        var usedTrackNames = Set(tracks.map(\.name))
        for url in urls {
            guard nc_audio_import_supported(url.path) else { continue }
            let trackId = Int(nc_track_add_audio(handle))
            guard trackId >= 0 else { return }

            // In a multitrack import each source file represents a channel/part, so use its
            // filename as the channel name instead of leaving anonymous "Audio N" tracks.
            // Keep the user's meaningful punctuation and language; only remove the extension
            // and collapse whitespace. Duplicate stems receive a stable numeric suffix.
            let trackName = uniqueImportedTrackName(for: url, usedNames: &usedTrackNames)
            _ = trackName.withCString { nc_track_rename(handle, Int32(trackId), $0) }

            var buffer = [CChar](repeating: 0, count: 512)
            if !nc_audio_import_analyzed(handle, Int32(trackId), url.path, start, analyze, applyToTimeline,
                                         &buffer, buffer.count) {
                lastError = String(cString: buffer)
            }
        }
        reloadTracks()
        reloadClips()
        if applyToTimeline {
            reloadMarkers()
            tempoBpm = Int(nc_project_tempo_bpm(handle))
            timeSignature = (Int(nc_project_time_signature_numerator(handle)),
                             Int(nc_project_time_signature_denominator(handle)))
        }
        refreshHistory()
    }

    private func uniqueImportedTrackName(for url: URL, usedNames: inout Set<String>) -> String {
        let stem = url.deletingPathExtension().lastPathComponent
        let cleaned = stem.split(whereSeparator: \.isWhitespace).joined(separator: " ")
        let base = cleaned.isEmpty ? "Audio" : cleaned
        var candidate = base
        var suffix = 2
        while usedNames.contains(candidate) {
            candidate = "\(base) \(suffix)"
            suffix += 1
        }
        usedNames.insert(candidate)
        return candidate
    }

    func moveClipToLane(_ clipId: String, laneIndex: Int, startSeconds: Double) {
        guard let handle, laneIndex < laneTracks.count else { return }
        let trackId = laneTracks[laneIndex].id

        var buffer = [CChar](repeating: 0, count: 128)
        guard nc_clip_move_to_track(handle, clipId, Int32(trackId), startSeconds,
                                    &buffer, buffer.count) else { return }
        selectClip(String(cString: buffer))
        // Overlapping the destination lane's clips crossfades them — the render derives that from
        // the overlap itself, so there is nothing to bake here.
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

    /// True when this build can read AAF (libAAF present), so the menu can hide the command.
    var aafImportAvailable: Bool { nc_aaf_import_available() }

    /// Imports an AAF session (Pro Tools / Media Composer / Resolve). Replaces the open document,
    /// so it asks before discarding unsaved work, exactly like opening a project.
    func importAafSession() {
        guard let handle else { return }
        guard confirmDiscardingChanges() else { return }
        let panel = NSOpenPanel()
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.allowedContentTypes = [.init(filenameExtension: "aaf")].compactMap { $0 }
        panel.prompt = "가져오기"
        panel.message = "가져올 AAF 세션을 고르세요"
        guard panel.runModal() == .OK, let url = panel.url else { return }

        var message = [CChar](repeating: 0, count: 512)
        let ok = nc_import_aaf(handle, url.path, &message, message.count)
        let text = String(cString: message)
        if ok {
            afterProjectReplaced()
            lastError = text
        } else {
            lastError = text.isEmpty ? "AAF를 가져오지 못했습니다" : text
        }
    }

    /// Exports one WAV per track — the session-interchange path every DAW accepts.
    ///
    /// Each stem runs the full session length from 00:00, so dropping them all at zero in the other
    /// DAW reproduces the arrangement exactly. Unlike a mixdown this keeps the tracks separable, and
    /// unlike AAF/OMF it needs no shared container format to be understood on the far side.
    func exportStems() {
        guard let handle, !bouncing else { return }
        guard !clips.isEmpty || !midiRegions.isEmpty else {
            lastError = "내보낼 내용이 없습니다"
            return
        }
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        panel.prompt = "스템 내보내기"
        panel.message = "트랙별 WAV를 저장할 폴더를 고르세요"
        guard panel.runModal() == .OK, let url = panel.url else { return }

        // The stems folder is named after the project so several exports do not collide.
        let folder = url.appendingPathComponent(
            (projectName.isEmpty ? "Session" : projectName) + " Stems", isDirectory: true)

        bouncing = true
        let folderPath = folder.path
        Task.detached(priority: .userInitiated) {
            var error = [CChar](repeating: 0, count: 512)
            let count = Int(nc_bounce_stems(handle, folderPath, &error, error.count))
            let message = String(cString: error)
            await MainActor.run {
                self.bouncing = false
                if count > 0 {
                    self.lastError = "스템 \(count)개를 내보냈습니다 — \(folderPath)"
                    NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: folderPath)])
                } else {
                    self.lastError = message.isEmpty ? "스템을 내보내지 못했습니다" : message
                }
            }
        }
    }

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
    /// Every track the Delete key targets: the whole multi-track selection (Pro Tools style), minus
    /// Master/Monitor which the engine refuses. Falls back to the single last-clicked track.
    private var deletableSelectedTracks: [Track] {
        var ids = selectedMixerTrackIds
        if let id = selectedTrackId { ids.insert(id) }
        // Keep timeline order so the confirm list and any messaging read top-to-bottom.
        return tracks.filter { ids.contains($0.id) && !$0.kind.isMasterish }
    }

    func deleteSelectedTrack() {
        guard let handle else { return }
        let targets = deletableSelectedTracks
        guard !targets.isEmpty else { return }

        // Ask first when any track carries work — clips, MIDI, an instrument, inserts or sends —
        // so a Delete never silently discards it. Empty tracks delete straight away. One dialog
        // covers the whole selection.
        var parts: [String] = []
        let clipCount = targets.reduce(0) { acc, t in acc + clips.filter { $0.trackName == t.name }.count }
        let regionCount = targets.reduce(0) { acc, t in acc + midiRegions.filter { $0.trackName == t.name }.count }
        let instrumentCount = targets.filter { !$0.instrumentName.isEmpty && $0.instrumentName != "No Instrument" }.count
        let insertCount = targets.reduce(0) { acc, t in acc + t.inserts.filter { !$0.isEmpty }.count }
        let sendCount = targets.reduce(0) { acc, t in acc + t.sends.count }
        if clipCount > 0 { parts.append("클립 \(clipCount)개") }
        if regionCount > 0 { parts.append("MIDI 리전 \(regionCount)개") }
        if instrumentCount > 0 { parts.append("악기 \(instrumentCount)개") }
        if insertCount > 0 { parts.append("인서트 \(insertCount)개") }
        if sendCount > 0 { parts.append("센드 \(sendCount)개") }
        if !parts.isEmpty {
            let alert = NSAlert()
            alert.messageText = targets.count == 1
                ? "\(targets[0].name) 트랙을 삭제할까요?"
                : "선택한 \(targets.count)개 트랙을 삭제할까요?"
            let names = targets.map { $0.name }.joined(separator: ", ")
            alert.informativeText = (targets.count == 1 ? "이 트랙의 " : "\(names) 트랙의 ")
                + parts.joined(separator: ", ") + "도 함께 삭제됩니다."
            alert.addButton(withTitle: "삭제")
            alert.addButton(withTitle: "취소")
            guard alert.runModal() == .alertFirstButtonReturn else { return }
        }

        let ids = targets.map { Int32($0.id) }
        let deleted = ids.withUnsafeBufferPointer { buf in
            nc_track_delete_many(handle, buf.baseAddress, Int32(buf.count), true)
        }
        guard deleted else {
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

    /// Master and Monitor refuse deletion in the engine; enable Delete when any selected track can go.
    var canDeleteSelectedTrack: Bool {
        !deletableSelectedTracks.isEmpty
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
        // Overlapping a clip onto a same-track neighbour becomes a crossfade — but that is now
        // derived by the render from the overlap itself (Pro Tools / Cubase / Logic: pull the clips
        // apart and the crossfade is gone), so the drop only has to canonicalize the playlist.
        // Move AND trim both slid/stretched the clip in place during the drag (no reconcile per frame).
        // Canonicalize the playlist + render once now, on drop — seamless (updateProject preserves the
        // continuous-playback buffers), so no click.
        if stepName == "Move clip" || stepName == "Move clips" || stepName == "Trim clip"
            || stepName == "Roll edit" {
            if let handle { nc_project_reconcile(handle) }
        }
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

    /// 스팟: where the clip FIRST landed on the timeline (import) — the Pro-Tools
    /// original time stamp. -1 = unknown (projects saved before the field existed).
    func clipOriginalStart(_ id: String) -> Double {
        guard let handle else { return -1 }
        return nc_clip_original_start_seconds(handle, id)
    }

    /// 스팟: move each clip back to its own original position. One undo step,
    /// recorded by the bridge (a discrete action, unlike a drag).
    func spotClipsToOriginal(_ ids: [String]) {
        guard let handle else { return }
        guard let moved = withClipIds(ids, { nc_clip_spot_to_original_many(handle, $0, $1) }),
              moved > 0 else { return }
        reloadClips()
        refreshHistory()
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

    /// Consolidate the selected clips (Pro Tools ⌥⇧3): bake each track's selection — gain, fades and
    /// crossfades — into one new audio file and replace them with a single clip. Handles overlapping
    /// crossfaded clips that Glue can't join.
    func consolidateSelection() {
        guard let handle, !selection.isEmpty else { lastError = "합칠 클립을 선택하세요."; return }
        var errBuf = [CChar](repeating: 0, count: 256)
        let ok = withClipIds(selection) { ptr, count in
            nc_clip_consolidate(handle, ptr, count, &errBuf, 256)
        } ?? false
        if ok {
            selectedClipIds = []
            reloadClips()
            reloadTracks()
            refreshHistory()
            lastError = "통합 완료"
        } else {
            let msg = String(cString: errBuf)
            lastError = msg.isEmpty ? "통합 실패" : msg
        }
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
        // Explicit clip selection: glue ONLY those clips to each other. Using a span here would let the
        // span's own end boundaries catch the unselected neighbours that abut them (the "gluing 2 clips
        // swallowed the ones before and after" bug). The range path is only for a bare edit range.
        if !selectedClipIds.isEmpty {
            let ids = clips.filter { selectedClipIds.contains($0.id) }.map { $0.id }
            guard ids.count >= 2 else { return }
            let healed = withClipIds(ids) { nc_clip_glue_selection(handle, $0, $1) } ?? 0
            if healed > 0 {
                reloadClips()
                refreshHistory()
            }
            return
        }
        guard hasEditRange else { return }
        if nc_clip_glue_range(handle, loopStartSeconds, loopEndSeconds) > 0 {
            reloadClips()
            refreshHistory()
        }
    }

    func deleteSelectedClips() {
        guard let handle else { return }
        // Shuffle mode ripples the deleted clip's OWN track left (Pro Tools default), not every track.
        if editMode == .shuffle, !selection.isEmpty {
            if let deleted = withClipIds(selection, { nc_clip_shuffle_delete_many(handle, $0, $1) }), deleted > 0 {
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
    @Published var editingRegionId: String? {
        didSet { if editingRegionId != oldValue { selectedNoteIds = [] } }
    }
    /// Notes selected in the piano roll (the highlighted ones the velocity field edits).
    /// Cleared whenever the open region changes.
    @Published var selectedNoteIds: Set<String> = []

    /// The piano-roll pointer tool (Cubase/Logic style). Select never creates a note on a plain
    /// click — that was the old always-draw behaviour. Draw paints notes; Erase deletes them.
    enum PianoRollTool: String, CaseIterable {
        case smart, select, draw, erase
        var title: String {
            switch self {
            case .smart: return "스마트"
            case .select: return "선택"
            case .draw: return "연필"
            case .erase: return "지우개"
            }
        }
        var symbol: String {
            switch self {
            case .smart: return "cursorarrow.rays"
            case .select: return "cursorarrow"
            case .draw: return "pencil"
            case .erase: return "eraser"
            }
        }
    }
    @Published var pianoRollTool: PianoRollTool = .select

    /// The docked piano roll's height, dragged from its top edge.
    @Published var pianoRollHeight: CGFloat = 300
    /// The controller/velocity lane's height, dragged from the bar above it. Independent of
    /// the editor's own height: making room to shape velocities should not have to steal it
    /// from the keyboard.
    @Published var editorLaneHeight: CGFloat = 44
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

    @discardableResult
    func addNote(pitch: Int, startBeats: Double, durationBeats: Double, velocity: Int = 96) -> String? {
        guard let handle, let regionId = editingRegionId else { return nil }
        // A click that lands a pixel short of an existing note must not stack a second
        // one on top of it — silently doubled notes are hard to see and easy to hear.
        let occupied = notes(inRegion: regionId).contains {
            $0.pitch == pitch && startBeats < $0.startBeats + $0.durationBeats
                && $0.startBeats < startBeats + durationBeats
        }
        guard !occupied else { return nil }
        var buffer = [CChar](repeating: 0, count: 128)
        guard nc_midi_note_add(handle, regionId, Int32(pitch), startBeats, durationBeats,
                               Int32(velocity), &buffer, buffer.count) else { return nil }
        reloadMidiRegions()
        refreshHistory()
        return String(cString: buffer)
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
        selectedNoteIds.remove(noteId)
        refreshHistory()
        objectWillChange.send()
    }

    // MARK: Info line (the Cubase-style numeric editor for the focused note)

    /// The note the info line edits: the earliest-starting note in the selection.
    var focusedNote: MidiNote? {
        guard let regionId = editingRegionId, !selectedNoteIds.isEmpty else { return nil }
        return notes(inRegion: regionId).filter { selectedNoteIds.contains($0.id) }
            .min { $0.startBeats < $1.startBeats }
    }

    /// Sets the focused note's start in beats (info-line field or nudge). One undo step.
    func setNoteStart(_ noteId: String, beats: Double) {
        guard let handle, let regionId = editingRegionId,
              let note = notes(inRegion: regionId).first(where: { $0.id == noteId }) else { return }
        guard nc_midi_note_move(handle, regionId, noteId, Int32(note.pitch), max(0, beats)) else { return }
        recordGesture("Note position")
        reloadMidiRegions()
    }

    func setNoteLength(_ noteId: String, beats: Double) {
        guard let handle, let regionId = editingRegionId,
              nc_midi_note_resize(handle, regionId, noteId, max(0.01, beats)) else { return }
        recordGesture("Note length")
        reloadMidiRegions()
    }

    func setNotePitch(_ noteId: String, pitch: Int) {
        guard let handle, let regionId = editingRegionId,
              let note = notes(inRegion: regionId).first(where: { $0.id == noteId }) else { return }
        let clamped = max(0, min(127, pitch))
        guard nc_midi_note_move(handle, regionId, noteId, Int32(clamped), note.startBeats) else { return }
        recordGesture("Note pitch")
        reloadMidiRegions()
    }

    /// Sets the piano-roll note selection (the highlighted set). Cheap; drives redraw.
    func setNoteSelection(_ ids: Set<String>) {
        guard ids != selectedNoteIds else { return }
        selectedNoteIds = ids
    }

    /// Cubase Glue: join the selected notes of each pitch into one long note. Two Cs become one C;
    /// a selected chord glues each voice separately rather than collapsing into a single note.
    func glueSelectedNotes() {
        guard let handle, let regionId = editingRegionId, selectedNoteIds.count >= 2 else { return }
        let ids = Array(selectedNoteIds)
        let cStrings = ids.map { strdup($0) }
        defer { cStrings.forEach { free($0) } }
        var pointers = cStrings.map { UnsafePointer<CChar>($0) }
        let ok = pointers.withUnsafeMutableBufferPointer { buf in
            nc_midi_notes_merge(handle, regionId, buf.baseAddress, Int32(ids.count))
        }
        guard ok else {
            lastError = "붙일 노트가 없습니다 — 같은 음정의 노트를 두 개 이상 선택하세요."
            return
        }
        // The surviving note keeps its id, so the selection stays meaningful; the roll re-reads the
        // region's notes from the engine on the next publish.
        refreshHistory()
        objectWillChange.send()
    }

    /// Runs one of the Key Editor note functions over the selection, or the whole region when
    /// nothing is selected — the convention those functions follow.
    private func runNoteFunction(_ body: (OpaquePointer, String, UnsafePointer<UnsafePointer<CChar>?>?, Int32) -> Bool) {
        guard let handle, let regionId = editingRegionId else { return }
        let ids = Array(selectedNoteIds)
        if ids.isEmpty {
            guard body(handle, regionId, nil, 0) else { return }
        } else {
            let cStrings = ids.map { strdup($0) }
            defer { cStrings.forEach { free($0) } }
            var pointers: [UnsafePointer<CChar>?] = cStrings.map { $0.map { UnsafePointer($0) } }
            let ok = pointers.withUnsafeMutableBufferPointer { buf in
                body(handle, regionId, buf.baseAddress, Int32(ids.count))
            }
            guard ok else { return }
        }
        refreshHistory()
        objectWillChange.send()
    }

    /// Legato: stretch each note to meet the next one that starts later.
    func applyNoteLegato() {
        runNoteFunction { handle, regionId, ids, count in
            nc_midi_notes_legato(handle, regionId, ids, count, 0)
        }
    }

    /// Delete Overlaps: shorten a note that runs past the next note of the same pitch.
    func deleteNoteOverlaps() {
        runNoteFunction { handle, regionId, ids, count in
            nc_midi_notes_delete_overlaps(handle, regionId, ids, count)
        }
    }

    /// Fixed Lengths: set every target note to one length in beats.
    func setNoteLengths(beats: Double) {
        runNoteFunction { handle, regionId, ids, count in
            nc_midi_notes_set_length(handle, regionId, ids, count, beats)
        }
    }

    /// True when the selection could be glued: at least two selected notes share a pitch.
    var canGlueSelectedNotes: Bool {
        guard selectedNoteIds.count >= 2, let regionId = editingRegionId else { return false }
        let selected = notes(inRegion: regionId).filter { selectedNoteIds.contains($0.id) }
        return Dictionary(grouping: selected, by: \.pitch).values.contains { $0.count >= 2 }
    }

    /// Duplicates a note in place and returns the new note's id, so an ⌥-drag can grab the
    /// copy and leave the original put — the piano-roll twin of the timeline's ⌥-drag copy.
    /// Bypasses `addNote`'s stacking guard on purpose: a copy starts exactly over its source.
    @discardableResult
    func copyNote(_ noteId: String) -> String? {
        guard let handle, let regionId = editingRegionId,
              let source = notes(inRegion: regionId).first(where: { $0.id == noteId }) else { return nil }
        var buffer = [CChar](repeating: 0, count: 128)
        guard nc_midi_note_add(handle, regionId, Int32(source.pitch), source.startBeats,
                               source.durationBeats, Int32(source.velocity),
                               &buffer, buffer.count) else { return nil }
        reloadMidiRegions()
        refreshHistory()
        return String(cString: buffer)
    }

    /// Sets velocity on every selected note. Continuous while `commit` is false (a stepper or
    /// field drag streams these); pass `commit: true` on the final value for one undo step.
    func setSelectedNotesVelocity(_ velocity: Int, commit: Bool) {
        guard let handle, let regionId = editingRegionId, !selectedNoteIds.isEmpty else { return }
        let clamped = max(1, min(127, velocity))
        var changed = false
        for id in selectedNoteIds {
            if nc_midi_note_set_velocity(handle, regionId, id, Int32(clamped)) { changed = true }
        }
        guard changed else { return }
        if commit { recordGesture("Note velocity") }
        objectWillChange.send()
    }

    /// Deletes every selected note as one undo step, for the piano roll's Delete key.
    func deleteSelectedNotes() {
        guard let handle, let regionId = editingRegionId, !selectedNoteIds.isEmpty else { return }
        for id in selectedNoteIds { _ = nc_midi_note_delete(handle, regionId, id) }
        selectedNoteIds = []
        reloadMidiRegions()
        refreshHistory()
    }

    /// The velocity shown in the header field: the common value when the selection agrees,
    /// otherwise the rounded average (typing a value sets them all to it).
    var selectedNotesVelocity: Int? {
        guard let regionId = editingRegionId, !selectedNoteIds.isEmpty else { return nil }
        let vels = notes(inRegion: regionId).filter { selectedNoteIds.contains($0.id) }.map(\.velocity)
        guard !vels.isEmpty else { return nil }
        if let first = vels.first, vels.allSatisfy({ $0 == first }) { return first }
        return Int((Double(vels.reduce(0, +)) / Double(vels.count)).rounded())
    }

    // MARK: Controller lanes

    /// Which lane the strip under the piano roll edits. Velocity is per-note; the rest are the
    /// region's controller / pitch-bend events, which the renderer already sends to the instrument.
    enum EditorLane: Equatable, Hashable {
        case velocity
        case controller(Int)   // a CC number, 0-127
        case pitchBend

        var title: String {
            switch self {
            case .velocity: return "벨로시티"
            case .pitchBend: return "피치벤드"
            case .controller(let cc):
                switch cc {
                case 1: return "모듈레이션"
                case 2: return "브레스"
                case 7: return "볼륨"
                case 10: return "팬"
                case 11: return "익스프레션"
                case 64: return "서스테인"
                default: return "CC \(cc)"
                }
            }
        }

        /// The full-scale integer value: velocity/CC top out at 127, pitch bend at 16383.
        var maxValue: Int { if case .pitchBend = self { return 16383 } ; return 127 }
        /// The rest position drawn as a centre line, if any (pitch bend sits at 8192).
        var centerFraction: Double? { if case .pitchBend = self { return 0.5 } ; return nil }
    }

    /// The lanes offered in the picker, in menu order.
    static let editorLanes: [EditorLane] = [
        .velocity, .controller(1), .controller(2), .controller(7),
        .controller(10), .controller(11), .controller(64), .pitchBend,
    ]

    @Published var editorLane: EditorLane = .velocity

    /// One point on a controller lane. `value` is the raw MIDI value (0-127, or 0-16383 for bend).
    struct ControllerEvent: Equatable {
        let id: String
        let beat: Double
        let value: Int
    }

    func controllerEvents(inRegion regionId: String, controller: Int) -> [ControllerEvent] {
        guard let handle else { return [] }
        let count = Int(nc_midi_cc_count(handle, regionId, Int32(controller)))
        return (0..<count).compactMap { index in
            var id = [CChar](repeating: 0, count: 128)
            var beat = 0.0
            var value: Int32 = 0
            guard nc_midi_cc_get(handle, regionId, Int32(controller), Int32(index),
                                 &id, id.count, &beat, &value) else { return nil }
            return ControllerEvent(id: String(cString: id), beat: beat, value: Int(value))
        }
    }

    func pitchBendEvents(inRegion regionId: String) -> [ControllerEvent] {
        guard let handle else { return [] }
        let count = Int(nc_midi_pb_count(handle, regionId))
        return (0..<count).compactMap { index in
            var id = [CChar](repeating: 0, count: 128)
            var beat = 0.0
            var value: Int32 = 0
            guard nc_midi_pb_get(handle, regionId, Int32(index), &id, id.count, &beat, &value) else { return nil }
            return ControllerEvent(id: String(cString: id), beat: beat, value: Int(value))
        }
    }

    /// The current lane's points (empty for velocity, which the velocity lane draws from notes).
    func laneEvents(inRegion regionId: String) -> [ControllerEvent] {
        switch editorLane {
        case .velocity: return []
        case .controller(let cc): return controllerEvents(inRegion: regionId, controller: cc)
        case .pitchBend: return pitchBendEvents(inRegion: regionId)
        }
    }

    /// Whether a lane holds any data in the region open in the editor — so the lane picker can mark
    /// the controllers that were actually recorded/drawn. Recorded modulation and pitch bend land on
    /// their own lanes, invisible until picked; this is what tells the user where to look.
    func laneHasData(_ lane: EditorLane) -> Bool {
        guard let handle, let regionId = editingRegionId else { return false }
        switch lane {
        case .velocity:
            return false   // velocity is always "present" (it rides the notes); never marked
        case .controller(let cc):
            return nc_midi_cc_count(handle, regionId, Int32(cc)) > 0
        case .pitchBend:
            return nc_midi_pb_count(handle, regionId) > 0
        }
    }

    /// True when the region has recorded controller/pitch-bend data on a lane other than the one
    /// showing — the cue to open the picker.
    var hasHiddenLaneData: Bool {
        EngineController.editorLanes.contains { $0 != editorLane && laneHasData($0) }
    }

    /// Adds a point to the current lane and returns its id, so a place-and-drag gesture can grab it.
    @discardableResult
    func addLaneEvent(beat: Double, value: Int) -> String? {
        guard let handle, let regionId = editingRegionId else { return nil }
        var id = [CChar](repeating: 0, count: 128)
        let ok: Bool
        switch editorLane {
        case .velocity:
            return nil
        case .controller(let cc):
            ok = nc_midi_cc_add(handle, regionId, Int32(cc), beat, Int32(value), &id, id.count)
        case .pitchBend:
            ok = nc_midi_pb_add(handle, regionId, beat, Int32(value), &id, id.count)
        }
        guard ok else { return nil }
        reloadMidiRegions()
        refreshHistory()
        return String(cString: id)
    }

    /// Continuous: a lane drag streams these; commit the gesture at the end for one undo step.
    func moveLaneEvent(_ eventId: String, beat: Double, value: Int) {
        guard let handle, let regionId = editingRegionId else { return }
        switch editorLane {
        case .velocity: return
        case .controller: _ = nc_midi_cc_move(handle, regionId, eventId, beat, Int32(value))
        case .pitchBend: _ = nc_midi_pb_move(handle, regionId, eventId, beat, Int32(value))
        }
        objectWillChange.send()
    }

    func deleteLaneEvent(_ eventId: String) {
        guard let handle, let regionId = editingRegionId else { return }
        switch editorLane {
        case .velocity: return
        case .controller: _ = nc_midi_cc_delete(handle, regionId, eventId)
        case .pitchBend: _ = nc_midi_pb_delete(handle, regionId, eventId)
        }
        reloadMidiRegions()
        refreshHistory()
    }

    // MARK: MIDI region tools

    /// Beats. A sixteenth is 0.25.
    func quantizeRegion(_ regionId: String, beatQuantum: Double) {
        guard let handle, nc_midi_region_quantize(handle, regionId, beatQuantum) > 0 else { return }
        reloadMidiRegions()
        refreshHistory()
    }

    /// Snaps only the selected notes' starts to the grid (Logic/Cubase quantize the selection
    /// when there is one, the whole region otherwise). One undo step.
    func quantizeSelectedNotes(beatQuantum: Double) {
        guard let handle, let regionId = editingRegionId,
              !selectedNoteIds.isEmpty, beatQuantum > 0 else { return }
        let target = notes(inRegion: regionId).filter { selectedNoteIds.contains($0.id) }
        var changed = false
        for note in target {
            let snapped = (note.startBeats / beatQuantum).rounded() * beatQuantum
            if abs(snapped - note.startBeats) > 1e-6,
               nc_midi_note_move(handle, regionId, note.id, Int32(note.pitch), snapped) {
                changed = true
            }
        }
        guard changed else { return }
        recordGesture("Quantize notes")
        reloadMidiRegions()
    }

    /// Quantizes the selection when notes are picked, else the whole region — the grid value
    /// comes from the header's Logic/Cubase-style menu.
    func quantize(regionId: String, beatQuantum: Double) {
        if selectedNoteIds.isEmpty {
            quantizeRegion(regionId, beatQuantum: beatQuantum)
        } else {
            quantizeSelectedNotes(beatQuantum: beatQuantum)
        }
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

    /// Merges the given MIDI regions into one part (Cubase Glue). Ids must be on one track.
    @discardableResult
    private func mergeRegions(_ ids: [String]) -> String? {
        guard let handle, ids.count >= 2 else { return nil }
        // Bridge to `const char* const*`: hold each C string, pass an array of their pointers.
        let cStrings = ids.map { strdup($0) }
        defer { cStrings.forEach { free($0) } }
        var pointers = cStrings.map { UnsafePointer<CChar>($0) }
        var out = [CChar](repeating: 0, count: 128)
        let ok = pointers.withUnsafeMutableBufferPointer { buf in
            nc_midi_regions_merge(handle, buf.baseAddress, Int32(ids.count), &out, out.count)
        }
        guard ok else { return nil }
        reloadMidiRegions()
        refreshHistory()
        let merged = String(cString: out)
        selectRegion(merged)
        return merged
    }

    /// Cubase Glue: merge a region with the next part on its track.
    func mergeRegionForward(_ regionId: String) {
        guard let region = midiRegions.first(where: { $0.id == regionId }),
              let next = midiRegions
                .filter({ $0.trackName == region.trackName && $0.startSeconds > region.startSeconds })
                .min(by: { $0.startSeconds < $1.startSeconds }) else { return }
        mergeRegions([region.id, next.id])
    }

    /// Collapse every MIDI part on a region's track into one.
    func mergeRegionsOnTrack(_ regionId: String) {
        guard let region = midiRegions.first(where: { $0.id == regionId }) else { return }
        let ids = midiRegions
            .filter { $0.trackName == region.trackName }
            .sorted { $0.startSeconds < $1.startSeconds }
            .map(\.id)
        mergeRegions(ids)
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
        songForm = (0..<Int(nc_song_section_count(handle))).map { i in
            ConductorEvent(id: i, timeSeconds: nc_song_section_time(handle, Int32(i)),
                           label: readEngineString { nc_song_section_name(handle, Int32(i), $0, $1) })
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

    /// Set the base tempo from the transport TEMPO field. Syncs the t=0 conductor anchor.
    func setBaseTempo(_ bpm: Int) {
        guard let handle else { return }
        if nc_project_set_tempo_bpm(handle, Int32(max(1, min(999, bpm)))) {
            tempoBpm = Int(nc_project_tempo_bpm(handle))
            reloadConductor(); refreshHistory()
        }
    }

    /// Set the base time signature from the transport SIG field. `text` is "num/den" (a bare
    /// number is taken as the numerator over the current denominator). Syncs the t=0 anchor.
    func setBaseTimeSignature(_ text: String) {
        guard let handle else { return }
        let parts = text.split(separator: "/")
        let num = Int(parts.first.map(String.init) ?? "") ?? timeSignature.numerator
        let den = parts.count > 1 ? (Int(parts[1]) ?? timeSignature.denominator) : timeSignature.denominator
        if nc_project_set_time_signature(handle, Int32(max(1, num)), Int32(max(1, den))) {
            timeSignature = (Int(nc_project_time_signature_numerator(handle)),
                             Int(nc_project_time_signature_denominator(handle)))
            reloadConductor(); refreshHistory()
        }
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

    // Song-form / arrangement sections (Intro, Verse, Chorus…). Per-PROJECT now (stored in the
    // .ndaw via nc_song_section_*), not app-global — so sections no longer carry across projects.
    // Each event starts a section that runs until the next one; loaded in reloadConductor().
    @Published private(set) var songForm: [ConductorEvent] = []
    func addSongSection(at seconds: Double, name: String) {
        guard let handle else { return }
        let label = name.trimmingCharacters(in: .whitespaces)
        if (label.isEmpty ? "Section" : label).withCString({ nc_song_section_add(handle, seconds, $0) }) {
            reloadConductor(); refreshHistory()
        }
    }
    func deleteSongSection(at seconds: Double) {
        guard let handle else { return }
        if nc_song_section_delete(handle, seconds, markerTolerance) { reloadConductor(); refreshHistory() }
    }
    func moveSongSection(from: Double, to: Double) {
        guard let handle else { return }
        if nc_song_section_move(handle, from, markerTolerance, max(0, snap(to))) { reloadConductor(); refreshHistory() }
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
        static let volumeTrim = AutomationParameter(id: "track.volume.trim", displayName: "볼륨 트림 (dB)", range: -24...24, pluginFallback: 0)
        static let pan = AutomationParameter(id: "track.pan", displayName: "팬", range: -1...1)
        static let mute = AutomationParameter(id: "track.mute", displayName: "뮤트", range: 0...1)
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
        guard laneIndex < laneTracks.count else { return [.volume, .pan, .mute] }
        let track = laneTracks[laneIndex]
        var options: [AutomationParameter] = [.volume, .volumeTrim, .pan, .mute]
        for (slot, send) in track.sends.enumerated() {
            let prefix = "센드 \(slot + 1) · \(send.bus)"
            options.append(AutomationParameter(id: "send.\(slot).level",
                                               displayName: "\(prefix) · 레벨", range: -60...12,
                                               pluginFallback: send.gainDb))
            options.append(AutomationParameter(id: "send.\(slot).pan",
                                               displayName: "\(prefix) · 팬", range: -1...1,
                                               pluginFallback: send.pan))
            options.append(AutomationParameter(id: "send.\(slot).mute",
                                               displayName: "\(prefix) · 뮤트", range: 0...1,
                                               pluginFallback: 0))
            options.append(AutomationParameter(id: "send.\(slot).pre_fader",
                                               displayName: "\(prefix) · Pre/Post", range: 0...1,
                                               pluginFallback: send.preFader ? 1 : 0))
        }
        options += insertAutomationParameters(trackId: track.id)
        options += instrumentAutomationParameters(trackId: track.id)
        return options
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

    func instrumentAutomationParameters(trackId: Int) -> [AutomationParameter] {
        guard let handle else { return [] }
        var out: [AutomationParameter] = []
        for slot in 0..<8 {
            let count = Int(nc_track_instrument_slot_param_count(handle, Int32(trackId), Int32(slot)))
            guard count > 0 else { continue }
            for p in 0..<count {
                let pid = nc_track_instrument_slot_param_id(handle, Int32(trackId), Int32(slot), Int32(p))
                let value = Float(nc_track_instrument_slot_param_value(handle, Int32(trackId), Int32(slot), Int32(p)))
                out.append(AutomationParameter(id: "instrument.\(slot).\(pid)",
                                               displayName: "악기 \(slot + 1) · Param \(pid)",
                                               range: 0...1, pluginFallback: value))
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

    /// Pro Tools range edit on the conductor lanes (송폼~가사): remove every marker / chord / lyric /
    /// song section / tempo / meter / key event inside the edit range, in one undo step.
    func clearConductorInRange() {
        guard let handle, hasEditRange else { return }
        let removed = Int(nc_conductor_clear_range(handle, loopStartSeconds, loopEndSeconds))
        // Key events live app-side (the engine has no key field) — clear them in the range too.
        let beforeKeys = keyEventStore.count
        keyEventStore.removeAll { $0.seconds >= loopStartSeconds - 1e-6 && $0.seconds <= loopEndSeconds + 1e-6 }
        let keysRemoved = beforeKeys - keyEventStore.count
        if keysRemoved > 0 { persistKeyEvents() }
        if removed > 0 {
            reloadMarkers()
            reloadConductor()
            reloadTracks()      // tempo/meter changes reshape the ruler
            refreshHistory()
        }
    }
    /// Whether the range holds any conductor event, so the menu item can enable/disable.
    var rangeHasConductor: Bool {
        guard hasEditRange else { return false }
        let lo = loopStartSeconds, hi = loopEndSeconds
        func any(_ t: Double) -> Bool { t >= lo - 1e-6 && t <= hi + 1e-6 }
        return markers.contains { any($0.timeSeconds) }
            || chords.contains { any($0.timeSeconds) }
            || lyrics.contains { any($0.timeSeconds) }
            || tempoMarkers.contains { any($0.timeSeconds) }
            || meterEvents.contains { any($0.timeSeconds) }
            || keyEvents.contains { any($0.timeSeconds) }
            || songForm.contains { any($0.timeSeconds) }
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
                fadeInCurvature: nc_clip_fade_in_curvature(handle, i),
                fadeOutCurvature: nc_clip_fade_out_curvature(handle, i),
                gainDb: nc_clip_gain_db(handle, i),
                muted: nc_clip_muted(handle, i),
                reversed: nc_clip_reversed(handle, i),
                polarityInverted: nc_clip_polarity(handle, i)
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

    /// Tracks a per-track toggle should hit: the whole multi-selection when the clicked track is part
    /// of it (Pro Tools — mute/solo/arm one selected track, they all follow), else just this track.
    private func trackToggleGroup(_ id: Int) -> [Int] {
        if selectedMixerTrackIds.count > 1 && selectedMixerTrackIds.contains(id) {
            return tracks.filter { selectedMixerTrackIds.contains($0.id) }.map { $0.id }
        }
        return [id]
    }
    /// Apply one flag (0=mute 1=solo 2=armed 3=inputMon) to a group of tracks in one undo step.
    private func applyTrackFlag(_ ids: [Int], flag: Int32, value: Bool) {
        guard let handle, !ids.isEmpty else { return }
        let arr = ids.map { Int32($0) }
        _ = arr.withUnsafeBufferPointer { nc_track_set_flag_many(handle, $0.baseAddress, Int32($0.count), flag, value) }
        reloadTracks()
        refreshHistory()
    }

    /// Is the record-armed audio track's input the reference tap? (decides whether a punch monitors it)
    private var recordingTapSource: Bool {
        tracks.first(where: { $0.recordArmed && $0.kind == .audio })?.inputBus == EngineController.referenceTapInputBus
    }

    func toggleTrackMute(_ id: Int) {
        guard let track = tracks.first(where: { $0.id == id }) else { return }
        applyTrackFlag(trackToggleGroup(id), flag: 0, value: !track.muted)
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

    /// Set a track's channel format to stereo or mono. Reconciles the graph (a mono track
    /// is summed to one channel), so it records one undo step.
    func setTrackStereo(_ id: Int, _ stereo: Bool) {
        guard let handle else { return }
        _ = (stereo ? "stereo" : "mono").withCString { nc_track_set_channel_format(handle, Int32(id), $0) }
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
    /// The sentinel input that records the "other apps" reference tap instead of a hardware pair.
    /// Must match `kReferenceTapInputBus` in the bridge.
    static let referenceTapInputBus = "다른 앱"

    func audioInputOptions() -> [String] {
        ["Input 1-2", "Input 3-4", "Input 5-6", "Input 7-8", EngineController.referenceTapInputBus]
    }

    func setTrackInputBus(_ id: Int, _ bus: String) {
        guard let handle else { return }
        _ = bus.withCString { nc_track_set_input_bus(handle, Int32(id), $0) }
        reloadTracks()
        refreshHistory()
        reconcileTapInputHold()
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
        // Multi-selection: solo the whole group additively (exclusive is a single-click notion), one step.
        let group = trackToggleGroup(id)
        if group.count > 1 {
            applyTrackFlag(group, flag: 1, value: willSolo)
            return
        }
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

    func setSoloSelectMode(_ mode: SoloSelectMode) {
        soloSelectMode = mode
        UserDefaults.standard.set(mode.rawValue, forKey: SettingsKey.soloSelect)
    }

    func setSoloMonitorMode(_ mode: SoloMonitorMode) { soloMonitorMode = mode }

    func toggleTrackArm(_ id: Int) {
        guard let track = tracks.first(where: { $0.id == id }) else { return }
        applyTrackFlag(trackToggleGroup(id), flag: 2, value: !track.recordArmed)
        // Arming is the signal that the user is about to play or record, which is when the
        // linear-phase monitor EQ's delay stops being acceptable. Rebuild through whichever
        // path that now selects.
        syncMonitorEqToContext()
    }

    func toggleTrackInputMonitoring(_ id: Int) {
        guard let track = tracks.first(where: { $0.id == id }) else { return }
        applyTrackFlag(trackToggleGroup(id), flag: 3, value: !track.inputMonitoring)
        reconcileTapInputHold()
        syncMonitorEqToContext()
    }

    /// Run + hear the tap continuously whenever any tap-input track's Input-Monitor toggle is on
    /// (Input mode), independent of the auto-input punch. Call after an input-monitor or input-bus change.
    func reconcileTapInputHold() {
        guard let handle else { return }
        let anyTapInputMon = tracks.contains {
            $0.inputMonitoring && $0.kind == .audio && $0.inputBus == EngineController.referenceTapInputBus
        }
        // Arming the tap creates a CoreAudio aggregate device (~1 s); defer to the next runloop so
        // the Input-Monitor button flips instantly instead of freezing under the device open.
        DispatchQueue.main.async { nc_monitor_set_tap_input_hold(handle, anyTapInputMon) }
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
        // Console switches are also mutable per-track fields. Without re-reading
        // them, a toggle reached the DSP but the button stayed visually stale and
        // every subsequent click sent the same value again (notably Dual Mono).
        tracks[position].consoleFilterEnabled =
            "filterEnabled".withCString { nc_track_console_bool(handle, i, $0) }
        tracks[position].consoleFilterCircuitMode =
            "filterCircuitMode".withCString { nc_track_console_bool(handle, i, $0) }
        tracks[position].consoleHighPassEnabled =
            "highPassEnabled".withCString { nc_track_console_bool(handle, i, $0) }
        tracks[position].consoleLowPassEnabled =
            "lowPassEnabled".withCString { nc_track_console_bool(handle, i, $0) }
        tracks[position].consoleEqEnabled =
            "eqEnabled".withCString { nc_track_console_bool(handle, i, $0) }
        tracks[position].consoleEqCircuitMode =
            "eqCircuitMode".withCString { nc_track_console_bool(handle, i, $0) }
        tracks[position].consoleEqHfBell =
            "eqHfBell".withCString { nc_track_console_bool(handle, i, $0) }
        tracks[position].consoleEqLfBell =
            "eqLfBell".withCString { nc_track_console_bool(handle, i, $0) }
        tracks[position].consoleCompEnabled =
            "compEnabled".withCString { nc_track_console_bool(handle, i, $0) }
        tracks[position].consoleCompCircuitMode =
            "compCircuitMode".withCString { nc_track_console_bool(handle, i, $0) }
        tracks[position].consoleCompFastAttack =
            "compFastAttack".withCString { nc_track_console_bool(handle, i, $0) }
        tracks[position].consoleGateEnabled =
            "gateEnabled".withCString { nc_track_console_bool(handle, i, $0) }
        tracks[position].consoleGateCircuitMode =
            "gateCircuitMode".withCString { nc_track_console_bool(handle, i, $0) }
        tracks[position].consoleGateFastAttack =
            "gateFastAttack".withCString { nc_track_console_bool(handle, i, $0) }
        tracks[position].consoleExpanderMode =
            "expanderMode".withCString { nc_track_console_bool(handle, i, $0) }
        tracks[position].consoleSaturatorEnabled =
            "saturatorEnabled".withCString { nc_track_console_bool(handle, i, $0) }
        tracks[position].consoleSaturatorCircuitMode =
            "saturatorCircuitMode".withCString { nc_track_console_bool(handle, i, $0) }
        tracks[position].consoleDualMono =
            "dualMono".withCString { nc_track_console_bool(handle, i, $0) }
    }

    /// Meters arrive keyed by track name, so match on name, not position.
    private func applyTrackMeters(_ status: NCEngineStatus) {
        guard !tracks.isEmpty else { return }

        var peaks: [String: (Float, Float, Float, Float)] = [:]
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
                    let gainReduction = withUnsafePointer(to: status.trackConsoleGainReductionDb) {
                        $0.withMemoryRebound(to: Float.self, capacity: Int(NC_MAX_TRACK_METERS)) { $0[index] }
                    }
                    let gateGainReduction = withUnsafePointer(to: status.trackConsoleGateGainReductionDb) {
                        $0.withMemoryRebound(to: Float.self, capacity: Int(NC_MAX_TRACK_METERS)) { $0[index] }
                    }
                    peaks[name] = (left, right, gainReduction, gateGainReduction)
                }
            }
        }

        for index in tracks.indices {
            let (left, right, gainReduction, gateGainReduction) = peaks[tracks[index].name] ?? (0, 0, 0, 0)
            // Ballistic, so a track meter falls to silence on stop instead of freezing.
            // Assign only on change: an idle strip must not republish tracks every tick, or
            // every menu observing the controller flickers.
            let nl = Self.decayedMeter(left, tracks[index].peakLeft)
            let nr = Self.decayedMeter(right, tracks[index].peakRight)
            if nl != tracks[index].peakLeft { tracks[index].peakLeft = nl }
            if nr != tracks[index].peakRight { tracks[index].peakRight = nr }
            let ngr = Self.decayedMeter(gainReduction, tracks[index].consoleCompGainReductionDb)
            if ngr != tracks[index].consoleCompGainReductionDb {
                tracks[index].consoleCompGainReductionDb = ngr
            }
            let nggr = Self.decayedMeter(gateGainReduction, tracks[index].consoleGateGainReductionDb)
            if nggr != tracks[index].consoleGateGainReductionDb {
                tracks[index].consoleGateGainReductionDb = nggr
            }
        }
    }

    /// Per-tick meter release. At ~30 Hz this falls a held peak to silence in ~0.2 s.
    private static let meterDecay: Float = 0.80
    /// Ballistic meter step that SNAPS to exact silence once it drops below ~-68 dBFS.
    /// A pure multiplicative decay only asymptotes toward 0, so `setIfChanged` saw a change
    /// every tick forever and republished the meter — re-rendering the whole engine-observing
    /// view tree at 30 Hz (idle ≈ 40% CPU). Flooring lets an idle meter settle to no-publish.
    private static func decayedMeter(_ target: Float, _ previous: Float) -> Float {
        let v = max(target, previous * meterDecay)
        return v < 0.0016 ? 0 : v   // ~-56 dBFS: settle to exact silence so idle stops republishing
    }

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
        // Compute into scratch and publish the whole array once, only if something moved
        // audibly — an idle spectrum asymptotically decaying toward zero must not republish
        // every tick (which flickers open menus). Fast attack, slow release.
        var changed = false
        for i in 0..<count {
            let incoming = spectrumScratch[i]
            let cur = spectrumBins[i]
            let next = incoming > cur ? incoming : cur * 0.72 + incoming * 0.28
            spectrumScratch[i] = next
            // ~-52 dBFS: below this the bins are jitter/noise-floor, and republishing them
            // re-rendered the whole engine-observing tree (flickering open menus, high CPU).
            if abs(next - cur) > 0.0025 { changed = true }
        }
        if changed { spectrumBins = Array(spectrumScratch[0..<count]) }
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
        // Publish only on real change: a silent goniometer (all near zero) must not
        // republish every tick and flicker open menus.
        var changed = goniometerSamples.count != count
        if !changed {
            for i in 0..<count where abs(goniometerScratch[i] - goniometerSamples[i]) > 0.0025 {
                changed = true; break
            }
        }
        if changed { goniometerSamples = Array(goniometerScratch[0..<count]) }
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
            let model = readString { nc_monitor_speaker_model(handle, s, $0, $1) }
            let bare = stripSlotPrefix(model)
            return SpeakerSet(
                id: slot,
                letter: ["A", "B", "C"][slot],
                name: names[slot],
                model: model,
                output: readString { nc_monitor_speaker_output(handle, s, $0, $1) },
                simWeight: nc_monitor_speaker_sim_weight(handle, s),
                roomEq: nc_monitor_speaker_room_eq(handle, s),
                amp: readString { nc_monitor_speaker_amp(handle, s, $0, $1) },
                cable: readString { nc_monitor_speaker_cable(handle, s, $0, $1) },
                modelIsPassive: !bare.isEmpty && bare.withCString { nc_speaker_model_is_passive($0) }
            )
        }

        activeSpeakerSlot = Int(nc_monitor_active_speaker_slot(handle))
        monitorVolumeDb = nc_monitor_volume_db(handle)
        monitorDim = nc_monitor_dim(handle)
        monitorDimDb = nc_monitor_dim_db(handle)
        monitorMono = nc_monitor_mono(handle)
        monitorMute = nc_monitor_mute(handle)
        monitorTalkback = nc_monitor_talkback(handle)
        talkbackRoute = readString { nc_monitor_talkback_route(handle, $0, $1) }
        talkbackChannel = Int(nc_monitor_talkback_channel(handle))
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
        externalDspEnabled = nc_dsp_external_enabled(handle) != 0
        remoteDspHost = readString { nc_dsp_remote_host(handle, $0, $1) }
        physicalSpeakerModel = readString { nc_monitor_physical_speaker_model(handle, $0, $1) }
        physicalHeadphoneModel = readString { nc_monitor_physical_headphone_model(handle, $0, $1) }
        physicalPowerAmpModel = readString { nc_monitor_physical_power_amp_model(handle, $0, $1) }
        physicalSpeakerCableModel = readString { nc_monitor_physical_speaker_cable_model(handle, $0, $1) }
        physicalPowerCableModel = readString { nc_monitor_physical_power_cable_model(handle, $0, $1) }
        physicalConnectorModel = readString { nc_monitor_physical_connector_model(handle, $0, $1) }
        physicalAudioInterfaceModel = readString { nc_monitor_physical_audio_interface_model(handle, $0, $1) }
        physicalAudioInterfaceTargetModel = readString { nc_monitor_physical_audio_interface_target(handle, $0, $1) }
        audioInterfaceTransformActive = nc_audio_interface_transform_active(handle)
        monitorInterfaceModelingEnabled = nc_monitor_interface_modeling_enabled(handle)
        monitorSwapLeftRight = nc_monitor_swap_left_right(handle)
        monitorOutputExclusive = nc_monitor_output_exclusive(handle)
        measurementMicModel = readString { nc_measurement_mic_model(handle, $0, $1) }
        autoFadeOutSeconds = nc_master_auto_fade_seconds(handle)
        autoFadeOutCurve = readString { nc_master_auto_fade_curve(handle, $0, $1) }
        reloadMonitorListen()
        // The single monitor EQ is derived state — re-derive it from the now-current context
        // (active A/B/C slot, its model or physical route, output mode, room measurement) so a
        // slot switch, undo/redo, or project load all leave the EQ matching what is monitored.
        // Runs only here (load / history / speaker setters), never on the poll tick.
        monitorEqLinearPhase = nc_monitor_eq_linear_phase(handle)
        monitorEqHeadphoneOeTarget = nc_monitor_eq_headphone_oe_target(handle)
        syncMonitorEqToContext()
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

    /// Broadcast-probe the LAN for a node and adopt its address if one answers, then read its specs.
    func discoverRemoteDspHost() {
        guard let handle else { return }
        let found = readString { nc_dsp_discover_remote_host(handle, $0, $1) }
        if !found.isEmpty { setRemoteDspHost(found) }
        refreshRemoteNodeSpecs()
    }

    /// Query the current remote host for its identity + hardware specs (blocks briefly on the network,
    /// on the main thread like discover, since engine calls are main-thread-only). Clears on no answer.
    func refreshRemoteNodeSpecs() {
        guard let handle else { return }
        var info = NCRemoteNodeInfo()
        if nc_dsp_remote_node_info(handle, &info) != 0 {
            remoteNodeSpecs = RemoteNodeSpecs(
                model: Self.cStringField(&info.model),
                cpuModel: Self.cStringField(&info.cpuModel),
                cpuMhz: info.cpuMhz,
                memoryMb: Int(info.memoryMb),
                coreCount: Int(info.coreCount),
                roundTripMs: info.roundTripMs)
        } else {
            remoteNodeSpecs = nil
        }
    }

    /// The "use this node" master switch. Applies live through the monitor path.
    func setExternalDspEnabled(_ enabled: Bool) {
        guard let handle else { return }
        nc_dsp_set_external_enabled(handle, enabled ? 1 : 0)
        externalDspEnabled = nc_dsp_external_enabled(handle) != 0
        refreshHistory()
    }

    /// Read a fixed-size C `char[]` field (imported into Swift as a tuple) as a String.
    private static func cStringField<T>(_ tuple: inout T) -> String {
        withUnsafePointer(to: &tuple) { ptr in
            ptr.withMemoryRebound(to: CChar.self, capacity: MemoryLayout<T>.size) {
                String(cString: $0)
            }
        }
    }

    // MARK: - Built-in test signal generator (track source)

    struct TestSignalState: Equatable {
        var present = false
        var enabled = true
        var waveform = 0          // 0 sine, 1 square, 2 triangle, 3 saw, 4 white, 5 pink, 6 sweep
        var frequencyHz = 1000.0
        var levelDb = -6.0
        var channel = 1           // 0 L, 1 stereo, 2 R
        var polarity = false
        static let waveformNames = ["사인", "사각", "삼각", "톱니", "화이트", "핑크", "스윕"]
        static let channelNames = ["L", "L+R", "R"]
    }

    func testSignalState(track: Int) -> TestSignalState {
        guard let handle else { return TestSignalState() }
        var s = TestSignalState()
        s.present = nc_track_test_signal_generator_slot(handle, Int32(track)) >= 0
        guard s.present else { return s }
        s.enabled = nc_track_test_signal_enabled(handle, Int32(track))
        s.waveform = Int(nc_track_test_signal_waveform(handle, Int32(track)))
        s.frequencyHz = nc_track_test_signal_frequency_hz(handle, Int32(track))
        s.levelDb = nc_track_test_signal_level_db(handle, Int32(track))
        s.channel = Int(nc_track_test_signal_channel(handle, Int32(track)))
        s.polarity = nc_track_test_signal_polarity(handle, Int32(track))
        return s
    }

    func addTestSignalGenerator(track: Int) {
        guard let handle else { return }
        nc_track_add_test_signal_generator(handle, Int32(track))
        reloadTracks(); refreshHistory(); objectWillChange.send()
    }
    func removeTestSignalGenerator(track: Int) {
        guard let handle else { return }
        nc_track_remove_test_signal_generator(handle, Int32(track))
        reloadTracks(); refreshHistory(); objectWillChange.send()
    }
    func setTestSignalEnabled(track: Int, _ on: Bool) {
        guard let handle else { return }
        nc_track_test_signal_set_enabled(handle, Int32(track), on); refreshHistory(); objectWillChange.send()
    }
    func setTestSignalWaveform(track: Int, _ waveform: Int) {
        guard let handle else { return }
        nc_track_test_signal_set_waveform(handle, Int32(track), Int32(waveform)); refreshHistory(); objectWillChange.send()
    }
    func setTestSignalChannel(track: Int, _ channel: Int) {
        guard let handle else { return }
        nc_track_test_signal_set_channel(handle, Int32(track), Int32(channel)); refreshHistory(); objectWillChange.send()
    }
    func setTestSignalPolarity(track: Int, _ inverted: Bool) {
        guard let handle else { return }
        nc_track_test_signal_set_polarity(handle, Int32(track), inverted); refreshHistory(); objectWillChange.send()
    }
    /// Continuous (slider) — records no undo step; call refreshHistory once at gesture end if needed.
    func setTestSignalFrequency(track: Int, _ hz: Double) {
        guard let handle else { return }
        nc_track_test_signal_set_frequency_hz(handle, Int32(track), hz); objectWillChange.send()
    }
    func setTestSignalLevel(track: Int, _ db: Double) {
        guard let handle else { return }
        nc_track_test_signal_set_level_db(handle, Int32(track), db); objectWillChange.send()
    }

    /// Isolation keeps a floor of 4 cores, matching the engine.
    // No 4-core floor while isolation is on — the user can reserve as few as 1 (bridge clamps 1–16).
    var minDspCoreCount: Int { 1 }

    func cycleStereo() { guard let handle else { return }; nc_monitor_cycle_stereo(handle); reloadMonitorListen() }
    func cycleMono() { guard let handle else { return }; nc_monitor_cycle_mono(handle); reloadMonitorListen() }
    func toggleMidSide() { guard let handle else { return }; nc_monitor_toggle_mid_side(handle); reloadMonitorListen() }
    func cyclePhase() { guard let handle else { return }; nc_monitor_cycle_phase(handle); reloadMonitorListen() }

    func toggleDim() {
        guard let handle else { return }
        nc_monitor_set_dim(handle, !monitorDim)
        monitorDim = nc_monitor_dim(handle)
    }

    /// How far Dim attenuates the monitor, in dB (negative). Right-click the Dim button to pick.
    @Published private(set) var monitorDimDb: Float = -20
    static let monitorDimDbOptions: [Float] = [-10, -15, -20, -25, -30, -40]
    func setMonitorDimDb(_ db: Float) {
        guard let handle else { return }
        nc_monitor_set_dim_db(handle, db)
        monitorDimDb = nc_monitor_dim_db(handle)
    }

    // MARK: Monitor station keyboard shortcuts (runtime-only, remappable)

    /// The monitor actions the number-key row can drive. Runtime-only — the key monitor lives on
    /// this controller and dies with the app, so the shortcuts vanish on quit (as requested).
    enum MonitorShortcutAction: String, CaseIterable, Identifiable {
        case volDown, volUp, dim, mute, talk, setA, setB, setC
        case stereo, mono, midSide, sourceMaster, sourceBlackHole, player
        var id: String { rawValue }
        var label: String {
            switch self {
            case .volDown: return "볼륨 −2 dB"
            case .volUp: return "볼륨 +2 dB"
            case .dim: return "Dim"
            case .mute: return "Mute"
            case .talk: return "Talk"
            case .setA: return "스피커 A"
            case .setB: return "스피커 B"
            case .setC: return "스피커 C"
            case .stereo: return "Stereo"
            case .mono: return "Mono"
            case .midSide: return "M/S"
            case .sourceMaster: return "입력: Master"
            case .sourceBlackHole: return "입력: BlackHole"
            case .player: return "입력: Player (미구현)"
            }
        }
        var defaultKeyCode: UInt16 {
            switch self {
            case .volDown: return 27        // -
            case .volUp: return 24          // = (the + key)
            case .dim: return 29            // 0
            case .mute: return 47           // .
            case .talk: return 36           // Return
            case .setA: return 18           // 1
            case .setB: return 19           // 2
            case .setC: return 20           // 3
            case .stereo: return 21         // 4
            case .mono: return 23           // 5
            case .midSide: return 22        // 6
            case .sourceMaster: return 26   // 7
            case .sourceBlackHole: return 28 // 8
            case .player: return 25         // 9
            }
        }
    }
    /// The number-row keys the user may bind, keyCode → display label.
    static let monitorShortcutAssignableKeys: [(code: UInt16, label: String)] = [
        (18, "1"), (19, "2"), (20, "3"), (21, "4"), (23, "5"), (22, "6"),
        (26, "7"), (28, "8"), (25, "9"), (29, "0"), (27, "−"), (24, "+"), (47, "."), (36, "↵"),
    ]
    static func monitorShortcutKeyLabel(_ code: UInt16) -> String {
        monitorShortcutAssignableKeys.first { $0.code == code }?.label ?? "?"
    }

    /// Master switch: while on, the number-row keys drive the monitor station instead of their
    /// normal function. Off by default; toggled from the dock button.
    @Published var monitorShortcutsEnabled = false
    /// action.rawValue → keyCode. A missing action is unbound.
    @Published var monitorShortcutKeys: [String: UInt16] = {
        var m: [String: UInt16] = [:]
        for a in MonitorShortcutAction.allCases { m[a.rawValue] = a.defaultKeyCode }
        return m
    }()

    func monitorShortcutKey(_ action: MonitorShortcutAction) -> UInt16? { monitorShortcutKeys[action.rawValue] }
    /// Assign (or clear with nil) a key to an action; a key is unique, so it is removed from any
    /// other action first.
    func setMonitorShortcutKey(_ action: MonitorShortcutAction, _ keyCode: UInt16?) {
        if let kc = keyCode {
            for a in MonitorShortcutAction.allCases where monitorShortcutKeys[a.rawValue] == kc {
                monitorShortcutKeys[a.rawValue] = nil
            }
            monitorShortcutKeys[action.rawValue] = kc
        } else {
            monitorShortcutKeys[action.rawValue] = nil
        }
        persistMonitorShortcuts()
    }
    func setMonitorShortcutsEnabled(_ on: Bool) {
        monitorShortcutsEnabled = on
        persistMonitorShortcuts()
    }
    func resetMonitorShortcutsToDefault() {
        for a in MonitorShortcutAction.allCases { monitorShortcutKeys[a.rawValue] = a.defaultKeyCode }
        persistMonitorShortcuts()
    }
    private func monitorShortcutAction(forKeyCode code: UInt16) -> MonitorShortcutAction? {
        // Keypad-ONLY: the ten-key drives the monitor station (Pro Tools style); the top number ROW
        // is deliberately left alone for its normal use. Fold a keypad code onto the logical key the
        // bindings are stored as; a non-keypad key returns nil so the row is never intercepted.
        guard let logical = Self.keypadToLogicalKeyCode(code) else { return nil }
        return MonitorShortcutAction.allCases.first { monitorShortcutKeys[$0.rawValue] == logical }
    }
    /// A numeric-keypad keycode → the logical (number-row) keycode the bindings use, or nil when
    /// `code` isn't a keypad key (so only the ten-key fires the monitor shortcuts).
    static func keypadToLogicalKeyCode(_ code: UInt16) -> UInt16? {
        switch code {
        case 82: return 29   // kp0 → 0
        case 83: return 18   // kp1 → 1
        case 84: return 19   // kp2 → 2
        case 85: return 20   // kp3 → 3
        case 86: return 21   // kp4 → 4
        case 87: return 23   // kp5 → 5
        case 88: return 22   // kp6 → 6
        case 89: return 26   // kp7 → 7
        case 91: return 28   // kp8 → 8
        case 92: return 25   // kp9 → 9
        case 65: return 47   // kp. → .
        case 69: return 24   // kp+ → +
        case 78: return 27   // kp− → −
        case 76: return 36   // kpEnter → Return
        default: return nil  // not a keypad key → leave the top row alone
        }
    }
    /// Run a monitor action. Called from the key monitor when the shortcut layer is on.
    func performMonitorShortcut(_ action: MonitorShortcutAction) {
        switch action {
        case .volDown: setMonitorVolume(max(-60, monitorVolumeDb - 2))
        case .volUp: setMonitorVolume(min(-12, monitorVolumeDb + 2))   // -12 dB monitor ceiling
        case .dim: toggleDim()
        case .mute: toggleMonitorMute()
        case .talk: setTalkbackEngaged(!monitorTalkback)
        case .setA: setSpeakerSlot(0)
        case .setB: setSpeakerSlot(1)
        case .setC: setSpeakerSlot(2)
        case .stereo: cycleStereo()
        case .mono: cycleMono()
        case .midSide: toggleMidSide()
        case .sourceMaster: selectMonitorInput(blackHole: false)
        case .sourceBlackHole: selectMonitorInput(blackHole: true)
        case .player: lastError = "모니터 입력 ‘Player’ 소스는 아직 구현 전입니다."
        }
    }
    /// Route a globally-captured keypad key through the SAME configurable monitor-shortcut bindings
    /// the in-app keypad uses — so the global pad and the dock's shortcut settings never disagree
    /// (this fixes Dim/Mute landing on the wrong keys and 1–9 doing nothing).
    ///
    /// Always returns true, i.e. the key is ALWAYS consumed. The feature is keypad *exclusivity*:
    /// while it is on the keypad belongs to the DAW, so an unbound key must be swallowed too rather
    /// than falling through and typing into whatever window happens to be in front. Passing unbound
    /// keys on meant pressing keypad 0 over another app typed a 0 there — not exclusive at all.
    private func handleGlobalKeypad(_ keyCode: UInt16, isDown: Bool, isRepeat: Bool) -> Bool {
        guard let action = monitorShortcutAction(forKeyCode: keyCode) else { return true }
        if action == .talk {
            // Momentary console talkback: engaged while the key is held, released on key-up. Ignore
            // the OS auto-repeat key-downs so a held key does not flicker it on and off.
            if !isRepeat { setTalkbackEngaged(isDown) }
            return true
        }
        // Every other action fires once per press (like the in-app keypad) — swallow repeats and ups.
        if isDown && !isRepeat { performMonitorShortcut(action) }
        return true
    }
    /// Toggle system-wide keypad capture. Requires Accessibility; if it is not granted we prompt for
    /// it (once) and surface a message so the user knows where to allow it.
    func setKeypadCapture(_ on: Bool) {
        if on {
            keypadCaptureRequested = true
            // The same keypad must also work while the DAW itself is frontmost. This used to be a
            // second, easy-to-miss switch ("키패드 단축키"), which made "독점" appear broken.
            if !monitorShortcutsEnabled { setMonitorShortcutsEnabled(true) }
            keypadCapture.onKey = { [weak self] keyCode, isDown, isRepeat in
                self?.handleGlobalKeypad(keyCode, isDown: isDown, isRepeat: isRepeat) ?? false
            }
            if keypadCapture.enable() {
                keypadCaptureEnabled = true
            } else {
                keypadCaptureEnabled = false
                lastError = "키패드 전역 제어에는 손쉬운 사용(Accessibility) 권한이 필요합니다. 시스템 설정 → 개인정보 보호 및 보안 → 손쉬운 사용에서 Neuracoust DAW를 켠 뒤 다시 시도해 주세요."
                let alert = NSAlert()
                alert.messageText = "키패드 독점 권한이 필요합니다"
                alert.informativeText = "시스템 설정 → 개인정보 보호 및 보안 → 손쉬운 사용에서 현재 Neuracoust DAW를 허용해 주세요. 허용 후 DAW로 돌아오면 자동으로 다시 연결합니다."
                alert.addButton(withTitle: "시스템 설정 열기")
                alert.addButton(withTitle: "나중에")
                if alert.runModal() == .alertFirstButtonReturn,
                   let settings = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility") {
                    NSWorkspace.shared.open(settings)
                }
            }
        } else {
            keypadCaptureRequested = false
            keypadCapture.disable()
            keypadCaptureEnabled = false
        }
        UserDefaults.standard.set(keypadCaptureRequested, forKey: "nc.keypadCaptureEnabled")
    }
    /// Re-arm keypad capture at launch if it was on last time (and the permission still holds).
    func restoreKeypadCapture() {
        // SAFETY: the global keypad CGEventTap has repeatedly frozen system-wide keyboard input, so it
        // is no longer auto-armed on launch — the app always starts with capture OFF. The user turns it
        // on explicitly (monitor dock) only when they want it, and can turn it off the instant the
        // freeze recurs. Do NOT re-arm from the persisted flag here.
        keypadCaptureRequested = false
        keypadCaptureEnabled = false
    }
    /// Release the global tap on quit so the keypad returns to the system.
    func releaseKeypadCapture() { keypadCapture.disable() }

    private func persistMonitorShortcuts() {
        let d = UserDefaults.standard
        d.set(monitorShortcutsEnabled, forKey: "nc.monitorShortcutsEnabled")
        let encoded = monitorShortcutKeys.map { "\($0.key):\($0.value)" }.joined(separator: ",")
        d.set(encoded, forKey: "nc.monitorShortcutKeys")
    }
    func restoreMonitorShortcuts() {
        let d = UserDefaults.standard
        monitorShortcutsEnabled = d.bool(forKey: "nc.monitorShortcutsEnabled")
        if let enc = d.string(forKey: "nc.monitorShortcutKeys"), !enc.isEmpty {
            var m: [String: UInt16] = [:]
            for pair in enc.split(separator: ",") {
                let kv = pair.split(separator: ":")
                if kv.count == 2, let code = UInt16(kv[1]) { m[String(kv[0])] = code }
            }
            if !m.isEmpty { monitorShortcutKeys = m }
        }
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
        static let monitorEqLinear = "nc.monitorEqLinearPhase"
        static let monitorEqLowLatency = "nc.monitorEqLowLatencyMonitoring"
        static let monitorEqOeTarget = "nc.monitorEqHeadphoneOeTarget"
        static let physSpeaker = "nc.physicalSpeakerModel"
        static let physHeadphone = "nc.physicalHeadphoneModel"
        static let physAmp = "nc.physicalPowerAmpModel"
        static let physInterface = "nc.physicalAudioInterfaceModel"
        static let physCable = "nc.physicalSpeakerCableModel"
        static let monitorVol = "nc.monitorVolumeDb"
        static let delayComp = "nc.delayCompensation"
        static let recentProjects = "nc.recentProjects"
        static let stopBehavior = "nc.stopBehavior"
        static let bufferSize = "nc.bufferSize"
        static let monitorTemplate = "nc.monitorTemplate"
        static let saved = "nc.settingsSaved"
    }

    /// The full project serialized to a string, sized exactly. Used to snapshot the monitor
    /// station as the global template.
    private func serializeProjectString() -> String {
        guard let handle else { return "" }
        let size = Int(nc_project_serialize(handle, nil, 0))
        guard size > 0 else { return "" }
        var buffer = [CChar](repeating: 0, count: size + 1)
        _ = nc_project_serialize(handle, &buffer, buffer.count)
        return String(cString: buffer)
    }

    /// Apply the saved monitor-station template onto the current project (a new/blank session),
    /// then refresh the dock. No-op if nothing was saved.
    private func applyMonitorTemplate() {
        guard let handle else { return }
        let blob = UserDefaults.standard.string(forKey: SettingsKey.monitorTemplate) ?? ""
        guard !blob.isEmpty else { return }
        if blob.withCString({ nc_apply_monitor_template(handle, $0) }) {
            reloadMonitorState()
        reloadRecordControllerState()
        }
    }

    /// Saves every app-level setting as the default for future launches.
    func saveAllSettings() {
        let d = UserDefaults.standard
        d.set(currentOutputDeviceId, forKey: SettingsKey.outputDevice)
        d.set(currentInputDeviceId, forKey: SettingsKey.inputDevice)
        // NOTE: monitorListenSource (BlackHole exclusive monitoring) is deliberately NOT persisted.
        // It silences the master, so restoring it on launch would boot the app into silence — you
        // would hear nothing and the source would be empty. It is a live choice, reset each launch.
        d.set(false, forKey: SettingsKey.listenSource)
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
        d.set(monitorEqLinearPhase, forKey: SettingsKey.monitorEqLinear)
        d.set(monitorEqHeadphoneOeTarget, forKey: SettingsKey.monitorEqOeTarget)
        d.set(physicalSpeakerModel, forKey: SettingsKey.physSpeaker)
        d.set(physicalHeadphoneModel, forKey: SettingsKey.physHeadphone)
        d.set(physicalPowerAmpModel, forKey: SettingsKey.physAmp)
        d.set(physicalAudioInterfaceModel, forKey: SettingsKey.physInterface)
        d.set(physicalSpeakerCableModel, forKey: SettingsKey.physCable)
        d.set(Double(monitorVolumeDb), forKey: SettingsKey.monitorVol)
        d.set(delayCompensationEnabled, forKey: SettingsKey.delayComp)
        d.set(stopBehavior.rawValue, forKey: SettingsKey.stopBehavior)
        d.set(requestedBufferSize, forKey: SettingsKey.bufferSize)
        // Snapshot the whole monitor station (listen mode, A/B/C speaker sets, DSP modules,
        // dim/talkback, trims) via the project serializer, so a new session inherits it
        // instead of resetting to the bare defaults.
        d.set(serializeProjectString(), forKey: SettingsKey.monitorTemplate)
        d.set(true, forKey: SettingsKey.saved)
        lastError = "전체 설정을 저장했습니다."
    }

    /// Restores saved app-level settings. Called once at start.
    func restorePersistedSettings() {
        let d = UserDefaults.standard
        loadRecentProjects()   // independent of the saved-settings gate below
        restoreMonitorShortcuts()  // its own keys; runs even on a first launch (keeps defaults)
        restoreKeypadCapture()     // re-arm global keypad if it was on and permission still holds
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
        // monitorListenSource is intentionally not restored — it silences the master (see save).
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
        showChannelDelayComp = d.bool(forKey: "nc.showChannelDelayComp")
        // Song form is per-project now (loaded via reloadConductor). Purge the old app-global key so
        // a stale "Intro" from an earlier session stops leaking into every project.
        d.removeObject(forKey: SettingsKey.songForm)
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
        if d.object(forKey: SettingsKey.monitorEqLinear) != nil, d.bool(forKey: SettingsKey.monitorEqLinear) {
            if let handle { nc_monitor_eq_set_linear_phase(handle, true); monitorEqLinearPhase = true }
        }
        if d.object(forKey: SettingsKey.monitorEqLowLatency) != nil {
            let low = d.bool(forKey: SettingsKey.monitorEqLowLatency)
            if let handle { nc_monitor_eq_set_low_latency_monitoring(handle, low) }
            monitorEqLowLatencyMonitoring = low
        }
        if d.object(forKey: SettingsKey.monitorEqOeTarget) != nil, d.bool(forKey: SettingsKey.monitorEqOeTarget) {
            if let handle { nc_monitor_eq_set_headphone_oe_target(handle, true); monitorEqHeadphoneOeTarget = true }
        }
        if let sp = d.string(forKey: SettingsKey.physSpeaker), !sp.isEmpty { setPhysicalSpeakerModel(sp) }
        if let hp = d.string(forKey: SettingsKey.physHeadphone), !hp.isEmpty { setPhysicalHeadphoneModel(hp) }
        if let amp = d.string(forKey: SettingsKey.physAmp), !amp.isEmpty { setPhysicalPowerAmpModel(amp) }
        if let itf = d.string(forKey: SettingsKey.physInterface), !itf.isEmpty { setPhysicalAudioInterfaceModel(itf) }
        if let cab = d.string(forKey: SettingsKey.physCable), !cab.isEmpty { setPhysicalSpeakerCableModel(cab) }
        if d.object(forKey: SettingsKey.monitorVol) != nil { setMonitorVolume(Float(d.double(forKey: SettingsKey.monitorVol))) }
        if d.object(forKey: SettingsKey.delayComp) != nil { setDelayCompensation(d.bool(forKey: SettingsKey.delayComp)) }
        // The saved monitor station last, so it wins over the field-by-field bits above.
        applyMonitorTemplate()
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
        // monitorListenSource is intentionally not restored — it silences the master (see save).
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
        if d.object(forKey: SettingsKey.monitorEqLinear) != nil, d.bool(forKey: SettingsKey.monitorEqLinear) {
            if let handle { nc_monitor_eq_set_linear_phase(handle, true); monitorEqLinearPhase = true }
        }
        if d.object(forKey: SettingsKey.monitorEqLowLatency) != nil {
            let low = d.bool(forKey: SettingsKey.monitorEqLowLatency)
            if let handle { nc_monitor_eq_set_low_latency_monitoring(handle, low) }
            monitorEqLowLatencyMonitoring = low
        }
        if d.object(forKey: SettingsKey.monitorEqOeTarget) != nil, d.bool(forKey: SettingsKey.monitorEqOeTarget) {
            if let handle { nc_monitor_eq_set_headphone_oe_target(handle, true); monitorEqHeadphoneOeTarget = true }
        }
        if let sp = d.string(forKey: SettingsKey.physSpeaker), !sp.isEmpty { setPhysicalSpeakerModel(sp) }
        if let hp = d.string(forKey: SettingsKey.physHeadphone), !hp.isEmpty { setPhysicalHeadphoneModel(hp) }
        if let amp = d.string(forKey: SettingsKey.physAmp), !amp.isEmpty { setPhysicalPowerAmpModel(amp) }
        if let itf = d.string(forKey: SettingsKey.physInterface), !itf.isEmpty { setPhysicalAudioInterfaceModel(itf) }
        if let cab = d.string(forKey: SettingsKey.physCable), !cab.isEmpty { setPhysicalSpeakerCableModel(cab) }
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
        // The saved monitor station comes last so it wins over the field-by-field bits above.
        applyMonitorTemplate()
    }

    /// The monitor station's input: the DAW Master, or the BlackHole loopback (the
    /// computer's audio). Two mutually-exclusive choices, like the Monitor DSP app.
    /// The capture device the source monitor is bound to (chosen from the BlackHole button's
    /// right-click menu). Empty = use the default (BlackHole 2ch when present).
    @Published private(set) var monitorInputDeviceId = ""

    /// Reference monitoring of other apps via the Core Audio process tap (no BlackHole, no input
    /// device, no mic permission — it captures other apps' output directly).
    ///
    /// This is "reference-hold": while armed, the tapped apps stay muted at their own output so you
    /// can A/B between the master and the reference without their sound ever leaking out of the
    /// computer. `otherApps: true` from the "다른 앱" button drives the A/B; the "Master" button
    /// (otherApps: false) flips to the master but keeps the hold armed. Disarm via `exitReference()`.
    func selectMonitorInput(blackHole otherApps: Bool) {
        guard let handle else { return }
        if otherApps {
            // Left-click "다른 앱": arm on first press, then A/B master↔reference on each press —
            // always staying armed (apps muted), so nothing leaks when you audition the master.
            if !referenceArmed {
                nc_monitor_set_reference_armed(handle, true)
                nc_monitor_set_listen_source(handle, true)
                // The process tap / aggregate device needs a few hundred ms to spin up after arming.
                // The very first press otherwise routes to a not-yet-producing tap and stays silent
                // until a master↔다른앱 toggle — which works because time passes AND it re-triggers the
                // rising edge. Mimic that once here: after the tap is up, re-latch false→true so the
                // monitor grabs the now-running tap instead of the empty pre-start state.
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self] in
                    guard let self, let h = self.handle,
                          self.referenceArmed, self.monitorListenSource else { return }
                    nc_monitor_set_listen_source(h, false)
                    nc_monitor_set_listen_source(h, true)
                    self.monitorListenSource = nc_monitor_listen_source(h)
                }
            } else {
                nc_monitor_set_listen_source(handle, !monitorListenSource)
            }
        } else {
            // "Master": hear the master. Keep the hold armed if it already is (no leak); if it was
            // never armed, this is just the default master monitoring.
            if referenceArmed {
                nc_monitor_set_listen_source(handle, false)
            }
        }
        referenceArmed = nc_monitor_reference_armed(handle)
        monitorListenSource = nc_monitor_listen_source(handle)
    }

    /// Fully leave reference-hold: stop the tap and unmute the tapped apps so they play normally
    /// from the computer again. Right-click "다른 앱" → 레퍼런스 종료.
    func exitReference() {
        guard let handle else { return }
        nc_monitor_set_reference_armed(handle, false)   // engine also clears the listening state
        referenceArmed = nc_monitor_reference_armed(handle)
        monitorListenSource = nc_monitor_listen_source(handle)
    }

    /// Explicitly bind the source monitor to a specific internal input device (the BlackHole
    /// button's right-click menu), and switch monitoring to it.
    func selectMonitorInputDevice(_ id: String) {
        monitorInputDeviceId = id
        openMonitorInput(deviceId: id)
    }

    /// Turn source monitoring on and open the capture device. `deviceId == nil` keeps the chosen
    /// device (or defaults to BlackHole 2ch); a value forces that device.
    private func openMonitorInput(deviceId: String?) {
        // Capturing any input device (even a virtual loopback like BlackHole) goes through the
        // macOS microphone privacy gate. Request it explicitly so the system prompt reliably
        // appears — the implicit prompt that opening the input queue would raise does not fire
        // dependably for a locally-built (ad-hoc-signed) app.
        requestMicrophoneAccess { [weak self] granted in
            guard let self else { return }
            guard granted else {
                self.lastError = "마이크(입력) 권한이 필요합니다. 시스템 설정 → 개인정보 보호 및 보안 → 마이크에서 Neuracoust DAW를 켜 주세요."
                return
            }
            // Light the button now, then open the input device on the next runloop. The device
            // enumeration + AudioQueue open are synchronous CoreAudio calls that block the main
            // thread ~1 s; deferring lets the UI reflect the press immediately.
            self.monitorListenSource = true
            DispatchQueue.main.async {
                if self.inputDevices.isEmpty { self.refreshInputDevices() }
                let target = deviceId ?? self.defaultMonitorInputDeviceId()
                if let id = target, !id.isEmpty {
                    self.monitorInputDeviceId = id
                    self.setInputDevice(id)
                }
                self.setMonitorListenSource(true)
            }
        }
    }

    /// The default capture device for source monitoring: the previously chosen one if still
    /// present, else a **BlackHole 2ch** loopback (the reference default), else any BlackHole,
    /// else the first input.
    private func defaultMonitorInputDeviceId() -> String? {
        func norm(_ s: String) -> String { s.lowercased().replacingOccurrences(of: " ", with: "") }
        if !monitorInputDeviceId.isEmpty, inputDevices.contains(where: { $0.id == monitorInputDeviceId }) {
            return monitorInputDeviceId
        }
        if let bh2 = inputDevices.first(where: { norm($0.name).contains("blackhole2ch") || norm($0.name).contains("blackhole2channel") }) {
            return bh2.id
        }
        if let bh = inputDevices.first(where: { $0.name.lowercased().contains("blackhole") }) {
            return bh.id
        }
        return inputDevices.first?.id
    }

    /// Ask macOS for microphone (audio input) access, invoking `completion` on the main actor
    /// with the result. Already-authorized returns true immediately; a first request shows the
    /// system prompt; a prior denial returns false (the caller points the user at System Settings).
    private func requestMicrophoneAccess(_ completion: @escaping (Bool) -> Void) {
        switch AVCaptureDevice.authorizationStatus(for: .audio) {
        case .authorized:
            completion(true)
        case .notDetermined:
            AVCaptureDevice.requestAccess(for: .audio) { granted in
                Task { @MainActor in completion(granted) }
            }
        default:
            completion(false)
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

    /// The talkback mic = the engine's open input device (shared with the reference-monitor source;
    /// the engine holds one input device at a time). The talkback route is already `listen_room`, so
    /// this mic is what the Listen Room broadcast carries when talkback is engaged.
    var talkbackMicId: String { currentInputDeviceId }
    /// Where the talkback mic goes: to the Listen Room broadcast (default), the local
    /// monitor bus, or both. Listen-room-only keeps the engineer's speakers dry.
    func setTalkbackRoute(_ route: String) {
        guard let handle else { return }
        nc_monitor_set_talkback_route(handle, route)
        talkbackRoute = readString { nc_monitor_talkback_route(handle, $0, $1) }
    }
    func setTalkbackMic(_ id: String) {
        requestMicrophoneAccess { [weak self] granted in
            guard let self else { return }
            guard granted else {
                self.lastError = "마이크(입력) 권한이 필요합니다. 시스템 설정 → 개인정보 보호 및 보안 → 마이크에서 Neuracoust DAW를 켜 주세요."
                return
            }
            self.setInputDevice(id)
        }
    }
    /// Number of physical input channels on the talkback device — the channel picker enumerates these.
    var talkbackChannelCount: Int {
        guard let handle else { return 1 }
        return max(1, Int(nc_talkback_channel_count(handle)))
    }
    /// Live peak (0..1) of a physical input channel, for the "which mics are live" indicator. Reads 0
    /// when the input queue is idle (engage Talk or input-monitoring to make the channels flow).
    func talkbackChannelActivity(_ oneBased: Int) -> Float {
        guard let handle else { return 0 }
        return nc_talkback_channel_activity(handle, Int32(oneBased))
    }
    func setTalkbackChannel(_ oneBased: Int) {
        guard let handle else { return }
        nc_monitor_set_talkback_channel(handle, Int32(max(1, oneBased)))
        talkbackChannel = Int(nc_monitor_talkback_channel(handle))
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
        // active, so its row text goes stale unless the list is re-read. reloadMonitorState
        // also swaps the single EQ's band values to this slot's context.
        reloadMonitorState()
        reloadRecordControllerState()
    }

    /// The speaker-model catalog is static; output routes are regenerated from the
    /// currently opened CoreAudio device's real channel count.
    // These catalogs are static engine data (no handle needed). Computed, not lazy, so a
    // one-time access before the engine existed can never cache an empty list.
    // These catalogs are static (a fixed ~200-entry speaker list, etc.). They were computed
    // properties, so every access re-ran hundreds of bridge calls + String allocations —
    // and SwiftUI eagerly evaluates the monitor dock's context-menu content on every body
    // recompute (30 Hz), so rebuilding them each tick pinned the main thread near 40% CPU at
    // idle. Cache them once (lazy), since they never change during a session.
    lazy var speakerModelCatalog: [String] = (0..<Int(nc_speaker_model_count())).map { i in
        readString { nc_speaker_model_name(Int32(i), $0, $1) }
    }
    @Published private(set) var speakerOutputRoutes: [String] = ["None", "Main 1-2"]
    @Published private(set) var outputChannelCount = 2
    private func reloadSpeakerOutputRoutes(_ channels: Int) {
        guard let handle else { return }
        let safeChannels = max(2, channels)
        let routes = (0..<Int(nc_speaker_output_route_count(handle))).map { i in
            readString { nc_speaker_output_route(handle, Int32(i), $0, $1) }
        }
        setIfChanged(\.outputChannelCount, safeChannels)
        setIfChanged(\.speakerOutputRoutes, routes)
    }
    lazy var headphoneModelCatalog: [String] = (0..<Int(nc_headphone_model_count())).map { i in
        readString { nc_headphone_model_name(Int32(i), $0, $1) }
    }
    lazy var powerAmpModelCatalog: [String] = (0..<Int(nc_power_amp_model_count())).map { i in
        readString { nc_power_amp_model_name(Int32(i), $0, $1) }
    }
    lazy var speakerCableModelCatalog: [String] = (0..<Int(nc_speaker_cable_model_count())).map { i in
        readString { nc_speaker_cable_model_name(Int32(i), $0, $1) }
    }
    lazy var powerCableModelCatalog: [String] = (0..<Int(nc_power_cable_model_count())).map { i in
        readString { nc_power_cable_model_name(Int32(i), $0, $1) }
    }
    lazy var connectorModelCatalog: [String] = (0..<Int(nc_connector_model_count())).map { i in
        readString { nc_connector_model_name(Int32(i), $0, $1) }
    }
    lazy var audioInterfaceModelCatalog: [String] = (0..<Int(nc_audio_interface_model_count())).map { i in
        readString { nc_audio_interface_model_name(Int32(i), $0, $1) }
    }
    /// Interface models with an independent measurement located (lights the "측정" badge only —
    /// no bundled D/A profile yet).
    // Interfaces with a profile the picker marks as "측정 있음": baked (성적서) OR live-measured
    // (your device). Computed, not cached, so a just-saved measurement shows up immediately.
    private lazy var audioInterfaceBakedMeasured: Set<String> = Set(audioInterfaceModelCatalog.filter { m in
        m.withCString { nc_audio_interface_model_measured($0) }
    })
    var audioInterfaceMeasured: Set<String> {
        audioInterfaceBakedMeasured.union(audioInterfaceModelCatalog.filter { interfaceMeasuredHasProfile($0) })
    }

    // The real speaker/headphone the user monitors on (definition, not a simulation),
    // and whether speaker and headphone are mutually exclusive.
    @Published var physicalSpeakerModel = ""
    @Published var physicalHeadphoneModel = ""
    // Power amp + speaker cable — only meaningful when the physical speaker is passive.
    @Published var physicalPowerAmpModel = ""
    @Published var physicalSpeakerCableModel = ""
    @Published var physicalPowerCableModel = ""
    @Published var physicalConnectorModel = ""
    // The audio interface's D/A output-stage model (catalog/definition only — no audio effect yet).
    @Published var physicalAudioInterfaceModel = ""
    // Purpose 2: render the physical interface AS this model (A→B). Stored intent; inert until raw
    // measured profiles exist for both. Empty = render as itself.
    @Published var physicalAudioInterfaceTargetModel = ""
    // True only when a real raw-measured A→B transform touches audio. Always false today.
    @Published var audioInterfaceTransformActive = false
    @Published var monitorOutputExclusive = true
    /// Swap L/R in the monitor path — for speakers wired backwards.
    @Published var monitorSwapLeftRight = false

    func toggleMonitorSwapLeftRight() {
        guard let handle else { return }
        nc_monitor_toggle_swap_left_right(handle)
        monitorSwapLeftRight = nc_monitor_swap_left_right(handle)
        refreshHistory()
    }

    /// A passive physical speaker needs an external amp + cable; an active monitor doesn't.
    var physicalSpeakerIsPassive: Bool {
        !physicalSpeakerModel.isEmpty && physicalSpeakerModel.withCString { nc_speaker_model_is_passive($0) }
    }

    func setPhysicalSpeakerModel(_ model: String) {
        guard let handle else { return }
        _ = model.withCString { nc_monitor_set_physical_speaker_model(handle, $0) }
        physicalSpeakerModel = readString { nc_monitor_physical_speaker_model(handle, $0, $1) }
        refreshHistory()
    }
    // The power amp / cable colour a PASSIVE speaker's sound via a name heuristic (until measured),
    // so their setters re-derive the monitor EQ like the speaker model does.
    var powerAmpToneActive: Bool { handle.map { nc_power_amp_tone_active($0) } ?? false }
    var speakerCableToneActive: Bool { handle.map { nc_speaker_cable_tone_active($0) } ?? false }
    func setPhysicalPowerAmpModel(_ model: String) {
        guard let handle else { return }
        _ = model.withCString { nc_monitor_set_physical_power_amp_model(handle, $0) }
        physicalPowerAmpModel = readString { nc_monitor_physical_power_amp_model(handle, $0, $1) }
        syncMonitorEqToContext()
        refreshHistory()
    }
    func setPhysicalSpeakerCableModel(_ model: String) {
        guard let handle else { return }
        _ = model.withCString { nc_monitor_set_physical_speaker_cable_model(handle, $0) }
        physicalSpeakerCableModel = readString { nc_monitor_physical_speaker_cable_model(handle, $0, $1) }
        syncMonitorEqToContext()
        refreshHistory()
    }
    func setPhysicalPowerCableModel(_ model: String) {
        guard let handle else { return }
        _ = model.withCString { nc_monitor_set_physical_power_cable_model(handle, $0) }
        physicalPowerCableModel = readString { nc_monitor_physical_power_cable_model(handle, $0, $1) }
        refreshHistory()
    }
    func setPhysicalConnectorModel(_ model: String) {
        guard let handle else { return }
        _ = model.withCString { nc_monitor_set_physical_connector_model(handle, $0) }
        physicalConnectorModel = readString { nc_monitor_physical_connector_model(handle, $0, $1) }
        refreshHistory()
    }
    func setPhysicalAudioInterfaceModel(_ model: String) {
        guard let handle else { return }
        _ = model.withCString { nc_monitor_set_physical_audio_interface_model(handle, $0) }
        physicalAudioInterfaceModel = readString { nc_monitor_physical_audio_interface_model(handle, $0, $1) }
        audioInterfaceTransformActive = nc_audio_interface_transform_active(handle)
        syncMonitorEqToContext()   // a measured interface (e.g. Kurzweil UNiTE-2) flattens its D/A FR here
        refreshHistory()
    }
    /// Purpose 2: pick the interface to render AS (A→B). Empty string clears it (render as itself).
    func setPhysicalAudioInterfaceTargetModel(_ model: String) {
        guard let handle else { return }
        _ = model.withCString { nc_monitor_set_physical_audio_interface_target(handle, $0) }
        physicalAudioInterfaceTargetModel = readString { nc_monitor_physical_audio_interface_target(handle, $0, $1) }
        audioInterfaceTransformActive = nc_audio_interface_transform_active(handle)
        syncMonitorEqToContext()   // a target with a measured profile colours the monitor as that interface
        refreshHistory()
    }
    /// Optional 2단계: apply the modeled interface's measured nonlinear harmonic character (waveshaper).
    @Published private(set) var monitorInterfaceModelingEnabled = false
    func setMonitorInterfaceModeling(_ on: Bool) {
        guard let handle else { return }
        nc_monitor_set_interface_modeling_enabled(handle, on)
        monitorInterfaceModelingEnabled = nc_monitor_interface_modeling_enabled(handle)
        refreshHistory()
    }

    // Monitor parametric EQ (0–64 bands, monitor path only).
    struct EqBand: Identifiable, Equatable {
        let id: Int          // band index in the engine
        var enabled: Bool
        var type: String     // peaking / low_shelf / high_shelf / high_pass / low_pass / notch
        var frequencyHz: Double
        var gainDb: Double
        var q: Double
    }
    @Published private(set) var monitorEqBands: [EqBand] = []
    @Published var monitorEqOpen = false
    static let eqBandTypes = ["peaking", "low_shelf", "high_shelf", "high_pass", "low_pass", "notch"]
    static let eqBandTypeLabels = ["peaking": "피킹", "low_shelf": "로우쉘프", "high_shelf": "하이쉘프",
                                   "high_pass": "하이패스", "low_pass": "로우패스", "notch": "노치"]

    func reloadMonitorEq() {
        guard let handle else { return }
        let count = Int(nc_monitor_eq_band_count(handle))
        monitorEqBands = (0..<count).map { i in
            var enabled = false, freq = 0.0, gain = 0.0, q = 0.0
            var typeBuf = [CChar](repeating: 0, count: 32)
            _ = nc_monitor_eq_band(handle, Int32(i), &enabled, &typeBuf, typeBuf.count, &freq, &gain, &q)
            return EqBand(id: i, enabled: enabled, type: String(cString: typeBuf),
                          frequencyHz: freq, gainDb: gain, q: q)
        }
    }
    func addEqBand(type: String = "peaking", freq: Double = 1000, gain: Double = 0, q: Double = 1) {
        guard let handle, monitorEqBands.count < 64 else { return }
        if nc_monitor_eq_add_band(handle, type, freq, gain, q) >= 0 { reloadMonitorEq(); refreshHistory() }
    }
    /// Live edit (a knob drag) — no undo step; the view commits the gesture.
    func updateEqBand(_ band: EqBand) {
        guard let handle else { return }
        _ = band.type.withCString {
            nc_monitor_eq_set_band(handle, Int32(band.id), band.enabled, $0,
                                   band.frequencyHz, band.gainDb, band.q)
        }
        reloadMonitorEq()
    }
    func removeEqBand(_ index: Int) {
        guard let handle else { return }
        if nc_monitor_eq_remove_band(handle, Int32(index)) { reloadMonitorEq(); refreshHistory() }
    }
    func clearMonitorEq() {
        guard let handle else { return }
        nc_monitor_eq_clear(handle); reloadMonitorEq(); refreshHistory()
    }
    // Acoustic measurement (②b): sweep out a channel, capture the mic, deconvolve to a curve.
    @Published private(set) var measurementActive = false
    @Published private(set) var measurementProgress: Double = 0
    private var measuringChannel = 0

    // Measurement microphone selection. A mic with a calibration file supports absolute tone
    // correction; without one, only L/R matching + relative correction is trustworthy.
    @Published var measurementMicModel = ""
    lazy var measurementMicCatalog: [String] = {
        guard let handle else { return [] }
        let count = Int(nc_measurement_mic_model_count())
        return (0..<count).map { i in readString { nc_measurement_mic_model_name(Int32(i), $0, $1) } }
    }()
    func setMeasurementMic(_ name: String) {
        guard let handle else { return }
        _ = name.withCString { nc_set_measurement_mic_model(handle, $0) }
        measurementMicModel = readString { nc_measurement_mic_model(handle, $0, $1) }
        refreshHistory()
    }
    func measurementMicHasCalibration(_ name: String) -> Bool {
        name.withCString { nc_measurement_mic_has_calibration($0) }
    }

    func startMeasurement(channel: Int) {
        guard let handle, !measurementActive else { return }
        if nc_measure_start(handle, Int32(channel)) {
            measuringChannel = channel
            measurementProgress = 0
            measurementActive = true
        }
    }
    func cancelMeasurement() {
        guard let handle else { return }
        nc_measure_cancel(handle)
        measurementActive = false
    }
    func measureHasCurve(_ channel: Int) -> Bool {
        guard let handle else { return false }
        return nc_measure_has_curve(handle, Int32(channel))
    }
    func measureCurveResponse(channel: Int, count: Int = 160) -> [Double] {
        guard let handle else { return Array(repeating: 0, count: count) }
        var out = [Double](repeating: 0, count: count)
        nc_measure_curve_response(handle, Int32(channel), &out, Int32(count), 20.0, 20000.0)
        return out
    }
    /// Flatten the measured in-room response toward the Harman target (room correction, ③).
    /// In the single-EQ design this turns room tuning ON for the active slot; the correction
    /// then rides on top of that slot's model curve when the EQ is re-derived (one combined EQ).
    func applyRoomCorrection(channel: Int) {
        guard let handle, nc_measure_has_curve(handle, Int32(channel)) else { return }
        if let set = activeSpeakerSet {
            nc_monitor_set_speaker_room_eq(handle, Int32(set.id), true)
        }
        reloadMonitorState()   // enables room tuning as one history step + re-derives the EQ
        refreshHistory()
    }
    // Interface loopback measurement (②d): patch the interface DAC output → ADC input, sweep,
    // and one ESS capture yields the D/A frequency response AND the harmonic coefficients for
    // the selected physical interface. A live measurement overrides the offline baked profile.
    @Published private(set) var measuringInterface = false
    // Loopback channel patch: which physical output the sweep exits, which input it returns on.
    @Published var measureOutputChannel = 1
    @Published var measureInputChannel = 1
    // Live gain-setup meter: peak (linear 0..1) of the chosen loopback input channel.
    @Published private(set) var measurementLevelCheckActive = false
    @Published private(set) var measurementInputLevel: Float = 0
    func setMeasurementLevelCheck(_ on: Bool) {
        guard let handle else { return }
        if on {
            requestMicrophoneAccess { [weak self] granted in
                guard let self, let handle = self.handle, granted else { return }
                nc_measure_level_check(handle, true)
                self.measurementLevelCheckActive = true
            }
        } else {
            nc_measure_level_check(handle, false)
            measurementLevelCheckActive = false
            measurementInputLevel = 0
        }
    }
    var measureOutputChannelCount: Int { guard let handle else { return 2 }; return max(2, Int(nc_measure_output_channel_count(handle))) }
    var measureInputChannelCount: Int { guard let handle else { return 1 }; return max(1, Int(nc_measure_input_channel_count(handle))) }
    func setMeasureOutputChannel(_ ch: Int) {
        guard let handle else { return }
        nc_measure_set_output_channel(handle, Int32(ch))
        measureOutputChannel = Int(nc_measure_output_channel(handle))
    }
    func setMeasureInputChannel(_ ch: Int) {
        guard let handle else { return }
        nc_measure_set_input_channel(handle, Int32(ch))
        measureInputChannel = Int(nc_measure_input_channel(handle))
    }
    func startInterfaceMeasurement() {
        guard let handle, !measurementActive else { return }
        guard !physicalAudioInterfaceModel.isEmpty else {
            lastError = "먼저 '실물 오디오 인터페이스' 모델을 선택하세요 — 측정 결과를 저장할 대상이 필요합니다."
            return
        }
        requestMicrophoneAccess { [weak self] granted in
            guard let self, let handle = self.handle else { return }
            guard granted else {
                self.lastError = "루프백 캡처에 입력(마이크) 권한이 필요합니다. 시스템 설정 → 개인정보 보호 및 보안 → 마이크에서 켜 주세요."
                return
            }
            if self.measurementLevelCheckActive {         // stop the reference tone before the sweep
                nc_measure_level_check(handle, false)
                self.measurementLevelCheckActive = false
            }
            if nc_measure_interface_start(handle) {
                self.measuringInterface = true
                self.measurementProgress = 0
                self.measurementActive = true
            }
        }
    }
    func interfaceMeasuredHasProfile(_ name: String) -> Bool {
        guard let handle, !name.isEmpty else { return false }
        return name.withCString { nc_measure_interface_has_profile(handle, $0) }
    }
    func interfaceMeasuredThd(_ name: String) -> Double {
        guard let handle, !name.isEmpty else { return 0 }
        return name.withCString { nc_measure_interface_thd(handle, $0) }
    }
    /// Measured harmonics [c2..c7] (linear amplitude ratios) for the card's harmonic graph.
    func interfaceMeasuredHarmonics(_ name: String) -> [Double] {
        guard let handle, !name.isEmpty else { return [] }
        var out = [Double](repeating: 0, count: 6)
        name.withCString { nc_measure_interface_harmonics(handle, $0, &out, 6) }
        return out
    }
    /// Measured D/A frequency response (dB deviation), sampled log 20 Hz–20 kHz, for the card graph.
    func interfaceMeasuredCurve(_ name: String, count: Int = 96) -> [Double] {
        guard let handle, !name.isEmpty else { return [] }
        var out = [Double](repeating: 0, count: count)
        name.withCString { nc_measure_interface_curve_response(handle, $0, &out, Int32(count), 20, 20000) }
        return out
    }
    /// THD-vs-level table for the current output interface (from the saved multi-level profile).
    func interfaceMeasuredThdVsLevel() -> [(dbfs: Double, thd: Double)] {
        guard let handle else { return [] }
        let n = Int(nc_measure_interface_level_count(handle))
        return (0..<n).map { (dbfs: nc_measure_interface_level_dbfs(handle, Int32($0)),
                              thd: nc_measure_interface_level_thd(handle, Int32($0))) }
    }
    func clearInterfaceMeasurement(_ name: String) {
        guard let handle, !name.isEmpty else { return }
        name.withCString { nc_measure_interface_clear(handle, $0) }
        syncMonitorEqToContext()
        objectWillChange.send()
    }

    // A just-finished measurement, held for the user to review (verdict) and confirm before it is
    // saved + applied — so a clipped or too-low capture never silently overwrites a good profile.
    @Published private(set) var measurementPendingValid = false
    @Published private(set) var measurementPendingThd: Double = 0
    @Published private(set) var measurementPendingPeak: Float = 0   // sweep peak 0..1 (>=~0.99 clipped)
    func commitInterfaceMeasurement() {
        guard let handle else { return }
        nc_measure_interface_commit(handle)
        measurementPendingValid = false
        syncMonitorEqToContext()   // apply the committed interface-FR compensation
        objectWillChange.send()
    }
    func discardInterfaceMeasurement() {
        guard let handle else { return }
        nc_measure_interface_discard(handle)
        measurementPendingValid = false
        multiLevelResults = []
        multiLevelDone = false
    }

    // Multi-level auto run (A단계): the user just connects the loopback and clicks once. We calibrate
    // the loopback gain from the reference tone, auto-scale each sweep so it lands at a target return
    // level without clipping, run the sweeps in sequence, and plot THD vs level (소자 반응 모니터링).
    @Published private(set) var multiLevelActive = false
    @Published private(set) var multiLevelStep = 0
    @Published private(set) var multiLevelTotal = 0
    @Published private(set) var multiLevelDone = false
    @Published private(set) var multiLevelResults: [(dbfs: Double, thd: Double)] = []
    private var multiLevelAmps: [Double] = []

    func startMultiLevelMeasurement() {
        guard let handle, !measurementActive, !multiLevelActive else { return }
        guard !physicalAudioInterfaceModel.isEmpty else {
            lastError = "먼저 '실물 오디오 인터페이스' 모델을 선택하세요."
            return
        }
        requestMicrophoneAccess { [weak self] granted in
            guard let self, let handle = self.handle else { return }
            guard granted else { self.lastError = "루프백 캡처에 입력(마이크) 권한이 필요합니다."; return }
            nc_measure_interface_reset_levels(handle)
            nc_measure_interface_discard(handle)
            self.multiLevelResults = []
            self.multiLevelDone = false
            self.measurementPendingValid = false
            // Calibrate loopback gain from the 0.5 reference tone (level check). The input queue can
            // take a beat to open (SoundGrid) and the tone to loop back, so poll until it appears.
            nc_measure_level_check(handle, true)
            self.multiLevelActive = true
            self.multiLevelStep = 0
            self.multiLevelTotal = 0
            self.calibratePolls = 0
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { [weak self] in self?.calibrateLoopbackThenRun() }
        }
    }

    private var calibratePolls = 0
    private func calibrateLoopbackThenRun() {
        guard let handle, multiLevelActive else { return }
        let ret = Double(nc_measure_input_level(handle))   // return level at 0.5 send
        calibratePolls += 1
        if ret > 0.02 {                                    // loopback signal established
            nc_measure_level_check(handle, false)
            let gain = ret / 0.5
            let targets: [Double] = [-1, -7, -13, -19, -25]   // target return dBFS, near max → down
            multiLevelAmps = targets.map { min(0.99, pow(10, $0 / 20) / gain) }
            multiLevelTotal = multiLevelAmps.count
            multiLevelStep = 0
            runNextMultiLevel()
            return
        }
        if calibratePolls > 20 {                           // ~4 s with no signal → give up
            nc_measure_level_check(handle, false)
            multiLevelActive = false
            lastError = "루프백 신호가 없습니다 — 자동 측정에는 입력 신호가 필요합니다. 케이블/SoundGrid 패치와 입력 장치(Waves SoundGrid) 선택을 확인하세요."
            return
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) { [weak self] in self?.calibrateLoopbackThenRun() }
    }

    private func runNextMultiLevel() {
        guard let handle, multiLevelActive else { return }
        guard multiLevelStep < multiLevelAmps.count else {
            // All levels done — surface the curve + save/discard review.
            multiLevelActive = false
            reloadMultiLevelResults()
            multiLevelDone = true
            measurementPendingValid = true   // reuse the review card (commit writes the multi-level profile)
            return
        }
        nc_measure_set_sweep_amplitude(handle, multiLevelAmps[multiLevelStep])
        if nc_measure_interface_start(handle) {
            measuringInterface = true
            measurementProgress = 0
            measurementActive = true
        }
    }

    private func reloadMultiLevelResults() {
        guard let handle else { return }
        let n = Int(nc_measure_interface_level_count(handle))
        multiLevelResults = (0..<n).map {
            (dbfs: nc_measure_interface_level_dbfs(handle, Int32($0)),
             thd: nc_measure_interface_level_thd(handle, Int32($0)))
        }
    }

    // Called each poll tick while measuring; finishes when the sweep has played out.
    fileprivate func pollMeasurement() {
        guard measurementActive, let handle else { return }
        measurementProgress = nc_measure_progress(handle)
        if !nc_measure_active(handle) {
            if measuringInterface {
                _ = nc_measure_interface_finish(handle)   // holds a PENDING result for review
                measuringInterface = false
                measurementPendingThd = nc_measure_interface_pending_thd(handle)
                measurementPendingPeak = nc_measure_interface_pending_peak(handle)
                if multiLevelActive {
                    // Multi-level run: record this level's result and chain to the next sweep.
                    let retDb = measurementPendingPeak > 1e-6 ? 20.0 * log10(Double(measurementPendingPeak)) : -120.0
                    nc_measure_interface_record_level(handle, retDb)
                    multiLevelStep += 1
                    measurementProgress = 1
                    measurementActive = false
                    DispatchQueue.main.asyncAfter(deadline: .now() + 0.35) { [weak self] in self?.runNextMultiLevel() }
                    return
                }
                measurementPendingValid = nc_measure_interface_pending(handle)
                // Not applied until the user commits (see commitInterfaceMeasurement).
            } else {
                _ = nc_measure_finish(handle, Int32(measuringChannel))
            }
            measurementProgress = 1
            measurementActive = false
        }
    }

    /// Speaker models with a measured curve — the targets the virtual monitor can model.
    lazy var virtualMonitorTargets: [String] = {
        guard let handle else { return [] }
        let count = Int(nc_virtual_monitor_count(handle))
        return (0..<count).map { i in
            readString { nc_virtual_monitor_name(handle, Int32(i), $0, $1) }
        }
    }()

    /// Headphone models that carry a measured curve — the headphone equivalent of the virtual
    /// monitor targets (drives the headphone-mode EQ + the "측정" badge).
    lazy var headphoneMonitorTargets: [String] = {
        guard let handle else { return [] }
        let count = Int(nc_headphone_profile_count(handle))
        return (0..<count).map { i in
            readString { nc_headphone_profile_name(handle, Int32(i), $0, $1) }
        }
    }()
    func headphoneModelHasCurve(_ model: String) -> Bool {
        headphoneMonitorTargets.contains { $0 == model || stripSpeakerSlotPrefix($0) == stripSpeakerSlotPrefix(model) }
    }
    /// A headphone model's measured curve as [[hz, db]] points (log grid), or nil if unmeasured.
    func headphoneCurvePoints(_ model: String, count: Int = 200) -> [[Double]]? {
        guard let handle, !model.isEmpty else { return nil }
        var out = [Double](repeating: 0, count: count)
        let ok = model.withCString { nc_headphone_profile_response(handle, $0, &out, Int32(count), 20.0, 20000.0) }
        guard ok else { return nil }
        let freqs = EngineController.monitorCurveFrequencies(count: count)
        return zip(freqs, out).map { [$0, $1] }
    }
    /// The measured D/A FR curve of an audio-interface model (nil when it has no measured profile).
    func audioInterfaceCurvePoints(_ model: String, count: Int = 200) -> [[Double]]? {
        guard let handle, !model.isEmpty else { return nil }
        var out = [Double](repeating: 0, count: count)
        let ok = model.withCString { nc_audio_interface_profile_response(handle, $0, &out, Int32(count), 20.0, 20000.0) }
        guard ok else { return nil }
        let freqs = EngineController.monitorCurveFrequencies(count: count)
        return zip(freqs, out).map { [$0, $1] }
    }
    func audioInterfaceHasProfile(_ model: String) -> Bool { audioInterfaceCurvePoints(model, count: 8) != nil }

    /// Model a target speaker on the physical monitor — loads its fitted curve into the monitor
    /// EQ so the output takes on that speaker's tonal character.
    func applyVirtualMonitor(_ catalogName: String) {
        guard let handle else { return }
        if catalogName.withCString({ nc_monitor_eq_apply_virtual_monitor(handle, $0) }) {
            reloadMonitorEq(); refreshHistory()
        }
    }

    /// Log-spaced magnitude curve (dB) for the EQ display, 20 Hz–20 kHz.
    func monitorEqResponse(count: Int = 160) -> [Double] {
        guard let handle else { return Array(repeating: 0, count: count) }
        var out = [Double](repeating: 0, count: count)
        nc_monitor_eq_response(handle, &out, Int32(count), 20.0, 20000.0)
        return out
    }
    /// The room-tuning correction curve (what a room measurement would impose), or nil until
    /// a measurement exists. Sampled on the same log grid as `monitorEqResponse`.
    func roomCorrectionResponse(channel: Int = 0, count: Int = 160) -> [Double]? {
        guard let handle else { return nil }
        var out = [Double](repeating: 0, count: count)
        let available = nc_monitor_room_correction_response(handle, Int32(channel), &out, Int32(count), 20.0, 20000.0)
        return available ? out : nil
    }
    /// The frequencies (Hz) matching the magnitude arrays above — log-spaced 20 Hz–20 kHz.
    static func monitorCurveFrequencies(count: Int = 160) -> [Double] {
        (0..<count).map { 20.0 * pow(1000.0, Double($0) / Double(max(1, count - 1))) }
    }
    func setPhysicalHeadphoneModel(_ model: String) {
        guard let handle else { return }
        _ = model.withCString { nc_monitor_set_physical_headphone_model(handle, $0) }
        physicalHeadphoneModel = readString { nc_monitor_physical_headphone_model(handle, $0, $1) }
        // The physical headphone is the correction term in headphone mode — re-derive the EQ so
        // selecting it actually flattens it out (otherwise the correction never enters the chain).
        syncMonitorEqToContext()
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
        reloadMonitorState()          // re-derives the single EQ if this is the active slot
        refreshHistory()
    }

    /// The one monitor EQ, rebuilt from the active monitoring context ("one EQ, values swap").
    /// One modeller at a time, following the output mode: speaker mode → the active A/B/C speaker
    /// model's curve (physical route → room-tuning only); headphone mode → the physical headphone
    /// model's curve. Room correction rides on top when measured and the slot's room-EQ is on.
    /// Derived state: records no undo step (the model/route/mode change already recorded one).
    func syncMonitorEqToContext() {
        guard let handle else { return }
        var slotModel = ""       // the A/B/C target to simulate (speaker or headphone)
        var correctionModel = "" // the physical headphone being worn (headphone mode → corrected)
        var includeRoom = false
        // The active A/B/C slot holds one model — a virtual SPEAKER or a HEADPHONE — to simulate.
        // A physical output route bypasses the modeller.
        if let set = activeSpeakerSet {
            let physical = !set.output.isEmpty && set.output.caseInsensitiveCompare("None") != .orderedSame
            if !physical {
                let bare = stripSpeakerSlotPrefix(set.model)
                if speakerModelHasCurve(bare) || headphoneModelHasCurve(bare) { slotModel = bare }
            }
            includeRoom = set.roomEq
        }
        // In headphone mode the physical headphone you wear is corrected toward neutral so the
        // simulated target reproduces cleanly, on top of whatever the slot models.
        if outputMode == .headphone {
            let hp = stripSpeakerSlotPrefix(physicalHeadphoneModel)
            if headphoneModelHasCurve(hp) { correctionModel = hp }
        }
        let roomAvailable = includeRoom && (nc_measure_has_curve(handle, 0) || nc_measure_has_curve(handle, 1))
        slotModel.withCString { slot in correctionModel.withCString { corr in
            nc_monitor_eq_sync(handle, slot, corr, roomAvailable)
        }}
        reloadMonitorEq()
        monitorEqLatencyMs = nc_monitor_eq_latency_ms(handle)
        monitorEqLowLatencyActive = nc_monitor_eq_low_latency_active(handle)
    }

    /// Linear-phase (FIR) monitor EQ: matches the target across the whole band (no biquad ripple
    /// or treble cramping) at the cost of latency. Off = the low-latency biquad path.
    @Published var monitorEqLinearPhase = false
    @Published private(set) var monitorEqLatencyMs: Double = 0
    /// While on (the default), an armed or input-monitoring track drops the linear-phase EQ's
    /// delay by falling back to the minimum-phase fit of the same curve — you play in time and
    /// still mix through the exact curve once nothing is armed.
    @Published var monitorEqLowLatencyMonitoring = true
    /// True while that fallback is actually engaged, so the dock can say why.
    @Published private(set) var monitorEqLowLatencyActive = false

    func setMonitorEqLowLatencyMonitoring(_ enabled: Bool) {
        guard let handle else { return }
        nc_monitor_eq_set_low_latency_monitoring(handle, enabled)
        monitorEqLowLatencyMonitoring = enabled
        UserDefaults.standard.set(enabled, forKey: SettingsKey.monitorEqLowLatency)
        syncMonitorEqToContext()
    }
    func setMonitorEqLinearPhase(_ on: Bool) {
        guard let handle else { return }
        nc_monitor_eq_set_linear_phase(handle, on)
        monitorEqLinearPhase = nc_monitor_eq_linear_phase(handle)
        syncMonitorEqToContext()   // rebuild the current context through the chosen path
    }

    /// Reference a headphone model to the Harman OE target: off = the raw curve (with ear gain),
    /// on = the headphone's deviation from neutral. Only affects headphone-model slots.
    @Published var monitorEqHeadphoneOeTarget = false
    func setMonitorEqHeadphoneOeTarget(_ on: Bool) {
        guard let handle else { return }
        nc_monitor_eq_set_headphone_oe_target(handle, on)
        monitorEqHeadphoneOeTarget = nc_monitor_eq_headphone_oe_target(handle)
        syncMonitorEqToContext()
    }

    /// True when a speaker model has a measured response curve (so selecting it drives the EQ).
    func speakerModelHasCurve(_ model: String) -> Bool {
        virtualMonitorTargets.contains { $0 == model || stripSpeakerSlotPrefix($0) == stripSpeakerSlotPrefix(model) }
    }
    private func stripSpeakerSlotPrefix(_ name: String) -> String {
        // Stored A/B/C models read "Speaker A: <name>"; the profile keys are the bare <name>.
        if let range = name.range(of: ": ") { return String(name[range.upperBound...]) }
        return name
    }

    /// Route a slot straight to a physical output pair (no simulation), or "None" to
    /// return to the modelled path.
    func setSpeakerOutput(_ slot: Int, _ route: String) {
        guard let handle else { return }
        nc_monitor_set_speaker_output(handle, Int32(slot), route)
        reloadMonitorState()          // physical route drops the modeller → EQ re-derives
        reloadRecordControllerState()
        refreshHistory()
    }

    /// A passive modeled speaker's power amp / cable colour the simulation (heuristic tone).
    func setSpeakerAmp(_ slot: Int, _ model: String) {
        guard let handle else { return }
        _ = model.withCString { nc_monitor_set_speaker_amp(handle, Int32(slot), $0) }
        reloadMonitorState()          // re-reads slots + re-derives the EQ with the amp tone
        refreshHistory()
    }
    func setSpeakerCable(_ slot: Int, _ model: String) {
        guard let handle else { return }
        _ = model.withCString { nc_monitor_set_speaker_cable(handle, Int32(slot), $0) }
        reloadMonitorState()
        reloadRecordControllerState()
        refreshHistory()
    }

    func setSpeakerRoomEq(_ slot: Int, _ enabled: Bool) {
        guard let handle else { return }
        nc_monitor_set_speaker_room_eq(handle, Int32(slot), enabled)
        reloadMonitorState()          // room tuning on/off re-derives the EQ
        refreshHistory()
    }

    // MARK: Live MIDI input

    /// True while a keyboard is open and feeding armed instrument tracks.
    @Published private(set) var midiLiveActive = false
    /// So auto-start is attempted once per set of connected inputs, not every tick.
    private var midiLiveAutoStarted = false
    private var midiLiveOpenFailLogged = false

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
                let pick = keyboard ?? sources.first
                let chosen = pick?.id ?? ""
                let ok = chosen.withCString { nc_midi_live_start(handle, $0) }
                // Only latch once the source is REALLY open. A transient failure — the device not
                // yet enumerated at launch, or CoreMIDI unsettled after a crash — must be retried on
                // the next tick, not swallowed, or the keyboard never comes in the whole session.
                if ok && nc_midi_live_active(handle) {
                    midiLiveAutoStarted = true
                    midiLiveOpenFailLogged = false
                    Diagnostics.shared.log("live MIDI input opened: \(pick?.name ?? chosen)")
                } else if !midiLiveOpenFailLogged {
                    midiLiveOpenFailLogged = true
                    let names = sources.map { $0.name }.joined(separator: ", ")
                    Diagnostics.shared.log("live MIDI open failed (retrying) — picked \(pick?.name ?? "none") of [\(names)]")
                }
            } else if count == 0 {
                midiLiveAutoStarted = false
                midiLiveOpenFailLogged = false
            }
        }
        drainLiveMidi(handle)
        let active = nc_midi_live_active(handle)
        if active != midiLiveActive { midiLiveActive = active }
    }

    /// Creates the reverse-audio monitor ring for an opening instrument editor: the editor's
    /// own instance renders GUI keyboard clicks (and the forwarded live MIDI) and the engine
    /// mixes that audio into the monitor path. Returns the shm coordinates for the spawn.
    func instrumentEditorOpened(trackId: Int) -> (shmName: String, maxBlock: Int, sampleRate: Double)? {
        guard let handle else { return nil }
        var name = [CChar](repeating: 0, count: 128)
        var maxBlock: Int32 = 0
        var sampleRate: Double = 0
        guard nc_track_instrument_editor_opened(handle, Int32(trackId), &name, name.count,
                                                &maxBlock, &sampleRate) else { return nil }
        return (String(cString: name), Int(maxBlock), sampleRate)
    }

    /// Tears that ring down and returns the live-MIDI path to the render instance.
    func instrumentEditorClosed(trackId: Int) {
        guard let handle else { return }
        nc_track_instrument_editor_closed(handle, Int32(trackId))
    }

    // MARK: Instrument patch handoff

    /// Where an instrument editor and the DAW exchange the plug-in's own patch (its VST3
    /// component state). A file, not a pipe line: a sampler's state runs to megabytes.
    func instrumentStateFileURL(trackId: Int, slotIndex: Int) -> URL {
        URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("neuracoust-instrument-state-\(trackId)-\(slotIndex).vststate")
    }

    /// Writes the stored patch out for the editor host to restore, so reopening an editor
    /// shows the program the project holds and not the plug-in's startup default.
    /// Returns false when there is nothing stored — the host then starts on its own default.
    @discardableResult
    func exportInstrumentState(trackId: Int, slotIndex: Int, to url: URL) -> Bool {
        guard let handle else { return false }
        try? FileManager.default.removeItem(at: url)
        return nc_track_instrument_slot_write_state_file(handle, Int32(trackId), Int32(slotIndex), url.path)
    }

    /// Takes the patch the closing editor left behind into the project and rebuilds the
    /// render instance on it. Without this the picture (and the editor's own sound) said
    /// one program while the track kept playing the one it was loaded with.
    @discardableResult
    func importInstrumentState(trackId: Int, slotIndex: Int, from url: URL) -> Bool {
        guard let handle else { return false }
        defer { try? FileManager.default.removeItem(at: url) }
        let changed = nc_track_instrument_slot_read_state_file(handle, Int32(trackId), Int32(slotIndex), url.path)
        if changed {
            projectDirty = nc_project_dirty(handle)
        }
        return changed
    }

    /// Pumps pending keyboard input into the instruments and mirrors the drained batch to
    /// any open instrument editor, whose separate-process plug-in instance would otherwise
    /// never see it — this is what makes a plug-in GUI's keyboard/wheel move while playing.
    private func drainLiveMidi(_ handle: OpaquePointer) {
        var events = [NCMidiLiveEvent](repeating: NCMidiLiveEvent(), count: 128)
        let count = Int(nc_midi_pump_live_input(handle, &events, Int32(events.count)))
        guard count > 0 else { return }
        // Recording: the same batch, stamped at the current playhead, is accumulated into the
        // take — independent of the monitor path, so what you hear never affects the capture.
        if nc_midi_record_active(handle) {
            nc_midi_record_feed(handle, &events, Int32(count), playheadSeconds)
            // Redraw: the take's region grew and may have gained notes as keys were released.
            reloadMidiRegions()
        }
        // Mirror the engine's routing rule: armed / input-monitoring tracks hear the
        // keyboard, plus the selected track (the live-MIDI target). Only instrument
        // editors (insertIndex < 0) receive the stream, so audio tracks filter out there.
        let eligible = Set(tracks.filter {
            $0.recordArmed || $0.inputMonitoring || $0.id == selectedTrackId
        }.map(\.id))
        guard !eligible.isEmpty else { return }
        pluginEditors.forwardLiveMidi(trackIds: eligible, events: Array(events.prefix(count)))
    }

    // MARK: Note audition

    /// Sounds (or releases) a pitch on the track holding the region being edited, so the
    /// piano roll's keyboard plays. No transport, no recording, nothing written down.
    func previewNote(pitch: Int, velocity: Int, on: Bool) {
        guard let handle, let trackId = editingRegionTrackId else { return }
        nc_midi_preview_note(handle, Int32(trackId), Int32(pitch), Int32(velocity), on)
    }

    func previewAllNotesOff() {
        guard let handle, let trackId = editingRegionTrackId else { return }
        nc_midi_preview_all_notes_off(handle, Int32(trackId))
    }

    var selectedVirtualKeyboardTrack: Track? {
        guard let selectedTrackId else { return nil }
        return tracks.first { $0.id == selectedTrackId && $0.kind == .instrument }
    }

    func virtualKeyboardNote(pitch: Int, velocity: Int, on: Bool) {
        guard let handle, let track = selectedVirtualKeyboardTrack else { return }
        nc_midi_preview_note(handle, Int32(track.id), Int32(pitch), Int32(velocity), on)
    }

    func virtualKeyboardAllNotesOff() {
        guard let handle, let track = selectedVirtualKeyboardTrack else { return }
        nc_midi_preview_all_notes_off(handle, Int32(track.id))
    }

    /// The track index of the region currently open in the editor.
    private var editingRegionTrackId: Int? {
        guard let region = editingRegion else { return nil }
        return tracks.first(where: { $0.name == region.trackName })?.id
    }

    // MARK: Recorded controllers

    /// The controllers offered in the record menu. Not all 128: these are the ones a player
    /// actually performs, in the order a keyboard's own panel tends to list them.
    static let recordableControllers: [(number: Int, label: String)] = [
        (64, "서스테인 페달 (CC64)"),
        (1,  "모듈레이션 (CC1)"),
        (11, "익스프레션 (CC11)"),
        (2,  "브레스 (CC2)"),
        (4,  "풋 컨트롤러 (CC4)"),
        (7,  "볼륨 (CC7)"),
        (10, "팬 (CC10)"),
        (66, "소스테누토 (CC66)"),
        (67, "소프트 페달 (CC67)"),
        (71, "레조넌스 (CC71)"),
        (74, "브라이트니스 (CC74)"),
    ]

    /// Republished so the menu redraws when a toggle changes; the project holds the truth.
    @Published private(set) var recordControllerRevision = 0
    @Published private(set) var recordPitchBendEnabled = true

    func recordControllerEnabled(_ controller: Int) -> Bool {
        guard let handle else { return false }
        _ = recordControllerRevision   // read so SwiftUI tracks this menu against the toggles
        return nc_midi_record_controller_enabled(handle, Int32(controller))
    }

    func setRecordControllerEnabled(_ controller: Int, _ enabled: Bool) {
        guard let handle else { return }
        nc_midi_record_set_controller_enabled(handle, Int32(controller), enabled)
        recordControllerRevision &+= 1
        projectDirty = nc_project_dirty(handle)
    }

    func setRecordPitchBendEnabled(_ enabled: Bool) {
        guard let handle else { return }
        nc_midi_record_set_pitch_bend_enabled(handle, enabled)
        recordPitchBendEnabled = nc_midi_record_pitch_bend_enabled(handle)
        projectDirty = nc_project_dirty(handle)
    }

    private func reloadRecordControllerState() {
        guard let handle else { return }
        recordPitchBendEnabled = nc_midi_record_pitch_bend_enabled(handle)
        recordControllerRevision &+= 1
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

    /// Assign a @Published property only when the value actually changes. Every unconditional
    /// assignment in the 30 Hz poll fires objectWillChange, which re-renders every view
    /// observing the controller — including open menus, whose checkmarks then visibly flicker.
    /// Writing only real changes means an idle engine publishes nothing and menus stay put.
    private func setIfChanged<T: Equatable>(_ keyPath: ReferenceWritableKeyPath<EngineController, T>, _ value: T) {
        if self[keyPath: keyPath] != value { self[keyPath: keyPath] = value }
    }

    private func tick() {
        guard let handle else { return }

        // While a menu (context menu, `Menu`, or the menu bar) is tracking, skip the whole
        // telemetry publish. Every setIfChanged below fires objectWillChange, re-rendering the
        // engine-observing views — and that re-render tears down the open NSMenu, so submenus
        // flicker and won't expand. Meters resume the instant the menu closes; live MIDI is
        // unaffected (its own 240 Hz timer drains it).
        if menuTrackingActive { return }

        var status = NCEngineStatus()
        nc_engine_status(handle, &status)
        if Int(status.outputChannels) != outputChannelCount {
            reloadSpeakerOutputRoutes(Int(status.outputChannels))
        }

        // The rest of the timing telemetry (jitter, render duration, DSP load) drifts a hair
        // every audio callback, so publishing it each poll re-laid-out the heavy MonitorDock
        // ~30×/s even at idle. Round it and refresh at ~6 Hz; a readout that ticks 6×/s is plenty.
        telemetrySlowCounter += 1
        if telemetrySlowCounter >= 5 {
            telemetrySlowCounter = 0
            setIfChanged(\.wakeJitterUs, status.realtimeAverageWakeJitterUs.rounded())
            setIfChanged(\.referenceTapFaults,
                         Int(status.referenceUnderrunBlocks) + Int(status.referenceOverrunDrops))
            setIfChanged(\.maxRenderDurationUs, status.realtimeMaxRenderDurationUs.rounded())
            setIfChanged(\.lateWakeCount, Int(status.realtimeLateWakeCount))
            // Engine restart resets the raw count to 0 — drop the stale baseline so new misses show.
            if lateWakeBaseline > lateWakeCount { lateWakeBaseline = lateWakeCount }
            setIfChanged(\.remoteDspRoundTripMs, (status.remoteDspRoundTripMs * 100).rounded() / 100)
        }
        setIfChanged(\.delayCompensationSamples, Int(nc_delay_compensation_samples(handle)))
        setIfChanged(\.remoteDspActive, status.remoteDspMonitorActive)
        setIfChanged(\.activeInsertCount, Int(status.activeRealtimeVst3TrackInserts)
            + Int(status.activeRealtimeVst3MasterInserts)
            + Int(status.activeRemoteDspTrackInserts))

        setIfChanged(\.running, status.running)
        let wasTransportRunning = transportRunning
        setIfChanged(\.transportRunning, status.transportRunning)
        if wasTransportRunning && !transportRunning { finishAutomationPass() }
        // Ballistic meters: snap up to a new peak, decay down. Without the decay a held
        // engine peak stays lit after stop; with it the meter always falls to silence.
        // Visual telemetry (meters, spectrum, goniometer, LUFS) at ~15 Hz, not 30. When a
        // signal is present these move every audio poll, and each publish re-renders the whole
        // engine-observing view tree — the dominant idle/playback CPU cost. Halving the rate
        // halves that cost and is imperceptible on a meter; the transport, playhead and
        // automation below still run every tick.
        visualTelemetryTick = visualTelemetryTick &+ 1
        if visualTelemetryTick & 1 == 0 {
            // Phase correlation + the 3-band spectrum are meters too — publish them here (~15 Hz) rather
            // than every tick, so they don't re-render the engine-observing UI 30×/s during playback.
            setIfChanged(\.phaseCorrelation, (status.phaseCorrelation * 100).rounded() / 100)
            setIfChanged(\.spectrumLow, status.spectrumLow)
            setIfChanged(\.spectrumMid, status.spectrumMid)
            setIfChanged(\.spectrumHigh, status.spectrumHigh)
            // Ballistic meters: snap up to a new peak, decay down (and floor to exact silence).
            setIfChanged(\.outputPeakLeft, Self.decayedMeter(status.outputPeakLeft, outputPeakLeft))
            setIfChanged(\.outputPeakRight, Self.decayedMeter(status.outputPeakRight, outputPeakRight))
            updateSpectrumBins(handle)
            updateGoniometer(handle)
            setIfChanged(\.momentaryLufs, status.momentaryLufs)
            setIfChanged(\.shortTermLufs, status.shortTermLufs)
            setIfChanged(\.integratedLufs, status.integratedLufs)
            setIfChanged(\.loudnessRange, status.loudnessRange)
            setIfChanged(\.truePeakDb, status.truePeakDb)
            setIfChanged(\.inputPeak, Self.decayedMeter(status.inputPeak, inputPeak))
            applyTrackMeters(status)
        }
        setIfChanged(\.sampleRate, status.sampleRate)
        setIfChanged(\.bufferSize, Int(status.requestedBufferSize))
        setIfChanged(\.delayCompensationMs, status.delayCompensationMs)
        setIfChanged(\.deviceName, withUnsafePointer(to: status.deviceName) {
            $0.withMemoryRebound(to: CChar.self, capacity: Int(NC_TEXT_LEN)) { String(cString: $0) }
        })

        updatePlayhead(engineSeconds: status.playbackSeconds)
        if pitchEditorClipId != nil && pitchEditorTimelineSync {
            pitchEditorClock.seconds = min(pitchEditorClipDuration,
                                           max(0, playheadSeconds - pitchEditorClipStartSeconds))
        }
        serviceCountIn(handle)
        serviceAudioRecordingWaveform(handle)
        // Drive any plugin-parameter automation lanes to the playhead, so a drawn plug-in
        // curve is heard live. (Volume/pan are baked by the renderer; this covers inserts.)
        if transportRunning { nc_apply_plugin_automation(handle, playheadSeconds) }
        serviceAutomation()

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
        serviceHui(handle)
        // The pump bumps the activity; read (which resets it) and decay for the meter.
        setIfChanged(\.midiActivity, max(nc_midi_input_activity(handle), midiActivity - 0.07))

        // Pick up hot-plugged interfaces (a UNiTE-2 connected mid-session) without a restart:
        // rescan the device lists a couple of times a second. refreshOutputDevices republishes
        // only when the list actually changed, so this stays flicker-free.
        // Rescan for hot-plugged interfaces a few seconds apart — enumerateAudioDevices can be
        // slow for virtual/network devices (SoundGrid, Splashtop), so keep the cadence gentle.
        // NOTE: no periodic device rescan here. enumerateAudioDevices() walks every CoreAudio
        // device synchronously on the main thread, and slow virtual/network devices (SoundGrid,
        // Splashtop) can block long enough to stall the 30 Hz poll during playback — a few-second
        // hitch/restart loop and a main-thread CPU storm. Devices are rescanned when a device
        // menu / the dock appears instead; a proper CoreAudio change-listener is the way to add
        // hot-plug detection without polling.

        pollMeasurement()
        if measurementLevelCheckActive || measuringInterface {
            measurementInputLevel = nc_measure_input_level(handle)
        }
        listenRoom?.refresh()
    }
    private var deviceRescanTicks = 0
    private var telemetrySlowCounter = 0
    private var visualTelemetryTick: UInt = 0

    private func updatePlayhead(engineSeconds: Double) {
        guard transportRunning else {
            // Guarded so a stopped transport doesn't republish the playhead every tick
            // (which flickers open menus). While playing it genuinely moves each tick.
            setPlayhead(engineSeconds)
            transportWallClockBase = engineSeconds
            transportWallClockStart = CACurrentMediaTime()
            return
        }

        let elapsed = CACurrentMediaTime() - transportWallClockStart
        let predicted = transportWallClockBase + elapsed

        // The playhead now lives on `playheadClock` (not the engine object), so this 30 Hz update
        // re-renders only the timeline / transport / piano roll — the heavy dock/mixer no longer
        // re-lay-out. `setPlayhead` only touches the clock when the value actually changes, so a
        // wedged transport still costs nothing.
        if abs(predicted - engineSeconds) > resyncThreshold {
            setPlayhead(engineSeconds)
            transportWallClockBase = engineSeconds
            transportWallClockStart = CACurrentMediaTime()
        } else {
            setPlayhead(predicted)
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
