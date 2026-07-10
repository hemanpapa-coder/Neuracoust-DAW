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

    // MARK: Monitor station

    struct MonitorModule: Identifiable {
        let id: Int
        let name: String
        let detail: String
        let stage: String
        var enabled: Bool

        var displayDetail: String { stripSlotPrefix(detail) }
    }

    struct SpeakerSet: Identifiable {
        let id: Int
        let letter: String
        let name: String
        var model: String
        var output: String
        var simWeight: Float
        var roomEq: Bool

        /// The engine stores models as "Speaker B: Yamaha NS-10M Studio (NF)".
        /// The slot letter is already on the tab, so drop the prefix.
        var displayModel: String { stripSlotPrefix(model) }
    }

    enum ListenMode: String, CaseIterable, Identifiable {
        case stereo = "LR"
        case mono = "MONO"
        case midSide = "MS"
        case polarity = "POL"

        var id: String { rawValue }
        var label: String {
            switch self {
            case .stereo: return "Stereo"
            case .mono: return "Mono"
            case .midSide: return "M/S"
            case .polarity: return "Ø"
            }
        }
    }

    enum OutputMode { case speaker, headphone }

    @Published private(set) var monitorModules: [MonitorModule] = []
    @Published private(set) var speakerSets: [SpeakerSet] = []
    @Published private(set) var activeSpeakerSlot = 0
    @Published private(set) var monitorVolumeDb: Float = -6
    @Published private(set) var listenMode: ListenMode = .stereo
    @Published private(set) var monitorDim = false
    @Published private(set) var monitorMono = false
    @Published private(set) var monitorTalkback = false
    @Published private(set) var monitorDspEnabled = true
    @Published private(set) var monitorPathMode = "internal"

    /// Output mode is a UI concept — the engine models speaker vs headphone as
    /// which simulation module is enabled.
    @Published var outputMode: OutputMode = .speaker

    // Live meters, refreshed each tick.
    @Published private(set) var phaseCorrelation: Float = 0
    @Published private(set) var spectrumLow: Float = 0
    @Published private(set) var spectrumMid: Float = 0
    @Published private(set) var spectrumHigh: Float = 0
    @Published private(set) var wakeJitterUs: Double = 0
    @Published private(set) var remoteDspActive = false
    @Published private(set) var remoteDspRoundTripMs: Double = 0

    /// Fraction of the buffer period consumed by the worst recent render pass.
    /// This is render headroom, which is what actually predicts dropouts — raw
    /// wake jitter reads ~1 buffer period even when idle on Waves SoundGrid.
    var dspLoadFraction: Double {
        guard sampleRate > 0, bufferSize > 0 else { return 0 }
        let bufferPeriodUs = Double(bufferSize) / sampleRate * 1_000_000
        return min(1.0, maxRenderDurationUs / bufferPeriodUs)
    }

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
        reloadMonitorState()

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

    // MARK: - Monitor station

    private func reloadMonitorState() {
        guard let handle else { return }

        monitorModules = (0..<Int(nc_monitor_module_count(handle))).map { index in
            let i = Int32(index)
            return MonitorModule(
                id: index,
                name: readString { nc_monitor_module_name(handle, i, $0, $1) },
                detail: readString { nc_monitor_module_detail(handle, i, $0, $1) },
                stage: readString { nc_monitor_module_stage(handle, i, $0, $1) },
                enabled: nc_monitor_module_enabled(handle, i)
            )
        }

        let names = ["Mains", "Nearfield", "Grot Box"]
        speakerSets = (0..<3).map { slot in
            let s = Int32(slot)
            return SpeakerSet(
                id: slot,
                letter: ["A", "B", "C"][slot],
                name: names[slot],
                model: readString { nc_monitor_speaker_model(handle, s, $0, $1) },
                output: readString { nc_monitor_speaker_output(handle, s, $0, $1) },
                simWeight: nc_monitor_speaker_sim_weight(handle, s),
                roomEq: nc_monitor_speaker_room_eq(handle, s)
            )
        }

        activeSpeakerSlot = Int(nc_monitor_active_speaker_slot(handle))
        monitorVolumeDb = nc_monitor_volume_db(handle)
        monitorDim = nc_monitor_dim(handle)
        monitorMono = nc_monitor_mono(handle)
        monitorTalkback = nc_monitor_talkback(handle)
        monitorDspEnabled = nc_monitor_dsp_enabled(handle)
        monitorPathMode = readString { nc_monitor_path_mode(handle, $0, $1) }
        listenMode = ListenMode(rawValue: readString { nc_monitor_listen_mode(handle, $0, $1) }) ?? .stereo
    }

    var activeSpeakerSet: SpeakerSet? {
        speakerSets.first { $0.id == activeSpeakerSlot }
    }

    func setMonitorVolume(_ db: Float) {
        guard let handle else { return }
        nc_monitor_set_volume_db(handle, db)
        monitorVolumeDb = nc_monitor_volume_db(handle)
    }

    func setListenMode(_ mode: ListenMode) {
        guard let handle else { return }
        // Mono is its own engine flag; the other three are listen-mode strings.
        nc_monitor_set_mono(handle, mode == .mono)
        nc_monitor_set_listen_mode(handle, mode.rawValue)
        listenMode = mode
        monitorMono = nc_monitor_mono(handle)
    }

    func toggleDim() {
        guard let handle else { return }
        nc_monitor_set_dim(handle, !monitorDim)
        monitorDim = nc_monitor_dim(handle)
    }

    func toggleMonitorMono() {
        guard let handle else { return }
        nc_monitor_set_mono(handle, !monitorMono)
        monitorMono = nc_monitor_mono(handle)
    }

    func toggleTalkback() {
        guard let handle else { return }
        nc_monitor_set_talkback(handle, !monitorTalkback)
        monitorTalkback = nc_monitor_talkback(handle)
    }

    func setSpeakerSlot(_ slot: Int) {
        guard let handle else { return }
        nc_monitor_set_active_speaker_slot(handle, Int32(slot))
        // The speaker-simulation module reports the model of whichever slot is
        // active, so its row text goes stale unless the list is re-read.
        reloadMonitorState()
    }

    func setModuleEnabled(_ index: Int, _ enabled: Bool) {
        guard let handle else { return }
        nc_monitor_set_module_enabled(handle, Int32(index), enabled)
        if let position = monitorModules.firstIndex(where: { $0.id == index }) {
            monitorModules[position].enabled = nc_monitor_module_enabled(handle, Int32(index))
        }
    }

    func bypassAllModules() {
        guard let handle else { return }
        nc_monitor_set_dsp_enabled(handle, !monitorDspEnabled)
        monitorDspEnabled = nc_monitor_dsp_enabled(handle)
    }

    func setMonitorPathMode(_ mode: String) {
        guard let handle else { return }
        nc_monitor_set_path_mode(handle, mode)
        monitorPathMode = readString { nc_monitor_path_mode(handle, $0, $1) }
    }

    // MARK: - Poll loop

    private func tick() {
        guard let handle else { return }

        var status = NCEngineStatus()
        nc_engine_status(handle, &status)

        phaseCorrelation = status.phaseCorrelation
        spectrumLow = status.spectrumLow
        spectrumMid = status.spectrumMid
        spectrumHigh = status.spectrumHigh
        wakeJitterUs = status.realtimeAverageWakeJitterUs
        remoteDspActive = status.remoteDspMonitorActive
        remoteDspRoundTripMs = status.remoteDspRoundTripMs

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

/// Drops a leading "Speaker A: " / "Headphone C: " label from an engine model string.
func stripSlotPrefix(_ text: String) -> String {
    guard let colon = text.firstIndex(of: ":") else { return text }
    let head = text[text.startIndex..<colon]
    guard head.hasPrefix("Speaker ") || head.hasPrefix("Headphone ") else { return text }
    return String(text[text.index(after: colon)...]).trimmingCharacters(in: .whitespaces)
}

/// dBFS from a linear peak, floored so silence doesn't read -inf.
func peakToDb(_ peak: Float) -> Double {
    peak <= 0.00001 ? -60.0 : max(-60.0, 20.0 * log10(Double(peak)))
}

/// Maps dBFS onto 0...1 across the meter's -60…0 dB span.
func meterFraction(_ peak: Float) -> Double {
    min(1.0, max(0.0, (peakToDb(peak) + 60.0) / 60.0))
}
