import SwiftUI

struct TitleBar: View {
    @EnvironmentObject private var engine: EngineController

    var body: some View {
        // The file name is centred on the top (traffic-light) line — the macOS title
        // position — with the engine status on the right; a slim bar reclaims the band.
        ZStack {
            HStack(spacing: Theme.Space.md) {
                Text("Neuracoust DAW")
                    .font(Theme.Font.ui(10.5, .semibold))
                    .foregroundStyle(Theme.Palette.textMuted)
                Text("· \(documentLabel)")
                    .font(Theme.Font.ui(10.5))
                    .foregroundStyle(Theme.Palette.text)
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
                    if engine.running {
                        deviceMenu
                    } else {
                        Text("엔진 정지")
                            .font(Theme.Font.mono(9))
                            .foregroundStyle(Theme.Palette.textFaint)
                    }
                }
                .padding(.trailing, Theme.Space.xxl)
            }
        }
        .frame(height: 26)
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

    /// The device readout doubles as the buffer-size (latency) picker: smaller buffer =
    /// lower latency, more CPU / dropout risk. The label shows the granted buffer and its
    /// one-buffer latency in ms.
    private var deviceMenu: some View {
        Menu {
            Section("버퍼 크기 — 작을수록 저지연 (CPU·드롭 위험↑)") {
                ForEach(EngineController.bufferSizeChoices, id: \.self) { size in
                    Button {
                        engine.setBufferSize(size)
                    } label: {
                        let text = "\(size) samples · \(String(format: "%.1f", engine.bufferLatencyMs(size))) ms"
                        if size == engine.requestedBufferSize {
                            Label(text, systemImage: "checkmark")
                        } else {
                            Text(text)
                        }
                    }
                }
            }
        } label: {
            Text("\(engine.deviceName) · \(formatSampleRate(engine.sampleRate)) · \(engine.bufferSize) (\(String(format: "%.1f", engine.bufferLatencyMs(engine.bufferSize)))ms)")
                .font(Theme.Font.mono(9))
                .foregroundStyle(Theme.Palette.textFaint)
        }
        .menuStyle(.borderlessButton).menuIndicator(.hidden).fixedSize()
    }
}

struct TransportBar: View {
    @EnvironmentObject private var engine: EngineController
    @State private var editingPod: String?
    @State private var podDraft = ""

    var body: some View {
        HStack(spacing: Theme.Space.lg) {
            transportButtons
            separator
            toggles
            separator
            displays
            tempoPods
            Spacer(minLength: Theme.Space.xl)
            inputMeters
            masterMeter
            panelToggles
            viewTabs
            helpChip
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
            // A green PLAY key, Sony-recorder style: lit bright green while playing, an
            // unlit dark-green key when stopped (still clearly the green button).
            transportKey(engine.transportRunning ? "pause.fill" : "play.fill",
                         tint: engine.transportRunning ? Color.black.opacity(0.85) : Theme.Palette.green,
                         keyFill: engine.transportRunning ? Theme.Palette.green : Theme.Palette.green.opacity(0.16),
                         badge: engine.loopEnabled ? ("repeat", engine.transportRunning ? Theme.Palette.green : Theme.Palette.green) : nil) {
                engine.togglePlay()
            }
            // Right-click for the playback mode, the way Pro Tools does. Loop playback
            // is the loop toggle, so the two always agree.
            .contextMenu {
                Text("재생 모드").font(.caption)
                transportModeItem("일반 재생", selected: !engine.loopEnabled) { engine.setLoop(false) }
                transportModeItem("루프 재생", selected: engine.loopEnabled) { engine.setLoop(true) }
            }
            transportKey("stop.fill") { engine.stop() }
                .contextMenu {
                    Text("정지 위치")
                    Picker("정지 위치", selection: Binding(
                        get: { engine.stopBehavior },
                        set: { engine.stopBehavior = $0 }
                    )) {
                        ForEach(EngineController.StopBehavior.allCases) { Text($0.label).tag($0) }
                    }
                    Divider()
                    Text("정지 후 인서트 DSP (Pro Tools HD 방식)")
                    Picker("인서트 DSP", selection: Binding(
                        get: { engine.insertTailOnStopSeconds },
                        set: { engine.setInsertTailOnStopSeconds($0) }
                    )) {
                        Text("항상 켜짐 (고정 · DSP 계속 구동)").tag(-1.0)
                        Text("끔 (즉시 컷)").tag(0.0)
                        Text("2초 링아웃").tag(2.0)
                        Text("5초 링아웃").tag(5.0)
                        Text("10초 링아웃").tag(10.0)
                    }
                }
            // Not a take recorder yet: it arms the input monitor path. Drawn hollow so
            // it does not read as a transport that captures audio. Right-click picks the
            // record mode the capture engine will use once it exists.
            // A red RECORD key: lit bright red while armed, an unlit dark-red key otherwise.
            transportKey("circle",
                         tint: engine.recording ? Color.black.opacity(0.85) : Theme.Palette.red,
                         keyFill: engine.recording ? Theme.Palette.red : Theme.Palette.red.opacity(0.16),
                         badge: recordBadge) {
                engine.toggleRecording()
            }
            .help("입력 모니터 경로 (녹음 아님) · 우클릭으로 레코드 모드")
            .contextMenu {
                Text("레코드 모드").font(.caption)
                ForEach(EngineController.RecordMode.allCases) { mode in
                    transportModeItem(mode.label, selected: engine.recordMode == mode) {
                        engine.setRecordMode(mode)
                    }
                }
                Divider()
                Text("펀치/루프 범위는 루프 구간을 사용합니다")
                Text("입력 캡처는 아직 구현되지 않았습니다 — 모드만 설정됩니다")
            }
        }
    }

    /// A flat context-menu item that shows a checkmark when selected — the Pro Tools
    /// layout, rather than a nested submenu.
    @ViewBuilder
    private func transportModeItem(_ title: String, selected: Bool,
                                   _ action: @escaping () -> Void) -> some View {
        Button(action: action) {
            if selected {
                Label(title, systemImage: "checkmark")
            } else {
                Text(title)
            }
        }
    }

    private func transportKey(_ symbol: String,
                              tint: Color = Theme.Palette.textSecondary,
                              keyFill: Color? = nil,
                              badge: (symbol: String, tint: Color)? = nil,
                              action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol)
                .font(.system(size: 10))
                .foregroundStyle(tint)
                .frame(width: 28, height: 24)
                .background(
                    RoundedRectangle(cornerRadius: Theme.Radius.button)
                        .fill(keyFill ?? Theme.Palette.button)
                        .overlay(
                            RoundedRectangle(cornerRadius: Theme.Radius.button)
                                .stroke(keyFill != nil ? Color.clear : Theme.Palette.border, lineWidth: 1)
                        )
                )
                // The mode badge rides the bottom-trailing corner, exactly as the
                // "Transport Mode Icons" design spec draws it.
                .overlay(alignment: .bottomTrailing) {
                    if let badge {
                        Image(systemName: badge.symbol)
                            .font(.system(size: 6, weight: .bold))
                            .foregroundStyle(badge.tint)
                            .frame(width: 9, height: 9)
                            .background(
                                RoundedRectangle(cornerRadius: 2.5)
                                    .fill(Theme.Palette.recess)
                                    .overlay(
                                        RoundedRectangle(cornerRadius: 2.5)
                                            .stroke(Color.black.opacity(0.55), lineWidth: 0.5)
                                    )
                            )
                            .offset(x: 2, y: 1)
                    }
                }
        }
        .buttonStyle(.plain)
    }

    /// The record button's corner badge, keyed to the staged record mode — the
    /// design's 새 테이크 / 루프 / 펀치 glyphs.
    private var recordBadge: (symbol: String, tint: Color)? {
        switch engine.recordMode {
        case .newTake: return nil
        case .loop:    return ("repeat", Theme.Palette.red)
        case .punch:   return ("arrowtriangle.down.fill", Theme.Palette.red)
        }
    }

    private var toggles: some View {
        HStack(spacing: Theme.Space.sm) {
            toggle("Loop", isOn: engine.loopEnabled, tint: Theme.Palette.green) { engine.toggleLoop() }
            toggle("Click", isOn: engine.clickEnabled, tint: Theme.Palette.amber) { engine.toggleClick() }
            // Shuffle / Slip / Spot / Grid are a separate group (edit modes), so a divider.
            Rectangle().fill(Theme.Palette.divider).frame(width: 1, height: 18)
            editModePicker
        }
    }

    /// Pro Tools edit modes replace the old Snap on/off: Grid snaps, Slip is free,
    /// Shuffle ripples, Spot places by typed time.
    private var editModePicker: some View {
        HStack(spacing: Theme.Space.sm) {
            ForEach(EngineController.EditMode.allCases) { mode in
                let active = engine.editMode == mode
                Button { engine.editMode = mode } label: {
                    Text(mode.label)
                        .font(Theme.Font.ui(9, .medium))
                        .foregroundStyle(active ? Theme.Palette.accent : Theme.Palette.textFaint)
                        .padding(.horizontal, 9)
                        .frame(height: 24)
                        .background(
                            RoundedRectangle(cornerRadius: Theme.Radius.button)
                                .fill(active ? Theme.Palette.accent.opacity(0.14) : Theme.Palette.button)
                                .overlay(
                                    RoundedRectangle(cornerRadius: Theme.Radius.button)
                                        .stroke(active ? Theme.Palette.accent.opacity(0.5) : Theme.Palette.border, lineWidth: 1)
                                )
                                .shadow(color: .black.opacity(0.3), radius: 1.5, y: 1)
                        )
                }
                .buttonStyle(.plain)
                .help("\(mode.label) 편집 모드")
            }
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
                        .shadow(color: .black.opacity(0.3), radius: 1.5, y: 1)
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
            editablePod(String(format: "%.1f", Double(engine.tempoBpm)), "TEMPO") {
                if let bpm = Double($0) { engine.setBaseTempo(Int(bpm.rounded())) }
            }
            editablePod("\(engine.timeSignature.numerator)/\(engine.timeSignature.denominator)", "SIG") {
                engine.setBaseTimeSignature($0)
            }
        }
        .padding(.leading, Theme.Space.lg)
    }

    /// TEMPO and SIG are editable: click to type a new value. Committing sets the base and its
    /// t=0 conductor anchor, so the transport and the 템포 / 박자 lanes stay in sync.
    private func editablePod(_ value: String, _ label: String, commit: @escaping (String) -> Void) -> some View {
        pod(value, label)
            .contentShape(Rectangle())
            .onTapGesture { podDraft = value; editingPod = label }
            .popover(isPresented: Binding(get: { editingPod == label },
                                          set: { if !$0 { editingPod = nil } })) {
                VStack(spacing: 8) {
                    Text(label).font(Theme.Font.mono(8)).tracking(0.6).foregroundStyle(Theme.Palette.textFaint)
                    TextField(label, text: $podDraft)
                        .textFieldStyle(.roundedBorder).frame(width: 90)
                        .font(Theme.Font.mono(13, .semibold))
                        .onSubmit { commit(podDraft); editingPod = nil }
                    Text(label == "TEMPO" ? "BPM (예: 128)" : "박자 (예: 3/4)")
                        .font(Theme.Font.mono(7)).foregroundStyle(Theme.Palette.textFaint)
                }
                .padding(14)
            }
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

    /// Incoming audio-interface input and live MIDI-input activity, beside the master out.
    private var inputMeters: some View {
        VStack(alignment: .trailing, spacing: 2) {
            Text("입력 미터")
                .font(Theme.Font.mono(6.5))
                .tracking(0.6)
                .foregroundStyle(Theme.Palette.textFaint)
            // Channel-independent input activity: audio interface in, and live MIDI in.
            meterLabelRow("오디오", meterFraction(engine.inputPeak), Theme.Palette.green)
            meterLabelRow("미디", Double(engine.midiActivity), Theme.Palette.purple)
        }
        .frame(width: 110)
    }

    private func meterLabelRow(_ label: String, _ fraction: Double, _ tint: Color) -> some View {
        HStack(spacing: Theme.Space.sm) {
            Text(label)
                .font(Theme.Font.mono(6.5))
                .tracking(0.4)
                .foregroundStyle(Theme.Palette.textFaint)
                .frame(width: 30, alignment: .trailing)
            GeometryReader { geo in
                ZStack(alignment: .leading) {
                    RoundedRectangle(cornerRadius: Theme.Radius.meterCell)
                        .fill(Theme.Palette.recess)
                    RoundedRectangle(cornerRadius: Theme.Radius.meterCell)
                        .fill(tint)
                        .mask(alignment: .leading) {
                            Rectangle().frame(width: geo.size.width * max(0, min(1, fraction)))
                        }
                }
            }
            .frame(height: 5)
        }
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

    /// Icon toggles for the side panels. Channel and Inspector are Edit-view only (they
    /// don't exist in the Mix tab); the Monitor dock shows in both, so its toggle stays.
    private var panelToggles: some View {
        HStack(spacing: 2) {
            if engine.viewTab == .edit {
                panelChip("slider.vertical.3", "Channel", on: engine.showChannelColumn) { engine.showChannelColumn.toggle() }
                panelChip("info.circle", "Inspector", on: engine.showInspector) { engine.showInspector.toggle() }
            }
            panelChip("hifispeaker.2.fill", "Monitor", on: engine.showMonitorDock) { engine.showMonitorDock.toggle() }
        }
        .padding(3)
        .background(RoundedRectangle(cornerRadius: Theme.Radius.panel).fill(Theme.Palette.surface))
    }

    private func panelChip(_ symbol: String, _ label: String, on: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol)
                .font(.system(size: 12, weight: .medium))
                .foregroundStyle(on ? Theme.Palette.accent : Theme.Palette.textDim)
                .frame(width: 28, height: 24)
                .background(
                    RoundedRectangle(cornerRadius: Theme.Radius.button)
                        .fill(on ? Color(hex: 0x20282e) : .clear)
                        .overlay(RoundedRectangle(cornerRadius: Theme.Radius.button)
                            .stroke(on ? Color(hex: 0x2c4657) : .clear, lineWidth: 1))
                )
        }
        .buttonStyle(.plain)
        .help("\(label) 패널 표시/숨김")
    }

    /// Help toggle: while lit, every icon control shows a hover tooltip explaining it.
    private var helpChip: some View {
        Button { engine.helpMode.toggle() } label: {
            Image(systemName: "questionmark")
                .font(.system(size: 12, weight: .bold))
                .foregroundStyle(engine.helpMode ? Theme.Palette.deepBorder : Theme.Palette.textDim)
                .frame(width: 28, height: 24)
                .background(
                    RoundedRectangle(cornerRadius: Theme.Radius.button)
                        .fill(engine.helpMode ? Theme.Palette.accent : .clear)
                )
        }
        .buttonStyle(.plain)
        .padding(3)
        .background(RoundedRectangle(cornerRadius: Theme.Radius.panel).fill(Theme.Palette.surface))
        .help(engine.helpMode ? engine.tr("help.mode_off") : engine.tr("help.mode_on"))
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

            // Insert-DSP-on-stop mode (right-click Stop to change).
            stat("INS DSP", engine.insertTailOnStopSeconds < 0 ? "항상"
                          : engine.insertTailOnStopSeconds == 0 ? "컷"
                          : String(format: "%.0f초", engine.insertTailOnStopSeconds))

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
