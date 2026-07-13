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
    @State private var showMemo = false

    var body: some View {
        VStack(spacing: 0) {
            toolbar
            routingBanner

            ScrollView(.horizontal) {
                HStack(alignment: .top, spacing: Theme.Space.md) {
                    ForEach(columns, id: \.id) { column in
                        column.view
                    }
                }
                .padding(Theme.Space.xl)
                // Size the row to the tallest strip's content so the maxHeight strips
                // equalize to that, not to the whole window.
                .fixedSize(horizontal: false, vertical: true)
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
                chip("cable.connector", engine.tr("help.mixer_io"), $showIO)
                chip("square.stack.3d.up.fill", engine.tr("help.mixer_inserts"), $showInserts)
                chip("arrow.up.forward", engine.tr("help.mixer_sends"), $showSends)
                chip("note.text", engine.tr("help.mixer_memo"), $showMemo)
            }

            panLawMenu
        }
        .padding(.horizontal, Theme.Space.xxl)
        .frame(height: 34)
        .background(Theme.Palette.ruler)
    }

    private static let panLaws: [(id: String, label: String)] = [
        ("-3dB", "−3 dB"), ("-4.5dB", "−4.5 dB"), ("-6dB", "−6 dB"), ("legacy", "레거시(선형)"),
    ]

    private var panLawMenu: some View {
        Menu {
            Text("팬 법칙 (모노 트랙)")
            ForEach(Self.panLaws, id: \.id) { law in
                Button {
                    engine.setPanLaw(law.id)
                } label: {
                    if engine.panLaw == law.id { Label(law.label, systemImage: "checkmark") } else { Text(law.label) }
                }
            }
        } label: {
            let current = Self.panLaws.first { $0.id == engine.panLaw }?.label ?? engine.panLaw
            Text("팬: \(current)")
                .font(Theme.Font.ui(9.5))
                .foregroundStyle(Theme.Palette.textMuted)
                .padding(.horizontal, 9).padding(.vertical, 5)
                .background(RoundedRectangle(cornerRadius: Theme.Radius.button).fill(Theme.Palette.button))
        }
        .menuStyle(.borderlessButton).menuIndicator(.hidden).fixedSize()
        .helpTip(engine.tr("help.pan_law"))
    }

    private func chip(_ systemImage: String, _ help: String, _ binding: Binding<Bool>) -> some View {
        Button { binding.wrappedValue.toggle() } label: {
            Image(systemName: systemImage)
                .font(.system(size: 12, weight: .medium))
                .foregroundStyle(binding.wrappedValue ? Theme.Palette.accent : Theme.Palette.textMuted)
                .frame(width: 30, height: 24)
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
        .helpTip(help)
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
            showMemo: showMemo,
            mixerHasInstrument: engine.tracks.contains { $0.kind == .instrument }
        )
        // Every mixer strip shares the tallest one's height (Master included), so adding
        // inserts/sends to one channel grows them all together.
        .frame(maxHeight: .infinity, alignment: .top)
    }
}

struct ChannelStrip: View {
    @EnvironmentObject private var engine: EngineController
    @EnvironmentObject private var editors: PluginEditorHost

    @State private var renaming = false
    @State private var draftName = ""
    @State private var reorderDX: CGFloat = 0
    @State private var memoDraft = ""
    @FocusState private var nameFieldFocused: Bool
    @FocusState private var memoFocused: Bool

    private var reorderable: Bool { track.kind != .master }

    /// Reorder by dragging the strip's header sideways. SwiftUI's `.draggable` never fired
    /// reliably here, so this maps the drag distance to a number of strip-widths and moves
    /// the channel that many slots — the mixer and the timeline share `moveTrackNear`, so
    /// either one reorders both.
    private var reorderGesture: some Gesture {
        DragGesture(minimumDistance: 8)
            .onChanged { reorderDX = $0.translation.width }
            .onEnded { value in
                reorderDX = 0
                let ordered = engine.tracks.filter { $0.kind != .master }
                guard let src = ordered.firstIndex(where: { $0.id == track.id }) else { return }
                let step = Int((value.translation.width / (stripWidth + Theme.Space.md)).rounded())
                guard step != 0 else { return }
                let dst = max(0, min(ordered.count - 1, src + step))
                guard dst != src else { return }
                engine.moveTrackNear(track.id, targetId: ordered[dst].id, after: step > 0)
            }
    }

    let track: EngineController.Track
    let isChild: Bool
    let showIO: Bool
    let showInserts: Bool
    let showSends: Bool
    let showMemo: Bool
    /// The Edit-view Channel column pins a uniform width, so the strip stays the same size
    /// whatever track is selected (and whatever per-track width the mixer uses). nil = mixer.
    var fixedWidth: CGFloat? = nil
    /// True when at least one strip in the mixer carries an instrument, so every strip
    /// reserves the instrument-slot row (blank where it doesn't apply) and the inserts /
    /// sends / pan / fader stay aligned across strips — instead of the instrument strip alone
    /// growing taller. Logic-style: the slot exists on every strip.
    var mixerHasInstrument: Bool = false

    private var accent: Color { track.kind.accent }

    var body: some View {
        VStack(spacing: 0) {
            header

            VStack(spacing: Theme.Space.md) {
                if showIO { inputSection }
                // Horizontal input meter (L/R) right under the input, before the inserts.
                HorizontalMeter(peakLeft: meterPeakLeft, peakRight: meterPeakRight)
                // The instrument slot is its own reserved row (Logic-style) so every strip's
                // inserts/sends/pan/fader line up when a sibling carries an instrument.
                if showInserts && (track.kind == .instrument || mixerHasInstrument) {
                    instrumentSlotSection
                }
                if showInserts && track.kind.showsInserts { insertSection }
                // Master has no sends; its auto fade-out takes that upper slot so the fader
                // drops down and lines up with the channel faders instead of floating high
                // over an empty gap.
                if track.kind == .master {
                    autoFadeSection
                    // The master has no sends; pad to the sends' height so its pan / buttons
                    // / fader drop to the same rows as the channels'.
                    Color.clear.frame(height: 51)
                } else if showSends && track.kind.showsSends {
                    sendSection
                }
                panSection
                buttonRow
                if track.kind.hasSolo || track.kind == .master { automationModeMenu }
                // Output (post-plugin) meter — horizontal, right above the fader. The one
                // under the input is the incoming meter; this one is after the inserts.
                HorizontalMeter(peakLeft: meterPeakLeft, peakRight: meterPeakRight)
                faderSection
                volumeReadout
                if showMemo { memoField }
            }
            .padding(.horizontal, Theme.Space.md)
            .padding(.vertical, Theme.Space.lg)

            // Absorbs the extra height when siblings are taller, so every strip in the
            // mixer shares the tallest one's height and the footer stays pinned to the bottom.
            Spacer(minLength: 0)

            nameplate
            channelStats

            // Output routing pinned to the very bottom of the strip. A divider + top gap
            // separates it from the stats block above (they were reading as overlapped),
            // and the extra trailing space reserves the bottom-right corner for the grip.
            if showIO {
                Rectangle().fill(Theme.Palette.deepBorder).frame(height: 1)
                outputSection
                    .padding(.leading, Theme.Space.md)
                    .padding(.trailing, 22)
                    .padding(.top, Theme.Space.sm)
                    .padding(.bottom, Theme.Space.md)
            }
        }
        .frame(width: stripWidth)
        .background(RoundedRectangle(cornerRadius: Theme.Radius.panel).fill(stripBackground))
        // Clip content to the rounded card so the header's accent bar doesn't poke square
        // corners out past the (rounded) selection outline.
        .clipShape(RoundedRectangle(cornerRadius: Theme.Radius.panel))
        .overlay(
            RoundedRectangle(cornerRadius: Theme.Radius.panel)
                .strokeBorder(strokeColor, lineWidth: isSelected ? 2 : 1)
        )
        // Width grip lives at the bottom-right corner (by the footer), not the ambiguous
        // mid-right edge. Hidden in the fixed-width Channel column.
        .overlay(alignment: .bottomTrailing) { if fixedWidth == nil && track.kind != .master { widthResizeHandle } }
        .shadow(color: .black.opacity(reorderDX != 0 ? 0.45 : 0.25), radius: reorderDX != 0 ? 8 : 3, y: 2)
        // Lift and follow the cursor while its header is being dragged sideways to reorder.
        .offset(x: fixedWidth == nil ? reorderDX : 0)
        .zIndex(reorderDX != 0 ? 10 : 0)
    }

    /// A corner toggle — click to switch this channel (and the whole mixer selection)
    /// between the default (large) and the narrow (small) width. Two states, no free
    /// widening past the default.
    private var widthResizeHandle: some View {
        // Midpoint test so it flips reliably whichever exact widths are in play.
        let isNarrow = engine.channelWidthFor(track.id) < (EngineController.channelWidthMin + EngineController.channelWidthDefault) / 2
        return Button {
            // Only this channel — or, if it is part of a multi-selection, the whole
            // selection. Never every strip.
            let sel = engine.selectedMixerTrackIds
            let targets: [Int] = (sel.contains(track.id) && sel.count > 1) ? Array(sel) : [track.id]
            engine.setChannelWidth(trackIds: targets,
                                   width: isNarrow ? EngineController.channelWidthDefault : EngineController.channelWidthMin)
            engine.commitChannelWidth()
        } label: {
            Text(isNarrow ? "‹ ›" : "› ‹")
                .font(Theme.Font.mono(7, .bold))
                .foregroundStyle(Color.white.opacity(0.5))
                .frame(width: 16, height: 15)
                .background(RoundedRectangle(cornerRadius: 3).fill(Color.white.opacity(0.06)))
        }
        .buttonStyle(.plain)
        .padding(.trailing, 3)
        .padding(.bottom, 7)
        .helpTip(engine.tr(isNarrow ? "help.channel_wide" : "help.channel_narrow"))
    }

    /// Selected (a `⌘/⇧`-extendable set); the last-clicked strip also drives the timeline.
    private var isSelected: Bool { engine.selectedMixerTrackIds.contains(track.id) }

    private var strokeColor: Color {
        if isSelected { return accent }
        return stripBorder
    }

    /// Width honours a live multi-selection drag preview before it commits.
    private var stripWidth: CGFloat {
        if let fixedWidth { return fixedWidth }
        // The master keeps a fixed full width — it has no narrow toggle.
        if track.kind == .master { return EngineController.channelWidthDefault }
        if let drag = engine.channelWidthDrag, drag.targets.contains(track.id) {
            return min(EngineController.channelWidthMax, max(EngineController.channelWidthMin, drag.width))
        }
        return max(EngineController.channelWidthMin, engine.channelWidthFor(track.id))
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

    @ViewBuilder
    private var header: some View {
        let content = VStack(spacing: 0) {
            Rectangle().fill(accent).frame(height: 3)
            HStack(spacing: Theme.Space.sm) {
                Circle().fill(accent).frame(width: 5, height: 5)
                Text(track.kind.label + (isChild ? " · CHILD" : ""))
                    .font(Theme.Font.mono(7, .semibold))
                    .tracking(0.5)
                    .foregroundStyle(Theme.Palette.textDim)
                    .lineLimit(1)
                Spacer(minLength: 0)
                // Stereo/mono, in the (uniform-height) header so it never breaks strip
                // alignment. Auto-defined on import / instrument load; this overrides by hand.
                Menu {
                    Button("스테레오") { engine.setTrackStereo(track.id, true) }
                    Button("모노") { engine.setTrackStereo(track.id, false) }
                } label: {
                    Text(track.isStereo ? "St" : "Mo")
                        .font(Theme.Font.mono(6.5, .semibold))
                        .foregroundStyle(accent)
                        .padding(.horizontal, 4).frame(height: 12)
                        .background(RoundedRectangle(cornerRadius: 2).fill(accent.opacity(0.22)))
                }
                .menuStyle(.borderlessButton).menuIndicator(.hidden).fixedSize()
            }
            .padding(.horizontal, Theme.Space.md)
            .frame(height: 18)
            .background(accent.opacity(0.13))
        }
        .contentShape(Rectangle())
        // The header is the reorder grip. Selecting the strip lives on the nameplate, so
        // no tap gesture competes with the drag here.
        if reorderable {
            content.gesture(reorderGesture)
        } else {
            content
        }
    }

    /// The instrument slot, its own row so it doesn't push the inserts/sends/pan/fader of an
    /// instrument strip out of line with the others. Instrument tracks show the slot (loaded
    /// or empty-to-load); every other strip renders the same height blank so the rows align.
    private var instrumentSlotSection: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text("악기")
                .font(Theme.Font.mono(6.5))
                .foregroundStyle(Theme.Palette.textFaint)
            if track.kind == .instrument {
                let slot = PluginEditorHost.Slot(trackId: track.id,
                                                 insertIndex: EngineController.instrumentSlotIndex)
                // "+N" badge when instruments are layered (all fed the same MIDI, summed). The
                // strip stays one fixed-height slot; the full rack is managed in the Inspector.
                let extraLayers = max(0, track.instrumentLayers.count - 1)
                let label = track.instrumentName + (extraLayers > 0 ? "  +\(extraLayers)" : "")
                SlotChip(label: label, accent: accent, lit: editors.isOpen(slot)) {
                    if track.instrumentName.isEmpty {
                        engine.loadInstrument(track: track.id)
                    } else {
                        editors.toggle(trackId: track.id, insertIndex: EngineController.instrumentSlotIndex)
                    }
                }
                .contextMenu {
                    Button("에디터 열기") {
                        editors.toggle(trackId: track.id, insertIndex: EngineController.instrumentSlotIndex)
                    }
                    Button("악기 교체…") { engine.loadInstrument(track: track.id) }
                    Button("레이어 추가…") { engine.addInstrumentLayer(track: track.id) }
                    Divider()
                    Button("악기 제거", role: .destructive) { engine.clearInstrument(track: track.id) }
                }
            } else {
                SlotChip(label: "", accent: accent) {}
            }
        }
        // Non-instrument strips reserve the identical height but show nothing.
        .opacity(track.kind == .instrument ? 1 : 0)
        .allowsHitTesting(track.kind == .instrument)
    }

    private var insertSection: some View {
        VStack(alignment: .leading, spacing: 2) {
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

    /// Five fixed send slots (A–E), a pre-allocated region like the inserts above: an
    /// empty slot is a dashed reserved box that assigns a bus on click; a filled one is
    /// the SendSlotRow (bus + level fader + pre/post), Pro Tools style.
    private var sendSection: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text("센드 A–E")
                .font(Theme.Font.mono(6.5))
                .foregroundStyle(Theme.Palette.textFaint)

            ForEach(0..<5, id: \.self) { slot in
                if slot < track.sends.count {
                    let send = track.sends[slot]
                    SendSlotRow(bus: send.bus, gainDb: send.gainDb, preFader: send.preFader,
                                onGain: { engine.setSendGain(track.id, slot: slot, gainDb: $0) },
                                onCommitGain: { engine.recordGesture("Send level") },
                                onTogglePrePost: { engine.setSendPreFader(track.id, slot: slot, pre: !send.preFader) })
                        .contextMenu { SendMenuContent(engine: engine, trackId: track.id, slot: slot, send: send) }
                } else {
                    Menu {
                        let options = engine.sendBusOptions(track.id)
                        ForEach(options, id: \.self) { bus in
                            Button(bus) { engine.addSend(track.id, to: bus) }
                        }
                        if !options.isEmpty { Divider() }
                        Button("Aux 버스 만들기") { engine.addAuxTrack() }
                    } label: {
                        emptySendSlot
                    }
                    // .borderlessButton silently drops a shape-only (textless) label, so the
                    // dashed box never drew — the same trap as the inspector routing menus.
                    .menuStyle(.button)
                    .buttonStyle(.plain)
                    .menuIndicator(.hidden)
                    .frame(maxWidth: .infinity)   // fill the row so the dashed box is visible
                }
            }
        }
    }

    /// A reserved, dashed send slot — the same look as an empty insert `SlotChip`.
    private var emptySendSlot: some View {
        RoundedRectangle(cornerRadius: Theme.Radius.pill)
            .strokeBorder(Theme.Palette.coolDivider, style: StrokeStyle(lineWidth: 1, dash: [2, 2]))
            .background(RoundedRectangle(cornerRadius: Theme.Radius.pill).fill(Theme.Palette.background))
            .frame(maxWidth: .infinity, minHeight: 14, maxHeight: 14)
            .contentShape(Rectangle())
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

    /// Input pill — pinned to the top of the strip. A MIDI source for instrument/MIDI
    /// tracks, a hardware pair otherwise.
    private var inputSection: some View {
        // No "IN" caption — the value alone reads as the input, and it saves a row.
        VStack(alignment: .leading, spacing: 2) {
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
        }
    }

    /// Output pill — pinned to the bottom of the strip. Master or any aux/bus track.
    private var outputSection: some View {
        // No "OUT" caption — "Master" alone reads as the output destination.
        VStack(alignment: .leading, spacing: 2) {
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
                      onCommit: {
                          engine.endAutomationTouch(track.id, "track.pan")
                          if !engine.transportRunning { engine.recordGesture("Pan " + track.name) }
                      },
                      onBegin: { engine.beginAutomationTouch(track.id, "track.pan") })
        }
    }

    private func automationModeColor(_ mode: String) -> Color {
        switch mode {
        case "write": return Theme.Palette.red
        case "touch", "latch": return Theme.Palette.yellow
        case "off": return Theme.Palette.textFaint
        default: return Theme.Palette.green            // read
        }
    }

    /// Pro Tools-style automation mode button: click cycles the mode; right-click picks one.
    private var automationModeMenu: some View {
        let mode = track.automationMode
        let label = EngineController.automationModes.first { $0.id == mode }?.label ?? "Read"
        return Button {
            engine.cycleAutomationMode(track.id)
        } label: {
            HStack(spacing: 3) {
                Circle().fill(automationModeColor(mode)).frame(width: 5, height: 5)
                Text(label.uppercased())
                    .font(Theme.Font.mono(7.5, .semibold))
                    .foregroundStyle(automationModeColor(mode))
            }
            .frame(maxWidth: .infinity)
            .frame(height: 15)
            .background(RoundedRectangle(cornerRadius: 3).fill(automationModeColor(mode).opacity(0.14)))
            .overlay(RoundedRectangle(cornerRadius: 3).stroke(automationModeColor(mode).opacity(0.35), lineWidth: 0.5))
        }
        .buttonStyle(.plain)
        .contextMenu {
            ForEach(EngineController.automationModes, id: \.id) { m in
                Button {
                    engine.setAutomationMode(track.id, m.id)
                } label: {
                    if mode == m.id { Label(m.label, systemImage: "checkmark") } else { Text(m.label) }
                }
            }
        }
        .help("클릭: 모드 순환 · 우클릭: 모드 선택")
    }

    private var buttonRow: some View {
        // M · S · R(arm) · I — the same order as the track header and inspector.
        HStack(spacing: 2) {
            // While another track is soloed, this one is silenced — blink its Mute the
            // way Pro Tools does, so it reads as held down by the solo rather than off.
            let soloSilenced = engine.anyTrackSoloed && !track.solo && track.kind.hasSolo
            stateButton("M", on: track.muted, tint: Theme.Palette.orange,
                        blink: soloSilenced && engine.soloBlinkOn) {
                engine.toggleTrackMute(track.id)
            }
            if track.kind.hasSolo {
                stateButton("S", on: track.solo, tint: Theme.Palette.yellow) {
                    engine.toggleTrackSolo(track.id)
                }
                .contextMenu { soloModeMenu }
            }
            if track.kind.hasArm {
                stateButton("●", on: track.recordArmed, tint: Theme.Palette.red) {
                    engine.toggleTrackArm(track.id)
                }
                stateButton("I", on: track.inputMonitoring, tint: Theme.Palette.accent) {
                    engine.toggleTrackInputMonitoring(track.id)
                }
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
            Group {
                // Record arms as a hollow circle (like the transport record key), ~2× the
                // old dot; everything else is a plain letter in the same font.
                if title == "●" {
                    // Record circle, a touch smaller than the transport key so it doesn't
                    // dominate the narrow strip.
                    Image(systemName: "circle").font(.system(size: 9))
                } else {
                    Text(title).font(Theme.Font.ui(9, .semibold))
                }
            }
            // Off is the same colour, unlit — a dim tint fill with a dimmed tint glyph,
            // like an LED that is off but clearly still red / yellow / orange / blue.
            .foregroundStyle(on || blink ? Theme.Palette.deepBorder : tint.opacity(0.8))
            .frame(maxWidth: .infinity)
            .frame(height: 20)
            .background(
                RoundedRectangle(cornerRadius: Theme.Radius.clip)
                    .fill(on ? tint : (blink ? tint.opacity(0.6) : tint.opacity(0.16)))
                    .overlay(
                        RoundedRectangle(cornerRadius: Theme.Radius.clip)
                            .stroke(on ? Theme.Palette.border : tint.opacity(0.32), lineWidth: 1)
                    )
            )
        }
        .buttonStyle(.plain)
    }

    private var faderSection: some View {
        // spacing 0 with explicit per-gap padding: the scale ticks hug the fader (3 pt)
        // and the meter ticks hug the meter (1 pt). The single dB readout lives in
        // `volumeReadout` below, not here.
        // The level meter is horizontal (above the fader), so the fader + its dB legend
        // sit together, centred — the scale hugs the knob (~2 pt) so it stays readable.
        HStack(spacing: 2) {
            FaderScaleMarks(capHeight: ChannelFader.capHeight).frame(height: 132)
            ChannelFader(volumeDb: track.volumeDb,
                         accent: accent,
                         onChange: { engine.setTrackVolume(track.id, $0) },
                         onCommit: {
                             engine.endAutomationTouch(track.id, "track.volume")
                             if !engine.transportRunning { engine.recordGesture("Volume " + track.name) }
                         },
                         onBegin: { engine.beginAutomationTouch(track.id, "track.volume") })
                .frame(width: 18, height: 132)
        }
        .frame(maxWidth: .infinity)
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
                    // The curve is chosen by right-clicking the graph now (the text picker
                    // below was redundant).
                    .contextMenu {
                        Text("페이드 커브")
                        ForEach(EngineController.autoFadeCurves, id: \.self) { curve in
                            Button {
                                engine.setAutoFadeCurve(curve)
                            } label: {
                                if curve == engine.autoFadeOutCurve {
                                    Label(autoFadeCurveLabel(curve), systemImage: "checkmark")
                                } else {
                                    Text(autoFadeCurveLabel(curve))
                                }
                            }
                        }
                    }
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
        // Centre in a fixed span so the value changing width (0.0 → -56.2 → -∞) never
        // resizes the strip while dragging the fader.
        .lineLimit(1)
        .frame(maxWidth: .infinity)
    }

    /// A per-channel memo (커맨드/메모). Free text, saved to the track; commits one undo
    /// step when editing ends, so typing does not spam history. Grows a few lines then
    /// scrolls. Master/Monitor keep it too — a place for mix notes.
    @ViewBuilder private var memoField: some View {
        TextField("메모…", text: $memoDraft, axis: .vertical)
            .textFieldStyle(.plain)
            .font(Theme.Font.ui(8.5))
            .foregroundStyle(Theme.Palette.text)
            .lineLimit(1...4)
            .padding(5)
            .frame(maxWidth: .infinity, alignment: .leading)
            // A warm panel tone that sits in the strip rather than a flat black hole.
            .background(
                RoundedRectangle(cornerRadius: Theme.Radius.button)
                    .fill(Theme.Palette.panel)
                    .overlay(RoundedRectangle(cornerRadius: Theme.Radius.button)
                        .stroke(Theme.Palette.divider, lineWidth: 1))
            )
            .focused($memoFocused)
            .onAppear { memoDraft = track.notes }
            // Pull in external changes (undo, project load, reorder) only while not typing.
            .onChange(of: track.notes) { _, new in if !memoFocused { memoDraft = new } }
            .onChange(of: memoFocused) { _, focused in
                if !focused, memoDraft != track.notes { engine.setTrackNotes(track.id, memoDraft) }
            }
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
                Text(track.name).lineLimit(1)
            }
        }
        .font(Theme.Font.ui(10, .bold))
        .foregroundStyle(Theme.Palette.text)
        .frame(maxWidth: .infinity)
        .padding(.vertical, Theme.Space.md)
        .background(accent.opacity(0.16))
        .contentShape(Rectangle())
        // Click the nameplate to select the strip (⌘/⇧ extends); double-click renames.
        .onTapGesture(count: 2) {
            guard !track.kind.isMasterish else { return }
            draftName = track.name
            renaming = true
            nameFieldFocused = true
        }
        .onTapGesture {
            let flags = NSEvent.modifierFlags
            engine.selectMixerTrack(track.id,
                                    additive: flags.contains(.command) || flags.contains(.shift))
        }
    }

    private func commitRename() {
        defer { renaming = false }
        guard draftName != track.name else { return }
        engine.renameTrack(track.id, to: draftName)
    }

    /// The channel's peak value, colour-graded like a console overload LED: green when
    /// there is headroom, yellow when it is getting hot, red at/over 0 dBFS — and the red
    /// blinks so a momentary clip is caught. Replaces the old peak/GR/DSP block (GR and DSP
    /// were never published, so they only ever read "—").
    private var channelStats: some View {
        let db = peakDb
        let color: Color = db >= Self.clipDb ? Theme.Palette.red
                         : db >= Self.hotDb ? Theme.Palette.yellow
                         : Theme.Palette.green
        // Only the red state blinks; dim it on the off phase.
        let lit = !(db >= Self.clipDb) || engine.clipBlinkOn
        return Text(peakValueLabel(db))
            .font(Theme.Font.mono(9.5, .bold))
            .foregroundStyle(lit ? color : color.opacity(0.22))
            .padding(.horizontal, Theme.Space.md)
            .padding(.vertical, Theme.Space.sm)
            .frame(maxWidth: .infinity)
            .background(Theme.Palette.stripFooter)
    }

    /// Overload thresholds in dBFS.
    private static let clipDb: Float = -0.1
    private static let hotDb: Float = -6

    private func peakValueLabel(_ db: Float) -> String {
        db <= FaderScale.silenceDb ? "-∞" : String(format: "%+.1f", db)
    }

    private var peakDb: Float {
        let peak = max(meterPeakLeft, meterPeakRight)
        return peak <= 0.00001 ? FaderScale.silenceDb : Float(peakToDb(peak))
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
