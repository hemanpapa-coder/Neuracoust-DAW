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

    var onAddNote: ((Int, Double, Double) -> Void)?          // (pitch, startBeats, durationBeats)
    var onMoveNote: ((String, Int, Double) -> Void)?         // (id, pitch, startBeats)
    var onResizeNote: ((String, Double) -> Void)?            // (id, durationBeats)
    var onDeleteNote: ((String) -> Void)?
    var onCommitEdit: ((String) -> Void)?

    /// C1 to C7 — the range a part is realistically written in, and enough that the
    /// view never needs to scroll.
    private static let lowestPitch = 24
    private static let highestPitch = 96
    private static let keyboardWidth: CGFloat = 44
    private static let rowHeight: CGFloat = 10
    private static let resizeHandleWidth: CGFloat = 6

    /// New notes are one sixteenth long, and the grid snaps to sixteenths.
    private static let quantumBeats = 0.25

    private enum Drag {
        case none
        case moving(id: String, grabOffsetBeats: Double)
        case resizing(id: String)
    }

    private var drag = Drag.none

    override var isFlipped: Bool { true }

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
        guard point.x >= gridRect.minX else { return }

        if let hit = note(at: point) {
            if event.clickCount >= 2 {
                onDeleteNote?(hit.id)
            } else if noteRect(hit).maxX - point.x <= Self.resizeHandleWidth {
                drag = .resizing(id: hit.id)
            } else {
                drag = .moving(id: hit.id, grabOffsetBeats: beat(atX: point.x) - hit.startBeats)
            }
            return
        }

        let start = max(0, snapped(beat(atX: point.x)))
        guard start < model.lengthBeats else { return }
        onAddNote?(pitch(atY: point.y), start, Self.quantumBeats)
    }

    override func mouseDragged(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)

        switch drag {
        case .none:
            break
        case .moving(let id, let grabOffset):
            let start = max(0, snapped(beat(atX: point.x) - grabOffset))
            onMoveNote?(id, pitch(atY: point.y), start)
        case .resizing(let id):
            guard let note = model.notes.first(where: { $0.id == id }) else { break }
            // A note has to stay long enough to sound; the engine clips it to the region.
            let duration = max(Self.quantumBeats, snapped(beat(atX: point.x) - note.startBeats))
            onResizeNote?(id, duration)
        }
    }

    override func mouseUp(with event: NSEvent) {
        switch drag {
        case .moving: onCommitEdit?("Move note")
        case .resizing: onCommitEdit?("Resize note")
        case .none: break
        }
        drag = .none
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

    private func drawNotes() {
        for note in model.notes {
            let rect = noteRect(note)
            // Velocity reads as brightness — the one place it is visible at all.
            let strength = 0.35 + 0.65 * CGFloat(note.velocity) / 127.0
            NSColor(hex: 0x9b7fd4).withAlphaComponent(strength).setFill()
            let body = NSBezierPath(roundedRect: rect, xRadius: 2, yRadius: 2)
            body.fill()
            NSColor(hex: 0xd8c8ff).setStroke()
            body.lineWidth = 1
            body.stroke()
        }
    }

    private func drawKeyboard(_ context: CGContext) {
        NSColor(hex: 0x151110).setFill()
        NSRect(x: 0, y: 0, width: Self.keyboardWidth, height: bounds.height).fill()

        for pitch in Self.lowestPitch...Self.highestPitch {
            let rect = NSRect(x: 0, y: y(forPitch: pitch), width: Self.keyboardWidth, height: Self.rowHeight)
            NSColor(hex: isAccidental(pitch) ? 0x1d1917 : 0xd6cec1).setFill()
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

struct PianoRoll: NSViewRepresentable {
    let model: PianoRollModel
    let onAddNote: (Int, Double, Double) -> Void
    let onMoveNote: (String, Int, Double) -> Void
    let onResizeNote: (String, Double) -> Void
    let onDeleteNote: (String) -> Void
    let onCommitEdit: (String) -> Void

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
        roll.onAddNote = onAddNote
        roll.onMoveNote = onMoveNote
        roll.onResizeNote = onResizeNote
        roll.onDeleteNote = onDeleteNote
        roll.onCommitEdit = onCommitEdit
        roll.frame = NSRect(x: 0, y: 0,
                            width: scroll.contentSize.width,
                            height: roll.idealHeight)
    }
}

/// The panel the piano roll lives in, docked under the timeline.
struct PianoRollPanel: View {
    @EnvironmentObject private var engine: EngineController

    var body: some View {
        if let region = engine.editingRegion {
            VStack(spacing: 0) {
                header(region)
                PianoRoll(
                    model: model(for: region),
                    onAddNote: { engine.addNote(pitch: $0, startBeats: $1, durationBeats: $2) },
                    onMoveNote: { engine.moveNote($0, pitch: $1, startBeats: $2) },
                    onResizeNote: { engine.resizeNote($0, durationBeats: $1) },
                    onDeleteNote: { engine.deleteNote($0) },
                    onCommitEdit: { engine.commitClipGesture($0) }
                )
            }
            .frame(height: 260)
            .background(Theme.Palette.panel)
        }
    }

    private func header(_ region: EngineController.MidiRegion) -> some View {
        HStack(spacing: Theme.Space.sm) {
            Text(region.name)
                .font(Theme.Font.ui(10, .bold))
                .foregroundStyle(Theme.Palette.text)
            Text(region.trackName)
                .font(Theme.Font.mono(8))
                .foregroundStyle(Theme.Palette.textFaint)

            Spacer()

            Text("클릭 = 노트 추가 · 더블클릭 = 삭제 · 오른쪽 끝 = 길이")
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
