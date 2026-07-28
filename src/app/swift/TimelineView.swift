import AppKit
import Combine
import SwiftUI

/// The timebases the ruler can show. Any subset may be visible at once.
enum RulerTimebase: Int, CaseIterable {
    case bars, time, samples
    var label: String {
        switch self {
        case .bars: return "마디"
        case .time: return "시간"
        case .samples: return "샘플"
        }
    }
}

/// What the timeline needs to draw one frame. A value type so the AppKit view can
/// diff it cheaply and skip redraws.
struct TimelineModel: Equatable {
    /// One automatable parameter, and the points the renderer will read.
    struct Automation: Equatable {
        let parameterId: String
        let displayName: String
        /// The value axis, top to bottom of the sub-lane.
        let range: ClosedRange<Float>
        /// Where the fader/knob sits, which is what the curve falls back to.
        let fallback: Float
        let points: [Point]

        struct Point: Equatable {
            let timeSeconds: Double
            let value: Float
        }
    }

    struct Lane: Equatable {
        let name: String
        let accent: NSColor
        let muted: Bool
        let selected: Bool
        /// Inline channel strip in the lane header, so editing needs no mixer trip.
        var trackId: Int = -1
        var soloed: Bool = false
        var armed: Bool = false
        var inputMonitor: Bool = false
        /// True when another track is soloed and this one is silenced by it — its Mute
        /// button blinks, the Pro-Tools tell that it is held down by the solo.
        var soloSilencedBlink: Bool = false
        var volumeDb: Float = 0
        var pan: Float = 0
        /// No meter levels here on purpose. TimelineModel is rebuilt from scratch whenever it is
        /// read, so a peak on a lane meant every lane, clip, automation curve and chip in the
        /// whole model was reconstructed ~15x a second. The view reads levels straight from the
        /// meter store when it draws instead — see `trackMeters`.
        /// Read / Touch / Latch / Write / Off — drawn as a coloured letter the user cycles.
        var automationMode: String = "read"
        /// nil while the lane's automation is folded away.
        var automation: Automation?
        /// The channel-strip inserts and sends, shown in the header like Pro Tools' edit
        /// window. Inserts are the first few slots; sends are the active ones.
        var inserts: [InsertChip] = []
        var sends: [SendChip] = []
        /// This lane's height. Per-track so a multi-selection can be resized together.
        var height: CGFloat = 100
    }

    struct InsertChip: Equatable { let name: String; let bypassed: Bool; let isEmpty: Bool }
    struct SendChip: Equatable { let label: String; var preFader: Bool = false }

    struct Clip: Equatable {
        let id: String
        let name: String
        let laneIndex: Int
        let startSeconds: Double
        let durationSeconds: Double
        let sourcePath: String
        let sourceOffsetSeconds: Double
        let selected: Bool
        let fadeInSeconds: Double
        let fadeOutSeconds: Double
        var fadeInCurve: String = "equal_power"
        var fadeOutCurve: String = "equal_power"
        var fadeInCurvature: Double = 0.0
        var fadeOutCurvature: Double = 0.0
        let gainDb: Float
        // Non-destructive processing state, reflected in the waveform: dimmed when muted, drawn
        // back-to-front when reversed, flipped vertically when polarity-inverted.
        var muted: Bool = false
        var reversed: Bool = false
        var polarityInverted: Bool = false
    }

    var lanes: [Lane] = []
    var clips: [Clip] = []
    var tempoBpm: Int = 120
    var beatsPerBar: Int = 4
    var sampleRate: Double = 48000
    /// Shared, adjustable lane height (drag a lane's bottom edge to change it).
    var laneHeight: CGFloat = 100
    /// Which timebases the ruler shows, top to bottom. Any subset may be on at once.
    var rulerBars: Bool = true
    var rulerTime: Bool = true
    var rulerSamples: Bool = false

    /// Seconds at the left edge, and seconds across the visible width.
    var visibleStart: Double = 0
    var visibleDuration: Double = 30

    struct Marker: Equatable {
        let name: String
        let timeSeconds: Double
    }

    /// A MIDI part on an instrument lane. Notes are drawn as a folded-down sketch.
    struct MidiRegion: Equatable {
        let id: String
        let name: String
        let laneIndex: Int
        let startSeconds: Double
        let durationSeconds: Double
        let muted: Bool
        let editing: Bool
        let selected: Bool
        /// (start seconds, duration seconds, pitch) — already in timeline time.
        let noteSketch: [Sketch]

        struct Sketch: Equatable {
            let startSeconds: Double
            let durationSeconds: Double
            let pitch: Int
        }
    }

    var markers: [Marker] = []
    var midiRegions: [MidiRegion] = []

    /// The loop range, which is also the range every range edit acts on.
    var rangeStart: Double = 0
    var rangeEnd: Double = 0
    var loopEnabled: Bool = false
    /// If the range was made by dragging a specific lane (Pro Tools selector), the lane it belongs
    /// to — so the edit-range band highlights that track. nil = a ruler range spanning all lanes.
    var editRangeLane: Int? = nil
}

/// Timeline surface: ruler, grid, lanes, clips with waveforms, playhead.
///
/// This is an NSView rather than SwiftUI because it repaints a waveform envelope
/// per clip and moves a playhead at 30 Hz. The playhead lives in its own layer so
/// moving it never triggers a redraw of the waveforms.
final class TimelineNSView: NSView, NSTextFieldDelegate {
    var model = TimelineModel() {
        didSet {
            guard model != oldValue else { return }
            needsDisplay = true
            window?.invalidateCursorRects(for: self)
        }
    }

    /// Seconds; drives only the playhead layer.
    var playheadSeconds: Double = 0 {
        didSet { layoutPlayhead() }
    }

    /// Subscribe the playhead layer straight to the 30 Hz clock, bypassing SwiftUI entirely: the sink
    /// fires on the main thread and moves ONLY the CALayer. Routing this through the representable's
    /// update path instead put a needsDisplay/layout invalidation inside SwiftUI's layout pass, which
    /// fed a full-speed hosting-view re-layout loop (~100 % CPU at idle).
    private var playheadSink: AnyCancellable?
    func bindPlayheadClock(_ clock: PlayheadClock) {
        guard playheadSink == nil else { return }
        playheadSeconds = clock.seconds
        playheadSink = clock.$seconds.sink { [weak self] s in self?.playheadSeconds = s }
    }

    /// Lane meter levels, bound straight to the store with Combine for exactly the reason the
    /// playhead above is: a 15 Hz value routed through the representable's update path invalidates
    /// inside SwiftUI's layout pass and drags the whole hosting view into a re-layout loop. Only
    /// the header column is marked dirty, so a moving meter never redraws the lanes.
    private(set) var trackMeterLevels: [String: EngineController.TrackMeters.Level] = [:]
    private var trackMeterSink: AnyCancellable?
    func bindTrackMeters(_ meters: EngineController.TrackMeters) {
        guard trackMeterSink == nil else { return }
        trackMeterLevels = meters.levels
        trackMeterSink = meters.$levels.sink { [weak self] levels in
            guard let self else { return }
            self.trackMeterLevels = levels
            self.setNeedsDisplay(NSRect(x: 0, y: 0, width: Self.headerWidth, height: self.bounds.height))
        }
    }

    /// Play or record in progress — a click on empty lane space must NOT relocate the playhead
    /// (edit freely while it rolls, the Pro Tools way). Ruler clicks still scrub deliberately.
    var isTransportRunning = false

    /// Peaks by source path, supplied by the owner. Decoding lives in the engine.
    var waveforms: [String: EngineController.WaveformData] = [:] {
        didSet { needsDisplay = true }
    }

    /// Live audio-record clips: one growing/frozen red clip per punch region on the armed lane.
    /// Guarded — updateNSView reassigns this on every SwiftUI pass, and an unconditional repaint
    /// there becomes a needsDisplay inside the layout pass (feedback-loop fuel).
    var recordingClips: [EngineController.RecordingClip] = [] {
        didSet { if recordingClips != oldValue { needsDisplay = true } }
    }
    var recordingChannels: Int = 2

    var onSeek: ((Double) -> Void)?
    var onToggleTimebase: ((RulerTimebase) -> Void)?
    // Lane-header inserts / sends (Pro Tools edit-window channel strip).
    var onBrowseInsert: ((Int) -> Void)?              // trackId — open the plugin browser
    var onToggleInsertEditor: ((Int, Int) -> Void)?   // trackId, slot
    var onBypassInsert: ((Int, Int) -> Void)?
    var onRemoveInsert: ((Int, Int) -> Void)?
    var onAddSend: ((Int, String) -> Void)?           // trackId, bus
    var onRemoveSend: ((Int, Int) -> Void)?
    var onSetSendGain: ((Int, Int, Float) -> Void)?
    var onSetSendPan: ((Int, Int, Float) -> Void)?
    var onSetSendPreFader: ((Int, Int, Bool) -> Void)?
    var onAddAux: (() -> Void)?
    var onSendBusOptions: ((Int) -> [String])?
    var onZoom: ((Double, Double) -> Void)?   // (visibleStart, visibleDuration)
    var onSelect: ((String?) -> Void)?
    var onSetRange: ((Double, Double) -> Void)?          // (start, end)
    var onSetRangeLane: ((Int?) -> Void)?                // which lane the range belongs to (nil = all)
    var onSelectRegion: ((String?) -> Void)?
    var onOpenRegion: ((String) -> Void)?
    /// Cubase Glue: merge this MIDI region with the next one on its track.
    var onMergeRegionForward: ((String) -> Void)?
    /// Merge every MIDI region on this region's track into one part.
    var onMergeRegionsOnTrack: ((String) -> Void)?
    var onMoveRegion: ((String, Int, Double) -> Void)?   // (id, lane, start) — continuous
    var onResizeRegion: ((String, Double) -> Void)?      // (id, duration) — continuous
    var onAddRegion: ((Int, Double) -> Void)?            // (lane, start)
    var onDropAudio: ((Int, Double, [URL]) -> Void)?     // (lane, start, file urls)
    var onDropMidi: ((Int, Double, [URL]) -> Void)?      // (lane, start, .mid urls) → MIDI regions
    var onMoveMarker: ((Double, Double) -> Void)?        // (from, to) — continuous
    var onDeleteMarker: ((Double) -> Void)?
    var onSelectBetweenMarkers: ((Double) -> Void)?
    var onToggleAutomation: ((Int) -> Void)?             // lane index
    var onCycleAutomationParameter: ((Int) -> Void)?     // lane index
    var onAutomationParamOptions: ((Int) -> [(id: String, name: String, on: Bool)])?
    var onSetAutomationParam: ((Int, String) -> Void)?
    var onSetLaneHeight: (([Int], CGFloat) -> Void)?
    var onCommitLaneHeight: (() -> Void)?
    var onReorderTrack: ((Int, Int, Bool) -> Void)?   // (sourceTrackId, targetTrackId, after)
    var onFadeCurveOptions: (() -> [(label: String, id: String)])?
    var onClipCurrentFades: ((String) -> (inCurve: String, outCurve: String))?
    var onSetClipFadeInCurve: ((String, String) -> Void)?
    var onSetClipFadeOutCurve: ((String, String) -> Void)?
    // Non-destructive clip processing (Logic/Cubase style), all keyed by clip id.
    var onReverseClip: ((String) -> Void)?
    var onNormalizeClip: ((String) -> Void)?
    var onToggleClipMute: ((String) -> Void)?
    var onToggleClipPolarity: ((String) -> Void)?
    var onApplyClipTimePitch: ((String, Double, Double) -> Void)?   // (clipId, timeRatio, semitones)
    var onDenoiseClip: ((String) -> Void)?   // neural noise-floor removal, repoints the clip
    /// (clipId, plug-in name, plug-in path) — opens the plug-in's own ARA editor over the clip.
    var onOpenAraEditor: ((String, String, String) -> Void)?
    var onClearAraEdits: ((String) -> Void)?
    /// The installed ARA plug-ins, and whether this clip already carries committed edits.
    var araPlugins: [(name: String, path: String)] = []
    var clipHasAraEdits: ((String) -> Bool)?
    var onAlignToReference: ((String, String) -> Void)?   // (dubId, refId) VocAlign time-align onto a lead
    var alignStrength: Double = 1.0   // current VocAlign amount, for the strength submenu checkmarks
    var onSetAlignStrength: ((Double) -> Void)?
    var onSeparateStems: ((String) -> Void)?   // Demucs 4-stem separation → new tracks (legacy default)
    var onSeparateStemsPreset: ((String, String) -> Void)?   // (clipId, preset): auto/karaoke/vocals/…/6s
    var stem6sAvailable: Bool = false   // show the 6-part option only when its model is bundled
    var drumSplitAvailable: Bool = false      // show drum-split (kick/snare/toms/cymbals) when bundled
    var orchestraSeparationAvailable: Bool = false   // show experimental orchestra-family split when bundled
    var onConvertToMidi: ((String) -> Void)?   // audio → MIDI (monophonic) on a new instrument track
    var onConvertToMidiPoly: ((String) -> Void)?   // polyphonic (basic-pitch) audio → MIDI
    var convertToMidiPolyAvailable: Bool = false   // show the polyphonic option only when its helper is bundled
    var onOpenPitchEditor: ((String) -> Void)?   // Melodyne / Serato anchor pitch editor
    var onSetCrossfadeLength: ((String, String, Double) -> Void)?  // (leftId, rightId, seconds)
    var onSetFadeCurvature: ((String, Bool, Double) -> Void)?      // (clipId, isFadeIn, curvature)
    var auditionRoll: (() -> Double)?
    var onSetAuditionRoll: ((Double) -> Void)?
    var onAuditionRegion: ((Double, Double, Bool) -> Void)?       // (startSeconds, endSeconds, loop)
    var onStopAudition: (() -> Void)?
    var onClipOriginalStart: ((String) -> Double)?       // 스팟: 원래 위치 (-1 = 미기록)
    var onSpotClips: (([String]) -> Void)?               // 스팟: 각 클립을 자기 원래 위치로
    var onAddAutomationPoint: ((Int, Double, Float) -> Void)?    // (lane, time, value)
    var onMoveAutomationPoint: ((Int, Int, Double, Float) -> Void)?  // (lane, point, time, value)
    var onDeleteAutomationPoint: ((Int, Int) -> Void)?   // (lane, point)
    var onToggleSelect: ((String) -> Void)?              // shift-click
    var onSelectMany: (([String]) -> Void)?              // marquee
    var onMoveClip: ((String, Double) -> Void)?          // (clipId, newStart)
    var onSplitClip: ((String, Double) -> Void)?         // 분할 tool: (clipId, seconds)
    /// The active edit tool as a raw string ("smart"/"grabber"/…); forces a behaviour.
    var editTool: String = "smart" {
        didSet {
            guard editTool != oldValue else { return }
            window?.invalidateCursorRects(for: self)
        }
    }
    /// Option-drag copy, committed on release: (originalClipId, targetLaneIndex, start).
    /// The copy is created only here, at the drop, so nothing sums in place during drag.
    var onDropCopy: ((String, Int, Double) -> Void)?     // (clipId, laneIndex, startSeconds)
    var onDropCopyToNewTrack: ((String, Double) -> Void)?  // drop past last lane → new track
    var onMoveSelection: ((Double) -> Void)?             // delta seconds
    // Duplicate the whole clip selection in place and select the copies; returns the copy of the
    // passed anchor clip (or nil on failure) so the drag can continue moving the copies.
    var onBeginCopySelection: ((String) -> String?)?
    var onTrimStart: ((String, Double) -> Void)?         // (clipId, newStart)
    var onTrimEnd: ((String, Double) -> Void)?           // (clipId, newEnd)
    var onRollBoundary: ((String, String, Double) -> Void)?  // (leftId, rightId, newBoundary)
    var onSetFades: ((String, Double, Double) -> Void)?  // (clipId, fadeIn, fadeOut)
    var onSetGain: ((String, Float) -> Void)?
    var onCommitGain: ((String) -> Void)?                // drag-end: reconcile + record
    var onSelectLane: ((Int, Bool) -> Void)?     // (laneIndex, additive/⇧)
    var onMoveClipToLane: ((String, Int, Double) -> Void)?  // (clipId, laneIndex, start)
    var onDropClipToNewTrack: ((String, Double) -> Void)?   // drop past last lane → new track
    var onCommitEdit: ((String) -> Void)?                // step name
    var snap: ((Double) -> Double)?

    // Inline lane-header channel strip: no mixer trip to mute/solo/arm or set level.
    var onToggleMute: ((Int) -> Void)?                   // trackId
    var onToggleSolo: ((Int) -> Void)?                   // trackId
    var soloSelectMode: String = "additive"
    var onSetSoloSelectMode: ((String) -> Void)?
    var onClearAllSolos: (() -> Void)?
    var onToggleArm: ((Int) -> Void)?                    // trackId
    var onToggleInputMonitor: ((Int) -> Void)?           // trackId
    var onRenameTrack: ((Int, String) -> Void)?          // (trackId, newName)
    var onSetVolumeDb: ((Int, Float) -> Void)?           // (trackId, db) — continuous
    var onSetPan: ((Int, Float) -> Void)?                // (trackId, pan −1…1) — continuous
    var onCycleAutomationMode: ((Int) -> Void)?          // trackId — Read→Touch→Latch→Write→Off
    /// A header fader/pan was grabbed / released, so Touch/Latch automation can punch in.
    var onBeginTouch: ((Int, String) -> Void)?           // (trackId, "track.volume"/"track.pan")
    var onEndTouch: ((Int, String) -> Void)?             // (trackId, param)

    /// Grab zone at each end of a clip.
    private static let trimHandleWidth: CGFloat = 8

    /// How far the cursor must travel horizontally before a clip drag stops being
    /// pinned to its start time — so a straight up/down lane move keeps its position.
    private static let axisLockThreshold: CGFloat = 9

    private enum Drag {
        case none
        case seeking
        /// Dragging the vertical / horizontal scrollbar thumb. `grab` is where inside the thumb the
        /// press landed, so the thumb does not jump under the cursor.
        case scrollingLanes(grab: CGFloat)
        case scrollingTime(grab: CGFloat)
        case marquee(origin: NSPoint, current: NSPoint)
        case rangingFrom(seconds: Double)
        /// Pro Tools selector: dragging in a lane's empty space makes a time-range edit selection
        /// anchored to that lane (so it can be edited during playback without touching the playhead).
        case rangingInLane(origin: Double, lane: Int)
        /// Dragging one edge of an existing range by its ruler handle, or sliding the
        /// whole range by grabbing its middle.
        case rangingEdgeStart
        case rangingEdgeEnd
        case movingRange(grabOffsetSeconds: Double)
        case movingAutomationPoint(laneIndex: Int, pointIndex: Int)
        case movingMarker(fromSeconds: Double)
        case movingRegion(id: String, grabOffsetSeconds: Double)
        case resizingRegion(id: String)
        /// startX is where the drag began; the clip's horizontal position stays put
        /// until the cursor leaves the axis-lock dead zone, so a vertical (lane) move
        /// does not smear the clip sideways. axisUnlocked latches once it crosses.
        case moving(clipId: String, grabOffsetSeconds: Double, startX: CGFloat, axisUnlocked: Bool)
        /// Option-drag copy. The original is left untouched and a ghost follows the
        /// cursor; the copy is created only on release (and only if actually moved), so
        /// an option-click never leaves a doubled clip summing in place.
        case copyMoving(clipId: String, grabOffsetSeconds: Double, startX: CGFloat, axisUnlocked: Bool)
        /// Dragging one clip of a multi-selection drags all of them. The anchor's
        /// live start is read back from the model each frame, so a clamp at zero
        /// simply stops the whole selection instead of drifting it apart.
        case movingSelection(anchorId: String, grabOffsetSeconds: Double, startX: CGFloat, axisUnlocked: Bool)
        /// Option-drag of a multi-selection = copy. Once the drag crosses the axis-lock threshold the
        /// whole selection is duplicated in place and the gesture continues as a movingSelection on the
        /// copies, so the originals stay put. A click that never crosses the threshold copies nothing.
        case copyMovingSelection(anchorId: String, grabOffsetSeconds: Double, startX: CGFloat)
        case trimmingStart(clipId: String)
        case trimmingEnd(clipId: String)
        /// Rolling the shared boundary of two exactly-abutting clips: trims the left clip's end and
        /// the right clip's start together, so the junction slides without opening a gap (Pro Tools).
        case rollingBoundary(leftId: String, rightId: String)
        case fadingIn(clip: TimelineModel.Clip)
        case fadingOut(clip: TimelineModel.Clip)
        /// Dragging the inline volume fader / pan bar in a lane header.
        case headerFader(trackId: Int)
        case headerPan(trackId: Int)
        case gaining(clip: TimelineModel.Clip, grabY: CGFloat, startGainDb: Float)
        /// Dragging a lane's bottom edge to resize all lanes.
        case resizingLane(startHeight: CGFloat, startY: CGFloat, laneIndex: Int)
        /// Dragging a lane header up/down to reorder the track (mixer follows).
        case reorderingLane(trackId: Int, laneIndex: Int, startY: CGFloat, currentY: CGFloat)
    }

    /// Clip gain is drawn as a horizontal line across this dB span.
    private static let gainRange: ClosedRange<Float> = -24...24
    private static let fadeHandleSize: CGFloat = 9
    /// How close (screen px) two same-lane clip edges must be to count as "abutting" for a roll edit.
    private static let boundaryRollTolerance: CGFloat = 2.5

    /// The same-lane clip whose edge exactly meets `clip`'s left (onLeft) or right edge, if any —
    /// the partner for a roll edit. Overlapping clips (a crossfade) are NOT abutting and return nil.
    private func abuttingNeighbor(of clip: TimelineModel.Clip, onLeft: Bool) -> TimelineModel.Clip? {
        let edgeX = onLeft ? x(forSeconds: clip.startSeconds)
                           : x(forSeconds: clip.startSeconds + clip.durationSeconds)
        return model.clips.first { other in
            guard other.id != clip.id, other.laneIndex == clip.laneIndex else { return false }
            let otherEdgeX = onLeft ? x(forSeconds: other.startSeconds + other.durationSeconds)
                                    : x(forSeconds: other.startSeconds)
            return abs(otherEdgeX - edgeX) <= Self.boundaryRollTolerance
        }
    }

    private var drag = Drag.none
    /// Held so the fade/crossfade editor popover survives while shown.
    private var fadeEditorPopover: NSPopover?

    /// Double-click on the LOWER half of a fade or crossfade region → open the editor popover.
    /// Returns true when it opened one (crossfade takes priority over a plain fade at the same x).
    private func presentFadeCrossfadeEditor(at point: NSPoint, hit: TimelineModel.Clip) -> Bool {
        let rect = clipRect(hit)
        guard point.y > rect.midY else { return false }   // lower half only
        let t = seconds(atX: point.x)

        // Crossfade: does `hit` overlap a same-lane neighbour, and is the click inside that overlap?
        for other in model.clips where other.laneIndex == hit.laneIndex && other.id != hit.id {
            let a = hit.startSeconds <= other.startSeconds ? hit : other      // earlier
            let b = hit.startSeconds <= other.startSeconds ? other : hit      // later
            let aEnd = a.startSeconds + a.durationSeconds
            let overlapEnd = min(aEnd, b.startSeconds + b.durationSeconds)
            guard aEnd > b.startSeconds + 1e-6 else { continue }
            if t >= b.startSeconds - 0.02 && t <= overlapEnd + 0.02 {
                let overlap = aEnd - b.startSeconds
                showEditorPopover(FadeCrossfadeEditorConfig(
                    target: .crossfade(leftId: a.id, rightId: b.id),
                    initialOutCurve: a.fadeOutCurve, initialInCurve: b.fadeInCurve,
                    initialSeconds: overlap,
                    maxSeconds: min(a.durationSeconds, b.durationSeconds),
                    initialCurvature: a.fadeOutCurvature,
                    setOutCurve: { [weak self] in self?.onSetClipFadeOutCurve?(a.id, $0) },
                    setInCurve: { [weak self] in self?.onSetClipFadeInCurve?(b.id, $0) },
                    setLength: { [weak self] in self?.onSetCrossfadeLength?(a.id, b.id, $0) },
                    setCurvature: { [weak self] c in
                        self?.onSetFadeCurvature?(a.id, false, c)   // front clip's fade-out
                        self?.onSetFadeCurvature?(b.id, true, c)    // back clip's fade-in
                    },
                    initialRoll: auditionRoll?() ?? 1.5,
                    setRoll: { [weak self] in self?.onSetAuditionRoll?($0) },
                    audition: { [weak self] loop in self?.onAuditionRegion?(b.startSeconds, overlapEnd, loop) },
                    stopAudition: { [weak self] in self?.onStopAudition?() },
                    remove: { [weak self] in self?.onSetCrossfadeLength?(a.id, b.id, 0) },
                    close: { [weak self] in self?.dismissFadeEditor() }),
                    anchor: rect, atX: point.x)
                return true
            }
        }

        // Fade-in / fade-out on the hit clip itself.
        let clipEnd = hit.startSeconds + hit.durationSeconds
        if hit.fadeInSeconds > 0.001, t >= hit.startSeconds - 0.02, t <= hit.startSeconds + hit.fadeInSeconds + 0.02 {
            showEditorPopover(fadeConfig(hit, edge: .fadeIn), anchor: rect, atX: point.x); return true
        }
        if hit.fadeOutSeconds > 0.001, t >= clipEnd - hit.fadeOutSeconds - 0.02, t <= clipEnd + 0.02 {
            showEditorPopover(fadeConfig(hit, edge: .fadeOut), anchor: rect, atX: point.x); return true
        }
        return false
    }

    private func fadeConfig(_ clip: TimelineModel.Clip, edge: FadeEdge) -> FadeCrossfadeEditorConfig {
        let isIn = edge == .fadeIn
        return FadeCrossfadeEditorConfig(
            target: .fade(clipId: clip.id, edge: edge),
            initialOutCurve: isIn ? clip.fadeInCurve : clip.fadeOutCurve, initialInCurve: "",
            initialSeconds: isIn ? clip.fadeInSeconds : clip.fadeOutSeconds,
            maxSeconds: clip.durationSeconds,
            initialCurvature: isIn ? clip.fadeInCurvature : clip.fadeOutCurvature,
            setOutCurve: { [weak self] in
                isIn ? self?.onSetClipFadeInCurve?(clip.id, $0) : self?.onSetClipFadeOutCurve?(clip.id, $0)
            },
            setInCurve: { _ in },
            setLength: { [weak self] in
                isIn ? self?.onSetFades?(clip.id, $0, clip.fadeOutSeconds)
                     : self?.onSetFades?(clip.id, clip.fadeInSeconds, $0)
            },
            setCurvature: { [weak self] c in self?.onSetFadeCurvature?(clip.id, isIn, c) },
            initialRoll: auditionRoll?() ?? 1.5,
            setRoll: { [weak self] in self?.onSetAuditionRoll?($0) },
            audition: { [weak self] loop in
                let end = clip.startSeconds + clip.durationSeconds
                isIn ? self?.onAuditionRegion?(clip.startSeconds, clip.startSeconds + clip.fadeInSeconds, loop)
                     : self?.onAuditionRegion?(end - clip.fadeOutSeconds, end, loop)
            },
            stopAudition: { [weak self] in self?.onStopAudition?() },
            remove: { [weak self] in
                isIn ? self?.onSetFades?(clip.id, 0, clip.fadeOutSeconds)
                     : self?.onSetFades?(clip.id, clip.fadeInSeconds, 0)
            },
            close: { [weak self] in self?.dismissFadeEditor() })
    }

    private func showEditorPopover(_ config: FadeCrossfadeEditorConfig, anchor rect: NSRect, atX x: CGFloat) {
        dismissFadeEditor()
        let pop = NSPopover()
        pop.behavior = .transient
        let host = NSHostingController(rootView: FadeCrossfadeEditorView(config: config))
        host.sizingOptions = [.preferredContentSize]
        pop.contentViewController = host
        fadeEditorPopover = pop
        let anchorRect = NSRect(x: x - 3, y: rect.minY, width: 6, height: rect.height)
        pop.show(relativeTo: anchorRect, of: self, preferredEdge: .maxY)
    }

    private func dismissFadeEditor() {
        onStopAudition?()   // stop a looping audition when the editor closes
        fadeEditorPopover?.close()
        fadeEditorPopover = nil
    }

    /// The live cursor point during a clip move, so a ghost can follow it into the target
    /// lane — the visual feedback that the clip is being carried.
    private var dragCursor: NSPoint?

    /// The lanes scroll under a fixed ruler. An NSScrollView would carry the ruler
    /// away with them, so the view owns the offset itself.
    private var scrollY: CGFloat = 0

    /// The lane a drag is hovering, so it can be lit while a file is over it.
    private var dropLaneIndex: Int?
    private var dropSeconds: Double = 0

    /// One row per enabled timebase (bars / time / samples), below the range strip.
    static let timebaseRowHeight: CGFloat = 13
    /// The enabled timebases, top to bottom. Always at least the bar ruler.
    var enabledTimebases: [RulerTimebase] {
        var t: [RulerTimebase] = []
        if model.rulerBars { t.append(.bars) }
        if model.rulerTime { t.append(.time) }
        if model.rulerSamples { t.append(.samples) }
        return t.isEmpty ? [.bars] : t
    }
    /// The ruler grows with the number of timebases shown.
    var rulerHeight: CGFloat {
        Self.rangeStripHeight + CGFloat(enabledTimebases.count) * Self.timebaseRowHeight
    }
    /// The top slice of the ruler sets the loop/edit range; below it the ruler scrubs.
    static let rangeStripHeight: CGFloat = 12
    /// Grab radius around each range edge for its ruler handle.
    static let rangeHandleWidth: CGFloat = 9
    static let defaultLaneHeight: CGFloat = 100
    static let minLaneHeight: CGFloat = 30
    static let maxLaneHeight: CGFloat = 320
    /// Progressive-disclosure thresholds for the inline lane-header strip. Shrinking a
    /// lane hides its controls in tiers rather than clipping them: full (fader+pan+meter)
    /// → buttons only (M/S/R/I + automation chips) → name only.
    /// The fader/pan/meter stack ends 83 pt below the lane top and the resize grip occupies the
    /// last 7 pt, so a lane shorter than this drew them into each other at the bottom edge.
    static let laneFaderMinHeight: CGFloat = 96
    static let laneButtonsMinHeight: CGFloat = 50
    /// The fader / pan / meter row is only drawn (and hit-tested) at/above this height.
    func laneShowsFaderRow(_ index: Int) -> Bool { laneHeight(index) >= Self.laneFaderMinHeight }
    /// The M/S/R/I buttons + automation chips are only drawn at/above this height.
    func laneShowsButtons(_ index: Int) -> Bool { laneHeight(index) >= Self.laneButtonsMinHeight }
    /// Per-track height — drag a lane's bottom edge to resize (snapped to a step). Falls back
    /// to the shared default for out-of-range indices.
    func laneHeight(_ index: Int) -> CGFloat {
        let h = (index >= 0 && index < model.lanes.count) ? model.lanes[index].height : model.laneHeight
        return min(Self.maxLaneHeight, max(Self.minLaneHeight, h))
    }
    /// Height to assume for the empty strip below the last lane (drop-to-new-track ghosts).
    private var bottomLaneHeight: CGFloat { laneHeight(model.lanes.count - 1) }
    /// Resize snaps to this step.
    static let laneHeightStep: CGFloat = 16
    static let automationHeight: CGFloat = 54
    static let headerWidth: CGFloat = 150

    private let playheadLayer = CALayer()
    /// The drop target is its own layer, not draw(_:), because a layer-backed view
    /// does not flush draw(_:) during the drag-tracking run loop but a layer change
    /// commits immediately — the same reason the playhead lives in a layer.
    private let dropBandLayer = CALayer()
    private let dropLineLayer = CALayer()

    override var isFlipped: Bool { true }

    // So clicking the timeline pulls focus off any text field (the node host field,
    // plugin search, a rename box). Without this the field editor stays first responder
    // and the keyCode monitor's "don't steal keys while typing" guard swallows Delete.
    override var acceptsFirstResponder: Bool { true }

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
        playheadLayer.backgroundColor = NSColor(hex: 0xff5252).cgColor
        playheadLayer.zPosition = 10
        layer?.addSublayer(playheadLayer)

        dropBandLayer.backgroundColor = NSColor(hex: 0x5f9fd6).withAlphaComponent(0.20).cgColor
        dropBandLayer.borderColor = NSColor(hex: 0x5f9fd6).cgColor
        dropBandLayer.borderWidth = 2
        dropBandLayer.isHidden = true
        dropBandLayer.zPosition = 9
        dropLineLayer.backgroundColor = NSColor(hex: 0x5f9fd6).cgColor
        dropLineLayer.isHidden = true
        dropLineLayer.zPosition = 11
        layer?.addSublayer(dropBandLayer)
        layer?.addSublayer(dropLineLayer)

        registerForDraggedTypes([.fileURL])
    }

    required init?(coder: NSCoder) { nil }

    // MARK: Geometry

    private var lanesRect: NSRect {
        NSRect(x: Self.headerWidth,
               y: rulerHeight,
               width: max(0, bounds.width - Self.headerWidth),
               height: max(0, bounds.height - rulerHeight))
    }

    private func x(forSeconds seconds: Double) -> CGFloat {
        let fraction = (seconds - model.visibleStart) / max(0.0001, model.visibleDuration)
        return lanesRect.minX + CGFloat(fraction) * lanesRect.width
    }

    private func seconds(atX pointX: CGFloat) -> Double {
        let fraction = Double((pointX - lanesRect.minX) / max(1, lanesRect.width))
        return model.visibleStart + fraction * model.visibleDuration
    }

    /// The 줌 tool: zoom in (or ⌥ out) keeping the time under the cursor put.
    private func zoomAtCursor(_ point: NSPoint, out: Bool) {
        let cursorTime = seconds(atX: point.x)
        let newDuration = max(0.5, min(3600.0, model.visibleDuration * (out ? 2.0 : 0.5)))
        let cursorFraction = Double((point.x - lanesRect.minX) / max(1, lanesRect.width))
        let newStart = max(0, cursorTime - cursorFraction * newDuration)
        onZoom?(newStart, newDuration)
    }

    // MARK: Interaction

    /// Lanes are no longer a uniform stack: an open automation lane adds to the one
    /// above it, so every vertical position has to be accumulated.
    private func laneTop(_ index: Int) -> CGFloat {
        var top = rulerHeight - scrollY
        for (i, lane) in model.lanes.prefix(index).enumerated() {
            top += laneHeight(i)
            if lane.automation != nil { top += Self.automationHeight }
        }
        return top
    }

    /// How far the lanes reach below the ruler, at scroll offset zero.
    private var contentHeight: CGFloat {
        model.lanes.indices.reduce(0) { $0 + laneHeight($1) + (model.lanes[$1].automation != nil ? Self.automationHeight : 0) }
    }

    private var maximumScrollY: CGFloat {
        max(0, contentHeight - (bounds.height - rulerHeight))
    }

    private func automationRect(_ index: Int) -> NSRect? {
        guard index < model.lanes.count, model.lanes[index].automation != nil else { return nil }
        return NSRect(x: 0, y: laneTop(index) + laneHeight(index),
                      width: bounds.width, height: Self.automationHeight)
    }

    private func clipRect(_ clip: TimelineModel.Clip) -> NSRect {
        let left = x(forSeconds: clip.startSeconds)
        let right = x(forSeconds: clip.startSeconds + clip.durationSeconds)
        let top = laneTop(clip.laneIndex) + 6
        return NSRect(x: left, y: top, width: max(2, right - left), height: laneHeight(clip.laneIndex) - 12)
    }

    /// While a clip is being carried across lanes, draw a translucent ghost of it at the
    /// lane under the cursor (or a "new track" band below the last lane), so the drag
    /// visibly follows the pointer vertically as well as horizontally.
    private func drawDragGhost(_ context: CGContext) {
        guard let cursor = dragCursor else { return }

        // Option-drag copy: the original stays put, so the ghost follows the cursor fully
        // (both axes) to preview where the copy will land on release.
        if case .copyMoving(let clipId, let grabOffset, _, _) = drag,
           let clip = model.clips.first(where: { $0.id == clipId }) {
            let below = droppedBelowLanes(cursor)
            let dropStart = max(0, snapped(seconds(atX: cursor.x) - grabOffset))
            let left = x(forSeconds: dropStart)
            let width = clipRect(clip).width
            let destTop: CGFloat = below
                ? laneTop(model.lanes.count - 1) + bottomLaneHeight + 6
                : laneTop(laneIndex(at: cursor) ?? clip.laneIndex) + 6
            context.saveGState()
            context.clip(to: NSRect(x: lanesRect.minX, y: rulerHeight,
                                    width: lanesRect.width, height: bounds.height - rulerHeight))
            let ghost = NSRect(x: left, y: destTop, width: width, height: laneHeight(clip.laneIndex) - 12)
            NSColor(hex: 0x35BFA8).withAlphaComponent(0.35).setFill()
            let path = NSBezierPath(roundedRect: ghost, xRadius: 3, yRadius: 3)
            path.fill()
            NSColor(hex: 0x8ff0e0).withAlphaComponent(0.85).setStroke()
            path.lineWidth = 1
            path.stroke()
            ("＋복사" as NSString).draw(at: NSPoint(x: ghost.minX + 4, y: ghost.minY + 3),
                withAttributes: [.font: NSFont.systemFont(ofSize: 8, weight: .bold),
                                 .foregroundColor: NSColor(hex: 0xeafff9)])
            context.restoreGState()
            return
        }

        var draggedIds: [String] = []
        var anchorLane: Int?
        switch drag {
        case .moving(let clipId, _, _, _):
            draggedIds = [clipId]
            anchorLane = model.clips.first { $0.id == clipId }?.laneIndex
        case .movingSelection(let anchorId, _, _, _):
            draggedIds = model.clips.filter { $0.selected }.map { $0.id }
            anchorLane = model.clips.first { $0.id == anchorId }?.laneIndex
        default:
            return
        }
        guard let anchorLane else { return }

        let below = droppedBelowLanes(cursor)
        let targetLane = below ? model.lanes.count : (laneIndex(at: cursor) ?? anchorLane)
        let laneDelta = targetLane - anchorLane
        guard below || laneDelta != 0 else { return }  // horizontal already tracks live

        context.saveGState()
        context.clip(to: NSRect(x: lanesRect.minX, y: rulerHeight,
                                width: lanesRect.width, height: bounds.height - rulerHeight))

        // A band on the destination lane (or the new-track strip below the last lane).
        let bandHeight = below ? bottomLaneHeight : laneHeight(targetLane)
        let bandY = below ? (laneTop(model.lanes.count - 1) + bottomLaneHeight) : laneTop(targetLane)
        NSColor(hex: 0x5f9fd6).withAlphaComponent(0.12).setFill()
        NSRect(x: lanesRect.minX, y: bandY, width: lanesRect.width, height: bandHeight).fill()
        if below {
            NSColor(hex: 0x5f9fd6).withAlphaComponent(0.5).setStroke()
            let dashed = NSBezierPath(rect: NSRect(x: lanesRect.minX + 2, y: bandY + 2,
                                                   width: lanesRect.width - 4, height: bandHeight - 4))
            dashed.setLineDash([5, 3], count: 2, phase: 0)
            dashed.stroke()
        }

        for id in draggedIds {
            guard let clip = model.clips.first(where: { $0.id == id }) else { continue }
            let destLane = below ? model.lanes.count : clip.laneIndex + laneDelta
            let destTop: CGFloat = destLane >= model.lanes.count
                ? laneTop(model.lanes.count - 1) + bottomLaneHeight + 6
                : laneTop(max(0, destLane)) + 6
            let base = clipRect(clip)
            let ghost = NSRect(x: base.minX, y: destTop, width: base.width, height: laneHeight(clip.laneIndex) - 12)
            NSColor(hex: 0x9b7fd4).withAlphaComponent(0.35).setFill()
            let path = NSBezierPath(roundedRect: ghost, xRadius: 3, yRadius: 3)
            path.fill()
            NSColor(hex: 0xc9b8f0).withAlphaComponent(0.8).setStroke()
            path.lineWidth = 1
            path.stroke()
        }
        context.restoreGState()
    }

    /// Topmost clip under the point, searched back to front.
    private func clip(at point: NSPoint) -> TimelineModel.Clip? {
        model.clips.reversed().first { clipRect($0).contains(point) }
    }

    private var selectionCount: Int { model.clips.reduce(0) { $1.selected ? $0 + 1 : $0 } }

    private func regionRect(_ region: TimelineModel.MidiRegion) -> NSRect {
        let left = x(forSeconds: region.startSeconds)
        let right = x(forSeconds: region.startSeconds + region.durationSeconds)
        let top = laneTop(region.laneIndex) + 6
        return NSRect(x: left, y: top, width: max(2, right - left), height: laneHeight(region.laneIndex) - 12)
    }

    private func region(at point: NSPoint) -> TimelineModel.MidiRegion? {
        model.midiRegions.reversed().first { regionRect($0).contains(point) }
    }

    private func drawMidiRegions(_ context: CGContext) {
        context.saveGState()
        context.clip(to: NSRect(x: lanesRect.minX, y: rulerHeight,
                                width: lanesRect.width, height: bounds.height - rulerHeight))

        for region in model.midiRegions {
            let rect = regionRect(region)
            let accent = NSColor(hex: region.muted ? 0x6b6156 : 0x9b7fd4)

            accent.withAlphaComponent(region.muted ? 0.10 : 0.20).setFill()
            let body = NSBezierPath(roundedRect: rect, xRadius: 3, yRadius: 3)
            body.fill()

            // The notes, folded into the region's height. Only pitches that are used
            // get a row, so a two-note part is legible at this size.
            let pitches = region.noteSketch.map(\.pitch)
            if let lowest = pitches.min(), let highest = pitches.max() {
                let span = max(1, highest - lowest)
                let usable = rect.height - 10
                accent.withAlphaComponent(0.9).setFill()
                for note in region.noteSketch {
                    let noteLeft = x(forSeconds: note.startSeconds)
                    let noteRight = x(forSeconds: note.startSeconds + note.durationSeconds)
                    let fraction = CGFloat(note.pitch - lowest) / CGFloat(span)
                    let noteY = rect.maxY - 5 - fraction * usable
                    NSRect(x: noteLeft, y: noteY - 1,
                           width: max(2, noteRight - noteLeft), height: 2.5).fill()
                }
            }

            (region.name as NSString).draw(
                at: NSPoint(x: rect.minX + 4, y: rect.minY + 2),
                withAttributes: [
                    .font: NSFont.systemFont(ofSize: 9, weight: .medium),
                    .foregroundColor: accent,
                ])

            if region.editing {
                NSColor(hex: 0xe6a23c).setStroke()
                body.lineWidth = 2
            } else if region.selected {
                NSColor(hex: 0xd8c8ff).setStroke()
                body.lineWidth = 2
            } else {
                accent.withAlphaComponent(0.8).setStroke()
                body.lineWidth = 1
            }
            body.stroke()
        }
        context.restoreGState()
    }

    /// Every clip the marquee touches, however slightly.
    private func clipsIntersecting(_ rect: NSRect) -> [String] {
        model.clips.filter { clipRect($0).intersects(rect) }.map(\.id)
    }

    /// Which lane a point falls in — its clip row, not its automation row.
    private func laneIndex(at point: NSPoint) -> Int? {
        guard point.y >= rulerHeight else { return nil }
        for index in model.lanes.indices {
            let top = laneTop(index)
            if point.y >= top && point.y < top + laneHeight(index) {
                return index
            }
        }
        return nil
    }

    /// True when a drop lands below the last lane, in the empty lane area — the cue to
    /// spin up a new track for the clip.
    private func droppedBelowLanes(_ point: NSPoint) -> Bool {
        guard point.y >= rulerHeight, !model.lanes.isEmpty else {
            return point.y >= rulerHeight && model.lanes.isEmpty
        }
        let lastBottom = laneTop(model.lanes.count - 1) + bottomLaneHeight
        return point.y >= lastBottom
    }

    /// Which open automation row a point falls in.
    private func automationIndex(at point: NSPoint) -> Int? {
        model.lanes.indices.first { automationRect($0)?.contains(point) ?? false }
    }

    override func mouseDown(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)
        // Take focus so a text field that was being edited resigns — otherwise the
        // keyCode monitor treats every key as "user is typing" and Delete never fires.
        if window?.firstResponder !== self {
            window?.makeFirstResponder(self)
        }

        // The scrollbars sit above everything and are checked first, so a press on one never
        // reaches the lanes underneath.
        if scrollbarHit(point) { return }

        // Clicking a lane header selects that track; the bottom-left disclosure folds automation out;
        // the inline strip mutes/solos/arms or drags the volume fader.
        if point.x < Self.headerWidth {
            if let lane = laneIndex(at: point) {
                let trackId = model.lanes[lane].trackId
                if abs(point.y - (laneTop(lane) + laneHeight(lane))) <= 4 {
                    // Grab the lane's bottom edge to resize this track (or the whole
                    // selection if this lane is part of it), snapped to a step.
                    drag = .resizingLane(startHeight: laneHeight(lane), startY: point.y, laneIndex: lane)
                } else if event.clickCount >= 2 && nameRect(lane).contains(point) {
                    beginRenamingLane(lane)
                } else if automationToggleRect(lane).contains(point) {
                    onToggleAutomation?(lane)
                } else if laneShowsButtons(lane) && headerAutomationModeRect(lane).contains(point) {
                    onCycleAutomationMode?(trackId)
                } else if laneShowsButtons(lane) && headerMuteRect(lane).contains(point) {
                    onToggleMute?(trackId)
                } else if laneShowsButtons(lane) && headerSoloRect(lane).contains(point) {
                    onToggleSolo?(trackId)
                } else if laneShowsButtons(lane) && headerArmRect(lane).contains(point) {
                    onToggleArm?(trackId)
                } else if laneShowsButtons(lane) && headerInputMonitorRect(lane).contains(point) {
                    onToggleInputMonitor?(trackId)
                } else if laneShowsFaderRow(lane) && headerFaderRect(lane).insetBy(dx: 0, dy: -5).contains(point) {
                    onSelectLane?(lane, false)
                    onBeginTouch?(trackId, "track.volume")
                    onSetVolumeDb?(trackId, headerFaderDb(atX: point.x, index: lane))
                    drag = .headerFader(trackId: trackId)
                } else if laneShowsFaderRow(lane) && headerPanRect(lane).insetBy(dx: 0, dy: -5).contains(point) {
                    onSelectLane?(lane, false)
                    onBeginTouch?(trackId, "track.pan")
                    onSetPan?(trackId, headerPan(atX: point.x, index: lane))
                    drag = .headerPan(trackId: trackId)
                } else {
                    onSelectLane?(lane, event.modifierFlags.contains(.shift))
                    // Body of the header with no modifier: a vertical drag reorders the
                    // track. Stays a plain select if the cursor never leaves the dead zone.
                    if !event.modifierFlags.contains(.shift) {
                        drag = .reorderingLane(trackId: trackId, laneIndex: lane,
                                               startY: point.y, currentY: point.y)
                    }
                }
            } else if let lane = automationIndex(at: point) {
                // The parameter name is the parameter picker: volume / pan / plug-in params.
                showAutomationParamMenu(lane: lane, event: event)
            }
            return
        }

        if let lane = automationIndex(at: point), let rect = automationRect(lane),
           let automation = model.lanes[lane].automation {
            if let existing = automationPoint(at: point, laneIndex: lane) {
                if event.clickCount >= 2 {
                    onDeleteAutomationPoint?(lane, existing)
                } else {
                    drag = .movingAutomationPoint(laneIndex: lane, pointIndex: existing)
                }
            } else {
                onAddAutomationPoint?(lane, max(0, snapped(seconds(atX: point.x))),
                                      automationValue(atY: point.y, in: rect, automation))
            }
            return
        }

        if point.y < Self.rangeStripHeight {
            // With a range already set, the strip carries a handle at each edge and a
            // grab in the middle to slide the whole range — the Pro Tools loop bar.
            // An empty strip (or a click outside the range) sweeps a fresh range.
            if model.rangeEnd > model.rangeStart {
                let leftX = x(forSeconds: model.rangeStart)
                let rightX = x(forSeconds: model.rangeEnd)
                if abs(point.x - leftX) <= Self.rangeHandleWidth {
                    drag = .rangingEdgeStart
                    return
                }
                if abs(point.x - rightX) <= Self.rangeHandleWidth {
                    drag = .rangingEdgeEnd
                    return
                }
                if point.x > leftX, point.x < rightX {
                    drag = .movingRange(grabOffsetSeconds: seconds(atX: point.x) - model.rangeStart)
                    return
                }
            }
            let origin = max(0, snapped(seconds(atX: point.x)))
            onSetRangeLane?(nil)   // a ruler-swept range spans all lanes, not one track
            drag = .rangingFrom(seconds: origin)
            return
        }

        // Markers moved to the Global Tracks bar; the ruler just scrubs now. In Grid
        // mode the playhead lands on the grid — a click steps to the nearest line.
        if point.y < rulerHeight {
            drag = .seeking
            onSeek?(snapped(seconds(atX: point.x)))
            return
        }

        // Edit tools that act over the lanes regardless of what is under the cursor.
        if point.y >= rulerHeight {
            if editTool == "zoom" {
                zoomAtCursor(point, out: event.modifierFlags.contains(.option))
                return
            }
            if editTool == "selector", point.x >= Self.headerWidth {
                if !event.modifierFlags.contains(.shift) { onSelect?(nil) }
                drag = .marquee(origin: point, current: point)
                needsDisplay = true
                return
            }
        }

        if let region = region(at: point) {
            let rect = regionRect(region)
            onSelectRegion?(region.id)
            if event.clickCount >= 2 {
                onOpenRegion?(region.id)
            } else if rect.maxX - point.x <= Self.trimHandleWidth {
                drag = .resizingRegion(id: region.id)
            } else {
                drag = .movingRegion(id: region.id,
                                     grabOffsetSeconds: seconds(atX: point.x) - region.startSeconds)
            }
            return
        }

        // Double-clicking the LOWER half of a fade or crossfade region opens the fade editor popover
        // (Pro Tools / Cubase). Checked before the instrument-lane double-click so it wins on a clip.
        if event.clickCount >= 2, let hit = clip(at: point), presentFadeCrossfadeEditor(at: point, hit: hit) {
            return
        }

        // Double-clicking an empty instrument lane starts a new part there.
        if event.clickCount >= 2, let lane = laneIndex(at: point) {
            onAddRegion?(lane, max(0, snapped(seconds(atX: point.x))))
            return
        }

        // Empty lane space. A plain drag makes a Pro Tools time-range edit selection anchored to
        // this lane — so you can define an edit region on an (even empty) track and edit during
        // playback, without moving the playhead. ⇧-drag keeps the multi-clip marquee (rectangle).
        guard let hit = clip(at: point) else {
            if event.modifierFlags.contains(.shift) {
                drag = .marquee(origin: point, current: point)
                needsDisplay = true
                return
            }
            onSelect?(nil)
            if let lane = laneIndex(at: point) {
                let t = max(0, snapped(seconds(atX: point.x)))
                onSetRange?(t, t)          // collapsed to start; the drag widens it
                onSetRangeLane?(lane)
                drag = .rangingInLane(origin: t, lane: lane)
            } else {
                drag = .marquee(origin: point, current: point)   // below the last lane → rectangle select
            }
            needsDisplay = true
            return
        }

        if event.modifierFlags.contains(.shift) {
            onToggleSelect?(hit.id)
            return
        }
        // Clicking inside a multi-selection keeps it, so it can be dragged whole.
        if !hit.selected {
            onSelect?(hit.id)
        }

        // Pro Tools: double-click a clip (Selector) to set the edit range to its whole length — the
        // fast "select this clip as a range" gesture. Anchored to the clip's own lane.
        if event.clickCount >= 2 {
            onSelect?(hit.id)
            onSetRange?(hit.startSeconds, hit.startSeconds + hit.durationSeconds)
            onSetRangeLane?(hit.laneIndex)
            return
        }

        // A specific tool forces its behaviour instead of the smart zone detection.
        switch editTool {
        case "split":
            performScissorsSnip()
            onSplitClip?(hit.id, max(0, snapped(seconds(atX: point.x))))
            return
        case "grabber":
            drag = .moving(clipId: hit.id, grabOffsetSeconds: seconds(atX: point.x) - hit.startSeconds,
                           startX: point.x, axisUnlocked: false)
            return
        case "trim":
            let r = clipRect(hit)
            drag = (point.x - r.minX < r.width / 2) ? .trimmingStart(clipId: hit.id)
                                                    : .trimmingEnd(clipId: hit.id)
            return
        case "fade":
            let r = clipRect(hit)
            drag = (point.x - r.minX < r.width / 2) ? .fadingIn(clip: hit) : .fadingOut(clip: hit)
            return
        default:
            break   // smart tool → the zone detection below
        }

        let rect = clipRect(hit)

        // Fade handles sit on the clip's top corners, offset by the current fade.
        if point.y - rect.minY <= Self.fadeHandleSize + 13 {
            let inX = x(forSeconds: hit.startSeconds + hit.fadeInSeconds)
            let outX = x(forSeconds: hit.startSeconds + hit.durationSeconds - hit.fadeOutSeconds)
            if abs(point.x - inX) <= Self.fadeHandleSize {
                drag = .fadingIn(clip: hit)
                return
            }
            if abs(point.x - outX) <= Self.fadeHandleSize {
                drag = .fadingOut(clip: hit)
                return
            }
        }

        // The gain line is grabbable only on a lone selected clip, away from the edges.
        if hit.selected, selectionCount == 1 {
            let lineY = gainLineY(hit, in: rect)
            if abs(point.y - lineY) <= 4,
               point.x - rect.minX > Self.trimHandleWidth,
               rect.maxX - point.x > Self.trimHandleWidth {
                drag = .gaining(clip: hit, grabY: point.y, startGainDb: hit.gainDb)
                return
            }
        }

        let grabOffset = seconds(atX: point.x) - hit.startSeconds
        if hit.selected, selectionCount > 1 {
            // Trimming a whole selection has no obvious meaning; a drag moves it. Holding ⌥ copies
            // the whole selection instead — the same as ⌥-dragging a single clip, but for all of them.
            if event.modifierFlags.contains(.option) {
                drag = .copyMovingSelection(anchorId: hit.id, grabOffsetSeconds: grabOffset, startX: point.x)
            } else {
                drag = .movingSelection(anchorId: hit.id, grabOffsetSeconds: grabOffset,
                                        startX: point.x, axisUnlocked: false)
            }
        } else if point.x - rect.minX <= Self.trimHandleWidth {
            // If the left edge exactly abuts a same-lane neighbour, grab the shared boundary (roll).
            if let leftN = abuttingNeighbor(of: hit, onLeft: true) {
                drag = .rollingBoundary(leftId: leftN.id, rightId: hit.id)
            } else {
                drag = .trimmingStart(clipId: hit.id)
            }
        } else if rect.maxX - point.x <= Self.trimHandleWidth {
            if let rightN = abuttingNeighbor(of: hit, onLeft: false) {
                drag = .rollingBoundary(leftId: hit.id, rightId: rightN.id)
            } else {
                drag = .trimmingEnd(clipId: hit.id)
            }
        } else if event.modifierFlags.contains(.option) {
            // Option-drag = copy, but the copy is made on release, not now — so an
            // option-click that never moves cannot leave a duplicate summing in place.
            drag = .copyMoving(clipId: hit.id, grabOffsetSeconds: grabOffset,
                               startX: point.x, axisUnlocked: false)
        } else {
            drag = .moving(clipId: hit.id, grabOffsetSeconds: grabOffset,
                           startX: point.x, axisUnlocked: false)
        }
    }

    override func mouseDragged(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)
        if scrollbarDrag(point) { return }
        let time = max(0, seconds(atX: point.x))

        switch drag {
        case .none:
            break
        case .scrollingLanes, .scrollingTime:
            break                       // handled above by scrollbarDrag
        case .seeking:
            onSeek?(snapped(time))
        case .movingRegion(let id, let grabOffset):
            guard let region = model.midiRegions.first(where: { $0.id == id }) else { break }
            let lane = laneIndex(at: point) ?? region.laneIndex
            onMoveRegion?(id, lane, max(0, snapped(time - grabOffset)))
        case .resizingRegion(let id):
            guard let region = model.midiRegions.first(where: { $0.id == id }) else { break }
            onResizeRegion?(id, max(0.1, snapped(time) - region.startSeconds))
        case .movingMarker(let from):
            let target = max(0, snapped(time))
            // The engine finds the marker by where it sits now, so follow it.
            onMoveMarker?(from, target)
            drag = .movingMarker(fromSeconds: target)
        case .rangingFrom(let origin):
            onSetRange?(origin, max(0, snapped(time)))
        case .rangingInLane(let origin, _):
            let t = max(0, snapped(time))
            onSetRange?(min(origin, t), max(origin, t))   // widen either direction from the anchor
            needsDisplay = true
        case .rangingEdgeStart:
            // Drag the left edge; keep it left of the right edge.
            onSetRange?(min(max(0, snapped(time)), model.rangeEnd - 0.01), model.rangeEnd)
        case .rangingEdgeEnd:
            onSetRange?(model.rangeStart, max(model.rangeStart + 0.01, max(0, snapped(time))))
        case .movingRange(let grabOffset):
            let width = model.rangeEnd - model.rangeStart
            let newStart = max(0, snapped(time - grabOffset))
            onSetRange?(newStart, newStart + width)
        case .movingAutomationPoint(let lane, let pointIndex):
            guard let rect = automationRect(lane), let automation = model.lanes[lane].automation else { break }
            onMoveAutomationPoint?(lane, pointIndex, max(0, snapped(time)),
                                   automationValue(atY: point.y, in: rect, automation))
        case .marquee(let origin, _):
            drag = .marquee(origin: origin, current: point)
            needsDisplay = true
        case .moving(let clipId, let grabOffset, let startX, let unlocked):
            dragCursor = point
            needsDisplay = true
            if unlocked {
                onMoveClip?(clipId, snapped(time - grabOffset))
            } else if abs(point.x - startX) > Self.axisLockThreshold {
                // Left the dead zone: from here the clip tracks the cursor. Re-baseline
                // the grab off its current start so it continues smoothly instead of
                // snapping by the threshold distance.
                let start = model.clips.first(where: { $0.id == clipId })?.startSeconds ?? (time - grabOffset)
                drag = .moving(clipId: clipId, grabOffsetSeconds: seconds(atX: point.x) - start,
                               startX: startX, axisUnlocked: true)
            }
            // Locked and inside the dead zone: hold the horizontal position; the lane
            // still resolves from the cursor Y at drop.
        case .copyMoving(let clipId, let grabOffset, let startX, let unlocked):
            // The original never moves; only the ghost follows. Latch the axis lock the
            // same way so a straight vertical carry does not smear the ghost sideways.
            dragCursor = point
            needsDisplay = true
            if !unlocked, abs(point.x - startX) > Self.axisLockThreshold {
                drag = .copyMoving(clipId: clipId, grabOffsetSeconds: grabOffset,
                                   startX: startX, axisUnlocked: true)
            }
        case .movingSelection(let anchorId, let grabOffset, let startX, let unlocked):
            dragCursor = point
            needsDisplay = true
            guard let anchor = model.clips.first(where: { $0.id == anchorId }) else { break }
            if unlocked {
                onMoveSelection?(snapped(time - grabOffset) - anchor.startSeconds)
            } else if abs(point.x - startX) > Self.axisLockThreshold {
                drag = .movingSelection(anchorId: anchorId,
                                        grabOffsetSeconds: seconds(atX: point.x) - anchor.startSeconds,
                                        startX: startX, axisUnlocked: true)
            }
        case .copyMovingSelection(let anchorId, let grabOffset, let startX):
            dragCursor = point
            needsDisplay = true
            // Only once the drag really moves: duplicate the whole selection in place, then continue
            // as a normal selection-move on the copies (originals left put). A click never copies.
            if abs(point.x - startX) > Self.axisLockThreshold {
                if let newAnchor = onBeginCopySelection?(anchorId) {
                    drag = .movingSelection(anchorId: newAnchor, grabOffsetSeconds: grabOffset,
                                            startX: startX, axisUnlocked: true)
                } else {
                    drag = .none
                }
            }
        case .trimmingStart(let clipId):
            onTrimStart?(clipId, snapped(time))
        case .trimmingEnd(let clipId):
            onTrimEnd?(clipId, snapped(time))
        case .rollingBoundary(let leftId, let rightId):
            onRollBoundary?(leftId, rightId, snapped(time))
        case .fadingIn(let clip):
            let fade = min(clip.durationSeconds, max(0, time - clip.startSeconds))
            onSetFades?(clip.id, fade, clip.fadeOutSeconds)
        case .fadingOut(let clip):
            let end = clip.startSeconds + clip.durationSeconds
            let fade = min(clip.durationSeconds, max(0, end - time))
            onSetFades?(clip.id, clip.fadeInSeconds, fade)
        case .gaining(let clip, let grabY, let startGainDb):
            // The full clip height spans the gain range; dragging up is louder.
            let span = Self.gainRange.upperBound - Self.gainRange.lowerBound
            let perPoint = span / Float(max(1, laneHeight(clip.laneIndex) - 12))
            let value = startGainDb - Float(point.y - grabY) * perPoint
            onSetGain?(clip.id, min(Self.gainRange.upperBound, max(Self.gainRange.lowerBound, value)))
        case .headerFader(let trackId):
            onSetVolumeDb?(trackId, headerFaderDb(atX: point.x, index: 0))
        case .headerPan(let trackId):
            onSetPan?(trackId, headerPan(atX: point.x, index: 0))
        case .resizingLane(let startHeight, let startY, let laneIndex):
            let raw = startHeight + (point.y - startY)
            let stepped = (raw / Self.laneHeightStep).rounded() * Self.laneHeightStep       // snap to a step
            let h = min(Self.maxLaneHeight, max(Self.minLaneHeight, stepped))
            // Resize the whole selection when this lane is part of it, else just this track.
            guard laneIndex < model.lanes.count else { break }
            let dragged = model.lanes[laneIndex]
            let targets = dragged.selected
                ? model.lanes.filter { $0.selected }.map { $0.trackId }
                : [dragged.trackId]
            onSetLaneHeight?(targets, h)
        case .reorderingLane(let trackId, let laneIndex, let startY, _):
            drag = .reorderingLane(trackId: trackId, laneIndex: laneIndex, startY: startY, currentY: point.y)
            needsDisplay = true          // redraw the insertion line
        }
    }

    override func mouseUp(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)

        switch drag {
        case .scrollingLanes, .scrollingTime:
            break                       // a scrollbar drag records nothing
        case .moving(let clipId, _, _, _):
            // Dropping past the last lane in empty space makes a new track and lands the
            // clip (or the option-drag copy) there. Dropping on another lane relocates it;
            // both record their own step, so no extra "Move clip".
            if let clip = model.clips.first(where: { $0.id == clipId }) {
                if droppedBelowLanes(point) {
                    onDropClipToNewTrack?(clipId, clip.startSeconds)
                } else if let lane = laneIndex(at: point), lane != clip.laneIndex {
                    onMoveClipToLane?(clipId, lane, clip.startSeconds)
                } else {
                    onCommitEdit?("Move clip")
                }
            } else {
                onCommitEdit?("Move clip")
            }
        case .copyMoving(let clipId, let grabOffset, _, let unlocked):
            // Make the copy now, at the drop — and only if the drag actually moved, so an
            // option-click in place never spawns an overlapping duplicate that sums.
            let movedLane = laneIndex(at: point).map { lane in
                model.clips.first { $0.id == clipId }.map { lane != $0.laneIndex } ?? false
            } ?? false
            if unlocked || movedLane || droppedBelowLanes(point) {
                let dropStart = max(0, snapped(seconds(atX: point.x) - grabOffset))
                if droppedBelowLanes(point) {
                    onDropCopyToNewTrack?(clipId, dropStart)
                } else {
                    onDropCopy?(clipId, laneIndex(at: point) ?? -1, dropStart)
                }
            }
        case .trimmingStart, .trimmingEnd:
            onCommitEdit?("Trim clip")
        case .rollingBoundary:
            onCommitEdit?("Roll edit")
        case .fadingIn, .fadingOut:
            onCommitEdit?("Clip fade")
        case .gaining(let clip, _, _):
            onCommitGain?(clip.id)
        case .movingSelection:
            onCommitEdit?("Move clips")
        case .copyMovingSelection:
            break   // never crossed the threshold — nothing was copied, nothing to commit
        case .marquee(let origin, let current):
            if origin == current {
                // A click, not a sweep: the playhead follows (snapped) — but NOT while playing or
                // recording, so you can click/deselect and edit without interrupting the roll.
                if !isTransportRunning {
                    onSeek?(snapped(seconds(atX: point.x)))
                }
            } else {
                onSelectMany?(clipsIntersecting(NSRect(x: min(origin.x, current.x),
                                                       y: min(origin.y, current.y),
                                                       width: abs(current.x - origin.x),
                                                       height: abs(current.y - origin.y))))
            }
            needsDisplay = true
        case .movingAutomationPoint:
            onCommitEdit?("Automation point")
        case .movingMarker:
            onCommitEdit?("Move marker")
        case .movingRegion:
            onCommitEdit?("Move MIDI region")
        case .resizingRegion:
            onCommitEdit?("Resize MIDI region")
        case .headerFader(let trackId):
            onEndTouch?(trackId, "track.volume")
            onCommitEdit?("Track volume")
        case .headerPan(let trackId):
            onEndTouch?(trackId, "track.pan")
            onCommitEdit?("Track pan")
        case .resizingLane:
            onCommitLaneHeight?()      // persist the new height
        case .reorderingLane(let trackId, let sourceLane, let startY, _):
            // Only a real vertical drag reorders; a tiny move was just the select. Use the
            // live drop point (mouseDragged may coalesce and leave the stored Y stale).
            if abs(point.y - startY) > 8, let target = laneIndex(at: point),
               target != sourceLane, target < model.lanes.count {
                let targetId = model.lanes[target].trackId
                // Dropped on the lower half of the target lane → land after it.
                let mid = laneTop(target) + laneHeight(target) / 2
                onReorderTrack?(trackId, targetId, point.y > mid)
            }
            needsDisplay = true
        case .none, .seeking, .rangingFrom, .rangingInLane, .rangingEdgeStart, .rangingEdgeEnd, .movingRange:
            // The range is a view of where to edit, not an edit. Nothing to undo.
            break
        }
        drag = .none
        if dragCursor != nil { dragCursor = nil; needsDisplay = true }
    }

    private func snapped(_ seconds: Double) -> Double {
        max(0, snap?(seconds) ?? seconds)
    }

    /// A white SF-Symbol cursor, cached — the timeline is dark, so a template (black)
    /// symbol would be invisible. `rotation` turns the glyph; `squashX` (< 1) narrows it,
    /// which the scissors uses to look "closed" for the snip animation. The hotspot is
    /// the image centre so the scissors' pivot sits on the cut point.
    private static var symbolCursorCache: [String: NSCursor] = [:]
    private static func symbolCursor(_ name: String, rotation: CGFloat = 0, squashX: CGFloat = 1) -> NSCursor {
        let key = "\(name)|\(rotation)|\(squashX)"
        if let cached = symbolCursorCache[key] { return cached }
        let config = NSImage.SymbolConfiguration(pointSize: 15, weight: .semibold)
        guard let symbol = NSImage(systemSymbolName: name, accessibilityDescription: nil)?
            .withSymbolConfiguration(config) else { return .arrow }
        let symbolSize = symbol.size
        // A square canvas so rotation never clips a corner.
        let side = ceil(max(symbolSize.width, symbolSize.height) * 1.5)
        let canvas = NSSize(width: side, height: side)
        let image = NSImage(size: canvas)
        image.lockFocus()
        let transform = NSAffineTransform()
        transform.translateX(by: side / 2, yBy: side / 2)
        transform.rotate(byDegrees: rotation)
        transform.scaleX(by: squashX, yBy: 1)
        transform.translateX(by: -symbolSize.width / 2, yBy: -symbolSize.height / 2)
        transform.concat()
        NSColor.white.set()
        symbol.draw(at: .zero, from: NSRect(origin: .zero, size: symbolSize),
                    operation: .sourceOver, fraction: 1.0)
        NSRect(origin: .zero, size: symbolSize).fill(using: .sourceAtop)
        image.unlockFocus()
        let cursor = NSCursor(image: image, hotSpot: NSPoint(x: side / 2, y: side / 2))
        symbolCursorCache[key] = cursor
        return cursor
    }

    /// The split tool's scissors, rotated 90° left so the blade pivot is the cut point.
    private static var scissorsCursor: NSCursor { symbolCursor("scissors", rotation: 90) }
    /// The same scissors "closed" — blades squashed together — for the snip on click.
    private static var scissorsSnipCursor: NSCursor { symbolCursor("scissors", rotation: 90, squashX: 0.4) }

    /// The cursor a forcing edit tool wants over the lanes; nil for smart (default arrow).
    private var toolCursor: NSCursor? {
        switch editTool {
        case "split": return Self.scissorsCursor
        case "zoom": return Self.symbolCursor("magnifyingglass")
        case "selector": return .crosshair
        case "trim": return .resizeLeftRight
        case "grabber": return .openHand
        case "fade": return Self.symbolCursor("line.diagonal")
        default: return nil
        }
    }

    /// A quick close-then-open of the scissors so a split click looks like a snip.
    private func performScissorsSnip() {
        Self.scissorsSnipCursor.set()
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.11) { [weak self] in
            guard let self else { return }
            if self.editTool == "split" { Self.scissorsCursor.set() }
            self.window?.invalidateCursorRects(for: self)
        }
    }

    /// A resize cursor over the trim handles tells the user they are there; a forcing
    /// tool paints its own cursor over the whole lanes area.
    override func resetCursorRects() {
        super.resetCursorRects()
        if let cursor = toolCursor {
            addCursorRect(lanesRect, cursor: cursor)
            return
        }
        for clip in model.clips {
            let rect = clipRect(clip)
            guard rect.width > Self.trimHandleWidth * 2 else { continue }
            addCursorRect(NSRect(x: rect.minX, y: rect.minY,
                                 width: Self.trimHandleWidth, height: rect.height),
                          cursor: .resizeLeftRight)
            addCursorRect(NSRect(x: rect.maxX - Self.trimHandleWidth, y: rect.minY,
                                 width: Self.trimHandleWidth, height: rect.height),
                          cursor: .resizeLeftRight)
        }
        // A roll cursor over each shared boundary of two abutting clips — added last so it wins over
        // the plain resize handles that meet there, telling the user this junction moves BOTH clips.
        for clip in model.clips {
            guard let right = abuttingNeighbor(of: clip, onLeft: false) else { continue }
            let rect = clipRect(clip)
            let boundaryX = clipRect(right).minX
            _ = rect
            addCursorRect(NSRect(x: boundaryX - Self.trimHandleWidth, y: rect.minY,
                                 width: Self.trimHandleWidth * 2, height: rect.height),
                          cursor: Self.rollCursor)
        }
    }

    private static var rollCursor: NSCursor { symbolCursor("arrow.left.and.right") }

    /// Scroll pans; ⌘-scroll (or a pinch) zooms about the pointer.
    override func scrollWheel(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)

        if event.modifierFlags.contains(.command) || event.phase == .changed && event.magnification != 0 {
            let anchor = seconds(atX: point.x)
            let factor = event.scrollingDeltaY > 0 ? 0.9 : 1.1
            let duration = min(3600, max(0.25, model.visibleDuration * factor))

            // Keep the time under the pointer pinned while the span changes.
            let fraction = Double((point.x - lanesRect.minX) / max(1, lanesRect.width))
            let start = max(0, anchor - fraction * duration)
            onZoom?(start, duration)
            return
        }

        let secondsPerPoint = model.visibleDuration / Double(max(1, lanesRect.width))
        let start = max(0, model.visibleStart + Double(event.scrollingDeltaX) * -secondsPerPoint)
        onZoom?(start, model.visibleDuration)

        // Vertical wheel scrolls the lanes under the fixed ruler.
        guard maximumScrollY > 0, event.scrollingDeltaY != 0 else { return }
        let next = min(maximumScrollY, max(0, scrollY - event.scrollingDeltaY))
        guard next != scrollY else { return }
        scrollY = next
        needsDisplay = true
    }

    // MARK: File drop

    /// Only audio files, and only over a lane, are a valid drop.
    private func audioURLs(from sender: NSDraggingInfo) -> [URL] {
        let options: [NSPasteboard.ReadingOptionKey: Any] = [
            .urlReadingFileURLsOnly: true,
            .urlReadingContentsConformToTypes: [
                "public.audio", "public.mp3", "com.microsoft.waveform-audio",
                "public.aifc-audio", "public.aiff-audio", "com.apple.m4a-audio",
            ],
        ]
        let urls = sender.draggingPasteboard.readObjects(forClasses: [NSURL.self],
                                                         options: options) as? [URL]
        return urls ?? []
    }

    /// Standard MIDI files dragged from the MIDI library (or Finder). Read as plain file URLs and
    /// filtered by extension — .mid UTIs are unreliable, but the extension is not.
    private func midiURLs(from sender: NSDraggingInfo) -> [URL] {
        let options: [NSPasteboard.ReadingOptionKey: Any] = [.urlReadingFileURLsOnly: true]
        let urls = (sender.draggingPasteboard.readObjects(forClasses: [NSURL.self],
                                                          options: options) as? [URL]) ?? []
        return urls.filter { ["mid", "midi"].contains($0.pathExtension.lowercased()) }
    }

    private func hideDropTarget() {
        guard !dropBandLayer.isHidden else { return }
        dropLaneIndex = nil
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        dropBandLayer.isHidden = true
        dropLineLayer.isHidden = true
        CATransaction.commit()
    }

    /// The lane a drop lands on and where in time. A drop over the track header — the
    /// name column, left of the lanes — lands at 0 s, so dragging a file onto the
    /// track drops it at the start of the timeline.
    private func dropTarget(at point: NSPoint) -> (lane: Int, seconds: Double)? {
        guard point.y >= rulerHeight else { return nil }
        let seconds = point.x < lanesRect.minX ? 0 : max(0, seconds(atX: point.x))
        if let lane = laneIndex(at: point) {
            return (lane, seconds)
        }
        // Empty space below the last lane: a one-past index tells the drop handler to
        // make a new track and land the clip on it.
        if point.y >= laneTop(model.lanes.count) {
            return (model.lanes.count, seconds)
        }
        return nil
    }

    private func updateDropTarget(_ sender: NSDraggingInfo) -> NSDragOperation {
        let point = convert(sender.draggingLocation, from: nil)
        guard let target = dropTarget(at: point),
              !(audioURLs(from: sender).isEmpty && midiURLs(from: sender).isEmpty) else {
            hideDropTarget()
            return []
        }
        let lane = target.lane
        dropLaneIndex = lane
        dropSeconds = target.seconds
        let top = laneTop(lane)
        let dropX = x(forSeconds: max(0, snapped(dropSeconds)))
        // The backing layer of a flipped view is geometry-flipped by AppKit, so the
        // view's own top-down coordinate is used directly — the same as the playhead.
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        dropBandLayer.frame = CGRect(x: lanesRect.minX, y: top,
                                     width: lanesRect.width, height: laneHeight(lane))
        dropLineLayer.frame = CGRect(x: dropX - 1, y: top, width: 3, height: laneHeight(lane))
        dropBandLayer.isHidden = false
        dropLineLayer.isHidden = false
        CATransaction.commit()
        return .copy
    }

    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation {
        updateDropTarget(sender)
    }

    override func draggingUpdated(_ sender: NSDraggingInfo) -> NSDragOperation {
        updateDropTarget(sender)
    }

    override func draggingExited(_ sender: NSDraggingInfo?) {
        hideDropTarget()
    }

    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        let point = convert(sender.draggingLocation, from: nil)
        defer { hideDropTarget() }
        guard let target = dropTarget(at: point) else { return false }
        // MIDI files take priority: a .mid drop becomes a MIDI region, not an audio import.
        let midi = midiURLs(from: sender)
        if !midi.isEmpty {
            onDropMidi?(target.lane, target.seconds, midi)
            return true
        }
        let urls = audioURLs(from: sender)
        guard !urls.isEmpty else { return false }
        onDropAudio?(target.lane, target.seconds, urls)
        return true
    }

    // MARK: Drawing

    override func layout() {
        scrollY = min(scrollY, maximumScrollY)
        super.layout()
        layoutPlayhead()
    }

    private func layoutPlayhead() {
        let position = x(forSeconds: playheadSeconds)
        let visible = position >= lanesRect.minX && position <= lanesRect.maxX
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        playheadLayer.isHidden = !visible
        playheadLayer.frame = CGRect(x: position, y: 0, width: 1, height: bounds.height)
        CATransaction.commit()
    }

    override func draw(_ dirtyRect: NSRect) {
        guard let context = NSGraphicsContext.current?.cgContext else { return }

        NSColor(hex: 0x2c2622).setFill()
        bounds.fill()

        drawRuler(context)
        drawRange(context)
        drawLaneHeaders(context)
        drawGrid(context)
        drawClips(context)
        drawRecordingClip(context)
        drawCrossfades(context)
        drawDragGhost(context)
        drawMidiRegions(context)
        drawAutomation(context)
        // Markers now live only in the Global Tracks bar's marker lane — the old ruler
        // flags were a duplicate, so they're no longer drawn here.

        // The lane header column sits above the grid but below the playhead.
        NSColor(hex: 0x0b0806).setFill()
        NSRect(x: Self.headerWidth - 1, y: 0, width: 1, height: bounds.height).fill()

        drawMarquee(context)
        drawReorderIndicator(context)
        drawScrollbars(context)
    }

    // MARK: Scrollbars
    //
    // The timeline owns its own offset (the lanes scroll under a fixed ruler, so it is not inside an
    // NSScrollView), which means it has to draw its own bars: a vertical one for the lanes and a
    // horizontal one for time. Both are also grab handles — see `scrollbarHit`.

    static let scrollbarThickness: CGFloat = 11

    /// Full horizontal extent the timeline can show, in seconds: the content, with the current view
    /// always inside it so the thumb never overflows its track.
    private var timeSpanSeconds: Double {
        let contentEnd = model.clips.reduce(0.0) { max($0, $1.startSeconds + $1.durationSeconds) }
        let regionEnd = model.midiRegions.reduce(0.0) { max($0, $1.startSeconds + $1.durationSeconds) }
        return max(max(contentEnd, regionEnd) * 1.05,
                   model.visibleStart + model.visibleDuration) + 1.0
    }

    private var verticalScrollbarRect: NSRect {
        NSRect(x: bounds.width - Self.scrollbarThickness, y: rulerHeight,
               width: Self.scrollbarThickness,
               height: max(0, bounds.height - rulerHeight - Self.scrollbarThickness))
    }

    private var horizontalScrollbarRect: NSRect {
        NSRect(x: Self.headerWidth, y: bounds.height - Self.scrollbarThickness,
               width: max(0, bounds.width - Self.headerWidth - Self.scrollbarThickness),
               height: Self.scrollbarThickness)
    }

    /// (thumb, track) for the lane scrollbar, or nil when everything already fits.
    private func verticalThumb() -> (NSRect, NSRect)? {
        let track = verticalScrollbarRect
        let visible = bounds.height - rulerHeight
        guard maximumScrollY > 0, contentHeight > 0, track.height > 20 else { return nil }
        let fraction = max(0.08, min(1.0, visible / contentHeight))
        let thumbHeight = max(24, track.height * fraction)
        let travel = track.height - thumbHeight
        let progress = maximumScrollY > 0 ? min(1, max(0, scrollY / maximumScrollY)) : 0
        let thumb = NSRect(x: track.minX + 2, y: track.minY + travel * progress,
                           width: track.width - 4, height: thumbHeight)
        return (thumb, track)
    }

    private func horizontalThumb() -> (NSRect, NSRect)? {
        let track = horizontalScrollbarRect
        let span = timeSpanSeconds
        guard span > 0, track.width > 20 else { return nil }
        let fraction = max(0.06, min(1.0, model.visibleDuration / span))
        let thumbWidth = max(28, track.width * fraction)
        let travel = track.width - thumbWidth
        let maxStart = max(0.0001, span - model.visibleDuration)
        let progress = min(1, max(0, model.visibleStart / maxStart))
        let thumb = NSRect(x: track.minX + travel * CGFloat(progress), y: track.minY + 2,
                           width: thumbWidth, height: track.height - 4)
        return (thumb, track)
    }

    private func drawScrollbars(_ context: CGContext) {
        func bar(_ thumb: NSRect, _ track: NSRect, active: Bool) {
            NSColor(hex: 0x1a1613).withAlphaComponent(0.9).setFill()
            track.fill()
            let radius = min(thumb.width, thumb.height) / 2
            let path = NSBezierPath(roundedRect: thumb, xRadius: radius, yRadius: radius)
            NSColor(hex: active ? 0xd8d2c4 : 0x8d867a).withAlphaComponent(active ? 0.95 : 0.7).setFill()
            path.fill()
        }
        if let (thumb, track) = verticalThumb() {
            if case .scrollingLanes = drag { bar(thumb, track, active: true) } else { bar(thumb, track, active: false) }
        }
        if let (thumb, track) = horizontalThumb() {
            if case .scrollingTime = drag { bar(thumb, track, active: true) } else { bar(thumb, track, active: false) }
        }
    }

    /// Press inside a scrollbar: grab the thumb, or page toward the click. Returns true when handled.
    private func scrollbarHit(_ point: NSPoint) -> Bool {
        if let (thumb, track) = verticalThumb(), track.contains(point) {
            if thumb.contains(point) {
                drag = .scrollingLanes(grab: point.y - thumb.minY)
            } else {
                let travel = max(1, track.height - thumb.height)
                let target = min(1, max(0, (point.y - track.minY - thumb.height / 2) / travel))
                scrollY = maximumScrollY * target
                drag = .scrollingLanes(grab: thumb.height / 2)
                needsDisplay = true
            }
            return true
        }
        if let (thumb, track) = horizontalThumb(), track.contains(point) {
            let span = timeSpanSeconds
            let maxStart = max(0.0001, span - model.visibleDuration)
            if thumb.contains(point) {
                drag = .scrollingTime(grab: point.x - thumb.minX)
            } else {
                let travel = max(1, track.width - thumb.width)
                let target = min(1, max(0, (point.x - track.minX - thumb.width / 2) / travel))
                onZoom?(maxStart * Double(target), model.visibleDuration)
                drag = .scrollingTime(grab: thumb.width / 2)
            }
            return true
        }
        return false
    }

    /// Continue a scrollbar drag. Returns true when it consumed the event.
    private func scrollbarDrag(_ point: NSPoint) -> Bool {
        switch drag {
        case .scrollingLanes(let grab):
            guard let (thumb, track) = verticalThumb() else { return true }
            let travel = max(1, track.height - thumb.height)
            let target = min(1, max(0, (point.y - grab - track.minY) / travel))
            scrollY = maximumScrollY * target
            needsDisplay = true
            return true
        case .scrollingTime(let grab):
            guard let (thumb, track) = horizontalThumb() else { return true }
            let travel = max(1, track.width - thumb.width)
            let target = min(1, max(0, (point.x - grab - track.minX) / travel))
            let maxStart = max(0.0001, timeSpanSeconds - model.visibleDuration)
            onZoom?(maxStart * Double(target), model.visibleDuration)
            return true
        default:
            return false
        }
    }

    /// While a lane header is dragged up/down, a bright bar marks where the track will
    /// land — the Nuendo track-list insertion line.
    private func drawReorderIndicator(_ context: CGContext) {
        guard case .reorderingLane(_, let sourceLane, let startY, let currentY) = drag,
              abs(currentY - startY) > 8, let target = laneIndex(at: NSPoint(x: 10, y: currentY)),
              target < model.lanes.count else { return }
        let mid = laneTop(target) + laneHeight(target) / 2
        let y = currentY > mid ? laneTop(target) + laneHeight(target) : laneTop(target)
        NSColor(hex: 0x5f9fd6).setFill()
        NSRect(x: 0, y: y - 1, width: Self.headerWidth, height: 2).fill()
        // Dim the dragged lane's header so it reads as "picked up".
        if sourceLane < model.lanes.count {
            NSColor(hex: 0x5f9fd6).withAlphaComponent(0.14).setFill()
            NSRect(x: 0, y: laneTop(sourceLane), width: Self.headerWidth, height: laneHeight(sourceLane)).fill()
        }
    }

    /// A band across every lane, plus a solid grip in the ruler strip that set it.
    private func drawRange(_ context: CGContext) {
        guard model.rangeEnd > model.rangeStart else { return }
        let left = max(lanesRect.minX, x(forSeconds: model.rangeStart))
        let right = min(lanesRect.maxX, x(forSeconds: model.rangeEnd))
        guard right > left else { return }

        // Green while it is looping, neutral while it is only an edit range.
        let tint = model.loopEnabled ? NSColor(hex: 0x5cb87a) : NSColor(hex: 0x9a8f80)

        tint.withAlphaComponent(0.08).setFill()
        NSRect(x: left, y: rulerHeight, width: right - left,
               height: bounds.height - rulerHeight).fill()

        // A lane-drag range (Pro Tools selector) belongs to its own track — highlight that lane
        // more strongly so the edit selection reads as being "on that track".
        if let lane = model.editRangeLane, lane < model.lanes.count {
            tint.withAlphaComponent(0.20).setFill()
            NSRect(x: left, y: laneTop(lane), width: right - left, height: laneHeight(lane)).fill()
        }

        tint.withAlphaComponent(0.55).setFill()
        NSRect(x: left, y: 0, width: right - left, height: Self.rangeStripHeight).fill()

        tint.withAlphaComponent(0.5).setStroke()
        let edges = NSBezierPath()
        for edge in [left, right] {
            edges.move(to: NSPoint(x: edge, y: Self.rangeStripHeight))
            edges.line(to: NSPoint(x: edge, y: bounds.height))
        }
        edges.lineWidth = 1
        edges.stroke()

        // Pro-Tools-style triangle grab points at each edge, filling the strip. The
        // view is flipped, so y=0 is the top: base up, apex pointing down.
        let handleTint = model.loopEnabled ? NSColor(hex: 0x6fce8f) : NSColor(hex: 0xc4b8a6)
        handleTint.setFill()
        let half: CGFloat = 5
        let tri = NSBezierPath()
        for edge in [left, right] {
            tri.removeAllPoints()
            tri.move(to: NSPoint(x: edge - half, y: 0))
            tri.line(to: NSPoint(x: edge + half, y: 0))
            tri.line(to: NSPoint(x: edge, y: Self.rangeStripHeight))
            tri.close()
            tri.fill()
        }
    }

    private func drawMarquee(_ context: CGContext) {
        guard case .marquee(let origin, let current) = drag, origin != current else { return }
        let rect = NSRect(x: min(origin.x, current.x), y: min(origin.y, current.y),
                          width: abs(current.x - origin.x), height: abs(current.y - origin.y))
        NSColor(hex: 0x5f9fd6).withAlphaComponent(0.15).setFill()
        rect.fill()
        NSColor(hex: 0x5f9fd6).withAlphaComponent(0.7).setStroke()
        NSBezierPath(rect: rect).stroke()
    }

    /// Bars while they are at least 40 pt apart, otherwise a coarser multiple.
    private func barStep() -> Int {
        let secondsPerBar = 60.0 / Double(model.tempoBpm) * Double(model.beatsPerBar)
        let pointsPerBar = Double(lanesRect.width) * secondsPerBar / max(0.0001, model.visibleDuration)
        for step in [1, 2, 4, 8, 16, 32, 64, 128] where Double(step) * pointsPerBar >= 40 {
            return step
        }
        return 256
    }

    /// Right-clicking the ruler chooses which timebases it shows (마디 / 시간 / 샘플);
    /// right-clicking a filled insert chip opens its edit menu.
    override func rightMouseDown(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)
        if point.x < Self.headerWidth, let lane = laneIndex(at: point),
           laneShowsButtons(lane), headerSoloRect(lane).contains(point) {
            let menu = NSMenu()
            for (title, mode) in [("추가 (Additive)", "additive"), ("배타 (Exclusive)", "exclusive")] {
                let item = NSMenuItem(title: title, action: #selector(selectSoloModeMenu(_:)), keyEquivalent: "")
                item.target = self
                item.representedObject = mode
                item.state = soloSelectMode == mode ? .on : .off
                menu.addItem(item)
            }
            menu.addItem(.separator())
            let clear = NSMenuItem(title: "모든 솔로 해제", action: #selector(clearAllSolosMenu(_:)), keyEquivalent: "")
            clear.target = self
            menu.addItem(clear)
            NSMenu.popUpContextMenu(menu, with: event, for: self)
            return
        }
        if point.x < Self.headerWidth, let lane = laneIndex(at: point),
           let slot = headerInsertSlotHit(lane, point: point) {
            let inserts = model.lanes[lane].inserts
            let trackId = model.lanes[lane].trackId
            if slot < inserts.count && !inserts[slot].isEmpty {
                let menu = NSMenu()
                let open = NSMenuItem(title: "에디터 열기", action: #selector(insertOpenMenu(_:)), keyEquivalent: "")
                open.target = self; open.representedObject = HeaderMenuRef(track: trackId, slot: slot); menu.addItem(open)
                let byp = NSMenuItem(title: inserts[slot].bypassed ? "바이패스 해제" : "바이패스",
                                     action: #selector(insertBypassMenu(_:)), keyEquivalent: "")
                byp.target = self; byp.representedObject = HeaderMenuRef(track: trackId, slot: slot); menu.addItem(byp)
                menu.addItem(.separator())
                let rem = NSMenuItem(title: "제거", action: #selector(insertRemoveMenu(_:)), keyEquivalent: "")
                rem.target = self; rem.representedObject = HeaderMenuRef(track: trackId, slot: slot); menu.addItem(rem)
                NSMenu.popUpContextMenu(menu, with: event, for: self)
                return
            } else {
                onBrowseInsert?(trackId); return
            }
        }
        if point.x >= Self.headerWidth, point.y >= rulerHeight, let region = region(at: point) {
            // A MIDI part's menu. Glue (merge with the next part on the track) is the Cubase move;
            // "merge all on track" collapses a stack of takes into one part.
            onSelectRegion?(region.id)
            let menu = NSMenu()
            let open = NSMenuItem(title: "에디터 열기", action: #selector(regionOpenMenu(_:)), keyEquivalent: "")
            open.target = self; open.representedObject = region.id; menu.addItem(open)
            menu.addItem(.separator())
            let hasNext = model.midiRegions.contains {
                $0.laneIndex == region.laneIndex && $0.startSeconds > region.startSeconds
            }
            let glue = NSMenuItem(title: "다음 파트와 합치기 (글루)", action: #selector(regionMergeForwardMenu(_:)), keyEquivalent: "")
            glue.target = self; glue.representedObject = region.id; glue.isEnabled = hasNext; menu.addItem(glue)
            let mergeAll = NSMenuItem(title: "트랙의 MIDI 파트 모두 합치기", action: #selector(regionMergeAllMenu(_:)), keyEquivalent: "")
            mergeAll.target = self; mergeAll.representedObject = region.id; mergeAll.isEnabled = hasNext; menu.addItem(mergeAll)
            NSMenu.popUpContextMenu(menu, with: event, for: self)
            return
        }
        if point.x >= Self.headerWidth, point.y >= rulerHeight, let clip = clip(at: point) {
            showClipFadeMenu(clip, event: event)
            return
        }
        guard point.y < rulerHeight else { super.rightMouseDown(with: event); return }
        let menu = NSMenu()
        menu.addItem({ let h = NSMenuItem(title: "눈금자 표시", action: nil, keyEquivalent: ""); h.isEnabled = false; return h }())
        for tb in RulerTimebase.allCases {
            let item = NSMenuItem(title: tb.label, action: #selector(toggleTimebaseMenu(_:)), keyEquivalent: "")
            item.target = self
            item.tag = tb.rawValue
            item.state = timebaseEnabled(tb) ? .on : .off
            menu.addItem(item)
        }
        NSMenu.popUpContextMenu(menu, with: event, for: self)
    }

    @objc private func selectSoloModeMenu(_ sender: NSMenuItem) {
        guard let mode = sender.representedObject as? String else { return }
        soloSelectMode = mode
        onSetSoloSelectMode?(mode)
    }

    @objc private func clearAllSolosMenu(_ sender: NSMenuItem) {
        onClearAllSolos?()
    }

    @objc private func toggleTimebaseMenu(_ sender: NSMenuItem) {
        if let tb = RulerTimebase(rawValue: sender.tag) { onToggleTimebase?(tb) }
    }

    private func timebaseEnabled(_ tb: RulerTimebase) -> Bool {
        switch tb {
        case .bars: return model.rulerBars
        case .time: return model.rulerTime
        case .samples: return model.rulerSamples
        }
    }

    private func drawRuler(_ context: CGContext) {
        NSColor(hex: 0x332c26).setFill()
        NSRect(x: 0, y: 0, width: bounds.width, height: rulerHeight).fill()

        let secondsPerBar = 60.0 / Double(model.tempoBpm) * Double(model.beatsPerBar)
        let step = barStep()
        let firstBar = max(0, Int((model.visibleStart / secondsPerBar).rounded(.down)))
        let lastBar = Int(((model.visibleStart + model.visibleDuration) / secondsPerBar).rounded(.up))

        let labelAttrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.monospacedSystemFont(ofSize: 8.5, weight: .medium),
            .foregroundColor: NSColor(hex: 0x9a8f7e),
        ]
        let tagAttrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.monospacedSystemFont(ofSize: 7, weight: .bold),
            .foregroundColor: NSColor(hex: 0x6a6154),
        ]

        // One row per enabled timebase, all aligned to the same bar ticks.
        for (rowIndex, timebase) in enabledTimebases.enumerated() {
            let rowTop = Self.rangeStripHeight + CGFloat(rowIndex) * Self.timebaseRowHeight
            if rowIndex > 0 {
                NSColor(hex: 0x28221c).setFill()
                NSRect(x: 0, y: rowTop, width: bounds.width, height: 1).fill()
            }
            // A tiny row tag at the far left so each timebase is identified.
            (timebase.label as NSString).draw(at: NSPoint(x: 4, y: rowTop + 2.5), withAttributes: tagAttrs)

            var bar = firstBar - firstBar % step
            while bar <= lastBar {
                let seconds = Double(bar) * secondsPerBar
                let position = x(forSeconds: seconds)
                if position >= lanesRect.minX - 1 {
                    NSColor(hex: 0x574d40).setFill()
                    NSRect(x: position, y: rowTop, width: 1, height: 5).fill()
                    let label = rulerLabel(for: timebase, bar: bar, seconds: seconds) as NSString
                    label.draw(at: NSPoint(x: position + 3, y: rowTop + 2.5), withAttributes: labelAttrs)
                }
                bar += step
            }
        }

        NSColor(hex: 0x0b0806).setFill()
        NSRect(x: 0, y: rulerHeight - 1, width: bounds.width, height: 1).fill()
    }

    private func rulerLabel(for timebase: RulerTimebase, bar: Int, seconds: Double) -> String {
        switch timebase {
        case .bars:
            return "\(bar + 1)"
        case .time:
            let total = max(0, seconds)
            let m = Int(total) / 60
            let s = Int(total) % 60
            let ms = Int((total - Double(Int(total))) * 1000)
            return ms == 0 ? String(format: "%d:%02d", m, s) : String(format: "%d:%02d.%03d", m, s, ms)
        case .samples:
            let samples = Int((seconds * (model.sampleRate > 0 ? model.sampleRate : 48000)).rounded())
            // Group thousands so large counts stay readable.
            var digits = String(samples), out = "", count = 0
            for ch in digits.reversed() {
                if count > 0 && count % 3 == 0 { out.append(",") }
                out.append(ch); count += 1
            }
            digits = String(out.reversed())
            return digits
        }
    }

    private func drawLaneHeaders(_ context: CGContext) {
        let nameAttributes: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 11, weight: .semibold),
            .foregroundColor: NSColor(hex: 0xe8e1d5),
        ]

        context.saveGState()
        context.clip(to: NSRect(x: 0, y: rulerHeight,
                                width: bounds.width, height: bounds.height - rulerHeight))

        for (index, lane) in model.lanes.enumerated() {
            let rect = NSRect(x: 0,
                              y: laneTop(index),
                              width: Self.headerWidth,
                              height: laneHeight(index))
            NSColor(hex: lane.selected ? 0x3d352e : 0x332c26).setFill()
            rect.fill()

            lane.accent.setFill()
            NSRect(x: 0, y: rect.minY, width: lane.selected ? 5 : 3, height: rect.height).fill()

            // Track number, then the name — the Nuendo track-list header.
            ("\(index + 1)" as NSString).draw(
                at: NSPoint(x: 11, y: rect.minY + 11),
                withAttributes: [
                    .font: NSFont.monospacedSystemFont(ofSize: 9.5, weight: .medium),
                    .foregroundColor: NSColor(hex: 0x8c8175),
                ])
            (lane.name as NSString).draw(at: NSPoint(x: 30, y: rect.minY + 10),
                                         withAttributes: nameAttributes)

            // Pro Tools-style automation disclosure stays pinned to the bottom-left at
            // every track height. It is deliberately independent of the progressive
            // disclosure tiers: shrinking a track may hide M/S/R/I, but must never hide
            // the control that opens its automation lane.
            let toggle = automationToggleRect(index)
            NSColor(hex: lane.automation != nil ? 0x5f9fd6 : 0x302a24).setFill()
            NSBezierPath(roundedRect: toggle, xRadius: 2, yRadius: 2).fill()
            NSColor(hex: lane.automation != nil ? 0x86bde8 : 0x665c51).setStroke()
            NSBezierPath(roundedRect: toggle.insetBy(dx: 0.5, dy: 0.5), xRadius: 2, yRadius: 2).stroke()
            let chevron = NSBezierPath()
            if lane.automation != nil {
                chevron.move(to: NSPoint(x: toggle.minX + 3, y: toggle.minY + 5))
                chevron.line(to: NSPoint(x: toggle.midX, y: toggle.maxY - 3))
                chevron.line(to: NSPoint(x: toggle.maxX - 3, y: toggle.minY + 5))
            } else {
                chevron.move(to: NSPoint(x: toggle.minX + 5, y: toggle.minY + 3))
                chevron.line(to: NSPoint(x: toggle.maxX - 3, y: toggle.midY))
                chevron.line(to: NSPoint(x: toggle.minX + 5, y: toggle.maxY - 3))
            }
            chevron.lineWidth = 1.4
            NSColor(hex: lane.automation != nil ? 0x101418 : 0xa79a8d).setStroke()
            chevron.stroke()

            // Buttons + automation mode only above the buttons threshold; below it the
            // disclosure remains alongside the name.
            if laneShowsButtons(index) {
                // The automation-mode chip remains in the M/S/R/I row.
                drawHeaderAutomationMode(index: index, lane: lane)
            }

            drawLaneHeaderStrip(lane, index: index)

            NSColor(hex: 0x1b1611).setFill()
            NSRect(x: 0, y: rect.maxY - 1, width: bounds.width, height: 1).fill()

            // A resize grip centred on the header's bottom edge — a Pro-Tools-style
            // double bar telling the user this edge drags the track height. Brighter
            // while this lane is being resized.
            let gripHovered: Bool
            if case .resizingLane(_, _, let li) = drag { gripHovered = (li == index) }
            else { gripHovered = false }
            let gripW: CGFloat = 22
            let gripX = (Self.headerWidth - gripW) / 2
            NSColor(hex: gripHovered ? 0x8c7f6a : 0x554c40).setFill()
            NSRect(x: gripX, y: rect.maxY - 4, width: gripW, height: 1.5).fill()
            NSRect(x: gripX, y: rect.maxY - 7, width: gripW, height: 1.5).fill()
        }
        context.restoreGState()
    }

    /// The inline channel strip drawn under each lane's name: M/S/R, a volume fader and
    /// a peak meter, so common moves need no mixer trip.
    private func drawLaneHeaderStrip(_ lane: TimelineModel.Lane, index: Int) {
        func button(_ frame: NSRect, _ title: String, on: Bool, onColor: UInt32, blink: Bool = false) {
            // `blink` is the solo-silenced pulse: a half-lit fill even when not on. Off is
            // the same colour, unlit — a dim tint fill + a dimmed tint glyph, not neutral.
            let fill = on ? NSColor(hex: onColor)
                     : (blink ? NSColor(hex: onColor).withAlphaComponent(0.55)
                              : NSColor(hex: onColor).withAlphaComponent(0.16))
            fill.setFill()
            NSBezierPath(roundedRect: frame, xRadius: 3, yRadius: 3).fill()
            NSColor(hex: onColor).withAlphaComponent(on ? 1 : 0.32).setStroke()
            let outline = NSBezierPath(roundedRect: frame.insetBy(dx: 0.5, dy: 0.5), xRadius: 3, yRadius: 3)
            outline.lineWidth = 1
            outline.stroke()
            let glyphColor = (on || blink) ? NSColor(hex: 0x14100a)
                                           : NSColor(hex: onColor).withAlphaComponent(0.85)
            if title == "●" {
                // Record shows a hollow circle, like the transport key / inspector — not "R".
                let d: CGFloat = 7
                let circle = NSBezierPath(ovalIn: NSRect(x: frame.midX - d / 2, y: frame.midY - d / 2,
                                                         width: d, height: d))
                circle.lineWidth = 1.3
                glyphColor.setStroke()
                circle.stroke()
            } else {
                let attrs: [NSAttributedString.Key: Any] = [
                    .font: NSFont.monospacedSystemFont(ofSize: 8.5, weight: .bold),
                    .foregroundColor: glyphColor,
                ]
                let size = (title as NSString).size(withAttributes: attrs)
                (title as NSString).draw(
                    at: NSPoint(x: frame.midX - size.width / 2, y: frame.midY - size.height / 2),
                    withAttributes: attrs)
            }
        }
        // Name-only tier: nothing below the name.
        guard laneShowsButtons(index) else { return }

        // Mute / solo / arm / monitor, then the inline horizontal fader, pan bar and
        // stereo meter under them.
        // Canonical tints, shared with the mixer / inspector: mute orange, solo yellow,
        // record red, input-monitor blue (was green here — now unified).
        button(headerMuteRect(index), "M", on: lane.muted, onColor: 0xff9f43, blink: lane.soloSilencedBlink)
        button(headerSoloRect(index), "S", on: lane.soloed, onColor: 0xe6d24a)
        button(headerArmRect(index), "●", on: lane.armed, onColor: 0xff5252)
        button(headerInputMonitorRect(index), "I", on: lane.inputMonitor, onColor: 0x5f9fd6)

        // Fader / pan / meter tier: hidden on the first shrink step (buttons-only).
        guard laneShowsFaderRow(index) else { return }

        drawHeaderFader(index: index, lane: lane)
        drawHeaderPan(index: index, lane: lane)

        // Stereo peak meter: L bar on top, R below. Same dB mapping + green→yellow→red
        // gradient as the mixer's HorizontalMeter, so the track meter reads identically.
        let meter = headerMeterRect(index)
        NSColor(hex: 0x140f0a).setFill()
        NSBezierPath(roundedRect: meter, xRadius: 1, yRadius: 1).fill()
        let barHeight = (meter.height - 1) / 2
        let meterGradient = NSGradient(colors: [NSColor(hex: 0x46d17f), NSColor(hex: 0xe6d24a), NSColor(hex: 0xff5252)],
                                       atLocations: [0.0, 0.6, 1.0], colorSpace: .deviceRGB)
        func meterBar(_ level: Float, atY y: CGFloat) {
            let frac = CGFloat(meterFraction(level))
            guard frac > 0.001 else { return }
            NSGraphicsContext.saveGraphicsState()
            NSBezierPath(rect: NSRect(x: meter.minX, y: y, width: meter.width * frac, height: barHeight)).addClip()
            // Draw the gradient across the full width so a colour maps to a level, not to the
            // bar's own length; the clip reveals only up to the current level.
            meterGradient?.draw(in: NSRect(x: meter.minX, y: y, width: meter.width, height: barHeight), angle: 0)
            NSGraphicsContext.restoreGraphicsState()
        }
        let level = trackMeterLevels[lane.name] ?? .init()
        meterBar(level.peakLeft, atY: meter.minY)
        meterBar(level.peakRight, atY: meter.minY + barHeight + 1)
    }

    /// The automation-mode chip (R/T/L/W/O), coloured like Pro Tools' mode buttons.
    private func drawHeaderAutomationMode(index: Int, lane: TimelineModel.Lane) {
        let rect = headerAutomationModeRect(index)
        let (letter, color): (String, UInt32)
        switch lane.automationMode {
        case "write": (letter, color) = ("W", 0xe5484d)
        case "touch": (letter, color) = ("T", 0xf4d35e)
        case "latch": (letter, color) = ("L", 0xe6a23c)
        case "off":   (letter, color) = ("O", 0x6a6157)
        default:      (letter, color) = ("R", 0x5fb85f)     // read
        }
        NSColor(hex: color).setFill()
        NSBezierPath(roundedRect: rect, xRadius: 3, yRadius: 3).fill()
        let attrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.monospacedSystemFont(ofSize: 9, weight: .bold),
            .foregroundColor: NSColor(hex: 0x14100a),
        ]
        let sz = (letter as NSString).size(withAttributes: attrs)
        (letter as NSString).draw(at: NSPoint(x: rect.midX - sz.width / 2, y: rect.midY - sz.height / 2),
                                  withAttributes: attrs)
    }

    private func truncatedChipText(_ text: String, width: CGFloat, attrs: [NSAttributedString.Key: Any]) -> String {
        var s = text
        while (s as NSString).size(withAttributes: attrs).width > width && s.count > 1 {
            s = String(s.dropLast())
        }
        return s
    }

    // MARK: header inserts/sends interaction

    private func headerInsertSlotHit(_ lane: Int, point: NSPoint) -> Int? {
        for slot in 0..<Self.headerInsertSlots where headerInsertRect(lane, slot: slot).contains(point) { return slot }
        return nil
    }
    private final class HeaderMenuRef: NSObject {
        let track: Int, slot: Int; let gain: Float; let bus: String
        init(track: Int, slot: Int = -1, gain: Float = 0, bus: String = "") {
            self.track = track; self.slot = slot; self.gain = gain; self.bus = bus
        }
    }

    @objc private func sendSetGainMenu(_ s: NSMenuItem) {
        guard let r = s.representedObject as? HeaderMenuRef else { return }
        onSetSendGain?(r.track, r.slot, r.gain)
    }
    @objc private func sendSetPanMenu(_ s: NSMenuItem) {
        guard let r = s.representedObject as? HeaderMenuRef else { return }
        onSetSendPan?(r.track, r.slot, r.gain)     // gain field carries the pan value here
    }
    @objc private func sendPreFaderMenu(_ s: NSMenuItem) {
        guard let r = s.representedObject as? HeaderMenuRef else { return }
        onSetSendPreFader?(r.track, r.slot, r.gain > 0.5)
    }
    @objc private func sendRemoveMenu(_ s: NSMenuItem) {
        guard let r = s.representedObject as? HeaderMenuRef else { return }
        onRemoveSend?(r.track, r.slot)
    }
    @objc private func sendAddMenu(_ s: NSMenuItem) {
        guard let r = s.representedObject as? HeaderMenuRef else { return }
        onAddSend?(r.track, r.bus)
    }
    @objc private func sendAddAuxMenu(_ s: NSMenuItem) { onAddAux?() }

    private final class AutoParamRef: NSObject { let lane: Int; let id: String
        init(_ lane: Int, _ id: String) { self.lane = lane; self.id = id } }

    /// Pro Tools-style grouped picker for track, send, insert and instrument automation.
    private func showAutomationParamMenu(lane: Int, event: NSEvent) {
        let options = onAutomationParamOptions?(lane) ?? []
        guard !options.isEmpty else { onCycleAutomationParameter?(lane); return }
        let menu = NSMenu()
        let header = NSMenuItem(title: "오토메이션 파라미터", action: nil, keyEquivalent: "")
        header.isEnabled = false
        menu.addItem(header)

        func addOption(_ opt: (id: String, name: String, on: Bool), to target: NSMenu) {
            let it = NSMenuItem(title: opt.name, action: #selector(pickAutomationParam(_:)), keyEquivalent: "")
            it.target = self
            it.representedObject = AutoParamRef(lane, opt.id)
            it.state = opt.on ? .on : .off
            target.addItem(it)
        }

        let groups: [(title: String, matches: (String) -> Bool)] = [
            ("트랙", { $0.hasPrefix("track.") }),
            ("센드", { $0.hasPrefix("send.") }),
            ("인서트", { $0.hasPrefix("insert.") }),
            ("악기", { $0.hasPrefix("instrument.") })
        ]
        for group in groups {
            let members = options.filter { group.matches($0.id) }
            let submenu = NSMenu(title: group.title)
            if members.isEmpty {
                let empty = NSMenuItem(
                    title: group.title == "센드" ? "활성 센드가 없습니다"
                         : group.title == "인서트" ? "로드된 인서트가 없습니다"
                         : "로드된 악기가 없습니다",
                    action: nil, keyEquivalent: "")
                empty.isEnabled = false
                submenu.addItem(empty)
            } else {
                members.forEach { addOption($0, to: submenu) }
            }
            let root = NSMenuItem(title: group.title, action: nil, keyEquivalent: "")
            root.submenu = submenu
            menu.addItem(root)
        }
        NSMenu.popUpContextMenu(menu, with: event, for: self)
    }
    @objc private func pickAutomationParam(_ s: NSMenuItem) {
        guard let r = s.representedObject as? AutoParamRef else { return }
        onSetAutomationParam?(r.lane, r.id)
    }

    private final class ClipCurveRef: NSObject { let clipId: String; let curve: String
        init(_ clipId: String, _ curve: String) { self.clipId = clipId; self.curve = curve } }
    private final class SpotMenuRef: NSObject { let clipIds: [String]
        init(_ clipIds: [String]) { self.clipIds = clipIds } }
    private final class AlignRef: NSObject { let dubId: String; let refId: String
        init(_ dubId: String, _ refId: String) { self.dubId = dubId; self.refId = refId } }
    private final class StemPresetRef: NSObject { let clipId: String; let preset: String
        init(_ clipId: String, _ preset: String) { self.clipId = clipId; self.preset = preset } }

    private final class AraPluginRef: NSObject { let clipId: String; let pluginName: String; let pluginPath: String
        init(_ clipId: String, _ pluginName: String, _ pluginPath: String) {
            self.clipId = clipId; self.pluginName = pluginName; self.pluginPath = pluginPath } }

    /// A live strength slider hosted inside the "리드에 정렬" submenu (0…100 %).
    private final class AlignStrengthMenuView: NSView {
        private let slider = NSSlider()
        private let pctLabel = NSTextField(labelWithString: "")
        private let onChange: (Double) -> Void
        init(strength: Double, onChange: @escaping (Double) -> Void) {
            self.onChange = onChange
            super.init(frame: NSRect(x: 0, y: 0, width: 244, height: 30))
            let title = NSTextField(labelWithString: "정렬 강도")
            title.font = .systemFont(ofSize: 11); title.textColor = .secondaryLabelColor
            title.frame = NSRect(x: 14, y: 7, width: 60, height: 16)
            addSubview(title)
            slider.minValue = 0; slider.maxValue = 1; slider.doubleValue = strength
            slider.isContinuous = true; slider.target = self; slider.action = #selector(changed)
            slider.frame = NSRect(x: 78, y: 5, width: 116, height: 20)
            addSubview(slider)
            pctLabel.font = .monospacedDigitSystemFont(ofSize: 11, weight: .regular)
            pctLabel.alignment = .right; pctLabel.frame = NSRect(x: 196, y: 7, width: 40, height: 16)
            pctLabel.stringValue = "\(Int((strength * 100).rounded()))%"
            addSubview(pctLabel)
        }
        required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }
        @objc private func changed() {
            pctLabel.stringValue = "\(Int((slider.doubleValue * 100).rounded()))%"
            onChange(slider.doubleValue)
        }
    }

    /// Right-click a clip → spot back to its original position, fade curves.
    private func showClipFadeMenu(_ clip: TimelineModel.Clip, event: NSEvent) {
        let menu = NSMenu()
        // 스팟: the Pro-Tools original time stamp — where the clip first landed
        // (import). Clicking inside a multi-selection spots the whole selection,
        // each clip to its own original, like Delete and drag do.
        let spotTargets = (clip.selected && selectionCount > 1)
            ? model.clips.filter(\.selected).map(\.id) : [clip.id]
        let original = onClipOriginalStart?(clip.id) ?? -1
        let title = spotTargets.count > 1
            ? "원래 위치로 스팟 (\(spotTargets.count)개)"
            : (original >= 0 ? "원래 위치로 스팟 (\(spotTimeLabel(original)))" : "원래 위치로 스팟")
        let spot = NSMenuItem(title: title, action: #selector(spotClipsMenu(_:)), keyEquivalent: "")
        spot.target = self
        spot.representedObject = SpotMenuRef(spotTargets)
        // A single clip from an old project has no stored original; a selection may
        // still contain clips that do, so it stays clickable.
        if spotTargets.count == 1 && original < 0 { spot.action = nil }
        menu.addItem(spot)

        // Non-destructive clip processing (Logic/Cubase style) — the renderer honours the flags, so
        // nothing writes a new file and every one undoes. Mute shows a checkmark from the model;
        // reverse/polarity are plain toggles.
        menu.addItem(.separator())
        func clipProc(_ title: String, _ sel: Selector, checked: Bool = false) {
            let it = NSMenuItem(title: title, action: sel, keyEquivalent: "")
            it.target = self
            it.representedObject = clip.id as NSString
            it.state = checked ? .on : .off
            menu.addItem(it)
        }
        clipProc("리버스", #selector(reverseClipMenu(_:)))
        clipProc("노멀라이즈", #selector(normalizeClipMenu(_:)))
        clipProc("뮤트", #selector(muteClipMenu(_:)))
        clipProc("Ø 위상 반전", #selector(polarityClipMenu(_:)))

        // Offline time/pitch print: open one manual editor instead of guessing from presets.
        if onApplyClipTimePitch != nil {
            let tpItem = NSMenuItem(title: "타임/피치 설정…", action: #selector(openTimePitchSettings(_:)), keyEquivalent: "")
            tpItem.target = self
            tpItem.representedObject = clip.id as NSString
            menu.addItem(tpItem)
        }
        if onDenoiseClip != nil {
            clipProc("노이즈 제거 (뉴럴 디노이저)", #selector(denoiseClipMenu(_:)))
        }
        // ARA: the plug-in's own editor, not ours. Only listed when one is actually installed —
        // an empty submenu would suggest the feature is broken rather than absent.
        if onOpenAraEditor != nil && !araPlugins.isEmpty {
            let edited = clipHasAraEdits?(clip.id) ?? false
            let araItem = NSMenuItem(title: edited ? "ARA 편집 계속…" : "ARA 편집 (플러그인 에디터)…",
                                     action: nil, keyEquivalent: "")
            let sub = NSMenu()
            for plugin in araPlugins {
                let item = NSMenuItem(title: plugin.name, action: #selector(openAraEditorMenu(_:)),
                                      keyEquivalent: "")
                item.target = self
                item.representedObject = AraPluginRef(clip.id, plugin.name, plugin.path)
                sub.addItem(item)
            }
            if edited {
                sub.addItem(NSMenuItem.separator())
                let clear = NSMenuItem(title: "ARA 편집 제거 (원본으로)",
                                       action: #selector(clearAraEditsMenu(_:)), keyEquivalent: "")
                clear.target = self
                clear.representedObject = clip.id as NSString
                sub.addItem(clear)
            }
            araItem.submenu = sub
            menu.addItem(araItem)
        }
        if let sepPreset = onSeparateStemsPreset {
            _ = sepPreset
            let stemItem = NSMenuItem(title: "스템 분리 설정…",
                                      action: #selector(separateStemPresetMenu(_:)), keyEquivalent: "")
            stemItem.target = self
            stemItem.representedObject = StemPresetRef(clip.id, "configure")
            menu.addItem(stemItem)
        } else if onSeparateStems != nil {
            clipProc("스템 분리 (드럼/베이스/보컬/기타)", #selector(separateStemsMenu(_:)))
        }
        if onConvertToMidi != nil {
            if convertToMidiPolyAvailable && onConvertToMidiPoly != nil {
                let midiItem = NSMenuItem(title: "MIDI로 변환", action: nil, keyEquivalent: "")
                let sub = NSMenu()
                let mono = NSMenuItem(title: "모노포닉 (베이스/보컬/리드)", action: #selector(convertToMidiMenu(_:)), keyEquivalent: "")
                mono.target = self; mono.representedObject = clip.id as NSString; sub.addItem(mono)
                let poly = NSMenuItem(title: "폴리포닉 (화음/피아노/기타)", action: #selector(convertToMidiPolyMenu(_:)), keyEquivalent: "")
                poly.target = self; poly.representedObject = clip.id as NSString; sub.addItem(poly)
                midiItem.submenu = sub
                menu.addItem(midiItem)
            } else {
                clipProc("MIDI로 변환 (인스트루먼트 트랙 생성)", #selector(convertToMidiMenu(_:)))
            }
        }
        if onOpenPitchEditor != nil {
            clipProc("피치앤타임 에디터", #selector(openPitchEditorMenu(_:)))
        }
        // VocAlign: time-align this clip onto another (the lead) picked from the other clips.
        if onAlignToReference != nil {
            let others = model.clips.filter { $0.id != clip.id }
            if !others.isEmpty {
                let alignItem = NSMenuItem(title: "리드에 정렬 (VocAlign)", action: nil, keyEquivalent: "")
                let sub = NSMenu()
                // Alignment strength — a live slider (how far to snap toward the lead).
                if let setStrength = onSetAlignStrength {
                    let strItem = NSMenuItem()
                    strItem.view = AlignStrengthMenuView(strength: alignStrength, onChange: setStrength)
                    sub.addItem(strItem)
                    sub.addItem(.separator())
                }
                for other in others {
                    let mm = Int(other.startSeconds) / 60, ss = Int(other.startSeconds) % 60
                    let nm = other.name.isEmpty ? "클립" : other.name
                    let it = NSMenuItem(title: String(format: "%@ · 트랙 %d · %d:%02d", nm, other.laneIndex + 1, mm, ss),
                                        action: #selector(alignToRefMenu(_:)), keyEquivalent: "")
                    it.target = self
                    it.representedObject = AlignRef(clip.id, other.id)
                    sub.addItem(it)
                }
                alignItem.submenu = sub
                menu.addItem(alignItem)
            }
        }

        let opts = onFadeCurveOptions?() ?? []
        if opts.isEmpty {
            NSMenu.popUpContextMenu(menu, with: event, for: self)
            return
        }
        menu.addItem(.separator())
        let current = onClipCurrentFades?(clip.id)
        func submenu(_ title: String, sel: Selector, selected: String?) {
            let item = NSMenuItem(title: title, action: nil, keyEquivalent: "")
            let sub = NSMenu()
            for o in opts {
                let it = NSMenuItem(title: o.label, action: sel, keyEquivalent: "")
                it.target = self; it.representedObject = ClipCurveRef(clip.id, o.id)
                it.state = (selected == o.id) ? .on : .off
                sub.addItem(it)
            }
            item.submenu = sub
            menu.addItem(item)
        }
        submenu("페이드 인 커브", sel: #selector(setFadeInCurveMenu(_:)), selected: current?.inCurve)
        submenu("페이드 아웃 커브", sel: #selector(setFadeOutCurveMenu(_:)), selected: current?.outCurve)
        NSMenu.popUpContextMenu(menu, with: event, for: self)
    }
    @objc private func spotClipsMenu(_ s: NSMenuItem) {
        guard let r = s.representedObject as? SpotMenuRef else { return }
        onSpotClips?(r.clipIds)
    }

    private func spotTimeLabel(_ seconds: Double) -> String {
        let total = max(0, seconds)
        let m = Int(total) / 60
        let sec = Int(total) % 60
        let ms = Int((total - Double(Int(total))) * 1000)
        return ms == 0 ? String(format: "%d:%02d", m, sec) : String(format: "%d:%02d.%03d", m, sec, ms)
    }

    @objc private func setFadeInCurveMenu(_ s: NSMenuItem) {
        guard let r = s.representedObject as? ClipCurveRef else { return }
        onSetClipFadeInCurve?(r.clipId, r.curve)
    }
    @objc private func regionOpenMenu(_ s: NSMenuItem) { if let id = s.representedObject as? String { onOpenRegion?(id) } }
    @objc private func regionMergeForwardMenu(_ s: NSMenuItem) { if let id = s.representedObject as? String { onMergeRegionForward?(id) } }
    @objc private func regionMergeAllMenu(_ s: NSMenuItem) { if let id = s.representedObject as? String { onMergeRegionsOnTrack?(id) } }
    @objc private func reverseClipMenu(_ s: NSMenuItem) { if let id = s.representedObject as? String { onReverseClip?(id) } }
    @objc private func normalizeClipMenu(_ s: NSMenuItem) { if let id = s.representedObject as? String { onNormalizeClip?(id) } }
    @objc private func openTimePitchSettings(_ s: NSMenuItem) {
        guard let clipId = s.representedObject as? String else { return }
        let alert = NSAlert()
        alert.messageText = "타임/피치"
        alert.informativeText = "길이와 음정을 각각 직접 설정합니다. 원본은 새 오디오 파일로 프린트됩니다."
        alert.addButton(withTitle: "적용")
        alert.addButton(withTitle: "취소")
        let ratio = NSTextField(string: "1.000")
        let semitones = NSTextField(string: "0.00")
        let form = NSGridView(views: [
            [NSTextField(labelWithString: "길이 비율"), ratio, NSTextField(labelWithString: "×")],
            [NSTextField(labelWithString: "피치"), semitones, NSTextField(labelWithString: "반음")]
        ])
        form.column(at: 1).width = 110
        form.rowSpacing = 8
        alert.accessoryView = form
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        let r = min(8.0, max(0.125, ratio.doubleValue))
        let st = min(24.0, max(-24.0, semitones.doubleValue))
        onApplyClipTimePitch?(clipId, r, st)
    }
    @objc private func denoiseClipMenu(_ s: NSMenuItem) { if let id = s.representedObject as? String { onDenoiseClip?(id) } }
    @objc private func openAraEditorMenu(_ s: NSMenuItem) {
        if let ref = s.representedObject as? AraPluginRef {
            onOpenAraEditor?(ref.clipId, ref.pluginName, ref.pluginPath)
        }
    }
    @objc private func clearAraEditsMenu(_ s: NSMenuItem) {
        if let id = s.representedObject as? String { onClearAraEdits?(id) }
    }
    @objc private func alignToRefMenu(_ s: NSMenuItem) { if let r = s.representedObject as? AlignRef { onAlignToReference?(r.dubId, r.refId) } }
    @objc private func separateStemsMenu(_ s: NSMenuItem) { if let id = s.representedObject as? String { onSeparateStems?(id) } }
    @objc private func convertToMidiMenu(_ s: NSMenuItem) { if let id = s.representedObject as? String { onConvertToMidi?(id) } }
    @objc private func convertToMidiPolyMenu(_ s: NSMenuItem) { if let id = s.representedObject as? String { onConvertToMidiPoly?(id) } }
    @objc private func separateStemPresetMenu(_ s: NSMenuItem) { if let r = s.representedObject as? StemPresetRef { onSeparateStemsPreset?(r.clipId, r.preset) } }
    @objc private func openPitchEditorMenu(_ s: NSMenuItem) { if let id = s.representedObject as? String { onOpenPitchEditor?(id) } }
    @objc private func muteClipMenu(_ s: NSMenuItem) { if let id = s.representedObject as? String { onToggleClipMute?(id) } }
    @objc private func polarityClipMenu(_ s: NSMenuItem) { if let id = s.representedObject as? String { onToggleClipPolarity?(id) } }
    @objc private func setFadeOutCurveMenu(_ s: NSMenuItem) {
        guard let r = s.representedObject as? ClipCurveRef else { return }
        onSetClipFadeOutCurve?(r.clipId, r.curve)
    }

    @objc private func insertBypassMenu(_ s: NSMenuItem) {
        guard let r = s.representedObject as? HeaderMenuRef else { return }
        onBypassInsert?(r.track, r.slot)
    }
    @objc private func insertRemoveMenu(_ s: NSMenuItem) {
        guard let r = s.representedObject as? HeaderMenuRef else { return }
        onRemoveInsert?(r.track, r.slot)
    }
    @objc private func insertOpenMenu(_ s: NSMenuItem) {
        guard let r = s.representedObject as? HeaderMenuRef else { return }
        onToggleInsertEditor?(r.track, r.slot)
    }

    /// Markers live in the ruler below the range strip; they must not eat the scrub.
    private static let markerFlagWidth: CGFloat = 7

    private func markerFlagRect(_ marker: TimelineModel.Marker) -> NSRect {
        NSRect(x: x(forSeconds: marker.timeSeconds), y: Self.rangeStripHeight,
               width: Self.markerFlagWidth, height: rulerHeight - Self.rangeStripHeight)
    }

    private func marker(at point: NSPoint) -> TimelineModel.Marker? {
        model.markers.last { markerFlagRect($0).insetBy(dx: -2, dy: 0).contains(point) }
    }

    private func drawMarkers(_ context: CGContext) {
        guard !model.markers.isEmpty else { return }
        context.saveGState()
        context.clip(to: NSRect(x: lanesRect.minX, y: 0,
                                width: lanesRect.width, height: bounds.height))

        for marker in model.markers {
            let position = x(forSeconds: marker.timeSeconds)

            // A hairline all the way down, so a marker can be lined up with a clip.
            NSColor(hex: 0xe6a23c).withAlphaComponent(0.22).setFill()
            NSRect(x: position, y: rulerHeight, width: 1,
                   height: bounds.height - rulerHeight).fill()

            let flag = markerFlagRect(marker)
            NSColor(hex: 0xe6a23c).setFill()
            flag.fill()

            (marker.name as NSString).draw(
                at: NSPoint(x: flag.maxX + 3, y: flag.minY + 2),
                withAttributes: [
                    .font: NSFont.systemFont(ofSize: 9, weight: .medium),
                    .foregroundColor: NSColor(hex: 0xe6a23c),
                ])
        }
        context.restoreGState()
    }

    private func automationToggleRect(_ index: Int) -> NSRect {
        // Pro Tools-style disclosure in the track header's fixed bottom-left corner.
        // Kept 5 pt above the resize edge so a click never starts a height drag.
        NSRect(x: 3, y: laneTop(index) + laneHeight(index) - 20, width: 14, height: 14)
    }

    /// The track-name band in the header — double-click here to rename.
    private func nameRect(_ index: Int) -> NSRect {
        NSRect(x: 28, y: laneTop(index) + 6, width: Self.headerWidth - 74, height: 22)
    }

    private var renameField: NSTextField?
    private var renamingTrackId: Int?

    /// Inline rename: float an NSTextField over the lane name, commit on Return / focus loss.
    private func beginRenamingLane(_ index: Int) {
        guard index < model.lanes.count else { return }
        renameField?.removeFromSuperview()
        let lane = model.lanes[index]
        let field = NSTextField(frame: nameRect(index))
        field.stringValue = lane.name
        field.font = NSFont.systemFont(ofSize: 11, weight: .semibold)
        field.isBezeled = true
        field.bezelStyle = .roundedBezel
        field.focusRingType = .none
        field.target = self
        field.action = #selector(commitRename(_:))
        field.delegate = self
        addSubview(field)
        window?.makeFirstResponder(field)
        field.selectText(nil)
        renameField = field
        renamingTrackId = lane.trackId
    }

    @objc private func commitRename(_ sender: NSTextField) {
        finishRenaming()
    }

    func controlTextDidEndEditing(_ obj: Notification) {
        finishRenaming()
    }

    private func finishRenaming() {
        guard let field = renameField else { return }
        let newName = field.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
        let trackId = renamingTrackId
        renameField = nil
        renamingTrackId = nil
        field.removeFromSuperview()
        if let trackId, !newName.isEmpty { onRenameTrack?(trackId, newName) }
    }

    // Inline lane-header channel strip. All rects are in the header column (x < headerWidth).
    private static let headerButtonWidth: CGFloat = 20
    private static let headerButtonHeight: CGFloat = 15

    private func headerMuteRect(_ index: Int) -> NSRect {
        NSRect(x: 12, y: laneTop(index) + 30, width: Self.headerButtonWidth, height: Self.headerButtonHeight)
    }
    private func headerSoloRect(_ index: Int) -> NSRect {
        NSRect(x: 12 + Self.headerButtonWidth + 3, y: laneTop(index) + 30,
               width: Self.headerButtonWidth, height: Self.headerButtonHeight)
    }
    private func headerArmRect(_ index: Int) -> NSRect {
        NSRect(x: 12 + 2 * (Self.headerButtonWidth + 3), y: laneTop(index) + 30,
               width: Self.headerButtonWidth, height: Self.headerButtonHeight)
    }
    private func headerInputMonitorRect(_ index: Int) -> NSRect {
        NSRect(x: 12 + 3 * (Self.headerButtonWidth + 3), y: laneTop(index) + 30,
               width: Self.headerButtonWidth, height: Self.headerButtonHeight)
    }
    /// Small square top-right of the header that shows/cycles the automation mode.
    private func headerAutomationModeRect(_ index: Int) -> NSRect {
        // Continues the M/S/R/I row: after the "I" button (ends x101), gap 3.
        NSRect(x: 12 + 4 * (Self.headerButtonWidth + 3), y: laneTop(index) + 30, width: 18, height: Self.headerButtonHeight)
    }

    // Inline volume fader, pan bar and stereo meter, drawn horizontally under the buttons.
    private func headerFaderRect(_ index: Int) -> NSRect {
        NSRect(x: 12, y: laneTop(index) + 50, width: Self.headerWidth - 24, height: 8)
    }
    private func headerPanRect(_ index: Int) -> NSRect {
        NSRect(x: 12, y: laneTop(index) + 63, width: Self.headerWidth - 24, height: 8)
    }
    private func headerMeterRect(_ index: Int) -> NSRect {
        NSRect(x: 12, y: laneTop(index) + 76, width: Self.headerWidth - 24, height: 7)
    }
    /// Same console taper as the mixer fader (0 dB ~78% along).
    private func headerFaderFraction(_ db: Float) -> CGFloat { CGFloat(FaderScale.position(forDb: db)) }
    private func headerFaderDb(atX pointX: CGFloat, index: Int) -> Float {
        let rect = headerFaderRect(index)
        return FaderScale.db(forPosition: Double(min(1, max(0, (pointX - rect.minX) / max(1, rect.width)))))
    }
    private func headerPan(atX pointX: CGFloat, index: Int) -> Float {
        let rect = headerPanRect(index)
        let pan = Float(min(1, max(0, (pointX - rect.minX) / max(1, rect.width)))) * 2 - 1
        return abs(pan) < 0.06 ? 0 : pan          // centre detent
    }

    private func drawHeaderFader(index: Int, lane: TimelineModel.Lane) {
        let fader = headerFaderRect(index)
        // A thin slot — just the moving bar's thickness, not a tall pill background.
        let slotH: CGFloat = 3
        let slot = NSRect(x: fader.minX, y: fader.midY - slotH / 2, width: fader.width, height: slotH)
        NSColor(hex: 0x151009).setFill()
        NSBezierPath(roundedRect: slot, xRadius: slotH / 2, yRadius: slotH / 2).fill()
        let knobX = fader.minX + headerFaderFraction(lane.volumeDb) * fader.width
        lane.accent.withAlphaComponent(0.7).setFill()
        NSBezierPath(roundedRect: NSRect(x: fader.minX, y: fader.midY - 1.5,
                                         width: max(0, knobX - fader.minX), height: 3),
                     xRadius: 1.5, yRadius: 1.5).fill()
        let knobW: CGFloat = 11, knobH: CGFloat = 12
        let knob = NSRect(x: knobX - knobW / 2, y: fader.midY - knobH / 2, width: knobW, height: knobH)
        let cap = NSBezierPath(roundedRect: knob, xRadius: 3, yRadius: 3)
        if let g = NSGradient(colors: [NSColor(hex: 0x6a727a), NSColor(hex: 0x363c44), NSColor(hex: 0x181c22)]) {
            g.draw(in: cap, angle: -90)
        }
        NSColor.white.withAlphaComponent(0.30).setStroke(); cap.lineWidth = 0.75; cap.stroke()
        NSColor.black.withAlphaComponent(0.35).setStroke()
        for dy in [-3.0, 0.0, 3.0] as [CGFloat] {
            let l = NSBezierPath()
            l.move(to: NSPoint(x: knob.minX + 2.5, y: knob.midY + dy))
            l.line(to: NSPoint(x: knob.maxX - 2.5, y: knob.midY + dy))
            l.lineWidth = 0.75; l.stroke()
        }
        lane.accent.setFill()
        NSRect(x: knob.midX - 0.75, y: knob.minY + 3, width: 1.5, height: knob.height - 6).fill()
    }

    private func drawHeaderPan(index: Int, lane: TimelineModel.Lane) {
        let bar = headerPanRect(index)
        // A thin slot, matching the fader — not a tall pill background.
        let slotH: CGFloat = 3
        let slot = NSRect(x: bar.minX, y: bar.midY - slotH / 2, width: bar.width, height: slotH)
        NSColor(hex: 0x151009).setFill()
        NSBezierPath(roundedRect: slot, xRadius: slotH / 2, yRadius: slotH / 2).fill()
        NSColor.white.withAlphaComponent(0.18).setFill()
        NSRect(x: bar.midX - 0.5, y: bar.midY - 2.5, width: 1, height: 5).fill()   // centre detent tick
        let knobX = bar.minX + CGFloat((lane.pan + 1) / 2) * bar.width
        lane.accent.withAlphaComponent(0.7).setFill()
        NSBezierPath(roundedRect: NSRect(x: min(bar.midX, knobX), y: bar.midY - 1.5,
                                         width: abs(knobX - bar.midX), height: 3),
                     xRadius: 1.5, yRadius: 1.5).fill()
        // A round thumb, matching the mixer's PanSlider circular thumb.
        let knobD: CGFloat = 9
        let knob = NSRect(x: knobX - knobD / 2, y: bar.midY - knobD / 2, width: knobD, height: knobD)
        let cap = NSBezierPath(ovalIn: knob)
        if let g = NSGradient(colors: [NSColor(hex: 0x6a727a), NSColor(hex: 0x363c44), NSColor(hex: 0x181c22)]) {
            g.draw(in: cap, angle: -90)
        }
        NSColor.white.withAlphaComponent(0.30).setStroke(); cap.lineWidth = 0.75; cap.stroke()
    }

    // Insert slot hit-testing for the header's right-click context menu: four slot chips.
    static let headerInsertSlots = 4
    private func headerInsertRect(_ index: Int, slot: Int) -> NSRect {
        let gap: CGFloat = 3
        let totalW = Self.headerWidth - 24
        let w = (totalW - gap * CGFloat(Self.headerInsertSlots - 1)) / CGFloat(Self.headerInsertSlots)
        return NSRect(x: 12 + CGFloat(slot) * (w + gap), y: laneTop(index) + 84, width: w, height: 13)
    }

    /// Value axis: the top of the row is the parameter's maximum. Volume uses the same
    /// console taper as the fader (0 dB ~78% up), so where the curve sits matches where
    /// the fader sits — a level set on the fader draws at the same height it was recorded.
    private func automationFraction(_ value: Float, _ automation: TimelineModel.Automation) -> CGFloat {
        if automation.parameterId == "track.volume" {
            return CGFloat(FaderScale.position(forDb: value))
        }
        let span = automation.range.upperBound - automation.range.lowerBound
        return CGFloat((value - automation.range.lowerBound) / max(0.0001, span))
    }
    private func automationValueFor(_ fraction: CGFloat, _ automation: TimelineModel.Automation) -> Float {
        if automation.parameterId == "track.volume" {
            return FaderScale.db(forPosition: Double(min(1, max(0, fraction))))
        }
        let span = automation.range.upperBound - automation.range.lowerBound
        let value = automation.range.lowerBound + Float(fraction) * span
        return min(automation.range.upperBound, max(automation.range.lowerBound, value))
    }

    private func automationY(_ value: Float, in rect: NSRect, _ automation: TimelineModel.Automation) -> CGFloat {
        rect.maxY - 6 - automationFraction(value, automation) * (rect.height - 12)
    }

    private func automationValue(atY pointY: CGFloat, in rect: NSRect,
                                 _ automation: TimelineModel.Automation) -> Float {
        let fraction = (rect.maxY - 6 - pointY) / max(1, rect.height - 12)
        return automationValueFor(fraction, automation)
    }

    private static let automationHandleRadius: CGFloat = 4

    private func automationPoint(at point: NSPoint, laneIndex: Int) -> Int? {
        guard let rect = automationRect(laneIndex),
              let automation = model.lanes[laneIndex].automation else { return nil }
        return automation.points.firstIndex { candidate in
            let centre = NSPoint(x: x(forSeconds: candidate.timeSeconds),
                                 y: automationY(candidate.value, in: rect, automation))
            return abs(centre.x - point.x) <= Self.automationHandleRadius + 3 &&
                   abs(centre.y - point.y) <= Self.automationHandleRadius + 3
        }
    }

    private func drawAutomation(_ context: CGContext) {
        for (index, lane) in model.lanes.enumerated() {
            guard let automation = lane.automation, let rect = automationRect(index) else { continue }

            NSColor(hex: 0x241f1b).setFill()
            rect.fill()
            NSColor(hex: 0x1b1611).setFill()
            NSRect(x: 0, y: rect.maxY - 1, width: bounds.width, height: 1).fill()

            // Header: the parameter's name, clickable to swap parameter.
            (automation.displayName as NSString).draw(
                at: NSPoint(x: 12, y: rect.minY + 6),
                withAttributes: [
                    .font: NSFont.monospacedSystemFont(ofSize: 9, weight: .medium),
                    .foregroundColor: NSColor(hex: 0x9a8f80),
                ])

            context.saveGState()
            // A scrolled automation row can reach up under the ruler.
            context.clip(to: NSRect(x: lanesRect.minX, y: rect.minY,
                                    width: lanesRect.width, height: rect.height)
                             .intersection(lanesRect))

            // The line the curve falls back to when there are no points at all.
            NSColor(hex: 0x453d34).setStroke()
            let baseline = NSBezierPath()
            let baselineY = automationY(automation.fallback, in: rect, automation)
            baseline.move(to: NSPoint(x: lanesRect.minX, y: baselineY))
            baseline.line(to: NSPoint(x: lanesRect.maxX, y: baselineY))
            baseline.lineWidth = 1
            baseline.setLineDash([2, 3], count: 2, phase: 0)
            baseline.stroke()

            guard !automation.points.isEmpty else {
                context.restoreGState()
                continue
            }

            // The curve holds its first and last value out to the edges, which is
            // exactly what automationValueAt does when it runs off either end.
            let curve = NSBezierPath()
            let first = automation.points[0]
            curve.move(to: NSPoint(x: lanesRect.minX,
                                   y: automationY(first.value, in: rect, automation)))
            for point in automation.points {
                curve.line(to: NSPoint(x: x(forSeconds: point.timeSeconds),
                                       y: automationY(point.value, in: rect, automation)))
            }
            if let last = automation.points.last {
                curve.line(to: NSPoint(x: lanesRect.maxX,
                                       y: automationY(last.value, in: rect, automation)))
            }
            lane.accent.withAlphaComponent(0.85).setStroke()
            curve.lineWidth = 1.5
            curve.stroke()

            for point in automation.points {
                let centre = NSPoint(x: x(forSeconds: point.timeSeconds),
                                     y: automationY(point.value, in: rect, automation))
                let handle = NSRect(x: centre.x - Self.automationHandleRadius,
                                    y: centre.y - Self.automationHandleRadius,
                                    width: Self.automationHandleRadius * 2,
                                    height: Self.automationHandleRadius * 2)
                NSColor(hex: 0x1b1611).setFill()
                NSBezierPath(ovalIn: handle).fill()
                lane.accent.setStroke()
                let ring = NSBezierPath(ovalIn: handle)
                ring.lineWidth = 1.5
                ring.stroke()
            }
            context.restoreGState()
        }
    }

    private func drawGrid(_ context: CGContext) {
        let secondsPerBar = 60.0 / Double(model.tempoBpm) * Double(model.beatsPerBar)
        let step = barStep()

        let firstBar = max(0, Int((model.visibleStart / secondsPerBar).rounded(.down)))
        let lastBar = Int(((model.visibleStart + model.visibleDuration) / secondsPerBar).rounded(.up))

        context.saveGState()
        context.clip(to: lanesRect)

        var bar = firstBar - firstBar % step
        while bar <= lastBar {
            let position = x(forSeconds: Double(bar) * secondsPerBar)
            NSColor(hex: 0x3d4650).withAlphaComponent(0.5).setFill()
            NSRect(x: position, y: lanesRect.minY, width: 1, height: lanesRect.height).fill()
            bar += step
        }
        context.restoreGState()
    }

    private func drawClips(_ context: CGContext) {
        context.saveGState()
        context.clip(to: lanesRect)

        for clip in model.clips {
            guard clip.laneIndex < model.lanes.count else { continue }
            let lane = model.lanes[clip.laneIndex]

            let left = x(forSeconds: clip.startSeconds)
            let right = x(forSeconds: clip.startSeconds + clip.durationSeconds)
            guard right > lanesRect.minX, left < lanesRect.maxX else { continue }

            let top = laneTop(clip.laneIndex) + 6
            let rect = NSRect(x: left, y: top, width: max(2, right - left), height: laneHeight(clip.laneIndex) - 12)

            let body = NSBezierPath(roundedRect: rect, xRadius: 4, yRadius: 4)
            NSColor(hex: clip.selected ? 0x2a4356 : 0x1e3140).setFill()
            body.fill()

            drawWaveform(clip, in: rect.insetBy(dx: 2, dy: 14), accent: lane.accent)
            drawFades(clip, in: rect)
            drawGainLine(clip, in: rect)

            // Name plate along the top of the clip.
            NSColor(hex: 0x0b0806).withAlphaComponent(0.55).setFill()
            NSRect(x: rect.minX, y: rect.minY, width: rect.width, height: 13).fill()

            context.saveGState()
            context.clip(to: rect)
            (clip.name as NSString).draw(
                at: NSPoint(x: rect.minX + 5, y: rect.minY + 1),
                withAttributes: [
                    .font: NSFont.systemFont(ofSize: 9, weight: .medium),
                    .foregroundColor: NSColor(hex: 0xddd5c8),
                ]
            )
            context.restoreGState()

            if clip.selected {
                NSColor(hex: 0xe6a23c).setStroke()
                body.lineWidth = 2
            } else {
                lane.accent.withAlphaComponent(0.7).setStroke()
                body.lineWidth = 1
            }
            body.stroke()

            // Muted clip: a translucent grey wash over the whole body so it reads as inactive
            // (Logic/Cubase style), on top of the already-dimmed waveform.
            if clip.muted {
                NSColor(hex: 0x0b0806).withAlphaComponent(0.5).setFill()
                NSBezierPath(roundedRect: rect, xRadius: 4, yRadius: 4).fill()
            }
        }

        context.restoreGState()
    }

    /// The live "recording" clips on the armed lane — one red-tinted body per punch region, each
    /// with its streaming waveform. Nothing draws during a plain background pass (empty array).
    private func drawRecordingClip(_ context: CGContext) {
        guard !recordingClips.isEmpty else { return }
        context.saveGState()
        context.clip(to: lanesRect)
        for clip in recordingClips {
            guard let laneIndex = model.lanes.firstIndex(where: { $0.trackId == clip.trackId }),
                  clip.durationSeconds > 0 else { continue }
            let left = x(forSeconds: clip.startSeconds)
            let right = x(forSeconds: clip.startSeconds + clip.durationSeconds)
            guard right > lanesRect.minX, left < lanesRect.maxX else { continue }
            let top = laneTop(laneIndex) + 6
            let rect = NSRect(x: left, y: top, width: max(2, right - left), height: laneHeight(laneIndex) - 12)

            let body = NSBezierPath(roundedRect: rect, xRadius: 4, yRadius: 4)
            NSColor(hex: 0x4a1f2a).setFill()
            body.fill()

            let wave = rect.insetBy(dx: 2, dy: 14)
            if recordingChannels > 1, !clip.peaksR.isEmpty {
                let gap: CGFloat = 2
                let half = (wave.height - gap) / 2
                drawRecordingEnvelope(clip.peaksL, in: NSRect(x: wave.minX, y: wave.minY, width: wave.width, height: half))
                drawRecordingEnvelope(clip.peaksR, in: NSRect(x: wave.minX, y: wave.maxY - half, width: wave.width, height: half))
            } else {
                drawRecordingEnvelope(clip.peaksL, in: wave)
            }

            NSColor(hex: 0x0b0806).withAlphaComponent(0.55).setFill()
            NSRect(x: rect.minX, y: rect.minY, width: rect.width, height: 13).fill()
            ("● REC" as NSString).draw(
                at: NSPoint(x: rect.minX + 5, y: rect.minY + 1),
                withAttributes: [
                    .font: NSFont.systemFont(ofSize: 9, weight: .bold),
                    .foregroundColor: NSColor(hex: 0xff6b81),
                ])

            NSColor(hex: 0xe0556a).setStroke()
            body.lineWidth = 1.5
            body.stroke()
        }
        context.restoreGState()
    }

    /// One symmetric live-record envelope from coarse mono peaks mapped across `rect`'s width.
    private func drawRecordingEnvelope(_ peaks: [Float], in rect: NSRect) {
        guard rect.width >= 1, !peaks.isEmpty, rect.height > 1 else { return }
        let midY = rect.midY
        let halfH = rect.height / 2
        let n = peaks.count
        let firstCol = Int(max(0, lanesRect.minX - rect.minX))
        let lastCol = Int(min(rect.width, lanesRect.maxX - rect.minX))
        guard lastCol > firstCol else { return }
        let path = NSBezierPath()
        for col in firstCol..<lastCol {
            let f0 = Double(col) / Double(rect.width)
            let f1 = Double(col + 1) / Double(rect.width)
            let i0 = min(n - 1, max(0, Int(f0 * Double(n))))
            let i1 = min(n - 1, max(i0, Int(f1 * Double(n))))
            var pk: Float = 0
            for i in i0...i1 { pk = max(pk, peaks[i]) }
            let h = max(0.5, min(halfH, CGFloat(pk) * halfH))
            let px = rect.minX + CGFloat(col) + 0.5
            path.move(to: NSPoint(x: px, y: midY - h))
            path.line(to: NSPoint(x: px, y: midY + h))
        }
        NSColor(hex: 0xff9fb0).withAlphaComponent(0.95).setStroke()
        path.lineWidth = 1
        path.stroke()
    }

    /// Pro Tools crossfade: where two same-lane clips overlap, draw the X — the earlier clip's
    /// fade-out crossing the later clip's fade-in — in a boxed region, so the crossfade reads as the
    /// two clips crossing rather than one just covering the other.
    private func drawCrossfades(_ context: CGContext) {
        context.saveGState()
        context.clip(to: lanesRect)
        let byLane = Dictionary(grouping: model.clips.filter { $0.laneIndex < model.lanes.count }) { $0.laneIndex }
        for (lane, clips) in byLane {
            let sorted = clips.sorted { $0.startSeconds < $1.startSeconds }
            for i in 0..<max(0, sorted.count - 1) {
                let a = sorted[i], b = sorted[i + 1]
                let aEnd = a.startSeconds + a.durationSeconds
                let overlapEnd = min(aEnd, b.startSeconds + b.durationSeconds)
                guard b.startSeconds < aEnd - 1e-6, overlapEnd > b.startSeconds else { continue }  // real overlap
                // Any same-track overlap IS a crossfade now — the render derives it from the overlap
                // itself (max with each clip's manual fade), so it draws wherever clips overlap and
                // clears the instant they are pulled apart. No baked fade to check.
                let left = x(forSeconds: b.startSeconds)
                let right = x(forSeconds: overlapEnd)
                guard right > left + 0.5, right > lanesRect.minX, left < lanesRect.maxX else { continue }

                let top = laneTop(lane) + 6 + 13                       // below the clip name bar
                let bottom = laneTop(lane) + laneHeight(lane) - 6
                guard bottom > top else { continue }
                let l = max(left, lanesRect.minX), r = min(right, lanesRect.maxX)

                // Tint the overlap so it stands apart from the two clip bodies.
                NSColor(hex: 0x0b0806).withAlphaComponent(0.30).setFill()
                NSRect(x: l, y: top, width: r - l, height: bottom - top).fill()

                // The crossing fade curves, sampled from each clip's actual curve so the picture
                // matches the sound (top = full level, bottom = silence). A (earlier) fades out with
                // its fadeOutCurve; B (later) fades in with its fadeInCurve.
                let steps = 24
                let outCurve = NSBezierPath()   // A: full→silence, left→right
                let inCurve = NSBezierPath()     // B: silence→full, left→right
                for i in 0...steps {
                    let t = CGFloat(i) / CGFloat(steps)
                    let px = left + (right - left) * t
                    let outGain = Self.fadeCurveGain(a.fadeOutCurve, Self.fadeCurvature(1 - t, a.fadeOutCurvature))
                    let inGain = Self.fadeCurveGain(b.fadeInCurve, Self.fadeCurvature(t, b.fadeInCurvature))
                    let outY = top + (1 - outGain) * (bottom - top)
                    let inY = top + (1 - inGain) * (bottom - top)
                    if i == 0 { outCurve.move(to: NSPoint(x: px, y: outY)); inCurve.move(to: NSPoint(x: px, y: inY)) }
                    else { outCurve.line(to: NSPoint(x: px, y: outY)); inCurve.line(to: NSPoint(x: px, y: inY)) }
                }
                NSColor(hex: 0xf0c674).withAlphaComponent(0.9).setStroke()
                outCurve.lineWidth = 1.2; outCurve.stroke()
                inCurve.lineWidth = 1.2; inCurve.stroke()

                // Box outline around the crossfade.
                let box = NSBezierPath(rect: NSRect(x: l, y: top, width: r - l, height: bottom - top))
                NSColor(hex: 0xe6a23c).withAlphaComponent(0.55).setStroke()
                box.lineWidth = 1
                box.stroke()
            }
        }
        context.restoreGState()
    }

    private func gainLineY(_ clip: TimelineModel.Clip, in rect: NSRect) -> CGFloat {
        let span = Self.gainRange.upperBound - Self.gainRange.lowerBound
        let fraction = (clip.gainDb - Self.gainRange.lowerBound) / span
        let y = rect.maxY - CGFloat(fraction) * rect.height
        // Keep the line — and its grab band and dB label — fully inside the clip and readable.
        // Bottom: 6 pt so the -24 dB floor doesn't sit on the edge with half its grab band in the next
        // lane (which made it impossible to catch and drag back up). Top: enough to clear the 13 pt name
        // bar PLUS the dB label, which is drawn 12 pt above the line — otherwise raising the gain slides
        // the line up under the name and its own value disappears.
        let topMargin: CGFloat = 26
        let bottomMargin: CGFloat = 6
        guard rect.height > topMargin + bottomMargin else { return rect.midY }
        return min(rect.maxY - bottomMargin, max(rect.minY + topMargin, y))
    }

    /// The render's fade curve, replicated for drawing so the picture matches the sound exactly
    /// (see fadeCurveGain in ProjectAudioRenderer.cpp): linear=x, slow=x², fast=√x, else equal-power.
    static func fadeCurveGain(_ curve: String, _ t: CGFloat) -> CGFloat {
        let x = max(0, min(1, t))
        switch curve {
        case "linear": return x
        case "slow": return x * x
        case "fast": return sqrt(x)
        default: return sin(x * .pi / 2)   // equal_power
        }
    }

    /// Continuous shape bend applied to the fade position before the curve — matches the render's
    /// applyFadeCurvature so the drawing and the sound agree. curvature 0 = unchanged.
    static func fadeCurvature(_ t: CGFloat, _ curvature: Double) -> CGFloat {
        let x = max(0, min(1, t))
        let c = max(-1.0, min(1.0, curvature))
        if c == 0 { return x }
        return pow(x, CGFloat(pow(2.0, c * 2.0)))
    }

    /// Fades are drawn as the region the clip loses: the area ABOVE the gain curve, darkened.
    /// The top edge follows the fade curve (not a straight line), so changing the curve reshapes
    /// the picture the same way it reshapes the sound. Handles ride where the fade ends.
    private func drawFades(_ clip: TimelineModel.Clip, in rect: NSRect) {
        NSColor(hex: 0x0b0806).withAlphaComponent(0.55).setFill()
        let steps = 24

        if clip.fadeInSeconds > 0 {
            let end = x(forSeconds: clip.startSeconds + clip.fadeInSeconds)
            // The lost region is above the rising gain curve: silence at the left edge up to full at `end`.
            let wedge = NSBezierPath()
            wedge.move(to: NSPoint(x: rect.minX, y: rect.minY))
            for i in 0...steps {
                let t = CGFloat(i) / CGFloat(steps)
                let px = rect.minX + (end - rect.minX) * t
                let gain = Self.fadeCurveGain(clip.fadeInCurve, Self.fadeCurvature(t, clip.fadeInCurvature))
                wedge.line(to: NSPoint(x: px, y: rect.minY + gain * rect.height))
            }
            wedge.line(to: NSPoint(x: rect.minX, y: rect.maxY))
            wedge.close()
            wedge.fill()
        }
        if clip.fadeOutSeconds > 0 {
            let begin = x(forSeconds: clip.startSeconds + clip.durationSeconds - clip.fadeOutSeconds)
            // The lost region is above the falling gain curve: full at `begin` down to silence at the right.
            let wedge = NSBezierPath()
            wedge.move(to: NSPoint(x: rect.maxX, y: rect.minY))
            for i in 0...steps {
                let t = CGFloat(i) / CGFloat(steps)
                let px = begin + (rect.maxX - begin) * t
                let gain = Self.fadeCurveGain(clip.fadeOutCurve, Self.fadeCurvature(1 - t, clip.fadeOutCurvature))
                wedge.line(to: NSPoint(x: px, y: rect.minY + gain * rect.height))
            }
            wedge.line(to: NSPoint(x: rect.maxX, y: rect.maxY))
            wedge.close()
            wedge.fill()
        }

        // Both handles belong to one clip: on a multi-selection a drag moves the
        // whole thing, so drawing grab points there would be a lie.
        guard clip.selected, selectionCount == 1 else { return }
        NSColor(hex: 0xe6a23c).setFill()
        for position in [x(forSeconds: clip.startSeconds + clip.fadeInSeconds),
                         x(forSeconds: clip.startSeconds + clip.durationSeconds - clip.fadeOutSeconds)] {
            guard position >= rect.minX, position <= rect.maxX else { continue }
            NSBezierPath(ovalIn: NSRect(x: position - 3, y: rect.minY + 14, width: 6, height: 6)).fill()
        }
    }

    /// Only a lone selected clip shows its gain line; otherwise the lane is noisy.
    private func drawGainLine(_ clip: TimelineModel.Clip, in rect: NSRect) {
        guard clip.selected, selectionCount == 1 else { return }
        let lineY = gainLineY(clip, in: rect)

        NSColor(hex: 0xe6a23c).withAlphaComponent(0.8).setStroke()
        let line = NSBezierPath()
        line.move(to: NSPoint(x: rect.minX + 2, y: lineY))
        line.line(to: NSPoint(x: rect.maxX - 2, y: lineY))
        line.lineWidth = 1
        line.stroke()

        let label = String(format: "%+.1f dB", clip.gainDb) as NSString
        label.draw(at: NSPoint(x: rect.minX + 6, y: lineY - 12),
                   withAttributes: [
                       .font: NSFont.monospacedSystemFont(ofSize: 8, weight: .medium),
                       .foregroundColor: NSColor(hex: 0xe6a23c),
                   ])
    }

    /// Resamples the cached peak buckets onto the clip's on-screen width. A stereo clip
    /// draws two envelopes — L on the top half, R on the bottom — while a mono clip draws
    /// one centered. Only the visible span is walked, so zooming in does not cost more.
    private func drawWaveform(_ clip: TimelineModel.Clip, in rect: NSRect, accent: NSColor) {
        guard rect.width >= 1, let peaks = waveforms[clip.sourcePath] else { return }
        let gainFactor = CGFloat(pow(10, clip.gainDb / 20))

        if peaks.channels > 1, !peaks.minsR.isEmpty {
            let gap: CGFloat = 2
            let half = (rect.height - gap) / 2
            let top = NSRect(x: rect.minX, y: rect.minY, width: rect.width, height: half)
            let bottom = NSRect(x: rect.minX, y: rect.maxY - half, width: rect.width, height: half)
            drawEnvelope(clip, mins: peaks.minsL, maxs: peaks.maxsL, fileDuration: peaks.durationSeconds,
                         in: top, gainFactor: gainFactor, accent: accent)
            drawEnvelope(clip, mins: peaks.minsR, maxs: peaks.maxsR, fileDuration: peaks.durationSeconds,
                         in: bottom, gainFactor: gainFactor, accent: accent)
        } else {
            drawEnvelope(clip, mins: peaks.minsL, maxs: peaks.maxsL, fileDuration: peaks.durationSeconds,
                         in: rect, gainFactor: gainFactor, accent: accent)
        }
    }

    private func drawEnvelope(_ clip: TimelineModel.Clip, mins: [Float], maxs: [Float],
                              fileDuration: Double, in rect: NSRect, gainFactor: CGFloat, accent: NSColor) {
        guard rect.width >= 1, !mins.isEmpty, rect.height > 1 else { return }
        let midY = rect.midY
        let halfHeight = rect.height / 2
        let buckets = mins.count

        let path = NSBezierPath()
        var drew = false

        let firstColumn = Int(max(0, lanesRect.minX - rect.minX))
        let lastColumn = Int(min(rect.width, lanesRect.maxX - rect.minX))
        guard lastColumn > firstColumn else { return }

        // The peaks span the whole file. A trimmed or split clip plays a window of it,
        // so map each on-screen column into that window rather than the file.
        let windowStart = fileDuration > 0 ? clip.sourceOffsetSeconds / fileDuration : 0
        let windowSpan = fileDuration > 0 ? clip.durationSeconds / fileDuration : 1

        for column in firstColumn..<lastColumn {
            // Reverse: draw the source window back-to-front so the picture mirrors what actually plays.
            let mappedColumn = clip.reversed ? (Double(rect.width) - 1 - Double(column)) : Double(column)
            let startFraction = windowStart + mappedColumn / Double(rect.width) * windowSpan
            let endFraction = windowStart + (mappedColumn + 1) / Double(rect.width) * windowSpan
            let first = min(buckets - 1, max(0, Int(startFraction * Double(buckets))))
            let last = min(buckets - 1, max(first, Int(endFraction * Double(buckets))))

            var low: Float = 0
            var high: Float = 0
            for bucket in first...last {
                low = min(low, mins[bucket])
                high = max(high, maxs[bucket])
            }
            // Polarity invert: flip the envelope vertically (max↔−min).
            if clip.polarityInverted { let h = high; high = -low; low = -h }
            let pointX = rect.minX + CGFloat(column) + 0.5
            let highY = max(-halfHeight, min(halfHeight, CGFloat(high) * gainFactor * halfHeight))
            let lowY = max(-halfHeight, min(halfHeight, CGFloat(low) * gainFactor * halfHeight))
            path.move(to: NSPoint(x: pointX, y: midY - highY))
            path.line(to: NSPoint(x: pointX, y: midY - lowY))
            drew = true
        }

        guard drew else { return }
        // Muted clips read as inactive — dim the waveform well down.
        accent.withAlphaComponent(clip.muted ? 0.22 : 0.85).setStroke()
        path.lineWidth = 1
        path.stroke()
    }
}

extension NSColor {
    /// Theme colours are declared once, in SwiftUI terms; the AppKit view borrows them.
    static func from(_ color: Color) -> NSColor {
        NSColor(color)
    }

    convenience init(hex: UInt32) {
        self.init(srgbRed: CGFloat((hex >> 16) & 0xff) / 255,
                  green: CGFloat((hex >> 8) & 0xff) / 255,
                  blue: CGFloat(hex & 0xff) / 255,
                  alpha: 1)
    }
}

/// SwiftUI wrapper. The playhead is pushed straight at the layer so the 30 Hz tick
/// never invalidates the waveform drawing.
struct TimelineView: NSViewRepresentable {
    let model: TimelineModel
    // NOT observed: the NSView subscribes to the clock itself (bindPlayheadClock) and moves only its
    // playhead layer, so the 30 Hz playhead never touches SwiftUI at all.
    let playheadClock: PlayheadClock
    /// Also NOT observed, for the same reason: the NSView subscribes to the meter store itself and
    /// redraws only the header column.
    let trackMeters: EngineController.TrackMeters
    var isTransportRunning: Bool = false
    let waveforms: [String: EngineController.WaveformData]
    // Live audio-record clips (empty = not recording / plain background pass).
    var recordingClips: [EngineController.RecordingClip] = []
    var recordingChannels: Int = 2
    let onSeek: (Double) -> Void
    let onZoom: (Double, Double) -> Void
    let onSelect: (String?) -> Void
    let onSetRange: (Double, Double) -> Void
    let onSetRangeLane: (Int?) -> Void
    let onSelectRegion: (String?) -> Void
    let onOpenRegion: (String) -> Void
    var onMergeRegionForward: (String) -> Void = { _ in }
    var onMergeRegionsOnTrack: (String) -> Void = { _ in }
    let onMoveRegion: (String, Int, Double) -> Void
    let onResizeRegion: (String, Double) -> Void
    let onAddRegion: (Int, Double) -> Void
    let onDropAudio: (Int, Double, [URL]) -> Void
    var onDropMidi: ((Int, Double, [URL]) -> Void)? = nil
    let onMoveMarker: (Double, Double) -> Void
    let onDeleteMarker: (Double) -> Void
    let onSelectBetweenMarkers: (Double) -> Void
    let onToggleAutomation: (Int) -> Void
    let onCycleAutomationParameter: (Int) -> Void
    var onAutomationParamOptions: ((Int) -> [(id: String, name: String, on: Bool)])? = nil
    var onSetAutomationParam: ((Int, String) -> Void)? = nil
    var onSetLaneHeight: (([Int], CGFloat) -> Void)? = nil
    var onCommitLaneHeight: (() -> Void)? = nil
    var onReorderTrack: ((Int, Int, Bool) -> Void)? = nil
    var onFadeCurveOptions: (() -> [(label: String, id: String)])? = nil
    var onClipCurrentFades: ((String) -> (inCurve: String, outCurve: String))? = nil
    var onSetClipFadeInCurve: ((String, String) -> Void)? = nil
    var onSetClipFadeOutCurve: ((String, String) -> Void)? = nil
    var onReverseClip: ((String) -> Void)? = nil
    var onNormalizeClip: ((String) -> Void)? = nil
    var onToggleClipMute: ((String) -> Void)? = nil
    var onToggleClipPolarity: ((String) -> Void)? = nil
    var onApplyClipTimePitch: ((String, Double, Double) -> Void)? = nil
    var onDenoiseClip: ((String) -> Void)? = nil
    var onOpenAraEditor: ((String, String, String) -> Void)? = nil
    var onClearAraEdits: ((String) -> Void)? = nil
    var araPlugins: [(name: String, path: String)] = []
    var clipHasAraEdits: ((String) -> Bool)? = nil
    var onAlignToReference: ((String, String) -> Void)? = nil
    var alignStrength: Double = 1.0
    var onSetAlignStrength: ((Double) -> Void)? = nil
    var onSeparateStems: ((String) -> Void)? = nil
    var onSeparateStemsPreset: ((String, String) -> Void)? = nil
    var stem6sAvailable: Bool = false
    var drumSplitAvailable: Bool = false
    var orchestraSeparationAvailable: Bool = false
    var onConvertToMidi: ((String) -> Void)? = nil
    var onConvertToMidiPoly: ((String) -> Void)? = nil
    var convertToMidiPolyAvailable: Bool = false
    var onOpenPitchEditor: ((String) -> Void)? = nil
    var onSetCrossfadeLength: ((String, String, Double) -> Void)? = nil
    var onSetFadeCurvature: ((String, Bool, Double) -> Void)? = nil
    var auditionRoll: (() -> Double)? = nil
    var onSetAuditionRoll: ((Double) -> Void)? = nil
    var onAuditionRegion: ((Double, Double, Bool) -> Void)? = nil
    var onStopAudition: (() -> Void)? = nil
    var onClipOriginalStart: ((String) -> Double)? = nil
    var onSpotClips: (([String]) -> Void)? = nil
    let onAddAutomationPoint: (Int, Double, Float) -> Void
    let onMoveAutomationPoint: (Int, Int, Double, Float) -> Void
    let onDeleteAutomationPoint: (Int, Int) -> Void
    let onToggleSelect: (String) -> Void
    let onSelectMany: ([String]) -> Void
    let onMoveClip: (String, Double) -> Void
    var onDropCopy: ((String, Int, Double) -> Void)? = nil
    var onDropCopyToNewTrack: ((String, Double) -> Void)? = nil
    var onSplitClip: ((String, Double) -> Void)? = nil
    var editTool: String = "smart"
    var onBeginCopySelection: ((String) -> String?)? = nil
    let onMoveSelection: (Double) -> Void
    let onTrimStart: (String, Double) -> Void
    let onTrimEnd: (String, Double) -> Void
    var onRollBoundary: ((String, String, Double) -> Void)? = nil
    let onSetFades: (String, Double, Double) -> Void
    let onSetGain: (String, Float) -> Void
    let onCommitGain: (String) -> Void
    let onSelectLane: (Int, Bool) -> Void
    let onMoveClipToLane: (String, Int, Double) -> Void
    let onDropClipToNewTrack: (String, Double) -> Void
    let onCommitEdit: (String) -> Void
    let snap: (Double) -> Double
    let onToggleMute: (Int) -> Void
    let onToggleSolo: (Int) -> Void
    var soloSelectMode: String = "additive"
    var onSetSoloSelectMode: ((String) -> Void)? = nil
    var onClearAllSolos: (() -> Void)? = nil
    let onToggleArm: (Int) -> Void
    let onToggleInputMonitor: (Int) -> Void
    var onRenameTrack: ((Int, String) -> Void)? = nil
    let onSetVolumeDb: (Int, Float) -> Void
    var onSetPan: ((Int, Float) -> Void)? = nil
    var onCycleAutomationMode: ((Int) -> Void)? = nil
    var onBeginTouch: ((Int, String) -> Void)? = nil
    var onEndTouch: ((Int, String) -> Void)? = nil
    var onToggleTimebase: ((RulerTimebase) -> Void)? = nil
    var onBrowseInsert: ((Int) -> Void)? = nil
    var onToggleInsertEditor: ((Int, Int) -> Void)? = nil
    var onBypassInsert: ((Int, Int) -> Void)? = nil
    var onRemoveInsert: ((Int, Int) -> Void)? = nil
    var onAddSend: ((Int, String) -> Void)? = nil
    var onRemoveSend: ((Int, Int) -> Void)? = nil
    var onSetSendGain: ((Int, Int, Float) -> Void)? = nil
    var onSetSendPan: ((Int, Int, Float) -> Void)? = nil
    var onSetSendPreFader: ((Int, Int, Bool) -> Void)? = nil
    var onAddAux: (() -> Void)? = nil
    var onSendBusOptions: ((Int) -> [String])? = nil

    func makeNSView(context: Context) -> TimelineNSView {
        let view = TimelineNSView(frame: .zero)
        view.bindPlayheadClock(playheadClock)
        view.bindTrackMeters(trackMeters)
        wire(view)
        return view
    }

    func updateNSView(_ view: TimelineNSView, context: Context) {
        wire(view)
        if view.waveforms.keys != waveforms.keys {
            view.waveforms = waveforms
        }
        view.model = model
        view.isTransportRunning = isTransportRunning
        view.recordingChannels = recordingChannels
        view.recordingClips = recordingClips            // didSet repaints
    }

    private func wire(_ view: TimelineNSView) {
        view.onSeek = onSeek
        view.onToggleTimebase = onToggleTimebase
        view.onBrowseInsert = onBrowseInsert
        view.onToggleInsertEditor = onToggleInsertEditor
        view.onBypassInsert = onBypassInsert
        view.onRemoveInsert = onRemoveInsert
        view.onAddSend = onAddSend
        view.onRemoveSend = onRemoveSend
        view.onSetSendGain = onSetSendGain
        view.onSetSendPan = onSetSendPan
        view.onSetSendPreFader = onSetSendPreFader
        view.onAddAux = onAddAux
        view.onSendBusOptions = onSendBusOptions
        view.onZoom = onZoom
        view.onSelect = onSelect
        view.onSetRange = onSetRange
        view.onSetRangeLane = onSetRangeLane
        view.onSelectRegion = onSelectRegion
        view.onOpenRegion = onOpenRegion
        view.onMergeRegionForward = onMergeRegionForward
        view.onMergeRegionsOnTrack = onMergeRegionsOnTrack
        view.onMoveRegion = onMoveRegion
        view.onResizeRegion = onResizeRegion
        view.onAddRegion = onAddRegion
        view.onDropAudio = onDropAudio
        view.onDropMidi = onDropMidi
        view.onMoveMarker = onMoveMarker
        view.onDeleteMarker = onDeleteMarker
        view.onSelectBetweenMarkers = onSelectBetweenMarkers
        view.onToggleAutomation = onToggleAutomation
        view.onCycleAutomationParameter = onCycleAutomationParameter
        view.onAutomationParamOptions = onAutomationParamOptions
        view.onSetAutomationParam = onSetAutomationParam
        view.onSetLaneHeight = onSetLaneHeight
        view.onCommitLaneHeight = onCommitLaneHeight
        view.onReorderTrack = onReorderTrack
        view.onFadeCurveOptions = onFadeCurveOptions
        view.onClipCurrentFades = onClipCurrentFades
        view.onClipOriginalStart = onClipOriginalStart
        view.onSpotClips = onSpotClips
        view.onSetClipFadeInCurve = onSetClipFadeInCurve
        view.onSetClipFadeOutCurve = onSetClipFadeOutCurve
        view.onReverseClip = onReverseClip
        view.onNormalizeClip = onNormalizeClip
        view.onApplyClipTimePitch = onApplyClipTimePitch
        view.onDenoiseClip = onDenoiseClip
        view.onOpenAraEditor = onOpenAraEditor
        view.onClearAraEdits = onClearAraEdits
        view.araPlugins = araPlugins
        view.clipHasAraEdits = clipHasAraEdits
        view.onAlignToReference = onAlignToReference
        view.alignStrength = alignStrength
        view.onSetAlignStrength = onSetAlignStrength
        view.onSeparateStems = onSeparateStems
        view.onSeparateStemsPreset = onSeparateStemsPreset
        view.stem6sAvailable = stem6sAvailable
        view.drumSplitAvailable = drumSplitAvailable
        view.orchestraSeparationAvailable = orchestraSeparationAvailable
        view.onConvertToMidi = onConvertToMidi
        view.onConvertToMidiPoly = onConvertToMidiPoly
        view.convertToMidiPolyAvailable = convertToMidiPolyAvailable
        view.onOpenPitchEditor = onOpenPitchEditor
        view.onToggleClipMute = onToggleClipMute
        view.onToggleClipPolarity = onToggleClipPolarity
        view.onSetCrossfadeLength = onSetCrossfadeLength
        view.onSetFadeCurvature = onSetFadeCurvature
        view.auditionRoll = auditionRoll
        view.onSetAuditionRoll = onSetAuditionRoll
        view.onAuditionRegion = onAuditionRegion
        view.onStopAudition = onStopAudition
        view.onAddAutomationPoint = onAddAutomationPoint
        view.onMoveAutomationPoint = onMoveAutomationPoint
        view.onDeleteAutomationPoint = onDeleteAutomationPoint
        view.onToggleSelect = onToggleSelect
        view.onSelectMany = onSelectMany
        view.onMoveClip = onMoveClip
        view.onDropCopy = onDropCopy
        view.onDropCopyToNewTrack = onDropCopyToNewTrack
        view.onSplitClip = onSplitClip
        view.editTool = editTool
        view.onMoveSelection = onMoveSelection
        view.onBeginCopySelection = onBeginCopySelection
        view.onTrimStart = onTrimStart
        view.onTrimEnd = onTrimEnd
        view.onRollBoundary = onRollBoundary
        view.onSetFades = onSetFades
        view.onSetGain = onSetGain
        view.onCommitGain = onCommitGain
        view.onSelectLane = onSelectLane
        view.onMoveClipToLane = onMoveClipToLane
        view.onDropClipToNewTrack = onDropClipToNewTrack
        view.onCommitEdit = onCommitEdit
        view.snap = snap
        view.onToggleMute = onToggleMute
        view.onToggleSolo = onToggleSolo
        view.soloSelectMode = soloSelectMode
        view.onSetSoloSelectMode = onSetSoloSelectMode
        view.onClearAllSolos = onClearAllSolos
        view.onToggleArm = onToggleArm
        view.onToggleInputMonitor = onToggleInputMonitor
        view.onRenameTrack = onRenameTrack
        view.onSetVolumeDb = onSetVolumeDb
        view.onSetPan = onSetPan
        view.onCycleAutomationMode = onCycleAutomationMode
        view.onBeginTouch = onBeginTouch
        view.onEndTouch = onEndTouch
    }
}
