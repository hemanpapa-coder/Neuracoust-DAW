import SwiftUI

/// A live FFT spectrum, drawn on a log-frequency axis with a frequency-mapped colour
/// (red low → violet high), like the reference analyzers. `bins` are 0..1 magnitudes
/// running low→high frequency; the engine's FFT is 2048-point, so bin i is
/// `i * sampleRate / 2048` Hz. `compact` drops the axis for the small dock widget.
struct SpectrumAnalyzerView: View {
    var bins: [Float]
    var sampleRate: Double
    var compact: Bool = false

    private static let fftSize: Double = 2048
    private static let minHz: Double = 20
    private static let maxHz: Double = 20_000

    var body: some View {
        Canvas { context, size in
            guard bins.count > 1, sampleRate > 0 else { return }
            let axisHeight: CGFloat = compact ? 0 : 14
            let plot = CGRect(x: 0, y: 0, width: size.width, height: size.height - axisHeight)
            guard plot.height > 2, plot.width > 2 else { return }

            let logMin = log10(Self.minHz)
            let logMax = log10(Self.maxHz)
            let binHz = sampleRate / Self.fftSize

            // One filled column per pixel, height from the loudest bin it covers.
            let columns = Int(plot.width)
            var path = Path()
            path.move(to: CGPoint(x: 0, y: plot.maxY))
            var lastX: CGFloat = 0
            for column in 0...columns {
                let frac = Double(column) / Double(max(1, columns))
                let hz = pow(10, logMin + frac * (logMax - logMin))
                let binF = hz / binHz
                let lo = max(0, Int(binF))
                let hi = min(bins.count - 1, max(lo, Int(hz * 1.03 / binHz)))
                var mag: Float = 0
                if lo <= hi { for b in lo...hi { mag = max(mag, bins[b]) } }
                let x = plot.minX + CGFloat(frac) * plot.width
                let y = plot.maxY - CGFloat(mag) * plot.height
                path.addLine(to: CGPoint(x: x, y: y))
                lastX = x
            }
            path.addLine(to: CGPoint(x: lastX, y: plot.maxY))
            path.closeSubpath()

            // Frequency-mapped gradient across the width.
            let gradient = Gradient(stops: [
                .init(color: Color(hue: 0.00, saturation: 0.75, brightness: 0.95), location: 0.00),
                .init(color: Color(hue: 0.11, saturation: 0.80, brightness: 0.98), location: 0.20),
                .init(color: Color(hue: 0.30, saturation: 0.75, brightness: 0.92), location: 0.42),
                .init(color: Color(hue: 0.50, saturation: 0.70, brightness: 0.92), location: 0.65),
                .init(color: Color(hue: 0.66, saturation: 0.72, brightness: 0.95), location: 0.85),
                .init(color: Color(hue: 0.80, saturation: 0.70, brightness: 0.95), location: 1.00),
            ])
            context.fill(path, with: .linearGradient(gradient,
                                                     startPoint: CGPoint(x: 0, y: 0),
                                                     endPoint: CGPoint(x: plot.width, y: 0)))
            context.stroke(path, with: .linearGradient(gradient,
                                                       startPoint: CGPoint(x: 0, y: 0),
                                                       endPoint: CGPoint(x: plot.width, y: 0)),
                           lineWidth: 1)

            guard !compact else { return }
            // Decade grid + labels on the large view.
            for hz in [Double(20), 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000] {
                let frac = (log10(hz) - logMin) / (logMax - logMin)
                let x = plot.minX + CGFloat(frac) * plot.width
                var line = Path()
                line.move(to: CGPoint(x: x, y: plot.minY))
                line.addLine(to: CGPoint(x: x, y: plot.maxY))
                context.stroke(line, with: .color(.white.opacity(0.06)), lineWidth: 1)
                let label = hz >= 1000 ? "\(Int(hz / 1000))k" : "\(Int(hz))"
                context.draw(Text(label).font(.system(size: 7, design: .monospaced))
                    .foregroundColor(.white.opacity(0.35)),
                             at: CGPoint(x: x, y: plot.maxY + 7))
            }
        }
    }
}

/// A goniometer / vectorscope: L/R plotted as a Lissajous, rotated 45° so a mono
/// signal draws a vertical line and a wide stereo image spreads horizontally — the
/// classic phase display. `samples` are interleaved L,R pairs.
struct GoniometerView: View {
    var samples: [Float]

    var body: some View {
        Canvas { context, size in
            let side = min(size.width, size.height)
            let cx = size.width / 2
            let cy = size.height / 2
            let radius = side / 2 - 4

            // Reference frame: the diamond and the L/R, M/S axes.
            var frame = Path()
            frame.move(to: CGPoint(x: cx, y: cy - radius))
            frame.addLine(to: CGPoint(x: cx + radius, y: cy))
            frame.addLine(to: CGPoint(x: cx, y: cy + radius))
            frame.addLine(to: CGPoint(x: cx - radius, y: cy))
            frame.closeSubpath()
            context.stroke(frame, with: .color(.white.opacity(0.10)), lineWidth: 1)
            var cross = Path()
            cross.move(to: CGPoint(x: cx, y: cy - radius)); cross.addLine(to: CGPoint(x: cx, y: cy + radius))
            cross.move(to: CGPoint(x: cx - radius, y: cy)); cross.addLine(to: CGPoint(x: cx + radius, y: cy))
            context.stroke(cross, with: .color(.white.opacity(0.06)), lineWidth: 1)

            guard samples.count >= 4 else { return }
            let scale = radius * 0.9
            var dots = Path()
            var i = 0
            while i + 1 < samples.count {
                let l = CGFloat(samples[i])
                let r = CGFloat(samples[i + 1])
                // Rotate 45°: mono (L=R) → vertical, side (L=-R) → horizontal.
                let x = cx + (l - r) * 0.7071 * scale
                let y = cy - (l + r) * 0.7071 * scale
                dots.addEllipse(in: CGRect(x: x - 0.8, y: y - 0.8, width: 1.6, height: 1.6))
                i += 2
            }
            context.fill(dots, with: .color(Color(hue: 0.08, saturation: 0.85, brightness: 1.0).opacity(0.8)))
        }
    }
}

/// ITU-R BS.1770 loudness readout: momentary / short-term / integrated LUFS, loudness
/// range and true-peak, with a bar for the live momentary/short values. True-peak is a
/// sample-peak approximation until oversampled true-peak DSP lands.
struct LoudnessView: View {
    var momentary: Float
    var shortTerm: Float
    var integrated: Float
    var range: Float
    var truePeak: Float

    // Maps a LUFS value in [-40, 0] to a 0..1 bar fraction.
    private func frac(_ lufs: Float) -> CGFloat {
        CGFloat(min(1, max(0, (lufs + 40) / 40)))
    }
    private func lufsText(_ v: Float) -> String { v <= -70 ? "−∞" : String(format: "%.1f", v) }

    var body: some View {
        HStack(alignment: .top, spacing: 18) {
            bar("M", momentary, tint: Color(hue: 0.09, saturation: 0.8, brightness: 1))
            bar("S", shortTerm, tint: Color(hue: 0.55, saturation: 0.7, brightness: 1))
            VStack(alignment: .leading, spacing: 8) {
                row("Integrated", lufsText(integrated) + " LUFS")
                row("Momentary", lufsText(momentary) + " LUFS")
                row("Short-term", lufsText(shortTerm) + " LUFS")
                row("Range (LRA)", String(format: "%.1f LU", range))
                row("True Peak", (truePeak <= -120 ? "−∞" : String(format: "%.1f", truePeak)) + " dBTP")
                Text("ITU-R BS.1770 · TP는 샘플 피크 근사")
                    .font(.system(size: 8))
                    .foregroundStyle(.white.opacity(0.3))
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .padding(4)
    }

    private func bar(_ label: String, _ lufs: Float, tint: Color) -> some View {
        VStack(spacing: 4) {
            GeometryReader { geo in
                ZStack(alignment: .bottom) {
                    RoundedRectangle(cornerRadius: 3).fill(Color.white.opacity(0.06))
                    RoundedRectangle(cornerRadius: 3)
                        .fill(tint)
                        .frame(height: geo.size.height * frac(lufs))
                }
            }
            .frame(width: 22)
            Text(label).font(.system(size: 9, weight: .bold, design: .monospaced))
                .foregroundStyle(.white.opacity(0.6))
        }
        .frame(maxHeight: .infinity)
    }

    private func row(_ label: String, _ value: String) -> some View {
        HStack {
            Text(label).font(.system(size: 10)).foregroundStyle(.white.opacity(0.5))
            Spacer()
            Text(value).font(.system(size: 11, weight: .semibold, design: .monospaced))
                .foregroundStyle(.white.opacity(0.9))
        }
    }
}
