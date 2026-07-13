import SwiftUI

// The Nuendo-style Edit-view side panels. Two columns that both follow the
// selected track (`engine.inspectedTrack`):
//
//   • ChannelColumn — reuses the mixer's `ChannelStrip` verbatim, so the leftmost
//     column IS the existing channel (no separate design).
//   • TrackInspector — the track's controls, laid out vertically.
//
// Both are toggled from the transport bar (`engine.showChannelColumn` /
// `showInspector`) and collapse via the header chevron.

// MARK: - Shared column header

private struct PanelColumnHeader: View {
    let title: String
    var trailingChevron: String = "‹"
    let onCollapse: () -> Void

    var body: some View {
        HStack(spacing: Theme.Space.sm) {
            Text(title)
                .font(Theme.Font.mono(8, .semibold))
                .tracking(0.6)
                .foregroundStyle(Theme.Palette.textMuted)
            Spacer(minLength: 0)
            Button(action: onCollapse) {
                Text(trailingChevron)
                    .font(Theme.Font.ui(11))
                    .foregroundStyle(Theme.Palette.textMuted)
                    .frame(width: 16, height: 16)
                    .background(RoundedRectangle(cornerRadius: 4).fill(Theme.Palette.button))
            }
            .buttonStyle(.plain)
            .help("패널 숨기기")
        }
        .padding(.horizontal, Theme.Space.md)
        .frame(height: 26)
        .background(Theme.Palette.ruler)
        .overlay(alignment: .bottom) { Rectangle().fill(Theme.Palette.deepBorder).frame(height: 1) }
    }
}

// MARK: - Channel column (reuses the existing ChannelStrip)

struct ChannelColumn: View {
    @EnvironmentObject private var engine: EngineController

    // Follows the selected channel's actual mixer width, so the small/large toggle set
    // in the mixer is reflected here too.
    private var stripWidth: CGFloat {
        engine.inspectedTrack.map { engine.channelWidthFor($0.id) } ?? EngineController.channelWidthMin
    }
    private var columnWidth: CGFloat { stripWidth + 12 }

    var body: some View {
        VStack(spacing: 0) {
            PanelColumnHeader(title: "CHANNEL") { engine.showChannelColumn = false }

            if let track = engine.inspectedTrack {
                ScrollView(.vertical, showsIndicators: false) {
                    ChannelStrip(track: track,
                                 isChild: false,
                                 showIO: true,
                                 showInserts: true,
                                 showSends: true,
                                 showMemo: false,
                                 fixedWidth: stripWidth)
                        .padding(.vertical, Theme.Space.md)
                        .frame(maxWidth: .infinity)
                }
            } else {
                Spacer()
            }
        }
        .frame(width: columnWidth)
        .background(Theme.Palette.surface)
    }
}

// MARK: - Inspector

struct TrackInspector: View {
    @EnvironmentObject private var engine: EngineController

    @State private var renaming = false
    @State private var draftName = ""
    @State private var routingExpanded = true
    @FocusState private var nameFocused: Bool

    private var track: EngineController.Track? { engine.inspectedTrack }

    var body: some View {
        VStack(spacing: 0) {
            PanelColumnHeader(title: "인스펙터", trailingChevron: "›") { engine.showInspector = false }

            if let track {
                ScrollView(.vertical, showsIndicators: false) {
                    VStack(alignment: .leading, spacing: Theme.Space.lg) {
                        identityRow(track)
                        channelFormatRow(track)
                        buttonGrid(track)
                        volumeRow(track)
                        meterRow(track)
                        panRow(track)
                        routingSection(track)
                    }
                    .padding(Theme.Space.lg)
                }
            } else {
                Spacer()
                Text("선택된 트랙 없음")
                    .font(Theme.Font.ui(10))
                    .foregroundStyle(Theme.Palette.textFaint)
                    .frame(maxWidth: .infinity)
                Spacer()
            }
        }
        .frame(width: 172)
        .background(Theme.Palette.rail)
    }

    // MARK: Sections

    @ViewBuilder
    private func identityRow(_ track: EngineController.Track) -> some View {
        HStack(spacing: Theme.Space.sm) {
            RoundedRectangle(cornerRadius: 2).fill(track.kind.accent).frame(width: 4, height: 22)
            if renaming {
                TextField("", text: $draftName)
                    .textFieldStyle(.plain)
                    .font(Theme.Font.ui(13, .semibold))
                    .foregroundStyle(Theme.Palette.textBright)
                    .focused($nameFocused)
                    .onSubmit(commitRename)
                    .onExitCommand { renaming = false }
            } else {
                Text(track.name)
                    .font(Theme.Font.ui(13, .semibold))
                    .foregroundStyle(Theme.Palette.textBright)
                    .lineLimit(1)
                    .onTapGesture(count: 2) {
                        draftName = track.name
                        renaming = true
                        nameFocused = true
                    }
            }
            Spacer(minLength: 0)
            Text(track.kind.label)
                .font(Theme.Font.mono(7.5))
                .foregroundStyle(track.kind.accent)
        }
    }

    private func commitRename() {
        if let track, !draftName.isEmpty { _ = engine.renameTrack(track.id, to: draftName) }
        renaming = false
    }

    @ViewBuilder
    private func buttonGrid(_ track: EngineController.Track) -> some View {
        // One compact row like the timeline track header: M · S · R(arm) · I · automation.
        // The automation mode chip that used to sit in its own row below is now the fifth
        // button; the space beneath is left free for controls added later.
        HStack(spacing: Theme.Space.sm) {
            stateButton("M", on: track.muted, onColor: Theme.Palette.orange) { engine.toggleTrackMute(track.id) }
            if track.kind.hasSolo {
                stateButton("S", on: track.solo, onColor: Theme.Palette.yellow) { engine.toggleTrackSolo(track.id) }
                stateButton("●", on: track.recordArmed, onColor: Theme.Palette.red) { engine.toggleTrackArm(track.id) }
                stateButton("I", on: track.inputMonitoring, onColor: Theme.Palette.accent) {
                    engine.toggleTrackInputMonitoring(track.id)
                }
                automationChip(track)
            }
        }
    }

    private func stateButton(_ label: String, on: Bool, onColor: Color, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Group {
                if label == "●" {
                    Image(systemName: "circle").font(.system(size: 9))
                } else {
                    Text(label).font(Theme.Font.ui(9.5, .semibold))
                }
            }
                // Off = the same colour, unlit (dim fill + dimmed glyph), not neutral grey.
                .foregroundStyle(on ? Color.black.opacity(0.85) : onColor.opacity(0.8))
                .frame(maxWidth: .infinity)
                .frame(height: 19)
                .background(
                    RoundedRectangle(cornerRadius: Theme.Radius.button)
                        .fill(on ? onColor : onColor.opacity(0.16))
                        .overlay(RoundedRectangle(cornerRadius: Theme.Radius.button)
                            .stroke(on ? Theme.Palette.divider : onColor.opacity(0.32), lineWidth: 1))
                )
        }
        .buttonStyle(.plain)
    }

    /// The automation-mode chip as the fifth button — its letter (R/T/L/W/O) and colour
    /// track the mode, clicking cycles it. Mirrors the timeline track header.
    private func automationChip(_ track: EngineController.Track) -> some View {
        let mode = EngineController.automationModes.first { $0.id == track.automationMode }
        let letter = String((mode?.label ?? "Read").prefix(1)).uppercased()
        return Button { engine.cycleAutomationMode(track.id) } label: {
            Text(letter)
                .font(Theme.Font.ui(9.5, .bold))
                .foregroundStyle(Color.black.opacity(0.85))
                .frame(maxWidth: .infinity)
                .frame(height: 19)
                .background(
                    RoundedRectangle(cornerRadius: Theme.Radius.button)
                        .fill(automationColor(track.automationMode))
                        .overlay(RoundedRectangle(cornerRadius: Theme.Radius.button)
                            .stroke(Theme.Palette.divider, lineWidth: 1))
                )
        }
        .buttonStyle(.plain)
        .helpTip(engine.tr("help.automation_mode"))
    }

    private func automationColor(_ mode: String) -> Color {
        switch mode {
        case "write": return Theme.Palette.red
        case "touch", "latch": return Theme.Palette.yellow
        case "off": return Theme.Palette.textFaint
        default: return Theme.Palette.accent
        }
    }

    @ViewBuilder
    private func volumeRow(_ track: EngineController.Track) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text("볼륨").font(Theme.Font.ui(9)).foregroundStyle(Theme.Palette.textFaint)
                Spacer()
                Text(dbLabel(track.volumeDb))
                    .font(Theme.Font.mono(9.5))
                    .foregroundStyle(Theme.Palette.textSecondary)
            }
            // The actual mixer ChannelFader, rotated 90° clockwise so it lies on its side
            // (right = louder) — the exact same control, not a look-alike.
            GeometryReader { geo in
                ChannelFader(volumeDb: track.volumeDb, accent: track.kind.accent,
                             onChange: { engine.setTrackVolume(track.id, $0) },
                             onCommit: { engine.recordGesture("볼륨") })
                    .frame(width: 24, height: geo.size.width)
                    .rotationEffect(.degrees(90))
                    .frame(width: geo.size.width, height: 24)
            }
            .frame(height: 24)
            inspectorFaderScale
        }
    }

    /// The channel fader's dB legend, laid out horizontally under the Inspector fader —
    /// the same FaderScale taper (+12 at the right … -∞ at the left).
    private var inspectorFaderScale: some View {
        let marks: [(String, Float)] = [("+12", 12), ("0", 0), ("-12", -12), ("-24", -24), ("∞", -120)]
        return GeometryReader { geo in
            ForEach(Array(marks.enumerated()), id: \.offset) { _, m in
                let x = geo.size.width * CGFloat(FaderScale.position(forDb: m.1))
                VStack(spacing: 1) {
                    Rectangle().fill(Color(hex: 0x4a4f56)).frame(width: 1, height: 2)
                    Text(m.0).font(Theme.Font.mono(6)).foregroundStyle(Theme.Palette.textFaint)
                }
                .fixedSize()
                .position(x: min(geo.size.width - 7, max(7, x)), y: 5)
            }
        }
        .frame(height: 11)
    }

    /// Mono/stereo channel format. A mono track sums to one channel panned into the field.
    @ViewBuilder
    private func channelFormatRow(_ track: EngineController.Track) -> some View {
        if track.kind.hasSolo {
            HStack(spacing: 4) {
                Text("채널").font(Theme.Font.ui(9)).foregroundStyle(Theme.Palette.textFaint)
                Spacer()
                formatChip("스테레오", selected: track.isStereo, accent: track.kind.accent) { engine.setTrackStereo(track.id, true) }
                formatChip("모노", selected: !track.isStereo, accent: track.kind.accent) { engine.setTrackStereo(track.id, false) }
            }
        }
    }

    private func formatChip(_ label: String, selected: Bool, accent: Color, _ action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(label)
                .font(Theme.Font.mono(9, .medium))
                .foregroundStyle(selected ? Theme.Palette.textBright : Theme.Palette.textSecondary)
                .padding(.horizontal, 8).frame(height: 18)
                .background(RoundedRectangle(cornerRadius: 4).fill(selected ? accent.opacity(0.5) : Theme.Palette.button))
        }
        .buttonStyle(.plain)
    }

    @ViewBuilder
    private func meterRow(_ track: EngineController.Track) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text("레벨").font(Theme.Font.ui(9)).foregroundStyle(Theme.Palette.textFaint)
            // The mixer's own meter view — same dB mapping, gradient and ballistics.
            HorizontalMeter(peakLeft: track.peakLeft, peakRight: track.peakRight)
        }
    }

    @ViewBuilder
    private func panRow(_ track: EngineController.Track) -> some View {
        if track.kind.hasSolo {
            VStack(alignment: .leading, spacing: 4) {
                HStack {
                    Text("팬").font(Theme.Font.ui(9)).foregroundStyle(Theme.Palette.textFaint)
                    Spacer()
                    Text(track.panLabel)
                        .font(Theme.Font.mono(9.5))
                        .foregroundStyle(Theme.Palette.textSecondary)
                }
                PanSlider(pan: track.pan, accent: track.kind.accent,
                          onChange: { engine.setTrackPan(track.id, $0) },
                          onCommit: { engine.recordGesture("팬") })
            }
        }
    }

    @ViewBuilder
    private func routingSection(_ track: EngineController.Track) -> some View {
        VStack(alignment: .leading, spacing: 5) {
            // Cubase-style disclosure header: click the triangle to fold the input/output
            // routing in and out.
            Button { withAnimation(.easeInOut(duration: 0.15)) { routingExpanded.toggle() } } label: {
                HStack(spacing: 5) {
                    Image(systemName: routingExpanded ? "chevron.down" : "chevron.right")
                        .font(.system(size: 8, weight: .semibold))
                        .foregroundStyle(Theme.Palette.textMuted)
                    Text("라우팅").font(Theme.Font.ui(10, .semibold)).foregroundStyle(Theme.Palette.textSecondary)
                    Spacer(minLength: 0)
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)

            if routingExpanded {
                if track.kind == .audio {
                    routingMenu(tint: Theme.Palette.green, label: "입력",
                                current: track.inputBus.isEmpty ? "없음" : track.inputBus,
                                options: engine.audioInputOptions()) { engine.setTrackInputBus(track.id, $0) }
                }
                routingMenu(tint: Theme.Palette.amber, label: "출력",
                            current: track.outputBus.isEmpty ? "Master" : track.outputBus,
                            options: engine.outputBusOptions(track.id)) { engine.setTrackOutputBus(track.id, $0) }
            }
        }
    }

    private func routingMenu(tint: Color, label: String, current: String,
                             options: [String], pick: @escaping (String) -> Void) -> some View {
        Menu {
            ForEach(options, id: \.self) { option in
                Button {
                    pick(option)
                } label: {
                    if option == current { Label(option, systemImage: "checkmark") } else { Text(option) }
                }
            }
        } label: {
            HStack(spacing: Theme.Space.sm) {
                Circle().fill(tint).frame(width: 6, height: 6)
                Text(label)
                    .font(Theme.Font.ui(9.5))
                    .foregroundStyle(Theme.Palette.textFaint)
                Spacer(minLength: 6)
                Text(current)
                    .font(Theme.Font.ui(10, .medium))
                    .foregroundStyle(Theme.Palette.textBright)
                    .lineLimit(1)
                Image(systemName: "chevron.down").font(.system(size: 7, weight: .semibold))
                    .foregroundStyle(Theme.Palette.textMuted)
            }
            .padding(.horizontal, 9)
            .padding(.vertical, 5)
            .background(
                RoundedRectangle(cornerRadius: Theme.Radius.button)
                    .fill(Theme.Palette.button)
                    .overlay(RoundedRectangle(cornerRadius: Theme.Radius.button)
                        .stroke(Theme.Palette.coolDivider, lineWidth: 1))
            )
        }
        .menuStyle(.button)
        .buttonStyle(.plain)
        .menuIndicator(.hidden)
    }
}
