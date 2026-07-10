import AppKit
import SwiftUI

/// What the timeline needs to draw one frame. A value type so the AppKit view can
/// diff it cheaply and skip redraws.
struct TimelineModel: Equatable {
    struct Lane: Equatable {
        let name: String
        let accent: NSColor
        let muted: Bool
    }

    struct Clip: Equatable {
        let id: String
        let name: String
        let laneIndex: Int
        let startSeconds: Double
        let durationSeconds: Double
        let sourcePath: String
    }

    var lanes: [Lane] = []
    var clips: [Clip] = []
    var tempoBpm: Int = 120
    var beatsPerBar: Int = 4

    /// Seconds at the left edge, and seconds across the visible width.
    var visibleStart: Double = 0
    var visibleDuration: Double = 30
}

/// Timeline surface: ruler, grid, lanes, clips with waveforms, playhead.
///
/// This is an NSView rather than SwiftUI because it repaints a waveform envelope
/// per clip and moves a playhead at 30 Hz. The playhead lives in its own layer so
/// moving it never triggers a redraw of the waveforms.
final class TimelineNSView: NSView {
    var model = TimelineModel() {
        didSet {
            guard model != oldValue else { return }
            needsDisplay = true
        }
    }

    /// Seconds; drives only the playhead layer.
    var playheadSeconds: Double = 0 {
        didSet { layoutPlayhead() }
    }

    /// Peaks by source path, supplied by the owner. Decoding lives in the engine.
    var waveforms: [String: (mins: [Float], maxs: [Float])] = [:] {
        didSet { needsDisplay = true }
    }

    var onSeek: ((Double) -> Void)?
    var onZoom: ((Double, Double) -> Void)?   // (visibleStart, visibleDuration)

    static let rulerHeight: CGFloat = 30
    static let laneHeight: CGFloat = 78
    static let headerWidth: CGFloat = 150

    private let playheadLayer = CALayer()

    override var isFlipped: Bool { true }

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
        playheadLayer.backgroundColor = NSColor(hex: 0xff5252).cgColor
        playheadLayer.zPosition = 10
        layer?.addSublayer(playheadLayer)
    }

    required init?(coder: NSCoder) { nil }

    // MARK: Geometry

    private var lanesRect: NSRect {
        NSRect(x: Self.headerWidth,
               y: Self.rulerHeight,
               width: max(0, bounds.width - Self.headerWidth),
               height: max(0, bounds.height - Self.rulerHeight))
    }

    private func x(forSeconds seconds: Double) -> CGFloat {
        let fraction = (seconds - model.visibleStart) / max(0.0001, model.visibleDuration)
        return lanesRect.minX + CGFloat(fraction) * lanesRect.width
    }

    private func seconds(atX pointX: CGFloat) -> Double {
        let fraction = Double((pointX - lanesRect.minX) / max(1, lanesRect.width))
        return model.visibleStart + fraction * model.visibleDuration
    }

    // MARK: Interaction

    override func mouseDown(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)
        guard point.x >= lanesRect.minX else { return }
        onSeek?(max(0, seconds(atX: point.x)))
    }

    /// Scroll pans; ⌘-scroll (or a pinch) zooms about the pointer.
    override func scrollWheel(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)

        if event.modifierFlags.contains(.command) || event.phase == .changed && event.magnification != 0 {
            let anchor = seconds(atX: point.x)
            let factor = event.scrollingDeltaY > 0 ? 0.9 : 1.1
            let duration = min(600, max(0.25, model.visibleDuration * factor))

            // Keep the time under the pointer pinned while the span changes.
            let fraction = Double((point.x - lanesRect.minX) / max(1, lanesRect.width))
            let start = max(0, anchor - fraction * duration)
            onZoom?(start, duration)
            return
        }

        let secondsPerPoint = model.visibleDuration / Double(max(1, lanesRect.width))
        let start = max(0, model.visibleStart + Double(event.scrollingDeltaX) * -secondsPerPoint)
        onZoom?(start, model.visibleDuration)
    }

    // MARK: Drawing

    override func layout() {
        super.layout()
        layoutPlayhead()
    }

    private func layoutPlayhead() {
        let position = x(forSeconds: playheadSeconds)
        let visible = position >= lanesRect.minX && position <= lanesRect.maxX
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        playheadLayer.isHidden = !visible
        playheadLayer.frame = CGRect(x: position, y: 0, width: 1, height: bounds.height)
        CATransaction.commit()
    }

    override func draw(_ dirtyRect: NSRect) {
        guard let context = NSGraphicsContext.current?.cgContext else { return }

        NSColor(hex: 0x2c2622).setFill()
        bounds.fill()

        drawRuler(context)
        drawLaneHeaders(context)
        drawGrid(context)
        drawClips(context)

        // The lane header column sits above the grid but below the playhead.
        NSColor(hex: 0x0b0806).setFill()
        NSRect(x: Self.headerWidth - 1, y: 0, width: 1, height: bounds.height).fill()
    }

    /// Bars while they are at least 40 pt apart, otherwise a coarser multiple.
    private func barStep() -> Int {
        let secondsPerBar = 60.0 / Double(model.tempoBpm) * Double(model.beatsPerBar)
        let pointsPerBar = Double(lanesRect.width) * secondsPerBar / max(0.0001, model.visibleDuration)
        for step in [1, 2, 4, 8, 16, 32, 64, 128] where Double(step) * pointsPerBar >= 40 {
            return step
        }
        return 256
    }

    private func drawRuler(_ context: CGContext) {
        NSColor(hex: 0x332c26).setFill()
        NSRect(x: 0, y: 0, width: bounds.width, height: Self.rulerHeight).fill()

        let secondsPerBar = 60.0 / Double(model.tempoBpm) * Double(model.beatsPerBar)
        let step = barStep()

        let firstBar = max(0, Int((model.visibleStart / secondsPerBar).rounded(.down)))
        let lastBar = Int(((model.visibleStart + model.visibleDuration) / secondsPerBar).rounded(.up))

        let attributes: [NSAttributedString.Key: Any] = [
            .font: NSFont.monospacedSystemFont(ofSize: 9, weight: .medium),
            .foregroundColor: NSColor(hex: 0x867b6a),
        ]

        var bar = firstBar - firstBar % step
        while bar <= lastBar {
            let position = x(forSeconds: Double(bar) * secondsPerBar)
            if position >= lanesRect.minX - 1 {
                NSColor(hex: 0x574d40).setFill()
                NSRect(x: position, y: Self.rulerHeight - 8, width: 1, height: 8).fill()

                let label = "\(bar + 1)" as NSString
                label.draw(at: NSPoint(x: position + 4, y: 6), withAttributes: attributes)
            }
            bar += step
        }

        NSColor(hex: 0x0b0806).setFill()
        NSRect(x: 0, y: Self.rulerHeight - 1, width: bounds.width, height: 1).fill()
    }

    private func drawLaneHeaders(_ context: CGContext) {
        let nameAttributes: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 11, weight: .semibold),
            .foregroundColor: NSColor(hex: 0xe8e1d5),
        ]

        for (index, lane) in model.lanes.enumerated() {
            let rect = NSRect(x: 0,
                              y: Self.rulerHeight + CGFloat(index) * Self.laneHeight,
                              width: Self.headerWidth,
                              height: Self.laneHeight)
            NSColor(hex: 0x332c26).setFill()
            rect.fill()

            lane.accent.setFill()
            NSRect(x: 0, y: rect.minY, width: 3, height: rect.height).fill()

            (lane.name as NSString).draw(at: NSPoint(x: 12, y: rect.minY + 10),
                                         withAttributes: nameAttributes)

            NSColor(hex: 0x1b1611).setFill()
            NSRect(x: 0, y: rect.maxY - 1, width: bounds.width, height: 1).fill()
        }
    }

    private func drawGrid(_ context: CGContext) {
        let secondsPerBar = 60.0 / Double(model.tempoBpm) * Double(model.beatsPerBar)
        let step = barStep()

        let firstBar = max(0, Int((model.visibleStart / secondsPerBar).rounded(.down)))
        let lastBar = Int(((model.visibleStart + model.visibleDuration) / secondsPerBar).rounded(.up))

        context.saveGState()
        context.clip(to: lanesRect)

        var bar = firstBar - firstBar % step
        while bar <= lastBar {
            let position = x(forSeconds: Double(bar) * secondsPerBar)
            NSColor(hex: 0x3d4650).withAlphaComponent(0.5).setFill()
            NSRect(x: position, y: lanesRect.minY, width: 1, height: lanesRect.height).fill()
            bar += step
        }
        context.restoreGState()
    }

    private func drawClips(_ context: CGContext) {
        context.saveGState()
        context.clip(to: lanesRect)

        for clip in model.clips {
            guard clip.laneIndex < model.lanes.count else { continue }
            let lane = model.lanes[clip.laneIndex]

            let left = x(forSeconds: clip.startSeconds)
            let right = x(forSeconds: clip.startSeconds + clip.durationSeconds)
            guard right > lanesRect.minX, left < lanesRect.maxX else { continue }

            let top = Self.rulerHeight + CGFloat(clip.laneIndex) * Self.laneHeight + 6
            let rect = NSRect(x: left, y: top, width: max(2, right - left), height: Self.laneHeight - 12)

            let body = NSBezierPath(roundedRect: rect, xRadius: 4, yRadius: 4)
            NSColor(hex: 0x1e3140).setFill()
            body.fill()

            drawWaveform(clip, in: rect.insetBy(dx: 2, dy: 14), accent: lane.accent)

            // Name plate along the top of the clip.
            NSColor(hex: 0x0b0806).withAlphaComponent(0.55).setFill()
            NSRect(x: rect.minX, y: rect.minY, width: rect.width, height: 13).fill()

            context.saveGState()
            context.clip(to: rect)
            (clip.name as NSString).draw(
                at: NSPoint(x: rect.minX + 5, y: rect.minY + 1),
                withAttributes: [
                    .font: NSFont.systemFont(ofSize: 9, weight: .medium),
                    .foregroundColor: NSColor(hex: 0xddd5c8),
                ]
            )
            context.restoreGState()

            lane.accent.withAlphaComponent(0.7).setStroke()
            body.lineWidth = 1
            body.stroke()
        }

        context.restoreGState()
    }

    /// Resamples the cached peak buckets onto the clip's on-screen width. Only the
    /// visible span is walked, so zooming in does not cost more.
    private func drawWaveform(_ clip: TimelineModel.Clip, in rect: NSRect, accent: NSColor) {
        guard rect.width >= 1,
              let peaks = waveforms[clip.sourcePath],
              !peaks.mins.isEmpty else { return }

        let midY = rect.midY
        let halfHeight = rect.height / 2
        let buckets = peaks.mins.count

        let path = NSBezierPath()
        var drew = false

        let firstColumn = Int(max(0, lanesRect.minX - rect.minX))
        let lastColumn = Int(min(rect.width, lanesRect.maxX - rect.minX))
        guard lastColumn > firstColumn else { return }

        for column in firstColumn..<lastColumn {
            let fraction = Double(column) / Double(rect.width)
            let bucket = min(buckets - 1, max(0, Int(fraction * Double(buckets))))

            let low = CGFloat(peaks.mins[bucket])
            let high = CGFloat(peaks.maxs[bucket])
            let pointX = rect.minX + CGFloat(column) + 0.5

            path.move(to: NSPoint(x: pointX, y: midY - high * halfHeight))
            path.line(to: NSPoint(x: pointX, y: midY - low * halfHeight))
            drew = true
        }

        guard drew else { return }
        accent.withAlphaComponent(0.85).setStroke()
        path.lineWidth = 1
        path.stroke()
    }
}

extension NSColor {
    /// Theme colours are declared once, in SwiftUI terms; the AppKit view borrows them.
    static func from(_ color: Color) -> NSColor {
        NSColor(color)
    }

    convenience init(hex: UInt32) {
        self.init(srgbRed: CGFloat((hex >> 16) & 0xff) / 255,
                  green: CGFloat((hex >> 8) & 0xff) / 255,
                  blue: CGFloat(hex & 0xff) / 255,
                  alpha: 1)
    }
}

/// SwiftUI wrapper. The playhead is pushed straight at the layer so the 30 Hz tick
/// never invalidates the waveform drawing.
struct TimelineView: NSViewRepresentable {
    let model: TimelineModel
    let playheadSeconds: Double
    let waveforms: [String: (mins: [Float], maxs: [Float])]
    let onSeek: (Double) -> Void
    let onZoom: (Double, Double) -> Void

    func makeNSView(context: Context) -> TimelineNSView {
        let view = TimelineNSView(frame: .zero)
        view.onSeek = onSeek
        view.onZoom = onZoom
        return view
    }

    func updateNSView(_ view: TimelineNSView, context: Context) {
        view.onSeek = onSeek
        view.onZoom = onZoom
        if view.waveforms.keys != waveforms.keys {
            view.waveforms = waveforms
        }
        view.model = model
        view.playheadSeconds = playheadSeconds
    }
}
