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
    static let maxDb: Float = 10

    /// (dB, position) anchors, ascending. Gaps shrink downward: 0→-5 is the widest.
    static let anchors: [(db: Float, pos: Double)] = [
        (-120, 0.000), (-40, 0.054), (-30, 0.114), (-20, 0.206),
        (-10, 0.344), (-5, 0.528), (0, 0.780), (5, 0.895), (10, 1.000),
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

    // Console-style legend: dense near unity, compressing below, ∞ at the true floor.
    static let marks: [(String, Float)] = [
        ("10", 10), ("5", 5), ("0", 0), ("5", -5), ("10", -10),
        ("20", -20), ("30", -30), ("40", -40), ("∞", silenceDb),
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
                Button("\(db) dB") { engine.setSendGain(trackId, slot: slot, gainDb: Float(db)) }
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
                HStack(spacing: 2.5) {
                    Text(label)
                        .font(Theme.Font.mono(6.5, unity ? .bold : .regular))
                        .foregroundStyle(Color(hex: unity ? 0xc0b49c : 0x7a6f5f))
                        .frame(width: 13, alignment: .trailing)
                    Rectangle()
                        .fill(Color(hex: unity ? 0x8a7d68 : 0x574d40))
                        .frame(width: unity ? 7 : 4, height: unity ? 1.5 : 1)
                }
                .position(x: 12, y: y)
            }
        }
        .frame(width: 24)
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

    @State private var dragStartDb: Float?

    var body: some View {
        GeometryReader { geo in
            let travel = geo.size.height - capHeight
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
                            .frame(width: 4, height: max(0, geo.size.height - capY - capHeight / 2))
                    }

                faderCap
                    .offset(y: capY)
            }
            .frame(maxWidth: .infinity)
            .contentShape(Rectangle())
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { drag in
                        let start = dragStartDb ?? volumeDb
                        if dragStartDb == nil { dragStartDb = start }
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

    private let capHeight: CGFloat = 26

    /// A modern take on the classic brushed-metal console cap: a metallic body with grip
    /// ribs, a brighter central grip band, and the accent line as the value indicator.
    private var faderCap: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 5)
                .fill(LinearGradient(
                    colors: [Color(hex: 0x646c74), Color(hex: 0x363c44), Color(hex: 0x14181d)],
                    startPoint: .top, endPoint: .bottom))
                .overlay(
                    RoundedRectangle(cornerRadius: 5)
                        .stroke(LinearGradient(colors: [.white.opacity(0.35), .black.opacity(0.45)],
                                               startPoint: .top, endPoint: .bottom), lineWidth: 1))
                .shadow(color: .black.opacity(0.55), radius: 3, y: 2)

            // Grip ribs across the whole cap.
            VStack(spacing: 3) {
                ForEach(0..<5, id: \.self) { _ in
                    Rectangle().fill(Color.black.opacity(0.30)).frame(height: 0.75)
                        .overlay(Rectangle().fill(Color.white.opacity(0.10)).frame(height: 0.75).offset(y: -0.75))
                }
            }
            .padding(.horizontal, 6)

            // Central grip band + accent indicator line.
            ZStack {
                RoundedRectangle(cornerRadius: 2)
                    .fill(LinearGradient(colors: [Color(hex: 0xd0d4d9), Color(hex: 0x9299a1), Color(hex: 0xb7bcc2)],
                                         startPoint: .top, endPoint: .bottom))
                    .frame(height: 9)
                    .overlay(RoundedRectangle(cornerRadius: 2).stroke(Color.black.opacity(0.25), lineWidth: 0.5))
                Rectangle().fill(accent).frame(height: 2).shadow(color: accent.opacity(0.9), radius: 2)
            }
            .padding(.horizontal, 2)
        }
        .frame(width: 34, height: capHeight)
    }
}

/// Horizontal pan slider with a centre detent.
struct PanSlider: View {
    let pan: Float
    let accent: Color
    let onChange: (Float) -> Void
    var onCommit: () -> Void = {}

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
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { drag in
                        let fraction = min(1, max(0, drag.location.x / geo.size.width))
                        var value = Float(fraction * 2 - 1)
                        if abs(value) < 0.05 { value = 0 }   // centre detent
                        onChange(value)
                    }
                    .onEnded { _ in onCommit() }
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
struct VerticalMeter: View {
    let peak: Float
    /// Dot count. Dense by default (~2× the old meter) for a finer read.
    var segments = 48

    @State private var held: Float = 0
    @State private var heldAt: CFTimeInterval = 0
    private let clock = Timer.publish(every: 0.05, on: .main, in: .common).autoconnect()

    var body: some View {
        GeometryReader { geo in
            let level = meterFraction(peak)
            let lit = Int((level * Double(segments)).rounded())
            let peakSeg = held > 0.0005 ? max(1, Int((meterFraction(held) * Double(segments)).rounded())) : 0
            let dotH = max(1, (geo.size.height - CGFloat(segments - 1)) / CGFloat(segments))
            VStack(spacing: 1) {
                ForEach(0..<segments, id: \.self) { index in
                    let fromBottom = segments - index          // 1…segments
                    let isLit = fromBottom <= lit
                    let isPeak = fromBottom == peakSeg
                    RoundedRectangle(cornerRadius: dotH / 2)
                        .fill(isPeak ? peakColor(index)
                              : (isLit ? segmentColor(index) : Theme.Palette.recess))
                        .frame(height: dotH)
                }
            }
        }
        .frame(width: 6)
        .onReceive(clock) { _ in
            // Peak hold: jump up instantly, hold ~1 s, then fall.
            let now = CACurrentMediaTime()
            if peak >= held { held = peak; heldAt = now }
            else if now - heldAt > 1.0 { held = max(0, held - 0.035) }
        }
    }

    private func segmentColor(_ fromTop: Int) -> Color {
        let fraction = Double(fromTop) / Double(segments)
        if fraction < 0.12 { return Theme.Palette.red }
        if fraction < 0.32 { return Theme.Palette.yellow }
        return Theme.Palette.green
    }
    /// The held-peak dot is a brighter version of its zone's colour.
    private func peakColor(_ fromTop: Int) -> Color {
        let fraction = Double(fromTop) / Double(segments)
        if fraction < 0.12 { return Color(hex: 0xff6b6b) }
        if fraction < 0.32 { return Color(hex: 0xffe27a) }
        return Color(hex: 0x9be89b)
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
