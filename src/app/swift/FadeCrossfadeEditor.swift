import SwiftUI
import AppKit

// Pro Tools / Cubase-style fade & crossfade editor, opened by double-clicking the lower half of a
// fade or crossfade region on the timeline. Spec: "Crossfade Editor.dc.html" in the
// "Pro Tools 스타일 DAW 디자인" project. The graph replicates the render's fadeCurveGain so the
// picture matches the sound; curve/length changes flow straight to the engine setters.

/// Fade shapes the editor offers — 1:1 with nc_clip_set_fade_curves ids and render fadeCurveGain.
let editorFadeCurves: [(label: String, id: String)] = [
    ("등파워", "equal_power"), ("리니어", "linear"), ("지수", "slow"), ("로그", "fast"),
]

/// The render's fade curve, replicated for the editor graph (linear=x, slow=x², fast=√x, else sin).
func editorFadeGain(_ curve: String, _ t: CGFloat) -> CGFloat {
    let x = max(0, min(1, t))
    switch curve {
    case "linear": return x
    case "slow": return x * x
    case "fast": return sqrt(x)
    default: return sin(x * .pi / 2)
    }
}

/// Continuous shape bend, mirroring the render's applyFadeCurvature (and TimelineNSView.fadeCurvature).
func editorFadeCurvature(_ t: CGFloat, _ curvature: Double) -> CGFloat {
    let x = max(0, min(1, t))
    let c = max(-1.0, min(1.0, curvature))
    if c == 0 { return x }
    return pow(x, CGFloat(pow(2.0, c * 2.0)))
}

enum FadeEdge { case fadeIn, fadeOut }

/// What the popover is editing — a single clip fade, or the crossfade between two clips.
enum FadeCrossfadeTarget {
    case fade(clipId: String, edge: FadeEdge)
    case crossfade(leftId: String, rightId: String)
}

/// Value + callback bundle handed to the SwiftUI popover. Curves/lengths seed @State; changes call
/// straight back to the EngineController setters (already reused from the context menu).
struct FadeCrossfadeEditorConfig {
    let target: FadeCrossfadeTarget
    let initialOutCurve: String   // for a fade this is the single curve
    let initialInCurve: String    // unused for a single fade
    let initialSeconds: Double
    let maxSeconds: Double        // clamp for the length field
    let initialCurvature: Double  // continuous shape bend [-1,1], driven by the graph drag
    let setOutCurve: (String) -> Void   // fade: the curve; crossfade: left clip's fade-out curve
    let setInCurve: (String) -> Void    // crossfade only: right clip's fade-in curve
    let setLength: (Double) -> Void
    let setCurvature: (Double) -> Void  // shape bend; fade: that edge, crossfade: both curves together
    let initialRoll: Double             // audition pre/post-roll seconds
    let setRoll: (Double) -> Void
    let audition: (Bool) -> Void  // play the region with pre/post-roll; Bool = loop it
    let stopAudition: () -> Void
    let remove: () -> Void
    let close: () -> Void
}

private let panelText = Color(red: 0xdd/255, green: 0xd5/255, blue: 0xc8/255)
private let panelMuted = Color(red: 0x8a/255, green: 0x7f/255, blue: 0x6e/255)
private let panelBG = Color(red: 0x28/255, green: 0x23/255, blue: 0x20/255)
private let graphBG = Color(red: 0x17/255, green: 0x13/255, blue: 0x0f/255)
private let amber = Color(red: 0xf0/255, green: 0xc6/255, blue: 0x74/255)
private let clipBlue = Color(red: 0x4a/255, green: 0x86/255, blue: 0xc0/255)
private let clipGreen = Color(red: 0x57/255, green: 0xb9/255, blue: 0x8a/255)

struct FadeCrossfadeEditorView: View {
    let config: FadeCrossfadeEditorConfig

    @State private var outCurve: String
    @State private var inCurve: String
    @State private var seconds: Double
    @State private var linkCurves: Bool = false
    @State private var loopAudition: Bool = false
    @State private var curvature: Double
    @State private var roll: Double
    /// Curvature at the start of a graph drag, so the shaping drag is relative (not jump-to-cursor).
    @State private var dragBaseCurvature: Double?
    /// Length at the start of an endpoint-node drag, so length scrubbing is relative.
    @State private var dragBaseSeconds: Double?

    init(config: FadeCrossfadeEditorConfig) {
        self.config = config
        _outCurve = State(initialValue: config.initialOutCurve)
        _inCurve = State(initialValue: config.initialInCurve)
        _seconds = State(initialValue: config.initialSeconds)
        _curvature = State(initialValue: config.initialCurvature)
        _roll = State(initialValue: config.initialRoll)
        // Start linked when both crossfade curves already match — the common case.
        _linkCurves = State(initialValue: config.initialOutCurve == config.initialInCurve)
    }

    private var isCrossfade: Bool {
        if case .crossfade = config.target { return true }
        return false
    }
    private var title: String {
        switch config.target {
        case .crossfade: return "크로스페이드"
        case .fade(_, let edge): return edge == .fadeIn ? "페이드 인" : "페이드 아웃"
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text(title).font(.system(size: 13, weight: .semibold)).foregroundColor(panelText)
                Spacer()
                Button(action: config.close) {
                    Image(systemName: "xmark").font(.system(size: 10, weight: .semibold))
                }.buttonStyle(.plain).foregroundColor(panelMuted)
            }

            graph
                .frame(height: isCrossfade ? 128 : 104)
                .background(graphBG)
                .clipShape(RoundedRectangle(cornerRadius: 6))
                .gesture(shapeDrag)
                .help("드래그해서 페이드 모양을 그리세요 (위=천천히, 아래=빠르게)")

            if isCrossfade {
                curveSection("아웃 커브 (앞 클립)", selected: outCurve) { set in pickOut(set) }
                curveSection("인 커브 (뒤 클립)", selected: inCurve) { set in pickIn(set) }
                Toggle(isOn: $linkCurves) {
                    Text("In=Out 링크").font(.system(size: 11)).foregroundColor(panelMuted)
                }
                .toggleStyle(.switch).controlSize(.mini)
                .onChange(of: linkCurves) { _, on in if on { pickIn(outCurve) } }
            } else {
                curveSection("커브", selected: outCurve) { set in
                    outCurve = set; config.setOutCurve(set)
                }
            }

            HStack(spacing: 8) {
                Text("길이").font(.system(size: 11)).foregroundColor(panelMuted)
                Text(String(format: "%d ms", Int((seconds * 1000).rounded())))
                    .font(.system(size: 12, design: .monospaced)).foregroundColor(panelText)
                    .frame(minWidth: 58, alignment: .trailing)
                    .padding(.vertical, 4).padding(.horizontal, 8)
                    .background(graphBG).clipShape(RoundedRectangle(cornerRadius: 5))
                Stepper("", value: Binding(
                    get: { seconds },
                    set: { new in
                        seconds = max(0, min(config.maxSeconds, new))
                        config.setLength(seconds)
                    }), in: 0...max(0.001, config.maxSeconds), step: 0.010)
                    .labelsHidden()
            }

            Divider().overlay(Color.black.opacity(0.4))
            HStack(spacing: 8) {
                Button { config.audition(loopAudition) } label: {
                    Label("오디션", systemImage: "play.fill").font(.system(size: 11, weight: .semibold))
                }
                .buttonStyle(.plain)
                .foregroundColor(Color(red: 0xdc/255, green: 0xea/255, blue: 0xf3/255))
                .padding(.vertical, 6).padding(.horizontal, 12)
                .background(Color(red: 0x2f/255, green: 0x4d/255, blue: 0x63/255))
                .clipShape(RoundedRectangle(cornerRadius: 6))
                Button {
                    loopAudition.toggle()
                    if !loopAudition { config.stopAudition() }
                } label: {
                    Image(systemName: "repeat").font(.system(size: 11, weight: .semibold))
                        .foregroundColor(loopAudition ? Color(red: 0x1c/255, green: 0x15/255, blue: 0x09/255) : panelMuted)
                        .padding(6)
                        .background(loopAudition ? amber : Color.white.opacity(0.05))
                        .clipShape(RoundedRectangle(cornerRadius: 6))
                }.buttonStyle(.plain)
                // Audition lead-in / tail (seconds before and after the region).
                HStack(spacing: 2) {
                    Text(String(format: "±%.1fs", roll)).font(.system(size: 10, design: .monospaced))
                        .foregroundColor(panelMuted)
                    Stepper("", value: Binding(
                        get: { roll },
                        set: { roll = max(0, min(4, $0)); config.setRoll(roll) }), in: 0...4, step: 0.5)
                        .labelsHidden().controlSize(.mini)
                }
                Button("제거") { config.remove(); config.close() }
                    .buttonStyle(.plain).font(.system(size: 11, weight: .semibold)).foregroundColor(panelMuted)
                Spacer()
                Button("완료") { config.close() }
                    .buttonStyle(.plain).font(.system(size: 11, weight: .semibold))
                    .foregroundColor(Color(red: 0x1c/255, green: 0x15/255, blue: 0x09/255))
                    .padding(.vertical, 6).padding(.horizontal, 14)
                    .background(amber).clipShape(RoundedRectangle(cornerRadius: 6))
            }
        }
        .padding(14)
        .frame(width: isCrossfade ? 340 : 288)
        .background(panelBG)
    }

    private func pickOut(_ set: String) {
        outCurve = set; config.setOutCurve(set)
        if linkCurves { inCurve = set; config.setInCurve(set) }
    }
    private func pickIn(_ set: String) {
        inCurve = set; config.setInCurve(set)
        if linkCurves { outCurve = set; config.setOutCurve(set) }
    }

    /// Drag the graph vertically to SHAPE the fade curve (Pro Tools fade tension) — pull up for a
    /// slower ease, down for a faster one. Relative to where the drag began. Length stays on the
    /// stepper, so the drag never moves clips (the old length-drag slid the back clip).
    private var shapeDrag: some Gesture {
        DragGesture(minimumDistance: 3)
            .onChanged { value in
                let base = dragBaseCurvature ?? curvature
                if dragBaseCurvature == nil { dragBaseCurvature = base }
                // Up (negative height) → more convex (positive curvature); ~120 px = full range.
                curvature = max(-1, min(1, base - Double(value.translation.height) / 120.0))
                config.setCurvature(curvature)
            }
            .onEnded { _ in dragBaseCurvature = nil }
    }

    private func curveSection(_ label: String, selected: String, _ pick: @escaping (String) -> Void) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(label).font(.system(size: 10, weight: .semibold)).foregroundColor(panelMuted)
                .textCase(.uppercase)
            HStack(spacing: 6) {
                ForEach(editorFadeCurves, id: \.id) { curve in
                    let on = curve.id == selected
                    Text(curve.label)
                        .font(.system(size: 11, weight: on ? .semibold : .regular))
                        .foregroundColor(on ? Color(red: 0x1c/255, green: 0x15/255, blue: 0x09/255) : panelText)
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 6)
                        .background(on ? amber : Color.white.opacity(0.05))
                        .clipShape(RoundedRectangle(cornerRadius: 6))
                        .contentShape(Rectangle())
                        .onTapGesture { pick(curve.id) }
                }
            }
        }
    }

    /// Endpoint node(s) at the full-level corner(s), each with a drag SIGN so that pulling the node
    /// OUTWARD (away from the crossing) always lengthens and inward shortens — the natural feel. A
    /// crossfade has both top corners; a single fade has the one at its full-level end.
    private func endpoints(_ w: CGFloat, isIn: Bool) -> [(x: CGFloat, sign: Double)] {
        if isCrossfade { return [(7, -1), (w - 7, 1)] }
        return [isIn ? (w - 7, 1) : (7, -1)]
    }

    /// Drag an endpoint node to change the fade/crossfade length; `sign` makes outward = longer. Safe:
    /// the crossfade length trims the front clip, so nothing slides the back clip.
    private func lengthHandleDrag(_ w: CGFloat, _ sign: Double) -> some Gesture {
        DragGesture(minimumDistance: 1)
            .onChanged { v in
                let base = dragBaseSeconds ?? seconds
                if dragBaseSeconds == nil { dragBaseSeconds = base }
                let scale = max(0.001, config.maxSeconds) / Double(max(1, w))
                seconds = max(0, min(config.maxSeconds, base + sign * Double(v.translation.width) * scale))
                config.setLength(seconds)
            }
            .onEnded { _ in dragBaseSeconds = nil }
    }

    /// The crossfade shows two crossing curves over blue (left) / green (right) halves; a single fade
    /// shows one curve with the lost region darkened. Full level = top, silence = bottom. Overlaid
    /// endpoint nodes drag the length; the middle handle (and vertical drag) bends the curve.
    private var graph: some View {
        GeometryReader { geo in
            let gw = geo.size.width
            let isIn = { if case .fade(_, let e) = config.target { return e == .fadeIn }; return false }()
            ZStack(alignment: .topLeading) {
                curveCanvas
                ForEach(endpoints(gw, isIn: isIn), id: \.x) { node in
                    Circle().fill(Color.white)
                        .overlay(Circle().stroke(Color(red: 0x8a/255, green: 0x6a/255, blue: 0x1e/255), lineWidth: 1))
                        .frame(width: 11, height: 11)
                        .position(x: node.x, y: 8)
                        .highPriorityGesture(lengthHandleDrag(gw, node.sign))
                }
            }
        }
    }

    private var curveCanvas: some View {
        Canvas { ctx, size in
            let w = size.width, h = size.height
            let pad: CGFloat = 8
            let top = pad, bot = h - pad
            func y(_ gain: CGFloat) -> CGFloat { top + (1 - gain) * (bot - top) }

            if isCrossfade {
                ctx.fill(Path(CGRect(x: 0, y: 0, width: w, height: h)), with: .color(clipBlue.opacity(0.10)))
                ctx.fill(Path(CGRect(x: w/2, y: 0, width: w/2, height: h)), with: .color(clipGreen.opacity(0.10)))
                var outP = Path(), inP = Path()
                let steps = 40
                for i in 0...steps {
                    let t = CGFloat(i) / CGFloat(steps)
                    let px = t * w
                    let outY = y(editorFadeGain(outCurve, editorFadeCurvature(1 - t, curvature)))
                    let inY = y(editorFadeGain(inCurve, editorFadeCurvature(t, curvature)))
                    if i == 0 { outP.move(to: CGPoint(x: px, y: outY)); inP.move(to: CGPoint(x: px, y: inY)) }
                    else { outP.addLine(to: CGPoint(x: px, y: outY)); inP.addLine(to: CGPoint(x: px, y: inY)) }
                }
                ctx.stroke(outP, with: .color(clipBlue), lineWidth: 2)
                ctx.stroke(inP, with: .color(clipGreen), lineWidth: 2)
                // Middle shape handle at the crossing point.
                let midGain = editorFadeGain(inCurve, editorFadeCurvature(0.5, curvature))
                ctx.fill(Path(ellipseIn: CGRect(x: w/2 - 4, y: y(midGain) - 4, width: 8, height: 8)), with: .color(.white))
            } else {
                let isIn = { if case .fade(_, let e) = config.target { return e == .fadeIn }; return false }()
                var curve = Path()
                let steps = 40
                for i in 0...steps {
                    let t = CGFloat(i) / CGFloat(steps)
                    let px = t * w
                    let warped = editorFadeCurvature(isIn ? t : 1 - t, curvature)
                    if i == 0 { curve.move(to: CGPoint(x: px, y: y(editorFadeGain(outCurve, warped)))) }
                    else { curve.addLine(to: CGPoint(x: px, y: y(editorFadeGain(outCurve, warped)))) }
                }
                ctx.stroke(curve, with: .color(amber), lineWidth: 2)
                let midGain = editorFadeGain(outCurve, editorFadeCurvature(isIn ? 0.5 : 0.5, curvature))
                ctx.fill(Path(ellipseIn: CGRect(x: w/2 - 4, y: y(midGain) - 4, width: 8, height: 8)), with: .color(.white))
            }
        }
    }
}
