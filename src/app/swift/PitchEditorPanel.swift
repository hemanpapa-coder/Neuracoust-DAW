import SwiftUI
import AppKit

// The pitch editor, opened for a clip under the timeline (like the piano roll). Two selectable modes:
//   • 멜로다인 — note blobs from YIN detection; drag a note up/down to change its pitch (semitone snap).
//   • 세라토 앵커 — global time-ratio + pitch, plus draggable time-remap anchors.
// The engine holds the detected notes and the DSP; this view is the interaction surface. v1 skeleton.
struct PitchEditorPanel: View {
    @EnvironmentObject private var engine: EngineController


    /// Melodyne's tool palette: one button per group, and a click-and-hold fly-out on the groups
    /// that hold more than one tool — which is exactly how Melodyne hides its sub-tools. Each group
    /// remembers which of its tools was last used, so clicking the button again picks that one up
    /// rather than resetting to the first.
    @State private var lastToolInGroup: [EngineController.PitchEditTool.Group: EngineController.PitchEditTool] = [:]

    private var toolPalette: some View {
        HStack(spacing: 1) {
            ForEach(EngineController.PitchEditTool.Group.allCases, id: \.self) { group in
                let members = EngineController.PitchEditTool.allCases.filter { $0.group == group }
                let shown = members.contains(engine.pitchEditTool)
                    ? engine.pitchEditTool
                    : (lastToolInGroup[group] ?? members[0])
                let active = members.contains(engine.pitchEditTool)

                if members.count == 1 {
                    Button {
                        engine.pitchEditTool = shown
                        lastToolInGroup[group] = shown
                    } label: {
                        toolIcon(shown, active: active)
                    }
                    .buttonStyle(.borderless)
                    .help("\(shown.rawValue) — \(shown.hint)")
                } else {
                    Menu {
                        ForEach(members, id: \.self) { tool in
                            Button {
                                engine.pitchEditTool = tool
                                lastToolInGroup[group] = tool
                            } label: {
                                Label("\(tool.rawValue) — \(tool.hint)", systemImage: tool.symbol)
                            }
                        }
                    } label: {
                        toolIcon(shown, active: active)
                    } primaryAction: {
                        engine.pitchEditTool = shown
                        lastToolInGroup[group] = shown
                    }
                    .menuStyle(.borderlessButton)
                    .menuIndicator(.hidden)
                    .fixedSize()
                    .help("\(shown.rawValue) — \(shown.hint) (길게 눌러 다른 도구)")
                }
            }
        }
    }

    private func toolIcon(_ tool: EngineController.PitchEditTool, active: Bool) -> some View {
        Image(systemName: tool.symbol)
            .font(.system(size: 11))
            .frame(width: 24, height: 20)
            .foregroundStyle(active ? Theme.Palette.accent : Theme.Palette.textDim)
            .background(
                RoundedRectangle(cornerRadius: 4)
                    .fill(active ? Theme.Palette.accent.opacity(0.16) : Color.clear))
    }

    var body: some View {
        if engine.pitchEditorClipId != nil {
            VStack(spacing: 0) {
                header
                Divider().overlay(Theme.Palette.divider)
                if engine.pitchEditorMode == .melodyne {
                    PitchEditorCanvasSection(engine: engine, clock: engine.pitchEditorClock)
                } else {
                    SeratoAnchorCanvas(
                        anchors: engine.timeMapAnchors,
                        peaks: engine.pitchClipPeaks,
                        clipDuration: engine.pitchEditorClipDuration,
                        onChange: { engine.timeMapAnchors = $0 })
                }
            }
            .frame(height: 420, alignment: .top)
            .fixedSize(horizontal: false, vertical: true)
            .background(Theme.Palette.background)
        }
    }

    private var header: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: Theme.Space.md) {
                Text("피치 에디터").font(Theme.Font.ui(11, .semibold)).foregroundStyle(Theme.Palette.text)
                    .lineLimit(1)
                Picker("", selection: $engine.pitchEditorMode) {
                    ForEach(EngineController.PitchEditorMode.allCases, id: \.self) { Text($0.title).tag($0) }
                }
                .pickerStyle(.segmented).labelsHidden().frame(width: 180)

                if engine.pitchEditorMode == .melodyne {
                    toolPalette
                    Button {
                        engine.resetAllPitchNotes()
                    } label: {
                        Image(systemName: "arrow.uturn.backward")
                    }
                    .buttonStyle(.borderless)
                    .help("모든 음을 검출된 상태로 되돌립니다")
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
                        .lineLimit(1)
                } else {
                    anchorControls
                }
                Spacer(minLength: Theme.Space.lg)
                Toggle("메인 타임라인 동기화", isOn: $engine.pitchEditorTimelineSync)
                    .toggleStyle(.checkbox).font(Theme.Font.ui(9)).fixedSize()
                Toggle("포먼트 보존", isOn: $engine.formantPreserve)
                    .toggleStyle(.checkbox).font(Theme.Font.ui(9)).fixedSize()
                    .help("음정을 바꿔도 음색(포먼트)을 유지 — 치프멍크 방지. 끄면 원음 그대로 이동")
                Button { engine.previewPitchEdit() } label: { Image(systemName: "play.circle") }
                    .buttonStyle(.plain).help("편집 결과 미리듣기")
                Button("파일로 내보내기") { engine.exportPitchEditToFile() }.font(Theme.Font.ui(10)).fixedSize()
                Button("적용") {
                    if engine.pitchEditorMode == .anchor { engine.applyTimeMapEdit() }
                    else if engine.detectionMode == .percussive { engine.applyPercussiveTiming() }
                    else { engine.applyPitchEdits() }
                }
                .font(Theme.Font.ui(10, .semibold)).fixedSize()
                Button("닫기") { engine.closePitchEditor() }.font(Theme.Font.ui(10)).fixedSize()
            }
            .fixedSize(horizontal: true, vertical: false)
            .padding(.horizontal, Theme.Space.lg)
        }
        .frame(height: 44)
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

private struct PitchEditorCanvasSection: View {
    @ObservedObject var engine: EngineController
    @ObservedObject var clock: PlayheadClock
    @State private var horizontalZoom = 1.0
    @State private var verticalZoom = 1.0
    @State private var timeOffset = 0.0
    @State private var pitchOffset = 0.0

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 8) {
                Image(systemName: "arrow.left.and.right")
                Slider(value: $horizontalZoom, in: 1...16).frame(width: 110).controlSize(.mini)
                Text(String(format: "%.1f×", horizontalZoom)).font(Theme.Font.mono(8))
                Image(systemName: "arrow.up.and.down")
                Slider(value: $verticalZoom, in: 1...8).frame(width: 90).controlSize(.mini)
                Text(String(format: "%.1f×", verticalZoom)).font(Theme.Font.mono(8))
                // Waveform amplitude, independent of the view zoom above: scales the drawn signal
                // so a quiet passage's shape is readable without resizing the editor.
                Image(systemName: "waveform")
                Slider(value: $engine.pitchWaveformGain, in: 0.5...4).frame(width: 90).controlSize(.mini)
                Text(String(format: "%.1f×", engine.pitchWaveformGain)).font(Theme.Font.mono(8))
                Spacer()
                Text("스크롤: 이동 · ⌘스크롤: 줌")
                    .font(Theme.Font.mono(8)).foregroundStyle(Theme.Palette.textFaint)
            }
            .foregroundStyle(Theme.Palette.textSecondary)
            .padding(.horizontal, 10).frame(height: 28)
            MelodyneNoteCanvas(
                notes: engine.pitchNotes,
                tool: engine.pitchEditTool,
                waveformGain: engine.pitchWaveformGain,
                clipDuration: engine.pitchEditorClipDuration,
                peaks: engine.pitchClipPeaks,
                movedTimes: engine.percussiveTimeEdits,
                playheadSeconds: clock.seconds,
                horizontalZoom: horizontalZoom,
                verticalZoom: verticalZoom,
                timeOffset: timeOffset,
                pitchOffset: pitchOffset,
                onOffset: { engine.setPitchNoteOffset($0, semitones: $1) },
                onTimeOffset: { engine.setPitchNoteTimeOffset($0, seconds: $1) },
                onDurationScale: { engine.setPitchNoteDurationScale($0, scale: $1) },
                onSplit: { engine.splitPitchNote($0, at: $1) },
                onGainDb: { engine.setPitchNoteGainDb($0, gainDb: $1) },
                onMute: { engine.setPitchNoteMuted($0, muted: $1) },
                onFormant: { engine.setPitchNoteFormant($0, semitones: $1) },
                onAttackSpeed: { engine.setPitchNoteAttackSpeed($0, speed: $1) },
                onModulation: { engine.setPitchNoteModulation($0, scale: $1) },
                onDrift: { engine.setPitchNoteDrift($0, scale: $1) },
                onResetNote: { engine.resetPitchNote($0) },
                onMoveTransient: { engine.setTransientTime($0, $1) },
                onAudition: { engine.auditionPitchEvent(localStart: $0, duration: $1) },
                onSeek: { engine.setPitchEditorPosition($0) },
                onViewportChange: { timeOffset = $0; pitchOffset = $1 },
                onZoomChange: { horizontalZoom = $0; verticalZoom = $1 })
        }
    }
}

// MARK: - Melodyne note canvas (AppKit)

struct MelodyneNoteCanvas: NSViewRepresentable {
    var notes: [EngineController.PitchNote]
    var tool: EngineController.PitchEditTool
    var waveformGain: Double = 1
    var clipDuration: Double
    var peaks: [SIMD2<Float>]
    var movedTimes: [Int: Double]
    var playheadSeconds: Double
    var horizontalZoom: Double
    var verticalZoom: Double
    var timeOffset: Double
    var pitchOffset: Double
    var onOffset: (Int, Double) -> Void
    var onTimeOffset: (Int, Double) -> Void
    var onDurationScale: (Int, Double) -> Void
    var onSplit: (Int, Double) -> Void
    var onGainDb: (Int, Double) -> Void
    var onMute: (Int, Bool) -> Void
    var onFormant: (Int, Double) -> Void
    var onAttackSpeed: (Int, Double) -> Void
    var onModulation: (Int, Double) -> Void
    var onDrift: (Int, Double) -> Void
    var onResetNote: (Int) -> Void
    var onMoveTransient: (Int, Double) -> Void
    var onAudition: (Double, Double) -> Void
    var onSeek: (Double) -> Void
    var onViewportChange: (Double, Double) -> Void
    var onZoomChange: (Double, Double) -> Void

    func makeNSView(context: Context) -> MelodyneNoteNSView {
        let v = MelodyneNoteNSView()
        v.onOffset = onOffset; v.onTimeOffset = onTimeOffset; v.onMoveTransient = onMoveTransient
        v.onDurationScale = onDurationScale; v.onSplit = onSplit
        v.onGainDb = onGainDb; v.onMute = onMute; v.onFormant = onFormant
        v.onAttackSpeed = onAttackSpeed; v.onResetNote = onResetNote
        v.onModulation = onModulation; v.onDrift = onDrift
        v.onAudition = onAudition; v.onSeek = onSeek
        v.onViewportChange = onViewportChange; v.onZoomChange = onZoomChange
        return v
    }
    func updateNSView(_ v: MelodyneNoteNSView, context: Context) {
        v.notes = notes
        v.tool = tool
        v.clipDuration = max(0.1, clipDuration)
        v.peaks = peaks
        v.waveformGain = CGFloat(waveformGain)
        v.movedTimes = movedTimes
        v.playheadSeconds = playheadSeconds
        v.horizontalZoom = horizontalZoom; v.verticalZoom = verticalZoom
        v.timeOffset = timeOffset; v.pitchOffset = pitchOffset
        v.onOffset = onOffset; v.onTimeOffset = onTimeOffset; v.onMoveTransient = onMoveTransient
        v.onDurationScale = onDurationScale; v.onSplit = onSplit
        v.onGainDb = onGainDb; v.onMute = onMute; v.onFormant = onFormant
        v.onAttackSpeed = onAttackSpeed; v.onResetNote = onResetNote
        v.onModulation = onModulation; v.onDrift = onDrift
        v.onAudition = onAudition; v.onSeek = onSeek
        v.onViewportChange = onViewportChange; v.onZoomChange = onZoomChange
        v.needsDisplay = true
    }
}

final class MelodyneNoteNSView: NSView {
    var notes: [EngineController.PitchNote] = [] { didSet { needsDisplay = true } }
    var tool: EngineController.PitchEditTool = .main { didSet { needsDisplay = true } }
    var onGainDb: ((Int, Double) -> Void)?
    var onMute: ((Int, Bool) -> Void)?
    var onFormant: ((Int, Double) -> Void)?
    var onAttackSpeed: ((Int, Double) -> Void)?
    var onModulation: ((Int, Double) -> Void)?
    var onDrift: ((Int, Double) -> Void)?
    var onResetNote: ((Int) -> Void)?
    var clipDuration: Double = 1.0
    var peaks: [SIMD2<Float>] = [] {
        didSet { peakReferenceCache = nil; needsDisplay = true }
    }
    /// Scales the drawn waveform's height only — the view zoom scales the screen, this scales the
    /// signal, which is what you want when shaping quiet passages.
    var waveformGain: CGFloat = 1 { didSet { needsDisplay = true } }

    private var peakReferenceCache: CGFloat?
    /// The clip's loudest peak, so the waveform is drawn relative to its own material rather than to
    /// full scale. Cached: recomputed only when the peaks change, never per drawn note.
    private var normalisedPeakReference: CGFloat {
        if let cached = peakReferenceCache { return cached }
        var loudest: CGFloat = 0
        for peak in peaks {
            loudest = max(loudest, CGFloat(max(abs(peak.x), abs(peak.y))))
        }
        // A silent or near-silent clip must not divide the shape up to full height.
        let reference = max(0.08, loudest)
        peakReferenceCache = reference
        return reference
    }

    var movedTimes: [Int: Double] = [:] { didSet { needsDisplay = true } }
    var playheadSeconds: Double = 0 { didSet { needsDisplay = true } }
    var horizontalZoom: Double = 1
    var verticalZoom: Double = 1
    var timeOffset: Double = 0
    var pitchOffset: Double = 0
    var onOffset: ((Int, Double) -> Void)?
    var onTimeOffset: ((Int, Double) -> Void)?
    var onDurationScale: ((Int, Double) -> Void)?
    var onSplit: ((Int, Double) -> Void)?
    var onMoveTransient: ((Int, Double) -> Void)?
    var onAudition: ((Double, Double) -> Void)?
    var onSeek: ((Double) -> Void)?
    var onViewportChange: ((Double, Double) -> Void)?
    var onZoomChange: ((Double, Double) -> Void)?

    private var dragIndex: Int? = nil
    private var dragStartY: CGFloat = 0
    private var dragStartX: CGFloat = 0
    private var dragBaseOffset: Double = 0
    private var dragBaseTimeOffset: Double = 0
    private var dragBaseDurationScale: Double = 1
    private var dragBaseGainDb: Double = 0
    private var dragBaseFormant: Double = 0
    private var dragBaseAttackSpeed: Double = 1
    private var dragBaseModulation: Double = 1
    private var dragBaseDrift: Double = 1
    private enum DragOperation { case contextual, pitch, time, stretchStart, stretchEnd, formant, amplitude, attack, modulation, drift }
    private var dragOperation: DragOperation = .contextual
    private var transientDragIndex: Int? = nil
    private let pitchGutterWidth: CGFloat = 38
    private let rulerHeight: CGFloat = 22
    private let overviewHeight: CGFloat = 18

    private var percussiveMode: Bool {
        !notes.isEmpty && notes.allSatisfy(isPercussive)
    }
    private var effectiveGutterWidth: CGFloat { percussiveMode ? 0 : pitchGutterWidth }

    private var editorRect: NSRect {
        NSRect(x: effectiveGutterWidth,
               y: overviewHeight,
               width: max(1, bounds.width - effectiveGutterWidth),
               height: max(1, bounds.height - rulerHeight - overviewHeight))
    }
    private var rulerRect: NSRect {
        NSRect(x: effectiveGutterWidth, y: bounds.height - rulerHeight,
               width: max(1, bounds.width - effectiveGutterWidth), height: rulerHeight)
    }
    private var overviewRect: NSRect {
        NSRect(x: effectiveGutterWidth, y: 0,
               width: max(1, bounds.width - effectiveGutterWidth), height: overviewHeight)
    }

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
        let fullSpan = hi - lo
        let span = fullSpan / max(1, verticalZoom)
        let center = (lo + hi) / 2 + pitchOffset
        return (center - span / 2, center + span / 2)
    }
    private var visibleDuration: Double { clipDuration / max(1, horizontalZoom) }
    private var visibleStart: Double { min(max(0, clipDuration - visibleDuration), max(0, timeOffset)) }
    private func xForTime(_ seconds: Double) -> CGFloat {
        editorRect.minX + CGFloat((seconds - visibleStart) / max(0.001, visibleDuration)) * editorRect.width
    }
    private func timeForX(_ x: CGFloat) -> Double {
        min(clipDuration, max(0, visibleStart + Double((x - editorRect.minX) / max(1, editorRect.width)) * visibleDuration))
    }
    private func rowHeight() -> CGFloat {
        let (lo, hi) = pitchRange()
        return editorRect.height / CGFloat(max(1.0, hi - lo))
    }
    private func yForMidi(_ midi: Double) -> CGFloat {
        let (lo, hi) = pitchRange()
        return editorRect.minY + CGFloat((midi - lo) / max(1.0, hi - lo)) * editorRect.height
    }
    private func rectForNote(_ n: EngineController.PitchNote) -> NSRect {
        let x = xForTime(n.editedStartSeconds)
        let w = max(3, CGFloat(n.editedDurationSeconds / visibleDuration) * editorRect.width)
        let h = max(6, rowHeight() * 0.8)
        let y = yForMidi(n.editedMidi) - h / 2
        return NSRect(x: x, y: y, width: w, height: h)
    }

    /// A note blob shaped by the source waveform under that note — no decorative/random ripple.
    private func blobPath(_ rect: NSRect, note: EngineController.PitchNote) -> NSBezierPath {
        let p = NSBezierPath()
        let steps = max(4, min(48, Int(rect.width / 4)))
        func amplitude(_ fraction: CGFloat) -> CGFloat {
            guard !peaks.isEmpty else { return 0.55 }
            let seconds = note.editedStartSeconds + Double(fraction) * note.editedDurationSeconds
            let index = min(peaks.count - 1, max(0, Int(seconds / max(0.001, clipDuration) * Double(peaks.count))))
            let peak = peaks[index]
            // Normalise against the clip's loudest peak, then apply the user's waveform gain. Drawing
            // the raw 0..1 sample value made a vocal that peaks at -15 dBFS occupy a sixth of the row
            // — the shape was there but far too small to read the phrasing off.
            let raw = CGFloat(max(abs(peak.x), abs(peak.y)))
            return max(0.16, min(1, raw / normalisedPeakReference * waveformGain))
        }
        p.move(to: NSPoint(x: rect.minX, y: rect.midY))
        for i in 0...steps {
            let f = CGFloat(i) / CGFloat(steps)
            p.line(to: NSPoint(x: rect.minX + rect.width * f,
                               y: rect.midY + amplitude(f) * rect.height * 0.48))
        }
        for i in stride(from: steps, through: 0, by: -1) {
            let f = CGFloat(i) / CGFloat(steps)
            p.line(to: NSPoint(x: rect.minX + rect.width * f,
                               y: rect.midY - amplitude(f) * rect.height * 0.48))
        }
        p.close()
        return p
    }

    override func draw(_ dirtyRect: NSRect) {
        NSGraphicsContext.saveGraphicsState()
        NSBezierPath(rect: bounds).addClip()
        defer { NSGraphicsContext.restoreGraphicsState() }
        NSColor(hex: 0xd5d5d5).setFill(); bounds.fill()
        NSColor(hex: 0xf5f5f3).setFill(); editorRect.fill()
        NSColor(hex: 0xe7e7e5).setFill(); rulerRect.fill()
        NSColor(hex: 0xeeeeec).setFill(); overviewRect.fill()
        let (lo, hi) = pitchRange()
        if percussiveMode {
            // Percussive material has no meaningful pitch axis. Give the waveform the
            // complete editor height and use alternating timing slices as the structure.
            let orderedTimes = notes.enumerated().map { transientTime($0.offset, $0.element) }.sorted()
            let boundaries = [visibleStart] + orderedTimes.filter {
                $0 > visibleStart && $0 < visibleStart + visibleDuration
            } + [visibleStart + visibleDuration]
            for i in 0..<max(0, boundaries.count - 1) where i % 2 == 1 {
                NSColor(hex: 0xe9edf0).setFill()
                NSRect(x: xForTime(boundaries[i]), y: editorRect.minY,
                       width: xForTime(boundaries[i + 1]) - xForTime(boundaries[i]),
                       height: editorRect.height).fill()
            }
            if !peaks.isEmpty {
                let mid = editorRect.midY
                // Same normalisation as the note blobs: relative to the clip's own loudest peak,
                // times the user's waveform gain, so quiet material still reads.
                let half = editorRect.height * 0.43 *
                    min(3.0, waveformGain / normalisedPeakReference)
                let waveform = NSBezierPath()
                var began = false
                for (i, peak) in peaks.enumerated() {
                    let seconds = Double(i) / Double(max(1, peaks.count - 1)) * clipDuration
                    let x = xForTime(seconds)
                    guard x >= editorRect.minX, x <= editorRect.maxX else { continue }
                    let y = mid + CGFloat(peak.y) * half
                    if !began { waveform.move(to: NSPoint(x: x, y: y)); began = true }
                    else { waveform.line(to: NSPoint(x: x, y: y)) }
                }
                for i in peaks.indices.reversed() {
                    let seconds = Double(i) / Double(max(1, peaks.count - 1)) * clipDuration
                    let x = xForTime(seconds)
                    guard x >= editorRect.minX, x <= editorRect.maxX else { continue }
                    waveform.line(to: NSPoint(x: x, y: mid + CGFloat(peaks[i].x) * half))
                }
                waveform.close()
                NSColor(hex: 0x5d87a3).withAlphaComponent(0.72).setFill()
                waveform.fill()
                NSColor(hex: 0x35647f).withAlphaComponent(0.9).setStroke()
                waveform.lineWidth = 0.8
                waveform.stroke()
            }
        } else {
            // Piano-style semitone rows and a permanent note-name gutter.
            for midi in Int(lo.rounded(.up))...Int(hi.rounded(.down)) {
                let y = yForMidi(Double(midi))
                let pc = ((midi % 12) + 12) % 12
                let black = [1, 3, 6, 8, 10].contains(pc)
                let band = NSRect(x: editorRect.minX, y: y - rowHeight() / 2,
                                  width: editorRect.width, height: rowHeight())
                (black ? NSColor(hex: 0xe8e8e6) : NSColor(hex: 0xf8f8f6)).setFill()
                band.intersection(editorRect).fill()
                NSColor(hex: 0xc9c9c6).setStroke()
                let p = NSBezierPath(); p.move(to: NSPoint(x: editorRect.minX, y: y - rowHeight() / 2))
                p.line(to: NSPoint(x: editorRect.maxX, y: y - rowHeight() / 2)); p.lineWidth = 0.5; p.stroke()
                let names = ["C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"]
                let label = names[pc] + (pc == 0 ? "\(midi / 12 - 1)" : "")
                (label as NSString).draw(at: NSPoint(x: 5, y: y - 5),
                                        withAttributes: [.font: NSFont.systemFont(ofSize: 8),
                                                         .foregroundColor: NSColor(hex: 0x606060)])
            }
        }
        if !percussiveMode {
            NSColor(hex: 0x9d9d9a).setStroke()
            let gutterEdge = NSBezierPath(); gutterEdge.move(to: NSPoint(x: editorRect.minX, y: 0))
            gutterEdge.line(to: NSPoint(x: editorRect.minX, y: bounds.height)); gutterEdge.lineWidth = 1; gutterEdge.stroke()
        }

        // Time ruler and vertical beat/second divisions.
        let targetLines = max(4, Int(editorRect.width / 110))
        let rawStep = visibleDuration / Double(targetLines)
        let steps = [0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 30, 60]
        let step = steps.first(where: { $0 >= rawStep }) ?? 60
        var t = ceil(visibleStart / step) * step
        while t <= visibleStart + visibleDuration {
            let x = xForTime(t)
            NSColor(hex: 0xb8b8b5).setStroke()
            let grid = NSBezierPath(); grid.move(to: NSPoint(x: x, y: editorRect.minY))
            grid.line(to: NSPoint(x: x, y: rulerRect.maxY)); grid.lineWidth = 0.6; grid.stroke()
            let label = step < 1 ? String(format: "%.1f", t) : String(format: "%.0f", t)
            (label as NSString).draw(at: NSPoint(x: x + 3, y: rulerRect.minY + 5),
                                     withAttributes: [.font: NSFont.monospacedDigitSystemFont(ofSize: 8, weight: .regular),
                                                      .foregroundColor: NSColor(hex: 0x565656)])
            t += step
        }
        // Percussive markers: a full-height transient line at each onset (drag left/right to retime).
        for (i, n) in notes.enumerated() where isPercussive(n) {
            let x = xForTime(transientTime(i, n))
            let moved = movedTimes[i] != nil
            (moved ? NSColor(hex: 0x6bd3c0) : NSColor(hex: 0xe0a04a)).withAlphaComponent(0.85).setStroke()
            let p = NSBezierPath(); p.move(to: NSPoint(x: x, y: editorRect.minY)); p.line(to: NSPoint(x: x, y: editorRect.maxY)); p.lineWidth = 1.5; p.stroke()
            NSColor(hex: moved ? 0x2d9c89 : 0xc47c24).setFill()
            NSBezierPath(roundedRect: NSRect(x: x - 3.5, y: editorRect.maxY - 11,
                                             width: 7, height: 9),
                         xRadius: 2, yRadius: 2).fill()
        }
        // Pitched notes: detected position (faint) + edited (solid), a link line when moved.
        for (_, n) in notes.enumerated() where !isPercussive(n) {
            let r = rectForNote(n)
            if abs(n.offsetSemitones) > 0.01 {
                let detY = yForMidi(n.detectedMidi)
                NSColor(hex: 0x3a3128).setStroke()
                let dp = NSBezierPath(); dp.move(to: NSPoint(x: r.midX, y: r.midY)); dp.line(to: NSPoint(x: r.midX, y: detY)); dp.stroke()
                let dr = NSRect(x: r.minX, y: detY - r.height / 2, width: r.width, height: r.height)
                NSColor(hex: 0x2a2622).setFill(); NSBezierPath(roundedRect: dr, xRadius: 3, yRadius: 3).fill()
            }
            let conf = CGFloat(max(0.25, min(1.0, n.confidence)))
            let blob = blobPath(r, note: n)
            // The blob's fill carries the note's level, the way Melodyne's blobs do: a −24 dB note
            // is nearly transparent, a boosted one solid. Muted notes are drawn hollow instead.
            let levelAlpha = CGFloat(min(1.0, max(0.12, pow(10.0, n.gainDb / 20.0))))
            (n.edited ? NSColor(hex: 0xf09b35) : NSColor(hex: 0xffbd45))
                .withAlphaComponent((0.62 + 0.32 * conf) * (n.muted ? 0.10 : levelAlpha)).setFill()
            blob.fill()
            NSColor(hex: 0xd8493f).withAlphaComponent(0.95).setStroke()
            blob.lineWidth = 1
            blob.stroke()
            // The cache currently exposes one analyzed pitch per note. Draw that measured
            // pitch as a straight center trace; never invent vibrato that was not detected.
            let trace = NSBezierPath()
            trace.move(to: NSPoint(x: r.minX + 2, y: r.midY))
            trace.line(to: NSPoint(x: r.maxX - 2, y: r.midY))
            NSColor(hex: 0xc92f35).withAlphaComponent(0.9).setStroke()
            trace.lineWidth = 1
            trace.stroke()
            if tool == .main || tool == .time {
                NSColor(hex: 0xb62f34).withAlphaComponent(0.9).setStroke()
                for x in [r.minX + 1, r.maxX - 1] {
                    let handle = NSBezierPath()
                    handle.move(to: NSPoint(x: x, y: r.minY + 1))
                    handle.line(to: NSPoint(x: x, y: r.maxY - 1))
                    handle.lineWidth = 1.4
                    handle.stroke()
                }
            }
            if n.muted {
                // A struck-through blob: unmistakably off, and it still shows where the note is.
                let slash = NSBezierPath()
                slash.move(to: NSPoint(x: r.minX + 1, y: r.minY + 1))
                slash.line(to: NSPoint(x: r.maxX - 1, y: r.maxY - 1))
                slash.lineWidth = 1.2
                NSColor(hex: 0xd8493f).withAlphaComponent(0.9).setStroke()
                slash.stroke()
            }
            if abs(n.formantSemitones) > 0.01 {
                // Formant marks sit on the blob's top edge — a separate axis from pitch, so they
                // must not be mistaken for the pitch label below.
                let ticks = min(6, Int(abs(n.formantSemitones).rounded()))
                NSColor(hex: 0x7fd1e0).withAlphaComponent(0.95).setStroke()
                for k in 0..<max(1, ticks) {
                    let x = r.minX + 3 + CGFloat(k) * 3
                    guard x < r.maxX - 2 else { break }
                    let tick = NSBezierPath()
                    let y = n.formantSemitones > 0 ? r.maxY - 1 : r.minY + 1
                    tick.move(to: NSPoint(x: x, y: y))
                    tick.line(to: NSPoint(x: x, y: n.formantSemitones > 0 ? y - 2.5 : y + 2.5))
                    tick.lineWidth = 1.2
                    tick.stroke()
                }
            }
            if abs(n.attackSpeed - 1) > 0.01 {
                // A wedge at the note's head, steeper for a faster attack.
                let width = max(3.0, min(r.width * 0.35, 14.0 / max(0.3, n.attackSpeed)))
                let wedge = NSBezierPath()
                wedge.move(to: NSPoint(x: r.minX + 1, y: r.minY + 1))
                wedge.line(to: NSPoint(x: r.minX + 1 + width, y: r.maxY - 1))
                wedge.lineWidth = 1.2
                NSColor(hex: 0xffe08a).withAlphaComponent(0.9).setStroke()
                wedge.stroke()
            }
            var labels: [String] = []
            if abs(n.offsetSemitones) > 0.01 { labels.append(String(format: "%+.0f", n.offsetSemitones)) }
            if abs(n.gainDb) > 0.01 { labels.append(String(format: "%+.0fdB", n.gainDb)) }
            if abs(n.formantSemitones) > 0.01 { labels.append(String(format: "F%+.1f", n.formantSemitones)) }
            if abs(n.attackSpeed - 1) > 0.01 { labels.append(String(format: "A%.2f", n.attackSpeed)) }
            if abs(n.modulationScale - 1) > 0.01 { labels.append(String(format: "V%.2f", n.modulationScale)) }
            if abs(n.driftScale - 1) > 0.01 { labels.append(String(format: "D%.2f", n.driftScale)) }
            if !labels.isEmpty {
                let lbl = labels.joined(separator: " ") as NSString
                lbl.draw(at: NSPoint(x: r.minX + 2, y: r.midY - 5), withAttributes: [.font: NSFont.monospacedSystemFont(ofSize: 8, weight: .bold), .foregroundColor: NSColor.white.withAlphaComponent(0.8)])
            }
        }
        // Full-clip overview waveform and visible-window handle.
        if !peaks.isEmpty {
            let mid = overviewRect.midY, half = overviewRect.height * 0.42
            let wf = NSBezierPath()
            for (i, peak) in peaks.enumerated() {
                let x = overviewRect.minX + CGFloat(i) / CGFloat(max(1, peaks.count - 1)) * overviewRect.width
                let y = mid + CGFloat(max(abs(peak.x), abs(peak.y))) * half
                if i == 0 { wf.move(to: NSPoint(x: x, y: y)) } else { wf.line(to: NSPoint(x: x, y: y)) }
            }
            for i in peaks.indices.reversed() {
                let x = overviewRect.minX + CGFloat(i) / CGFloat(max(1, peaks.count - 1)) * overviewRect.width
                let amp = CGFloat(max(abs(peaks[i].x), abs(peaks[i].y))) * half
                wf.line(to: NSPoint(x: x, y: mid - amp))
            }
            wf.close(); NSColor(hex: 0x777777).withAlphaComponent(0.72).setFill(); wf.fill()
            let windowX = overviewRect.minX + CGFloat(visibleStart / clipDuration) * overviewRect.width
            let windowW = CGFloat(visibleDuration / clipDuration) * overviewRect.width
            NSColor(hex: 0x3377a8).withAlphaComponent(0.16).setFill()
            NSRect(x: windowX, y: overviewRect.minY, width: windowW, height: overviewRect.height).fill()
            NSColor(hex: 0x3377a8).setStroke()
            NSBezierPath(rect: NSRect(x: windowX, y: overviewRect.minY, width: windowW, height: overviewRect.height)).stroke()
        }
        let playX = xForTime(playheadSeconds)
        if playX >= editorRect.minX && playX <= editorRect.maxX {
            NSColor(hex: 0xe85757).setStroke()
            let p = NSBezierPath(); p.move(to: NSPoint(x: playX, y: overviewRect.maxY))
            p.line(to: NSPoint(x: playX, y: rulerRect.maxY)); p.lineWidth = 1.5; p.stroke()
        }
    }

    override func mouseDown(with event: NSEvent) {
        let pt = convert(event.locationInWindow, from: nil)
        if overviewRect.contains(pt) {
            let t = Double((pt.x - overviewRect.minX) / max(1, overviewRect.width)) * clipDuration
            onSeek?(t)
            let next = min(max(0, clipDuration - visibleDuration), max(0, t - visibleDuration / 2))
            onViewportChange?(next, pitchOffset)
            return
        }
        guard pt.x >= editorRect.minX else { return }
        // Percussive transient? grab the nearest line for a horizontal (timing) drag.
        for (i, n) in notes.enumerated() where isPercussive(n) {
            let x = xForTime(transientTime(i, n))
            if abs(pt.x - x) < 5 {
                transientDragIndex = i; dragIndex = nil
                onAudition?(transientTime(i, n), max(0.12, n.durationSeconds)); return
            }
        }
        // Melodyne blobs can be only a few pixels high at a wide pitch range. Keep the
        // audible target comfortably larger than the painted contour.
        for (i, n) in notes.enumerated() where !isPercussive(n) && rectForNote(n).insetBy(dx: -5, dy: -7).contains(pt) {
            let r = rectForNote(n)
            if tool == .separate {
                if event.clickCount == 2 { onSplit?(i, timeForX(pt.x)) }
                return
            }
            // Mute is a click, not a drag — Melodyne's amplitude/mute tool toggles the blob.
            if tool == .mute {
                onMute?(i, !n.muted)
                return
            }
            dragIndex = i; dragStartY = pt.y; dragStartX = pt.x
            dragBaseOffset = n.offsetSemitones
            dragBaseTimeOffset = n.timeOffsetSeconds
            dragBaseDurationScale = n.durationScale
            dragBaseGainDb = n.gainDb
            dragBaseFormant = n.formantSemitones
            dragBaseAttackSpeed = n.attackSpeed
            dragBaseModulation = n.modulationScale
            dragBaseDrift = n.driftScale
            let edgeWidth = min(8, max(4, r.width * 0.18))
            // A tool that names one operation always performs it — only 메인 reads the grab zone,
            // which is the difference between Melodyne's multi-tool and its dedicated tools.
            switch tool {
            case .pitch: dragOperation = .pitch
            case .modulation: dragOperation = .modulation
            case .drift: dragOperation = .drift
            case .formant: dragOperation = .formant
            case .amplitude: dragOperation = .amplitude
            case .attack: dragOperation = .attack
            case .mute, .separate: dragOperation = .contextual
            case .time, .main:
                if pt.x <= r.minX + edgeWidth { dragOperation = .stretchStart }
                else if pt.x >= r.maxX - edgeWidth { dragOperation = .stretchEnd }
                else if tool == .time { dragOperation = .time }
                else { dragOperation = .contextual }
            }
            onAudition?(n.editedStartSeconds, n.editedDurationSeconds)
            return
        }
        onSeek?(timeForX(pt.x))
        dragIndex = nil; transientDragIndex = nil
    }

    override func resetCursorRects() {
        super.resetCursorRects()
        for n in notes where !isPercussive(n) {
            let r = rectForNote(n).insetBy(dx: -3, dy: -5)
            switch tool {
            case .pitch, .formant, .amplitude, .modulation, .drift: addCursorRect(r, cursor: .resizeUpDown)
            case .time, .attack: addCursorRect(r, cursor: .resizeLeftRight)
            case .separate: addCursorRect(r, cursor: .crosshair)
            case .mute: addCursorRect(r, cursor: .pointingHand)
            case .main: addCursorRect(r, cursor: .openHand)
            }
        }
    }
    override func mouseDragged(with event: NSEvent) {
        let pt = convert(event.locationInWindow, from: nil)
        if let ti = transientDragIndex {
            let t = timeForX(pt.x)
            onMoveTransient?(ti, t)
            return
        }
        guard let i = dragIndex else { return }
        let dx = pt.x - dragStartX
        let dy = pt.y - dragStartY
        // The dedicated tools first: each reads one axis and nothing else.
        if dragOperation == .formant {
            // Half a semitone of formant per row, so the whole ±12 range is a comfortable drag
            // rather than the hair-trigger a 1:1 mapping against the pitch grid would give.
            onFormant?(i, dragBaseFormant + Double(dy / max(1, rowHeight())) * 0.5)
            return
        }
        if dragOperation == .modulation {
            // Up = deeper. A drag of 100 pt doubles it, and the floor is a genuinely flat note.
            onModulation?(i, max(0, dragBaseModulation * pow(2.0, Double(dy) / 100.0)))
            return
        }
        if dragOperation == .drift {
            onDrift?(i, max(0, dragBaseDrift * pow(2.0, Double(dy) / 100.0)))
            return
        }
        if dragOperation == .amplitude {
            onGainDb?(i, dragBaseGainDb + Double(dy) * 0.12)   // ±24 dB over ±200 pt
            return
        }
        if dragOperation == .attack {
            // Right = sharper. Exponential so 1.0 sits at the centre and the range is symmetric.
            onAttackSpeed?(i, dragBaseAttackSpeed * pow(2.0, Double(dx) / 120.0))
            return
        }
        if dragOperation == .stretchStart || dragOperation == .stretchEnd {
            let deltaSeconds = Double(dx / max(1, editorRect.width)) * visibleDuration
            let signed = dragOperation == .stretchStart ? -deltaSeconds : deltaSeconds
            let baseDuration = max(0.02, notes[i].durationSeconds * dragBaseDurationScale)
            onDurationScale?(i, (baseDuration + signed) / max(0.02, notes[i].durationSeconds))
            if dragOperation == .stretchStart {
                onTimeOffset?(i, dragBaseTimeOffset + deltaSeconds)
            }
        } else if dragOperation == .time || (dragOperation == .contextual && abs(dx) > abs(dy)) {
            let deltaSeconds = Double(dx / max(1, editorRect.width)) * visibleDuration
            onTimeOffset?(i, dragBaseTimeOffset + deltaSeconds)
        } else {
            let deltaSemis = Double(dy / max(1, rowHeight()))
            onOffset?(i, dragBaseOffset + deltaSemis)   // engine snaps to a whole semitone
        }
    }
    override func mouseUp(with event: NSEvent) {
        // Re-audition after a pitch drag so the release immediately demonstrates the
        // edited note. The mouse-down audition necessarily used the pre-drag render.
        if let i = dragIndex, i >= 0, i < notes.count {
            let n = notes[i]
            onAudition?(n.editedStartSeconds, n.editedDurationSeconds)
        }
        dragIndex = nil
        transientDragIndex = nil
    }

    override func scrollWheel(with event: NSEvent) {
        if event.modifierFlags.contains(.command) {
            let hz = min(16, max(1, horizontalZoom * (1 - Double(event.scrollingDeltaY) * 0.025)))
            let vz = min(8, max(1, verticalZoom * (1 - Double(event.scrollingDeltaX) * 0.025)))
            onZoomChange?(hz, vz)
        } else {
            let nextTime = min(max(0, clipDuration - visibleDuration),
                               max(0, visibleStart + Double(event.scrollingDeltaX) / max(1, Double(editorRect.width)) * visibleDuration))
            let nextPitch = pitchOffset + Double(event.scrollingDeltaY) / max(1, Double(editorRect.height)) * (pitchRange().hi - pitchRange().lo)
            onViewportChange?(nextTime, nextPitch)
        }
    }
}

// MARK: - Serato anchor time-map canvas (AppKit, v1)

struct SeratoAnchorCanvas: NSViewRepresentable {
    var anchors: [SIMD2<Double>]        // (source, dest) normalized 0…1
    var peaks: [SIMD2<Float>]
    var clipDuration: Double
    var onChange: ([SIMD2<Double>]) -> Void

    func makeNSView(context: Context) -> SeratoAnchorNSView {
        let v = SeratoAnchorNSView(); v.onChange = onChange; return v
    }
    func updateNSView(_ v: SeratoAnchorNSView, context: Context) {
        v.anchors = anchors; v.peaks = peaks; v.clipDuration = clipDuration
        v.onChange = onChange; v.needsDisplay = true
    }
}

final class SeratoAnchorNSView: NSView {
    var anchors: [SIMD2<Double>] = [] { didSet { needsDisplay = true } }
    var peaks: [SIMD2<Float>] = [] { didSet { needsDisplay = true } }
    var clipDuration: Double = 1
    var onChange: (([SIMD2<Double>]) -> Void)?
    private var dragIndex: Int? = nil

    // A diagonal map line: x = source position, y = destination position (both 0…1 over the view).
    private func pointFor(_ a: SIMD2<Double>) -> NSPoint {
        NSPoint(x: CGFloat(a.x) * bounds.width, y: CGFloat(a.y) * bounds.height)
    }
    private func sorted() -> [SIMD2<Double>] { anchors.sorted { $0.x < $1.x } }

    override func draw(_ dirtyRect: NSRect) {
        NSGraphicsContext.saveGraphicsState()
        NSBezierPath(rect: bounds).addClip()
        defer { NSGraphicsContext.restoreGraphicsState() }
        NSColor(hex: 0x0e0b09).setFill(); bounds.fill()
        // The source waveform is the primary reference. Anchors now sit on recognizable audio,
        // rather than on an abstract empty graph.
        if !peaks.isEmpty {
            let mid = bounds.midY
            let half = bounds.height * 0.38
            let fill = NSBezierPath()
            fill.move(to: NSPoint(x: 0, y: mid))
            for (i, peak) in peaks.enumerated() {
                let x = CGFloat(i) / CGFloat(max(1, peaks.count - 1)) * bounds.width
                fill.line(to: NSPoint(x: x, y: mid + CGFloat(peak.y) * half))
            }
            for i in peaks.indices.reversed() {
                let x = CGFloat(i) / CGFloat(max(1, peaks.count - 1)) * bounds.width
                fill.line(to: NSPoint(x: x, y: mid + CGFloat(peaks[i].x) * half))
            }
            fill.close()
            NSColor(hex: 0x39749b).withAlphaComponent(0.42).setFill()
            fill.fill()
        }
        // Time grid makes the source position readable.
        let seconds = max(0.1, clipDuration)
        let gridStep = seconds > 30 ? 5.0 : seconds > 10 ? 2.0 : 1.0
        var t = 0.0
        while t <= seconds {
            let x = CGFloat(t / seconds) * bounds.width
            NSColor(hex: 0x29241f).setStroke()
            let g = NSBezierPath(); g.move(to: NSPoint(x: x, y: 0)); g.line(to: NSPoint(x: x, y: bounds.height)); g.lineWidth = 0.5; g.stroke()
            let label = String(format: "%.1fs", t) as NSString
            label.draw(at: NSPoint(x: x + 3, y: bounds.height - 14),
                       withAttributes: [.font: NSFont.monospacedSystemFont(ofSize: 8, weight: .regular),
                                        .foregroundColor: NSColor(hex: 0x756a60)])
            t += gridStep
        }
        // Reference identity diagonal.
        NSColor(hex: 0x75604a).withAlphaComponent(0.35).setStroke()
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
            NSColor(hex: 0xe0a04a).withAlphaComponent(0.55).setStroke()
            let marker = NSBezierPath(); marker.move(to: NSPoint(x: p.x, y: 0))
            marker.line(to: NSPoint(x: p.x, y: bounds.height)); marker.lineWidth = 1; marker.stroke()
            let r = NSRect(x: p.x - 4, y: p.y - 4, width: 8, height: 8)
            NSColor(hex: 0xe0a04a).setFill(); NSBezierPath(ovalIn: r).fill()
        }
        // Legible: a readable size and a light colour on the dark canvas, with a subtle shadow so
        // it reads over the waveform. Explains what the anchor bars actually do.
        let hint = "세로 막대 = 워프 앵커 · 가로 위치는 원본 시각, 위/아래로 끌면 그 지점의 재생을 앞당기거나 늦춰 구간을 늘이고 줄입니다 · 더블클릭으로 앵커 추가" as NSString
        let shadow = NSShadow()
        shadow.shadowColor = NSColor.black.withAlphaComponent(0.7)
        shadow.shadowBlurRadius = 2
        shadow.shadowOffset = NSSize(width: 0, height: -1)
        hint.draw(at: NSPoint(x: 8, y: 6), withAttributes: [
            .font: NSFont.systemFont(ofSize: 11, weight: .medium),
            .foregroundColor: NSColor(hex: 0xd8cbb8),
            .shadow: shadow,
        ])
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
