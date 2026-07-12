import AppKit
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
        var peakLeft: Float = 0
        var peakRight: Float = 0
        /// Read / Touch / Latch / Write / Off — drawn as a coloured letter the user cycles.
        var automationMode: String = "read"
        /// nil while the lane's automation is folded away.
        var automation: Automation?
        /// The channel-strip inserts and sends, shown in the header like Pro Tools' edit
        /// window. Inserts are the first few slots; sends are the active ones.
        var inserts: [InsertChip] = []
        var sends: [SendChip] = []
        /// This lane's height. Per-track so a multi-selection can be resized together.
        var height: CGFloat = 118
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
        let gainDb: Float
    }

    var lanes: [Lane] = []
    var clips: [Clip] = []
    var tempoBpm: Int = 120
    var beatsPerBar: Int = 4
    var sampleRate: Double = 48000
    /// Shared, adjustable lane height (drag a lane's bottom edge to change it).
    var laneHeight: CGFloat = 118
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

    /// Peaks by source path, supplied by the owner. Decoding lives in the engine.
    var waveforms: [String: EngineController.WaveformData] = [:] {
        didSet { needsDisplay = true }
    }

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
    var onSelectRegion: ((String?) -> Void)?
    var onOpenRegion: ((String) -> Void)?
    var onMoveRegion: ((String, Int, Double) -> Void)?   // (id, lane, start) — continuous
    var onResizeRegion: ((String, Double) -> Void)?      // (id, duration) — continuous
    var onAddRegion: ((Int, Double) -> Void)?            // (lane, start)
    var onDropAudio: ((Int, Double, [URL]) -> Void)?     // (lane, start, file urls)
    var onMoveMarker: ((Double, Double) -> Void)?        // (from, to) — continuous
    var onDeleteMarker: ((Double) -> Void)?
    var onSelectBetweenMarkers: ((Double) -> Void)?
    var onToggleAutomation: ((Int) -> Void)?             // lane index
    var onCycleAutomationParameter: ((Int) -> Void)?     // lane index
    var onAutomationParamOptions: ((Int) -> [(id: String, name: String, on: Bool)])?
    var onSetAutomationParam: ((Int, String) -> Void)?
    var onSetLaneHeight: (([Int], CGFloat) -> Void)?
    var onCommitLaneHeight: (() -> Void)?
    var onFadeCurveOptions: (() -> [(label: String, id: String)])?
    var onClipCurrentFades: ((String) -> (inCurve: String, outCurve: String))?
    var onSetClipFadeInCurve: ((String, String) -> Void)?
    var onSetClipFadeOutCurve: ((String, String) -> Void)?
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
    var onTrimStart: ((String, Double) -> Void)?         // (clipId, newStart)
    var onTrimEnd: ((String, Double) -> Void)?           // (clipId, newEnd)
    var onSetFades: ((String, Double, Double) -> Void)?  // (clipId, fadeIn, fadeOut)
    var onSetGain: ((String, Float) -> Void)?
    var onCommitGain: ((String) -> Void)?                // drag-end: reconcile + record
    var onSelectLane: ((Int) -> Void)?
    var onMoveClipToLane: ((String, Int, Double) -> Void)?  // (clipId, laneIndex, start)
    var onDropClipToNewTrack: ((String, Double) -> Void)?   // drop past last lane → new track
    var onCommitEdit: ((String) -> Void)?                // step name
    var snap: ((Double) -> Double)?

    // Inline lane-header channel strip: no mixer trip to mute/solo/arm or set level.
    var onToggleMute: ((Int) -> Void)?                   // trackId
    var onToggleSolo: ((Int) -> Void)?                   // trackId
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
        case marquee(origin: NSPoint, current: NSPoint)
        case rangingFrom(seconds: Double)
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
        case trimmingStart(clipId: String)
        case trimmingEnd(clipId: String)
        case fadingIn(clip: TimelineModel.Clip)
        case fadingOut(clip: TimelineModel.Clip)
        case gaining(clip: TimelineModel.Clip, grabY: CGFloat, startGainDb: Float)
        /// Dragging the inline volume fader in a lane header.
        case headerFader(trackId: Int)
        case headerPan(trackId: Int)
        /// Dragging a lane's bottom edge to resize all lanes.
        case resizingLane(startHeight: CGFloat, startY: CGFloat, laneIndex: Int)
    }

    /// Clip gain is drawn as a horizontal line across this dB span.
    private static let gainRange: ClosedRange<Float> = -24...12
    private static let fadeHandleSize: CGFloat = 9

    private var drag = Drag.none

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
    static let defaultLaneHeight: CGFloat = 106
    static let minLaneHeight: CGFloat = 40
    static let maxLaneHeight: CGFloat = 320
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

        // Clicking a lane header selects that track; the "A" chip folds automation out;
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
                } else if headerAutomationModeRect(lane).contains(point) {
                    onCycleAutomationMode?(trackId)
                } else if headerMuteRect(lane).contains(point) {
                    onToggleMute?(trackId)
                } else if headerSoloRect(lane).contains(point) {
                    onToggleSolo?(trackId)
                } else if headerArmRect(lane).contains(point) {
                    onToggleArm?(trackId)
                } else if headerInputMonitorRect(lane).contains(point) {
                    onToggleInputMonitor?(trackId)
                } else if headerFaderRect(lane).insetBy(dx: 0, dy: -5).contains(point) {
                    onSelectLane?(lane)
                    onBeginTouch?(trackId, "track.volume")
                    onSetVolumeDb?(trackId, headerFaderDb(atX: point.x, index: lane))
                    drag = .headerFader(trackId: trackId)
                } else if headerPanRect(lane).insetBy(dx: 0, dy: -5).contains(point) {
                    onSelectLane?(lane)
                    onBeginTouch?(trackId, "track.pan")
                    onSetPan?(trackId, headerPan(atX: point.x, index: lane))
                    drag = .headerPan(trackId: trackId)
                } else if let slot = headerInsertSlotHit(lane, point: point) {
                    handleInsertClick(trackId: trackId, lane: lane, slot: slot, command: event.modifierFlags.contains(.command))
                } else if let s = headerSendSlotHit(lane, point: point) {
                    handleSendClick(trackId: trackId, lane: lane, slot: s, event: event)
                } else {
                    onSelectLane?(lane)
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
            drag = .rangingFrom(seconds: origin)
            return
        }

        // The rest of the ruler carries the markers, and otherwise scrubs.
        if point.y < rulerHeight {
            if let hit = marker(at: point) {
                if event.clickCount >= 2 {
                    onDeleteMarker?(hit.timeSeconds)
                } else if event.modifierFlags.contains(.shift) {
                    onSelectBetweenMarkers?(hit.timeSeconds + 0.001)
                } else {
                    drag = .movingMarker(fromSeconds: hit.timeSeconds)
                }
                return
            }
            drag = .seeking
            onSeek?(max(0, seconds(atX: point.x)))
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

        // Double-clicking an empty instrument lane starts a new part there.
        if event.clickCount >= 2, let lane = laneIndex(at: point) {
            onAddRegion?(lane, max(0, snapped(seconds(atX: point.x))))
            return
        }

        // Dragging from empty lane space sweeps a selection rectangle.
        guard let hit = clip(at: point) else {
            if !event.modifierFlags.contains(.shift) {
                onSelect?(nil)
            }
            drag = .marquee(origin: point, current: point)
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
            // Trimming a whole selection has no obvious meaning; a drag moves it.
            drag = .movingSelection(anchorId: hit.id, grabOffsetSeconds: grabOffset,
                                    startX: point.x, axisUnlocked: false)
        } else if point.x - rect.minX <= Self.trimHandleWidth {
            drag = .trimmingStart(clipId: hit.id)
        } else if rect.maxX - point.x <= Self.trimHandleWidth {
            drag = .trimmingEnd(clipId: hit.id)
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
        let time = max(0, seconds(atX: point.x))

        switch drag {
        case .none:
            break
        case .seeking:
            onSeek?(time)
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
        case .trimmingStart(let clipId):
            onTrimStart?(clipId, snapped(time))
        case .trimmingEnd(let clipId):
            onTrimEnd?(clipId, snapped(time))
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
            // The fader's x-mapping is the same for every lane, so index 0 suffices.
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
        }
    }

    override func mouseUp(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)

        switch drag {
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
        case .fadingIn, .fadingOut:
            onCommitEdit?("Clip fade")
        case .gaining(let clip, _, _):
            onCommitGain?(clip.id)
        case .movingSelection:
            onCommitEdit?("Move clips")
        case .marquee(let origin, let current):
            if origin == current {
                // A click, not a sweep: the playhead follows, as it always has.
                onSeek?(max(0, seconds(atX: point.x)))
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
        case .none, .seeking, .rangingFrom, .rangingEdgeStart, .rangingEdgeEnd, .movingRange:
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
    }

    /// Scroll pans; ⌘-scroll (or a pinch) zooms about the pointer.
    override func scrollWheel(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)

        if event.modifierFlags.contains(.command) || event.phase == .changed && event.magnification != 0 {
            let anchor = seconds(atX: point.x)
            let factor = event.scrollingDeltaY > 0 ? 0.9 : 1.1
            let duration = min(600, max(0.25, model.visibleDuration * factor))

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
        guard let target = dropTarget(at: point), !audioURLs(from: sender).isEmpty else {
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
        drawDragGhost(context)
        drawMidiRegions(context)
        drawAutomation(context)
        // Last, so a marker's hairline is not painted over by the clips it lines up with.
        drawMarkers(context)

        // The lane header column sits above the grid but below the playhead.
        NSColor(hex: 0x0b0806).setFill()
        NSRect(x: Self.headerWidth - 1, y: 0, width: 1, height: bounds.height).fill()

        drawMarquee(context)
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

            (lane.name as NSString).draw(at: NSPoint(x: 12, y: rect.minY + 10),
                                         withAttributes: nameAttributes)

            // The button that folds the automation row out from under the lane.
            let toggle = automationToggleRect(index)
            NSColor(hex: lane.automation != nil ? 0x5f9fd6 : 0x453d34).setFill()
            NSBezierPath(roundedRect: toggle, xRadius: 3, yRadius: 3).fill()
            ("A" as NSString).draw(
                at: NSPoint(x: toggle.minX + 5, y: toggle.minY + 1),
                withAttributes: [
                    .font: NSFont.monospacedSystemFont(ofSize: 9, weight: .bold),
                    .foregroundColor: NSColor(hex: lane.automation != nil ? 0x101418 : 0x8c8175),
                ])

            // The automation-mode chip sits just left of the "A" toggle.
            drawHeaderAutomationMode(index: index, lane: lane)

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
            // `blink` is the solo-silenced pulse: a half-lit fill even when not on.
            let fill = on ? NSColor(hex: onColor)
                     : (blink ? NSColor(hex: onColor).withAlphaComponent(0.55) : NSColor(hex: 0x2a241e))
            fill.setFill()
            NSBezierPath(roundedRect: frame, xRadius: 3, yRadius: 3).fill()
            let attrs: [NSAttributedString.Key: Any] = [
                .font: NSFont.monospacedSystemFont(ofSize: 8.5, weight: .bold),
                .foregroundColor: (on || blink) ? NSColor(hex: 0x14100a) : NSColor(hex: 0x9a8f7e),
            ]
            let size = (title as NSString).size(withAttributes: attrs)
            (title as NSString).draw(
                at: NSPoint(x: frame.midX - size.width / 2, y: frame.midY - size.height / 2),
                withAttributes: attrs)
        }
        // The strip degrades gracefully as the lane shrinks: rows drop out from the bottom.
        let laneH = laneHeight(index)
        if laneH >= 48 {
            button(headerMuteRect(index), "M", on: lane.muted, onColor: 0xe6a23c, blink: lane.soloSilencedBlink)
            button(headerSoloRect(index), "S", on: lane.soloed, onColor: 0xf4d35e)
            button(headerArmRect(index), "R", on: lane.armed, onColor: 0xe5484d)
            button(headerInputMonitorRect(index), "I", on: lane.inputMonitor, onColor: 0x5fb85f)
        }

        if laneH >= 62 {
            drawHeaderFader(index: index, lane: lane)
        }

        if laneH >= 73 {
            drawHeaderPan(index: index, lane: lane)
        }

        if laneH >= 83 {
            // Stereo peak meter: L bar on top, R below, so a stereo track reads as stereo.
            let meter = headerMeterRect(index)
            NSColor(hex: 0x140f0a).setFill()
            NSBezierPath(roundedRect: meter, xRadius: 1, yRadius: 1).fill()
            let barHeight = (meter.height - 1) / 2
            func meterBar(_ level: Float, atY y: CGFloat) {
                guard level > 0.0001 else { return }
                let l = min(1, max(0, CGFloat(level)))
                let color = l > 0.9 ? NSColor(hex: 0xe5484d)
                          : l > 0.7 ? NSColor(hex: 0xe6a23c)
                          : NSColor(hex: 0x5fb85f)
                color.setFill()
                NSRect(x: meter.minX, y: y, width: meter.width * l, height: barHeight).fill()
            }
            meterBar(lane.peakLeft, atY: meter.minY)
            meterBar(lane.peakRight, atY: meter.minY + barHeight + 1)
        }

        if laneH >= 100 { drawHeaderInsertsSends(lane, index: index) }
    }

    /// Horizontal inline volume fader with a brushed-metal cap — the compact sibling of the
    /// mixer's ChannelFader, sharing the same FaderScale taper (0 dB ~78% along).
    private func drawHeaderFader(index: Int, lane: TimelineModel.Lane) {
        let fader = headerFaderRect(index)

        // Recessed slot: dark trough + a faint bottom highlight so it reads as inset.
        NSColor(hex: 0x151009).setFill()
        NSBezierPath(roundedRect: fader, xRadius: fader.height / 2, yRadius: fader.height / 2).fill()
        NSColor.white.withAlphaComponent(0.06).setStroke()
        let rim = NSBezierPath(roundedRect: fader.insetBy(dx: 0.5, dy: 0.5), xRadius: fader.height / 2, yRadius: fader.height / 2)
        rim.lineWidth = 0.75; rim.stroke()

        let knobX = fader.minX + headerFaderFraction(lane.volumeDb) * fader.width

        // Filled portion from the left up to the cap, in the track's accent.
        lane.accent.withAlphaComponent(0.7).setFill()
        NSBezierPath(roundedRect: NSRect(x: fader.minX, y: fader.midY - 1.5,
                                         width: max(0, knobX - fader.minX), height: 3),
                     xRadius: 1.5, yRadius: 1.5).fill()

        // Brushed-metal cap: vertical gradient body, rim, horizontal grip lines, accent tick.
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

        (String(format: "%+.0f", lane.volumeDb) as NSString).draw(
            at: NSPoint(x: fader.maxX - 22, y: fader.minY - 11),
            withAttributes: [
                .font: NSFont.monospacedSystemFont(ofSize: 7.5, weight: .regular),
                .foregroundColor: NSColor(hex: 0x867b6a),
            ])
    }

    /// Horizontal pan bar with a centre tick, its filled portion running from the middle
    /// out to the knob — the inline sibling of the mixer's PanSlider.
    private func drawHeaderPan(index: Int, lane: TimelineModel.Lane) {
        let bar = headerPanRect(index)

        NSColor(hex: 0x151009).setFill()
        NSBezierPath(roundedRect: bar, xRadius: bar.height / 2, yRadius: bar.height / 2).fill()
        NSColor.white.withAlphaComponent(0.06).setStroke()
        let rim = NSBezierPath(roundedRect: bar.insetBy(dx: 0.5, dy: 0.5), xRadius: bar.height / 2, yRadius: bar.height / 2)
        rim.lineWidth = 0.75; rim.stroke()

        // Centre reference tick.
        NSColor.white.withAlphaComponent(0.14).setFill()
        NSRect(x: bar.midX - 0.5, y: bar.minY + 1, width: 1, height: bar.height - 2).fill()

        let frac = CGFloat((lane.pan + 1) / 2)
        let knobX = bar.minX + frac * bar.width
        // Fill from centre to the knob.
        lane.accent.withAlphaComponent(0.7).setFill()
        let fillX = min(bar.midX, knobX), fillW = abs(knobX - bar.midX)
        NSBezierPath(roundedRect: NSRect(x: fillX, y: bar.midY - 1.5, width: fillW, height: 3),
                     xRadius: 1.5, yRadius: 1.5).fill()

        // Knob.
        let knobW: CGFloat = 9, knobH: CGFloat = 11
        let knob = NSRect(x: knobX - knobW / 2, y: bar.midY - knobH / 2, width: knobW, height: knobH)
        let cap = NSBezierPath(roundedRect: knob, xRadius: 2.5, yRadius: 2.5)
        if let g = NSGradient(colors: [NSColor(hex: 0x6a727a), NSColor(hex: 0x363c44), NSColor(hex: 0x181c22)]) {
            g.draw(in: cap, angle: -90)
        }
        NSColor.white.withAlphaComponent(0.30).setStroke(); cap.lineWidth = 0.75; cap.stroke()

        (lane.pan == 0 ? "C" : String(format: lane.pan < 0 ? "L%.0f" : "R%.0f", abs(lane.pan) * 100) as NSString).draw(
            at: NSPoint(x: bar.minX, y: bar.minY - 11),
            withAttributes: [
                .font: NSFont.monospacedSystemFont(ofSize: 7.5, weight: .regular),
                .foregroundColor: NSColor(hex: 0x867b6a),
            ])
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

    /// The Pro-Tools-style inserts row and sends row under the lane's fader/meter.
    private func drawHeaderInsertsSends(_ lane: TimelineModel.Lane, index: Int) {
        func chip(_ rect: NSRect, text: String, fill: NSColor, stroke: NSColor, textColor: NSColor,
                  dashed: Bool = false, strike: Bool = false) {
            fill.setFill()
            let path = NSBezierPath(roundedRect: rect, xRadius: 2.5, yRadius: 2.5)
            path.fill()
            stroke.setStroke()
            if dashed { path.setLineDash([2, 2], count: 2, phase: 0) }
            path.lineWidth = 0.75
            path.stroke()
            let attrs: [NSAttributedString.Key: Any] = [
                .font: NSFont.monospacedSystemFont(ofSize: 7.5, weight: .medium),
                .foregroundColor: textColor,
            ]
            let truncated = truncatedChipText(text, width: rect.width - 4, attrs: attrs)
            let size = (truncated as NSString).size(withAttributes: attrs)
            (truncated as NSString).draw(at: NSPoint(x: rect.minX + 3, y: rect.midY - size.height / 2), withAttributes: attrs)
            if strike {
                NSColor(hex: 0x9a8f7e).setStroke()
                let line = NSBezierPath()
                line.move(to: NSPoint(x: rect.minX + 3, y: rect.midY))
                line.line(to: NSPoint(x: rect.minX + 3 + size.width, y: rect.midY))
                line.lineWidth = 0.75
                line.stroke()
            }
        }

        // Inserts (labelled i on the far left of the row).
        for slot in 0..<Self.headerInsertSlots {
            let rect = headerInsertRect(index, slot: slot)
            let ins = slot < lane.inserts.count ? lane.inserts[slot] : nil
            let filled = ins != nil && !(ins!.isEmpty)
            if filled {
                let bypassed = ins!.bypassed
                chip(rect, text: ins!.name,
                     fill: NSColor(hex: 0x2f3b34).withAlphaComponent(bypassed ? 0.5 : 1),
                     stroke: NSColor(hex: 0x5f9fd6).withAlphaComponent(bypassed ? 0.4 : 0.7),
                     textColor: NSColor(hex: bypassed ? 0x6f6a60 : 0xbcd0e0),
                     strike: bypassed)
            } else {
                chip(rect, text: ["A", "B", "C", "D"][slot],
                     fill: NSColor(hex: 0x231e18), stroke: NSColor(hex: 0x3a332b),
                     textColor: NSColor(hex: 0x5a5145), dashed: true)
            }
        }

        // Sends need another row; only when the lane is tall enough for it.
        guard laneHeight(index) >= 106 else { return }
        // Sends: active sends then a trailing "+".
        let sendCount = min(2, lane.sends.count)
        for i in 0..<sendCount {
            let rect = headerSendRect(index, slot: i)
            chip(rect, text: lane.sends[i].label,
                 fill: NSColor(hex: 0x25322f), stroke: NSColor(hex: 0x35bfa8).withAlphaComponent(0.6),
                 textColor: NSColor(hex: 0x9fe4d6))
        }
        let plusRect = headerSendRect(index, slot: sendCount)
        chip(plusRect, text: "+ 센드", fill: NSColor(hex: 0x231e18),
             stroke: NSColor(hex: 0x3a332b), textColor: NSColor(hex: 0x6a6154), dashed: true)
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
    private func headerSendSlotHit(_ lane: Int, point: NSPoint) -> Int? {
        let count = min(2, model.lanes[lane].sends.count) + 1     // existing sends + the "+"
        for slot in 0..<count where headerSendRect(lane, slot: slot).contains(point) { return slot }
        return nil
    }

    private final class HeaderMenuRef: NSObject {
        let track: Int, slot: Int; let gain: Float; let bus: String
        init(track: Int, slot: Int = -1, gain: Float = 0, bus: String = "") {
            self.track = track; self.slot = slot; self.gain = gain; self.bus = bus
        }
    }

    private func handleInsertClick(trackId: Int, lane: Int, slot: Int, command: Bool) {
        onSelectLane?(lane)
        let inserts = model.lanes[lane].inserts
        let filled = slot < inserts.count && !inserts[slot].isEmpty
        if !filled { onBrowseInsert?(trackId); return }
        if command { onBypassInsert?(trackId, slot); return }   // Pro Tools ⌘-click = bypass
        onToggleInsertEditor?(trackId, slot)                    // left-click a filled insert opens its editor
    }

    private func handleSendClick(trackId: Int, lane: Int, slot: Int, event: NSEvent) {
        onSelectLane?(lane)
        let sends = model.lanes[lane].sends
        let menu = NSMenu()
        if slot < min(2, sends.count) {
            let send = sends[slot]
            let level = NSMenuItem(title: "레벨", action: nil, keyEquivalent: "")
            let sub = NSMenu()
            for db in SendControls.levels {
                let it = NSMenuItem(title: "\(db) dB", action: #selector(sendSetGainMenu(_:)), keyEquivalent: "")
                it.target = self; it.representedObject = HeaderMenuRef(track: trackId, slot: slot, gain: Float(db))
                sub.addItem(it)
            }
            level.submenu = sub
            menu.addItem(level)

            let panItem = NSMenuItem(title: "팬", action: nil, keyEquivalent: "")
            let panSub = NSMenu()
            for pan in SendControls.pans {
                let it = NSMenuItem(title: pan.label, action: #selector(sendSetPanMenu(_:)), keyEquivalent: "")
                it.target = self; it.representedObject = HeaderMenuRef(track: trackId, slot: slot, gain: pan.value)
                panSub.addItem(it)
            }
            panItem.submenu = panSub
            menu.addItem(panItem)

            let pf = NSMenuItem(title: send.preFader ? "포스트 페이더로" : "프리 페이더로",
                                action: #selector(sendPreFaderMenu(_:)), keyEquivalent: "")
            pf.target = self; pf.representedObject = HeaderMenuRef(track: trackId, slot: slot, gain: send.preFader ? 0 : 1)
            menu.addItem(pf)

            menu.addItem(.separator())
            let rm = NSMenuItem(title: "센드 제거", action: #selector(sendRemoveMenu(_:)), keyEquivalent: "")
            rm.target = self; rm.representedObject = HeaderMenuRef(track: trackId, slot: slot)
            menu.addItem(rm)
        } else {
            for bus in (onSendBusOptions?(trackId) ?? []) {
                let it = NSMenuItem(title: bus, action: #selector(sendAddMenu(_:)), keyEquivalent: "")
                it.target = self; it.representedObject = HeaderMenuRef(track: trackId, bus: bus)
                menu.addItem(it)
            }
            if !(onSendBusOptions?(trackId) ?? []).isEmpty { menu.addItem(.separator()) }
            let aux = NSMenuItem(title: "Aux 버스 만들기", action: #selector(sendAddAuxMenu(_:)), keyEquivalent: "")
            aux.target = self; menu.addItem(aux)
        }
        NSMenu.popUpContextMenu(menu, with: event, for: self)
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

    /// Pick what an automation lane targets: track volume / pan, or a plug-in parameter.
    private func showAutomationParamMenu(lane: Int, event: NSEvent) {
        let options = onAutomationParamOptions?(lane) ?? []
        guard !options.isEmpty else { onCycleAutomationParameter?(lane); return }
        let menu = NSMenu()
        let header = NSMenuItem(title: "오토메이션 파라미터", action: nil, keyEquivalent: "")
        header.isEnabled = false
        menu.addItem(header)
        for opt in options {
            let it = NSMenuItem(title: opt.name, action: #selector(pickAutomationParam(_:)), keyEquivalent: "")
            it.target = self
            it.representedObject = AutoParamRef(lane, opt.id)
            it.state = opt.on ? .on : .off
            menu.addItem(it)
        }
        NSMenu.popUpContextMenu(menu, with: event, for: self)
    }
    @objc private func pickAutomationParam(_ s: NSMenuItem) {
        guard let r = s.representedObject as? AutoParamRef else { return }
        onSetAutomationParam?(r.lane, r.id)
    }

    private final class ClipCurveRef: NSObject { let clipId: String; let curve: String
        init(_ clipId: String, _ curve: String) { self.clipId = clipId; self.curve = curve } }

    /// Right-click a clip → pick its fade-in / fade-out curve shape.
    private func showClipFadeMenu(_ clip: TimelineModel.Clip, event: NSEvent) {
        let opts = onFadeCurveOptions?() ?? []
        guard !opts.isEmpty else { return }
        let current = onClipCurrentFades?(clip.id)
        let menu = NSMenu()
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
    @objc private func setFadeInCurveMenu(_ s: NSMenuItem) {
        guard let r = s.representedObject as? ClipCurveRef else { return }
        onSetClipFadeInCurve?(r.clipId, r.curve)
    }
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
        NSRect(x: Self.headerWidth - 26, y: laneTop(index) + 10, width: 18, height: 14)
    }

    /// The track-name band in the header — double-click here to rename.
    private func nameRect(_ index: Int) -> NSRect {
        NSRect(x: 10, y: laneTop(index) + 6, width: Self.headerWidth - 40, height: 22)
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
    /// The volume fader track spans this rect; the fill maps -60…+6 dB left→right.
    private func headerFaderRect(_ index: Int) -> NSRect {
        NSRect(x: 12, y: laneTop(index) + 50, width: Self.headerWidth - 24, height: 8)
    }
    /// Horizontal pan bar, centre-detented, directly under the fader.
    private func headerPanRect(_ index: Int) -> NSRect {
        NSRect(x: 12, y: laneTop(index) + 61, width: Self.headerWidth - 24, height: 8)
    }
    private func headerMeterRect(_ index: Int) -> NSRect {
        NSRect(x: 12, y: laneTop(index) + 72, width: Self.headerWidth - 24, height: 7)
    }
    /// Small square top-right of the header that shows/cycles the automation mode.
    private func headerAutomationModeRect(_ index: Int) -> NSRect {
        NSRect(x: Self.headerWidth - 46, y: laneTop(index) + 10, width: 16, height: 14)
    }

    // Inserts row: four slot chips. Sends row: active send chips plus a "+".
    static let headerInsertSlots = 4
    private func headerInsertRect(_ index: Int, slot: Int) -> NSRect {
        let gap: CGFloat = 3
        let totalW = Self.headerWidth - 24
        let w = (totalW - gap * CGFloat(Self.headerInsertSlots - 1)) / CGFloat(Self.headerInsertSlots)
        return NSRect(x: 12 + CGFloat(slot) * (w + gap), y: laneTop(index) + 84, width: w, height: 13)
    }
    private func headerSendRect(_ index: Int, slot: Int) -> NSRect {
        // Up to three chips across the row (existing sends, then the "+").
        let gap: CGFloat = 3
        let totalW = Self.headerWidth - 24
        let count = 3
        let w = (totalW - gap * CGFloat(count - 1)) / CGFloat(count)
        return NSRect(x: 12 + CGFloat(slot) * (w + gap), y: laneTop(index) + 100, width: w, height: 13)
    }

    // The inline fader uses the same console taper as the mixer, so 0 dB sits ~78% along
    // and near-unity moves are expanded — not a straight line.
    private func headerFaderFraction(_ db: Float) -> CGFloat {
        CGFloat(FaderScale.position(forDb: db))
    }
    private func headerFaderDb(atX pointX: CGFloat, index: Int) -> Float {
        let rect = headerFaderRect(index)
        let fraction = Double(min(1, max(0, (pointX - rect.minX) / max(1, rect.width))))
        return FaderScale.db(forPosition: fraction)
    }
    /// Pan maps linearly across the bar, −1 (L) … +1 (R), with a small snap to centre.
    private func headerPan(atX pointX: CGFloat, index: Int) -> Float {
        let rect = headerPanRect(index)
        let f = Float(min(1, max(0, (pointX - rect.minX) / max(1, rect.width))))
        let pan = f * 2 - 1
        return abs(pan) < 0.06 ? 0 : pan          // centre detent
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
        }

        context.restoreGState()
    }

    private func gainLineY(_ clip: TimelineModel.Clip, in rect: NSRect) -> CGFloat {
        let span = Self.gainRange.upperBound - Self.gainRange.lowerBound
        let fraction = (clip.gainDb - Self.gainRange.lowerBound) / span
        return rect.maxY - CGFloat(fraction) * rect.height
    }

    /// Fades are drawn as the region the clip loses: a wedge from silence up to
    /// full level. Handles ride the top edge where the fade ends.
    private func drawFades(_ clip: TimelineModel.Clip, in rect: NSRect) {
        NSColor(hex: 0x0b0806).withAlphaComponent(0.55).setFill()

        if clip.fadeInSeconds > 0 {
            let end = x(forSeconds: clip.startSeconds + clip.fadeInSeconds)
            let wedge = NSBezierPath()
            wedge.move(to: NSPoint(x: rect.minX, y: rect.minY))
            wedge.line(to: NSPoint(x: min(end, rect.maxX), y: rect.minY))
            wedge.line(to: NSPoint(x: rect.minX, y: rect.maxY))
            wedge.close()
            wedge.fill()
        }
        if clip.fadeOutSeconds > 0 {
            let begin = x(forSeconds: clip.startSeconds + clip.durationSeconds - clip.fadeOutSeconds)
            let wedge = NSBezierPath()
            wedge.move(to: NSPoint(x: max(begin, rect.minX), y: rect.minY))
            wedge.line(to: NSPoint(x: rect.maxX, y: rect.minY))
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
            let startFraction = windowStart + Double(column) / Double(rect.width) * windowSpan
            let endFraction = windowStart + Double(column + 1) / Double(rect.width) * windowSpan
            let first = min(buckets - 1, max(0, Int(startFraction * Double(buckets))))
            let last = min(buckets - 1, max(first, Int(endFraction * Double(buckets))))

            var low: Float = 0
            var high: Float = 0
            for bucket in first...last {
                low = min(low, mins[bucket])
                high = max(high, maxs[bucket])
            }
            let pointX = rect.minX + CGFloat(column) + 0.5
            let highY = max(-halfHeight, min(halfHeight, CGFloat(high) * gainFactor * halfHeight))
            let lowY = max(-halfHeight, min(halfHeight, CGFloat(low) * gainFactor * halfHeight))
            path.move(to: NSPoint(x: pointX, y: midY - highY))
            path.line(to: NSPoint(x: pointX, y: midY - lowY))
            drew = true
        }

        guard drew else { return }
        accent.withAlphaComponent(0.85).setStroke()
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
    let playheadSeconds: Double
    let waveforms: [String: EngineController.WaveformData]
    let onSeek: (Double) -> Void
    let onZoom: (Double, Double) -> Void
    let onSelect: (String?) -> Void
    let onSetRange: (Double, Double) -> Void
    let onSelectRegion: (String?) -> Void
    let onOpenRegion: (String) -> Void
    let onMoveRegion: (String, Int, Double) -> Void
    let onResizeRegion: (String, Double) -> Void
    let onAddRegion: (Int, Double) -> Void
    let onDropAudio: (Int, Double, [URL]) -> Void
    let onMoveMarker: (Double, Double) -> Void
    let onDeleteMarker: (Double) -> Void
    let onSelectBetweenMarkers: (Double) -> Void
    let onToggleAutomation: (Int) -> Void
    let onCycleAutomationParameter: (Int) -> Void
    var onAutomationParamOptions: ((Int) -> [(id: String, name: String, on: Bool)])? = nil
    var onSetAutomationParam: ((Int, String) -> Void)? = nil
    var onSetLaneHeight: (([Int], CGFloat) -> Void)? = nil
    var onCommitLaneHeight: (() -> Void)? = nil
    var onFadeCurveOptions: (() -> [(label: String, id: String)])? = nil
    var onClipCurrentFades: ((String) -> (inCurve: String, outCurve: String))? = nil
    var onSetClipFadeInCurve: ((String, String) -> Void)? = nil
    var onSetClipFadeOutCurve: ((String, String) -> Void)? = nil
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
    let onMoveSelection: (Double) -> Void
    let onTrimStart: (String, Double) -> Void
    let onTrimEnd: (String, Double) -> Void
    let onSetFades: (String, Double, Double) -> Void
    let onSetGain: (String, Float) -> Void
    let onCommitGain: (String) -> Void
    let onSelectLane: (Int) -> Void
    let onMoveClipToLane: (String, Int, Double) -> Void
    let onDropClipToNewTrack: (String, Double) -> Void
    let onCommitEdit: (String) -> Void
    let snap: (Double) -> Double
    let onToggleMute: (Int) -> Void
    let onToggleSolo: (Int) -> Void
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
        wire(view)
        return view
    }

    func updateNSView(_ view: TimelineNSView, context: Context) {
        wire(view)
        if view.waveforms.keys != waveforms.keys {
            view.waveforms = waveforms
        }
        view.model = model
        view.playheadSeconds = playheadSeconds
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
        view.onSelectRegion = onSelectRegion
        view.onOpenRegion = onOpenRegion
        view.onMoveRegion = onMoveRegion
        view.onResizeRegion = onResizeRegion
        view.onAddRegion = onAddRegion
        view.onDropAudio = onDropAudio
        view.onMoveMarker = onMoveMarker
        view.onDeleteMarker = onDeleteMarker
        view.onSelectBetweenMarkers = onSelectBetweenMarkers
        view.onToggleAutomation = onToggleAutomation
        view.onCycleAutomationParameter = onCycleAutomationParameter
        view.onAutomationParamOptions = onAutomationParamOptions
        view.onSetAutomationParam = onSetAutomationParam
        view.onSetLaneHeight = onSetLaneHeight
        view.onCommitLaneHeight = onCommitLaneHeight
        view.onFadeCurveOptions = onFadeCurveOptions
        view.onClipCurrentFades = onClipCurrentFades
        view.onSetClipFadeInCurve = onSetClipFadeInCurve
        view.onSetClipFadeOutCurve = onSetClipFadeOutCurve
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
        view.onTrimStart = onTrimStart
        view.onTrimEnd = onTrimEnd
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
