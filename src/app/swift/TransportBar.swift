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
    // The playhead moved off the engine object; observe it here so the timecode / bars-beats readout
    // still updates during playback (without re-rendering the heavy dock, which no longer sees it).
    /// NOT @ObservedObject: the clock publishes 30x/s during playback, and observing it here
    /// re-evaluated and re-laid-out the entire transport bar every frame. Only the two views
    /// that actually move — the numerals and the top playhead line — observe it, below.
    let clock: PlayheadClock
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
                    HStack(alignment: .top, spacing: Theme.Space.lg) {
                        displays
                        VStack(alignment: .leading, spacing: 5) {
                            tempoPods
                            beatDots
                        }
                    }
                }
            }
            Spacer(minLength: Theme.Space.xl)
            // Right toolbar cluster: the panel-toggle + help chips on top, the Edit/Mix tabs
            // directly underneath them — above the monitor station, never in the dock's column.
            // Clips off with the rest of the toolbar when the window narrows (the dock stays put).
            // No Edit/Mix tabs: Pro Tools has no on-screen pair either — ⌘= toggles the two, and
            // that shortcut already works here. Dropping them returns the width to the toolbar.
            HStack(spacing: Theme.Space.lg) {
                panelToggles
                helpChip
            }
        }
        .padding(.horizontal, Theme.Space.xxl)
        .frame(height: 72)
        .frame(maxWidth: .infinity)
        // The design's one moving element outside the numerals: a 2 pt line across the top of the
        // bar showing where the playhead sits inside the visible timeline.
        .overlay(alignment: .top) {
            TransportPlayheadLine(clock: clock)
        }
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
            // The small lower keys make each transport mode visible and editable without
            // requiring the user to discover a right-click menu (Harrison-style mode shelf).
            VStack(spacing: 2) {
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
            }
            VStack(spacing: 2) {
                // A green PLAY key, Sony-recorder style: lit bright green while playing, an
                // unlit dark-green key when stopped (still clearly the green button).
                transportKey(engine.transportRunning ? "pause.fill" : "play.fill",
                             tint: engine.transportRunning ? Color.black.opacity(0.85) : Theme.Palette.green,
                             keyFill: engine.transportRunning ? Theme.Palette.green : Theme.Palette.green.opacity(0.16),
                             badge: engine.loopEnabled ? ("repeat", Theme.Palette.green) : nil) {
                    engine.togglePlay()
                }
                // Right-click remains as a fast expert path; the lower mode shelf is the
                // discoverable path and always shows the current state.
                .contextMenu {
                    Text("재생 모드").font(.caption)
                    transportModeItem("일반 재생", selected: !engine.loopEnabled) { engine.setLoop(false) }
                    transportModeItem("루프 재생", selected: engine.loopEnabled) { engine.setLoop(true) }
                    Divider()
                    Button("프리/포스트롤 설정…") { engine.presentLoopRollSettings() }
                    Text(String(format: "Pre %.3fs · Post %.3fs",
                                engine.preRollSeconds, engine.postRollSeconds))
                }
            }
            VStack(spacing: 2) {
                // A red RECORD key: lit bright red while recording, dark red otherwise.
                transportKey("circle",
                             tint: engine.recording ? Color.black.opacity(0.85) : Theme.Palette.red,
                             keyFill: engine.recording ? Theme.Palette.red : Theme.Palette.red.opacity(0.16),
                             badge: recordBadge) {
                    engine.toggleRecording()
                }
                .help("녹음 · 우클릭 또는 아래 버튼으로 레코드 모드 선택")
                .contextMenu {
                    recordModeMenuItems
                }
            }
        }
    }

    private var recordModeShortLabel: String {   // now an SF Symbol name (icon-only shelf)
        switch engine.recordMode {
        case .newTake: return "circle.fill"
        case .loop: return "repeat"
        case .punch: return "arrowtriangle.down.fill"
        case .punchLoop: return "repeat.circle.fill"
        }
    }

    @ViewBuilder
    private var recordModeMenuItems: some View {
        Text("레코드 모드").font(.caption)
        ForEach(EngineController.RecordMode.allCases) { mode in
            transportModeItem(mode.label, selected: engine.recordMode == mode) {
                engine.setRecordMode(mode)
            }
        }
        Divider()
        Menu("기록할 컨트롤러") {
            // A keyboard sends far more than a part needs. Only what is ticked here
            // is written into the take; the rest is still heard while playing.
            Button {
                engine.setRecordPitchBendEnabled(!engine.recordPitchBendEnabled)
            } label: {
                Label("피치벤드", systemImage: engine.recordPitchBendEnabled ? "checkmark" : "")
            }
            ForEach(EngineController.recordableControllers, id: \.number) { controller in
                Button {
                    engine.setRecordControllerEnabled(controller.number,
                                                      !engine.recordControllerEnabled(controller.number))
                } label: {
                    Label(controller.label,
                          systemImage: engine.recordControllerEnabled(controller.number) ? "checkmark" : "")
                }
            }
        }
        Divider()
        Text("펀치/루프 범위는 루프 구간을 사용합니다")
    }

    private func transportOptionButton(_ systemImage: String, selected: Bool, tint: Color,
                                       action: @escaping () -> Void) -> some View {
        Button(action: action) {
            transportOptionLabel(systemImage, selected: selected, tint: tint)
        }
        .buttonStyle(.plain)
    }

    /// The small mode shelf under the transport keys — icon-only (no text), the way the user asked.
    private func transportOptionLabel(_ systemImage: String, selected: Bool, tint: Color) -> some View {
        Image(systemName: systemImage)
            .font(.system(size: 7.5, weight: .bold))
            .foregroundStyle(selected ? Color.black.opacity(0.82) : Theme.Palette.textFaint)
            .frame(width: 28, height: 11)
            .background(
                RoundedRectangle(cornerRadius: 2.5)
                    .fill(selected ? tint : Theme.Palette.recess)
                    .overlay(RoundedRectangle(cornerRadius: 2.5)
                        .stroke(Theme.Palette.border, lineWidth: 0.5))
            )
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
            // Flat key, per the design: a square face with a 3 pt corner, no bevel, no outline.
            // State is the FILL — an engaged key inverts to a solid face with a dark glyph, rather
            // than tinting an outline. Bigger than the old 28x24 now that the meters left the bar.
            Image(systemName: symbol)
                .font(.system(size: 13))
                .foregroundStyle(keyFill != nil ? Theme.Palette.background : tint)
                .frame(width: 34, height: 30)
                .background(
                    RoundedRectangle(cornerRadius: 3)
                        .fill(keyFill ?? Theme.Palette.keyFace)
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
        case .newTake:   return nil
        case .loop:      return ("repeat", Theme.Palette.red)
        case .punch:     return ("arrowtriangle.down.fill", Theme.Palette.red)
        case .punchLoop: return ("repeat.circle.fill", Theme.Palette.red)
        }
    }

    private var toggles: some View {
        HStack(spacing: Theme.Space.sm) {
            toggle("Loop", isOn: engine.loopEnabled, tint: Theme.Palette.green,
                   icon: "repeat") { engine.toggleLoop() }
            rollControl("Pre", seconds: engine.preRollSeconds, tint: Theme.Palette.green) {
                engine.setLoopRoll(pre: $0, post: engine.postRollSeconds)
            }
            rollControl("Post", seconds: engine.postRollSeconds, tint: Theme.Palette.green) {
                engine.setLoopRoll(pre: engine.preRollSeconds, post: $0)
            }
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
                    Divider()
                    Text("오디오로 프린트")
                    Button("전체 세션을 새 Metronome 트랙으로") {
                        engine.printMetronomeToTrack(loopRangeOnly: false)
                    }
                    Button("루프/편집 범위를 새 Metronome 트랙으로") {
                        engine.printMetronomeToTrack(loopRangeOnly: true)
                    }
                    .disabled(!engine.loopEnabled || engine.loopEndSeconds <= engine.loopStartSeconds)
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

    /// Always-visible Pro Tools-style pre/post-roll control: the key enables/disables it and the
    /// adjacent numeric field edits seconds directly. Zero means off; enabling starts at one second.
    private func rollControl(_ title: String, seconds: Double, tint: Color,
                             set: @escaping (Double) -> Void) -> some View {
        HStack(spacing: 2) {
            // Flat label, uppercase mono, no box — the design writes PRE / POST as type and lets
            // the value beside it carry the weight.
            Button(title.uppercased()) { set(seconds > 0 ? 0 : 1.0) }
                .font(Theme.Font.mono(8, .semibold))
                .foregroundStyle(seconds > 0 ? tint : Theme.Palette.textFainter)
                .fixedSize()          // "POST" wrapped to two lines inside a 26 pt box
                .buttonStyle(.plain)
            TextField("", text: Binding(
                get: { String(format: "%.2f", seconds) },
                set: { if let value = Double($0.replacingOccurrences(of: ",", with: ".")) {
                    set(min(3600, max(0, value)))
                }}
            ))
            .textFieldStyle(.plain)
            .font(Theme.Font.mono(8))
            .multilineTextAlignment(.trailing)
            .frame(width: 34, height: 18)
            .padding(.horizontal, 3)
            .background(RoundedRectangle(cornerRadius: 3).fill(Theme.Palette.recess))
            Text("s")
                .font(Theme.Font.mono(7))
                .foregroundStyle(Theme.Palette.textSecondary.opacity(0.7))
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
        HStack(spacing: 16) {
            ForEach(EngineController.EditMode.allCases) { mode in
                let active = engine.editMode == mode
                Button { engine.editMode = mode } label: {
                    // Underlined type, not a pill — same rule as the Edit/Mix tabs, so the two
                    // rows of mode choices read as one family (the design draws them stacked).
                    Text(mode.label)
                        .font(Theme.Font.ui(11, .medium))
                        .foregroundStyle(active ? Theme.Palette.accent : Theme.Palette.textFaint)
                        .padding(.top, 2)
                        .padding(.bottom, 5)
                        .padding(.horizontal, 2)
                        .overlay(alignment: .bottom) {
                            Rectangle()
                                .fill(active ? Theme.Palette.accent : .clear)
                                .frame(height: 2)
                        }
                        .contentShape(Rectangle())
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
            // Same flat key as the transport buttons: a square face, engaged = filled with the
            // tint and the glyph inverted. Icon only when there is one — the design leans on the
            // shape, not a word, for Loop and Click.
            Group {
                if let icon {
                    Image(systemName: icon).font(.system(size: 13, weight: .medium))
                } else {
                    Text(title).font(Theme.Font.ui(10, .medium))
                }
            }
                .foregroundStyle(isOn ? Theme.Palette.background : Theme.Palette.textFaint)
                .frame(width: icon == nil ? 44 : 34, height: 30)
                .background(RoundedRectangle(cornerRadius: 3).fill(isOn ? tint : Theme.Palette.keyFace))
                .help(title)
        }
        .buttonStyle(.plain)
    }

    /// Type-led, after the transport-bar design: the timecode is the one large element and
    /// everything else steps down from it in size and brightness. No panel behind them — the
    /// design drops the recessed boxes and lets the numerals carry the block on their own.
    private var displays: some View {
        TransportNumeralsDisplay(clock: clock)
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

    /// Four dots under the tempo, one lit on the current beat — the design's way of showing the
    /// meter without a second numeric readout.

    private var beatDots: some View {
        let beat = engine.barsBeats.beat
        return HStack(spacing: 4) {
            ForEach(1...max(1, engine.timeSignature.numerator), id: \.self) { i in
                Rectangle()
                    .fill(engine.transportRunning && i == beat ? Theme.Palette.accent
                                                               : Theme.Palette.divider)
                    .frame(width: 13, height: 3)
            }
        }
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
        HStack(spacing: 16) {
            ForEach(EngineController.ViewTab.allCases) { tab in
                Button {
                    engine.viewTab = tab
                } label: {
                    // Underlined type instead of a pill, per the transport-bar design: the active
                    // tab is marked by a 2 pt rule under it, not by a filled capsule.
                    Text(tab.rawValue.uppercased())
                        .font(Theme.Font.mono(11, .medium))
                        .tracking(1.6)
                        .foregroundStyle(engine.viewTab == tab ? Theme.Palette.text : Theme.Palette.textDim)
                        .padding(.top, 2)
                        .padding(.bottom, 5)
                        .padding(.horizontal, 2)
                        .overlay(alignment: .bottom) {
                            Rectangle()
                                .fill(engine.viewTab == tab ? Theme.Palette.text : .clear)
                                .frame(height: 2)
                        }
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
            }
        }
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

/// The level meters, as a block the monitor station owns rather than the transport bar.
///
/// Two captioned groups because they are two different measurements: 입력 (IN, MIDI) are activity
/// indicators at the interface, and 컨트롤룸 (OUT, a stacked L/R pair) is the monitor bus read
/// BEFORE its level — so solo and mono/stereo move it while the monitor knob does not. They live
/// here, beside the monitor controls that shape them, and the transport bar gets its width back.
/// The transport's moving numerals, split out so the 30 Hz playhead clock re-renders THIS and
/// nothing else. Values still come off the engine (timecode/barsBeats are computed from the
/// same playhead the clock mirrors); the clock is purely the invalidation signal.
private struct TransportNumeralsDisplay: View {
    @EnvironmentObject private var engine: EngineController
    @ObservedObject var clock: PlayheadClock

    var body: some View {
        let position = engine.barsBeats
        return VStack(alignment: .leading, spacing: 2) {
            Text(engine.timecode)
                .font(Theme.Font.mono(30, .medium))
                .monospacedDigit()
                .foregroundStyle(Theme.Palette.text)
                .fixedSize()
            HStack(alignment: .firstTextBaseline, spacing: 9) {
                Text(String(format: "%03d.%02d.%03d", position.bar, position.beat, position.tick))
                    .font(Theme.Font.mono(13, .medium))
                    .monospacedDigit()
                    .foregroundStyle(Theme.Palette.accent)
                Text("BARS·BEATS")
                    .font(Theme.Font.mono(6.5)).tracking(0.6)
                    .foregroundStyle(Theme.Palette.textFaint)
                Text("25 FPS")
                    .font(Theme.Font.mono(6.5)).tracking(0.6)
                    .foregroundStyle(Theme.Palette.textFaint)
            }
        }
        // Never let the clock digits truncate to "00:0…" when the window is tight — the flexible
        // spacers/meters around them yield first, and the hScrollWhenNarrow wrapper scrolls if even
        // that is not enough.
        .fixedSize(horizontal: true, vertical: false)
    }
}

/// The 2 pt playhead-position line across the top of the bar — the other clock consumer.
private struct TransportPlayheadLine: View {
    @EnvironmentObject private var engine: EngineController
    @ObservedObject var clock: PlayheadClock

    var body: some View {
        let span = max(0.0001, engine.visibleDuration)
        let fraction = CGFloat(min(1, max(0, (clock.seconds - engine.visibleStart) / span)))
        return GeometryReader { geo in
            Rectangle()
                .fill(Theme.Palette.accent)
                .frame(width: geo.size.width * fraction, height: 2)
        }
        .frame(height: 2)
    }
}

struct ControlRoomMeters: View {
    /// The meter store, not the controller: these bars redraw ~15x/s with signal, and observing
    /// the controller would drag every transport re-render along with them.
    @ObservedObject var meters: EngineController.EngineMeters

    var body: some View {
        VStack(alignment: .leading, spacing: 7) {
            group("입력") {
                // Stereo: the audio input is a pair, and one combined bar could not tell a dead
                // right channel from a quiet take. MIDI stays a single activity bar.
                HStack(spacing: 5) {
                    label("IN")
                    VStack(spacing: 2) {
                        bar(meterFraction(meters.inputPeakLeft), Theme.Palette.green)
                        bar(meterFraction(meters.inputPeakRight), Theme.Palette.green)
                    }
                    Spacer(minLength: 32)
                }
                row("MIDI", Double(meters.midiActivity), Theme.Palette.purple)
            }
            group("컨트롤룸") {
                HStack(spacing: 5) {
                    label("OUT")
                    VStack(spacing: 2) {
                        bar(meterFraction(meters.monitorPrePeakLeft), Theme.Palette.yellow)
                        bar(meterFraction(meters.monitorPrePeakRight), Theme.Palette.yellow)
                    }
                    Text(dbLabel)
                        .font(Theme.Font.mono(8, .semibold))
                        .foregroundStyle(Theme.Palette.yellow)
                        .frame(width: 32, alignment: .trailing)
                }
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private var dbLabel: String {
        let peak = max(meters.monitorPrePeakLeft, meters.monitorPrePeakRight)
        return peak <= 0.00001 ? "-∞" : String(format: "%.1f", 20 * log10(Double(peak)))
    }

    @ViewBuilder private func group<Content: View>(_ title: String,
                                                  @ViewBuilder _ rows: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(title)
                .font(Theme.Font.mono(7))
                .foregroundStyle(Theme.Palette.textFainter)
            rows()
        }
    }

    private func row(_ name: String, _ fraction: Double, _ tint: Color) -> some View {
        HStack(spacing: 5) {
            label(name)
            bar(fraction, tint)
            Spacer(minLength: 32)
        }
    }

    private func label(_ text: String) -> some View {
        Text(text)
            .font(Theme.Font.mono(7))
            .foregroundStyle(Theme.Palette.textFaint)
            .frame(width: 26, alignment: .leading)
    }

    private func bar(_ fraction: Double, _ tint: Color) -> some View {
        GeometryReader { geo in
            ZStack(alignment: .leading) {
                Rectangle().fill(Theme.Palette.recess)
                Rectangle().fill(tint)
                    .frame(width: geo.size.width * max(0, min(1, fraction)))
            }
        }
        .frame(height: 4)
    }
}
