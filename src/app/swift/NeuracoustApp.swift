import SwiftUI

/// Finder hands files to the app through the delegate, not through SwiftUI.
final class AppDelegate: NSObject, NSApplicationDelegate {
    weak var engine: EngineController?

    func application(_ application: NSApplication, open urls: [URL]) {
        Task { @MainActor [weak self] in
            self?.engine?.open(urls: urls)
        }
    }

    /// Quitting is the last chance to save. Autosave is a safety net, not a save.
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard let engine else { return .terminateNow }
        return MainActor.assumeIsolated {
            guard engine.confirmDiscardingChanges() else { return .terminateCancel }
            // Editor hosts are child processes: orphaned, their windows outlive the DAW.
            engine.pluginEditors.closeAll()
            return .terminateNow
        }
    }
}

@main
struct NeuracoustApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @StateObject private var engine: EngineController
    @StateObject private var listen: ListenRoomController
    @StateObject private var ai: AiAssistantController

    init() {
        Diagnostics.shared.start()   // open the session log + capture stderr before anything else
        let engine = EngineController()
        let listen = ListenRoomController(engine: engine)
        engine.listenRoom = listen
        _engine = StateObject(wrappedValue: engine)
        _listen = StateObject(wrappedValue: listen)
        _ai = StateObject(wrappedValue: AiAssistantController(engine: engine))
    }

    var body: some Scene {
        Window("Neuracoust DAW", id: "main") {
            RootView()
                .environmentObject(engine)
                .environmentObject(engine.pluginEditors)
                .environmentObject(listen)
                .environmentObject(ai)
                .task {
                    appDelegate.engine = engine
                    // Auto-launch (login item) opens straight into the compact monitor station.
                    if engine.launchInMonitorMode { engine.compactMonitorMode = true }
                    engine.start()
                }
                .onDisappear {
                    // The relay is a child process; it must not outlive the window.
                    listen.shutdown()
                    engine.shutdown()
                }
        }
        .windowStyle(.hiddenTitleBar)
        .defaultSize(width: 1600, height: 980)
        // The shortcut is delivered by EngineController's NSEvent monitor, which
        // matches on key code. SwiftUI's keyboardShortcut matches on characters and
        // therefore never fires while a Korean input source is active.
        .commands {
            CommandGroup(replacing: .newItem) {
                Button("새 프로젝트") { engine.newProject() }
                    .keyboardShortcut("n", modifiers: .command)
                Button("열기…") { engine.openProject() }
                    .keyboardShortcut("o", modifiers: .command)
                Menu("최근 항목") {
                    if engine.recentProjects.isEmpty {
                        Button("최근 항목 없음") {}.disabled(true)
                    } else {
                        ForEach(engine.recentProjects, id: \.self) { url in
                            Button(url.deletingPathExtension().lastPathComponent) {
                                engine.openRecentProject(url)
                            }
                        }
                        Divider()
                        Button("최근 항목 지우기") { engine.clearRecentProjects() }
                    }
                }
            }
            CommandGroup(replacing: .saveItem) {
                Button("저장") { engine.saveProject() }
                    .keyboardShortcut("s", modifiers: .command)
                Button("다른 이름으로 저장…") { engine.saveProjectAs() }
                    .keyboardShortcut("s", modifiers: [.command, .shift])
                // Self-contained copy (collects external media), working document stays on the original.
                Button("복사본으로 저장 (미디어 수집)…") { engine.saveProjectCopy() }
            }
            CommandGroup(after: .saveItem) {
                Divider()
                Button("전체 설정 저장") { engine.saveAllSettings() }
                    .keyboardShortcut("s", modifiers: [.command, .option])
                Divider()
                Button("오디오 가져오기…") { engine.importAudio(intoTrack: 0) }
                    .keyboardShortcut("i", modifiers: [.command, .shift])   // Pro Tools
                Button("바운스…") { engine.bounceProject() }
                    .keyboardShortcut("b", modifiers: [.command, .option])  // Pro Tools
                    .disabled(engine.bouncing)
            }
            CommandGroup(replacing: .undoRedo) {
                Button("실행 취소\(engine.canUndo ? ": \(engine.undoStepName)" : "")") {
                    engine.undo()
                }
                .keyboardShortcut("z", modifiers: .command)
                .disabled(!engine.canUndo)

                Button("다시 실행\(engine.canRedo ? ": \(engine.redoStepName)" : "")") {
                    engine.redo()
                }
                .keyboardShortcut("z", modifiers: [.command, .shift])
                .disabled(!engine.canRedo)
            }

            // Declared so it is discoverable; the NSEvent monitor is what delivers it.
            CommandGroup(after: .undoRedo) {
                Button("마커 추가") { engine.addMarkerAtPlayhead() }
                    .keyboardShortcut("m", modifiers: .command)
            }
            CommandMenu("트랙") {
                Button("오디오 트랙 추가") { engine.addAudioTrack() }
                    .keyboardShortcut("n", modifiers: [.command, .shift])            // Pro Tools: New Track
                Button("악기 트랙 추가") { engine.addInstrumentTrack() }
                    .keyboardShortcut("n", modifiers: [.command, .shift, .option])
                Button("MIDI 트랙 추가") { engine.addMidiTrack() }
                Button("Aux(버스) 트랙 추가") { engine.addAuxTrack() }
                Divider()
                Button("선택 트랙 복제…") {
                    if let id = engine.selectedTrackId { engine.duplicateTrackTarget = id }
                }
                .keyboardShortcut("d", modifiers: [.command, .shift])
                .disabled(engine.selectedTrackId == nil)
                Button("선택 트랙 삭제") { engine.deleteSelectedTrack() }
                    .disabled(engine.selectedTrackId == nil)
            }
            CommandMenu("클립") {
                // Consolidate the selection into one audio file (Pro Tools ⌥⇧3). Number-key shortcut,
                // so the Korean-IME character-remap issue does not apply.
                Button("통합 (Consolidate)") { engine.consolidateSelection() }
                    .keyboardShortcut("3", modifiers: [.option, .shift])
                    .disabled(engine.selectedClipIds.isEmpty)
            }
            CommandMenu("AI") {
                // ⌥⌘A — ⇧⌘I now belongs to Import Audio (Pro Tools).
                Button(ai.open ? "AI 어시스턴트 닫기" : "AI 어시스턴트 열기") { ai.toggle() }
                    .keyboardShortcut("a", modifiers: [.command, .option])
            }
            // FabFilter-style Help menu: a checkmarked toggle for the hover hints, the
            // same helpMode the toolbar "?" flips. A Toggle renders with the ✓ in a menu.
            CommandGroup(replacing: .help) {
                Toggle("대화형 도움말 표시", isOn: $engine.helpMode)
            }
        }
    }
}

struct RootView: View {
    @EnvironmentObject private var engine: EngineController
    @EnvironmentObject private var ai: AiAssistantController

    var body: some View {
        ZStack {
            if engine.compactMonitorMode {
                MonitorStationShell()
            } else {
                fullDawView
            }
        }
        // Resize the window to fit the active mode (compact monitor vs full DAW), remembering the
        // DAW frame so expanding restores it. The engine keeps running across the switch.
        .background(WindowConfigurator(compact: engine.compactMonitorMode))
    }

    private var fullDawView: some View {
        VStack(spacing: 0) {
            TitleBar()
            TransportBar()
            StatusStrip()

            HStack(spacing: 0) {
                Group {
                    switch engine.viewTab {
                    case .edit: EditView()
                    case .mix: MixerView()
                    }
                }
                // The edit/mix area yields first as the window narrows (it may clip to nothing); the
                // monitor dock has priority so it is NEVER covered — it is the station the whole app is
                // built around. Turn the dock off to hide it; narrowing must not eat it.
                .frame(minWidth: 0, maxWidth: .infinity, maxHeight: .infinity)
                .layoutPriority(0)
                .clipped()

                if engine.showMonitorDock {
                    Rectangle().fill(Theme.Palette.deepBorder).frame(width: 1)
                    MonitorDock()
                        .frame(width: Theme.monitorDockWidth)
                        .layoutPriority(1)
                }
            }
        }
        .background(Theme.Palette.background)
        .preferredColorScheme(.dark)
        .coordinateSpace(name: "helpRoot")
        // A single top-level help tooltip, drawn beside whatever control is hovered while
        // help mode is on. Rendered here so it is never clipped by a toolbar's bounds.
        .overlay {
            if let hover = engine.helpHover, engine.helpMode {
                Text(hover.text)
                    .font(.system(size: 11, weight: .medium))
                    .foregroundStyle(.white)
                    .padding(.horizontal, 8).padding(.vertical, 5)
                    .background(RoundedRectangle(cornerRadius: 6).fill(Color.black.opacity(0.92)))
                    .overlay(RoundedRectangle(cornerRadius: 6).stroke(Color.white.opacity(0.18), lineWidth: 1))
                    .fixedSize()
                    .shadow(color: .black.opacity(0.5), radius: 6, y: 2)
                    .position(x: hover.frame.midX, y: hover.frame.maxY + 16)
                    .allowsHitTesting(false)
                    .transition(.opacity.animation(.easeOut(duration: 0.08)))
            }
        }
        .overlay {
            if engine.pluginBrowserOpen {
                PluginBrowser()
            }
        }
        .overlay(alignment: .topTrailing) {
            if ai.open {
                AiAssistantPanel(ai: ai)
            }
        }
        .overlay(alignment: .center) {
            if engine.monitorEqOpen {
                MonitorEqView()
            }
        }
        .overlay(alignment: .bottom) {
            BounceStatus()
        }
        .overlay(alignment: .top) {
            if let p = engine.stemSeparationProgress {
                HStack(spacing: Theme.Space.md) {
                    ProgressView().controlSize(.small).scaleEffect(0.7)
                    Text(engine.stemSeparationStatus).font(Theme.Font.ui(11, .medium))
                        .foregroundStyle(Theme.Palette.text)
                    ProgressView(value: p).frame(width: 120)
                }
                .padding(.horizontal, Theme.Space.lg).padding(.vertical, Theme.Space.md)
                .background(RoundedRectangle(cornerRadius: Theme.Radius.panel).fill(Theme.Palette.panel))
                .overlay(RoundedRectangle(cornerRadius: Theme.Radius.panel).stroke(Theme.Palette.divider, lineWidth: 1))
                .padding(.top, Theme.Space.lg)
                .transition(.move(edge: .top).combined(with: .opacity))
            }
        }
        .sheet(isPresented: Binding(
            get: { engine.spotTargetClipId != nil },
            set: { if !$0 { engine.spotTargetClipId = nil } }
        )) {
            SpotDialog()
        }
        .sheet(isPresented: Binding(
            get: { engine.duplicateTrackTarget != nil },
            set: { if !$0 { engine.duplicateTrackTarget = nil } }
        )) {
            DuplicateTrackDialog()
        }
    }
}

/// Track duplication options: copy everything, or drop clips/inserts/sends from the copy.
private struct DuplicateTrackDialog: View {
    @EnvironmentObject private var engine: EngineController
    @State private var includeClips = true
    @State private var includeInserts = true
    @State private var includeSends = true

    private var trackName: String {
        engine.tracks.first { $0.id == engine.duplicateTrackTarget }?.name ?? ""
    }

    var body: some View {
        VStack(alignment: .leading, spacing: Theme.Space.lg) {
            Text("트랙 복제")
                .font(Theme.Font.ui(12, .semibold))
                .foregroundStyle(Theme.Palette.textBright)
            Text(trackName)
                .font(Theme.Font.mono(9))
                .foregroundStyle(Theme.Palette.textFaint)
            VStack(alignment: .leading, spacing: Theme.Space.sm) {
                Toggle("클립 포함", isOn: $includeClips)
                Toggle("인서트 포함", isOn: $includeInserts)
                Toggle("센드 포함", isOn: $includeSends)
            }
            .toggleStyle(.checkbox)
            .font(Theme.Font.ui(10))
            .foregroundStyle(Theme.Palette.textSecondary)
            HStack {
                Spacer()
                Button("취소") { engine.duplicateTrackTarget = nil }
                Button("복제") {
                    if let id = engine.duplicateTrackTarget {
                        engine.duplicateTrack(id,
                                              includeClips: includeClips,
                                              includeInserts: includeInserts,
                                              includeSends: includeSends)
                    }
                }
                .keyboardShortcut(.defaultAction)
            }
        }
        .padding(Theme.Space.xxl)
        .frame(width: 280)
    }
}

/// Spot edit mode: type an exact start time (seconds) for the dropped clip.
private struct SpotDialog: View {
    @EnvironmentObject private var engine: EngineController
    @State private var seconds: String = ""

    private var clip: EngineController.Clip? {
        engine.clips.first { $0.id == engine.spotTargetClipId }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: Theme.Space.lg) {
            Text("스팟 — 정확한 위치 지정")
                .font(Theme.Font.ui(12, .semibold))
                .foregroundStyle(Theme.Palette.textBright)
            if let clip {
                Text(clip.name)
                    .font(Theme.Font.mono(9))
                    .foregroundStyle(Theme.Palette.textFaint)
            }
            HStack(spacing: Theme.Space.sm) {
                Text("시작 (초)")
                    .font(Theme.Font.ui(10))
                    .foregroundStyle(Theme.Palette.textSecondary)
                TextField("0.000", text: $seconds)
                    .textFieldStyle(.roundedBorder)
                    .frame(width: 120)
                    .onSubmit(place)
            }
            HStack {
                Spacer()
                Button("취소") { engine.spotTargetClipId = nil }
                Button("배치") { place() }
                    .keyboardShortcut(.defaultAction)
            }
        }
        .padding(Theme.Space.xxl)
        .frame(width: 300)
        .onAppear {
            if let clip { seconds = String(format: "%.3f", clip.startSeconds) }
        }
    }

    private func place() {
        guard let id = engine.spotTargetClipId, let value = Double(seconds) else {
            engine.spotTargetClipId = nil
            return
        }
        engine.spotPlaceClip(id, at: value)
    }
}

/// Reports its window-space frame + help text to the engine while hovered in help mode, so
/// the single top-level overlay in RootView can draw the tooltip beside it.
private struct HelpTipModifier: ViewModifier {
    @EnvironmentObject private var engine: EngineController
    let text: String
    @State private var frame: CGRect = .zero

    func body(content: Content) -> some View {
        content
            .background(
                GeometryReader { geo in
                    Color.clear
                        .onAppear { frame = geo.frame(in: .named("helpRoot")) }
                        .onChange(of: geo.frame(in: .named("helpRoot"))) { _, f in
                            frame = f
                            if engine.helpHover?.frame != .zero, engine.helpHover?.text == text {
                                engine.helpHover = .init(text: text, frame: f)
                            }
                        }
                }
            )
            .onHover { hovering in
                guard engine.helpMode, !text.isEmpty else {
                    if engine.helpHover?.frame == frame { engine.helpHover = nil }
                    return
                }
                if hovering {
                    engine.helpHover = .init(text: text, frame: frame)
                } else if engine.helpHover?.frame == frame {
                    engine.helpHover = nil
                }
            }
    }
}

extension View {
    /// Custom help tooltip (shown only in help mode), replacing the unreliable native .help.
    func helpTip(_ text: String) -> some View { modifier(HelpTipModifier(text: text)) }
}

// The three regions below are structural stubs. They hold the layout the design
// specifies so the shell can be verified against a running engine before the
// heavy AppKit/Metal views land inside them.

private struct EditView: View {
    @EnvironmentObject private var engine: EngineController

    var body: some View {
        // Flatten the parent's size proposal to a CONCRETE value and pin the content to it
        // (`.frame(width:height:)`, not `maxWidth/maxHeight: .infinity`). The timeline is an
        // AppKit NSView; given only a size RANGE it re-negotiates its height with SwiftUI every
        // layout pass and the whole track area trembles (Edit tab only; a Mix round-trip rebuilds
        // it and it settles). A definite size ends the negotiation. No conditional content here —
        // that (the width-based panel hiding) is what flickered before.
        GeometryReader { geo in
            HStack(spacing: 0) {
                // Nuendo-style side columns sit at the far left (like the mixer), so the edit
                // window's left margin matches it — the edit-tool rail moved in front of the
                // timeline where it belongs.
                if engine.showChannelColumn {
                    ChannelColumn()
                    Rectangle().fill(Theme.Palette.deepBorder).frame(width: 1)
                }
                if engine.showInspector {
                    TrackInspector()
                    Rectangle().fill(Theme.Palette.deepBorder).frame(width: 1)
                }

                VStack(spacing: 0) {
                    toolbar

                GlobalTracksBar()

                TimelineView(
                    model: engine.timelineModel,
                    playheadSeconds: engine.playheadSeconds,
                    isTransportRunning: engine.transportRunning || engine.recording,
                    waveforms: engine.waveforms,
                    recordingClips: engine.recordingClips,
                    recordingChannels: engine.recordingChannels,
                    onSeek: { engine.seek($0) },
                    onZoom: { engine.setViewport(start: $0, duration: $1) },
                    onSelect: { engine.selectClip($0) },
                    onSetRange: { engine.setLoopRange(start: $0, end: $1) },
                    onSetRangeLane: { engine.editRangeLane = $0 },
                    onSelectRegion: { engine.selectRegion($0) },
                    onOpenRegion: { engine.editingRegionId = $0 },
                    onMoveRegion: { engine.moveMidiRegion($0, laneIndex: $1, startSeconds: $2) },
                    onResizeRegion: { engine.resizeMidiRegion($0, durationSeconds: $1) },
                    onAddRegion: { engine.addMidiRegion(laneIndex: $0, startSeconds: $1) },
                    onDropAudio: { engine.dropAudio(onLane: $0, atSeconds: $1, urls: $2) },
                    onMoveMarker: { engine.moveMarker(from: $0, to: $1) },
                    onDeleteMarker: { engine.deleteMarker(at: $0) },
                    onSelectBetweenMarkers: { engine.selectBetweenMarkers(around: $0) },
                    onToggleAutomation: { engine.toggleAutomation(laneIndex: $0) },
                    onCycleAutomationParameter: { engine.cycleAutomationParameter(laneIndex: $0) },
                    onAutomationParamOptions: { lane in
                        let current = engine.laneTracks.indices.contains(lane)
                            ? engine.automationLanes[engine.laneTracks[lane].id]?.id : nil
                        return engine.automationParameterOptions(laneIndex: lane)
                            .map { (id: $0.id, name: $0.displayName, on: $0.id == current) }
                    },
                    onSetAutomationParam: { engine.setAutomationParameter(laneIndex: $0, id: $1) },
                    onSetLaneHeight: { engine.setLaneHeight(trackIds: $0, height: $1) },
                    onCommitLaneHeight: { engine.commitLaneHeight() },
                    onReorderTrack: { engine.moveTrackNear($0, targetId: $1, after: $2) },
                    onFadeCurveOptions: { EngineController.fadeCurves.map { (label: $0.label, id: $0.id) } },
                    onClipCurrentFades: { id in
                        let c = engine.clips.first { $0.id == id }
                        return (inCurve: c?.fadeInCurve ?? "equal_power", outCurve: c?.fadeOutCurve ?? "equal_power")
                    },
                    onSetClipFadeInCurve: { engine.setClipFadeInCurve($0, $1) },
                    onSetClipFadeOutCurve: { engine.setClipFadeOutCurve($0, $1) },
                    onReverseClip: { engine.reverseClip($0) },
                    onNormalizeClip: { engine.normalizeClip($0) },
                    onToggleClipMute: { engine.toggleClipMute($0) },
                    onToggleClipPolarity: { engine.toggleClipPolarity($0) },
                    onApplyClipTimePitch: { engine.applyClipTimePitch($0, timeRatio: $1, semitones: $2) },
                    onDenoiseClip: { engine.denoiseClip($0) },
                    onSeparateStems: { engine.separateClipStems($0) },
                    onOpenPitchEditor: { engine.openPitchEditor($0) },
                    onSetCrossfadeLength: { engine.setCrossfadeLength($0, $1, to: $2) },
                    onSetFadeCurvature: { engine.setClipFadeCurvature($0, fadeIn: $1, $2) },
                    auditionRoll: { engine.auditionRollSeconds },
                    onSetAuditionRoll: { engine.auditionRollSeconds = $0 },
                    onAuditionRegion: { engine.auditionRegion(from: $0, to: $1, loop: $2) },
                    onStopAudition: { engine.stopAudition() },
                    onClipOriginalStart: { engine.clipOriginalStart($0) },
                    onSpotClips: { engine.spotClipsToOriginal($0) },
                    onAddAutomationPoint: { engine.addAutomationPoint(laneIndex: $0, timeSeconds: $1, value: $2) },
                    onMoveAutomationPoint: { engine.moveAutomationPoint(laneIndex: $0, pointIndex: $1, timeSeconds: $2, value: $3) },
                    onDeleteAutomationPoint: { engine.deleteAutomationPoint(laneIndex: $0, pointIndex: $1) },
                    onToggleSelect: { engine.toggleClipSelection($0) },
                    onSelectMany: { engine.selectClips($0) },
                    onMoveClip: { engine.previewMoveClip($0, to: $1) },
                    onDropCopy: { engine.dropClipCopy($0, laneIndex: $1, startSeconds: $2) },
                    onDropCopyToNewTrack: { engine.dropClipCopyToNewTrack($0, startSeconds: $1) },
                    onSplitClip: { engine.splitClipAt($0, seconds: $1) },
                    editTool: engine.editTool.rawValue,
                    onBeginCopySelection: { engine.beginCopySelection(anchorId: $0) },
                    onMoveSelection: { engine.previewMoveSelection(by: $0) },
                    onTrimStart: { engine.trimClipStart($0, to: $1) },
                    onTrimEnd: { engine.trimClipEnd($0, to: $1) },
                    onRollBoundary: { engine.rollBoundary($0, $1, to: $2) },
                    onSetFades: { engine.setClipFades($0, fadeIn: $1, fadeOut: $2) },
                    onSetGain: { engine.setClipGain($0, $1) },
                    onCommitGain: { engine.commitClipGain($0) },
                    onSelectLane: { engine.selectLane($0, additive: $1) },
                    onMoveClipToLane: { engine.moveClipToLane($0, laneIndex: $1, startSeconds: $2) },
                    onDropClipToNewTrack: { engine.dropClipToNewTrack($0, startSeconds: $1) },
                    onCommitEdit: { engine.commitClipGesture($0) },
                    snap: { engine.snap($0) },
                    onToggleMute: { engine.toggleTrackMute($0) },
                    onToggleSolo: { engine.toggleTrackSolo($0) },
                    onToggleArm: { engine.toggleTrackArm($0) },
                    onToggleInputMonitor: { engine.toggleTrackInputMonitoring($0) },
                    onRenameTrack: { _ = engine.renameTrack($0, to: $1) },
                    onSetVolumeDb: { engine.setTrackVolume($0, $1) },
                    onSetPan: { engine.setTrackPan($0, $1) },
                    onCycleAutomationMode: { engine.cycleAutomationMode($0) },
                    onBeginTouch: { engine.beginAutomationTouch($0, $1) },
                    onEndTouch: { engine.endAutomationTouch($0, $1) },
                    onToggleTimebase: { engine.toggleRulerTimebase($0) },
                    onBrowseInsert: { engine.openPluginBrowser(forTrack: $0) },
                    onToggleInsertEditor: { engine.pluginEditors.toggle(trackId: $0, insertIndex: $1) },
                    onBypassInsert: { engine.toggleInsertBypass(track: $0, slot: $1) },
                    onRemoveInsert: { engine.removeInsert(track: $0, slot: $1) },
                    onAddSend: { engine.addSend($0, to: $1) },
                    onRemoveSend: { engine.removeSend($0, slot: $1) },
                    onSetSendGain: { engine.setSendGain($0, slot: $1, gainDb: $2) },
                    onSetSendPan: { engine.setSendPan($0, slot: $1, pan: $2) },
                    onSetSendPreFader: { engine.setSendPreFader($0, slot: $1, pre: $2) },
                    onAddAux: { engine.addAuxTrack() },
                    onSendBusOptions: { engine.sendBusOptions($0) }
                )

                PianoRollPanel()
                PitchEditorPanel()
            }
            }
            .frame(width: geo.size.width, height: geo.size.height)
        }
    }

    private var toolbar: some View {
        HStack(spacing: Theme.Space.sm) {
            toolSelector

            Rectangle().fill(Theme.Palette.divider).frame(width: 1, height: 16)

            zoomIconButton("minus.magnifyingglass", help: engine.tr("help.zoom_out")) { engine.zoomTimeline(by: 1.5) }
            zoomIconButton("arrow.up.left.and.down.right.magnifyingglass", help: engine.tr("help.zoom_fit")) { engine.fitTimeline() }
            zoomIconButton("plus.magnifyingglass", help: engine.tr("help.zoom_in")) { engine.zoomTimeline(by: 1 / 1.5) }

            Rectangle().fill(Theme.Palette.divider).frame(width: 1, height: 16)

            gridMenu

            Rectangle().fill(Theme.Palette.divider).frame(width: 1, height: 16)

            let hasSelection = !engine.selectedClipIds.isEmpty

            iconButton("square.split.2x1", help: engine.tr("help.split"), enabled: hasSelection) {
                engine.splitSelectedClipsAtPlayhead()
            }
            iconButton("arrow.triangle.merge", help: engine.tr("help.heal"), enabled: hasSelection || engine.hasEditRange) {
                engine.healSelectedClips()
            }
            iconButton("trash", help: engine.tr("help.delete"), enabled: hasSelection) {
                engine.deleteSelectedClips()
            }

            Rectangle().fill(Theme.Palette.divider).frame(width: 1, height: 16)

            iconButton("doc.on.doc", help: engine.tr("help.copy"), enabled: hasSelection) { engine.copySelectedClips() }
            iconButton("scissors", help: engine.tr("help.cut"), enabled: hasSelection) { engine.cutSelectedClips() }
            iconButton("doc.on.clipboard", help: engine.tr("help.paste"), enabled: engine.clipboardClipName != nil) { engine.pasteClipsAtPlayhead() }
            iconButton("plus.square.on.square", help: engine.tr("help.duplicate"), enabled: hasSelection) { engine.duplicateSelectedClips() }

            Rectangle().fill(Theme.Palette.divider).frame(width: 1, height: 16)

            // The four range edits live under one menu — the icons alone couldn't tell
            // them apart from the clip ops.
            let hasRange = engine.hasEditRange
            iconMenu("rectangle.dashed", help: engine.tr("help.range"), enabled: hasRange) {
                Button("구간 복사") { engine.copyRange() }
                Button("구간 잘라내기") { engine.cutRange() }
                Button("구간 지우기") { engine.clearRange() }
                Button("구간 분리") { engine.separateRange() }
                Divider()
                Button("구간의 컨덕터 지우기 (마커·코드·송폼…)") { engine.clearConductorInRange() }
                    .disabled(!engine.rangeHasConductor)
            }

            Rectangle().fill(Theme.Palette.divider).frame(width: 1, height: 16)

            iconButton("waveform.badge.plus", help: engine.tr("help.add_audio")) { engine.addAudioTrack() }
            iconButton("pianokeys", help: engine.tr("help.add_instrument")) { engine.addInstrumentTrack() }
            iconButton("music.note.list", help: engine.tr("help.add_midi")) { engine.addMidiTrack() }
            iconButton("rectangle.badge.minus", help: engine.tr("help.delete_track"), enabled: engine.canDeleteSelectedTrack) {
                engine.deleteSelectedTrack()
            }

            Spacer()
        }
        .padding(.horizontal, Theme.Space.xxl)
        .frame(height: 30)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Theme.Palette.ruler)
        // Buttons keep their size; when the window is too narrow they run off the right
        // edge and are simply clipped, rather than shrinking/truncating.
        .clipped()
        .overlay(alignment: .bottom) {
            Rectangle().fill(Theme.Palette.border).frame(height: 1)
        }
    }

    /// The edit-tool picker: each tool forces a mouse behaviour in the timeline.
    private var gridMenu: some View {
        Menu {
            Section("박자 단위 (템포 따라감)") {
                ForEach(EngineController.GridUnit.musicalCases) { gridUnitButton($0) }
            }
            Section("시간 단위 (고정)") {
                ForEach(EngineController.GridUnit.timeCases) { gridUnitButton($0) }
            }
        } label: {
            HStack(spacing: 4) {
                Text("#").font(Theme.Font.mono(11, .semibold))
                Text(engine.gridUnit.label).font(Theme.Font.mono(10))
            }
            .foregroundStyle(Theme.Palette.textSecondary)
            .padding(.horizontal, 8).frame(height: 22)
            .background(RoundedRectangle(cornerRadius: 5).fill(Theme.Palette.button))
        }
        .menuStyle(.borderlessButton).menuIndicator(.hidden).fixedSize()
        .helpTip(engine.tr("help.grid"))
    }

    private func gridUnitButton(_ unit: EngineController.GridUnit) -> some View {
        Button {
            engine.setGridUnit(unit)
        } label: {
            if engine.gridUnit == unit { Label(unit.label, systemImage: "checkmark") } else { Text(unit.label) }
        }
    }

    private var toolSelector: some View {
        HStack(spacing: Theme.Space.sm) {
            ForEach(EngineController.EditTool.allCases) { tool in
                let active = engine.editTool == tool
                Button { engine.editTool = tool } label: {
                    Image(systemName: tool.symbol)
                        .font(.system(size: 11, weight: .medium))
                        .foregroundStyle(active ? Theme.Palette.deepBorder : Theme.Palette.textDim)
                        .frame(width: 26, height: 22)
                        .background(
                            RoundedRectangle(cornerRadius: Theme.Radius.button)
                                .fill(active ? Theme.Palette.accent : Theme.Palette.button)
                                .overlay(
                                    RoundedRectangle(cornerRadius: Theme.Radius.button)
                                        .stroke(Theme.Palette.divider, lineWidth: 1)
                                )
                        )
                }
                .buttonStyle(.plain)
                .help(tool.label)
            }
        }
    }

    /// Icon variant for the zoom controls: magnifier −/+ and a fit-to-window glyph.
    private func zoomIconButton(_ systemImage: String, help: String,
                                action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: systemImage)
                .font(.system(size: 11, weight: .medium))
                .foregroundStyle(Theme.Palette.textSecondary)
                .frame(width: 26, height: 20)
                .background(
                    RoundedRectangle(cornerRadius: Theme.Radius.button)
                        .fill(Theme.Palette.button)
                        .overlay(RoundedRectangle(cornerRadius: Theme.Radius.button)
                            .stroke(Theme.Palette.divider, lineWidth: 1))
                )
        }
        .buttonStyle(.plain)
        .help(help)
    }

    /// A uniform icon button for the edit toolbar — every action is a same-size glyph
    /// with a tooltip carrying the name.
    private func iconButton(_ systemImage: String, help: String, enabled: Bool = true,
                            action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: systemImage)
                .font(.system(size: 11, weight: .medium))
                .foregroundStyle(enabled ? Theme.Palette.textSecondary : Theme.Palette.textFainter)
                .frame(width: 26, height: 22)
                .background(
                    RoundedRectangle(cornerRadius: Theme.Radius.button)
                        .fill(Theme.Palette.button)
                        .overlay(RoundedRectangle(cornerRadius: Theme.Radius.button)
                            .stroke(Theme.Palette.divider, lineWidth: 1))
                )
        }
        .buttonStyle(.plain).disabled(!enabled).helpTip(help)
    }

    /// Same look as iconButton, but opens a menu (used to group the range edits).
    private func iconMenu<Content: View>(_ systemImage: String, help: String, enabled: Bool = true,
                                         @ViewBuilder content: () -> Content) -> some View {
        Menu {
            content()
        } label: {
            Image(systemName: systemImage)
                .font(.system(size: 11, weight: .medium))
                .foregroundStyle(enabled ? Theme.Palette.textSecondary : Theme.Palette.textFainter)
                .frame(width: 26, height: 22)
                .background(
                    RoundedRectangle(cornerRadius: Theme.Radius.button)
                        .fill(Theme.Palette.button)
                        .overlay(RoundedRectangle(cornerRadius: Theme.Radius.button)
                            .stroke(Theme.Palette.divider, lineWidth: 1))
                )
        }
        .menuStyle(.borderlessButton).menuIndicator(.hidden).fixedSize().disabled(!enabled).helpTip(help)
    }

    private func zoomButton(_ title: String,
                            enabled: Bool = true,
                            action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(title)
                .font(Theme.Font.ui(9, .medium))
                .foregroundStyle(enabled ? Theme.Palette.textSecondary : Theme.Palette.textFainter)
                .fixedSize()                                    // keep full size; overflow clips
                .padding(.horizontal, Theme.Space.lg)
                .frame(height: 20)
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
}

/// Bounce runs off the main thread, so the app stays live; this is the only
/// signal that it is happening, and afterwards, what came out.
private struct BounceStatus: View {
    @EnvironmentObject private var engine: EngineController

    var body: some View {
        Group {
            if engine.bouncing {
                banner {
                    ProgressView()
                        .controlSize(.small)
                    Text("바운스 중…")
                        .font(Theme.Font.ui(10))
                        .foregroundStyle(Theme.Palette.text)
                }
            } else if let summary = engine.bounceSummary {
                banner {
                    Circle()
                        .fill(summary.clipped ? Theme.Palette.red : Theme.Palette.green)
                        .frame(width: 6, height: 6)
                    Text((summary.path as NSString).lastPathComponent)
                        .font(Theme.Font.ui(10, .medium))
                        .foregroundStyle(Theme.Palette.textBright)
                    Text(String(format: "%.1f s · 피크 %.1f dBFS", summary.durationSeconds, summary.peakDbfs))
                        .font(Theme.Font.mono(9))
                        .foregroundStyle(Theme.Palette.textDim)
                    if summary.clipped {
                        Text("클리핑")
                            .font(Theme.Font.mono(8, .semibold))
                            .foregroundStyle(Theme.Palette.red)
                    }
                    if summary.nearSilent {
                        Text("거의 무음")
                            .font(Theme.Font.mono(8, .semibold))
                            .foregroundStyle(Theme.Palette.amber)
                    }
                    Button("닫기") { engine.dismissBounceSummary() }
                        .buttonStyle(.plain)
                        .font(Theme.Font.ui(9))
                        .foregroundStyle(Theme.Palette.textFaint)
                }
            }
        }
        .padding(.bottom, Theme.Space.xxl)
    }

    private func banner<Content: View>(@ViewBuilder _ content: () -> Content) -> some View {
        HStack(spacing: Theme.Space.xl) {
            content()
        }
        .padding(.horizontal, Theme.Space.xxl)
        .padding(.vertical, Theme.Space.xl)
        .background(
            RoundedRectangle(cornerRadius: Theme.Radius.panel)
                .fill(Theme.Palette.toolbar)
                .overlay(
                    RoundedRectangle(cornerRadius: Theme.Radius.panel)
                        .stroke(Theme.Palette.divider, lineWidth: 1)
                )
                .shadow(color: .black.opacity(0.5), radius: 12, y: 6)
        )
    }
}

private struct PlaceholderLabel: View {
    let text: String
    init(_ text: String) { self.text = text }

    var body: some View {
        Text(text)
            .font(Theme.Font.ui(11))
            .foregroundStyle(Theme.Palette.textFainter)
    }
}
