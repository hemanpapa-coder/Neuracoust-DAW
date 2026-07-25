import SwiftUI
import Combine
import QuartzCore

/// Maps dB to fader travel with a real mixing-console taper — not a straight line.
///
/// Physical spacing is widest around unity and compresses as it descends: the 0→-5
/// step is the tallest, then -5→-10, -10→-20, -20→-30, -30→-40 each shorter than the
/// last, and -40…-120 collapses into the last sliver above the ∞ floor. The law is a
/// piecewise-linear interpolation over hand-tuned anchor points, so the drag feel and
/// the printed legend agree exactly. Position is 0 at the bottom of the throw, 1 at the top.
enum FaderScale {
    static let silenceDb: Float = -120
    static let maxDb: Float = 12

    /// (dB, position) anchors, ascending. The fader boosts to +12 dB at the very top;
    /// unity (0 dB) sits ~80% up, with the 6-dB steps below near-even and a gentle
    /// downward compression toward the floor — the console/Nuendo look.
    static let anchors: [(db: Float, pos: Double)] = [
        (-120, 0.000), (-60, 0.070), (-48, 0.140), (-42, 0.195), (-36, 0.260),
        (-30, 0.335), (-24, 0.415), (-18, 0.500), (-12, 0.590), (-6, 0.690),
        (0, 0.800), (6, 0.900), (12, 1.000),
    ]

    static func position(forDb db: Float) -> Double {
        let clamped = min(maxDb, max(silenceDb, db))
        for i in 1..<anchors.count {
            let lo = anchors[i - 1], hi = anchors[i]
            if clamped <= hi.db {
                let t = Double((clamped - lo.db) / (hi.db - lo.db))
                return lo.pos + t * (hi.pos - lo.pos)
            }
        }
        return 1
    }

    static func db(forPosition position: Double) -> Float {
        let clamped = min(1, max(0, position))
        for i in 1..<anchors.count {
            let lo = anchors[i - 1], hi = anchors[i]
            if clamped <= hi.pos {
                let t = Float((clamped - lo.pos) / (hi.pos - lo.pos))
                return lo.db + t * (hi.db - lo.db)
            }
        }
        return maxDb
    }

    // Signed legend: +12 at the top, unity in the middle, -∞ at the very bottom.
    static let marks: [(String, Float)] = [
        ("+12", 12), ("+6", 6), ("0", 0), ("-6", -6), ("-12", -12),
        ("-24", -24), ("-36", -36), ("-48", -48), ("∞", -120),
    ]
}

/// Shared send-control choices, so the mixer strip and the edit-window track header offer
/// the exact same level and pan options — Pro Tools sends carry both.
enum SendControls {
    static let levels: [Int] = [0, -3, -6, -12, -18, -24, -36]
    static let pans: [(label: String, value: Float)] = [
        ("L100", -1), ("L75", -0.75), ("L50", -0.5), ("L25", -0.25),
        ("C", 0), ("R25", 0.25), ("R50", 0.5), ("R75", 0.75), ("R100", 1),
    ]
}

func sendPanLabel(_ pan: Float) -> String {
    let v = Int((abs(pan) * 100).rounded())
    if v == 0 { return "C" }
    return pan < 0 ? "L\(v)" : "R\(v)"
}

/// The send menu (level / pan / pre-post / remove), reused by the mixer strip so every
/// SwiftUI caller shows identical options.
struct SendMenuContent: View {
    let engine: EngineController
    let trackId: Int
    let slot: Int
    let send: EngineController.TrackSend

    var body: some View {
        Text(send.bus)
        Menu("레벨") {
            ForEach(SendControls.levels, id: \.self) { db in
                Button("\(db) dB") {
                    engine.setSendGain(trackId, slot: slot, gainDb: Float(db))
                    engine.recordGesture("Send level")   // discrete pick = one undo step
                }
            }
        }
        Menu("팬") {
            ForEach(SendControls.pans, id: \.label) { pan in
                Button(pan.label) { engine.setSendPan(trackId, slot: slot, pan: pan.value) }
            }
        }
        Button(send.preFader ? "포스트 페이더로" : "프리 페이더로") {
            engine.setSendPreFader(trackId, slot: slot, pre: !send.preFader)
        }
        Divider()
        Button("센드 제거", role: .destructive) { engine.removeSend(trackId, slot: slot) }
    }
}

/// One fixed send slot on a strip: a pre/post toggle, the destination bus name, and a
/// horizontal send-level fader with its dB value — the send equivalent of an insert slot.
struct SendSlotRow: View {
    let bus: String
    let gainDb: Float
    let preFader: Bool
    let onGain: (Float) -> Void
    var onCommitGain: () -> Void = {}
    let onTogglePrePost: () -> Void

    @State private var dragStart: Float?

    var body: some View {
        HStack(spacing: 3) {
            Button(action: onTogglePrePost) {
                Text(preFader ? "PRE" : "PST")
                    .font(Theme.Font.mono(6, .bold))
                    .foregroundStyle(preFader ? Theme.Palette.yellow : Theme.Palette.teal)
                    .frame(width: 20, height: 15)
                    .background(RoundedRectangle(cornerRadius: 2)
                        .fill((preFader ? Theme.Palette.yellow : Theme.Palette.teal).opacity(0.16)))
            }
            .buttonStyle(.plain)
            .help(preFader ? "프리 페이더" : "포스트 페이더")

            GeometryReader { geo in
                let frac = CGFloat(FaderScale.position(forDb: gainDb))
                ZStack(alignment: .leading) {
                    RoundedRectangle(cornerRadius: 2).fill(Theme.Palette.recess)
                    RoundedRectangle(cornerRadius: 2)
                        .fill(Theme.Palette.teal.opacity(0.45))
                        .frame(width: max(0, geo.size.width * frac))
                    HStack(spacing: 0) {
                        Text(bus).font(Theme.Font.mono(7)).foregroundStyle(Theme.Palette.textSecondary)
                            .lineLimit(1).truncationMode(.tail)
                        Spacer(minLength: 2)
                        Text(dbLabel(gainDb)).font(Theme.Font.mono(6.5)).foregroundStyle(Theme.Palette.textDim)
                    }
                    .padding(.horizontal, 4)
                }
                .contentShape(Rectangle())
                // This row lives inside the mixer's two-axis ScrollView. The control must win
                // the drag arena or the scroll view consumes the gesture and the send appears
                // completely inert.
                .highPriorityGesture(
                    DragGesture(minimumDistance: 0)
                        .onChanged { drag in
                            if dragStart == nil { dragStart = gainDb }
                            let pos = Double(min(1, max(0, drag.location.x / max(1, geo.size.width))))
                            onGain(FaderScale.db(forPosition: pos))
                        }
                        .onEnded { _ in dragStart = nil; onCommitGain() }
                )
            }
            .frame(height: 15)
        }
    }
}

/// dB scale drawn beside a fader. Marks sit at their real positions on the throw,
/// not evenly spaced — otherwise the 0 dB label does not line up with a 0 dB cap.
struct FaderScaleMarks: View {
    let capHeight: CGFloat

    var body: some View {
        GeometryReader { geo in
            let travel = geo.size.height - capHeight
            ForEach(FaderScale.marks, id: \.1) { label, db in
                let y = capHeight / 2 + travel * (1 - CGFloat(FaderScale.position(forDb: db)))
                let unity = db == 0
                let isInf = label == "∞"
                HStack(spacing: 2) {                       // tick ↔ number gap (matches the meter)
                    Text(label)
                        // ∞ carries no minus and is set larger, right-aligned in the same
                        // column as the numbers above it so the legend stays balanced.
                        .font(Theme.Font.mono(isInf ? 11 : 7, (unity || isInf) ? .semibold : .regular))
                        .foregroundStyle(Color(hex: unity ? 0xb6bbc2 : 0x8b9096))
                        .frame(width: 17, alignment: .trailing)
                    Rectangle()
                        .fill(Color(hex: 0x4a4f56))
                        .frame(width: unity ? 6 : 4, height: 1)
                }
                // Pin the tick to the trailing (fader) edge, so its distance to the fader
                // is set by the section's padding rather than by centring math.
                .frame(width: geo.size.width, alignment: .trailing)
                .position(x: geo.size.width / 2, y: y)
            }
        }
        .frame(width: 25)
    }
}

/// "-∞" only at the engine's true floor. A -60 dB signal is quiet, not silent.
func dbLabel(_ db: Float) -> String {
    db <= FaderScale.silenceDb + 0.5 ? "-∞" : String(format: "%.1f", db)
}

/// Vertical channel fader. Drag the cap; double-click returns to unity.
struct ChannelFader: View {
    let volumeDb: Float
    let accent: Color
    let onChange: (Float) -> Void
    /// Fires once when the drag ends, so one drag is one undo step.
    var onCommit: () -> Void = {}
    /// Fires when the cap is grabbed (for automation touch/latch).
    var onBegin: () -> Void = {}

    @State private var dragStartDb: Float?

    /// Shared so the scale marks beside the fader line up with the cap exactly.
    static let capHeight: CGFloat = 26

    var body: some View {
        GeometryReader { geo in
            let travel = geo.size.height - Self.capHeight
            let capY = travel * (1 - FaderScale.position(forDb: volumeDb))

            ZStack(alignment: .top) {
                // Slot
                Capsule()
                    .fill(Theme.Palette.recess)
                    .frame(width: 4)
                    .frame(maxWidth: .infinity)
                    .overlay(alignment: .bottom) {
                        Capsule()
                            .fill(accent.opacity(0.55))
                            .frame(width: 4, height: max(0, geo.size.height - capY - Self.capHeight / 2))
                    }

                faderCap
                    .offset(y: capY)
            }
            .frame(maxWidth: .infinity)
            .contentShape(Rectangle())
            // Fader drags take precedence over the enclosing mixer ScrollView.
            .highPriorityGesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { drag in
                        let start = dragStartDb ?? volumeDb
                        if dragStartDb == nil { dragStartDb = start; onBegin() }
                        let delta = -drag.translation.height / max(1, travel)
                        let position = FaderScale.position(forDb: start) + Double(delta)
                        onChange(FaderScale.db(forPosition: position))
                    }
                    .onEnded { _ in
                        dragStartDb = nil
                        onCommit()
                    }
            )
            // A DragGesture with minimumDistance 0 swallows taps, so the reset has
            // to outrank it.
            .highPriorityGesture(TapGesture(count: 2).onEnded {
                onChange(0)
                onCommit()
            })
        }
    }

    /// The design's cap is a physical knob: two radial gradients plus an inset rim.
    private var faderCap: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 4)
                .fill(
                    RadialGradient(
                        colors: [Color(hex: 0x3c444e), Color(hex: 0x171c22)],
                        center: UnitPoint(x: 0.5, y: 0.65),
                        startRadius: 1, endRadius: 26
                    )
                )
                .overlay(
                    RoundedRectangle(cornerRadius: 4)
                        .stroke(Color.white.opacity(0.18), lineWidth: 1)
                )
                .shadow(color: .black.opacity(0.6), radius: 3, y: 2)

            Rectangle()
                .fill(accent)
                .frame(height: 2)
                .shadow(color: accent.opacity(0.8), radius: 2)
        }
        // A slim cap frees horizontal room, letting the fader + meter sit further left
        // (the meter numbers may pass behind the cap, which is fine).
        .frame(width: 16, height: Self.capHeight)
    }
}

/// Horizontal pan slider with a centre detent.
struct PanSlider: View {
    let pan: Float
    let accent: Color
    let onChange: (Float) -> Void
    var onCommit: () -> Void = {}
    var onBegin: () -> Void = {}

    @State private var dragging = false

    var body: some View {
        GeometryReader { geo in
            let x = geo.size.width * Double((pan + 1) / 2)
            ZStack(alignment: .leading) {
                Capsule()
                    .fill(Theme.Palette.recess)
                    .frame(height: 3)
                    .frame(maxHeight: .infinity)

                Rectangle()
                    .fill(Theme.Palette.textFainter)
                    .frame(width: 1, height: 7)
                    .position(x: geo.size.width / 2, y: geo.size.height / 2)

                Circle()
                    .fill(accent)
                    .frame(width: 9, height: 9)
                    .position(x: x, y: geo.size.height / 2)
            }
            .contentShape(Rectangle())
            // Pan drags take precedence over the enclosing mixer ScrollView.
            .highPriorityGesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { drag in
                        if !dragging { dragging = true; onBegin() }
                        let fraction = min(1, max(0, drag.location.x / geo.size.width))
                        var value = Float(fraction * 2 - 1)
                        if abs(value) < 0.05 { value = 0 }   // centre detent
                        onChange(value)
                    }
                    .onEnded { _ in dragging = false; onCommit() }
            )
            .highPriorityGesture(TapGesture(count: 2).onEnded {
                onChange(0)
                onCommit()
            })
        }
        .frame(height: 12)
    }
}

/// Segmented vertical meter, red at the top. Matches the design's scanline look.
/// The unified dot level meter. Dense dot segments show the moving level (VU-style bar);
/// a single bright dot holds the peak for ~1 s before falling. Used everywhere so every
/// meter in the app reads the same way.
/// Vertical level meter: a solid orange gradient bar rising from the bottom, with a red
/// peak-hold cap that lights the headroom zone — matching the reference hardware look.
struct VerticalMeter: View {
    let peak: Float
    var width: CGFloat = 12

    @State private var held: Float = 0
    @State private var heldAt: CFTimeInterval = 0

    /// Fraction of the throw above which the meter is into headroom (red).
    private let redZone: CGFloat = 0.88

    // Peak-hold, driven by changes to `peak` (the engine's meter cadence) instead of a
    // per-meter Timer. A Timer.publish here ran 20 Hz forever on EVERY strip's meter, so the
    // main run loop re-rendered continuously even at idle — the real cause of the ~40% idle
    // CPU and the flickering menus. Now nothing ticks when the meter is silent.
    private func updateHold(_ newPeak: Float) {
        let now = CACurrentMediaTime()
        if newPeak >= held { held = newPeak; heldAt = now }
        else if now - heldAt > 1.0 { held = max(newPeak, held - 0.06) }
        if newPeak <= 0.0016 { held = 0 }   // silence: drop the cap so it doesn't freeze lit
    }

    var body: some View {
        GeometryReader { geo in
            let h = geo.size.height
            let level = CGFloat(meterFraction(peak))
            let heldLevel = CGFloat(meterFraction(held))
            ZStack(alignment: .bottom) {
                RoundedRectangle(cornerRadius: 2).fill(Color(hex: 0x141519))
                    .overlay(RoundedRectangle(cornerRadius: 2).stroke(Color.black.opacity(0.9), lineWidth: 1))

                // Orange body up to the current level; the part in the red zone turns red.
                RoundedRectangle(cornerRadius: 2)
                    .fill(LinearGradient(colors: [Color(hex: 0xff7a12), Color(hex: 0xff8a1e), Color(hex: 0xffb24d)],
                                         startPoint: .bottom, endPoint: .top))
                    .frame(height: max(0, min(level, redZone) * h))
                if level > redZone {
                    RoundedRectangle(cornerRadius: 2)
                        .fill(LinearGradient(colors: [Color(hex: 0xff5a4d), Color(hex: 0xff3b30)],
                                             startPoint: .bottom, endPoint: .top))
                        .frame(height: (level - redZone) * h)
                        .offset(y: -redZone * h)
                }

                // Peak-hold cap.
                if held > 0.0005 {
                    Rectangle()
                        .fill(heldLevel > redZone ? Color(hex: 0xff3b30) : Color(hex: 0xffd08a))
                        .frame(height: 2.5)
                        .offset(y: -(heldLevel * h) + 1.25)
                }
            }
            .clipShape(RoundedRectangle(cornerRadius: 2))
        }
        .frame(width: width)
        .onChange(of: peak) { _, newPeak in updateHold(newPeak) }
    }
}

/// A compact horizontal input meter — L over R (stereo). Sits at the top of the strip,
/// right under the input, so signal is visible before the fader. 0 dBFS = full width.
struct HorizontalMeter: View {
    let peakLeft: Float
    let peakRight: Float

    // 0 dBFS at the right (full), down to the floor. Sparse on purpose so the numbers
    // never crowd at narrow widths — 0 kept prominent, the top of the meter is what
    // matters most.
    private static let marks: [(String, Double)] = [("0", 0), ("-24", -24), ("-48", -48)]

    var body: some View {
        VStack(spacing: 2) {
            bar(peakLeft)
            bar(peakRight)
            scale
        }
    }

    private var scale: some View {
        GeometryReader { geo in
            ForEach(Array(Self.marks.enumerated()), id: \.offset) { _, m in
                let x = geo.size.width * ((m.1 + 60) / 60)
                VStack(spacing: 1) {
                    Rectangle().fill(Color(hex: 0x4a4f56)).frame(width: 1, height: 2)
                    Text(m.0).font(Theme.Font.mono(5)).foregroundStyle(Color(hex: 0x6f7c68))
                }
                .fixedSize()
                .position(x: min(geo.size.width - 5, max(6, x)), y: 5)
            }
        }
        .frame(height: 10)
    }

    private func bar(_ peak: Float) -> some View {
        GeometryReader { geo in
            let f = CGFloat(meterFraction(peak))
            ZStack(alignment: .leading) {
                RoundedRectangle(cornerRadius: 1).fill(Color(hex: 0x140f0a))
                RoundedRectangle(cornerRadius: 1)
                    .fill(LinearGradient(colors: [Color(hex: 0x46d17f), Color(hex: 0xe6d24a), Color(hex: 0xff5252)],
                                         startPoint: .leading, endPoint: .trailing))
                    .frame(width: max(0, geo.size.width * f))
            }
        }
        .frame(height: 4)
    }
}

/// The dBFS scale beside the meters. A digital peak meter tops out at 0 dBFS — the same
/// range the bar itself spans (meterFraction maps 0 dBFS → full, -60 dBFS → empty).
struct MeterScale: View {
    // (label, dBFS) — 0 at the very top, down to the -60 dBFS floor.
    private let marks: [(String, Double)] = [
        ("0", 0), ("-6", -6), ("-12", -12), ("-24", -24), ("-36", -36), ("-48", -48), ("-60", -60),
    ]
    private let topDb = 0.0, botDb = -60.0

    var body: some View {
        GeometryReader { geo in
            ForEach(Array(marks.enumerated()), id: \.offset) { _, m in
                let frac = (topDb - m.1) / (topDb - botDb)          // 0 dBFS at top
                HStack(spacing: 2) {                       // number ↔ tick gap (matches the fader)
                    Text(m.0)
                        .font(Theme.Font.mono(7, m.1 == 0 ? .bold : .regular))
                        .foregroundStyle(Color(hex: 0x3fb950))
                        .frame(width: 15, alignment: .trailing)
                    Rectangle().fill(Color(hex: 0x4a4f56)).frame(width: 4, height: 1)
                }
                // The scale sits to the LEFT of the meter, so the tick pins to the trailing
                // (meter) edge and the number stays clear on the fader side.
                .frame(width: geo.size.width, alignment: .trailing)
                .position(x: geo.size.width / 2, y: CGFloat(frac) * geo.size.height)
            }
        }
        .frame(width: 21)
    }
}

/// One of the five insert or send slots on a strip. Empty slots are dashed.
struct SlotChip: View {
    let label: String
    let accent: Color
    var bypassed = false
    var badge: String = ""
    /// The plug-in's editor window is open.
    var lit = false
    var action: (() -> Void)?

    var body: some View {
        Group {
            if label.isEmpty {
                RoundedRectangle(cornerRadius: Theme.Radius.pill)
                    .strokeBorder(Theme.Palette.coolDivider, style: StrokeStyle(lineWidth: 1, dash: [2, 2]))
                    .background(
                        RoundedRectangle(cornerRadius: Theme.Radius.pill).fill(Theme.Palette.background)
                    )
            } else {
                HStack(spacing: 0) {
                    Rectangle().fill(bypassed ? Theme.Palette.textFainter : accent).frame(width: 2)
                    Text(label)
                        .font(Theme.Font.ui(8))
                        .foregroundStyle(bypassed ? Theme.Palette.textFaint : accent)
                        .strikethrough(bypassed)
                        .lineLimit(1)
                        .truncationMode(.tail)
                        .padding(.leading, 4)
                    Spacer(minLength: 2)
                    // "INT" and "RINT" mean the plug-in runs off the audio thread.
                    if !badge.isEmpty && badge != "NAT" && !bypassed {
                        Text(badge)
                            .font(Theme.Font.mono(6))
                            .foregroundStyle(Theme.Palette.purple)
                            .padding(.trailing, 3)
                    }
                }
                .background(
                    RoundedRectangle(cornerRadius: Theme.Radius.pill)
                        .fill((bypassed ? Theme.Palette.textFainter : accent).opacity(lit ? 0.28 : 0.12))
                )
                .overlay(
                    RoundedRectangle(cornerRadius: Theme.Radius.pill)
                        .strokeBorder(accent.opacity(lit ? 0.9 : 0), lineWidth: 1)
                )
                .clipShape(RoundedRectangle(cornerRadius: Theme.Radius.pill))
            }
        }
        .frame(height: 14)
        // Without this an empty slot only responds on its dashed stroke.
        .contentShape(Rectangle())
        .onTapGesture { action?() }
    }
}
