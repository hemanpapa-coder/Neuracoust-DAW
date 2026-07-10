import SwiftUI

/// Finder hands files to the app through the delegate, not through SwiftUI.
final class AppDelegate: NSObject, NSApplicationDelegate {
    weak var engine: EngineController?

    func application(_ application: NSApplication, open urls: [URL]) {
        Task { @MainActor [weak self] in
            self?.engine?.open(urls: urls)
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
                    case .edit: EditViewPlaceholder()
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

/// The timeline itself is still to come. Until then this lists what the document
/// actually holds, so imports can be verified without a renderer.
private struct EditViewPlaceholder: View {
    @EnvironmentObject private var engine: EngineController

    var body: some View {
        HStack(spacing: 0) {
            Rectangle()
                .fill(Theme.Palette.rail)
                .frame(width: Theme.toolRailWidth)

            ZStack {
                Theme.Palette.surface

                if engine.clips.isEmpty {
                    PlaceholderLabel("Edit — 타임라인 (AppKit/Metal 예정) · ⌘I 로 오디오 가져오기")
                } else {
                    VStack(alignment: .leading, spacing: Theme.Space.md) {
                        Text("클립 \(engine.clips.count)개 · 타임라인 렌더러는 아직 없습니다")
                            .font(Theme.Font.ui(9))
                            .foregroundStyle(Theme.Palette.textFaint)

                        ForEach(engine.clips) { clip in
                            HStack(spacing: Theme.Space.xl) {
                                Text(clip.trackName)
                                    .font(Theme.Font.mono(9))
                                    .foregroundStyle(Theme.Palette.accent)
                                    .frame(width: 70, alignment: .leading)
                                Text(clip.name)
                                    .font(Theme.Font.ui(10, .medium))
                                    .foregroundStyle(Theme.Palette.text)
                                Spacer()
                                Text(String(format: "%.2f s → %.2f s",
                                            clip.startSeconds,
                                            clip.startSeconds + clip.durationSeconds))
                                    .font(Theme.Font.mono(9))
                                    .foregroundStyle(Theme.Palette.textDim)
                            }
                            .padding(.horizontal, Theme.Space.xl)
                            .padding(.vertical, Theme.Space.md)
                            .background(
                                RoundedRectangle(cornerRadius: Theme.Radius.clip)
                                    .fill(Theme.Palette.accent.opacity(0.10))
                            )
                        }
                        Spacer()
                    }
                    .padding(Theme.Space.xxl)
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
                }
            }
        }
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
