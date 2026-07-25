import AppKit
import SwiftUI

private final class VirtualKeyboardNSView: NSView {
    var basePitch = 48 { didSet { needsDisplay = true } }
    var velocity = 100
    var noteEvent: ((Int, Int, Bool) -> Void)?
    var allNotesOff: (() -> Void)?
    private var heldPitch: Int?

    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }

    private let blackPitchClasses: Set<Int> = [1, 3, 6, 8, 10]

    private func whitePitches() -> [Int] {
        (basePitch...(basePitch + 24)).filter { !blackPitchClasses.contains($0 % 12) }
    }

    private func whiteRect(index: Int) -> NSRect {
        let count = CGFloat(whitePitches().count)
        let width = bounds.width / max(1, count)
        return NSRect(x: CGFloat(index) * width, y: 0, width: width + 0.5, height: bounds.height)
    }

    private func blackRect(pitch: Int) -> NSRect? {
        guard blackPitchClasses.contains(pitch % 12) else { return nil }
        let whites = whitePitches()
        guard let next = whites.firstIndex(where: { $0 > pitch }) else { return nil }
        let whiteWidth = bounds.width / CGFloat(whites.count)
        return NSRect(x: CGFloat(next) * whiteWidth - whiteWidth * 0.31,
                      y: 0, width: whiteWidth * 0.62, height: bounds.height * 0.62)
    }

    private func pitch(at point: NSPoint) -> Int? {
        for pitch in basePitch...(basePitch + 24) {
            if let rect = blackRect(pitch: pitch), rect.contains(point) { return pitch }
        }
        for (index, pitch) in whitePitches().enumerated() where whiteRect(index: index).contains(point) {
            return pitch
        }
        return nil
    }

    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self)
        changeHeldPitch(to: pitch(at: convert(event.locationInWindow, from: nil)))
    }

    override func mouseDragged(with event: NSEvent) {
        changeHeldPitch(to: pitch(at: convert(event.locationInWindow, from: nil)))
    }

    override func mouseUp(with event: NSEvent) { changeHeldPitch(to: nil) }

    override func viewDidMoveToWindow() {
        if window == nil {
            changeHeldPitch(to: nil)
            allNotesOff?()
        }
    }

    private func changeHeldPitch(to pitch: Int?) {
        guard pitch != heldPitch else { return }
        if let heldPitch { noteEvent?(heldPitch, 0, false) }
        heldPitch = pitch
        if let pitch { noteEvent?(pitch, velocity, true) }
        needsDisplay = true
    }

    override func draw(_ dirtyRect: NSRect) {
        NSColor(hex: 0x171615).setFill()
        bounds.fill()
        for (index, pitch) in whitePitches().enumerated() {
            let rect = whiteRect(index: index).insetBy(dx: 0.6, dy: 1)
            (heldPitch == pitch ? NSColor(hex: 0xf0a43b) : NSColor(hex: 0xe8e4dc)).setFill()
            NSBezierPath(roundedRect: rect, xRadius: 2, yRadius: 2).fill()
            NSColor(hex: 0x3a3835).setStroke()
            NSBezierPath(roundedRect: rect, xRadius: 2, yRadius: 2).stroke()
            if pitch % 12 == 0 {
                let label = "C\(pitch / 12 - 1)" as NSString
                label.draw(at: NSPoint(x: rect.minX + 4, y: rect.maxY - 17),
                           withAttributes: [.font: NSFont.monospacedSystemFont(ofSize: 9, weight: .medium),
                                            .foregroundColor: NSColor(hex: 0x504c46)])
            }
        }
        for pitch in basePitch...(basePitch + 24) {
            guard let rect = blackRect(pitch: pitch) else { continue }
            (heldPitch == pitch ? NSColor(hex: 0xe68d27) : NSColor(hex: 0x1b1a19)).setFill()
            NSBezierPath(roundedRect: rect, xRadius: 2, yRadius: 2).fill()
            NSColor(hex: 0x080808).setStroke()
            NSBezierPath(roundedRect: rect, xRadius: 2, yRadius: 2).stroke()
        }
    }
}

private struct VirtualKeyboardSurface: NSViewRepresentable {
    let basePitch: Int
    let velocity: Int
    let onNote: (Int, Int, Bool) -> Void
    let onAllNotesOff: () -> Void

    func makeNSView(context: Context) -> VirtualKeyboardNSView {
        let view = VirtualKeyboardNSView()
        view.noteEvent = onNote
        view.allNotesOff = onAllNotesOff
        return view
    }

    func updateNSView(_ view: VirtualKeyboardNSView, context: Context) {
        view.basePitch = basePitch
        view.velocity = velocity
        view.noteEvent = onNote
        view.allNotesOff = onAllNotesOff
    }

    static func dismantleNSView(_ view: VirtualKeyboardNSView, coordinator: ()) {
        view.allNotesOff?()
    }
}

struct VirtualKeyboardView: View {
    @EnvironmentObject private var engine: EngineController
    @State private var octave = 3
    @State private var velocity: Double = 100

    private var targetName: String {
        engine.selectedVirtualKeyboardTrack?.name ?? "악기 트랙을 선택하세요"
    }

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 10) {
                Image(systemName: "pianokeys")
                    .foregroundStyle(Theme.Palette.instrument)
                VStack(alignment: .leading, spacing: 1) {
                    Text("VIRTUAL MIDI KEYBOARD")
                        .font(Theme.Font.mono(10, .bold))
                    Text(targetName)
                        .font(Theme.Font.ui(9))
                        .foregroundStyle(engine.selectedVirtualKeyboardTrack == nil
                                         ? Theme.Palette.red : Theme.Palette.textDim)
                }
                Spacer()
                Button { octave = max(0, octave - 1); engine.virtualKeyboardAllNotesOff() } label: {
                    Image(systemName: "minus")
                }
                Text("C\(octave)")
                    .font(Theme.Font.mono(10, .semibold))
                    .frame(width: 28)
                Button { octave = min(7, octave + 1); engine.virtualKeyboardAllNotesOff() } label: {
                    Image(systemName: "plus")
                }
                Text("VELOCITY").font(Theme.Font.mono(8, .semibold))
                Slider(value: $velocity, in: 1...127, step: 1).frame(width: 105)
                Text("\(Int(velocity))").font(Theme.Font.mono(9, .semibold)).frame(width: 26)
                Button("ALL OFF") { engine.virtualKeyboardAllNotesOff() }
                    .font(Theme.Font.mono(8, .semibold))
            }
            .buttonStyle(.borderless)
            .padding(.horizontal, 12)
            .frame(height: 42)
            .background(Theme.Palette.ruler)

            VirtualKeyboardSurface(basePitch: 12 * (octave + 1),
                                   velocity: Int(velocity),
                                   onNote: { engine.virtualKeyboardNote(pitch: $0, velocity: $1, on: $2) },
                                   onAllNotesOff: { engine.virtualKeyboardAllNotesOff() })
                .disabled(engine.selectedVirtualKeyboardTrack == nil)
        }
        .background(Theme.Palette.background)
        .onDisappear { engine.virtualKeyboardAllNotesOff() }
    }
}

struct VirtualKeyboardCommands: Commands {
    @Environment(\.openWindow) private var openWindow
    var body: some Commands {
        CommandMenu("윈도우") {
            Button("가상 MIDI 키보드") { openWindow(id: "virtual-keyboard") }
                .keyboardShortcut("k", modifiers: [.command, .option])
        }
    }
}
