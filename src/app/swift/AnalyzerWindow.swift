import AppKit
import SwiftUI

/// The analyzer types the popouts can show. Spectrum is live; the others are wired as
/// the engine gains the DSP for them (goniometer, LUFS, true-peak, spectrogram).
enum AnalyzerKind: String, CaseIterable, Identifiable {
    case spectrum, goniometer, loudness, truePeak, spectrogram
    var id: String { rawValue }
    var label: String {
        switch self {
        case .spectrum: return "스펙트럼"
        case .goniometer: return "고니오미터"
        case .loudness: return "라우드니스 (LUFS)"
        case .truePeak: return "트루 피크"
        case .spectrogram: return "스펙트로그램"
        }
    }
    /// Spectrum, goniometer and loudness are live; the spectrogram still needs its DSP.
    /// (True-peak is shown inside the loudness panel.)
    var isAvailable: Bool { self != .spectrogram && self != .truePeak }
}

/// Owns the floating analyzer windows. Each `open` mints a new window the user can drag
/// anywhere and place independently, so several analyzers can be watched at once.
@MainActor
final class AnalyzerWindowManager {
    static let shared = AnalyzerWindowManager()
    private var windows: [NSWindow] = []

    func open(kind: AnalyzerKind, engine: EngineController) {
        let host = NSHostingController(
            rootView: AnalyzerWindowView(kind: kind).environmentObject(engine))
        let window = NSWindow(contentViewController: host)
        window.styleMask = [.titled, .closable, .miniaturizable, .resizable]
        window.title = "분석 — \(kind.label)"
        window.setContentSize(NSSize(width: 720, height: 320))
        window.isReleasedWhenClosed = false
        window.titlebarAppearsTransparent = true
        // Cascade so a second popup does not land exactly on the first.
        let offset = CGFloat(windows.count % 8) * 26
        window.cascadeTopLeft(from: NSPoint(x: 140 + offset, y: 720 - offset))
        window.makeKeyAndOrderFront(nil)
        windows.append(window)

        NotificationCenter.default.addObserver(forName: NSWindow.willCloseNotification,
                                               object: window, queue: .main) { [weak self, weak window] _ in
            MainActor.assumeIsolated {
                self?.windows.removeAll { $0 === window }
            }
        }
    }
}

/// The large analyzer surface. Right-click picks the type shown here and can spawn
/// another popup — so analyzers can be arranged anywhere on screen.
private struct AnalyzerWindowView: View {
    @EnvironmentObject private var engine: EngineController
    @State var kind: AnalyzerKind

    var body: some View {
        ZStack {
            Color.black.opacity(0.9).ignoresSafeArea()
            content
                .padding(12)
        }
        .frame(minWidth: 320, minHeight: 160)
        .contextMenu {
            Menu("이 창에서 보기") {
                ForEach(AnalyzerKind.allCases) { k in
                    Button(k.label + (k.isAvailable ? "" : " (준비 중)")) { kind = k }
                        .disabled(!k.isAvailable)
                }
            }
            Divider()
            Menu("새 팝업 열기") {
                ForEach(AnalyzerKind.allCases) { k in
                    Button(k.label + (k.isAvailable ? "" : " (준비 중)")) {
                        AnalyzerWindowManager.shared.open(kind: k, engine: engine)
                    }
                    .disabled(!k.isAvailable)
                }
            }
        }
    }

    @ViewBuilder private var content: some View {
        switch kind {
        case .spectrum:
            SpectrumAnalyzerView(bins: engine.spectrumBins, sampleRate: engine.sampleRate)
        case .goniometer:
            GoniometerView(samples: engine.goniometerSamples)
        case .loudness:
            LoudnessView(momentary: engine.momentaryLufs,
                         shortTerm: engine.shortTermLufs,
                         integrated: engine.integratedLufs,
                         range: engine.loudnessRange,
                         truePeak: engine.truePeakDb)
        default:
            VStack(spacing: 8) {
                Text(kind.label)
                    .font(.system(size: 13, weight: .semibold))
                    .foregroundStyle(.white.opacity(0.8))
                Text("이 분석기는 엔진 DSP 준비 후 제공됩니다.")
                    .font(.system(size: 10))
                    .foregroundStyle(.white.opacity(0.45))
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }
}
