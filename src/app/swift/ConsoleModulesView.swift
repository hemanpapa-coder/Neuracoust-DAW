import SwiftUI
import AppKit

// Scroll-wheel adjust for a knob: a zero-footprint view that never hit-tests (so it blocks no
// clicks/drags), and reports wheel deltas only while the cursor is over the knob, via a local
// scroll-event monitor.
private struct KnobScrollWheel: NSViewRepresentable {
    var active: Bool                        // true while the cursor hovers this knob (SwiftUI, scale-aware)
    var onScroll: (CGFloat, Bool) -> Void   // (delta, isPreciseTrackpad)
    func makeNSView(context: Context) -> NSView {
        let v = Catcher(); v.active = active; v.onScroll = onScroll; return v
    }
    func updateNSView(_ nsView: NSView, context: Context) {
        guard let v = nsView as? Catcher else { return }
        v.active = active; v.onScroll = onScroll
    }
    final class Catcher: NSView {
        var active = false
        var onScroll: ((CGFloat, Bool) -> Void)?
        private var monitor: Any?
        override func hitTest(_ point: NSPoint) -> NSView? { nil }   // mouse passes straight through
        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            if let monitor { NSEvent.removeMonitor(monitor); self.monitor = nil }
            guard window != nil else { return }
            // Gate on the SwiftUI hover flag, not on geometry: the module is drawn scaled, so an
            // NSView bounds check (which ignores the SwiftUI scale transform) would miss.
            monitor = NSEvent.addLocalMonitorForEvents(matching: .scrollWheel) { [weak self] event in
                guard let self, self.active, let win = self.window, event.window === win else { return event }
                if event.momentumPhase != [] { return nil }   // ignore inertial coast (would overshoot)
                let precise = event.hasPreciseScrollingDeltas
                let raw = precise ? event.scrollingDeltaY : event.deltaY
                // Normalize to the physical wheel: up = positive = increase = clockwise, regardless
                // of the system's natural-scroll setting.
                let dy = event.isDirectionInvertedFromDevice ? -raw : raw
                if dy != 0 { self.onScroll?(dy, precise); return nil }
                return event
            }
        }
        deinit { if let monitor { NSEvent.removeMonitor(monitor) } }
    }
}

// SSL 4000E-style console channel — the Claude Design "Neuracoust Modules" look, reproduced in
// SwiftUI and WIRED to the engine's console parameters. Each module is drawn at its native 205px
// design size (machined knobs, colored caps, name plate) and scaled to fit the mixer strip width.

// MARK: - Knob color

private struct ConsoleKnobColor {
    let mid: Color, lo: Color, dot: Color
    // Flat cap colors (swatches 1-4). `lo` unused now the caps are flat.
    static let black  = ConsoleKnobColor(mid: Color(hex: 0x1b1c1f), lo: Color(hex: 0x1b1c1f), dot: Color(hex: 0xf4f1e8))
    static let silver = ConsoleKnobColor(mid: Color(hex: 0xa29c8d), lo: Color(hex: 0xa29c8d), dot: Color(hex: 0x2a2a2a))
    static let red    = ConsoleKnobColor(mid: Color(hex: 0x5f2a26), lo: Color(hex: 0x5f2a26), dot: Color(hex: 0xf4f1e8))
    static let green  = ConsoleKnobColor(mid: Color(hex: 0x2e5a3c), lo: Color(hex: 0x2e5a3c), dot: Color(hex: 0xf4f1e8))
    static let blue   = ConsoleKnobColor(mid: Color(hex: 0x3f6193), lo: Color(hex: 0x3f6193), dot: Color(hex: 0xf4f1e8))
    static let brown  = ConsoleKnobColor(mid: Color(hex: 0x4e3a2c), lo: Color(hex: 0x4e3a2c), dot: Color(hex: 0xf4f1e8))
    static let orange = ConsoleKnobColor(mid: Color(hex: 0xa8632e), lo: Color(hex: 0xa8632e), dot: Color(hex: 0xf4f1e8))
    static let bezel  = Color(hex: 0x9a9a9a)   // swatch 5 (outer ring)
}

/// One console rotary — machined bezel, matte colored cap, carved dimple pointer, radial marks.
/// Interactive: vertical drag changes the value, double-click restores the default.
private struct ConsoleKnob: View {
    var color: ConsoleKnobColor = .silver
    var marks: [String] = []
    var markRadius: CGFloat = 12
    var unit: String = ""
    var markStart: Double = -135
    var markEnd: Double = 135
    var value: Float = 0
    var range: ClosedRange<Float> = 0...1
    var defaultValue: Float = 0
    var diameter: CGFloat = 50
    var markFont: CGFloat = 7
    var unitFont: CGFloat = 6.5
    var unitAtZero: Bool = false        // draw the unit in place of the "0" mark instead of at the bottom
    var unitAbove: Bool = false         // draw the unit above the knob instead of below
    var dotDiameter: CGFloat = 0        // "·" marks render as a filled circle of this size (0 = as text)
    var dotGap: CGFloat = 4             // gap from the knob rim to a dot mark
    var dimpleSize: CGFloat = 8         // pointer dimple outer diameter (inner = half)
    var extraBottom: CGFloat = 0        // extra frame height so the unit clears the rim below
    var centerFormat: ((Float) -> String)? = nil   // live readout drawn on the knob face
    var wheelStep: Float = 0            // value change per wheel notch (0 = proportional, span/100)
    var wheelLog: Bool = false          // if true, wheelStep is octaves/notch, applied multiplicatively
    var logScale: Bool = false          // pointer + drag map logarithmically (frequency knobs)
    var reverse: Bool = false           // high value at the left (e.g. SSL comp threshold)
    var onChange: (Float) -> Void = { _ in }
    var onCommit: () -> Void = {}
    @State private var dragStart: Float?
    @State private var liveValue: Float?
    @State private var scrollCommit: DispatchWorkItem?
    @State private var scrollAccum: CGFloat = 0
    @State private var hovering = false
    @State private var valueShown = false
    @State private var fadeWork: DispatchWorkItem?

    private var normalized: Double {
        let v = Double(liveValue ?? value)
        let lo = Double(range.lowerBound), hi = Double(range.upperBound)
        if logScale && lo > 0 && hi > lo {
            return (log(max(lo, min(hi, v))) - log(lo)) / (log(hi) - log(lo))
        }
        return (v - lo) / max(0.0001, hi - lo)
    }
    private var valueDeg: Double {
        let n = reverse ? (1 - normalized) : normalized
        return markStart + n * (markEnd - markStart)
    }

    var body: some View {
        ZStack {
            // Bezel — flat outer ring (swatch 5), wider/softer shadow, no 3D.
            Circle()
                .fill(ConsoleKnobColor.bezel)
                .overlay(Circle().stroke(.black.opacity(0.22), lineWidth: 1))
                .shadow(color: .black.opacity(0.20), radius: 6, y: 2)
                .frame(width: diameter, height: diameter)
            // Flat colored cap (swatches 1-4), no gradient/highlight.
            Circle()
                .fill(color.mid)
                .overlay(Circle().stroke(.black.opacity(0.25), lineWidth: 1))
                .frame(width: diameter - 8, height: diameter - 8)
            // Pointer line, near the rim, in the bezel grey. Grows 3pt further inward (outer end
            // pinned at the rim): height 8→11, centre shifted so only the inner end moves.
            RoundedRectangle(cornerRadius: 1.5)
                .fill(ConsoleKnobColor.bezel)
                .frame(width: 4, height: 11)
                .offset(y: -(diameter / 2 - 8.5))
                .rotationEffect(.degrees(valueDeg))
            // Live value on the knob face — appears while adjusting, fades out ~2s after.
            if let centerFormat {
                Text(centerFormat(liveValue ?? value))
                    .font(.system(size: markFont, weight: .semibold, design: .monospaced))
                    .foregroundStyle(color.dot)
                    .lineLimit(1)
                    .fixedSize()
                    .opacity(valueShown ? 1 : 0)
            }
        }
        .frame(width: diameter + markRadius * 2 + 16, height: diameter + markRadius * 2 + 14 + extraBottom)
        .overlay(marksOverlay)
        .overlay(KnobScrollWheel(active: hovering) { dy, precise in applyScroll(dy, precise) }.frame(width: 0, height: 0))
        .contentShape(Rectangle())
        .onHover { hovering = $0 }
        .gesture(DragGesture(minimumDistance: 1)
            .onChanged { drag in
                let start = dragStart ?? value
                if dragStart == nil { dragStart = start }
                let lo = range.lowerBound, hi = range.upperBound
                var next: Float
                if logScale && lo > 0 && hi > lo {
                    let startNorm = (log(Double(start)) - log(Double(lo))) / (log(Double(hi)) - log(Double(lo)))
                    let nextNorm = min(1, max(0, startNorm + Double(-drag.translation.height / 200)))
                    next = Float(Double(lo) * pow(Double(hi) / Double(lo), nextNorm))
                } else {
                    let dv = Float(-drag.translation.height / 90) * (hi - lo)
                    next = min(hi, max(lo, start + (reverse ? -dv : dv)))
                }
                liveValue = next; onChange(next); flashValue()
            }
            .onEnded { _ in dragStart = nil; liveValue = nil; onCommit() })
        .highPriorityGesture(TapGesture(count: 2).onEnded {
            onChange(defaultValue); onCommit(); flashValue()
        })
    }

    // Show the on-face value now, then fade it out ~2s after the last adjustment.
    private func flashValue() {
        fadeWork?.cancel()
        valueShown = true
        let w = DispatchWorkItem { withAnimation(.easeOut(duration: 1.6)) { valueShown = false } }
        fadeWork = w
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.4, execute: w)
    }

    // Wheel over the knob: one detent = one step (1 dB for gain, a semitone for freq, etc.);
    // change live, then record one undo step once scrolling settles.
    private func applyScroll(_ delta: CGFloat, _ precise: Bool) {
        var notches = 0
        if precise {
            // Reversing direction starts fresh, so up-then-down responds at once instead of first
            // having to cancel leftover travel from the previous direction.
            if scrollAccum != 0 && (delta > 0) != (scrollAccum > 0) { scrollAccum = 0 }
            scrollAccum += delta
            let perNotch: CGFloat = 25          // trackpad: ~25pt of travel per step, one step max per event
            if abs(scrollAccum) >= perNotch {
                notches = scrollAccum > 0 ? 1 : -1
                scrollAccum -= CGFloat(notches) * perNotch
            }
        } else {
            notches = delta > 0 ? 1 : -1        // one mouse detent = one step (no acceleration)
        }
        guard notches != 0 else { return }
        let dir = Float(notches) * (reverse ? -1 : 1)
        let base = liveValue ?? value
        var next: Float
        if wheelLog {
            let oct = wheelStep > 0 ? wheelStep : 1.0 / 12    // default one semitone per notch
            next = base * powf(2, dir * oct)
        } else {
            let inc = wheelStep > 0 ? wheelStep : (range.upperBound - range.lowerBound) / 100
            next = base + dir * inc
        }
        next = min(range.upperBound, max(range.lowerBound, next))
        liveValue = next
        onChange(next)
        flashValue()
        scrollCommit?.cancel()
        let work = DispatchWorkItem { onCommit(); liveValue = nil }
        scrollCommit = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.4, execute: work)
    }

    private var marksOverlay: some View {
        GeometryReader { geo in
            let cx = geo.size.width / 2, cy = geo.size.height / 2
            // Scale text sits 2pt further from the knob than before, in a milk-white ink.
            let r = diameter / 2 + markRadius + 2
            let dotR = diameter / 2 + dotGap + dotDiameter / 2
            let ink = Color(hex: 0xf4f1e8)
            ForEach(Array(marks.enumerated()), id: \.offset) { i, label in
                let t = marks.count > 1 ? Double(i) / Double(marks.count - 1) : 0.5
                let a = (markStart + (markEnd - markStart) * t) * .pi / 180
                if label == "·" && dotDiameter > 0 {
                    Circle()
                        .fill(ink)
                        .frame(width: dotDiameter, height: dotDiameter)
                        .position(x: cx + dotR * sin(a), y: cy - dotR * cos(a))
                } else if label == "QN" || label == "QW" {
                    QCurveIcon(wide: label == "QW")
                        .stroke(ink, style: StrokeStyle(lineWidth: 1.2, lineJoin: .round))
                        .frame(width: 16, height: 10)
                        .position(x: cx + r * sin(a), y: cy - r * cos(a))
                } else if unitAtZero && label == "0" {
                    Text(unit)
                        .font(.system(size: markFont, design: .monospaced))
                        .foregroundStyle(ink)
                        .position(x: cx + r * sin(a), y: cy - r * cos(a))
                } else {
                    Text(label)
                        .font(.system(size: label == "∞" ? markFont * 2 : markFont, design: .monospaced))
                        .foregroundStyle(ink)
                        .position(x: cx + r * sin(a), y: cy - r * cos(a))
                }
            }
            if !unit.isEmpty && !(unitAtZero && marks.contains("0")) {
                Text(unit)
                    .font(.system(size: unitFont, design: .monospaced))
                    .foregroundStyle(ink)
                    .position(x: cx, y: unitAbove ? cy - diameter / 2 - 4 - unitFont / 2
                                                  : cy + diameter / 2 + 4 + unitFont / 2)
            }
        }
    }
}

// A small bell curve — narrow (high Q) or wide (low Q) — drawn beside the Q knob.
private struct QCurveIcon: Shape {
    var wide: Bool
    func path(in rect: CGRect) -> Path {
        var p = Path()
        let w = rect.width, h = rect.height, base = rect.maxY, mid = rect.midX
        let width = wide ? w * 0.42 : w * 0.16
        let steps = 20
        p.move(to: CGPoint(x: rect.minX, y: base))
        for i in 0...steps {
            let x = w * CGFloat(i) / CGFloat(steps)
            let dx = (x - mid) / width
            let y = base - h * CGFloat(exp(-Double(dx * dx)))
            p.addLine(to: CGPoint(x: x, y: y))
        }
        p.addLine(to: CGPoint(x: rect.maxX, y: base))
        return p
    }
}

/// A single horizontal line across the frame's vertical centre — used for the dashed
/// "stereo, L·R independent" link in the module footer.
private struct HLine: Shape {
    func path(in rect: CGRect) -> Path {
        var p = Path()
        p.move(to: CGPoint(x: rect.minX, y: rect.midY))
        p.addLine(to: CGPoint(x: rect.maxX, y: rect.midY))
        return p
    }
}

/// Perceptual-model glyph: six rounded bars of an ear/loudness response, from the
/// Claude Design "Neuracoust Modules" footer (20×12 design space).
private struct EarBarsIcon: View {
    var color: Color
    private let heights: [CGFloat] = [2, 5.2, 10, 7.2, 4, 1.6]
    var body: some View {
        HStack(alignment: .center, spacing: 1.4) {
            ForEach(Array(heights.enumerated()), id: \.offset) { _, h in
                Capsule().fill(color).frame(width: 1.6, height: h)
            }
        }
        .frame(width: 20, height: 12)
    }
}

/// Circuit-model glyph: the zigzag waveform stroke from the same footer design.
private struct CircuitWaveIcon: View {
    var color: Color
    var body: some View {
        ZigzagShape()
            .stroke(color, style: StrokeStyle(lineWidth: 1.2, lineCap: .round, lineJoin: .round))
            .frame(width: 20, height: 12)
    }
    private struct ZigzagShape: Shape {
        func path(in rect: CGRect) -> Path {
            let sx = rect.width / 20, sy = rect.height / 12
            func P(_ x: CGFloat, _ y: CGFloat) -> CGPoint { CGPoint(x: x * sx, y: y * sy) }
            var p = Path()
            p.move(to: P(0.8, 6))
            p.addLine(to: P(4, 6))
            p.addLine(to: P(5.2, 2.6))
            p.addLine(to: P(7.2, 9.4))
            p.addLine(to: P(9.2, 2.6))
            p.addLine(to: P(11.2, 9.4))
            p.addLine(to: P(12.4, 6))
            p.addLine(to: P(19.2, 6))
            return p
        }
    }
}

/// SSL filter-section slope glyph: high-pass (lows rolled off, rising to a plateau) or
/// low-pass (highs rolled off, plateau falling away). Printed beside each filter knob.
private struct FilterSlopeShape: Shape {
    var highPass: Bool
    func path(in rect: CGRect) -> Path {
        let w = rect.width, h = rect.height
        let top = h * 0.14, bot = h * 0.96
        var p = Path()
        if highPass {
            p.move(to: CGPoint(x: 0, y: bot))
            p.addLine(to: CGPoint(x: w * 0.26, y: bot))
            p.addQuadCurve(to: CGPoint(x: w * 0.64, y: top), control: CGPoint(x: w * 0.46, y: top))
            p.addLine(to: CGPoint(x: w, y: top))
        } else {
            p.move(to: CGPoint(x: 0, y: top))
            p.addLine(to: CGPoint(x: w * 0.36, y: top))
            p.addQuadCurve(to: CGPoint(x: w * 0.74, y: bot), control: CGPoint(x: w * 0.54, y: bot))
            p.addLine(to: CGPoint(x: w, y: bot))
        }
        return p
    }
}

private struct FilterSlopeIcon: View {
    var highPass: Bool
    var color: Color = Color(hex: 0xc9c2b2)
    var body: some View {
        FilterSlopeShape(highPass: highPass)
            .stroke(color, style: StrokeStyle(lineWidth: 1.4, lineCap: .round, lineJoin: .round))
            .frame(width: 26, height: 14)
    }
}

/// Attack-time glyph: an envelope edge that rises steeply for FAST, gently for SLOW.
private struct AttackIcon: View {
    var fast: Bool
    var color: Color
    var body: some View { AttackShape(fast: fast).stroke(color, style: StrokeStyle(lineWidth: 1.6, lineCap: .round, lineJoin: .round)).frame(width: 20, height: 13) }
    private struct AttackShape: Shape {
        var fast: Bool
        func path(in r: CGRect) -> Path {
            var p = Path()
            p.move(to: CGPoint(x: 0, y: r.maxY))
            p.addLine(to: CGPoint(x: r.width * (fast ? 0.30 : 0.85), y: 0))  // steep vs gradual rise
            p.addLine(to: CGPoint(x: r.width, y: 0))
            return p
        }
    }
}

/// Dynamics glyph: a soft downward slope for EXP (expander), a hard step for GATE.
private struct GateExpIcon: View {
    var exp: Bool
    var color: Color
    var body: some View { GateExpShape(exp: exp).stroke(color, style: StrokeStyle(lineWidth: 1.6, lineCap: .round, lineJoin: .round)).frame(width: 20, height: 13) }
    private struct GateExpShape: Shape {
        var exp: Bool
        func path(in r: CGRect) -> Path {
            var p = Path()
            if exp {
                p.move(to: CGPoint(x: 0, y: 0)); p.addLine(to: CGPoint(x: r.width, y: r.maxY))     // gentle slope
            } else {
                p.move(to: CGPoint(x: 0, y: 0)); p.addLine(to: CGPoint(x: r.width * 0.5, y: 0))     // hard step down
                p.addLine(to: CGPoint(x: r.width * 0.5, y: r.maxY)); p.addLine(to: CGPoint(x: r.width, y: r.maxY))
            }
            return p
        }
    }
}

/// Polarity glyph: a circle with a slash — the console Ø key as a picture.
private struct PhaseIcon: View {
    var color: Color
    var body: some View {
        ZStack {
            Circle().stroke(color, lineWidth: 1.6)
            Path { p in p.move(to: CGPoint(x: 1.5, y: 12.5)); p.addLine(to: CGPoint(x: 12.5, y: 1.5)) }
                .stroke(color, lineWidth: 1.6)
        }.frame(width: 14, height: 14)
    }
}

// MARK: - EQ curve graph (FabFilter-style)

// One biquad section; magnitude in dB at a frequency.
private struct Biquad {
    var b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0
    func magDb(_ f: Double, _ fs: Double) -> Double {
        let w = 2 * Double.pi * f / fs
        let c1 = cos(w), s1 = sin(w), c2 = cos(2 * w), s2 = sin(2 * w)
        let nRe = b0 + b1 * c1 + b2 * c2, nIm = -(b1 * s1 + b2 * s2)
        let dRe = a0 + a1 * c1 + a2 * c2, dIm = -(a1 * s1 + a2 * s2)
        return 10 * log10(max(1e-9, (nRe * nRe + nIm * nIm) / max(1e-12, dRe * dRe + dIm * dIm)))
    }
    static func peak(_ fc: Double, _ g: Double, _ q: Double, _ fs: Double) -> Biquad {
        let A = pow(10, g / 40), w0 = 2 * .pi * fc / fs, cw = cos(w0), alpha = sin(w0) / (2 * max(0.1, q))
        return Biquad(b0: 1 + alpha * A, b1: -2 * cw, b2: 1 - alpha * A, a0: 1 + alpha / A, a1: -2 * cw, a2: 1 - alpha / A)
    }
    static func lowShelf(_ fc: Double, _ g: Double, _ fs: Double) -> Biquad {
        let A = pow(10, g / 40), w0 = 2 * .pi * fc / fs, cw = cos(w0), sq = 2 * sqrt(A) * (sin(w0) / 2 * 1.4142)
        return Biquad(b0: A * ((A + 1) - (A - 1) * cw + sq), b1: 2 * A * ((A - 1) - (A + 1) * cw),
                      b2: A * ((A + 1) - (A - 1) * cw - sq), a0: (A + 1) + (A - 1) * cw + sq,
                      a1: -2 * ((A - 1) + (A + 1) * cw), a2: (A + 1) + (A - 1) * cw - sq)
    }
    static func highShelf(_ fc: Double, _ g: Double, _ fs: Double) -> Biquad {
        let A = pow(10, g / 40), w0 = 2 * .pi * fc / fs, cw = cos(w0), sq = 2 * sqrt(A) * (sin(w0) / 2 * 1.4142)
        return Biquad(b0: A * ((A + 1) + (A - 1) * cw + sq), b1: -2 * A * ((A - 1) + (A + 1) * cw),
                      b2: A * ((A + 1) + (A - 1) * cw - sq), a0: (A + 1) - (A - 1) * cw + sq,
                      a1: 2 * ((A - 1) - (A + 1) * cw), a2: (A + 1) - (A - 1) * cw - sq)
    }
    static func highPass(_ fc: Double, _ fs: Double) -> Biquad {
        let w0 = 2 * .pi * fc / fs, cw = cos(w0), alpha = sin(w0) / (2 * 0.707)
        return Biquad(b0: (1 + cw) / 2, b1: -(1 + cw), b2: (1 + cw) / 2, a0: 1 + alpha, a1: -2 * cw, a2: 1 - alpha)
    }
    static func lowPass(_ fc: Double, _ fs: Double) -> Biquad {
        let w0 = 2 * .pi * fc / fs, cw = cos(w0), alpha = sin(w0) / (2 * 0.707)
        return Biquad(b0: (1 - cw) / 2, b1: 1 - cw, b2: (1 - cw) / 2, a0: 1 + alpha, a1: -2 * cw, a2: 1 - alpha)
    }
}

private struct EqGraphView: View {
    @ObservedObject var engine: EngineController
    let trackId: Int
    private let minF = 20.0, maxF = 20000.0, dbRange = 18.0
    private let steps = 96

    private func sections() -> [Biquad] {
        let fs = (engine.sampleRate > 0 ? engine.sampleRate : 48000)
        func v(_ p: String) -> Double { Double(engine.consoleValue(trackId, p)) }
        var s: [Biquad] = []
        let hfF = v("eqHfHz"), hfG = v("eqHfGainDb")
        s.append(engine.consoleBool(trackId, "eqHfBell") ? .peak(hfF, hfG, 0.9, fs) : .highShelf(hfF, hfG, fs))
        s.append(.peak(v("eqHmfHz"), v("eqHmfGainDb"), v("eqHmfQ"), fs))
        s.append(.peak(v("eqLmfHz"), v("eqLmfGainDb"), v("eqLmfQ"), fs))
        let lfF = v("eqLfHz"), lfG = v("eqLfGainDb")
        s.append(engine.consoleBool(trackId, "eqLfBell") ? .peak(lfF, lfG, 0.9, fs) : .lowShelf(lfF, lfG, fs))
        let hp = v("highPassHz"); if hp > 21 { s.append(.highPass(hp, fs)) }
        let lp = v("lowPassHz"); if lp < 11900 { s.append(.lowPass(lp, fs)) }
        return s
    }

    var body: some View {
        Canvas { ctx, size in
            let W = size.width, H = size.height
            let fs = (engine.sampleRate > 0 ? engine.sampleRate : 48000)
            func xAt(_ f: Double) -> CGFloat { CGFloat(log(f / minF) / log(maxF / minF)) * W }
            func yAtDb(_ db: Double) -> CGFloat { H / 2 - CGFloat(db / dbRange) * (H / 2 - 5) }

            for f in [100.0, 1000.0, 10000.0] {
                var p = Path(); p.move(to: CGPoint(x: xAt(f), y: 0)); p.addLine(to: CGPoint(x: xAt(f), y: H))
                ctx.stroke(p, with: .color(.white.opacity(0.06)), lineWidth: 1)
            }
            var mid = Path(); mid.move(to: CGPoint(x: 0, y: H / 2)); mid.addLine(to: CGPoint(x: W, y: H / 2))
            ctx.stroke(mid, with: .color(.white.opacity(0.10)), lineWidth: 1)

            // Real-time spectrum background.
            let bins = engine.spectrumBins
            if bins.count > 1 {
                let ny = fs / 2
                var sp = Path(); sp.move(to: CGPoint(x: 0, y: H))
                var started = false
                for i in 0..<bins.count {
                    let f = (Double(i) + 0.5) * ny / Double(bins.count)
                    if f < minF || f > maxF { continue }
                    let pt = CGPoint(x: xAt(f), y: H - CGFloat(max(0, min(1, bins[i]))) * H)
                    if !started { sp.addLine(to: CGPoint(x: pt.x, y: H)); started = true }
                    sp.addLine(to: pt)
                }
                sp.addLine(to: CGPoint(x: W, y: H)); sp.closeSubpath()
                ctx.fill(sp, with: .color(Color(hex: 0x5bd6a0).opacity(0.16)))
            }

            // EQ response curve, computed from the live params each draw (cheap at 96 points).
            let secs = sections()
            var curve = Path()
            for i in 0...steps {
                let f = minF * pow(maxF / minF, Double(i) / Double(steps))
                let db = max(-dbRange, min(dbRange, secs.reduce(0.0) { $0 + $1.magDb(f, fs) }))
                let pt = CGPoint(x: CGFloat(Double(i) / Double(steps)) * W, y: yAtDb(db))
                if i == 0 { curve.move(to: pt) } else { curve.addLine(to: pt) }
            }
            ctx.stroke(curve, with: .color(Color(hex: 0xffd166)), lineWidth: 1.6)
        }
        .background(Color.black.opacity(0.55))
        .clipShape(RoundedRectangle(cornerRadius: 4))
        .overlay(RoundedRectangle(cornerRadius: 4).stroke(.black.opacity(0.5), lineWidth: 1))
    }
}

// The compressor's parallel-MIX bar: a horizontal fader driven by the actual bar width (so drag
// tracks the cursor exactly), a live drag state (smooth), and wheel support.
/// Horizontal gain-reduction meter for the comp / gate — a right-anchored amber bar that grows left
/// as reduction increases, with the live value in dB. Text sized to match the module's knob labels.
private struct GrMeter: View {
    let label: String
    let gr: Float                 // gain reduction, dB (positive)
    var maxGr: Float = 30
    var body: some View {
        let frac = CGFloat(max(0, min(1, gr / maxGr)))
        HStack(spacing: 7) {
            Text(label).font(.system(size: 13, weight: .bold, design: .monospaced)).tracking(0.5)
                .foregroundStyle(Color(hex: 0xa39c8b)).fixedSize()
            GeometryReader { g in
                ZStack(alignment: .trailing) {
                    Capsule().fill(Color(hex: 0x140a06))
                        .overlay(Capsule().stroke(.black, lineWidth: 1))
                    Capsule()
                        .fill(LinearGradient(colors: [Color(hex: 0xf0b23a), Color(hex: 0xd9691c)], startPoint: .trailing, endPoint: .leading))
                        .frame(width: frac * g.size.width)
                        .shadow(color: Color(hex: 0xf0902e).opacity(0.55), radius: 2)
                }
            }.frame(height: 11)
            Text(gr > 0.1 ? String(format: "-%.0f", gr) : "0")
                .font(.system(size: 15, weight: .semibold, design: .monospaced))
                .foregroundStyle(gr > 0.1 ? Color(hex: 0xe0c33e) : Color(hex: 0x6a6456))
                .frame(width: 26, alignment: .trailing)
        }
        .padding(.horizontal, 8).padding(.top, 5).padding(.bottom, 3)
    }
}

private struct MixBar: View {
    @ObservedObject var engine: EngineController
    let trackId: Int
    @State private var live: Float?
    @State private var hovering = false
    @State private var accum: CGFloat = 0
    @State private var commit: DispatchWorkItem?

    private func scheduleCommit() {
        commit?.cancel()
        let w = DispatchWorkItem { engine.recordGesture("4000E compMix"); live = nil }
        commit = w
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.4, execute: w)
    }
    private func wheel(_ delta: CGFloat, _ precise: Bool) {
        var n = 0
        if precise {
            if accum != 0 && (delta > 0) != (accum > 0) { accum = 0 }
            accum += delta
            if abs(accum) >= 25 { n = accum > 0 ? 1 : -1; accum -= CGFloat(n) * 25 }
        } else { n = delta > 0 ? 1 : -1 }
        guard n != 0 else { return }
        let base = live ?? engine.consoleValue(trackId, "compMix")
        let v = min(1, max(0, base + Float(n) * 0.02))   // 2% per notch
        live = v; engine.setConsoleValue(trackId, "compMix", v); scheduleCommit()
    }

    var body: some View {
        HStack(spacing: 7) {
            GeometryReader { g in
                let value = live ?? engine.consoleValue(trackId, "compMix")
                let w = g.size.width
                ZStack(alignment: .leading) {
                    RoundedRectangle(cornerRadius: 3).fill(Color(hex: 0x0a1410))
                    RoundedRectangle(cornerRadius: 2).fill(Color(hex: 0x54e08a).opacity(0.35))
                        .frame(width: w * CGFloat(max(0, min(1, value))))
                    Text(String(format: "%.0f%%", value * 100)).font(.system(size: 13, design: .monospaced))
                        .foregroundStyle(Color(hex: 0x7dffb4)).frame(width: w)
                }
                .contentShape(Rectangle())
                .gesture(DragGesture(minimumDistance: 0).onChanged { d in
                    let v = Float(max(0, min(1, d.location.x / max(1, w))))
                    live = v; engine.setConsoleValue(trackId, "compMix", v)
                }.onEnded { _ in engine.recordGesture("4000E compMix"); live = nil })
                .overlay(KnobScrollWheel(active: hovering) { dy, p in wheel(dy, p) }.frame(width: 0, height: 0))
                .onHover { hovering = $0 }
            }
            .frame(height: 22)
            .overlay(RoundedRectangle(cornerRadius: 3).stroke(.black, lineWidth: 1))
            Text("MIX").font(.system(size: 13, weight: .bold)).tracking(1.2).foregroundStyle(Color(hex: 0xe6dfd0))
        }
        .padding(.horizontal, 10).frame(height: 30)
    }
}

// MARK: - Module chrome

private struct ConsoleModuleChrome<Content: View>: View {
    let title: String
    let modelName: String
    let models: [String]
    let onSelectModel: (String) -> Void
    let inOn: Bool
    let onToggleIn: () -> Void
    @ViewBuilder var content: () -> Content

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 8) {
                Text(title).font(.system(size: 14, weight: .bold)).tracking(1.6).foregroundStyle(Color(hex: 0xe6dfd0))
                Spacer()
                Button(action: onToggleIn) {
                    Text("IN").font(.system(size: 12, weight: .bold)).tracking(1)
                        .foregroundStyle(inOn ? Color(hex: 0x2a1f10) : Color(hex: 0x7a7f86))
                        .frame(width: 42, height: 22)
                        .background(inOn
                            ? AnyView(LinearGradient(colors: [Color(hex: 0xe0a94b), Color(hex: 0xa9741f)], startPoint: .top, endPoint: .bottom))
                            : AnyView(LinearGradient(colors: [Color(hex: 0x33353a), Color(hex: 0x202225)], startPoint: .top, endPoint: .bottom)))
                        .clipShape(RoundedRectangle(cornerRadius: 4))
                        .overlay(RoundedRectangle(cornerRadius: 4).stroke(.black, lineWidth: 1))
                }.buttonStyle(.plain)
            }
            .padding(.horizontal, 10).frame(height: 34)
            .background(LinearGradient(colors: [Color(hex: 0x35373a), Color(hex: 0x26282a)], startPoint: .top, endPoint: .bottom))
            .overlay(Rectangle().fill(.black).frame(height: 1), alignment: .bottom)

            content()
            // The model name plate is rendered by NeuracoustConsoleModulesView at the module's real
            // width (outside the scale) so it stays legible and clickable — see `modelPlate`.
        }
        .frame(width: 205)
        .background(LinearGradient(colors: [Color(hex: 0x2b2d2f), Color(hex: 0x232527)], startPoint: .top, endPoint: .bottom))
        .clipShape(RoundedRectangle(cornerRadius: 5))
        .overlay(RoundedRectangle(cornerRadius: 5).stroke(.black, lineWidth: 1))
    }
}

// MARK: - Wired module view

struct NeuracoustConsoleModulesView: View {
    let module: MixerModuleFocus
    var width: CGFloat = 205
    @ObservedObject var engine: EngineController
    let trackId: Int
    /// (moduleEnabled, toggle) for the amber IN button — the host knows the per-module bool.
    let inOn: Bool
    let onToggleIn: () -> Void

    // Gain knobs: "-" / "0" / "+" only, no dots.
    private let gainMarks = ["–", "0", "+"]

    private func moduleHeight(_ m: MixerModuleFocus) -> CGFloat {
        switch m {
        case .filter:    return 244
        case .eq:        return 798
        case .comp:      return 336
        case .gate:      return 308
        case .saturator: return 244
        default:         return 258
        }
    }

    var body: some View {
        let scale = min(1, width / 205)
        // Circuit mode lives in each module's knob area; dual/stereo is a mixer-channel control. The
        // model name-plate renders here, UNSCALED, at the strip's real width, so it stays legible and
        // clickable (the scaled module chrome would shrink it to nothing).
        VStack(spacing: 0) {
            moduleBody
                .scaleEffect(scale, anchor: .top)
                .frame(width: width, height: moduleHeight(module) * scale, alignment: .top)
            modelPlate
        }
    }

    // The per-module model selector. Comp/gate pick from their own model libraries; the other
    // modules show the shared console model. Real-size (not scaled), so it's always usable.
    private var modelPlate: some View {
        let tk = engine.tracks.first(where: { $0.id == trackId })
        let name: String
        let models: [String]
        let pick: (String) -> Void
        switch module {
        case .comp: name = tk?.consoleCompType ?? "SSL 4000E"; models = EngineController.compModels; pick = { engine.setCompModel(trackId, $0) }
        case .gate: name = tk?.consoleGateType ?? "SSL 4000E"; models = EngineController.gateModels; pick = { engine.setGateModel(trackId, $0) }
        default:    name = engine.consoleModel; models = EngineController.consoleModels; pick = { engine.setConsoleModel($0) }
        }
        return Menu {
            ForEach(models, id: \.self) { m in
                Button { pick(m) } label: { if m == name { Label(m, systemImage: "checkmark") } else { Text(m) } }
            }
        } label: {
            Text(name)
                .font(.system(size: 10, weight: .bold)).tracking(0.6).lineLimit(1).minimumScaleFactor(0.7)
                .foregroundStyle(Color(hex: 0x1d1e20))
                .frame(maxWidth: .infinity).frame(height: 22)
                .background(LinearGradient(colors: [Color(hex: 0xdedad0), Color(hex: 0xb8b4a9)], startPoint: .top, endPoint: .bottom))
                .clipShape(RoundedRectangle(cornerRadius: 3))
                .overlay(RoundedRectangle(cornerRadius: 3).stroke(.black, lineWidth: 1))
        }
        .menuStyle(.borderlessButton).menuIndicator(.hidden)
        .padding(.horizontal, 6).padding(.top, 3).padding(.bottom, 6)
        .frame(width: width)
    }

    @ViewBuilder private var moduleBody: some View {
        switch module {
        case .filter:    filters
        case .eq:        equaliser
        case .comp:      compress
        case .gate:      gate
        case .saturator: saturator
        default:         EmptyView()
        }
    }

    // A knob bound to a console parameter.
    private func knob(_ param: String, _ range: ClosedRange<Float>, _ def: Float,
                      _ color: ConsoleKnobColor, marks: [String], unit: String, markRadius: CGFloat = 12,
                      diameter: CGFloat = 50, markFont: CGFloat = 7, unitFont: CGFloat = 6.5,
                      unitAtZero: Bool = false, dotDiameter: CGFloat = 0, dimpleSize: CGFloat = 8,
                      extraBottom: CGFloat = 0, centerFormat: ((Float) -> String)? = nil,
                      wheelStep: Float = 0, wheelLog: Bool = false, logScale: Bool = false,
                      reverse: Bool = false, unitAbove: Bool = false) -> some View {
        ConsoleKnob(color: color, marks: marks, markRadius: markRadius, unit: unit,
                    value: engine.consoleValue(trackId, param), range: range, defaultValue: def,
                    diameter: diameter, markFont: markFont, unitFont: unitFont, unitAtZero: unitAtZero, unitAbove: unitAbove,
                    dotDiameter: dotDiameter, dimpleSize: dimpleSize,
                    extraBottom: extraBottom, centerFormat: centerFormat,
                    wheelStep: wheelStep, wheelLog: wheelLog, logScale: logScale, reverse: reverse,
                    onChange: { engine.setConsoleValue(trackId, param, $0) },
                    onCommit: { engine.recordGesture("4000E \(param)") })
    }

    private func placed(_ cx: CGFloat, _ cy: CGFloat, _ v: some View, size: CGFloat = 86) -> some View {
        v.frame(width: size, height: size).position(x: cx, y: cy)
    }

    // Round on/off lamp (also the enable switch) that sits at a knob's top-right, SSL-style: amber
    // glow when the band is engaged, dark when bypassed. Toggles the given enable param.
    private func filterLamp(_ param: String) -> some View {
        let on = engine.consoleBool(trackId, param)
        return Button {
            engine.setConsoleBool(trackId, param, !on); engine.recordGesture("4000E \(param)")
        } label: {
            Circle()
                .fill(on
                    ? RadialGradient(colors: [Color(hex: 0xffe6b0), Color(hex: 0xe0a23a), Color(hex: 0x7d4a0e)],
                                     center: UnitPoint(x: 0.36, y: 0.28), startRadius: 0, endRadius: 8)
                    : RadialGradient(colors: [Color(hex: 0x43454a), Color(hex: 0x191b1e)],
                                     center: UnitPoint(x: 0.36, y: 0.28), startRadius: 0, endRadius: 7))
                .frame(width: 15, height: 15)
                .overlay(Circle().stroke(.black.opacity(0.7), lineWidth: 1))
                .shadow(color: on ? Color(hex: 0xe0a94b).opacity(0.9) : .clear, radius: 5)
        }.buttonStyle(.plain)
        .help(on ? "필터 밴드 ON" : "필터 밴드 OFF")
    }

    // MARK: modules

    private var filters: some View {
        ConsoleModuleChrome(title: "FILTERS", modelName: engine.consoleModel, models: EngineController.consoleModels, onSelectModel: { engine.setConsoleModel($0) }, inOn: inOn, onToggleIn: onToggleIn) {
          VStack(spacing: 0) {
            let lx: CGFloat = 58, rx: CGFloat = 148, sz: CGFloat = 112
            ZStack {
                // Only the two end-point numbers around each knob, plus the unit. Low cut 20→350 Hz;
                // high cut 20 kHz (open) → 3 kHz (turn right = cut more, so LPF is reversed).
                placed(lx, 66, cKnob("highPassHz", 20...350, 20, .black, ["20", "350"], "Hz", Self.freqLabel, 0, log: true, unitAbove: true), size: sz)
                placed(rx, 120, cKnob("lowPassHz", 3000...20000, 20000, .black, ["20", "3"], "kHz", Self.freqLabel, 0, log: true, reverse: true, unitAbove: true), size: sz)
                FilterSlopeIcon(highPass: true).position(x: lx, y: 120)    // where the "HPF" label was
                FilterSlopeIcon(highPass: false).position(x: rx, y: 174)   // where the "LPF" label was
                // On/off lamp + switch at each knob's top-right, ~10pt clear of the knob rim.
                filterLamp("highPassEnabled").position(x: lx + 38, y: 28)
                filterLamp("lowPassEnabled").position(x: rx + 38, y: 82)
                // Circuit toggle below the HPF knob.
                circuitChip(.filter).position(x: 58, y: 152)
            }
            .frame(height: 210)
          }
        }
    }

    private var equaliser: some View {
        ConsoleModuleChrome(title: "EQUALISER", modelName: engine.consoleModel, models: EngineController.consoleModels, onSelectModel: { engine.setConsoleModel($0) }, inOn: inOn, onToggleIn: onToggleIn) {
          VStack(spacing: 0) {
            EqGraphView(engine: engine, trackId: trackId)
                .frame(height: 132).padding(.horizontal, 6).padding(.bottom, 6)
            // Gain: -/0/+ text. Freq: two end numbers + live value. Q: narrow/wide bell icons.
            let lx: CGFloat = 58, rx: CGFloat = 148, sz: CGFloat = 112
            ZStack {
                placed(lx, 52, eqGain("eqHfGainDb", .red), size: sz)
                placed(rx, 106, eqFreq("eqHfHz", 1500...16000, 8000, .red, ["1.5", "16"], "kHz"), size: sz)
                placed(lx, 162, eqGain("eqHmfGainDb", .green), size: sz)
                placed(rx, 216, eqFreq("eqHmfHz", 600...7000, 3000, .green, [".6", "7"], "kHz"), size: sz)
                placed(lx, 272, eqQ("eqHmfQ", .green), size: sz)
                placed(rx, 326, eqQ("eqLmfQ", .blue), size: sz)
                placed(lx, 382, eqGain("eqLmfGainDb", .blue), size: sz)
                placed(rx, 436, eqFreq("eqLmfHz", 400...2500, 1000, .blue, [".4", "2.5"], "kHz"), size: sz)
                placed(lx, 492, eqGain("eqLfGainDb", .brown), size: sz)
                placed(rx, 546, eqFreq("eqLfHz", 30...450, 200, .brown, ["30", "450"], "Hz"), size: sz)
                bellButton("eqHfBell", on: engine.consoleBool(trackId, "eqHfBell")).position(x: rx, y: 25)
                bellButton("eqLfBell", on: engine.consoleBool(trackId, "eqLfBell")).position(x: lx, y: 560)
                circuitChip(.eq).position(x: lx, y: 596)   // below the left (LF) bell button
            }
            .frame(height: 620)
          }
        }
    }

    // EQ knob categories. Shared: 60pt body, 14pt labels, 5pt dots 2pt off the rim.
    private func eqGain(_ param: String, _ color: ConsoleKnobColor) -> some View {
        knob(param, -18...18, 0, color, marks: gainMarks, unit: "",
             diameter: 73, markFont: 15, centerFormat: Self.dbLabel, wheelStep: 1)   // 1 dB per notch
    }
    private func eqFreq(_ param: String, _ range: ClosedRange<Float>, _ def: Float,
                        _ color: ConsoleKnobColor, _ marks: [String], _ unit: String) -> some View {
        knob(param, range, def, color, marks: marks, unit: unit, markRadius: 14,
             diameter: 73, markFont: 15, unitFont: 15,
             centerFormat: Self.freqLabel, wheelLog: true, logScale: true)   // log pointer + semitone wheel
    }
    private func eqQ(_ param: String, _ color: ConsoleKnobColor) -> some View {
        // Reversed: turning toward the wide-bell side lowers Q, narrow side raises it (SSL feel).
        knob(param, 0.2...10, 1, color, marks: ["QN", "QW"], unit: "",
             diameter: 73, markFont: 15, unitFont: 15, centerFormat: Self.qLabel, wheelStep: 0.4, reverse: true)
    }

    // Shared big knob for the non-EQ 4000E modules: end labels, live value on the face, name below.
    private func cKnob(_ param: String, _ range: ClosedRange<Float>, _ def: Float, _ color: ConsoleKnobColor,
                       _ ends: [String], _ name: String, _ fmt: @escaping (Float) -> String,
                       _ step: Float, log: Bool = false, reverse: Bool = false, unitAbove: Bool = false) -> some View {
        knob(param, range, def, color, marks: ends, unit: name, markRadius: 14,
             diameter: 73, markFont: 15, unitFont: 15, centerFormat: fmt,
             wheelStep: step, wheelLog: log, logScale: log, reverse: reverse, unitAbove: unitAbove)
    }

    static func dbLabel(_ v: Float) -> String { String(format: "%+.0f", v) }
    static func qLabel(_ v: Float) -> String { String(format: "%.1f", v) }
    static func msLabel(_ v: Float) -> String { v >= 1000 ? String(format: "%.1fs", v / 1000) : String(format: "%.0f", v) }
    static func ratioLabel(_ v: Float) -> String { String(format: "%.1f", v) }
    static func pctLabel(_ v: Float) -> String { String(format: "%.0f", v * 100) }
    static func intLabel(_ v: Float) -> String { String(format: "%.0f", v) }

    static func freqLabel(_ v: Float) -> String {
        if v >= 10000 { return String(format: "%.0fk", v / 1000) }
        if v >= 1000 { return String(format: "%.1fk", v / 1000) }
        return String(format: "%.0f", v)
    }

    // The per-module footer bar, reproduced from the Claude Design "Neuracoust Modules"
    // reference: an inset rail carrying the dual-mono/stereo link pill and the
    // circuit/perceptual mode button. The channel-type (model) selector lives in the
    // module chrome header, so these two are what the footer restores.
    private func circuitModeParam(_ module: MixerModuleFocus) -> String {
        switch module {
        case .filter:    return "filterCircuitMode"
        case .eq:        return "eqCircuitMode"
        case .comp:      return "compCircuitMode"
        case .gate:      return "gateCircuitMode"
        case .saturator: return "saturatorCircuitMode"
        default:         return "eqCircuitMode"
        }
    }

    // The knob-area mode buttons are drawn as GLYPHS (pictures), not text, so they stay legible at
    // the module's small scale. Each toggles its console bool; lit = active accent, dim = grey.
    private func glyphButton<Icon: View>(_ param: String, lit: Bool, accent: Color, w: CGFloat = 42,
                                         @ViewBuilder _ icon: () -> Icon) -> some View {
        Button {
            engine.setConsoleBool(trackId, param, !engine.consoleBool(trackId, param))
            engine.recordGesture("4000E \(param)")
        } label: {
            icon()
                .frame(width: w, height: 28)
                .background(
                    RoundedRectangle(cornerRadius: 4)
                        .fill(LinearGradient(colors: [Color(hex: 0x3a3f44), Color(hex: 0x1e2225)], startPoint: .top, endPoint: .bottom))
                        .overlay(RoundedRectangle(cornerRadius: 4).stroke(lit ? accent.opacity(0.55) : .black, lineWidth: 1))
                        .overlay(RoundedRectangle(cornerRadius: 4).stroke(.black, lineWidth: 1).opacity(0.6))
                )
        }.buttonStyle(.plain).help(param)
    }

    // Circuit ⇄ perceptual voicing (always "lit" — both are modes): amber circuit-wave glyph for
    // 회로, green ear-response glyph for 청감.
    private func circuitChip(_ module: MixerModuleFocus) -> some View {
        let param = circuitModeParam(module)
        let isCircuit = engine.consoleBool(trackId, param)
        let accent = isCircuit ? Color(hex: 0xe0a94b) : Color(hex: 0x5fd18a)
        return glyphButton(param, lit: true, accent: accent) {
            Group { if isCircuit { CircuitWaveIcon(color: accent).scaleEffect(1.15) } else { EarBarsIcon(color: accent).scaleEffect(1.15) } }
        }
    }
    // FAST ⇄ SLOW attack — steep vs gradual envelope glyph, amber when FAST.
    private func attackChip(_ param: String) -> some View {
        let fast = engine.consoleBool(trackId, param)
        let c = fast ? Color(hex: 0xe0a94b) : Color(hex: 0x9aa0a6)
        return glyphButton(param, lit: fast, accent: Color(hex: 0xe0a94b)) { AttackIcon(fast: fast, color: c) }
    }
    // EXP ⇄ GATE — soft slope vs hard step glyph, amber when EXP.
    private func expChip(_ param: String) -> some View {
        let exp = engine.consoleBool(trackId, param)
        let c = exp ? Color(hex: 0xe0a94b) : Color(hex: 0x9aa0a6)
        return glyphButton(param, lit: exp, accent: Color(hex: 0xe0a94b)) { GateExpIcon(exp: exp, color: c) }
    }
    // Ø polarity — circle-slash glyph, amber when inverted.
    private func phaseChip(_ param: String) -> some View {
        let on = engine.consoleBool(trackId, param)
        let c = on ? Color(hex: 0xe0a94b) : Color(hex: 0x9aa0a6)
        return glyphButton(param, lit: on, accent: Color(hex: 0xe0a94b), w: 32) { PhaseIcon(color: c) }
    }

    private func dualParam(_ module: MixerModuleFocus) -> String {
        switch module {
        case .filter:    return "filterDualMono"
        case .eq:        return "eqDualMono"
        case .comp:      return "compDualMono"
        case .gate:      return "gateDualMono"
        case .saturator: return "saturatorDualMono"
        default:         return "eqDualMono"
        }
    }

    // Per-module footer (Claude Design "SSL Module Footer"): a recessed rail in the module chassis
    // carrying two engraved, illuminated console keys — STEREO/DUAL and CIRCUIT/청감. Each module has
    // its OWN keys (independent). Rendered at the strip's real width so it stays legible. The
    // dual/stereo key is hidden on a mono track (no dual concept there).
    @ViewBuilder func moduleFooter(_ module: MixerModuleFocus) -> some View {
        let isMono = engine.tracks.first(where: { $0.id == trackId })?.isStereo == false
        // Stacked full-width keys so the engraved labels stay legible in the narrow (86pt) strip.
        VStack(spacing: 6) {
            if !isMono { dualKey(dualParam(module)) }
            circuitKey(circuitModeParam(module))
        }
        .padding(7)
        .background(
            RoundedRectangle(cornerRadius: 5)
                .fill(LinearGradient(colors: [Color(hex: 0x232527), Color(hex: 0x191b1d)], startPoint: .top, endPoint: .bottom))
                .overlay(RoundedRectangle(cornerRadius: 5).stroke(.black, lineWidth: 1))
        )
        .padding(.horizontal, 8).padding(.top, 2).padding(.bottom, 8)
        .frame(width: width)
    }

    // A mode switch that lives in the KNOB area (FAST/SLOW, PEAK/RMS, EXP/GATE, E/G), placed among
    // the knobs like the SSL reference — amber when the "on" side is active. Sized in the module's
    // own coordinate space (it scales with the knobs).
    private func knobChip(_ param: String, on: String, off: String, w: CGFloat = 52) -> some View {
        let lit = engine.consoleBool(trackId, param)
        return Button {
            engine.setConsoleBool(trackId, param, !lit); engine.recordGesture("4000E \(param)")
        } label: {
            Text(lit ? on : off)
                .font(.system(size: 13, weight: .bold)).tracking(0.6)
                .foregroundStyle(lit ? Color(hex: 0x2a1f10) : Color(hex: 0xded7c9))
                .frame(width: w, height: 26)
                .background(lit
                    ? AnyView(LinearGradient(colors: [Color(hex: 0xe0a94b), Color(hex: 0xa9741f)], startPoint: .top, endPoint: .bottom))
                    : AnyView(LinearGradient(colors: [Color(hex: 0x3d3f41), Color(hex: 0x232527)], startPoint: .top, endPoint: .bottom)))
                .clipShape(RoundedRectangle(cornerRadius: 4))
                .overlay(RoundedRectangle(cornerRadius: 4).stroke(.black, lineWidth: 1))
        }.buttonStyle(.plain)
        .help(param)
    }

    // STEREO (off) ⇄ DUAL (on, amber lamp) — this module's own dynamics stereo-link.
    private func dualKey(_ param: String) -> some View {
        let on = engine.consoleBool(trackId, param)
        return railKey(label: on ? "DUAL" : "STEREO", lit: on, accent: Color(hex: 0xe0a94b)) {
            engine.setConsoleBool(trackId, param, !on); engine.recordGesture("4000E \(param)")
        }
    }

    // CIRCUIT (amber) ⇄ 청감/perceptual (green). Always lit — the lamp colour shows the active voicing.
    private func circuitKey(_ param: String) -> some View {
        let isCircuit = engine.consoleBool(trackId, param)
        let accent = isCircuit ? Color(hex: 0xe0a94b) : Color(hex: 0x5fd18a)
        return railKey(label: isCircuit ? "CIRCUIT" : "청감", lit: true, accent: accent) {
            engine.setConsoleBool(trackId, param, !isCircuit); engine.recordGesture("4000E \(param)")
        }
    }

    // One engraved console key with a lamp dot; illuminates in `accent` when lit — the SSL hardware
    // look from the Claude Design footer, matching the module's IN key and chassis.
    private func railKey(label: String, lit: Bool, accent: Color, _ action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack(spacing: 6) {
                Circle()
                    .fill(lit
                        ? RadialGradient(colors: [.white, accent], center: UnitPoint(x: 0.36, y: 0.30), startRadius: 0, endRadius: 5)
                        : RadialGradient(colors: [Color(hex: 0x3a3d42), Color(hex: 0x191b1e)], center: UnitPoint(x: 0.36, y: 0.30), startRadius: 0, endRadius: 4))
                    .frame(width: 7, height: 7)
                    .shadow(color: lit ? accent.opacity(0.8) : .clear, radius: 3)
                Text(label)
                    .font(.system(size: 11, weight: .bold)).tracking(0.5)
                    .foregroundStyle(lit ? accent : Color(hex: 0x7f8790))
            }
            .frame(maxWidth: .infinity).frame(height: 30)
            .background(
                RoundedRectangle(cornerRadius: 4)
                    .fill(LinearGradient(colors: lit ? [Color(hex: 0x3a3f44), Color(hex: 0x1e2225)]
                                                     : [Color(hex: 0x2a2c30), Color(hex: 0x202225)],
                                         startPoint: .top, endPoint: .bottom))
                    .overlay(RoundedRectangle(cornerRadius: 4).stroke(.black, lineWidth: 1))
                    .overlay(lit ? RoundedRectangle(cornerRadius: 4).stroke(accent.opacity(0.4), lineWidth: 1) : nil)
            )
            .shadow(color: lit ? accent.opacity(0.30) : .clear, radius: 4)
        }.buttonStyle(.plain)
        .help(label)
    }

    private func bellButton(_ param: String, on: Bool) -> some View {
        Button {
            engine.setConsoleBool(trackId, param, !on); engine.recordGesture("4000E \(param)")
        } label: {
            QCurveIcon(wide: true)
                // When on, matches the module IN button: amber fill, dark ink, black border.
                .stroke(on ? Color(hex: 0x2a1f10) : Color(hex: 0xded7c9), style: StrokeStyle(lineWidth: 1.5, lineJoin: .round))
                .frame(width: 18, height: 11)
                .frame(width: 30, height: 24)
                .background(on
                    ? AnyView(LinearGradient(colors: [Color(hex: 0xe0a94b), Color(hex: 0xa9741f)], startPoint: .top, endPoint: .bottom))
                    : AnyView(LinearGradient(colors: [Color(hex: 0x3d3f41), Color(hex: 0x232527)], startPoint: .top, endPoint: .bottom)))
                .clipShape(RoundedRectangle(cornerRadius: 4))
                .overlay(RoundedRectangle(cornerRadius: 4).stroke(.black, lineWidth: 1))
        }.buttonStyle(.plain)
    }

    private var compress: some View {
        let tk = engine.tracks.first(where: { $0.id == trackId })
        let gr = tk?.consoleCompGainReductionDb ?? 0
        return ConsoleModuleChrome(title: "COMPRESS", modelName: tk?.consoleCompType ?? "SSL 4000E", models: EngineController.compModels, onSelectModel: { engine.setCompModel(trackId, $0) }, inOn: inOn, onToggleIn: onToggleIn) {
            VStack(spacing: 0) {
                MixBar(engine: engine, trackId: trackId)

                let lx: CGFloat = 58, rx: CGFloat = 148, sz: CGFloat = 112
                ZStack {
                    // Knob cluster raised toward the MIX bar (smaller top gap); FAST + circuit nudged down.
                    placed(lx, 50, cKnob("compRatio", 1...25, 5, .orange, ["1", "∞"], "RAT", Self.ratioLabel, 0, log: true), size: sz)
                    placed(rx, 104, cKnob("compThresholdDb", -20...10, 0, .orange, ["+10", "-20"], "THR", Self.intLabel, 1, reverse: true), size: sz)
                    placed(lx, 160, cKnob("compReleaseMs", 40...4000, 400, .orange, ["40", "4s"], "REL", Self.msLabel, 0, log: true), size: sz)
                    // FAST attack as text; circuit glyph below the THRESHOLD knob.
                    knobChip("compFastAttack", on: "FAST", off: "SLOW").position(x: 150, y: 44)
                    circuitChip(.comp).position(x: 148, y: 200)
                }
                .frame(height: 240)
                GrMeter(label: "GR", gr: gr)
                Spacer(minLength: 5)   // 5pt more room at the bottom
            }
        }
    }

    private var gate: some View {
        let tk = engine.tracks.first(where: { $0.id == trackId })
        let gr = tk?.consoleGateGainReductionDb ?? 0
        return ConsoleModuleChrome(title: "GATE / EXP", modelName: tk?.consoleGateType ?? "SSL 4000E", models: EngineController.gateModels, onSelectModel: { engine.setGateModel(trackId, $0) }, inOn: inOn, onToggleIn: onToggleIn) {
          VStack(spacing: 0) {
            let lx: CGFloat = 58, rx: CGFloat = 148, sz: CGFloat = 112
            ZStack {
                // Knobs shifted up to trim the empty top; EXP/circuit stack top-right, SLOW lower.
                placed(lx, 56, cKnob("gateRangeDb", 0...40, 20, .green, ["0", "40"], "RNG", Self.intLabel, 1), size: sz)
                placed(rx, 124, cKnob("gateThresholdDb", -30...5, -18, .orange, ["-30", "+5"], "THR", Self.intLabel, 1), size: sz)
                placed(lx, 176, cKnob("gateReleaseMs", 40...4000, 400, .green, ["40", "4s"], "REL", Self.msLabel, 0, log: true), size: sz)
                knobChip("expanderMode", on: "EXP", off: "GATE").position(x: 150, y: 30)   // EXP/GATE up top
                knobChip("gateFastAttack", on: "FAST", off: "SLOW").position(x: 150, y: 62) // SLOW below EXP
                circuitChip(.gate).position(x: 150, y: 208)                                  // circuit further down (swapped)
            }
            .frame(height: 250)
            GrMeter(label: "GR", gr: gr)
          }
        }
    }

    private var saturator: some View {
        ConsoleModuleChrome(title: "SATURATOR", modelName: engine.consoleModel, models: EngineController.consoleModels, onSelectModel: { engine.setConsoleModel($0) }, inOn: inOn, onToggleIn: onToggleIn) {
          VStack(spacing: 0) {
            let lx: CGFloat = 58, rx: CGFloat = 148, sz: CGFloat = 112
            ZStack {
                placed(lx, 72, cKnob("saturatorDriveDb", 0...24, 6, .orange, ["0", "24"], "DRIVE", Self.intLabel, 1), size: sz)
                placed(rx, 126, cKnob("saturatorMix", 0...1, 1, .orange, ["0", "100"], "MIX", Self.pctLabel, 0.05), size: sz)
                // Channel polarity per side (ØL / ØR) as symbols; circuit glyph below the DRIVE knob.
                knobChip("phaseInvertL", on: "ØL", off: "ØL", w: 34).position(x: 150, y: 36)
                knobChip("phaseInvertR", on: "ØR", off: "ØR", w: 34).position(x: 150, y: 66)
                circuitChip(.saturator).position(x: 58, y: 150)
            }
            .frame(height: 210)
          }
        }
    }
}
