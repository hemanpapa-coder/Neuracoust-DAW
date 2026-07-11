import SwiftUI

/// The conductor / global-track bar above the timeline, Pro Tools style: one ruler each for
/// markers, tempo, meter (time signature), key, chords and lyrics. Every ruler has a left
/// name column (colour bar + name + "＋" add-at-playhead) and an event lane over the
/// timeline's time axis, above a GPU bar-line grid. Each ruler's height is adjustable by
/// dragging the separator below it, and the heights persist. Double-click a lane adds an
/// event, drag an event to move it (bar-snapped), right-click to delete.
struct GlobalTracksBar: View {
    @EnvironmentObject private var engine: EngineController

    private static let headerWidth: CGFloat = 152
    private static let minRow: CGFloat = 16
    private static let maxRow: CGFloat = 72
    private static let sepHeight: CGFloat = 1

    enum Ruler: String, CaseIterable, Identifiable {
        case marker = "마커", tempo = "템포", meter = "박자", key = "조성", chord = "코드", lyric = "가사"
        var id: String { rawValue }
        var color: Color {
            switch self {
            case .marker: return Color(hex: 0xe8c84a)
            case .tempo:  return Color(hex: 0x5fb85f)
            case .meter:  return Color(hex: 0x5f9fd6)
            case .key:    return Color(hex: 0xb79cf0)
            case .chord:  return Color(hex: 0xe6a23c)
            case .lyric:  return Color(hex: 0x35bfa8)
            }
        }
        var icon: String {
            switch self {
            case .marker: return "mappin"
            case .tempo:  return "metronome"
            case .meter:  return "textformat.123"
            case .key:    return "music.note"
            case .chord:  return "pianokeys"
            case .lyric:  return "text.quote"
            }
        }
        var defaultHeight: CGFloat { self == .lyric || self == .chord ? 22 : 20 }
    }

    @State private var heights: [String: CGFloat] = [:]
    @State private var addTarget: AddTarget?
    @State private var dragSeconds: [String: Double] = [:]
    private struct AddTarget: Identifiable { let id = UUID(); let ruler: Ruler; let seconds: Double }

    private func height(_ r: Ruler) -> CGFloat { heights[r.rawValue] ?? r.defaultHeight }
    private var totalHeight: CGFloat {
        Ruler.allCases.reduce(0) { $0 + height($1) } + Self.sepHeight * CGFloat(Ruler.allCases.count)
    }

    var body: some View {
        GeometryReader { geo in
            let laneWidth = max(1, geo.size.width - Self.headerWidth)
            ZStack(alignment: .topLeading) {
                // GPU bar-line grid behind every lane.
                gridBackdrop(laneWidth: laneWidth)
                    .frame(width: laneWidth, height: totalHeight)
                    .offset(x: Self.headerWidth)
                    .allowsHitTesting(false)

                VStack(spacing: 0) {
                    ForEach(Ruler.allCases) { ruler in
                        row(ruler, laneWidth: laneWidth)
                            .frame(height: height(ruler))
                        separator(below: ruler)
                    }
                }
            }
        }
        .frame(height: totalHeight)
        .background(Color(hex: 0x232019))
        .onAppear(perform: restoreHeights)
        .sheet(item: $addTarget) { t in
            ConductorAddSheet(ruler: t.ruler, seconds: t.seconds) { text in commitAdd(t, text: text) }
                dismiss: { addTarget = nil }
        }
    }

    @ViewBuilder
    private func gridBackdrop(laneWidth: CGFloat) -> some View {
        let spb = max(0.0001, engine.secondsPerBar)
        if MetalGridView.isAvailable {
            MetalGridView(startBars: engine.visibleStart / spb,
                          spanBars: engine.visibleDuration / spb,
                          barsPerGroup: max(1, engine.timeSignature.numerator))
        } else {
            barGridCanvas(laneWidth: laneWidth)
        }
    }

    // MARK: geometry

    private func x(_ t: Double, _ laneWidth: CGFloat) -> CGFloat {
        Self.headerWidth + CGFloat((t - engine.visibleStart) / max(0.001, engine.visibleDuration)) * laneWidth
    }
    private func seconds(atX px: CGFloat, _ laneWidth: CGFloat) -> Double {
        engine.visibleStart + Double((px - Self.headerWidth) / laneWidth) * engine.visibleDuration
    }

    // MARK: row

    @ViewBuilder
    private func row(_ ruler: Ruler, laneWidth: CGFloat) -> some View {
        HStack(spacing: 0) {
            nameColumn(ruler)
            lanePanel(ruler, laneWidth: laneWidth)
        }
    }

    private func nameColumn(_ ruler: Ruler) -> some View {
        HStack(spacing: 6) {
            // Colour accent bar + icon, so rulers read apart at a glance.
            RoundedRectangle(cornerRadius: 1.5).fill(ruler.color).frame(width: 3, height: 12)
            Image(systemName: ruler.icon)
                .font(.system(size: 9, weight: .semibold))
                .foregroundStyle(ruler.color.opacity(0.9))
                .frame(width: 12)
            Text(ruler.rawValue)
                .font(Theme.Font.mono(9.5, .medium))
                .foregroundStyle(Theme.Palette.textDim)
            Spacer(minLength: 0)
            Button {
                addTarget = AddTarget(ruler: ruler, seconds: max(0, engine.snap(engine.playheadSeconds)))
            } label: {
                Image(systemName: "plus")
                    .font(.system(size: 8, weight: .bold))
                    .foregroundStyle(ruler.color)
                    .frame(width: 15, height: 15)
                    .background(RoundedRectangle(cornerRadius: 3.5).fill(ruler.color.opacity(0.16)))
                    .overlay(RoundedRectangle(cornerRadius: 3.5).stroke(ruler.color.opacity(0.30), lineWidth: 0.5))
            }
            .buttonStyle(.plain)
            .help("\(ruler.rawValue) 추가 (재생 위치)")
        }
        .padding(.leading, 8).padding(.trailing, 6)
        .frame(width: Self.headerWidth, alignment: .leading)
        .background(Color(hex: 0x2c2820))
        .overlay(alignment: .trailing) {
            Rectangle().fill(Color.black.opacity(0.45)).frame(width: 1)
        }
    }

    @ViewBuilder
    private func lanePanel(_ ruler: Ruler, laneWidth: CGFloat) -> some View {
        ZStack(alignment: .leading) {
            ruler.color.opacity(0.04)   // faint per-ruler tint to separate lanes
            lane(ruler, laneWidth: laneWidth)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .contentShape(Rectangle())
        .overlay {
            GeometryReader { _ in
                Color.clear.contentShape(Rectangle())
                    .gesture(TapGesture(count: 2).sequenced(before: DragGesture(minimumDistance: 0))
                        .onEnded { v in
                            if case .second(_, let d?) = v {
                                addTarget = AddTarget(ruler: ruler,
                                    seconds: max(0, engine.snap(seconds(atX: d.startLocation.x + Self.headerWidth, laneWidth))))
                            }
                        })
            }
        }
    }

    /// The draggable separator below a ruler, which resizes that ruler's height.
    private func separator(below ruler: Ruler) -> some View {
        ZStack {
            Rectangle().fill(Color.black.opacity(0.55)).frame(height: Self.sepHeight)
            Color.clear.frame(height: 5).contentShape(Rectangle())
                .onHover { inside in
                    if inside { NSCursor.resizeUpDown.push() } else { NSCursor.pop() }
                }
                .gesture(
                    DragGesture(minimumDistance: 1)
                        .onChanged { v in
                            let h = min(Self.maxRow, max(Self.minRow, height(ruler) + v.translation.height))
                            heights[ruler.rawValue] = h
                        }
                        .onEnded { _ in persistHeights() }
                )
        }
        .frame(height: Self.sepHeight)
        .zIndex(5)
    }

    // MARK: bar grid fallback (Canvas)

    private func barGridCanvas(laneWidth: CGFloat) -> some View {
        Canvas { ctx, size in
            let spb = engine.secondsPerBar
            guard spb > 0.001 else { return }
            let firstBar = Int(engine.visibleStart / spb)
            var bar = firstBar
            while bar - firstBar <= 4096 {
                let t = Double(bar) * spb
                if t > engine.visibleStart + engine.visibleDuration { break }
                let px = x(t, laneWidth) - Self.headerWidth
                if px >= 0 {
                    var p = Path()
                    p.move(to: CGPoint(x: px, y: 0)); p.addLine(to: CGPoint(x: px, y: size.height))
                    let down = (bar % max(1, engine.timeSignature.numerator)) == 0
                    ctx.stroke(p, with: .color(.white.opacity(down ? 0.14 : 0.07)), lineWidth: 1)
                }
                bar += 1
            }
        }
    }

    // MARK: lanes

    @ViewBuilder
    private func lane(_ ruler: Ruler, laneWidth: CGFloat) -> some View {
        switch ruler {
        case .marker:
            ForEach(engine.markers.indices, id: \.self) { i in
                let m = engine.markers[i]
                markerFlag(time: m.timeSeconds, label: m.name, laneWidth: laneWidth,
                           onMove: { engine.moveMarker(from: m.timeSeconds, to: $0) },
                           onDelete: { engine.deleteMarker(at: m.timeSeconds) })
            }
        case .tempo:
            Rectangle().fill(Ruler.tempo.color.opacity(0.35)).frame(height: 1)
                .frame(maxHeight: .infinity, alignment: .center)
            ForEach(engine.tempoMarkers) { e in tempoPoint(e, laneWidth: laneWidth) }
        case .meter:
            ForEach(engine.meterEvents) { e in
                eventChip(e, ruler: .meter, glyph: .square, laneWidth: laneWidth,
                          anchored: e.timeSeconds < 0.01,
                          onMove: { engine.moveMeter(from: e.timeSeconds, to: $0) },
                          onDelete: { engine.deleteMeter(at: e.timeSeconds) })
            }
        case .key:
            if engine.keyEvents.isEmpty {
                eventChip(EngineController.ConductorEvent(id: -1, timeSeconds: 0, label: engine.musicalKey),
                          ruler: .key, glyph: .circle, laneWidth: laneWidth, anchored: true,
                          onMove: { _ in }, onDelete: {})
            } else {
                ForEach(engine.keyEvents) { e in
                    eventChip(e, ruler: .key, glyph: .circle, laneWidth: laneWidth,
                              anchored: e.timeSeconds < 0.01,
                              onMove: { engine.moveKey(from: e.timeSeconds, to: $0) },
                              onDelete: { engine.deleteKey(at: e.timeSeconds) })
                }
            }
        case .chord:
            ForEach(engine.chords) { e in
                eventChip(e, ruler: .chord, glyph: .diamond, laneWidth: laneWidth, anchored: false,
                          onMove: { engine.moveChord(from: e.timeSeconds, to: $0) },
                          onDelete: { engine.deleteChord(at: e.timeSeconds) })
            }
        case .lyric:
            ForEach(engine.lyrics) { e in
                eventChip(e, ruler: .lyric, glyph: .none, laneWidth: laneWidth, anchored: false,
                          onMove: { engine.moveLyric(from: e.timeSeconds, to: $0) },
                          onDelete: { engine.deleteLyric(at: e.timeSeconds) })
            }
        }
    }

    // MARK: event views

    private func markerFlag(time: Double, label: String, laneWidth: CGFloat,
                            onMove: @escaping (Double) -> Void, onDelete: @escaping () -> Void) -> some View {
        let px = x(dragSeconds["m\(time)"] ?? time, laneWidth)
        return Group {
            if px >= Self.headerWidth - 40 && px <= Self.headerWidth + laneWidth {
                HStack(spacing: 2) {
                    MarkerPin().fill(Ruler.marker.color).frame(width: 9, height: 12)
                    if !label.isEmpty {
                        Text(label).font(Theme.Font.mono(9, .medium)).foregroundStyle(Ruler.marker.color).lineLimit(1)
                    }
                }
                .offset(x: px - Self.headerWidth)
                .gesture(dragGesture(key: "m\(time)", start: time, laneWidth: laneWidth, onMove: onMove))
                .contextMenu { Button("삭제", role: .destructive) { onDelete() } }
            }
        }
    }

    private func tempoPoint(_ e: EngineController.ConductorEvent, laneWidth: CGFloat) -> some View {
        let px = x(e.timeSeconds, laneWidth)
        return Group {
            if px >= Self.headerWidth - 20 && px <= Self.headerWidth + laneWidth {
                HStack(spacing: 3) {
                    RoundedRectangle(cornerRadius: 2).fill(Ruler.tempo.color).frame(width: 6, height: 6)
                    Text(e.label).font(Theme.Font.mono(9, .medium)).foregroundStyle(Ruler.tempo.color)
                }
                .offset(x: px - Self.headerWidth - 3)
                .contextMenu { Button("삭제", role: .destructive) { engine.deleteTempoMarker(at: e.timeSeconds) } }
            }
        }
    }

    private enum Glyph { case diamond, square, circle, none }

    private func eventChip(_ e: EngineController.ConductorEvent, ruler: Ruler, glyph: Glyph, laneWidth: CGFloat,
                           anchored: Bool,
                           onMove: @escaping (Double) -> Void, onDelete: @escaping () -> Void) -> some View {
        let px = x(dragSeconds["\(ruler.rawValue)\(e.timeSeconds)"] ?? e.timeSeconds, laneWidth)
        return Group {
            if px >= Self.headerWidth - 30 && px <= Self.headerWidth + laneWidth {
                HStack(spacing: 3) {
                    switch glyph {
                    case .diamond: Rectangle().fill(ruler.color).frame(width: 5, height: 5).rotationEffect(.degrees(45))
                    case .square:  RoundedRectangle(cornerRadius: 1).fill(ruler.color).frame(width: 5, height: 5)
                    case .circle:  Circle().fill(ruler.color).frame(width: 5, height: 5)
                    case .none:    EmptyView()
                    }
                    Text(e.label.isEmpty ? "•" : e.label)
                        .font(Theme.Font.mono(9, .semibold)).foregroundStyle(ruler.color).lineLimit(1)
                }
                .padding(.horizontal, 5).frame(height: 15)
                .background(RoundedRectangle(cornerRadius: 3).fill(ruler.color.opacity(0.16)))
                .overlay(RoundedRectangle(cornerRadius: 3).stroke(ruler.color.opacity(anchored ? 0.15 : 0.35), lineWidth: 0.5))
                .offset(x: px - Self.headerWidth + 2)
                .gesture(anchored ? nil : dragGesture(key: "\(ruler.rawValue)\(e.timeSeconds)", start: e.timeSeconds, laneWidth: laneWidth, onMove: onMove))
                .contextMenu { if !anchored { Button("삭제", role: .destructive) { onDelete() } } }
            }
        }
    }

    private func dragGesture(key: String, start: Double, laneWidth: CGFloat,
                             onMove: @escaping (Double) -> Void) -> some Gesture {
        DragGesture(minimumDistance: 3)
            .onChanged { v in
                dragSeconds[key] = max(0, start + Double(v.translation.width / laneWidth) * engine.visibleDuration)
            }
            .onEnded { v in
                let t = max(0, start + Double(v.translation.width / laneWidth) * engine.visibleDuration)
                dragSeconds[key] = nil
                onMove(t)
            }
    }

    // MARK: add + persistence

    private func commitAdd(_ t: AddTarget, text: String) {
        switch t.ruler {
        case .marker: engine.addMarker(at: t.seconds, name: text)
        case .tempo:  engine.addTempoMarker(at: t.seconds, bpm: Double(text) ?? Double(engine.tempoBpm))
        case .meter:  engine.addMeter(at: t.seconds, text: text.isEmpty ? "\(engine.timeSignature.numerator)/\(engine.timeSignature.denominator)" : text)
        case .key:    engine.addKey(at: t.seconds, name: text)
        case .chord:  engine.addChord(at: t.seconds, name: text)
        case .lyric:  engine.addLyric(at: t.seconds, text: text)
        }
        addTarget = nil
    }

    private static let heightsKey = "nc.conductorRowHeights"
    private func persistHeights() {
        let s = Ruler.allCases.map { "\($0.rawValue):\(Int(height($0)))" }.joined(separator: "|")
        UserDefaults.standard.set(s, forKey: Self.heightsKey)
    }
    private func restoreHeights() {
        guard let s = UserDefaults.standard.string(forKey: Self.heightsKey) else { return }
        for pair in s.split(separator: "|") {
            let kv = pair.split(separator: ":")
            if kv.count == 2, let h = Double(kv[1]) {
                heights[String(kv[0])] = min(Self.maxRow, max(Self.minRow, CGFloat(h)))
            }
        }
    }
}

/// Pro Tools marker pin — a downward pentagon.
private struct MarkerPin: Shape {
    func path(in r: CGRect) -> Path {
        var p = Path()
        p.move(to: CGPoint(x: r.minX, y: r.minY))
        p.addLine(to: CGPoint(x: r.maxX, y: r.minY))
        p.addLine(to: CGPoint(x: r.maxX, y: r.minY + r.height * 0.62))
        p.addLine(to: CGPoint(x: r.midX, y: r.maxY))
        p.addLine(to: CGPoint(x: r.minX, y: r.minY + r.height * 0.62))
        p.closeSubpath()
        return p
    }
}

/// Text-entry sheet for adding a conductor event, tuned per ruler.
private struct ConductorAddSheet: View {
    let ruler: GlobalTracksBar.Ruler
    let seconds: Double
    let commit: (String) -> Void
    let dismiss: () -> Void
    @State private var text = ""

    var body: some View {
        VStack(alignment: .leading, spacing: Theme.Space.lg) {
            HStack(spacing: 6) {
                Image(systemName: ruler.icon).foregroundStyle(ruler.color)
                Text("\(ruler.rawValue) 추가 · \(String(format: "%.2f s", seconds))")
                    .font(Theme.Font.ui(12, .semibold)).foregroundStyle(Theme.Palette.textBright)
            }
            if ruler == .meter {
                meterPresets
            } else if ruler == .key {
                keyPresets
            }
            TextField(placeholder, text: $text)
                .textFieldStyle(.roundedBorder).frame(width: 220).onSubmit { commit(text) }
            HStack {
                Spacer()
                Button("취소") { dismiss() }
                Button("추가") { commit(text) }.keyboardShortcut(.defaultAction)
            }
        }
        .padding(Theme.Space.xxl).frame(width: 300)
    }

    private var meterPresets: some View {
        HStack(spacing: 5) {
            ForEach(["4/4", "3/4", "6/8", "2/4", "5/4", "7/8"], id: \.self) { m in
                Button(m) { commit(m) }.buttonStyle(.bordered).controlSize(.small)
            }
        }
    }
    private var keyPresets: some View {
        HStack(spacing: 5) {
            ForEach(["C", "G", "D", "A", "Am", "Em", "F", "Bb"], id: \.self) { k in
                Button(k) { commit(k) }.buttonStyle(.bordered).controlSize(.small)
            }
        }
    }

    private var placeholder: String {
        switch ruler {
        case .tempo: return "BPM (예: 128)"
        case .meter: return "박자 (예: 4/4)"
        case .key:   return "조성 (예: C, Am, F#)"
        case .chord: return "코드 (예: Cmaj7)"
        case .lyric: return "가사"
        case .marker: return "이름"
        }
    }
}
