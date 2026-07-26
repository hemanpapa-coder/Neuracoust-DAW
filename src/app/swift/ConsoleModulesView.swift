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
            // Pointer line (8×4), near the rim, in the cap's marker color.
            RoundedRectangle(cornerRadius: 1.5)
                .fill(color.dot)
                .frame(width: 4, height: 8)
                .offset(y: -(diameter / 2 - 7))
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
        .frame(width: diameter + markRadius * 2 + 12, height: diameter + markRadius * 2 + 10 + extraBottom)
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
            let r = diameter / 2 + markRadius
            let dotR = diameter / 2 + dotGap + dotDiameter / 2
            ForEach(Array(marks.enumerated()), id: \.offset) { i, label in
                let t = marks.count > 1 ? Double(i) / Double(marks.count - 1) : 0.5
                let a = (markStart + (markEnd - markStart) * t) * .pi / 180
                if label == "·" && dotDiameter > 0 {
                    Circle()
                        .fill(ConsoleKnobColor.bezel)
                        .frame(width: dotDiameter, height: dotDiameter)
                        .position(x: cx + dotR * sin(a), y: cy - dotR * cos(a))
                } else if label == "QN" || label == "QW" {
                    QCurveIcon(wide: label == "QW")
                        .stroke(ConsoleKnobColor.bezel, style: StrokeStyle(lineWidth: 1.2, lineJoin: .round))
                        .frame(width: 16, height: 10)
                        .position(x: cx + r * sin(a), y: cy - r * cos(a))
                } else if unitAtZero && label == "0" {
                    Text(unit)
                        .font(.system(size: markFont, design: .monospaced))
                        .foregroundStyle(ConsoleKnobColor.bezel)
                        .position(x: cx + r * sin(a), y: cy - r * cos(a))
                } else {
                    Text(label)
                        .font(.system(size: label == "∞" ? markFont * 2 : markFont, design: .monospaced))
                        .foregroundStyle(ConsoleKnobColor.bezel)
                        .position(x: cx + r * sin(a), y: cy - r * cos(a))
                }
            }
            if !unit.isEmpty && !(unitAtZero && marks.contains("0")) {
                Text(unit)
                    .font(.system(size: unitFont, design: .monospaced))
                    .foregroundStyle(ConsoleKnobColor.bezel)
                    .position(x: cx, y: cy + diameter / 2 + 2 + unitFont / 2)   // 2pt below the rim
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

            // Name plate = console model selector. Pick a different console to switch the model.
            Menu {
                ForEach(models, id: \.self) { m in
                    Button { onSelectModel(m) } label: {
                        if m == modelName { Label(m, systemImage: "checkmark") } else { Text(m) }
                    }
                }
            } label: {
                Text(modelName).font(.system(size: 11, weight: .bold)).tracking(1.1).foregroundStyle(Color(hex: 0x1d1e20))
                    .frame(maxWidth: .infinity).frame(height: 26)
                    .background(LinearGradient(colors: [Color(hex: 0xdedad0), Color(hex: 0xb8b4a9)], startPoint: .top, endPoint: .bottom))
                    .clipShape(RoundedRectangle(cornerRadius: 3))
                    .overlay(RoundedRectangle(cornerRadius: 3).stroke(.black, lineWidth: 1))
            }
            .menuStyle(.borderlessButton)
            .menuIndicator(.hidden)
            .padding(8)
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
        case .filter:    return 286
        case .eq:        return 840
        case .comp:      return 364
        case .gate:      return 334
        case .saturator: return 286
        default:         return 300
        }
    }

    var body: some View {
        let scale = min(1, width / 205)
        moduleBody
            .scaleEffect(scale, anchor: .top)
            .frame(width: width, height: moduleHeight(module) * scale, alignment: .top)
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
                      reverse: Bool = false) -> some View {
        ConsoleKnob(color: color, marks: marks, markRadius: markRadius, unit: unit,
                    value: engine.consoleValue(trackId, param), range: range, defaultValue: def,
                    diameter: diameter, markFont: markFont, unitFont: unitFont, unitAtZero: unitAtZero,
                    dotDiameter: dotDiameter, dimpleSize: dimpleSize,
                    extraBottom: extraBottom, centerFormat: centerFormat,
                    wheelStep: wheelStep, wheelLog: wheelLog, logScale: logScale, reverse: reverse,
                    onChange: { engine.setConsoleValue(trackId, param, $0) },
                    onCommit: { engine.recordGesture("4000E \(param)") })
    }

    private func placed(_ cx: CGFloat, _ cy: CGFloat, _ v: some View, size: CGFloat = 86) -> some View {
        v.frame(width: size, height: size).position(x: cx, y: cy)
    }

    // MARK: modules

    private var filters: some View {
        ConsoleModuleChrome(title: "FILTERS", modelName: engine.consoleModel, models: EngineController.consoleModels, onSelectModel: { engine.setConsoleModel($0) }, inOn: inOn, onToggleIn: onToggleIn) {
            let lx: CGFloat = 58, rx: CGFloat = 148, sz: CGFloat = 112
            ZStack {
                placed(lx, 72, cKnob("highPassHz", 20...350, 20, .black, ["20", "350"], "HPF", Self.freqLabel, 0, log: true), size: sz)
                placed(rx, 126, cKnob("lowPassHz", 3000...12000, 12000, .black, ["3k", "12k"], "LPF", Self.freqLabel, 0, log: true), size: sz)
            }
            .frame(height: 210)
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
        knob(param, 0.2...10, 1, color, marks: ["QN", "QW"], unit: "",
             diameter: 73, markFont: 15, unitFont: 15, centerFormat: Self.qLabel, wheelStep: 0.4)
    }

    // Shared big knob for the non-EQ 4000E modules: end labels, live value on the face, name below.
    private func cKnob(_ param: String, _ range: ClosedRange<Float>, _ def: Float, _ color: ConsoleKnobColor,
                       _ ends: [String], _ name: String, _ fmt: @escaping (Float) -> String,
                       _ step: Float, log: Bool = false, reverse: Bool = false) -> some View {
        knob(param, range, def, color, marks: ends, unit: name, markRadius: 14,
             diameter: 73, markFont: 15, unitFont: 15, centerFormat: fmt,
             wheelStep: step, wheelLog: log, logScale: log, reverse: reverse)
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

    private func bellButton(_ param: String, on: Bool) -> some View {
        Button {
            engine.setConsoleBool(trackId, param, !on); engine.recordGesture("4000E \(param)")
        } label: {
            QCurveIcon(wide: true)
                .stroke(on ? Color(hex: 0x5a3f1e) : Color(hex: 0xded7c9), style: StrokeStyle(lineWidth: 1.5, lineJoin: .round))
                .frame(width: 18, height: 11)
                .frame(width: 30, height: 24)
                .background(on
                    ? AnyView(LinearGradient(colors: [Color(hex: 0xf6d6a4), Color(hex: 0xeec384)], startPoint: .top, endPoint: .bottom))
                    : AnyView(LinearGradient(colors: [Color(hex: 0x3d3f41), Color(hex: 0x232527)], startPoint: .top, endPoint: .bottom)))
                .clipShape(RoundedRectangle(cornerRadius: 4))
                .overlay(RoundedRectangle(cornerRadius: 4).stroke(on ? Color(hex: 0xf7e0be) : .black, lineWidth: 1))
                .shadow(color: on ? Color(hex: 0xf3cd9a).opacity(0.95) : .clear, radius: 6)   // peach light when on
        }.buttonStyle(.plain)
    }

    private var compress: some View {
        ConsoleModuleChrome(title: "COMPRESS", modelName: engine.consoleModel, models: EngineController.consoleModels, onSelectModel: { engine.setConsoleModel($0) }, inOn: inOn, onToggleIn: onToggleIn) {
            VStack(spacing: 0) {
                MixBar(engine: engine, trackId: trackId)

                let lx: CGFloat = 58, rx: CGFloat = 148, sz: CGFloat = 112
                ZStack {
                    placed(lx, 66, cKnob("compRatio", 1...20, 4, .orange, ["1", "∞"], "RAT", Self.ratioLabel, 0.5), size: sz)
                    placed(rx, 120, cKnob("compThresholdDb", -20...10, 0, .orange, ["+10", "-20"], "THR", Self.intLabel, 1, reverse: true), size: sz)
                    placed(lx, 176, cKnob("compReleaseMs", 100...1500, 360, .orange, [".1", "1.5"], "REL", Self.msLabel, 10), size: sz)
                }
                .frame(height: 258)
            }
        }
    }

    private var gate: some View {
        ConsoleModuleChrome(title: "GATE / EXP", modelName: engine.consoleModel, models: EngineController.consoleModels, onSelectModel: { engine.setConsoleModel($0) }, inOn: inOn, onToggleIn: onToggleIn) {
            let lx: CGFloat = 58, rx: CGFloat = 148, sz: CGFloat = 112
            ZStack {
                placed(lx, 66, cKnob("gateRangeDb", 0...40, 20, .green, ["0", "40"], "RNG", Self.intLabel, 1), size: sz)
                placed(rx, 120, cKnob("gateThresholdDb", -30...5, -18, .orange, ["-30", "+5"], "THR", Self.intLabel, 1), size: sz)
                placed(lx, 176, cKnob("gateReleaseMs", 100...1500, 360, .green, [".1", "1.5"], "REL", Self.msLabel, 10), size: sz)
            }
            .frame(height: 258)
        }
    }

    private var saturator: some View {
        ConsoleModuleChrome(title: "SATURATOR", modelName: engine.consoleModel, models: EngineController.consoleModels, onSelectModel: { engine.setConsoleModel($0) }, inOn: inOn, onToggleIn: onToggleIn) {
            let lx: CGFloat = 58, rx: CGFloat = 148, sz: CGFloat = 112
            ZStack {
                placed(lx, 72, cKnob("saturatorDriveDb", 0...24, 6, .orange, ["0", "24"], "DRIVE", Self.intLabel, 1), size: sz)
                placed(rx, 126, cKnob("saturatorMix", 0...1, 1, .orange, ["0", "100"], "MIX", Self.pctLabel, 0.05), size: sz)
            }
            .frame(height: 210)
        }
    }
}
