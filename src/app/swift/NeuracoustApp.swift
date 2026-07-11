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
                Divider()
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

                Rectangle().fill(Theme.Palette.deepBorder).frame(width: 1)

                MonitorDock()
                    .frame(width: Theme.monitorDockWidth)
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

            VStack(spacing: 0) {
                toolbar

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
                    onAddAutomationPoint: { engine.addAutomationPoint(laneIndex: $0, timeSeconds: $1, value: $2) },
                    onMoveAutomationPoint: { engine.moveAutomationPoint(laneIndex: $0, pointIndex: $1, timeSeconds: $2, value: $3) },
                    onDeleteAutomationPoint: { engine.deleteAutomationPoint(laneIndex: $0, pointIndex: $1) },
                    onToggleSelect: { engine.toggleClipSelection($0) },
                    onSelectMany: { engine.selectClips($0) },
                    onMoveClip: { engine.moveClip($0, to: $1) },
                    onBeginCopyDrag: { engine.duplicateClipForDrag($0, at: $1) },
                    onMoveSelection: { engine.moveSelection(by: $0) },
                    onTrimStart: { engine.trimClipStart($0, to: $1) },
                    onTrimEnd: { engine.trimClipEnd($0, to: $1) },
                    onSetFades: { engine.setClipFades($0, fadeIn: $1, fadeOut: $2) },
                    onSetGain: { engine.setClipGain($0, $1) },
                    onSelectLane: { engine.selectLane($0) },
                    onMoveClipToLane: { engine.moveClipToLane($0, laneIndex: $1, startSeconds: $2) },
                    onCommitEdit: { engine.commitClipGesture($0) },
                    snap: { engine.snap($0) }
                )

                PianoRollPanel()
            }
        }
    }

    private var toolbar: some View {
        HStack(spacing: Theme.Space.sm) {
            zoomButton("줌-") { engine.zoomTimeline(by: 1.5) }
            zoomButton("맞춤") { engine.fitTimeline() }
            zoomButton("줌+") { engine.zoomTimeline(by: 1 / 1.5) }

            Rectangle().fill(Theme.Palette.divider).frame(width: 1, height: 16)

            let hasSelection = !engine.selectedClipIds.isEmpty

            zoomButton("분할 (B)", enabled: hasSelection) {
                engine.splitSelectedClipsAtPlayhead()
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
