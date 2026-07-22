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
                Text(TitleBar.buildStamp)
                    .font(Theme.Font.mono(9))
                    .foregroundStyle(Theme.Palette.textFainter)
                    .help("빌드 시각 (연월일.시분) — 실행 중인 바이너리")
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
                    // Collapse to the compact monitor-station shell (the engine keeps running).
                    Button { engine.collapseToMonitor() } label: {
                        HStack(spacing: 3) {
                            Image(systemName: "rectangle.compress.vertical")
                                .font(.system(size: 9, weight: .semibold))
                            Text("모니터만").font(Theme.Font.ui(9.5, .semibold))
                        }
                        .foregroundStyle(Theme.Palette.textSecondary)
                        .padding(.horizontal, 7).frame(height: 18)
                        .background(RoundedRectangle(cornerRadius: 5).fill(Theme.Palette.button))
                    }
                    .buttonStyle(.plain)
                    .help("모니터 스테이션만 남기고 DAW를 접습니다 (엔진·재생은 유지)")
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

    /// Build identifier shown in the titlebar: the running binary's own modification time as
    /// 연월일.시분 (e.g. "260715.1634"). Read from the executable so it always reflects the
    /// binary actually running — no build-system plumbing, no chance of a stale hardcoded value.
    static let buildStamp: String = {
        guard let url = Bundle.main.executableURL,
              let date = try? FileManager.default.attributesOfItem(atPath: url.path)[.modificationDate] as? Date
        else { return "" }
        let fmt = DateFormatter()
        fmt.dateFormat = "yyMMdd.HHmm"
        return fmt.string(from: date)
    }()

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
    @State private var barWidth: CGFloat = 0

    /// Below this the bar is too narrow to hold the position/time displays cleanly — hide them
    /// outright (the user asked for them to disappear, not shrink into "00…" / "S:FF…").
    private var showDisplays: Bool { barWidth == 0 || barWidth >= 1000 }

    var body: some View {
        HStack(spacing: Theme.Space.lg) {
            transportButtons
            separator
            toggles
            if showDisplays {
                separator
                // Displays + tempo on top, the thin meter row tucked underneath — so the meters cost
                // vertical space, not the ~310 px of width they used to take on the right.
                VStack(alignment: .leading, spacing: 4) {
                    HStack(spacing: Theme.Space.lg) {
                        displays
                        tempoPods
                    }
                    thinMeters
                }
            }
            Spacer(minLength: Theme.Space.xl)
            // Right toolbar cluster: the panel-toggle + help chips on top, the Edit/Mix tabs
            // directly underneath them — above the monitor station, never in the dock's column.
            // Clips off with the rest of the toolbar when the window narrows (the dock stays put).
            VStack(alignment: .leading, spacing: 4) {
                HStack(spacing: Theme.Space.lg) {
                    panelToggles
                    helpChip
                }
                ViewTabsControl()
            }
        }
        .padding(.horizontal, Theme.Space.xxl)
        .frame(height: 64)
        .frame(maxWidth: .infinity)
        .background(
            GeometryReader { geo in
                Color.clear
                    .onAppear { barWidth = geo.size.width }
                    .onChange(of: geo.size.width) { barWidth = geo.size.width }
            }
        )
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
            toggle("Loop", isOn: engine.loopEnabled, tint: Theme.Palette.green,
                   icon: "repeat") { engine.toggleLoop() }
            toggle("Click", isOn: engine.clickEnabled, tint: Theme.Palette.amber,
                   icon: "metronome") { engine.toggleClick() }
                // Right-click: click resolution + record count-in (Pro Tools / Cubase style).
                // Flat buttons with checkmarks — a Picker/submenu inside a macOS context menu
                // renders the arrow but never opens its contents.
                .contextMenu {
                    Text("장르 그루브 (강약)")
                    ForEach(EngineController.metronomeGenreCategories, id: \.self) { category in
                        Text("· \(category)")
                        ForEach(EngineController.metronomeGenres.filter { $0.category == category }, id: \.id) { genre in
                            Button { engine.setMetronomeGenre(genre.id) } label: {
                                Text(engine.metronomeGenre == genre.id ? "✓ \(genre.title)" : "    \(genre.title)")
                            }
                        }
                    }
                    Divider()
                    Text("분할 (클릭 해상도)")
                    subdivisionButton("자동", "auto")
                    subdivisionButton("♩ 4분", "quarter")
                    subdivisionButton("♪ 8분", "eighth")
                    subdivisionButton("♬ 16분", "sixteenth")
                    Divider()
                    Button { engine.setMetronomeAccentFirst(!engine.metronomeAccentFirst) } label: {
                        Text(engine.metronomeAccentFirst ? "✓ 첫 박 악센트" : "    첫 박 악센트")
                    }
                    Divider()
                    Text("사운드")
                    soundButton("비프", "beep")
                    soundButton("우드블록", "wood")
                    soundButton("림", "rim")
                    soundButton("카우벨", "cowbell")
                    Divider()
                    Text("볼륨")
                    gainButton("50%", 0.5)
                    gainButton("75%", 0.75)
                    gainButton("100%", 1.0)
                    gainButton("150%", 1.5)
                    gainButton("200%", 2.0)
                    Divider()
                    Text("스윙")
                    grooveButton("없음 (스트레이트)", "straight")
                    grooveButton("셔플", "shuffle")
                    grooveButton("트리플렛", "triplet")
                    // Swing amount as a percentage (50% = straight, ~66% ≈ triplet). Picking one
                    // sets the shuffle feel too, so it always works even from straight.
                    Text("셔플 스윙 양")
                    swingButton("52%", 0.52)
                    swingButton("56%", 0.56)
                    swingButton("60%", 0.60)
                    swingButton("64%", 0.64)
                    swingButton("68%", 0.68)
                    Divider()
                    Text("카운트인 (녹음 프리카운트)")
                    countInButton("없음", 0)
                    countInButton("1마디", 1)
                    countInButton("2마디", 2)
                }
            // Shuffle / Slip / Spot / Grid are a separate group (edit modes), so a divider.
            Rectangle().fill(Theme.Palette.divider).frame(width: 1, height: 18)
            editModePicker
        }
        // Never let the transport row's crowding squeeze the metronome out of view.
        .fixedSize()
    }

    // A text checkmark, not Label(systemImage:) — the SF Symbol replays its scale-in transition
    // every time the menu re-renders (which the ~30 Hz engine publishes trigger), so the check
    // visibly pulses. Plain text renders identically each pass and stays still.
    private func subdivisionButton(_ label: String, _ value: String) -> some View {
        Button { engine.setMetronomeSubdivision(value) } label: {
            Text(engine.metronomeSubdivision == value ? "✓ \(label)" : "    \(label)")
        }
    }

    private func countInButton(_ label: String, _ value: Int) -> some View {
        Button { engine.setCountInBars(value) } label: {
            Text(engine.countInBars == value ? "✓ \(label)" : "    \(label)")
        }
    }

    private func soundButton(_ label: String, _ value: String) -> some View {
        Button { engine.setMetronomeSound(value) } label: {
            Text(engine.metronomeSound == value ? "✓ \(label)" : "    \(label)")
        }
    }

    private func gainButton(_ label: String, _ value: Double) -> some View {
        Button { engine.setMetronomeGain(value) } label: {
            Text(abs(engine.metronomeGain - value) < 0.001 ? "✓ \(label)" : "    \(label)")
        }
    }

    private func grooveButton(_ label: String, _ value: String) -> some View {
        Button { engine.setGroove(feel: value) } label: {
            Text(engine.grooveFeel == value ? "✓ \(label)" : "    \(label)")
        }
    }

    private func swingButton(_ label: String, _ value: Double) -> some View {
        let active = engine.grooveFeel == "shuffle" && abs(engine.grooveSwingAmount - value) < 0.01
        return Button { engine.setGroove(feel: "shuffle", swingAmount: value) } label: {
            Text(active ? "✓ \(label)" : "    \(label)")
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
                        icon: String? = nil,
                        action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack(spacing: 4) {
                if let icon {
                    Image(systemName: icon).font(.system(size: 10, weight: .medium))
                }
                Text(title)
                    .font(Theme.Font.ui(9, .medium))
            }
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
        // Never let the clock digits truncate to "00:0…" when the window is tight — the flexible
        // spacers/meters around them yield first, and the hScrollWhenNarrow wrapper scrolls if even
        // that is not enough.
        .fixedSize(horizontal: true, vertical: false)
    }

    private var divider: some View {
        Text("|").font(Theme.Font.mono(14)).foregroundStyle(Theme.Palette.textFainter)
    }

    private func numeric(_ text: String) -> some View {
        Text(text)
            .font(Theme.Font.mono(20, .semibold))
            .monospacedDigit()   // fixed-width digits so the display never reflows (no left/right shake during playback)
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

    /// One thin row holding all the meters — audio/MIDI input + master L/R + the dB read — so they can
    /// tuck UNDER the transport displays instead of eating ~310 px of bar width to their right. Frees
    /// that width for the tabs, so narrowing the window no longer pushes them off (user's idea).
    private var thinMeters: some View {
        HStack(spacing: Theme.Space.md) {
            compactMeter("오디오 입력", meterFraction(engine.inputPeak), Theme.Palette.green)
            compactMeter("미디 입력", Double(engine.midiActivity), Theme.Palette.purple)
            compactMeter("L", meterFraction(engine.outputPeakLeft), Theme.Palette.yellow)
            compactMeter("R", meterFraction(engine.outputPeakRight), Theme.Palette.yellow)
            Text(dbLabel)
                .font(Theme.Font.mono(8, .semibold))
                .foregroundStyle(Theme.Palette.yellow)
                .frame(width: 30, alignment: .trailing)
        }
    }

    private func compactMeter(_ label: String, _ fraction: Double, _ tint: Color) -> some View {
        HStack(spacing: 3) {
            Text(label)
                .font(Theme.Font.mono(7))
                .foregroundStyle(Theme.Palette.textFaint)
                .fixedSize()   // grow to fit the full label — there is horizontal room now
            GeometryReader { geo in
                ZStack(alignment: .leading) {
                    RoundedRectangle(cornerRadius: Theme.Radius.meterCell).fill(Theme.Palette.recess)
                    RoundedRectangle(cornerRadius: Theme.Radius.meterCell).fill(tint)
                        .mask(alignment: .leading) {
                            Rectangle().frame(width: geo.size.width * max(0, min(1, fraction)))
                        }
                }
            }
            .frame(width: 54, height: 5)
        }
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

}

/// Edit / Mix view switcher. Lives in the transport bar normally, but rides at the TOP of the monitor
/// dock when the dock is shown — the dock is always on-screen (never clipped by narrowing), so the tabs
/// stay reachable at any window width. Falls back to the transport bar only when the dock is hidden.
struct ViewTabsControl: View {
    @EnvironmentObject private var engine: EngineController

    var body: some View {
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
