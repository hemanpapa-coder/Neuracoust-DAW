import SwiftUI
import AppKit

// Scroll-wheel adjust for a knob: a zero-footprint view that never hit-tests (so it blocks no
// clicks/drags), and reports wheel deltas only while the cursor is over the knob, via a local
// scroll-event monitor.
private struct KnobScrollWheel: NSViewRepresentable {
    var onScroll: (CGFloat, Bool) -> Void   // (delta, isPreciseTrackpad)
    func makeNSView(context: Context) -> NSView {
        let v = Catcher(); v.onScroll = onScroll; return v
    }
    func updateNSView(_ nsView: NSView, context: Context) {
        (nsView as? Catcher)?.onScroll = onScroll
    }
    final class Catcher: NSView {
        var onScroll: ((CGFloat, Bool) -> Void)?
        private var monitor: Any?
        override func hitTest(_ point: NSPoint) -> NSView? { nil }   // mouse passes straight through
        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            if let monitor { NSEvent.removeMonitor(monitor); self.monitor = nil }
            guard window != nil else { return }
            monitor = NSEvent.addLocalMonitorForEvents(matching: .scrollWheel) { [weak self] event in
                guard let self, let win = self.window, event.window === win else { return event }
                let rect = self.convert(self.bounds, to: nil)
                if rect.contains(event.locationInWindow) {
                    let precise = event.hasPreciseScrollingDeltas
                    let dy = precise ? event.scrollingDeltaY : event.deltaY
                    if dy != 0 { self.onScroll?(dy, precise); return nil }
                }
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
    static let black  = ConsoleKnobColor(mid: Color(hex: 0x1b1c1f), lo: Color(hex: 0x070806), dot: Color(hex: 0xf4f1e8))
    static let silver = ConsoleKnobColor(mid: Color(hex: 0xa29c8d), lo: Color(hex: 0x605c52), dot: Color(hex: 0x2a2a2a))
    static let red    = ConsoleKnobColor(mid: Color(hex: 0x8a2519), lo: Color(hex: 0x450f09), dot: Color(hex: 0xf4f1e8))
    static let green  = ConsoleKnobColor(mid: Color(hex: 0x1f6534), lo: Color(hex: 0x0b2e17), dot: Color(hex: 0xf4f1e8))
    static let blue   = ConsoleKnobColor(mid: Color(hex: 0x25578c), lo: Color(hex: 0x0f2b47), dot: Color(hex: 0xf4f1e8))
    static let brown  = ConsoleKnobColor(mid: Color(hex: 0x563628), lo: Color(hex: 0x241310), dot: Color(hex: 0xf4f1e8))
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
    var onChange: (Float) -> Void = { _ in }
    var onCommit: () -> Void = {}
    @State private var dragStart: Float?
    @State private var liveValue: Float?
    @State private var scrollCommit: DispatchWorkItem?
    @State private var scrollAccum: CGFloat = 0

    private var normalized: Double {
        Double(((liveValue ?? value) - range.lowerBound) / max(0.0001, range.upperBound - range.lowerBound))
    }
    private var valueDeg: Double { markStart + normalized * (markEnd - markStart) }

    var body: some View {
        ZStack {
            // Bezel — spun aluminium collar (lighter, gradient reaches the full rim).
            Circle()
                .fill(RadialGradient(colors: [Color(hex: 0xa9aaa1), Color(hex: 0x63645d), Color(hex: 0x3d3e38)],
                                     center: UnitPoint(x: 0.40, y: 0.24), startRadius: 1, endRadius: diameter / 2))
                .overlay(AngularGradient(gradient: Gradient(colors: [
                    .white.opacity(0.20), .black.opacity(0.16), .white.opacity(0.14),
                    .black.opacity(0.18), .white.opacity(0.20)]), center: .center, angle: .degrees(208))
                    .clipShape(Circle()).opacity(0.55))
                .overlay(Circle().stroke(Color(hex: 0x2b2c28), lineWidth: 1))
                .shadow(color: .black.opacity(0.32), radius: 3, y: 2)
                .frame(width: diameter, height: diameter)
            // Matte colored cap.
            Circle()
                .fill(color.mid)
                .overlay(Circle().fill(LinearGradient(colors: [.white.opacity(0.10), .clear, .black.opacity(0.40)],
                                                      startPoint: .top, endPoint: .bottom)))
                .overlay(Circle().fill(RadialGradient(colors: [.white.opacity(0.14), .clear],
                                                      center: UnitPoint(x: 0.5, y: 0.30), startRadius: 0, endRadius: 22)))
                .overlay(Circle().stroke(.white.opacity(0.07), lineWidth: 1))
                .shadow(color: .black.opacity(0.45), radius: 2, y: 1)
                .frame(width: diameter - 8, height: diameter - 8)
            // Pointer line (5×2), near the rim, in the cap's marker color.
            RoundedRectangle(cornerRadius: 1)
                .fill(color.dot)
                .frame(width: 2, height: 5)
                .offset(y: -(diameter / 2 - 6))
                .rotationEffect(.degrees(valueDeg))
            // Live value on the knob face (e.g. current frequency).
            if let centerFormat {
                Text(centerFormat(liveValue ?? value))
                    .font(.system(size: 12, weight: .semibold, design: .monospaced))
                    .foregroundStyle(color.dot)
                    .lineLimit(1)
                    .fixedSize()
            }
        }
        .frame(width: diameter + markRadius * 2 + 12, height: diameter + markRadius * 2 + 10 + extraBottom)
        .overlay(marksOverlay)
        .overlay(KnobScrollWheel { dy, precise in applyScroll(dy, precise) }.frame(width: diameter, height: diameter))
        .contentShape(Rectangle())
        .gesture(DragGesture(minimumDistance: 1)
            .onChanged { drag in
                let start = dragStart ?? value
                if dragStart == nil { dragStart = start }
                let span = range.upperBound - range.lowerBound
                let next = min(range.upperBound, max(range.lowerBound, start + Float(-drag.translation.height / 90) * span))
                liveValue = next; onChange(next)
            }
            .onEnded { _ in dragStart = nil; liveValue = nil; onCommit() })
        .highPriorityGesture(TapGesture(count: 2).onEnded {
            onChange(defaultValue); onCommit()
        })
    }

    // Wheel over the knob: one detent = one step (1 dB for gain, a semitone for freq, etc.);
    // change live, then record one undo step once scrolling settles.
    private func applyScroll(_ delta: CGFloat, _ precise: Bool) {
        var notches = 0
        if precise {
            scrollAccum += delta
            let perNotch: CGFloat = 10          // ~10pt of trackpad travel = one step
            let n = (scrollAccum / perNotch).rounded(.towardZero)
            if n != 0 { scrollAccum -= n * perNotch; notches = Int(n) }
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
                        .fill(Color(hex: 0xb9b3a6))
                        .frame(width: dotDiameter, height: dotDiameter)
                        .position(x: cx + dotR * sin(a), y: cy - dotR * cos(a))
                } else if label == "QN" || label == "QW" {
                    QCurveIcon(wide: label == "QW")
                        .stroke(Color(hex: 0xb9b3a6), style: StrokeStyle(lineWidth: 1.2, lineJoin: .round))
                        .frame(width: 16, height: 10)
                        .position(x: cx + r * sin(a), y: cy - r * cos(a))
                } else if unitAtZero && label == "0" {
                    Text(unit)
                        .font(.system(size: markFont, design: .monospaced))
                        .foregroundStyle(Color(hex: 0xd8d2c4))
                        .position(x: cx + r * sin(a), y: cy - r * cos(a))
                } else {
                    Text(label)
                        .font(.system(size: markFont, design: .monospaced))
                        .foregroundStyle(Color(hex: 0xb9b3a6))
                        .position(x: cx + r * sin(a), y: cy - r * cos(a))
                }
            }
            if !unit.isEmpty && !(unitAtZero && marks.contains("0")) {
                Text(unit)
                    .font(.system(size: unitFont, design: .monospaced))
                    .foregroundStyle(Color(hex: 0x8d8878))
                    .position(x: cx, y: geo.size.height - 3)
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
        case .filter: return 188
        case .eq:     return 676
        case .comp:   return 372
        case .gate:   return 330
        default:      return 300
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
        case .filter: filters
        case .eq:     equaliser
        case .comp:   compress
        case .gate:   gate
        default:      EmptyView()
        }
    }

    // A knob bound to a console parameter.
    private func knob(_ param: String, _ range: ClosedRange<Float>, _ def: Float,
                      _ color: ConsoleKnobColor, marks: [String], unit: String, markRadius: CGFloat = 12,
                      diameter: CGFloat = 50, markFont: CGFloat = 7, unitFont: CGFloat = 6.5,
                      unitAtZero: Bool = false, dotDiameter: CGFloat = 0, dimpleSize: CGFloat = 8,
                      extraBottom: CGFloat = 0, centerFormat: ((Float) -> String)? = nil,
                      wheelStep: Float = 0, wheelLog: Bool = false) -> some View {
        ConsoleKnob(color: color, marks: marks, markRadius: markRadius, unit: unit,
                    value: engine.consoleValue(trackId, param), range: range, defaultValue: def,
                    diameter: diameter, markFont: markFont, unitFont: unitFont, unitAtZero: unitAtZero,
                    dotDiameter: dotDiameter, dimpleSize: dimpleSize,
                    extraBottom: extraBottom, centerFormat: centerFormat,
                    wheelStep: wheelStep, wheelLog: wheelLog,
                    onChange: { engine.setConsoleValue(trackId, param, $0) },
                    onCommit: { engine.recordGesture("4000E \(param)") })
    }

    private func placed(_ cx: CGFloat, _ cy: CGFloat, _ v: some View, size: CGFloat = 86) -> some View {
        v.frame(width: size, height: size).position(x: cx, y: cy)
    }

    // MARK: modules

    private var filters: some View {
        ConsoleModuleChrome(title: "FILTERS", modelName: engine.consoleModel, models: EngineController.consoleModels, onSelectModel: { engine.setConsoleModel($0) }, inOn: inOn, onToggleIn: onToggleIn) {
            ZStack {
                placed(148, 56, knob("lowPassHz", 3000...12000, 12000, .black,
                                     marks: ["12", "8", "5", "3.5", "3"], unit: "kHz", markRadius: 11))
                placed(56, 56, knob("highPassHz", 20...350, 20, .black,
                                    marks: ["20", "30", "50", "70", "120", "200", "300", "350"], unit: "Hz", markRadius: 11))
            }
            .frame(height: 112)
        }
    }

    private var equaliser: some View {
        ConsoleModuleChrome(title: "EQUALISER", modelName: engine.consoleModel, models: EngineController.consoleModels, onSelectModel: { engine.setConsoleModel($0) }, inOn: inOn, onToggleIn: onToggleIn) {
            // Gain: dot scale (dots 3×, 2pt off the knob) with -/0/+ text and a 2× dimple.
            // Freq: only the two end numbers, the rest dots. Q: narrow/wide bell icons + dots.
            let lx: CGFloat = 58, rx: CGFloat = 148, sz: CGFloat = 112
            ZStack {
                placed(lx, 52, eqGain("eqHfGainDb", .red), size: sz)
                placed(rx, 86, eqFreq("eqHfHz", 4000...16000, 8000, .red, ["1.5", "16"], "kHz"), size: sz)
                placed(lx, 162, eqGain("eqHmfGainDb", .green), size: sz)
                placed(rx, 196, eqFreq("eqHmfHz", 1200...7500, 3000, .green, [".6", "7"], "kHz"), size: sz)
                placed(lx, 272, eqQ("eqHmfQ", .green), size: sz)
                placed(rx, 306, eqQ("eqLmfQ", .blue), size: sz)
                placed(lx, 382, eqGain("eqLmfGainDb", .blue), size: sz)
                placed(rx, 416, eqFreq("eqLmfHz", 400...2500, 1000, .blue, [".4", "2.5"], "kHz"), size: sz)
                placed(lx, 492, eqGain("eqLfGainDb", .brown), size: sz)
                placed(rx, 526, eqFreq("eqLfHz", 90...450, 200, .brown, ["30", "450"], "Hz"), size: sz)
                bellButton("eqHfBell", on: engine.consoleValue(trackId, "eqHfBell") > 0.5).position(x: rx, y: 25)
                bellButton("eqLfBell", on: engine.consoleValue(trackId, "eqLfBell") > 0.5).position(x: lx, y: 555)
            }
            .frame(height: 600)
        }
    }

    // EQ knob categories. Shared: 60pt body, 14pt labels, 5pt dots 2pt off the rim.
    private func eqGain(_ param: String, _ color: ConsoleKnobColor) -> some View {
        knob(param, -18...18, 0, color, marks: gainMarks, unit: "",
             diameter: 66, markFont: 15, wheelStep: 1)                 // 1 dB per notch
    }
    private func eqFreq(_ param: String, _ range: ClosedRange<Float>, _ def: Float,
                        _ color: ConsoleKnobColor, _ marks: [String], _ unit: String) -> some View {
        // Live frequency in the knob face; unit dropped clear of the rim below.
        knob(param, range, def, color, marks: marks, unit: unit,
             diameter: 66, markFont: 15, unitFont: 15,
             extraBottom: 15, centerFormat: Self.freqLabel, wheelLog: true)   // one semitone per notch
    }
    private func eqQ(_ param: String, _ color: ConsoleKnobColor) -> some View {
        knob(param, 0.2...10, 1, color, marks: ["QN", "QW"], unit: "",
             diameter: 66, markFont: 15, unitFont: 15, wheelStep: 0.1)
    }

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

                ZStack {
                    placed(52, 56, knob("compRatio", 1...20, 4, .silver, marks: ["2", "3", "5", "8", "∞"], unit: "RATIO"))
                    placed(152, 90, knob("compThresholdDb", -40...0, -18, .silver, marks: ["0", "-5", "-10", "-15", "-20", "-30", "-40"], unit: "THRESHOLD", markRadius: 11))
                    placed(52, 142, knob("compReleaseMs", 40...1500, 360, .silver, marks: [".04", ".1", ".3", ".6", "1.5"], unit: "RELEASE"))
                }
                .frame(height: 190)
            }
        }
    }

    private var gate: some View {
        ConsoleModuleChrome(title: "GATE / EXP", modelName: engine.consoleModel, models: EngineController.consoleModels, onSelectModel: { engine.setConsoleModel($0) }, inOn: inOn, onToggleIn: onToggleIn) {
            ZStack {
                placed(152, 56, knob("gateThresholdDb", -60...0, -36, .silver, marks: ["0", "-10", "-20", "-30", "-40", "-50", "-60"], unit: "THRESHOLD", markRadius: 11))
                placed(56, 56, knob("gateRangeDb", 0...40, 20, .green, marks: ["0", "5", "10", "20", "30", "35", "40"], unit: "RANGE"))
                placed(56, 142, knob("gateReleaseMs", 40...1500, 360, .green, marks: [".04", ".1", ".3", ".6", "1.5"], unit: "RELEASE"))
            }
            .frame(height: 222)
        }
    }
}
