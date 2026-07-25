import SwiftUI
import UniformTypeIdentifiers

private struct MixerPrePanHeightKey: PreferenceKey {
    static var defaultValue: [Int: CGFloat] = [:]
    static func reduce(value: inout [Int: CGFloat], nextValue: () -> [Int: CGFloat]) {
        value.merge(nextValue(), uniquingKeysWith: { _, new in new })
    }
}

/// Small console knob for narrow mixer strips. Vertical drag changes the value;
/// double-click restores the hardware-inspired default.
private struct ConsoleMiniKnob: View {
    let value: Float
    let range: ClosedRange<Float>
    let defaultValue: Float
    let display: (Float) -> String
    let onChange: (Float) -> Void
    let onCommit: () -> Void
    var tint: Color = Theme.Palette.textBright
    var faceTint: Color? = nil
    @State private var dragStart: Float?
    @State private var liveValue: Float?

    private var normalized: Double {
        Double(((liveValue ?? value) - range.lowerBound) / max(0.0001, range.upperBound - range.lowerBound))
    }

    var body: some View {
        VStack(spacing: 1) {
            ZStack {
                // The 4000E controls use a restrained 270° hardware scale.  The
                // alternating long/short ticks keep the control readable at mixer size.
                ForEach(0..<11, id: \.self) { tick in
                    Capsule()
                        .fill(tick == 5 ? Color.white.opacity(0.72) : Color.white.opacity(0.28))
                        .frame(width: tick == 5 ? 1.2 : 0.8,
                               height: tick.isMultiple(of: 5) ? 3.5 : 2.2)
                        .offset(y: -14)
                        .rotationEffect(.degrees(-135 + Double(tick) * 27))
                }
                Circle()
                    .fill(Color.black.opacity(0.38))
                    .frame(width: 27, height: 27)
                    .shadow(color: .black.opacity(0.75), radius: 1.5, x: 0, y: 1)
                Circle()
                    .fill(RadialGradient(colors: [
                        faceTint?.opacity(0.98) ?? Color(hex: 0x5a5b58),
                        faceTint?.opacity(0.72) ?? Color(hex: 0x252725),
                        Color.black.opacity(0.96)
                    ], center: .topLeading, startRadius: 1, endRadius: 18))
                    .overlay(Circle().stroke(Color.white.opacity(0.23), lineWidth: 0.7))
                    .frame(width: 23, height: 23)
                Capsule()
                    .fill(tint)
                    .frame(width: 1.8, height: 8.5)
                    .offset(y: -4.5)
                    .rotationEffect(.degrees(-135 + normalized * 270))
                    .shadow(color: .black.opacity(0.55), radius: 0.5, x: 0, y: 0.5)
                Circle()
                    .fill(Color.black.opacity(0.36))
                    .frame(width: 5, height: 5)
            }
            .frame(width: 31, height: 31)
            Text(display(liveValue ?? value))
                .font(Theme.Font.mono(5.8, .semibold))
                .foregroundStyle(Color(hex: 0xc9c7bd))
                .lineLimit(1)
        }
        .contentShape(Rectangle())
        .gesture(DragGesture(minimumDistance: 1)
            .onChanged { drag in
                let start = dragStart ?? value
                if dragStart == nil { dragStart = start }
                let span = range.upperBound - range.lowerBound
                let next = min(range.upperBound,
                               max(range.lowerBound, start + Float(-drag.translation.height / 90) * span))
                liveValue = next
                onChange(next)
            }
            .onEnded { _ in dragStart = nil; liveValue = nil; onCommit() })
        .highPriorityGesture(TapGesture(count: 2).onEnded {
            liveValue = defaultValue
            onChange(defaultValue)
            onCommit()
            DispatchQueue.main.async { liveValue = nil }
        })
    }
}

/// Harrison-style channel-module focus. The engine currently exposes channel
/// processors through the insert chain, so processor choices focus that real
/// chain; routing choices focus their dedicated mixer sections.
enum MixerModuleFocus: String, CaseIterable, Identifiable {
    case filter, comp, gate, eq, saturator, deEss, insert, inRec, sends, denoise

    static var allCases: [MixerModuleFocus] {
        [.filter, .eq, .gate, .comp, .saturator, .insert, .sends]
    }

    var id: String { rawValue }

    var label: String {
        switch self {
        case .filter: return "Hi/Lo Cut"
        case .comp: return "Comp"
        case .gate: return "Gate"
        case .eq: return "EQ"
        case .saturator: return "Saturator"
        case .deEss: return "DeEss"
        case .insert: return "Insert"
        case .inRec: return "In/Rec"
        case .sends: return "Sends"
        case .denoise: return "Denoise"
        }
    }

    var usesInsertChain: Bool {
        switch self {
        case .filter, .comp, .gate, .eq, .saturator, .deEss, .insert, .denoise: return true
        case .inRec, .sends: return false
        }
    }
}

/// Drag a filled insert slot onto another to move it (reorder within a track, or across
/// tracks); hold Option to copy. Payload is "trackId:slot" so the drop knows the source track.
private struct InsertDragDrop: ViewModifier {
    let isMaster: Bool
    let trackId: Int
    let slot: Int
    let filled: Bool
    /// (sourceTrackId, sourceSlot, destSlot, copy)
    let onDropInsert: (Int, Int, Int, Bool) -> Void

    func body(content: Content) -> some View {
        let dropped = content.onDrop(of: [.plainText], isTargeted: nil) { providers in
            guard !isMaster, let provider = providers.first else { return false }
            let copy = NSEvent.modifierFlags.contains(.option)
            _ = provider.loadObject(ofClass: NSString.self) { object, _ in
                guard let text = object as? String else { return }
                let parts = text.split(separator: ":").compactMap { Int($0) }
                guard parts.count == 2 else { return }
                DispatchQueue.main.async { onDropInsert(parts[0], parts[1], slot, copy) }
            }
            return true
        }
        if filled && !isMaster {
            return AnyView(dropped.onDrag { NSItemProvider(object: "\(trackId):\(slot)" as NSString) })
        }
        return AnyView(dropped)
    }
}

/// One insert slot's chip + its right-click menu, pulled out of ChannelStrip so it does NOT
/// observe EngineController. The strip re-renders ~30 Hz as meters update; when this chip lived
/// inline, an OPEN context menu was rebuilt on every one of those ticks and its submenu disclosure
/// arrow flickered. Here `engine`/`editors` are plain refs (not @EnvironmentObject), so SwiftUI
/// skips this view's body whenever the value inputs are unchanged, and the menu stays still.
private struct InsertSlotChipView: View {
    let engine: EngineController
    let editors: PluginEditorHost
    let accent: Color
    let isMaster: Bool
    let ownerId: Int
    let slot: Int
    let name: String
    let isEmpty: Bool
    let bypassed: Bool
    let badge: String
    let chainCount: Int
    let lit: Bool

    var body: some View {
        SlotChip(label: isEmpty ? "" : name, accent: accent, bypassed: bypassed, badge: badge, lit: lit) {
            let filled = !isEmpty
            if NSEvent.modifierFlags.contains(.command) && filled {
                if isMaster { engine.toggleMasterInsertBypass(slot: slot) }
                else { engine.toggleInsertBypass(track: ownerId, slot: slot) }
            } else if filled {
                editors.toggle(trackId: ownerId, insertIndex: slot)
            } else {
                engine.openPluginBrowser(forTrack: ownerId)
            }
        }
        .contextMenu {
            if isEmpty {
                pluginPickerMenu
                Button("플러그인 추가…") { engine.openPluginBrowser(forTrack: ownerId) }
            } else {
                Button("에디터 열기") { editors.toggle(trackId: ownerId, insertIndex: slot) }
                Button(bypassed ? "바이패스 해제" : "바이패스") {
                    if isMaster { engine.toggleMasterInsertBypass(slot: slot) }
                    else { engine.toggleInsertBypass(track: ownerId, slot: slot) }
                }
                Divider()
                Menu("DSP 실행 모드") {
                    dspModeButton("Native · 인프로세스", "native", checked: badge == "NAT")
                    dspModeButton("Internal DSP · 격리 코어", "internal", checked: badge == "INT")
                }
                Divider()
                if !isMaster {
                    Menu("슬롯으로 이동") {
                        ForEach(0..<ChannelStrip.mixerSlotCount, id: \.self) { target in
                            Button("슬롯 \(ChannelStrip.slotLetter(target))") {
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
                .disabled(slot >= chainCount - 1)
                Divider()
                Button("제거", role: .destructive) {
                    if isMaster { engine.removeMasterInsert(slot: slot) }
                    else { engine.removeInsert(track: ownerId, slot: slot) }
                }
            }
        }
        .modifier(InsertDragDrop(isMaster: isMaster, trackId: ownerId, slot: slot,
                                 filled: !isEmpty,
                                 onDropInsert: { srcTrack, srcSlot, dstSlot, copy in
                                     if copy {
                                         engine.copyInsert(srcTrack: srcTrack, srcSlot: srcSlot,
                                                           dstTrack: ownerId, dstSlot: dstSlot)
                                     } else if srcTrack == ownerId {
                                         engine.moveInsert(track: ownerId, from: srcSlot, to: dstSlot)
                                     } else {
                                         engine.moveInsertAcross(srcTrack: srcTrack, srcSlot: srcSlot,
                                                                 dstTrack: ownerId, dstSlot: dstSlot)
                                     }
                                 }))
    }

    /// Pick a plug-in straight from the slot's menu — grouped by category OR by brand — so an empty
    /// slot doesn't force a trip through the browser. Instruments are excluded — they can't be inserts.
    @ViewBuilder private var pluginPickerMenu: some View {
        let fx = engine.plugins.filter { $0.category != "Instrument" }
        if !fx.isEmpty {
            Menu("플러그인 선택") {
                Menu("카테고리별") {
                    ForEach(groupKeys(fx, \.category), id: \.self) { cat in
                        Menu(cat) { pluginButtons(fx.filter { $0.category == cat }) }
                    }
                }
                Menu("브랜드별") {
                    ForEach(groupKeys(fx, \.brand), id: \.self) { brand in
                        Menu(brand) { pluginButtons(fx.filter { $0.brand == brand }) }
                    }
                }
            }
        }
    }

    @ViewBuilder private func pluginButtons(_ list: [EngineController.PluginCandidate]) -> some View {
        ForEach(list.sorted { $0.name < $1.name }) { plugin in
            Button(plugin.name) { engine.addInsertDirect(track: ownerId, pluginIndex: plugin.id) }
        }
    }

    /// Native/Internal DSP mode row, checkmarked to the slot's effective badge. Master and track
    /// inserts each have their own setter; both rebuild the chain through a declick.
    @ViewBuilder private func dspModeButton(_ label: String, _ mode: String, checked: Bool) -> some View {
        Button {
            if isMaster { engine.setMasterInsertDspMode(slot: slot, mode: mode) }
            else { engine.setInsertDspMode(track: ownerId, slot: slot, mode: mode) }
        } label: {
            if checked { Label(label, systemImage: "checkmark") } else { Text(label) }
        }
    }

    /// Distinct non-empty values for a key, sorted — the submenu headings.
    private func groupKeys(_ list: [EngineController.PluginCandidate],
                           _ key: (EngineController.PluginCandidate) -> String) -> [String] {
        var seen = Set<String>()
        for plugin in list where !key(plugin).isEmpty { seen.insert(key(plugin)) }
        return seen.sorted()
    }
}

struct MixerView: View {
    @EnvironmentObject private var engine: EngineController

    /// Section visibility, mirroring the design's toolbar chips.
    @State private var showIO = true
    @State private var showInserts = true
    @State private var showSends = true
    @State private var showMemo = false
    @State private var globalModuleFocus: MixerModuleFocus = .comp
    @State private var globalModuleFocusRevision = 0
    @State private var prePanHeights: [Int: CGFloat] = [:]

    var body: some View {
        VStack(spacing: 0) {
            toolbar
            routingBanner

            // Both axes: with 10 insert + 10 send slots a strip is taller than the pane, so it
            // must scroll vertically too (the 표시 chips can also hide inserts/sends to shorten it).
            ScrollView([.horizontal, .vertical]) {
                HStack(alignment: .top, spacing: Theme.Space.md) {
                    ForEach(columns, id: \.id) { column in
                        column.view
                    }
                }
                .padding(Theme.Space.xl)
                // Size the row to the tallest strip's content so the maxHeight strips
                // equalize to that, not to the whole window.
                .fixedSize(horizontal: false, vertical: true)
                .onPreferenceChange(MixerPrePanHeightKey.self) { prePanHeights = $0 }
            }
            .scrollIndicators(.visible)
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

            globalModuleMenu
            panLawMenu
        }
        .padding(.horizontal, Theme.Space.xxl)
        .frame(height: 34)
        .background(Theme.Palette.ruler)
    }

    private var globalModuleMenu: some View {
        Menu {
            Text("전체 믹서 모듈 표시")
            ForEach(MixerModuleFocus.allCases) { module in
                Button {
                    globalModuleFocus = module
                    // Selecting the already-active item must still re-apply it to
                    // channels that were changed independently afterwards.
                    globalModuleFocusRevision += 1
                } label: {
                    if globalModuleFocus == module {
                        Label(module.label, systemImage: "checkmark")
                    } else {
                        Text(module.label)
                    }
                }
            }
        } label: {
            HStack(spacing: 4) {
                Image(systemName: "rectangle.3.group")
                Text("전체 · \(globalModuleFocus.label)")
            }
            .font(Theme.Font.ui(9, .semibold))
            .foregroundStyle(Theme.Palette.textMuted)
            .padding(.horizontal, 8)
            .frame(height: 24)
            .background(
                RoundedRectangle(cornerRadius: Theme.Radius.button)
                    .fill(Theme.Palette.button)
                    .overlay(
                        RoundedRectangle(cornerRadius: Theme.Radius.button)
                            .stroke(Theme.Palette.divider, lineWidth: 1)
                    )
            )
        }
        .menuStyle(.borderlessButton)
        .menuIndicator(.hidden)
        .fixedSize()
        .helpTip("모든 채널 스트립에 같은 모듈 보기를 적용합니다.")
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
            mixerHasInstrument: engine.tracks.contains { $0.kind == .instrument },
            globalModuleFocus: globalModuleFocus,
            globalModuleFocusRevision: globalModuleFocusRevision,
            alignedPrePanHeight: prePanHeights.values.max()
        )
        // Every mixer strip shares the tallest one's height (Master included), so adding
        // inserts/sends to one channel grows them all together.
        .frame(maxHeight: .infinity, alignment: .top)
    }
}

struct ChannelStrip: View {
    @EnvironmentObject private var engine: EngineController
    @EnvironmentObject private var editors: PluginEditorHost

    /// How many insert / send slots each strip pre-allocates (the engine ceiling is higher).
    static let mixerSlotCount = 10
    /// Slot label A, B, … J (26-safe).
    static func slotLetter(_ index: Int) -> String {
        guard index >= 0 && index < 26 else { return "\(index + 1)" }
        return String(UnicodeScalar(65 + index)!)
    }

    @State private var renaming = false
    @State private var draftName = ""
    @State private var reorderDX: CGFloat = 0
    @State private var memoDraft = ""
    @State private var moduleFocus: MixerModuleFocus = .comp
    @State private var selectedModules: Set<MixerModuleFocus> = [.comp]
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
    /// Mixer-wide command. nil in the Inspector, where this strip remains fully independent.
    var globalModuleFocus: MixerModuleFocus? = nil
    var globalModuleFocusRevision: Int = 0
    var alignedPrePanHeight: CGFloat? = nil

    private var accent: Color { track.kind.accent }

    var body: some View {
        VStack(spacing: 0) {
            header

            VStack(spacing: Theme.Space.md) {
                prePanSection
                    .frame(minHeight: alignedPrePanHeight, alignment: .top)
                panSection
                if selectedModules.contains(.inRec) { buttonRow }
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
            if engine.showChannelDelayComp { channelDelayComp }

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
        .onChange(of: globalModuleFocusRevision) { _, _ in
            if let globalModuleFocus {
                moduleFocus = globalModuleFocus
                selectedModules = [globalModuleFocus]
            }
        }
    }

    private var prePanSection: some View {
        VStack(spacing: Theme.Space.md) {
            if showIO && selectedModules.contains(.inRec) { inputSection }
            HorizontalMeter(peakLeft: meterPeakLeft, peakRight: meterPeakRight)
            moduleFocusPanel
            if showInserts && selectedModules.contains(.insert)
                && (track.kind == .instrument || mixerHasInstrument) {
                instrumentSlotSection
            }
            if showInserts && selectedModules.contains(.insert) && track.kind.showsInserts { insertSection }
            if selectedModules.contains(.eq) { consoleEqSection }
            if selectedModules.contains(.filter) { consoleFilterSection }
            if selectedModules.contains(.comp) { consoleCompSection }
            if selectedModules.contains(.gate) { consoleGateSection }
            if selectedModules.contains(.saturator) { consoleSaturatorSection }
            if (selectedModules.contains(.deEss) || selectedModules.contains(.denoise))
                && showInserts && track.kind.showsInserts { insertSection }
            if track.kind == .master && selectedModules.contains(.sends) {
                sendSection.hidden().allowsHitTesting(false).overlay(alignment: .top) { autoFadeSection }
            } else if showSends && selectedModules.contains(.sends) && track.kind.showsSends {
                sendSection
            }
        }
        .background(
            GeometryReader { proxy in
                Color.clear.preference(key: MixerPrePanHeightKey.self,
                                       value: [track.id: proxy.size.height])
            }
        )
    }

    /// Compact two-column selector inspired by Harrison's channel-strip module picker.
    /// A normal click affects only this strip; the mixer's "전체" menu broadcasts the
    /// same choice to every strip through `globalModuleFocusRevision`.
    private var moduleFocusPanel: some View {
        let order = moduleDisplayOrder
        let leftCount = (order.count + 1) / 2
        let left = Array(order.prefix(leftCount))
        let right = Array(order.dropFirst(leftCount))
        let rowCount = max(left.count, right.count)
        let panelHeight = CGFloat(rowCount * 16 + 6)
        return ZStack {
            // Signal flow is deliberately a quiet background guide. It must not read as
            // another control or consume vertical room when the mixer strip grows.
            Canvas { context, size in
                guard rowCount > 0 else { return }
                let x = size.width / 2
                var route = Path()
                route.move(to: CGPoint(x: x, y: 4))
                route.addLine(to: CGPoint(x: x, y: size.height - 4))
                context.stroke(route, with: .color(accent.opacity(0.18)),
                               style: StrokeStyle(lineWidth: 5, lineCap: .round))
            }
            .allowsHitTesting(false)

            HStack(alignment: .top, spacing: 2) {
                VStack(spacing: 2) {
                    ForEach(left) { module in moduleFocusButton(module) }
                }
                .frame(maxWidth: .infinity)
                VStack(spacing: 2) {
                    ForEach(right) { module in moduleFocusButton(module) }
                }
                .frame(maxWidth: .infinity)
            }
        }
        .frame(height: panelHeight, alignment: .top)
        .fixedSize(horizontal: false, vertical: true)
        .padding(3)
        .background(
            RoundedRectangle(cornerRadius: 5)
                .fill(Theme.Palette.background.opacity(0.72))
                .overlay(
                    RoundedRectangle(cornerRadius: 5)
                        .stroke(Theme.Palette.coolDivider, lineWidth: 1)
                )
        )
    }

    private func moduleFocusButton(_ module: MixerModuleFocus) -> some View {
        let enabled = moduleIsEnabled(module)
        let focused = selectedModules.contains(module)
        return Button {
            moduleFocus = module
            if NSEvent.modifierFlags.contains(.shift) {
                if selectedModules.contains(module) {
                    if selectedModules.count > 1 { selectedModules.remove(module) }
                } else {
                    selectedModules.insert(module)
                }
            } else {
                selectedModules = [module]
            }
        } label: {
            HStack(spacing: 4) {
                Circle()
                    .fill(enabled ? Theme.Palette.green : Theme.Palette.textFainter.opacity(0.45))
                    .frame(width: 4, height: 4)
                Text(module.label)
                    .lineLimit(1)
                Spacer(minLength: 0)
            }
                .font(Theme.Font.ui(7.5, enabled || focused ? .bold : .regular))
                .foregroundStyle(enabled ? Theme.Palette.textBright
                                         : (focused ? accent : Theme.Palette.textDim))
                .padding(.horizontal, 4)
                .frame(maxWidth: .infinity)
                .frame(height: 14)
                .background(
                    RoundedRectangle(cornerRadius: 3)
                        .fill(enabled
                              ? accent.opacity(0.40)
                              : Theme.Palette.background.opacity(0.88))
                        .overlay(
                            RoundedRectangle(cornerRadius: 3)
                                .stroke(focused ? accent.opacity(0.82) : Color.clear,
                                        lineWidth: 1)
                        )
                )
        }
        .buttonStyle(.plain)
        .onDrag { NSItemProvider(object: module.rawValue as NSString) }
        .onDrop(of: [.plainText], isTargeted: nil) { providers in
            guard let provider = providers.first else { return false }
            _ = provider.loadObject(ofClass: NSString.self) { object, _ in
                guard let raw = object as? String,
                      let source = MixerModuleFocus(rawValue: raw) else { return }
                DispatchQueue.main.async { moveModule(source, before: module) }
            }
            return true
        }
        .helpTip("\(module.label) 채널 영역을 표시합니다.")
    }

    private func moduleIsEnabled(_ module: MixerModuleFocus) -> Bool {
        switch module {
        case .filter:
            return track.consoleFilterEnabled || track.consoleHighPassEnabled
                || track.consoleLowPassEnabled
        case .eq: return track.consoleEqEnabled
        case .gate: return track.consoleGateEnabled
        case .comp: return track.consoleCompEnabled
        case .saturator: return track.consoleSaturatorEnabled
        case .insert, .deEss, .denoise:
            return track.inserts.contains { !$0.name.isEmpty }
        case .sends: return !track.sends.isEmpty
        case .inRec: return track.recordArmed || track.inputMonitoring
        }
    }

    private var moduleDisplayOrder: [MixerModuleFocus] {
        let visible = Set(MixerModuleFocus.allCases)
        let saved = track.consoleModuleOrder.split(separator: ",")
            .compactMap { MixerModuleFocus(rawValue: String($0)) }
            .filter { visible.contains($0) }
        return saved + MixerModuleFocus.allCases.filter { !saved.contains($0) }
    }

    private func moveModule(_ source: MixerModuleFocus, before target: MixerModuleFocus) {
        guard source != target else { return }
        var order = moduleDisplayOrder
        guard let from = order.firstIndex(of: source), let to = order.firstIndex(of: target) else { return }
        order.remove(at: from)
        order.insert(source, at: from < to ? max(0, to - 1) : to)
        engine.setConsoleModuleOrder(track.id, order.map(\.rawValue))
    }

    private func consoleToggle(_ title: String, parameter: String, enabled: Bool) -> some View {
        Button { engine.setConsoleBool(track.id, parameter, !enabled) } label: {
            HStack(spacing: 4) {
                Circle().fill(enabled ? Theme.Palette.green : Theme.Palette.textFainter)
                    .frame(width: 5, height: 5)
                Text(title)
                Spacer(minLength: 0)
                Text("4000E").foregroundStyle(accent)
            }
            .font(Theme.Font.mono(6.5, .semibold))
            .foregroundStyle(Theme.Palette.textDim)
        }
        .buttonStyle(.plain)
    }

    private func consoleSlider(_ label: String, _ parameter: String,
                               range: ClosedRange<Float>, format: String) -> some View {
        let value = engine.consoleValue(track.id, parameter)
        return VStack(spacing: 1) {
            HStack {
                Text(label)
                Spacer()
                Text(String(format: format, value))
            }
            .font(Theme.Font.mono(6.5))
            .foregroundStyle(Theme.Palette.textDim)
            Slider(value: Binding(
                get: { engine.consoleValue(track.id, parameter) },
                set: { engine.setConsoleValue(track.id, parameter, $0) }
            ), in: range, onEditingChanged: { editing in
                if !editing { engine.recordGesture("4000E \(parameter)") }
            })
            .controlSize(.mini)
        }
    }

    private var consoleCompSection: some View {
        VStack(spacing: 3) {
            HStack {
                Text("COMPRESSOR")
                    .font(Theme.Font.mono(6.5, .bold))
                    .foregroundStyle(Theme.Palette.textDim)
                Spacer(minLength: 0)
                Text("4000E").font(Theme.Font.mono(6, .semibold)).foregroundStyle(accent)
            }

            HStack(alignment: .center, spacing: 5) {
                compressorKnob("MIX", parameter: "compMix", range: 0...1,
                               defaultValue: 1, display: { String(format: "%.0f%%", $0 * 100) })
                Spacer(minLength: 0)
                compressorSwitch("FAST ATTK", parameter: "compFastAttack",
                                 enabled: track.consoleCompFastAttack)
            }
            HStack(alignment: .center, spacing: 5) {
                compressorKnob("RATIO", parameter: "compRatio", range: 1...20,
                               defaultValue: 4, display: { String(format: "%.1f:1", $0) })
                Spacer(minLength: 0)
                compressorSwitch("PEAK", parameter: "compPeakMode",
                                 enabled: track.consoleCompPeakMode)
            }
            HStack(alignment: .center, spacing: 5) {
                compressorKnob("THRESH", parameter: "compThresholdDb", range: -40...0,
                               defaultValue: -18, display: { String(format: "%.0f dB", $0) })
                Spacer(minLength: 0)
                compressorGainReductionMeter
            }
            HStack(alignment: .center, spacing: 5) {
                compressorKnob("RELEASE", parameter: "compReleaseMs", range: 40...1500,
                               defaultValue: 360, display: {
                                   $0 >= 1000 ? String(format: "%.1fs", $0 / 1000) : String(format: "%.0fms", $0)
                               })
                Spacer(minLength: 0)
                compressorSwitch("COMP", parameter: "compEnabled",
                                 enabled: track.consoleCompEnabled)
            }

            HStack(spacing: 3) {
                Text("TYPE").font(Theme.Font.mono(6, .semibold))
                Spacer(minLength: 0)
                Menu("SSL 4000E") {
                    Button("SSL 4000E") {}
                }
                .font(Theme.Font.mono(6.2, .semibold))
                .menuStyle(.borderlessButton)
                .fixedSize()
            }
            .foregroundStyle(Theme.Palette.textFaint)
            circuitModeSwitch(parameter: "compCircuitMode", enabled: track.consoleCompCircuitMode)
        }
        .padding(4).background(consoleModuleBackground)
    }

    private func compressorKnob(_ title: String, parameter: String,
                                range: ClosedRange<Float>, defaultValue: Float,
                                tint: Color = Theme.Palette.textBright,
                                faceTint: Color? = nil,
                                display: @escaping (Float) -> String) -> some View {
        let value = engine.consoleValue(track.id, parameter)
        return VStack(spacing: 0) {
            Text(title)
                .font(Theme.Font.mono(5.8, .semibold))
                .foregroundStyle(Theme.Palette.textDim)
                .lineLimit(1)
                .minimumScaleFactor(0.72)
            ConsoleMiniKnob(value: value, range: range, defaultValue: defaultValue,
                            display: display,
                            onChange: { engine.setConsoleValue(track.id, parameter, $0) },
                            onCommit: { engine.recordGesture("4000E \(parameter)") },
                            tint: tint, faceTint: faceTint)
        }
        .frame(width: 31)
    }

    private func compressorSwitch(_ title: String, parameter: String, enabled: Bool) -> some View {
        Button { engine.setConsoleBool(track.id, parameter, !enabled) } label: {
            VStack(spacing: 1) {
                Circle()
                    .fill(enabled ? Color(hex: 0xf0b54a) : Color(hex: 0x292d2c))
                    .frame(width: 6, height: 6)
                    .overlay(Circle().stroke(Color.black.opacity(0.8), lineWidth: 0.7))
                    .shadow(color: enabled ? Color(hex: 0xf0b54a).opacity(0.7) : .clear, radius: 2)
                Text(title)
                    .font(Theme.Font.mono(5.6, .semibold))
                    .lineLimit(1)
                    .padding(.horizontal, 4)
                    .frame(minHeight: 14)
                    .background(
                        RoundedRectangle(cornerRadius: 2)
                            .fill(enabled ? Color(hex: 0x51544f) : Color(hex: 0x282b29))
                            .overlay(RoundedRectangle(cornerRadius: 2)
                                .stroke(Color.white.opacity(enabled ? 0.28 : 0.12), lineWidth: 0.7))
                    )
            }
            .foregroundStyle(enabled ? Color(hex: 0xf1eee4) : Color(hex: 0x8b8d87))
        }
        .buttonStyle(.plain)
    }

    private var compressorGainReductionMeter: some View {
        gainReductionMeter(track.consoleCompGainReductionDb, title: "GR dB")
    }

    private func gainReductionMeter(_ value: Float, title: String) -> some View {
        let gr = max(0, min(20, value))
        let thresholds: [Float] = [20, 14, 8, 6, 3, 1]
        return VStack(alignment: .leading, spacing: 1) {
            Text(title)
                .font(Theme.Font.mono(5.6, .semibold))
                .foregroundStyle(Theme.Palette.textFaint)
            HStack(alignment: .bottom, spacing: 2) {
                VStack(spacing: 1) {
                    ForEach(thresholds, id: \.self) { threshold in
                        RoundedRectangle(cornerRadius: 1)
                            .fill(gr >= threshold
                                  ? (threshold >= 14 ? Theme.Palette.red : Theme.Palette.amber)
                                  : Theme.Palette.recess)
                            .frame(width: 10, height: 3)
                    }
                }
                Text(String(format: "%.1f", gr))
                    .font(Theme.Font.mono(5.8, .semibold))
                    .foregroundStyle(Theme.Palette.textNumeric)
            }
        }
    }

    private var consoleFilterSection: some View {
        VStack(spacing: 3) {
            HStack {
                Text("FILTERS")
                    .font(Theme.Font.mono(6.5, .bold))
                    .foregroundStyle(Theme.Palette.textDim)
                Spacer(minLength: 0)
                Text("4000E")
                    .font(Theme.Font.mono(6, .semibold))
                    .foregroundStyle(accent)
            }
            HStack(alignment: .top, spacing: 8) {
                filterControl(title: "LOW CUT",
                              parameter: "highPassHz",
                              range: 20...350,
                              defaultValue: 20,
                              enabledParameter: "highPassEnabled",
                              enabled: track.consoleHighPassEnabled,
                              unit: "Hz",
                              display: { String(format: "%.0f", $0) })
                Spacer(minLength: 0)
                filterControl(title: "HIGH CUT",
                              parameter: "lowPassHz",
                              range: 3000...12000,
                              defaultValue: 12000,
                              enabledParameter: "lowPassEnabled",
                              enabled: track.consoleLowPassEnabled,
                              unit: "kHz",
                              display: { String(format: "%.1f", $0 / 1000) })
            }
            circuitModeSwitch(parameter: "filterCircuitMode", enabled: track.consoleFilterCircuitMode)
        }
        .padding(4).background(consoleModuleBackground)
    }

    private func filterControl(title: String, parameter: String,
                               range: ClosedRange<Float>, defaultValue: Float,
                               enabledParameter: String, enabled: Bool,
                               unit: String,
                               display: @escaping (Float) -> String) -> some View {
        VStack(spacing: 1) {
            Text(title)
                .font(Theme.Font.mono(5.8, .semibold))
                .foregroundStyle(Theme.Palette.textDim)
            compressorKnob("FREQ", parameter: parameter, range: range,
                           defaultValue: defaultValue, display: display)
            Button {
                engine.setConsoleBool(track.id, enabledParameter, !enabled)
            } label: {
                HStack(spacing: 2) {
                    Text("OUT")
                    Circle()
                        .fill(enabled ? Theme.Palette.green : Theme.Palette.recess)
                        .frame(width: 7, height: 7)
                        .overlay(Circle().stroke(Theme.Palette.coolDividerBright, lineWidth: 1))
                    Text(unit)
                }
                .font(Theme.Font.mono(5.5, .semibold))
                .foregroundStyle(enabled ? Theme.Palette.textBright : Theme.Palette.textFaint)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .help("\(title) \(enabled ? "끄기" : "켜기")")
        }
    }

    private var consoleGateSection: some View {
        VStack(spacing: 3) {
            HStack {
                Text("GATE / EXPANDER")
                    .font(Theme.Font.mono(6.5, .bold))
                    .foregroundStyle(Theme.Palette.textDim)
                Spacer(minLength: 0)
                Text("4000E").font(Theme.Font.mono(6, .semibold)).foregroundStyle(accent)
            }
            HStack(alignment: .center, spacing: 5) {
                compressorKnob("RANGE", parameter: "gateRangeDb", range: 0...40,
                               defaultValue: 20, display: { String(format: "%.0f dB", $0) })
                Spacer(minLength: 0)
                compressorKnob("THRESH", parameter: "gateThresholdDb", range: -60...0,
                               defaultValue: -36, display: { String(format: "%.0f dB", $0) })
            }
            HStack(alignment: .center, spacing: 5) {
                compressorKnob("RELEASE", parameter: "gateReleaseMs", range: 40...1500,
                               defaultValue: 360, display: {
                                   $0 >= 1000 ? String(format: "%.1fs", $0 / 1000) : String(format: "%.0fms", $0)
                               })
                Spacer(minLength: 0)
                compressorKnob("HOLD", parameter: "gateHoldMs", range: 0...800,
                               defaultValue: 0, display: { String(format: "%.0fms", $0) })
            }
            HStack(alignment: .center, spacing: 5) {
                compressorSwitch("EXPAND", parameter: "expanderMode",
                                 enabled: track.consoleExpanderMode)
                Spacer(minLength: 0)
                compressorSwitch("FAST ATTK", parameter: "gateFastAttack",
                                 enabled: track.consoleGateFastAttack)
            }
            HStack(alignment: .center, spacing: 5) {
                compressorSwitch("GATE", parameter: "gateEnabled",
                                 enabled: track.consoleGateEnabled)
                Spacer(minLength: 0)
                gainReductionMeter(track.consoleGateGainReductionDb, title: "GR dB")
            }
            HStack(spacing: 3) {
                Text("TYPE").font(Theme.Font.mono(6, .semibold))
                Spacer(minLength: 0)
                Menu("SSL 4000E") { Button("SSL 4000E") {} }
                    .font(Theme.Font.mono(6.2, .semibold))
                    .menuStyle(.borderlessButton)
                    .fixedSize()
            }
            .foregroundStyle(Theme.Palette.textFaint)
            circuitModeSwitch(parameter: "gateCircuitMode", enabled: track.consoleGateCircuitMode)
        }
        .padding(4).background(consoleModuleBackground)
    }

    private var consoleEqSection: some View {
        let hf = Color(hex: 0xa92d22)
        let hmf = Color(hex: 0x31913f)
        let lmf = Color(hex: 0x36a6b8)
        let lf = Color(hex: 0x242426)
        let pointer = Color(hex: 0xf2eee5)
        return VStack(spacing: 4) {
            HStack {
                Text("EQUALISER")
                    .font(Theme.Font.mono(6.5, .bold))
                    .foregroundStyle(Theme.Palette.textDim)
                Spacer(minLength: 0)
                Button { engine.setConsoleBool(track.id, "eqEnabled", !track.consoleEqEnabled) } label: {
                    HStack(spacing: 3) {
                        Circle()
                            .fill(track.consoleEqEnabled ? Theme.Palette.red : Theme.Palette.recess)
                            .frame(width: 7, height: 7)
                            .overlay(Circle().stroke(Theme.Palette.coolDividerBright, lineWidth: 1))
                        Text("4000E")
                    }
                    .font(Theme.Font.mono(6, .semibold))
                    .foregroundStyle(track.consoleEqEnabled ? Theme.Palette.textBright : accent)
                }
                .buttonStyle(.plain)
            }

            HStack(alignment: .center, spacing: 8) {
                compressorKnob("HF GAIN", parameter: "eqHfGainDb", range: -18...18,
                               defaultValue: 0, tint: pointer, faceTint: hf,
                               display: { String(format: "%+.0f dB", $0) })
                Spacer(minLength: 0)
                VStack(spacing: 4) {
                    compressorSwitch("BELL", parameter: "eqHfBell", enabled: track.consoleEqHfBell)
                    compressorKnob("HF FREQ", parameter: "eqHfHz", range: 4000...16000,
                                   defaultValue: 8000, tint: pointer, faceTint: hf,
                                   display: { String(format: "%.1f kHz", $0 / 1000) })
                }
            }
            eqSectionDivider(hf)

            HStack(alignment: .top, spacing: 3) {
                compressorKnob("HMF GAIN", parameter: "eqHmfGainDb", range: -18...18,
                               defaultValue: 0, tint: pointer, faceTint: hmf,
                               display: { String(format: "%+.0f dB", $0) })
                compressorKnob("HMF FREQ", parameter: "eqHmfHz", range: 1200...7500,
                               defaultValue: 3000, tint: pointer, faceTint: hmf,
                               display: { String(format: "%.1f kHz", $0 / 1000) })
                compressorKnob("HMF Q", parameter: "eqHmfQ", range: 0.2...10,
                               defaultValue: 1, tint: pointer, faceTint: hmf,
                               display: { String(format: "%.1f", $0) })
            }
            .frame(maxWidth: .infinity)
            eqSectionDivider(hmf)

            HStack(alignment: .top, spacing: 3) {
                compressorKnob("LMF GAIN", parameter: "eqLmfGainDb", range: -18...18,
                               defaultValue: 0, tint: pointer, faceTint: lmf,
                               display: { String(format: "%+.0f dB", $0) })
                compressorKnob("LMF FREQ", parameter: "eqLmfHz", range: 400...2500,
                               defaultValue: 1000, tint: pointer, faceTint: lmf,
                               display: { String(format: "%.2f kHz", $0 / 1000) })
                compressorKnob("LMF Q", parameter: "eqLmfQ", range: 0.2...10,
                               defaultValue: 1, tint: pointer, faceTint: lmf,
                               display: { String(format: "%.1f", $0) })
            }
            .frame(maxWidth: .infinity)
            eqSectionDivider(lmf)

            HStack(alignment: .center, spacing: 8) {
                VStack(spacing: 3) {
                    compressorKnob("LF FREQ", parameter: "eqLfHz", range: 90...450,
                                   defaultValue: 200, tint: pointer, faceTint: lf,
                                   display: { String(format: "%.0f Hz", $0) })
                    compressorKnob("LF GAIN", parameter: "eqLfGainDb", range: -18...18,
                                   defaultValue: 0, tint: pointer, faceTint: lf,
                                   display: { String(format: "%+.0f dB", $0) })
                }
                Spacer(minLength: 0)
                compressorSwitch("BELL", parameter: "eqLfBell", enabled: track.consoleEqLfBell)
            }
            circuitModeSwitch(parameter: "eqCircuitMode", enabled: track.consoleEqCircuitMode)
        }
        .padding(4).background(consoleModuleBackground)
    }

    private func eqSectionDivider(_ color: Color) -> some View {
        Rectangle()
            .fill(LinearGradient(colors: [color.opacity(0.08), color.opacity(0.62), color.opacity(0.08)],
                                 startPoint: .leading, endPoint: .trailing))
            .frame(height: 1)
    }

    private var consoleSaturatorSection: some View {
        VStack(spacing: 4) {
            HStack {
                Text("SATURATOR")
                    .font(Theme.Font.mono(6.5, .bold))
                    .foregroundStyle(Theme.Palette.textDim)
                Spacer(minLength: 0)
                Text("4000E").font(Theme.Font.mono(6, .semibold)).foregroundStyle(accent)
            }
            HStack(spacing: 6) {
                compressorKnob("DRIVE", parameter: "saturatorDriveDb", range: 0...24,
                               defaultValue: 6, tint: Theme.Palette.amber,
                               display: { String(format: "%.1f dB", $0) })
                Spacer(minLength: 0)
                compressorKnob("MIX", parameter: "saturatorMix", range: 0...1,
                               defaultValue: 1, tint: Theme.Palette.amber,
                               display: { String(format: "%.0f%%", $0 * 100) })
                Spacer(minLength: 0)
                compressorSwitch("SAT IN", parameter: "saturatorEnabled",
                                 enabled: track.consoleSaturatorEnabled)
            }
            circuitModeSwitch(parameter: "saturatorCircuitMode",
                              enabled: track.consoleSaturatorCircuitMode)
        }
        .padding(4).background(consoleModuleBackground)
    }

    private func circuitModeSwitch(parameter: String, enabled: Bool) -> some View {
        HStack(spacing: 5) {
            Text("MODE").font(Theme.Font.mono(5.8, .semibold))
            Spacer(minLength: 0)
            Button { engine.setConsoleBool(track.id, parameter, false) } label: {
                Image(systemName: "waveform.path")
                    .font(.system(size: 9, weight: !enabled ? .bold : .regular))
                    .foregroundStyle(!enabled ? accent : Theme.Palette.textFaint)
                    .frame(width: 14, height: 13)
            }
            .helpTip("클린 모드 — 회로 컬러 없이 처리합니다.")
            Button { engine.setConsoleBool(track.id, parameter, true) } label: {
                Image(systemName: "memorychip")
                    .font(.system(size: 9, weight: enabled ? .bold : .regular))
                    .foregroundStyle(enabled ? Theme.Palette.amber : Theme.Palette.textFaint)
                    .frame(width: 14, height: 13)
            }
            .helpTip("회로 모드 — 4000E 회로 특성과 컬러를 적용합니다.")
        }
        .font(Theme.Font.mono(5.8, .semibold))
        .buttonStyle(.plain)
    }

    private func consoleEqBand<Content: View>(_ title: String,
                                              @ViewBuilder content: () -> Content) -> some View {
        VStack(spacing: 3) {
            HStack {
                Text(title)
                    .font(Theme.Font.mono(6.5, .bold))
                    .foregroundStyle(accent)
                Spacer(minLength: 0)
            }
            content()
        }
        .padding(.top, 3)
        .overlay(alignment: .top) {
            Rectangle().fill(Theme.Palette.coolDivider).frame(height: 1)
        }
    }

    private var consoleModuleBackground: some View {
        RoundedRectangle(cornerRadius: 5)
            .fill(
                LinearGradient(colors: [
                    Color(hex: 0x252a2a),
                    Color(hex: 0x171b1b),
                    Color(hex: 0x202423)
                ], startPoint: .topLeading, endPoint: .bottomTrailing)
            )
            .overlay(alignment: .leading) {
                Rectangle()
                    .fill(Color(hex: 0xb6a978).opacity(0.42))
                    .frame(width: 1)
                    .padding(.vertical, 5)
            }
            .overlay(
                RoundedRectangle(cornerRadius: 5)
                    .stroke(Color(hex: 0x77776f).opacity(0.52), lineWidth: 0.8)
            )
            .shadow(color: .black.opacity(0.5), radius: 1.5, x: 0, y: 1)
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
            Text("인서트 A–J")
                .font(Theme.Font.mono(6.5))
                .foregroundStyle(Theme.Palette.textFaint)
            ForEach(0..<ChannelStrip.mixerSlotCount, id: \.self) { slot in
                // The master chain is the project's, not the Master track's.
                let isMaster = track.kind == .master
                let ownerId = isMaster ? EngineController.masterInsertTargetId : track.id
                let chain = isMaster ? engine.masterInserts : track.inserts
                let insert = slot < chain.count ? chain[slot] : nil
                InsertSlotChipView(
                    engine: engine,
                    editors: editors,
                    accent: accent,
                    isMaster: isMaster,
                    ownerId: ownerId,
                    slot: slot,
                    name: insert?.name ?? "",
                    isEmpty: insert?.isEmpty ?? true,
                    bypassed: insert?.bypassed ?? false,
                    badge: insert?.modeBadge ?? "",
                    chainCount: chain.count,
                    lit: editors.isOpen(.init(trackId: ownerId, insertIndex: slot))
                )
            }
        }
    }

    /// Five fixed send slots (A–E), a pre-allocated region like the inserts above: an
    /// empty slot is a dashed reserved box that assigns a bus on click; a filled one is
    /// the SendSlotRow (bus + level fader + pre/post), Pro Tools style.
    private var sendSection: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text("센드 A–J")
                .font(Theme.Font.mono(6.5))
                .foregroundStyle(Theme.Palette.textFaint)

            ForEach(0..<ChannelStrip.mixerSlotCount, id: \.self) { slot in
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
            set: { engine.setSoloSelectMode($0) }
        )) {
            Text("추가 (Additive)").tag(EngineController.SoloSelectMode.additive)
            Text("배타 (Exclusive)").tag(EngineController.SoloSelectMode.exclusive)
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
            .contextMenu {
                Button {
                    engine.setShowChannelDelayComp(!engine.showChannelDelayComp)
                } label: {
                    if engine.showChannelDelayComp { Label("채널 지연 보정(PDC) 표시", systemImage: "checkmark") }
                    else { Text("채널 지연 보정(PDC) 표시") }
                }
            }
    }

    /// Per-strip plugin-delay-compensation readout (samples + ms), shown when the mixer PDC toggle is on.
    private var channelDelayComp: some View {
        let smp = track.delayCompSamples
        let ms = engine.sampleRate > 0 ? Double(smp) / engine.sampleRate * 1000.0 : 0
        return Text(smp > 0 ? String(format: "PDC %.1fms", ms) : "PDC 0")
            .font(Theme.Font.mono(7))
            .foregroundStyle(smp > 0 ? Theme.Palette.accent : Theme.Palette.textFainter)
            .frame(maxWidth: .infinity)
            .padding(.bottom, 2)
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
