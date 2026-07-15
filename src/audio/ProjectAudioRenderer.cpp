#include "audio/ProjectAudioRenderer.h"
#include "audio/MasterInsertProcessor.h"
#include "audio/MixMath.h"
#include "audio/MixerProcessorChain.h"
#include "audio/RemoteDspPluginCatalog.h"
#include "plugins/Vst3HostFoundation.h"
#include "plugins/Vst3ModuleRuntime.h"
#include "plugins/Vst3SdkAdapter.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <deque>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>

namespace neuracoust::daw {

namespace {

constexpr float kPi = 3.14159265358979323846f;

std::string lowercaseCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool isSignalGeneratorInsertName(const std::string& pluginName, const std::string& pluginClassName) {
    const std::string name = lowercaseCopy(pluginName);
    const std::string className = lowercaseCopy(pluginClassName);
    return name.find("emo-generator") != std::string::npos ||
        name.find("signal generator") != std::string::npos ||
        className.find("emo-generator") != std::string::npos ||
        className.find("signal generator") != std::string::npos;
}

void ensureSignalGeneratorDefaults(InsertState& insert) {
    if (!isSignalGeneratorInsertName(insert.pluginName, insert.pluginClassName)) {
        return;
    }
    auto onOffIt = std::find_if(insert.parameters.begin(), insert.parameters.end(), [](const Vst3ParameterValueState& parameter) {
        return parameter.parameterId == 0u;
    });
    if (onOffIt == insert.parameters.end()) {
        insert.parameters.push_back({0u, "On Off", 1.0});
    } else {
        if (onOffIt->displayName.empty()) {
            onOffIt->displayName = "On Off";
        }
        onOffIt->normalizedValue = 1.0f;
    }
}

std::string effectiveTrackInsertDspExecutionMode(const TrackInsertSlot& insert) {
    if (isRemoteInternalDspExecutionMode(insert.dspExecutionMode) &&
        remoteDspCapabilityForInsert(insert, false, true).moduleId.empty()) {
        return "native";
    }
    return insert.dspExecutionMode.empty() ? "native" : insert.dspExecutionMode;
}

double insertParameterValueOr(const InsertState& insert, uint32_t parameterId, double fallback) {
    const auto found = std::find_if(insert.parameters.begin(), insert.parameters.end(), [&](const Vst3ParameterValueState& parameter) {
        return parameter.parameterId == parameterId;
    });
    if (found == insert.parameters.end() || !std::isfinite(found->normalizedValue)) {
        return fallback;
    }
    return std::clamp(static_cast<double>(found->normalizedValue), 0.0, 1.0);
}

float dbToGain(float db) {
    if (db <= -119.5f) {
        return 0.0f;
    }
    return std::pow(10.0f, db / 20.0f);
}

const TrackState* findTrack(const ProjectAudioRenderPlan& plan, const std::string& trackName) {
    auto it = std::find_if(plan.tracks.begin(), plan.tracks.end(), [&](const TrackState& track) {
        return track.name == trackName;
    });
    return it == plan.tracks.end() ? nullptr : &(*it);
}

bool trackExcludedFromSoloPlayback(const TrackState& track) {
    return track.trackType == "master" ||
        track.trackType == "monitor" ||
        track.trackType == "vca" ||
        track.trackType == "folder" ||
        track.name == "Master" ||
        track.name == "Monitor";
}

bool trackParticipatesInSolo(const TrackState& track) {
    return track.solo && !trackExcludedFromSoloPlayback(track);
}

bool hasSoloedAudioTrack(const ProjectAudioRenderPlan& plan) {
    return std::any_of(plan.tracks.begin(), plan.tracks.end(), [](const TrackState& track) {
        return trackParticipatesInSolo(track);
    });
}

bool busAlreadyMarkedForSoloPlayback(const std::vector<std::string>& buses, const std::string& busName) {
    return !busName.empty() && std::find(buses.begin(), buses.end(), busName) != buses.end();
}

bool midiTrackRoutesToInstrument(const TrackState& source, const TrackState& destination) {
    if (source.trackType != "midi" || destination.trackType != "instrument" || destination.name.empty()) {
        return false;
    }
    const std::string explicitRoute = "instrument:" + destination.name;
    return source.outputBus == explicitRoute || source.outputBus == destination.name;
}

bool trackReceivesSoloedPlayback(const ProjectAudioRenderPlan& plan, const TrackState& targetTrack) {
    if (targetTrack.inputBus.empty() || trackExcludedFromSoloPlayback(targetTrack)) {
        return false;
    }
    std::vector<std::string> soloBuses;
    for (const auto& track : plan.tracks) {
        if (trackParticipatesInSolo(track) && !track.muted && !track.outputBus.empty()) {
            soloBuses.push_back(track.outputBus);
        }
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& track : plan.tracks) {
            if (track.muted ||
                trackExcludedFromSoloPlayback(track) ||
                track.inputBus.empty() ||
                !busAlreadyMarkedForSoloPlayback(soloBuses, track.inputBus)) {
                continue;
            }
            if (track.name == targetTrack.name) {
                return true;
            }
            if (!track.outputBus.empty() && !busAlreadyMarkedForSoloPlayback(soloBuses, track.outputBus)) {
                soloBuses.push_back(track.outputBus);
                changed = true;
            }
        }
    }
    return false;
}

bool trackAllowedBySoloPlayback(const ProjectAudioRenderPlan& plan, const TrackState& track) {
    return trackParticipatesInSolo(track) ||
        trackReceivesSoloedPlayback(plan, track) ||
        track.trackType == "master" ||
        track.trackType == "monitor" ||
        track.name == "Master" ||
        track.name == "Monitor";
}

bool trackSupportsPhysicalInputMonitoring(const TrackState& track) {
    return track.trackType == "audio" ||
        track.trackType == "aux" ||
        track.trackType == "bus_folder";
}

std::vector<TempoMarkerState> sortedValidTempoMap(const ProjectAudioRenderPlan& plan) {
    std::vector<TempoMarkerState> markers;
    for (const auto& marker : plan.tempoMap) {
        if (std::isfinite(marker.timeSeconds) && std::isfinite(marker.bpm) &&
            marker.timeSeconds >= 0.0 && marker.bpm >= 20.0 && marker.bpm <= 400.0) {
            markers.push_back(marker);
        }
    }
    std::sort(markers.begin(), markers.end(), [](const TempoMarkerState& left, const TempoMarkerState& right) {
        return left.timeSeconds < right.timeSeconds;
    });
    if (markers.empty()) {
        markers.push_back({0.0, std::max(20.0, std::min(400.0, plan.tempoBpm))});
    }
    return markers;
}

double tempoAtSecondsFromMarkers(const std::vector<TempoMarkerState>& markers, double seconds, double fallbackBpm) {
    if (markers.empty() || !std::isfinite(seconds)) {
        return fallbackBpm;
    }
    const double safeSeconds = std::max(0.0, seconds);
    const TempoMarkerState* left = nullptr;
    const TempoMarkerState* right = nullptr;
    for (const auto& marker : markers) {
        if (marker.timeSeconds <= safeSeconds) {
            left = &marker;
        } else {
            right = &marker;
            break;
        }
    }
    if (left == nullptr && right == nullptr) {
        return fallbackBpm;
    }
    if (left == nullptr) {
        return right->bpm;
    }
    if (right == nullptr) {
        return left->bpm;
    }
    const double span = right->timeSeconds - left->timeSeconds;
    if (span <= 0.0) {
        return right->bpm;
    }
    const double t = std::max(0.0, std::min(1.0, (safeSeconds - left->timeSeconds) / span));
    return left->bpm + (right->bpm - left->bpm) * t;
}

double secondsForBeatOffsetFromTempoMap(const std::vector<TempoMarkerState>& markers,
                                        double startSeconds,
                                        double beatOffset,
                                        double fallbackBpm) {
    if (!std::isfinite(startSeconds) || !std::isfinite(beatOffset) || beatOffset <= 0.0) {
        return std::max(0.0, startSeconds);
    }
    double time = std::max(0.0, startSeconds);
    double remainingBeats = beatOffset;
    size_t guard = 0;
    while (remainingBeats > 0.0000001 && guard++ < 10000) {
        const double currentBpm = std::max(20.0, std::min(400.0, tempoAtSecondsFromMarkers(markers, time, fallbackBpm)));
        const TempoMarkerState* left = nullptr;
        const TempoMarkerState* right = nullptr;
        for (const auto& marker : markers) {
            if (marker.timeSeconds <= time + 0.0000001) {
                left = &marker;
            } else {
                right = &marker;
                break;
            }
        }
        if (left == nullptr && !markers.empty()) {
            left = &markers.front();
        }
        const double segmentEnd = right != nullptr ? right->timeSeconds : std::numeric_limits<double>::infinity();
        const double slope = (left != nullptr && right != nullptr && right->timeSeconds > left->timeSeconds)
            ? (right->bpm - left->bpm) / (right->timeSeconds - left->timeSeconds)
            : 0.0;
        const double segmentDuration = std::isfinite(segmentEnd) ? std::max(0.0, segmentEnd - time) : std::numeric_limits<double>::infinity();
        const double segmentBeats = std::isfinite(segmentDuration)
            ? std::max(0.0, (currentBpm * segmentDuration + 0.5 * slope * segmentDuration * segmentDuration) / 60.0)
            : std::numeric_limits<double>::infinity();
        if (remainingBeats <= segmentBeats + 0.0000001 || !std::isfinite(segmentBeats)) {
            if (std::abs(slope) < 0.0000001) {
                return time + remainingBeats * 60.0 / currentBpm;
            }
            const double a = 0.5 * slope;
            const double b = currentBpm;
            const double c = -remainingBeats * 60.0;
            const double discriminant = std::max(0.0, b * b - 4.0 * a * c);
            const double dt = (-b + std::sqrt(discriminant)) / (2.0 * a);
            if (std::isfinite(dt) && dt >= 0.0) {
                return time + dt;
            }
            return time + remainingBeats * 60.0 / currentBpm;
        }
        if (!std::isfinite(segmentDuration) || segmentDuration <= 0.0) {
            return time + remainingBeats * 60.0 / currentBpm;
        }
        remainingBeats -= segmentBeats;
        time = segmentEnd;
    }
    return time;
}

bool trackInputMonitorOverridesTimelinePlayback(const ProjectAudioRenderPlan& plan, const TrackState& track) {
    return trackSupportsPhysicalInputMonitoring(track) &&
        (track.inputMonitoring || (plan.transportRecordingActive && track.recordArmed));
}

bool isMainBus(const std::string& busName) {
    return busName.rfind("Main", 0) == 0;
}

bool isInternalBus(const std::string& busName) {
    return busName.rfind("Bus ", 0) == 0;
}

bool isMasterOutputBus(const std::string& busName) {
    return busName == "Master" || isMainBus(busName);
}

bool routeUsesTrackChannelFormat(MixerRouteKind kind) {
    return kind == MixerRouteKind::Audio ||
        kind == MixerRouteKind::Instrument ||
        kind == MixerRouteKind::Aux ||
        kind == MixerRouteKind::RoutingFolder;
}

bool trackIsMonoChannel(const TrackState* track, MixerRouteKind kind) {
    return track != nullptr &&
        track->channelFormat == "mono" &&
        routeUsesTrackChannelFormat(kind);
}

MixerStereoFrame applyTrackChannelFormat(MixerStereoFrame frame, const TrackState* track, MixerRouteKind kind) {
    if (!trackIsMonoChannel(track, kind)) {
        return frame;
    }
    const float mono = (frame.left + frame.right) * 0.5f;
    return {mono, mono};
}

void applyTrackChannelFormatToBlock(std::vector<float>& interleavedStereo,
                                    const TrackState* track,
                                    MixerRouteKind kind) {
    if (!trackIsMonoChannel(track, kind)) {
        return;
    }
    for (size_t index = 0; index + 1 < interleavedStereo.size(); index += 2) {
        const float mono = (interleavedStereo[index] + interleavedStereo[index + 1]) * 0.5f;
        interleavedStereo[index] = mono;
        interleavedStereo[index + 1] = mono;
    }
}

float automationValueAt(const std::vector<AutomationPointState>& points, double timeSeconds, float fallback) {
    if (points.empty() || !std::isfinite(timeSeconds)) {
        return fallback;
    }
    if (timeSeconds <= points.front().timeSeconds) {
        return points.front().value;
    }
    for (size_t index = 1; index < points.size(); ++index) {
        const auto& right = points[index];
        if (timeSeconds <= right.timeSeconds) {
            const auto& left = points[index - 1];
            const double span = right.timeSeconds - left.timeSeconds;
            if (span <= 0.0) {
                return right.value;
            }
            const double t = std::max(0.0, std::min(1.0, (timeSeconds - left.timeSeconds) / span));
            return static_cast<float>(left.value + (right.value - left.value) * t);
        }
    }
    return points.back().value;
}

float automationLaneValueAt(const TrackState& track, const std::string& parameterId, double timeSeconds, float fallback) {
    if (track.automationMode == "off") return fallback;   // Off ignores written automation
    auto laneIt = std::find_if(track.automationLanes.begin(), track.automationLanes.end(), [&](const AutomationLaneState& lane) {
        return lane.parameterId == parameterId;
    });
    if (laneIt == track.automationLanes.end()) {
        return fallback;
    }
    return automationValueAt(laneIt->points, timeSeconds, fallback);
}

const TrackState* vcaControlMasterForTrack(const ProjectAudioRenderPlan& plan, const TrackState& track) {
    if (track.controlMasterTrackName.empty() ||
        track.trackType == "master" ||
        track.trackType == "monitor" ||
        track.trackType == "vca" ||
        track.name == "Master" ||
        track.name == "Monitor") {
        return nullptr;
    }
    const TrackState* controlMaster = findTrack(plan, track.controlMasterTrackName);
    if (controlMaster == nullptr || controlMaster->trackType != "vca") {
        return nullptr;
    }
    return controlMaster;
}

bool trackPlaybackMuted(const ProjectAudioRenderPlan& plan, const TrackState& track, bool soloMode) {
    if (track.muted || (soloMode && !trackAllowedBySoloPlayback(plan, track))) {
        return true;
    }
    const TrackState* controlMaster = vcaControlMasterForTrack(plan, track);
    return controlMaster != nullptr && controlMaster->muted;
}

float effectiveTrackVolumeDb(const ProjectAudioRenderPlan& plan, const TrackState& track, double timeSeconds) {
    float volumeDb = track.automationMode == "off"
        ? track.volumeDb
        : automationValueAt(track.volumeAutomation, timeSeconds, track.volumeDb);
    const TrackState* controlMaster = vcaControlMasterForTrack(plan, track);
    if (controlMaster != nullptr) {
        volumeDb += automationValueAt(controlMaster->volumeAutomation, timeSeconds, controlMaster->volumeDb);
    }
    return std::max(-120.0f, std::min(24.0f, volumeDb));
}

float fadeCurveGain(const std::string& curve, double normalized) {
    const float x = static_cast<float>(std::max(0.0, std::min(1.0, normalized)));
    if (curve == "linear") {
        return x;
    }
    if (curve == "slow") {
        return x * x;
    }
    if (curve == "fast") {
        return std::sqrt(x);
    }
    return std::sin(x * kPi * 0.5f);
}

float clipFadeGain(const ClipState& clip, double localSeconds) {
    float gain = 1.0f;
    if (clip.fadeInSeconds > 0.0) {
        const double normalized = std::max(0.0, std::min(1.0, localSeconds / clip.fadeInSeconds));
        gain = std::min(gain, fadeCurveGain(clip.fadeInCurve, normalized));
    }
    if (clip.fadeOutSeconds > 0.0) {
        const double remaining = clip.durationSeconds - localSeconds;
        const double normalized = std::max(0.0, std::min(1.0, remaining / clip.fadeOutSeconds));
        gain = std::min(gain, fadeCurveGain(clip.fadeOutCurve, normalized));
    }
    return gain;
}

inline double normalizedSinc(double x) {
    if (std::abs(x) < 1e-9) return 1.0;
    const double px = kPi * x;
    return std::sin(px) / px;
}

// Blackman-windowed-sinc resampler. `step` is how many source frames advance per output
// frame (sourceRate/projectRate ÷ timeScale): >1 means downsampling, so the sinc is
// lowpassed to the output Nyquist to stop aliasing; <=1 reconstructs at the source Nyquist.
// Replaces the old 2-point linear interpolation, which had no anti-aliasing and rolled off
// the highs. A step≈1, integer-aligned read takes a direct fast path (the common case).
constexpr int kSrcZeroCrossings = 16;   // taps each side at unity; widened on downsample

float sourceSampleAt(const WavAudioData& source, double sourceFrame, int channel, double step = 1.0) {
    if (source.channels <= 0 || source.interleavedSamples.empty() || sourceFrame < 0.0) {
        return 0.0f;
    }
    const int64_t frameCount = source.frameCount();
    const int ch = source.channels > 1 ? std::min(channel, source.channels - 1) : 0;
    const auto sampleAt = [&](int64_t f) -> double {
        if (f < 0 || f >= frameCount) return 0.0;
        const size_t idx = static_cast<size_t>(f * source.channels + ch);
        return idx < source.interleavedSamples.size() ? static_cast<double>(source.interleavedSamples[idx]) : 0.0;
    };

    const int64_t base = static_cast<int64_t>(std::floor(sourceFrame));
    const double frac = sourceFrame - static_cast<double>(base);
    if (base < 0 || base >= frameCount) return 0.0f;

    // Fast path: unity read on an integer boundary (no resampling / time-stretch).
    if (step <= 1.0000001 && frac < 1e-7) {
        return static_cast<float>(sampleAt(base));
    }

    const double cutoff = std::min(1.0, 1.0 / std::max(1.0, step));   // fraction of source Nyquist
    // Kernel widens as it downsamples, but cap it — beyond ~4x the extra taps cost more than
    // they buy, and an unbounded kernel would be unsafe in the realtime path.
    const int reach = std::min(64, static_cast<int>(std::ceil(static_cast<double>(kSrcZeroCrossings) / cutoff)));
    double acc = 0.0, norm = 0.0;
    for (int k = -reach + 1; k <= reach; ++k) {
        const double d = frac - static_cast<double>(k);              // distance to sample base+k
        const double arg = cutoff * d;
        if (std::abs(arg) > static_cast<double>(kSrcZeroCrossings)) continue;
        const double t = (arg / static_cast<double>(kSrcZeroCrossings) + 1.0) * 0.5;   // 0..1
        const double win = 0.42 - 0.5 * std::cos(2.0 * kPi * t) + 0.08 * std::cos(4.0 * kPi * t);
        const double h = cutoff * normalizedSinc(arg) * win;
        acc += sampleAt(base + k) * h;
        norm += h;
    }
    return static_cast<float>(norm != 0.0 ? acc / norm : acc);
}

std::pair<float, float> dryClipSampleAtTimelineFrame(const ProjectRenderClip& renderClip,
                                                     int64_t timelineFrame,
                                                     double sampleRate) {
    const auto& clip = renderClip.clip;
    const auto clipStartFrame = static_cast<int64_t>(clip.startSeconds * sampleRate);
    const auto clipEndFrame = static_cast<int64_t>((clip.startSeconds + clip.durationSeconds) * sampleRate);
    if (clip.muted || timelineFrame < clipStartFrame || timelineFrame >= clipEndFrame) {
        return {0.0f, 0.0f};
    }
    const float clipGain = dbToLinearGain(clip.gainDb);
    const double sourceRateRatio = renderClip.source.sampleRate > 0
        ? static_cast<double>(renderClip.source.sampleRate) / sampleRate
        : 1.0;
    const double timeScale = std::max(0.05, std::min(20.0, std::isfinite(clip.timeScale) ? clip.timeScale : 1.0));
    const double clipLocalSeconds = static_cast<double>(timelineFrame - clipStartFrame) / sampleRate;
    const float fadeGain = clipFadeGain(clip, clipLocalSeconds);
    const float polarity = clip.polarityInverted ? -1.0f : 1.0f;
    const double sourceFrame = clip.sourceOffsetSeconds * renderClip.source.sampleRate
        + (static_cast<double>(timelineFrame - clipStartFrame) * sourceRateRatio / timeScale);
    const double resampleStep = sourceRateRatio / timeScale;         // source frames per output frame
    const float dryLeft = sourceSampleAt(renderClip.source, sourceFrame, 0, resampleStep) * clipGain * fadeGain * polarity;
    const float dryRight = sourceSampleAt(renderClip.source, sourceFrame, 1, resampleStep) * clipGain * fadeGain * polarity;
    return {dryLeft, dryRight};
}

bool hasValidLoopRange(const ProjectAudioRenderPlan& plan) {
    return plan.loopEnabled && plan.loopEndSeconds > plan.loopStartSeconds && plan.sampleRate > 0.0;
}

int64_t loopStartFrame(const ProjectAudioRenderPlan& plan) {
    return std::max<int64_t>(0, static_cast<int64_t>(std::round(plan.loopStartSeconds * plan.sampleRate)));
}

int64_t loopEndFrame(const ProjectAudioRenderPlan& plan) {
    return std::max<int64_t>(0, static_cast<int64_t>(std::round(plan.loopEndSeconds * plan.sampleRate)));
}

int64_t timelineFrameForPlaybackFrame(const ProjectAudioRenderPlan& plan, int64_t playbackFrame) {
    if (!hasValidLoopRange(plan)) {
        return playbackFrame;
    }
    const int64_t start = loopStartFrame(plan);
    const int64_t end = loopEndFrame(plan);
    const int64_t length = end - start;
    if (length <= 0 || playbackFrame < end) {
        return playbackFrame;
    }
    return start + ((playbackFrame - start) % length);
}

unsigned int routeDelayCompensationSamples(const ProjectAudioRenderPlan& plan, const std::string& routeName) {
    if (!plan.delayCompensationEnabled) {
        return 0;
    }
    const auto it = plan.routeDelayCompensationSamples.find(routeName);
    return it == plan.routeDelayCompensationSamples.end() ? 0 : it->second;
}

MixerStereoFrame applyRouteDelayCompensation(std::map<std::string, std::deque<MixerStereoFrame>>& delayLines,
                                             const std::string& key,
                                             MixerStereoFrame frame,
                                             unsigned int delaySamples) {
    if (delaySamples == 0) {
        return frame;
    }
    auto& line = delayLines[key];
    line.push_back(frame);
    if (line.size() <= delaySamples) {
        return {};
    }
    const MixerStereoFrame delayed = line.front();
    line.pop_front();
    return delayed;
}

void mixTimelineFrame(const ProjectAudioRenderPlan& plan,
                      int64_t timelineFrame,
                      int64_t blockFrameOffset,
                      bool soloMode,
                      float& leftOut,
                      float& rightOut,
                      std::map<std::string, std::deque<MixerStereoFrame>>* routeDelayLines = nullptr,
                      MonitorDspProcessor* monitorDspProcessor = nullptr,
                      const std::map<std::string, std::vector<float>>* instrumentAudioBlocks = nullptr,
                      std::map<std::string, std::pair<float, float>>* trackFrameSums = nullptr) {
    MixerGraph fallbackGraph;
    const MixerGraph* graph = &plan.mixerGraph;
    if (graph->routes.empty()) {
        ProjectDocument fallbackProject;
        fallbackProject.tracks = plan.tracks;
        fallbackGraph = buildMixerGraph(fallbackProject);
        graph = &fallbackGraph;
    }
    std::map<std::string, std::pair<float, float>> busSums;
    const double timelineSeconds = static_cast<double>(timelineFrame) / plan.sampleRate;
    for (const auto& routeName : graph->renderOrder) {
        const auto route = std::find_if(graph->routes.begin(), graph->routes.end(), [&](const MixerRouteNode& candidate) {
            return candidate.name == routeName;
        });
        if (route == graph->routes.end() || !route->audioCarrying) {
            continue;
        }
        const TrackState* track = findTrack(plan, route->name);
        if (track != nullptr && trackPlaybackMuted(plan, *track, soloMode)) {
            continue;
        }
        const bool timelinePlaybackSuppressed = track != nullptr && trackInputMonitorOverridesTimelinePlayback(plan, *track);
        const bool hasLiveInstrumentBlock = route->kind == MixerRouteKind::Instrument &&
            instrumentAudioBlocks != nullptr &&
            instrumentAudioBlocks->find(route->name) != instrumentAudioBlocks->end();
        if (timelinePlaybackSuppressed && !hasLiveInstrumentBlock) {
            continue;
        }

        MixerStereoFrame routeInput;
        if (!timelinePlaybackSuppressed &&
            (route->kind == MixerRouteKind::Audio || route->kind == MixerRouteKind::Instrument)) {
            for (const auto& renderClip : plan.clips) {
                if (renderClip.clip.trackName != route->name) {
                    continue;
                }
                const auto [dryLeft, dryRight] = dryClipSampleAtTimelineFrame(renderClip, timelineFrame, plan.sampleRate);
                routeInput.left += dryLeft;
                routeInput.right += dryRight;
            }
        }
        if (route->kind == MixerRouteKind::Instrument && instrumentAudioBlocks != nullptr) {
            const auto instrumentIt = instrumentAudioBlocks->find(route->name);
            const auto sampleIndex = static_cast<size_t>(std::max<int64_t>(0, blockFrameOffset)) * 2;
            if (instrumentIt != instrumentAudioBlocks->end() && sampleIndex + 1 < instrumentIt->second.size()) {
                routeInput.left += instrumentIt->second[sampleIndex];
                routeInput.right += instrumentIt->second[sampleIndex + 1];
            }
        }
        const std::string receiveBus = !route->inputBus.empty() ? route->inputBus : route->name;
        if (const auto bus = busSums.find(receiveBus); bus != busSums.end()) {
            routeInput.left += bus->second.first;
            routeInput.right += bus->second.second;
        }
        routeInput = applyTrackChannelFormat(routeInput, track, route->kind);

        MixerRouteProcessorInput processorInput;
        processorInput.input = routeInput;
        processorInput.routeKind = route->kind;
        processorInput.applyRouteFaderPan = track != nullptr &&
            route->kind != MixerRouteKind::Master &&
            route->kind != MixerRouteKind::Monitor;
        if (track != nullptr) {
            processorInput.gainDb = effectiveTrackVolumeDb(plan, *track, timelineSeconds);
            processorInput.pan = automationLaneValueAt(*track, "track.pan", timelineSeconds, track->pan);
            processorInput.panLaw = plan.panLaw;
            processorInput.isMonoTrack = track->channelFormat == "mono";
            processorInput.sends = track->sends;
        }
        const auto processedRoute = processMixerRouteProcessors(processorInput);
        auto postProcessorFrame = processedRoute.postFader;
        if (route->kind == MixerRouteKind::Monitor &&
            plan.renderMonitorDsp &&
            monitorDspProcessor != nullptr) {
            const float monitorInputTrimGain = dbToGain(std::max(-12.0f, std::min(0.0f, plan.monitorInputTrimDb)));
            postProcessorFrame.left *= monitorInputTrimGain;
            postProcessorFrame.right *= monitorInputTrimGain;
            StereoFrame monitorFrame;
            monitorFrame.left = postProcessorFrame.left;
            monitorFrame.right = postProcessorFrame.right;
            const StereoFrame processedMonitorFrame = monitorDspProcessor->process(monitorFrame);
            postProcessorFrame.left = processedMonitorFrame.left;
            postProcessorFrame.right = processedMonitorFrame.right;
        }
        const unsigned int routeDelaySamples = routeDelayCompensationSamples(plan, route->name);
        const auto delayedRouteFrame = routeDelayLines != nullptr
            ? applyRouteDelayCompensation(*routeDelayLines,
                                          "route:" + route->name,
                                          postProcessorFrame,
                                          routeDelaySamples)
            : postProcessorFrame;
        const auto routeLeft = delayedRouteFrame.left;
        const auto routeRight = delayedRouteFrame.right;
        for (const auto& sendTap : processedRoute.sendTaps) {
            const auto delayedSendFrame = routeDelayLines != nullptr
                ? applyRouteDelayCompensation(*routeDelayLines,
                                              "send:" + route->name + ":" + sendTap.busName,
                                              sendTap.frame,
                                              routeDelaySamples)
                : sendTap.frame;
            auto& sum = busSums[sendTap.busName];
            sum.first += delayedSendFrame.left;
            sum.second += delayedSendFrame.right;
        }

        if (trackFrameSums != nullptr && track != nullptr &&
            route->kind != MixerRouteKind::Master &&
            route->kind != MixerRouteKind::Monitor) {
            auto& sum = (*trackFrameSums)[track->name];
            sum.first += routeLeft;
            sum.second += routeRight;
        }

        for (const auto& edge : graph->edges) {
            if (edge.sourceRoute != route->name || edge.send) {
                continue;
            }
            if (edge.physicalOutput) {
                leftOut += routeLeft;
                rightOut += routeRight;
                continue;
            }
            auto& sum = busSums[edge.busName];
            sum.first += routeLeft;
            sum.second += routeRight;
        }
    }

    if (findTrack(plan, "Master") == nullptr) {
        if (const auto masterBus = busSums.find("Master"); masterBus != busSums.end()) {
            leftOut += masterBus->second.first;
            rightOut += masterBus->second.second;
        }
    }
    if (findTrack(plan, "Monitor") == nullptr) {
        if (const auto monitorBus = busSums.find("Monitor"); monitorBus != busSums.end()) {
            leftOut += monitorBus->second.first;
            rightOut += monitorBus->second.second;
        }
    }

    if (const TrackState* master = findTrack(plan, "Master")) {
        if (master->muted) {
            leftOut = 0.0f;
            rightOut = 0.0f;
        } else {
            const auto masterFrame = applyMixerMasterGainBalance({leftOut, rightOut},
                                                                automationValueAt(master->volumeAutomation, timelineSeconds, master->volumeDb),
                                                                automationLaneValueAt(*master, "track.pan", timelineSeconds, master->pan));
            leftOut = masterFrame.left;
            rightOut = masterFrame.right;
        }
    }
}

bool trackInsertIsActiveVst3(const TrackInsertSlot& insert) {
    InsertState renderInsert;
    renderInsert.pluginName = insert.pluginName;
    renderInsert.pluginFormat = insert.pluginFormat;
    renderInsert.pluginPath = insert.pluginPath;
    renderInsert.bypassed = insert.bypassed;
    renderInsert.available = insert.enabled;
    renderInsert.dspExecutionMode = insert.dspExecutionMode;
    renderInsert.assignedDspServerId = insert.assignedDspServerId;
    renderInsert.serverModuleId = insert.serverModuleId;
    renderInsert.reportedLatencySamples = insert.reportedLatencySamples;
    renderInsert.dspAvailable = insert.dspAvailable;
    renderInsert.dspLastError = insert.dspLastError;
    renderInsert.parameters = insert.parameters;
    return insert.enabled && !insert.bypassed && isVst3MasterInsert(renderInsert);
}

InsertState renderInsertForTrackInsert(const TrackInsertSlot& insert) {
    InsertState renderInsert;
    renderInsert.pluginName = insert.pluginName;
    renderInsert.pluginFormat = insert.pluginFormat;
    renderInsert.pluginPath = insert.pluginPath;
    renderInsert.pluginClassId = insert.pluginClassId;
    renderInsert.pluginClassName = insert.pluginClassName;
    renderInsert.bypassed = insert.bypassed;
    renderInsert.available = insert.enabled;
    renderInsert.dspExecutionMode = effectiveTrackInsertDspExecutionMode(insert);
    renderInsert.assignedDspServerId = insert.assignedDspServerId;
    renderInsert.serverModuleId = insert.serverModuleId;
    renderInsert.reportedLatencySamples = insert.reportedLatencySamples;
    renderInsert.dspAvailable = insert.dspAvailable;
    renderInsert.dspLastError = insert.dspLastError;
    renderInsert.parameters = insert.parameters;
    ensureSignalGeneratorDefaults(renderInsert);
    return renderInsert;
}

bool routeMayProcessInserts(MixerRouteKind kind) {
    return kind == MixerRouteKind::Audio ||
        kind == MixerRouteKind::Instrument ||
        kind == MixerRouteKind::Aux ||
        kind == MixerRouteKind::RoutingFolder;
}

bool trackInsertShouldRunInLocalRouteGraph(const TrackInsertSlot& insert) {
    if (!trackInsertIsActiveVst3(insert) || insert.pluginPath.empty()) {
        return false;
    }
    // Leave only server-backed remote/external slots for the NDS path. A
    // third-party VST3 with no server module must still run in the local
    // Native-safe insert graph even if an old project saved a remote mode.
    return !isRemoteInternalDspExecutionMode(effectiveTrackInsertDspExecutionMode(insert)) ||
        insert.serverModuleId.empty();
}

std::vector<InsertState> activeLocalRouteInserts(const TrackState& track) {
    std::vector<InsertState> inserts;
    for (const auto& insert : track.inserts) {
        if (!trackInsertShouldRunInLocalRouteGraph(insert)) {
            continue;
        }
        inserts.push_back(renderInsertForTrackInsert(insert));
    }
    return inserts;
}

// Deterministic bridge shared-memory keys for the active local route inserts, in
// the SAME order activeLocalRouteInserts() returns them, keyed by the original
// track insert slot so the editor host / UI can observe the exact insert.
std::vector<std::string> activeLocalRouteInsertShmKeys(const TrackState& track) {
    std::vector<std::string> keys;
    for (size_t slotIndex = 0; slotIndex < track.inserts.size(); ++slotIndex) {
        if (!trackInsertShouldRunInLocalRouteGraph(track.inserts[slotIndex])) {
            continue;
        }
        keys.push_back(track.name + "\x1f" + std::to_string(slotIndex));
    }
    return keys;
}

std::string routeInsertChainKey(const std::vector<InsertState>& inserts,
                                double sampleRate,
                                int maxBlockSize) {
    std::string key = std::to_string(static_cast<int>(std::round(sampleRate))) + ":" +
        std::to_string(std::max(1, maxBlockSize)) + "\n";
    for (const auto& insert : inserts) {
        key += insert.pluginName + "|" +
            insert.pluginPath + "|" +
            insert.pluginClassId + "|" +
            insert.pluginClassName + "|" +
            insert.dspExecutionMode + "|";
        key += "\n";
    }
    return key;
}

bool processRouteInsertBlock(ProjectAudioRenderState& state,
                             const std::string& routeName,
                             const std::vector<InsertState>& inserts,
                             double sampleRate,
                             int64_t frameCount,
                             std::vector<float>& interleavedStereo,
                             std::string& error,
                             const std::vector<RealtimeMasterInsertChain::InsertProcessMeter>** processMeters = nullptr,
                             const std::vector<std::string>& bridgeShmKeys = {},
                             bool synchronousPrepare = false,
                             bool* processedWet = nullptr) {
    error.clear();
    if (processMeters != nullptr) {
        *processMeters = nullptr;
    }
    if (processedWet != nullptr) {
        *processedWet = false;   // true only once the chain actually runs on this block
    }
    if (inserts.empty() || frameCount <= 0 || interleavedStereo.size() < static_cast<size_t>(frameCount) * 2u) {
        return true;
    }
    const int requestedBlock = static_cast<int>(std::max<int64_t>(1, frameCount));
    // Key on plug-in identity + sample rate only, NOT the per-segment block size.
    // Loop boundaries render a short final segment; keying on its size would tear
    // down and rebuild the chain (re-spawning the sandbox worker on the audio
    // thread → a big jitter spike and distortion at every loop). Instead we keep
    // one chain prepared for the largest block seen and just process fewer frames.
    if (state.insertPreparer == nullptr) {
        state.insertPreparer = std::make_unique<AsyncInsertChainPreparer>();
    }
    const int prepareMax = std::max(requestedBlock, 4096);   // one max-block prepare, reused
    const std::string key = routeInsertChainKey(inserts, sampleRate, 0);
    auto keyIt = state.routeInsertChainKeys.find(routeName);
    const bool identityChanged = keyIt == state.routeInsertChainKeys.end() || keyIt->second != key;
    if (identityChanged) {
        // Retire the old chain OFF the audio thread (worker teardown blocks), and mark the new
        // signature. Prepare happens in the background; until it's ready we pass dry.
        auto old = state.routeInsertChains.find(routeName);
        if (old != state.routeInsertChains.end()) {
            state.insertPreparer->retire(std::move(old->second));
            state.routeInsertChains.erase(old);
        }
        state.routeInsertChainKeys[routeName] = key;
        state.routeInsertChainMaxBlock.erase(routeName);
    }
    auto chainIt = state.routeInsertChains.find(routeName);
    if (chainIt == state.routeInsertChains.end()) {
        if (synchronousPrepare) {
            // Offline (bounce): faster than realtime, so the background thread can't keep up —
            // prepare inline. Blocking is fine off the audio thread.
            ProjectAudioRenderPlan offlinePlan;
            offlinePlan.sampleRate = sampleRate;
            offlinePlan.hasActiveVst3Inserts = true;
            offlinePlan.activeVst3Inserts = inserts;
            auto chain = std::make_unique<RealtimeMasterInsertChain>();
            if (!chain->prepare(offlinePlan, sampleRate, prepareMax, error, bridgeShmKeys)) {
                state.routeInsertLastErrors[routeName] = error;
                return false;
            }
            auto [inserted, _] = state.routeInsertChains.emplace(routeName, std::move(chain));
            chainIt = inserted;
            state.routeInsertChainMaxBlock[routeName] = prepareMax;
            state.routeInsertLastErrors.erase(routeName);
        } else {
            // Realtime: ask the background thread to prepare it, and try to pick up a chain it
            // already finished for this signature. No blocking prepare on the audio thread.
            auto ready = state.insertPreparer->tryTake(key);
            if (ready != nullptr) {
                auto [inserted, _] = state.routeInsertChains.emplace(routeName, std::move(ready));
                chainIt = inserted;
                state.routeInsertChainMaxBlock[routeName] = prepareMax;
                state.routeInsertLastErrors.erase(routeName);
            } else {
                state.insertPreparer->request(key, inserts, sampleRate, prepareMax, bridgeShmKeys);
                const std::string err = state.insertPreparer->lastError(key);
                if (!err.empty()) state.routeInsertLastErrors[routeName] = err;
                // Play dry until the chain is ready — the input is already in interleavedStereo.
                return true;
            }
        }
    }
    RealtimeMasterInsertChain& chain = *chainIt->second;
    for (size_t insertIndex = 0; insertIndex < inserts.size(); ++insertIndex) {
        for (const auto& parameter : inserts[insertIndex].parameters) {
            chain.updateParameter(insertIndex,
                                  parameter.parameterId,
                                  parameter.displayName,
                                  parameter.normalizedValue);
        }
    }
    if (!chain.processInterleavedStereo(interleavedStereo, static_cast<int>(frameCount), error)) {
        state.routeInsertLastErrors[routeName] = error;
        return false;
    }
    if (processMeters != nullptr) {
        *processMeters = &chain.lastProcessMeters();
    }
    if (processedWet != nullptr) {
        *processedWet = true;
    }
    state.routeInsertLastErrors.erase(routeName);
    return true;
}

void addBlockToBus(std::map<std::string, std::vector<float>>& busBlocks,
                   const std::string& busName,
                   const std::vector<float>& block) {
    if (busName.empty() || block.empty()) {
        return;
    }
    auto& sum = busBlocks[busName];
    if (sum.size() < block.size()) {
        sum.resize(block.size(), 0.0f);
    }
    for (size_t index = 0; index < block.size(); ++index) {
        sum[index] += block[index];
    }
}

float blockPeakAbs(const std::vector<float>& block) {
    float peak = 0.0f;
    for (const float sample : block) {
        peak = std::max(peak, std::abs(sample));
    }
    return std::clamp(peak, 0.0f, 1.0f);
}

bool insertIsSourceGenerator(const InsertState& insert) {
    return isSignalGeneratorInsertName(insert.pluginName, insert.pluginClassName);
}

bool synthesizeSourceGeneratorFallback(ProjectAudioRenderState& state,
                                       const std::string& routeName,
                                       const std::vector<InsertState>& inserts,
                                       double sampleRate,
                                       int64_t frameCount,
                                       std::vector<float>& interleavedStereo) {
    if (frameCount <= 0 || sampleRate <= 1000.0 ||
        interleavedStereo.size() < static_cast<size_t>(frameCount) * 2u) {
        return false;
    }
    const auto generator = std::find_if(inserts.begin(), inserts.end(), insertIsSourceGenerator);
    if (generator == inserts.end()) {
        return false;
    }
    const double on = insertParameterValueOr(*generator, 0u, 1.0);
    if (on < 0.5) {
        return false;
    }
    const double signal = insertParameterValueOr(*generator, 1u, 0.0);
    const double frequencyNorm = insertParameterValueOr(*generator, 2u, 0.562351);
    const double gainNorm = insertParameterValueOr(*generator, 3u, 0.833);
    const double route = insertParameterValueOr(*generator, 4u, 0.5);
    const double phaseFlip = insertParameterValueOr(*generator, 5u, 0.0);
    const double frequency = std::clamp(100.0 * std::pow(100.0, frequencyNorm), 20.0, 20000.0);
    const float gain = dbToGain(static_cast<float>(-60.0 + gainNorm * 48.0));
    double& phase = state.sourceGeneratorPhases[routeName];
    uint32_t noise = static_cast<uint32_t>(std::hash<std::string>{}(routeName)) ^ 0x9E3779B9u;
    float pink = 0.0f;
    for (int64_t frame = 0; frame < frameCount; ++frame) {
        float sample = 0.0f;
        if (signal < 0.25) {
            sample = static_cast<float>(std::sin(phase));
            phase += (2.0 * 3.14159265358979323846 * frequency) / sampleRate;
            if (phase >= 2.0 * 3.14159265358979323846) {
                phase = std::fmod(phase, 2.0 * 3.14159265358979323846);
            }
        } else {
            noise = noise * 1664525u + 1013904223u;
            const float white = (static_cast<float>((noise >> 8) & 0x00FFFFFFu) / 8388607.5f) - 1.0f;
            if (signal < 0.75) {
                sample = white;
            } else {
                pink = 0.98f * pink + 0.02f * white;
                sample = pink * 3.0f;
            }
        }
        sample = std::clamp(sample * gain, -1.0f, 1.0f);
        float left = sample;
        float right = sample;
        if (route < 0.25) {
            right = 0.0f;
        } else if (route > 0.75) {
            left = 0.0f;
        }
        if (phaseFlip >= 0.5) {
            right = -right;
        }
        const auto index = static_cast<size_t>(frame) * 2u;
        interleavedStereo[index] = left;
        interleavedStereo[index + 1u] = right;
    }
    return blockPeakAbs(interleavedStereo) > 0.000001f;
}

unsigned int activeTrackVst3LatencySamples(const TrackState& track, int sampleRate) {
    if (sampleRate <= 0) {
        return 0;
    }
    unsigned int total = 0;
    for (const auto& insert : track.inserts) {
        if (!trackInsertIsActiveVst3(insert) || insert.pluginPath.empty()) {
            continue;
        }
        if (isAnyInternalDspExecutionMode(insert.dspExecutionMode) &&
            insert.reportedLatencySamples > 0) {
            total += insert.reportedLatencySamples;
            continue;
        }
        InsertState renderInsert;
        renderInsert.pluginName = insert.pluginName;
        renderInsert.pluginFormat = insert.pluginFormat;
        renderInsert.pluginPath = insert.pluginPath;
        renderInsert.pluginClassId = insert.pluginClassId;
        renderInsert.pluginClassName = insert.pluginClassName;
        renderInsert.bypassed = insert.bypassed;
        renderInsert.available = insert.enabled;
        renderInsert.dspExecutionMode = insert.dspExecutionMode;
        renderInsert.assignedDspServerId = insert.assignedDspServerId;
        renderInsert.serverModuleId = insert.serverModuleId;
        renderInsert.reportedLatencySamples = insert.reportedLatencySamples;
        renderInsert.dspAvailable = insert.dspAvailable;
        renderInsert.dspLastError = insert.dspLastError;
        renderInsert.parameters = insert.parameters;
        const auto descriptor = resolveVst3PluginDescriptorForInsert(renderInsert.pluginName,
                                                                    renderInsert.pluginPath,
                                                                    renderInsert.pluginClassId,
                                                                    renderInsert.pluginClassName);
        total += probeVst3ProcessorWithSdk(descriptor, sampleRate).latencySamples;
    }
    return total;
}

unsigned int activeMasterVst3LatencySamples(const std::vector<InsertState>& inserts, int sampleRate) {
    if (sampleRate <= 0) {
        return 0;
    }
    unsigned int total = 0;
    for (const auto& insert : inserts) {
        if (insert.bypassed || !isVst3MasterInsert(insert)) {
            continue;
        }
        if (insert.reportedLatencySamples > 0) {
            total += insert.reportedLatencySamples;
            continue;
        }
        if (insert.pluginPath.empty()) {
            continue;
        }
        const auto descriptor = resolveVst3PluginDescriptorForInsert(insert.pluginName,
                                                                    insert.pluginPath,
                                                                    insert.pluginClassId,
                                                                    insert.pluginClassName);
        total += probeVst3ProcessorWithSdk(descriptor, sampleRate).latencySamples;
    }
    return total;
}

void applyActiveTrackVst3InsertsToStereoBlock(const ProjectAudioRenderPlan& plan,
                                              int64_t startFrame,
                                              int64_t frameCount,
                                              bool soloMode,
                                              std::vector<float>& interleavedStereo) {
    if (!plan.renderTrackVst3Inserts || !plan.hasActiveTrackVst3Inserts ||
        frameCount <= 0 || interleavedStereo.size() < static_cast<size_t>(frameCount) * 2) {
        return;
    }

    const int sampleRate = static_cast<int>(std::round(plan.sampleRate));
    if (sampleRate <= 0) {
        return;
    }

    const TrackState* master = findTrack(plan, "Master");
    for (const auto& track : plan.tracks) {
        if (trackPlaybackMuted(plan, track, soloMode) || !isMasterOutputBus(track.outputBus)) {
            continue;
        }
        if (track.inserts.empty() || track.trackType == "master" || track.trackType == "monitor" || track.trackType == "folder") {
            continue;
        }

        bool hasActiveInsert = false;
        for (const auto& insert : track.inserts) {
            hasActiveInsert = hasActiveInsert || trackInsertIsActiveVst3(insert);
        }
        if (!hasActiveInsert) {
            continue;
        }

        const unsigned int trackLatencySamples = plan.delayCompensationEnabled
            ? activeTrackVst3LatencySamples(track, sampleRate)
            : 0;
        std::vector<float> dryInsertInput(static_cast<size_t>(frameCount) * 2, 0.0f);
        std::vector<float> compensatedInsertInput(static_cast<size_t>(frameCount) * 2, 0.0f);
        for (int64_t frameOffset = 0; frameOffset < frameCount; ++frameOffset) {
            const int64_t timelineFrame = timelineFrameForPlaybackFrame(plan, startFrame + frameOffset);
            const int64_t compensatedTimelineFrame = timelineFrameForPlaybackFrame(
                plan,
                startFrame + frameOffset + static_cast<int64_t>(trackLatencySamples));
            float left = 0.0f;
            float right = 0.0f;
            float compensatedLeft = 0.0f;
            float compensatedRight = 0.0f;
            for (const auto& renderClip : plan.clips) {
                if (renderClip.clip.trackName != track.name) {
                    continue;
                }
                const auto [dryLeft, dryRight] = dryClipSampleAtTimelineFrame(renderClip, timelineFrame, plan.sampleRate);
                left += dryLeft;
                right += dryRight;
                const auto [compensatedDryLeft, compensatedDryRight] =
                    dryClipSampleAtTimelineFrame(renderClip, compensatedTimelineFrame, plan.sampleRate);
                compensatedLeft += compensatedDryLeft;
                compensatedRight += compensatedDryRight;
            }
            const auto bufferIndex = static_cast<size_t>(frameOffset) * 2;
            auto dryFrame = applyTrackChannelFormat({left, right}, &track, MixerRouteKind::Audio);
            auto compensatedDryFrame = applyTrackChannelFormat({compensatedLeft, compensatedRight}, &track, MixerRouteKind::Audio);
            dryInsertInput[bufferIndex] = dryFrame.left;
            dryInsertInput[bufferIndex + 1] = dryFrame.right;
            compensatedInsertInput[bufferIndex] = compensatedDryFrame.left;
            compensatedInsertInput[bufferIndex + 1] = compensatedDryFrame.right;
        }

        WavAudioData trackAudio;
        trackAudio.sampleRate = sampleRate;
        trackAudio.channels = 2;
        trackAudio.interleavedSamples = compensatedInsertInput;
        bool processedAny = false;
        for (const auto& insert : track.inserts) {
            if (!trackInsertIsActiveVst3(insert) || insert.pluginPath.empty()) {
                continue;
            }
            InsertState renderInsert;
            renderInsert.pluginName = insert.pluginName;
            renderInsert.pluginFormat = insert.pluginFormat;
            renderInsert.pluginPath = insert.pluginPath;
            renderInsert.pluginClassId = insert.pluginClassId;
            renderInsert.pluginClassName = insert.pluginClassName;
            renderInsert.bypassed = insert.bypassed;
            renderInsert.available = insert.enabled;
            renderInsert.dspExecutionMode = insert.dspExecutionMode;
            renderInsert.assignedDspServerId = insert.assignedDspServerId;
            renderInsert.serverModuleId = insert.serverModuleId;
            renderInsert.reportedLatencySamples = insert.reportedLatencySamples;
            renderInsert.dspAvailable = insert.dspAvailable;
            renderInsert.dspLastError = insert.dspLastError;
            renderInsert.parameters = insert.parameters;
            auto descriptor = resolveVst3PluginDescriptorForInsert(renderInsert.pluginName,
                                                                  renderInsert.pluginPath,
                                                                  renderInsert.pluginClassId,
                                                                  renderInsert.pluginClassName);
            auto result = processStereoBufferWithVst3(descriptor, trackAudio, 256, insert.parameters);
            if (!result.processed) {
                const auto worker = defaultVst3ProcessWorkerPath();
                if (worker.empty()) {
                    continue;
                }
                auto isolated = processStereoBufferWithIsolatedVst3(worker, descriptor, trackAudio, 256, 20);
                if (!isolated.processed) {
                    continue;
                }
            }
            processedAny = true;
        }
        if (!processedAny || trackAudio.interleavedSamples.size() < dryInsertInput.size()) {
            continue;
        }

        for (int64_t frameOffset = 0; frameOffset < frameCount; ++frameOffset) {
            const int64_t timelineFrame = timelineFrameForPlaybackFrame(plan, startFrame + frameOffset);
            const double timelineSeconds = static_cast<double>(timelineFrame) / plan.sampleRate;
            const auto bufferIndex = static_cast<size_t>(frameOffset) * 2;
            auto dryLeft = dryInsertInput[bufferIndex];
            auto dryRight = dryInsertInput[bufferIndex + 1];
            auto processedLeft = trackAudio.interleavedSamples[bufferIndex];
            auto processedRight = trackAudio.interleavedSamples[bufferIndex + 1];
            const bool trackIsMono = track.channelFormat == "mono";
            const auto dryPostFader = applyMixerGainPan({dryLeft, dryRight},
                                                        effectiveTrackVolumeDb(plan, track, timelineSeconds),
                                                        automationLaneValueAt(track, "track.pan", timelineSeconds, track.pan),
                                                        plan.panLaw, trackIsMono);
            const auto processedPostFader = applyMixerGainPan({processedLeft, processedRight},
                                                              effectiveTrackVolumeDb(plan, track, timelineSeconds),
                                                              automationLaneValueAt(track, "track.pan", timelineSeconds, track.pan),
                                                              plan.panLaw, trackIsMono);
            dryLeft = dryPostFader.left;
            dryRight = dryPostFader.right;
            processedLeft = processedPostFader.left;
            processedRight = processedPostFader.right;
            if (master != nullptr) {
                if (master->muted) {
                    dryLeft = 0.0f;
                    dryRight = 0.0f;
                    processedLeft = 0.0f;
                    processedRight = 0.0f;
                } else {
                    const auto dryMaster = applyMixerMasterGainBalance({dryLeft, dryRight},
                                                                       automationValueAt(master->volumeAutomation, timelineSeconds, master->volumeDb),
                                                                       automationLaneValueAt(*master, "track.pan", timelineSeconds, master->pan));
                    const auto processedMaster = applyMixerMasterGainBalance({processedLeft, processedRight},
                                                                             automationValueAt(master->volumeAutomation, timelineSeconds, master->volumeDb),
                                                                             automationLaneValueAt(*master, "track.pan", timelineSeconds, master->pan));
                    dryLeft = dryMaster.left;
                    dryRight = dryMaster.right;
                    processedLeft = processedMaster.left;
                    processedRight = processedMaster.right;
                }
            }
            interleavedStereo[bufferIndex] += processedLeft - dryLeft;
            interleavedStereo[bufferIndex + 1] += processedRight - dryRight;
        }
    }
}

} // namespace

double projectDurationSeconds(const ProjectDocument& project) {
    ProjectDocument effectiveProject = project;
    normalizeProjectEditModel(effectiveProject);
    rebuildProjectClipsFromActivePlaylists(effectiveProject);
    double endSeconds = 0.0;
    for (const auto& clip : effectiveProject.clips) {
        endSeconds = std::max(endSeconds, clip.startSeconds + clip.durationSeconds);
    }
    for (const auto& region : effectiveProject.midiRegions) {
        if (!region.muted) {
            endSeconds = std::max(endSeconds, region.startSeconds + region.durationSeconds);
        }
    }
    return endSeconds;
}

size_t activeVst3MasterInsertCount(const ProjectDocument& project) {
    return static_cast<size_t>(std::count_if(project.masterInserts.begin(), project.masterInserts.end(), [](const InsertState& insert) {
        return !insert.bypassed && isVst3MasterInsert(insert);
    }));
}

size_t activeVst3TrackInsertCount(const ProjectDocument& project) {
    size_t count = 0;
    for (const auto& track : project.tracks) {
        for (const auto& insert : track.inserts) {
            InsertState renderInsert;
            renderInsert.pluginName = insert.pluginName;
            renderInsert.pluginFormat = insert.pluginFormat;
            renderInsert.pluginPath = insert.pluginPath;
            renderInsert.bypassed = insert.bypassed;
            renderInsert.available = insert.enabled;
            if (insert.enabled && !insert.bypassed && isVst3MasterInsert(renderInsert)) {
                ++count;
            }
        }
    }
    return count;
}

bool hasActiveVst3MasterInserts(const ProjectDocument& project) {
    return activeVst3MasterInsertCount(project) > 0;
}

bool hasActiveVst3TrackInserts(const ProjectDocument& project) {
    return activeVst3TrackInsertCount(project) > 0;
}

namespace {

// Decoded-WAV cache. Building a render plan happens on every project edit (clip
// copy, solo/mute, gain, adding a track), and each build previously re-read AND
// re-decoded every clip's source file from disk — with several tracks sharing one
// long file, that meant hundreds of MB of disk I/O + WAV parsing per edit, which
// stalled the main thread for seconds (a spinning beachball). Cache the decoded
// audio keyed by path + size + mtime so repeat builds only pay a fast in-memory
// copy. Invalidated automatically when the file on disk changes.
struct CachedWavEntry {
    std::uintmax_t size = 0;
    int64_t mtime = 0;
    WavAudioData data;
};
std::mutex g_wavCacheMutex;
std::map<std::string, CachedWavEntry> g_wavCache;
size_t g_wavCacheBytes = 0;
constexpr size_t kWavCacheMaxBytes = static_cast<size_t>(1536) * 1024 * 1024; // ~1.5 GB ceiling

size_t wavEntryBytes(const WavAudioData& data) {
    return data.interleavedSamples.size() * sizeof(float);
}

bool statWavFile(const std::string& path, std::uintmax_t& sizeOut, int64_t& mtimeOut) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        return false;
    }
    const auto writeTime = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return false;
    }
    sizeOut = size;
    mtimeOut = static_cast<int64_t>(writeTime.time_since_epoch().count());
    return true;
}

bool readPcmWavFileCached(const std::string& path, WavAudioData& out, std::string& error) {
    if (path.empty()) {
        error = "empty source path";
        return false;
    }
    std::uintmax_t size = 0;
    int64_t mtime = 0;
    const bool statOk = statWavFile(path, size, mtime);
    if (statOk) {
        std::lock_guard<std::mutex> lock(g_wavCacheMutex);
        const auto it = g_wavCache.find(path);
        if (it != g_wavCache.end() && it->second.size == size && it->second.mtime == mtime) {
            out = it->second.data; // fast in-memory copy — no disk read, no decode
            return true;
        }
    }
    WavAudioData fresh;
    if (!readPcmWavFile(path, fresh, error)) {
        return false;
    }
    out = fresh;
    if (statOk) {
        const size_t entryBytes = wavEntryBytes(fresh);
        std::lock_guard<std::mutex> lock(g_wavCacheMutex);
        const auto existing = g_wavCache.find(path);
        if (existing != g_wavCache.end()) {
            g_wavCacheBytes -= std::min(g_wavCacheBytes, wavEntryBytes(existing->second.data));
            g_wavCache.erase(existing);
        }
        if (entryBytes <= kWavCacheMaxBytes) {
            while (!g_wavCache.empty() && g_wavCacheBytes + entryBytes > kWavCacheMaxBytes) {
                auto victim = g_wavCache.begin();
                g_wavCacheBytes -= std::min(g_wavCacheBytes, wavEntryBytes(victim->second.data));
                g_wavCache.erase(victim);
            }
            g_wavCacheBytes += entryBytes;
            g_wavCache.emplace(path, CachedWavEntry{size, mtime, std::move(fresh)});
        }
    }
    return true;
}

} // namespace

bool makeProjectAudioRenderPlan(const ProjectDocument& project, ProjectAudioRenderPlan& plan, std::string& error) {
    ProjectDocument effectiveProject = project;
    normalizeProjectEditModel(effectiveProject);
    rebuildProjectClipsFromActivePlaylists(effectiveProject);
    plan = {};
    plan.sampleRate = effectiveProject.sampleRate > 0.0 ? effectiveProject.sampleRate : 48000.0;
    plan.panLaw = effectiveProject.panLaw.empty() ? "legacy" : effectiveProject.panLaw;
    plan.tempoBpm = std::max(20.0, std::min(400.0, static_cast<double>(effectiveProject.tempoBpm)));
    plan.loopEnabled = effectiveProject.loopEnabled;
    plan.loopStartSeconds = std::max(0.0, effectiveProject.loopStartSeconds);
    plan.loopEndSeconds = std::max(0.0, effectiveProject.loopEndSeconds);
    plan.delayCompensationEnabled = effectiveProject.delayCompensationEnabled;
    if (plan.loopEndSeconds <= plan.loopStartSeconds) {
        plan.loopEnabled = false;
    }
    plan.tracks = effectiveProject.tracks;
    plan.midiRegions = effectiveProject.midiRegions;
    plan.tempoMap = effectiveProject.tempoMap;
    plan.monitorInputTrimDb = std::max(-12.0f, std::min(0.0f, effectiveProject.monitorInputTrimDb));
    plan.monitorModules = effectiveProject.monitorModules;
    plan.mixerGraph = buildMixerGraph(effectiveProject);
    unsigned int graphPathLatencySamples = 0;
    if (effectiveProject.delayCompensationEnabled) {
        graphPathLatencySamples = plan.mixerGraph.maxPathLatencySamples;
        plan.delayCompensationSamples = std::max(plan.delayCompensationSamples, graphPathLatencySamples);
        for (const auto& route : plan.mixerGraph.routes) {
            if (!route.audioCarrying || route.pathLatencySamples >= plan.mixerGraph.maxPathLatencySamples) {
                continue;
            }
            plan.routeDelayCompensationSamples[route.name] = plan.mixerGraph.maxPathLatencySamples - route.pathLatencySamples;
        }
    }
    for (const auto& insert : effectiveProject.masterInserts) {
        const bool canPrepareBypassedInsert =
            insert.available && !insert.pluginPath.empty() && std::filesystem::exists(insert.pluginPath);
        if (isVst3MasterInsert(insert) && (!insert.bypassed || canPrepareBypassedInsert)) {
            plan.hasActiveVst3Inserts = true;
            plan.activeVst3Inserts.push_back(insert);
        }
    }
    if (effectiveProject.delayCompensationEnabled) {
        const int sampleRate = static_cast<int>(std::round(plan.sampleRate));
        plan.delayCompensationSamples = std::max(
            plan.delayCompensationSamples,
            graphPathLatencySamples + activeMasterVst3LatencySamples(plan.activeVst3Inserts, sampleRate));
    }
    for (const auto& track : effectiveProject.tracks) {
        for (const auto& insert : track.inserts) {
            InsertState renderInsert;
            renderInsert.pluginName = insert.pluginName;
            renderInsert.pluginFormat = insert.pluginFormat;
            renderInsert.pluginPath = insert.pluginPath;
            renderInsert.bypassed = insert.bypassed;
            renderInsert.available = insert.enabled;
            renderInsert.dspExecutionMode = insert.dspExecutionMode;
            renderInsert.assignedDspServerId = insert.assignedDspServerId;
            renderInsert.serverModuleId = insert.serverModuleId;
            renderInsert.reportedLatencySamples = insert.reportedLatencySamples;
            renderInsert.dspAvailable = insert.dspAvailable;
            renderInsert.dspLastError = insert.dspLastError;
            renderInsert.parameters = insert.parameters;
            if (insert.enabled && !insert.bypassed && isVst3MasterInsert(renderInsert)) {
                plan.hasActiveTrackVst3Inserts = true;
                plan.activeTrackVst3InsertLabels.push_back(track.name + ": " + insert.pluginName);
                if (effectiveProject.delayCompensationEnabled && insert.reportedLatencySamples > 0) {
                    plan.delayCompensationSamples = std::max(plan.delayCompensationSamples, insert.reportedLatencySamples);
                }
            }
        }
    }
    for (const auto& clip : effectiveProject.clips) {
        ProjectRenderClip renderClip;
        renderClip.clip = clip;
        if (!readPcmWavFileCached(clip.sourcePath, renderClip.source, error)) {
            renderClip.source = {};
            renderClip.source.channels = 2;
            renderClip.source.sampleRate = static_cast<int>(std::round(plan.sampleRate));
            renderClip.missingSource = true;
            plan.hasMissingMedia = true;
            plan.missingMediaClipIds.push_back(clip.id.empty() ? clip.sourcePath : clip.id);
            error.clear();
        }
        plan.clips.push_back(std::move(renderClip));
    }
    error.clear();
    return true;
}

std::vector<ProjectRenderMidiEvent> collectMidiEventsForRenderBlock(const ProjectAudioRenderPlan& plan,
                                                                    const std::string& trackName,
                                                                    int64_t startFrame,
                                                                    int64_t frameCount) {
    std::vector<ProjectRenderMidiEvent> events;
    if (trackName.empty() || frameCount <= 0 || plan.sampleRate <= 0.0) {
        return events;
    }
    const int64_t endFrame = startFrame + frameCount;
    const double fallbackBpm = std::max(20.0, std::min(400.0, plan.tempoBpm));
    const auto tempoMarkers = sortedValidTempoMap(plan);
    for (const auto& region : plan.midiRegions) {
        if (region.trackName != trackName || region.muted || region.durationSeconds <= 0.0) {
            continue;
        }
        const int64_t regionEndFrame = static_cast<int64_t>(std::round((region.startSeconds + region.durationSeconds) * plan.sampleRate));
        const auto eventFrameForBeat = [&](double beat) {
            const double eventSeconds = secondsForBeatOffsetFromTempoMap(tempoMarkers, region.startSeconds, beat, fallbackBpm);
            return static_cast<int64_t>(std::round(eventSeconds * plan.sampleRate));
        };
        for (const auto& note : region.notes) {
            if (note.muted || note.pitch < 0 || note.pitch > 127 || note.durationBeats <= 0.0) {
                continue;
            }
            const int64_t noteOnFrame = eventFrameForBeat(note.startBeats);
            const int64_t noteOffFrame = std::max<int64_t>(noteOnFrame + 1, eventFrameForBeat(note.startBeats + note.durationBeats));
            const int64_t clippedNoteOffFrame = std::min(noteOffFrame, regionEndFrame);
            if (noteOnFrame >= startFrame && noteOnFrame < endFrame && noteOnFrame < regionEndFrame) {
                events.push_back({
                    region.trackName,
                    region.id,
                    note.id,
                    noteOnFrame - startFrame,
                    std::max(0, std::min(127, note.pitch)),
                    std::max(1, std::min(127, note.velocity)),
                    std::max(1, std::min(16, note.channel)),
                    true,
                    ProjectRenderMidiEventKind::Note
                });
            }
            if (clippedNoteOffFrame >= startFrame && clippedNoteOffFrame < endFrame && clippedNoteOffFrame > noteOnFrame) {
                events.push_back({
                    region.trackName,
                    region.id,
                    note.id,
                    clippedNoteOffFrame - startFrame,
                    std::max(0, std::min(127, note.pitch)),
                    0,
                    std::max(1, std::min(16, note.channel)),
                    false,
                    ProjectRenderMidiEventKind::Note
                });
            }
        }
        for (const auto& event : region.controllerEvents) {
            const int64_t eventFrame = eventFrameForBeat(event.beat);
            if (eventFrame >= startFrame && eventFrame < endFrame && eventFrame < regionEndFrame) {
                ProjectRenderMidiEvent renderEvent;
                renderEvent.trackName = region.trackName;
                renderEvent.regionId = region.id;
                renderEvent.noteId = event.id;
                renderEvent.frameOffset = eventFrame - startFrame;
                renderEvent.channel = std::max(1, std::min(16, event.channel));
                renderEvent.kind = ProjectRenderMidiEventKind::Controller;
                renderEvent.controller = std::max(0, std::min(127, event.controller));
                renderEvent.value = std::max(0, std::min(127, event.value));
                events.push_back(renderEvent);
            }
        }
        for (const auto& event : region.pitchBendEvents) {
            const int64_t eventFrame = eventFrameForBeat(event.beat);
            if (eventFrame >= startFrame && eventFrame < endFrame && eventFrame < regionEndFrame) {
                ProjectRenderMidiEvent renderEvent;
                renderEvent.trackName = region.trackName;
                renderEvent.regionId = region.id;
                renderEvent.noteId = event.id;
                renderEvent.frameOffset = eventFrame - startFrame;
                renderEvent.channel = std::max(1, std::min(16, event.channel));
                renderEvent.kind = ProjectRenderMidiEventKind::PitchBend;
                renderEvent.value = std::max(0, std::min(16383, event.value));
                events.push_back(renderEvent);
            }
        }
        for (const auto& event : region.programChangeEvents) {
            const int64_t eventFrame = eventFrameForBeat(event.beat);
            if (eventFrame >= startFrame && eventFrame < endFrame && eventFrame < regionEndFrame) {
                ProjectRenderMidiEvent renderEvent;
                renderEvent.trackName = region.trackName;
                renderEvent.regionId = region.id;
                renderEvent.noteId = event.id;
                renderEvent.frameOffset = eventFrame - startFrame;
                renderEvent.channel = std::max(1, std::min(16, event.channel));
                renderEvent.kind = ProjectRenderMidiEventKind::ProgramChange;
                renderEvent.program = std::max(0, std::min(127, event.program));
                events.push_back(renderEvent);
            }
        }
    }
    std::sort(events.begin(), events.end(), [](const ProjectRenderMidiEvent& left, const ProjectRenderMidiEvent& right) {
        if (left.frameOffset != right.frameOffset) {
            return left.frameOffset < right.frameOffset;
        }
        if (left.kind != right.kind) {
            return static_cast<int>(left.kind) < static_cast<int>(right.kind);
        }
        if (left.noteOn != right.noteOn) {
            return !left.noteOn && right.noteOn;
        }
        if (left.pitch != right.pitch) {
            return left.pitch < right.pitch;
        }
        return left.noteId < right.noteId;
    });
    return events;
}

std::vector<ProjectRenderMidiEvent> applyMidiTrackInsertEffects(const TrackState& sourceTrack,
                                                               std::vector<ProjectRenderMidiEvent> events,
                                                               int64_t frameCount,
                                                               double sampleRate) {
    if (events.empty() || sourceTrack.inserts.empty() || frameCount <= 0 || sampleRate <= 0.0) {
        return events;
    }
    for (const auto& insert : sourceTrack.inserts) {
        if (!insert.enabled || insert.bypassed || insert.pluginFormat != "MIDI") {
            continue;
        }
        std::vector<ProjectRenderMidiEvent> generated;
        const std::string name = insert.pluginName;
        if (name == "MIDI Transpose +12") {
            for (auto& event : events) {
                if (event.kind == ProjectRenderMidiEventKind::Note) {
                    event.pitch = std::max(0, std::min(127, event.pitch + 12));
                }
            }
        } else if (name == "MIDI Transpose -12") {
            for (auto& event : events) {
                if (event.kind == ProjectRenderMidiEventKind::Note) {
                    event.pitch = std::max(0, std::min(127, event.pitch - 12));
                }
            }
        } else if (name == "MIDI Velocity +10") {
            for (auto& event : events) {
                if (event.kind == ProjectRenderMidiEventKind::Note && event.noteOn) {
                    event.velocity = std::max(1, std::min(127, event.velocity + 10));
                }
            }
        } else if (name == "MIDI Velocity -10") {
            for (auto& event : events) {
                if (event.kind == ProjectRenderMidiEventKind::Note && event.noteOn) {
                    event.velocity = std::max(1, std::min(127, event.velocity - 10));
                }
            }
        } else if (name == "MIDI Delay 1/8" || name == "MIDI Echo") {
            const int64_t delayFrames = std::max<int64_t>(1, static_cast<int64_t>(std::round(sampleRate * 0.25)));
            for (const auto& event : events) {
                if (event.kind != ProjectRenderMidiEventKind::Note) {
                    continue;
                }
                auto delayed = event;
                delayed.frameOffset += delayFrames;
                delayed.velocity = event.noteOn ? std::max(1, event.velocity * 2 / 3) : 0;
                delayed.noteId += ":delay";
                if (delayed.frameOffset >= 0 && delayed.frameOffset < frameCount) {
                    generated.push_back(delayed);
                }
                if (name == "MIDI Echo") {
                    auto echo = delayed;
                    echo.frameOffset += delayFrames;
                    echo.velocity = event.noteOn ? std::max(1, event.velocity / 2) : 0;
                    echo.noteId += ":2";
                    if (echo.frameOffset >= 0 && echo.frameOffset < frameCount) {
                        generated.push_back(echo);
                    }
                }
            }
        } else if (name == "MIDI Chorder") {
            for (const auto& event : events) {
                if (event.kind != ProjectRenderMidiEventKind::Note) {
                    continue;
                }
                for (int interval : {4, 7}) {
                    auto harmony = event;
                    harmony.pitch = std::max(0, std::min(127, event.pitch + interval));
                    harmony.noteId += ":chord" + std::to_string(interval);
                    generated.push_back(harmony);
                }
            }
        } else if (name == "MIDI Humanize") {
            for (auto& event : events) {
                if (event.kind != ProjectRenderMidiEventKind::Note || !event.noteOn) {
                    continue;
                }
                const int seed = static_cast<int>((event.pitch * 31 + event.velocity * 17 + event.channel * 13 + event.frameOffset) % 9) - 4;
                event.velocity = std::max(1, std::min(127, event.velocity + seed));
                event.frameOffset = std::max<int64_t>(0, std::min<int64_t>(frameCount - 1, event.frameOffset + seed * 8));
            }
        }
        if (!generated.empty()) {
            events.insert(events.end(), generated.begin(), generated.end());
            std::sort(events.begin(), events.end(), [](const ProjectRenderMidiEvent& left, const ProjectRenderMidiEvent& right) {
                return left.frameOffset < right.frameOffset;
            });
        }
    }
    return events;
}

std::map<std::string, std::vector<float>> renderInstrumentAudioBlocksForRenderBlock(const ProjectAudioRenderPlan& plan,
                                                                                   int64_t startFrame,
                                                                                   int64_t frameCount,
                                                                                   bool soloMode,
                                                                                   ProjectAudioRenderState* state) {
    std::map<std::string, std::vector<float>> blocks;
    if (frameCount <= 0 || plan.sampleRate <= 0.0) {
        return blocks;
    }
    for (const auto& track : plan.tracks) {
        if (track.trackType != "instrument" || trackPlaybackMuted(plan, track, soloMode)) {
            continue;
        }
        std::vector<InstrumentSlotState> instrumentSlots = track.instrumentSlots;
        if (instrumentSlots.empty()) {
            instrumentSlots.push_back(track.instrument);
        }
        if (instrumentSlots.size() > 8) {
            instrumentSlots.resize(8);
        }
        const bool timelinePlaybackSuppressed = trackInputMonitorOverridesTimelinePlayback(plan, track);
        auto events = timelinePlaybackSuppressed
            ? std::vector<ProjectRenderMidiEvent> {}
            : collectMidiEventsForRenderBlock(plan, track.name, startFrame, frameCount);
        if (!timelinePlaybackSuppressed) {
            for (const auto& sourceTrack : plan.tracks) {
                if (trackPlaybackMuted(plan, sourceTrack, soloMode) || !midiTrackRoutesToInstrument(sourceTrack, track)) {
                    continue;
                }
                auto routedEvents = collectMidiEventsForRenderBlock(plan, sourceTrack.name, startFrame, frameCount);
                routedEvents = applyMidiTrackInsertEffects(sourceTrack, std::move(routedEvents), frameCount, plan.sampleRate);
                events.insert(events.end(), routedEvents.begin(), routedEvents.end());
            }
        }
        events = applyMidiTrackInsertEffects(track, std::move(events), frameCount, plan.sampleRate);
        std::vector<Vst3MidiEvent> trackVstEvents;
        trackVstEvents.reserve(events.size());
        for (const auto& event : events) {
            Vst3MidiEvent vstEvent;
            vstEvent.frameOffset = static_cast<int>(std::max<int64_t>(0, std::min<int64_t>(frameCount - 1, event.frameOffset)));
            vstEvent.pitch = event.pitch;
            vstEvent.velocity = event.velocity;
            vstEvent.channel = event.channel;
            vstEvent.noteOn = event.noteOn;
            switch (event.kind) {
            case ProjectRenderMidiEventKind::Controller:
                vstEvent.kind = Vst3MidiEventKind::Controller;
                vstEvent.controller = event.controller;
                vstEvent.value = event.value;
                break;
            case ProjectRenderMidiEventKind::PitchBend:
                vstEvent.kind = Vst3MidiEventKind::PitchBend;
                vstEvent.value = event.value;
                break;
            case ProjectRenderMidiEventKind::ProgramChange:
                vstEvent.kind = Vst3MidiEventKind::ProgramChange;
                vstEvent.program = event.program;
                break;
            case ProjectRenderMidiEventKind::Note:
            default:
                vstEvent.kind = Vst3MidiEventKind::Note;
                break;
            }
            trackVstEvents.push_back(vstEvent);
        }
        if (state != nullptr) {
            auto liveIt = state->liveMidiEvents.find(track.name);
            if (liveIt != state->liveMidiEvents.end()) {
                for (auto liveEvent : liveIt->second) {
                    liveEvent.frameOffset = std::max(0, std::min<int>(static_cast<int>(std::max<int64_t>(1, frameCount)) - 1, liveEvent.frameOffset));
                    trackVstEvents.push_back(liveEvent);
                }
                state->liveMidiEvents.erase(liveIt);
            }
            for (const auto& sourceTrack : plan.tracks) {
                if (!midiTrackRoutesToInstrument(sourceTrack, track)) {
                    continue;
                }
                auto routedLiveIt = state->liveMidiEvents.find(sourceTrack.name);
                if (routedLiveIt == state->liveMidiEvents.end()) {
                    continue;
                }
                for (auto liveEvent : routedLiveIt->second) {
                    liveEvent.frameOffset = std::max(0, std::min<int>(static_cast<int>(std::max<int64_t>(1, frameCount)) - 1, liveEvent.frameOffset));
                    trackVstEvents.push_back(liveEvent);
                }
                state->liveMidiEvents.erase(routedLiveIt);
            }
        }
        std::vector<float> summedOutput(static_cast<size_t>(frameCount) * 2, 0.0f);
        bool renderedAnySlot = false;
        // Per-layer solo: when any layer in this rack is soloed, only soloed layers sound.
        const bool anyLayerSoloed = std::any_of(instrumentSlots.begin(), instrumentSlots.end(),
                                                [](const InstrumentSlotState& slot) { return slot.soloed; });
        for (size_t slotIndex = 0; slotIndex < instrumentSlots.size(); ++slotIndex) {
            const auto& instrument = instrumentSlots[slotIndex];
            if (instrument.pluginPath.empty() ||
                !instrument.enabled ||
                instrument.bypassed ||
                (anyLayerSoloed && !instrument.soloed) ||
                instrument.pluginName.empty() ||
                instrument.pluginName == "No Instrument") {
                continue;
            }
            std::vector<Vst3MidiEvent> slotEvents;
            slotEvents.reserve(trackVstEvents.size());
            for (const auto& event : trackVstEvents) {
                if (instrument.midiChannel == 0 || event.channel == instrument.midiChannel) {
                    slotEvents.push_back(event);
                }
            }
            WavAudioData outputAudio;
            outputAudio.channels = 2;
            outputAudio.sampleRate = static_cast<int>(std::round(plan.sampleRate));
            outputAudio.interleavedSamples.assign(static_cast<size_t>(frameCount) * 2, 0.0f);
            const auto descriptor = resolveVst3PluginDescriptorForInsert(instrument.pluginName,
                                                                        instrument.pluginPath,
                                                                        instrument.pluginClassId,
                                                                        instrument.pluginClassName);
            const std::string processorKey = track.name + "#I" + std::to_string(slotIndex + 1);
            if (state != nullptr) {
                // Key on plug-in identity + sample rate only, NOT the per-block frameCount. On
                // this machine SoundGrid delivers callbacks in bursts, so frameCount varies
                // block to block; keying the cache on it re-prepared the plug-in every block,
                // resetting the synth's voices each time — an audible "직직" crackle. Keep one
                // processor prepared for the largest block seen and just process fewer frames
                // (a VST3 processor accepts any block <= its prepared maximum), growing the
                // prepared size only when a bigger block actually arrives — the same approach
                // the track-insert chain uses.
                const int requestedBlock = static_cast<int>(std::max<int64_t>(1, frameCount));
                const std::string cacheKey = instrument.pluginName + "\n" +
                    instrument.pluginPath + "\n" +
                    std::to_string(static_cast<int>(std::round(plan.sampleRate)));
                auto keyIt = state->instrumentProcessorKeys.find(processorKey);
                auto maxIt = state->instrumentProcessorMaxBlock.find(processorKey);
                const int preparedMax = maxIt != state->instrumentProcessorMaxBlock.end() ? maxIt->second : 0;
                const bool identityChanged = keyIt == state->instrumentProcessorKeys.end() || keyIt->second != cacheKey;
                if (identityChanged || requestedBlock > preparedMax) {
                    state->instrumentProcessors.erase(processorKey);
                    state->instrumentProcessorKeys[processorKey] = cacheKey;
                }
                auto processorIt = state->instrumentProcessors.find(processorKey);
                if (processorIt == state->instrumentProcessors.end()) {
                    auto [inserted, _] = state->instrumentProcessors.emplace(processorKey, Vst3RealtimeProcessor());
                    processorIt = inserted;
                    const int prepareBlock = std::max(requestedBlock, preparedMax);
                    std::string prepareMessage;
                    if (!processorIt->second.prepare(descriptor, plan.sampleRate, prepareBlock, prepareMessage)) {
                        state->instrumentLastErrors[processorKey] = prepareMessage;
                        state->instrumentProcessors.erase(processorKey);
                        continue;
                    }
                    state->instrumentProcessorMaxBlock[processorKey] = prepareBlock;
                }
                std::string processMessage;
                auto result = processorIt->second.processMidiInstrument(outputAudio.interleavedSamples.data(),
                                                                        static_cast<int>(frameCount),
                                                                        slotEvents,
                                                                        instrument.parameters,
                                                                        processMessage);
                if (!result.processed || outputAudio.interleavedSamples.size() != static_cast<size_t>(frameCount) * 2) {
                    state->instrumentLastErrors[processorKey] = processMessage.empty() ? result.message : processMessage;
                    continue;
                }
                state->instrumentLastErrors.erase(processorKey);
            } else {
                const auto result = processMidiInstrumentWithVst3(descriptor,
                                                                  slotEvents,
                                                                  outputAudio,
                                                                  256,
                                                                  instrument.parameters);
                if (!result.processed || outputAudio.interleavedSamples.size() != static_cast<size_t>(frameCount) * 2) {
                    continue;
                }
            }
            for (size_t sampleIndex = 0; sampleIndex < summedOutput.size(); ++sampleIndex) {
                summedOutput[sampleIndex] += outputAudio.interleavedSamples[sampleIndex];
            }
            renderedAnySlot = true;
        }
        if (renderedAnySlot) {
            blocks[track.name] = std::move(summedOutput);
        }
    }
    return blocks;
}

void renderProjectAudioBlock(const ProjectAudioRenderPlan& plan,
                             int64_t startFrame,
                             int64_t frameCount,
                             std::vector<float>& interleavedStereo) {
    renderProjectAudioBlockWithMeters(plan, startFrame, frameCount, interleavedStereo, nullptr);
}

void renderProjectAudioBlockWithMeters(const ProjectAudioRenderPlan& plan,
                                       int64_t startFrame,
                                       int64_t frameCount,
                                       std::vector<float>& interleavedStereo,
                                       ProjectAudioBlockMeters* meters) {
    ProjectAudioRenderState state;
    renderProjectAudioBlockWithStateAndMeters(plan, state, startFrame, frameCount, interleavedStereo, meters);
}

void ProjectAudioRenderState::reset() {
    routeDelayLines.clear();
    masterInsertChain.reset();
    instrumentProcessors.clear();
    instrumentProcessorKeys.clear();
    instrumentLastErrors.clear();
    routeInsertChains.clear();
    routeInsertChainKeys.clear();
    routeInsertChainMaxBlock.clear();
    routeInsertLastErrors.clear();
    routeInsertWasWet.clear();
    routeInsertDeclickHold.clear();
    routeInsertDeclickRemaining.clear();
    routeInsertDeclickTotal.clear();
    routeInsertEngagementChangedThisBlock = false;
    sourceGeneratorPhases.clear();
    liveMidiEvents.clear();
    masterInsertDryFallback.clear();
    masterInsertChainMaxBlockSize = 0;
    monitorDspConfigured = false;
    masterInsertChainPrepared = false;
    masterInsertProcessingFailed = false;
    masterInsertLastError.clear();
}

void ProjectAudioRenderState::resetForSeek() {
    routeDelayLines.clear();
    sourceGeneratorPhases.clear();
    liveMidiEvents.clear();
    masterInsertProcessingFailed = false;
    masterInsertLastError.clear();
    instrumentLastErrors.clear();
    routeInsertLastErrors.clear();
    // A warm-up render (done while stopped) may have flipped a route dry↔wet; don't let that arm
    // the output crossfade on the first real playback block.
    routeInsertEngagementChangedThisBlock = false;
}

void renderProjectAudioBlockWithStateAndMeters(const ProjectAudioRenderPlan& plan,
                                               ProjectAudioRenderState& state,
                                               int64_t startFrame,
                                               int64_t frameCount,
                                               std::vector<float>& interleavedStereo,
                                               ProjectAudioBlockMeters* meters,
                                               bool offline,
                                               bool transportRunning) {
    if (frameCount <= 0) {
        interleavedStereo.clear();
        if (meters != nullptr) {
            *meters = {};
        }
        return;
    }
    interleavedStereo.assign(static_cast<size_t>(frameCount) * 2, 0.0f);
    if (plan.renderMonitorDsp && !state.monitorDspConfigured) {
        state.monitorDspProcessor.configure(plan.sampleRate, plan.monitorModules);
        state.monitorDspConfigured = true;
    }
    state.masterInsertProcessingFailed = false;
    state.masterInsertLastError.clear();
    if (plan.hasActiveVst3Inserts &&
        (!state.masterInsertChainPrepared || frameCount > state.masterInsertChainMaxBlockSize)) {
        state.masterInsertChain.reset();
        const int maxBlockSize = static_cast<int>(std::max<int64_t>(1, frameCount));
        if (!state.masterInsertChain.prepare(plan, plan.sampleRate, maxBlockSize, state.masterInsertLastError)) {
            state.masterInsertProcessingFailed = true;
            interleavedStereo.assign(static_cast<size_t>(frameCount) * 2, 0.0f);
            if (meters != nullptr) {
                *meters = {};
            }
            return;
        }
        state.masterInsertChainPrepared = true;
        state.masterInsertChainMaxBlockSize = maxBlockSize;
    }
    std::map<std::string, size_t> meterIndexByTrack;
    if (meters != nullptr) {
        meters->trackNames.clear();
        meters->trackPeakLeft.clear();
        meters->trackPeakRight.clear();
        meters->trackInsertMeterTrackNames.clear();
        meters->trackInsertMeterSlotIndices.clear();
        meters->trackInsertInputPeak.clear();
        meters->trackInsertOutputPeak.clear();
        for (const auto& track : plan.tracks) {
            if (track.trackType == "master" || track.trackType == "monitor" || track.name == "Master" || track.name == "Monitor") {
                continue;
            }
            meterIndexByTrack[track.name] = meters->trackNames.size();
            meters->trackNames.push_back(track.name);
            meters->trackPeakLeft.push_back(0.0f);
            meters->trackPeakRight.push_back(0.0f);
        }
    }
    const bool soloMode = hasSoloedAudioTrack(plan);
    const auto instrumentAudioBlocks = renderInstrumentAudioBlocksForRenderBlock(plan, startFrame, frameCount, soloMode, &state);
    MixerGraph fallbackGraph;
    const MixerGraph* graph = &plan.mixerGraph;
    if (graph->routes.empty()) {
        ProjectDocument fallbackProject;
        fallbackProject.tracks = plan.tracks;
        fallbackGraph = buildMixerGraph(fallbackProject);
        graph = &fallbackGraph;
    }
    std::map<std::string, std::vector<float>> busBlocks;
    const auto blockSampleCount = static_cast<size_t>(frameCount) * 2u;
    for (const auto& routeName : graph->renderOrder) {
        const auto route = std::find_if(graph->routes.begin(), graph->routes.end(), [&](const MixerRouteNode& candidate) {
            return candidate.name == routeName;
        });
        if (route == graph->routes.end() || !route->audioCarrying) {
            continue;
        }
        const TrackState* track = findTrack(plan, route->name);
        const bool trackMuted = track != nullptr && trackPlaybackMuted(plan, *track, soloMode);
        std::vector<InsertState> routeInserts;
        if (plan.renderTrackVst3Inserts && track != nullptr && routeMayProcessInserts(route->kind)) {
            routeInserts = activeLocalRouteInserts(*track);
        }
        // A muted track still runs its inserts (so an open plug-in editor keeps
        // metering the real signal), but it contributes silence to the mix. Skip
        // it outright only when there is nothing to keep processing.
        if (trackMuted && routeInserts.empty()) {
            continue;
        }
        const bool timelinePlaybackSuppressed = track != nullptr && trackInputMonitorOverridesTimelinePlayback(plan, *track);
        const bool hasLiveInstrumentBlock = route->kind == MixerRouteKind::Instrument &&
            instrumentAudioBlocks.find(route->name) != instrumentAudioBlocks.end();
        if (timelinePlaybackSuppressed && !hasLiveInstrumentBlock && routeInserts.empty()) {
            continue;
        }

        std::vector<float> routeInput(blockSampleCount, 0.0f);
        // When the transport is stopped, timeline clips must contribute silence — otherwise a
        // still-active insert render (always-on tail, or an instrument track keeping the render
        // alive) re-samples the same parked clip window every block and it loops audibly, so
        // "stop" never goes quiet. Inserts then ring out / meter on silence, which is correct.
        if (!timelinePlaybackSuppressed && transportRunning &&
            (route->kind == MixerRouteKind::Audio || route->kind == MixerRouteKind::Instrument)) {
            for (int64_t frameOffset = 0; frameOffset < frameCount; ++frameOffset) {
                const int64_t timelineFrame = timelineFrameForPlaybackFrame(plan, startFrame + frameOffset);
                float left = 0.0f;
                float right = 0.0f;
                for (const auto& renderClip : plan.clips) {
                    if (renderClip.clip.trackName != route->name) {
                        continue;
                    }
                    const auto [clipLeft, clipRight] = dryClipSampleAtTimelineFrame(renderClip, timelineFrame, plan.sampleRate);
                    left += clipLeft;
                    right += clipRight;
                }
                const auto index = static_cast<size_t>(frameOffset) * 2u;
                routeInput[index] += left;
                routeInput[index + 1u] += right;
            }
        }
        if (route->kind == MixerRouteKind::Instrument) {
            const auto instrumentIt = instrumentAudioBlocks.find(route->name);
            if (instrumentIt != instrumentAudioBlocks.end()) {
                for (size_t index = 0; index < std::min(routeInput.size(), instrumentIt->second.size()); ++index) {
                    routeInput[index] += instrumentIt->second[index];
                }
            }
        }
        const std::string receiveBus = !route->inputBus.empty() ? route->inputBus : route->name;
        if (const auto bus = busBlocks.find(receiveBus); bus != busBlocks.end()) {
            for (size_t index = 0; index < std::min(routeInput.size(), bus->second.size()); ++index) {
                routeInput[index] += bus->second[index];
            }
        }
        applyTrackChannelFormatToBlock(routeInput, track, route->kind);

        bool routeProcessedWet = false;   // did an insert actually run on this block (vs dry)?
        if (!routeInserts.empty()) {
            const auto dryRouteInput = routeInput;
            const float insertInputPeak = blockPeakAbs(dryRouteInput);
            std::string insertError;
            const std::vector<RealtimeMasterInsertChain::InsertProcessMeter>* processMeters = nullptr;
            const auto routeInsertShmKeys = activeLocalRouteInsertShmKeys(*track);
            if (!processRouteInsertBlock(state,
                                         route->name,
                                         routeInserts,
                                         plan.sampleRate,
                                         frameCount,
                                         routeInput,
                                         insertError,
                                         &processMeters,
                                         routeInsertShmKeys,
                                         offline,           // offline bounce prepares synchronously
                                         &routeProcessedWet)) {
                routeInput = dryRouteInput;
                state.routeInsertLastErrors[route->name] = insertError;
            }
            if (insertInputPeak <= 0.000001f &&
                blockPeakAbs(routeInput) <= 0.000001f &&
                synthesizeSourceGeneratorFallback(state,
                                                  route->name,
                                                  routeInserts,
                                                  plan.sampleRate,
                                                  frameCount,
                                                  routeInput)) {
                processMeters = nullptr;
                state.routeInsertLastErrors.erase(route->name);
            }
            if (meters != nullptr) {
                const float insertOutputPeak = blockPeakAbs(routeInput);
                int slotIndex = 0;
                size_t processMeterIndex = 0;
                for (const auto& insert : track->inserts) {
                    if (trackInsertShouldRunInLocalRouteGraph(insert)) {
                        float meterInputPeak = insertInputPeak;
                        float meterOutputPeak = insertOutputPeak;
                        if (processMeters != nullptr && processMeterIndex < processMeters->size()) {
                            meterInputPeak = (*processMeters)[processMeterIndex].inputPeak;
                            meterOutputPeak = (*processMeters)[processMeterIndex].outputPeak;
                        }
                        meters->trackInsertMeterTrackNames.push_back(track->name);
                        meters->trackInsertMeterSlotIndices.push_back(slotIndex);
                        meters->trackInsertInputPeak.push_back(meterInputPeak);
                        meters->trackInsertOutputPeak.push_back(meterOutputPeak);
                        ++processMeterIndex;
                    }
                    ++slotIndex;
                }
            }
        }

        // Declick add / remove / reorder. Whenever a route flips between dry and wet — a chain
        // becomes ready (async), is retired, is replaced, or the last insert is removed (which
        // skips the block above entirely) — crossfade over ~15 ms from the last output sample we
        // emitted into the current output. The held sample is continuous with whatever was playing
        // a block ago, so the seam never clicks ("지직"). This runs for every route, inserts or
        // not, so removing the last insert is covered too.
        {
            const int wetNow = routeProcessedWet ? 1 : 0;
            auto wasIt = state.routeInsertWasWet.find(route->name);
            const int wasWet = (wasIt == state.routeInsertWasWet.end()) ? -1 : wasIt->second;
            auto& hold = state.routeInsertDeclickHold[route->name];
            if (wasWet != -1 && wasWet != wetNow) {
                const int fade = std::max(1, static_cast<int>(plan.sampleRate * 0.015));
                state.routeInsertDeclickTotal[route->name] = fade;
                state.routeInsertDeclickRemaining[route->name] = fade;
                // Tell the engine to re-arm its 80 ms full-waveform output crossfade at THIS block —
                // the real dry→wet swap instant — so the click is masked after PDC/monitor DSP.
                state.routeInsertEngagementChangedThisBlock = true;
            }
            state.routeInsertWasWet[route->name] = wetNow;
            int& remaining = state.routeInsertDeclickRemaining[route->name];
            if (remaining > 0 && routeInput.size() >= static_cast<size_t>(frameCount) * 2u) {
                const int total = std::max(1, state.routeInsertDeclickTotal[route->name]);
                for (int64_t f = 0; f < frameCount && remaining > 0; ++f) {
                    const double prog = std::min(1.0, 1.0 - static_cast<double>(remaining) / total);
                    const float a = static_cast<float>(0.5 - 0.5 * std::cos(M_PI * prog));  // 0→1
                    const auto idx = static_cast<size_t>(f) * 2u;
                    routeInput[idx] = hold.first * (1.0f - a) + routeInput[idx] * a;
                    routeInput[idx + 1u] = hold.second * (1.0f - a) + routeInput[idx + 1u] * a;
                    --remaining;
                }
            }
            // Remember the last emitted sample for the next transition's crossfade seam.
            if (frameCount > 0 && routeInput.size() >= static_cast<size_t>(frameCount) * 2u) {
                const auto last = static_cast<size_t>(frameCount - 1) * 2u;
                hold.first = routeInput[last];
                hold.second = routeInput[last + 1u];
            }
        }

        if (trackMuted) {
            // The inserts have already run and fed any open plug-in editors; the
            // muted track now contributes silence to the downstream mix.
            std::fill(routeInput.begin(), routeInput.end(), 0.0f);
        }

        for (int64_t frameOffset = 0; frameOffset < frameCount; ++frameOffset) {
            const int64_t timelineFrame = timelineFrameForPlaybackFrame(plan, startFrame + frameOffset);
            const double timelineSeconds = static_cast<double>(timelineFrame) / plan.sampleRate;
            const auto index = static_cast<size_t>(frameOffset) * 2u;
            MixerRouteProcessorInput processorInput;
            processorInput.input = {routeInput[index], routeInput[index + 1u]};
            processorInput.routeKind = route->kind;
            processorInput.applyRouteFaderPan = track != nullptr &&
                route->kind != MixerRouteKind::Master &&
                route->kind != MixerRouteKind::Monitor;
            if (track != nullptr) {
                processorInput.gainDb = effectiveTrackVolumeDb(plan, *track, timelineSeconds);
                processorInput.pan = automationLaneValueAt(*track, "track.pan", timelineSeconds, track->pan);
                processorInput.panLaw = plan.panLaw;
                processorInput.isMonoTrack = track->channelFormat == "mono";
                processorInput.sends = track->sends;
            }
            const auto processedRoute = processMixerRouteProcessors(processorInput);
            auto postProcessorFrame = processedRoute.postFader;
            if (route->kind == MixerRouteKind::Monitor &&
                plan.renderMonitorDsp &&
                state.monitorDspConfigured) {
                const float monitorInputTrimGain = dbToGain(std::max(-12.0f, std::min(0.0f, plan.monitorInputTrimDb)));
                postProcessorFrame.left *= monitorInputTrimGain;
                postProcessorFrame.right *= monitorInputTrimGain;
                StereoFrame monitorFrame;
                monitorFrame.left = postProcessorFrame.left;
                monitorFrame.right = postProcessorFrame.right;
                const StereoFrame processedMonitorFrame = state.monitorDspProcessor.process(monitorFrame);
                postProcessorFrame.left = processedMonitorFrame.left;
                postProcessorFrame.right = processedMonitorFrame.right;
            }
            const unsigned int routeDelaySamples = routeDelayCompensationSamples(plan, route->name);
            const auto delayedRouteFrame = applyRouteDelayCompensation(state.routeDelayLines,
                                                                      "route:" + route->name,
                                                                      postProcessorFrame,
                                                                      routeDelaySamples);
            for (const auto& sendTap : processedRoute.sendTaps) {
                const auto delayedSendFrame = applyRouteDelayCompensation(state.routeDelayLines,
                                                                         "send:" + route->name + ":" + sendTap.busName,
                                                                         sendTap.frame,
                                                                         routeDelaySamples);
                auto& sum = busBlocks[sendTap.busName];
                if (sum.size() < blockSampleCount) {
                    sum.resize(blockSampleCount, 0.0f);
                }
                sum[index] += delayedSendFrame.left;
                sum[index + 1u] += delayedSendFrame.right;
            }

            if (meters != nullptr && track != nullptr &&
                route->kind != MixerRouteKind::Master &&
                route->kind != MixerRouteKind::Monitor) {
                const auto indexIt = meterIndexByTrack.find(track->name);
                if (indexIt != meterIndexByTrack.end()) {
                    const size_t meterIndex = indexIt->second;
                    meters->trackPeakLeft[meterIndex] = std::max(meters->trackPeakLeft[meterIndex], std::abs(delayedRouteFrame.left));
                    meters->trackPeakRight[meterIndex] = std::max(meters->trackPeakRight[meterIndex], std::abs(delayedRouteFrame.right));
                }
            }

            for (const auto& edge : graph->edges) {
                if (edge.sourceRoute != route->name || edge.send) {
                    continue;
                }
                if (edge.physicalOutput) {
                    interleavedStereo[index] += delayedRouteFrame.left;
                    interleavedStereo[index + 1u] += delayedRouteFrame.right;
                    continue;
                }
                auto& sum = busBlocks[edge.busName];
                if (sum.size() < blockSampleCount) {
                    sum.resize(blockSampleCount, 0.0f);
                }
                sum[index] += delayedRouteFrame.left;
                sum[index + 1u] += delayedRouteFrame.right;
            }
        }
    }

    if (findTrack(plan, "Master") == nullptr) {
        if (const auto masterBus = busBlocks.find("Master"); masterBus != busBlocks.end()) {
            for (size_t index = 0; index < std::min(interleavedStereo.size(), masterBus->second.size()); ++index) {
                interleavedStereo[index] += masterBus->second[index];
            }
        }
    }
    if (findTrack(plan, "Monitor") == nullptr) {
        if (const auto monitorBus = busBlocks.find("Monitor"); monitorBus != busBlocks.end()) {
            for (size_t index = 0; index < std::min(interleavedStereo.size(), monitorBus->second.size()); ++index) {
                interleavedStereo[index] += monitorBus->second[index];
            }
        }
    }
    if (const TrackState* master = findTrack(plan, "Master")) {
        for (int64_t frameOffset = 0; frameOffset < frameCount; ++frameOffset) {
            const int64_t timelineFrame = timelineFrameForPlaybackFrame(plan, startFrame + frameOffset);
            const double timelineSeconds = static_cast<double>(timelineFrame) / plan.sampleRate;
            const auto index = static_cast<size_t>(frameOffset) * 2u;
            if (master->muted) {
                interleavedStereo[index] = 0.0f;
                interleavedStereo[index + 1u] = 0.0f;
            } else {
                const auto masterFrame = applyMixerMasterGainBalance({interleavedStereo[index], interleavedStereo[index + 1u]},
                                                                    automationValueAt(master->volumeAutomation, timelineSeconds, master->volumeDb),
                                                                    automationLaneValueAt(*master, "track.pan", timelineSeconds, master->pan));
                interleavedStereo[index] = masterFrame.left;
                interleavedStereo[index + 1u] = masterFrame.right;
            }
        }
    }
    if (plan.hasActiveVst3Inserts) {
        std::vector<bool> masterBypassStates;
        masterBypassStates.reserve(plan.activeVst3Inserts.size());
        for (const auto& insert : plan.activeVst3Inserts) {
            masterBypassStates.push_back(insert.bypassed || !insert.available);
        }
        state.masterInsertChain.setBypassStates(masterBypassStates);
    }
    if (plan.hasActiveVst3Inserts) {
        state.masterInsertDryFallback = interleavedStereo;
    }
    if (plan.hasActiveVst3Inserts &&
        !state.masterInsertChain.processInterleavedStereo(
            interleavedStereo,
            static_cast<int>(frameCount),
            state.masterInsertLastError)) {
        state.masterInsertProcessingFailed = true;
        interleavedStereo = state.masterInsertDryFallback;
    }
}

bool renderTrackPreFaderStereoBlock(const ProjectAudioRenderPlan& plan,
                                    const std::string& trackName,
                                    int64_t startFrame,
                                    int64_t frameCount,
                                    std::vector<float>& interleavedStereo) {
    interleavedStereo.assign(static_cast<size_t>(std::max<int64_t>(0, frameCount)) * 2, 0.0f);
    if (frameCount <= 0 || trackName.empty()) {
        return false;
    }
    const TrackState* track = findTrack(plan, trackName);
    if (track == nullptr ||
        track->muted ||
        trackInputMonitorOverridesTimelinePlayback(plan, *track) ||
        track->trackType == "folder" ||
        track->trackType == "bus_folder" ||
        track->trackType == "master" ||
        track->trackType == "monitor" ||
        !isMasterOutputBus(track->outputBus)) {
        return false;
    }
    const bool soloMode = hasSoloedAudioTrack(plan);
    if (soloMode && !track->solo) {
        return false;
    }
    bool wroteAudio = false;
    for (int64_t frameOffset = 0; frameOffset < frameCount; ++frameOffset) {
        const int64_t timelineFrame = timelineFrameForPlaybackFrame(plan, startFrame + frameOffset);
        float left = 0.0f;
        float right = 0.0f;
        for (const auto& renderClip : plan.clips) {
            if (renderClip.clip.trackName != trackName) {
                continue;
            }
            const auto [clipLeft, clipRight] = dryClipSampleAtTimelineFrame(renderClip, timelineFrame, plan.sampleRate);
            left += clipLeft;
            right += clipRight;
        }
        const auto destination = static_cast<size_t>(frameOffset) * 2;
        const auto formattedFrame = applyTrackChannelFormat({left, right}, track, MixerRouteKind::Audio);
        interleavedStereo[destination] = formattedFrame.left;
        interleavedStereo[destination + 1] = formattedFrame.right;
        wroteAudio = wroteAudio ||
            std::abs(formattedFrame.left) > 0.0000001f ||
            std::abs(formattedFrame.right) > 0.0000001f;
    }
    return wroteAudio;
}

bool renderExternalSidechainBusStereoBlock(const ProjectAudioRenderPlan& plan,
                                           const std::string& busName,
                                           int64_t startFrame,
                                           int64_t frameCount,
                                           std::vector<float>& interleavedStereo) {
    interleavedStereo.assign(static_cast<size_t>(std::max<int64_t>(0, frameCount)) * 2, 0.0f);
    if (frameCount <= 0 || plan.externalSidechainBuses.empty() || plan.sampleRate <= 0.0) {
        return false;
    }
    const auto busIt = busName.empty()
        ? plan.externalSidechainBuses.begin()
        : std::find_if(plan.externalSidechainBuses.begin(), plan.externalSidechainBuses.end(), [&](const ProjectExternalSidechainBus& bus) {
              return bus.name == busName;
          });
    if (busIt == plan.externalSidechainBuses.end() ||
        busIt->source.channels <= 0 ||
        busIt->source.sampleRate <= 0 ||
        busIt->source.interleavedSamples.empty()) {
        return false;
    }

    const double sourceRateRatio = static_cast<double>(busIt->source.sampleRate) / plan.sampleRate;
    const int64_t busStartFrame = static_cast<int64_t>(std::round(busIt->startSeconds * plan.sampleRate));
    const double sourceOffsetFrames = std::max(0.0, busIt->sourceOffsetSeconds) * busIt->source.sampleRate;
    bool wroteAudio = false;
    for (int64_t frameOffset = 0; frameOffset < frameCount; ++frameOffset) {
        const int64_t timelineFrame = timelineFrameForPlaybackFrame(plan, startFrame + frameOffset);
        if (timelineFrame < busStartFrame) {
            continue;
        }
        const double sourceFrame = sourceOffsetFrames + static_cast<double>(timelineFrame - busStartFrame) * sourceRateRatio;
        const auto destination = static_cast<size_t>(frameOffset) * 2;
        interleavedStereo[destination] = sourceSampleAt(busIt->source, sourceFrame, 0, sourceRateRatio);
        interleavedStereo[destination + 1] = sourceSampleAt(busIt->source, sourceFrame, 1, sourceRateRatio);
        wroteAudio = wroteAudio ||
            std::abs(interleavedStereo[destination]) > 0.0000001f ||
            std::abs(interleavedStereo[destination + 1]) > 0.0000001f;
    }
    return wroteAudio;
}

bool printRecordedTakeThroughTrackDsp(const ProjectDocument& project,
                                      const std::string& trackName,
                                      const std::string& wavPath,
                                      std::string& error) {
    error.clear();
    if (trackName.empty() || wavPath.empty()) {
        error = "Recorded take print target is missing.";
        return false;
    }
    ProjectAudioRenderPlan lookupPlan;
    lookupPlan.tracks = project.tracks;
    const TrackState* track = findTrack(lookupPlan, trackName);
    if (track == nullptr) {
        error = "Recorded take print track was not found: " + trackName;
        return false;
    }
    const auto inserts = activeLocalRouteInserts(*track);
    if (inserts.empty()) {
        return true;
    }

    WavAudioData source;
    if (!readPcmWavFile(wavPath, source, error)) {
        error = "Could not read recorded take for DSP print: " + error;
        return false;
    }
    if (source.channels <= 0 || source.sampleRate <= 0 || source.frameCount() <= 0) {
        error = "Recorded take is empty or has an unsupported channel layout.";
        return false;
    }

    const int originalChannels = source.channels;
    WavAudioData stereo;
    stereo.channels = 2;
    stereo.sampleRate = source.sampleRate;
    stereo.bitsPerSample = source.bitsPerSample;
    stereo.floatingPoint = source.floatingPoint;
    stereo.interleavedSamples.assign(static_cast<size_t>(source.frameCount()) * 2u, 0.0f);
    for (int64_t frame = 0; frame < source.frameCount(); ++frame) {
        const auto sourceIndex = static_cast<size_t>(frame) * static_cast<size_t>(source.channels);
        const float left = sourceIndex < source.interleavedSamples.size() ? source.interleavedSamples[sourceIndex] : 0.0f;
        const float right = source.channels > 1 && sourceIndex + 1u < source.interleavedSamples.size()
            ? source.interleavedSamples[sourceIndex + 1u]
            : left;
        const auto destination = static_cast<size_t>(frame) * 2u;
        stereo.interleavedSamples[destination] = left;
        stereo.interleavedSamples[destination + 1u] = right;
    }

    ProjectAudioRenderState printState;
    if (!processRouteInsertBlock(printState,
                                 trackName + "#record-print",
                                 inserts,
                                 stereo.sampleRate,
                                 stereo.frameCount(),
                                 stereo.interleavedSamples,
                                 error,
                                 nullptr,
                                 {},
                                 /*synchronousPrepare=*/true)) {
        error = "Recorded take DSP print failed: " + error;
        return false;
    }

    WavAudioData output = stereo;
    if (originalChannels == 1) {
        output.channels = 1;
        output.interleavedSamples.assign(static_cast<size_t>(stereo.frameCount()), 0.0f);
        for (int64_t frame = 0; frame < stereo.frameCount(); ++frame) {
            const auto stereoIndex = static_cast<size_t>(frame) * 2u;
            output.interleavedSamples[static_cast<size_t>(frame)] =
                0.5f * (stereo.interleavedSamples[stereoIndex] + stereo.interleavedSamples[stereoIndex + 1u]);
        }
    }

    const std::filesystem::path target(wavPath);
    const int bits = source.bitsPerSample > 0 ? source.bitsPerSample : 24;
    if (source.floatingPoint && bits > 32) {
        return writeFloat64WavFileAtomically(target, output, error);
    }
    if (source.floatingPoint) {
        return writeFloat32WavFileAtomically(target, output, error);
    }
    if (bits <= 16) {
        return writePcm16WavFileAtomically(target, output, error);
    }
    return writePcm24WavFileAtomically(target, output, error);
}

} // namespace neuracoust::daw
