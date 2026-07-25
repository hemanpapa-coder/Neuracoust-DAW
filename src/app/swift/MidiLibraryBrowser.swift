import SwiftUI

/// In-window MIDI library — a bottom panel that expands under the timeline (like the piano roll / Logic's
/// loop browser), NOT a separate modal window. Categories on the left, results on the right; rows drag
/// onto a timeline track or click to insert at the playhead.
struct MidiLibraryPanel: View {
    @EnvironmentObject private var engine: EngineController
    @ObservedObject var library: MidiLibrary

    // English classifier keys → Korean labels.
    private static let instKo = [
        "Drums": "드럼", "Piano": "피아노", "Keys": "건반", "Guitar": "기타", "Bass": "베이스",
        "Strings": "스트링", "Brass": "브라스", "Reed": "목관(리드)", "Pipe": "관(플루트)",
        "Organ": "오르간", "Synth": "신스", "Lead": "리드", "Mallet": "말렛", "Percussion": "퍼커션",
        "Ethnic": "월드", "SFX": "효과음", "Unknown": "기타", "Multi": "멀티(풀송)",
    ]
    private static let moodKo = [
        "bright": "밝음", "dark": "어두움", "energetic": "활기참", "calm": "차분함",
        "tense": "긴장", "melancholic": "우울",
    ]
    private func instLabel(_ s: String) -> String { Self.instKo[s] ?? s }
    private func moodLabel(_ s: String) -> String { Self.moodKo[s] ?? s }

    var body: some View {
        VStack(spacing: 0) {
            resizeHandleAndHeader
            Divider()
            HStack(spacing: 0) {
                categoryColumn.frame(width: 200)
                Rectangle().fill(Theme.Palette.deepBorder).frame(width: 1)
                resultsList
            }
        }
        .frame(height: 300)
        .background(Theme.Palette.panel)
        .onAppear { library.loadCacheIfAvailable() }
    }

    // MARK: Header (title, index, filters, close)

    private var resizeHandleAndHeader: some View {
        HStack(spacing: Theme.Space.md) {
            Text("MIDI 라이브러리")
                .font(Theme.Font.ui(12, .bold)).foregroundStyle(Theme.Palette.textBright)
                .lineLimit(1).fixedSize()

            if library.isIndexing {
                Text(library.indexProgress.isEmpty ? "색인 중…" : library.indexProgress)
                    .font(Theme.Font.mono(9)).foregroundStyle(Theme.Palette.textFaint)
                    .lineLimit(1).fixedSize()
            } else {
                Text(library.totalIndexed > 0 ? "\(library.totalIndexed)개" :
                        (library.hasCollection ? "미색인" : "컬렉션 없음"))
                    .font(Theme.Font.mono(9)).foregroundStyle(Theme.Palette.textFaint)
                    .lineLimit(1).fixedSize()
                Button(library.totalIndexed > 0 ? "다시 색인" : "색인 만들기") { library.rebuildIndex() }
                    .disabled(!library.hasCollection)
                    .font(Theme.Font.ui(10, .medium)).fixedSize()
            }

            Divider().frame(height: 14)

            // Drum vs melodic — the headline split.
            Picker("", selection: $library.drumFilter) {
                Text("전체").tag(MidiLibrary.DrumFilter.all)
                Text("드럼").tag(MidiLibrary.DrumFilter.drumsOnly)
                Text("멜로디").tag(MidiLibrary.DrumFilter.melodicOnly)
            }.pickerStyle(.segmented).labelsHidden().frame(width: 150).fixedSize()

            instrumentMenu
            moodMenu

            Spacer(minLength: Theme.Space.md)

            TextField("검색", text: $library.searchText)
                .textFieldStyle(.roundedBorder).frame(minWidth: 90, maxWidth: 150).font(Theme.Font.ui(11))

            Button {
                engine.closeMidiLibrary()
            } label: { Image(systemName: "xmark.circle.fill").foregroundStyle(Theme.Palette.textFaint) }
                .buttonStyle(.plain).help("닫기").fixedSize()
        }
        .padding(.horizontal, Theme.Space.lg).padding(.vertical, Theme.Space.sm)
        .background(Theme.Palette.toolbar)
    }

    private var instrumentMenu: some View {
        Menu {
            Button("전체 악기") { library.instrumentFilter = nil }
            Divider()
            ForEach(library.instruments, id: \.self) { i in
                Button(instLabel(i)) { library.instrumentFilter = i }
            }
        } label: {
            Text(library.instrumentFilter.map(instLabel) ?? "악기")
                .font(Theme.Font.ui(10)).lineLimit(1)
        }.frame(width: 84).fixedSize()
    }

    private var moodMenu: some View {
        Menu {
            Button("전체 분위기") { library.moodFilter = nil }
            Divider()
            ForEach(["bright", "energetic", "calm", "dark", "tense", "melancholic"], id: \.self) { m in
                Button(moodLabel(m)) { library.moodFilter = m }
            }
        } label: {
            Text(library.moodFilter.map(moodLabel) ?? "분위기")
                .font(Theme.Font.ui(10)).lineLimit(1)
        }.frame(width: 84).fixedSize()
    }

    // MARK: Left categories — genre + BPM + fill

    private var categoryColumn: some View {
        VStack(alignment: .leading, spacing: Theme.Space.sm) {
            Picker("", selection: $library.fillFilter) {
                Text("전체").tag(MidiLibrary.FillFilter.all)
                Text("그루브").tag(MidiLibrary.FillFilter.groovesOnly)
                Text("필").tag(MidiLibrary.FillFilter.fillsOnly)
            }.pickerStyle(.segmented).labelsHidden()

            HStack(spacing: 4) {
                Text("BPM").font(Theme.Font.mono(8)).foregroundStyle(Theme.Palette.textFaint)
                Stepper("\(library.bpmMin)", value: $library.bpmMin, in: 0...300, step: 5).font(Theme.Font.mono(8))
                Stepper("\(library.bpmMax)", value: $library.bpmMax, in: 0...300, step: 5).font(Theme.Font.mono(8))
            }

            Text("장르").font(Theme.Font.mono(7)).tracking(0.6).foregroundStyle(Theme.Palette.textFaint)
            ScrollView {
                VStack(alignment: .leading, spacing: 1) {
                    genreRow("전체", value: nil)
                    ForEach(library.genres, id: \.self) { g in genreRow(g, value: g) }
                }
            }
        }
        .padding(Theme.Space.md)
    }

    private func genreRow(_ label: String, value: String?) -> some View {
        let selected = library.genreFilter == value
        return Text(label)
            .font(Theme.Font.ui(10, selected ? .bold : .regular))
            .foregroundStyle(selected ? Theme.Palette.textBright : Theme.Palette.textDim)
            .lineLimit(1)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.vertical, 2).padding(.horizontal, 6)
            .background(selected ? Theme.Palette.accent.opacity(0.25) : .clear)
            .clipShape(RoundedRectangle(cornerRadius: 4))
            .contentShape(Rectangle())
            .onTapGesture { library.genreFilter = value }
    }

    // MARK: Results

    private var resultsList: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack {
                Text("\(library.results.count)개" + (library.results.count >= MidiLibrary.displayCap ? " (상위)" : ""))
                    .font(Theme.Font.mono(9)).foregroundStyle(Theme.Palette.textFaint)
                Spacer()
                Text("드래그 → 트랙 · 클릭 → 삽입")
                    .font(Theme.Font.mono(8)).foregroundStyle(Theme.Palette.textFaint)
            }
            .padding(.horizontal, Theme.Space.md).padding(.vertical, 4)
            Rectangle().fill(Theme.Palette.deepBorder).frame(height: 1)
            ScrollView {
                LazyVStack(spacing: 1) {
                    ForEach(library.results) { row($0) }
                }
                .padding(Theme.Space.sm)
            }
        }
    }

    private func row(_ e: MidiLibraryEntry) -> some View {
        HStack(spacing: 8) {
            Image(systemName: e.isDrum ? "square.grid.2x2" : (e.isFill ? "arrow.turn.down.right" : "pianokeys"))
                .font(.system(size: 10))
                .foregroundStyle(e.isDrum ? Theme.Palette.accent : Theme.Palette.textDim)
                .frame(width: 14)
            VStack(alignment: .leading, spacing: 1) {
                Text(e.name).font(Theme.Font.ui(11)).foregroundStyle(Theme.Palette.textBright).lineLimit(1)
                Text(e.pack).font(Theme.Font.mono(7)).foregroundStyle(Theme.Palette.textFaint).lineLimit(1)
            }
            Spacer()
            Text(instLabel(e.instrument)).font(Theme.Font.mono(8)).foregroundStyle(Theme.Palette.textDim)
            if !e.mood.isEmpty {
                Text(moodLabel(e.mood)).font(Theme.Font.mono(8)).foregroundStyle(Theme.Palette.textFaint)
            }
            if !e.genre.isEmpty {
                Text(e.genre).font(Theme.Font.mono(8)).foregroundStyle(Theme.Palette.textFaint).lineLimit(1)
            }
            if let b = e.bpm, b > 0 {
                Text("\(b)").font(Theme.Font.mono(8, .bold)).foregroundStyle(Theme.Palette.textDim)
                    .padding(.horizontal, 5).padding(.vertical, 1)
                    .background(RoundedRectangle(cornerRadius: 3).fill(Theme.Palette.deepBorder))
            }
        }
        .padding(.vertical, 4).padding(.horizontal, 8)
        .background(RoundedRectangle(cornerRadius: 4).fill(Theme.Palette.rail))
        .contentShape(Rectangle())
        .onTapGesture { engine.insertMidiFileAtPlayhead(e.path) }
        .onDrag { NSItemProvider(object: URL(fileURLWithPath: e.path) as NSURL) }
    }
}
