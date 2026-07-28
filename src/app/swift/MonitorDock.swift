import SwiftUI
import AppKit

struct MonitorDock: View {
    @EnvironmentObject private var engine: EngineController
    @EnvironmentObject private var listen: ListenRoomController
    @Environment(\.openWindow) private var openWindow

    /// Opens the searchable model picker sheet. Nested context submenus of ~200 items do
    /// not render on macOS, so model selection uses this instead.
    @State var modelPicker: ModelPickerContext?
    @State private var showSpeakerComparison = false

    /// Dashboard panels the user can show/hide. The core monitor controls (input / level / output)
    /// are always visible; the DSP and analysis panels are optional, toggled from the header chips
    /// — persisted so the layout sticks. Hidden set stored as a comma-joined list of raw values.
    enum DockPanel: String, CaseIterable, Identifiable {
        case reference, meter, modules, dspSource, listenRoom, remoteCore
        var id: String { rawValue }
        var title: String {
            switch self {
            case .reference: return "레퍼런스"
            case .meter: return "미터·스펙트럼"
            case .modules: return "DSP 모듈"
            case .dspSource: return "DSP 소스"
            case .listenRoom: return "리슨룸"
            case .remoteCore: return "원격 코어"
            }
        }
    }
    @AppStorage("nc.monitorHiddenPanels") private var hiddenPanelsRaw = ""
    private var hiddenPanels: Set<String> {
        Set(hiddenPanelsRaw.split(separator: ",").map(String.init))
    }
    private func togglePanel(_ panel: DockPanel) {
        var hidden = hiddenPanels
        if hidden.contains(panel.rawValue) { hidden.remove(panel.rawValue) } else { hidden.insert(panel.rawValue) }
        hiddenPanelsRaw = hidden.sorted().joined(separator: ",")
    }
    private func shows(_ panel: DockPanel) -> Bool { !hiddenPanels.contains(panel.rawValue) }

    /// NC_DIAG_STRIP bisection gate, shared with RootView. Empty in normal use.
    private static let diagStrip: Set<String> = {
        guard let raw = ProcessInfo.processInfo.environment["NC_DIAG_STRIP"] else { return [] }
        return Set(raw.split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) })
    }()
    private static func diag(_ name: String) -> Bool { diagStrip.contains(name) }

    var body: some View {
        VStack(spacing: 0) {
            // Edit / Mix tabs live in the transport toolbar row (alongside the panel-toggle and
            // help chips), not here — the dock's column is reserved for the monitor station.
            header
            panelChips

            ScrollView {
                VStack(spacing: Theme.Space.xl) {
                    // The Self.diag gates are the same NC_DIAG_STRIP bisection mechanism as
                    // RootView's — see there. Empty in normal use.
                    if !Self.diag("d-input") { inputSection }
                    if !Self.diag("d-level") { levelAndModes }
                    if !Self.diag("d-output") { outputMode }
                    if shows(.reference) { referenceMonitoring }
                    if shows(.meter) && !Self.diag("d-meter") { meterCard }
                    if shows(.modules) { moduleList }
                    if shows(.dspSource) && !Self.diag("d-dsp") { dspSource }
                    if shows(.listenRoom) { listenRoom }
                    if shows(.remoteCore) && !Self.diag("d-remote") { remoteCore }
                }
                .padding(Theme.Space.xxl)
            }
            .scrollIndicators(.never)
        }
        .background(Theme.Palette.panel)
        .sheet(item: $modelPicker) { ctx in
            ModelPickerSheet(context: ctx) { modelPicker = nil }
        }
        .sheet(isPresented: $showSpeakerComparison) {
            SpeakerComparisonView(engine: engine)
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
            // Pop the station out into its own window — the same jump the 윈도우 menu offers,
            // but visible where the station actually lives. The dock column yields while the
            // window is open and returns when it closes (MonitorStationWindowRoot).
            Button {
                openWindow(id: "monitor-station")
            } label: {
                Image(systemName: "rectangle.on.rectangle")
                    .font(.system(size: 11, weight: .semibold))
                    .foregroundStyle(Theme.Palette.textMuted)
                    .frame(width: 24, height: 26)
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .help("모니터 스테이션을 별도 창으로 엽니다 (윈도우 메뉴 → 모니터 스테이션 창)")
            Button {
                launchVocalConverter()
            } label: {
                Image(systemName: "waveform.and.mic")
                    .font(.system(size: 13, weight: .semibold))
                    .foregroundStyle(Theme.Palette.purpleLight)
                    .frame(width: 30, height: 26)
                    .background(
                        RoundedRectangle(cornerRadius: 7)
                            .fill(Theme.Palette.purple.opacity(0.2))
                            .overlay(RoundedRectangle(cornerRadius: 7)
                                .stroke(Theme.Palette.purple.opacity(0.45), lineWidth: 1))
                    )
            }
            .buttonStyle(.plain)
            .help("AI 보컬 변환기 열기 · Suno 보컬을 새 가수 음색으로 오프라인 렌더링합니다.")
        }
        .padding(.horizontal, Theme.Space.xxl)
        .frame(height: 46)
        .frame(maxWidth: .infinity)
        .background(Theme.Gradient.monitorHeader)
    }

    private func launchVocalConverter() {
        let workspace = NSWorkspace.shared
        let sibling = Bundle.main.bundleURL.deletingLastPathComponent()
            .appendingPathComponent("Neuracoust Vocal Converter.app", isDirectory: true)
        let candidates = [
            sibling,
            URL(fileURLWithPath: "/Applications/Neuracoust Vocal Converter.app"),
            FileManager.default.homeDirectoryForCurrentUser
                .appendingPathComponent("Applications/Neuracoust Vocal Converter.app", isDirectory: true)
        ]
        guard let app = candidates.first(where: {
            FileManager.default.fileExists(atPath: $0.path)
        }) else {
            let alert = NSAlert()
            alert.messageText = "AI 보컬 변환기를 찾지 못했습니다."
            alert.informativeText = "Neuracoust Vocal Converter 앱을 먼저 빌드하거나 Applications 폴더에 설치해주세요."
            alert.alertStyle = .warning
            alert.runModal()
            return
        }
        let configuration = NSWorkspace.OpenConfiguration()
        configuration.activates = true
        workspace.openApplication(at: app, configuration: configuration) { _, error in
            guard let error else { return }
            DispatchQueue.main.async {
                let alert = NSAlert()
                alert.messageText = "AI 보컬 변환기를 열 수 없습니다."
                alert.informativeText = error.localizedDescription
                alert.alertStyle = .warning
                alert.runModal()
            }
        }
    }

    /// The dashboard: chips that show/hide each optional panel (the core monitor controls stay).
    private var panelChips: some View {
        LazyVGrid(columns: [GridItem(.adaptive(minimum: 70), spacing: 4)], spacing: 4) {
            ForEach(DockPanel.allCases) { panel in
                let on = shows(panel)
                Button { togglePanel(panel) } label: {
                    Text(panel.title)
                        .font(Theme.Font.mono(7.5))
                        .foregroundStyle(on ? Theme.Palette.textBright : Theme.Palette.textFaint)
                        .lineLimit(1)
                        .padding(.horizontal, 6)
                        .padding(.vertical, 3)
                        .frame(maxWidth: .infinity)
                        .background(RoundedRectangle(cornerRadius: Theme.Radius.pill)
                            .fill(on ? Theme.Palette.purple.opacity(0.28) : Theme.Palette.button))
                }
                .buttonStyle(.plain)
                .help("\(panel.title) 패널 \(on ? "숨기기" : "표시")")
            }
        }
        .padding(.horizontal, Theme.Space.xxl)
        .padding(.vertical, 5)
        .background(Theme.Palette.panel)
    }

    private var routingDescription: String {
        guard let set = engine.activeSpeakerSet else { return "Master → Monitor" }
        // "None" is the modelled/virtual path (out the main L/R), not a dead output —
        // show the speaker model so the header does not read like silence.
        let dest = set.output == "None" ? "\(set.displayModel) · Main 1-2" : set.output
        return "Master → Monitor → \(set.letter): \(dest)"
    }

    // MARK: Level + modes

    private var levelAndModes: some View {
        HStack(alignment: .top, spacing: Theme.Space.xl) {
            VStack(spacing: Theme.Space.md) {
                RotaryKnob(
                    value: engine.monitorVolumeDb,
                    range: -60...(-12),          // capped at -12 dB so speaker-sim EQ boosts keep D/A headroom
                    resetValue: -30,
                    onChange: { engine.setMonitorVolume($0) }
                )
                .contextMenu { shortcutMenu(.volDown); shortcutMenu(.volUp) }
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
                        .contextMenu { shortcutMenu(.stereo) }
                    dimButton(listen.monoTitle, listen.monoActive, Theme.Palette.accent) { engine.cycleMono() }
                        .contextMenu { shortcutMenu(.mono) }
                    dimButton("M/S", listen.midSide, Theme.Palette.accent) { engine.toggleMidSide() }
                        .contextMenu { shortcutMenu(.midSide) }
                    dimButton(listen.phaseTitle, listen.phaseActive, Theme.Palette.purple) { engine.cyclePhase() }
                }
                HStack(spacing: Theme.Space.sm) {
                    dimButton("Dim", engine.monitorDim, Theme.Palette.orange) { engine.toggleDim() }
                        .contextMenu { dimAmountMenu; Divider(); shortcutMenu(.dim) }
                    dimButton("Mute", engine.monitorMute, Theme.Palette.orange) { engine.toggleMonitorMute() }
                        .contextMenu { shortcutMenu(.mute) }
                    TalkbackButton()   // right-click (mode + talkback mic) handled in the NSView
                }
                monitorShortcutToggle
            }
        }
    }

    /// Master switch for the runtime-only number-row monitor shortcuts, with a reset. Right-click
    /// any monitor button to change which key drives it.
    private var monitorShortcutToggle: some View {
        HStack(spacing: Theme.Space.sm) {
            Toggle(isOn: Binding(get: { engine.monitorShortcutsEnabled },
                                 set: { engine.setMonitorShortcutsEnabled($0) })) {
                Text("키패드 단축키").font(Theme.Font.ui(8.5, .medium))
            }
            .toggleStyle(.switch).scaleEffect(0.7).fixedSize()
            .help("켜면 숫자 키패드(텐키)가 모니터를 제어합니다. 상단 숫자열은 그대로 유지됩니다. 각 버튼 우클릭으로 키 변경. DAW 구동 중에만 동작.")
            Spacer()
            Button("기본값") { engine.resetMonitorShortcutsToDefault() }
                .font(Theme.Font.ui(8))
                .buttonStyle(.plain)
                .foregroundStyle(Theme.Palette.textFaint)
        }
        .frame(maxWidth: .infinity)
    }

    /// Dim-amount picker for the Dim button's right-click menu.
    @ViewBuilder
    private var dimAmountMenu: some View {
        Menu("디밍 레벨 — \(Int(engine.monitorDimDb)) dB") {
            ForEach(EngineController.monitorDimDbOptions, id: \.self) { db in
                Button {
                    engine.setMonitorDimDb(db)
                } label: {
                    if Int(engine.monitorDimDb) == Int(db) { Label("\(Int(db)) dB", systemImage: "checkmark") }
                    else { Text("\(Int(db)) dB") }
                }
            }
        }
    }

    /// Right-click reassignment submenu for one monitor action.
    @ViewBuilder
    private func shortcutMenu(_ action: EngineController.MonitorShortcutAction) -> some View {
        let current = engine.monitorShortcutKey(action)
        Menu("\(action.label) 단축키 — \(current.map(EngineController.monitorShortcutKeyLabel) ?? "없음")") {
            ForEach(EngineController.monitorShortcutAssignableKeys, id: \.code) { key in
                Button {
                    engine.setMonitorShortcutKey(action, key.code)
                } label: {
                    if current == key.code { Label(key.label, systemImage: "checkmark") } else { Text(key.label) }
                }
            }
            Divider()
            Button("없음") { engine.setMonitorShortcutKey(action, nil) }
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
            // Speaker vs headphone is the modeller choice, and it is always one or the other — the
            // inactive tab dims. (Plugging headphones into the interface while in speaker mode is
            // just physical output routing; it applies no headphone DSP and is unrelated to this.)
            HStack(spacing: Theme.Space.sm) {
                outputTab("🔊 스피커", active: engine.outputMode == .speaker) {
                    engine.outputMode = .speaker
                }
                .opacity(engine.outputMode == .speaker ? 1 : 0.45)
                .contextMenu { speakerMenu }
                outputTab("🎧 헤드폰", active: engine.outputMode == .headphone) {
                    engine.outputMode = .headphone
                }
                .opacity(engine.outputMode == .headphone ? 1 : 0.45)
                .contextMenu { headphoneMenu }
            }

            // The physical model of whichever output is active (right-click a tab to set it).
            HStack(spacing: Theme.Space.sm) {
                Text("실물 모델")
                    .font(Theme.Font.ui(9, .medium))
                    .foregroundStyle(Theme.Palette.textSecondary)
                Spacer()
                Text(activePhysicalModelLabel)
                    .font(Theme.Font.mono(7.5))
                    .foregroundStyle(Theme.Palette.textFaint)
                    .lineLimit(1)
            }

            speakerSets
            if engine.outputMode == .headphone {
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
                           measured: Set<String> = [],
                           onPick: @escaping (String) -> Void) -> some View {
        Button("\(title)…") {
            modelPicker = ModelPickerContext(title: title, catalog: catalog,
                                             selected: selected, measured: measured, onPick: onPick)
        }
    }

    /// Audio-interface D/A picker group, shared by both output tabs. Two purposes:
    /// (1) name the physical interface (flat environment), (2) render it AS another model (A→B).
    /// Purpose 2 is catalog-only until raw measured profiles exist, so it shows its own honest
    /// status instead of pretending to colour the sound.
    @ViewBuilder
    private var audioInterfaceMenuGroup: some View {
        modelMenu("실물 오디오 인터페이스", catalog: engine.audioInterfaceModelCatalog,
                  selected: engine.physicalAudioInterfaceModel,
                  measured: engine.audioInterfaceMeasured) { engine.setPhysicalAudioInterfaceModel($0) }
        Menu("오디오 인터페이스 모델링") {
            Button {
                engine.setPhysicalAudioInterfaceTargetModel("")
            } label: {
                if engine.physicalAudioInterfaceTargetModel.isEmpty { Label("사용 안 함(원본 그대로)", systemImage: "checkmark") }
                else { Text("사용 안 함(원본 그대로)") }
            }
            modelMenu("대상 모델", catalog: engine.audioInterfaceModelCatalog,
                      selected: engine.physicalAudioInterfaceTargetModel,
                      measured: engine.audioInterfaceMeasured) { engine.setPhysicalAudioInterfaceTargetModel($0) }
            Divider()
            if engine.audioInterfaceTransformActive {
                Text("FR 변환 활성 (실측 프로필)").font(.caption)
            } else {
                Text("카탈로그 전용 — 오디오 변환 없음").font(.caption).foregroundStyle(.secondary)
                Text("실측 원시 프로필 확보 시 활성화").font(.caption2).foregroundStyle(.secondary)
            }
            Divider()
            // Optional 2단계: nonlinear harmonic character (waveshaper). The long tooltip explains it.
            Button {
                engine.setMonitorInterfaceModeling(!engine.monitorInterfaceModelingEnabled)
            } label: {
                if engine.monitorInterfaceModelingEnabled { Label("고조파 모델링 (웨이브셰이퍼)", systemImage: "checkmark") }
                else { Text("고조파 모델링 (웨이브셰이퍼)") }
            }
            .help(Self.waveshaperHelp)
        }
        // Interface loopback measurement (level meter, auto multi-level, THD curve) lives in the
        // "모니터 EQ · 응답" window's output-interface card — no duplicate menu here.
    }

    /// Long help for the harmonic (waveshaper) modeling option — kept verbatim per request.
    static let waveshaperHelp = """
    고조파 모델링 (웨이브셰이퍼)

    웨이브셰이퍼는 입력 샘플값을 전달 함수(곡선) f(x)에 통과시켜 출력을 만드는 비선형 DSP입니다. \
    이 곡선이 완벽한 직선(y=x)이면 소리가 안 바뀌고, 살짝 휘면 고조파(하모닉)와 왜곡이 생깁니다. \
    진공관·테이프·아날로그 회로 특유의 "따뜻한" 색깔이 바로 이 비선형 전달 곡선에서 나옵니다.

    • 1단계 EQ = 주파수별 크기만 바꿈 → 선형, 고조파 없음.
    • 2단계 웨이브셰이퍼(이 옵션) = 파형 모양 자체를 휘어 비선형 → H2·H3… 고조파를 만듦.

    이 옵션은 성적서의 레벨별 H2–H7 실측치에 맞춰 체비쇼프(Chebyshev) 다항식 전달 곡선을 설계해, \
    모델링하는 인터페이스가 실제로 내는 고조파를 그대로 재현합니다. 대상(모델링) 인터페이스가 있으면 \
    그쪽, 없으면 실물 인터페이스의 실측 고조파를 씁니다.

    주의: 커즈와일 UNiTE-2처럼 깨끗한 인터페이스는 고조파가 −67~−117 dBc로 가청 한계 아래라 \
    사실상 안 들립니다. 이 기능은 고조파가 큰 컬러드/빈티지 인터페이스를 측정했을 때 진가를 발휘합니다. \
    실측 데이터가 없으면 아무것도 하지 않습니다(계수 0 = 완전 통과, null test 통과).
    """

    /// The 스피커 tab's right-click menu: physical output device plus the real speaker
    /// model the user monitors on (not the A/B/C simulator).
    @ViewBuilder
    private var speakerMenu: some View {
        deviceMenu
        Divider()
        // Physical output = the interface D/A only. The real speaker (+ its amp/cable) now lives
        // per-slot on each A/B/C set, so it moved out of this device picker.
        audioInterfaceMenuGroup
        Divider()
        // Speakers wired backwards: swap L/R in the monitor path.
        Button {
            engine.toggleMonitorSwapLeftRight()
        } label: {
            if engine.monitorSwapLeftRight { Label("좌우(L/R) 스왑", systemImage: "checkmark") }
            else { Text("좌우(L/R) 스왑") }
        }
    }

    @ViewBuilder
    private var headphoneMenu: some View {
        deviceMenu
        Divider()
        // "모델 없음" clears the physical headphone model (no correction / raw output).
        Button { engine.setPhysicalHeadphoneModel("") } label: {
            if engine.physicalHeadphoneModel.isEmpty { Label("모델 없음", systemImage: "checkmark") }
            else { Text("모델 없음") }
        }
        modelMenu("실물 헤드폰 모델", catalog: engine.headphoneModelCatalog,
                  selected: engine.physicalHeadphoneModel,
                  measured: engine.measuredHeadphoneTargetSet) { engine.setPhysicalHeadphoneModel($0) }
        // The headphone side's OWN output pair — no longer riding the active speaker slot's
        // route. "메인 (기본)" is the main L/R pair, the same non-mute meaning as the speaker
        // slots' "None".
        Menu("물리 출력") {
            let current = engine.headphoneOutputRoute
            Button { engine.setHeadphoneOutput("") } label: {
                Text((current.isEmpty || current == "None" ? "✓ " : "") + "메인 (기본)")
            }
            Button { engine.setHeadphoneOutput("Main 1-2") } label: {
                Text((current == "Main 1-2" ? "✓ " : "") + "Main 1-2")
            }
            ForEach(Array(stride(from: 3, through: 31, by: 2)), id: \.self) { first in
                let route = "Output \(first)-\(first + 1)"
                Button { engine.setHeadphoneOutput(route) } label: {
                    Text((current == route ? "✓ " : "") + route)
                }
            }
        }
        audioInterfaceMenuGroup
    }

    /// Right-click device picker. "시스템 기본" leaves the id empty, which is a valid
    /// choice — the engine opens the OS default. Building the menu rescans the devices.
    @ViewBuilder
    private var deviceMenu: some View {
        // The selected mark is a text glyph, not a swapped-in SF Symbol: a branch between
        // Label(…, systemImage:) and Text re-inserts the image every time the menu re-renders,
        // which SwiftUI animates (the checkmark "grows and shrinks"). A prefixed character has
        // stable identity, so it never flickers no matter how often the menu re-evaluates.
        Text("출력 장치").font(.caption)
        Button {
            engine.setOutputDevice("")
        } label: {
            Text((engine.currentOutputDeviceId.isEmpty ? "✓  " : "     ")
                 + "시스템 기본 · \(engine.activeOutputDeviceName)")
        }
        Divider()
        ForEach(engine.outputDevices) { device in
            Button {
                engine.setOutputDevice(device.id)
            } label: {
                Text((engine.currentOutputDeviceId == device.id ? "✓  " : "     ") + device.name)
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
                            value: engine.speakerSimulationActive
                                ? String(format: "%.0f", set.simWeight * 100)
                                : String(format: "%.0f · %@", set.simWeight * 100, set.roomEq ? "룸 EQ" : "직결"),
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

            Button {
                showSpeakerComparison = true
            } label: {
                Label("모니터 EQ · 응답", systemImage: "chart.xyaxis.line")
                    .font(Theme.Font.ui(9.5, .semibold))
                    .foregroundStyle(Theme.Palette.purpleLight)
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 7)
                    .background(
                        RoundedRectangle(cornerRadius: Theme.Radius.button)
                            .fill(Theme.Palette.purple.opacity(0.12))
                            .overlay(RoundedRectangle(cornerRadius: Theme.Radius.button)
                                .stroke(Theme.Palette.purple.opacity(0.38), lineWidth: 1))
                    )
            }
            .buttonStyle(.plain)
            .accessibilityLabel("모니터 EQ · 응답")

            // Linear-phase FIR toggle out here (not just inside the EQ window) — it's a mix/master
            // decision (exact phase vs added latency), so keep it one click away with its latency.
            HStack(spacing: 6) {
                Text("선형위상 EQ").font(Theme.Font.mono(9)).foregroundStyle(Theme.Palette.textSecondary)
                Spacer(minLength: 0)
                if engine.monitorEqLinearPhase, engine.monitorEqLatencyMs > 0 {
                    Text(String(format: "지연 %.0f ms", engine.monitorEqLatencyMs))
                        .font(Theme.Font.mono(9)).foregroundStyle(Theme.Palette.amber)
                }
                Toggle("", isOn: Binding(get: { engine.monitorEqLinearPhase },
                                         set: { engine.setMonitorEqLinearPhase($0) }))
                    .labelsHidden().toggleStyle(.switch).controlSize(.mini)
            }
            .help("FIR 선형위상 — 전대역 정확 매칭(저역 급경사·고역 딥 포함), 대신 지연 추가. 믹스/마스터용.")

            // The linear-phase EQ's delay is what makes a keyboard feel late. This is the
            // switch that hands it back while you play, and says when it is doing so.
            if engine.monitorEqLinearPhase {
                HStack(spacing: 6) {
                    Text("연주 시 저지연").font(Theme.Font.mono(9)).foregroundStyle(Theme.Palette.textSecondary)
                    Spacer(minLength: 0)
                    if engine.monitorEqLowLatencyActive {
                        Text("적용 중").font(Theme.Font.mono(9)).foregroundStyle(Theme.Palette.green)
                    }
                    Toggle("", isOn: Binding(get: { engine.monitorEqLowLatencyMonitoring },
                                             set: { engine.setMonitorEqLowLatencyMonitoring($0) }))
                        .labelsHidden().toggleStyle(.switch).controlSize(.mini)
                }
                .help("녹음 대기·인풋 모니터 중인 트랙이 있으면 선형위상 대신 최소위상으로 — 같은 커브, 지연 0. 연주는 제때, 믹스는 정확하게.")
            }

            vrCorrectionSection
        }
    }

    /// VR / headset-worn monitor correction. Wearing a Meta Quest 3 (or any headset) while
    /// monitoring on real speakers reshapes the sound at the ears; this measures that change
    /// (a room sweep with the headset OFF, then ON) and undoes it in the monitor EQ.
    private var vrCorrectionSection: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack(spacing: 6) {
                Label("VR 헤드셋 보정", systemImage: "visionpro")
                    .font(Theme.Font.mono(9)).foregroundStyle(Theme.Palette.textSecondary)
                Spacer(minLength: 0)
                if engine.vrCorrectionActive {
                    Text(engine.vrCorrectionEnabled ? "적용 중" : "꺼짐")
                        .font(Theme.Font.mono(9))
                        .foregroundStyle(engine.vrCorrectionEnabled ? Theme.Palette.green : Theme.Palette.textFaint)
                }
                Toggle("", isOn: Binding(get: { engine.vrCorrectionEnabled },
                                         set: { engine.setVrCorrectionEnabled($0) }))
                    .labelsHidden().toggleStyle(.switch).controlSize(.mini)
                    .disabled(!engine.vrCorrectionActive)
            }
            .help("헤드셋을 쓰고 실물 스피커로 모니터할 때의 음색 변화를 상쇄합니다. 먼저 룸 측정을 실행한 뒤 기준/착용을 잡으세요.")

            HStack(spacing: 6) {
                Button {
                    _ = engine.vrCaptureBaseline()
                } label: {
                    Text(engine.vrHasBaseline ? "기준 ✓" : "기준(벗음)")
                        .font(Theme.Font.mono(8.5))
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 3)
                }
                .buttonStyle(.plain)
                .background(RoundedRectangle(cornerRadius: 4).fill(Theme.Palette.button))
                .overlay(RoundedRectangle(cornerRadius: 4).stroke(engine.vrHasBaseline ? Theme.Palette.green.opacity(0.5) : Theme.Palette.divider, lineWidth: 1))
                .help("헤드셋을 벗은 상태에서 룸 측정을 실행한 뒤 눌러 기준으로 저장합니다.")

                Button {
                    _ = engine.vrCaptureWorn()
                } label: {
                    Text("착용 보정")
                        .font(Theme.Font.mono(8.5))
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 3)
                }
                .buttonStyle(.plain)
                .background(RoundedRectangle(cornerRadius: 4).fill(Theme.Palette.button))
                .overlay(RoundedRectangle(cornerRadius: 4).stroke(Theme.Palette.divider, lineWidth: 1))
                .disabled(!engine.vrHasBaseline)
                .opacity(engine.vrHasBaseline ? 1 : 0.4)
                .help("헤드셋을 쓰고 다시 룸 측정을 실행한 뒤 눌러 보정 커브(기준−착용)를 만듭니다.")

                if engine.vrCorrectionActive {
                    Button { engine.vrClearCorrection() } label: {
                        Image(systemName: "trash").font(.system(size: 9))
                            .frame(width: 22).padding(.vertical, 3)
                    }
                    .buttonStyle(.plain)
                    .background(RoundedRectangle(cornerRadius: 4).fill(Theme.Palette.button))
                    .overlay(RoundedRectangle(cornerRadius: 4).stroke(Theme.Palette.divider, lineWidth: 1))
                    .help("보정 지우기")
                }
            }
            if !engine.vrStatusMessage.isEmpty {
                Text(engine.vrStatusMessage)
                    .font(Theme.Font.mono(8.5))
                    .foregroundStyle(engine.vrCorrectionActive ? Theme.Palette.green : Theme.Palette.textFaint)
                    .fixedSize(horizontal: false, vertical: true)
            } else if !engine.vrHasMeasurement {
                Text("먼저 모니터 측정(룸/헤드셋)을 실행해야 활성화됩니다")
                    .font(Theme.Font.mono(8.5))
                    .foregroundStyle(Theme.Palette.textFaint)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .padding(.top, 2)
        .helpTip("""
        VR 헤드셋 보정
        헤드셋을 쓰고 실물 스피커로 모니터할 때 생기는
        음색 변화를 측정해 상쇄하는 보정 EQ입니다.

        [사용법]
        1) 헤드셋을 벗고 룸 측정 → '기준(벗음)'
        2) 헤드셋을 쓰고 다시 룸 측정 → '착용 보정'
           (두 측정의 차이 = 기준−착용 이 보정 커브)
        3) 스위치로 켜고 끄기, 휴지통으로 초기화
        ※ 측정 마이크(UMIK-1 등)가 있어야 작동합니다.

        [이 기능의 범위와 한계]
        · 헤드셋이 입히는 '음색(주파수응답) 착색'만
          되돌립니다. 크기(magnitude) 커브만 다루며
          위상·시간영역·좌우 개별 보정은 없습니다.
        · 공간을 만드는 '바이너럴/HRTF'가 아닙니다.
          (Sennheiser AMBEO·Smyth Realiser 같은
          '헤드폰 위 가상 스튜디오'와는 방향이 다름)

        [규격만으로 되는 것 / 안 되는 것]
        · 헤드셋의 기기 주파수응답은 규격·실측으로
          추론 가능 — 이 보정이 다루는 영역입니다.
        · HRTF는 기기 규격이 아니라 '듣는 사람의
          신체'에 대한 함수라, 헤드셋 스펙만으론
          추론할 수 없습니다.

        [가상 스튜디오로 확장한다면 (로드맵)]
        · 개인 측정 없이도 시작 가능합니다:
          공개 범용 HRTF(KEMAR/SADIE/CIPIC)
          + 우리 룸·스피커 시뮬
          + VR 런타임의 헤드 트래킹(공짜)을 결합.
        · 범용 HRTF는 '쓸만함' 수준(사람에 따라
          앞뒤 혼동), 레퍼런스급은 개인화가 필요합니다.
        """)
    }

    /// The right-click menu for an A/B/C speaker set: choose its modelled speaker, or a
    /// physical output pair (which bypasses the model), plus the room-EQ toggle.
    @ViewBuilder
    private func speakerSetMenu(_ set: EngineController.SpeakerSet) -> some View {
        let bare = set.displayModel
        let isHeadphone = engine.headphoneModelHasCurve(bare)
        let modelled = set.output == "None"
        Text("\(set.letter) · \(set.name)")
        // Number-row shortcut for selecting this set (A/B/C).
        shortcutMenu(set.id == 1 ? .setB : set.id == 2 ? .setC : .setA)
        Divider()
        // Each A/B/C slot fully describes one monitor path: the REAL speaker you have (+ its amp
        // and cable, for a passive one — leave empty for an active speaker), then the SIMULATOR
        // you want to hear, room EQ, and the physical output pair it drives.
        modelMenu("실물 스피커 모델", catalog: engine.speakerModelCatalog,
                  selected: set.realModel,
                  measured: engine.measuredMonitorTargetSet) { engine.setSpeakerRealModel(set.id, $0) }
        // The REAL speaker's amp + cable — only for a PASSIVE real speaker (an active monitor has
        // them built in). This is the chain you actually hear on; the correction flattens it.
        if set.realModelIsPassive {
            modelMenu("실물 파워앰프", catalog: engine.powerAmpModelCatalog,
                      selected: set.realAmp) { engine.setSpeakerRealAmp(set.id, $0) }
            modelMenu("실물 스피커 케이블", catalog: engine.speakerCableModelCatalog,
                      selected: set.realCable) { engine.setSpeakerRealCable(set.id, $0) }
        }
        Divider()
        // The SIMULATOR this slot voices toward. In HEADPHONE mode it may instead model a
        // headphone (mutually exclusive — picking one replaces the other).
        modelMenu("스피커 시뮬레이터", catalog: engine.speakerModelCatalog,
                  selected: (modelled && !isHeadphone) ? bare : "",
                  measured: engine.measuredMonitorTargetSet) { engine.setSpeakerModel(set.id, $0) }
        if engine.outputMode == .headphone {
            modelMenu("헤드폰 시뮬레이터", catalog: engine.headphoneModelCatalog,
                      selected: (modelled && isHeadphone) ? bare : "",
                      measured: engine.measuredHeadphoneTargetSet) { engine.setSpeakerModel(set.id, $0) }
        }
        // The MODELING (target) speaker's amp + cable — only for a PASSIVE modeled speaker. These
        // colour the simulation (added), independent of the real speaker's amp/cable above.
        if set.modelIsPassive && !isHeadphone {
            modelMenu("모델링 파워앰프", catalog: engine.powerAmpModelCatalog,
                      selected: set.amp) { engine.setSpeakerAmp(set.id, $0) }
            modelMenu("모델링 스피커 케이블", catalog: engine.speakerCableModelCatalog,
                      selected: set.cable) { engine.setSpeakerCable(set.id, $0) }
        }
        Divider()
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
        // Room EQ applies on top of the modelled/physical path for this slot.
        Button {
            engine.setSpeakerRoomEq(set.id, !set.roomEq)
        } label: {
            if set.roomEq { Label("룸 EQ", systemImage: "checkmark") } else { Text("룸 EQ") }
        }
    }

    private var headphonePanel: some View {
        VStack(alignment: .leading, spacing: Theme.Space.sm) {
            Text("헤드폰 모니터 → 위 A/B/C 가상 스피커 + 크로스피드 / 헤드폰 보정")
                .font(Theme.Font.ui(8.5))
                .foregroundStyle(Theme.Palette.textFaint)
            let sim = engine.speakerSimulationActive
            let cross = engine.headphoneSimulationActive || engine.crossfeedActive
            StatRow(label: "가상 스피커", value: sim ? "켜짐" : "꺼짐",
                    valueColor: sim ? Theme.Palette.purpleLight : Theme.Palette.textFaint)
            StatRow(label: "크로스피드/보정", value: cross ? "켜짐" : "꺼짐",
                    valueColor: cross ? Theme.Palette.purpleLight : Theme.Palette.textFaint)
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
                .contextMenu { shortcutMenu(.sourceMaster) }
                // "Other apps" = Core Audio process tap (replaced the BlackHole loopback). Captures
                // every other app's output directly — no BlackHole, no device, no mic permission.
                // Reference-hold: left-click arms + A/Bs master↔reference (apps stay muted the whole
                // time, so nothing leaks); right-click → 레퍼런스 종료 unmutes the apps.
                dimButton("다른 앱", engine.monitorListenSource, Theme.Palette.teal) {
                    engine.selectMonitorInput(blackHole: true)
                }
                .overlay(alignment: .topTrailing) {
                    // Armed but auditioning the master: the apps are still muted (held), so mark it.
                    if engine.referenceArmed && !engine.monitorListenSource {
                        Text("홀드")
                            .font(Theme.Font.ui(7, .bold))
                            .foregroundStyle(Theme.Palette.teal)
                            .padding(.horizontal, 3).padding(.vertical, 1)
                            .background(RoundedRectangle(cornerRadius: 3).fill(Theme.Palette.teal.opacity(0.18)))
                            .padding(3)
                    }
                }
                .contextMenu {
                    if engine.referenceArmed {
                        Button("레퍼런스 종료 (다른 앱 소리 복구)") { engine.exitReference() }
                        Divider()
                    }
                    shortcutMenu(.sourceBlackHole)
                }
                .help("다른 앱(브라우저·유튜브 등) 소리를 프로세스 탭으로 모니터에 가져옵니다. 켜면 그 앱들은 계속 음소거되어 컴퓨터로 새지 않고(홀드), 좌클릭으로 마스터↔다른 앱을 A/B 합니다. 우클릭 → 레퍼런스 종료로 원래대로. BlackHole 불필요.")
            }
            // Global keypad capture: drive the monitor station from the numeric keypad even when
            // another app is frontmost. (An "interface exclusive/hog" toggle used to sit here too,
            // but macOS hog mode does not actually block other apps from the device — verified — so
            // it was removed rather than mislead.)
            HStack(spacing: Theme.Space.sm) {
                dimButton(engine.keypadCaptureRequested && !engine.keypadCaptureEnabled
                              ? "키패드 권한 필요" : "키패드 독점",
                          engine.keypadCaptureRequested, Theme.Palette.teal) {
                    engine.setKeypadCapture(!engine.keypadCaptureRequested)
                }
                .help("켜면 다른 앱이 앞에 있어도 숫자 키패드로 모니터 볼륨(+/−)·Dim(0)·Mute(.)·Talk(Enter, 누르는 동안)·스피커 A/B/C(1/2/3) 등을 제어합니다. 바인딩은 키패드 단축키 설정을 따릅니다. 손쉬운 사용 권한 필요.")
                Spacer(minLength: 0)
            }
            // The level meters live here now rather than in the transport bar: the control-room
            // meter belongs beside the monitor controls that shape it, and moving them gives the
            // transport bar its width back.
            ControlRoomMeters(meters: engine.meters)
                .padding(.top, 2)
        }
        .onAppear { engine.refreshInputDevices() }
    }

    /// Right-click menu on the BlackHole source button: pick which internal input device to
    /// capture (BlackHole 2ch/16ch, or any input). Defaults to BlackHole 2ch when first enabled.
    @ViewBuilder
    private var blackHoleSourceMenu: some View {
        Menu("입력 장치") {
            if engine.inputDevices.isEmpty {
                Text("입력 장치 없음").font(.caption)
            } else {
                ForEach(engine.inputDevices) { dev in
                    Button {
                        engine.selectMonitorInputDevice(dev.id)
                    } label: {
                        if engine.monitorListenSource && engine.currentInputDeviceId == dev.id {
                            Label(dev.name, systemImage: "checkmark")
                        } else {
                            Text(dev.name)
                        }
                    }
                }
            }
            Divider()
            Button("장치 새로고침") { engine.refreshInputDevices() }
        }
        Divider()
        shortcutMenu(.sourceBlackHole)
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

    /// Job -> machine, one row each, plus the auto-overflow switch that turns the assignments into
    /// starting points. A machine that is switched off or unreachable is still selectable: the
    /// assignment is intent, and the load rows above say whether it is being honoured.
    private var dspRoleTable: some View {
        VStack(alignment: .leading, spacing: Theme.Space.sm) {
            HStack {
                sectionLabel("DSP 역할 배정")
                Spacer(minLength: 0)
                Text(engine.dspAutoOverflow ? "자동 넘김" : "직접 배정")
                    .font(Theme.Font.mono(7))
                    .foregroundStyle(engine.dspAutoOverflow ? Theme.Palette.teal : Theme.Palette.textFaint)
                Toggle("", isOn: Binding(get: { engine.dspAutoOverflow },
                                         set: { engine.setDspAutoOverflow($0) }))
                    .labelsHidden().toggleStyle(.switch).scaleEffect(0.7).frame(width: 34)
            }
            ForEach(EngineController.DspJob.allCases) { job in
                HStack(spacing: Theme.Space.sm) {
                    Text(job.label)
                        .font(Theme.Font.ui(9))
                        .foregroundStyle(Theme.Palette.textSecondary)
                        .frame(width: 62, alignment: .leading)
                    ForEach(EngineController.DspMachine.allCases) { machine in
                        let picked = engine.dspRole(job) == machine
                        // The machine card's own switch (below) gates its column here: a machine
                        // that is turned OFF cannot take new assignments, and an assignment it
                        // already holds reads amber — saved, but not running anywhere.
                        let machineOn = dspMachineEnabled(machine)
                        // A job with no remote path yet still stores its assignment, but it must
                        // not look live: amber, not teal, so the row reads as "saved, not routed".
                        let live = picked && machineOn && (job.routed || machine == .internalDsp)
                        Button { engine.setDspRole(job, machine) } label: {
                            Text(machine.label)
                                .font(Theme.Font.mono(7.5, picked ? .bold : .regular))
                                .foregroundStyle(picked ? Theme.Palette.background : Theme.Palette.textFaint)
                                .frame(maxWidth: .infinity)
                                .padding(.vertical, 3)
                                .background(RoundedRectangle(cornerRadius: 3)
                                    .fill(!picked ? Theme.Palette.keyFace
                                          : live ? Theme.Palette.teal : Theme.Palette.amber))
                        }
                        .buttonStyle(.plain)
                        .disabled(!machineOn && !picked)
                        .opacity(machineOn || picked ? 1 : 0.3)
                        .help(machineOn ? "" : "\(machine.label) 기계가 꺼져 있습니다 — 아래 카드에서 켜면 선택할 수 있습니다")
                    }
                }
            }
            Text(engine.dspAutoOverflow
                    ? "배정한 기계가 모자라면 내장 → NDS → 외부 노드 순으로 넘어갑니다."
                    : "배정한 기계에서만 처리합니다. 모자라도 다른 기계로 넘어가지 않습니다.")
                .font(Theme.Font.mono(6.5))
                .foregroundStyle(Theme.Palette.textFainter)
                .fixedSize(horizontal: false, vertical: true)
            // Say plainly what a remote assignment does and does not cover, so the table is not
            // read as "the DAW runs on the node". Playback, recording, instruments, third-party
            // plug-ins and the bounce are always local — the bounce deliberately so.
            let unrouted = EngineController.DspJob.allCases
                .filter { !$0.routed && engine.dspRole($0) != .internalDsp }
            if !unrouted.isEmpty {
                Text("주황색 = 저장만 됨 (\(unrouted.map(\.label).joined(separator: " · "))은 아직 원격 경로가 없어 내장에서 처리)")
                    .font(Theme.Font.mono(6.5))
                    .foregroundStyle(Theme.Palette.amber)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Text("파일 재생·녹음 캡처·악기·서드파티 플러그인·믹스다운은 항상 이 맥에서 처리합니다.")
                .font(Theme.Font.mono(6.5))
                .foregroundStyle(Theme.Palette.textFainter)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    /// A machine's load: its own busiest core, read from that machine's own report — the two nodes
    /// answer separately, so they cannot share one figure. "사용 안 함" when the machine is switched
    /// off, "응답 없음" when it is on but nothing came back, "대기" when it answered but reports no
    /// load yet.
    private func remoteLoadText(_ machine: EngineController.DspMachine, active: Bool) -> String {
        guard active else { return "사용 안 함" }
        guard let specs = engine.nodeSpecs(for: machine) else { return "응답 없음" }
        guard specs.cpuLoadPercent >= 0 else { return "대기" }
        return String(format: "%.0f%%", specs.cpuLoadPercent)
    }

    /// A machine's load as a 0…1 bar fill. Unknown reads as empty rather than as zero-with-a-mark:
    /// the number beside it says which it is.
    private func remoteLoadFraction(_ machine: EngineController.DspMachine) -> Double {
        guard let specs = engine.nodeSpecs(for: machine), specs.cpuLoadPercent >= 0 else { return 0 }
        return min(1.0, specs.cpuLoadPercent / 100.0)
    }

    /// One machine's load: name, figure, bar. Identical for all three so they compare at a glance;
    /// a switched-off machine keeps its row and dims, so its absence from the mix is visible.
    private func machineLoadMeter(label: String,
                                  on: Bool,
                                  fraction: Double,
                                  text: String,
                                  hot: Bool) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack(spacing: Theme.Space.sm) {
                Text(label)
                    .font(Theme.Font.ui(9))
                    .foregroundStyle(on ? Theme.Palette.textLabel : Theme.Palette.textFainter)
                Spacer(minLength: 0)
                Text(text)
                    .font(Theme.Font.mono(9, .medium))
                    .foregroundStyle(!on ? Theme.Palette.textFainter
                                     : hot ? Theme.Palette.red : Theme.Palette.textSecondary)
            }
            MeterBar(fraction: on ? fraction : 0, gradient: Theme.Gradient.dspLoad)
                .opacity(on ? 1 : 0.4)
        }
    }

    private var meterCard: some View {
        VStack(alignment: .leading, spacing: Theme.Space.md) {
            // Three machines, three meters. They are peers — work is assigned to any of them —
            // so they read as peers: same row, same bar, same scale. All three stay visible
            // whether or not they are in use, because a meter that vanishes cannot tell you it
            // is idle, and a machine is ON by its own switch rather than by whether the monitor
            // happens to point at it (a node carrying only track inserts still has a load).
            sectionLabel("DSP 부하")
            machineLoadMeter(label: "내장",
                             on: true,
                             fraction: engine.dspLoadFraction,
                             text: String(format: "%.0f%%", engine.dspLoadFraction * 100),
                             hot: engine.dspLoadFraction > 0.8)
            machineLoadMeter(label: "NDS",
                             on: engine.ndsEnabled,
                             fraction: remoteLoadFraction(.nds),
                             text: remoteLoadText(.nds, active: engine.ndsEnabled),
                             hot: remoteLoadFraction(.nds) > 0.8)
            machineLoadMeter(label: "외부 노드",
                             on: engine.externalDspEnabled,
                             fraction: remoteLoadFraction(.external),
                             text: remoteLoadText(.external, active: engine.externalDspEnabled),
                             hot: remoteLoadFraction(.external) > 0.8)

            StatRow(label: "지터 (Jitter)", value: String(format: "%.0f µs", engine.wakeJitterUs))
            // The jitter row above watches the OUTPUT render thread. A crackle while listening to
            // another app comes from the TAP CAPTURE side, which that number can never see — so it
            // gets its own row. Only shown once the tap has actually faulted.
            if engine.referenceTapFaults > 0 {
                StatRow(label: "레퍼런스 탭 결함",
                        value: "\(engine.referenceTapFaults)회",
                        valueColor: engine.referenceTapFaults < 5 ? Theme.Palette.amber : Theme.Palette.red)
            }
            MeterBar(fraction: jitterFraction,
                     gradient: LinearGradient(colors: [Theme.Palette.green, Color(hex: 0x6fa6d0)],
                                              startPoint: .leading, endPoint: .trailing))

            // Render-deadline misses = potential dropouts (playback keeps rolling but glitches).
            // 0 = clean. A rising count means this buffer/OS load is not safe for recording. The ↺
            // clears the tally once you have addressed the cause and want to watch it fresh.
            HStack {
                Text("드롭아웃 (레이트)")
                    .font(Theme.Font.ui(9))
                    .foregroundStyle(Theme.Palette.textLabel)
                Spacer()
                Text(engine.dropoutCount == 0 ? "0 · 안전" : "\(engine.dropoutCount)회")
                    .font(Theme.Font.mono(9, .medium))
                    .foregroundStyle(engine.dropoutCount == 0 ? Theme.Palette.green
                                   : engine.dropoutCount < 5 ? Theme.Palette.amber : Theme.Palette.red)
                if engine.dropoutCount > 0 {
                    Button { engine.resetDropoutCount() } label: {
                        Image(systemName: "arrow.counterclockwise")
                            .font(.system(size: 8, weight: .bold))
                    }
                    .buttonStyle(.plain)
                    .foregroundStyle(Theme.Palette.textFaint)
                    .help("드롭아웃 카운트 리셋")
                }
            }

            DockPhaseRows(meters: engine.meters)

            delayCompensationRow

            VStack(alignment: .leading, spacing: Theme.Space.sm) {
                Text("SPECTRUM")
                    .font(Theme.Font.mono(6.5))
                    .tracking(0.6)
                    .foregroundStyle(Theme.Palette.textFaint)
                DockSpectrumMini(meters: engine.meters, sampleRate: engine.sampleRate)
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

            Button { engine.reloadMonitorEq(); engine.monitorEqOpen = true } label: {
                HStack(spacing: 6) {
                    Image(systemName: "slider.horizontal.3").font(.system(size: 9))
                    Text("모니터 EQ").font(Theme.Font.ui(9, .medium))
                    if !engine.monitorEqBands.isEmpty {
                        Text("\(engine.monitorEqBands.count)").font(Theme.Font.mono(8)).foregroundStyle(Theme.Palette.accent)
                    }
                    Spacer()
                    Image(systemName: "chevron.right").font(.system(size: 7))
                }
                .foregroundStyle(Theme.Palette.textSecondary)
                .padding(.horizontal, Theme.Space.lg).padding(.vertical, Theme.Space.md)
                .background(RoundedRectangle(cornerRadius: Theme.Radius.button).fill(Theme.Palette.button))
            }
            .buttonStyle(.plain)

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

    /// The machine cards' switches gate everything above them that names the machine — the
    /// role-table columns and these source buttons. Off machine, dead controls.
    private func dspMachineEnabled(_ machine: EngineController.DspMachine) -> Bool {
        switch machine {
        case .internalDsp: return true
        case .nds: return engine.ndsEnabled
        case .external: return engine.externalDspEnabled
        }
    }

    private func dspSourceButton(_ title: String, _ source: EngineController.DspSource) -> some View {
        let on = engine.usesDspSource(source)
        let available = source == .internalDsp ? true
                      : source == .nds ? engine.ndsEnabled
                      : engine.externalDspEnabled
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
        .disabled(!available)
        .opacity(available ? 1 : 0.3)
        .help(available ? "" : "이 기계가 꺼져 있습니다 — 원격 코어 카드에서 켜면 선택할 수 있습니다")
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

            // Which machine handles what. This is the control that actually decides routing; the
            // machine cards below only say where each machine is and whether it is switched on.
            // Explicit by default because a path that changes under you mid-take is worse than
            // one that runs short predictably.
            dspRoleTable

            Divider().overlay(Theme.Palette.divider)

            // Two machines, two cards. They speak the same NART protocol but they are not
            // interchangeable: NDS is the dedicated appliance (its own OS, its own timing, the
            // one a live console would run on), 외부 노드 is a general-purpose computer lent to
            // the session. One address and one switch each, so either can carry work alone.
            //
            // No internal core-isolation control here: macOS cannot reserve cores for a process.
            // THREAD_AFFINITY_POLICY returns KERN_NOT_SUPPORTED on Apple Silicon (measured), and
            // the render already runs on CoreAudio's IO thread, which the OS gives
            // time-constraint priority and audio-workgroup membership.
            machineCard(title: "NDS",
                        subtitle: "전용 어플라이언스 · 고정 지연 · OS 독립",
                        on: engine.ndsEnabled,
                        setOn: { engine.setNdsEnabled($0) },
                        host: RemoteHostField(kind: .nds),
                        detail: machineDetail(.nds, on: engine.ndsEnabled))
                // SoundGrid-style server options, where SoundGrid puts them: on the server card.
                .contextMenu {
                    Menu("서버 네트워크 버퍼") {
                        ForEach([64, 96, 128, 192, 256, 384, 512], id: \.self) { frames in
                            let ms = Double(frames) / 48.0
                            Button {
                                engine.setRemoteNetworkBufferFrames(frames)
                            } label: {
                                Text((engine.remoteNetworkBufferFrames == frames ? "✓ " : "")
                                     + "\(frames) / \(String(format: "%.1f", ms)) ms")
                            }
                        }
                    }
                    Menu("믹서 채널 구성") {
                        ForEach([8, 16, 32, 64], id: \.self) { channels in
                            Button {
                                engine.setRemoteMixerChannels(channels)
                            } label: {
                                Text((engine.remoteMixerChannels == channels ? "✓ " : "")
                                     + "\(channels) 입력 채널")
                            }
                        }
                    }
                    Text("버퍼가 작을수록 지연이 짧고, 클수록 LAN 지터에 강합니다. 채널 구성은 원격 믹서(M1+)가 준비할 용량입니다.")
                }

            machineCard(title: "외부 노드",
                        subtitle: "범용 컴퓨터 · 여유 코어 빌려 쓰기",
                        on: engine.externalDspEnabled,
                        setOn: { engine.setExternalDspEnabled($0) },
                        host: RemoteHostField(kind: .external),
                        // No core-count stepper: a connected node reports its own core count and
                        // that report wins, so the number only ever applied to a node that was
                        // not answering — it read as control over something it did not control.
                        detail: machineDetail(.external, on: engine.externalDspEnabled))

            // The discovered node's own hardware — shown once 검색 (or a refresh) gets a reply.
            if let specs = engine.remoteNodeSpecs {
                remoteNodeSpecsView(specs)
            }
        }
        .padding(Theme.Space.xl)
        .background(
            RoundedRectangle(cornerRadius: Theme.Radius.panel)
                .fill(Theme.Palette.background)
        )
    }

    /// A machine card's one-line state, from that machine's own answer: what it is, or why it is
    /// silent. Reading "노드 대기" while the node is plainly connected was the old panel's worst
    /// lie, and it came from nobody ever asking the node anything after 검색.
    private func machineDetail(_ machine: EngineController.DspMachine, on: Bool) -> String {
        guard on else { return "사용 안 함" }
        guard let specs = engine.nodeSpecs(for: machine) else { return "응답 없음" }
        // Only jobs that actually route: listing 마스터 here claimed the master bus was running on
        // the node when nothing sends it there.
        let jobs = EngineController.DspJob.allCases.filter { engine.dspRole($0) == machine && $0.routed }
        let where_ = jobs.isEmpty ? "배정 대기" : jobs.map(\.label).joined(separator: " · ")
        return "코어 \(specs.coreCount)개 · \(where_)"
    }

    /// One remote machine: name, what it is for, its master switch, its address, and one line of
    /// state. Both machines use the same card so the difference between them is the text, not the
    /// layout — the two were previously impossible to tell apart because only one had a card.
    private func machineCard(title: String,
                             subtitle: String,
                             on: Bool,
                             setOn: @escaping (Bool) -> Void,
                             host: RemoteHostField,
                             detail: String) -> some View {
        VStack(alignment: .leading, spacing: Theme.Space.sm) {
            HStack(spacing: Theme.Space.sm) {
                Toggle("", isOn: Binding(get: { on }, set: setOn))
                    .labelsHidden().toggleStyle(.switch).scaleEffect(0.7).frame(width: 34)
                VStack(alignment: .leading, spacing: 0) {
                    Text(title)
                        .font(Theme.Font.ui(9, .medium))
                        .foregroundStyle(on ? Theme.Palette.text : Theme.Palette.textFaint)
                        .lineLimit(1)
                    Text(subtitle)
                        .font(Theme.Font.mono(6.5))
                        .foregroundStyle(Theme.Palette.textFainter)
                        .lineLimit(1)
                }
                Spacer(minLength: 0)
                Text(detail)
                    .font(Theme.Font.mono(7))
                    .foregroundStyle(on ? Theme.Palette.textFaint : Theme.Palette.textFainter)
            }
            host
        }
        .padding(Theme.Space.md)
        .background(
            RoundedRectangle(cornerRadius: Theme.Radius.button)
                .fill(Theme.Palette.recess)
                .overlay(RoundedRectangle(cornerRadius: Theme.Radius.button)
                    .stroke(on ? Theme.Palette.divider : Theme.Palette.divider.opacity(0.4), lineWidth: 1))
        )
        .opacity(on ? 1.0 : 0.65)
    }

    // The discovered node's hardware, laid out as label/value rows. Only meaningful fields show —
    // an Apple-Silicon node reports 0 MHz (no fixed clock), so the clock is folded into the CPU line
    // only when non-zero.
    private func remoteNodeSpecsView(_ specs: EngineController.RemoteNodeSpecs) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            if !specs.model.isEmpty { specRow("노드", specs.model) }
            if !specs.cpuModel.isEmpty && specs.cpuModel != "unknown" {
                specRow("CPU", specs.cpuMhz > 0
                        ? String(format: "%@ · %.2f GHz", specs.cpuModel, specs.cpuMhz / 1000.0)
                        : specs.cpuModel)
            }
            if specs.coreCount > 0 { specRow("코어", "\(specs.coreCount)") }
            if specs.memoryMb > 0 {
                specRow("메모리", String(format: "%.0f GB", Double(specs.memoryMb) / 1024.0))
            }

            // What the NODE is doing. The rows above describe the hardware; these say whether it is
            // keeping up, which is the reason to watch a remote core at all. The node reports a
            // load per core, so this is its busiest one; bad packets are its dropout signal.
            if specs.cpuLoadPercent >= 0 || specs.packetsIn > 0 {
                Divider().overlay(Theme.Palette.divider).padding(.vertical, 2)
                if specs.cpuLoadPercent >= 0 {
                    StatRow(label: "노드 DSP 부하",
                            value: String(format: "%.0f%%", specs.cpuLoadPercent),
                            valueColor: specs.cpuLoadPercent > 80 ? Theme.Palette.red : Theme.Palette.textSecondary)
                    MeterBar(fraction: min(1, specs.cpuLoadPercent / 100), gradient: Theme.Gradient.dspLoad)
                }
                StatRow(label: "노드 왕복", value: String(format: "%.2f ms", specs.roundTripMs))
                StatRow(label: "노드 드롭아웃",
                        value: specs.badPackets == 0 ? "0 · 안전" : "\(specs.badPackets)회",
                        valueColor: specs.badPackets == 0 ? Theme.Palette.green : Theme.Palette.red)
                if specs.temperatureC > 0 {
                    StatRow(label: "노드 온도", value: String(format: "%.1f°C", specs.temperatureC))
                }
                specRow("패킷", "\(specs.packetsIn) in · \(specs.packetsOut) out")
            }
        }
        .padding(Theme.Space.md)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(
            RoundedRectangle(cornerRadius: Theme.Radius.card)
                .fill(Theme.Palette.surface)
        )
    }

    private func specRow(_ label: String, _ value: String) -> some View {
        HStack(alignment: .top, spacing: Theme.Space.sm) {
            Text(label)
                .font(Theme.Font.ui(8, .medium))
                .foregroundStyle(Theme.Palette.textFaint)
                .frame(width: 34, alignment: .leading)
            Text(value)
                .font(Theme.Font.mono(8))
                .foregroundStyle(Theme.Palette.textSecondary)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    private func sectionLabel(_ text: String) -> some View {
        Text(text)
            .font(Theme.Font.ui(9, .medium))
            .foregroundStyle(Theme.Palette.textLabel)
    }
}

/// The remote DSP node address field: edits a draft locally and commits on Enter or
/// blur so the engine is not retargeted (and history recorded) on every keystroke.
/// The address of one remote machine. Both machines get one; only the external node offers 검색,
/// because the broadcast probe answers with whatever general-purpose node replies first and would
/// happily overwrite the appliance's fixed address with it.
private struct RemoteHostField: View {
    enum Kind { case nds, external }
    let kind: Kind

    @EnvironmentObject private var engine: EngineController
    @State private var draft = ""
    @FocusState private var focused: Bool

    private var current: String { kind == .nds ? engine.ndsHost : engine.remoteDspHost }
    private var placeholder: String { kind == .nds ? "192.168.0.198" : "studio.local" }

    var body: some View {
        HStack(spacing: Theme.Space.sm) {
            Text("주소")
                .font(Theme.Font.mono(7.5))
                .foregroundStyle(Theme.Palette.textFaint)
            TextField(placeholder, text: $draft)
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
                        .fill(Theme.Palette.background)
                        .overlay(RoundedRectangle(cornerRadius: Theme.Radius.button)
                            .stroke(Theme.Palette.divider, lineWidth: 1))
                )
            if kind == .external {
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
        }
        .onAppear { draft = current }
        .onChange(of: current) { draft = $1 }   // reflect discover / project load
    }

    private func commit() {
        let trimmed = draft.trimmingCharacters(in: .whitespaces)
        guard !trimmed.isEmpty, trimmed != current else {
            draft = current
            return
        }
        switch kind {
        case .nds: engine.setNdsHost(trimmed)
        case .external: engine.setRemoteDspHost(trimmed)
        }
    }
}

/// The monitor Talkback key: momentary while held (and it pulls Dim in with it), double-click to
/// latch it on, a click on a latched key releases it — one console-style behavior, not a mode.
/// Right-click picks the talkback mic (interface input). SwiftUI press gestures do not fire reliably
/// for a static press inside the dock's ScrollView, so the whole control is a self-drawing NSView
/// that reads mouseDown/mouseUp (and clickCount) directly.
private struct TalkbackButton: View {
    @EnvironmentObject private var engine: EngineController

    var body: some View {
        TalkbackKeyRepresentable(isOn: engine.monitorTalkback,
                                 mics: engine.inputDevices.map { (id: $0.id, name: $0.name) },
                                 selectedMicId: engine.talkbackMicId,
                                 talkbackRoute: engine.talkbackRoute,
                                 talkbackChannel: engine.talkbackChannel,
                                 channelCount: engine.talkbackChannelCount,
                                 channelActivity: { engine.talkbackChannelActivity($0) },
                                 onEngaged: { engine.setTalkbackEngaged($0) },
                                 onSelectMic: { engine.setTalkbackMic($0) },
                                 onSelectRoute: { engine.setTalkbackRoute($0) },
                                 onSelectChannel: { engine.setTalkbackChannel($0) },
                                 onRefreshMics: { engine.refreshInputDevices() })
            .frame(maxWidth: .infinity)
            .frame(height: 26)
            .onAppear { engine.refreshInputDevices() }
    }
}

private struct TalkbackKeyRepresentable: NSViewRepresentable {
    let isOn: Bool
    let mics: [(id: String, name: String)]
    let selectedMicId: String
    let talkbackRoute: String
    let talkbackChannel: Int
    let channelCount: Int
    let channelActivity: (Int) -> Float
    let onEngaged: (Bool) -> Void
    let onSelectMic: (String) -> Void
    let onSelectRoute: (String) -> Void
    let onSelectChannel: (Int) -> Void
    let onRefreshMics: () -> Void
    func makeNSView(context: Context) -> TalkbackKeyView {
        let view = TalkbackKeyView()
        apply(to: view)
        return view
    }
    func updateNSView(_ view: TalkbackKeyView, context: Context) { apply(to: view) }
    private func apply(to view: TalkbackKeyView) {
        view.onEngaged = onEngaged
        view.onSelectMic = onSelectMic
        view.onSelectRoute = onSelectRoute
        view.onSelectChannel = onSelectChannel
        view.onRefreshMics = onRefreshMics
        view.mics = mics
        view.selectedMicId = selectedMicId
        view.talkbackRoute = talkbackRoute
        view.talkbackChannel = talkbackChannel
        view.channelCount = channelCount
        view.channelActivity = channelActivity
        view.isOn = isOn
    }
}

/// Draws the "Talk" key and runs the press logic for the current mode. `momentary`: live only
/// while held. `latch`: double-click engages, a click releases. Right-click picks the mode.
/// `suppress` swallows the rest of a click sequence after an unlatch so a double-click used to
/// release cannot immediately re-engage.
final class TalkbackKeyView: NSView {
    var onEngaged: ((Bool) -> Void)?
    var onSelectMic: ((String) -> Void)?
    var onSelectRoute: ((String) -> Void)?
    var onSelectChannel: ((Int) -> Void)?
    var onRefreshMics: (() -> Void)?
    var mics: [(id: String, name: String)] = []
    var selectedMicId: String = ""
    var talkbackRoute: String = "listen_room"
    var talkbackChannel: Int = 1
    var channelCount: Int = 1
    var channelActivity: ((Int) -> Float)?
    var isOn = false { didSet { if isOn != oldValue { needsDisplay = true } } }
    private var latched = false
    private var suppress = false
    private var downTimestamp: TimeInterval = 0
    private let tapSeconds: TimeInterval = 0.30

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

    // Console talkback key: a quick CLICK latches it on (click again = off); a HOLD is momentary
    // (talk while held) — the same tap/hold behavior as the keypad Talk shortcut.
    override func mouseDown(with event: NSEvent) {
        if latched {
            latched = false
            onEngaged?(false)                            // a click on a latched key turns it off
            suppress = true                              // ignore this whole press
            return
        }
        suppress = false
        downTimestamp = event.timestamp
        onEngaged?(true)                                 // engage; mouseUp decides click(latch) vs hold(release)
    }

    override func mouseUp(with event: NSEvent) {
        if suppress { suppress = false; return }
        if event.timestamp - downTimestamp < tapSeconds {
            latched = true                               // quick click → latch on (stays engaged)
        } else {
            onEngaged?(false)                            // hold → release
        }
    }

    /// Right-click: pick the talkback mic (interface input channel).
    override func rightMouseDown(with event: NSEvent) {
        onRefreshMics?()
        let menu = NSMenu()
        let header = NSMenuItem(title: "톡백 마이크", action: nil, keyEquivalent: "")
        header.isEnabled = false
        menu.addItem(header)
        if mics.isEmpty {
            let none = NSMenuItem(title: "  입력 장치 없음", action: nil, keyEquivalent: "")
            none.isEnabled = false
            menu.addItem(none)
        } else {
            for mic in mics {
                let item = NSMenuItem(title: "  " + mic.name, action: #selector(selectMic(_:)), keyEquivalent: "")
                item.target = self
                item.representedObject = mic.id
                item.state = (mic.id == selectedMicId) ? .on : .off
                menu.addItem(item)
            }
        }
        // Talkback mic channel. A talkback mic is one input channel (e.g. ch2 when ch1 is the
        // singer's mic). A ● marks channels with a live signal right now — so on a multi-input
        // interface the talkback mic is easy to spot. Activity only reads while the input is
        // flowing (Talk engaged or input monitoring on); idle channels show ○.
        menu.addItem(.separator())
        let chHeader = NSMenuItem(title: "톡백 채널 (마이크 입력)", action: nil, keyEquivalent: "")
        chHeader.isEnabled = false
        menu.addItem(chHeader)
        let chCount = max(1, channelCount)
        for ch in 1...chCount {
            let live = (channelActivity?(ch) ?? 0) > 0.003   // ~ -50 dBFS
            let dot = live ? "● " : "○ "
            let item = NSMenuItem(title: "  " + dot + "입력 \(ch)" + (live ? "  (라이브)" : ""),
                                  action: #selector(selectChannel(_:)), keyEquivalent: "")
            item.target = self
            item.tag = ch
            item.state = (ch == talkbackChannel) ? .on : .off
            menu.addItem(item)
        }

        // Talkback destination. Listen-room-only keeps the engineer's monitor dry (no
        // feedback on speakers); the mic rides over the programme to the remote listeners.
        menu.addItem(.separator())
        let routeHeader = NSMenuItem(title: "톡백 대상", action: nil, keyEquivalent: "")
        routeHeader.isEnabled = false
        menu.addItem(routeHeader)
        for route in [(id: "listen_room", name: "리슨룸 (원격 청취자)"),
                      (id: "monitor_bus", name: "모니터 (내 스피커)"),
                      (id: "all", name: "둘 다")] {
            let item = NSMenuItem(title: "  " + route.name, action: #selector(selectRoute(_:)), keyEquivalent: "")
            item.target = self
            item.representedObject = route.id
            item.state = (route.id == talkbackRoute) ? .on : .off
            menu.addItem(item)
        }
        NSMenu.popUpContextMenu(menu, with: event, for: self)
    }
    @objc private func selectMic(_ sender: NSMenuItem) {
        if let id = sender.representedObject as? String { onSelectMic?(id) }
    }
    @objc private func selectRoute(_ sender: NSMenuItem) {
        if let id = sender.representedObject as? String { onSelectRoute?(id) }
    }
    @objc private func selectChannel(_ sender: NSMenuItem) {
        onSelectChannel?(sender.tag)
    }
}

/// A model-selection request for the searchable picker sheet.
struct ModelPickerContext: Identifiable {
    let id = UUID()
    let title: String
    let catalog: [String]
    let selected: String
    // Models with a measured response curve (they drive the EQ); the rest fall back to the name
    // heuristic. Empty for catalogs where the distinction doesn't apply (amps, cables, headphones).
    var measured: Set<String> = []
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
                                HStack(spacing: 6) {
                                    Text(model)
                                        .font(Theme.Font.ui(11))
                                        .foregroundStyle(Theme.Palette.text)
                                    if !context.measured.isEmpty {
                                        let has = context.measured.contains(model)
                                        Text(has ? "측정" : "측정 없음")
                                            .font(Theme.Font.mono(7, .bold))
                                            .foregroundStyle(has ? Theme.Palette.green : Theme.Palette.textFaint)
                                            .padding(.horizontal, 5).padding(.vertical, 2)
                                            .background((has ? Theme.Palette.green : Theme.Palette.textFaint).opacity(0.13), in: Capsule())
                                    }
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


/// Leaf views over the meter store: the dock's phase rows and mini spectrum redraw at meter rate,
/// and observing the store HERE (not in MonitorDock) keeps the rest of the dock out of it.
private struct DockPhaseRows: View {
    @ObservedObject var meters: EngineController.EngineMeters

    var body: some View {
        StatRow(label: "위상 상관", value: String(format: "%+.2f", meters.phaseCorrelation))
        PhaseCorrelationDotMeter(correlation: meters.phaseCorrelation)
    }
}

private struct DockSpectrumMini: View {
    @ObservedObject var meters: EngineController.EngineMeters
    let sampleRate: Double

    var body: some View {
        SpectrumAnalyzerView(bins: meters.spectrumBins, sampleRate: sampleRate, compact: true)
    }
}
