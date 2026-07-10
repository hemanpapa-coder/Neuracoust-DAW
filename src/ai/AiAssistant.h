#pragma once

#include "audio/WavFile.h"
#include "project/ProjectDocument.h"
#include "project/StemMagicBridge.h"
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace neuracoust::daw {

enum class AiCommandType {
    Unknown,
    SetTrackGain,
    SetTrackPan,
    SetTrackMute,
    SetTrackSolo,
    ArmTrackForRecording,
    AddMarker,
    RequestStemMagic,
    ScheduleRtNeuralTraining,
    ClassifyClipWithYamNet
};

struct AiCommand {
    AiCommandType type = AiCommandType::Unknown;
    std::string targetTrackName;
    std::string targetClipId;
    std::string label;
    std::filesystem::path beforeAudioPath;
    std::filesystem::path afterAudioPath;
    float gainDb = 0.0f;
    float pan = 0.0f;
    double timeSeconds = 0.0;
    bool enabled = false;
    std::string reason;
};

struct AiCommandValidation {
    bool ok = false;
    bool requiresUserConfirmation = true;
    bool modifiesAudioFiles = false;
    bool runsExternalModel = false;
    std::string message;
};

struct AiProjectSnapshot {
    std::string projectName;
    double sampleRate = 0.0;
    int tempoBpm = 0;
    int trackCount = 0;
    int audioTrackCount = 0;
    int clipCount = 0;
    int masterInsertCount = 0;
    std::vector<std::string> trackSummaries;
    std::vector<std::string> clipSummaries;
    std::string healthSummary;
};

struct AiAudioFeatureSummary {
    bool ok = false;
    int channels = 0;
    int sampleRate = 0;
    double durationSeconds = 0.0;
    float peak = 0.0f;
    float rms = 0.0f;
    float spectralCentroidHz = 0.0f;
    float crestFactor = 0.0f;
    bool likelyClipped = false;
    bool veryQuiet = false;
    bool lowFrequencyHeavy = false;
    bool highFrequencyHeavy = false;
    std::string message;
};

struct AiMixMemoryEntry {
    std::string id;
    std::string projectName;
    std::string sourceDescription;
    std::string userFeedback;
    std::vector<AiCommand> acceptedCommands;
    std::vector<std::string> tags;
};

struct AiLocalAssistantConfig {
    bool enabled = false;
    std::string provider = "ollama";
    std::string endpoint = "http://127.0.0.1:11434/api/chat";
    std::string model;
};

struct AiLocalModelInventory {
    std::string ollamaVersion;
    std::vector<std::string> installedModels;
};

struct AiLocalModelRecommendation {
    std::string interactiveModel;
    std::string codingModel;
    std::string reasoningModel;
    std::string lightweightModel;
    std::string embeddingModel;
    std::vector<std::string> installSuggestions;
    std::vector<std::string> upgradeSuggestions;
    std::string summary;
};

enum class AiModelTier {
    Unknown,
    Light,
    Standard,
    Heavy,
    RealtimeOnly
};

enum class AiExecutionDecision {
    AllowNow,
    QueueUntilIdle,
    Block
};

enum class AiHostPlatform {
    Unknown,
    MacOS,
    Windows,
    Linux
};

enum class AiGpuVendor {
    Unknown,
    Apple,
    Nvidia,
    Amd,
    Intel,
    Qualcomm,
    CpuOnly
};

enum class AiGpuGeneration {
    Unknown,
    AppleSilicon,
    NvidiaPascal,
    NvidiaTuringOrNewer,
    AmdGcnOrPolaris,
    AmdRdnaOrNewer,
    IntelXeOrNewer,
    QualcommNpu
};

enum class AiRuntimeBackend {
    CoreML,
    WindowsML,
    OnnxRuntimeDirectML,
    OnnxRuntimeCPU,
    WhisperCppCUDA,
    CudaBatch,
    OpenVINOCpu,
    RtNeuralRealtime,
    ExternalSidecar
};

enum class AiAnalysisJobKind {
    MixDiagnostic,
    SoundClassification,
    Transcription,
    LyricTranscription,
    StemSeparation,
    Denoise,
    AudioEmbedding
};

struct AiHardwareProfile {
    AiHostPlatform platform = AiHostPlatform::Unknown;
    AiGpuVendor gpuVendor = AiGpuVendor::Unknown;
    AiGpuGeneration gpuGeneration = AiGpuGeneration::Unknown;
    std::string deviceName;
    double gpuMemoryGb = 0.0;
    int cudaComputeCapabilityMajor = 0;
    int cudaComputeCapabilityMinor = 0;
    bool supportsAppleNeuralEngine = false;
    bool supportsCoreML = false;
    bool supportsWindowsML = false;
    bool supportsDirectML = false;
    bool supportsDirectX12 = false;
    bool supportsCuda = false;
    bool supportsNpu = false;
    bool hasTensorCores = false;
    bool fastFp16 = false;
    bool fastInt8 = false;
};

struct AiFeatureBackendPlan {
    AiAnalysisJobKind feature = AiAnalysisJobKind::MixDiagnostic;
    AiRuntimeBackend primaryBackend = AiRuntimeBackend::OnnxRuntimeCPU;
    AiRuntimeBackend fallbackBackend = AiRuntimeBackend::OnnxRuntimeCPU;
    AiModelTier modelTier = AiModelTier::Light;
    bool realtimeSafe = false;
    bool offlineOnly = true;
    std::string modelFamily;
    std::string note;
};

struct AiRuntimeState {
    bool transportRunning = false;
    bool recordingActive = false;
    bool offlineBounceActive = false;
    bool realtimeAudioEngineActive = false;
    double availableRamGb = 0.0;
    double availableGpuMemoryGb = 0.0;
    bool memoryPressure = false;
};

struct AiExecutionPolicy {
    bool safeModeEnabled = true;
    double minimumRamGbForStandard = 6.0;
    double minimumRamGbForHeavy = 12.0;
    double minimumGpuGbForStandard = 4.0;
    double minimumGpuGbForHeavy = 10.0;
};

struct AiExecutionGuardResult {
    AiExecutionDecision decision = AiExecutionDecision::Block;
    AiModelTier modelTier = AiModelTier::Unknown;
    bool shouldUnloadAfterUse = true;
    int recommendedKeepAliveSeconds = 0;
    int recommendedNumPredict = 256;
    std::string message;
};

struct AiExternalJobRequest {
    std::string engine;
    std::string mode;
    std::filesystem::path inputPath;
    std::filesystem::path referencePath;
    std::filesystem::path outputDirectory;
    std::string prompt;
};

enum class AiAnalysisTargetType {
    ProjectMix,
    MasterOutput,
    TrackOutput,
    ClipSource
};

enum class AiAnalysisAudioPath {
    MasterOutput,
    TrackOutput,
    ClipSource,
    RenderedSelection
};

enum class AiAnalysisJobStatus {
    Queued,
    Running,
    Completed,
    Failed,
    Cancelled
};

struct AiAnalysisTarget {
    AiAnalysisTargetType type = AiAnalysisTargetType::ProjectMix;
    AiAnalysisAudioPath audioPath = AiAnalysisAudioPath::MasterOutput;
    std::string projectName;
    std::string trackName;
    std::string clipId;
    std::filesystem::path sourcePath;
    double startSeconds = 0.0;
    double durationSeconds = 0.0;
};

struct AiAnalysisJob {
    std::string id;
    AiAnalysisJobKind kind = AiAnalysisJobKind::MixDiagnostic;
    AiAnalysisTarget target;
    AiAnalysisJobStatus status = AiAnalysisJobStatus::Queued;
    std::string modelId;
    std::filesystem::path outputDirectory;
    std::string createdAt;
};

struct AiAnalysisCacheEntry {
    std::string key;
    AiAnalysisJobKind kind = AiAnalysisJobKind::MixDiagnostic;
    AiAnalysisTarget target;
    AiAnalysisJobStatus status = AiAnalysisJobStatus::Completed;
    std::string modelId;
    std::string summary;
    std::vector<std::string> tags;
    std::vector<std::filesystem::path> artifactPaths;
    std::string error;
};

struct AiAnalysisQueueState {
    std::vector<AiAnalysisJob> jobs;
    std::vector<AiAnalysisCacheEntry> cache;
};

enum class AiIssueSeverity {
    Info,
    Warning,
    Critical
};

struct AiMixDiagnosticIssue {
    AiIssueSeverity severity = AiIssueSeverity::Info;
    std::string category;
    std::string targetName;
    std::string message;
    std::string recommendation;
};

struct AiPluginChainSuggestion {
    std::string targetTrackName;
    std::string inferredRole;
    std::vector<std::string> processors;
    std::string rationale;
};

struct AiEditSuggestion {
    std::string action;
    std::string targetName;
    std::string reason;
    bool requiresUserConfirmation = true;
};

struct AiRecordingCoachMessage {
    AiIssueSeverity severity = AiIssueSeverity::Info;
    std::string message;
    std::string recommendation;
};

struct AiMixMemorySearchResult {
    std::string id;
    int score = 0;
    std::string summary;
};

std::string aiCommandTypeToString(AiCommandType type);
AiCommandType aiCommandTypeFromString(const std::string& text);
AiCommandValidation validateAiCommand(const ProjectDocument& project, const AiCommand& command);
std::string serializeAiCommandPreview(const AiCommand& command);

AiProjectSnapshot makeAiProjectSnapshot(const ProjectDocument& project);
std::string serializeAiProjectSnapshot(const AiProjectSnapshot& snapshot);
std::string buildAiAssistantSystemPrompt();
std::string buildOllamaChatRequestJson(const AiLocalAssistantConfig& config,
                                       const AiProjectSnapshot& snapshot,
                                       const std::string& userMessage);
bool ollamaVersionAtLeast(const std::string& versionText, int major, int minor, int patch);
AiLocalModelRecommendation recommendLocalAiModels(const AiLocalModelInventory& inventory,
                                                  bool appleSilicon);
std::string aiModelTierToString(AiModelTier tier);
std::string aiExecutionDecisionToString(AiExecutionDecision decision);
std::string aiIssueSeverityToString(AiIssueSeverity severity);
std::string aiAnalysisTargetTypeToString(AiAnalysisTargetType type);
std::string aiAnalysisAudioPathToString(AiAnalysisAudioPath path);
std::string aiAnalysisJobKindToString(AiAnalysisJobKind kind);
std::string aiAnalysisJobStatusToString(AiAnalysisJobStatus status);
std::string aiRuntimeBackendToString(AiRuntimeBackend backend);
std::string aiGpuVendorToString(AiGpuVendor vendor);
std::string aiGpuGenerationToString(AiGpuGeneration generation);
AiModelTier classifyAiModelTier(const std::string& modelName);
AiExecutionGuardResult evaluateAiExecutionGuard(const std::string& modelName,
                                                const AiRuntimeState& runtime,
                                                const AiExecutionPolicy& policy = {});
bool aiHardwareHasModernGpuAcceleration(const AiHardwareProfile& hardware);
std::vector<AiFeatureBackendPlan> recommendAiFeatureBackendPlan(const AiHardwareProfile& hardware);

AiAudioFeatureSummary analyzeAudioForAi(const WavAudioData& audio);
std::vector<std::string> suggestTrackTagsFromYamNetLabels(const std::vector<std::string>& labels);
std::string inferTrackRoleForAi(const std::string& trackName,
                                const std::vector<std::string>& tags);
std::vector<AiMixDiagnosticIssue> diagnoseTrackForAi(const std::string& targetName,
                                                     const AiAudioFeatureSummary& features,
                                                     const std::vector<std::string>& tags = {});
AiPluginChainSuggestion suggestPluginChainForAi(const std::string& trackName,
                                                const AiAudioFeatureSummary& features,
                                                const std::vector<std::string>& tags = {});
std::vector<AiEditSuggestion> suggestEditActionsForAi(const std::string& targetName,
                                                      const AiAudioFeatureSummary& features,
                                                      const std::vector<std::string>& tags = {});
std::vector<AiRecordingCoachMessage> makeRecordingCoachMessages(const AiAudioFeatureSummary& liveInputFeatures,
                                                                float livePeak,
                                                                float estimatedNoiseFloorRms);
std::vector<AiMixMemorySearchResult> searchMixMemoryEntries(const std::vector<AiMixMemoryEntry>& entries,
                                                            const std::vector<std::string>& queryTags,
                                                            const std::string& queryText,
                                                            size_t limit = 5);
AiExternalJobRequest makeYamNetClassificationJob(const ClipState& clip,
                                                 const std::filesystem::path& outputDirectory);
AiExternalJobRequest makeVadSegmentationJob(const ClipState& clip,
                                            const std::filesystem::path& outputDirectory);
AiExternalJobRequest makeWhisperTranscriptionJob(const ClipState& clip,
                                                 const AiFeatureBackendPlan& backendPlan,
                                                 const std::filesystem::path& outputDirectory);
AiExternalJobRequest makeDenoiseCleanupJob(const ClipState& clip,
                                           const AiFeatureBackendPlan& backendPlan,
                                           const std::filesystem::path& outputDirectory);
AiExternalJobRequest makeAudioEmbeddingIndexJob(const ClipState& clip,
                                                const AiFeatureBackendPlan& backendPlan,
                                                const std::filesystem::path& outputDirectory);
AiExternalJobRequest makeRtNeuralTrainingJob(const std::filesystem::path& beforeAudioPath,
                                             const std::filesystem::path& afterAudioPath,
                                             const std::filesystem::path& outputDirectory);
AiExternalJobRequest makeStemMagicAssistantJob(const ClipState& clip,
                                               StemMagicJobMode mode,
                                               const std::filesystem::path& outputDirectory);
AiExternalJobRequest makeTempoKeyAnalysisJob(const ClipState& clip,
                                             const std::filesystem::path& outputDirectory);
AiExternalJobRequest makeMusicGenerationJob(const std::string& prompt,
                                            const std::filesystem::path& referenceAudioPath,
                                            const std::filesystem::path& outputDirectory);

AiAnalysisTarget makeClipAnalysisTarget(const ProjectDocument& project, const ClipState& clip);
AiAnalysisTarget makeTrackAnalysisTarget(const ProjectDocument& project, const TrackState& track);
AiAnalysisTarget makeMasterAnalysisTarget(const ProjectDocument& project);
std::string aiAnalysisTargetKey(const AiAnalysisTarget& target);
std::string aiAnalysisCacheKey(AiAnalysisJobKind kind, const AiAnalysisTarget& target);
AiAnalysisJob makeAiAnalysisJob(AiAnalysisJobKind kind,
                                const AiAnalysisTarget& target,
                                const std::filesystem::path& outputDirectory,
                                const std::string& modelId = {});
AiAnalysisJob enqueueAiAnalysisJob(AiAnalysisQueueState& queue,
                                   AiAnalysisJobKind kind,
                                   const AiAnalysisTarget& target,
                                   const std::filesystem::path& outputDirectory,
                                   const std::string& modelId = {});
const AiAnalysisCacheEntry* findAiAnalysisCacheEntry(const AiAnalysisQueueState& queue,
                                                     AiAnalysisJobKind kind,
                                                     const AiAnalysisTarget& target);
void storeAiAnalysisCacheEntry(AiAnalysisQueueState& queue, AiAnalysisCacheEntry entry);
std::string serializeAiAnalysisJob(const AiAnalysisJob& job);

std::string serializeMixMemoryEntry(const AiMixMemoryEntry& entry);
bool writeMixMemoryEntry(const AiMixMemoryEntry& entry,
                         const std::filesystem::path& memoryDirectory,
                         std::filesystem::path& writtenPath,
                         std::string& error);

} // namespace neuracoust::daw
