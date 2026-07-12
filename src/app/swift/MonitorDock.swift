import SwiftUI
import AppKit

struct MonitorDock: View {
    @EnvironmentObject private var engine: EngineController
    @EnvironmentObject private var listen: ListenRoomController

    /// Opens the searchable model picker sheet. Nested context submenus of ~200 items do
    /// not render on macOS, so model selection uses this instead.
    @State var modelPicker: ModelPickerContext?

    var body: some View {
        VStack(spacing: 0) {
            header

            ScrollView {
                VStack(spacing: Theme.Space.xl) {
                    inputSection
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
        .sheet(item: $modelPicker) { ctx in
            ModelPickerSheet(context: ctx) { modelPicker = nil }
        }
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
                    TalkbackButton()
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
                let exclusive = engine.monitorOutputExclusive
                outputTab("🔊 스피커", active: engine.outputMode == .speaker) {
                    engine.outputMode = .speaker
                }
                .opacity(exclusive && engine.outputMode != .speaker ? 0.45 : 1)
                .contextMenu { speakerMenu }
                outputTab("🎧 헤드폰", active: engine.outputMode == .headphone) {
                    engine.outputMode = .headphone
                }
                .opacity(exclusive && engine.outputMode != .headphone ? 0.45 : 1)
                .contextMenu { headphoneMenu }
            }

            // Whether speaker and headphone are mutually exclusive, and the physical
            // model of whichever output is active.
            HStack(spacing: Theme.Space.sm) {
                Toggle("", isOn: Binding(get: { engine.monitorOutputExclusive },
                                         set: { engine.setMonitorOutputExclusive($0) }))
                    .labelsHidden().toggleStyle(.switch).scaleEffect(0.7).frame(width: 34)
                Text("스피커/헤드폰 배타")
                    .font(Theme.Font.ui(9, .medium))
                    .foregroundStyle(Theme.Palette.textSecondary)
                Spacer()
                Text(activePhysicalModelLabel)
                    .font(Theme.Font.mono(7.5))
                    .foregroundStyle(Theme.Palette.textFaint)
                    .lineLimit(1)
            }

            if engine.outputMode == .speaker {
                speakerSets
            } else {
                headphonePanel
            }
        }
        .onAppear { engine.refreshOutputDevices() }
    }

    private var activePhysicalModelLabel: String {
        let model = engine.outputMode == .speaker ? engine.physicalSpeakerModel
                                                   : engine.physicalHeadphoneModel
        return model.isEmpty ? "실물 모델 미지정" : model
    }

    /// A single menu item that opens the searchable model picker sheet. Nested context
    /// submenus of ~200 items do not render on macOS, so this replaces them.
    @ViewBuilder
    private func modelMenu(_ title: String, catalog: [String], selected: String,
                           onPick: @escaping (String) -> Void) -> some View {
        Button("\(title)…") {
            modelPicker = ModelPickerContext(title: title, catalog: catalog,
                                             selected: selected, onPick: onPick)
        }
    }

    /// The 스피커 tab's right-click menu: physical output device plus the real speaker
    /// model the user monitors on (not the A/B/C simulator).
    @ViewBuilder
    private var speakerMenu: some View {
        deviceMenu
        Divider()
        modelMenu("실물 스피커 모델", catalog: engine.speakerModelCatalog,
                  selected: engine.physicalSpeakerModel) { engine.setPhysicalSpeakerModel($0) }
    }

    @ViewBuilder
    private var headphoneMenu: some View {
        deviceMenu
        Divider()
        modelMenu("실물 헤드폰 모델", catalog: engine.headphoneModelCatalog,
                  selected: engine.physicalHeadphoneModel) { engine.setPhysicalHeadphoneModel($0) }
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
                    // Right-click a set to pick its real speaker MODEL (spec-based
                    // virtual monitoring) or route it straight to a physical output.
                    .contextMenu { speakerSetMenu(set) }
                }
            }

            if let set = engine.activeSpeakerSet {
                VStack(alignment: .leading, spacing: Theme.Space.md) {
                    StatRow(label: "스피커 모델", value: set.displayModel, valueColor: Theme.Palette.textSecondary)
                    if !engine.speakerSimulationActive {
                        StatRow(label: "물리 출력", value: set.output, valueColor: Theme.Palette.ioValue)
                    }
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

    /// The right-click menu for an A/B/C speaker set: choose its modelled speaker, or a
    /// physical output pair (which bypasses the model), plus the room-EQ toggle.
    @ViewBuilder
    private func speakerSetMenu(_ set: EngineController.SpeakerSet) -> some View {
        Text("\(set.letter) · \(set.name)")
        modelMenu("스피커 모델", catalog: engine.speakerModelCatalog,
                  selected: set.output == "None" ? set.displayModel : "") { engine.setSpeakerModel(set.id, $0) }
        // Physical output is a raw passthrough that bypasses the modelled path, so it is
        // only offered when Speaker Simulation is off (photo-2 context).
        if !engine.speakerSimulationActive {
            Menu("물리 출력") {
                ForEach(engine.speakerOutputRoutes, id: \.self) { route in
                    Button {
                        engine.setSpeakerOutput(set.id, route)
                    } label: {
                        if set.output == route {
                            Label(route, systemImage: "checkmark")
                        } else {
                            Text(route)
                        }
                    }
                }
            }
        }
        Divider()
        Button {
            engine.setSpeakerRoomEq(set.id, !set.roomEq)
        } label: {
            if set.roomEq { Label("룸 EQ", systemImage: "checkmark") } else { Text("룸 EQ") }
        }
        .disabled(set.output != "None")   // physical passthrough has no room EQ
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

    /// The monitor station's input stage — the first thing in the dock, like the Monitor
    /// DSP app. Two mutually-exclusive sources: the DAW Master, or BlackHole (the
    /// computer's audio). This replaces the old reference-input dropdown.
    private var inputSection: some View {
        VStack(alignment: .leading, spacing: Theme.Space.sm) {
            sectionLabel("입력")
            HStack(spacing: Theme.Space.sm) {
                dimButton("Master", !engine.monitorListenSource, Theme.Palette.accent) {
                    engine.selectMonitorInput(blackHole: false)
                }
                dimButton("BlackHole", engine.monitorListenSource, Theme.Palette.teal) {
                    engine.selectMonitorInput(blackHole: true)
                }
            }
            if engine.monitorListenSource && !engine.hasBlackHoleInput {
                Text("BlackHole 입력을 찾지 못했습니다. 설치/장치 확인 필요.")
                    .font(Theme.Font.mono(7))
                    .foregroundStyle(Theme.Palette.orange)
            }
        }
        .onAppear { engine.refreshInputDevices() }
    }

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
            PhaseCorrelationDotMeter(correlation: engine.phaseCorrelation)

            delayCompensationRow

            StatRow(label: "Low / Mid / High", value: bandLabel)
            StatRow(label: "L / R Peak", value: peakLabel)

            VStack(alignment: .leading, spacing: Theme.Space.sm) {
                Text("SPECTRUM")
                    .font(Theme.Font.mono(6.5))
                    .tracking(0.6)
                    .foregroundStyle(Theme.Palette.textFaint)
                SpectrumAnalyzerView(bins: engine.spectrumBins,
                                     sampleRate: engine.sampleRate,
                                     compact: true)
                    .frame(height: 40)
                    .background(RoundedRectangle(cornerRadius: 4).fill(Color.black.opacity(0.35)))
                    .contentShape(Rectangle())
                    .onTapGesture { engine.openAnalyzerWindow() }
                    .contextMenu {
                        Menu("큰 창으로 열기") {
                            ForEach(AnalyzerKind.allCases) { k in
                                Button(k.label + (k.isAvailable ? "" : " (준비 중)")) {
                                    AnalyzerWindowManager.shared.open(kind: k, engine: engine)
                                }
                                .disabled(!k.isAvailable)
                            }
                        }
                        Divider()
                        Picker("클릭 시 열 종류", selection: $engine.dockAnalyzerKind) {
                            ForEach(AnalyzerKind.allCases) { k in
                                Text(k.label).tag(k)
                            }
                        }
                    }
                    .help("클릭: 큰 분석 창 열기 · 우클릭: 종류 선택")
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

    /// Plugin delay compensation: enable switch plus the engine's reported alignment.
    private var delayCompensationRow: some View {
        HStack(spacing: 6) {
            Text("지연 보정 (PDC)")
                .font(Theme.Font.mono(9)).foregroundStyle(Theme.Palette.textSecondary)
            Spacer(minLength: 0)
            Text(engine.delayCompensationEnabled
                 ? (engine.delayCompensationSamples > 0
                    ? String(format: "%.1f ms · %d smp", engine.delayCompensationMs, engine.delayCompensationSamples)
                    : "0.0 ms")
                 : "끔")
                .font(Theme.Font.mono(9))
                .foregroundStyle(engine.delayCompensationEnabled ? Theme.Palette.green : Theme.Palette.textFaint)
            Toggle("", isOn: Binding(
                get: { engine.delayCompensationEnabled },
                set: { engine.setDelayCompensation($0) }))
                .labelsHidden().toggleStyle(.switch).controlSize(.mini)
        }
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

            // The node the engine streams External/NDS monitor audio to. Type a host or
            // IP, or 검색 to broadcast-probe the LAN.
            RemoteHostField()

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

/// The remote DSP node address field: edits a draft locally and commits on Enter or
/// blur so the engine is not retargeted (and history recorded) on every keystroke.
private struct RemoteHostField: View {
    @EnvironmentObject private var engine: EngineController
    @State private var draft = ""
    @FocusState private var focused: Bool

    var body: some View {
        HStack(spacing: Theme.Space.sm) {
            Text("노드")
                .font(Theme.Font.mono(7.5))
                .foregroundStyle(Theme.Palette.textFaint)
            TextField("studio.local", text: $draft)
                .textFieldStyle(.plain)
                .font(Theme.Font.mono(9))
                .foregroundStyle(Theme.Palette.text)
                .focused($focused)
                .onSubmit { commit() }
                .onChange(of: focused) { if !$1 { commit() } }
                .padding(.horizontal, 6)
                .padding(.vertical, 3)
                .background(
                    RoundedRectangle(cornerRadius: Theme.Radius.button)
                        .fill(Theme.Palette.recess)
                        .overlay(RoundedRectangle(cornerRadius: Theme.Radius.button)
                            .stroke(Theme.Palette.divider, lineWidth: 1))
                )
            Button { engine.discoverRemoteDspHost() } label: {
                Text("검색")
                    .font(Theme.Font.ui(8.5, .medium))
                    .foregroundStyle(Theme.Palette.textSecondary)
                    .padding(.horizontal, 8)
                    .frame(height: 22)
                    .background(RoundedRectangle(cornerRadius: Theme.Radius.button).fill(Theme.Palette.button))
            }
            .buttonStyle(.plain)
        }
        .onAppear { draft = engine.remoteDspHost }
        .onChange(of: engine.remoteDspHost) { draft = $1 }   // reflect discover / project load
    }

    private func commit() {
        let trimmed = draft.trimmingCharacters(in: .whitespaces)
        if !trimmed.isEmpty, trimmed != engine.remoteDspHost {
            engine.setRemoteDspHost(trimmed)
        } else {
            draft = engine.remoteDspHost
        }
    }
}

/// The monitor Talkback key: momentary while held (and it pulls Dim in with it),
/// double-click to latch it on, click a latched button to release. A console talkback
/// key, not a plain toggle. SwiftUI press gestures do not fire reliably for a static
/// press inside the dock's ScrollView, so the whole control is a self-drawing NSView
/// that reads mouseDown/mouseUp (and clickCount) directly.
private struct TalkbackButton: View {
    @EnvironmentObject private var engine: EngineController

    var body: some View {
        TalkbackKeyRepresentable(isOn: engine.monitorTalkback) { engine.setTalkbackEngaged($0) }
            .frame(maxWidth: .infinity)
            .frame(height: 26)
    }
}

private struct TalkbackKeyRepresentable: NSViewRepresentable {
    let isOn: Bool
    let onEngaged: (Bool) -> Void
    func makeNSView(context: Context) -> TalkbackKeyView {
        let view = TalkbackKeyView()
        view.onEngaged = onEngaged
        view.isOn = isOn
        return view
    }
    func updateNSView(_ view: TalkbackKeyView, context: Context) {
        view.onEngaged = onEngaged
        view.isOn = isOn
    }
}

/// Draws the "Talk" key and runs the press/hold/latch logic. Momentary while held;
/// double-click latches on; a click on a latched key releases it. `suppress` swallows
/// the rest of a click sequence after an unlatch so a double-click used to release
/// cannot immediately re-engage.
final class TalkbackKeyView: NSView {
    var onEngaged: ((Bool) -> Void)?
    var isOn = false { didSet { if isOn != oldValue { needsDisplay = true } } }
    private var latched = false
    private var suppress = false

    override var isFlipped: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    private static func rgb(_ hex: UInt32, _ alpha: CGFloat = 1) -> NSColor {
        NSColor(srgbRed: CGFloat((hex >> 16) & 0xff) / 255,
                green: CGFloat((hex >> 8) & 0xff) / 255,
                blue: CGFloat(hex & 0xff) / 255, alpha: alpha)
    }

    override func draw(_ dirtyRect: NSRect) {
        let red = Self.rgb(0xff5252)
        let path = NSBezierPath(roundedRect: bounds.insetBy(dx: 0.5, dy: 0.5), xRadius: 5, yRadius: 5)
        (isOn ? red.withAlphaComponent(0.16) : Self.rgb(0x3d352e)).setFill()
        path.fill()
        (isOn ? red.withAlphaComponent(0.55) : Self.rgb(0x4f4339)).setStroke()
        path.lineWidth = 1
        path.stroke()

        let style = NSMutableParagraphStyle()
        style.alignment = .center
        let font = NSFont(name: "Space Grotesk", size: 10)
            ?? NSFont.systemFont(ofSize: 10, weight: isOn ? .semibold : .regular)
        let attrs: [NSAttributedString.Key: Any] = [
            .font: font,
            .foregroundColor: isOn ? red : Self.rgb(0x918676),
            .paragraphStyle: style,
        ]
        let text = "Talk" as NSString
        let h = text.size(withAttributes: attrs).height
        text.draw(in: NSRect(x: 0, y: (bounds.height - h) / 2, width: bounds.width, height: h),
                  withAttributes: attrs)
    }

    override func mouseDown(with event: NSEvent) {
        if event.clickCount == 1 { suppress = false }   // a fresh gesture begins
        if latched {
            latched = false
            onEngaged?(false)
            suppress = true                              // ignore the rest of this sequence
            return
        }
        if suppress { return }
        onEngaged?(true)                                // momentary engage
    }

    override func mouseUp(with event: NSEvent) {
        if suppress { return }
        if event.clickCount >= 2 {
            latched = true
            onEngaged?(true)                            // stays on
        } else {
            onEngaged?(false)                           // momentary release
        }
    }
}

/// A model-selection request for the searchable picker sheet.
struct ModelPickerContext: Identifiable {
    let id = UUID()
    let title: String
    let catalog: [String]
    let selected: String
    let onPick: (String) -> Void
}

/// A searchable, scrollable model picker — reliable where a 200-item nested context
/// submenu is not, and the shape the popup redesign wants.
struct ModelPickerSheet: View {
    let context: ModelPickerContext
    let dismiss: () -> Void
    @State private var query = ""

    private var filtered: [String] {
        let q = query.trimmingCharacters(in: .whitespaces).lowercased()
        guard !q.isEmpty else { return context.catalog }
        return context.catalog.filter { $0.lowercased().contains(q) }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: Theme.Space.md) {
            HStack {
                Text(context.title)
                    .font(Theme.Font.ui(12, .semibold))
                    .foregroundStyle(Theme.Palette.textBright)
                Spacer()
                Button("닫기") { dismiss() }
            }
            TextField("검색…", text: $query)
                .textFieldStyle(.roundedBorder)

            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 1) {
                        ForEach(filtered, id: \.self) { model in
                            Button {
                                context.onPick(model)
                                dismiss()
                            } label: {
                                HStack {
                                    Text(model)
                                        .font(Theme.Font.ui(11))
                                        .foregroundStyle(Theme.Palette.text)
                                    Spacer()
                                    if model == context.selected {
                                        Image(systemName: "checkmark")
                                            .foregroundStyle(Theme.Palette.accent)
                                    }
                                }
                                .padding(.horizontal, Theme.Space.md)
                                .padding(.vertical, 5)
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .background(model == context.selected
                                            ? Theme.Palette.accent.opacity(0.12) : Color.clear)
                                .contentShape(Rectangle())
                            }
                            .buttonStyle(.plain)
                            .id(model)
                        }
                    }
                }
                .onAppear { if !context.selected.isEmpty { proxy.scrollTo(context.selected, anchor: .center) } }
            }
        }
        .padding(Theme.Space.xl)
        .frame(width: 340, height: 460)
        .background(Theme.Palette.panel)
    }
}
