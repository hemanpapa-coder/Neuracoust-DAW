#include "audio/RecordingTake.h"
#include "project/EditOperations.h"
#include "project/ProjectDocument.h"
#include "project/TimelineExport.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool writeTextFile(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    return static_cast<bool>(out);
}

bool writeBytesFile(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

bool createWav(const std::filesystem::path& path) {
    neuracoust::daw::RecordingTake take(2, 48000);
    std::vector<int16_t> samples(48000 * 2 * 3);
    for (size_t frame = 0; frame < samples.size() / 2; ++frame) {
        const double phase = static_cast<double>(frame) * 440.0 * 6.283185307179586 / 48000.0;
        const auto value = static_cast<int16_t>(std::sin(phase) * 9000.0);
        samples[frame * 2] = value;
        samples[frame * 2 + 1] = value;
    }
    take.appendInterleavedInt16(samples.data(), static_cast<int>(samples.size() / 2));
    std::string error;
    return take.saveWav(path.string(), error);
}

} // namespace

int main(int argc, char** argv) {
    const std::filesystem::path outDir = argc > 1
        ? std::filesystem::path(argv[1])
        : std::filesystem::temp_directory_path() / "neuracoust-daw-external-roundtrip";
    std::filesystem::create_directories(outDir);

    const auto wavPath = outDir / "Session Take 01.wav";
    if (!createWav(wavPath)) {
        std::cerr << "Could not create fixture WAV: " << wavPath << "\n";
        return 2;
    }

    auto project = neuracoust::daw::defaultProject();
    project.name = "Neuracoust External Roundtrip";
    project.sampleRate = 48000.0;
    project.bitDepth = 64;
    project.timecodeStartSeconds = 3600.0;
    project.videoFrameRate = 29.97;
    project.timecodeDropFrame = true;
    project.editMode = "Grid";
    project.gridUnit = "1 beat";
    project.timeSignatureNumerator = 3;
    project.timeSignatureDenominator = 4;
    project.timeSignatureMap = {{0.0, 3, 4}, {16.0, 6, 8}};
    project.grooveFeel = "triplet";
    project.grooveSwingAmount = 0.333;
    project.detectedKey = "G";
    project.detectedKeyMode = "major";
    project.tempoBpm = 120.0;
    project.tempoMap = {{0.0, 120.0}, {12.0, 126.5}};
    project.markers.push_back({"roundtrip-marker", "Verse 1", 1.25});
    project.chordEvents.push_back({"roundtrip-chord", "Verse / Cmaj7", 1.5});
    project.lyricEvents.push_back({"roundtrip-lyric", "Hello Neuracoust", 1.75});

    const auto clipId = neuracoust::daw::appendAudioClipAt(project, "Audio 1", wavPath.string(), 2.0, 1.0);
    neuracoust::daw::setClipRegionName(project, clipId, "Verse Guitar");
    neuracoust::daw::setClipGainDb(project, clipId, -3.0f);
    neuracoust::daw::setClipFades(project, clipId, 0.1, 0.2);
    neuracoust::daw::setClipFadeCurves(project, clipId, "linear", "fast");
    for (auto& clip : project.clips) {
        if (clip.id == clipId) {
            clip.sourceHasBroadcastTimeReference = true;
            clip.sourceTimeReferenceSamples = 96000;
            clip.sourceTimeReferenceSeconds = 2.0;
            clip.sourceTempoBpm = 117.6;
            clip.sourceTimeSignatureNumerator = 6;
            clip.sourceTimeSignatureDenominator = 8;
            clip.sourceGrooveFeel = "shuffle";
            clip.sourceGrooveSwingAmount = 0.58;
        }
    }

    const auto fcpxml = neuracoust::daw::exportProjectToFcpxml(project);
    const auto edl = neuracoust::daw::exportProjectToCmx3600Edl(project, 29.97);
    const auto resolveFcpxml = neuracoust::daw::exportProjectToFcpxml(project, neuracoust::daw::TimelineInterchangeProfile::DaVinciResolve);
    const auto resolveEdl = neuracoust::daw::exportProjectToCmx3600Edl(project, 29.97, neuracoust::daw::TimelineInterchangeProfile::DaVinciResolve);
    const auto aaf = neuracoust::daw::exportProjectToAafReference(project);
    const auto midi = neuracoust::daw::exportProjectTempoMapToMidi(project, 480);
    if (!fcpxml.ok || !edl.ok || !resolveFcpxml.ok || !resolveEdl.ok || !aaf.ok || !midi.ok) {
        std::cerr << "Fixture export failed.\n";
        return 3;
    }

    const auto fcpxmlPath = outDir / "Neuracoust External Roundtrip.fcpxml";
    const auto edlPath = outDir / "Neuracoust External Roundtrip.edl";
    const auto resolveFcpxmlPath = outDir / "Neuracoust External Roundtrip Resolve.fcpxml";
    const auto resolveEdlPath = outDir / "Neuracoust External Roundtrip Resolve.edl";
    const auto aafPath = outDir / "Neuracoust External Roundtrip.aaf";
    const auto midiPath = outDir / "Neuracoust External Roundtrip.mid";
    if (!writeTextFile(fcpxmlPath, fcpxml.text) ||
        !writeTextFile(edlPath, edl.text) ||
        !writeTextFile(resolveFcpxmlPath, resolveFcpxml.text) ||
        !writeTextFile(resolveEdlPath, resolveEdl.text) ||
        !writeTextFile(aafPath, aaf.text) ||
        !writeBytesFile(midiPath, midi.data)) {
        std::cerr << "Could not write one or more interchange files.\n";
        return 4;
    }

    std::cout << "fixtureDir=" << outDir << "\n"
              << "wav=" << wavPath << "\n"
              << "fcpxml=" << fcpxmlPath << "\n"
              << "edl=" << edlPath << "\n"
              << "resolveFcpxml=" << resolveFcpxmlPath << "\n"
              << "resolveEdl=" << resolveEdlPath << "\n"
              << "aafReference=" << aafPath << "\n"
              << "midi=" << midiPath << "\n";
    return 0;
}
