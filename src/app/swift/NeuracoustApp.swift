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
            engine.confirmDiscardingChanges() ? .terminateNow : .terminateCancel
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
                    onZoom: { engine.setViewport(start: $0, duration: $1) }
                )
            }
        }
    }

    private var toolbar: some View {
        HStack(spacing: Theme.Space.sm) {
            zoomButton("줌-") { engine.zoomTimeline(by: 1.5) }
            zoomButton("맞춤") { engine.fitTimeline() }
            zoomButton("줌+") { engine.zoomTimeline(by: 1 / 1.5) }

            Text(String(format: "%.1f s 표시 · 스크롤: 이동 · ⌘스크롤: 확대 · 클릭: 이동", engine.visibleDuration))
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

    private func zoomButton(_ title: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(title)
                .font(Theme.Font.ui(9, .medium))
                .foregroundStyle(Theme.Palette.textSecondary)
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
