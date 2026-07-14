import SwiftUI
import AppKit

private struct SpeakerDatasetEntry: Decodable, Identifiable {
    struct FrequencyResponse: Decodable {
        let low: Double?
        let high: Double?
        let toleranceDb: Double?
        enum CodingKeys: String, CodingKey { case low, high; case toleranceDb = "tolerance_db" }
    }
    struct ResponseCurve: Decodable {
        let confidence: String
        let source: String
        let sourceUrl: String?
        let points: [[Double]]?
        enum CodingKeys: String, CodingKey { case confidence, source, points; case sourceUrl = "source_url" }
    }
    let catalogName: String
    let brand: String
    let model: String
    let type: String?
    let field: String
    let formFactor: String?
    let drivers: String?
    let enclosure: String?
    let frequencyResponse: FrequencyResponse
    let sensitivityDb: Double?
    let impedanceOhm: Double?
    let amplification: String?
    let crossoverHz: [Double]
    let tonalSignature: String
    let responseCurve: ResponseCurve
    let sharedCurveWith: String?
    let sources: [String]
    let notes: String
    var id: String { catalogName }
    var hasCurve: Bool { !(responseCurve.points ?? []).isEmpty }

    enum CodingKeys: String, CodingKey {
        case catalogName = "catalog_name", brand, model, type, field
        case formFactor = "form_factor", drivers, enclosure
        case frequencyResponse = "freq_response_hz"
        case sensitivityDb = "sensitivity_db", impedanceOhm = "impedance_ohm"
        case amplification, crossoverHz = "crossover_hz", tonalSignature = "tonal_signature"
        case responseCurve = "response_curve", sharedCurveWith = "shared_curve_with", sources, notes
    }
}

private enum SpeakerDataset {
    static let entries: [SpeakerDatasetEntry] = {
        guard let url = Bundle.main.url(forResource: "speaker_model_dataset", withExtension: "json"),
              let data = try? Data(contentsOf: url),
              let decoded = try? JSONDecoder().decode([SpeakerDatasetEntry].self, from: data) else { return [] }
        return decoded
    }()
}

struct SpeakerComparisonView: View {
    @Environment(\.dismiss) private var dismiss
    @State private var firstId = "Genelec 8341A (MF)"
    @State private var secondId = "Yamaha NS-10M Studio (NF)"
    @State private var showDifference = true

    private var entries: [SpeakerDatasetEntry] { SpeakerDataset.entries }
    private var first: SpeakerDatasetEntry? { entries.first { $0.id == firstId } ?? entries.first }
    private var second: SpeakerDatasetEntry? { entries.first { $0.id == secondId } ?? entries.dropFirst().first }

    var body: some View {
        VStack(spacing: 0) {
            header
            if entries.isEmpty {
                ContentUnavailableView("데이터셋을 불러올 수 없습니다", systemImage: "exclamationmark.triangle",
                                       description: Text("앱 번들의 speaker_model_dataset.json을 확인하세요."))
            } else {
                ScrollView {
                    VStack(spacing: 14) {
                        selectors
                        responseChart
                        comparisonCards
                        sourcePanel
                    }
                    .padding(18)
                }
            }
            footer
        }
        .frame(minWidth: 860, idealWidth: 980, minHeight: 650, idealHeight: 760)
        .background(Theme.Palette.background)
        .preferredColorScheme(.dark)
    }

    private var header: some View {
        HStack {
            Image(systemName: "waveform.path.ecg.rectangle").foregroundStyle(Theme.Palette.purpleLight)
            VStack(alignment: .leading, spacing: 2) {
                Text("스피커 응답 비교").font(Theme.Font.ui(16, .bold)).foregroundStyle(Theme.Palette.textBright)
                Text("실측 Listening Window 우선 · 자체 평균 기준 0 dB")
                    .font(Theme.Font.mono(8)).foregroundStyle(Theme.Palette.textFaint)
            }
            Spacer()
            Button("닫기") { dismiss() }.buttonStyle(.bordered)
        }
        .padding(.horizontal, 18).frame(height: 58).background(Theme.Gradient.monitorHeader)
    }

    private var selectors: some View {
        HStack(spacing: 12) {
            modelPicker("A", selection: $firstId, color: Theme.Palette.accent)
            Image(systemName: "arrow.left.arrow.right").foregroundStyle(Theme.Palette.textFaint)
            modelPicker("B", selection: $secondId, color: Theme.Palette.orange)
            Toggle("A−B 차이", isOn: $showDifference).toggleStyle(.switch).font(Theme.Font.ui(9))
        }
    }

    private func modelPicker(_ label: String, selection: Binding<String>, color: Color) -> some View {
        HStack(spacing: 8) {
            Text(label).font(Theme.Font.mono(10, .bold)).foregroundStyle(color)
            Picker(label, selection: selection) {
                ForEach(entries) { item in
                    Text(item.catalogName + (item.hasCurve ? "" : " · 측정 없음")).tag(item.id)
                }
            }
            .labelsHidden().pickerStyle(.menu).frame(maxWidth: .infinity, alignment: .leading)
        }
        .padding(8).background(cardBackground)
    }

    private var responseChart: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 14) {
                legend(color: Theme.Palette.accent, text: first?.catalogName ?? "A")
                legend(color: Theme.Palette.orange, text: second?.catalogName ?? "B")
                if showDifference { legend(color: Theme.Palette.purpleLight, text: "A − B") }
                Spacer()
                Text("20 Hz — 20 kHz / dB").font(Theme.Font.mono(8)).foregroundStyle(Theme.Palette.textFaint)
            }
            SpeakerResponseChart(first: first?.responseCurve.points,
                                 second: second?.responseCurve.points,
                                 showDifference: showDifference)
                .frame(height: 300)
            if first?.hasCurve != true || second?.hasCurve != true {
                Label("실측 곡선이 없는 모델은 표시하지 않습니다. 추정 곡선은 생성하지 않습니다.", systemImage: "info.circle")
                    .font(Theme.Font.ui(8.5)).foregroundStyle(Theme.Palette.amber)
            }
        }
        .padding(12).background(cardBackground)
    }

    private var comparisonCards: some View {
        HStack(alignment: .top, spacing: 12) {
            if let first { detailCard(first, color: Theme.Palette.accent) }
            if let second { detailCard(second, color: Theme.Palette.orange) }
        }
    }

    private func detailCard(_ item: SpeakerDatasetEntry, color: Color) -> some View {
        VStack(alignment: .leading, spacing: 7) {
            Text(item.catalogName).font(Theme.Font.ui(11, .bold)).foregroundStyle(color)
            HStack { badge(item.responseCurve.confidence, color: confidenceColor(item.responseCurve.confidence)); badge(item.field, color: Theme.Palette.purple) }
            detailRow("형식", [item.type, item.formFactor].compactMap { $0 }.joined(separator: " · "))
            detailRow("인클로저", item.enclosure ?? "자료 없음")
            detailRow("드라이버", item.drivers ?? "자료 없음")
            detailRow("증폭", item.amplification ?? "자료 없음")
            detailRow("감도", item.sensitivityDb.map { String(format: "%.1f dB", $0) } ?? "자료 없음")
            detailRow("임피던스", item.impedanceOhm.map { String(format: "%.1f Ω", $0) } ?? "자료 없음")
            Divider().overlay(Theme.Palette.divider)
            Text(item.tonalSignature).font(Theme.Font.ui(8.5)).foregroundStyle(Theme.Palette.textSecondary)
                .fixedSize(horizontal: false, vertical: true)
            Text(item.responseCurve.source).font(Theme.Font.mono(7.5)).foregroundStyle(Theme.Palette.textFaint)
        }
        .frame(maxWidth: .infinity, alignment: .leading).padding(12).background(cardBackground)
    }

    private var sourcePanel: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("측정 및 개발 자료").font(Theme.Font.ui(10, .bold)).foregroundStyle(Theme.Palette.textBright)
            ForEach([first, second].compactMap { $0 }) { item in
                VStack(alignment: .leading, spacing: 4) {
                    Text(item.catalogName).font(Theme.Font.ui(9, .semibold)).foregroundStyle(Theme.Palette.textSecondary)
                    if item.sources.isEmpty { Text("검증된 링크 없음").font(Theme.Font.ui(8)).foregroundStyle(Theme.Palette.textFaint) }
                    ForEach(Array(item.sources.prefix(5).enumerated()), id: \.offset) { _, value in
                        if let url = URL(string: value) {
                            Link(destination: url) {
                                Label(shortSource(value), systemImage: "arrow.up.right.square")
                                    .font(Theme.Font.mono(7.5)).lineLimit(1)
                            }
                        }
                    }
                }
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading).padding(12).background(cardBackground)
    }

    private var footer: some View {
        HStack {
            Text("260714.1330 · (C) 2026 Neuracoust")
                .font(Theme.Font.mono(7)).foregroundStyle(Theme.Palette.textFaint)
            Spacer()
            Text("독립 측정과 제조사 자료를 구분해 표시합니다.")
                .font(Theme.Font.ui(7.5)).foregroundStyle(Theme.Palette.textFaint)
        }
        .padding(.horizontal, 18).frame(height: 30).background(Theme.Palette.panel)
    }

    private var cardBackground: some View {
        RoundedRectangle(cornerRadius: Theme.Radius.card).fill(Theme.Palette.surface)
            .overlay(RoundedRectangle(cornerRadius: Theme.Radius.card).stroke(Theme.Palette.divider, lineWidth: 1))
    }
    private func legend(color: Color, text: String) -> some View { HStack(spacing: 5) { Capsule().fill(color).frame(width: 18, height: 2); Text(text).font(Theme.Font.ui(8)).lineLimit(1) } }
    private func badge(_ text: String, color: Color) -> some View { Text(text.uppercased()).font(Theme.Font.mono(7, .bold)).foregroundStyle(color).padding(.horizontal, 6).padding(.vertical, 3).background(color.opacity(0.13), in: Capsule()) }
    private func confidenceColor(_ value: String) -> Color { value == "measured" ? Theme.Palette.green : value == "datasheet" ? Theme.Palette.amber : Theme.Palette.textFaint }
    private func detailRow(_ name: String, _ value: String) -> some View { HStack(alignment: .top) { Text(name).foregroundStyle(Theme.Palette.textFaint).frame(width: 62, alignment: .leading); Text(value.isEmpty ? "자료 없음" : value).foregroundStyle(Theme.Palette.textSecondary); Spacer() }.font(Theme.Font.ui(8)) }
    private func shortSource(_ value: String) -> String { (URL(string: value)?.host ?? value) + " · " + (URL(string: value)?.lastPathComponent.removingPercentEncoding ?? "") }
}

private struct SpeakerResponseChart: View {
    let first: [[Double]]?
    let second: [[Double]]?
    let showDifference: Bool
    private let minDb = -18.0, maxDb = 18.0

    var body: some View {
        Canvas { context, size in
            let plot = CGRect(x: 48, y: 12, width: max(1, size.width - 62), height: max(1, size.height - 38))
            drawGrid(context: &context, plot: plot)
            draw(first, color: Theme.Palette.accent, context: &context, plot: plot, dashed: false)
            draw(second, color: Theme.Palette.orange, context: &context, plot: plot, dashed: false)
            if showDifference, let a = first, let b = second, a.count == b.count {
                let delta = zip(a, b).map { [$0.0[0], $0.0[1] - $0.1[1]] }
                draw(delta, color: Theme.Palette.purpleLight, context: &context, plot: plot, dashed: true)
            }
        }
        .background(Theme.Palette.recess, in: RoundedRectangle(cornerRadius: Theme.Radius.display))
    }

    private func x(_ hz: Double, _ plot: CGRect) -> CGFloat { plot.minX + CGFloat(log10(max(20, min(20000, hz)) / 20) / 3) * plot.width }
    private func y(_ db: Double, _ plot: CGRect) -> CGFloat { plot.maxY - CGFloat((max(minDb, min(maxDb, db)) - minDb) / (maxDb - minDb)) * plot.height }
    private func drawGrid(context: inout GraphicsContext, plot: CGRect) {
        for db in stride(from: -18, through: 18, by: 6) {
            let yy = y(Double(db), plot); var p = Path(); p.move(to: CGPoint(x: plot.minX, y: yy)); p.addLine(to: CGPoint(x: plot.maxX, y: yy))
            context.stroke(p, with: .color(db == 0 ? Theme.Palette.coolDividerBright : Theme.Palette.divider.opacity(0.55)), lineWidth: db == 0 ? 1 : 0.5)
            context.draw(Text("\(db)").font(Theme.Font.mono(7)).foregroundColor(Theme.Palette.textFaint), at: CGPoint(x: plot.minX - 8, y: yy), anchor: .trailing)
        }
        for (hz, label) in [(20.0,"20"),(50,"50"),(100,"100"),(200,"200"),(500,"500"),(1000,"1k"),(2000,"2k"),(5000,"5k"),(10000,"10k"),(20000,"20k")] {
            let xx = x(hz, plot); var p = Path(); p.move(to: CGPoint(x: xx, y: plot.minY)); p.addLine(to: CGPoint(x: xx, y: plot.maxY))
            context.stroke(p, with: .color(Theme.Palette.divider.opacity(0.45)), lineWidth: 0.5)
            context.draw(Text(label).font(Theme.Font.mono(7)).foregroundColor(Theme.Palette.textFaint), at: CGPoint(x: xx, y: plot.maxY + 11))
        }
    }
    private func draw(_ points: [[Double]]?, color: Color, context: inout GraphicsContext, plot: CGRect, dashed: Bool) {
        guard let points, !points.isEmpty else { return }; var path = Path()
        for (index, point) in points.enumerated() where point.count >= 2 {
            let p = CGPoint(x: x(point[0], plot), y: y(point[1], plot)); index == 0 ? path.move(to: p) : path.addLine(to: p)
        }
        context.stroke(path, with: .color(color), style: StrokeStyle(lineWidth: dashed ? 1.2 : 1.8, dash: dashed ? [5,4] : []))
    }
}
