import Foundation
import SwiftUI

/// Owns the C++ engine and drives every live readout from a 30 Hz poll of its
/// status snapshot. The engine pushes nothing — there are no callbacks, no KVO,
/// no notifications (docs/legacy-ui-contract.md §1).
///
/// All engine calls happen on the main actor because the engine's public API is
/// main-thread-only.
@MainActor
final class EngineController: ObservableObject {
    enum ViewTab: String, CaseIterable, Identifiable {
        case edit = "Edit"
        case mix = "Mix"
        var id: String { rawValue }
    }

    // Engine-derived state, refreshed each tick.
    @Published private(set) var running = false
    @Published private(set) var transportRunning = false
    @Published private(set) var outputPeakLeft: Float = 0
    @Published private(set) var outputPeakRight: Float = 0
    @Published private(set) var sampleRate: Double = 0
    @Published private(set) var bufferSize: Int = 0
    @Published private(set) var delayCompensationMs: Double = 0
    @Published private(set) var maxRenderDurationUs: Double = 0
    @Published private(set) var deviceName = ""
    @Published private(set) var startupError: String?

    /// Smoothed playhead. Between polls it advances on the wall clock; it only
    /// snaps back to the engine when the two disagree by more than
    /// `resyncThreshold`. Reading `playbackSeconds` straight from the snapshot
    /// makes the playhead step at 30 Hz.
    @Published private(set) var playheadSeconds: Double = 0

    // UI state the engine knows nothing about.
    @Published var viewTab: ViewTab = .edit
    @Published var loopEnabled = false
    @Published var clickEnabled = false
    @Published var snapEnabled = true
    @Published var recording = false

    @Published private(set) var projectName = ""
    @Published private(set) var tempoBpm = 120
    @Published private(set) var timeSignature = (numerator: 4, denominator: 4)

    /// `NCEngine` is opaque in C, so Swift imports the handle as an OpaquePointer.
    /// Marked nonisolated so `deinit` can free it; nothing else ever holds it.
    private nonisolated(unsafe) var handle: OpaquePointer?
    private var timer: Timer?

    private var transportWallClockStart: CFTimeInterval = 0
    private var transportWallClockBase: Double = 0

    private let tickInterval = 1.0 / 30.0
    private let resyncThreshold = 0.18

    init() {
        handle = nc_engine_create()
    }

    deinit {
        if let handle {
            nc_engine_stop(handle)
            nc_engine_destroy(handle)
        }
    }

    func start() {
        guard let handle else { return }

        var errorBuffer = [CChar](repeating: 0, count: 256)
        let ok = nc_engine_start(handle, &errorBuffer, errorBuffer.count)
        if !ok {
            startupError = String(cString: errorBuffer)
        }

        projectName = readString { nc_project_name(handle, $0, $1) }
        tempoBpm = Int(nc_project_tempo_bpm(handle))
        timeSignature = (
            Int(nc_project_time_signature_numerator(handle)),
            Int(nc_project_time_signature_denominator(handle))
        )
        loopEnabled = nc_project_loop_enabled(handle)

        let timer = Timer(timeInterval: tickInterval, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.tick() }
        }
        RunLoop.main.add(timer, forMode: .common)
        self.timer = timer
    }

    func shutdown() {
        timer?.invalidate()
        timer = nil
        if let handle {
            nc_engine_stop(handle)
        }
    }

    // MARK: - Transport

    func togglePlay() {
        setTransport(running: !transportRunning)
    }

    func stop() {
        setTransport(running: false)
        seek(0)
    }

    func rewind() {
        guard let handle else { return }
        nc_engine_rewind(handle)
        playheadSeconds = 0
        transportWallClockBase = 0
        transportWallClockStart = CACurrentMediaTime()
    }

    func seek(_ seconds: Double) {
        guard let handle else { return }
        let clamped = max(0, seconds)
        nc_engine_seek(handle, clamped)
        playheadSeconds = clamped
        transportWallClockBase = clamped
        transportWallClockStart = CACurrentMediaTime()
    }

    func toggleRecording() {
        guard let handle else { return }
        recording.toggle()
        nc_engine_set_recording(handle, recording)
    }

    func toggleLoop() {
        guard let handle else { return }
        loopEnabled.toggle()
        nc_project_set_loop_enabled(handle, loopEnabled)
    }

    func toggleClick() {
        guard let handle else { return }
        clickEnabled.toggle()
        nc_engine_set_metronome_enabled(handle, clickEnabled)
    }

    private func setTransport(running: Bool) {
        guard let handle else { return }
        nc_engine_set_transport_running(handle, running)
        transportRunning = running
        transportWallClockBase = playheadSeconds
        transportWallClockStart = CACurrentMediaTime()
    }

    // MARK: - Readouts

    var timecode: String {
        guard let handle else { return "00:00:00:00" }
        return readString { nc_project_timecode(handle, playheadSeconds, $0, $1) }
    }

    var barsBeats: (bar: Int, beat: Int, tick: Int) {
        guard let handle else { return (1, 1, 0) }
        var bar: Int32 = 1
        var beat: Int32 = 1
        var tick: Int32 = 0
        nc_project_bars_beats(handle, playheadSeconds, &bar, &beat, &tick)
        return (Int(bar), Int(beat), Int(tick))
    }

    // MARK: - Poll loop

    private func tick() {
        guard let handle else { return }

        var status = NCEngineStatus()
        nc_engine_status(handle, &status)

        running = status.running
        transportRunning = status.transportRunning
        outputPeakLeft = status.outputPeakLeft
        outputPeakRight = status.outputPeakRight
        sampleRate = status.sampleRate
        bufferSize = Int(status.requestedBufferSize)
        delayCompensationMs = status.delayCompensationMs
        maxRenderDurationUs = status.realtimeMaxRenderDurationUs
        deviceName = withUnsafePointer(to: status.deviceName) {
            $0.withMemoryRebound(to: CChar.self, capacity: Int(NC_TEXT_LEN)) { String(cString: $0) }
        }

        updatePlayhead(engineSeconds: status.playbackSeconds)
    }

    private func updatePlayhead(engineSeconds: Double) {
        guard transportRunning else {
            playheadSeconds = engineSeconds
            transportWallClockBase = engineSeconds
            transportWallClockStart = CACurrentMediaTime()
            return
        }

        let elapsed = CACurrentMediaTime() - transportWallClockStart
        let predicted = transportWallClockBase + elapsed

        if abs(predicted - engineSeconds) > resyncThreshold {
            playheadSeconds = engineSeconds
            transportWallClockBase = engineSeconds
            transportWallClockStart = CACurrentMediaTime()
        } else {
            playheadSeconds = predicted
        }
    }

    private func readString(_ fill: (UnsafeMutablePointer<CChar>, Int) -> Void) -> String {
        var buffer = [CChar](repeating: 0, count: Int(NC_TEXT_LEN))
        fill(&buffer, buffer.count)
        return String(cString: buffer)
    }
}

/// dBFS from a linear peak, floored so silence doesn't read -inf.
func peakToDb(_ peak: Float) -> Double {
    peak <= 0.00001 ? -60.0 : max(-60.0, 20.0 * log10(Double(peak)))
}

/// Maps dBFS onto 0...1 across the meter's -60…0 dB span.
func meterFraction(_ peak: Float) -> Double {
    min(1.0, max(0.0, (peakToDb(peak) + 60.0) / 60.0))
}
