#include "audio/ProjectAudioRenderer.h"
#include "audio/OfflineBounce.h"
#include "audio/WavFile.h"
#include "project/AudioImportAnalysis.h"
#include "project/EditOperations.h"
#include "project/ProjectDocument.h"

#include <algorithm>
#include <fstream>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::filesystem::path homeDesktopWav(const char* filename) {
    const char* home = std::getenv("HOME");
    if (home == nullptr || std::string(home).empty()) {
        return {};
    }
    return std::filesystem::path(home) / "Desktop" / filename;
}

bool allFinite(const std::vector<float>& samples) {
    return std::all_of(samples.begin(), samples.end(), [](float sample) {
        return std::isfinite(sample);
    });
}

std::filesystem::path workflowTempRoot() {
    auto root = std::filesystem::temp_directory_path() / "neuracoust-daw-desktop-wav-workflow";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

float peakAbs(const std::vector<float>& samples) {
    float peak = 0.0f;
    for (const auto sample : samples) {
        peak = std::max(peak, std::abs(sample));
    }
    return peak;
}

} // namespace

int main() {
    const auto favelaPath = homeDesktopWav("Lee Ritenour & Eric Marienthal _ Favela.wav");
    const auto mojavePath = homeDesktopWav("Yellowjackets & Lee Ritenour _ Mojave.wav");
    if (favelaPath.empty() || !std::filesystem::exists(favelaPath) || !std::filesystem::exists(mojavePath)) {
        std::cout << "Desktop WAV workflow smoke skipped: expected sample WAV files are not on ~/Desktop\n";
        return 0;
    }

    std::string error;
    neuracoust::daw::WavAudioData favela;
    neuracoust::daw::WavAudioData mojave;
    if (!neuracoust::daw::readPcmWavFile(favelaPath.string(), favela, error) || favela.frameCount() <= 0) {
        std::cerr << "Could not read Favela desktop WAV: " << error << "\n";
        return 2;
    }
    if (!neuracoust::daw::readPcmWavFile(mojavePath.string(), mojave, error) || mojave.frameCount() <= 0) {
        std::cerr << "Could not read Mojave desktop WAV: " << error << "\n";
        return 3;
    }

    auto project = neuracoust::daw::defaultProject();
    project.sampleRate = favela.sampleRate > 0 ? favela.sampleRate : 48000.0;
    project.bitDepth = 32;
    project.tempoBpm = 120;

    const double favelaDuration = std::min(12.0, static_cast<double>(favela.frameCount()) / static_cast<double>(favela.sampleRate));
    const double mojaveDuration = std::min(12.0, static_cast<double>(mojave.frameCount()) / static_cast<double>(mojave.sampleRate));
    auto analysisProject = neuracoust::daw::defaultProject();
    const auto analysisSummary = neuracoust::daw::analyzeImportedAudioIntoProject(analysisProject,
                                                                                  favela,
                                                                                  0.0,
                                                                                  favelaDuration,
                                                                                  favela.embeddedTempoBpm > 0.0);
    if (analysisProject.tempoBpm < 55 ||
        analysisProject.tempoBpm > 190 ||
        analysisProject.tempoMap.empty() ||
        analysisProject.timeSignatureNumerator < 1 ||
        (analysisProject.timeSignatureDenominator != 4 && analysisProject.timeSignatureDenominator != 8) ||
        analysisProject.markers.empty() ||
        analysisSummary.find("BPM") == std::string::npos ||
        analysisSummary.find("bar(s)") == std::string::npos) {
        std::cerr << "Desktop WAV musical import analysis did not produce usable tempo, meter, and marker metadata: "
                  << analysisSummary << "\n";
        return 31;
    }
    if (analysisProject.detectedKey.empty() ||
        (analysisProject.detectedKeyMode != "major" &&
         analysisProject.detectedKeyMode != "minor" &&
         analysisProject.detectedKeyMode != "unknown")) {
        std::cerr << "Desktop WAV musical import analysis produced invalid key metadata\n";
        return 32;
    }
    const auto favelaClipId = neuracoust::daw::appendAudioClipAt(project, "Audio 1", favelaPath.string(), 0.0, favelaDuration);
    const auto mojaveClipId = neuracoust::daw::appendAudioClipAt(project, "Audio 2", mojavePath.string(), 0.0, mojaveDuration);
    if (favelaClipId.empty() || mojaveClipId.empty()) {
        std::cerr << "Could not append desktop WAV clips to the project\n";
        return 4;
    }
    if (!neuracoust::daw::setClipRegionName(project, favelaClipId, "Favela Edit A") ||
        project.clips.front().sourceFileUid.rfind("src-", 0) != 0) {
        std::cerr << "Could not assign desktop WAV clip region metadata\n";
        return 4;
    }

    std::string splitId;
    if (!neuracoust::daw::splitClip(project, favelaClipId, 4.0, splitId) || splitId.empty()) {
        std::cerr << "Could not split the Favela clip at the edit point\n";
        return 5;
    }
    const auto splitLeftClip = std::find_if(project.clips.begin(), project.clips.end(), [&](const neuracoust::daw::ClipState& clip) {
        return clip.id == favelaClipId;
    });
    const auto splitRightClip = std::find_if(project.clips.begin(), project.clips.end(), [&](const neuracoust::daw::ClipState& clip) {
        return clip.id == splitId;
    });
    if (splitLeftClip == project.clips.end() ||
        splitRightClip == project.clips.end() ||
        splitLeftClip->sourcePath != splitRightClip->sourcePath ||
        splitLeftClip->sourceFileUid != splitRightClip->sourceFileUid ||
        std::abs(splitLeftClip->durationSeconds - 4.0) > 0.0001 ||
        std::abs(splitRightClip->startSeconds - 4.0) > 0.0001 ||
        std::abs(splitRightClip->sourceOffsetSeconds - 4.0) > 0.0001) {
        std::cerr << "Playhead split did not preserve the desktop WAV as a non-destructive source reference\n";
        return 5;
    }
    double editBoundarySeconds = 0.0;
    if (!neuracoust::daw::setEditSelectionToAdjacentClipBoundary(project, 3.2, true, editBoundarySeconds, "Audio 1") ||
        std::abs(editBoundarySeconds - 4.0) > 0.0001 ||
        !project.editSelectionEnabled ||
        std::abs(project.editSelectionStartSeconds - 3.2) > 0.0001 ||
        std::abs(project.editSelectionEndSeconds - 4.0) > 0.0001) {
        std::cerr << "Desktop WAV clip boundary selection did not follow the selected track edit point\n";
        return 5;
    }
    auto liveTrimProject = project;
    if (!neuracoust::daw::trimClipStart(liveTrimProject, splitId, 4.25) ||
        !neuracoust::daw::trimClipEnd(liveTrimProject, favelaClipId, 3.50)) {
        std::cerr << "Could not apply live-playhead style trim edits to the desktop WAV split\n";
        return 5;
    }
    const auto trimmedRightClip = std::find_if(liveTrimProject.clips.begin(), liveTrimProject.clips.end(), [&](const neuracoust::daw::ClipState& clip) {
        return clip.id == splitId;
    });
    const auto trimmedLeftClip = std::find_if(liveTrimProject.clips.begin(), liveTrimProject.clips.end(), [&](const neuracoust::daw::ClipState& clip) {
        return clip.id == favelaClipId;
    });
    if (trimmedRightClip == liveTrimProject.clips.end() ||
        trimmedLeftClip == liveTrimProject.clips.end() ||
        trimmedRightClip->sourcePath != splitRightClip->sourcePath ||
        trimmedRightClip->sourceFileUid != splitRightClip->sourceFileUid ||
        std::abs(trimmedRightClip->startSeconds - 4.25) > 0.0001 ||
        std::abs(trimmedRightClip->sourceOffsetSeconds - 4.25) > 0.0001 ||
        std::abs(trimmedLeftClip->durationSeconds - 3.50) > 0.0001) {
        std::cerr << "Live-playhead trim edits did not stay non-destructive and time-aligned\n";
        return 5;
    }
    if (!neuracoust::daw::moveClip(project, splitId, 3.75)) {
        std::cerr << "Could not move the split clip into an overlap region\n";
        return 6;
    }
    if (!neuracoust::daw::nudgeClip(project, splitId, 0.10) ||
        !neuracoust::daw::nudgeClip(project, splitId, -0.10)) {
        std::cerr << "Could not nudge the edited desktop WAV clip\n";
        return 6;
    }
    if (!neuracoust::daw::applyAutomaticClipCrossfades(project, splitId)) {
        std::cerr << "Expected overlap crossfade was not applied\n";
        return 7;
    }
    std::vector<std::string> separatedIds;
    if (!neuracoust::daw::separateClipRange(project, 0.5, 1.5, separatedIds) || separatedIds.empty()) {
        std::cerr << "Could not separate the desktop WAV loop range boundaries\n";
        return 7;
    }
    std::vector<std::string> duplicatedRangeIds;
    if (!neuracoust::daw::duplicateClipRange(project, 0.0, 2.0, duplicatedRangeIds) || duplicatedRangeIds.empty()) {
        std::cerr << "Could not duplicate the edited desktop WAV loop range\n";
        return 7;
    }
    auto cutPasteProject = project;
    std::vector<neuracoust::daw::ClipState> cutRangeClipboard;
    if (!neuracoust::daw::cutClipRange(cutPasteProject, 0.25, 0.75, cutRangeClipboard) ||
        cutRangeClipboard.empty()) {
        std::cerr << "Could not non-destructively cut the desktop WAV edit range\n";
        return 7;
    }
    std::vector<std::string> pastedRangeIds;
    if (!neuracoust::daw::pasteClipRange(cutPasteProject, cutRangeClipboard, 6.5, pastedRangeIds) ||
        pastedRangeIds.empty()) {
        std::cerr << "Could not paste the non-destructive desktop WAV edit range\n";
        return 7;
    }
    const auto pastedRangeClip = std::find_if(cutPasteProject.clips.begin(), cutPasteProject.clips.end(), [&](const neuracoust::daw::ClipState& clip) {
        return std::find(pastedRangeIds.begin(), pastedRangeIds.end(), clip.id) != pastedRangeIds.end();
    });
    if (pastedRangeClip == cutPasteProject.clips.end() ||
        pastedRangeClip->sourcePath != cutRangeClipboard.front().sourcePath ||
        pastedRangeClip->sourceFileUid != cutRangeClipboard.front().sourceFileUid ||
        std::abs(pastedRangeClip->sourceOffsetSeconds - cutRangeClipboard.front().sourceOffsetSeconds) > 0.0001) {
        std::cerr << "Pasted edit range did not remain a virtual non-destructive source reference\n";
        return 7;
    }
    auto shuffleDeleteProject = project;
    if (!neuracoust::daw::shuffleDeleteClipRange(shuffleDeleteProject, 0.25, 0.75) ||
        shuffleDeleteProject.clips.empty()) {
        std::cerr << "Could not shuffle-delete the desktop WAV edit range\n";
        return 7;
    }
    auto trimmedSelectionProject = project;
    std::vector<std::string> trimmedSelectionIds;
    if (!neuracoust::daw::trimClipRangeToSelection(trimmedSelectionProject, 0.25, 1.75, trimmedSelectionIds) ||
        trimmedSelectionIds.empty()) {
        std::cerr << "Could not trim the desktop WAV arrangement to an edit selection\n";
        return 7;
    }
    if (std::any_of(trimmedSelectionProject.clips.begin(), trimmedSelectionProject.clips.end(), [](const neuracoust::daw::ClipState& clip) {
            return clip.startSeconds < 0.25 - 0.0001 || clip.startSeconds + clip.durationSeconds > 1.75 + 0.0001;
        })) {
        std::cerr << "Trimmed desktop WAV selection left clips outside the selected range\n";
        return 7;
    }
    if (!neuracoust::daw::nudgeClip(project, duplicatedRangeIds.front(), 0.07)) {
        std::cerr << "Could not prepare an off-grid desktop WAV clip for quantize\n";
        return 7;
    }
    std::vector<std::string> quantizedIds;
    if (!neuracoust::daw::quantizeClipStartsInRange(project, 0.0, 6.0, 0.25, quantizedIds)) {
        std::cerr << "Could not quantize desktop WAV clip starts in the edit range\n";
        return 7;
    }
    std::vector<std::string> rangeGainIds;
    if (!neuracoust::daw::adjustClipGainInRange(project, 0.5, 1.5, 2.0f, rangeGainIds) || rangeGainIds.empty()) {
        std::cerr << "Could not apply clip gain to the selected desktop WAV range\n";
        return 7;
    }
    const auto gainedRangeClip = std::find_if(project.clips.begin(), project.clips.end(), [&](const neuracoust::daw::ClipState& clip) {
        return std::find(rangeGainIds.begin(), rangeGainIds.end(), clip.id) != rangeGainIds.end() &&
            clip.startSeconds >= 0.5 - 0.0001 &&
            clip.startSeconds + clip.durationSeconds <= 1.5 + 0.0001 &&
            std::abs(clip.gainDb - 2.0f) < 0.0001f;
    });
    if (gainedRangeClip == project.clips.end()) {
        std::cerr << "Desktop WAV range gain did not target the selected clip segment\n";
        return 7;
    }
    std::vector<std::string> rangeMuteIds;
    if (!neuracoust::daw::setClipMutedInRange(project, 0.75, 1.25, true, rangeMuteIds) || rangeMuteIds.empty()) {
        std::cerr << "Could not mute the selected desktop WAV range\n";
        return 7;
    }
    const auto mutedRangeClip = std::find_if(project.clips.begin(), project.clips.end(), [&](const neuracoust::daw::ClipState& clip) {
        return std::find(rangeMuteIds.begin(), rangeMuteIds.end(), clip.id) != rangeMuteIds.end() &&
            clip.startSeconds >= 0.75 - 0.0001 &&
            clip.startSeconds + clip.durationSeconds <= 1.25 + 0.0001 &&
            clip.muted;
    });
    if (mutedRangeClip == project.clips.end()) {
        std::cerr << "Desktop WAV range mute did not target the selected clip segment\n";
        return 7;
    }
    if (!neuracoust::daw::setClipMutedInRange(project, 0.75, 1.25, false, rangeMuteIds) || rangeMuteIds.empty()) {
        std::cerr << "Could not unmute the selected desktop WAV range\n";
        return 7;
    }
    std::vector<std::string> normalizedRangeIds;
    std::string normalizeMessage;
    if (!neuracoust::daw::normalizeClipGainInRange(project, 1.25, 1.75, -6.0f, normalizedRangeIds, normalizeMessage) ||
        normalizedRangeIds.empty()) {
        std::cerr << "Could not normalize the selected desktop WAV range: " << normalizeMessage << "\n";
        return 7;
    }
    const auto normalizedRangeClip = std::find_if(project.clips.begin(), project.clips.end(), [&](const neuracoust::daw::ClipState& clip) {
        return std::find(normalizedRangeIds.begin(), normalizedRangeIds.end(), clip.id) != normalizedRangeIds.end() &&
            clip.startSeconds >= 1.25 - 0.0001 &&
            clip.startSeconds + clip.durationSeconds <= 1.75 + 0.0001 &&
            std::isfinite(clip.gainDb);
    });
    if (normalizedRangeClip == project.clips.end()) {
        std::cerr << "Desktop WAV range normalize did not target the selected clip segment\n";
        return 7;
    }

    const auto left = std::find_if(project.clips.begin(), project.clips.end(), [&](const neuracoust::daw::ClipState& clip) {
        return clip.id == favelaClipId;
    });
    const auto right = std::find_if(project.clips.begin(), project.clips.end(), [&](const neuracoust::daw::ClipState& clip) {
        return clip.id == splitId;
    });
    if (left == project.clips.end() || right == project.clips.end() ||
        std::abs(left->fadeOutSeconds - 0.25) > 0.0001 ||
        std::abs(right->fadeInSeconds - 0.25) > 0.0001) {
        std::cerr << "Desktop WAV crossfade values are not aligned with the overlap\n";
        return 8;
    }

	    if (!neuracoust::daw::setTrackVolumeDb(project, "Audio 1", -3.0f) ||
	        !neuracoust::daw::setTrackPan(project, "Audio 2", 0.25f) ||
	        !neuracoust::daw::setClipGainDb(project, mojaveClipId, -6.0f)) {
	        std::cerr << "Could not apply basic mix edits to desktop WAV workflow\n";
	        return 9;
	    }
	    std::string crossTrackVirtualCopyId;
	    if (!neuracoust::daw::duplicateClipToTrack(project, favelaClipId, 2.0, "Audio 2", crossTrackVirtualCopyId) ||
	        crossTrackVirtualCopyId.empty()) {
	        std::cerr << "Could not create a non-destructive cross-track clip copy\n";
	        return 9;
	    }
	    const auto crossTrackVirtualCopy = std::find_if(project.clips.begin(), project.clips.end(), [&](const neuracoust::daw::ClipState& clip) {
	        return clip.id == crossTrackVirtualCopyId;
	    });
	    const auto favelaSource = std::find_if(project.clips.begin(), project.clips.end(), [&](const neuracoust::daw::ClipState& clip) {
	        return clip.id == favelaClipId;
	    });
	    if (crossTrackVirtualCopy == project.clips.end() ||
	        favelaSource == project.clips.end() ||
	        crossTrackVirtualCopy->trackName != "Audio 2" ||
	        crossTrackVirtualCopy->sourcePath != favelaSource->sourcePath ||
	        crossTrackVirtualCopy->sourceFileUid != favelaSource->sourceFileUid) {
	        std::cerr << "Cross-track clip copy is not a virtual non-destructive source reference\n";
	        return 9;
	    }
	    std::string duplicatedTrackName;
    std::vector<std::string> duplicatedTrackClipIds;
    if (!neuracoust::daw::duplicateTrackWithClips(project, "Audio 1", duplicatedTrackName, duplicatedTrackClipIds) ||
        duplicatedTrackName.empty() || duplicatedTrackClipIds.empty()) {
        std::cerr << "Could not duplicate the edited desktop WAV track\n";
        return 9;
    }

    neuracoust::daw::ProjectAudioRenderPlan plan;
    if (!neuracoust::daw::makeProjectAudioRenderPlan(project, plan, error)) {
        std::cerr << "Could not create desktop WAV render plan: " << error << "\n";
        return 10;
    }
    if (plan.hasMissingMedia || plan.clips.size() != project.clips.size()) {
        std::cerr << "Desktop WAV render plan has missing media or dropped clips\n";
        return 11;
    }

	    std::vector<float> introBlock;
	    neuracoust::daw::ProjectAudioBlockMeters introMeters;
	    neuracoust::daw::renderProjectAudioBlockWithMeters(plan, 0, static_cast<int64_t>(project.sampleRate), introBlock, &introMeters);
	    if (introBlock.size() != static_cast<size_t>(project.sampleRate) * 2 || !allFinite(introBlock) || peakAbs(introBlock) <= 0.0001f) {
	        std::cerr << "Desktop WAV intro render block is silent, invalid, or wrong size\n";
	        return 12;
	    }
	    if (introMeters.trackNames.size() != introMeters.trackPeakLeft.size() ||
	        introMeters.trackNames.size() != introMeters.trackPeakRight.size()) {
	        std::cerr << "Desktop WAV track meter arrays are not aligned\n";
	        return 12;
	    }
	    auto meterPeakForTrack = [&](const std::string& trackName) {
	        for (size_t index = 0; index < introMeters.trackNames.size(); ++index) {
	            if (introMeters.trackNames[index] == trackName) {
	                return std::max(introMeters.trackPeakLeft[index], introMeters.trackPeakRight[index]);
	            }
	        }
	        return 0.0f;
	    };
	    if (meterPeakForTrack("Audio 1") <= 0.0001f ||
	        meterPeakForTrack("Audio 2") <= 0.0001f ||
	        meterPeakForTrack(duplicatedTrackName) <= 0.0001f) {
	        std::cerr << "Desktop WAV per-track meters did not report signal for active tracks\n";
	        return 12;
	    }

    std::vector<float> overlapBlock;
    const auto overlapStartFrame = static_cast<int64_t>(3.80 * project.sampleRate);
    neuracoust::daw::renderProjectAudioBlock(plan, overlapStartFrame, 4096, overlapBlock);
    if (overlapBlock.size() != 8192 || !allFinite(overlapBlock) || peakAbs(overlapBlock) <= 0.0001f) {
        std::cerr << "Desktop WAV overlap render block is silent or invalid\n";
        return 13;
    }

    const auto tempRoot = workflowTempRoot();
    const auto projectPath = tempRoot / "Desktop WAV Workflow.ndaw";
    if (!neuracoust::daw::saveProjectFileWithBackup(project, projectPath, error) || !std::filesystem::exists(projectPath)) {
        std::cerr << "Could not save desktop WAV workflow project: " << error << "\n";
        return 14;
    }
    {
        std::ifstream savedProject(projectPath, std::ios::binary);
        std::string savedProjectText((std::istreambuf_iterator<char>(savedProject)), std::istreambuf_iterator<char>());
        if (savedProjectText.find("Favela Edit A") == std::string::npos ||
            savedProjectText.find("Yellowjackets & Lee Ritenour _ Mojave.wav") == std::string::npos ||
            savedProjectText.find("\"trackType\":\"audio\"") == std::string::npos) {
            std::cerr << "Saved desktop WAV workflow project is missing expected timeline metadata\n";
            return 15;
        }
    }

    auto collectedProject = project;
    const auto collectReport = neuracoust::daw::collectProjectMedia(collectedProject, projectPath);
    if (collectReport.failedClips != 0 ||
        collectReport.missingClips != 0 ||
        collectReport.copiedClips < 2 ||
        !std::filesystem::exists(tempRoot / "Audio Files")) {
        std::cerr << "Desktop WAV workflow did not collect external media into the project Audio Files folder\n";
        return 151;
    }
    const auto collectedProjectPath = tempRoot / "Desktop WAV Workflow Collected.ndaw";
    if (!neuracoust::daw::saveProjectFileWithBackup(collectedProject, collectedProjectPath, error) ||
        !std::filesystem::exists(collectedProjectPath)) {
        std::cerr << "Could not save collected desktop WAV workflow project: " << error << "\n";
        return 152;
    }
    {
        std::ifstream savedProject(collectedProjectPath, std::ios::binary);
        std::string savedProjectText((std::istreambuf_iterator<char>(savedProject)), std::istreambuf_iterator<char>());
        if (savedProjectText.find("Audio Files/") == std::string::npos ||
            savedProjectText.find("/Desktop/") != std::string::npos) {
            std::cerr << "Collected desktop WAV project did not save portable Audio Files media paths\n";
            return 153;
        }
    }
    neuracoust::daw::ProjectDocument collectedRoundTrip;
    {
        std::ifstream savedProject(collectedProjectPath, std::ios::binary);
        std::string savedProjectText((std::istreambuf_iterator<char>(savedProject)), std::istreambuf_iterator<char>());
        if (!neuracoust::daw::deserializeProjectForPath(savedProjectText, collectedProjectPath, collectedRoundTrip, error)) {
            std::cerr << "Could not reload collected desktop WAV workflow project: " << error << "\n";
            return 154;
        }
    }
    neuracoust::daw::ProjectAudioRenderPlan collectedPlan;
    if (!neuracoust::daw::makeProjectAudioRenderPlan(collectedRoundTrip, collectedPlan, error) ||
        collectedPlan.hasMissingMedia ||
        collectedPlan.clips.size() != collectedProject.clips.size()) {
        std::cerr << "Collected desktop WAV workflow project did not reload with resolvable media\n";
        return 155;
    }

    auto bounceProject = project;
    bounceProject.editSelectionEnabled = true;
    bounceProject.editSelectionStartSeconds = 0.0;
    bounceProject.editSelectionEndSeconds = 6.0;
    neuracoust::daw::BounceOptions bounceOptions;
    bounceOptions.rangeMode = neuracoust::daw::BounceRangeMode::EditSelection;
    const auto bouncePath = (tempRoot / "Desktop WAV Workflow Bounce.wav").string();
    const auto bounce = neuracoust::daw::bounceProjectToWav(bounceProject, bouncePath, bounceOptions);
    if (!bounce.ok || !std::filesystem::exists(bouncePath) || bounce.levelStats.nearSilent || bounce.levelStats.clippingDetected) {
        std::cerr << "Desktop WAV workflow bounce failed or produced invalid levels: " << bounce.message << "\n";
        return 16;
    }
    neuracoust::daw::WavAudioData bounced;
    if (!neuracoust::daw::readPcmWavFile(bouncePath, bounced, error) ||
        bounced.channels != 2 ||
        bounced.frameCount() <= 0 ||
        !allFinite(bounced.interleavedSamples) ||
        peakAbs(bounced.interleavedSamples) <= 0.0001f) {
        std::cerr << "Desktop WAV workflow bounced WAV could not be read or is silent: " << error << "\n";
        return 17;
    }
    if (bounce.manifestPath.empty() || !std::filesystem::exists(bounce.manifestPath)) {
        std::cerr << "Desktop WAV workflow bounce did not write a manifest\n";
        return 18;
    }

    const auto stemsDir = tempRoot / "Stems";
    const auto stemExport = neuracoust::daw::exportProjectTrackStems(project, stemsDir);
    if (!stemExport.ok || stemExport.exportedStems < 2 || stemExport.outputPaths.size() != stemExport.exportedStems) {
        std::cerr << "Desktop WAV workflow stem export failed: " << stemExport.message << "\n";
        return 19;
    }
    size_t readableNonSilentStems = 0;
    for (const auto& stemPath : stemExport.outputPaths) {
        neuracoust::daw::WavAudioData stem;
        if (!std::filesystem::exists(stemPath) || !neuracoust::daw::readPcmWavFile(stemPath, stem, error) || stem.channels != 2 || stem.frameCount() <= 0) {
            std::cerr << "Desktop WAV workflow stem could not be read: " << stemPath << " " << error << "\n";
            return 20;
        }
        if (allFinite(stem.interleavedSamples) && peakAbs(stem.interleavedSamples) > 0.0001f) {
            ++readableNonSilentStems;
        }
    }
    if (readableNonSilentStems < 2 || stemExport.manifestPath.empty() || !std::filesystem::exists(stemExport.manifestPath)) {
        std::cerr << "Desktop WAV workflow stems were unexpectedly silent or missing a manifest\n";
        return 21;
    }

    std::cout << "Desktop WAV workflow smoke passed with " << project.clips.size()
              << " edited clips; artifacts: " << tempRoot.string() << "\n";
    return 0;
}
