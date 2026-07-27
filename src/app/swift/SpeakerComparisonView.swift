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

// Headphone provenance metadata (measurement source, form factor), baked from the AutoEq handoff.
private struct HeadphoneMetaEntry: Decodable {
    let name: String
    let source: String
    let sourceUrl: String
    let formFactor: String
    let sourceCount: Int
    let pointCount: Int
    let status: String
    let sources: [String]
}
private enum HeadphoneMeta {
    static let byName: [String: HeadphoneMetaEntry] = {
        guard let url = Bundle.main.url(forResource: "headphone_model_meta", withExtension: "json"),
              let data = try? Data(contentsOf: url),
              let decoded = try? JSONDecoder().decode([HeadphoneMetaEntry].self, from: data) else { return [:] }
        return Dictionary(decoded.map { ($0.name, $0) }, uniquingKeysWith: { a, _ in a })
    }()
}

// Audio-interface catalog metadata (from resources/audio_interface_catalog.json) — every spec field
// for the response window's interface detail card.
private struct InterfaceMetaEntry: Decodable {
    let name, brand, family, model, era, status, host: String
    let maxSampleRateKhz, bitDepth, analogOutput: String
    let dynamicRangeDba, thdnDb, maxOutputDbu, outputImpedanceOhm: String
    let independentMeasurement, measurementSource, confidence, note: String
}
private enum InterfaceMeta {
    static let byName: [String: InterfaceMetaEntry] = {
        guard let url = Bundle.main.url(forResource: "audio_interface_catalog", withExtension: "json"),
              let data = try? Data(contentsOf: url),
              let decoded = try? JSONDecoder().decode([InterfaceMetaEntry].self, from: data) else { return [:] }
        return Dictionary(decoded.map { ($0.name, $0) }, uniquingKeysWith: { a, _ in a })
    }()
}

struct SpeakerComparisonView: View {
    @Environment(\.dismiss) private var dismiss
    @ObservedObject var engine: EngineController

    /// The overlay-able curves that describe the current monitoring chain.
    private enum Curve: String, CaseIterable, Identifiable {
        // Signal-flow order: interface (D/A) first, then the speaker.
        case outputInterface, modelingInterface, physical, simulator, appliedEq, roomTuning, finalResponse
        var id: String { rawValue }
        var label: String {
            switch self {
            case .physical: return "물리 스피커"
            case .simulator: return "모델링 스피커"
            case .outputInterface: return "실물 인터페이스"
            case .modelingInterface: return "모델링 인터페이스"
            case .appliedEq: return "적용 EQ"
            case .roomTuning: return "룸튜닝"
            case .finalResponse: return "최종 응답"
            }
        }
        var color: Color {
            switch self {
            case .physical: return Theme.Palette.accent
            case .simulator: return Theme.Palette.purpleLight
            case .outputInterface: return Theme.Palette.teal
            case .modelingInterface: return Theme.Palette.orange
            case .appliedEq: return Theme.Palette.green
            case .roomTuning: return Theme.Palette.amber
            case .finalResponse: return Theme.Palette.textBright
            }
        }
    }

    @State private var visible: Set<Curve> = [.physical, .simulator, .outputInterface, .modelingInterface, .appliedEq, .finalResponse]
    @State private var detailTab: Int = 1   // default to the physical (first) tab
    private let curveCount = 160
    private var entries: [SpeakerDatasetEntry] { SpeakerDataset.entries }

    // MARK: Monitoring context

    private var activeSet: EngineController.SpeakerSet? { engine.activeSpeakerSet }
    private var headphoneMode: Bool { engine.outputMode == .headphone }
    private var isPhysicalRoute: Bool {
        guard let out = activeSet?.output else { return false }
        return !out.isEmpty && out.caseInsensitiveCompare("None") != .orderedSame
    }
    private var simulatorName: String { activeSet.map { stripSlotPrefix($0.model) } ?? "" }
    private var simulatorIsHeadphone: Bool { engine.headphoneModelHasCurve(simulatorName) }
    private var physicalName: String {
        if headphoneMode { return stripSlotPrefix(engine.physicalHeadphoneModel) }
        // Prefer the per-slot (A/B/C) real speaker; fall back to the legacy global field when the
        // slot has none set (empty / Flat / Off), so a real speaker configured either way shows.
        let slotReal = activeSet.map { stripSlotPrefix($0.realModel) } ?? ""
        let hasSlotReal = !slotReal.isEmpty && slotReal != "Flat" && slotReal != "Off"
        return hasSlotReal ? slotReal : stripSlotPrefix(engine.physicalSpeakerModel)
    }
    private func entry(_ name: String) -> SpeakerDatasetEntry? {
        name.isEmpty ? nil : entries.first { $0.catalogName == name }
    }

    // MARK: Curve sources

    private func curvePoints(_ mags: [Double]) -> [[Double]] {
        let freqs = EngineController.monitorCurveFrequencies(count: mags.count)
        return zip(freqs, mags).map { [$0, $1] }
    }
    private var physicalPoints: [[Double]]? {
        // In headphone mode the "physical" curve is the measured headphone response; otherwise it is
        // the REAL speaker's FR — measured profile, else the spec approximation (matches the EQ).
        if headphoneMode { return smooth(engine.headphoneCurvePoints(physicalName)) }
        return smooth(engine.speakerCurvePoints(physicalName))
    }
    private var simulatorPoints: [[Double]]? {
        // The active slot may model a speaker OR headphone — even alongside a physical output route,
        // so it is shown regardless of the route. Headphone first, then the speaker FR.
        if simulatorName.isEmpty { return nil }
        if let hp = engine.headphoneCurvePoints(simulatorName) { return smooth(hp) }
        return smooth(engine.speakerCurvePoints(simulatorName))
    }
    /// Fractional-octave smoothing for display, mirroring the engine's fit-time smoothing so the
    /// raw measured dataset curves read as clean lines instead of the jagged source data.
    private func smooth(_ pts: [[Double]]?, octave: Double = 1.0 / 6.0) -> [[Double]]? {
        guard let pts, pts.count > 2 else { return pts }
        let n = 240, lo = 20.0, hi = 20000.0
        let freqs = (0..<n).map { lo * pow(hi / lo, Double($0) / Double(n - 1)) }
        let src = freqs.map { interpDb(pts, atHz: $0) }
        let binsPerOct = Double(n - 1) / log2(hi / lo)
        let sigma = max(0.5, octave * binsPerOct * 0.5)
        let half = max(1, Int(ceil(sigma * 3)))
        return (0..<n).map { k in
            var acc = 0.0, wsum = 0.0
            for d in -half...half {
                let j = min(max(k + d, 0), n - 1)
                let w = exp(-0.5 * pow(Double(d) / sigma, 2))
                acc += w * src[j]; wsum += w
            }
            return [freqs[k], acc / wsum]
        }
    }
    // The physical output-stage interface's measured D/A FR, and (if set) the modeling-target
    // interface's FR — both from the measured interface profiles.
    private var outputInterfacePoints: [[Double]]? { smooth(engine.audioInterfaceCurvePoints(engine.physicalAudioInterfaceModel)) }
    private var modelingInterfacePoints: [[Double]]? { smooth(engine.audioInterfaceCurvePoints(engine.physicalAudioInterfaceTargetModel)) }
    private var appliedEqMags: [Double] { engine.monitorEqResponse(count: curveCount) }
    private var roomTuningPoints: [[Double]]? {
        engine.roomCorrectionResponse(count: curveCount).map { curvePoints($0) }
    }
    /// What actually reaches the ear: the physical speaker's own response plus the applied EQ.
    /// Falls back to the applied EQ alone (relative to flat) when the physical curve is unknown.
    private var finalPoints: [[Double]] {
        let freqs = EngineController.monitorCurveFrequencies(count: curveCount)
        let eq = appliedEqMags
        let phys = physicalPoints
        return freqs.enumerated().map { i, f in
            let base = phys.map { interpDb($0, atHz: f) } ?? 0
            return [f, base + eq[i]]
        }
    }
    private func interpDb(_ points: [[Double]], atHz hz: Double) -> Double {
        guard let f0 = points.first, let fl = points.last else { return 0 }
        if hz <= f0[0] { return f0[1] }
        if hz >= fl[0] { return fl[1] }
        for i in 1..<points.count where points[i][0] >= hz {
            let a = points[i - 1], b = points[i]
            let denom = log10(b[0]) - log10(a[0])
            let t = denom == 0 ? 0 : (log10(hz) - log10(a[0])) / denom
            return a[1] + (b[1] - a[1]) * t
        }
        return fl[1]
    }

    private func isAvailable(_ c: Curve) -> Bool {
        switch c {
        case .physical: return physicalPoints != nil
        case .simulator: return simulatorPoints != nil
        case .outputInterface: return outputInterfacePoints != nil
        case .modelingInterface: return modelingInterfacePoints != nil
        case .roomTuning: return roomTuningPoints != nil
        case .appliedEq, .finalResponse: return true
        }
    }
    private func points(for c: Curve) -> [[Double]]? {
        switch c {
        case .physical: return physicalPoints
        case .simulator: return simulatorPoints
        case .outputInterface: return outputInterfacePoints
        case .modelingInterface: return modelingInterfacePoints
        case .appliedEq: return curvePoints(appliedEqMags)
        case .roomTuning: return roomTuningPoints
        case .finalResponse: return finalPoints
        }
    }
    private var series: [MonitorOverlayChart.Series] {
        Curve.allCases.compactMap { c in
            guard visible.contains(c), isAvailable(c), let pts = points(for: c) else { return nil }
            let width: CGFloat = c == .finalResponse ? 2.4 : (c == .appliedEq ? 2.0 : 1.5)
            return .init(points: pts, color: c.color, dashed: c == .roomTuning, width: width)
        }
    }

    // MARK: Layout

    var body: some View {
        VStack(spacing: 0) {
            header
            if entries.isEmpty {
                ContentUnavailableView("데이터셋을 불러올 수 없습니다", systemImage: "exclamationmark.triangle",
                                       description: Text("앱 번들의 speaker_model_dataset.json을 확인하세요."))
            } else {
                ScrollView {
                    VStack(spacing: 14) {
                        contextBar
                        curveToggles
                        chartCard
                        readout
                        detailCards
                        physicalChainNote
                        contextNote
                    }
                    .padding(18)
                }
            }
            footer
        }
        .frame(minWidth: 860, idealWidth: 980, minHeight: 640, idealHeight: 760)
        .background(Theme.Palette.background)
        .preferredColorScheme(.dark)
    }

    private var header: some View {
        HStack {
            Image(systemName: "waveform.path.ecg.rectangle").foregroundStyle(Theme.Palette.purpleLight)
            VStack(alignment: .leading, spacing: 2) {
                Text("모니터 EQ · 응답").font(Theme.Font.ui(16, .bold)).foregroundStyle(Theme.Palette.textBright)
                Text("출력단·모델링 인터페이스 · 물리·모델링 스피커 · 적용 EQ · 룸튜닝 · 최종 응답을 겹쳐 봅니다")
                    .font(Theme.Font.mono(8)).foregroundStyle(Theme.Palette.textFaint)
            }
            Spacer()
            Button("닫기") { dismiss() }.buttonStyle(.bordered)
        }
        .padding(.horizontal, 18).frame(height: 58).background(Theme.Gradient.monitorHeader)
    }

    private var contextBar: some View {
        HStack(spacing: 10) {
            if let set = activeSet {
                pill(set.letter, Theme.Palette.purpleLight)
                Text(isPhysicalRoute ? "물리 출력 \(set.output)"
                     : "\(simulatorIsHeadphone ? "헤드폰" : "스피커") \(simulatorName)")
                    .font(Theme.Font.mono(9)).foregroundStyle(Theme.Palette.textSecondary)
            }
            if !physicalName.isEmpty {
                Text("· 물리 \(physicalName)").font(Theme.Font.mono(9)).foregroundStyle(Theme.Palette.textFaint).lineLimit(1)
            }
            Spacer()
            Text("20 Hz — 20 kHz / dB").font(Theme.Font.mono(8)).foregroundStyle(Theme.Palette.textFaint)
        }
    }

    /// Multi-select chips: each toggles one overlay curve; unavailable ones dim out.
    private var curveToggles: some View {
        HStack(spacing: 8) {
            ForEach(Curve.allCases) { curve in
                curveChip(curve)
            }
            Spacer()
            if simulatorIsHeadphone { oeTargetControl }
            linearPhaseControl
        }
    }

    /// OE-target toggle — only meaningful for a headphone-model slot. On: reference the raw curve
    /// to the Harman OE target (its deviation from neutral). Off: the raw measured curve.
    private var oeTargetControl: some View {
        Toggle(isOn: Binding(
            get: { engine.monitorEqHeadphoneOeTarget },
            set: { engine.setMonitorEqHeadphoneOeTarget($0) }
        )) {
            Text("OE 타깃").font(Theme.Font.ui(9, .semibold))
        }
        .toggleStyle(.switch)
        .help("하만 OE 타깃 기준 — 켜면 헤드폰의 중립 대비 편차(이어게인 제거), 끄면 원시 곡선")
    }

    /// Linear-phase (FIR) vs low-latency biquad. FIR matches the target across the whole band —
    /// steep bass rolloff and treble dips the biquad fit rippled or capped — at the cost of latency.
    private var linearPhaseControl: some View {
        HStack(spacing: 8) {
            if engine.monitorEqLinearPhase, engine.monitorEqLatencyMs > 0 {
                Text(String(format: "지연 %.0f ms", engine.monitorEqLatencyMs))
                    .font(Theme.Font.mono(8)).foregroundStyle(Theme.Palette.amber)
            }
            Toggle(isOn: Binding(
                get: { engine.monitorEqLinearPhase },
                set: { engine.setMonitorEqLinearPhase($0) }
            )) {
                Text("선형위상").font(Theme.Font.ui(9, .semibold))
            }
            .toggleStyle(.switch)
            .help("FIR 선형위상 — 전대역 정확 매칭(저역 급경사·고역 딥 포함), 대신 지연 추가")
        }
    }
    /// Curve label, mode-aware: the "physical" curve is a headphone in headphone mode.
    private func label(_ curve: Curve) -> String {
        if curve == .physical && headphoneMode { return "물리 헤드폰" }
        return curve.label
    }
    private func curveChip(_ curve: Curve) -> some View {
        let available = isAvailable(curve)
        let on = visible.contains(curve) && available
        return Button {
            if visible.contains(curve) { visible.remove(curve) } else { visible.insert(curve) }
        } label: {
            HStack(spacing: 6) {
                Capsule().fill(curve.color).frame(width: 16, height: 3).opacity(on ? 1 : 0.35)
                Text(label(curve)).font(Theme.Font.ui(9, on ? .semibold : .regular))
                    .foregroundStyle(on ? Theme.Palette.textBright : Theme.Palette.textFaint)
                if !available { Text("없음").font(Theme.Font.mono(6.5)).foregroundStyle(Theme.Palette.textFaint) }
            }
            .padding(.horizontal, 10).padding(.vertical, 6)
            .background(RoundedRectangle(cornerRadius: Theme.Radius.button)
                .fill(on ? curve.color.opacity(0.14) : Theme.Palette.surface))
            .overlay(RoundedRectangle(cornerRadius: Theme.Radius.button)
                .stroke(on ? curve.color.opacity(0.5) : Theme.Palette.divider, lineWidth: 1))
        }
        .buttonStyle(.plain)
        .disabled(!available)
        .opacity(available ? 1 : 0.45)
        .help(available ? curve.label : "\(curve.label) — 데이터 없음")
    }

    private var chartCard: some View {
        VStack(alignment: .leading, spacing: 8) {
            MonitorOverlayChart(series: series).frame(height: 320)
        }
        .padding(12).background(cardBackground)
    }

    private var readout: some View {
        let eq = appliedEqMags
        let maxBoost = eq.max() ?? 0, maxCut = eq.min() ?? 0
        return HStack(spacing: 18) {
            readoutItem("적용 EQ", String(format: "+%.1f / %.1f dB", max(0, maxBoost), min(0, maxCut)), Theme.Palette.green)
            if let e = entry(simulatorName), simulatorPoints != nil {
                readoutItem("시뮬 밸런스", balanceSummary(e), Theme.Palette.purpleLight)
            }
            if let e = entry(physicalName), physicalPoints != nil {
                readoutItem("물리 밸런스", balanceSummary(e), Theme.Palette.accent)
            }
            readoutItem("룸튜닝", roomTuningPoints != nil ? "측정 적용됨" : "측정 전", Theme.Palette.amber)
            Spacer()
        }
        .font(Theme.Font.mono(8))
    }
    private func readoutItem(_ label: String, _ value: String, _ color: Color) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(label).foregroundStyle(Theme.Palette.textFaint)
            Text(value).foregroundStyle(color)
        }
    }
    private func balanceSummary(_ entry: SpeakerDatasetEntry) -> String {
        guard let pts = entry.responseCurve.points, !pts.isEmpty else { return "—" }
        let lo = pts.filter { $0[0] <= 200 }.map { $0[1] }
        let hi = pts.filter { $0[0] >= 3000 }.map { $0[1] }
        let bass = lo.isEmpty ? 0 : lo.reduce(0, +) / Double(lo.count)
        let treble = hi.isEmpty ? 0 : hi.reduce(0, +) / Double(hi.count)
        return String(format: "저 %+.1f · 고 %+.1f dB", bass, treble)
    }

    private enum DetailKind { case speaker, headphone, interface }
    private struct DetailTab: Identifiable {
        let id: Int; let label: String; let name: String; let kind: DetailKind; let color: Color
    }
    private var detailTabList: [DetailTab] {
        var t: [DetailTab] = []
        // Signal-flow order: audio interface (D/A) first, then the speaker.
        if !engine.physicalAudioInterfaceModel.isEmpty {
            t.append(.init(id: 2, label: "실물 인터페이스", name: engine.physicalAudioInterfaceModel,
                           kind: .interface, color: Curve.outputInterface.color))
        }
        if !engine.physicalAudioInterfaceTargetModel.isEmpty {
            t.append(.init(id: 3, label: "모델링 인터페이스", name: engine.physicalAudioInterfaceTargetModel,
                           kind: .interface, color: Curve.modelingInterface.color))
        }
        if physicalPoints != nil {
            t.append(.init(id: 1, label: headphoneMode ? "물리 헤드폰" : "물리 스피커", name: physicalName,
                           kind: headphoneMode ? .headphone : .speaker, color: Curve.physical.color))
        }
        if simulatorPoints != nil {
            t.append(.init(id: 0, label: "모델링 스피커", name: simulatorName,
                           kind: simulatorIsHeadphone ? .headphone : .speaker, color: Curve.simulator.color))
        }
        return t
    }

    /// A tabbed detail area — one tab per active model (output simulator / physical). Each tab
    /// shows the rich speaker card, or the headphone provenance card for a headphone model.
    private var detailCards: some View {
        let tabs = detailTabList
        return VStack(alignment: .leading, spacing: 8) {
            if !tabs.isEmpty {
                HStack(spacing: 6) {
                    ForEach(tabs) { tab in
                        Button { detailTab = tab.id } label: {
                            Text(tab.label)
                                .font(Theme.Font.ui(9.5, detailTab == tab.id ? .semibold : .regular))
                                .foregroundStyle(detailTab == tab.id ? tab.color : Theme.Palette.textFaint)
                                .padding(.horizontal, 10).padding(.vertical, 5)
                                .background(RoundedRectangle(cornerRadius: Theme.Radius.button)
                                    .fill(detailTab == tab.id ? tab.color.opacity(0.14) : Color.clear))
                                .overlay(RoundedRectangle(cornerRadius: Theme.Radius.button)
                                    .stroke(detailTab == tab.id ? tab.color.opacity(0.5) : Theme.Palette.divider, lineWidth: 1))
                        }
                        .buttonStyle(.plain)
                    }
                    Spacer()
                }
                let sel = tabs.first { $0.id == detailTab } ?? tabs[0]
                switch sel.kind {
                case .headphone: headphoneDetailCard(sel.name, role: sel.label, color: sel.color)
                case .interface: interfaceDetailCard(sel.name, role: sel.label, color: sel.color)
                case .speaker: if let e = entry(sel.name) { detailCard(e, color: sel.color, role: sel.label) }
                }
            }
        }
    }

    /// Detail card for a headphone model — no driver/enclosure specs like the speaker dataset, but
    /// its measured balance, form factor, and AutoEq provenance.
    private func headphoneDetailCard(_ name: String, role: String, color: Color) -> some View {
        let meta = HeadphoneMeta.byName[name]
        let pts = engine.headphoneCurvePoints(name)
        return VStack(alignment: .leading, spacing: 7) {
            HStack(spacing: 6) {
                Text(role).font(Theme.Font.mono(7, .bold)).foregroundStyle(color)
                    .padding(.horizontal, 6).padding(.vertical, 2).background(color.opacity(0.13), in: Capsule())
                Spacer()
            }
            Text(name).font(Theme.Font.ui(11, .bold)).foregroundStyle(color)
            HStack {
                badge("measured", color: Theme.Palette.green)
                if let ff = meta?.formFactor, !ff.isEmpty { badge(ff, color: Theme.Palette.purple) }
                if let st = meta?.status, !st.isEmpty { badge(st, color: Theme.Palette.amber) }
            }
            detailRow("형식", (meta?.formFactor).flatMap { $0.isEmpty ? nil : "\($0) 헤드폰" } ?? "헤드폰")
            detailRow("측정 밸런스", headphoneBalance(pts))
            detailRow("측정 소스", meta?.source.isEmpty == false ? meta!.source : "자료 없음")
            detailRow("소스 수", meta.map { "\($0.sourceCount)곡선 · \($0.pointCount)점" } ?? "자료 없음")
            detailRow("상태", meta?.status.isEmpty == false ? meta!.status : "자료 없음")
            // Hardware specs (impedance / sensitivity / driver / weight) are not in the AutoEq
            // frequency-response handoff, so they read as "자료 없음" until a spec source is added.
            detailRow("드라이버", "자료 없음")
            detailRow("임피던스", "자료 없음")
            detailRow("감도", "자료 없음")
            detailRow("무게", "자료 없음")
            Divider().overlay(Theme.Palette.divider)
            if let sources = meta?.sources, !sources.isEmpty {
                Text("측정 출처 (\(sources.count)): \(sources.joined(separator: ", "))")
                    .font(Theme.Font.ui(8)).foregroundStyle(Theme.Palette.textFaint)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Text("원시 커플러 측정 — 이어게인이 포함됩니다. ‘OE 타깃’을 켜면 하만 OE 기준의 중립 대비 편차로 봅니다.")
                .font(Theme.Font.ui(8.5)).foregroundStyle(Theme.Palette.textSecondary)
                .fixedSize(horizontal: false, vertical: true)
            if let u = meta?.sourceUrl, let url = URL(string: u) {
                Link(destination: url) {
                    Label("AutoEq 측정 원본", systemImage: "arrow.up.right.square").font(Theme.Font.mono(7.5)).lineLimit(1)
                }
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading).padding(12).background(cardBackground)
    }

    /// Detail card for an audio-interface D/A model — every catalog spec, plus the measured FR
    /// balance and modeling status when a real profile exists.
    private func interfaceDetailCard(_ name: String, role: String, color: Color) -> some View {
        let m = InterfaceMeta.byName[name]
        let hasProfile = engine.audioInterfaceHasProfile(name)
        let pts = engine.audioInterfaceCurvePoints(name)
        func val(_ s: String?, _ unit: String = "") -> String {
            guard let s, !s.isEmpty else { return "자료 없음" }
            return unit.isEmpty ? s : "\(s) \(unit)"
        }
        return VStack(alignment: .leading, spacing: 7) {
            HStack(spacing: 6) {
                Text(role).font(Theme.Font.mono(7, .bold)).foregroundStyle(color)
                    .padding(.horizontal, 6).padding(.vertical, 2).background(color.opacity(0.13), in: Capsule())
                Spacer()
            }
            Text(name).font(Theme.Font.ui(11, .bold)).foregroundStyle(color)
            let liveMeasured = engine.interfaceMeasuredHasProfile(name)
            HStack(spacing: 5) {
                if liveMeasured { badge("실측(내 장치)", color: Theme.Palette.teal) }
                if hasProfile { badge("modeled", color: Theme.Palette.green) }
                if m?.independentMeasurement == "available" { badge("성적서", color: Theme.Palette.purpleLight) }
                if let st = m?.status, !st.isEmpty { badge(st, color: Theme.Palette.amber) }
                if let c = m?.confidence, !c.isEmpty { badge("신뢰 \(c)", color: Theme.Palette.purple) }
            }
            // Two columns: catalog spec (left) vs measured results + graphs (right, using the whitespace).
            HStack(alignment: .top, spacing: 20) {
                VStack(alignment: .leading, spacing: 0) {
                    Text("카탈로그 사양").font(Theme.Font.mono(7.5, .bold)).foregroundStyle(Theme.Palette.textFaint)
                        .padding(.bottom, 3)
                    detailRow("브랜드/계열", { let s = [m?.brand, m?.family].compactMap { $0?.isEmpty == false ? $0 : nil }.joined(separator: " · "); return s.isEmpty ? "자료 없음" : s }())
                    detailRow("모델/세대", val(m?.model))
                    detailRow("시대", val(m?.era))
                    detailRow("연결", val(m?.host))
                    detailRow("샘플레이트", val(m?.maxSampleRateKhz, "kHz"))
                    detailRow("비트뎁스", val(m?.bitDepth, "bit"))
                    detailRow("아날로그 출력", val(m?.analogOutput))
                    detailRow("다이나믹레인지", val(m?.dynamicRangeDba, "dBA"))
                    detailRow("THD+N", val(m?.thdnDb, "dB"))
                    detailRow("최대 출력", val(m?.maxOutputDbu, "dBu"))
                    detailRow("출력 임피던스", val(m?.outputImpedanceOhm, "Ω"))
                    detailRow("측정 출처", val(m?.measurementSource))
                }
                .frame(width: 300, alignment: .leading)
                if liveMeasured {
                    interfaceMeasuredColumn(name).frame(maxWidth: .infinity, alignment: .leading)
                } else {
                    Text("‘자동 측정’을 실행하면 여기에 당신의 실물 장치 실측 결과와 그래프(THD·주파수응답·고조파·THD-vs-레벨)가 표시됩니다.")
                        .font(Theme.Font.ui(8.5)).foregroundStyle(Theme.Palette.textFaint)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            Divider().overlay(Theme.Palette.divider)
            if let note = m?.note, !note.isEmpty {
                Text("노트: \(note)").font(Theme.Font.ui(8.5)).foregroundStyle(Theme.Palette.textSecondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Text(hasProfile
                 ? "실측 D/A 주파수 응답 보유 — ‘실물’로 선택하면 이 편차를 보상(평탄화)합니다."
                 : "실측 프로파일 없음 — 카탈로그/스펙 표시만, 오디오는 변경하지 않습니다.")
                .font(Theme.Font.ui(8.5)).foregroundStyle(hasProfile ? Theme.Palette.green : Theme.Palette.textFaint)
                .fixedSize(horizontal: false, vertical: true)
            // Live loopback measurement + input meter, shown for the current output interface.
            if name == engine.physicalAudioInterfaceModel {
                Divider().overlay(Theme.Palette.divider)
                interfaceLoopbackControls
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading).padding(12).background(cardBackground)
    }

    private var measureInputDeviceName: String {
        engine.inputDevices.first { $0.id == engine.currentInputDeviceId }?.name ?? "시스템 기본"
    }

    // The measured-results column: values (measured vs catalog) + graphs, filling the card's whitespace.
    private func interfaceMeasuredColumn(_ name: String) -> some View {
        let thd = engine.interfaceMeasuredThd(name)
        let harm = engine.interfaceMeasuredHarmonics(name)
        let curve = engine.interfaceMeasuredCurve(name)
        let isOutput = name == engine.physicalAudioInterfaceModel
        let thdLevels = isOutput ? engine.interfaceMeasuredThdVsLevel() : []
        let m = InterfaceMeta.byName[name]
        return VStack(alignment: .leading, spacing: 6) {
            Text("실측 결과 · 내 장치").font(Theme.Font.mono(8, .bold)).foregroundStyle(Theme.Palette.teal)
            measuredRow("THD (실측)", String(format: "%.3f %%", thd), Theme.Palette.green)
            measuredRow("카탈로그 THD+N", (m?.thdnDb).flatMap { $0.isEmpty ? nil : "\($0) dB" } ?? "자료 없음", Theme.Palette.textFaint)
            measuredRow("측정 FR 밸런스", interfaceFrBalance(curve), Theme.Palette.textSecondary)
            if harm.contains(where: { $0 > 0 }) {
                Text("고조파 H2–H7 (dBc)").font(Theme.Font.mono(7.5)).foregroundStyle(Theme.Palette.textFaint).padding(.top, 2)
                harmonicsBars(harm)
            }
            if !curve.isEmpty {
                Text("측정 D/A 주파수 응답 (±6 dB)").font(Theme.Font.mono(7.5)).foregroundStyle(Theme.Palette.textFaint).padding(.top, 2)
                measuredFrMini(curve)
            }
            if !thdLevels.isEmpty {
                Text("THD vs 레벨 (소자 반응)").font(Theme.Font.mono(7.5)).foregroundStyle(Theme.Palette.textFaint).padding(.top, 2)
                thdLevelMini(thdLevels)
            }
            Text("오프라인 성적서가 아니라 당신의 실물 장치를 ESS 스위프로 측정한 값입니다. 제조사 사양과 다를 수 있고, 다르면 이 실측이 오디오에 우선 적용됩니다.")
                .font(Theme.Font.ui(8)).foregroundStyle(Theme.Palette.textSecondary)
                .fixedSize(horizontal: false, vertical: true).padding(.top, 2)
        }
    }

    private func measuredRow(_ label: String, _ value: String, _ color: Color) -> some View {
        HStack {
            Text(label).font(Theme.Font.ui(8.5)).foregroundStyle(Theme.Palette.textFaint)
            Spacer(minLength: 8)
            Text(value).font(Theme.Font.mono(8.5)).foregroundStyle(color)
        }
    }

    private func interfaceFrBalance(_ curve: [Double]) -> String {
        guard curve.count >= 9 else { return "—" }
        let n = curve.count
        func avg(_ r: Range<Int>) -> Double { let s = Array(curve[r]); return s.isEmpty ? 0 : s.reduce(0, +) / Double(s.count) }
        return String(format: "저 %+.1f · 중 %+.1f · 고 %+.1f dB", avg(0..<(n/3)), avg((n/3)..<(2*n/3)), avg((2*n/3)..<n))
    }

    private func harmonicsBars(_ harm: [Double]) -> some View {
        let dbc = harm.map { $0 > 1e-9 ? 20.0 * log10($0) : -120.0 }   // linear → dBc
        return HStack(alignment: .bottom, spacing: 6) {
            ForEach(dbc.indices, id: \.self) { i in
                let frac = max(0.02, min(1.0, (dbc[i] + 120.0) / 100.0))   // −120..−20 dBc → 0..1
                VStack(spacing: 2) {
                    Text(String(format: "%.0f", dbc[i])).font(Theme.Font.mono(6)).foregroundStyle(Theme.Palette.textFaint)
                    RoundedRectangle(cornerRadius: 2).fill(Theme.Palette.teal).frame(width: 14, height: CGFloat(frac) * 30)
                    Text("H\(i + 2)").font(Theme.Font.mono(6.5)).foregroundStyle(Theme.Palette.textFaint)
                }
            }
            Spacer(minLength: 0)
        }.frame(height: 50)
    }

    private func measuredFrMini(_ curve: [Double]) -> some View {
        let yr = 6.0
        return GeometryReader { geo in
            let w = geo.size.width, h = geo.size.height
            ZStack {
                Path { p in p.move(to: CGPoint(x: 0, y: h / 2)); p.addLine(to: CGPoint(x: w, y: h / 2)) }
                    .stroke(Theme.Palette.textFaint.opacity(0.4), style: StrokeStyle(lineWidth: 0.5, dash: [2, 2]))
                Path { p in
                    for (i, db) in curve.enumerated() {
                        let x = curve.count > 1 ? CGFloat(i) / CGFloat(curve.count - 1) * w : 0
                        let y = h / 2 - CGFloat(max(-yr, min(yr, db)) / yr) * (h / 2)
                        if i == 0 { p.move(to: CGPoint(x: x, y: y)) } else { p.addLine(to: CGPoint(x: x, y: y)) }
                    }
                }.stroke(Theme.Palette.teal, lineWidth: 1.5)
            }
        }
        .frame(height: 42)
        .background(RoundedRectangle(cornerRadius: 4).fill(Theme.Palette.recess))
    }

    private func thdLevelMini(_ pts: [(dbfs: Double, thd: Double)]) -> some View {
        let sorted = pts.sorted { $0.dbfs < $1.dbfs }
        let xMin = -30.0, xMax = 0.0, yLo = log10(0.001), yHi = log10(10.0)
        func nx(_ d: Double) -> Double { max(0, min(1, (d - xMin) / (xMax - xMin))) }
        func ny(_ t: Double) -> Double { max(0, min(1, (log10(max(0.0001, t)) - yLo) / (yHi - yLo))) }
        return GeometryReader { geo in
            let w = geo.size.width, h = geo.size.height
            ZStack {
                Path { p in
                    for (i, pt) in sorted.enumerated() {
                        let x = nx(pt.dbfs) * w, y = (1 - ny(pt.thd)) * h
                        if i == 0 { p.move(to: CGPoint(x: x, y: y)) } else { p.addLine(to: CGPoint(x: x, y: y)) }
                    }
                }.stroke(Theme.Palette.purpleLight, lineWidth: 1.5)
                ForEach(sorted.indices, id: \.self) { i in
                    Circle().fill(Theme.Palette.purpleLight).frame(width: 4, height: 4)
                        .position(x: nx(sorted[i].dbfs) * w, y: (1 - ny(sorted[i].thd)) * h)
                }
            }
        }
        .frame(height: 42)
        .background(RoundedRectangle(cornerRadius: 4).fill(Theme.Palette.recess))
    }

    // Loopback measurement controls with a live input-level meter for gain setup (avoid ADC clip).
    @ViewBuilder private var interfaceLoopbackControls: some View {
        let name = engine.physicalAudioInterfaceModel
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 6) {
                Image(systemName: "cable.connector").font(.system(size: 10)).foregroundStyle(Theme.Palette.teal)
                Text("루프백 측정").font(Theme.Font.ui(10, .bold)).foregroundStyle(Theme.Palette.teal)
                Spacer()
                if engine.interfaceMeasuredHasProfile(name) {
                    Text(String(format: "실측 THD %.3f %%", engine.interfaceMeasuredThd(name)))
                        .font(Theme.Font.mono(7.5)).foregroundStyle(Theme.Palette.green)
                    Button("측정 지우기") { engine.clearInterfaceMeasurement(name) }
                        .buttonStyle(.plain).font(Theme.Font.mono(7.5)).foregroundStyle(Theme.Palette.textFaint)
                }
            }
            HStack(spacing: 10) {
                Menu("입력 장치: \(measureInputDeviceName)") {
                    ForEach(engine.inputDevices) { dev in
                        Button {
                            engine.setTalkbackMic(dev.id)
                        } label: {
                            if dev.id == engine.currentInputDeviceId { Label(dev.name, systemImage: "checkmark") } else { Text(dev.name) }
                        }
                    }
                }.font(Theme.Font.ui(8.5)).onAppear { engine.refreshInputDevices() }
                Menu("출력 \(engine.measureOutputChannel)") {
                    ForEach(1...max(2, engine.measureOutputChannelCount), id: \.self) { c in
                        Button { engine.setMeasureOutputChannel(c) } label: {
                            if c == engine.measureOutputChannel { Label("출력 \(c)", systemImage: "checkmark") } else { Text("출력 \(c)") }
                        }
                    }
                }.font(Theme.Font.ui(8.5)).fixedSize()
                Menu("입력 \(engine.measureInputChannel)") {
                    ForEach(1...max(1, engine.measureInputChannelCount), id: \.self) { c in
                        Button { engine.setMeasureInputChannel(c) } label: {
                            if c == engine.measureInputChannel { Label("입력 \(c)", systemImage: "checkmark") } else { Text("입력 \(c)") }
                        }
                    }
                }.font(Theme.Font.ui(8.5)).fixedSize()
                Spacer()
                Button(engine.measurementLevelCheckActive ? "레벨 정지" : "레벨 확인") {
                    engine.setMeasurementLevelCheck(!engine.measurementLevelCheckActive)
                }.buttonStyle(.plain).font(Theme.Font.ui(9, .medium))
                 .foregroundStyle(engine.measurementLevelCheckActive ? Theme.Palette.orange : Theme.Palette.accent)
                if !engine.measurementActive && !engine.multiLevelActive {
                    Button("측정 시작") { engine.startInterfaceMeasurement() }
                        .buttonStyle(.plain).font(Theme.Font.ui(9, .medium)).foregroundStyle(Theme.Palette.teal)
                    Button("자동 측정") { engine.startMultiLevelMeasurement() }
                        .buttonStyle(.plain).font(Theme.Font.ui(9, .medium)).foregroundStyle(Theme.Palette.purpleLight)
                        .help("케이블만 연결하면 나머지는 자동: 루프백 게인을 보정해 여러 레벨(−1~−25 dBFS)에서 자동으로 스위프해 THD-vs-레벨 곡선(소자 반응)을 그립니다.")
                }
            }
            if engine.multiLevelActive {
                HStack(spacing: 6) {
                    ProgressView().controlSize(.small)
                    Text(engine.multiLevelTotal == 0 ? "루프백 게인 보정 중…"
                                                      : "자동 측정 · 레벨 \(min(engine.multiLevelStep + 1, engine.multiLevelTotal))/\(engine.multiLevelTotal)")
                        .font(Theme.Font.ui(8.5)).foregroundStyle(Theme.Palette.purpleLight)
                }
            } else if engine.measurementActive {
                HStack(spacing: 6) {
                    ProgressView(value: engine.measurementProgress).frame(width: 90).controlSize(.small)
                    Text("스윕 재생 중…").font(Theme.Font.ui(8)).foregroundStyle(Theme.Palette.textFaint)
                    Button("취소") { engine.cancelMeasurement() }.buttonStyle(.plain).font(Theme.Font.ui(8.5)).foregroundStyle(Theme.Palette.orange)
                }
            }
            if !engine.multiLevelResults.isEmpty {
                thdVsLevelCurve
            }
            if engine.measurementPendingValid {
                pendingReviewRow
            }
            if engine.measurementLevelCheckActive || engine.measuringInterface {
                interfaceLevelMeter
            } else if !engine.measurementPendingValid {
                Text("‘레벨 확인’을 켜면 스위프와 똑같은 레벨의 1 kHz 기준 톤이 출력 채널로 나갑니다. 미터가 ‘적정(−6~−12)’이 되게 입력 게인을 맞춘 뒤 측정하면 스위프도 클립하지 않습니다. DAC 출력→ADC 입력 케이블 연결 필요.")
                    .font(Theme.Font.ui(8)).foregroundStyle(Theme.Palette.textFaint)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }

    private var interfaceLevelMeter: some View {
        let peak = Double(engine.measurementInputLevel)
        let db = peak > 1e-6 ? 20.0 * log10(peak) : -120.0
        let norm = max(0.0, min(1.0, (db + 60.0) / 60.0))
        let (verdict, color): (String, Color) = {
            if peak >= 0.999 { return ("클립! 입력 게인 낮추세요", Theme.Palette.red) }
            if db > -3.0 { return ("너무 큼", Theme.Palette.orange) }
            if db < -30.0 { return ("너무 낮음", Theme.Palette.textFaint) }
            return ("적정 (−6~−12 목표)", Theme.Palette.green)
        }()
        return HStack(spacing: 6) {
            GeometryReader { geo in
                ZStack(alignment: .leading) {
                    Capsule().fill(Theme.Palette.recess)
                    Capsule().fill(color).frame(width: geo.size.width * norm)
                    Rectangle().fill(Theme.Palette.textFaint).frame(width: 1)
                        .offset(x: geo.size.width * ((-6.0 + 60.0) / 60.0))
                }
            }.frame(height: 9)
            Text(db > -119.0 ? String(format: "%.1f dB", db) : "−∞")
                .font(Theme.Font.mono(8)).foregroundStyle(color).frame(width: 54, alignment: .trailing)
            Text(verdict).font(Theme.Font.mono(7.5)).foregroundStyle(color).frame(width: 120, alignment: .leading)
        }
    }

    // THD-vs-level curve — the device's level dependence (소자 반응). X = return dBFS, Y = THD% (log).
    private var thdVsLevelCurve: some View {
        let pts = engine.multiLevelResults.sorted { $0.dbfs < $1.dbfs }
        let xMin = -30.0, xMax = 0.0
        let yLo = log10(0.001), yHi = log10(10.0)      // THD% log range
        func nx(_ d: Double) -> Double { max(0, min(1, (d - xMin) / (xMax - xMin))) }
        func ny(_ thd: Double) -> Double { max(0, min(1, (log10(max(0.0001, thd)) - yLo) / (yHi - yLo))) }
        let worst = pts.map(\.thd).max() ?? 0
        return VStack(alignment: .leading, spacing: 2) {
            HStack {
                Text("THD vs 레벨 (소자 반응)").font(Theme.Font.mono(7.5)).foregroundStyle(Theme.Palette.purpleLight)
                Spacer()
                Text(String(format: "최대 %.3f%% · %d단계", worst, pts.count))
                    .font(Theme.Font.mono(7)).foregroundStyle(Theme.Palette.textFaint)
            }
            GeometryReader { geo in
                let w = geo.size.width, h = geo.size.height
                ZStack {
                    Path { p in
                        for (i, pt) in pts.enumerated() {
                            let x = nx(pt.dbfs) * w, y = (1 - ny(pt.thd)) * h
                            if i == 0 { p.move(to: CGPoint(x: x, y: y)) } else { p.addLine(to: CGPoint(x: x, y: y)) }
                        }
                    }.stroke(Theme.Palette.purpleLight, lineWidth: 1.5)
                    ForEach(pts.indices, id: \.self) { i in
                        Circle().fill(Theme.Palette.purpleLight).frame(width: 4, height: 4)
                            .position(x: nx(pts[i].dbfs) * w, y: (1 - ny(pts[i].thd)) * h)
                    }
                }
            }
            .frame(height: 52)
            .background(RoundedRectangle(cornerRadius: 4).fill(Theme.Palette.recess))
            HStack {
                Text("−30 dBFS").font(Theme.Font.mono(6.5)).foregroundStyle(Theme.Palette.textFaint)
                Spacer()
                Text("Y: THD 0.001–10% (log)").font(Theme.Font.mono(6.5)).foregroundStyle(Theme.Palette.textFaint)
                Spacer()
                Text("0 dBFS").font(Theme.Font.mono(6.5)).foregroundStyle(Theme.Palette.textFaint)
            }
        }
    }

    // Post-measurement quality verdict + explicit save/discard, so a clipped or too-low capture
    // is never silently written over a good profile.
    private var pendingReviewRow: some View {
        let peak = Double(engine.measurementPendingPeak)
        let db = peak > 1e-6 ? 20.0 * log10(peak) : -120.0
        let thd = engine.measurementPendingThd
        let (ok, verdict, color): (Bool, String, Color) = {
            if peak >= 0.99 { return (false, "클리핑됨 — 입력 게인 낮추고 다시 측정", Theme.Palette.red) }
            if db < -40.0 { return (false, "레벨 너무 낮음 — 게인 올리고 다시", Theme.Palette.orange) }
            if thd > 5.0 { return (false, "THD 비정상(클리핑/노이즈 의심) — 재측정 권장", Theme.Palette.orange) }
            return (true, String(format: "양호 · THD %.3f%% · 피크 %.1f dB", thd, db), Theme.Palette.green)
        }()
        return VStack(alignment: .leading, spacing: 4) {
            HStack(spacing: 6) {
                Image(systemName: ok ? "checkmark.seal.fill" : "exclamationmark.triangle.fill")
                    .font(.system(size: 10)).foregroundStyle(color)
                Text(verdict).font(Theme.Font.mono(8)).foregroundStyle(color)
                    .fixedSize(horizontal: false, vertical: true)
                Spacer(minLength: 0)
            }
            HStack(spacing: 8) {
                Text("이 측정을 저장할까요?").font(Theme.Font.ui(8.5)).foregroundStyle(Theme.Palette.textSecondary)
                Spacer(minLength: 0)
                Button(ok ? "저장" : "그래도 저장") { engine.commitInterfaceMeasurement() }
                    .buttonStyle(.plain).font(Theme.Font.ui(9, .medium))
                    .foregroundStyle(ok ? Theme.Palette.green : Theme.Palette.orange)
                Button("버리기") { engine.discardInterfaceMeasurement() }
                    .buttonStyle(.plain).font(Theme.Font.ui(9, .medium)).foregroundStyle(Theme.Palette.textFaint)
            }
        }
        .padding(8)
        .background(RoundedRectangle(cornerRadius: Theme.Radius.button).fill(color.opacity(0.08)))
        .overlay(RoundedRectangle(cornerRadius: Theme.Radius.button).stroke(color.opacity(0.4), lineWidth: 1))
    }

    private func headphoneBalance(_ pts: [[Double]]?) -> String {
        guard let pts, !pts.isEmpty else { return "—" }
        let band: (ClosedRange<Double>) -> Double = { r in
            let v = pts.filter { r.contains($0[0]) }.map { $0[1] }
            return v.isEmpty ? 0 : v.reduce(0, +) / Double(v.count)
        }
        return String(format: "저 %+.1f · 중 %+.1f · 고 %+.1f dB", band(20...200), band(200...2000), band(3000...20000))
    }

    // The full physical output chain (interface → amp → cable → speaker) with per-stage status, so
    // "I set the amp/cable but see nothing" is answered honestly: what's applied vs definition-only.
    @ViewBuilder private var physicalChainNote: some View {
        let iface = engine.physicalAudioInterfaceModel
        let spk = engine.physicalSpeakerModel
        if !iface.isEmpty || !spk.isEmpty || !engine.physicalPowerAmpModel.isEmpty ||
            !engine.physicalPowerCableModel.isEmpty || !engine.physicalConnectorModel.isEmpty {
            VStack(alignment: .leading, spacing: 4) {
                Text("실물 출력 체인").font(Theme.Font.mono(8, .bold)).foregroundStyle(Theme.Palette.textSecondary)
                chainRow("① 인터페이스 D/A", iface,
                         engine.interfaceMeasuredHasProfile(iface) ? ("실측 반영", Theme.Palette.green)
                               : (engine.audioInterfaceHasProfile(iface) ? ("성적서 반영", Theme.Palette.teal)
                                  : ("정의만 · 오디오 미반영 (측정 대기)", Theme.Palette.textFaint)))
                chainRow("AC 전원 케이블", engine.physicalPowerCableModel,
                         engine.physicalPowerCableModel.isEmpty ? ("미지정", Theme.Palette.textFaint)
                            : ("스펙만 · 톤 모델 없음", Theme.Palette.teal))
                catalogSpecLine("power_cable_catalog", engine.physicalPowerCableModel,
                                keys: [("gauge_awg", "AWG"), ("connector_type", "")])
                chainRow("커넥터", engine.physicalConnectorModel,
                         engine.physicalConnectorModel.isEmpty ? ("미지정", Theme.Palette.textFaint)
                            : ("스펙만 · 정상 접점은 평탄", Theme.Palette.teal))
                catalogSpecLine("connector_catalog", engine.physicalConnectorModel,
                                keys: [("connector_type", "")])
                if engine.physicalSpeakerIsPassive {
                    chainRow("② 파워앰프", engine.physicalPowerAmpModel,
                             engine.physicalPowerAmpModel.isEmpty ? ("미지정", Theme.Palette.textFaint)
                                : (engine.powerAmpToneActive ? ("휴리스틱 톤 반영 (측정 시 실측)", Theme.Palette.amber)
                                   : ("휴리스틱 ≈ 평탄 (측정 대기)", Theme.Palette.textFaint)))
                    catalogSpecLine("power_amp_catalog", engine.physicalPowerAmpModel,
                                    keys: [("power_w_8ohm", "W/8Ω"), ("damping_factor", "DF"), ("thd_percent", "% THD")])
                    chainRow("③ 스피커 케이블", engine.physicalSpeakerCableModel,
                             engine.physicalSpeakerCableModel.isEmpty ? ("미지정", Theme.Palette.textFaint)
                                : (engine.speakerCableToneActive ? ("휴리스틱 톤 반영 (측정 시 실측)", Theme.Palette.amber)
                                   : ("휴리스틱 ≈ 평탄 (측정 대기)", Theme.Palette.textFaint)))
                    catalogSpecLine("speaker_cable_catalog", engine.physicalSpeakerCableModel,
                                    keys: [("gauge_awg", "AWG"), ("resistance_ohm_per_m", "Ω/m"), ("inductance_uh_per_m", "µH/m")])
                }
                chainRow("④ 스피커", spk,
                         spk.isEmpty ? ("미지정", Theme.Palette.textFaint)
                            : (engine.virtualMonitorTargets.contains(spk) ? ("실측 곡선 반영", Theme.Palette.green)
                               : ("이름 휴리스틱 반영", Theme.Palette.amber)))
                Text("파워앰프·스피커 케이블은 패시브 스피커에서 공개 스펙 기반의 작은 레벨 매칭 곡선만 반영합니다. AC 전원 케이블과 정상 커넥터는 가청 톤을 만들지 않으므로 의도적으로 오디오에 반영하지 않습니다. 독립 원시 측정이 추가되기 전에는 어떤 항목도 실측 모델로 표시하지 않습니다.")
                    .font(Theme.Font.ui(8)).foregroundStyle(Theme.Palette.textFaint).fixedSize(horizontal: false, vertical: true)
            }
            .frame(maxWidth: .infinity, alignment: .leading).padding(10)
            .background(RoundedRectangle(cornerRadius: Theme.Radius.button).fill(Theme.Palette.recess.opacity(0.6)))
        }
    }

    private func chainRow(_ stage: String, _ model: String, _ status: (String, Color)) -> some View {
        HStack(spacing: 8) {
            Text(stage).font(Theme.Font.ui(8.5)).foregroundStyle(Theme.Palette.textFaint).frame(width: 96, alignment: .leading)
            Text(model.isEmpty ? "미지정" : model)
                .font(Theme.Font.mono(8)).foregroundStyle(model.isEmpty ? Theme.Palette.textFaint : Theme.Palette.textSecondary)
                .lineLimit(1)
            Spacer(minLength: 8)
            Text(status.0).font(Theme.Font.mono(7.5)).foregroundStyle(status.1)
        }
    }

    @ViewBuilder private func catalogSpecLine(_ resource: String, _ name: String,
                                              keys: [(String, String)]) -> some View {
        if let summary = Self.catalogSpecSummary(resource, name, keys: keys), !summary.isEmpty {
            Text(summary).font(Theme.Font.mono(7.5)).foregroundStyle(Theme.Palette.textFaint)
                .padding(.leading, 104).fixedSize(horizontal: false, vertical: true)
        }
    }

    private static func catalogSpecSummary(_ resource: String, _ name: String,
                                           keys: [(String, String)]) -> String? {
        guard !name.isEmpty,
              let url = Bundle.main.url(forResource: resource, withExtension: "json"),
              let data = try? Data(contentsOf: url),
              let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let entries = root["entries"] as? [[String: Any]],
              let entry = entries.first(where: { $0["name"] as? String == name }) else { return nil }
        var parts: [String] = []
        for (key, unit) in keys {
            guard let value = entry[key], !(value is NSNull) else { continue }
            let text: String
            if let n = value as? NSNumber { text = String(format: "%g", n.doubleValue) }
            else if let s = value as? String { text = s }
            else { continue }
            parts.append(unit.isEmpty ? text : "\(text) \(unit)")
        }
        if let confidence = entry["confidence"] as? String { parts.append("신뢰도 \(confidence)") }
        return parts.joined(separator: " · ")
    }

    @ViewBuilder private var contextNote: some View {
        if headphoneMode && physicalPoints == nil {
            note("물리 헤드폰 미지정 — 헤드폰 탭(우클릭) → ‘실물 헤드폰 모델’에서 착용 중인 헤드폰을 고르면 그 곡선이 여기 표시되고 보정에 반영됩니다.",
                 "headphones", Theme.Palette.amber)
        }
        if isPhysicalRoute {
            note("물리 출력 라우트 — 모델러 없음. 룸튜닝(측정한 경우)만 적용됩니다.",
                 "cable.connector", Theme.Palette.textFaint)
        } else if simulatorPoints != nil {
            note(simulatorIsHeadphone
                 ? "이 슬롯은 헤드폰 모델러 — 원시 커플러 곡선(이어게인 포함)이라 톤 색을 입힙니다(중립 보정 아님)."
                 : "이 슬롯은 스피커 시뮬레이터 — 모델의 실측 곡선을 적용합니다.",
                 simulatorIsHeadphone ? "headphones" : "hifispeaker", Theme.Palette.textFaint)
        } else if !simulatorName.isEmpty {
            note("‘\(simulatorName)’ 실측 곡선이 없어 이름 휴리스틱 톤을 씁니다. EQ에는 곡선이 실리지 않습니다.",
                 "info.circle", Theme.Palette.amber)
        }
    }
    private func note(_ text: String, _ symbol: String, _ color: Color) -> some View {
        Label(text, systemImage: symbol).font(Theme.Font.ui(8.5)).foregroundStyle(color)
            .frame(maxWidth: .infinity, alignment: .leading)
    }

    private func detailCard(_ item: SpeakerDatasetEntry, color: Color, role: String) -> some View {
        VStack(alignment: .leading, spacing: 7) {
            HStack(spacing: 6) {
                Text(role).font(Theme.Font.mono(7, .bold)).foregroundStyle(color)
                    .padding(.horizontal, 6).padding(.vertical, 2).background(color.opacity(0.13), in: Capsule())
                Spacer()
            }
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
    private func pill(_ text: String, _ color: Color) -> some View { Text(text).font(Theme.Font.mono(9, .bold)).foregroundStyle(color).padding(.horizontal, 7).padding(.vertical, 3).background(color.opacity(0.14), in: Capsule()) }
    private func badge(_ text: String, color: Color) -> some View { Text(text.uppercased()).font(Theme.Font.mono(7, .bold)).foregroundStyle(color).padding(.horizontal, 6).padding(.vertical, 3).background(color.opacity(0.13), in: Capsule()) }
    private func confidenceColor(_ value: String) -> Color { value == "measured" ? Theme.Palette.green : value == "datasheet" ? Theme.Palette.amber : Theme.Palette.textFaint }
    private func detailRow(_ name: String, _ value: String) -> some View { HStack(alignment: .top) { Text(name).foregroundStyle(Theme.Palette.textFaint).frame(width: 62, alignment: .leading); Text(value.isEmpty ? "자료 없음" : value).foregroundStyle(Theme.Palette.textSecondary); Spacer() }.font(Theme.Font.ui(8)) }
}

// A general dB-vs-log-Hz overlay of several labelled curves, sharing the reference chart's axes.
private struct MonitorOverlayChart: View {
    struct Series { let points: [[Double]]; let color: Color; let dashed: Bool; let width: CGFloat }
    let series: [Series]
    private let minDb = -18.0, maxDb = 18.0

    var body: some View {
        Canvas { context, size in
            let plot = CGRect(x: 48, y: 12, width: max(1, size.width - 62), height: max(1, size.height - 38))
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
            for s in series {
                guard !s.points.isEmpty else { continue }
                var path = Path()
                for (index, point) in s.points.enumerated() where point.count >= 2 {
                    let p = CGPoint(x: x(point[0], plot), y: y(point[1], plot)); index == 0 ? path.move(to: p) : path.addLine(to: p)
                }
                context.stroke(path, with: .color(s.color), style: StrokeStyle(lineWidth: s.width, dash: s.dashed ? [5,4] : []))
            }
        }
        .background(Theme.Palette.recess, in: RoundedRectangle(cornerRadius: Theme.Radius.display))
    }
    private func x(_ hz: Double, _ plot: CGRect) -> CGFloat { plot.minX + CGFloat(log10(max(20, min(20000, hz)) / 20) / 3) * plot.width }
    private func y(_ db: Double, _ plot: CGRect) -> CGFloat { plot.maxY - CGFloat((max(minDb, min(maxDb, db)) - minDb) / (maxDb - minDb)) * plot.height }
}
