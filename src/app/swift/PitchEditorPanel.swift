import SwiftUI
import AppKit

// The pitch editor, opened for a clip under the timeline (like the piano roll). Two selectable modes:
//   • 멜로다인 — note blobs from YIN detection; drag a note up/down to change its pitch (semitone snap).
//   • 세라토 앵커 — global time-ratio + pitch, plus draggable time-remap anchors.
// The engine holds the detected notes and the DSP; this view is the interaction surface. v1 skeleton.
struct PitchEditorPanel: View {
    @EnvironmentObject private var engine: EngineController

    var body: some View {
        if engine.pitchEditorClipId != nil {
            VStack(spacing: 0) {
                header
                Divider().overlay(Theme.Palette.divider)
                if engine.pitchEditorMode == .melodyne {
                    MelodyneNoteCanvas(
                        notes: engine.pitchNotes,
                        clipDuration: engine.pitchEditorClipDuration,
                        peaks: engine.pitchClipPeaks,
                        movedTimes: engine.percussiveTimeEdits,
                        onOffset: { engine.setPitchNoteOffset($0, semitones: $1) },
                        onMoveTransient: { engine.setTransientTime($0, $1) })
                } else {
                    SeratoAnchorCanvas(
                        anchors: engine.timeMapAnchors,
                        onChange: { engine.timeMapAnchors = $0 })
                }
            }
            .frame(height: 260)
            .background(Theme.Palette.background)
        }
    }

    private var header: some View {
        HStack(spacing: Theme.Space.md) {
            Text("피치 에디터").font(Theme.Font.ui(11, .semibold)).foregroundStyle(Theme.Palette.text)
            Picker("", selection: $engine.pitchEditorMode) {
                ForEach(EngineController.PitchEditorMode.allCases, id: \.self) { Text($0.rawValue).tag($0) }
            }
            .pickerStyle(.segmented).labelsHidden().frame(width: 180)

            if engine.pitchEditorMode == .melodyne {
                Picker("", selection: Binding(
                    get: { engine.detectionMode },
                    set: { engine.detectionMode = $0; engine.redetectNotes() })) {
                    ForEach(EngineController.DetectionMode.allCases, id: \.self) { Text($0.label).tag($0) }
                }
                .pickerStyle(.segmented).labelsHidden().frame(width: 168)
                if engine.crepeAvailable && engine.detectionMode == .melodic {
                    Toggle("정밀(CREPE)", isOn: Binding(
                        get: { engine.useCrepe },
                        set: { engine.useCrepe = $0; engine.redetectNotes() }))
                        .toggleStyle(.checkbox).font(Theme.Font.ui(9))
                    if engine.useCrepe && engine.crepeTinyAvailable {
                        Toggle("빠름(tiny)", isOn: Binding(
                            get: { engine.useCrepeTiny },
                            set: { engine.useCrepeTiny = $0; engine.redetectNotes() }))
                            .toggleStyle(.checkbox).font(Theme.Font.ui(9))
                            .help("작고 빠른 모델 — 긴 파일 빠른 검출용, 정확도는 약간 낮음")
                    }
                }
                Text(engine.detectionMode == .percussive
                     ? "\(engine.pitchNotes.count)개 트랜지언트"
                     : "\(engine.pitchNotes.count)개 노트 · 위/아래 드래그로 음정")
                    .font(Theme.Font.mono(9)).foregroundStyle(Theme.Palette.textFaint)
            } else {
                anchorControls
            }
            Spacer()
            Toggle("포먼트 보존", isOn: $engine.formantPreserve)
                .toggleStyle(.checkbox).font(Theme.Font.ui(9))
                .help("음정을 바꿔도 음색(포먼트)을 유지 — 치프멍크 방지. 끄면 원음 그대로 이동")
            Button { engine.previewPitchEdit() } label: { Image(systemName: "play.circle") }
                .buttonStyle(.plain).help("편집 결과 미리듣기")
            Button("파일로 내보내기") { engine.exportPitchEditToFile() }.font(Theme.Font.ui(10))
            Button("적용") {
                if engine.pitchEditorMode == .anchor { engine.applyTimeMapEdit() }
                else if engine.detectionMode == .percussive { engine.applyPercussiveTiming() }
                else { engine.applyPitchEdits() }
            }
            .font(Theme.Font.ui(10, .semibold))
            Button("닫기") { engine.closePitchEditor() }.font(Theme.Font.ui(10))
        }
        .padding(.horizontal, Theme.Space.lg).padding(.vertical, Theme.Space.sm)
    }

    private var anchorControls: some View {
        HStack(spacing: Theme.Space.md) {
            HStack(spacing: 4) {
                Text("길이").font(Theme.Font.ui(9)).foregroundStyle(Theme.Palette.textFaint)
                Slider(value: $engine.pitchEditTimeRatio, in: 0.25...4.0).frame(width: 90).controlSize(.mini)
                Text(String(format: "%.2f×", engine.pitchEditTimeRatio)).font(Theme.Font.mono(9)).foregroundStyle(Theme.Palette.textSecondary)
            }
            HStack(spacing: 4) {
                Text("피치").font(Theme.Font.ui(9)).foregroundStyle(Theme.Palette.textFaint)
                Slider(value: $engine.pitchEditSemitones, in: -12...12).frame(width: 90).controlSize(.mini)
                Text(String(format: "%+.0f st", engine.pitchEditSemitones)).font(Theme.Font.mono(9)).foregroundStyle(Theme.Palette.textSecondary)
            }
            Text("· 더블클릭으로 앵커 추가").font(Theme.Font.mono(9)).foregroundStyle(Theme.Palette.textFaint)
        }
    }
}

// MARK: - Melodyne note canvas (AppKit)

struct MelodyneNoteCanvas: NSViewRepresentable {
    var notes: [EngineController.PitchNote]
    var clipDuration: Double
    var peaks: [SIMD2<Float>]
    var movedTimes: [Int: Double]
    var onOffset: (Int, Double) -> Void
    var onMoveTransient: (Int, Double) -> Void

    func makeNSView(context: Context) -> MelodyneNoteNSView {
        let v = MelodyneNoteNSView()
        v.onOffset = onOffset; v.onMoveTransient = onMoveTransient
        return v
    }
    func updateNSView(_ v: MelodyneNoteNSView, context: Context) {
        v.notes = notes
        v.clipDuration = max(0.1, clipDuration)
        v.peaks = peaks
        v.movedTimes = movedTimes
        v.onOffset = onOffset; v.onMoveTransient = onMoveTransient
        v.needsDisplay = true
    }
}

final class MelodyneNoteNSView: NSView {
    var notes: [EngineController.PitchNote] = [] { didSet { needsDisplay = true } }
    var clipDuration: Double = 1.0
    var peaks: [SIMD2<Float>] = []
    var movedTimes: [Int: Double] = [:] { didSet { needsDisplay = true } }
    var onOffset: ((Int, Double) -> Void)?
    var onMoveTransient: ((Int, Double) -> Void)?

    private var dragIndex: Int? = nil
    private var dragStartY: CGFloat = 0
    private var dragBaseOffset: Double = 0
    private var transientDragIndex: Int? = nil

    // Percussive transient's shown time (its dragged position, or its detected onset).
    private func transientTime(_ i: Int, _ n: EngineController.PitchNote) -> Double { movedTimes[i] ?? n.startSeconds }

    private func isPercussive(_ n: EngineController.PitchNote) -> Bool { n.detectedMidi < 1.0 }

    // Pitch range shown, from the PITCHED notes only (percussive markers have no pitch), min 14 tall.
    private func pitchRange() -> (lo: Double, hi: Double) {
        let pitched = notes.filter { !isPercussive($0) }
        guard !pitched.isEmpty else { return (57, 71) }
        var lo = Double.greatestFiniteMagnitude, hi = -Double.greatestFiniteMagnitude
        for n in pitched { lo = min(lo, n.editedMidi); hi = max(hi, n.editedMidi) }
        lo = floor(lo) - 3; hi = ceil(hi) + 3
        if hi - lo < 14 { let mid = (lo + hi) / 2; lo = mid - 7; hi = mid + 7 }
        return (lo, hi)
    }
    private func rowHeight() -> CGFloat {
        let (lo, hi) = pitchRange()
        return bounds.height / CGFloat(max(1.0, hi - lo))
    }
    private func yForMidi(_ midi: Double) -> CGFloat {
        let (lo, hi) = pitchRange()
        return CGFloat((midi - lo) / max(1.0, hi - lo)) * bounds.height
    }
    private func rectForNote(_ n: EngineController.PitchNote) -> NSRect {
        let x = CGFloat(n.startSeconds / clipDuration) * bounds.width
        let w = max(3, CGFloat(n.durationSeconds / clipDuration) * bounds.width)
        let h = max(6, rowHeight() * 0.8)
        let y = yForMidi(n.editedMidi) - h / 2
        return NSRect(x: x, y: y, width: w, height: h)
    }

    override func draw(_ dirtyRect: NSRect) {
        NSColor(hex: 0x0e0b09).setFill(); bounds.fill()
        // Clip waveform behind everything (faint), so notes line up with the audio.
        if !peaks.isEmpty {
            let mid = bounds.height / 2, halfH = bounds.height * 0.45
            NSColor(hex: 0x2a2622).withAlphaComponent(0.6).setStroke()
            let wf = NSBezierPath(); wf.lineWidth = 1
            for (i, p) in peaks.enumerated() {
                let x = CGFloat(i) / CGFloat(peaks.count) * bounds.width
                wf.move(to: NSPoint(x: x, y: mid + CGFloat(p.x) * halfH))
                wf.line(to: NSPoint(x: x, y: mid + CGFloat(p.y) * halfH))
            }
            wf.stroke()
        }
        let (lo, hi) = pitchRange()
        // Semitone rows + C labels.
        for midi in Int(lo.rounded(.up))...Int(hi.rounded(.down)) {
            let y = yForMidi(Double(midi))
            let isC = (midi % 12) == 0
            (isC ? NSColor(hex: 0x241f1a) : NSColor(hex: 0x161210)).setStroke()
            let p = NSBezierPath(); p.move(to: NSPoint(x: 0, y: y)); p.line(to: NSPoint(x: bounds.width, y: y)); p.lineWidth = isC ? 1 : 0.5; p.stroke()
            if isC {
                let name = "C\(midi / 12 - 1)" as NSString
                name.draw(at: NSPoint(x: 3, y: y + 1), withAttributes: [.font: NSFont.monospacedSystemFont(ofSize: 8, weight: .regular), .foregroundColor: NSColor(hex: 0x5a5048)])
            }
        }
        // Percussive markers: a full-height transient line at each onset (drag left/right to retime).
        for (i, n) in notes.enumerated() where isPercussive(n) {
            let x = CGFloat(transientTime(i, n) / clipDuration) * bounds.width
            let moved = movedTimes[i] != nil
            (moved ? NSColor(hex: 0x6bd3c0) : NSColor(hex: 0xe0a04a)).withAlphaComponent(0.85).setStroke()
            let p = NSBezierPath(); p.move(to: NSPoint(x: x, y: 0)); p.line(to: NSPoint(x: x, y: bounds.height)); p.lineWidth = 1.5; p.stroke()
        }
        // Pitched notes: detected position (faint) + edited (solid), a link line when moved.
        for n in notes where !isPercussive(n) {
            let r = rectForNote(n)
            if abs(n.offsetSemitones) > 0.01 {
                let detY = yForMidi(n.detectedMidi)
                NSColor(hex: 0x3a3128).setStroke()
                let dp = NSBezierPath(); dp.move(to: NSPoint(x: r.midX, y: r.midY)); dp.line(to: NSPoint(x: r.midX, y: detY)); dp.stroke()
                let dr = NSRect(x: r.minX, y: detY - r.height / 2, width: r.width, height: r.height)
                NSColor(hex: 0x2a2622).setFill(); NSBezierPath(roundedRect: dr, xRadius: 3, yRadius: 3).fill()
            }
            let conf = CGFloat(max(0.25, min(1.0, n.confidence)))
            let fill = (abs(n.offsetSemitones) > 0.01 ? NSColor(hex: 0x6bd3c0) : NSColor(hex: 0x4a90d9)).withAlphaComponent(0.35 + 0.55 * conf)
            fill.setFill(); NSBezierPath(roundedRect: r, xRadius: 3, yRadius: 3).fill()
            NSColor.white.withAlphaComponent(0.15).setStroke(); NSBezierPath(roundedRect: r, xRadius: 3, yRadius: 3).stroke()
            if abs(n.offsetSemitones) > 0.01 {
                let lbl = String(format: "%+.0f", n.offsetSemitones) as NSString
                lbl.draw(at: NSPoint(x: r.minX + 2, y: r.midY - 5), withAttributes: [.font: NSFont.monospacedSystemFont(ofSize: 8, weight: .bold), .foregroundColor: NSColor.white.withAlphaComponent(0.8)])
            }
        }
    }

    override func mouseDown(with event: NSEvent) {
        let pt = convert(event.locationInWindow, from: nil)
        // Percussive transient? grab the nearest line for a horizontal (timing) drag.
        for (i, n) in notes.enumerated() where isPercussive(n) {
            let x = CGFloat(transientTime(i, n) / clipDuration) * bounds.width
            if abs(pt.x - x) < 5 { transientDragIndex = i; dragIndex = nil; return }
        }
        for (i, n) in notes.enumerated() where !isPercussive(n) && rectForNote(n).insetBy(dx: -2, dy: -2).contains(pt) {
            dragIndex = i; dragStartY = pt.y; dragBaseOffset = n.offsetSemitones
            return
        }
        dragIndex = nil; transientDragIndex = nil
    }
    override func mouseDragged(with event: NSEvent) {
        let pt = convert(event.locationInWindow, from: nil)
        if let ti = transientDragIndex {
            let t = Double(max(0, min(bounds.width, pt.x)) / bounds.width) * clipDuration
            onMoveTransient?(ti, t)
            return
        }
        guard let i = dragIndex else { return }
        let deltaSemis = Double((pt.y - dragStartY) / max(1, rowHeight()))
        onOffset?(i, dragBaseOffset + deltaSemis)   // engine snaps to a whole semitone
    }
    override func mouseUp(with event: NSEvent) { dragIndex = nil; transientDragIndex = nil }
}

// MARK: - Serato anchor time-map canvas (AppKit, v1)

struct SeratoAnchorCanvas: NSViewRepresentable {
    var anchors: [SIMD2<Double>]        // (source, dest) normalized 0…1
    var onChange: ([SIMD2<Double>]) -> Void

    func makeNSView(context: Context) -> SeratoAnchorNSView {
        let v = SeratoAnchorNSView(); v.onChange = onChange; return v
    }
    func updateNSView(_ v: SeratoAnchorNSView, context: Context) {
        v.anchors = anchors; v.onChange = onChange; v.needsDisplay = true
    }
}

final class SeratoAnchorNSView: NSView {
    var anchors: [SIMD2<Double>] = [] { didSet { needsDisplay = true } }
    var onChange: (([SIMD2<Double>]) -> Void)?
    private var dragIndex: Int? = nil

    // A diagonal map line: x = source position, y = destination position (both 0…1 over the view).
    private func pointFor(_ a: SIMD2<Double>) -> NSPoint {
        NSPoint(x: CGFloat(a.x) * bounds.width, y: CGFloat(a.y) * bounds.height)
    }
    private func sorted() -> [SIMD2<Double>] { anchors.sorted { $0.x < $1.x } }

    override func draw(_ dirtyRect: NSRect) {
        NSColor(hex: 0x0e0b09).setFill(); bounds.fill()
        // Reference identity diagonal.
        NSColor(hex: 0x241f1a).setStroke()
        let id = NSBezierPath(); id.move(to: .zero); id.line(to: NSPoint(x: bounds.width, y: bounds.height)); id.lineWidth = 0.5; id.stroke()
        // The remap polyline through (0,0) → anchors → (1,1).
        let pts = [SIMD2<Double>(0, 0)] + sorted() + [SIMD2<Double>(1, 1)]
        NSColor(hex: 0xe0a04a).setStroke()
        let line = NSBezierPath(); line.lineWidth = 1.5
        for (i, a) in pts.enumerated() { let p = pointFor(a); if i == 0 { line.move(to: p) } else { line.line(to: p) } }
        line.stroke()
        // Anchor handles.
        for a in sorted() {
            let p = pointFor(a)
            let r = NSRect(x: p.x - 4, y: p.y - 4, width: 8, height: 8)
            NSColor(hex: 0xe0a04a).setFill(); NSBezierPath(ovalIn: r).fill()
        }
        let hint = "가로=원본 위치, 세로=목표 위치 · 앵커를 위/아래로 끌어 구간을 늘이거나 줄입니다" as NSString
        hint.draw(at: NSPoint(x: 6, y: 4), withAttributes: [.font: NSFont.systemFont(ofSize: 8), .foregroundColor: NSColor(hex: 0x5a5048)])
    }

    override func mouseDown(with event: NSEvent) {
        let pt = convert(event.locationInWindow, from: nil)
        // Grab an existing anchor to drag it.
        for a in sorted() where NSRect(x: pointFor(a).x - 6, y: pointFor(a).y - 6, width: 12, height: 12).contains(pt) {
            dragIndex = anchors.firstIndex(where: { $0.x == a.x && $0.y == a.y })
            return
        }
        // Double-click on empty space adds an anchor at the cursor (source=x, dest=y).
        if event.clickCount == 2 {
            let sx = min(0.98, max(0.02, Double(pt.x / bounds.width)))
            let sy = min(0.98, max(0.02, Double(pt.y / bounds.height)))
            var next = anchors; next.append(SIMD2<Double>(sx, sy)); onChange?(next)
        }
        dragIndex = nil
    }
    override func mouseDragged(with event: NSEvent) {
        guard let i = dragIndex, i < anchors.count else { return }
        let pt = convert(event.locationInWindow, from: nil)
        var next = anchors
        next[i] = SIMD2<Double>(min(0.98, max(0.02, Double(pt.x / bounds.width))),
                                min(0.98, max(0.02, Double(pt.y / bounds.height))))
        onChange?(next)
    }
    override func mouseUp(with event: NSEvent) { dragIndex = nil }

    override func rightMouseDown(with event: NSEvent) {
        // Right-click removes the anchor under the cursor.
        let pt = convert(event.locationInWindow, from: nil)
        if let i = anchors.firstIndex(where: { NSRect(x: pointFor($0).x - 8, y: pointFor($0).y - 8, width: 16, height: 16).contains(pt) }) {
            var next = anchors; next.remove(at: i); onChange?(next)
        }
    }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
}
