#include "ai/AiAssistant.h"
#include "project/ProjectDocument.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace {

std::filesystem::path testTempRoot() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = std::filesystem::temp_directory_path() / ("neuracoust-daw-ai-smoke-" + std::to_string(stamp));
    std::filesystem::create_directories(path);
    return path;
}

neuracoust::daw::WavAudioData makeSineWav() {
    neuracoust::daw::WavAudioData wav;
    wav.channels = 1;
    wav.sampleRate = 8000;
    wav.bitsPerSample = 32;
    wav.floatingPoint = true;
    const int frames = wav.sampleRate;
    wav.interleavedSamples.resize(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        const double phase = 2.0 * 3.14159265358979323846 * 440.0 * static_cast<double>(i) / wav.sampleRate;
        wav.interleavedSamples[static_cast<size_t>(i)] = static_cast<float>(std::sin(phase) * 0.5);
    }
    return wav;
}

} // namespace

int main() {
    auto project = neuracoust::daw::defaultProject();
    project.name = "AI Session";
    project.tracks.front().name = "Lead Vocal";
    project.tracks.front().volumeDb = -2.0f;

    neuracoust::daw::ClipState clip;
    clip.id = "clip-vocal";
    clip.trackName = "Lead Vocal";
    clip.regionName = "Verse Vocal";
    clip.sourcePath = "/tmp/verse-vocal.wav";
    clip.durationSeconds = 12.0;
    project.clips.push_back(clip);

    const auto snapshot = neuracoust::daw::makeAiProjectSnapshot(project);
    assert(snapshot.projectName == "AI Session");
    assert(snapshot.trackCount >= 1);
    assert(snapshot.clipCount == 1);
    const auto snapshotJson = neuracoust::daw::serializeAiProjectSnapshot(snapshot);
    assert(snapshotJson.find("Lead Vocal") != std::string::npos);
    assert(snapshotJson.find("Verse Vocal") != std::string::npos);

    neuracoust::daw::AiCommand gainCommand;
    gainCommand.type = neuracoust::daw::AiCommandType::SetTrackGain;
    gainCommand.targetTrackName = "Lead Vocal";
    gainCommand.gainDb = -4.5f;
    gainCommand.reason = "Place the vocal inside the mix before compression.";
    const auto validation = neuracoust::daw::validateAiCommand(project, gainCommand);
    assert(validation.ok);
    assert(validation.requiresUserConfirmation);
    const auto preview = neuracoust::daw::serializeAiCommandPreview(gainCommand);
    assert(preview.find("set_track_gain") != std::string::npos);
    assert(preview.find("Lead Vocal") != std::string::npos);

    gainCommand.gainDb = -100.0f;
    assert(!neuracoust::daw::validateAiCommand(project, gainCommand).ok);

    neuracoust::daw::AiLocalAssistantConfig config;
    config.enabled = true;
    config.model = "llama3.1:8b";
    const auto chatJson = neuracoust::daw::buildOllamaChatRequestJson(config, snapshot, "보컬을 앞으로 빼줘");
    assert(chatJson.find("\"stream\":false") != std::string::npos);
    assert(chatJson.find("llama3.1:8b") != std::string::npos);
    assert(chatJson.find("Project snapshot") != std::string::npos);
    assert(neuracoust::daw::ollamaVersionAtLeast("ollama version is 0.31.1", 0, 31, 0));
    assert(!neuracoust::daw::ollamaVersionAtLeast("ollama version is 0.30.11", 0, 31, 0));
    neuracoust::daw::AiLocalModelInventory inventory;
    inventory.ollamaVersion = "ollama version is 0.30.11";
    inventory.installedModels = {
        "qwen3.6:27b-mlx",
        "qwen3-coder:30b",
        "gemma3n:e4b",
        "magistral:24b",
        "nomic-embed-text:latest"
    };
    const auto modelRecommendation = neuracoust::daw::recommendLocalAiModels(inventory, true);
    assert(modelRecommendation.interactiveModel == "qwen3.6:27b-mlx");
    assert(modelRecommendation.codingModel == "qwen3-coder:30b");
    assert(modelRecommendation.reasoningModel == "magistral:24b");
    assert(modelRecommendation.lightweightModel == "gemma3n:e4b");
    assert(modelRecommendation.embeddingModel == "nomic-embed-text:latest");
    assert(!modelRecommendation.upgradeSuggestions.empty());
    assert(!modelRecommendation.installSuggestions.empty());
    assert(neuracoust::daw::classifyAiModelTier("gemma3n:e4b") == neuracoust::daw::AiModelTier::Light);
    assert(neuracoust::daw::classifyAiModelTier("gemma4:12b") == neuracoust::daw::AiModelTier::Standard);
    assert(neuracoust::daw::classifyAiModelTier("qwen3.6:27b-mlx") == neuracoust::daw::AiModelTier::Heavy);
    assert(neuracoust::daw::classifyAiModelTier("rtneural:frozen-vocal") == neuracoust::daw::AiModelTier::RealtimeOnly);
    neuracoust::daw::AiRuntimeState idleRuntime;
    idleRuntime.availableRamGb = 16.0;
    idleRuntime.availableGpuMemoryGb = 12.0;
    const auto idleGemma4 = neuracoust::daw::evaluateAiExecutionGuard("gemma4:12b", idleRuntime);
    assert(idleGemma4.decision == neuracoust::daw::AiExecutionDecision::AllowNow);
    assert(idleGemma4.shouldUnloadAfterUse);
    assert(idleGemma4.recommendedKeepAliveSeconds == 0);
    assert(idleGemma4.recommendedNumPredict >= 256);
    auto playbackRuntime = idleRuntime;
    playbackRuntime.transportRunning = true;
    playbackRuntime.realtimeAudioEngineActive = true;
    assert(neuracoust::daw::evaluateAiExecutionGuard("gemma4:12b", playbackRuntime).decision ==
           neuracoust::daw::AiExecutionDecision::QueueUntilIdle);
    assert(neuracoust::daw::evaluateAiExecutionGuard("qwen3.6:27b-mlx", playbackRuntime).decision ==
           neuracoust::daw::AiExecutionDecision::QueueUntilIdle);
    const auto lightDuringPlayback = neuracoust::daw::evaluateAiExecutionGuard("gemma3n:e4b", playbackRuntime);
    assert(lightDuringPlayback.decision == neuracoust::daw::AiExecutionDecision::AllowNow);
    assert(!lightDuringPlayback.shouldUnloadAfterUse);
    auto recordingRuntime = idleRuntime;
    recordingRuntime.recordingActive = true;
    assert(neuracoust::daw::evaluateAiExecutionGuard("gemma3n:e4b", recordingRuntime).decision ==
           neuracoust::daw::AiExecutionDecision::Block);
    auto pressureRuntime = idleRuntime;
    pressureRuntime.memoryPressure = true;
    assert(neuracoust::daw::evaluateAiExecutionGuard("gemma4:12b", pressureRuntime).decision ==
           neuracoust::daw::AiExecutionDecision::Block);
    auto lowMemoryRuntime = idleRuntime;
    lowMemoryRuntime.availableRamGb = 4.0;
    assert(neuracoust::daw::evaluateAiExecutionGuard("gemma4:12b", lowMemoryRuntime).decision ==
           neuracoust::daw::AiExecutionDecision::QueueUntilIdle);
    assert(neuracoust::daw::evaluateAiExecutionGuard("rtneural:frozen-vocal", recordingRuntime).decision ==
           neuracoust::daw::AiExecutionDecision::AllowNow);

    neuracoust::daw::AiHardwareProfile gtx1060;
    gtx1060.platform = neuracoust::daw::AiHostPlatform::Windows;
    gtx1060.gpuVendor = neuracoust::daw::AiGpuVendor::Nvidia;
    gtx1060.gpuGeneration = neuracoust::daw::AiGpuGeneration::NvidiaPascal;
    gtx1060.deviceName = "GeForce GTX 1060 6GB";
    gtx1060.gpuMemoryGb = 6.0;
    gtx1060.cudaComputeCapabilityMajor = 6;
    gtx1060.cudaComputeCapabilityMinor = 1;
    gtx1060.supportsCuda = true;
    gtx1060.supportsDirectX12 = true;
    gtx1060.supportsDirectML = true;
    const auto gtx1060Plan = neuracoust::daw::recommendAiFeatureBackendPlan(gtx1060);
    auto findPlan = [&](const std::vector<neuracoust::daw::AiFeatureBackendPlan>& plans,
                        neuracoust::daw::AiAnalysisJobKind kind) {
        return std::find_if(plans.begin(), plans.end(), [&](const auto& plan) {
            return plan.feature == kind;
        });
    };
    const auto gtxTranscription = findPlan(gtx1060Plan, neuracoust::daw::AiAnalysisJobKind::Transcription);
    assert(gtxTranscription != gtx1060Plan.end());
    assert(gtxTranscription->primaryBackend == neuracoust::daw::AiRuntimeBackend::WhisperCppCUDA);
    assert(gtxTranscription->note.find("TensorRT") != std::string::npos);
    const auto gtxStems = findPlan(gtx1060Plan, neuracoust::daw::AiAnalysisJobKind::StemSeparation);
    assert(gtxStems != gtx1060Plan.end());
    assert(gtxStems->offlineOnly);
    assert(gtxStems->primaryBackend == neuracoust::daw::AiRuntimeBackend::CudaBatch);
    assert(!neuracoust::daw::aiHardwareHasModernGpuAcceleration(gtx1060));

    neuracoust::daw::AiHardwareProfile amdPolaris;
    amdPolaris.platform = neuracoust::daw::AiHostPlatform::Windows;
    amdPolaris.gpuVendor = neuracoust::daw::AiGpuVendor::Amd;
    amdPolaris.gpuGeneration = neuracoust::daw::AiGpuGeneration::AmdGcnOrPolaris;
    amdPolaris.deviceName = "Radeon RX 580 8GB";
    amdPolaris.gpuMemoryGb = 8.0;
    amdPolaris.supportsDirectX12 = true;
    amdPolaris.supportsDirectML = true;
    const auto amdPolarisPlan = neuracoust::daw::recommendAiFeatureBackendPlan(amdPolaris);
    const auto amdClassification = findPlan(amdPolarisPlan, neuracoust::daw::AiAnalysisJobKind::SoundClassification);
    assert(amdClassification != amdPolarisPlan.end());
    assert(amdClassification->primaryBackend == neuracoust::daw::AiRuntimeBackend::OnnxRuntimeDirectML);
    const auto amdTranscription = findPlan(amdPolarisPlan, neuracoust::daw::AiAnalysisJobKind::Transcription);
    assert(amdTranscription != amdPolarisPlan.end());
    assert(amdTranscription->fallbackBackend == neuracoust::daw::AiRuntimeBackend::OnnxRuntimeCPU);

    neuracoust::daw::AiHardwareProfile rtxGpu = gtx1060;
    rtxGpu.deviceName = "GeForce RTX 3060";
    rtxGpu.gpuGeneration = neuracoust::daw::AiGpuGeneration::NvidiaTuringOrNewer;
    rtxGpu.cudaComputeCapabilityMajor = 8;
    rtxGpu.cudaComputeCapabilityMinor = 6;
    rtxGpu.gpuMemoryGb = 12.0;
    rtxGpu.hasTensorCores = true;
    rtxGpu.fastFp16 = true;
    assert(neuracoust::daw::aiHardwareHasModernGpuAcceleration(rtxGpu));
    assert(neuracoust::daw::aiRuntimeBackendToString(neuracoust::daw::AiRuntimeBackend::WindowsML) == "windows_ml");
    assert(neuracoust::daw::aiGpuVendorToString(neuracoust::daw::AiGpuVendor::Amd) == "amd");
    assert(neuracoust::daw::aiGpuGenerationToString(neuracoust::daw::AiGpuGeneration::NvidiaPascal) == "nvidia_pascal");

    const auto features = neuracoust::daw::analyzeAudioForAi(makeSineWav());
    assert(features.ok);
    assert(features.channels == 1);
    assert(features.sampleRate == 8000);
    assert(features.durationSeconds > 0.99 && features.durationSeconds < 1.01);
    assert(features.peak > 0.49f && features.peak < 0.51f);
    assert(features.rms > 0.30f && features.rms < 0.40f);
    assert(features.spectralCentroidHz > 100.0f);
    assert(features.crestFactor > 1.0f);
    assert(!features.likelyClipped);

    const auto tags = neuracoust::daw::suggestTrackTagsFromYamNetLabels({"Singing", "Drum kit", "Noise"});
    assert(tags.size() >= 3);
    assert(neuracoust::daw::inferTrackRoleForAi("Lead Vocal", tags) == "vocal");
    const auto chain = neuracoust::daw::suggestPluginChainForAi("Lead Vocal", features, tags);
    assert(chain.inferredRole == "vocal");
    assert(!chain.processors.empty());
    assert(chain.processors[0].find("EQ") != std::string::npos ||
           chain.processors[0].find("Normalize") != std::string::npos ||
           chain.processors[0].find("repair") != std::string::npos);
    const auto diagnosticIssues = neuracoust::daw::diagnoseTrackForAi("Lead Vocal", features, tags);
    assert(diagnosticIssues.empty() || !diagnosticIssues.front().message.empty());
    const auto editSuggestions = neuracoust::daw::suggestEditActionsForAi("Lead Vocal", features, tags);
    assert(!editSuggestions.empty());
    const auto coachOk = neuracoust::daw::makeRecordingCoachMessages(features, 0.45f, 0.005f);
    assert(!coachOk.empty());
    assert(coachOk.front().severity == neuracoust::daw::AiIssueSeverity::Info);
    const auto coachClip = neuracoust::daw::makeRecordingCoachMessages(features, 0.99f, 0.005f);
    assert(coachClip.front().severity == neuracoust::daw::AiIssueSeverity::Critical);
    assert(neuracoust::daw::aiIssueSeverityToString(coachClip.front().severity) == "critical");

    const auto yamnetJob = neuracoust::daw::makeYamNetClassificationJob(clip, testTempRoot());
    assert(yamnetJob.engine == "yamnet");
    assert(yamnetJob.mode == "audioset_clip_classification");
    assert(yamnetJob.inputPath.string().find("verse-vocal.wav") != std::string::npos);

    const auto vadJob = neuracoust::daw::makeVadSegmentationJob(clip, testTempRoot());
    assert(vadJob.engine == "silero_vad");
    assert(vadJob.mode == "speech_silence_segments_onnx_cpu");

    const auto whisperJob = neuracoust::daw::makeWhisperTranscriptionJob(clip, *gtxTranscription, testTempRoot());
    assert(whisperJob.engine == "whisper");
    assert(whisperJob.mode == "whisper_cpp_cuda");
    assert(whisperJob.prompt.find("tiny/base/small") != std::string::npos);

    const auto gtxDenoise = findPlan(gtx1060Plan, neuracoust::daw::AiAnalysisJobKind::Denoise);
    assert(gtxDenoise != gtx1060Plan.end());
    const auto denoiseJob = neuracoust::daw::makeDenoiseCleanupJob(clip, *gtxDenoise, testTempRoot());
    assert(denoiseJob.engine == "voice_clean");
    assert(denoiseJob.prompt.find("original clip unchanged") != std::string::npos);

    const auto amdEmbedding = findPlan(amdPolarisPlan, neuracoust::daw::AiAnalysisJobKind::AudioEmbedding);
    assert(amdEmbedding != amdPolarisPlan.end());
    const auto embeddingJob = neuracoust::daw::makeAudioEmbeddingIndexJob(clip, *amdEmbedding, testTempRoot());
    assert(embeddingJob.engine == "audio_embedding");
    assert(embeddingJob.mode == "onnxruntime_directml");

    const auto rtJob = neuracoust::daw::makeRtNeuralTrainingJob("/tmp/before.wav", "/tmp/after.wav", testTempRoot());
    assert(rtJob.engine == "rtneural");
    assert(rtJob.referencePath.string().find("after.wav") != std::string::npos);

    const auto stemJob = neuracoust::daw::makeStemMagicAssistantJob(clip, neuracoust::daw::StemMagicJobMode::FourStem, testTempRoot());
    assert(stemJob.engine == "stem_magic");
    assert(stemJob.mode == "four_stem");
    const auto tempoKeyJob = neuracoust::daw::makeTempoKeyAnalysisJob(clip, testTempRoot());
    assert(tempoKeyJob.engine == "essentia");
    assert(tempoKeyJob.mode == "tempo_key_section_analysis");
    const auto generationJob = neuracoust::daw::makeMusicGenerationJob("Generate an 80s synth bass loop", "/tmp/melody.wav", testTempRoot());
    assert(generationJob.engine == "musicgen");
    assert(generationJob.mode == "melody_conditioned_music");

    const auto clipTarget = neuracoust::daw::makeClipAnalysisTarget(project, clip);
    assert(clipTarget.type == neuracoust::daw::AiAnalysisTargetType::ClipSource);
    assert(clipTarget.audioPath == neuracoust::daw::AiAnalysisAudioPath::ClipSource);
    assert(clipTarget.clipId == "clip-vocal");
    assert(neuracoust::daw::aiAnalysisTargetKey(clipTarget).find("clip-vocal") != std::string::npos);
    const auto trackTarget = neuracoust::daw::makeTrackAnalysisTarget(project, project.tracks.front());
    assert(trackTarget.type == neuracoust::daw::AiAnalysisTargetType::TrackOutput);
    assert(trackTarget.audioPath == neuracoust::daw::AiAnalysisAudioPath::TrackOutput);
    const auto masterTarget = neuracoust::daw::makeMasterAnalysisTarget(project);
    assert(masterTarget.audioPath == neuracoust::daw::AiAnalysisAudioPath::MasterOutput);

    neuracoust::daw::AiAnalysisQueueState analysisQueue;
    const auto queuedSoundJob = neuracoust::daw::enqueueAiAnalysisJob(
        analysisQueue,
        neuracoust::daw::AiAnalysisJobKind::SoundClassification,
        clipTarget,
        testTempRoot() / "AI Analysis",
        "coreml-sound-classifier-v1");
    assert(analysisQueue.jobs.size() == 1);
    assert(queuedSoundJob.status == neuracoust::daw::AiAnalysisJobStatus::Queued);
    assert(neuracoust::daw::serializeAiAnalysisJob(queuedSoundJob).find("sound_classification") != std::string::npos);
    const auto duplicateSoundJob = neuracoust::daw::enqueueAiAnalysisJob(
        analysisQueue,
        neuracoust::daw::AiAnalysisJobKind::SoundClassification,
        clipTarget,
        testTempRoot() / "AI Analysis",
        "coreml-sound-classifier-v1");
    assert(analysisQueue.jobs.size() == 1);
    assert(duplicateSoundJob.id == queuedSoundJob.id);
    (void)neuracoust::daw::enqueueAiAnalysisJob(
        analysisQueue,
        neuracoust::daw::AiAnalysisJobKind::Transcription,
        clipTarget,
        testTempRoot() / "AI Transcript",
        "whisper-coreml-small");
    assert(analysisQueue.jobs.size() == 2);
    const auto lyricJob = neuracoust::daw::enqueueAiAnalysisJob(
        analysisQueue,
        neuracoust::daw::AiAnalysisJobKind::LyricTranscription,
        clipTarget,
        testTempRoot() / "AI Lyrics",
        "stemmagic-mps+whisper-coreml");
    assert(analysisQueue.jobs.size() == 3);
    assert(neuracoust::daw::serializeAiAnalysisJob(lyricJob).find("lyric_transcription") != std::string::npos);

    neuracoust::daw::AiAnalysisCacheEntry cacheEntry;
    cacheEntry.kind = neuracoust::daw::AiAnalysisJobKind::SoundClassification;
    cacheEntry.target = clipTarget;
    cacheEntry.modelId = "coreml-sound-classifier-v1";
    cacheEntry.summary = "speech, vocal, noise";
    cacheEntry.tags = {"speech", "vocal"};
    neuracoust::daw::storeAiAnalysisCacheEntry(analysisQueue, cacheEntry);
    const auto* cached = neuracoust::daw::findAiAnalysisCacheEntry(
        analysisQueue,
        neuracoust::daw::AiAnalysisJobKind::SoundClassification,
        clipTarget);
    assert(cached != nullptr);
    assert(cached->summary.find("speech") != std::string::npos);

    neuracoust::daw::AiMixMemoryEntry memory;
    memory.id = "ai-session-vocal-approval";
    memory.projectName = project.name;
    memory.sourceDescription = "User approved vocal gain move.";
    memory.userFeedback = "좋다";
    memory.acceptedCommands.push_back(gainCommand);
    memory.tags = {"vocal", "ballad"};
    const auto memoryJson = neuracoust::daw::serializeMixMemoryEntry(memory);
    assert(memoryJson.find("neuracoust-ai-mix-memory-v1") != std::string::npos);
    assert(memoryJson.find("ballad") != std::string::npos);
    std::filesystem::path writtenPath;
    std::string error;
    assert(neuracoust::daw::writeMixMemoryEntry(memory, testTempRoot(), writtenPath, error));
    assert(std::filesystem::exists(writtenPath));
    assert(error.empty());
    neuracoust::daw::AiMixMemoryEntry drumsMemory;
    drumsMemory.id = "ai-session-drums";
    drumsMemory.projectName = "Rock Session";
    drumsMemory.sourceDescription = "User approved parallel drum compression.";
    drumsMemory.userFeedback = "좋다";
    drumsMemory.tags = {"drums", "rock"};
    const auto memoryResults = neuracoust::daw::searchMixMemoryEntries({memory, drumsMemory}, {"vocal"}, "vocal", 5);
    assert(!memoryResults.empty());
    assert(memoryResults.front().id == "ai-session-vocal-approval");

    return 0;
}
