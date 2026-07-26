import SwiftUI

// SSL 4000E-style console channel — 4 modules (Filters / EQ / Compress / Gate), reproduced from the
// Claude Design "Neuracoust Modules" spec. Visual-first: static values now, engine-wired next.
// Rendered at the design's native size and scaled to fit the host width (narrows to the DAW window).

// MARK: - Knob

private struct ConsoleKnobColor {
    let hi: Color, mid: Color, lo: Color
    static let black  = ConsoleKnobColor(hi: Color(hex: 0x3c3d39), mid: Color(hex: 0x1b1c1f), lo: Color(hex: 0x070806))
    static let silver = ConsoleKnobColor(hi: Color(hex: 0xe3e1d8), mid: Color(hex: 0xa29c8d), lo: Color(hex: 0x605c52))
    static let red    = ConsoleKnobColor(hi: Color(hex: 0xc4483a), mid: Color(hex: 0x8a2519), lo: Color(hex: 0x450f09))
    static let green  = ConsoleKnobColor(hi: Color(hex: 0x4aa565), mid: Color(hex: 0x1f6534), lo: Color(hex: 0x0b2e17))
    static let blue   = ConsoleKnobColor(hi: Color(hex: 0x528bcb), mid: Color(hex: 0x25578c), lo: Color(hex: 0x0f2b47))
    static let brown  = ConsoleKnobColor(hi: Color(hex: 0x96684a), mid: Color(hex: 0x563628), lo: Color(hex: 0x241310))
}

/// One console rotary: machined-aluminium bezel, flat matte colored cap, a carved dimple pointer,
/// radial tick labels and a unit caption — matching the design spec.
private struct ConsoleKnob: View {
    var diameter: CGFloat = 50
    var color: ConsoleKnobColor = .silver
    /// Pointer angle in degrees (0 = up), matching the design's `val`.
    var valueDeg: Double = 0
    var marks: [String] = []
    var markRadius: CGFloat = 12
    var unit: String = ""
    /// Angular span of the marks (design default −128°…128°).
    var markStart: Double = -128
    var markEnd: Double = 128

    private var boxSize: CGFloat { diameter }

    var body: some View {
        VStack(spacing: 2) {
            ZStack {
                // Bezel — spun aluminium collar.
                Circle()
                    .fill(
                        RadialGradient(colors: [Color(hex: 0x6a6b64), Color(hex: 0x232420), Color(hex: 0x08090a)],
                                       center: UnitPoint(x: 0.40, y: 0.24), startRadius: 1, endRadius: diameter * 0.6)
                    )
                    .overlay(
                        AngularGradient(gradient: Gradient(colors: [
                            .white.opacity(0.22), .black.opacity(0.30), .white.opacity(0.15),
                            .black.opacity(0.32), .white.opacity(0.22)]),
                            center: .center, angle: .degrees(208))
                            .clipShape(Circle()).opacity(0.7)
                    )
                    .overlay(Circle().stroke(Color.black, lineWidth: 1))
                    .shadow(color: .black.opacity(0.5), radius: 6, y: 6)
                    .frame(width: diameter, height: diameter)

                // Matte colored cap.
                Circle()
                    .fill(LinearGradient(colors: [color.mid, color.mid, color.lo],
                                         startPoint: .top, endPoint: .bottom))
                    .overlay(Circle().stroke(.white.opacity(0.06), lineWidth: 1))
                    .overlay(Circle().stroke(.black.opacity(0.34), lineWidth: 0.5).blur(radius: 0.5).offset(y: 1))
                    .shadow(color: .black.opacity(0.45), radius: 3, y: 2)
                    .frame(width: diameter - 8, height: diameter - 8)

                // Carved dimple pointer with a bright cream fill, rotated to the value.
                Circle()
                    .fill(Color.black.opacity(0.5))
                    .overlay(
                        Circle()
                            .fill(RadialGradient(colors: [color.hi, color.mid, color.lo],
                                                 center: UnitPoint(x: 0.38, y: 0.30), startRadius: 0, endRadius: 3))
                            .frame(width: 4.5, height: 4.5)
                    )
                    .frame(width: 8, height: 8)
                    .offset(y: -(diameter / 2 - 12))
                    .rotationEffect(.degrees(valueDeg))
            }
            .frame(width: boxSize + markRadius * 2 + 14, height: boxSize + markRadius * 2 + 6)
            .overlay(marksOverlay)
        }
    }

    private var marksOverlay: some View {
        GeometryReader { geo in
            let cx = geo.size.width / 2, cy = geo.size.height / 2
            let r = diameter / 2 + markRadius
            ForEach(Array(marks.enumerated()), id: \.offset) { i, label in
                let t = marks.count > 1 ? Double(i) / Double(marks.count - 1) : 0.5
                let a = (markStart + (markEnd - markStart) * t) * .pi / 180
                Text(label)
                    .font(.custom("Menlo", size: 7))
                    .foregroundStyle(Color(hex: 0xb9b3a6))
                    .position(x: cx + r * sin(a), y: cy - r * cos(a))
            }
            if !unit.isEmpty {
                Text(unit)
                    .font(.custom("Menlo", size: 7))
                    .foregroundStyle(Color(hex: 0x8d8878))
                    .position(x: cx, y: geo.size.height - 4)
            }
        }
    }
}

// MARK: - Module chrome

private struct ConsoleModuleChrome<Content: View>: View {
    let title: String
    let plate: String
    @ViewBuilder var content: () -> Content

    var body: some View {
        VStack(spacing: 0) {
            // Header
            HStack(spacing: 8) {
                Text(title)
                    .font(.custom("BarlowCondensed-Bold", size: 15))
                    .tracking(1.8)
                    .foregroundStyle(Color(hex: 0xe6dfd0))
                Spacer()
                Text("IN").font(.custom("BarlowCondensed-Bold", size: 13)).tracking(1.3)
                    .foregroundStyle(Color(hex: 0x2a1f10))
                    .frame(width: 44, height: 24)
                    .background(LinearGradient(colors: [Color(hex: 0xe0a94b), Color(hex: 0xa9741f)], startPoint: .top, endPoint: .bottom))
                    .clipShape(RoundedRectangle(cornerRadius: 4))
                    .overlay(RoundedRectangle(cornerRadius: 4).stroke(.black, lineWidth: 1))
            }
            .padding(.horizontal, 10).frame(height: 34)
            .background(LinearGradient(colors: [Color(hex: 0x35373a), Color(hex: 0x26282a)], startPoint: .top, endPoint: .bottom))
            .overlay(Rectangle().fill(.black).frame(height: 1), alignment: .bottom)

            content()

            // Name plate
            Text(plate)
                .font(.custom("BarlowCondensed-Bold", size: 12)).tracking(1.2)
                .foregroundStyle(Color(hex: 0x1d1e20))
                .frame(maxWidth: .infinity).frame(height: 28)
                .background(LinearGradient(colors: [Color(hex: 0xdedad0), Color(hex: 0xb8b4a9)], startPoint: .top, endPoint: .bottom))
                .clipShape(RoundedRectangle(cornerRadius: 3))
                .overlay(RoundedRectangle(cornerRadius: 3).stroke(.black, lineWidth: 1))
                .padding(8)
        }
        .frame(width: 205)
        .background(LinearGradient(colors: [Color(hex: 0x2b2d2f), Color(hex: 0x232527)], startPoint: .top, endPoint: .bottom))
        .clipShape(RoundedRectangle(cornerRadius: 5))
        .overlay(RoundedRectangle(cornerRadius: 5).stroke(.black, lineWidth: 1))
        .shadow(color: .black.opacity(0.55), radius: 22, y: 18)
    }
}

/// A knob at an absolute position inside a module's content area (design uses absolute px layout).
private struct PlacedKnob: View {
    let cx: CGFloat, cy: CGFloat
    let knob: ConsoleKnob
    var body: some View {
        knob.frame(width: 88, height: 88).position(x: cx, y: cy)
    }
}

// MARK: - Top-level view (static values matching the design; engine wiring is the next step)

struct NeuracoustConsoleModulesView: View {
    /// When set, render just this one module (scaled to fit the host width) — the mixer summons EQ /
    /// Comp / Filter / Gate individually. nil renders all four side by side (the design overview).
    var only: MixerModuleFocus? = nil
    /// Host width for the single-module case; the 205px design scales to fit it.
    var width: CGFloat = 205
    private let dbMarks = ["-15", "12", "9", "6", "3", "0", "3", "6", "9", "12", "+15"]

    /// Native (unscaled) height of each single module, so scale-to-fit reserves the right space.
    private func moduleHeight(_ m: MixerModuleFocus) -> CGFloat {
        switch m {
        case .filter: return 190
        case .eq:     return 576
        case .comp:   return 384
        case .gate:   return 330
        default:      return 300
        }
    }

    @ViewBuilder private func moduleView(_ m: MixerModuleFocus) -> some View {
        switch m {
        case .filter: filters
        case .eq:     equaliser
        case .comp:   compress
        case .gate:   gate
        default:      EmptyView()
        }
    }

    var body: some View {
        if let only {
            // Single module: intrinsic size (no GeometryReader — it would eat the strip's height).
            let scale = min(1, width / 205)
            moduleView(only)
                .scaleEffect(scale, anchor: .top)
                .frame(width: width, height: moduleHeight(only) * scale, alignment: .top)
        } else {
            GeometryReader { geo in
                let native: CGFloat = 4 * 205 + 3 * 14   // four modules + gaps
                let scale = min(1, geo.size.width / native)
                HStack(alignment: .top, spacing: 14) {
                    filters; equaliser; compress; gate
                }
                .scaleEffect(scale, anchor: .topLeading)
                .frame(width: native * scale, height: 600 * scale, alignment: .topLeading)
            }
        }
    }

    private var filters: some View {
        ConsoleModuleChrome(title: "FILTERS", plate: "NEURACOUST · NC-F") {
            ZStack {
                PlacedKnob(cx: 148, cy: 56, knob: ConsoleKnob(diameter: 50, color: .black, valueDeg: -104,
                    marks: ["12","8","5","3.5","3"], markRadius: 11, unit: "kHz"))
                PlacedKnob(cx: 56, cy: 56, knob: ConsoleKnob(diameter: 50, color: .black, valueDeg: 34,
                    marks: ["20","30","50","70","120","200","300","350"], markRadius: 11, unit: "Hz"))
            }
            .frame(height: 112)
        }
    }

    private var equaliser: some View {
        ConsoleModuleChrome(title: "EQUALISER", plate: "NEURACOUST · NC-EQ E") {
            ZStack {
                PlacedKnob(cx: 54, cy: 52, knob: ConsoleKnob(color: .red, valueDeg: 22, marks: dbMarks, unit: "dB"))
                PlacedKnob(cx: 152, cy: 86, knob: ConsoleKnob(color: .red, valueDeg: 46, marks: ["1.5","3","5","8","10","14","16"], markRadius: 11, unit: "kHz"))
                PlacedKnob(cx: 54, cy: 138, knob: ConsoleKnob(color: .green, valueDeg: 14, marks: dbMarks, unit: "dB"))
                PlacedKnob(cx: 152, cy: 172, knob: ConsoleKnob(color: .green, valueDeg: -18, marks: [".6","1","2","3","4","5","7"], markRadius: 11, unit: "kHz"))
                PlacedKnob(cx: 54, cy: 224, knob: ConsoleKnob(color: .green, valueDeg: -34, marks: ["3","2","1.5","1",".5"], markRadius: 10, unit: "◠"))
                PlacedKnob(cx: 152, cy: 258, knob: ConsoleKnob(color: .blue, valueDeg: 28, marks: ["3","2","1.5","1",".5"], markRadius: 10, unit: "◠"))
                PlacedKnob(cx: 54, cy: 310, knob: ConsoleKnob(color: .blue, valueDeg: -8, marks: dbMarks, unit: "dB"))
                PlacedKnob(cx: 152, cy: 344, knob: ConsoleKnob(color: .blue, valueDeg: -52, marks: [".2",".4",".8","1","1.5","2.5","4.5"], markRadius: 11, unit: "kHz"))
                PlacedKnob(cx: 54, cy: 396, knob: ConsoleKnob(color: .brown, valueDeg: 6, marks: dbMarks, unit: "dB"))
                PlacedKnob(cx: 152, cy: 430, knob: ConsoleKnob(color: .brown, valueDeg: -76, marks: ["30","50","100","200","300","400","450"], markRadius: 11, unit: "Hz"))
                bell.position(x: 152, y: 25)
                bell.position(x: 54, y: 461)
            }
            .frame(height: 490)
        }
    }

    private var bell: some View {
        Text("BELL").font(.custom("BarlowCondensed-SemiBold", size: 12)).tracking(1.2)
            .foregroundStyle(Color(hex: 0xded7c9))
            .frame(width: 52, height: 26)
            .background(LinearGradient(colors: [Color(hex: 0x3d3f41), Color(hex: 0x232527)], startPoint: .top, endPoint: .bottom))
            .clipShape(RoundedRectangle(cornerRadius: 4))
            .overlay(RoundedRectangle(cornerRadius: 4).stroke(.black, lineWidth: 1))
    }

    private var compress: some View {
        ConsoleModuleChrome(title: "COMPRESS", plate: "NEURACOUST · NC-COMP") {
            VStack(spacing: 0) {
                // MIX bar
                HStack(spacing: 7) {
                    ZStack {
                        RoundedRectangle(cornerRadius: 3).fill(Color(hex: 0x0a1410))
                        GeometryReader { g in
                            RoundedRectangle(cornerRadius: 2).fill(Color(hex: 0x54e08a).opacity(0.3))
                                .frame(width: g.size.width * 0.8)
                        }
                        Text("80.0%").font(.custom("Menlo", size: 11)).foregroundStyle(Color(hex: 0x7dffb4))
                    }
                    .frame(height: 22)
                    .overlay(RoundedRectangle(cornerRadius: 3).stroke(.black, lineWidth: 1))
                    Text("MIX").font(.custom("BarlowCondensed-Bold", size: 13)).tracking(1.3).foregroundStyle(Color(hex: 0xe6dfd0))
                }
                .padding(.horizontal, 10).frame(height: 30)

                ZStack {
                    fastButton.position(x: 152, y: 25)
                    PlacedKnob(cx: 52, cy: 56, knob: ConsoleKnob(color: .silver, valueDeg: -34, marks: ["2","3","5","8","∞"], unit: "RATIO"))
                    PlacedKnob(cx: 152, cy: 90, knob: ConsoleKnob(color: .silver, valueDeg: 24, marks: ["+10","+5","0","-5","-10","-15","-20"], markRadius: 11, unit: "THRESHOLD"))
                    PlacedKnob(cx: 52, cy: 142, knob: ConsoleKnob(color: .silver, valueDeg: -62, marks: [".15",".2",".4","+1","+2"], unit: "RELEASE"))
                }
                .frame(height: 204)

                // GR meter
                VStack(spacing: 5) {
                    HStack {
                        Text("GAIN REDUCTION").font(.custom("BarlowCondensed-SemiBold", size: 11)).tracking(1.3).foregroundStyle(Color(hex: 0x8d8878))
                        Spacer()
                        Text("-2.4 dB").font(.custom("Menlo", size: 10)).foregroundStyle(Color(hex: 0xe0c33e))
                    }
                    ZStack(alignment: .trailing) {
                        RoundedRectangle(cornerRadius: 2).fill(Color(hex: 0x140a06))
                        GeometryReader { g in
                            RoundedRectangle(cornerRadius: 2)
                                .fill(LinearGradient(colors: [Color(hex: 0xf0902e), Color(hex: 0xd9691c)], startPoint: .trailing, endPoint: .leading))
                                .frame(width: g.size.width * 0.28).frame(maxWidth: .infinity, alignment: .trailing)
                        }
                    }
                    .frame(height: 8)
                    .overlay(RoundedRectangle(cornerRadius: 2).stroke(.black, lineWidth: 1))
                }
                .padding(.horizontal, 10).padding(.vertical, 8)
            }
        }
    }

    private var gate: some View {
        ConsoleModuleChrome(title: "GATE / EXP", plate: "NEURACOUST · NC-GATE") {
            VStack(spacing: 0) {
                HStack(spacing: 5) {
                    modeButton("EXP", amber: true)
                    modeButton("GATE", amber: false)
                    Spacer()
                    modeButton("FAST", amber: false)
                }
                .padding(.horizontal, 10).frame(height: 30)

                ZStack {
                    PlacedKnob(cx: 152, cy: 56, knob: ConsoleKnob(color: .silver, valueDeg: 30, marks: ["-30","-25","-20","-12","-4","-15","+5"], markRadius: 11, unit: "THRESHOLD"))
                    PlacedKnob(cx: 56, cy: 56, knob: ConsoleKnob(color: .green, valueDeg: -12, marks: ["0","5","10","20","30","35","40"], unit: "RANGE"))
                    PlacedKnob(cx: 56, cy: 142, knob: ConsoleKnob(color: .green, valueDeg: -58, marks: [".15",".2",".4","+1","+2"], unit: "RELEASE"))
                    grLadder.position(x: 152, y: 150)
                }
                .frame(height: 222)
            }
        }
    }

    private var grLadder: some View {
        VStack(alignment: .leading, spacing: 7) {
            ForEach(Array([("20", false), ("14", false), ("10", true), ("6", true), ("3", true)].enumerated()), id: \.offset) { _, item in
                HStack(spacing: 7) {
                    Circle().fill(item.1 ? Color(hex: 0xe0c33e) : Color(hex: 0x2a221f)).frame(width: 9, height: 9)
                    Text(item.0).font(.custom("Menlo", size: 10)).foregroundStyle(Color(hex: 0xc9c2b2)).frame(width: 14)
                    Circle().fill(Color(hex: 0x5fbf7a)).frame(width: 8, height: 8)
                }
            }
        }
    }

    private var fastButton: some View { modeButton("FAST", amber: false).frame(width: 52) }

    private func modeButton(_ label: String, amber: Bool) -> some View {
        Text(label).font(.custom("BarlowCondensed-\(amber ? "Bold" : "SemiBold")", size: amber ? 13 : 12)).tracking(1.2)
            .foregroundStyle(amber ? Color(hex: 0x2a1f10) : Color(hex: 0xded7c9))
            .frame(width: 48, height: 22)
            .background(amber
                        ? AnyView(LinearGradient(colors: [Color(hex: 0xe0a94b), Color(hex: 0xa9741f)], startPoint: .top, endPoint: .bottom))
                        : AnyView(LinearGradient(colors: [Color(hex: 0x3d3f41), Color(hex: 0x232527)], startPoint: .top, endPoint: .bottom)))
            .clipShape(RoundedRectangle(cornerRadius: 4))
            .overlay(RoundedRectangle(cornerRadius: 4).stroke(.black, lineWidth: 1))
    }
}
