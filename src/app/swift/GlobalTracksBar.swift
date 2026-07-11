import SwiftUI

/// The conductor / global-track bar above the timeline: tempo, time signature, key,
/// chords, lyrics and markers, all mapped to the same time axis as the lanes (it reads
/// the timeline's visibleStart/visibleDuration, so it zooms and pans in lock-step).
/// Double-click a row's lane area to add an event there.
struct GlobalTracksBar: View {
    @EnvironmentObject private var engine: EngineController

    private static let headerWidth: CGFloat = 150
    private static let rowHeight: CGFloat = 17

    private enum Row: String, CaseIterable { case tempo = "템포", sig = "박자", key = "조성",
                                                  chord = "코드", lyric = "가사", marker = "마커" }

    @State private var addKind: AddKind?
    private struct AddKind: Identifiable { let id = UUID(); let row: Row; let seconds: Double }

    var body: some View {
        GeometryReader { geo in
            let laneWidth = max(1, geo.size.width - Self.headerWidth)
            VStack(spacing: 0) {
                ForEach(Row.allCases, id: \.self) { row in
                    rowView(row, laneWidth: laneWidth)
                        .frame(height: Self.rowHeight)
                    Rectangle().fill(Theme.Palette.divider.opacity(0.4)).frame(height: 0.5)
                }
            }
        }
        .frame(height: Self.rowHeight * CGFloat(Row.allCases.count) + CGFloat(Row.allCases.count))
        .background(Theme.Palette.ruler)
        .sheet(item: $addKind) { add in
            ConductorAddSheet(row: add.row.rawValue, seconds: add.seconds) { text in
                commitAdd(add, text: text)
            } dismiss: { addKind = nil }
        }
    }

    private func x(_ t: Double, _ laneWidth: CGFloat) -> CGFloat {
        Self.headerWidth + CGFloat((t - engine.visibleStart) / max(0.001, engine.visibleDuration)) * laneWidth
    }
    private func seconds(atX px: CGFloat, _ laneWidth: CGFloat) -> Double {
        engine.visibleStart + Double((px - Self.headerWidth) / laneWidth) * engine.visibleDuration
    }

    @ViewBuilder
    private func rowView(_ row: Row, laneWidth: CGFloat) -> some View {
        HStack(spacing: 0) {
            // Label column, aligned with the timeline's track headers.
            HStack(spacing: 4) {
                Text(row.rawValue)
                    .font(Theme.Font.mono(8, .medium))
                    .foregroundStyle(Theme.Palette.textDim)
                if row == .key {
                    keyPicker
                }
                Spacer()
            }
            .padding(.leading, 10)
            .frame(width: Self.headerWidth, alignment: .leading)
            .background(Color(hex: 0x332c26))

            // Lane area with the events.
            ZStack(alignment: .leading) {
                Color.clear.contentShape(Rectangle())
                laneContent(row, laneWidth: laneWidth)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .overlay(alignment: .leading) {
                // Double-click to add an event at that time (except key/sig, which are global).
                if row == .tempo || row == .chord || row == .lyric || row == .marker {
                    GeometryReader { g in
                        Color.clear.contentShape(Rectangle())
                            .gesture(TapGesture(count: 2).sequenced(before: DragGesture(minimumDistance: 0))
                                .onEnded { value in
                                    if case .second(_, let drag?) = value {
                                        let px = drag.startLocation.x + Self.headerWidth
                                        addKind = AddKind(row: row, seconds: max(0, seconds(atX: px, laneWidth)))
                                    }
                                })
                    }
                }
            }
        }
    }

    @ViewBuilder
    private func laneContent(_ row: Row, laneWidth: CGFloat) -> some View {
        switch row {
        case .tempo:
            eventChips(engine.tempoMarkers, laneWidth: laneWidth, tint: Theme.Palette.amber,
                       leading: "\(engine.tempoBpm)", onDelete: { engine.deleteTempoMarker(at: $0) })
        case .sig:
            Text("\(engine.timeSignature.0)/\(engine.timeSignature.1)")
                .font(Theme.Font.mono(8)).foregroundStyle(Theme.Palette.textSecondary)
                .padding(.leading, 4)
        case .key:
            EmptyView()
        case .chord:
            eventChips(engine.chords, laneWidth: laneWidth, tint: Theme.Palette.instrument,
                       leading: nil, onDelete: { engine.deleteChord(at: $0) })
        case .lyric:
            eventChips(engine.lyrics, laneWidth: laneWidth, tint: Theme.Palette.teal,
                       leading: nil, onDelete: { engine.deleteLyric(at: $0) })
        case .marker:
            eventChips(engine.markers.map { .init(id: Int($0.timeSeconds * 1000), timeSeconds: $0.timeSeconds, label: $0.name) },
                       laneWidth: laneWidth, tint: Theme.Palette.orange, leading: nil,
                       onDelete: { engine.deleteMarker(at: $0) })
        }
    }

    @ViewBuilder
    private func eventChips(_ events: [EngineController.ConductorEvent], laneWidth: CGFloat,
                            tint: Color, leading: String?, onDelete: @escaping (Double) -> Void) -> some View {
        if let leading {
            Text(leading).font(Theme.Font.mono(8)).foregroundStyle(Theme.Palette.textSecondary).padding(.leading, 4)
        }
        ForEach(events) { event in
            let px = x(event.timeSeconds, laneWidth)
            if px >= Self.headerWidth - 20 && px <= Self.headerWidth + laneWidth {
                Text(event.label.isEmpty ? "•" : event.label)
                    .font(Theme.Font.mono(8, .medium))
                    .foregroundStyle(tint)
                    .lineLimit(1)
                    .padding(.horizontal, 3)
                    .background(RoundedRectangle(cornerRadius: 2).fill(tint.opacity(0.16)))
                    .offset(x: px - Self.headerWidth)
                    .contextMenu { Button("삭제", role: .destructive) { onDelete(event.timeSeconds) } }
            }
        }
    }

    private var keyPicker: some View {
        Menu {
            ForEach(["C","G","D","A","E","B","F#","Db","Ab","Eb","Bb","F"], id: \.self) { root in
                Menu(root) {
                    Button("\(root) Major") { engine.musicalKey = "\(root)" }
                    Button("\(root) minor") { engine.musicalKey = "\(root)m" }
                }
            }
        } label: {
            Text(engine.musicalKey)
                .font(Theme.Font.mono(8, .medium))
                .foregroundStyle(Theme.Palette.accent)
        }
        .menuStyle(.borderlessButton)
        .fixedSize()
    }

    private func commitAdd(_ add: AddKind, text: String) {
        switch add.row {
        case .tempo: engine.addTempoMarker(at: add.seconds, bpm: Double(text) ?? Double(engine.tempoBpm))
        case .chord: engine.addChord(at: add.seconds, name: text)
        case .lyric: engine.addLyric(at: add.seconds, text: text)
        case .marker: engine.addMarker(at: add.seconds, name: text)
        default: break
        }
        addKind = nil
    }
}

/// Small text-entry sheet for adding a conductor event at a time.
private struct ConductorAddSheet: View {
    let row: String
    let seconds: Double
    let commit: (String) -> Void
    let dismiss: () -> Void
    @State private var text = ""

    var body: some View {
        VStack(alignment: .leading, spacing: Theme.Space.lg) {
            Text("\(row) 추가 · \(String(format: "%.2f s", seconds))")
                .font(Theme.Font.ui(12, .semibold))
                .foregroundStyle(Theme.Palette.textBright)
            TextField(placeholder, text: $text)
                .textFieldStyle(.roundedBorder)
                .frame(width: 200)
                .onSubmit { commit(text) }
            HStack {
                Spacer()
                Button("취소") { dismiss() }
                Button("추가") { commit(text) }.keyboardShortcut(.defaultAction)
            }
        }
        .padding(Theme.Space.xxl)
        .frame(width: 280)
    }

    private var placeholder: String {
        switch row {
        case "템포": return "BPM (예: 128)"
        case "코드": return "코드 (예: Cmaj7)"
        case "가사": return "가사"
        default: return "이름"
        }
    }
}
