import SwiftUI
import UniformTypeIdentifiers

/// Drag a filled insert slot onto another to reorder it (track inserts only).
private struct InsertDragDrop: ViewModifier {
    let isMaster: Bool
    let slot: Int
    let filled: Bool
    let onMove: (Int, Int) -> Void

    func body(content: Content) -> some View {
        let dropped = content.onDrop(of: [.plainText], isTargeted: nil) { providers in
            guard !isMaster, let provider = providers.first else { return false }
            _ = provider.loadObject(ofClass: NSString.self) { object, _ in
                if let text = object as? String, let from = Int(text) {
                    DispatchQueue.main.async { onMove(from, slot) }
                }
            }
            return true
        }
        if filled && !isMaster {
            return AnyView(dropped.onDrag { NSItemProvider(object: "\(slot)" as NSString) })
        }
        return AnyView(dropped)
    }
}

struct MixerView: View {
    @EnvironmentObject private var engine: EngineController

    /// Section visibility, mirroring the design's toolbar chips.
    @State private var showIO = true
    @State private var showInserts = true
    @State private var showSends = true
    @State private var showDynamics = false

    var body: some View {
        VStack(spacing: 0) {
            toolbar
            routingBanner

            ScrollView(.horizontal) {
                HStack(alignment: .top, spacing: Theme.Space.md) {
                    ForEach(columns, id: \.id) { column in
                        column.view
                    }
                    MasterMeterPanel()
                }
                .padding(Theme.Space.xl)
            }
            .scrollIndicators(.visible)

            Spacer(minLength: 0)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        .background(Theme.Palette.surface)
    }

    // MARK: Toolbar

    private var toolbar: some View {
        HStack(spacing: Theme.Space.xl) {
            Text("믹서")
                .font(Theme.Font.ui(11, .bold))
                .foregroundStyle(Theme.Palette.text)
            Text("Audio · Instrument · Aux · VCA · Bus Folder · Master")
                .font(Theme.Font.ui(8.5))
                .foregroundStyle(Theme.Palette.textFaint)

            Spacer()

            Text("표시")
                .font(Theme.Font.ui(8.5))
                .foregroundStyle(Theme.Palette.textLabel)

            HStack(spacing: Theme.Space.sm) {
                chip("I/O", $showIO)
                chip("인서트", $showInserts)
                chip("센드", $showSends)
                chip("다이나믹", $showDynamics)
            }
        }
        .padding(.horizontal, Theme.Space.xxl)
        .frame(height: 34)
        .background(Theme.Palette.ruler)
    }

    private func chip(_ title: String, _ binding: Binding<Bool>) -> some View {
        Button { binding.wrappedValue.toggle() } label: {
            Text(title)
                .font(Theme.Font.ui(10.5))
                .foregroundStyle(binding.wrappedValue ? Theme.Palette.accent : Theme.Palette.textMuted)
                .padding(.horizontal, 11)
                .padding(.vertical, 5)
                .background(
                    RoundedRectangle(cornerRadius: Theme.Radius.button)
                        .fill(binding.wrappedValue ? Color(hex: 0x20282e) : Theme.Palette.button)
                        .overlay(
                            RoundedRectangle(cornerRadius: Theme.Radius.button)
                                .stroke(binding.wrappedValue ? Color(hex: 0x2c4657) : Theme.Palette.divider, lineWidth: 1)
                        )
                )
        }
        .buttonStyle(.plain)
    }

    // MARK: Routing banner

    /// The engine reports delay compensation; anything non-zero is worth surfacing.
    private var routingBanner: some View {
        Group {
            if engine.delayCompensationMs > 0.001 {
                HStack(spacing: Theme.Space.md) {
                    Text("⚠")
                    Text("라우팅:")
                    Text(String(format: "지연 보정 %.1fms 적용됨", engine.delayCompensationMs))
                        .foregroundStyle(Theme.Palette.yellow)
                    Text("·").foregroundStyle(Theme.Palette.textFainter)
                    Text("해결되지 않은 경로 없음")
                        .foregroundStyle(Color(hex: 0xc9b26a))
                    Spacer()
                }
                .font(Theme.Font.ui(9))
                .foregroundStyle(Color(hex: 0xe6c04a))
                .padding(.horizontal, Theme.Space.xxl)
                .frame(height: 22)
                .frame(maxWidth: .infinity)
                .background(Theme.Palette.amber.opacity(0.10))
            }
        }
    }

    // MARK: Columns

    private struct Column: Identifiable {
        let id: String
        let view: AnyView
    }

    /// Folder tracks own the strips whose `folder` names them; everything else
    /// stands alone. Master goes last, next to the meter panel.
    private var columns: [Column] {
        let all = engine.mixerTracks
        let folders = all.filter { $0.kind == .folder || $0.kind == .bus }
        let grouped = Set(all.filter { !$0.folder.isEmpty }.map(\.folder))

        var result: [Column] = []

        for folder in folders {
            let children = all.filter { $0.folder == folder.name }
            result.append(Column(id: "folder-\(folder.id)", view: AnyView(
                folderColumn(folder, children: children)
            )))
        }

        for track in all where track.folder.isEmpty
            && track.kind != .master
            && !(track.kind == .folder || track.kind == .bus)
            && !grouped.contains(track.name) {
            result.append(Column(id: "track-\(track.id)", view: AnyView(strip(track))))
        }

        for track in all where track.kind == .master {
            result.append(Column(id: "master-\(track.id)", view: AnyView(strip(track))))
        }

        return result
    }

    private func folderColumn(_ folder: EngineController.Track,
                              children: [EngineController.Track]) -> some View {
        let accent = folder.kind.accent
        return VStack(alignment: .leading, spacing: 5) {
            Text("\(folder.kind == .bus ? "BUS FOLDER" : "FOLDER") · \(children.count) CH")
                .font(Theme.Font.ui(8.5, .bold))
                .tracking(0.6)
                .foregroundStyle(accent)
                .padding(.horizontal, 4)

            HStack(alignment: .top, spacing: 5) {
                strip(folder)
                ForEach(children) { child in strip(child, isChild: true) }
            }
        }
        .padding(Theme.Space.md)
        .background(
            RoundedRectangle(cornerRadius: Theme.Radius.modal)
                .fill(accent.opacity(0.06))
                .overlay(
                    RoundedRectangle(cornerRadius: Theme.Radius.modal)
                        .stroke(accent.opacity(0.6), lineWidth: 1.5)
                )
        )
    }

    private func strip(_ track: EngineController.Track, isChild: Bool = false) -> some View {
        ChannelStrip(
            track: track,
            isChild: isChild,
            showIO: showIO,
            showInserts: showInserts,
            showSends: showSends,
            showDynamics: showDynamics
        )
    }
}

struct ChannelStrip: View {
    @EnvironmentObject private var engine: EngineController
    @EnvironmentObject private var editors: PluginEditorHost

    @State private var renaming = false
    @State private var draftName = ""
    @FocusState private var nameFieldFocused: Bool

    let track: EngineController.Track
    let isChild: Bool
    let showIO: Bool
    let showInserts: Bool
    let showSends: Bool
    let showDynamics: Bool

    private var accent: Color { track.kind.accent }

    var body: some View {
        VStack(spacing: 0) {
            header

            VStack(spacing: Theme.Space.md) {
                if showInserts && track.kind.showsInserts { insertSection }
                if showSends && track.kind.showsSends { sendSection }
                if showIO { ioSection }
                panSection
                buttonRow
                faderSection
                volumeReadout
                if track.kind == .master { autoFadeSection }
                if showDynamics { dynamicsSelector }
            }
            .padding(.horizontal, Theme.Space.md)
            .padding(.vertical, Theme.Space.lg)

            nameplate
            channelStats
        }
        .frame(width: 122)
        .background(
            RoundedRectangle(cornerRadius: Theme.Radius.panel)
                .fill(stripBackground)
                .overlay(
                    RoundedRectangle(cornerRadius: Theme.Radius.panel)
                        .stroke(stripBorder, lineWidth: 1)
                )
        )
        .shadow(color: .black.opacity(0.25), radius: 3, y: 2)
    }

    private var stripBackground: Color {
        switch track.kind {
        case .master: return Color(hex: 0x31291a)
        case .aux, .bus: return Color(hex: 0x20302c)
        case .vca: return Color(hex: 0x2b2637)
        case .folder: return Color(hex: 0x262c34)
        default: return Theme.Palette.ruler
        }
    }

    private var stripBorder: Color {
        switch track.kind {
        case .master: return Color(hex: 0x5a4526)
        case .aux, .bus: return Color(hex: 0x2a5148)
        case .vca: return Color(hex: 0x463a63)
        case .folder: return accent.opacity(0.53)
        default: return Theme.Palette.coolDivider
        }
    }

    // MARK: Sections

    private var header: some View {
        VStack(spacing: 0) {
            Rectangle().fill(accent).frame(height: 3)
            HStack(spacing: Theme.Space.sm) {
                Circle().fill(accent).frame(width: 5, height: 5)
                Text(track.kind.label + (isChild ? " · CHILD" : ""))
                    .font(Theme.Font.mono(7, .semibold))
                    .tracking(0.5)
                    .foregroundStyle(Theme.Palette.textDim)
                    .lineLimit(1)
                Spacer(minLength: 0)
            }
            .padding(.horizontal, Theme.Space.md)
            .frame(height: 18)
            .background(accent.opacity(0.13))
        }
    }

    private var insertSection: some View {
        VStack(alignment: .leading, spacing: 2) {
            // An instrument track's sound starts here, not in an insert.
            if !track.instrumentName.isEmpty {
                let slot = PluginEditorHost.Slot(trackId: track.id,
                                                 insertIndex: EngineController.instrumentSlotIndex)
                Text("악기")
                    .font(Theme.Font.mono(6.5))
                    .foregroundStyle(Theme.Palette.textFaint)
                SlotChip(label: track.instrumentName, accent: accent, lit: editors.isOpen(slot)) {
                    editors.toggle(trackId: track.id, insertIndex: EngineController.instrumentSlotIndex)
                }
                .contextMenu {
                    Button("에디터 열기") {
                        editors.toggle(trackId: track.id, insertIndex: EngineController.instrumentSlotIndex)
                    }
                    Button("악기 교체…") { engine.openPluginBrowser(forTrack: track.id) }
                    Divider()
                    Button("악기 제거", role: .destructive) { engine.clearInstrument(track: track.id) }
                }
            }
            Text("인서트 A–E")
                .font(Theme.Font.mono(6.5))
                .foregroundStyle(Theme.Palette.textFaint)
            ForEach(0..<5, id: \.self) { slot in
                // The master chain is the project's, not the Master track's.
                let isMaster = track.kind == .master
                let ownerId = isMaster ? EngineController.masterInsertTargetId : track.id
                let chain = isMaster ? engine.masterInserts : track.inserts
                let insert = slot < chain.count ? chain[slot] : nil
                SlotChip(
                    label: (insert?.isEmpty ?? true) ? "" : insert!.name,
                    accent: accent,
                    bypassed: insert?.bypassed ?? false,
                    badge: insert?.modeBadge ?? "",
                    lit: editors.isOpen(.init(trackId: ownerId, insertIndex: slot))
                ) {
                    let filled = !(insert?.isEmpty ?? true)
                    if NSEvent.modifierFlags.contains(.command) && filled {
                        // Pro Tools: ⌘-click an insert bypasses it.
                        if isMaster { engine.toggleMasterInsertBypass(slot: slot) }
                        else { engine.toggleInsertBypass(track: ownerId, slot: slot) }
                    } else if filled {
                        editors.toggle(trackId: ownerId, insertIndex: slot)
                    } else {
                        engine.openPluginBrowser(forTrack: ownerId)
                    }
                }
                .contextMenu {
                    if insert?.isEmpty ?? true {
                        Button("플러그인 추가…") { engine.openPluginBrowser(forTrack: ownerId) }
                    } else {
                        Button("에디터 열기") { editors.toggle(trackId: ownerId, insertIndex: slot) }
                        Button(insert!.bypassed ? "바이패스 해제" : "바이패스") {
                            if isMaster { engine.toggleMasterInsertBypass(slot: slot) }
                            else { engine.toggleInsertBypass(track: ownerId, slot: slot) }
                        }
                        Divider()
                        if !isMaster {
                            Menu("슬롯으로 이동") {
                                ForEach(0..<5, id: \.self) { target in
                                    Button("슬롯 \(["A","B","C","D","E"][target])") {
                                        engine.moveInsert(track: ownerId, from: slot, to: target)
                                    }
                                    .disabled(target == slot)
                                }
                            }
                        }
                        Button("앞으로 이동") {
                            if isMaster { engine.moveMasterInsert(slot: slot, direction: -1) }
                            else { engine.moveInsert(track: ownerId, slot: slot, direction: -1) }
                        }
                        .disabled(slot == 0)
                        Button("뒤로 이동") {
                            if isMaster { engine.moveMasterInsert(slot: slot, direction: 1) }
                            else { engine.moveInsert(track: ownerId, slot: slot, direction: 1) }
                        }
                        .disabled(slot >= chain.count - 1)
                        Divider()
                        Button("제거", role: .destructive) {
                            if isMaster { engine.removeMasterInsert(slot: slot) }
                            else { engine.removeInsert(track: ownerId, slot: slot) }
                        }
                    }
                }
                // Drag a plugin onto another slot to reorder it (track inserts).
                .modifier(InsertDragDrop(isMaster: isMaster, slot: slot,
                                         filled: !(insert?.isEmpty ?? true),
                                         onMove: { from, to in
                                             if !isMaster { engine.moveInsert(track: ownerId, from: from, to: to) }
                                         }))
            }
        }
    }

    /// Sends: each existing send is a menu (change level / remove); the last row adds
    /// one, offering the aux/bus tracks — or making an Aux bus if there are none yet.
    private var sendSection: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text("센드")
                .font(Theme.Font.mono(6.5))
                .foregroundStyle(Theme.Palette.textFaint)

            ForEach(Array(track.sends.enumerated()), id: \.offset) { idx, send in
                Menu {
                    Text(send.bus)
                    Menu("레벨") {
                        ForEach([0, -3, -6, -12, -18, -24], id: \.self) { db in
                            Button("\(db) dB") { engine.setSendGain(track.id, slot: idx, gainDb: Float(db)) }
                        }
                    }
                    Divider()
                    Button("센드 제거", role: .destructive) { engine.removeSend(track.id, slot: idx) }
                } label: {
                    sendChip("\(send.bus) · \(Int(send.gainDb))dB", filled: true)
                }
                .menuStyle(.borderlessButton)
                .menuIndicator(.hidden)
            }

            Menu {
                let options = engine.sendBusOptions(track.id)
                ForEach(options, id: \.self) { bus in
                    Button(bus) { engine.addSend(track.id, to: bus) }
                }
                if !options.isEmpty { Divider() }
                Button("Aux 버스 만들기") { engine.addAuxTrack() }
            } label: {
                sendChip("+ 센드", filled: false)
            }
            .menuStyle(.borderlessButton)
            .menuIndicator(.hidden)
        }
    }

    private func sendChip(_ text: String, filled: Bool) -> some View {
        Text(text)
            .font(Theme.Font.ui(8))
            .foregroundStyle(filled ? Theme.Palette.teal : Theme.Palette.textFaint)
            .lineLimit(1)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.horizontal, Theme.Space.md)
            .frame(height: 16)
            .background(
                RoundedRectangle(cornerRadius: Theme.Radius.clip)
                    .fill(filled ? Theme.Palette.teal.opacity(0.14) : Theme.Palette.button)
                    .overlay(
                        RoundedRectangle(cornerRadius: Theme.Radius.clip)
                            .stroke(Theme.Palette.divider, lineWidth: 1)
                    )
            )
    }

    private func slotSection(_ title: String, _ items: [String], _ tint: Color) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(title)
                .font(Theme.Font.mono(6.5))
                .foregroundStyle(Theme.Palette.textFaint)
            // The design shows five slots whether or not the engine filled them.
            ForEach(0..<5, id: \.self) { slot in
                SlotChip(label: slot < items.count ? items[slot] : "", accent: tint)
            }
        }
    }

    private var ioSection: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text("I / O")
                .font(Theme.Font.mono(6.5))
                .foregroundStyle(Theme.Palette.textFaint)

            // Input — a MIDI source for instrument/MIDI tracks, a hardware pair otherwise.
            Menu {
                if track.kind == .instrument || track.kind == .midi {
                    let sources = engine.midiInputs()
                    if sources.isEmpty {
                        Text("연결된 MIDI 입력 없음")
                    } else {
                        ForEach(sources, id: \.id) { src in
                            Button(src.name) {
                                engine.setTrackMidiSource(track.id, sourceId: src.id, label: src.name)
                            }
                        }
                    }
                } else {
                    ForEach(engine.audioInputOptions(), id: \.self) { opt in
                        Button(opt) { engine.setTrackInputBus(track.id, opt) }
                    }
                }
            } label: {
                ioPillLabel(track.inputBus.isEmpty ? "—" : track.inputBus)
            }
            .menuStyle(.borderlessButton)
            .menuIndicator(.hidden)

            // Output — Master or any aux/bus track.
            Menu {
                ForEach(engine.outputBusOptions(track.id), id: \.self) { opt in
                    Button(opt) { engine.setTrackOutputBus(track.id, opt) }
                }
            } label: {
                ioPillLabel(track.outputBus.isEmpty ? "—" : track.outputBus)
            }
            .menuStyle(.borderlessButton)
            .menuIndicator(.hidden)
        }
    }

    private func ioPillLabel(_ text: String) -> some View {
        HStack(spacing: 0) {
            Text(text)
                .font(Theme.Font.ui(8.5))
                .foregroundStyle(Theme.Palette.textDim)
                .lineLimit(1)
            Spacer(minLength: 2)
            Text("▾")
                .font(Theme.Font.ui(6))
                .foregroundStyle(Theme.Palette.textFaint)
        }
        .padding(.horizontal, Theme.Space.md)
        .padding(.vertical, 2)
        .background(
            RoundedRectangle(cornerRadius: Theme.Radius.pill)
                .fill(Theme.Palette.background)
                .overlay(
                    RoundedRectangle(cornerRadius: Theme.Radius.pill)
                        .stroke(Theme.Palette.divider, lineWidth: 1)
                )
        )
    }

    private var panSection: some View {
        VStack(spacing: 2) {
            HStack {
                Text("PAN")
                    .font(Theme.Font.mono(6.5))
                    .foregroundStyle(Theme.Palette.textFaint)
                Spacer()
                Text(track.panLabel)
                    .font(Theme.Font.mono(7.5, .medium))
                    .foregroundStyle(Theme.Palette.textDim)
            }
            PanSlider(pan: track.pan,
                      accent: accent,
                      onChange: { engine.setTrackPan(track.id, $0) },
                      onCommit: { engine.recordGesture("Pan " + track.name) })
        }
    }

    private var buttonRow: some View {
        HStack(spacing: 2) {
            if track.kind.hasArm {
                stateButton("●", on: track.recordArmed, tint: Theme.Palette.red) {
                    engine.toggleTrackArm(track.id)
                }
                stateButton("I", on: track.inputMonitoring, tint: Theme.Palette.accent) {
                    engine.toggleTrackInputMonitoring(track.id)
                }
            }
            if track.kind.hasSolo {
                stateButton("S", on: track.solo, tint: Theme.Palette.yellow) {
                    engine.toggleTrackSolo(track.id)
                }
                .contextMenu { soloModeMenu }
            }
            // While another track is soloed, this one is silenced — blink its Mute the
            // way Pro Tools does, so it reads as held down by the solo rather than off.
            let soloSilenced = engine.anyTrackSoloed && !track.solo && track.kind.hasSolo
            stateButton("M", on: track.muted, tint: Theme.Palette.orange,
                        blink: soloSilenced && engine.soloBlinkOn) {
                engine.toggleTrackMute(track.id)
            }
        }
    }

    /// The Solo button's right-click menu, ported from the old UI: solo-select behaviour,
    /// solo-monitor mode, and Clear All Solos.
    @ViewBuilder private var soloModeMenu: some View {
        Picker("솔로 선택", selection: Binding(
            get: { engine.soloSelectMode },
            set: { engine.soloSelectMode = $0 }
        )) {
            Text("추가 (Additive)").tag(EngineController.SoloSelectMode.additive)
            Text("배타 (Exclusive)").tag(EngineController.SoloSelectMode.exclusive)
        }
        Divider()
        Picker("솔로 모니터", selection: Binding(
            get: { engine.soloMonitorMode },
            set: { engine.setSoloMonitorMode($0) }
        )) {
            Text("SIP · Solo In Place").tag(EngineController.SoloMonitorMode.sip)
            Text("AFL · After Fader (준비 중)").tag(EngineController.SoloMonitorMode.afl)
            Text("PFL · Pre Fader (준비 중)").tag(EngineController.SoloMonitorMode.pfl)
        }
        Divider()
        Button("모든 솔로 해제") { engine.clearAllSolos() }
    }

    private func stateButton(_ title: String,
                             on: Bool,
                             tint: Color,
                             blink: Bool = false,
                             action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(title)
                .font(Theme.Font.ui(9, .semibold))
                .foregroundStyle(on || blink ? Theme.Palette.deepBorder : Theme.Palette.textMuted)
                .frame(maxWidth: .infinity)
                .frame(height: 20)
                .background(
                    RoundedRectangle(cornerRadius: Theme.Radius.clip)
                        .fill(on ? tint : (blink ? tint.opacity(0.6) : Theme.Palette.button))
                        .overlay(
                            RoundedRectangle(cornerRadius: Theme.Radius.clip)
                                .stroke(Theme.Palette.border, lineWidth: 1)
                        )
                )
        }
        .buttonStyle(.plain)
    }

    private var faderSection: some View {
        HStack(alignment: .top, spacing: Theme.Space.sm) {
            FaderScaleMarks(capHeight: 26)
                .frame(height: 132)

            ChannelFader(volumeDb: track.volumeDb,
                         accent: accent,
                         onChange: { engine.setTrackVolume(track.id, $0) },
                         onCommit: { engine.recordGesture("Volume " + track.name) })
                .frame(width: 34, height: 132)

            HStack(spacing: 2) {
                VerticalMeter(peak: meterPeakLeft)
                VerticalMeter(peak: meterPeakRight)
            }
            .frame(height: 132)
        }
    }

    // The Master strip has no per-track signal of its own — it is the sum bus, so its
    // meter reads the engine's summed output peak (the same thing MASTER METER shows).
    private var meterPeakLeft: Float { track.kind == .master ? engine.outputPeakLeft : track.peakLeft }
    private var meterPeakRight: Float { track.kind == .master ? engine.outputPeakRight : track.peakRight }

    /// Master-only: an auto fade-out over the last N seconds, with a curve preview and
    /// picker. The seconds and curve write master volume automation in the engine.
    private var autoFadeSection: some View {
        VStack(alignment: .leading, spacing: 3) {
            Text("오토 페이드아웃")
                .font(Theme.Font.mono(6.5))
                .foregroundStyle(Theme.Palette.textFaint)
            HStack(spacing: Theme.Space.sm) {
                AutoFadeCurvePreview(curve: engine.autoFadeOutCurve,
                                     active: engine.autoFadeOutSeconds > 0)
                    .environmentObject(engine)
                    .frame(width: 30, height: 20)
                Button { engine.setAutoFadeSeconds(max(0, engine.autoFadeOutSeconds - 1)) } label: {
                    Text("−").font(Theme.Font.ui(11, .bold)).frame(width: 16, height: 16)
                }
                .buttonStyle(.plain)
                .foregroundStyle(engine.autoFadeOutSeconds > 0 ? Theme.Palette.text : Theme.Palette.textFainter)
                Text(engine.autoFadeOutSeconds <= 0 ? "끔" : "\(Int(engine.autoFadeOutSeconds.rounded()))s")
                    .font(Theme.Font.mono(8.5)).foregroundStyle(Theme.Palette.text)
                    .frame(minWidth: 22)
                Button {
                    // Turning it on from off jumps to the default length, then ±1 s.
                    let next = engine.autoFadeOutSeconds <= 0
                        ? EngineController.defaultAutoFadeSeconds
                        : engine.autoFadeOutSeconds + 1
                    engine.setAutoFadeSeconds(next)
                } label: {
                    Text("+").font(Theme.Font.ui(11, .bold)).frame(width: 16, height: 16)
                }
                .buttonStyle(.plain)
                .foregroundStyle(Theme.Palette.text)
            }
            Menu {
                ForEach(EngineController.autoFadeCurves, id: \.self) { curve in
                    Button(autoFadeCurveLabel(curve)) { engine.setAutoFadeCurve(curve) }
                }
            } label: {
                Text("커브: \(autoFadeCurveLabel(engine.autoFadeOutCurve)) ▾")
                    .font(Theme.Font.ui(8))
                    .foregroundStyle(Theme.Palette.textDim)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .menuStyle(.borderlessButton)
            .menuIndicator(.hidden)
        }
    }

    private func autoFadeCurveLabel(_ curve: String) -> String {
        switch curve {
        case "linear": return "직선"
        case "exponential": return "지수"
        case "logarithmic": return "로그"
        case "s_curve": return "S자"
        default: return "등파워"
        }
    }

    private var volumeReadout: some View {
        HStack(spacing: 2) {
            Text(dbLabel(track.volumeDb))
                .font(Theme.Font.mono(11, .semibold))
                .foregroundStyle(Theme.Palette.textNumeric)
            Text("dB")
                .font(Theme.Font.mono(6.5))
                .foregroundStyle(Theme.Palette.textFaint)
        }
    }

    private var dynamicsSelector: some View {
        HStack(spacing: 2) {
            Text("dyn")
                .font(Theme.Font.ui(8))
                .foregroundStyle(Theme.Palette.textDim)
            Text("▾")
                .font(Theme.Font.ui(6))
                .foregroundStyle(Theme.Palette.textFaint)
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 3)
        .background(
            RoundedRectangle(cornerRadius: Theme.Radius.pill)
                .fill(Theme.Palette.button)
        )
    }

    /// Double-click to rename. A rejected name (Master, Monitor, a duplicate) simply
    /// snaps back rather than reporting an error the user cannot act on.
    private var nameplate: some View {
        Group {
            if renaming {
                TextField("", text: $draftName)
                    .textFieldStyle(.plain)
                    .multilineTextAlignment(.center)
                    .focused($nameFieldFocused)
                    .onSubmit { commitRename() }
                    .onExitCommand { renaming = false }
                    .onChange(of: nameFieldFocused) { _, focused in
                        if !focused { commitRename() }
                    }
            } else {
                Text(track.name)
                    .lineLimit(1)
                    .onTapGesture(count: 2) {
                        guard !track.kind.isMasterish else { return }
                        draftName = track.name
                        renaming = true
                        nameFieldFocused = true
                    }
            }
        }
        .font(Theme.Font.ui(10, .bold))
        .foregroundStyle(Theme.Palette.text)
        .frame(maxWidth: .infinity)
        .padding(.vertical, Theme.Space.md)
        .background(accent.opacity(0.16))
    }

    private func commitRename() {
        defer { renaming = false }
        guard draftName != track.name else { return }
        engine.renameTrack(track.id, to: draftName)
    }

    /// peak is real. GR needs per-insert gain reduction the engine does not publish,
    /// and per-track DSP time is not exposed either — both read "—" rather than lie.
    private var channelStats: some View {
        VStack(spacing: 1) {
            statRow("peak", dbLabel(peakDb))
            statRow("GR", "—")
            statRow("DSP", "—")
        }
        .padding(.horizontal, Theme.Space.md)
        .padding(.vertical, Theme.Space.sm)
        .frame(maxWidth: .infinity)
        .background(Theme.Palette.stripFooter)
    }

    private var peakDb: Float {
        let peak = max(meterPeakLeft, meterPeakRight)
        return peak <= 0.00001 ? FaderScale.silenceDb : Float(peakToDb(peak))
    }

    private func statRow(_ label: String, _ value: String) -> some View {
        HStack {
            Text(label)
                .font(Theme.Font.mono(6.5))
                .foregroundStyle(Theme.Palette.textFainter)
            Spacer()
            Text(value)
                .font(Theme.Font.mono(6.5))
                .foregroundStyle(value == "—" ? Theme.Palette.textFainter : Theme.Palette.textLabel)
        }
    }
}

/// A tiny preview of the auto fade-out curve: full at the left, silent at the right.
private struct AutoFadeCurvePreview: View {
    @EnvironmentObject var engine: EngineController
    let curve: String
    let active: Bool

    var body: some View {
        Canvas { ctx, size in
            var path = Path()
            let steps = 20
            for i in 0...steps {
                let t = Double(i) / Double(steps)
                let amp = CGFloat(engine.autoFadeAmplitude(curve, t))
                let point = CGPoint(x: size.width * CGFloat(t), y: size.height * (1 - amp))
                if i == 0 { path.move(to: point) } else { path.addLine(to: point) }
            }
            ctx.stroke(path, with: .color(active ? Theme.Palette.accent : Theme.Palette.textFainter),
                       lineWidth: 1.5)
        }
        .background(RoundedRectangle(cornerRadius: 3).fill(Theme.Palette.recess))
    }
}

/// Dorrough-style master meter beside the strips.
struct MasterMeterPanel: View {
    @EnvironmentObject private var engine: EngineController

    private static let marks = ["0", "4", "8", "12", "16", "20", "24", "30", "40"]

    var body: some View {
        VStack(spacing: Theme.Space.lg) {
            HStack(spacing: Theme.Space.md) {
                Circle().fill(Theme.Palette.amber).frame(width: 5, height: 5)
                Text("MASTER METER")
                    .font(Theme.Font.mono(7.5, .semibold))
                    .tracking(0.6)
                    .foregroundStyle(Theme.Palette.amber)
            }

            HStack(alignment: .top, spacing: Theme.Space.md) {
                VStack(alignment: .trailing, spacing: 0) {
                    ForEach(Self.marks, id: \.self) { mark in
                        Text(mark)
                            .font(Theme.Font.mono(6))
                            .foregroundStyle(Color(hex: 0x7a6f5f))
                        if mark != Self.marks.last { Spacer(minLength: 0) }
                    }
                }
                .frame(width: 16, height: 210)

                HStack(spacing: 3) {
                    VerticalMeter(peak: engine.outputPeakLeft, segments: 60)
                    VerticalMeter(peak: engine.outputPeakRight, segments: 60)
                }
                .frame(height: 210)
            }

            VStack(spacing: Theme.Space.sm) {
                labelledRow("PEAK", ["Auto", "Hold", "Reset"], active: 0)
                labelledRow("OVERS", ["Display", "Reset"], active: 0)
                labelledRow("MODE", ["Phase", "Sum/Diff", "Left/Right"], active: 2)
            }

            HStack(spacing: Theme.Space.lg) {
                led("PHASE", lit: engine.phaseCorrelation < 0, tint: Theme.Palette.yellow)
                led("OVERS", lit: max(engine.outputPeakLeft, engine.outputPeakRight) >= 0.999,
                    tint: Theme.Palette.red)
            }
        }
        .padding(Theme.Space.xl)
        .frame(width: 168)
        .background(
            RoundedRectangle(cornerRadius: Theme.Radius.panel)
                .fill(Color(hex: 0x31291a))
                .overlay(
                    RoundedRectangle(cornerRadius: Theme.Radius.panel)
                        .stroke(Color(hex: 0x5a4526), lineWidth: 1)
                )
        )
    }

    /// The button rows are display-only until the engine exposes meter modes.
    private func labelledRow(_ title: String, _ items: [String], active: Int) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(title)
                .font(Theme.Font.mono(6))
                .foregroundStyle(Theme.Palette.textFaint)
            HStack(spacing: 2) {
                ForEach(Array(items.enumerated()), id: \.offset) { index, item in
                    Text(item)
                        .font(Theme.Font.ui(8))
                        .foregroundStyle(index == active ? Theme.Palette.amber : Theme.Palette.textMuted)
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 4)
                        .background(
                            RoundedRectangle(cornerRadius: Theme.Radius.button)
                                .fill(index == active ? Color(hex: 0x2a2113) : Theme.Palette.button)
                                .overlay(
                                    RoundedRectangle(cornerRadius: Theme.Radius.button)
                                        .stroke(index == active ? Color(hex: 0x4d3a20) : Theme.Palette.divider,
                                                lineWidth: 1)
                                )
                        )
                }
            }
        }
    }

    private func led(_ title: String, lit: Bool, tint: Color) -> some View {
        HStack(spacing: Theme.Space.sm) {
            Circle()
                .fill(lit ? tint : Theme.Palette.recess)
                .frame(width: 6, height: 6)
                .shadow(color: lit ? tint.opacity(0.8) : .clear, radius: 3)
            Text(title)
                .font(Theme.Font.mono(6.5))
                .foregroundStyle(Theme.Palette.textFaint)
        }
    }
}
