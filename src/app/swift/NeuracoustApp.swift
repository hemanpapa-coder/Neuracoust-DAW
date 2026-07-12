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

    init() {
        let engine = EngineController()
        let listen = ListenRoomController(engine: engine)
        engine.listenRoom = listen
        _engine = StateObject(wrappedValue: engine)
        _listen = StateObject(wrappedValue: listen)
    }

    var body: some Scene {
        Window("Neuracoust DAW", id: "main") {
            RootView()
                .environmentObject(engine)
                .environmentObject(engine.pluginEditors)
                .environmentObject(listen)
                .task {
                    appDelegate.engine = engine
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
            }
            CommandGroup(replacing: .saveItem) {
                Button("저장") { engine.saveProject() }
                    .keyboardShortcut("s", modifiers: .command)
                Button("다른 이름으로 저장…") { engine.saveProjectAs() }
                    .keyboardShortcut("s", modifiers: [.command, .shift])
            }
            CommandGroup(after: .saveItem) {
                Divider()
                Button("전체 설정 저장") { engine.saveAllSettings() }
                    .keyboardShortcut("s", modifiers: [.command, .option])
                Divider()
                Button("오디오 가져오기…") { engine.importAudio(intoTrack: 0) }
                    .keyboardShortcut("i", modifiers: .command)
                Button("바운스…") { engine.bounceProject() }
                    .keyboardShortcut("e", modifiers: .command)
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
                    .keyboardShortcut("t", modifiers: .command)
                Button("악기 트랙 추가") { engine.addInstrumentTrack() }
                    .keyboardShortcut("t", modifiers: [.command, .shift])
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
        }
    }
}

struct RootView: View {
    @EnvironmentObject private var engine: EngineController

    var body: some View {
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
                .frame(maxWidth: .infinity, maxHeight: .infinity)

                if engine.showMonitorDock {
                    Rectangle().fill(Theme.Palette.deepBorder).frame(width: 1)
                    MonitorDock()
                        .frame(width: Theme.monitorDockWidth)
                }
            }
        }
        .background(Theme.Palette.background)
        .preferredColorScheme(.dark)
        .overlay {
            if engine.pluginBrowserOpen {
                PluginBrowser()
            }
        }
        .overlay(alignment: .bottom) {
            BounceStatus()
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

// The three regions below are structural stubs. They hold the layout the design
// specifies so the shell can be verified against a running engine before the
// heavy AppKit/Metal views land inside them.

private struct EditView: View {
    @EnvironmentObject private var engine: EngineController

    var body: some View {
        HStack(spacing: 0) {
            Rectangle()
                .fill(Theme.Palette.rail)
                .frame(width: Theme.toolRailWidth)

            // Nuendo-style side columns: the Channel (existing mixer strip) and the
            // Inspector, both following the selected track and toggled from the transport.
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
                    waveforms: engine.waveforms,
                    onSeek: { engine.seek($0) },
                    onZoom: { engine.setViewport(start: $0, duration: $1) },
                    onSelect: { engine.selectClip($0) },
                    onSetRange: { engine.setLoopRange(start: $0, end: $1) },
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
                    onFadeCurveOptions: { EngineController.fadeCurves.map { (label: $0.label, id: $0.id) } },
                    onClipCurrentFades: { id in
                        let c = engine.clips.first { $0.id == id }
                        return (inCurve: c?.fadeInCurve ?? "equal_power", outCurve: c?.fadeOutCurve ?? "equal_power")
                    },
                    onSetClipFadeInCurve: { engine.setClipFadeInCurve($0, $1) },
                    onSetClipFadeOutCurve: { engine.setClipFadeOutCurve($0, $1) },
                    onAddAutomationPoint: { engine.addAutomationPoint(laneIndex: $0, timeSeconds: $1, value: $2) },
                    onMoveAutomationPoint: { engine.moveAutomationPoint(laneIndex: $0, pointIndex: $1, timeSeconds: $2, value: $3) },
                    onDeleteAutomationPoint: { engine.deleteAutomationPoint(laneIndex: $0, pointIndex: $1) },
                    onToggleSelect: { engine.toggleClipSelection($0) },
                    onSelectMany: { engine.selectClips($0) },
                    onMoveClip: { engine.moveClip($0, to: $1) },
                    onDropCopy: { engine.dropClipCopy($0, laneIndex: $1, startSeconds: $2) },
                    onDropCopyToNewTrack: { engine.dropClipCopyToNewTrack($0, startSeconds: $1) },
                    onSplitClip: { engine.splitClipAt($0, seconds: $1) },
                    editTool: engine.editTool.rawValue,
                    onMoveSelection: { engine.moveSelection(by: $0) },
                    onTrimStart: { engine.trimClipStart($0, to: $1) },
                    onTrimEnd: { engine.trimClipEnd($0, to: $1) },
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
            }
        }
    }

    private var toolbar: some View {
        HStack(spacing: Theme.Space.sm) {
            toolSelector

            Rectangle().fill(Theme.Palette.divider).frame(width: 1, height: 16)

            zoomButton("줌-") { engine.zoomTimeline(by: 1.5) }
            zoomButton("맞춤") { engine.fitTimeline() }
            zoomButton("줌+") { engine.zoomTimeline(by: 1 / 1.5) }

            Rectangle().fill(Theme.Palette.divider).frame(width: 1, height: 16)

            gridMenu

            Rectangle().fill(Theme.Palette.divider).frame(width: 1, height: 16)

            let hasSelection = !engine.selectedClipIds.isEmpty

            zoomButton("분할 (B)", enabled: hasSelection) {
                engine.splitSelectedClipsAtPlayhead()
            }
            zoomButton("붙이기 (H)", enabled: hasSelection || engine.hasEditRange) {
                engine.healSelectedClips()
            }
            zoomButton("삭제", enabled: hasSelection) {
                engine.deleteSelectedClips()
            }

            Rectangle().fill(Theme.Palette.divider).frame(width: 1, height: 16)

            zoomButton("복사", enabled: hasSelection) { engine.copySelectedClips() }
            zoomButton("잘라내기", enabled: hasSelection) { engine.cutSelectedClips() }
            zoomButton("붙여넣기", enabled: engine.clipboardClipName != nil) { engine.pasteClipsAtPlayhead() }
            zoomButton("복제", enabled: hasSelection) { engine.duplicateSelectedClips() }

            Rectangle().fill(Theme.Palette.divider).frame(width: 1, height: 16)

            zoomButton("마커 (⌘M)") { engine.addMarkerAtPlayhead() }

            Rectangle().fill(Theme.Palette.divider).frame(width: 1, height: 16)

            // Range edits act on the loop range, dragged out along the top of the ruler.
            let hasRange = engine.hasEditRange
            zoomButton("구간 복사", enabled: hasRange) { engine.copyRange() }
            zoomButton("구간 잘라내기", enabled: hasRange) { engine.cutRange() }
            zoomButton("구간 지우기", enabled: hasRange) { engine.clearRange() }
            zoomButton("구간 분리", enabled: hasRange) { engine.separateRange() }

            Rectangle().fill(Theme.Palette.divider).frame(width: 1, height: 16)

            zoomButton("+오디오") { engine.addAudioTrack() }
            zoomButton("+악기") { engine.addInstrumentTrack() }
            zoomButton("트랙 삭제", enabled: engine.canDeleteSelectedTrack) {
                engine.deleteSelectedTrack()
            }

            Text(String(format: "%.1f s 표시 · 드래그: 이동 · 가장자리: 트림 · B: 분할 · Delete: 삭제", engine.visibleDuration))
                .font(Theme.Font.mono(8))
                .foregroundStyle(Theme.Palette.textFainter)

            Spacer()
        }
        .padding(.horizontal, Theme.Space.xxl)
        .frame(height: 30)
        .frame(maxWidth: .infinity)
        .background(Theme.Palette.ruler)
        .overlay(alignment: .bottom) {
            Rectangle().fill(Theme.Palette.border).frame(height: 1)
        }
    }

    /// The edit-tool picker: each tool forces a mouse behaviour in the timeline.
    private var gridMenu: some View {
        Menu {
            ForEach(EngineController.GridUnit.allCases) { unit in
                Button {
                    engine.setGridUnit(unit)
                } label: {
                    if engine.gridUnit == unit { Label(unit.label, systemImage: "checkmark") } else { Text(unit.label) }
                }
            }
        } label: {
            HStack(spacing: 3) {
                Image(systemName: "grid").font(.system(size: 9))
                Text("그리드: \(engine.gridUnit.label)").font(Theme.Font.mono(10))
            }
            .foregroundStyle(Theme.Palette.textSecondary)
            .padding(.horizontal, 8).frame(height: 22)
            .background(RoundedRectangle(cornerRadius: 5).fill(Theme.Palette.button))
        }
        .menuStyle(.borderlessButton).menuIndicator(.hidden).fixedSize()
        .help("그리드 해상도 (스냅 단위)")
    }

    private var toolSelector: some View {
        HStack(spacing: 2) {
            ForEach(EngineController.EditTool.allCases) { tool in
                let active = engine.editTool == tool
                Button { engine.editTool = tool } label: {
                    Image(systemName: tool.symbol)
                        .font(.system(size: 10))
                        .foregroundStyle(active ? Theme.Palette.deepBorder : Theme.Palette.textDim)
                        .frame(width: 24, height: 20)
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

    private func zoomButton(_ title: String,
                            enabled: Bool = true,
                            action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(title)
                .font(Theme.Font.ui(9, .medium))
                .foregroundStyle(enabled ? Theme.Palette.textSecondary : Theme.Palette.textFainter)
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
