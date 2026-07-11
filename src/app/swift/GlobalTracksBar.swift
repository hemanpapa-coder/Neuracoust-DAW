import SwiftUI

/// The conductor / global-track bar above the timeline, Pro Tools style: thin rulers for
/// markers, tempo, meter, key, chords and lyrics, each with a left name column (colour
/// dot + name + "＋" add-at-playhead) and an event lane on the timeline's time axis with
/// bar grid lines. Double-click adds, drag moves (bar-snapped), right-click deletes.
struct GlobalTracksBar: View {
    @EnvironmentObject private var engine: EngineController

    private static let headerWidth: CGFloat = 150
    private static let rowHeight: CGFloat = 19

    private enum Ruler: String, CaseIterable {
        case marker = "마커", tempo = "템포", meter = "박자", key = "조성", chord = "코드", lyric = "가사"
        var color: Color {
            switch self {
            case .marker: return Color(hex: 0xe8c84a)
            case .tempo: return Color(hex: 0x5fb85f)
            case .meter: return Color(hex: 0x5f9fd6)
            case .key: return Color(hex: 0xb79cf0)
            case .chord: return Color(hex: 0xe6a23c)
            case .lyric: return Color(hex: 0x35bfa8)
            }
        }
        var canAdd: Bool { self != .meter && self != .key }
    }

    @State private var addTarget: AddTarget?
    private struct AddTarget: Identifiable { let id = UUID(); let ruler: Ruler; let seconds: Double }
    @State private var dragSeconds: [String: Double] = [:]

    var body: some View {
        GeometryReader { geo in
            let laneWidth = max(1, geo.size.width - Self.headerWidth)
            VStack(spacing: 0) {
                ForEach(Ruler.allCases, id: \.self) { ruler in
                    row(ruler, laneWidth: laneWidth)
                        .frame(height: Self.rowHeight)
                    Rectangle().fill(Color(hex: 0x3a342d)).frame(height: 0.5)
                }
            }
        }
        .frame(height: Self.rowHeight * CGFloat(Ruler.allCases.count) + CGFloat(Ruler.allCases.count))
        .background(Color(hex: 0x2a2622))
        .sheet(item: $addTarget) { t in
            ConductorAddSheet(ruler: t.ruler.rawValue, seconds: t.seconds) { text in
                commitAdd(t, text: text)
            } dismiss: { addTarget = nil }
        }
    }

    private func x(_ t: Double, _ laneWidth: CGFloat) -> CGFloat {
        Self.headerWidth + CGFloat((t - engine.visibleStart) / max(0.001, engine.visibleDuration)) * laneWidth
    }
    private func seconds(atX px: CGFloat, _ laneWidth: CGFloat) -> Double {
        engine.visibleStart + Double((px - Self.headerWidth) / laneWidth) * engine.visibleDuration
    }

    @ViewBuilder
    private func row(_ ruler: Ruler, laneWidth: CGFloat) -> some View {
        HStack(spacing: 0) {
            // Name column — colour dot, name, and a ＋ that adds at the playhead.
            HStack(spacing: 5) {
                Circle().fill(ruler.color).frame(width: 6, height: 6)
                Text(ruler.rawValue)
                    .font(Theme.Font.mono(9, .medium))
                    .foregroundStyle(Theme.Palette.textDim)
                if ruler == .key { keyPicker }
                Spacer(minLength: 0)
                if ruler.canAdd {
                    Button {
                        addTarget = AddTarget(ruler: ruler, seconds: max(0, engine.snap(engine.playheadSeconds)))
                    } label: {
                        Image(systemName: "plus")
                            .font(.system(size: 8, weight: .bold))
                            .foregroundStyle(Theme.Palette.textDim)
                            .frame(width: 13, height: 13)
                            .background(RoundedRectangle(cornerRadius: 3).fill(Color(hex: 0x403830)))
                    }
                    .buttonStyle(.plain)
                }
            }
            .padding(.horizontal, 8)
            .frame(width: Self.headerWidth, alignment: .leading)
            .background(Color(hex: 0x332c26))

            // Event lane, with bar grid + events.
            ZStack(alignment: .leading) {
                barGrid(laneWidth: laneWidth)
                lane(ruler, laneWidth: laneWidth)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .contentShape(Rectangle())
            .overlay {
                if ruler.canAdd {
                    GeometryReader { g in
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
        }
    }

    @ViewBuilder
    private func barGrid(_ ignore: Bool = false, laneWidth: CGFloat) -> some View {
        Canvas { ctx, size in
            let spb = engine.secondsPerBar
            guard spb > 0.001 else { return }
            let firstBar = Int(engine.visibleStart / spb)
            var bar = firstBar
            while true {
                let t = Double(bar) * spb
                if t > engine.visibleStart + engine.visibleDuration { break }
                let px = x(t, laneWidth) - Self.headerWidth
                if px >= 0 {
                    var p = Path()
                    p.move(to: CGPoint(x: px, y: 0)); p.addLine(to: CGPoint(x: px, y: size.height))
                    ctx.stroke(p, with: .color(.white.opacity(0.07)), lineWidth: 1)
                }
                bar += 1
                if bar - firstBar > 4096 { break }
            }
        }
    }

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
            Rectangle().fill(Ruler.tempo.color.opacity(0.5)).frame(height: 1.5)
                .frame(maxHeight: .infinity, alignment: .center)
                .offset(x: Self.headerWidth * 0)
                .padding(.leading, 0)
            ForEach(engine.tempoMarkers) { e in
                tempoPoint(e, laneWidth: laneWidth)
            }
        case .meter:
            chip("\(engine.timeSignature.0)/\(engine.timeSignature.1)", color: Ruler.meter.color, atX: 2)
        case .key:
            EmptyView()
        case .chord:
            ForEach(engine.chords) { e in
                eventChip(e, ruler: .chord, diamond: true, laneWidth: laneWidth,
                          onMove: { engine.moveChord(from: e.timeSeconds, to: $0) },
                          onDelete: { engine.deleteChord(at: e.timeSeconds) })
            }
        case .lyric:
            ForEach(engine.lyrics) { e in
                eventChip(e, ruler: .lyric, diamond: false, laneWidth: laneWidth,
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
                    MarkerPin().fill(Ruler.marker.color).frame(width: 9, height: 13)
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
                    RoundedRectangle(cornerRadius: 2).fill(Ruler.tempo.color).frame(width: 7, height: 7)
                    Text(e.label).font(Theme.Font.mono(9, .medium)).foregroundStyle(Ruler.tempo.color)
                }
                .offset(x: px - Self.headerWidth - 3)
                .contextMenu { Button("삭제", role: .destructive) { engine.deleteTempoMarker(at: e.timeSeconds) } }
            }
        }
    }

    private func eventChip(_ e: EngineController.ConductorEvent, ruler: Ruler, diamond: Bool, laneWidth: CGFloat,
                           onMove: @escaping (Double) -> Void, onDelete: @escaping () -> Void) -> some View {
        let px = x(dragSeconds["\(ruler.rawValue)\(e.timeSeconds)"] ?? e.timeSeconds, laneWidth)
        return Group {
            if px >= Self.headerWidth - 30 && px <= Self.headerWidth + laneWidth {
                HStack(spacing: 3) {
                    if diamond {
                        Rectangle().fill(ruler.color).frame(width: 5, height: 5).rotationEffect(.degrees(45))
                    }
                    Text(e.label.isEmpty ? "•" : e.label)
                        .font(Theme.Font.mono(9, .medium)).foregroundStyle(ruler.color).lineLimit(1)
                }
                .padding(.horizontal, 4)
                .frame(height: 14)
                .background(RoundedRectangle(cornerRadius: 3).fill(ruler.color.opacity(0.14)))
                .offset(x: px - Self.headerWidth + 2)
                .gesture(dragGesture(key: "\(ruler.rawValue)\(e.timeSeconds)", start: e.timeSeconds, laneWidth: laneWidth, onMove: onMove))
                .contextMenu { Button("삭제", role: .destructive) { onDelete() } }
            }
        }
    }

    private func chip(_ text: String, color: Color, atX: CGFloat) -> some View {
        Text(text)
            .font(Theme.Font.mono(9, .medium)).foregroundStyle(color)
            .padding(.horizontal, 4).frame(height: 14)
            .background(RoundedRectangle(cornerRadius: 3).fill(color.opacity(0.14)))
            .offset(x: atX)
            .frame(maxHeight: .infinity, alignment: .center)
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

    private var keyPicker: some View {
        Menu {
            ForEach(["C","G","D","A","E","B","F#","Db","Ab","Eb","Bb","F"], id: \.self) { root in
                Menu(root) {
                    Button("\(root) Major") { engine.musicalKey = root }
                    Button("\(root) minor") { engine.musicalKey = "\(root)m" }
                }
            }
        } label: {
            Text(engine.musicalKey).font(Theme.Font.mono(9, .medium)).foregroundStyle(Ruler.key.color)
        }
        .menuStyle(.borderlessButton).fixedSize()
    }

    private func commitAdd(_ t: AddTarget, text: String) {
        switch t.ruler {
        case .tempo: engine.addTempoMarker(at: t.seconds, bpm: Double(text) ?? Double(engine.tempoBpm))
        case .chord: engine.addChord(at: t.seconds, name: text)
        case .lyric: engine.addLyric(at: t.seconds, text: text)
        case .marker: engine.addMarker(at: t.seconds, name: text)
        default: break
        }
        addTarget = nil
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

/// Small text-entry sheet for adding a conductor event at a time.
private struct ConductorAddSheet: View {
    let ruler: String
    let seconds: Double
    let commit: (String) -> Void
    let dismiss: () -> Void
    @State private var text = ""

    var body: some View {
        VStack(alignment: .leading, spacing: Theme.Space.lg) {
            Text("\(ruler) 추가 · \(String(format: "%.2f s", seconds))")
                .font(Theme.Font.ui(12, .semibold)).foregroundStyle(Theme.Palette.textBright)
            TextField(placeholder, text: $text)
                .textFieldStyle(.roundedBorder).frame(width: 200).onSubmit { commit(text) }
            HStack {
                Spacer()
                Button("취소") { dismiss() }
                Button("추가") { commit(text) }.keyboardShortcut(.defaultAction)
            }
        }
        .padding(Theme.Space.xxl).frame(width: 280)
    }

    private var placeholder: String {
        switch ruler {
        case "템포": return "BPM (예: 128)"
        case "코드": return "코드 (예: Cmaj7)"
        case "가사": return "가사"
        default: return "이름"
        }
    }
}
