import SwiftUI

/// Maps dB to fader travel. Linear in dB would waste the top of the throw, so the
/// design's scale is compressed below -12: 12 at the top, then 0, -12, -24, -∞.
enum FaderScale {
    static let minDb: Float = -60
    static let maxDb: Float = 12

    /// 0 at the bottom of the throw, 1 at the top.
    static func position(forDb db: Float) -> Double {
        let clamped = min(maxDb, max(minDb, db))
        return Double((clamped - minDb) / (maxDb - minDb))
    }

    static func db(forPosition position: Double) -> Float {
        let clamped = min(1, max(0, position))
        return minDb + Float(clamped) * (maxDb - minDb)
    }

    static let marks: [(String, Float)] = [
        ("12", 12), ("0", 0), ("-12", -12), ("-24", -24), ("-∞", -60),
    ]
}

/// dB scale drawn beside a fader. Marks sit at their real positions on the throw,
/// not evenly spaced — otherwise the 0 dB label does not line up with a 0 dB cap.
struct FaderScaleMarks: View {
    let capHeight: CGFloat

    var body: some View {
        GeometryReader { geo in
            let travel = geo.size.height - capHeight
            ForEach(FaderScale.marks, id: \.0) { label, db in
                Text(label)
                    .font(Theme.Font.mono(6))
                    .foregroundStyle(Color(hex: 0x7a6f5f))
                    .frame(width: 18, alignment: .trailing)
                    .position(
                        x: 9,
                        y: capHeight / 2 + travel * (1 - CGFloat(FaderScale.position(forDb: db)))
                    )
            }
        }
        .frame(width: 18)
    }
}

func dbLabel(_ db: Float) -> String {
    db <= FaderScale.minDb ? "-∞" : String(format: "%.1f", db)
}

/// Vertical channel fader. Drag the cap; double-click returns to unity.
struct ChannelFader: View {
    let volumeDb: Float
    let accent: Color
    let onChange: (Float) -> Void

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
                    .onEnded { _ in dragStartDb = nil }
            )
            .onTapGesture(count: 2) { onChange(0) }
        }
    }

    private let capHeight: CGFloat = 26

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
        .frame(width: 30, height: capHeight)
    }
}

/// Horizontal pan slider with a centre detent.
struct PanSlider: View {
    let pan: Float
    let accent: Color
    let onChange: (Float) -> Void

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
            )
            .onTapGesture(count: 2) { onChange(0) }
        }
        .frame(height: 12)
    }
}

/// Segmented vertical meter, red at the top. Matches the design's scanline look.
struct VerticalMeter: View {
    let peak: Float
    var segments = 24

    var body: some View {
        GeometryReader { geo in
            let lit = Int((meterFraction(peak) * Double(segments)).rounded())
            VStack(spacing: 1) {
                ForEach(0..<segments, id: \.self) { index in
                    let fromTop = index
                    let isLit = (segments - fromTop) <= lit
                    Rectangle()
                        .fill(isLit ? segmentColor(fromTop) : Theme.Palette.recess)
                        .frame(height: max(1, (geo.size.height - CGFloat(segments - 1)) / CGFloat(segments)))
                }
            }
        }
        .frame(width: 5)
    }

    private func segmentColor(_ fromTop: Int) -> Color {
        let fraction = Double(fromTop) / Double(segments)
        if fraction < 0.12 { return Theme.Palette.red }
        if fraction < 0.32 { return Theme.Palette.yellow }
        return Theme.Palette.green
    }
}

/// One of the five insert or send slots on a strip. Empty slots are dashed.
struct SlotChip: View {
    let label: String
    let accent: Color
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
                    Rectangle().fill(accent).frame(width: 2)
                    Text(label)
                        .font(Theme.Font.ui(8))
                        .foregroundStyle(accent)
                        .lineLimit(1)
                        .truncationMode(.tail)
                        .padding(.leading, 4)
                    Spacer(minLength: 0)
                }
                .background(
                    RoundedRectangle(cornerRadius: Theme.Radius.pill).fill(accent.opacity(0.12))
                )
                .clipShape(RoundedRectangle(cornerRadius: Theme.Radius.pill))
            }
        }
        .frame(height: 14)
        .onTapGesture { action?() }
    }
}
