import SwiftUI

/// Rotary monitor knob. Vertical drag changes the value; double-click resets.
/// Cheap enough for SwiftUI — it repaints on gesture, not on the 30 Hz tick.
struct RotaryKnob: View {
    let value: Float
    let range: ClosedRange<Float>
    let resetValue: Float
    let onChange: (Float) -> Void

    @State private var dragStartValue: Float?

    private var normalized: Double {
        Double((value - range.lowerBound) / (range.upperBound - range.lowerBound))
    }

    /// Pointer sweeps 270°, from 7 o'clock round to 5 o'clock.
    private var angle: Angle {
        .degrees(-135 + normalized * 270)
    }

    var body: some View {
        ZStack {
            Circle()
                .fill(
                    RadialGradient(
                        colors: [Color(hex: 0x564a3e), Theme.Palette.panel],
                        center: UnitPoint(x: 0.5, y: 0.36),
                        startRadius: 2,
                        endRadius: 42
                    )
                )
                .overlay(Circle().stroke(Theme.Palette.divider, lineWidth: 1))
                .shadow(color: .black.opacity(0.5), radius: 6, y: 3)

            // Value arc
            Circle()
                .trim(from: 0, to: normalized * 0.75)
                .stroke(Theme.Palette.purple, style: StrokeStyle(lineWidth: 2, lineCap: .round))
                .rotationEffect(.degrees(135))
                .padding(3)

            // Pointer
            Capsule()
                .fill(Theme.Palette.purpleLight)
                .frame(width: 2.5, height: 16)
                .offset(y: -14)
                .rotationEffect(angle)

            Text(String(format: "%.0f", value))
                .font(Theme.Font.mono(13, .semibold))
                .foregroundStyle(Theme.Palette.textNumeric)
                .offset(y: 12)
        }
        .frame(width: 64, height: 64)
        .contentShape(Circle())
        .gesture(
            DragGesture(minimumDistance: 1)
                .onChanged { drag in
                    let start = dragStartValue ?? value
                    if dragStartValue == nil { dragStartValue = start }
                    let span = range.upperBound - range.lowerBound
                    let delta = Float(-drag.translation.height / 120) * span
                    onChange(min(range.upperBound, max(range.lowerBound, start + delta)))
                }
                .onEnded { _ in dragStartValue = nil }
        )
        .onTapGesture(count: 2) { onChange(resetValue) }
    }
}

/// Horizontal fill bar for the DSP-load / jitter / phase readouts.
struct MeterBar: View {
    let fraction: Double
    let gradient: LinearGradient

    var body: some View {
        GeometryReader { geo in
            ZStack(alignment: .leading) {
                Capsule().fill(Theme.Palette.recess)
                Capsule()
                    .fill(gradient)
                    .frame(width: max(0, min(1, fraction)) * geo.size.width)
            }
        }
        .frame(height: 4)
    }
}

/// The design draws 24 spectrum bars. The engine publishes three bands, so each
/// band fills eight bars — no interpolation is invented between them.
struct SpectrumBars: View {
    let low: Float
    let mid: Float
    let high: Float

    private var bands: [(Float, Color)] {
        [(low, Theme.Palette.accent), (mid, Theme.Palette.teal), (high, Theme.Palette.purple)]
    }

    var body: some View {
        HStack(alignment: .bottom, spacing: 2) {
            ForEach(0..<24, id: \.self) { index in
                let (level, color) = bands[index / 8]
                RoundedRectangle(cornerRadius: Theme.Radius.meterCell)
                    .fill(color)
                    .frame(height: max(2, CGFloat(meterFraction(level)) * 34))
            }
        }
        .frame(height: 34, alignment: .bottom)
    }
}

/// Pill toggle used by the Monitor DSP module rows.
struct ModuleSwitch: View {
    let isOn: Bool
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            ZStack(alignment: isOn ? .trailing : .leading) {
                Capsule()
                    .fill(isOn ? Theme.Palette.purple : Theme.Palette.divider)
                    .frame(width: 30, height: 17)
                Circle()
                    .fill(.white)
                    .frame(width: 13, height: 13)
                    .shadow(color: .black.opacity(0.4), radius: 1, y: 1)
                    .padding(2)
            }
        }
        .buttonStyle(.plain)
        .animation(.easeOut(duration: 0.15), value: isOn)
    }
}

/// Small labelled row: caption on the left, value on the right.
struct StatRow: View {
    let label: String
    let value: String
    var valueColor: Color = Theme.Palette.textSecondary

    var body: some View {
        HStack {
            Text(label)
                .font(Theme.Font.ui(9))
                .foregroundStyle(Theme.Palette.textLabel)
            Spacer()
            Text(value)
                .font(Theme.Font.mono(9, .medium))
                .foregroundStyle(valueColor)
        }
    }
}

/// Segmented row of equal-width buttons.
struct SegmentedRow<T: Hashable>: View {
    let items: [T]
    let label: (T) -> String
    let isActive: (T) -> Bool
    let action: (T) -> Void
    var activeTint: Color = Theme.Palette.purpleLight
    var activeFill: Color = Color(hex: 0x2a1f3d)
    var activeBorder: Color = Color(hex: 0x48376b)
    var fontSize: CGFloat = 10

    var body: some View {
        HStack(spacing: Theme.Space.sm) {
            ForEach(items, id: \.self) { item in
                let active = isActive(item)
                Button { action(item) } label: {
                    Text(label(item))
                        .font(Theme.Font.ui(fontSize, active ? .semibold : .regular))
                        .foregroundStyle(active ? activeTint : Theme.Palette.textMuted)
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, Theme.Space.md)
                        .background(
                            RoundedRectangle(cornerRadius: Theme.Radius.button)
                                .fill(active ? activeFill : Theme.Palette.button)
                                .overlay(
                                    RoundedRectangle(cornerRadius: Theme.Radius.button)
                                        .stroke(active ? activeBorder : Theme.Palette.divider, lineWidth: 1)
                                )
                        )
                }
                .buttonStyle(.plain)
            }
        }
    }
}
