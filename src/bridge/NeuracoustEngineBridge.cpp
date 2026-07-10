#include "bridge/NeuracoustEngineBridge.h"

#include "audio/RealtimeAudioEngine.h"
#include "project/EditOperations.h"
#include "project/ProjectDocument.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

using neuracoust::daw::AudioEngineSettings;
using neuracoust::daw::AudioEngineStatus;
using neuracoust::daw::ProjectDocument;
using neuracoust::daw::RealtimeAudioEngine;

namespace {

void copyText(char* dst, size_t dstLen, const std::string& src) {
    if (dstLen == 0) {
        return;
    }
    const size_t n = std::min(src.size(), dstLen - 1);
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

} // namespace

struct NCEngine {
    RealtimeAudioEngine engine;
    ProjectDocument project = neuracoust::daw::defaultProject();
};

NCEngine* nc_engine_create(void) {
    return new NCEngine();
}

void nc_engine_destroy(NCEngine* engine) {
    delete engine;
}

bool nc_engine_start(NCEngine* engine, char* error, size_t errorLen) {
    if (engine == nullptr) {
        copyText(error, errorLen, "engine is null");
        return false;
    }

    std::string loadError;
    neuracoust::daw::normalizeProjectRouting(engine->project);
    if (!engine->engine.loadProject(engine->project, loadError)) {
        copyText(error, errorLen, loadError.empty() ? "loadProject failed" : loadError);
        return false;
    }

    AudioEngineSettings settings;
    settings.sampleRate = engine->project.sampleRate;
    settings.bufferSize = engine->project.defaultBufferSize;
    settings.tempoBpm = engine->project.tempoBpm;
    settings.timeSignatureNumerator = engine->project.timeSignatureNumerator;
    settings.timeSignatureDenominator = engine->project.timeSignatureDenominator;
    settings.transportRunning = false;
    settings.metronomeEnabled = false;

    if (!engine->engine.start(settings)) {
        const AudioEngineStatus status = engine->engine.status();
        copyText(error, errorLen, status.message.empty() ? "audio device did not open" : status.message);
        return false;
    }
    copyText(error, errorLen, "");
    return true;
}

void nc_engine_stop(NCEngine* engine) {
    if (engine != nullptr) {
        engine->engine.stop();
    }
}

void nc_engine_status(NCEngine* engine, NCEngineStatus* out) {
    if (out == nullptr) {
        return;
    }
    std::memset(out, 0, sizeof(*out));
    if (engine == nullptr) {
        return;
    }

    const AudioEngineStatus s = engine->engine.status();

    out->running = s.running;
    out->transportRunning = s.transportRunning;
    out->sampleRate = s.sampleRate;
    out->outputChannels = s.outputChannels;
    out->requestedBufferSize = s.requestedBufferSize;
    out->outputPeakLeft = s.outputPeakLeft;
    out->outputPeakRight = s.outputPeakRight;
    out->phaseCorrelation = s.phaseCorrelation;
    out->spectrumLow = s.spectrumLow;
    out->spectrumMid = s.spectrumMid;
    out->spectrumHigh = s.spectrumHigh;
    out->playbackSeconds = s.playbackSeconds;
    out->delayCompensationEnabled = s.delayCompensationEnabled;
    out->delayCompensationMs = s.delayCompensationMs;
    out->inputChannels = s.inputChannels;
    out->inputPeak = s.inputPeak;
    out->recordArmedTrackCount = s.recordArmedTrackCount;
    out->realtimeCallbackCount = s.realtimeCallbackCount;
    out->realtimeAverageWakeJitterUs = s.realtimeAverageWakeJitterUs;
    out->realtimeMaxWakeJitterUs = s.realtimeMaxWakeJitterUs;
    out->realtimeMaxRenderDurationUs = s.realtimeMaxRenderDurationUs;
    out->realtimeLateWakeCount = s.realtimeLateWakeCount;
    out->remoteDspMonitorActive = s.remoteDspMonitorActive;
    out->remoteDspRoundTripMs = s.remoteDspRoundTripMs;

    const size_t meterCount = std::min({s.trackPeakLeft.size(),
                                        s.trackPeakRight.size(),
                                        static_cast<size_t>(NC_MAX_TRACK_METERS)});
    out->trackMeterCount = static_cast<int>(meterCount);
    for (size_t i = 0; i < meterCount; ++i) {
        out->trackPeakLeft[i] = s.trackPeakLeft[i];
        out->trackPeakRight[i] = s.trackPeakRight[i];
    }

    copyText(out->deviceName, NC_TEXT_LEN, s.deviceName);
    copyText(out->dspEngineName, NC_TEXT_LEN, s.dspEngineName);
    copyText(out->monitorDspPathMode, NC_TEXT_LEN, s.monitorDspPathMode);
    copyText(out->message, NC_TEXT_LEN, s.message);
}

void nc_engine_set_transport_running(NCEngine* engine, bool running) {
    if (engine != nullptr) {
        engine->engine.setTransportRunning(running);
    }
}

void nc_engine_set_recording(NCEngine* engine, bool active) {
    if (engine != nullptr) {
        engine->engine.setTransportRecordingActive(active);
    }
}

void nc_engine_seek(NCEngine* engine, double seconds) {
    if (engine != nullptr) {
        engine->engine.seek(std::max(0.0, seconds));
    }
}

void nc_engine_rewind(NCEngine* engine) {
    if (engine != nullptr) {
        engine->engine.rewind();
    }
}

void nc_engine_set_metronome_enabled(NCEngine* engine, bool enabled) {
    if (engine == nullptr) {
        return;
    }
    engine->engine.setMetronomeEnabled(enabled,
                                       engine->project.tempoBpm,
                                       engine->project.tempoMap,
                                       engine->project.timeSignatureNumerator,
                                       engine->project.timeSignatureDenominator,
                                       engine->project.grooveFeel,
                                       engine->project.grooveSwingAmount,
                                       engine->project.timeSignatureMap,
                                       engine->project.metronomeSubdivision);
}

void nc_engine_set_test_tone_enabled(NCEngine* engine, bool enabled) {
    if (engine != nullptr) {
        engine->engine.setTestToneEnabled(enabled);
    }
}

void nc_project_name(NCEngine* engine, char* out, size_t outLen) {
    copyText(out, outLen, engine != nullptr ? engine->project.name : std::string{});
}

void nc_project_timecode(NCEngine* engine, double seconds, char* out, size_t outLen) {
    if (engine == nullptr) {
        copyText(out, outLen, "");
        return;
    }
    copyText(out, outLen, neuracoust::daw::projectTimecodeString(engine->project, seconds));
}

int nc_project_tempo_bpm(NCEngine* engine) {
    return engine != nullptr ? engine->project.tempoBpm : 120;
}

int nc_project_time_signature_numerator(NCEngine* engine) {
    return engine != nullptr ? engine->project.timeSignatureNumerator : 4;
}

int nc_project_time_signature_denominator(NCEngine* engine) {
    return engine != nullptr ? engine->project.timeSignatureDenominator : 4;
}

void nc_project_bars_beats(NCEngine* engine, double seconds, int* bar, int* beat, int* tick) {
    // Constant-tempo conversion. A project carrying a tempo map will drift here;
    // the legacy UI integrated across markers and this must too before tempo
    // changes are exposed in the UI.
    const int fallbackBar = 1;
    const int fallbackBeat = 1;
    if (engine == nullptr) {
        if (bar) *bar = fallbackBar;
        if (beat) *beat = fallbackBeat;
        if (tick) *tick = 0;
        return;
    }

    const double bpm = std::max(1.0, neuracoust::daw::projectTempoAtSeconds(engine->project, seconds));
    const int beatsPerBar = std::max(1, engine->project.timeSignatureNumerator);
    const double secondsPerBeat = 60.0 / bpm;
    const double totalBeats = std::max(0.0, seconds) / secondsPerBeat;

    const int wholeBeats = static_cast<int>(std::floor(totalBeats));
    const double beatFraction = totalBeats - static_cast<double>(wholeBeats);

    if (bar) *bar = wholeBeats / beatsPerBar + 1;
    if (beat) *beat = wholeBeats % beatsPerBar + 1;
    if (tick) *tick = static_cast<int>(std::floor(beatFraction * 960.0));
}

bool nc_project_loop_enabled(NCEngine* engine) {
    return engine != nullptr && engine->project.loopEnabled;
}

void nc_project_set_loop_enabled(NCEngine* engine, bool enabled) {
    if (engine == nullptr || engine->project.loopEnabled == enabled) {
        return;
    }
    engine->project.loopEnabled = enabled;

    // updateProject first, loadProject only as fallback — never rebuild the whole
    // graph on an edit (docs/legacy-ui-contract.md §1).
    std::string error;
    if (!engine->engine.updateProject(engine->project, error)) {
        engine->engine.loadProject(engine->project, error);
    }
}

double nc_project_loop_start(NCEngine* engine) {
    return engine != nullptr ? engine->project.loopStartSeconds : 0.0;
}

double nc_project_loop_end(NCEngine* engine) {
    return engine != nullptr ? engine->project.loopEndSeconds : 0.0;
}
