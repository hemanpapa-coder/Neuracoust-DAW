import SwiftUI

struct TitleBar: View {
    @EnvironmentObject private var engine: EngineController

    var body: some View {
        ZStack {
            HStack(spacing: Theme.Space.md) {
                Text("Neuracoust DAW")
                    .font(Theme.Font.ui(11, .semibold))
                    .foregroundStyle(Theme.Palette.text)
                Text("· \(documentLabel)")
                    .font(Theme.Font.ui(11))
                    .foregroundStyle(Theme.Palette.textMuted)
                // Amber only when there are unsaved changes.
                Circle()
                    .fill(engine.projectDirty ? Theme.Palette.amber : Theme.Palette.textFainter)
                    .frame(width: 6, height: 6)
            }

            HStack {
                Spacer()
                HStack(spacing: Theme.Space.md) {
                    Circle()
                        .fill(engine.running ? Theme.Palette.green : Theme.Palette.red)
                        .frame(width: 6, height: 6)
                    Text(engine.running
                         ? "\(engine.deviceName) · \(formatSampleRate(engine.sampleRate)) · \(engine.bufferSize)"
                         : "엔진 정지")
                        .font(Theme.Font.mono(9))
                        .foregroundStyle(Theme.Palette.textFaint)
                }
                .padding(.trailing, Theme.Space.xxl)
            }
        }
        .frame(height: 34)
        .frame(maxWidth: .infinity)
        .background(Theme.Gradient.titlebar)
    }

    /// The file name once the document has a home; otherwise its in-memory name.
    private var documentLabel: String {
        engine.projectPath.isEmpty
            ? "\(engine.projectName).ndaw"
            : (engine.projectPath as NSString).lastPathComponent
    }

    private func formatSampleRate(_ rate: Double) -> String {
        rate <= 0 ? "—" : String(format: "%.1fk", rate / 1000.0)
    }
}

struct TransportBar: View {
    @EnvironmentObject private var engine: EngineController

    var body: some View {
        HStack(spacing: Theme.Space.lg) {
            transportButtons
            separator
            toggles
            separator
            displays
            tempoPods
            Spacer(minLength: Theme.Space.xl)
            masterMeter
            viewTabs
        }
        .padding(.horizontal, Theme.Space.xxl)
        .frame(height: 52)
        .frame(maxWidth: .infinity)
        .background(Theme.Gradient.transport)
    }

    private var separator: some View {
        Rectangle()
            .fill(Theme.Palette.coolDivider)
            .frame(width: 1, height: 24)
    }

    private var transportButtons: some View {
        HStack(spacing: Theme.Space.sm) {
            transportKey("backward.end.fill") { engine.rewind() }
            transportKey("backward.fill") { engine.seek(engine.playheadSeconds - 2) }
            transportKey("forward.fill") { engine.seek(engine.playheadSeconds + 2) }
            transportKey("forward.end.fill") {}
            transportKey(engine.transportRunning ? "pause.fill" : "play.fill",
                         tint: engine.transportRunning ? Theme.Palette.green : Theme.Palette.text) {
                engine.togglePlay()
            }
            transportKey("stop.fill") { engine.stop() }
            // Not a take recorder: it arms the input monitor path. Drawn hollow so it
            // does not read as a transport that captures audio.
            transportKey("circle",
                         tint: engine.recording ? Theme.Palette.red : Theme.Palette.textDim) {
                engine.toggleRecording()
            }
            .help("입력 모니터 경로 (녹음 아님)")
        }
    }

    private func transportKey(_ symbol: String,
                              tint: Color = Theme.Palette.textSecondary,
                              action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol)
                .font(.system(size: 10))
                .foregroundStyle(tint)
                .frame(width: 28, height: 24)
                .background(
                    RoundedRectangle(cornerRadius: Theme.Radius.button)
                        .fill(Theme.Palette.button)
                        .overlay(
                            RoundedRectangle(cornerRadius: Theme.Radius.button)
                                .stroke(Theme.Palette.border, lineWidth: 1)
                        )
                )
        }
        .buttonStyle(.plain)
    }

    private var toggles: some View {
        HStack(spacing: Theme.Space.sm) {
            toggle("Loop", isOn: engine.loopEnabled, tint: Theme.Palette.green) { engine.toggleLoop() }
            toggle("Click", isOn: engine.clickEnabled, tint: Theme.Palette.amber) { engine.toggleClick() }
            toggle("Snap", isOn: engine.snapEnabled, tint: Theme.Palette.accent) { engine.snapEnabled.toggle() }
        }
    }

    private func toggle(_ title: String,
                        isOn: Bool,
                        tint: Color,
                        action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(title)
                .font(Theme.Font.ui(9, .medium))
                .foregroundStyle(isOn ? tint : Theme.Palette.textFaint)
                .padding(.horizontal, 9)
                .frame(height: 24)
                .background(
                    RoundedRectangle(cornerRadius: Theme.Radius.button)
                        .fill(isOn ? tint.opacity(0.14) : Theme.Palette.button)
                        .overlay(
                            RoundedRectangle(cornerRadius: Theme.Radius.button)
                                .stroke(isOn ? tint.opacity(0.5) : Theme.Palette.border, lineWidth: 1)
                        )
                )
        }
        .buttonStyle(.plain)
    }

    private var displays: some View {
        let position = engine.barsBeats
        return HStack(spacing: 0) {
            display {
                HStack(spacing: Theme.Space.md) {
                    numeric(String(format: "%03d", position.bar))
                    divider
                    numeric(String(format: "%02d", position.beat))
                    divider
                    numeric(String(format: "%03d", position.tick))
                }
                caption("BARS | BEATS")
            }
            display {
                numeric(engine.timecode)
                caption("HH:MM:SS:FF · 25fps")
            }
        }
    }

    private var divider: some View {
        Text("|").font(Theme.Font.mono(14)).foregroundStyle(Theme.Palette.textFainter)
    }

    private func numeric(_ text: String) -> some View {
        Text(text)
            .font(Theme.Font.mono(20, .semibold))
            .foregroundStyle(Theme.Palette.accent)
    }

    private func caption(_ text: String) -> some View {
        Text(text)
            .font(Theme.Font.mono(6.5))
            .tracking(0.6)
            .foregroundStyle(Theme.Palette.textFaint)
    }

    private func display<Content: View>(@ViewBuilder _ content: () -> Content) -> some View {
        VStack(spacing: 1) { content() }
            .padding(.horizontal, Theme.Space.xxl)
            .frame(height: 40)
            .background(
                RoundedRectangle(cornerRadius: Theme.Radius.display)
                    .fill(Theme.Palette.recess)
            )
            .padding(.horizontal, 1)
    }

    private var tempoPods: some View {
        HStack(spacing: Theme.Space.xs) {
            pod(String(format: "%.1f", Double(engine.tempoBpm)), "TEMPO")
            pod("\(engine.timeSignature.numerator)/\(engine.timeSignature.denominator)", "SIG")
        }
        .padding(.leading, Theme.Space.lg)
    }

    private func pod(_ value: String, _ label: String) -> some View {
        VStack(spacing: 1) {
            Text(value)
                .font(Theme.Font.mono(16, .semibold))
                .foregroundStyle(Theme.Palette.textNumeric)
            Text(label)
                .font(Theme.Font.mono(6.5))
                .tracking(0.6)
                .foregroundStyle(Theme.Palette.textFaint)
        }
        .frame(width: 58, height: 40)
        .background(
            RoundedRectangle(cornerRadius: Theme.Radius.display)
                .fill(Theme.Palette.ruler)
        )
    }

    private var masterMeter: some View {
        VStack(alignment: .trailing, spacing: 2) {
            Text("MASTER OUT")
                .font(Theme.Font.mono(6.5))
                .tracking(0.6)
                .foregroundStyle(Theme.Palette.textFaint)
            HStack(spacing: Theme.Space.md) {
                Text(dbLabel)
                    .font(Theme.Font.mono(9, .semibold))
                    .foregroundStyle(Theme.Palette.yellow)
                    .frame(width: 34, alignment: .trailing)
                VStack(spacing: 2) {
                    meterBar(meterFraction(engine.outputPeakLeft))
                    meterBar(meterFraction(engine.outputPeakRight))
                }
                .frame(width: 160)
            }
        }
    }

    private var dbLabel: String {
        let peak = max(engine.outputPeakLeft, engine.outputPeakRight)
        return peak <= 0.00001 ? "-∞" : String(format: "%.1f", peakToDb(peak))
    }

    private func meterBar(_ fraction: Double) -> some View {
        GeometryReader { geo in
            ZStack(alignment: .leading) {
                RoundedRectangle(cornerRadius: Theme.Radius.meterCell)
                    .fill(Theme.Palette.recess)
                RoundedRectangle(cornerRadius: Theme.Radius.meterCell)
                    .fill(Theme.Gradient.masterMeter)
                    .mask(alignment: .leading) {
                        Rectangle().frame(width: geo.size.width * fraction)
                    }
            }
        }
        .frame(height: 6)
    }

    private var viewTabs: some View {
        HStack(spacing: 2) {
            ForEach(EngineController.ViewTab.allCases) { tab in
                Button {
                    engine.viewTab = tab
                } label: {
                    Text(tab.rawValue)
                        .font(Theme.Font.ui(10, .semibold))
                        .foregroundStyle(engine.viewTab == tab ? Theme.Palette.deepBorder : Theme.Palette.textDim)
                        .padding(.horizontal, Theme.Space.xxl)
                        .frame(height: 24)
                        .background(
                            RoundedRectangle(cornerRadius: Theme.Radius.button)
                                .fill(engine.viewTab == tab ? Theme.Palette.tabActive : .clear)
                        )
                }
                .buttonStyle(.plain)
            }
        }
        .padding(3)
        .background(
            RoundedRectangle(cornerRadius: Theme.Radius.panel)
                .fill(Theme.Palette.surface)
        )
    }
}

struct StatusStrip: View {
    @EnvironmentObject private var engine: EngineController

    var body: some View {
        HStack(spacing: Theme.Space.xl) {
            stat("DSP", engine.sampleRate > 0 ? String(format: "%.1fk", engine.sampleRate / 1000) : "—")
            stat("PDC", String(format: "%.2f ms", engine.delayCompensationMs))
            stat("RENDER", String(format: "%.1f ms", engine.maxRenderDurationUs / 1000.0))

            if engine.activeInsertCount > 0 {
                stat("INSERTS", "\(engine.activeInsertCount)")
            }

            if engine.canUndo {
                stat("UNDO", engine.undoStepName)
            }

            marker("◆", "MONITOR DSP", Theme.Palette.purple)
            marker("◇", "REMOTE CORE", Theme.Palette.teal)

            Spacer()

            if let error = engine.startupError {
                Text("⚠︎ \(error)")
                    .font(Theme.Font.mono(8.5))
                    .foregroundStyle(Theme.Palette.red)
            }

            Text("플러그인 / 인서트")
                .font(Theme.Font.ui(8.5, .medium))
                .foregroundStyle(Theme.Palette.textSecondary)
                .padding(.horizontal, Theme.Space.lg)
                .frame(height: 18)
                .background(
                    RoundedRectangle(cornerRadius: Theme.Radius.pill)
                        .fill(Theme.Palette.buttonProminent)
                )
        }
        .padding(.horizontal, Theme.Space.xxl)
        .frame(height: 22)
        .frame(maxWidth: .infinity)
        .background(Theme.Palette.surface)
        .overlay(alignment: .bottom) {
            Rectangle().fill(Theme.Palette.border).frame(height: 1)
        }
    }

    private func stat(_ label: String, _ value: String) -> some View {
        HStack(spacing: Theme.Space.sm) {
            Text(label)
                .font(Theme.Font.mono(7.5))
                .foregroundStyle(Theme.Palette.textFaint)
            Text(value)
                .font(Theme.Font.mono(8.5))
                .foregroundStyle(Theme.Palette.textDim)
        }
    }

    private func marker(_ glyph: String, _ label: String, _ tint: Color) -> some View {
        HStack(spacing: Theme.Space.sm) {
            Text(glyph).font(Theme.Font.mono(7)).foregroundStyle(tint)
            Text(label)
                .font(Theme.Font.mono(7.5))
                .tracking(0.5)
                .foregroundStyle(Theme.Palette.textLabel)
        }
    }
}
