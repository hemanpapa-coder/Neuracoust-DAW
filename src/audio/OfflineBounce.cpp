#include "audio/OfflineBounce.h"
#include "audio/MasterInsertProcessor.h"
#include "audio/ProjectAudioRenderer.h"
#include "audio/WavFile.h"
#include "project/EditOperations.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

namespace neuracoust::daw {

namespace {

bool isExportableAudioTrack(const TrackState& track) {
    return track.name != "Master" && track.name != "Monitor";
}

int normalizedExportBitDepth(int bitDepth) {
    if (bitDepth == 16 || bitDepth == 24 || bitDepth == 32 || bitDepth == 64) {
        return bitDepth;
    }
    return 24;
}

bool writeProjectWavForBitDepth(const std::filesystem::path& path,
                                const WavAudioData& audio,
                                int bitDepth,
                                std::string& error) {
    switch (normalizedExportBitDepth(bitDepth)) {
        case 16:
            return writePcm16WavFileAtomically(path, audio, error);
        case 32:
            return writeFloat32WavFileAtomically(path, audio, error);
        case 64:
            return writeFloat64WavFileAtomically(path, audio, error);
        case 24:
        default:
            return writePcm24WavFileAtomically(path, audio, error);
    }
}

int effectiveSourceBitDepth(const ProjectDocument& project, const BounceOptions& options) {
    return normalizedExportBitDepth(options.sourceBitDepth > 0 ? options.sourceBitDepth : project.bitDepth);
}

bool shouldApplyExportDither(const ProjectDocument& project, const BounceOptions& options, int exportBitDepth) {
    if (!options.ditherEnabled) {
        return false;
    }
    if (exportBitDepth != 16 && exportBitDepth != 24) {
        return false;
    }
    const int sourceBitDepth = effectiveSourceBitDepth(project, options);
    return sourceBitDepth > exportBitDepth || sourceBitDepth == 32 || sourceBitDepth == 64;
}

const char* exportDitherAlgorithmName(bool active) {
    return active ? "Neuracoust Resolution Guard shaped TPDF v1" : "none";
}

const char* exportDitherReason(const ProjectDocument& project, const BounceOptions& options, int exportBitDepth) {
    if (!options.ditherEnabled) {
        return "disabled-by-user";
    }
    if (exportBitDepth != 16 && exportBitDepth != 24) {
        return "not-needed-for-floating-or-high-resolution-export";
    }
    const int sourceBitDepth = effectiveSourceBitDepth(project, options);
    if (sourceBitDepth > exportBitDepth || sourceBitDepth == 32 || sourceBitDepth == 64) {
        return "high-resolution-to-fixed-pcm";
    }
    return "source-resolution-not-higher-than-export";
}

void applyNeuracoustExportDither(WavAudioData& audio, int exportBitDepth) {
    const double scale = exportBitDepth == 16 ? 32768.0 : 8388608.0;
    const float lsb = static_cast<float>(1.0 / scale);
    uint32_t state = 0x6e647761u;
    std::vector<float> previousNoise(static_cast<size_t>(std::max(1, audio.channels)), 0.0f);
    auto nextUnitNoise = [&]() {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>((state >> 8) * (1.0 / 16777216.0));
    };
    for (size_t index = 0; index < audio.interleavedSamples.size(); ++index) {
        const auto channel = static_cast<size_t>(audio.channels > 0 ? index % static_cast<size_t>(audio.channels) : 0);
        const float tpdf = (nextUnitNoise() - nextUnitNoise()) * lsb;
        const float decorrelated = (nextUnitNoise() - 0.5f) * lsb * 0.18f;
        const float shaped = tpdf + decorrelated - previousNoise[channel] * 0.55f;
        previousNoise[channel] = tpdf + decorrelated;
        audio.interleavedSamples[index] = std::max(-1.0f, std::min(1.0f, audio.interleavedSamples[index] + shaped));
    }
}

void applyExternalSidechainDuckingToStereoMix(const ProjectAudioRenderPlan& plan,
                                              WavAudioData& audio) {
    if (plan.externalSidechainBuses.empty() || audio.channels <= 0 || audio.sampleRate <= 0 || audio.interleavedSamples.empty()) {
        return;
    }

    std::vector<float> sidechainBlock;
    if (!renderExternalSidechainBusStereoBlock(plan, "", 0, audio.frameCount(), sidechainBlock) ||
        sidechainBlock.empty()) {
        return;
    }

    const float attack = std::exp(-1.0f / std::max(1.0f, 0.006f * static_cast<float>(audio.sampleRate)));
    const float release = std::exp(-1.0f / std::max(1.0f, 0.080f * static_cast<float>(audio.sampleRate)));
    float envelope = 0.0f;
    const int64_t frames = std::min<int64_t>(audio.frameCount(), static_cast<int64_t>(sidechainBlock.size() / 2u));
    for (int64_t frame = 0; frame < frames; ++frame) {
        const auto sideIndex = static_cast<size_t>(frame) * 2u;
        const float key = std::max(std::abs(sidechainBlock[sideIndex]), std::abs(sidechainBlock[sideIndex + 1]));
        const float coeff = key > envelope ? attack : release;
        envelope = key + coeff * (envelope - key);
        const float duck = std::min(0.82f, envelope * 1.15f);
        const float gain = std::max(0.12f, 1.0f - duck);
        for (int channel = 0; channel < audio.channels; ++channel) {
            const auto index = static_cast<size_t>(frame) * static_cast<size_t>(audio.channels) + static_cast<size_t>(channel);
            if (index < audio.interleavedSamples.size()) {
                audio.interleavedSamples[index] *= gain;
            }
        }
    }
}

void applyPeakCeilingGuard(WavAudioData& audio, float ceilingDbfs) {
    if (audio.interleavedSamples.empty() || !std::isfinite(ceilingDbfs)) {
        return;
    }
    const float targetPeak = std::pow(10.0f, ceilingDbfs / 20.0f);
    if (!(targetPeak > 0.0f) || targetPeak >= 1.0f) {
        return;
    }
    float peak = 0.0f;
    for (float sample : audio.interleavedSamples) {
        peak = std::max(peak, std::abs(sample));
    }
    if (peak > targetPeak) {
        const float gain = targetPeak / peak;
        for (float& sample : audio.interleavedSamples) {
            sample *= gain;
        }
    }
}

struct StemManifestEntry {
    TrackState track;
    std::filesystem::path path;
    std::vector<ClipState> clips;
};

struct BounceRenderRange {
    double startSeconds = 0.0;
    double endSeconds = 1.0;
    const char* manifestName = "full-project";
};

std::string sanitizedStemName(std::string name) {
    for (char& ch : name) {
        const auto uch = static_cast<unsigned char>(ch);
        if (uch < 0x20 || ch == '/' || ch == '\\' || ch == ':' || ch == '*' ||
            ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|') {
            ch = '_';
        }
    }
    name.erase(std::unique(name.begin(), name.end(), [](char a, char b) {
        return a == '_' && b == '_';
    }), name.end());
    while (!name.empty() && (name.front() == '_' || name.front() == ' ' || name.front() == '.')) {
        name.erase(name.begin());
    }
    while (!name.empty() && (name.back() == '_' || name.back() == ' ' || name.back() == '.')) {
        name.pop_back();
    }
    return name.empty() ? "Track" : name;
}

std::filesystem::path uniqueStemOutputPath(const std::filesystem::path& outputDirectory,
                                           const std::string& stemName) {
    auto candidate = outputDirectory / (stemName + ".wav");
    if (!std::filesystem::exists(candidate)) {
        return candidate;
    }
    for (int suffix = 2; suffix < 10000; ++suffix) {
        candidate = outputDirectory / (stemName + " " + std::to_string(suffix) + ".wav");
        if (!std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

float linearToDb(float value) {
    return value > 0.000001f ? 20.0f * std::log10(std::max(0.000001f, value)) : -96.0f;
}

bool resolveBounceRenderRange(const ProjectDocument& project,
                              const BounceOptions& options,
                              BounceRenderRange& range,
                              std::string& error) {
    if (options.rangeMode == BounceRangeMode::EditSelection) {
        if (!project.editSelectionEnabled || project.editSelectionEndSeconds <= project.editSelectionStartSeconds) {
            error = "No valid edit selection for selection bounce.";
            return false;
        }
        range.startSeconds = std::max(0.0, project.editSelectionStartSeconds);
        range.endSeconds = std::max(range.startSeconds, project.editSelectionEndSeconds);
        if (range.endSeconds <= range.startSeconds) {
            error = "No valid edit selection for selection bounce.";
            return false;
        }
        range.manifestName = "edit-selection";
        error.clear();
        return true;
    }

    range.startSeconds = 0.0;
    range.endSeconds = projectDurationSeconds(project);
    if (range.endSeconds <= 0.0) {
        range.endSeconds = 1.0;
    }
    range.manifestName = "full-project";
    error.clear();
    return true;
}

std::vector<std::string> projectHealthWarningFragments(const ProjectHealthReport& health, bool includeMissingMedia) {
    std::vector<std::string> fragments;
    if (includeMissingMedia && health.missingMediaClips > 0) {
        fragments.push_back(std::to_string(health.missingMediaClips) + " missing media clip(s)");
    }
    if (health.missingVst3Inserts > 0) {
        fragments.push_back(std::to_string(health.missingVst3Inserts) + " missing VST3 insert(s)");
    }
    if (health.overlappingClipPairs > 0) {
        fragments.push_back(std::to_string(health.overlappingClipPairs) + " overlapping clip pair(s)");
    }
    if (health.mutedAudioTracks > 0) {
        fragments.push_back(std::to_string(health.mutedAudioTracks) + " muted track(s)");
    }
    if (health.soloedAudioTracks > 0) {
        fragments.push_back(std::to_string(health.soloedAudioTracks) + " soloed track(s)");
    }
    if (health.mutedClips > 0) {
        fragments.push_back(std::to_string(health.mutedClips) + " muted clip(s)");
    }
    return fragments;
}

void appendWarningFragments(std::string& message, const std::vector<std::string>& fragments) {
    if (fragments.empty()) {
        return;
    }
    message += " Warning:";
    for (size_t index = 0; index < fragments.size(); ++index) {
        message += index == 0 ? " " : ", ";
        message += fragments[index];
    }
    message += ".";
}

AudioLevelStats analyzeInterleavedStereoLevels(const WavAudioData& audio) {
    AudioLevelStats stats;
    if (audio.channels <= 0 || audio.interleavedSamples.empty()) {
        return stats;
    }

    double sumSquaresLeft = 0.0;
    double sumSquaresRight = 0.0;
    int64_t frames = 0;
    const int64_t frameCount = audio.frameCount();
    for (int64_t frame = 0; frame < frameCount; ++frame) {
        const auto base = static_cast<size_t>(frame * audio.channels);
        const float left = audio.interleavedSamples[base];
        const float right = audio.channels > 1
            ? audio.interleavedSamples[base + 1]
            : left;
        const float leftAbs = std::abs(left);
        const float rightAbs = std::abs(right);
        stats.peakLeft = std::max(stats.peakLeft, leftAbs);
        stats.peakRight = std::max(stats.peakRight, rightAbs);
        if (leftAbs >= 1.0f) {
            ++stats.clippedSampleCount;
        }
        if (rightAbs >= 1.0f) {
            ++stats.clippedSampleCount;
        }
        sumSquaresLeft += static_cast<double>(left) * left;
        sumSquaresRight += static_cast<double>(right) * right;
        ++frames;
    }
    if (frames > 0) {
        stats.rmsLeft = static_cast<float>(std::sqrt(sumSquaresLeft / frames));
        stats.rmsRight = static_cast<float>(std::sqrt(sumSquaresRight / frames));
    }
    stats.clippingDetected = stats.clippedSampleCount > 0;
    const float maxPeak = std::max(stats.peakLeft, stats.peakRight);
    const float maxRms = std::max(stats.rmsLeft, stats.rmsRight);
    stats.nearSilent = maxPeak < 0.00001f && maxRms < 0.000001f;
    return stats;
}

void writeLevelStatsJson(std::ostream& out, const AudioLevelStats& stats, const char* indent) {
    out << indent << "\"peakLeft\": " << stats.peakLeft << ",\n";
    out << indent << "\"peakRight\": " << stats.peakRight << ",\n";
    out << indent << "\"rmsLeft\": " << stats.rmsLeft << ",\n";
    out << indent << "\"rmsRight\": " << stats.rmsRight << ",\n";
    out << indent << "\"clippedSampleCount\": " << stats.clippedSampleCount << ",\n";
    out << indent << "\"clippingDetected\": " << (stats.clippingDetected ? "true" : "false") << ",\n";
    out << indent << "\"nearSilent\": " << (stats.nearSilent ? "true" : "false") << ",\n";
    out << indent << "\"peakLeftDb\": " << linearToDb(stats.peakLeft) << ",\n";
    out << indent << "\"peakRightDb\": " << linearToDb(stats.peakRight) << ",\n";
    out << indent << "\"rmsLeftDb\": " << linearToDb(stats.rmsLeft) << ",\n";
    out << indent << "\"rmsRightDb\": " << linearToDb(stats.rmsRight) << "\n";
}

std::string escapeManifestString(const std::string& value);

void writeMonitorModulesJson(std::ostream& out, const ProjectDocument& project, const char* key, const char* indent) {
    out << indent << "\"" << key << "\": [";
    for (size_t index = 0; index < project.monitorModules.size(); ++index) {
        const auto& module = project.monitorModules[index];
        out << (index == 0 ? "" : ", ")
            << "{\"id\":\"" << escapeManifestString(module.id)
            << "\",\"enabled\":" << (module.enabled ? "true" : "false")
            << ",\"realModel\":\"" << escapeManifestString(module.realModel)
            << "\",\"targetModelA\":\"" << escapeManifestString(module.targetModelA)
            << "\",\"targetModelB\":\"" << escapeManifestString(module.targetModelB)
            << "\",\"targetModelC\":\"" << escapeManifestString(module.targetModelC)
            << "\",\"speakerOutputA\":\"" << escapeManifestString(module.speakerOutputA)
            << "\",\"speakerOutputB\":\"" << escapeManifestString(module.speakerOutputB)
            << "\",\"speakerOutputC\":\"" << escapeManifestString(module.speakerOutputC)
            << "\",\"streamingPreview\":\"" << escapeManifestString(module.streamingPreview)
            << "\",\"activeTargetSlot\":" << std::max(0, std::min(2, module.activeTargetSlot))
            << ",\"speakerSimulationWeightA\":" << std::max(-0.5f, std::min(1.0f, module.speakerSimulationWeightA))
            << ",\"speakerSimulationWeightB\":" << std::max(-0.5f, std::min(1.0f, module.speakerSimulationWeightB))
            << ",\"speakerSimulationWeightC\":" << std::max(-0.5f, std::min(1.0f, module.speakerSimulationWeightC))
            << "}";
    }
    out << "]";
}

void writeMasterInsertChainJson(std::ostream& out, const ProjectDocument& project, const char* key, const char* indent) {
    out << indent << "\"" << key << "\": [\n";
    for (size_t index = 0; index < project.masterInserts.size(); ++index) {
        const auto& insert = project.masterInserts[index];
        const bool activeInRender = !insert.bypassed &&
            (insert.pluginAppId == "neuracoust-monitor-dsp" || isVst3MasterInsert(insert));
        out << indent << "  {\"index\":" << index
            << ",\"pluginName\":\"" << escapeManifestString(insert.pluginName)
            << "\",\"pluginAppId\":\"" << escapeManifestString(insert.pluginAppId)
            << "\",\"pluginFormat\":\"" << escapeManifestString(insert.pluginFormat)
            << "\",\"pluginPath\":\"" << escapeManifestString(insert.pluginPath)
            << "\",\"bypassed\":" << (insert.bypassed ? "true" : "false")
            << ",\"available\":" << (insert.available ? "true" : "false")
            << ",\"dspExecutionMode\":\"" << escapeManifestString(insert.dspExecutionMode.empty() ? "native" : insert.dspExecutionMode)
            << "\",\"assignedDspServerId\":\"" << escapeManifestString(insert.assignedDspServerId)
            << "\",\"serverModuleId\":\"" << escapeManifestString(insert.serverModuleId)
            << "\",\"reportedLatencySamples\":" << insert.reportedLatencySamples
            << ",\"dspAvailable\":" << (insert.dspAvailable ? "true" : "false")
            << ",\"dspLastError\":\"" << escapeManifestString(insert.dspLastError)
            << "\",\"activeInRender\":" << (activeInRender ? "true" : "false")
            << "}";
        out << (index + 1 == project.masterInserts.size() ? "\n" : ",\n");
    }
    out << indent << "]";
}

void writeProjectHealthJson(std::ostream& out, const ProjectHealthReport& health, const char* key, const char* indent) {
    out << indent << "\"" << key << "\": {\n";
    out << indent << "  \"summary\": \"" << escapeManifestString(summarizeProjectHealth(health)) << "\",\n";
    out << indent << "  \"clips\": " << health.clips << ",\n";
    out << indent << "  \"overlappingClipPairs\": " << health.overlappingClipPairs << ",\n";
    out << indent << "  \"missingMediaClips\": " << health.missingMediaClips << ",\n";
    out << indent << "  \"masterInserts\": " << health.masterInserts << ",\n";
    out << indent << "  \"trackInserts\": " << health.trackInserts << ",\n";
    out << indent << "  \"vst3MasterInserts\": " << health.vst3MasterInserts << ",\n";
    out << indent << "  \"vst3TrackInserts\": " << health.vst3TrackInserts << ",\n";
    out << indent << "  \"activeVst3TrackInserts\": " << health.activeVst3TrackInserts << ",\n";
    out << indent << "  \"activeVst3TrackInsertLabels\": [";
    for (size_t index = 0; index < health.activeVst3TrackInsertLabels.size(); ++index) {
        out << (index == 0 ? "" : ", ")
            << "\"" << escapeManifestString(health.activeVst3TrackInsertLabels[index]) << "\"";
    }
    out << "],\n";
    out << indent << "  \"missingVst3Inserts\": " << health.missingVst3Inserts << ",\n";
    out << indent << "  \"mutedAudioTracks\": " << health.mutedAudioTracks << ",\n";
    out << indent << "  \"soloedAudioTracks\": " << health.soloedAudioTracks << ",\n";
    out << indent << "  \"mutedClips\": " << health.mutedClips << ",\n";
    out << indent << "  \"disabledMonitorModules\": " << health.disabledMonitorModules << ",\n";
    out << indent << "  \"messages\": [";
    for (size_t index = 0; index < health.messages.size(); ++index) {
        out << (index == 0 ? "" : ", ")
            << "\"" << escapeManifestString(health.messages[index]) << "\"";
    }
    out << "]\n";
    out << indent << "}";
}

void writeTimelineContextJson(std::ostream& out,
                              const ProjectDocument& project,
                              bool renderedLoop,
                              const char* key,
                              const char* indent) {
    out << indent << "\"" << key << "\": {\n";
    out << indent << "  \"tempoBpm\": " << project.tempoBpm << ",\n";
    out << indent << "  \"beatSnapEnabled\": " << (project.beatSnapEnabled ? "true" : "false") << ",\n";
    out << indent << "  \"editMode\": \"" << escapeManifestString(project.editMode) << "\",\n";
    out << indent << "  \"gridUnit\": \"" << escapeManifestString(project.gridUnit) << "\",\n";
    out << indent << "  \"snapQuantumSeconds\": " << projectTimelineQuantumSeconds(project) << ",\n";
    out << indent << "  \"projectLoopEnabled\": " << (project.loopEnabled ? "true" : "false") << ",\n";
    out << indent << "  \"loopStartSeconds\": " << project.loopStartSeconds << ",\n";
    out << indent << "  \"loopEndSeconds\": " << project.loopEndSeconds << ",\n";
    out << indent << "  \"renderedLoop\": " << (renderedLoop ? "true" : "false") << ",\n";
    out << indent << "  \"markers\": [";
    for (size_t index = 0; index < project.markers.size(); ++index) {
        const auto& marker = project.markers[index];
        out << (index == 0 ? "" : ", ")
            << "{\"id\":\"" << escapeManifestString(marker.id)
            << "\",\"name\":\"" << escapeManifestString(marker.name)
            << "\",\"timeSeconds\":" << marker.timeSeconds << "}";
    }
    out << "],\n";
    out << indent << "  \"chordSections\": [";
    for (size_t index = 0; index < project.chordEvents.size(); ++index) {
        const auto& chord = project.chordEvents[index];
        out << (index == 0 ? "" : ", ")
            << "{\"id\":\"" << escapeManifestString(chord.id)
            << "\",\"name\":\"" << escapeManifestString(chord.name)
            << "\",\"timeSeconds\":" << chord.timeSeconds << "}";
    }
    out << "]\n";
    out << indent << "}";
}

size_t clipCountForTrack(const ProjectDocument& project, const std::string& trackName) {
    return static_cast<size_t>(std::count_if(project.clips.begin(), project.clips.end(), [&](const ClipState& clip) {
        return clip.trackName == trackName;
    }));
}

std::vector<ClipState> clipsForTrack(const ProjectDocument& project, const std::string& trackName) {
    std::vector<ClipState> clips;
    for (const auto& clip : project.clips) {
        if (clip.trackName == trackName) {
            clips.push_back(clip);
        }
    }
    std::stable_sort(clips.begin(), clips.end(), [](const ClipState& a, const ClipState& b) {
        if (a.startSeconds != b.startSeconds) {
            return a.startSeconds < b.startSeconds;
        }
        return a.id < b.id;
    });
    return clips;
}

void writeClipSnapshotJson(std::ostream& out, const std::vector<ClipState>& clips) {
    out << "[";
    for (size_t index = 0; index < clips.size(); ++index) {
        const auto& clip = clips[index];
        const bool missingSource = !clip.sourcePath.empty() && !std::filesystem::exists(clip.sourcePath);
        out << (index == 0 ? "" : ",")
            << "{\"id\":\"" << escapeManifestString(clip.id)
            << "\",\"trackName\":\"" << escapeManifestString(clip.trackName)
            << "\",\"regionName\":\"" << escapeManifestString(clip.regionName)
            << "\",\"sourceFileUid\":\"" << escapeManifestString(clip.sourceFileUid)
            << "\",\"sourceChannels\":" << clip.sourceChannels
            << ",\"sourceSampleRate\":" << clip.sourceSampleRate
            << ",\"sourceBitsPerSample\":" << clip.sourceBitsPerSample
            << ",\"sourceFloatingPoint\":" << (clip.sourceFloatingPoint ? "true" : "false")
            << ",\"sourceHasBroadcastTimeReference\":" << (clip.sourceHasBroadcastTimeReference ? "true" : "false")
            << ",\"sourceTimeReferenceSamples\":" << clip.sourceTimeReferenceSamples
            << ",\"sourceTimeReferenceSeconds\":" << clip.sourceTimeReferenceSeconds
            << ",\"colorHex\":\"" << escapeManifestString(clip.colorHex)
            << "\",\"sourcePath\":\"" << escapeManifestString(clip.sourcePath)
            << "\",\"startSeconds\":" << clip.startSeconds
            << ",\"durationSeconds\":" << clip.durationSeconds
            << ",\"sourceOffsetSeconds\":" << clip.sourceOffsetSeconds
            << ",\"timeScale\":" << clip.timeScale
            << ",\"gainDb\":" << clip.gainDb
            << ",\"fadeInSeconds\":" << clip.fadeInSeconds
            << ",\"fadeOutSeconds\":" << clip.fadeOutSeconds
            << ",\"fadeInCurve\":\"" << escapeManifestString(clip.fadeInCurve)
            << "\",\"fadeOutCurve\":\"" << escapeManifestString(clip.fadeOutCurve) << "\""
            << ",\"muted\":" << (clip.muted ? "true" : "false")
            << ",\"polarityInverted\":" << (clip.polarityInverted ? "true" : "false")
            << ",\"locked\":" << (clip.locked ? "true" : "false")
            << ",\"missingSource\":" << (missingSource ? "true" : "false")
            << "}";
    }
    out << "]";
}

void writeTrackSendSnapshotJson(std::ostream& out, const std::vector<TrackSendState>& sends) {
    out << "[";
    for (size_t index = 0; index < sends.size(); ++index) {
        const auto& send = sends[index];
        out << (index == 0 ? "" : ",")
            << "{\"index\":" << index
            << ",\"busName\":\"" << escapeManifestString(send.busName)
            << "\",\"gainDb\":" << send.gainDb
            << ",\"pan\":" << send.pan
            << ",\"enabled\":" << (send.enabled ? "true" : "false")
            << ",\"preFader\":" << (send.preFader ? "true" : "false")
            << ",\"stereo\":" << (send.stereo ? "true" : "false")
            << "}";
    }
    out << "]";
}

void writeTrackInsertSnapshotJson(std::ostream& out, const std::vector<TrackInsertSlot>& inserts) {
    out << "[";
    for (size_t index = 0; index < inserts.size(); ++index) {
        const auto& insert = inserts[index];
        const bool active = insert.enabled && !insert.bypassed &&
            (insert.pluginFormat == "VST3" || insert.pluginFormat == "VST3/AU");
        out << (index == 0 ? "" : ",")
            << "{\"index\":" << index
            << ",\"pluginName\":\"" << escapeManifestString(insert.pluginName)
            << "\",\"pluginFormat\":\"" << escapeManifestString(insert.pluginFormat)
            << "\",\"pluginPath\":\"" << escapeManifestString(insert.pluginPath)
            << "\",\"enabled\":" << (insert.enabled ? "true" : "false")
            << ",\"bypassed\":" << (insert.bypassed ? "true" : "false")
            << ",\"dspExecutionMode\":\"" << escapeManifestString(insert.dspExecutionMode.empty() ? "native" : insert.dspExecutionMode)
            << "\",\"assignedDspServerId\":\"" << escapeManifestString(insert.assignedDspServerId)
            << "\",\"serverModuleId\":\"" << escapeManifestString(insert.serverModuleId)
            << "\",\"reportedLatencySamples\":" << insert.reportedLatencySamples
            << ",\"dspAvailable\":" << (insert.dspAvailable ? "true" : "false")
            << ",\"dspLastError\":\"" << escapeManifestString(insert.dspLastError)
            << "\",\"activeTrackVst3PendingRenderHost\":" << (active ? "true" : "false")
            << "}";
    }
    out << "]";
}

void writeTrackMixSnapshotJson(std::ostream& out,
                               const ProjectDocument& project,
                               const char* key,
                               const char* indent) {
    out << indent << "\"" << key << "\": [\n";
    for (size_t index = 0; index < project.tracks.size(); ++index) {
        const auto& track = project.tracks[index];
        const auto clips = clipsForTrack(project, track.name);
        out << indent << "  {\"name\":\"" << escapeManifestString(track.name)
            << "\",\"trackType\":\"" << escapeManifestString(track.trackType)
            << "\",\"colorHex\":\"" << escapeManifestString(track.colorHex)
            << "\",\"inputBus\":\"" << escapeManifestString(track.inputBus)
            << "\",\"outputBus\":\"" << escapeManifestString(track.outputBus)
            << "\",\"exportable\":" << (isExportableAudioTrack(track) ? "true" : "false")
            << ",\"clipCount\":" << clips.size()
            << ",\"volumeDb\":" << track.volumeDb
            << ",\"pan\":" << track.pan
            << ",\"muted\":" << (track.muted ? "true" : "false")
            << ",\"solo\":" << (track.solo ? "true" : "false")
            << ",\"recordArmed\":" << (track.recordArmed ? "true" : "false")
            << ",\"inserts\":";
        writeTrackInsertSnapshotJson(out, track.inserts);
        out << ",\"sends\":";
        writeTrackSendSnapshotJson(out, track.sends);
        out << ",\"clips\":";
        writeClipSnapshotJson(out, clips);
        out << "}";
        out << (index + 1 == project.tracks.size() ? "\n" : ",\n");
    }
    out << indent << "]";
}

std::string escapeManifestString(const std::string& value) {
    std::ostringstream out;
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (ch < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(ch) << std::dec << std::setfill(' ');
                } else {
                    out << static_cast<char>(ch);
                }
                break;
        }
    }
    return out.str();
}

std::string pathRelativeToDirectory(const std::filesystem::path& path,
                                    const std::filesystem::path& directory) {
    const auto relative = path.lexically_relative(directory);
    if (!relative.empty() && relative.begin() != relative.end() && *relative.begin() != "..") {
        return relative.generic_string();
    }
    return path.generic_string();
}

bool writeTextFileAtomically(const std::filesystem::path& path,
                             const std::string& text,
                             std::string& error) {
    error.clear();
    if (path.empty()) {
        error = "Output path is empty.";
        return false;
    }

    std::error_code fsError;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), fsError);
        if (fsError) {
            error = "Could not create output folder: " + fsError.message();
            return false;
        }
    }

    auto tempPath = path;
    tempPath += ".writing";
    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out.good()) {
            error = "Could not create temporary output file.";
            return false;
        }
        out << text;
        out.close();
        if (!out) {
            std::filesystem::remove(tempPath, fsError);
            error = "Could not finish writing temporary output file.";
            return false;
        }
    }

    std::filesystem::rename(tempPath, path, fsError);
    if (fsError) {
        std::filesystem::remove(path, fsError);
        fsError.clear();
        std::filesystem::rename(tempPath, path, fsError);
    }
    if (fsError) {
        const auto message = fsError.message();
        std::filesystem::remove(tempPath, fsError);
        error = "Could not replace output file: " + message;
        return false;
    }
    return true;
}

bool writeStemExportManifest(const std::filesystem::path& manifestPath,
                             const std::filesystem::path& outputDirectory,
                             const ProjectDocument& project,
                             const std::vector<StemManifestEntry>& exportedStems,
                             const std::vector<AudioLevelStats>& exportedStats,
                             const std::vector<std::string>& missingMediaClipIds,
                             double durationSeconds,
                             int sampleRate,
                             int bitDepth,
                             std::string& error) {
    std::ostringstream out;

    out << "{\n";
    out << "  \"format\": \"neuracoust-daw-stem-export-v1\",\n";
    out << "  \"projectName\": \"" << escapeManifestString(project.name) << "\",\n";
    out << "  \"sampleRate\": " << sampleRate << ",\n";
    out << "  \"bitDepth\": " << bitDepth << ",\n";
    out << "  \"durationSeconds\": " << durationSeconds << ",\n";
    out << "  \"renderType\": \"pre-master-track-stems\",\n";
    out << "  \"excludesMonitorDsp\": true,\n";
    out << "  \"excludesMasterVst3Inserts\": true,\n";
    writeMonitorModulesJson(out, project, "excludedMonitorDspModules", "  ");
    out << ",\n";
    writeMasterInsertChainJson(out, project, "excludedMasterInsertChain", "  ");
    out << ",\n";
    writeProjectHealthJson(out, analyzeProjectHealth(project), "projectHealth", "  ");
    out << ",\n";
    writeTimelineContextJson(out, project, false, "timelineContext", "  ");
    out << ",\n";
    out << "  \"missingMediaRenderedAsSilence\": " << (missingMediaClipIds.empty() ? "false" : "true") << ",\n";
    out << "  \"missingMediaClipIds\": [";
    for (size_t index = 0; index < missingMediaClipIds.size(); ++index) {
        out << (index == 0 ? "" : ", ")
            << "\"" << escapeManifestString(missingMediaClipIds[index]) << "\"";
    }
    out << "],\n";
    out << "  \"stems\": [\n";
    for (size_t index = 0; index < exportedStems.size(); ++index) {
        const auto& stem = exportedStems[index];
        out << "    {\"trackName\":\"" << escapeManifestString(stem.track.name)
            << "\",\"file\":\"" << escapeManifestString(pathRelativeToDirectory(stem.path, outputDirectory))
            << "\",\"clipCount\":" << stem.clips.size()
            << ",\"trackState\":{"
            << "\"trackType\":\"" << escapeManifestString(stem.track.trackType)
            << "\",\"colorHex\":\"" << escapeManifestString(stem.track.colorHex)
            << "\",\"inputBus\":\"" << escapeManifestString(stem.track.inputBus)
            << "\",\"outputBus\":\"" << escapeManifestString(stem.track.outputBus)
            << "\",\"volumeDb\":" << stem.track.volumeDb
            << ",\"pan\":" << stem.track.pan
            << ",\"muted\":" << (stem.track.muted ? "true" : "false")
            << ",\"solo\":" << (stem.track.solo ? "true" : "false")
            << ",\"recordArmed\":" << (stem.track.recordArmed ? "true" : "false")
            << "}"
            << ",\"clips\":";
        writeClipSnapshotJson(out, stem.clips);
        out << ""
            << ",\"levelStats\":{";
        const auto stats = index < exportedStats.size() ? exportedStats[index] : AudioLevelStats {};
        out << "\"peakLeft\":" << stats.peakLeft
            << ",\"peakRight\":" << stats.peakRight
            << ",\"rmsLeft\":" << stats.rmsLeft
            << ",\"rmsRight\":" << stats.rmsRight
            << ",\"clippedSampleCount\":" << stats.clippedSampleCount
            << ",\"clippingDetected\":" << (stats.clippingDetected ? "true" : "false")
            << ",\"nearSilent\":" << (stats.nearSilent ? "true" : "false")
            << ",\"peakLeftDb\":" << linearToDb(stats.peakLeft)
            << ",\"peakRightDb\":" << linearToDb(stats.peakRight)
            << ",\"rmsLeftDb\":" << linearToDb(stats.rmsLeft)
            << ",\"rmsRightDb\":" << linearToDb(stats.rmsRight)
            << "}}";
        out << (index + 1 == exportedStems.size() ? "\n" : ",\n");
    }
    out << "  ]\n";
    out << "}\n";
    if (!writeTextFileAtomically(manifestPath, out.str(), error)) {
        const auto writeError = error;
        error = "Could not write stem export manifest: " + writeError;
        return false;
    }
    error.clear();
    return true;
}

bool writeBounceManifest(const std::filesystem::path& manifestPath,
                         const std::filesystem::path& outputPath,
                         const ProjectDocument& project,
                         const ProjectAudioRenderPlan& plan,
                         const BounceOptions& options,
                         const BounceRenderRange& range,
                         double durationSeconds,
                         double renderElapsedSeconds,
                         bool realtimePacingApplied,
                         int sampleRate,
                         int bitDepth,
                         size_t activeVst3InsertCount,
                         const AudioLevelStats& levelStats,
                         std::string& error) {
    std::ostringstream out;

    out << "{\n";
    out << "  \"format\": \"neuracoust-daw-bounce-v1\",\n";
    out << "  \"projectName\": \"" << escapeManifestString(project.name) << "\",\n";
    out << "  \"file\": \"" << escapeManifestString(outputPath.filename().generic_string()) << "\",\n";
    out << "  \"sampleRate\": " << sampleRate << ",\n";
    out << "  \"bitDepth\": " << bitDepth << ",\n";
    out << "  \"sourceBitDepth\": " << effectiveSourceBitDepth(project, options) << ",\n";
    out << "  \"ditherRequested\": " << (options.ditherEnabled ? "true" : "false") << ",\n";
    out << "  \"ditherEnabled\": " << (shouldApplyExportDither(project, options, bitDepth) ? "true" : "false") << ",\n";
    out << "  \"ditherAlgorithm\": \"" << exportDitherAlgorithmName(shouldApplyExportDither(project, options, bitDepth)) << "\",\n";
    out << "  \"ditherReason\": \"" << exportDitherReason(project, options, bitDepth) << "\",\n";
    out << "  \"externalSidechainBusCount\": " << options.externalSidechainBuses.size() << ",\n";
    out << "  \"externalSidechainDuckingEnabled\": " << (options.externalSidechainDuckingEnabled ? "true" : "false") << ",\n";
    out << "  \"peakCeilingGuardEnabled\": " << (options.peakCeilingGuardEnabled ? "true" : "false") << ",\n";
    out << "  \"peakCeilingDbfs\": " << options.peakCeilingDbfs << ",\n";
    out << "  \"durationSeconds\": " << durationSeconds << ",\n";
    out << "  \"channels\": 2,\n";
    out << "  \"renderType\": \"stereo-master-bounce\",\n";
    out << "  \"bounceMode\": \"" << (options.renderMode == BounceRenderMode::Realtime ? "realtime" : "offline") << "\",\n";
    out << "  \"rangeMode\": \"" << range.manifestName << "\",\n";
    out << "  \"renderStartSeconds\": " << range.startSeconds << ",\n";
    out << "  \"renderEndSeconds\": " << range.endSeconds << ",\n";
    out << "  \"renderDurationSeconds\": " << durationSeconds << ",\n";
    out << "  \"renderElapsedSeconds\": " << renderElapsedSeconds << ",\n";
    out << "  \"realtimeRequested\": " << (options.renderMode == BounceRenderMode::Realtime ? "true" : "false") << ",\n";
    out << "  \"realtimePacingApplied\": " << (realtimePacingApplied ? "true" : "false") << ",\n";
    out << "  \"realtimePacingTargetSeconds\": " << (options.renderMode == BounceRenderMode::Realtime ? durationSeconds : 0.0) << ",\n";
    out << "  \"externalHardwareTimingReserved\": " << (options.renderMode == BounceRenderMode::Realtime ? "true" : "false") << ",\n";
    out << "  \"includesMonitorDsp\": true,\n";
    out << "  \"monitorDspRenderPath\": \"" << (plan.renderMonitorDsp ? "mixer-graph-monitor-route" : "master-insert-post-process") << "\",\n";
    out << "  \"activeVst3MasterInserts\": " << activeVst3InsertCount << ",\n";
    out << "  \"activeVst3TrackInserts\": " << plan.activeTrackVst3InsertLabels.size() << ",\n";
    out << "  \"trackVst3AudioRendered\": " << (plan.activeTrackVst3InsertLabels.empty() ? "false" : "true") << ",\n";
    out << "  \"trackVst3RenderStatus\": \"" << (plan.activeTrackVst3InsertLabels.empty() ? "no-active-track-vst3-inserts" : "direct-master-output-rendered") << "\",\n";
    out << "  \"activeVst3TrackInsertLabels\": [";
    for (size_t index = 0; index < plan.activeTrackVst3InsertLabels.size(); ++index) {
        out << (index == 0 ? "" : ", ")
            << "\"" << escapeManifestString(plan.activeTrackVst3InsertLabels[index]) << "\"";
    }
    out << "],\n";
    writeMonitorModulesJson(out, project, "monitorDspModules", "  ");
    out << ",\n";
    writeMasterInsertChainJson(out, project, "masterInsertChain", "  ");
    out << ",\n";
    writeProjectHealthJson(out, analyzeProjectHealth(project), "projectHealth", "  ");
    out << ",\n";
    writeTimelineContextJson(out, project, false, "timelineContext", "  ");
    out << ",\n";
    writeTrackMixSnapshotJson(out, project, "trackMixSnapshot", "  ");
    out << ",\n";
    out << "  \"levelStats\": {\n";
    writeLevelStatsJson(out, levelStats, "    ");
    out << "  },\n";
    out << "  \"loopRendered\": false,\n";
    out << "  \"missingMediaRenderedAsSilence\": " << (plan.hasMissingMedia ? "true" : "false") << ",\n";
    out << "  \"missingMediaClipIds\": [";
    for (size_t index = 0; index < plan.missingMediaClipIds.size(); ++index) {
        out << (index == 0 ? "" : ", ")
            << "\"" << escapeManifestString(plan.missingMediaClipIds[index]) << "\"";
    }
    out << "]\n";
    out << "}\n";
    if (!writeTextFileAtomically(manifestPath, out.str(), error)) {
        const auto writeError = error;
        error = "Could not write bounce manifest: " + writeError;
        return false;
    }
    error.clear();
    return true;
}

ProjectDocument projectForStemTrack(const ProjectDocument& project, const TrackState& track) {
    ProjectDocument stem = project;
    stem.tracks.clear();
    stem.tracks.push_back(track);
    stem.tracks.front().muted = false;
    stem.tracks.front().solo = false;
    stem.tracks.front().recordArmed = false;
    stem.clips.erase(std::remove_if(stem.clips.begin(), stem.clips.end(), [&](const ClipState& clip) {
        return clip.trackName != track.name;
    }), stem.clips.end());
    stem.masterInserts.clear();
    stem.monitorModules.clear();
    stem.loopEnabled = false;
    return stem;
}

} // namespace

BounceResult bounceProjectToWav(const ProjectDocument& project, const std::string& outputPath, const BounceOptions& options) {
    BounceResult result;
    const auto bounceStartTime = std::chrono::steady_clock::now();
    const int sampleRate = static_cast<int>(project.sampleRate);
    const int bitDepth = normalizedExportBitDepth(project.bitDepth);
    if (sampleRate <= 0) {
        result.message = "Invalid sample rate.";
        return result;
    }

    BounceRenderRange range;
    if (!resolveBounceRenderRange(project, options, range, result.message)) {
        return result;
    }
    const auto startFrame = static_cast<int64_t>(std::round(range.startSeconds * sampleRate));
    const auto frameCount = std::max<int64_t>(1, static_cast<int64_t>(std::round((range.endSeconds - range.startSeconds) * sampleRate)));

    ProjectAudioRenderPlan plan;
    std::string error;
    if (!makeProjectAudioRenderPlan(project, plan, error)) {
        result.message = error;
        return result;
    }
    plan.externalSidechainBuses = options.externalSidechainBuses;
    plan.loopEnabled = false;
    plan.renderMonitorDsp = true;

    WavAudioData mix;
    mix.channels = 2;
    mix.sampleRate = sampleRate;
    ProjectAudioRenderState renderState;
    // offline=true: prepare insert chains synchronously (the async preparer can't keep up with
    // faster-than-realtime rendering, and a bounce must not drop inserts to dry).
    renderProjectAudioBlockWithStateAndMeters(plan, renderState, startFrame, frameCount, mix.interleavedSamples, nullptr, true);
    if (renderState.masterInsertProcessingFailed) {
        result.message = "VST3 insert failed: master insert: " + renderState.masterInsertLastError;
        return result;
    }
    if (options.externalSidechainDuckingEnabled && !options.externalSidechainBuses.empty()) {
        applyExternalSidechainDuckingToStereoMix(plan, mix);
    }

    if (!applyProjectMasterInsertsToStereoMix(project, mix, error, false, false)) {
        result.message = error;
        return result;
    }
    if (options.peakCeilingGuardEnabled) {
        applyPeakCeilingGuard(mix, options.peakCeilingDbfs);
    }

    if (shouldApplyExportDither(project, options, bitDepth)) {
        applyNeuracoustExportDither(mix, bitDepth);
    }
    const bool wroteWav = writeProjectWavForBitDepth(outputPath, mix, bitDepth, error);
    if (!wroteWav) {
        result.message = error;
        return result;
    }
    const auto levelStats = analyzeInterleavedStereoLevels(mix);
    const double bounceDurationSeconds = mix.sampleRate > 0
        ? static_cast<double>(mix.frameCount()) / mix.sampleRate
        : range.endSeconds - range.startSeconds;
    bool realtimePacingApplied = false;
    if (options.renderMode == BounceRenderMode::Realtime && bounceDurationSeconds > 0.0) {
        const auto targetEndTime = bounceStartTime + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(bounceDurationSeconds));
        const auto now = std::chrono::steady_clock::now();
        if (targetEndTime > now) {
            std::this_thread::sleep_until(targetEndTime);
        }
        realtimePacingApplied = true;
    }
    const double renderElapsedSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - bounceStartTime).count();
    const auto manifestPath = std::filesystem::path(outputPath).replace_extension(".bounce.json");
    if (!writeBounceManifest(
            manifestPath,
            outputPath,
            project,
            plan,
            options,
            range,
            bounceDurationSeconds,
            renderElapsedSeconds,
            realtimePacingApplied,
            sampleRate,
            bitDepth,
            activeVst3MasterInsertCount(project),
            levelStats,
            error)) {
        result.message = error;
        return result;
    }
    result.ok = true;
    result.durationSeconds = bounceDurationSeconds;
    result.levelStats = levelStats;
    result.missingMediaClipIds = plan.missingMediaClipIds;
    result.manifestPath = manifestPath.generic_string();
    result.message = options.renderMode == BounceRenderMode::Realtime
        ? "Realtime bounce complete."
        : "Offline bounce complete.";
    std::vector<std::string> warnings;
    if (levelStats.nearSilent || levelStats.clippingDetected || plan.hasMissingMedia) {
        if (levelStats.nearSilent) {
            warnings.push_back("output is near-silent");
        }
        if (levelStats.clippingDetected) {
            warnings.push_back("clipping detected");
        }
        if (plan.hasMissingMedia) {
            warnings.push_back("missing media was rendered as silence");
        }
    }
    const auto healthWarnings = projectHealthWarningFragments(analyzeProjectHealth(project), false);
    warnings.insert(warnings.end(), healthWarnings.begin(), healthWarnings.end());
    appendWarningFragments(result.message, warnings);
    return result;
}

BounceResult bounceProjectToWav(const ProjectDocument& project, const std::string& outputPath) {
    return bounceProjectToWav(project, outputPath, {});
}

StemExportResult exportProjectTrackStems(const ProjectDocument& project, const std::filesystem::path& outputDirectory) {
    StemExportResult result;
    const int sampleRate = static_cast<int>(project.sampleRate);
    const int bitDepth = normalizedExportBitDepth(project.bitDepth);
    if (sampleRate <= 0) {
        result.message = "Invalid sample rate.";
        return result;
    }
    if (outputDirectory.empty()) {
        result.message = "Output directory is empty.";
        return result;
    }

    std::error_code fsError;
    std::filesystem::create_directories(outputDirectory, fsError);
    if (fsError) {
        result.message = "Could not create stem export folder: " + fsError.message();
        return result;
    }

    double endSeconds = projectDurationSeconds(project);
    if (endSeconds <= 0.0) {
        endSeconds = 1.0;
    }
    const auto frameCount = static_cast<int64_t>(endSeconds * sampleRate);

    size_t consideredTracks = 0;
    std::vector<StemManifestEntry> exportedStems;
    std::vector<AudioLevelStats> exportedStats;
    std::vector<std::string> missingMediaClipIds;
    std::set<std::string> missingMediaClipIdSet;
    for (const auto& track : project.tracks) {
        if (!isExportableAudioTrack(track)) {
            continue;
        }
        const bool hasTrackClip = std::any_of(project.clips.begin(), project.clips.end(), [&](const ClipState& clip) {
            return clip.trackName == track.name;
        });
        if (!hasTrackClip) {
            continue;
        }
        ++consideredTracks;

        auto stemProject = projectForStemTrack(project, track);
        ProjectAudioRenderPlan plan;
        std::string error;
        if (!makeProjectAudioRenderPlan(stemProject, plan, error)) {
            result.message = error;
            return result;
        }
        plan.loopEnabled = false;
        for (const auto& missingId : plan.missingMediaClipIds) {
            if (missingMediaClipIdSet.insert(missingId).second) {
                missingMediaClipIds.push_back(missingId);
            }
        }

        WavAudioData stemAudio;
        stemAudio.channels = 2;
        stemAudio.sampleRate = sampleRate;
        renderProjectAudioBlock(plan, 0, frameCount, stemAudio.interleavedSamples);

        const auto outputPath = uniqueStemOutputPath(outputDirectory, sanitizedStemName(track.name));
        if (outputPath.empty()) {
            result.message = "Could not find an available stem filename for " + track.name + ".";
            return result;
        }
        const bool wroteWav = writeProjectWavForBitDepth(outputPath, stemAudio, bitDepth, error);
        if (!wroteWav) {
            result.message = error;
            return result;
        }
        exportedStats.push_back(analyzeInterleavedStereoLevels(stemAudio));
        result.outputPaths.push_back(outputPath.generic_string());
        exportedStems.push_back({track, outputPath, clipsForTrack(project, track.name)});
    }

    if (consideredTracks == 0) {
        result.message = "No audio tracks with clips to export.";
        return result;
    }

    const auto manifestPath = outputDirectory / "Neuracoust DAW Stem Export Manifest.json";
    std::string manifestError;
    if (!writeStemExportManifest(manifestPath, outputDirectory, project, exportedStems, exportedStats, missingMediaClipIds, endSeconds, sampleRate, bitDepth, manifestError)) {
        result.message = manifestError;
        return result;
    }

    result.ok = true;
    result.exportedStems = result.outputPaths.size();
    result.durationSeconds = endSeconds;
    result.stemLevelStats = exportedStats;
    result.missingMediaClipIds = missingMediaClipIds;
    result.manifestPath = manifestPath.generic_string();
    const auto nearSilentStems = std::count_if(exportedStats.begin(), exportedStats.end(), [](const AudioLevelStats& stats) {
        return stats.nearSilent;
    });
    const auto clippedStems = std::count_if(exportedStats.begin(), exportedStats.end(), [](const AudioLevelStats& stats) {
        return stats.clippingDetected;
    });
    result.message = "Stem export complete.";
    std::vector<std::string> warnings;
    if (nearSilentStems > 0) {
        warnings.push_back(std::to_string(nearSilentStems) + " near-silent stem(s)");
    }
    if (clippedStems > 0) {
        warnings.push_back(std::to_string(clippedStems) + " clipped stem(s)");
    }
    const auto healthWarnings = projectHealthWarningFragments(analyzeProjectHealth(project), true);
    warnings.insert(warnings.end(), healthWarnings.begin(), healthWarnings.end());
    appendWarningFragments(result.message, warnings);
    return result;
}

} // namespace neuracoust::daw
