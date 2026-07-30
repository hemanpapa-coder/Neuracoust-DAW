import SwiftUI
import AppKit

/// The API 525A compressor faceplate — a skeuomorphic model plate rendered from the bundled
/// UI kit (Resources/api525a): a silkscreened backboard, 128-frame film-strip knobs, 2-frame
/// buttons, LED overlays and a real VU-style gain-reduction needle. Shown in place of the SSL
/// COMPRESS module when the strip's comp model is "API 525A".
///
/// Control mapping (documented approximation, the project's honesty rule):
///   IN THRESH   → compThresholdDb (−20…+10 dB)
///   OUT MAKE-UP → compMakeupDb (0…20 dB, wet-path post gain)
///   ATTACK      → compAttackMs, stepped 15µ/.25/1/2/5/10/15 ms (15µ lands on the 0.1 ms floor)
///   REL button  → compReleaseMs ladder 2.0/0.5/0.2/0.05 s (the kit's four LED sockets)
///   C (2:1)     → compRatio 2 · L (20:1) → compRatio 20 (a radio pair)
///   BYP         → compEnabled, inverted
///   CEILING     → compCeilingDb 0…20: threshold down AND make-up up by the same dB —
///                 "COMPRESSION+GAIN", the 525's one-knob more-of-everything control.
///
/// All geometry comes from the kit's manifest (panel units, 182×640); the 2x assets render
/// crisp at any strip width ≤ 364 pt.

// MARK: kit geometry (manifest.json, panel units)

private enum K {
    static let panel = CGSize(width: 182, height: 640)

    struct Knob {
        let asset: String
        let frames: Int
        let frameSize: CGFloat
        let centre: CGPoint
    }
    static let thresh  = Knob(asset: "knob_green", frames: 128, frameSize: 74, centre: .init(x: 55, y: 80))
    static let makeup  = Knob(asset: "knob_green", frames: 128, frameSize: 74, centre: .init(x: 127, y: 80))
    static let attack  = Knob(asset: "knob_grey",  frames: 128, frameSize: 74, centre: .init(x: 72.5, y: 269))
    static let ceiling = Knob(asset: "knob_red",   frames: 128, frameSize: 140, centre: .init(x: 85, y: 535))

    struct Button { let centre: CGPoint; static let size: CGFloat = 31 }
    static let compButton   = Button(centre: .init(x: 64, y: 357))
    static let relButton    = Button(centre: .init(x: 129.5, y: 357))
    static let limitButton  = Button(centre: .init(x: 64, y: 413))
    static let bypassButton = Button(centre: .init(x: 129.5, y: 413))

    static let grLed = CGPoint(x: 154.5, y: 220)
    static let ledSize: CGFloat = 22
    /// Release ladder LEDs, top to bottom, with the seconds each one stands for.
    static let releaseLeds: [(value: Float, centre: CGPoint)] = [
        (2.0,  .init(x: 152.5, y: 250.5)),
        (0.5,  .init(x: 152.5, y: 275.0)),
        (0.2,  .init(x: 152.5, y: 299.5)),
        (0.05, .init(x: 152.5, y: 324.0)),
    ]

    /// GR meter: needle x is NON-LINEAR in dB — interpolate through the silkscreen marks.
    static let meterTravelY: ClosedRange<CGFloat> = 163.5...195.0
    static let meterMarks: [(db: CGFloat, x: CGFloat)] = [
        (0, 113.4), (2, 101.4), (4, 89.2), (6, 80.6), (10, 71.2), (20, 61.6),
    ]
    static func needleX(forGrDb gr: CGFloat) -> CGFloat {
        let g = max(0, min(20, gr))
        for i in 1..<meterMarks.count {
            let (d0, x0) = meterMarks[i - 1]
            let (d1, x1) = meterMarks[i]
            if g <= d1 {
                let t = (g - d0) / max(0.001, d1 - d0)
                return x0 + (x1 - x0) * t
            }
        }
        return meterMarks.last!.x
    }

    /// ATTACK detents (ms), dial order slow-CCW→fast? — silkscreen runs 15µ (CCW) → 15 (CW).
    static let attackStepsMs: [Float] = [0.1, 0.25, 1, 2, 5, 10, 15]   // 15µ rides the 0.1 ms floor
}

// MARK: asset cache

private final class Api525AAssets {
    static let shared = Api525AAssets()
    private var images: [String: NSImage] = [:]
    func image(_ name: String) -> NSImage? {
        if let cached = images[name] { return cached }
        let base = Bundle.main.resourcePath.map { $0 + "/api525a/" } ?? ""
        guard let image = NSImage(contentsOfFile: base + name + ".png") else { return nil }
        images[name] = image
        return image
    }
    /// One frame of a vertical film strip. The whole strip is sliced ONCE per asset and cached —
    /// deriving a CGImage from the 19k-pixel-tall NSImage per render is what made drags stutter.
    private var strips: [String: [CGImage]] = [:]
    func frame(_ name: String, index: Int, frames: Int) -> CGImage? {
        if strips[name] == nil {
            guard let image = image(name),
                  let cg = image.cgImage(forProposedRect: nil, context: nil, hints: nil) else { return nil }
            let frameHeight = cg.height / frames
            strips[name] = (0..<frames).compactMap { i in
                cg.cropping(to: CGRect(x: 0, y: i * frameHeight, width: cg.width, height: frameHeight))
            }
        }
        guard let sliced = strips[name], !sliced.isEmpty else { return nil }
        return sliced[max(0, min(sliced.count - 1, index))]
    }
}

// MARK: film-strip knob

private struct FilmStripKnob: View {
    let knob: K.Knob
    let scale: CGFloat
    let value: Float                    // normalized 0…1
    let onChange: (Float) -> Void       // continuous — the host records the gesture on commit
    let onCommit: () -> Void
    let defaultValue: Float
    var steps: [Float]? = nil           // detents in normalized space (rotary-switch feel)

    @State private var dragStart: Float?
    @State private var hovering = false
    @State private var wheelAccum: CGFloat = 0
    @State private var wheelCommit: DispatchWorkItem?
    /// Shown while a gesture is in flight, so the film strip follows the mouse frame-for-frame
    /// instead of waiting for the engine's published tick (which capped drags at ~30 fps).
    @State private var liveValue: Float?

    var body: some View {
        let shown = liveValue ?? value
        let frameIndex = Int((CGFloat(max(0, min(1, shown))) * CGFloat(knob.frames - 1)).rounded())
        let side = knob.frameSize * scale
        Group {
            if let cg = Api525AAssets.shared.frame(knob.asset, index: frameIndex, frames: knob.frames) {
                Image(decorative: cg, scale: 2 / scale)   // 2x assets
                    .resizable()
                    .frame(width: side, height: side)
            } else {
                Circle().fill(Color.gray).frame(width: side, height: side)
            }
        }
        // Waves-style focus: the control the wheel would turn is the one wearing the ring.
        .overlay(
            Circle()
                .stroke(Color(red: 0.37, green: 0.62, blue: 0.84).opacity(hovering ? 0.85 : 0), lineWidth: 1.5)
                .padding(1)
        )
        .frame(width: side, height: side)          // the view's frame IS the knob → exact hover
        .contentShape(Circle())
        .onHover { hovering = $0 }
        // The wheel over a knob turns the KNOB — never the mixer's scroll view under it.
        .overlay(KnobScrollWheel(active: hovering) { dy, precise in applyWheel(dy, precise) }
            .frame(width: 0, height: 0))
        .gesture(DragGesture(minimumDistance: 1)
            .onChanged { drag in
                let start = dragStart ?? value
                if dragStart == nil { dragStart = start }
                var next = max(0, min(1, start + Float(-drag.translation.height / 160)))
                if let steps, !steps.isEmpty {
                    next = steps.min(by: { abs($0 - next) < abs($1 - next) }) ?? next
                }
                liveValue = next
                onChange(next)
            }
            .onEnded { _ in dragStart = nil; liveValue = nil; onCommit() })
        .highPriorityGesture(TapGesture(count: 2).onEnded { onChange(defaultValue); onCommit() })
        .offset(x: (knob.centre.x - knob.frameSize / 2) * scale,
                y: (knob.centre.y - knob.frameSize / 2) * scale)
    }

    /// One mouse notch = one detent on a stepped knob, a fine nudge on a continuous one.
    /// Trackpads accumulate ~25 pt of travel per step so a flick isn't a leap.
    private func applyWheel(_ delta: CGFloat, _ precise: Bool) {
        var notches = 0
        if precise {
            if wheelAccum != 0 && (delta > 0) != (wheelAccum > 0) { wheelAccum = 0 }
            wheelAccum += delta
            if abs(wheelAccum) >= 25 {
                notches = wheelAccum > 0 ? 1 : -1
                wheelAccum -= CGFloat(notches) * 25
            }
        } else {
            notches = delta > 0 ? 1 : -1
        }
        guard notches != 0 else { return }
        let base = liveValue ?? value
        var next: Float
        if let steps, !steps.isEmpty {
            let ordered = steps.sorted()
            let currentIndex = ordered.enumerated()
                .min(by: { abs($0.element - base) < abs($1.element - base) })?.offset ?? 0
            next = ordered[max(0, min(ordered.count - 1, currentIndex + notches))]
        } else {
            next = max(0, min(1, base + Float(notches) * 0.02))
        }
        liveValue = next
        onChange(next)
        wheelCommit?.cancel()
        let work = DispatchWorkItem { onCommit(); liveValue = nil }
        wheelCommit = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.4, execute: work)
    }
}

// MARK: the plate

struct Api525APlate: View {
    @ObservedObject var engine: EngineController
    @EnvironmentObject private var trackMeters: EngineController.TrackMeters
    let trackId: Int
    var width: CGFloat = 205

    private var scale: CGFloat { width / K.panel.width }
    var plateHeight: CGFloat { K.panel.height * (width / K.panel.width) }

    // Parameter plumbing — the same console params every model shares.
    private func value(_ name: String) -> Float { engine.consoleValue(trackId, name) }
    private func set(_ name: String, _ v: Float) { engine.setConsoleValue(trackId, name, v) }
    private func commit(_ name: String) { engine.recordGesture("525A \(name)") }

    var body: some View {
        ZStack(alignment: .topLeading) {
            if let panel = Api525AAssets.shared.image("panel_background") {
                Image(nsImage: panel)
                    .resizable()
                    .frame(width: K.panel.width * scale, height: K.panel.height * scale)
            }

            // GR needle — behind nothing (the housing is silkscreened on the backboard).
            needle

            // THRESH: −20…+10 dB, silkscreen "IN".
            FilmStripKnob(knob: K.thresh, scale: scale,
                          value: (value("compThresholdDb") + 20) / 30,
                          onChange: { set("compThresholdDb", $0 * 30 - 20) },
                          onCommit: { commit("thresh") },
                          defaultValue: (0 + 20) / 30)
            // MAKE-UP: 0…20 dB out.
            FilmStripKnob(knob: K.makeup, scale: scale,
                          value: value("compMakeupDb") / 20,
                          onChange: { set("compMakeupDb", $0 * 20) },
                          onCommit: { commit("makeup") },
                          defaultValue: 0)
            // ATTACK: seven silkscreened detents, 15µ→15 ms.
            FilmStripKnob(knob: K.attack, scale: scale,
                          value: attackNormalized,
                          onChange: { setAttack(normalized: $0) },
                          onCommit: { commit("attack") },
                          defaultValue: 3.0 / 6.0,
                          steps: (0...6).map { Float($0) / 6 })
            // CEILING: 0…20 — compression AND gain, the one-knob 525 move.
            FilmStripKnob(knob: K.ceiling, scale: scale,
                          value: value("compCeilingDb") / 20,
                          onChange: { set("compCeilingDb", $0 * 20) },
                          onCommit: { commit("ceiling") },
                          defaultValue: 0)

            plateButton(K.compButton, on: ratio < 10 && engaged) { setRatio(2) }
            plateButton(K.limitButton, on: ratio >= 10 && engaged) { setRatio(20) }
            plateButton(K.relButton, on: false) { cycleRelease() }
            plateButton(K.bypassButton, on: !engaged) {
                engine.setConsoleBool(trackId, "compEnabled", !engaged)
                engine.recordGesture("525A bypass")
            }

            // LEDs: release ladder (nearest socket lit) + GR activity.
            ForEach(Array(K.releaseLeds.enumerated()), id: \.offset) { _, led in
                if abs(led.value - nearestReleaseValue) < 0.001 {
                    ledImage("led_green_on")
                        .position(x: led.centre.x * scale, y: led.centre.y * scale)
                }
            }
            if grDb > 0.5 {
                ledImage("led_red_on")
                    .position(x: K.grLed.x * scale, y: K.grLed.y * scale)
            }
        }
        .frame(width: K.panel.width * scale, height: K.panel.height * scale)
    }

    // MARK: pieces

    private var engaged: Bool { engine.consoleBool(trackId, "compEnabled") }
    private var ratio: Float { value("compRatio") }
    private func setRatio(_ r: Float) {
        set("compRatio", r)
        if !engaged { engine.setConsoleBool(trackId, "compEnabled", true) }
        engine.recordGesture("525A ratio")
    }

    private var grDb: CGFloat {
        let name = engine.tracks.first(where: { $0.id == trackId })?.name ?? ""
        return CGFloat(trackMeters.level(name).compGainReductionDb)
    }

    private var needle: some View {
        let x = K.needleX(forGrDb: engaged ? grDb : 0) * scale
        return Group {
            if let needleImage = Api525AAssets.shared.image("meter_needle") {
                Image(nsImage: needleImage)
                    .resizable()
                    .frame(width: 2.6 * scale,
                           height: (K.meterTravelY.upperBound - K.meterTravelY.lowerBound) * scale)
                    .position(x: x, y: (K.meterTravelY.lowerBound + K.meterTravelY.upperBound) / 2 * scale)
                    .animation(.linear(duration: 0.08), value: x)
            }
        }
    }

    private func ledImage(_ name: String) -> some View {
        Group {
            if let led = Api525AAssets.shared.image(name) {
                Image(nsImage: led).resizable()
                    .frame(width: K.ledSize * scale, height: K.ledSize * scale)
            }
        }
    }

    private func plateButton(_ button: K.Button, on: Bool, action: @escaping () -> Void) -> some View {
        let side = K.Button.size * scale
        return Group {
            if let cg = Api525AAssets.shared.frame("button", index: on ? 1 : 0, frames: 2) {
                Image(decorative: cg, scale: 2 / scale).resizable()
            } else {
                RoundedRectangle(cornerRadius: 3).fill(on ? Color.white : Color.gray)
            }
        }
        .frame(width: side, height: side)
        .position(x: button.centre.x * scale, y: button.centre.y * scale)
        .contentShape(Rectangle().path(in: CGRect(x: button.centre.x * scale - side / 2,
                                                  y: button.centre.y * scale - side / 2,
                                                  width: side, height: side)))
        .onTapGesture(perform: action)
    }

    // MARK: attack / release ladders

    private var attackNormalized: Float {
        let ms = value("compAttackMs")
        let index = K.attackStepsMs.enumerated().min(by: { abs($0.element - ms) < abs($1.element - ms) })?.offset ?? 3
        return Float(index) / Float(K.attackStepsMs.count - 1)
    }
    private func setAttack(normalized: Float) {
        let index = Int((normalized * Float(K.attackStepsMs.count - 1)).rounded())
        set("compAttackMs", K.attackStepsMs[max(0, min(K.attackStepsMs.count - 1, index))])
    }

    private var nearestReleaseValue: Float {
        let seconds = value("compReleaseMs") / 1000
        return K.releaseLeds.map(\.value).min(by: { abs($0 - seconds) < abs($1 - seconds) }) ?? 0.2
    }
    private func cycleRelease() {
        let ladder = K.releaseLeds.map(\.value)
        let current = ladder.firstIndex(where: { abs($0 - nearestReleaseValue) < 0.001 }) ?? 0
        let next = ladder[(current + 1) % ladder.count]
        set("compReleaseMs", next * 1000)
        engine.recordGesture("525A release")
    }
}
