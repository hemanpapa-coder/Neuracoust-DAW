#include "ai/AiAssistant.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace neuracoust::daw {
namespace {

std::string jsonEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char ch : text) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += ch; break;
        }
    }
    return out;
}

bool trackExists(const ProjectDocument& project, const std::string& trackName) {
    return std::any_of(project.tracks.begin(), project.tracks.end(), [&](const TrackState& track) {
        return track.name == trackName;
    });
}

bool clipExists(const ProjectDocument& project, const std::string& clipId) {
    return std::any_of(project.clips.begin(), project.clips.end(), [&](const ClipState& clip) {
        return clip.id == clipId;
    });
}

std::string boolText(bool value) {
    return value ? "true" : "false";
}

std::string baseName(const std::filesystem::path& path) {
    return path.filename().string();
}

std::string shortHashText(const std::string& text) {
    const auto value = std::hash<std::string>{}(text);
    std::ostringstream out;
    out << std::hex << value;
    return out.str();
}

std::string nowStampText() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::to_string(std::chrono::duration_cast<std::chrono::seconds>(now).count());
}

bool hasModel(const AiLocalModelInventory& inventory, const std::string& prefix) {
    return std::any_of(inventory.installedModels.begin(), inventory.installedModels.end(), [&](const std::string& model) {
        return model == prefix || model.rfind(prefix + ":", 0) == 0;
    });
}

std::string firstInstalled(const AiLocalModelInventory& inventory, const std::vector<std::string>& candidates) {
    for (const auto& candidate : candidates) {
        auto exact = std::find(inventory.installedModels.begin(), inventory.installedModels.end(), candidate);
        if (exact != inventory.installedModels.end()) {
            return *exact;
        }
        auto prefix = std::find_if(inventory.installedModels.begin(), inventory.installedModels.end(), [&](const std::string& model) {
            return model.rfind(candidate + ":", 0) == 0;
        });
        if (prefix != inventory.installedModels.end()) {
            return *prefix;
        }
    }
    return {};
}

std::vector<int> parseVersionTriplet(const std::string& versionText) {
    std::vector<int> parts;
    std::string current;
    bool started = false;
    for (const char ch : versionText) {
        if (std::isdigit(static_cast<unsigned char>(ch))) {
            current += ch;
            started = true;
        } else if (started && ch == '.') {
            parts.push_back(current.empty() ? 0 : std::stoi(current));
            current.clear();
        } else if (started) {
            break;
        }
    }
    if (started) {
        parts.push_back(current.empty() ? 0 : std::stoi(current));
    }
    while (parts.size() < 3) {
        parts.push_back(0);
    }
    return parts;
}

std::string normalizedModelName(std::string modelName) {
    std::transform(modelName.begin(), modelName.end(), modelName.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return modelName;
}

bool containsToken(const std::string& text, const std::string& token) {
    return normalizedModelName(text).find(normalizedModelName(token)) != std::string::npos;
}

bool hasTag(const std::vector<std::string>& tags, const std::string& wanted) {
    return std::any_of(tags.begin(), tags.end(), [&](const std::string& tag) {
        return normalizedModelName(tag) == normalizedModelName(wanted);
    });
}

} // namespace

std::string aiCommandTypeToString(AiCommandType type) {
    switch (type) {
        case AiCommandType::SetTrackGain: return "set_track_gain";
        case AiCommandType::SetTrackPan: return "set_track_pan";
        case AiCommandType::SetTrackMute: return "set_track_mute";
        case AiCommandType::SetTrackSolo: return "set_track_solo";
        case AiCommandType::ArmTrackForRecording: return "arm_track_for_recording";
        case AiCommandType::AddMarker: return "add_marker";
        case AiCommandType::RequestStemMagic: return "request_stem_magic";
        case AiCommandType::ScheduleRtNeuralTraining: return "schedule_rtneural_training";
        case AiCommandType::ClassifyClipWithYamNet: return "classify_clip_with_yamnet";
        case AiCommandType::Unknown: break;
    }
    return "unknown";
}

AiCommandType aiCommandTypeFromString(const std::string& text) {
    if (text == "set_track_gain") return AiCommandType::SetTrackGain;
    if (text == "set_track_pan") return AiCommandType::SetTrackPan;
    if (text == "set_track_mute") return AiCommandType::SetTrackMute;
    if (text == "set_track_solo") return AiCommandType::SetTrackSolo;
    if (text == "arm_track_for_recording") return AiCommandType::ArmTrackForRecording;
    if (text == "add_marker") return AiCommandType::AddMarker;
    if (text == "request_stem_magic") return AiCommandType::RequestStemMagic;
    if (text == "schedule_rtneural_training") return AiCommandType::ScheduleRtNeuralTraining;
    if (text == "classify_clip_with_yamnet") return AiCommandType::ClassifyClipWithYamNet;
    return AiCommandType::Unknown;
}

AiCommandValidation validateAiCommand(const ProjectDocument& project, const AiCommand& command) {
    AiCommandValidation result;
    result.requiresUserConfirmation = true;
    switch (command.type) {
        case AiCommandType::SetTrackGain:
            if (!trackExists(project, command.targetTrackName)) {
                result.message = "AI command rejected: target track was not found.";
                return result;
            }
            if (command.gainDb < -72.0f || command.gainDb > 18.0f) {
                result.message = "AI command rejected: track gain is outside the safe range.";
                return result;
            }
            result.ok = true;
            result.message = "Preview track gain change before applying.";
            return result;
        case AiCommandType::SetTrackPan:
            if (!trackExists(project, command.targetTrackName)) {
                result.message = "AI command rejected: target track was not found.";
                return result;
            }
            if (command.pan < -1.0f || command.pan > 1.0f) {
                result.message = "AI command rejected: pan is outside the safe range.";
                return result;
            }
            result.ok = true;
            result.message = "Preview track pan change before applying.";
            return result;
        case AiCommandType::SetTrackMute:
        case AiCommandType::SetTrackSolo:
        case AiCommandType::ArmTrackForRecording:
            if (!trackExists(project, command.targetTrackName)) {
                result.message = "AI command rejected: target track was not found.";
                return result;
            }
            result.ok = true;
            result.message = "Preview track state change before applying.";
            return result;
        case AiCommandType::AddMarker:
            if (command.label.empty()) {
                result.message = "AI command rejected: marker label is empty.";
                return result;
            }
            if (command.timeSeconds < 0.0) {
                result.message = "AI command rejected: marker time is negative.";
                return result;
            }
            result.ok = true;
            result.message = "Preview marker creation before applying.";
            return result;
        case AiCommandType::RequestStemMagic:
        case AiCommandType::ClassifyClipWithYamNet:
            if (!clipExists(project, command.targetClipId)) {
                result.message = "AI command rejected: target clip was not found.";
                return result;
            }
            result.ok = true;
            result.runsExternalModel = true;
            result.message = "Queue external AI analysis after user confirmation.";
            return result;
        case AiCommandType::ScheduleRtNeuralTraining:
            if (command.beforeAudioPath.empty() || command.afterAudioPath.empty()) {
                result.message = "AI command rejected: before/after audio paths are required.";
                return result;
            }
            result.ok = true;
            result.modifiesAudioFiles = false;
            result.runsExternalModel = true;
            result.message = "Queue offline RTNeural training from approved before/after audio.";
            return result;
        case AiCommandType::Unknown:
            break;
    }
    result.message = "AI command rejected: unknown command type.";
    return result;
}

std::string serializeAiCommandPreview(const AiCommand& command) {
    std::ostringstream out;
    out << "{"
        << "\"type\":\"" << aiCommandTypeToString(command.type) << "\","
        << "\"targetTrackName\":\"" << jsonEscape(command.targetTrackName) << "\","
        << "\"targetClipId\":\"" << jsonEscape(command.targetClipId) << "\","
        << "\"label\":\"" << jsonEscape(command.label) << "\","
        << "\"gainDb\":" << command.gainDb << ","
        << "\"pan\":" << command.pan << ","
        << "\"timeSeconds\":" << command.timeSeconds << ","
        << "\"enabled\":" << boolText(command.enabled) << ","
        << "\"beforeAudioPath\":\"" << jsonEscape(command.beforeAudioPath.string()) << "\","
        << "\"afterAudioPath\":\"" << jsonEscape(command.afterAudioPath.string()) << "\","
        << "\"reason\":\"" << jsonEscape(command.reason) << "\""
        << "}";
    return out.str();
}

AiProjectSnapshot makeAiProjectSnapshot(const ProjectDocument& project) {
    AiProjectSnapshot snapshot;
    snapshot.projectName = project.name;
    snapshot.sampleRate = project.sampleRate;
    snapshot.tempoBpm = project.tempoBpm;
    snapshot.trackCount = static_cast<int>(project.tracks.size());
    snapshot.clipCount = static_cast<int>(project.clips.size());
    snapshot.masterInsertCount = static_cast<int>(project.masterInserts.size());
    for (const auto& track : project.tracks) {
        if (track.trackType == "audio") {
            ++snapshot.audioTrackCount;
        }
        std::ostringstream line;
        line << track.name << " type=" << track.trackType
             << " gainDb=" << track.volumeDb
             << " pan=" << track.pan
             << " muted=" << boolText(track.muted)
             << " solo=" << boolText(track.solo)
             << " armed=" << boolText(track.recordArmed)
             << " inserts=" << track.inserts.size();
        snapshot.trackSummaries.push_back(line.str());
    }
    for (const auto& clip : project.clips) {
        std::ostringstream line;
        line << clip.id << " track=" << clip.trackName
             << " name=" << (clip.regionName.empty() ? baseName(clip.sourcePath) : clip.regionName)
             << " start=" << clip.startSeconds
             << " duration=" << clip.durationSeconds
             << " gainDb=" << clip.gainDb
             << " source=" << baseName(clip.sourcePath);
        snapshot.clipSummaries.push_back(line.str());
    }
    snapshot.healthSummary = summarizeProjectHealth(project);
    return snapshot;
}

std::string serializeAiProjectSnapshot(const AiProjectSnapshot& snapshot) {
    std::ostringstream out;
    out << "{"
        << "\"projectName\":\"" << jsonEscape(snapshot.projectName) << "\","
        << "\"sampleRate\":" << snapshot.sampleRate << ","
        << "\"tempoBpm\":" << snapshot.tempoBpm << ","
        << "\"trackCount\":" << snapshot.trackCount << ","
        << "\"audioTrackCount\":" << snapshot.audioTrackCount << ","
        << "\"clipCount\":" << snapshot.clipCount << ","
        << "\"masterInsertCount\":" << snapshot.masterInsertCount << ","
        << "\"healthSummary\":\"" << jsonEscape(snapshot.healthSummary) << "\",";
    out << "\"tracks\":[";
    for (size_t i = 0; i < snapshot.trackSummaries.size(); ++i) {
        if (i != 0) out << ",";
        out << "\"" << jsonEscape(snapshot.trackSummaries[i]) << "\"";
    }
    out << "],\"clips\":[";
    for (size_t i = 0; i < snapshot.clipSummaries.size(); ++i) {
        if (i != 0) out << ",";
        out << "\"" << jsonEscape(snapshot.clipSummaries[i]) << "\"";
    }
    out << "]}";
    return out.str();
}

std::string buildAiAssistantSystemPrompt() {
    return "You are the Neuracoust DAW assistant. Return only safe JSON command previews. "
           "Do not claim that audio was changed until the host confirms Apply. "
           "Use project snapshots, audio analysis, mix memory, Stem Magic jobs, YAMNet labels, "
           "and RTNeural offline training jobs as evidence.";
}

std::string buildOllamaChatRequestJson(const AiLocalAssistantConfig& config,
                                       const AiProjectSnapshot& snapshot,
                                       const std::string& userMessage) {
    std::ostringstream out;
    out << "{"
        << "\"model\":\"" << jsonEscape(config.model) << "\","
        << "\"stream\":false,"
        << "\"messages\":["
        << "{\"role\":\"system\",\"content\":\"" << jsonEscape(buildAiAssistantSystemPrompt()) << "\"},"
        << "{\"role\":\"user\",\"content\":\"Project snapshot: " << jsonEscape(serializeAiProjectSnapshot(snapshot))
        << "\\nUser request: " << jsonEscape(userMessage) << "\"}"
        << "]}";
    return out.str();
}

bool ollamaVersionAtLeast(const std::string& versionText, int major, int minor, int patch) {
    const auto parts = parseVersionTriplet(versionText);
    if (parts[0] != major) return parts[0] > major;
    if (parts[1] != minor) return parts[1] > minor;
    return parts[2] >= patch;
}

AiLocalModelRecommendation recommendLocalAiModels(const AiLocalModelInventory& inventory,
                                                  bool appleSilicon) {
    AiLocalModelRecommendation recommendation;
    recommendation.interactiveModel = firstInstalled(inventory, {
        "gemma4",
        "qwen3.6:27b-mlx",
        "qwen3.6:27b",
        "mistral-small3.2:24b",
        "gpt-oss:20b",
        "qwen3:14b",
        "llama3.1:8b"
    });
    recommendation.codingModel = firstInstalled(inventory, {
        "qwen3-coder:30b",
        "qwen2.5-coder:14b",
        "gemma4",
        "qwen3.6:27b-mlx"
    });
    recommendation.reasoningModel = firstInstalled(inventory, {
        "magistral:24b",
        "deepseek-r1:14b",
        "phi4-mini-reasoning",
        "gpt-oss:20b"
    });
    recommendation.lightweightModel = firstInstalled(inventory, {
        "qwen2.5-coder:1.5b",
        "gemma3n:e4b",
        "llama3.1:8b"
    });
    recommendation.embeddingModel = firstInstalled(inventory, {
        "nomic-embed-text"
    });

    if (recommendation.interactiveModel.empty()) {
        recommendation.installSuggestions.push_back("ollama pull gemma4:12b");
    }
    if (!hasModel(inventory, "gemma4")) {
        recommendation.installSuggestions.push_back(appleSilicon ? "ollama pull gemma4:12b" : "ollama pull gemma4:e4b");
    }
    if (!hasModel(inventory, "nomic-embed-text")) {
        recommendation.installSuggestions.push_back("ollama pull nomic-embed-text");
    }

    const bool gemma4MtpReady = ollamaVersionAtLeast(inventory.ollamaVersion, 0, 31, 0);
    if (appleSilicon && !gemma4MtpReady) {
        recommendation.upgradeSuggestions.push_back("Upgrade Ollama to 0.31 or newer before evaluating Gemma 4 speed.");
    }
    if (hasModel(inventory, "gemma3") && !hasModel(inventory, "gemma4")) {
        recommendation.upgradeSuggestions.push_back("Keep Gemma 3n for lightweight tasks, but add Gemma 4 for DAW agent workflows.");
    }

    std::ostringstream summary;
    summary << "Interactive=" << (recommendation.interactiveModel.empty() ? "none" : recommendation.interactiveModel)
            << "; coding=" << (recommendation.codingModel.empty() ? "none" : recommendation.codingModel)
            << "; reasoning=" << (recommendation.reasoningModel.empty() ? "none" : recommendation.reasoningModel)
            << "; lightweight=" << (recommendation.lightweightModel.empty() ? "none" : recommendation.lightweightModel);
    recommendation.summary = summary.str();
    return recommendation;
}

std::string aiModelTierToString(AiModelTier tier) {
    switch (tier) {
        case AiModelTier::Light: return "light";
        case AiModelTier::Standard: return "standard";
        case AiModelTier::Heavy: return "heavy";
        case AiModelTier::RealtimeOnly: return "realtime_only";
        case AiModelTier::Unknown: break;
    }
    return "unknown";
}

std::string aiExecutionDecisionToString(AiExecutionDecision decision) {
    switch (decision) {
        case AiExecutionDecision::AllowNow: return "allow_now";
        case AiExecutionDecision::QueueUntilIdle: return "queue_until_idle";
        case AiExecutionDecision::Block: return "block";
    }
    return "block";
}

std::string aiIssueSeverityToString(AiIssueSeverity severity) {
    switch (severity) {
        case AiIssueSeverity::Info: return "info";
        case AiIssueSeverity::Warning: return "warning";
        case AiIssueSeverity::Critical: return "critical";
    }
    return "info";
}

std::string aiAnalysisTargetTypeToString(AiAnalysisTargetType type) {
    switch (type) {
        case AiAnalysisTargetType::ProjectMix: return "project_mix";
        case AiAnalysisTargetType::MasterOutput: return "master_output";
        case AiAnalysisTargetType::TrackOutput: return "track_output";
        case AiAnalysisTargetType::ClipSource: return "clip_source";
    }
    return "project_mix";
}

std::string aiAnalysisAudioPathToString(AiAnalysisAudioPath path) {
    switch (path) {
        case AiAnalysisAudioPath::MasterOutput: return "master_output";
        case AiAnalysisAudioPath::TrackOutput: return "track_output";
        case AiAnalysisAudioPath::ClipSource: return "clip_source";
        case AiAnalysisAudioPath::RenderedSelection: return "rendered_selection";
    }
    return "master_output";
}

std::string aiAnalysisJobKindToString(AiAnalysisJobKind kind) {
    switch (kind) {
        case AiAnalysisJobKind::MixDiagnostic: return "mix_diagnostic";
        case AiAnalysisJobKind::SoundClassification: return "sound_classification";
        case AiAnalysisJobKind::Transcription: return "transcription";
        case AiAnalysisJobKind::LyricTranscription: return "lyric_transcription";
        case AiAnalysisJobKind::StemSeparation: return "stem_separation";
        case AiAnalysisJobKind::Denoise: return "denoise";
        case AiAnalysisJobKind::AudioEmbedding: return "audio_embedding";
    }
    return "mix_diagnostic";
}

std::string aiAnalysisJobStatusToString(AiAnalysisJobStatus status) {
    switch (status) {
        case AiAnalysisJobStatus::Queued: return "queued";
        case AiAnalysisJobStatus::Running: return "running";
        case AiAnalysisJobStatus::Completed: return "completed";
        case AiAnalysisJobStatus::Failed: return "failed";
        case AiAnalysisJobStatus::Cancelled: return "cancelled";
    }
    return "queued";
}

std::string aiRuntimeBackendToString(AiRuntimeBackend backend) {
    switch (backend) {
        case AiRuntimeBackend::CoreML: return "coreml";
        case AiRuntimeBackend::WindowsML: return "windows_ml";
        case AiRuntimeBackend::OnnxRuntimeDirectML: return "onnxruntime_directml";
        case AiRuntimeBackend::OnnxRuntimeCPU: return "onnxruntime_cpu";
        case AiRuntimeBackend::WhisperCppCUDA: return "whisper_cpp_cuda";
        case AiRuntimeBackend::CudaBatch: return "cuda_batch";
        case AiRuntimeBackend::OpenVINOCpu: return "openvino_cpu";
        case AiRuntimeBackend::RtNeuralRealtime: return "rtneural_realtime";
        case AiRuntimeBackend::ExternalSidecar: return "external_sidecar";
    }
    return "onnxruntime_cpu";
}

std::string aiGpuVendorToString(AiGpuVendor vendor) {
    switch (vendor) {
        case AiGpuVendor::Apple: return "apple";
        case AiGpuVendor::Nvidia: return "nvidia";
        case AiGpuVendor::Amd: return "amd";
        case AiGpuVendor::Intel: return "intel";
        case AiGpuVendor::Qualcomm: return "qualcomm";
        case AiGpuVendor::CpuOnly: return "cpu_only";
        case AiGpuVendor::Unknown: break;
    }
    return "unknown";
}

std::string aiGpuGenerationToString(AiGpuGeneration generation) {
    switch (generation) {
        case AiGpuGeneration::AppleSilicon: return "apple_silicon";
        case AiGpuGeneration::NvidiaPascal: return "nvidia_pascal";
        case AiGpuGeneration::NvidiaTuringOrNewer: return "nvidia_turing_or_newer";
        case AiGpuGeneration::AmdGcnOrPolaris: return "amd_gcn_or_polaris";
        case AiGpuGeneration::AmdRdnaOrNewer: return "amd_rdna_or_newer";
        case AiGpuGeneration::IntelXeOrNewer: return "intel_xe_or_newer";
        case AiGpuGeneration::QualcommNpu: return "qualcomm_npu";
        case AiGpuGeneration::Unknown: break;
    }
    return "unknown";
}

AiModelTier classifyAiModelTier(const std::string& modelName) {
    const auto model = normalizedModelName(modelName);
    if (model.find("rtneural") != std::string::npos || model.find("frozen") != std::string::npos) {
        return AiModelTier::RealtimeOnly;
    }
    if (model.find("qwen3.6:27b") != std::string::npos ||
        model.find("qwen3-coder:30b") != std::string::npos ||
        model.find("mistral-small3.2:24b") != std::string::npos ||
        model.find("magistral:24b") != std::string::npos ||
        model.find("deepseek-r1:14b") != std::string::npos ||
        model.find("gpt-oss:20b") != std::string::npos ||
        model.find(":24b") != std::string::npos ||
        model.find(":27b") != std::string::npos ||
        model.find(":30b") != std::string::npos ||
        model.find(":31b") != std::string::npos) {
        return AiModelTier::Heavy;
    }
    if (model.find("gemma4") != std::string::npos ||
        model.find("gemma3:12b") != std::string::npos ||
        model.find("qwen3:14b") != std::string::npos ||
        model.find("qwen2.5-coder:14b") != std::string::npos ||
        model.find(":12b") != std::string::npos ||
        model.find(":14b") != std::string::npos) {
        return AiModelTier::Standard;
    }
    if (model.find("gemma3n:e4b") != std::string::npos ||
        model.find("llama3.1:8b") != std::string::npos ||
        model.find("qwen2.5-coder:1.5b") != std::string::npos ||
        model.find("phi4-mini") != std::string::npos ||
        model.find(":1.5b") != std::string::npos ||
        model.find(":4b") != std::string::npos ||
        model.find(":8b") != std::string::npos) {
        return AiModelTier::Light;
    }
    return AiModelTier::Unknown;
}

AiExecutionGuardResult evaluateAiExecutionGuard(const std::string& modelName,
                                                const AiRuntimeState& runtime,
                                                const AiExecutionPolicy& policy) {
    AiExecutionGuardResult result;
    result.modelTier = classifyAiModelTier(modelName);
    result.shouldUnloadAfterUse = result.modelTier != AiModelTier::Light &&
        result.modelTier != AiModelTier::RealtimeOnly;
    result.recommendedKeepAliveSeconds = result.shouldUnloadAfterUse ? 0 : 300;
    result.recommendedNumPredict = result.modelTier == AiModelTier::Light ? 128 : 256;

    if (result.modelTier == AiModelTier::Unknown) {
        result.decision = AiExecutionDecision::QueueUntilIdle;
        result.message = "Unknown AI model tier; queue until the DAW is idle.";
        return result;
    }
    if (result.modelTier == AiModelTier::RealtimeOnly) {
        result.decision = AiExecutionDecision::AllowNow;
        result.shouldUnloadAfterUse = false;
        result.recommendedKeepAliveSeconds = 0;
        result.recommendedNumPredict = 0;
        result.message = "Realtime-only frozen model is allowed on the audio path.";
        return result;
    }
    if (!policy.safeModeEnabled) {
        result.decision = AiExecutionDecision::AllowNow;
        result.message = "AI Safe Mode is disabled; allow local AI immediately.";
        return result;
    }
    if (runtime.recordingActive) {
        result.decision = AiExecutionDecision::Block;
        result.message = "Recording is active; local LLM execution is blocked to protect audio capture.";
        return result;
    }
    if (runtime.offlineBounceActive && result.modelTier != AiModelTier::Light) {
        result.decision = AiExecutionDecision::QueueUntilIdle;
        result.message = "Bounce/export is active; queue standard or heavy AI until idle.";
        return result;
    }
    if ((runtime.transportRunning || runtime.realtimeAudioEngineActive) &&
        result.modelTier != AiModelTier::Light) {
        result.decision = AiExecutionDecision::QueueUntilIdle;
        result.message = "Playback or realtime audio is active; queue standard/heavy AI until idle.";
        return result;
    }
    if (runtime.memoryPressure) {
        result.decision = result.modelTier == AiModelTier::Light ? AiExecutionDecision::QueueUntilIdle : AiExecutionDecision::Block;
        result.message = result.modelTier == AiModelTier::Light
            ? "System memory pressure is active; queue light AI until idle."
            : "System memory pressure is active; block standard/heavy AI.";
        return result;
    }
    if (runtime.availableRamGb > 0.0) {
        const double required = result.modelTier == AiModelTier::Heavy
            ? policy.minimumRamGbForHeavy
            : (result.modelTier == AiModelTier::Standard ? policy.minimumRamGbForStandard : 0.0);
        if (required > 0.0 && runtime.availableRamGb < required) {
            result.decision = AiExecutionDecision::QueueUntilIdle;
            result.message = "Available RAM is below the selected model tier threshold; queue AI until resources recover.";
            return result;
        }
    }
    if (runtime.availableGpuMemoryGb > 0.0) {
        const double required = result.modelTier == AiModelTier::Heavy
            ? policy.minimumGpuGbForHeavy
            : (result.modelTier == AiModelTier::Standard ? policy.minimumGpuGbForStandard : 0.0);
        if (required > 0.0 && runtime.availableGpuMemoryGb < required) {
            result.decision = AiExecutionDecision::QueueUntilIdle;
            result.message = "Available GPU memory is below the selected model tier threshold; queue AI until resources recover.";
            return result;
        }
    }
    result.decision = AiExecutionDecision::AllowNow;
    result.message = "Local AI is allowed under the current DAW safety policy.";
    return result;
}

bool aiHardwareHasModernGpuAcceleration(const AiHardwareProfile& hardware) {
    if (hardware.supportsAppleNeuralEngine || hardware.supportsNpu) {
        return true;
    }
    if (hardware.gpuVendor == AiGpuVendor::Nvidia) {
        return hardware.hasTensorCores ||
            hardware.gpuGeneration == AiGpuGeneration::NvidiaTuringOrNewer ||
            hardware.cudaComputeCapabilityMajor >= 7;
    }
    if (hardware.gpuVendor == AiGpuVendor::Amd) {
        return hardware.gpuGeneration == AiGpuGeneration::AmdRdnaOrNewer &&
            (hardware.supportsWindowsML || hardware.supportsDirectML);
    }
    if (hardware.gpuVendor == AiGpuVendor::Intel) {
        return hardware.gpuGeneration == AiGpuGeneration::IntelXeOrNewer &&
            (hardware.supportsWindowsML || hardware.supportsDirectML);
    }
    if (hardware.gpuVendor == AiGpuVendor::Qualcomm) {
        return hardware.supportsWindowsML || hardware.supportsNpu;
    }
    return false;
}

std::vector<AiFeatureBackendPlan> recommendAiFeatureBackendPlan(const AiHardwareProfile& hardware) {
    const bool macCoreMl = hardware.platform == AiHostPlatform::MacOS &&
        (hardware.supportsCoreML || hardware.supportsAppleNeuralEngine);
    const bool windowsMl = hardware.platform == AiHostPlatform::Windows && hardware.supportsWindowsML;
    const bool directMl = hardware.platform == AiHostPlatform::Windows &&
        (hardware.supportsDirectML || hardware.supportsDirectX12);
    const bool nvidiaCuda = hardware.gpuVendor == AiGpuVendor::Nvidia && hardware.supportsCuda;
    const bool pascalCuda = nvidiaCuda &&
        (hardware.gpuGeneration == AiGpuGeneration::NvidiaPascal ||
         (hardware.cudaComputeCapabilityMajor == 6 && hardware.cudaComputeCapabilityMinor >= 0));
    const bool modernGpu = aiHardwareHasModernGpuAcceleration(hardware);
    const bool vram6GbOrMore = hardware.gpuMemoryGb >= 6.0 || hardware.gpuMemoryGb <= 0.0;
    const AiRuntimeBackend genericWindowsGpu = windowsMl
        ? AiRuntimeBackend::WindowsML
        : (directMl ? AiRuntimeBackend::OnnxRuntimeDirectML : AiRuntimeBackend::OnnxRuntimeCPU);
    const AiRuntimeBackend genericGpu = macCoreMl ? AiRuntimeBackend::CoreML : genericWindowsGpu;

    std::vector<AiFeatureBackendPlan> plans;

    plans.push_back({AiAnalysisJobKind::MixDiagnostic,
                     AiRuntimeBackend::OnnxRuntimeCPU,
                     AiRuntimeBackend::OnnxRuntimeCPU,
                     AiModelTier::Light,
                     false,
                     false,
                     "silero-vad-plus-daw-features",
                     "Run VAD, strip-silence candidates, and mix diagnostics on CPU or a tiny ONNX model; keep this off the audio callback."});

    AiFeatureBackendPlan transcription;
    transcription.feature = AiAnalysisJobKind::Transcription;
    transcription.fallbackBackend = AiRuntimeBackend::OnnxRuntimeCPU;
    transcription.modelTier = pascalCuda ? AiModelTier::Standard : (modernGpu ? AiModelTier::Standard : AiModelTier::Light);
    transcription.realtimeSafe = false;
    transcription.offlineOnly = true;
    transcription.modelFamily = pascalCuda ? "whisper.cpp tiny/base/small cuda" : "whisper tiny/base/small";
    if (macCoreMl) {
        transcription.primaryBackend = AiRuntimeBackend::CoreML;
        transcription.note = "Use CoreML/ANE where available; queue transcription as a background job.";
    } else if (nvidiaCuda && vram6GbOrMore) {
        transcription.primaryBackend = AiRuntimeBackend::WhisperCppCUDA;
        transcription.note = pascalCuda
            ? "GTX 10-series path: prefer FP32/INT8-friendly whisper.cpp CUDA models; avoid assuming fast FP16 or TensorRT."
            : "NVIDIA CUDA path: use CUDA for Whisper and allow larger models when VRAM and Safe Mode permit.";
    } else {
        transcription.primaryBackend = genericGpu;
        transcription.note = "Use Windows ML/DirectML for ONNX transcription models when available, then CPU fallback.";
    }
    plans.push_back(transcription);

    plans.push_back({AiAnalysisJobKind::SoundClassification,
                     genericGpu,
                     AiRuntimeBackend::OnnxRuntimeCPU,
                     AiModelTier::Light,
                     false,
                     false,
                     "small-cnn-crnn-or-yamnet-onnx",
                     "Best first Windows feature: small ONNX classifier works on AMD/NVIDIA/Intel through Windows ML or DirectML and falls back cleanly to CPU."});

    AiFeatureBackendPlan denoise;
    denoise.feature = AiAnalysisJobKind::Denoise;
    denoise.primaryBackend = genericGpu;
    denoise.fallbackBackend = AiRuntimeBackend::OnnxRuntimeCPU;
    denoise.modelTier = modernGpu ? AiModelTier::Standard : AiModelTier::Light;
    denoise.realtimeSafe = false;
    denoise.offlineOnly = !modernGpu;
    denoise.modelFamily = "deepfilternet-small-onnx";
    denoise.note = modernGpu
        ? "Allow preview/background cleanup with conservative buffer scheduling; do not execute model inference on the realtime render thread."
        : "Use small CPU models or offline cleanup on older GPUs to avoid underruns.";
    plans.push_back(denoise);

    AiFeatureBackendPlan stems;
    stems.feature = AiAnalysisJobKind::StemSeparation;
    stems.primaryBackend = nvidiaCuda ? AiRuntimeBackend::CudaBatch : genericGpu;
    stems.fallbackBackend = AiRuntimeBackend::OnnxRuntimeCPU;
    stems.modelTier = AiModelTier::Heavy;
    stems.realtimeSafe = false;
    stems.offlineOnly = true;
    stems.modelFamily = nvidiaCuda ? "demucs-cuda-chunked" : "demucs-or-scnet-onnx-chunked";
    stems.note = nvidiaCuda && pascalCuda
        ? "GTX 1060 class: offline only, reduce segment/chunk size, batch=1, shifts low, and expect CPU fallback for oversized jobs."
        : "Offline stem separation; newer NVIDIA GPUs can exploit CUDA/Tensor Cores, AMD/Intel use Windows ML/DirectML where model coverage allows.";
    plans.push_back(stems);

    plans.push_back({AiAnalysisJobKind::AudioEmbedding,
                     genericGpu,
                     AiRuntimeBackend::OnnxRuntimeCPU,
                     modernGpu ? AiModelTier::Standard : AiModelTier::Light,
                     false,
                     true,
                     "clap-or-audio-embedding-onnx",
                     "Run sample-browser embeddings as a background indexer; AMD and newer Windows devices should use Windows ML/DirectML before CPU fallback."});

    return plans;
}

AiAudioFeatureSummary analyzeAudioForAi(const WavAudioData& audio) {
    AiAudioFeatureSummary summary;
    summary.channels = audio.channels;
    summary.sampleRate = audio.sampleRate;
    if (audio.channels <= 0 || audio.sampleRate <= 0 || audio.interleavedSamples.empty()) {
        summary.message = "Audio analysis skipped: missing samples or invalid format.";
        return summary;
    }
    const size_t frames = audio.interleavedSamples.size() / static_cast<size_t>(audio.channels);
    if (frames == 0) {
        summary.message = "Audio analysis skipped: no complete frames.";
        return summary;
    }
    double sumSquares = 0.0;
    for (const float sample : audio.interleavedSamples) {
        summary.peak = std::max(summary.peak, std::abs(sample));
        sumSquares += static_cast<double>(sample) * static_cast<double>(sample);
    }
    summary.rms = static_cast<float>(std::sqrt(sumSquares / static_cast<double>(audio.interleavedSamples.size())));
    summary.durationSeconds = static_cast<double>(frames) / static_cast<double>(audio.sampleRate);

    const size_t analysisFrames = std::min<size_t>(frames, 4096);
    double weighted = 0.0;
    double magnitudeSum = 0.0;
    for (size_t bin = 1; bin < analysisFrames / 2; ++bin) {
        double real = 0.0;
        double imag = 0.0;
        for (size_t frame = 0; frame < analysisFrames; ++frame) {
            double mono = 0.0;
            for (int ch = 0; ch < audio.channels; ++ch) {
                mono += audio.interleavedSamples[frame * static_cast<size_t>(audio.channels) + static_cast<size_t>(ch)];
            }
            mono /= static_cast<double>(audio.channels);
            const double phase = -2.0 * 3.14159265358979323846 * static_cast<double>(bin * frame) / static_cast<double>(analysisFrames);
            real += mono * std::cos(phase);
            imag += mono * std::sin(phase);
        }
        const double magnitude = std::sqrt(real * real + imag * imag);
        const double hz = static_cast<double>(bin) * static_cast<double>(audio.sampleRate) / static_cast<double>(analysisFrames);
        weighted += hz * magnitude;
        magnitudeSum += magnitude;
    }
    if (magnitudeSum > 0.0) {
        summary.spectralCentroidHz = static_cast<float>(weighted / magnitudeSum);
    }
    summary.crestFactor = summary.rms > 0.000001f ? summary.peak / summary.rms : 0.0f;
    summary.likelyClipped = summary.peak >= 0.995f && summary.crestFactor < 6.0f;
    summary.veryQuiet = summary.peak < 0.08f || summary.rms < 0.01f;
    summary.lowFrequencyHeavy = summary.spectralCentroidHz > 0.0f && summary.spectralCentroidHz < 180.0f;
    summary.highFrequencyHeavy = summary.spectralCentroidHz > 5500.0f;
    summary.ok = true;
    summary.message = "Audio analysis complete.";
    return summary;
}

std::vector<std::string> suggestTrackTagsFromYamNetLabels(const std::vector<std::string>& labels) {
    std::vector<std::string> tags;
    auto addIf = [&](const std::string& tag) {
        if (std::find(tags.begin(), tags.end(), tag) == tags.end()) {
            tags.push_back(tag);
        }
    };
    for (auto label : labels) {
        std::transform(label.begin(), label.end(), label.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (label.find("sing") != std::string::npos || label.find("speech") != std::string::npos ||
            label.find("voice") != std::string::npos) {
            addIf("vocal");
        }
        if (label.find("drum") != std::string::npos || label.find("snare") != std::string::npos ||
            label.find("cymbal") != std::string::npos) {
            addIf("drums");
        }
        if (label.find("guitar") != std::string::npos) addIf("guitar");
        if (label.find("piano") != std::string::npos || label.find("keyboard") != std::string::npos) addIf("keys");
        if (label.find("bass") != std::string::npos) addIf("bass");
        if (label.find("noise") != std::string::npos || label.find("hum") != std::string::npos) addIf("cleanup");
    }
    return tags;
}

std::string inferTrackRoleForAi(const std::string& trackName,
                                const std::vector<std::string>& tags) {
    const std::string name = normalizedModelName(trackName);
    if (hasTag(tags, "vocal") || containsToken(name, "vocal") || containsToken(name, "vox") ||
        containsToken(name, "voice") || containsToken(name, "lead")) {
        return "vocal";
    }
    if (hasTag(tags, "drums") || containsToken(name, "drum") || containsToken(name, "kick") ||
        containsToken(name, "snare") || containsToken(name, "hat")) {
        return "drums";
    }
    if (hasTag(tags, "bass") || containsToken(name, "bass") || containsToken(name, "808")) {
        return "bass";
    }
    if (hasTag(tags, "guitar") || containsToken(name, "gtr") || containsToken(name, "guitar")) {
        return "guitar";
    }
    if (hasTag(tags, "keys") || containsToken(name, "piano") || containsToken(name, "keys") ||
        containsToken(name, "synth")) {
        return "keys";
    }
    if (hasTag(tags, "cleanup") || containsToken(name, "noise") || containsToken(name, "room")) {
        return "cleanup";
    }
    return "general";
}

std::vector<AiMixDiagnosticIssue> diagnoseTrackForAi(const std::string& targetName,
                                                     const AiAudioFeatureSummary& features,
                                                     const std::vector<std::string>& tags) {
    std::vector<AiMixDiagnosticIssue> issues;
    if (!features.ok) {
        issues.push_back({AiIssueSeverity::Warning,
                          "analysis",
                          targetName,
                          "오디오 분석을 사용할 수 없습니다.",
                          "믹스 결정을 요청하기 전에 오프라인 클립 분석 작업을 먼저 실행하세요."});
        return issues;
    }
    if (features.likelyClipped) {
        issues.push_back({AiIssueSeverity::Critical,
                          "level",
                          targetName,
                          "The signal appears clipped or very close to full scale.",
                          "Lower clip gain or input gain, then re-record or repair before compression."});
    } else if (features.peak > 0.92f) {
        issues.push_back({AiIssueSeverity::Warning,
                          "headroom",
                          targetName,
                          "The signal has very little peak headroom.",
                          "Trim the clip or track down by 3-6 dB before adding processors."});
    }
    if (features.veryQuiet) {
        issues.push_back({AiIssueSeverity::Warning,
                          "level",
                          targetName,
                          "The signal is very quiet.",
                          "Normalize the clip or raise clip gain before detailed mixing."});
    }
    const std::string role = inferTrackRoleForAi(targetName, tags);
    if (features.lowFrequencyHeavy && role != "bass" && role != "drums") {
        issues.push_back({AiIssueSeverity::Info,
                          "spectrum",
                          targetName,
                          "Low-frequency energy is dominant for this track role.",
                          "Consider a high-pass filter or subtractive EQ before compression."});
    }
    if (features.highFrequencyHeavy && role == "vocal") {
        issues.push_back({AiIssueSeverity::Info,
                          "spectrum",
                          targetName,
                          "The vocal looks bright and may need sibilance control.",
                          "Audition a de-esser before adding broad top-end EQ."});
    }
    return issues;
}

AiPluginChainSuggestion suggestPluginChainForAi(const std::string& trackName,
                                                const AiAudioFeatureSummary& features,
                                                const std::vector<std::string>& tags) {
    AiPluginChainSuggestion suggestion;
    suggestion.targetTrackName = trackName;
    suggestion.inferredRole = inferTrackRoleForAi(trackName, tags);
    if (suggestion.inferredRole == "vocal") {
        suggestion.processors = {"Subtractive EQ", "De-esser", "Compressor", "Saturation", "Reverb send"};
        suggestion.rationale = "Vocal chains usually need cleanup, dynamics, presence, and a controlled space send.";
    } else if (suggestion.inferredRole == "drums") {
        suggestion.processors = {"Transient shaper", "Bus compressor", "Saturation", "Parallel compression send"};
        suggestion.rationale = "Drum tracks benefit from transient control and parallel density.";
    } else if (suggestion.inferredRole == "bass") {
        suggestion.processors = {"Low cleanup EQ", "Compressor", "Saturation", "Sidechain candidate"};
        suggestion.rationale = "Bass chains should stabilize low end and expose harmonics on small speakers.";
    } else if (suggestion.inferredRole == "guitar") {
        suggestion.processors = {"High-pass EQ", "Tone EQ", "Compressor", "Room send"};
        suggestion.rationale = "Guitar chains usually need range control and contextual space.";
    } else if (suggestion.inferredRole == "keys") {
        suggestion.processors = {"High-pass EQ", "Stereo width check", "Gentle compressor", "Delay or reverb send"};
        suggestion.rationale = "Keys often need space and width management more than heavy dynamics.";
    } else if (suggestion.inferredRole == "cleanup") {
        suggestion.processors = {"Noise reduction candidate", "High-pass EQ", "Manual edit review"};
        suggestion.rationale = "Cleanup material should be repaired before musical processing.";
    } else {
        suggestion.processors = {"Gain trim", "Subtractive EQ", "Compressor", "Ambience send"};
        suggestion.rationale = "A conservative general chain keeps AI changes reviewable.";
    }
    if (features.likelyClipped) {
        suggestion.processors.insert(suggestion.processors.begin(), "Clip repair or re-record review");
    } else if (features.veryQuiet) {
        suggestion.processors.insert(suggestion.processors.begin(), "Normalize or clip gain");
    }
    return suggestion;
}

std::vector<AiEditSuggestion> suggestEditActionsForAi(const std::string& targetName,
                                                      const AiAudioFeatureSummary& features,
                                                      const std::vector<std::string>& tags) {
    std::vector<AiEditSuggestion> suggestions;
    if (!features.ok) {
        return suggestions;
    }
    if (features.veryQuiet) {
        suggestions.push_back({"normalize_clip", targetName, "The source level is low enough that editing and metering may be unreliable.", true});
    }
    if (features.likelyClipped) {
        suggestions.push_back({"clip_repair_review", targetName, "The waveform appears clipped or near full scale.", true});
    }
    if (hasTag(tags, "cleanup") || inferTrackRoleForAi(targetName, tags) == "cleanup") {
        suggestions.push_back({"strip_silence_candidate", targetName, "The clip was tagged as cleanup/noise material.", true});
    }
    if (features.durationSeconds > 0.2) {
        suggestions.push_back({"add_short_fades", targetName, "Short edge fades can reduce clicks after edits.", true});
    }
    return suggestions;
}

std::vector<AiRecordingCoachMessage> makeRecordingCoachMessages(const AiAudioFeatureSummary& liveInputFeatures,
                                                                float livePeak,
                                                                float estimatedNoiseFloorRms) {
    std::vector<AiRecordingCoachMessage> messages;
    if (livePeak >= 0.98f || liveInputFeatures.likelyClipped) {
        messages.push_back({AiIssueSeverity::Critical,
                            "Input is clipping.",
                            "Lower preamp or interface gain before recording another take."});
    } else if (livePeak > 0.85f) {
        messages.push_back({AiIssueSeverity::Warning,
                            "Input is close to clipping.",
                            "Leave more headroom for louder phrases."});
    } else if (livePeak > 0.0f && livePeak < 0.08f) {
        messages.push_back({AiIssueSeverity::Warning,
                            "Input level is very low.",
                            "Move closer to the mic or raise preamp gain carefully."});
    }
    if (estimatedNoiseFloorRms > 0.03f) {
        messages.push_back({AiIssueSeverity::Warning,
                            "Room or electrical noise looks high.",
                            "Check fan noise, grounding, and mic position before continuing."});
    }
    if (messages.empty()) {
        messages.push_back({AiIssueSeverity::Info,
                            "Recording input looks healthy.",
                            "Keep the same gain staging and mic distance."});
    }
    return messages;
}

std::vector<AiMixMemorySearchResult> searchMixMemoryEntries(const std::vector<AiMixMemoryEntry>& entries,
                                                            const std::vector<std::string>& queryTags,
                                                            const std::string& queryText,
                                                            size_t limit) {
    std::vector<AiMixMemorySearchResult> results;
    const std::string normalizedQuery = normalizedModelName(queryText);
    for (const auto& entry : entries) {
        int score = 0;
        for (const auto& queryTag : queryTags) {
            if (hasTag(entry.tags, queryTag)) {
                score += 4;
            }
        }
        const std::string haystack = normalizedModelName(entry.projectName + " " +
                                                        entry.sourceDescription + " " +
                                                        entry.userFeedback);
        if (!normalizedQuery.empty() && haystack.find(normalizedQuery) != std::string::npos) {
            score += 3;
        }
        if (score <= 0) {
            continue;
        }
        results.push_back({entry.id,
                           score,
                           entry.projectName + ": " + entry.sourceDescription});
    }
    std::sort(results.begin(), results.end(), [](const auto& left, const auto& right) {
        if (left.score == right.score) {
            return left.id < right.id;
        }
        return left.score > right.score;
    });
    if (results.size() > limit) {
        results.resize(limit);
    }
    return results;
}

AiExternalJobRequest makeYamNetClassificationJob(const ClipState& clip,
                                                 const std::filesystem::path& outputDirectory) {
    AiExternalJobRequest job;
    job.engine = "yamnet";
    job.mode = "audioset_clip_classification";
    job.inputPath = clip.sourcePath;
    job.outputDirectory = outputDirectory;
    job.prompt = "Classify this clip with YAMNet/AudioSet labels and return top labels, confidence, and DAW tags.";
    return job;
}

AiExternalJobRequest makeVadSegmentationJob(const ClipState& clip,
                                            const std::filesystem::path& outputDirectory) {
    AiExternalJobRequest job;
    job.engine = "silero_vad";
    job.mode = "speech_silence_segments_onnx_cpu";
    job.inputPath = clip.sourcePath;
    job.outputDirectory = outputDirectory;
    job.prompt = "Detect speech, silence, and likely editable gaps. Return time ranges only; do not modify audio.";
    return job;
}

AiExternalJobRequest makeWhisperTranscriptionJob(const ClipState& clip,
                                                 const AiFeatureBackendPlan& backendPlan,
                                                 const std::filesystem::path& outputDirectory) {
    AiExternalJobRequest job;
    job.engine = "whisper";
    job.mode = aiRuntimeBackendToString(backendPlan.primaryBackend);
    job.inputPath = clip.sourcePath;
    job.outputDirectory = outputDirectory;
    job.prompt = "Transcribe spoken or sung words with word/segment timestamps using " +
        backendPlan.modelFamily + ". Backend note: " + backendPlan.note;
    return job;
}

AiExternalJobRequest makeDenoiseCleanupJob(const ClipState& clip,
                                           const AiFeatureBackendPlan& backendPlan,
                                           const std::filesystem::path& outputDirectory) {
    AiExternalJobRequest job;
    job.engine = "voice_clean";
    job.mode = aiRuntimeBackendToString(backendPlan.primaryBackend);
    job.inputPath = clip.sourcePath;
    job.outputDirectory = outputDirectory;
    job.prompt = "Render a cleanup preview and metrics only. Keep the original clip unchanged until the user applies the result. Backend note: " +
        backendPlan.note;
    return job;
}

AiExternalJobRequest makeAudioEmbeddingIndexJob(const ClipState& clip,
                                                const AiFeatureBackendPlan& backendPlan,
                                                const std::filesystem::path& outputDirectory) {
    AiExternalJobRequest job;
    job.engine = "audio_embedding";
    job.mode = aiRuntimeBackendToString(backendPlan.primaryBackend);
    job.inputPath = clip.sourcePath;
    job.outputDirectory = outputDirectory;
    job.prompt = "Create searchable audio embeddings and compact tags for the sample browser. Backend note: " +
        backendPlan.note;
    return job;
}

AiExternalJobRequest makeRtNeuralTrainingJob(const std::filesystem::path& beforeAudioPath,
                                             const std::filesystem::path& afterAudioPath,
                                             const std::filesystem::path& outputDirectory) {
    AiExternalJobRequest job;
    job.engine = "rtneural";
    job.mode = "offline_before_after_effect_training";
    job.inputPath = beforeAudioPath;
    job.referencePath = afterAudioPath;
    job.outputDirectory = outputDirectory;
    job.prompt = "Train an offline before/after audio effect model, then export frozen weights for realtime RTNeural inference.";
    return job;
}

AiExternalJobRequest makeStemMagicAssistantJob(const ClipState& clip,
                                               StemMagicJobMode mode,
                                               const std::filesystem::path& outputDirectory) {
    AiExternalJobRequest job;
    job.engine = "stem_magic";
    job.mode = mode == StemMagicJobMode::DrumSplit ? "drum_split" : "four_stem";
    job.inputPath = clip.sourcePath;
    job.outputDirectory = outputDirectory;
    job.prompt = "Render stems and return created track names so the AI assistant can suggest follow-up mix chains.";
    return job;
}

AiExternalJobRequest makeTempoKeyAnalysisJob(const ClipState& clip,
                                             const std::filesystem::path& outputDirectory) {
    AiExternalJobRequest job;
    job.engine = "essentia";
    job.mode = "tempo_key_section_analysis";
    job.inputPath = clip.sourcePath;
    job.outputDirectory = outputDirectory;
    job.prompt = "Analyze tempo, key, beat grid confidence, and likely arrangement sections for this clip.";
    return job;
}

AiExternalJobRequest makeMusicGenerationJob(const std::string& prompt,
                                            const std::filesystem::path& referenceAudioPath,
                                            const std::filesystem::path& outputDirectory) {
    AiExternalJobRequest job;
    job.engine = "musicgen";
    job.mode = referenceAudioPath.empty() ? "text_to_music" : "melody_conditioned_music";
    job.inputPath = referenceAudioPath;
    job.outputDirectory = outputDirectory;
    job.prompt = prompt;
    return job;
}

AiAnalysisTarget makeClipAnalysisTarget(const ProjectDocument& project, const ClipState& clip) {
    AiAnalysisTarget target;
    target.type = AiAnalysisTargetType::ClipSource;
    target.audioPath = AiAnalysisAudioPath::ClipSource;
    target.projectName = project.name;
    target.trackName = clip.trackName;
    target.clipId = clip.id;
    target.sourcePath = clip.sourcePath;
    target.startSeconds = clip.startSeconds;
    target.durationSeconds = clip.durationSeconds;
    return target;
}

AiAnalysisTarget makeTrackAnalysisTarget(const ProjectDocument& project, const TrackState& track) {
    AiAnalysisTarget target;
    target.type = track.name == "Master" ? AiAnalysisTargetType::MasterOutput : AiAnalysisTargetType::TrackOutput;
    target.audioPath = track.name == "Master" ? AiAnalysisAudioPath::MasterOutput : AiAnalysisAudioPath::TrackOutput;
    target.projectName = project.name;
    target.trackName = track.name;
    return target;
}

AiAnalysisTarget makeMasterAnalysisTarget(const ProjectDocument& project) {
    AiAnalysisTarget target;
    target.type = AiAnalysisTargetType::ProjectMix;
    target.audioPath = AiAnalysisAudioPath::MasterOutput;
    target.projectName = project.name;
    target.trackName = "Master";
    return target;
}

std::string aiAnalysisTargetKey(const AiAnalysisTarget& target) {
    std::ostringstream out;
    out << aiAnalysisTargetTypeToString(target.type)
        << "|path=" << aiAnalysisAudioPathToString(target.audioPath)
        << "|project=" << target.projectName
        << "|track=" << target.trackName
        << "|clip=" << target.clipId
        << "|source=" << target.sourcePath.string()
        << "|start=" << std::fixed << std::setprecision(3) << target.startSeconds
        << "|duration=" << std::fixed << std::setprecision(3) << target.durationSeconds;
    return out.str();
}

std::string aiAnalysisCacheKey(AiAnalysisJobKind kind, const AiAnalysisTarget& target) {
    return aiAnalysisJobKindToString(kind) + "|" + aiAnalysisTargetKey(target);
}

AiAnalysisJob makeAiAnalysisJob(AiAnalysisJobKind kind,
                                const AiAnalysisTarget& target,
                                const std::filesystem::path& outputDirectory,
                                const std::string& modelId) {
    AiAnalysisJob job;
    job.kind = kind;
    job.target = target;
    job.status = AiAnalysisJobStatus::Queued;
    job.modelId = modelId;
    job.outputDirectory = outputDirectory;
    job.createdAt = nowStampText();
    job.id = aiAnalysisJobKindToString(kind) + "-" + shortHashText(aiAnalysisCacheKey(kind, target) + "|" + modelId);
    return job;
}

AiAnalysisJob enqueueAiAnalysisJob(AiAnalysisQueueState& queue,
                                   AiAnalysisJobKind kind,
                                   const AiAnalysisTarget& target,
                                   const std::filesystem::path& outputDirectory,
                                   const std::string& modelId) {
    const std::string requestedKey = aiAnalysisCacheKey(kind, target);
    for (const auto& job : queue.jobs) {
        const bool active = job.status == AiAnalysisJobStatus::Queued || job.status == AiAnalysisJobStatus::Running;
        if (active && job.modelId == modelId && aiAnalysisCacheKey(job.kind, job.target) == requestedKey) {
            return job;
        }
    }
    AiAnalysisJob job = makeAiAnalysisJob(kind, target, outputDirectory, modelId);
    queue.jobs.push_back(job);
    return job;
}

const AiAnalysisCacheEntry* findAiAnalysisCacheEntry(const AiAnalysisQueueState& queue,
                                                     AiAnalysisJobKind kind,
                                                     const AiAnalysisTarget& target) {
    const std::string requestedKey = aiAnalysisCacheKey(kind, target);
    for (auto it = queue.cache.rbegin(); it != queue.cache.rend(); ++it) {
        if (it->key == requestedKey) {
            return &(*it);
        }
    }
    return nullptr;
}

void storeAiAnalysisCacheEntry(AiAnalysisQueueState& queue, AiAnalysisCacheEntry entry) {
    if (entry.key.empty()) {
        entry.key = aiAnalysisCacheKey(entry.kind, entry.target);
    }
    queue.cache.erase(std::remove_if(queue.cache.begin(), queue.cache.end(), [&](const AiAnalysisCacheEntry& existing) {
        return existing.key == entry.key && existing.modelId == entry.modelId;
    }), queue.cache.end());
    queue.cache.push_back(std::move(entry));
}

std::string serializeAiAnalysisJob(const AiAnalysisJob& job) {
    std::ostringstream out;
    out << "{"
        << "\"schema\":\"neuracoust-ai-analysis-job-v1\","
        << "\"id\":\"" << jsonEscape(job.id) << "\","
        << "\"kind\":\"" << aiAnalysisJobKindToString(job.kind) << "\","
        << "\"status\":\"" << aiAnalysisJobStatusToString(job.status) << "\","
        << "\"modelId\":\"" << jsonEscape(job.modelId) << "\","
        << "\"createdAt\":\"" << jsonEscape(job.createdAt) << "\","
        << "\"outputDirectory\":\"" << jsonEscape(job.outputDirectory.string()) << "\","
        << "\"target\":{"
        << "\"type\":\"" << aiAnalysisTargetTypeToString(job.target.type) << "\","
        << "\"audioPath\":\"" << aiAnalysisAudioPathToString(job.target.audioPath) << "\","
        << "\"projectName\":\"" << jsonEscape(job.target.projectName) << "\","
        << "\"trackName\":\"" << jsonEscape(job.target.trackName) << "\","
        << "\"clipId\":\"" << jsonEscape(job.target.clipId) << "\","
        << "\"sourcePath\":\"" << jsonEscape(job.target.sourcePath.string()) << "\","
        << "\"startSeconds\":" << job.target.startSeconds << ","
        << "\"durationSeconds\":" << job.target.durationSeconds
        << "}}";
    return out.str();
}

std::string serializeMixMemoryEntry(const AiMixMemoryEntry& entry) {
    std::ostringstream out;
    out << "{"
        << "\"schema\":\"neuracoust-ai-mix-memory-v1\","
        << "\"id\":\"" << jsonEscape(entry.id) << "\","
        << "\"projectName\":\"" << jsonEscape(entry.projectName) << "\","
        << "\"sourceDescription\":\"" << jsonEscape(entry.sourceDescription) << "\","
        << "\"userFeedback\":\"" << jsonEscape(entry.userFeedback) << "\",";
    out << "\"tags\":[";
    for (size_t i = 0; i < entry.tags.size(); ++i) {
        if (i != 0) out << ",";
        out << "\"" << jsonEscape(entry.tags[i]) << "\"";
    }
    out << "],\"acceptedCommands\":[";
    for (size_t i = 0; i < entry.acceptedCommands.size(); ++i) {
        if (i != 0) out << ",";
        out << serializeAiCommandPreview(entry.acceptedCommands[i]);
    }
    out << "]}";
    return out.str();
}

bool writeMixMemoryEntry(const AiMixMemoryEntry& entry,
                         const std::filesystem::path& memoryDirectory,
                         std::filesystem::path& writtenPath,
                         std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(memoryDirectory, ec);
    if (ec) {
        error = "Could not create AI mix memory directory: " + ec.message();
        return false;
    }
    const std::string fileName = entry.id.empty() ? "memory-entry.json" : entry.id + ".json";
    writtenPath = memoryDirectory / fileName;
    std::ofstream out(writtenPath, std::ios::binary);
    if (!out) {
        error = "Could not write AI mix memory entry.";
        return false;
    }
    out << serializeMixMemoryEntry(entry) << "\n";
    return true;
}

} // namespace neuracoust::daw
