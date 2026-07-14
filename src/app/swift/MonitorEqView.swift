import SwiftUI

/// The monitor parametric EQ panel — a magnitude curve over the 0–64 bands the user adds on
/// demand, with per-band controls. Monitor path only (it never touches the printed mix).
struct MonitorEqView: View {
    @EnvironmentObject private var engine: EngineController

    private let minHz = 20.0, maxHz = 20000.0
    private let dbRange = 18.0

    var body: some View {
        VStack(spacing: 0) {
            header
            Divider().overlay(Theme.Palette.divider)
            curve.frame(height: 150).padding(10)
            Divider().overlay(Theme.Palette.divider)
            bandList
        }
        .frame(width: 460, height: 560)
        .background(Theme.Palette.panel)
        .clipShape(RoundedRectangle(cornerRadius: 12))
        .overlay(RoundedRectangle(cornerRadius: 12).stroke(Theme.Palette.divider, lineWidth: 1))
        .shadow(color: .black.opacity(0.5), radius: 20, y: 10)
        .padding(24)
    }

    private var header: some View {
        HStack(spacing: 8) {
            Image(systemName: "slider.horizontal.3").foregroundStyle(Theme.Palette.accent)
            Text("모니터 EQ").font(Theme.Font.ui(13, .semibold)).foregroundStyle(Theme.Palette.textBright)
            Text("\(engine.monitorEqBands.count)/64").font(Theme.Font.mono(9)).foregroundStyle(Theme.Palette.textFaint)
            Spacer()
            Button("+ 밴드") { engine.addEqBand(freq: 1000) }
                .font(Theme.Font.ui(10, .semibold)).buttonStyle(.plain).foregroundStyle(Theme.Palette.accent)
                .disabled(engine.monitorEqBands.count >= 64)
            Button("전체 지우기") { engine.clearMonitorEq() }
                .font(Theme.Font.ui(9)).buttonStyle(.plain).foregroundStyle(Theme.Palette.textMuted)
                .disabled(engine.monitorEqBands.isEmpty)
            Button { engine.monitorEqOpen = false } label: {
                Image(systemName: "xmark").font(.system(size: 11, weight: .bold))
            }
            .buttonStyle(.plain).foregroundStyle(Theme.Palette.textMuted)
        }
        .padding(.horizontal, 14).frame(height: 44)
    }

    // Log-frequency x, ±dbRange y. Draws a grid, the 0 dB line, and the response curve.
    private var curve: some View {
        let response = engine.monitorEqResponse(count: 200)
        return Canvas { ctx, size in
            let w = size.width, h = size.height
            func y(_ db: Double) -> CGFloat { h/2 - CGFloat(db / dbRange) * (h/2) }
            // Grid: 0 dB + ±6/12 lines.
            for db in [-12.0, -6, 0, 6, 12] {
                var p = Path(); p.move(to: CGPoint(x: 0, y: y(db))); p.addLine(to: CGPoint(x: w, y: y(db)))
                ctx.stroke(p, with: .color(.white.opacity(db == 0 ? 0.22 : 0.07)), lineWidth: db == 0 ? 1 : 0.5)
            }
            // Vertical decade markers (100 / 1k / 10k).
            for f in [100.0, 1000, 10000] {
                let x = w * CGFloat(log(f/minHz) / log(maxHz/minHz))
                var p = Path(); p.move(to: CGPoint(x: x, y: 0)); p.addLine(to: CGPoint(x: x, y: h))
                ctx.stroke(p, with: .color(.white.opacity(0.06)), lineWidth: 0.5)
            }
            guard response.count > 1 else { return }
            var path = Path()
            for (i, db) in response.enumerated() {
                let x = w * CGFloat(i) / CGFloat(response.count - 1)
                let pt = CGPoint(x: x, y: y(max(-dbRange, min(dbRange, db))))
                if i == 0 { path.move(to: pt) } else { path.addLine(to: pt) }
            }
            ctx.stroke(path, with: .color(Theme.Palette.accent), lineWidth: 1.8)
        }
        .background(RoundedRectangle(cornerRadius: 6).fill(Theme.Palette.recess))
    }

    private var bandList: some View {
        ScrollView {
            VStack(spacing: 6) {
                if engine.monitorEqBands.isEmpty {
                    Text("밴드가 없습니다 — '+ 밴드'로 주파수를 하나씩 추가하세요.\n활성 밴드만 처리되어 DSP 부하가 없습니다.")
                        .font(Theme.Font.ui(10)).foregroundStyle(Theme.Palette.textFaint)
                        .multilineTextAlignment(.center).padding(.top, 20)
                }
                ForEach(engine.monitorEqBands) { band in
                    bandRow(band)
                }
            }
            .padding(12)
        }
    }

    private func bandRow(_ band: EngineController.EqBand) -> some View {
        VStack(spacing: 4) {
            HStack(spacing: 6) {
                Button { var b = band; b.enabled.toggle(); engine.updateEqBand(b) } label: {
                    Image(systemName: band.enabled ? "circle.fill" : "circle")
                        .font(.system(size: 9)).foregroundStyle(band.enabled ? Theme.Palette.accent : Theme.Palette.textFaint)
                }.buttonStyle(.plain)
                Menu {
                    ForEach(EngineController.eqBandTypes, id: \.self) { t in
                        Button(EngineController.eqBandTypeLabels[t] ?? t) { var b = band; b.type = t; engine.updateEqBand(b) }
                    }
                } label: {
                    Text(EngineController.eqBandTypeLabels[band.type] ?? band.type)
                        .font(Theme.Font.mono(9)).foregroundStyle(Theme.Palette.textSecondary).frame(width: 56, alignment: .leading)
                }.menuStyle(.button).buttonStyle(.plain)
                Text(freqLabel(band.frequencyHz)).font(Theme.Font.mono(9)).foregroundStyle(Theme.Palette.textNumeric).frame(width: 52, alignment: .trailing)
                Text("\(String(format: "%+.1f", band.gainDb))dB").font(Theme.Font.mono(9)).foregroundStyle(Theme.Palette.textNumeric).frame(width: 52, alignment: .trailing)
                Text("Q\(String(format: "%.2f", band.q))").font(Theme.Font.mono(8)).foregroundStyle(Theme.Palette.textFaint).frame(width: 44, alignment: .trailing)
                Spacer(minLength: 0)
                Button { engine.removeEqBand(band.id) } label: {
                    Image(systemName: "xmark").font(.system(size: 8, weight: .bold))
                }.buttonStyle(.plain).foregroundStyle(Theme.Palette.textFaint)
            }
            // Freq (log) + gain sliders; Q slider on its own row.
            HStack(spacing: 8) {
                slider(value: logFreqBinding(band), label: "F")
                slider(value: Binding(get: { band.gainDb }, set: { var b = band; b.gainDb = $0; engine.updateEqBand(b) }),
                       range: -dbRange...dbRange, label: "G")
                slider(value: Binding(get: { band.q }, set: { var b = band; b.q = $0; engine.updateEqBand(b) }),
                       range: 0.1...10, label: "Q")
            }
        }
        .padding(.horizontal, 8).padding(.vertical, 6)
        .background(RoundedRectangle(cornerRadius: 6).fill(Theme.Palette.surface.opacity(band.enabled ? 1 : 0.5)))
    }

    private func slider(value: Binding<Double>, range: ClosedRange<Double> = 0...1, label: String) -> some View {
        HStack(spacing: 3) {
            Text(label).font(Theme.Font.mono(7)).foregroundStyle(Theme.Palette.textFaint)
            Slider(value: value, in: range).controlSize(.mini)
        }
    }

    // Frequency slider works in log space so the whole range is reachable smoothly.
    private func logFreqBinding(_ band: EngineController.EqBand) -> Binding<Double> {
        Binding(
            get: { log(band.frequencyHz / minHz) / log(maxHz / minHz) },
            set: { t in var b = band; b.frequencyHz = minHz * pow(maxHz / minHz, max(0, min(1, t))); engine.updateEqBand(b) }
        )
    }

    private func freqLabel(_ hz: Double) -> String {
        hz >= 1000 ? String(format: "%.2fk", hz / 1000) : String(format: "%.0fHz", hz)
    }
}
