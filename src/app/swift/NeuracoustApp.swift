import SwiftUI

@main
struct NeuracoustApp: App {
    @StateObject private var engine = EngineController()

    var body: some Scene {
        Window("Neuracoust DAW", id: "main") {
            RootView()
                .environmentObject(engine)
                .task { engine.start() }
                .onDisappear { engine.shutdown() }
        }
        .windowStyle(.hiddenTitleBar)
        .defaultSize(width: 1600, height: 980)
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
                    case .mix: MixViewPlaceholder()
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

private struct MixViewPlaceholder: View {
    var body: some View {
        ZStack {
            Theme.Palette.surface
            PlaceholderLabel("Mix — 채널 스트립 (예정)")
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
