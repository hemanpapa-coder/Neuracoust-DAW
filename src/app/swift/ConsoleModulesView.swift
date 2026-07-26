import SwiftUI

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
    var onChange: (Float) -> Void = { _ in }
    var onCommit: () -> Void = {}
    @State private var dragStart: Float?
    @State private var liveValue: Float?

    private var normalized: Double {
        Double(((liveValue ?? value) - range.lowerBound) / max(0.0001, range.upperBound - range.lowerBound))
    }
    private var valueDeg: Double { markStart + normalized * (markEnd - markStart) }

    var body: some View {
        ZStack {
            // Bezel — spun aluminium collar.
            Circle()
                .fill(RadialGradient(colors: [Color(hex: 0x6a6b64), Color(hex: 0x232420), Color(hex: 0x08090a)],
                                     center: UnitPoint(x: 0.40, y: 0.24), startRadius: 1, endRadius: 26))
                .overlay(AngularGradient(gradient: Gradient(colors: [
                    .white.opacity(0.22), .black.opacity(0.30), .white.opacity(0.15),
                    .black.opacity(0.32), .white.opacity(0.22)]), center: .center, angle: .degrees(208))
                    .clipShape(Circle()).opacity(0.65))
                .overlay(Circle().stroke(.black, lineWidth: 1))
                .shadow(color: .black.opacity(0.5), radius: 4, y: 3)
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
            // Carved dimple pointer.
            Circle()
                .fill(Color.black.opacity(0.55))
                .overlay(Circle().fill(color.dot).frame(width: 4, height: 4))
                .frame(width: 8, height: 8)
                .offset(y: -(diameter / 2 - 9))
                .rotationEffect(.degrees(valueDeg))
        }
        .frame(width: diameter + markRadius * 2 + 12, height: diameter + markRadius * 2 + 10)
        .overlay(marksOverlay)
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

    private var marksOverlay: some View {
        GeometryReader { geo in
            let cx = geo.size.width / 2, cy = geo.size.height / 2
            let r = diameter / 2 + markRadius
            ForEach(Array(marks.enumerated()), id: \.offset) { i, label in
                let t = marks.count > 1 ? Double(i) / Double(marks.count - 1) : 0.5
                let a = (markStart + (markEnd - markStart) * t) * .pi / 180
                Text(label)
                    .font(.system(size: markFont, design: .monospaced))
                    .foregroundStyle(Color(hex: 0xb9b3a6))
                    .position(x: cx + r * sin(a), y: cy - r * cos(a))
            }
            if !unit.isEmpty {
                Text(unit)
                    .font(.system(size: unitFont, design: .monospaced))
                    .foregroundStyle(Color(hex: 0x8d8878))
                    .position(x: cx, y: geo.size.height - 3)
            }
        }
    }
}

// MARK: - Module chrome

private struct ConsoleModuleChrome<Content: View>: View {
    let title: String
    let plate: String
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

            Text(plate).font(.system(size: 11, weight: .bold)).tracking(1.1).foregroundStyle(Color(hex: 0x1d1e20))
                .frame(maxWidth: .infinity).frame(height: 26)
                .background(LinearGradient(colors: [Color(hex: 0xdedad0), Color(hex: 0xb8b4a9)], startPoint: .top, endPoint: .bottom))
                .clipShape(RoundedRectangle(cornerRadius: 3))
                .overlay(RoundedRectangle(cornerRadius: 3).stroke(.black, lineWidth: 1))
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

    private let dbMarks = ["-15", "12", "9", "6", "3", "0", "3", "6", "9", "12", "+15"]

    private func moduleHeight(_ m: MixerModuleFocus) -> CGFloat {
        switch m {
        case .filter: return 188
        case .eq:     return 566
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
                      diameter: CGFloat = 50, markFont: CGFloat = 7, unitFont: CGFloat = 6.5) -> some View {
        ConsoleKnob(color: color, marks: marks, markRadius: markRadius, unit: unit,
                    value: engine.consoleValue(trackId, param), range: range, defaultValue: def,
                    diameter: diameter, markFont: markFont, unitFont: unitFont,
                    onChange: { engine.setConsoleValue(trackId, param, $0) },
                    onCommit: { engine.recordGesture("4000E \(param)") })
    }

    private func placed(_ cx: CGFloat, _ cy: CGFloat, _ v: some View, size: CGFloat = 86) -> some View {
        v.frame(width: size, height: size).position(x: cx, y: cy)
    }

    // MARK: modules

    private var filters: some View {
        ConsoleModuleChrome(title: "FILTERS", plate: "NEURACOUST · NC-F", inOn: inOn, onToggleIn: onToggleIn) {
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
        ConsoleModuleChrome(title: "EQUALISER", plate: "NEURACOUST · NC-EQ E", inOn: inOn, onToggleIn: onToggleIn) {
            // EQ knobs 20% larger, mark labels +4pt, and the two columns pulled closer together.
            let d: CGFloat = 60, mf: CGFloat = 11, uf: CGFloat = 10, lx: CGFloat = 58, rx: CGFloat = 148, sz: CGFloat = 100
            ZStack {
                eqSpine
                placed(lx, 52, knob("eqHfGainDb", -18...18, 0, .red, marks: dbMarks, unit: "dB", diameter: d, markFont: mf, unitFont: uf), size: sz)
                placed(rx, 86, knob("eqHfHz", 4000...16000, 8000, .red, marks: ["1.5", "3", "5", "8", "10", "14", "16"], unit: "kHz", markRadius: 11, diameter: d, markFont: mf, unitFont: uf), size: sz)
                placed(lx, 138, knob("eqHmfGainDb", -18...18, 0, .green, marks: dbMarks, unit: "dB", diameter: d, markFont: mf, unitFont: uf), size: sz)
                placed(rx, 172, knob("eqHmfHz", 1200...7500, 3000, .green, marks: [".6", "1", "2", "3", "4", "5", "7"], unit: "kHz", markRadius: 11, diameter: d, markFont: mf, unitFont: uf), size: sz)
                placed(lx, 224, knob("eqHmfQ", 0.2...10, 1, .green, marks: ["3", "2", "1.5", "1", ".5"], unit: "Q", markRadius: 10, diameter: d, markFont: mf, unitFont: uf), size: sz)
                placed(rx, 258, knob("eqLmfQ", 0.2...10, 1, .blue, marks: ["3", "2", "1.5", "1", ".5"], unit: "Q", markRadius: 10, diameter: d, markFont: mf, unitFont: uf), size: sz)
                placed(lx, 310, knob("eqLmfGainDb", -18...18, 0, .blue, marks: dbMarks, unit: "dB", diameter: d, markFont: mf, unitFont: uf), size: sz)
                placed(rx, 344, knob("eqLmfHz", 400...2500, 1000, .blue, marks: [".4", ".8", "1", "1.5", "2.5"], unit: "kHz", markRadius: 11, diameter: d, markFont: mf, unitFont: uf), size: sz)
                placed(lx, 396, knob("eqLfGainDb", -18...18, 0, .brown, marks: dbMarks, unit: "dB", diameter: d, markFont: mf, unitFont: uf), size: sz)
                placed(rx, 430, knob("eqLfHz", 90...450, 200, .brown, marks: ["30", "50", "100", "200", "300", "400", "450"], unit: "Hz", markRadius: 11, diameter: d, markFont: mf, unitFont: uf), size: sz)
                bellButton("eqHfBell", on: engine.consoleValue(trackId, "eqHfBell") > 0.5).position(x: rx, y: 25)
                bellButton("eqLfBell", on: engine.consoleValue(trackId, "eqLfBell") > 0.5).position(x: lx, y: 461)
            }
            .frame(height: 490)
        }
    }

    private var eqSpine: some View {
        ZStack {
            spineRail(Color(hex: 0xa5372c), top: 12, height: 84)
            spineRail(Color(hex: 0x2f7a45), top: 98, height: 168)
            spineRail(Color(hex: 0x3a6fa8), top: 270, height: 82)
            spineRail(Color(hex: 0x6b4a3a), top: 356, height: 122)
        }
    }
    private func spineRail(_ c: Color, top: CGFloat, height: CGFloat) -> some View {
        RoundedRectangle(cornerRadius: 2).fill(c)
            .frame(width: 5, height: height)
            .position(x: 2.5, y: top + height / 2)
    }

    private func bellButton(_ param: String, on: Bool) -> some View {
        Button {
            engine.setConsoleBool(trackId, param, !on); engine.recordGesture("4000E \(param)")
        } label: {
            Text("BELL").font(.system(size: 11, weight: .semibold)).tracking(1)
                .foregroundStyle(on ? Color(hex: 0x2a1f10) : Color(hex: 0xded7c9))
                .frame(width: 50, height: 24)
                .background(on
                    ? AnyView(LinearGradient(colors: [Color(hex: 0xe0a94b), Color(hex: 0xa9741f)], startPoint: .top, endPoint: .bottom))
                    : AnyView(LinearGradient(colors: [Color(hex: 0x3d3f41), Color(hex: 0x232527)], startPoint: .top, endPoint: .bottom)))
                .clipShape(RoundedRectangle(cornerRadius: 4))
                .overlay(RoundedRectangle(cornerRadius: 4).stroke(.black, lineWidth: 1))
        }.buttonStyle(.plain)
    }

    private var compress: some View {
        ConsoleModuleChrome(title: "COMPRESS", plate: "NEURACOUST · NC-COMP", inOn: inOn, onToggleIn: onToggleIn) {
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
        ConsoleModuleChrome(title: "GATE / EXP", plate: "NEURACOUST · NC-GATE", inOn: inOn, onToggleIn: onToggleIn) {
            ZStack {
                placed(152, 56, knob("gateThresholdDb", -60...0, -36, .silver, marks: ["0", "-10", "-20", "-30", "-40", "-50", "-60"], unit: "THRESHOLD", markRadius: 11))
                placed(56, 56, knob("gateRangeDb", 0...40, 20, .green, marks: ["0", "5", "10", "20", "30", "35", "40"], unit: "RANGE"))
                placed(56, 142, knob("gateReleaseMs", 40...1500, 360, .green, marks: [".04", ".1", ".3", ".6", "1.5"], unit: "RELEASE"))
            }
            .frame(height: 222)
        }
    }
}
