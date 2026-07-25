import AppKit
import SwiftUI

/// The note editor for one MIDI region.
///
/// Notes are stored in **beats from the region's start**, which is also what the
/// renderer reads, so nothing here converts to seconds. The region's length bounds
/// the grid: a note past the end is never sounded.
struct PianoRollModel: Equatable {
    struct Note: Equatable {
        let id: String
        let pitch: Int
        let startBeats: Double
        let durationBeats: Double
        let velocity: Int
    }

    var regionName = ""
    var lengthBeats: Double = 8
    var beatsPerBar = 4
    var notes: [Note] = []
    /// Where the transport is, in beats from the region start. Negative when the
    /// playhead has not reached the region yet.
    var playheadBeats: Double = -1
}

final class PianoRollNSView: NSView {
    var model = PianoRollModel() {
        didSet {
            guard model != oldValue else { return }
            needsDisplay = true
        }
    }

    var onAddNote: ((Int, Double, Double) -> String?)?       // (pitch, startBeats, durationBeats) -> id
    var onMoveNote: ((String, Int, Double) -> Void)?         // (id, pitch, startBeats)
    var onResizeNote: ((String, Double) -> Void)?            // (id, durationBeats)
    var onDeleteNote: ((String) -> Void)?
    var onCommitEdit: ((String) -> Void)?
    var onSelect: ((Set<String>) -> Void)?                   // the highlighted note set
    var onCopyNote: ((String) -> String?)?                   // ⌥-drag: duplicate, return new id
    /// Audition: (pitch, velocity, noteOn). The keyboard down the left edge plays the
    /// track's instrument so a part can be found by ear, the way every other editor works.
    var onPreviewNote: ((Int, Int, Bool) -> Void)?
    var onPreviewAllNotesOff: (() -> Void)?

    /// The pitch the mouse is currently holding on the keyboard, so a drag can glissando
    /// (release the old note, sound the new one) and mouse-up always releases exactly one.
    private var previewPitch: Int?
    /// The pitch of the grid note currently being auditioned by a click/drag. Separate from
    /// previewPitch so dragging a note is not treated as a keyboard glissando.
    private var auditionPitch: Int?

    /// The selected notes, drawn in a distinct colour. Driven from the controller.
    var selectedIds: Set<String> = [] {
        didSet { if selectedIds != oldValue { needsDisplay = true } }
    }

    /// The active pointer tool: "select" (never creates on a plain click), "draw", or "erase".
    var tool = "select"

    /// The whole MIDI range. A narrower window would hide notes rather than clamp
    /// them: transposing an octave up used to move a note off the top of the view.
    private static let lowestPitch = 0
    private static let highestPitch = 127
    private static let keyboardWidth: CGFloat = 44
    private static let rowHeight: CGFloat = 10
    private static let resizeHandleWidth: CGFloat = 6
    /// How far the cursor must travel sideways before a vertical note move stops pinning the note's
    /// start time — the dead zone that keeps small mouse wobble from smearing the attack.
    private static let axisLockThreshold: CGFloat = 9

    /// New notes are one sixteenth long, and the grid snaps to sixteenths.
    private static let quantumBeats = 0.25

    private enum Drag {
        case none
        /// Moving one or several notes together: `origin` is every moved note's start pitch and
        /// beat, so the whole selection shifts by the grabbed note's delta. `startX`/`axisUnlocked`
        /// carry the axis-lock — until the cursor leaves a small dead zone horizontally, a note's
        /// start time is pinned so a straight up/down move never nudges its attack off the beat.
        case moving(grabId: String, grabOffsetBeats: Double, origin: [String: (pitch: Int, start: Double)],
                    startX: CGFloat, axisUnlocked: Bool)
        case resizing(id: String)
        /// A press-drag over empty grid: a click (no movement) adds a note, a drag rubber-bands
        /// a selection rectangle.
        case marquee(origin: NSPoint, current: NSPoint)
    }

    private var drag = Drag.none

    override var isFlipped: Bool { true }

    /// The editor closing mid-press must not leave a note ringing forever.
    override func viewWillMove(toWindow newWindow: NSWindow?) {
        super.viewWillMove(toWindow: newWindow)
        if newWindow == nil, previewPitch != nil || auditionPitch != nil {
            previewPitch = nil
            auditionPitch = nil
            onPreviewAllNotesOff?()
        }
    }

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
    }

    required init?(coder: NSCoder) { nil }

    // MARK: Geometry

    private var pitchCount: Int { Self.highestPitch - Self.lowestPitch + 1 }

    var idealHeight: CGFloat { CGFloat(pitchCount) * Self.rowHeight }

    private var gridRect: NSRect {
        NSRect(x: Self.keyboardWidth, y: 0,
               width: max(0, bounds.width - Self.keyboardWidth), height: bounds.height)
    }

    private func x(forBeat beat: Double) -> CGFloat {
        gridRect.minX + CGFloat(beat / max(0.001, model.lengthBeats)) * gridRect.width
    }

    private func beat(atX pointX: CGFloat) -> Double {
        Double((pointX - gridRect.minX) / max(1, gridRect.width)) * model.lengthBeats
    }

    /// Top of the view is the highest pitch, the way a keyboard stands on its side.
    private func y(forPitch pitch: Int) -> CGFloat {
        CGFloat(Self.highestPitch - pitch) * Self.rowHeight
    }

    private func pitch(atY pointY: CGFloat) -> Int {
        let row = Int(pointY / Self.rowHeight)
        return max(Self.lowestPitch, min(Self.highestPitch, Self.highestPitch - row))
    }

    private var lastPitchRange: ClosedRange<Int>?

    /// Where to scroll so the notes are on screen, or nil to leave the view alone.
    ///
    /// Only follows when the part's pitch range *changes* — a transpose that walks
    /// the notes off the top. Scrolling every update would drag the view back from
    /// wherever the user had put it.
    func scrollTarget(visible: NSRect) -> CGFloat? {
        let pitches = model.notes.map(\.pitch)
        guard let lowest = pitches.min(), let highest = pitches.max() else {
            lastPitchRange = nil
            return nil
        }
        let range = lowest...highest
        defer { lastPitchRange = range }
        guard lastPitchRange != range else { return nil }

        let top = y(forPitch: highest)
        let bottom = y(forPitch: lowest) + Self.rowHeight
        if top >= visible.minY && bottom <= visible.maxY {
            return nil
        }
        let centre = (top + bottom) / 2 - visible.height / 2
        return max(0, min(idealHeight - visible.height, centre))
    }

    private func snapped(_ beats: Double) -> Double {
        (beats / Self.quantumBeats).rounded() * Self.quantumBeats
    }

    private func noteRect(_ note: PianoRollModel.Note) -> NSRect {
        let left = x(forBeat: note.startBeats)
        let right = x(forBeat: note.startBeats + note.durationBeats)
        return NSRect(x: left, y: y(forPitch: note.pitch) + 1,
                      width: max(3, right - left), height: Self.rowHeight - 2)
    }

    private func note(at point: NSPoint) -> PianoRollModel.Note? {
        model.notes.reversed().first { noteRect($0).contains(point) }
    }

    /// The black keys, which also stripe the grid.
    private func isAccidental(_ pitch: Int) -> Bool {
        [1, 3, 6, 8, 10].contains(((pitch % 12) + 12) % 12)
    }

    private func pitchName(_ pitch: Int) -> String {
        let names = ["C", "C♯", "D", "D♯", "E", "F", "F♯", "G", "G♯", "A", "A♯", "B"]
        return names[((pitch % 12) + 12) % 12] + String(pitch / 12 - 1)
    }

    // MARK: Interaction

    override func mouseDown(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)
        // The keyboard down the left edge is a playable instrument, not decoration.
        if point.x < gridRect.minX {
            let pitch = self.pitch(atY: point.y)
            previewPitch = pitch
            // Velocity from where the key was struck along its length, like a weighted
            // keybed: near the front (right, by the grid) is loud, at the back is soft.
            let across = max(0, min(1, point.x / max(1, Self.keyboardWidth)))
            onPreviewNote?(pitch, Int(40 + across * 87), true)
            needsDisplay = true
            return
        }
        guard point.x >= gridRect.minX else { return }
        let shift = event.modifierFlags.contains(.shift) || event.modifierFlags.contains(.command)
        let option = event.modifierFlags.contains(.option)

        // Erase tool: a click on a note removes it; empty clicks do nothing.
        if tool == "erase" {
            if let hit = note(at: point) { onDeleteNote?(hit.id) }
            return
        }

        if let hit = note(at: point) {
            if event.clickCount >= 2 {
                onDeleteNote?(hit.id)
                return
            }
            // Audition the note you grab, at its own velocity, so editing is by ear as well as by
            // eye. Held until mouseUp; a move re-auditions as the pitch changes (below). Kept
            // separate from the keyboard strip's previewPitch so a note drag is not mistaken for a
            // keyboard glissando in mouseDragged.
            auditionPitch = hit.pitch
            onPreviewNote?(hit.pitch, hit.velocity, true)
            // ⌥-drag copies: duplicate in place, select the copy, and drag it — the original stays.
            if option, let newId = onCopyNote?(hit.id) {
                onSelect?([newId])
                drag = .moving(grabId: newId, grabOffsetBeats: beat(atX: point.x) - hit.startBeats,
                               origin: [newId: (hit.pitch, hit.startBeats)],
                               startX: point.x, axisUnlocked: false)
                return
            }
            // Selection: ⇧/⌘ toggles a note in the set (and does not start a move); a plain click
            // on an unselected note replaces the selection with it.
            if shift {
                var sel = selectedIds
                if sel.contains(hit.id) { sel.remove(hit.id) } else { sel.insert(hit.id) }
                onSelect?(sel)
                return
            }
            var sel = selectedIds
            if !sel.contains(hit.id) { sel = [hit.id]; onSelect?(sel) }
            if noteRect(hit).maxX - point.x <= Self.resizeHandleWidth {
                drag = .resizing(id: hit.id)
                return
            }
            // Capture every selected note's start so the whole selection moves as one.
            var origin: [String: (pitch: Int, start: Double)] = [:]
            for note in model.notes where sel.contains(note.id) {
                origin[note.id] = (note.pitch, note.startBeats)
            }
            drag = .moving(grabId: hit.id, grabOffsetBeats: beat(atX: point.x) - hit.startBeats,
                           origin: origin, startX: point.x, axisUnlocked: false)
            return
        }

        // Empty grid — behaviour depends on the tool (Cubase/Logic). Smart draws here too, so it
        // is the one tool that both edits notes and paints new ones.
        if tool == "draw" || tool == "smart" {
            // Paint a note and drag its right edge to size it in the same gesture.
            let start = max(0, snapped(beat(atX: point.x)))
            guard start < model.lengthBeats else { return }
            if let newId = onAddNote?(pitch(atY: point.y), start, Self.quantumBeats) {
                onSelect?([newId])
                drag = .resizing(id: newId)
            }
            return
        }
        // Select: a double-click drops a note; a plain click deselects; a drag rubber-bands.
        if event.clickCount >= 2 {
            let start = max(0, snapped(beat(atX: point.x)))
            if start < model.lengthBeats, let newId = onAddNote?(pitch(atY: point.y), start, Self.quantumBeats) {
                onSelect?([newId])
            }
            return
        }
        drag = .marquee(origin: point, current: point)
    }

    override func mouseDragged(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)

        // Sliding along the keyboard glissandos: release the key we were on, sound the new one.
        if let holding = previewPitch {
            let pitch = self.pitch(atY: point.y)
            if pitch != holding {
                onPreviewNote?(holding, 0, false)
                let across = max(0, min(1, point.x / max(1, Self.keyboardWidth)))
                onPreviewNote?(pitch, Int(40 + across * 87), true)
                previewPitch = pitch
                needsDisplay = true
            }
            return
        }

        switch drag {
        case .none:
            break
        case .moving(let grabId, let grabOffset, let origin, let startX, let axisUnlocked):
            guard let grab = origin[grabId] else { break }
            // Axis-lock: while the cursor stays within the dead zone horizontally, pin every note's
            // start time (beatDelta = 0) so a straight up/down move can't nudge the attack off the
            // beat from a little mouse wobble. Cross the zone and it re-baselines the grab and moves
            // freely in both axes from then on.
            if !axisUnlocked, abs(point.x - startX) > Self.axisLockThreshold {
                drag = .moving(grabId: grabId, grabOffsetBeats: beat(atX: point.x) - grab.start,
                               origin: origin, startX: startX, axisUnlocked: true)
            }
            let horizontallyFree = axisUnlocked || abs(point.x - startX) > Self.axisLockThreshold
            let beatDelta = horizontallyFree
                ? max(0, snapped(beat(atX: point.x) - grabOffset)) - grab.start
                : 0
            let pitchDelta = pitch(atY: point.y) - grab.pitch
            for (id, o) in origin {
                onMoveNote?(id, max(Self.lowestPitch, min(Self.highestPitch, o.pitch + pitchDelta)),
                            max(0, o.start + beatDelta))
            }
            // Re-audition ONLY when the grabbed note actually crosses to a new pitch — never on the
            // horizontal wobble the axis-lock now absorbs — so a move plays the scale under the
            // cursor without a burst of retriggers.
            let movedPitch = max(Self.lowestPitch, min(Self.highestPitch, grab.pitch + pitchDelta))
            if auditionPitch != movedPitch {
                if let old = auditionPitch { onPreviewNote?(old, 0, false) }
                let velocity = model.notes.first(where: { $0.id == grabId })?.velocity ?? 96
                onPreviewNote?(movedPitch, velocity, true)
                auditionPitch = movedPitch
            }
        case .resizing(let id):
            guard let note = model.notes.first(where: { $0.id == id }) else { break }
            // A note has to stay long enough to sound; the engine clips it to the region.
            let duration = max(Self.quantumBeats, snapped(beat(atX: point.x) - note.startBeats))
            onResizeNote?(id, duration)
        case .marquee(let origin, _):
            drag = .marquee(origin: origin, current: point)
            needsDisplay = true
        }
    }

    override func mouseUp(with event: NSEvent) {
        if let holding = previewPitch {
            onPreviewNote?(holding, 0, false)
            previewPitch = nil
            needsDisplay = true
            return
        }
        // Release the note we were auditioning (a plain click or the end of a move).
        if let a = auditionPitch {
            onPreviewNote?(a, 0, false)
            auditionPitch = nil
        }
        switch drag {
        case .moving: onCommitEdit?("Move note")
        case .resizing: onCommitEdit?("Resize note")
        case .marquee(let origin, let current):
            if hypot(current.x - origin.x, current.y - origin.y) < 4 {
                // A plain click on empty grid clears the selection (Cubase select tool). Notes are
                // made with the Draw tool or a double-click, never a single click here.
                onSelect?([])
            } else {
                let rect = NSRect(x: min(origin.x, current.x), y: min(origin.y, current.y),
                                  width: abs(current.x - origin.x), height: abs(current.y - origin.y))
                let ids = model.notes.filter { noteRect($0).intersects(rect) }.map(\.id)
                onSelect?(Set(ids))
            }
        case .none:
            break
        }
        drag = .none
        needsDisplay = true
    }

    // MARK: Drawing

    override func draw(_ dirtyRect: NSRect) {
        guard let context = NSGraphicsContext.current?.cgContext else { return }

        NSColor(hex: 0x201b18).setFill()
        bounds.fill()

        drawRows()
        drawBeatLines()
        drawNotes()
        drawKeyboard(context)
        drawPlayhead()
        drawMarquee()
    }

    private func drawRows() {
        for pitch in Self.lowestPitch...Self.highestPitch {
            let rect = NSRect(x: gridRect.minX, y: y(forPitch: pitch),
                              width: gridRect.width, height: Self.rowHeight)
            NSColor(hex: isAccidental(pitch) ? 0x1b1613 : 0x262019).setFill()
            rect.fill()

            // A brighter line under every C, so octaves can be counted at a glance.
            if pitch % 12 == 0 {
                NSColor(hex: 0x3a322b).setFill()
                NSRect(x: gridRect.minX, y: rect.maxY - 1, width: gridRect.width, height: 1).fill()
            }
        }
    }

    private func drawBeatLines() {
        var beat = 0.0
        while beat <= model.lengthBeats {
            let isBar = Int(beat) % max(1, model.beatsPerBar) == 0 && beat == beat.rounded()
            NSColor(hex: isBar ? 0x453d34 : 0x2f2823).setFill()
            NSRect(x: x(forBeat: beat), y: 0, width: isBar ? 1 : 0.5, height: bounds.height).fill()
            beat += 1
        }
    }

    /// Velocity as hue, the way Cubase and Studio One colour a part: quiet notes sit cool and
    /// recede, loud ones run warm and jump out. Brightness alone (all this used to do) is very
    /// hard to read against a dark lane once notes are short — two notes a third apart in
    /// velocity looked identical. The ramp stays inside the design's indigo→amber family
    /// rather than becoming a rainbow.
    private static let velocityRamp: [NSColor] = [
        NSColor(hex: 0x4b62c8),   // ppp — indigo
        NSColor(hex: 0x7b6fd0),   // p   — violet
        NSColor(hex: 0xa96fc0),   // mf  — orchid
        NSColor(hex: 0xd98a72),   // f   — coral
        NSColor(hex: 0xe8623f),   // fff — red
    ]

    static func velocityColor(_ velocity: Int) -> NSColor {
        let ramp = velocityRamp
        let clamped = CGFloat(max(1, min(127, velocity)) - 1) / 126.0
        let scaled = clamped * CGFloat(ramp.count - 1)
        let lower = min(ramp.count - 1, Int(scaled))
        let upper = min(ramp.count - 1, lower + 1)
        let mix = scaled - CGFloat(lower)
        guard let a = ramp[lower].usingColorSpace(.sRGB),
              let b = ramp[upper].usingColorSpace(.sRGB) else { return ramp[lower] }
        return NSColor(srgbRed: a.redComponent + (b.redComponent - a.redComponent) * mix,
                       green: a.greenComponent + (b.greenComponent - a.greenComponent) * mix,
                       blue: a.blueComponent + (b.blueComponent - a.blueComponent) * mix,
                       alpha: 1.0)
    }

    private func drawNotes() {
        for note in model.notes {
            let rect = noteRect(note)
            let selected = selectedIds.contains(note.id)
            // Selected notes switch to a warm amber so they stand apart from the field; an
            // unselected note carries its velocity as colour.
            let fill = selected ? NSColor(hex: 0xffb15c) : Self.velocityColor(note.velocity)
            // A touch of alpha still tracks velocity so the ramp reads even where two
            // neighbouring stops are close in hue.
            fill.withAlphaComponent(selected ? 1.0 : 0.72 + 0.28 * CGFloat(note.velocity) / 127.0).setFill()
            let body = NSBezierPath(roundedRect: rect, xRadius: 2, yRadius: 2)
            body.fill()
            (selected ? NSColor.white : NSColor(hex: 0xd8c8ff).withAlphaComponent(0.75)).setStroke()
            body.lineWidth = selected ? 1.5 : 1
            body.stroke()
        }
    }

    private func drawMarquee() {
        guard case .marquee(let origin, let current) = drag,
              hypot(current.x - origin.x, current.y - origin.y) >= 4 else { return }
        let rect = NSRect(x: min(origin.x, current.x), y: min(origin.y, current.y),
                          width: abs(current.x - origin.x), height: abs(current.y - origin.y))
        NSColor(hex: 0xffb15c).withAlphaComponent(0.12).setFill()
        rect.fill()
        NSColor(hex: 0xffb15c).withAlphaComponent(0.7).setStroke()
        let path = NSBezierPath(rect: rect)
        path.lineWidth = 1
        path.stroke()
    }

    private func drawKeyboard(_ context: CGContext) {
        NSColor(hex: 0x151110).setFill()
        NSRect(x: 0, y: 0, width: Self.keyboardWidth, height: bounds.height).fill()

        for pitch in Self.lowestPitch...Self.highestPitch {
            let rect = NSRect(x: 0, y: y(forPitch: pitch), width: Self.keyboardWidth, height: Self.rowHeight)
            if previewPitch == pitch {
                // The key the mouse is holding down, so a glissando is visible as well as audible.
                NSColor(hex: 0xffb15c).setFill()
            } else {
                NSColor(hex: isAccidental(pitch) ? 0x1d1917 : 0xd6cec1).setFill()
            }
            rect.insetBy(dx: 0, dy: 0.5).fill()

            if pitch % 12 == 0 {
                (pitchName(pitch) as NSString).draw(
                    at: NSPoint(x: 4, y: rect.minY - 1),
                    withAttributes: [
                        .font: NSFont.monospacedSystemFont(ofSize: 7, weight: .medium),
                        .foregroundColor: NSColor(hex: 0x3a322b),
                    ])
            }
        }

        NSColor(hex: 0x0b0806).setFill()
        NSRect(x: Self.keyboardWidth - 1, y: 0, width: 1, height: bounds.height).fill()
    }

    private func drawPlayhead() {
        guard model.playheadBeats >= 0, model.playheadBeats <= model.lengthBeats else { return }
        NSColor(hex: 0xff5252).setFill()
        NSRect(x: x(forBeat: model.playheadBeats), y: 0, width: 1, height: bounds.height).fill()
    }
}

/// The velocity lane, pinned under the roll rather than scrolling with the keyboard.
/// It shares the roll's left gutter and beat mapping so a bar stands under its note.
final class VelocityLaneNSView: NSView {
    var model = PianoRollModel() {
        didSet {
            guard model != oldValue else { return }
            needsDisplay = true
        }
    }

    var onSetVelocity: ((String, Int) -> Void)?
    var onCommitEdit: ((String) -> Void)?
    var onSelect: ((Set<String>) -> Void)?

    var selectedIds: Set<String> = [] {
        didSet { if selectedIds != oldValue { needsDisplay = true } }
    }

    private static let gutterWidth: CGFloat = 44
    private var draggingNoteId: String?

    override var isFlipped: Bool { true }
    required init?(coder: NSCoder) { nil }
    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
    }

    private var laneRect: NSRect {
        NSRect(x: Self.gutterWidth, y: 0,
               width: max(0, bounds.width - Self.gutterWidth), height: bounds.height)
    }

    private func x(forBeat beat: Double) -> CGFloat {
        laneRect.minX + CGFloat(beat / max(0.001, model.lengthBeats)) * laneRect.width
    }

    /// Notes that start on the same beat stack into one bar; the topmost one wins.
    private func note(at point: NSPoint) -> PianoRollModel.Note? {
        model.notes.reversed().first { abs(x(forBeat: $0.startBeats) - point.x) <= 5 }
    }

    private func velocity(atY pointY: CGFloat) -> Int {
        let fraction = (laneRect.maxY - 4 - pointY) / max(1, laneRect.height - 8)
        return max(1, min(127, Int((fraction * 127).rounded())))
    }

    override func mouseDown(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)
        guard let hit = note(at: point) else { return }
        draggingNoteId = hit.id
        // Grabbing a bar also selects its note, so the header velocity field follows.
        if !selectedIds.contains(hit.id) { onSelect?([hit.id]) }
        onSetVelocity?(hit.id, velocity(atY: point.y))
    }

    override func mouseDragged(with event: NSEvent) {
        guard let id = draggingNoteId else { return }
        onSetVelocity?(id, velocity(atY: convert(event.locationInWindow, from: nil).y))
    }

    override func mouseUp(with event: NSEvent) {
        if draggingNoteId != nil { onCommitEdit?("Note velocity") }
        draggingNoteId = nil
    }

    override func draw(_ dirtyRect: NSRect) {
        NSColor(hex: 0x191512).setFill()
        bounds.fill()
        NSColor(hex: 0x0b0806).setFill()
        NSRect(x: 0, y: 0, width: bounds.width, height: 1).fill()

        ("VEL" as NSString).draw(
            at: NSPoint(x: 6, y: 5),
            withAttributes: [
                .font: NSFont.monospacedSystemFont(ofSize: 7, weight: .bold),
                .foregroundColor: NSColor(hex: 0x6b6156),
            ])

        for note in model.notes {
            let barX = x(forBeat: note.startBeats)
            let top = laneRect.maxY - 4 - CGFloat(note.velocity) / 127.0 * (laneRect.height - 8)
            let selected = selectedIds.contains(note.id)
            // Selected bars glow amber like their notes; the rest carry the same velocity
            // colour as the note they belong to, so the two lanes read as one picture.
            (selected ? NSColor(hex: 0xffb15c) : PianoRollNSView.velocityColor(note.velocity)).setFill()
            NSRect(x: barX - 1.5, y: top, width: 3, height: laneRect.maxY - 4 - top).fill()
            (selected ? NSColor.white : NSColor(hex: 0xd8c8ff)).setFill()
            NSRect(x: barX - 3, y: top - 1, width: 6, height: 2).fill()
        }
    }
}

struct VelocityLane: NSViewRepresentable {
    let model: PianoRollModel
    let selectedIds: Set<String>
    let onSetVelocity: (String, Int) -> Void
    let onCommitEdit: (String) -> Void
    let onSelect: (Set<String>) -> Void

    func makeNSView(context: Context) -> VelocityLaneNSView { VelocityLaneNSView(frame: .zero) }

    func updateNSView(_ lane: VelocityLaneNSView, context: Context) {
        lane.model = model
        lane.selectedIds = selectedIds
        lane.onSetVelocity = onSetVelocity
        lane.onCommitEdit = onCommitEdit
        lane.onSelect = onSelect
    }
}

// MARK: - Controller lane (CC / pitch bend)

struct ControllerLaneModel: Equatable {
    var lengthBeats: Double = 8
    var title = "CC"
    var maxValue = 127
    /// A rest line to draw across the lane (0.5 for pitch bend), or nil.
    var centerFraction: Double?
    var events: [Event] = []
    var playheadBeats: Double = -1

    struct Event: Equatable {
        let id: String
        let beat: Double
        let value: Int
    }
}

/// A generic controller editor: points are placed, dragged (time + value) and double-clicked to
/// delete, connected by a line so the curve reads at a glance. One lane at a time — the picker
/// above chooses which controller it edits.
final class ControllerLaneNSView: NSView {
    var model = ControllerLaneModel() {
        didSet { if model != oldValue { needsDisplay = true } }
    }

    var onAdd: ((Double, Int) -> String?)?      // (beat, value) -> new id
    var onMove: ((String, Double, Int) -> Void)?
    var onDelete: ((String) -> Void)?
    var onCommitEdit: ((String) -> Void)?

    private static let gutterWidth: CGFloat = 44
    private var draggingId: String?

    override var isFlipped: Bool { true }
    required init?(coder: NSCoder) { nil }
    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
    }

    private var laneRect: NSRect {
        NSRect(x: Self.gutterWidth, y: 0,
               width: max(0, bounds.width - Self.gutterWidth), height: bounds.height)
    }

    private func x(forBeat beat: Double) -> CGFloat {
        laneRect.minX + CGFloat(beat / max(0.001, model.lengthBeats)) * laneRect.width
    }

    private func beat(atX pointX: CGFloat) -> Double {
        max(0, min(model.lengthBeats,
                   Double((pointX - laneRect.minX) / max(1, laneRect.width)) * model.lengthBeats))
    }

    private func y(forFraction fraction: Double) -> CGFloat {
        laneRect.maxY - 4 - CGFloat(fraction) * (laneRect.height - 8)
    }

    private func fraction(atY pointY: CGFloat) -> Double {
        Double(max(0, min(1, (laneRect.maxY - 4 - pointY) / max(1, laneRect.height - 8))))
    }

    private func value(atY pointY: CGFloat) -> Int {
        Int((fraction(atY: pointY) * Double(model.maxValue)).rounded())
    }

    private func event(at point: NSPoint) -> ControllerLaneModel.Event? {
        model.events.reversed().first {
            let ex = x(forBeat: $0.beat)
            let ey = y(forFraction: Double($0.value) / Double(max(1, model.maxValue)))
            return hypot(ex - point.x, ey - point.y) <= 6
        }
    }

    override func mouseDown(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)
        guard point.x >= laneRect.minX else { return }
        if let hit = self.event(at: point) {
            if event.clickCount >= 2 {
                onDelete?(hit.id)
                return
            }
            draggingId = hit.id
            return
        }
        // Empty: drop a point here and grab it, so one gesture places and shapes it.
        if let newId = onAdd?(beat(atX: point.x), value(atY: point.y)) {
            draggingId = newId
        }
    }

    override func mouseDragged(with event: NSEvent) {
        guard let id = draggingId else { return }
        let point = convert(event.locationInWindow, from: nil)
        onMove?(id, beat(atX: point.x), value(atY: point.y))
    }

    override func mouseUp(with event: NSEvent) {
        if draggingId != nil { onCommitEdit?("Controller edit") }
        draggingId = nil
    }

    override func draw(_ dirtyRect: NSRect) {
        NSColor(hex: 0x191512).setFill()
        bounds.fill()
        NSColor(hex: 0x0b0806).setFill()
        NSRect(x: 0, y: 0, width: bounds.width, height: 1).fill()

        (model.title as NSString).draw(
            at: NSPoint(x: 6, y: 5),
            withAttributes: [
                .font: NSFont.monospacedSystemFont(ofSize: 7, weight: .bold),
                .foregroundColor: NSColor(hex: 0x8a7f70),
            ])

        // The rest line (pitch bend centre).
        if let centre = model.centerFraction {
            NSColor(hex: 0x3a322b).setFill()
            NSRect(x: laneRect.minX, y: y(forFraction: centre), width: laneRect.width, height: 1).fill()
        }

        let sorted = model.events.sorted { $0.beat < $1.beat }
        let points = sorted.map { NSPoint(x: x(forBeat: $0.beat),
                                          y: y(forFraction: Double($0.value) / Double(max(1, model.maxValue)))) }

        // A soft fill under the connecting line, then the line, then the handles.
        if points.count >= 1 {
            let baseline = laneRect.maxY - 4
            let area = NSBezierPath()
            area.move(to: NSPoint(x: points.first!.x, y: baseline))
            for p in points { area.line(to: p) }
            area.line(to: NSPoint(x: points.last!.x, y: baseline))
            area.close()
            NSColor(hex: 0x9b7fd4).withAlphaComponent(0.14).setFill()
            area.fill()

            if points.count >= 2 {
                let line = NSBezierPath()
                line.move(to: points.first!)
                for p in points.dropFirst() { line.line(to: p) }
                NSColor(hex: 0x9b7fd4).setStroke()
                line.lineWidth = 1.5
                line.stroke()
            }
        }

        for p in points {
            let handle = NSRect(x: p.x - 2.5, y: p.y - 2.5, width: 5, height: 5)
            NSColor(hex: 0xd8c8ff).setFill()
            NSBezierPath(ovalIn: handle).fill()
        }

        if model.playheadBeats >= 0, model.playheadBeats <= model.lengthBeats {
            NSColor(hex: 0xff5252).withAlphaComponent(0.8).setFill()
            NSRect(x: x(forBeat: model.playheadBeats), y: 0, width: 1, height: bounds.height).fill()
        }
    }
}

struct ControllerLane: NSViewRepresentable {
    let model: ControllerLaneModel
    let onAdd: (Double, Int) -> String?
    let onMove: (String, Double, Int) -> Void
    let onDelete: (String) -> Void
    let onCommitEdit: (String) -> Void

    func makeNSView(context: Context) -> ControllerLaneNSView { ControllerLaneNSView(frame: .zero) }

    func updateNSView(_ lane: ControllerLaneNSView, context: Context) {
        lane.model = model
        lane.onAdd = onAdd
        lane.onMove = onMove
        lane.onDelete = onDelete
        lane.onCommitEdit = onCommitEdit
    }
}

struct PianoRoll: NSViewRepresentable {
    let model: PianoRollModel
    let selectedIds: Set<String>
    let tool: String
    let onAddNote: (Int, Double, Double) -> String?
    let onMoveNote: (String, Int, Double) -> Void
    let onResizeNote: (String, Double) -> Void
    let onDeleteNote: (String) -> Void
    let onCommitEdit: (String) -> Void
    let onSelect: (Set<String>) -> Void
    let onCopyNote: (String) -> String?
    let onPreviewNote: (Int, Int, Bool) -> Void
    let onPreviewAllNotesOff: () -> Void

    func makeNSView(context: Context) -> NSScrollView {
        let roll = PianoRollNSView(frame: .zero)
        let scroll = NSScrollView()
        scroll.hasVerticalScroller = true
        scroll.drawsBackground = false
        scroll.documentView = roll
        return scroll
    }

    func updateNSView(_ scroll: NSScrollView, context: Context) {
        guard let roll = scroll.documentView as? PianoRollNSView else { return }
        roll.model = model
        roll.selectedIds = selectedIds
        roll.tool = tool
        roll.onAddNote = onAddNote
        roll.onMoveNote = onMoveNote
        roll.onResizeNote = onResizeNote
        roll.onDeleteNote = onDeleteNote
        roll.onCommitEdit = onCommitEdit
        roll.onSelect = onSelect
        roll.onCopyNote = onCopyNote
        roll.onPreviewNote = onPreviewNote
        roll.onPreviewAllNotesOff = onPreviewAllNotesOff
        roll.frame = NSRect(x: 0, y: 0,
                            width: scroll.contentSize.width,
                            height: roll.idealHeight)

        // 128 rows is mostly silence. Centre on the notes the first time they appear,
        // and follow them when a transpose walks them off the visible rows.
        if let focus = roll.scrollTarget(visible: scroll.contentView.bounds) {
            scroll.contentView.scroll(to: NSPoint(x: 0, y: focus))
            scroll.reflectScrolledClipView(scroll.contentView)
        }
    }
}

/// The panel the piano roll lives in, docked under the timeline.
struct PianoRollPanel: View {
    @EnvironmentObject private var engine: EngineController
    // Playhead lives on its own clock now — observe it so the roll's playhead keeps moving in playback.
    @ObservedObject var clock: PlayheadClock

    @State private var dragStartHeight: CGFloat?
    @State private var laneDragStartHeight: CGFloat?

    var body: some View {
        if let region = engine.editingRegion {
            VStack(spacing: 0) {
                resizeHandle
                header(region)
                infoLine(region)
                PianoRoll(
                    model: model(for: region),
                    selectedIds: engine.selectedNoteIds,
                    tool: engine.pianoRollTool.rawValue,
                    onAddNote: { engine.addNote(pitch: $0, startBeats: $1, durationBeats: $2) },
                    onMoveNote: { engine.moveNote($0, pitch: $1, startBeats: $2) },
                    onResizeNote: { engine.resizeNote($0, durationBeats: $1) },
                    onDeleteNote: { engine.deleteNote($0) },
                    onCommitEdit: { engine.commitClipGesture($0) },
                    onSelect: { engine.setNoteSelection($0) },
                    onCopyNote: { engine.copyNote($0) },
                    onPreviewNote: { engine.previewNote(pitch: $0, velocity: $1, on: $2) },
                    onPreviewAllNotesOff: { engine.previewAllNotesOff() }
                )

                laneBar
                laneView(region)
                    .frame(height: engine.editorLaneHeight)
            }
            .frame(height: engine.pianoRollHeight)
            .background(Theme.Palette.panel)
        }
    }

    /// A grab bar on the top edge: drag up to make the editor taller, down to shrink it.
    private var resizeHandle: some View {
        ZStack {
            Theme.Palette.toolbar
            RoundedRectangle(cornerRadius: 1.5).fill(Theme.Palette.divider).frame(width: 44, height: 3)
        }
        .frame(height: 9)
        .contentShape(Rectangle())
        .gesture(
            // Measure in the global (screen) space, not the handle's own space: resizing moves the
            // handle, and a local-space translation would then chase its own movement and jitter.
            DragGesture(minimumDistance: 0, coordinateSpace: .global)
                .onChanged { value in
                    let base = dragStartHeight ?? engine.pianoRollHeight
                    if dragStartHeight == nil { dragStartHeight = base }
                    engine.pianoRollHeight = min(760, max(200, base - value.translation.height))
                }
                .onEnded { _ in dragStartHeight = nil }
        )
        .onHover { $0 ? NSCursor.resizeUpDown.push() : NSCursor.pop() }
    }

    /// The pointer-tool picker (Cubase/Logic): 선택 / 연필 / 지우개. Select never creates a note
    /// on a plain click.
    private var toolPicker: some View {
        HStack(spacing: 2) {
            ForEach(EngineController.PianoRollTool.allCases, id: \.self) { tool in
                Button { engine.pianoRollTool = tool } label: {
                    Image(systemName: tool.symbol)
                        .font(.system(size: 10))
                        .foregroundStyle(engine.pianoRollTool == tool ? Theme.Palette.text : Theme.Palette.textDim)
                        .frame(width: 22, height: 18)
                        .background(RoundedRectangle(cornerRadius: Theme.Radius.pill)
                            .fill(engine.pianoRollTool == tool ? Theme.Palette.accent.opacity(0.35) : Theme.Palette.button))
                }
                .buttonStyle(.plain)
                .help(tool.title)
            }
        }
    }

    /// The Cubase-style info line: the focused note's position, length, pitch and velocity, shown
    /// as numbers and editable in place. It follows a drag live, so nudging a note shows its value.
    @ViewBuilder
    private func infoLine(_ region: EngineController.MidiRegion) -> some View {
        let note = engine.focusedNote
        HStack(spacing: Theme.Space.md) {
            Text("정보")
                .font(Theme.Font.mono(8, .bold))
                .foregroundStyle(Theme.Palette.textFaint)

            InfoNumberField(label: "위치", value: note.map(\.startBeats), step: 0.25, format: "%.3f") { v in
                if let id = note?.id { engine.setNoteStart(id, beats: v) }
            }
            InfoNumberField(label: "길이", value: note.map(\.durationBeats), step: 0.25, format: "%.3f") { v in
                if let id = note?.id { engine.setNoteLength(id, beats: v) }
            }
            pitchField(note)
            VelocityField(value: engine.selectedNotesVelocity) { velocity, commit in
                engine.setSelectedNotesVelocity(velocity, commit: commit)
            }

            Spacer()
            if engine.selectedNoteIds.count > 1 {
                Text("\(engine.selectedNoteIds.count) 노트 · 세기는 전체 적용")
                    .font(Theme.Font.mono(8))
                    .foregroundStyle(Theme.Palette.textFainter)
            }
        }
        .padding(.horizontal, Theme.Space.md)
        .padding(.vertical, 3)
        .background(Theme.Palette.recess.opacity(0.6))
    }

    private func pitchField(_ note: EngineController.MidiNote?) -> some View {
        HStack(spacing: 3) {
            Text("피치")
                .font(Theme.Font.mono(8))
                .foregroundStyle(note == nil ? Theme.Palette.textFainter : Theme.Palette.textDim)
            stepperButton("−") { if let n = note { engine.setNotePitch(n.id, pitch: n.pitch - 1) } }
            Text(note.map { Self.pitchName($0.pitch) } ?? "—")
                .font(Theme.Font.mono(9))
                .foregroundStyle(Theme.Palette.text)
                .frame(width: 34)
            stepperButton("+") { if let n = note { engine.setNotePitch(n.id, pitch: n.pitch + 1) } }
        }
        .opacity(note == nil ? 0.45 : 1)
        .disabled(note == nil)
    }

    private func stepperButton(_ label: String, _ action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(label)
                .font(Theme.Font.mono(10, .bold))
                .foregroundStyle(Theme.Palette.textDim)
                .frame(width: 14, height: 16)
                .background(RoundedRectangle(cornerRadius: Theme.Radius.pill).fill(Theme.Palette.button))
        }
        .buttonStyle(.plain)
    }

    static func pitchName(_ pitch: Int) -> String {
        let names = ["C", "C♯", "D", "D♯", "E", "F", "F♯", "G", "G♯", "A", "A♯", "B"]
        return names[((pitch % 12) + 12) % 12] + String(pitch / 12 - 1)
    }

    /// The controller-lane picker + its hint, sitting between the roll and the lane.
    private var laneBar: some View {
        HStack(spacing: Theme.Space.sm) {
            Text("컨트롤러")
                .font(Theme.Font.mono(8))
                .foregroundStyle(Theme.Palette.textFaint)
            Menu {
                ForEach(EngineController.editorLanes, id: \.self) { lane in
                    Button {
                        engine.editorLane = lane
                    } label: {
                        // A filled dot marks a lane that actually holds recorded/drawn data, so a
                        // recorded modulation or pitch bend is findable instead of hidden behind the
                        // default velocity lane.
                        let mark = engine.editorLane == lane ? "checkmark"
                                 : (engine.laneHasData(lane) ? "circle.fill" : "")
                        Label(lane.title, systemImage: mark)
                    }
                }
            } label: {
                HStack(spacing: 3) {
                    Text("\(engine.editorLane.title) ▾")
                        .font(Theme.Font.mono(8))
                        .foregroundStyle(Theme.Palette.textDim)
                    // A dot on the picker itself when another lane has data the user isn't seeing.
                    if engine.hasHiddenLaneData {
                        Circle().fill(Theme.Palette.amber).frame(width: 4, height: 4)
                    }
                }
                .padding(.horizontal, 6)
                .padding(.vertical, 2)
                .background(RoundedRectangle(cornerRadius: Theme.Radius.pill).fill(Theme.Palette.button))
            }
            .menuStyle(.borderlessButton)
            .menuIndicator(.hidden)
            .fixedSize()

            Text(engine.editorLane == .velocity
                 ? "VEL 레인: 바 드래그 = 세기 · 바 클릭 = 노트 선택"
                 : "레인: 빈 곳 클릭 = 점 추가·드래그로 조정 · 점 더블클릭 = 삭제")
                .font(Theme.Font.mono(8))
                .foregroundStyle(Theme.Palette.textFainter)
            Spacer()
        }
        .padding(.horizontal, Theme.Space.md)
        .padding(.vertical, 3)
        .background(Theme.Palette.toolbar)
        // The bar doubles as the lane's grab handle: drag it up to give the velocities more
        // room, down to hand it back to the keyboard. Same global-space measurement as the
        // editor's own handle — a local translation would chase the handle's own movement.
        .contentShape(Rectangle())
        .onHover { inside in
            if inside { NSCursor.resizeUpDown.push() } else { NSCursor.pop() }
        }
        .gesture(
            DragGesture(minimumDistance: 0, coordinateSpace: .global)
                .onChanged { value in
                    let base = laneDragStartHeight ?? engine.editorLaneHeight
                    if laneDragStartHeight == nil { laneDragStartHeight = base }
                    // Dragging up (negative dy) grows the lane, and it can never eat the
                    // whole editor.
                    let maximum = max(44, engine.pianoRollHeight - 160)
                    engine.editorLaneHeight = min(maximum, max(24, base - value.translation.height))
                }
                .onEnded { _ in laneDragStartHeight = nil }
        )
    }

    @ViewBuilder
    private func laneView(_ region: EngineController.MidiRegion) -> some View {
        if engine.editorLane == .velocity {
            VelocityLane(
                model: model(for: region),
                selectedIds: engine.selectedNoteIds,
                onSetVelocity: { engine.setNoteVelocity($0, $1) },
                onCommitEdit: { engine.commitClipGesture($0) },
                onSelect: { engine.setNoteSelection($0) }
            )
        } else {
            ControllerLane(
                model: controllerModel(for: region),
                onAdd: { engine.addLaneEvent(beat: $0, value: $1) },
                onMove: { engine.moveLaneEvent($0, beat: $1, value: $2) },
                onDelete: { engine.deleteLaneEvent($0) },
                onCommitEdit: { engine.commitClipGesture($0) }
            )
        }
    }

    private func controllerModel(for region: EngineController.MidiRegion) -> ControllerLaneModel {
        let secondsPerBeat = 60.0 / Double(engine.tempoBpm)
        var model = ControllerLaneModel()
        model.lengthBeats = max(1, region.durationSeconds / secondsPerBeat)
        model.title = engine.editorLane.title
        model.maxValue = engine.editorLane.maxValue
        model.centerFraction = engine.editorLane.centerFraction
        model.events = engine.laneEvents(inRegion: region.id).map {
            ControllerLaneModel.Event(id: $0.id, beat: $0.beat, value: $0.value)
        }
        model.playheadBeats = (engine.playheadSeconds - region.startSeconds) / secondsPerBeat
        return model
    }

    /// The Logic/Cubase quantize grid: straight, triplet (T), and dotted (.) values, in beats
    /// (a quarter note is 1 beat). Quantizes the selection when notes are picked, else the region.
    private static let quantizeGrid: [(String, Double)] = [
        ("1/1", 4.0), ("1/2", 2.0), ("1/4", 1.0),
        ("1/8", 0.5), ("1/16", 0.25), ("1/32", 0.125), ("1/64", 0.0625),
        ("1/4T", 2.0 / 3.0), ("1/8T", 1.0 / 3.0), ("1/16T", 1.0 / 6.0),
        ("1/4.", 1.5), ("1/8.", 0.75), ("1/16.", 0.375),
    ]

    private func header(_ region: EngineController.MidiRegion) -> some View {
        HStack(spacing: Theme.Space.sm) {
            Text(region.name)
                .font(Theme.Font.ui(10, .bold))
                .foregroundStyle(Theme.Palette.text)
            Text(region.trackName)
                .font(Theme.Font.mono(8))
                .foregroundStyle(Theme.Palette.textFaint)

            Divider().frame(height: 12)

            toolPicker

            Divider().frame(height: 12)

            quantizeMenu(region)
            tool("+8ve") { engine.transposeRegion(region.id, semitones: 12) }
            tool("−8ve") { engine.transposeRegion(region.id, semitones: -12) }
            tool("+1") { engine.transposeRegion(region.id, semitones: 1) }
            tool("−1") { engine.transposeRegion(region.id, semitones: -1) }
            tool("휴머나이즈") { engine.humanizeRegion(region.id) }
            tool("복제") { engine.duplicateRegion(region.id) }
            tool("분할") { engine.splitRegionAtPlayhead(region.id) }
            // Glue: joins the selected notes of each pitch into one. Disabled until the selection
            // actually contains two notes that share a pitch, so it never silently does nothing.
            tool("붙이기") { engine.glueSelectedNotes() }
                .disabled(!engine.canGlueSelectedNotes)
                .help("선택한 노트를 음정별로 하나로 붙입니다 (사이 간격은 흡수)")
            // The rest of the Key Editor functions. With nothing selected they act on the whole
            // region, which is how Cubase's Functions menu behaves.
            tool("레가토") { engine.applyNoteLegato() }
                .help("각 노트를 다음 노트 시작점까지 늘입니다 · 선택 없으면 리전 전체")
            tool("겹침 제거") { engine.deleteNoteOverlaps() }
                .help("같은 음정의 노트가 다음 노트를 침범하면 잘라냅니다 · 선택 없으면 리전 전체")
            Menu("길이 고정") {
                ForEach([("1/16", 0.25), ("1/8", 0.5), ("1/4", 1.0), ("1/2", 2.0), ("1마디", 4.0)],
                        id: \.0) { label, beats in
                    Button(label) { engine.setNoteLengths(beats: beats) }
                }
            }
            .menuStyle(.borderlessButton)
            .fixedSize()
            .help("선택한 노트를 같은 길이로 · 선택 없으면 리전 전체")

            Spacer()

            if !engine.selectedNoteIds.isEmpty {
                Text("\(engine.selectedNoteIds.count) 선택")
                    .font(Theme.Font.mono(8))
                    .foregroundStyle(Theme.Palette.accent)
            }
            Text("클릭 = 노트 · 드래그 = 마퀴 선택 · ⇧클릭 = 다중 · ⌥드래그 = 복사 · 더블클릭 = 삭제")
                .font(Theme.Font.mono(8))
                .foregroundStyle(Theme.Palette.textFainter)

            Button("닫기") { engine.editingRegionId = nil }
                .buttonStyle(.plain)
                .font(Theme.Font.ui(9))
                .foregroundStyle(Theme.Palette.textDim)
                .padding(.horizontal, 8)
                .padding(.vertical, 3)
                .background(
                    RoundedRectangle(cornerRadius: Theme.Radius.pill).fill(Theme.Palette.button)
                )
        }
        .padding(.horizontal, Theme.Space.md)
        .padding(.vertical, 6)
        .background(Theme.Palette.toolbar)
    }

    private func quantizeMenu(_ region: EngineController.MidiRegion) -> some View {
        Menu {
            let scope = engine.selectedNoteIds.isEmpty ? "리전 전체" : "선택 \(engine.selectedNoteIds.count)"
            Text("퀀타이즈 대상: \(scope)")
            Divider()
            ForEach(Self.quantizeGrid, id: \.0) { label, beats in
                Button(label) { engine.quantize(regionId: region.id, beatQuantum: beats) }
            }
        } label: {
            Text("퀀타이즈 ▾")
                .font(Theme.Font.mono(8))
                .foregroundStyle(Theme.Palette.textDim)
                .padding(.horizontal, 6)
                .padding(.vertical, 3)
                .background(
                    RoundedRectangle(cornerRadius: Theme.Radius.pill).fill(Theme.Palette.button)
                )
        }
        .menuStyle(.borderlessButton)
        .menuIndicator(.hidden)
        .fixedSize()
    }

    private func tool(_ label: String, _ action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(label)
                .font(Theme.Font.mono(8))
                .foregroundStyle(Theme.Palette.textDim)
                .padding(.horizontal, 6)
                .padding(.vertical, 3)
                .background(
                    RoundedRectangle(cornerRadius: Theme.Radius.pill).fill(Theme.Palette.button)
                )
        }
        .buttonStyle(.plain)
    }

    private func model(for region: EngineController.MidiRegion) -> PianoRollModel {
        let secondsPerBeat = 60.0 / Double(engine.tempoBpm)
        var model = PianoRollModel()
        model.regionName = region.name
        model.lengthBeats = max(1, region.durationSeconds / secondsPerBeat)
        model.beatsPerBar = engine.timeSignature.numerator
        model.notes = engine.notes(inRegion: region.id).map {
            PianoRollModel.Note(id: $0.id, pitch: $0.pitch, startBeats: $0.startBeats,
                                durationBeats: $0.durationBeats, velocity: $0.velocity)
        }
        // Negative while the transport is outside the region, which hides the playhead.
        model.playheadBeats = (engine.playheadSeconds - region.startSeconds) / secondsPerBeat
        return model
    }
}

/// The selection's velocity: typed exactly, or nudged ±1 with the steppers. `value` is nil (and
/// the whole control disabled/dimmed) when nothing is selected; it shows the common value, or the
/// rounded average when the selection disagrees. A commit is one undo step.
private struct VelocityField: View {
    let value: Int?
    let apply: (Int, Bool) -> Void   // (velocity, commit)

    @State private var text = ""
    @FocusState private var focused: Bool

    var body: some View {
        HStack(spacing: 3) {
            Text("세기")
                .font(Theme.Font.mono(8))
                .foregroundStyle(value == nil ? Theme.Palette.textFainter : Theme.Palette.textDim)
            stepper("−") { if let v = value { apply(max(1, v - 1), true) } }
            TextField("", text: $text)
                .textFieldStyle(.plain)
                .font(Theme.Font.mono(9))
                .foregroundStyle(Theme.Palette.text)
                .multilineTextAlignment(.center)
                .frame(width: 26)
                .focused($focused)
                .onSubmit { commit() }
                .onChange(of: focused) { if !$1 { commit() } }
                .padding(.horizontal, 4)
                .padding(.vertical, 2)
                .background(
                    RoundedRectangle(cornerRadius: Theme.Radius.button)
                        .fill(Theme.Palette.recess)
                        .overlay(RoundedRectangle(cornerRadius: Theme.Radius.button)
                            .stroke(Theme.Palette.divider, lineWidth: 1))
                )
            stepper("+") { if let v = value { apply(min(127, v + 1), true) } }
        }
        .opacity(value == nil ? 0.45 : 1)
        .disabled(value == nil)
        .onAppear { text = value.map(String.init) ?? "" }
        // Follow the selection while the field is not being typed into.
        .onChange(of: value) { if !focused { text = $1.map(String.init) ?? "" } }
    }

    private func commit() {
        guard let v = Int(text.trimmingCharacters(in: .whitespaces)) else {
            text = value.map(String.init) ?? ""
            return
        }
        apply(max(1, min(127, v)), true)
    }

    private func stepper(_ label: String, _ action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(label)
                .font(Theme.Font.mono(10, .bold))
                .foregroundStyle(Theme.Palette.textDim)
                .frame(width: 14, height: 16)
                .background(RoundedRectangle(cornerRadius: Theme.Radius.pill).fill(Theme.Palette.button))
        }
        .buttonStyle(.plain)
    }
}

/// A numeric info-line field for a `Double` value (note position / length in beats). Follows the
/// selection live, commits on Enter or blur, and steps by `step`. Dimmed with no focused note.
private struct InfoNumberField: View {
    let label: String
    let value: Double?
    let step: Double
    let format: String
    let apply: (Double) -> Void

    @State private var text = ""
    @FocusState private var focused: Bool

    var body: some View {
        HStack(spacing: 3) {
            Text(label)
                .font(Theme.Font.mono(8))
                .foregroundStyle(value == nil ? Theme.Palette.textFainter : Theme.Palette.textDim)
            stepper("−") { if let v = value { apply(max(0, v - step)) } }
            TextField("", text: $text)
                .textFieldStyle(.plain)
                .font(Theme.Font.mono(9))
                .foregroundStyle(Theme.Palette.text)
                .multilineTextAlignment(.center)
                .frame(width: 44)
                .focused($focused)
                .onSubmit { commit() }
                .onChange(of: focused) { if !$1 { commit() } }
                .padding(.horizontal, 4)
                .padding(.vertical, 2)
                .background(
                    RoundedRectangle(cornerRadius: Theme.Radius.button)
                        .fill(Theme.Palette.recess)
                        .overlay(RoundedRectangle(cornerRadius: Theme.Radius.button)
                            .stroke(Theme.Palette.divider, lineWidth: 1))
                )
            stepper("+") { if let v = value { apply(v + step) } }
        }
        .opacity(value == nil ? 0.45 : 1)
        .disabled(value == nil)
        .onAppear { text = formatted(value) }
        .onChange(of: value) { if !focused { text = formatted($1) } }
    }

    private func formatted(_ v: Double?) -> String { v.map { String(format: format, $0) } ?? "" }

    private func commit() {
        guard let v = Double(text.trimmingCharacters(in: .whitespaces)) else {
            text = formatted(value)
            return
        }
        apply(max(0, v))
    }

    private func stepper(_ label: String, _ action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(label)
                .font(Theme.Font.mono(10, .bold))
                .foregroundStyle(Theme.Palette.textDim)
                .frame(width: 14, height: 16)
                .background(RoundedRectangle(cornerRadius: Theme.Radius.pill).fill(Theme.Palette.button))
        }
        .buttonStyle(.plain)
    }
}
