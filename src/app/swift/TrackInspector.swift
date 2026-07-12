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

    // Uniform, fixed — the column stays the same size whatever track is selected and
    // whatever per-track width that channel uses in the mixer.
    private let stripWidth = EngineController.channelWidthDefault
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
                                 showDynamics: false,
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
    @FocusState private var nameFocused: Bool

    private var track: EngineController.Track? { engine.inspectedTrack }

    var body: some View {
        VStack(spacing: 0) {
            PanelColumnHeader(title: "인스펙터", trailingChevron: "›") { engine.showInspector = false }

            if let track {
                ScrollView(.vertical, showsIndicators: false) {
                    VStack(alignment: .leading, spacing: Theme.Space.lg) {
                        identityRow(track)
                        buttonGrid(track)
                        volumeRow(track)
                        panRow(track)
                        routingSection(track)
                        collapsedSections
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
        HStack(spacing: Theme.Space.sm) {
            stateButton("M", on: track.muted, onColor: Theme.Palette.amber) { engine.toggleTrackMute(track.id) }
            if track.kind.hasSolo {
                stateButton("S", on: track.solo, onColor: Theme.Palette.yellow) { engine.toggleTrackSolo(track.id) }
                stateButton("●", on: track.recordArmed, onColor: Theme.Palette.red) { engine.toggleTrackArm(track.id) }
                stateButton("🔊", on: track.inputMonitoring, onColor: Theme.Palette.accent) {
                    engine.toggleTrackInputMonitoring(track.id)
                }
            }
        }
        if track.kind.hasSolo { automationModeRow(track) }
    }

    private func stateButton(_ label: String, on: Bool, onColor: Color, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(label)
                .font(Theme.Font.ui(10, .semibold))
                .foregroundStyle(on ? Color.black.opacity(0.85) : Theme.Palette.textMuted)
                .frame(maxWidth: .infinity)
                .frame(height: 22)
                .background(
                    RoundedRectangle(cornerRadius: Theme.Radius.button)
                        .fill(on ? onColor : Theme.Palette.button)
                        .overlay(RoundedRectangle(cornerRadius: Theme.Radius.button)
                            .stroke(Theme.Palette.divider, lineWidth: 1))
                )
        }
        .buttonStyle(.plain)
    }

    @ViewBuilder
    private func automationModeRow(_ track: EngineController.Track) -> some View {
        let mode = EngineController.automationModes.first { $0.id == track.automationMode }
        Button { engine.cycleAutomationMode(track.id) } label: {
            HStack(spacing: Theme.Space.sm) {
                Text("자동화")
                    .font(Theme.Font.ui(9))
                    .foregroundStyle(Theme.Palette.textFaint)
                Spacer(minLength: 0)
                Text((mode?.label ?? "Read").uppercased())
                    .font(Theme.Font.mono(8.5, .semibold))
                    .foregroundStyle(automationColor(track.automationMode))
                    .padding(.horizontal, 8).padding(.vertical, 3)
                    .background(RoundedRectangle(cornerRadius: 3).fill(automationColor(track.automationMode).opacity(0.16)))
            }
            .padding(.horizontal, Theme.Space.sm)
            .frame(height: 24)
            .background(RoundedRectangle(cornerRadius: Theme.Radius.button).fill(Theme.Palette.button))
        }
        .buttonStyle(.plain)
        .help("자동화 모드 순환 (Read → Touch → Latch → Write → Off)")
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
            InspectorHFader(volumeDb: track.volumeDb, accent: track.kind.accent,
                            onChange: { engine.setTrackVolume(track.id, $0) },
                            onCommit: { engine.recordGesture("볼륨") })
                .frame(height: 14)
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
            Text("라우팅").font(Theme.Font.ui(9)).foregroundStyle(Theme.Palette.textFaint)

            if track.kind == .audio {
                routingMenu(icon: "arrow.down.right", tint: Theme.Palette.green, label: "입력",
                            current: track.inputBus.isEmpty ? "없음" : track.inputBus,
                            options: engine.audioInputOptions()) { engine.setTrackInputBus(track.id, $0) }
            }
            routingMenu(icon: "arrow.up.right", tint: Theme.Palette.amber, label: "출력",
                        current: track.outputBus.isEmpty ? "Master" : track.outputBus,
                        options: engine.outputBusOptions(track.id)) { engine.setTrackOutputBus(track.id, $0) }
        }
    }

    /// Sections the design lays out but the engine doesn't back yet — shown collapsed,
    /// exactly as the mockup renders them, rather than faked as working controls.
    private var collapsedSections: some View {
        VStack(alignment: .leading, spacing: Theme.Space.md) {
            Rectangle().fill(Theme.Palette.divider).frame(height: 1).padding(.vertical, 2)
            ForEach(["트랙 프리셋", "트랙 버전", "퀵 컨트롤", "노트패드"], id: \.self) { title in
                HStack(spacing: Theme.Space.sm) {
                    Image(systemName: "chevron.right").font(.system(size: 7)).foregroundStyle(Theme.Palette.textFaint)
                    Text(title).font(Theme.Font.ui(9.5)).foregroundStyle(Theme.Palette.textMuted)
                    Spacer(minLength: 0)
                }
            }
        }
    }

    private func routingMenu(icon: String, tint: Color, label: String, current: String,
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
                Image(systemName: icon).font(.system(size: 9, weight: .semibold)).foregroundStyle(tint)
                Text(label)
                    .font(Theme.Font.ui(9.5))
                    .foregroundStyle(Theme.Palette.textFaint)
                Text(current)
                    .font(Theme.Font.ui(10, .medium))
                    .foregroundStyle(Theme.Palette.textBright)
                    .lineLimit(1)
                Spacer(minLength: 0)
                Image(systemName: "chevron.down").font(.system(size: 7, weight: .semibold))
                    .foregroundStyle(Theme.Palette.textMuted)
            }
            .padding(.horizontal, 9)
            .frame(height: 24)
            .background(
                RoundedRectangle(cornerRadius: Theme.Radius.button)
                    .fill(Theme.Palette.button)
                    .overlay(RoundedRectangle(cornerRadius: Theme.Radius.button)
                        .stroke(Theme.Palette.coolDivider, lineWidth: 1))
            )
        }
        .menuStyle(.borderlessButton).menuIndicator(.hidden)
    }
}

// MARK: - Horizontal tapered volume fader (mirrors the mixer's FaderScale taper)

struct InspectorHFader: View {
    let volumeDb: Float
    let accent: Color
    let onChange: (Float) -> Void
    var onCommit: () -> Void = {}

    var body: some View {
        GeometryReader { geo in
            let pos = FaderScale.position(forDb: volumeDb)
            ZStack(alignment: .leading) {
                Capsule().fill(Theme.Palette.recess)
                Capsule().fill(LinearGradient(colors: [accent.opacity(0.5), accent],
                                              startPoint: .leading, endPoint: .trailing))
                    .frame(width: max(6, geo.size.width * pos))
                // The same physical cap as the channel fader, laid on its side (a
                // vertical accent line, since this fader travels left-right).
                ZStack {
                    RoundedRectangle(cornerRadius: 3)
                        .fill(RadialGradient(colors: [Color(hex: 0x3c444e), Color(hex: 0x171c22)],
                                             center: UnitPoint(x: 0.5, y: 0.35), startRadius: 1, endRadius: 16))
                        .overlay(RoundedRectangle(cornerRadius: 3).stroke(Color.white.opacity(0.18), lineWidth: 1))
                        .shadow(color: .black.opacity(0.6), radius: 2, y: 1)
                    Rectangle().fill(accent).frame(width: 2)
                        .shadow(color: accent.opacity(0.8), radius: 2)
                }
                .frame(width: 12, height: 18)
                .offset(x: max(0, min(geo.size.width - 12, geo.size.width * pos - 6)))
            }
            .contentShape(Rectangle())
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { value in
                        let p = min(1, max(0, value.location.x / geo.size.width))
                        onChange(FaderScale.db(forPosition: p))
                    }
                    .onEnded { _ in onCommit() }
            )
        }
    }
}
