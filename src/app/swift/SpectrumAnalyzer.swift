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
