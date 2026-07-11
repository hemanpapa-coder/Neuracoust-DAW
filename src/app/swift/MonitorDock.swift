import SwiftUI

struct MonitorDock: View {
    @EnvironmentObject private var engine: EngineController
    @EnvironmentObject private var listen: ListenRoomController

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
                // The listen buttons cycle rather than select, matching the old UI:
                // Stereo → Left → Right, Mono → L → R, M/S toggle, Ø phase cycle.
                let listen = engine.monitorListen
                HStack(spacing: Theme.Space.sm) {
                    dimButton(listen.stereoTitle, listen.stereoActive, Theme.Palette.accent) { engine.cycleStereo() }
                    dimButton(listen.monoTitle, listen.monoActive, Theme.Palette.accent) { engine.cycleMono() }
                    dimButton("M/S", listen.midSide, Theme.Palette.accent) { engine.toggleMidSide() }
                    dimButton(listen.phaseTitle, listen.phaseActive, Theme.Palette.purple) { engine.cyclePhase() }
                }
                HStack(spacing: Theme.Space.sm) {
                    dimButton("Mute", engine.monitorMute, Theme.Palette.orange) { engine.toggleMonitorMute() }
                    dimButton("Dim", engine.monitorDim, Theme.Palette.orange) { engine.toggleDim() }
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
                .contextMenu { deviceMenu }
                outputTab("🎧 헤드폰", active: engine.outputMode == .headphone) {
                    engine.outputMode = .headphone
                }
                .contextMenu { deviceMenu }
            }

            if engine.outputMode == .speaker {
                speakerSets
            } else {
                headphonePanel
            }
        }
        .onAppear { engine.refreshOutputDevices() }
    }

    /// Right-click device picker. "시스템 기본" leaves the id empty, which is a valid
    /// choice — the engine opens the OS default. Building the menu rescans the devices.
    @ViewBuilder
    private var deviceMenu: some View {
        Text("출력 장치").font(.caption)
        Button {
            engine.setOutputDevice("")
        } label: {
            let onDefault = engine.currentOutputDeviceId.isEmpty
            let label = "시스템 기본 · \(engine.activeOutputDeviceName)"
            if onDefault { Label(label, systemImage: "checkmark") } else { Text(label) }
        }
        Divider()
        ForEach(engine.outputDevices) { device in
            Button {
                engine.setOutputDevice(device.id)
            } label: {
                if engine.currentOutputDeviceId == device.id {
                    Label(device.name, systemImage: "checkmark")
                } else {
                    Text(device.name)
                }
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
        // Multi-select, not one-of-three: any combination is allowed and lands on the
        // engine's "auto" mode, which distributes across whatever is available.
        HStack(spacing: Theme.Space.sm) {
            dspSourceButton("내부 DSP", .internalDsp)
            dspSourceButton("외부 DSP", .external)
            dspSourceButton("NDS", .nds)
        }
    }

    private func dspSourceButton(_ title: String, _ source: EngineController.DspSource) -> some View {
        let on = engine.usesDspSource(source)
        return Button { engine.toggleDspSource(source) } label: {
            Text(title)
                .font(Theme.Font.ui(9, on ? .semibold : .regular))
                .foregroundStyle(on ? Theme.Palette.purpleLight : Theme.Palette.textMuted)
                .frame(maxWidth: .infinity)
                .padding(.vertical, Theme.Space.md)
                .background(
                    RoundedRectangle(cornerRadius: Theme.Radius.button)
                        .fill(on ? Color(hex: 0x2a1f3d) : Theme.Palette.button)
                        .overlay(
                            RoundedRectangle(cornerRadius: Theme.Radius.button)
                                .stroke(on ? Color(hex: 0x48376b) : Theme.Palette.divider, lineWidth: 1)
                        )
                )
        }
        .buttonStyle(.plain)
    }

    private var listenRoom: some View {
        VStack(alignment: .leading, spacing: Theme.Space.lg) {
            HStack {
                Text("Listen Room")
                    .font(Theme.Font.ui(10, .semibold))
                    .foregroundStyle(Theme.Palette.text)
                Spacer()
                Text(listenStatusLabel)
                    .font(Theme.Font.mono(7.5))
                    .foregroundStyle(listenStatusTint)
            }

            HStack(spacing: Theme.Space.sm) {
                actionButton(listen.enabled ? "■ 중지" : "▶ 송출",
                             tint: listen.enabled ? Theme.Palette.red : Theme.Palette.accent) {
                    listen.toggle()
                }
                actionButton("⧉ 링크", enabled: listen.enabled) { listen.copyShareLink() }
                actionButton("⊞ QR", enabled: listen.enabled) { listen.qrOpen.toggle() }
                    .popover(isPresented: $listen.qrOpen, arrowEdge: .bottom) {
                        ListenInvitePanel(shareURL: listen.externalShareUrl) { listen.copyShareLink() }
                    }
            }

            // Quality, latency, a fresh link, and a studio ping — the rest of the old
            // UI's Listen Room controls. Quality and latency cycle on click.
            HStack(spacing: Theme.Space.sm) {
                actionButton("음질 · \(listen.qualityTitle)",
                             tint: listen.enabled ? Theme.Palette.accent : Theme.Palette.textSecondary) {
                    listen.cycleQuality()
                }
                actionButton("지연 · \(listen.latencyTitle)",
                             tint: listen.enabled ? Theme.Palette.accent : Theme.Palette.textSecondary) {
                    listen.cycleLatency()
                }
            }
            HStack(spacing: Theme.Space.sm) {
                actionButton("↻ 새 링크") { listen.resetToken() }
                actionButton("◎ Ping", enabled: listen.enabled) { listen.ping() }
            }

            Button {
                listen.chatOpen.toggle()
                if listen.chatOpen { listen.markChatRead() }
            } label: {
                HStack(spacing: Theme.Space.sm) {
                    Text("💬 대화창 · 클라이언트")
                        .font(Theme.Font.ui(9))
                        .foregroundStyle(Theme.Palette.amber)
                    if listen.chatUnread > 0 {
                        Text("\(listen.chatUnread)")
                            .font(Theme.Font.mono(7, .semibold))
                            .foregroundStyle(Theme.Palette.deepBorder)
                            .padding(.horizontal, 5)
                            .padding(.vertical, 1)
                            .background(Capsule().fill(Theme.Palette.amber))
                    }
                }
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
            .buttonStyle(.plain)

            Text(listenStatsLabel)
                .font(Theme.Font.mono(7.5))
                .foregroundStyle(Theme.Palette.textFainter)

            if let error = listen.lastError {
                Text("⚠︎ \(error)")
                    .font(Theme.Font.mono(7.5))
                    .foregroundStyle(Theme.Palette.amber)
                    .fixedSize(horizontal: false, vertical: true)
            }

            if listen.chatOpen {
                ChatPanel()
            }
        }
        .padding(Theme.Space.xl)
        .background(
            RoundedRectangle(cornerRadius: Theme.Radius.panel)
                .fill(Theme.Palette.background)
        )
    }

    private var listenStatusLabel: String {
        if !listen.enabled { return "○ Idle" }
        if listen.offerReady { return "● Offer Ready" }
        if listen.relayReachable { return "● Relay 연결됨" }
        return "◐ Relay 대기"
    }

    private var listenStatusTint: Color {
        guard listen.enabled else { return Theme.Palette.textFaint }
        return listen.relayReachable || listen.offerReady ? Theme.Palette.teal : Theme.Palette.amber
    }

    private var listenStatsLabel: String {
        let mode = listen.transportMode.isEmpty ? "direct_fallback" : listen.transportMode
        return "queued \(listen.packetsQueued) · drops \(listen.packetsDropped) · \(mode)"
    }

    private func actionButton(_ title: String,
                              tint: Color = Theme.Palette.textSecondary,
                              enabled: Bool = true,
                              action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(title)
                .font(Theme.Font.ui(9))
                .foregroundStyle(enabled ? tint : Theme.Palette.textFainter)
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
        .buttonStyle(.plain)
        .disabled(!enabled)
    }

    /// − / count / + for the DSP core count, floored by isolation.
    /// A − / count 코어 / + stepper. Both the internal reserve and the external DSP
    /// Manager reserve share it; only the bounds and actions differ.
    private func coreStepper(value: Int, minValue: Int,
                             dec: @escaping () -> Void,
                             inc: @escaping () -> Void) -> some View {
        HStack(spacing: Theme.Space.sm) {
            Button(action: dec) {
                Text("−").font(Theme.Font.ui(12, .bold)).frame(width: 20, height: 18)
            }
            .buttonStyle(.plain)
            .foregroundStyle(value > minValue ? Theme.Palette.text : Theme.Palette.textFainter)
            .disabled(value <= minValue)

            Text("\(value)")
                .font(Theme.Font.mono(11, .semibold))
                .foregroundStyle(Theme.Palette.text)
                .frame(minWidth: 20)
            Text("코어").font(Theme.Font.mono(7)).foregroundStyle(Theme.Palette.textFaint)

            Button(action: inc) {
                Text("+").font(Theme.Font.ui(12, .bold)).frame(width: 20, height: 18)
            }
            .buttonStyle(.plain)
            .foregroundStyle(value < 16 ? Theme.Palette.text : Theme.Palette.textFainter)
            .disabled(value >= 16)
        }
    }

    private var internalCoreStepper: some View {
        coreStepper(value: engine.dspCoreCount, minValue: engine.minDspCoreCount,
                    dec: { engine.setDspCoreCount(engine.dspCoreCount - 1) },
                    inc: { engine.setDspCoreCount(engine.dspCoreCount + 1) })
    }

    private var externalCoreStepper: some View {
        coreStepper(value: engine.externalDspCoreCount, minValue: 1,
                    dec: { engine.setExternalDspCoreCount(engine.externalDspCoreCount - 1) },
                    inc: { engine.setExternalDspCoreCount(engine.externalDspCoreCount + 1) })
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

            Divider().overlay(Theme.Palette.divider)

            // Internal DSP core allocation. Isolation keeps a floor of 4.
            HStack {
                Toggle("", isOn: Binding(
                    get: { engine.coreIsolationEnabled },
                    set: { engine.setCoreIsolation($0) }))
                    .labelsHidden()
                    .toggleStyle(.switch)
                    .scaleEffect(0.7)
                    .frame(width: 34)
                VStack(alignment: .leading, spacing: 0) {
                    Text("내부 코어 격리")
                        .font(Theme.Font.ui(9, .medium))
                        .foregroundStyle(Theme.Palette.textSecondary)
                    Text(engine.coreIsolationEnabled ? "DSP 우선 배정" : "Native 실행")
                        .font(Theme.Font.mono(7))
                        .foregroundStyle(Theme.Palette.textFaint)
                }
                Spacer()
                internalCoreStepper
            }

            // The external DSP Manager's core reserve. Settable here or in the manager
            // itself; a connected node's own report wins over this hint.
            HStack {
                VStack(alignment: .leading, spacing: 0) {
                    Text("외부 DSP 코어")
                        .font(Theme.Font.ui(9, .medium))
                        .foregroundStyle(Theme.Palette.textSecondary)
                    Text("DSP 매니저 예약")
                        .font(Theme.Font.mono(7))
                        .foregroundStyle(Theme.Palette.textFaint)
                }
                Spacer()
                externalCoreStepper
            }
        }
        .padding(Theme.Space.xl)
        .background(
            RoundedRectangle(cornerRadius: Theme.Radius.panel)
                .fill(Theme.Palette.background)
        )
    }

    private func sectionLabel(_ text: String) -> some View {
        Text(text)
            .font(Theme.Font.ui(9, .medium))
            .foregroundStyle(Theme.Palette.textLabel)
    }
}
