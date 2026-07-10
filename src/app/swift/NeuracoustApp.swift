import SwiftUI

@main
struct NeuracoustApp: App {
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
                .task { engine.start() }
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

private struct EditViewPlaceholder: View {
    var body: some View {
        HStack(spacing: 0) {
            Rectangle()
                .fill(Theme.Palette.rail)
                .frame(width: Theme.toolRailWidth)
            ZStack {
                Theme.Palette.surface
                PlaceholderLabel("Edit — 타임라인 (AppKit/Metal 예정)")
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
