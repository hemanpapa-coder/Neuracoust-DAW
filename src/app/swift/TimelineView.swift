import AppKit
import SwiftUI

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
        /// nil while the lane's automation is folded away.
        var automation: Automation?
    }

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
final class TimelineNSView: NSView {
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
    var waveforms: [String: (mins: [Float], maxs: [Float], durationSeconds: Double)] = [:] {
        didSet { needsDisplay = true }
    }

    var onSeek: ((Double) -> Void)?
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
    var onAddAutomationPoint: ((Int, Double, Float) -> Void)?    // (lane, time, value)
    var onMoveAutomationPoint: ((Int, Int, Double, Float) -> Void)?  // (lane, point, time, value)
    var onDeleteAutomationPoint: ((Int, Int) -> Void)?   // (lane, point)
    var onToggleSelect: ((String) -> Void)?              // shift-click
    var onSelectMany: (([String]) -> Void)?              // marquee
    var onMoveClip: ((String, Double) -> Void)?          // (clipId, newStart)
    /// Option-drag: duplicate the clip in place and return the copy's id, which the
    /// drag then moves — leaving the original where it was.
    var onBeginCopyDrag: ((String, Double) -> String?)?  // (clipId, startSeconds) -> newId
    var onMoveSelection: ((Double) -> Void)?             // delta seconds
    var onTrimStart: ((String, Double) -> Void)?         // (clipId, newStart)
    var onTrimEnd: ((String, Double) -> Void)?           // (clipId, newEnd)
    var onSetFades: ((String, Double, Double) -> Void)?  // (clipId, fadeIn, fadeOut)
    var onSetGain: ((String, Float) -> Void)?
    var onSelectLane: ((Int) -> Void)?
    var onMoveClipToLane: ((String, Int, Double) -> Void)?  // (clipId, laneIndex, start)
    var onCommitEdit: ((String) -> Void)?                // step name
    var snap: ((Double) -> Double)?

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
        /// Dragging one clip of a multi-selection drags all of them. The anchor's
        /// live start is read back from the model each frame, so a clamp at zero
        /// simply stops the whole selection instead of drifting it apart.
        case movingSelection(anchorId: String, grabOffsetSeconds: Double, startX: CGFloat, axisUnlocked: Bool)
        case trimmingStart(clipId: String)
        case trimmingEnd(clipId: String)
        case fadingIn(clip: TimelineModel.Clip)
        case fadingOut(clip: TimelineModel.Clip)
        case gaining(clip: TimelineModel.Clip, grabY: CGFloat, startGainDb: Float)
    }

    /// Clip gain is drawn as a horizontal line across this dB span.
    private static let gainRange: ClosedRange<Float> = -24...12
    private static let fadeHandleSize: CGFloat = 9

    private var drag = Drag.none

    /// The lanes scroll under a fixed ruler. An NSScrollView would carry the ruler
    /// away with them, so the view owns the offset itself.
    private var scrollY: CGFloat = 0

    /// The lane a drag is hovering, so it can be lit while a file is over it.
    private var dropLaneIndex: Int?
    private var dropSeconds: Double = 0

    static let rulerHeight: CGFloat = 30
    /// The top slice of the ruler sets the loop/edit range; below it the ruler scrubs.
    static let rangeStripHeight: CGFloat = 12
    /// Grab radius around each range edge for its ruler handle.
    static let rangeHandleWidth: CGFloat = 9
    static let laneHeight: CGFloat = 78
    static let automationHeight: CGFloat = 54
    static let headerWidth: CGFloat = 150

    private let playheadLayer = CALayer()
    /// The drop target is its own layer, not draw(_:), because a layer-backed view
    /// does not flush draw(_:) during the drag-tracking run loop but a layer change
    /// commits immediately — the same reason the playhead lives in a layer.
    private let dropBandLayer = CALayer()
    private let dropLineLayer = CALayer()

    override var isFlipped: Bool { true }

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
               y: Self.rulerHeight,
               width: max(0, bounds.width - Self.headerWidth),
               height: max(0, bounds.height - Self.rulerHeight))
    }

    private func x(forSeconds seconds: Double) -> CGFloat {
        let fraction = (seconds - model.visibleStart) / max(0.0001, model.visibleDuration)
        return lanesRect.minX + CGFloat(fraction) * lanesRect.width
    }

    private func seconds(atX pointX: CGFloat) -> Double {
        let fraction = Double((pointX - lanesRect.minX) / max(1, lanesRect.width))
        return model.visibleStart + fraction * model.visibleDuration
    }

    // MARK: Interaction

    /// Lanes are no longer a uniform stack: an open automation lane adds to the one
    /// above it, so every vertical position has to be accumulated.
    private func laneTop(_ index: Int) -> CGFloat {
        var top = Self.rulerHeight - scrollY
        for lane in model.lanes.prefix(index) {
            top += Self.laneHeight
            if lane.automation != nil { top += Self.automationHeight }
        }
        return top
    }

    /// How far the lanes reach below the ruler, at scroll offset zero.
    private var contentHeight: CGFloat {
        model.lanes.reduce(0) { $0 + Self.laneHeight + ($1.automation != nil ? Self.automationHeight : 0) }
    }

    private var maximumScrollY: CGFloat {
        max(0, contentHeight - (bounds.height - Self.rulerHeight))
    }

    private func automationRect(_ index: Int) -> NSRect? {
        guard index < model.lanes.count, model.lanes[index].automation != nil else { return nil }
        return NSRect(x: 0, y: laneTop(index) + Self.laneHeight,
                      width: bounds.width, height: Self.automationHeight)
    }

    private func clipRect(_ clip: TimelineModel.Clip) -> NSRect {
        let left = x(forSeconds: clip.startSeconds)
        let right = x(forSeconds: clip.startSeconds + clip.durationSeconds)
        let top = laneTop(clip.laneIndex) + 6
        return NSRect(x: left, y: top, width: max(2, right - left), height: Self.laneHeight - 12)
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
        return NSRect(x: left, y: top, width: max(2, right - left), height: Self.laneHeight - 12)
    }

    private func region(at point: NSPoint) -> TimelineModel.MidiRegion? {
        model.midiRegions.reversed().first { regionRect($0).contains(point) }
    }

    private func drawMidiRegions(_ context: CGContext) {
        context.saveGState()
        context.clip(to: NSRect(x: lanesRect.minX, y: Self.rulerHeight,
                                width: lanesRect.width, height: bounds.height - Self.rulerHeight))

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
        guard point.y >= Self.rulerHeight else { return nil }
        for index in model.lanes.indices {
            let top = laneTop(index)
            if point.y >= top && point.y < top + Self.laneHeight {
                return index
            }
        }
        return nil
    }

    /// Which open automation row a point falls in.
    private func automationIndex(at point: NSPoint) -> Int? {
        model.lanes.indices.first { automationRect($0)?.contains(point) ?? false }
    }

    override func mouseDown(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)

        // Clicking a lane header selects that track; the "A" chip folds automation out.
        if point.x < Self.headerWidth {
            if let lane = laneIndex(at: point) {
                if automationToggleRect(lane).contains(point) {
                    onToggleAutomation?(lane)
                } else {
                    onSelectLane?(lane)
                }
            } else if let lane = automationIndex(at: point) {
                // The parameter name doubles as the parameter picker.
                onCycleAutomationParameter?(lane)
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
        if point.y < Self.rulerHeight {
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
        } else {
            // Holding Option turns the move into a copy: duplicate in place and drag
            // the new clip, so the original stays put.
            var clipId = hit.id
            if event.modifierFlags.contains(.option),
               let newId = onBeginCopyDrag?(hit.id, hit.startSeconds) {
                clipId = newId
            }
            drag = .moving(clipId: clipId, grabOffsetSeconds: grabOffset,
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
        case .movingSelection(let anchorId, let grabOffset, let startX, let unlocked):
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
            let perPoint = span / Float(max(1, Self.laneHeight - 12))
            let value = startGainDb - Float(point.y - grabY) * perPoint
            onSetGain?(clip.id, min(Self.gainRange.upperBound, max(Self.gainRange.lowerBound, value)))
        }
    }

    override func mouseUp(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)

        switch drag {
        case .moving(let clipId, _, _, _):
            // Dropping on a different lane relocates the clip; that records its own
            // step, so do not also commit a "Move clip" one.
            if let lane = laneIndex(at: point),
               let clip = model.clips.first(where: { $0.id == clipId }),
               lane != clip.laneIndex {
                onMoveClipToLane?(clipId, lane, clip.startSeconds)
            } else {
                onCommitEdit?("Move clip")
            }
        case .trimmingStart, .trimmingEnd:
            onCommitEdit?("Trim clip")
        case .fadingIn, .fadingOut:
            onCommitEdit?("Clip fade")
        case .gaining:
            onCommitEdit?("Clip gain")
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
        case .none, .seeking, .rangingFrom, .rangingEdgeStart, .rangingEdgeEnd, .movingRange:
            // The range is a view of where to edit, not an edit. Nothing to undo.
            break
        }
        drag = .none
    }

    private func snapped(_ seconds: Double) -> Double {
        max(0, snap?(seconds) ?? seconds)
    }

    /// A resize cursor over the trim handles tells the user they are there.
    override func resetCursorRects() {
        super.resetCursorRects()
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
        guard point.y >= Self.rulerHeight else { return nil }
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
                                     width: lanesRect.width, height: Self.laneHeight)
        dropLineLayer.frame = CGRect(x: dropX - 1, y: top, width: 3, height: Self.laneHeight)
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
        NSRect(x: left, y: Self.rulerHeight, width: right - left,
               height: bounds.height - Self.rulerHeight).fill()

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

    private func drawRuler(_ context: CGContext) {
        NSColor(hex: 0x332c26).setFill()
        NSRect(x: 0, y: 0, width: bounds.width, height: Self.rulerHeight).fill()

        let secondsPerBar = 60.0 / Double(model.tempoBpm) * Double(model.beatsPerBar)
        let step = barStep()

        let firstBar = max(0, Int((model.visibleStart / secondsPerBar).rounded(.down)))
        let lastBar = Int(((model.visibleStart + model.visibleDuration) / secondsPerBar).rounded(.up))

        let attributes: [NSAttributedString.Key: Any] = [
            .font: NSFont.monospacedSystemFont(ofSize: 9, weight: .medium),
            .foregroundColor: NSColor(hex: 0x867b6a),
        ]

        var bar = firstBar - firstBar % step
        while bar <= lastBar {
            let position = x(forSeconds: Double(bar) * secondsPerBar)
            if position >= lanesRect.minX - 1 {
                NSColor(hex: 0x574d40).setFill()
                NSRect(x: position, y: Self.rulerHeight - 8, width: 1, height: 8).fill()

                let label = "\(bar + 1)" as NSString
                label.draw(at: NSPoint(x: position + 4, y: 6), withAttributes: attributes)
            }
            bar += step
        }

        NSColor(hex: 0x0b0806).setFill()
        NSRect(x: 0, y: Self.rulerHeight - 1, width: bounds.width, height: 1).fill()
    }

    private func drawLaneHeaders(_ context: CGContext) {
        let nameAttributes: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 11, weight: .semibold),
            .foregroundColor: NSColor(hex: 0xe8e1d5),
        ]

        context.saveGState()
        context.clip(to: NSRect(x: 0, y: Self.rulerHeight,
                                width: bounds.width, height: bounds.height - Self.rulerHeight))

        for (index, lane) in model.lanes.enumerated() {
            let rect = NSRect(x: 0,
                              y: laneTop(index),
                              width: Self.headerWidth,
                              height: Self.laneHeight)
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

            NSColor(hex: 0x1b1611).setFill()
            NSRect(x: 0, y: rect.maxY - 1, width: bounds.width, height: 1).fill()
        }
        context.restoreGState()
    }

    /// Markers live in the ruler below the range strip; they must not eat the scrub.
    private static let markerFlagWidth: CGFloat = 7

    private func markerFlagRect(_ marker: TimelineModel.Marker) -> NSRect {
        NSRect(x: x(forSeconds: marker.timeSeconds), y: Self.rangeStripHeight,
               width: Self.markerFlagWidth, height: Self.rulerHeight - Self.rangeStripHeight)
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
            NSRect(x: position, y: Self.rulerHeight, width: 1,
                   height: bounds.height - Self.rulerHeight).fill()

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


    /// Value axis: the top of the row is the parameter's maximum.
    private func automationY(_ value: Float, in rect: NSRect, _ automation: TimelineModel.Automation) -> CGFloat {
        let span = automation.range.upperBound - automation.range.lowerBound
        let fraction = (value - automation.range.lowerBound) / max(0.0001, span)
        return rect.maxY - 6 - CGFloat(fraction) * (rect.height - 12)
    }

    private func automationValue(atY pointY: CGFloat, in rect: NSRect,
                                 _ automation: TimelineModel.Automation) -> Float {
        let span = automation.range.upperBound - automation.range.lowerBound
        let fraction = Float((rect.maxY - 6 - pointY) / max(1, rect.height - 12))
        let value = automation.range.lowerBound + fraction * span
        return min(automation.range.upperBound, max(automation.range.lowerBound, value))
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
            let rect = NSRect(x: left, y: top, width: max(2, right - left), height: Self.laneHeight - 12)

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

    /// Resamples the cached peak buckets onto the clip's on-screen width. Only the
    /// visible span is walked, so zooming in does not cost more.
    private func drawWaveform(_ clip: TimelineModel.Clip, in rect: NSRect, accent: NSColor) {
        guard rect.width >= 1,
              let peaks = waveforms[clip.sourcePath],
              !peaks.mins.isEmpty else { return }

        let midY = rect.midY
        let halfHeight = rect.height / 2
        let buckets = peaks.mins.count

        let path = NSBezierPath()
        var drew = false

        let firstColumn = Int(max(0, lanesRect.minX - rect.minX))
        let lastColumn = Int(min(rect.width, lanesRect.maxX - rect.minX))
        guard lastColumn > firstColumn else { return }

        // The peaks span the whole file. A trimmed or split clip plays a window of it,
        // so map each on-screen column into that window rather than the file.
        let fileDuration = peaks.durationSeconds
        let windowStart = fileDuration > 0 ? clip.sourceOffsetSeconds / fileDuration : 0
        let windowSpan = fileDuration > 0 ? clip.durationSeconds / fileDuration : 1

        // A column usually covers many peaks; take the loudest of them so a transient
        // is never skipped between samples. Zoomed in it covers less than one peak,
        // and both edges land on the same index — still crisp.
        for column in firstColumn..<lastColumn {
            let startFraction = windowStart + Double(column) / Double(rect.width) * windowSpan
            let endFraction = windowStart + Double(column + 1) / Double(rect.width) * windowSpan
            let first = min(buckets - 1, max(0, Int(startFraction * Double(buckets))))
            let last = min(buckets - 1, max(first, Int(endFraction * Double(buckets))))

            var low: Float = 0
            var high: Float = 0
            for bucket in first...last {
                low = min(low, peaks.mins[bucket])
                high = max(high, peaks.maxs[bucket])
            }
            let pointX = rect.minX + CGFloat(column) + 0.5
            path.move(to: NSPoint(x: pointX, y: midY - CGFloat(high) * halfHeight))
            path.line(to: NSPoint(x: pointX, y: midY - CGFloat(low) * halfHeight))
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
    let waveforms: [String: (mins: [Float], maxs: [Float], durationSeconds: Double)]
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
    let onAddAutomationPoint: (Int, Double, Float) -> Void
    let onMoveAutomationPoint: (Int, Int, Double, Float) -> Void
    let onDeleteAutomationPoint: (Int, Int) -> Void
    let onToggleSelect: (String) -> Void
    let onSelectMany: ([String]) -> Void
    let onMoveClip: (String, Double) -> Void
    var onBeginCopyDrag: ((String, Double) -> String?)? = nil
    let onMoveSelection: (Double) -> Void
    let onTrimStart: (String, Double) -> Void
    let onTrimEnd: (String, Double) -> Void
    let onSetFades: (String, Double, Double) -> Void
    let onSetGain: (String, Float) -> Void
    let onSelectLane: (Int) -> Void
    let onMoveClipToLane: (String, Int, Double) -> Void
    let onCommitEdit: (String) -> Void
    let snap: (Double) -> Double

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
        view.onAddAutomationPoint = onAddAutomationPoint
        view.onMoveAutomationPoint = onMoveAutomationPoint
        view.onDeleteAutomationPoint = onDeleteAutomationPoint
        view.onToggleSelect = onToggleSelect
        view.onSelectMany = onSelectMany
        view.onMoveClip = onMoveClip
        view.onBeginCopyDrag = onBeginCopyDrag
        view.onMoveSelection = onMoveSelection
        view.onTrimStart = onTrimStart
        view.onTrimEnd = onTrimEnd
        view.onSetFades = onSetFades
        view.onSetGain = onSetGain
        view.onSelectLane = onSelectLane
        view.onMoveClipToLane = onMoveClipToLane
        view.onCommitEdit = onCommitEdit
        view.snap = snap
    }
}
