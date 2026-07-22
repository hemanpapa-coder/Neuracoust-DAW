import SwiftUI
import AppKit

/// The compact "monitor station" shell — the always-available monitor controller the app collapses
/// down to. Only the monitor station shows (level, listen modes, sources, meters, DSP); a header
/// button expands back to the full DAW. The audio engine keeps running the whole time, so music the
/// user is monitoring never drops when switching modes.
struct MonitorStationShell: View {
    @EnvironmentObject private var engine: EngineController

    var body: some View {
        VStack(spacing: 0) {
            header
            Rectangle().fill(Theme.Palette.border).frame(height: 1)
            MonitorDock()
                .frame(maxHeight: .infinity)
            Rectangle().fill(Theme.Palette.border).frame(height: 1)
            footer
        }
        .frame(width: Theme.monitorDockWidth)
        .frame(maxHeight: .infinity)
        .background(Theme.Palette.background)
        .preferredColorScheme(.dark)
    }

    private var header: some View {
        HStack(spacing: Theme.Space.md) {
            // Transport — the DAW keeps playing behind the compact shell.
            Button { engine.togglePlay() } label: {
                Image(systemName: engine.transportRunning ? "pause.fill" : "play.fill")
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundStyle(engine.transportRunning ? Theme.Palette.teal : Theme.Palette.textSecondary)
                    .frame(width: 26, height: 22)
                    .background(RoundedRectangle(cornerRadius: 5).fill(Theme.Palette.button))
            }
            .buttonStyle(.plain)
            Button { engine.stop() } label: {
                Image(systemName: "stop.fill")
                    .font(.system(size: 11, weight: .semibold))
                    .foregroundStyle(Theme.Palette.textMuted)
                    .frame(width: 26, height: 22)
                    .background(RoundedRectangle(cornerRadius: 5).fill(Theme.Palette.button))
            }
            .buttonStyle(.plain)

            VStack(alignment: .leading, spacing: 0) {
                Text("Neuracoust")
                    .font(Theme.Font.ui(12, .bold))
                    .foregroundStyle(Theme.Palette.textBright)
                Text("모니터 스테이션")
                    .font(Theme.Font.ui(8))
                    .foregroundStyle(Theme.Palette.textMuted)
            }

            Spacer(minLength: 0)

            // Expand into the full DAW — the workspace the monitor shell hosts.
            Button { engine.expandToDaw() } label: {
                HStack(spacing: 4) {
                    Image(systemName: "rectangle.expand.vertical")
                        .font(.system(size: 10, weight: .semibold))
                    Text("DAW 열기").font(Theme.Font.ui(10, .semibold))
                }
                .foregroundStyle(Theme.Palette.deepBorder)
                .padding(.horizontal, 10).frame(height: 24)
                .background(RoundedRectangle(cornerRadius: 6).fill(Theme.Palette.teal))
            }
            .buttonStyle(.plain)
            .help("전체 DAW로 펼치기 (엔진·모니터는 그대로 유지됩니다)")
        }
        .padding(.horizontal, Theme.Space.lg)
        .frame(height: 44)
        .background(Theme.Palette.panel)
    }

    private var footer: some View {
        HStack(spacing: Theme.Space.md) {
            Toggle(isOn: $engine.launchAtLogin) {
                Text("로그인 시 자동 실행").font(Theme.Font.ui(9))
            }
            .toggleStyle(.switch)
            .controlSize(.mini)
            .tint(Theme.Palette.teal)
            .help("컴퓨터를 켜면 모니터 스테이션이 자동으로 뜹니다 (설치본에서 안정 동작).")

            Toggle(isOn: $engine.launchInMonitorMode) {
                Text("모니터 모드로 시작").font(Theme.Font.ui(9))
            }
            .toggleStyle(.switch)
            .controlSize(.mini)
            .tint(Theme.Palette.teal)
            .help("자동 실행 시 전체 DAW가 아니라 콤팩트 모니터로 시작합니다.")

            Spacer(minLength: 0)
        }
        .foregroundStyle(Theme.Palette.textSecondary)
        .padding(.horizontal, Theme.Space.lg)
        .frame(height: 36)
        .background(Theme.Palette.panel)
    }
}

/// Resizes the host NSWindow to fit the active mode — a compact monitor strip vs the full DAW —
/// and remembers the DAW frame so expanding restores exactly where it was. macOS only.
struct WindowConfigurator: NSViewRepresentable {
    let compact: Bool

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeNSView(context: Context) -> NSView {
        let view = NSView()
        DispatchQueue.main.async { context.coordinator.apply(compact: compact, window: view.window, anchor: view.window?.frame) }
        return view
    }

    func updateNSView(_ nsView: NSView, context: Context) {
        // Capture the window frame SYNCHRONOUSLY, before SwiftUI's own resize (from the mode-dependent
        // min width) runs — otherwise the async apply below reads an already-moved frame and the
        // right-edge anchor is lost (the monitor station appeared to jump).
        let anchor = nsView.window?.frame
        DispatchQueue.main.async { context.coordinator.apply(compact: compact, window: nsView.window, anchor: anchor) }
    }

    final class Coordinator {
        private var dawFrame: NSRect?
        private var lastCompact: Bool?

        func apply(compact: Bool, window: NSWindow?, anchor: NSRect?) {
            guard let window else { return }
            guard lastCompact != compact else { return }
            let previous = lastCompact
            lastCompact = compact
            let current = anchor ?? window.frame   // the pre-toggle frame — the true anchor

            if compact {
                if previous != true { dawFrame = current }   // remember where the DAW was
                let width = Theme.monitorDockWidth + 8
                let maxH = window.screen?.visibleFrame.height ?? 900
                let height = min(maxH - 40, 840)
                // Shrink onto the DAW's RIGHT edge — that is where the monitor dock already sat, so the
                // strip does not jump, and expanding again (which pins the right edge) grows leftward
                // from exactly here.
                let frame = NSRect(x: current.maxX - width, y: current.maxY - height, width: width, height: height)
                window.minSize = NSSize(width: width, height: 460)
                window.setFrame(frame, display: true, animate: false)
            } else {
                // A modest floor so the window is always recoverable and the monitor dock always fits;
                // above it the window narrows freely (toolbars clip off, dock keeps priority). Do NOT
                // set a large min here — it fought SwiftUI/the mixer and stranded the window off-screen.
                window.minSize = NSSize(width: Theme.monitorDockWidth + 120, height: 480)
                guard previous == true else { return }   // only reposition when expanding FROM compact
                // The monitor dock lives on the DAW's RIGHT edge, and the compact strip IS that dock —
                // so keep the strip's current RIGHT edge PINNED and grow the DAW leftward. The strip
                // never moves. Clamp the width to the screen and keep the left edge on-screen.
                let screen = window.screen?.visibleFrame ?? current
                let width = min(dawFrame?.size.width ?? 1600, screen.width)
                let height = min(dawFrame?.size.height ?? 980, screen.height)
                var x = current.maxX - width                 // right (monitor) edge fixed
                x = max(screen.minX, min(x, screen.maxX - width))
                var y = current.maxY - height                // top edge fixed
                y = max(screen.minY, min(y, screen.maxY - height))
                window.setFrame(NSRect(x: x, y: y, width: width, height: height), display: true, animate: false)
            }
        }
    }
}
