import SwiftUI

struct MonitorDock: View {
    @EnvironmentObject private var engine: EngineController

    var body: some View {
        VStack(spacing: 0) {
            header

            ScrollView {
                VStack(spacing: Theme.Space.xl) {
                    levelAndModes
                    outputMode
                    referenceMonitoring
                    meterCard
                    moduleList
                    dspSource
                    listenRoom
                    remoteCore
                }
                .padding(Theme.Space.xxl)
            }
            .scrollIndicators(.never)
        }
        .background(Theme.Palette.panel)
    }

    // MARK: Header

    private var header: some View {
        HStack(spacing: Theme.Space.lg) {
            Circle().fill(Theme.Palette.purple).frame(width: 6, height: 6)
            VStack(alignment: .leading, spacing: 1) {
                HStack(spacing: Theme.Space.sm) {
                    Text("모니터 스테이션")
                        .font(Theme.Font.ui(11, .bold))
                        .foregroundStyle(Theme.Palette.textBright)
                    Text("· 상시 표시")
                        .font(Theme.Font.ui(8.5))
                        .foregroundStyle(Theme.Palette.textFaint)
                }
                Text(routingDescription)
                    .font(Theme.Font.mono(7.5))
                    .foregroundStyle(Theme.Palette.textFaint)
            }
            Spacer()
        }
        .padding(.horizontal, Theme.Space.xxl)
        .frame(height: 46)
        .frame(maxWidth: .infinity)
        .background(Theme.Gradient.monitorHeader)
    }

    private var routingDescription: String {
        guard let set = engine.activeSpeakerSet else { return "Master → Monitor" }
        return "Master → Monitor → \(set.letter): \(set.output)"
    }

    // MARK: Level + modes

    private var levelAndModes: some View {
        HStack(alignment: .top, spacing: Theme.Space.xl) {
            VStack(spacing: Theme.Space.md) {
                RotaryKnob(
                    value: engine.monitorVolumeDb,
                    range: -60...6,
                    resetValue: -6,
                    onChange: { engine.setMonitorVolume($0) }
                )
                Text("MONITOR dB")
                    .font(Theme.Font.mono(6.5))
                    .tracking(0.6)
                    .foregroundStyle(Theme.Palette.textFaint)
            }

            VStack(spacing: Theme.Space.sm) {
                SegmentedRow(
                    items: EngineController.ListenMode.allCases,
                    label: \.label,
                    isActive: { $0 == engine.listenMode },
                    action: { engine.setListenMode($0) }
                )
                HStack(spacing: Theme.Space.sm) {
                    dimButton("Dim", engine.monitorDim, Theme.Palette.orange) { engine.toggleDim() }
                    dimButton("Mono", engine.monitorMono, Theme.Palette.accent) { engine.toggleMonitorMono() }
                    dimButton("Talk", engine.monitorTalkback, Theme.Palette.red) { engine.toggleTalkback() }
                }
            }
        }
    }

    private func dimButton(_ title: String,
                           _ isOn: Bool,
                           _ tint: Color,
                           _ action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(title)
                .font(Theme.Font.ui(10, isOn ? .semibold : .regular))
                .foregroundStyle(isOn ? tint : Theme.Palette.textMuted)
                .frame(maxWidth: .infinity)
                .padding(.vertical, Theme.Space.md)
                .background(
                    RoundedRectangle(cornerRadius: Theme.Radius.button)
                        .fill(isOn ? tint.opacity(0.16) : Theme.Palette.button)
                        .overlay(
                            RoundedRectangle(cornerRadius: Theme.Radius.button)
                                .stroke(isOn ? tint.opacity(0.5) : Theme.Palette.divider, lineWidth: 1)
                        )
                )
        }
        .buttonStyle(.plain)
    }

    // MARK: Output mode

    private var outputMode: some View {
        VStack(spacing: Theme.Space.lg) {
            HStack(spacing: Theme.Space.sm) {
                outputTab("🔊 스피커", active: engine.outputMode == .speaker) {
                    engine.outputMode = .speaker
                }
                outputTab("🎧 헤드폰", active: engine.outputMode == .headphone) {
                    engine.outputMode = .headphone
                }
            }

            if engine.outputMode == .speaker {
                speakerSets
            } else {
                headphonePanel
            }
        }
    }

    private func outputTab(_ title: String, active: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(title)
                .font(Theme.Font.ui(11, .semibold))
                .foregroundStyle(active ? Theme.Palette.purpleLight : Theme.Palette.textMuted)
                .frame(maxWidth: .infinity)
                .padding(.vertical, Theme.Space.lg)
                .background(
                    RoundedRectangle(cornerRadius: Theme.Radius.card)
                        .fill(active ? Color(hex: 0x2a1f3d) : Theme.Palette.button)
                        .overlay(
                            RoundedRectangle(cornerRadius: Theme.Radius.card)
                                .stroke(active ? Color(hex: 0x48376b) : Theme.Palette.divider, lineWidth: 1)
                        )
                )
        }
        .buttonStyle(.plain)
    }

    private var speakerSets: some View {
        VStack(spacing: Theme.Space.lg) {
            HStack(spacing: Theme.Space.sm) {
                ForEach(engine.speakerSets) { set in
                    let active = set.id == engine.activeSpeakerSlot
                    Button { engine.setSpeakerSlot(set.id) } label: {
                        VStack(spacing: 1) {
                            Text(set.letter)
                                .font(Theme.Font.ui(11, .bold))
                                .foregroundStyle(active ? Theme.Palette.purpleLight : Theme.Palette.textMuted)
                            Text(set.name)
                                .font(Theme.Font.ui(7.5))
                                .foregroundStyle(Theme.Palette.textFaint)
                        }
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 7)
                        .background(
                            RoundedRectangle(cornerRadius: Theme.Radius.display)
                                .fill(active ? Color(hex: 0x2a1f3d) : Theme.Palette.button)
                                .overlay(
                                    RoundedRectangle(cornerRadius: Theme.Radius.display)
                                        .stroke(active ? Color(hex: 0x48376b) : Theme.Palette.divider, lineWidth: 1)
                                )
                        )
                    }
                    .buttonStyle(.plain)
                }
            }

            if let set = engine.activeSpeakerSet {
                VStack(alignment: .leading, spacing: Theme.Space.md) {
                    StatRow(label: "스피커 모델", value: set.displayModel, valueColor: Theme.Palette.textSecondary)
                    StatRow(label: "물리 출력", value: set.output, valueColor: Theme.Palette.ioValue)
                    StatRow(label: "시뮬 가중치",
                            value: String(format: "%.0f · %@", set.simWeight * 100, set.roomEq ? "룸 EQ" : "직결"),
                            valueColor: Theme.Palette.purpleLight)
                }
                .padding(9)
                .background(
                    RoundedRectangle(cornerRadius: Theme.Radius.card)
                        .fill(Color(hex: 0x2a3037))
                        .overlay(
                            RoundedRectangle(cornerRadius: Theme.Radius.card)
                                .stroke(Theme.Palette.coolDivider, lineWidth: 1)
                        )
                )
            }
        }
    }

    private var headphonePanel: some View {
        VStack(alignment: .leading, spacing: Theme.Space.md) {
            StatRow(label: "모델", value: "Sonarworks · HD 650")
            Text("헤드폰 모니터 → 크로스피드 + 가상 스피커/헤드폰 보정")
                .font(Theme.Font.ui(8.5))
                .foregroundStyle(Theme.Palette.textFaint)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(9)
        .background(
            RoundedRectangle(cornerRadius: Theme.Radius.card)
                .fill(Theme.Palette.surface)
                .overlay(
                    RoundedRectangle(cornerRadius: Theme.Radius.card)
                        .stroke(Theme.Palette.divider, lineWidth: 1)
                )
        )
    }

    // MARK: Reference monitoring

    private static let streamRefs: [(String, Color)] = [
        ("YouTube", Theme.Palette.red),
        ("Spotify", Theme.Palette.green),
        ("Tidal", Color(hex: 0x6fa6d0)),
        ("Melon", Theme.Palette.instrument),
        ("Bugs", Theme.Palette.orange),
    ]

    private var referenceMonitoring: some View {
        VStack(alignment: .leading, spacing: Theme.Space.md) {
            sectionLabel("레퍼런스 모니터링 · A/B")
            HStack(spacing: Theme.Space.sm) {
                ForEach(Self.streamRefs, id: \.0) { name, dot in
                    HStack(spacing: Theme.Space.xs) {
                        Circle().fill(dot).frame(width: 4, height: 4)
                        Text(name)
                            .font(Theme.Font.ui(9))
                            .foregroundStyle(Theme.Palette.textSecondary)
                            .lineLimit(1)
                    }
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 5)
                    .background(
                        RoundedRectangle(cornerRadius: Theme.Radius.button)
                            .fill(Theme.Palette.button)
                            .overlay(
                                RoundedRectangle(cornerRadius: Theme.Radius.button)
                                    .stroke(Theme.Palette.divider, lineWidth: 1)
                            )
                    )
                }
            }
        }
    }

    // MARK: Meters

    private var meterCard: some View {
        VStack(alignment: .leading, spacing: Theme.Space.md) {
            StatRow(label: "DSP 부하",
                    value: String(format: "%.0f%%", engine.dspLoadFraction * 100),
                    valueColor: engine.dspLoadFraction > 0.8 ? Theme.Palette.red : Theme.Palette.textSecondary)
            MeterBar(fraction: engine.dspLoadFraction, gradient: Theme.Gradient.dspLoad)

            StatRow(label: "지터 (Jitter)", value: String(format: "%.0f µs", engine.wakeJitterUs))
            MeterBar(fraction: jitterFraction,
                     gradient: LinearGradient(colors: [Theme.Palette.green, Color(hex: 0x6fa6d0)],
                                              startPoint: .leading, endPoint: .trailing))

            StatRow(label: "위상 상관", value: String(format: "%+.2f", engine.phaseCorrelation))
            MeterBar(fraction: Double(engine.phaseCorrelation + 1) / 2,
                     gradient: LinearGradient(colors: [Theme.Palette.yellow, Theme.Palette.green],
                                              startPoint: .leading, endPoint: .trailing))

            StatRow(label: "Low / Mid / High", value: bandLabel)
            StatRow(label: "L / R Peak", value: peakLabel)

            VStack(alignment: .leading, spacing: Theme.Space.sm) {
                Text("SPECTRUM")
                    .font(Theme.Font.mono(6.5))
                    .tracking(0.6)
                    .foregroundStyle(Theme.Palette.textFaint)
                SpectrumBars(low: engine.spectrumLow, mid: engine.spectrumMid, high: engine.spectrumHigh)
            }
            .padding(.top, Theme.Space.sm)
        }
        .padding(Theme.Space.xl)
        .background(
            RoundedRectangle(cornerRadius: Theme.Radius.card)
                .fill(Theme.Palette.videoCell)
        )
    }

    /// Jitter is only meaningful against the buffer period: SoundGrid delivers
    /// callbacks in bursts, so an idle system still reads about one period.
    private var jitterFraction: Double {
        guard engine.sampleRate > 0, engine.bufferSize > 0 else { return 0 }
        let bufferPeriodUs = Double(engine.bufferSize) / engine.sampleRate * 1_000_000
        return min(1.0, engine.wakeJitterUs / bufferPeriodUs)
    }

    private var bandLabel: String {
        String(format: "%.0f · %.0f · %.0f",
               meterFraction(engine.spectrumLow) * 100,
               meterFraction(engine.spectrumMid) * 100,
               meterFraction(engine.spectrumHigh) * 100)
    }

    private var peakLabel: String {
        func db(_ peak: Float) -> String {
            peak <= 0.00001 ? "-∞" : String(format: "%.1f", peakToDb(peak))
        }
        return "\(db(engine.outputPeakLeft)) / \(db(engine.outputPeakRight))"
    }

    // MARK: Module list

    private var moduleList: some View {
        VStack(alignment: .leading, spacing: Theme.Space.lg) {
            HStack {
                Text("Monitor DSP 모듈")
                    .font(Theme.Font.ui(10, .semibold))
                    .foregroundStyle(Theme.Palette.text)
                Spacer()
                Button { engine.bypassAllModules() } label: {
                    Text(engine.monitorDspEnabled ? "전체 Bypass" : "Bypass 해제")
                        .font(Theme.Font.ui(8.5, .medium))
                        .foregroundStyle(engine.monitorDspEnabled ? Theme.Palette.purpleLight : Theme.Palette.orange)
                        .padding(.horizontal, Theme.Space.lg)
                        .padding(.vertical, Theme.Space.sm)
                        .background(
                            RoundedRectangle(cornerRadius: Theme.Radius.pill)
                                .fill(Color(hex: 0x2a1f3d))
                        )
                }
                .buttonStyle(.plain)
            }

            VStack(spacing: 1) {
                ForEach(engine.monitorModules) { module in
                    moduleRow(module)
                }
            }
        }
        .padding(Theme.Space.xl)
        .background(
            RoundedRectangle(cornerRadius: Theme.Radius.panel)
                .fill(Theme.Palette.background)
        )
        .opacity(engine.monitorDspEnabled ? 1 : 0.5)
    }

    private func moduleRow(_ module: EngineController.MonitorModule) -> some View {
        HStack(spacing: Theme.Space.lg) {
            ModuleSwitch(isOn: module.enabled) {
                engine.setModuleEnabled(module.id, !module.enabled)
            }
            VStack(alignment: .leading, spacing: 0) {
                Text(module.name)
                    .font(Theme.Font.ui(10, .medium))
                    .foregroundStyle(module.enabled ? Theme.Palette.textNumeric : Theme.Palette.textLabel)
                Text(module.displayDetail)
                    .font(Theme.Font.ui(8))
                    .foregroundStyle(Theme.Palette.textFaint)
                    .lineLimit(1)
            }
            Spacer()
            Text(module.enabled ? "ACTIVE" : "OFF")
                .font(Theme.Font.mono(7, .semibold))
                .foregroundStyle(module.enabled ? Theme.Palette.green : Theme.Palette.textFaint)
        }
        .padding(.horizontal, Theme.Space.lg)
        .padding(.vertical, Theme.Space.md)
        .background(
            RoundedRectangle(cornerRadius: Theme.Radius.display)
                .fill(module.enabled ? Color(hex: 0x1a1e24) : .clear)
        )
    }

    // MARK: DSP source, listen room, remote core

    private var dspSource: some View {
        SegmentedRow(
            items: ["internal", "remote_internal"],
            label: { $0 == "internal" ? "내부 DSP" : "EXT / NDS" },
            isActive: { $0 == engine.monitorPathMode },
            action: { engine.setMonitorPathMode($0) },
            fontSize: 10
        )
    }

    private var listenRoom: some View {
        VStack(alignment: .leading, spacing: Theme.Space.lg) {
            HStack {
                Text("Listen Room")
                    .font(Theme.Font.ui(10, .semibold))
                    .foregroundStyle(Theme.Palette.text)
                Spacer()
                Text("● Offer Ready")
                    .font(Theme.Font.mono(7.5))
                    .foregroundStyle(Theme.Palette.teal)
            }
            HStack(spacing: Theme.Space.sm) {
                stubButton("▶ 송출", tint: Theme.Palette.accent)
                stubButton("⟳ Ping")
                stubButton("⧉ 링크")
            }
            stubButton("💬 대화창 · 클라이언트", tint: Theme.Palette.amber)
            Text("queued 0 · drops 0 · relay fallback 준비")
                .font(Theme.Font.mono(7.5))
                .foregroundStyle(Theme.Palette.textFainter)
        }
        .padding(Theme.Space.xl)
        .background(
            RoundedRectangle(cornerRadius: Theme.Radius.panel)
                .fill(Theme.Palette.background)
        )
    }

    private var remoteCore: some View {
        VStack(alignment: .leading, spacing: Theme.Space.lg) {
            HStack(spacing: Theme.Space.md) {
                Circle()
                    .fill(engine.remoteDspActive ? Theme.Palette.teal : Theme.Palette.textFainter)
                    .frame(width: 6, height: 6)
                Text("Remote Core / DSP")
                    .font(Theme.Font.ui(10, .semibold))
                    .foregroundStyle(Theme.Palette.text)
            }
            HStack {
                VStack(alignment: .leading, spacing: 1) {
                    Text(engine.remoteDspActive ? "원격 코어 연결됨" : "검색된 코어 없음")
                        .font(Theme.Font.ui(9, .medium))
                        .foregroundStyle(Theme.Palette.textSecondary)
                    Text(engine.remoteDspActive
                         ? String(format: "UDP · %.1f ms", engine.remoteDspRoundTripMs)
                         : "LAN 대기")
                        .font(Theme.Font.mono(7.5))
                        .foregroundStyle(Theme.Palette.textFaint)
                }
                Spacer()
                Text(engine.remoteDspActive ? "● Connected" : "○ Idle")
                    .font(Theme.Font.mono(7.5))
                    .foregroundStyle(engine.remoteDspActive ? Theme.Palette.teal : Theme.Palette.textFaint)
            }
        }
        .padding(Theme.Space.xl)
        .background(
            RoundedRectangle(cornerRadius: Theme.Radius.panel)
                .fill(Theme.Palette.background)
        )
    }

    /// Listen Room is UI-only until its engine surface is wired.
    private func stubButton(_ title: String, tint: Color = Theme.Palette.textSecondary) -> some View {
        Text(title)
            .font(Theme.Font.ui(9))
            .foregroundStyle(tint)
            .frame(maxWidth: .infinity)
            .padding(.vertical, Theme.Space.md)
            .background(
                RoundedRectangle(cornerRadius: Theme.Radius.button)
                    .fill(Theme.Palette.button)
                    .overlay(
                        RoundedRectangle(cornerRadius: Theme.Radius.button)
                            .stroke(Theme.Palette.divider, lineWidth: 1)
                    )
            )
    }

    private func sectionLabel(_ text: String) -> some View {
        Text(text)
            .font(Theme.Font.ui(9, .medium))
            .foregroundStyle(Theme.Palette.textLabel)
    }
}
