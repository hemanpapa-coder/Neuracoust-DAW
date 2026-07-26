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
                let dy = precise ? event.scrollingDeltaY : event.deltaY
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
    private var valueDeg: Double { markStart + normalized * (markEnd - markStart) }

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
                    next = min(hi, max(lo, start + Float(-drag.translation.height / 90) * (hi - lo)))
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
        let dir = Float(notches)
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
                        .font(.system(size: markFont, design: .monospaced))
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
        case .filter:    return 226
        case .eq:        return 696
        case .comp:      return 356
        case .gate:      return 326
        case .saturator: return 226
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
                      wheelStep: Float = 0, wheelLog: Bool = false, logScale: Bool = false) -> some View {
        ConsoleKnob(color: color, marks: marks, markRadius: markRadius, unit: unit,
                    value: engine.consoleValue(trackId, param), range: range, defaultValue: def,
                    diameter: diameter, markFont: markFont, unitFont: unitFont, unitAtZero: unitAtZero,
                    dotDiameter: dotDiameter, dimpleSize: dimpleSize,
                    extraBottom: extraBottom, centerFormat: centerFormat,
                    wheelStep: wheelStep, wheelLog: wheelLog, logScale: logScale,
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
                placed(rx, 72, cKnob("lowPassHz", 3000...12000, 12000, .black, ["3k", "12k"], "LPF", Self.freqLabel, 0, log: true), size: sz)
            }
            .frame(height: 150)
        }
    }

    private var equaliser: some View {
        ConsoleModuleChrome(title: "EQUALISER", modelName: engine.consoleModel, models: EngineController.consoleModels, onSelectModel: { engine.setConsoleModel($0) }, inOn: inOn, onToggleIn: onToggleIn) {
            // Gain: dot scale (dots 3×, 2pt off the knob) with -/0/+ text and a 2× dimple.
            // Freq: only the two end numbers, the rest dots. Q: narrow/wide bell icons + dots.
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
                       _ step: Float, log: Bool = false) -> some View {
        knob(param, range, def, color, marks: ends, unit: name, markRadius: 14,
             diameter: 73, markFont: 15, unitFont: 15, centerFormat: fmt, wheelStep: step, wheelLog: log, logScale: log)
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
                HStack(spacing: 7) {
                    let mix = engine.consoleValue(trackId, "compMix")
                    ZStack(alignment: .leading) {
                        RoundedRectangle(cornerRadius: 3).fill(Color(hex: 0x0a1410))
                        GeometryReader { g in
                            RoundedRectangle(cornerRadius: 2).fill(Color(hex: 0x54e08a).opacity(0.3))
                                .frame(width: g.size.width * CGFloat(mix))
                        }
                        Text(String(format: "%.0f%%", mix * 100)).font(.system(size: 11, design: .monospaced))
                            .foregroundStyle(Color(hex: 0x7dffb4)).frame(maxWidth: .infinity)
                    }
                    .frame(height: 22).overlay(RoundedRectangle(cornerRadius: 3).stroke(.black, lineWidth: 1))
                    .contentShape(Rectangle())
                    .gesture(DragGesture(minimumDistance: 0).onChanged { d in
                        let w = max(1, width == 205 ? 150 : (width - 55))   // approx bar width; fine for control feel
                        let v = Float(max(0, min(1, d.location.x / w)))
                        engine.setConsoleValue(trackId, "compMix", v)
                    }.onEnded { _ in engine.recordGesture("4000E compMix") })
                    Text("MIX").font(.system(size: 13, weight: .bold)).tracking(1.2).foregroundStyle(Color(hex: 0xe6dfd0))
                }
                .padding(.horizontal, 10).frame(height: 30)

                let lx: CGFloat = 58, rx: CGFloat = 148, sz: CGFloat = 112
                ZStack {
                    placed(lx, 66, cKnob("compRatio", 1...20, 4, .silver, ["1", "20"], "RATIO", Self.ratioLabel, 0.5), size: sz)
                    placed(rx, 66, cKnob("compThresholdDb", -40...0, -18, .silver, ["0", "-40"], "THRESH", Self.intLabel, 1), size: sz)
                    placed(lx, 176, cKnob("compReleaseMs", 40...1500, 360, .silver, ["40", "1.5s"], "RELEASE", Self.msLabel, 10), size: sz)
                }
                .frame(height: 250)
            }
        }
    }

    private var gate: some View {
        ConsoleModuleChrome(title: "GATE / EXP", modelName: engine.consoleModel, models: EngineController.consoleModels, onSelectModel: { engine.setConsoleModel($0) }, inOn: inOn, onToggleIn: onToggleIn) {
            let lx: CGFloat = 58, rx: CGFloat = 148, sz: CGFloat = 112
            ZStack {
                placed(lx, 66, cKnob("gateRangeDb", 0...40, 20, .green, ["0", "40"], "RANGE", Self.intLabel, 1), size: sz)
                placed(rx, 66, cKnob("gateThresholdDb", -60...0, -36, .silver, ["0", "-60"], "THRESH", Self.intLabel, 1), size: sz)
                placed(lx, 176, cKnob("gateReleaseMs", 40...1500, 360, .green, ["40", "1.5s"], "RELEASE", Self.msLabel, 10), size: sz)
            }
            .frame(height: 250)
        }
    }

    private var saturator: some View {
        ConsoleModuleChrome(title: "SATURATOR", modelName: engine.consoleModel, models: EngineController.consoleModels, onSelectModel: { engine.setConsoleModel($0) }, inOn: inOn, onToggleIn: onToggleIn) {
            let lx: CGFloat = 58, rx: CGFloat = 148, sz: CGFloat = 112
            ZStack {
                placed(lx, 72, cKnob("saturatorDriveDb", 0...24, 6, .silver, ["0", "24"], "DRIVE", Self.intLabel, 1), size: sz)
                placed(rx, 72, cKnob("saturatorMix", 0...1, 1, .silver, ["0", "100"], "MIX", Self.pctLabel, 0.05), size: sz)
            }
            .frame(height: 150)
        }
    }
}
