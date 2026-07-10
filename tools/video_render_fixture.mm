#include "audio/RecordingTake.h"
#include "project/EditOperations.h"
#include "project/ProjectDocument.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <cstdlib>
#include <vector>

#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

namespace {

bool writeTextFile(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    return static_cast<bool>(out);
}

bool createToneWav(const std::filesystem::path& path, double durationSeconds) {
    neuracoust::daw::RecordingTake take(2, 48000);
    std::vector<int16_t> samples(static_cast<size_t>(std::max(1.0, durationSeconds) * 48000.0) * 2);
    for (size_t frame = 0; frame < samples.size() / 2; ++frame) {
        const double phase = static_cast<double>(frame) * 440.0 * 6.283185307179586 / 48000.0;
        const auto value = static_cast<int16_t>(std::sin(phase) * 10000.0);
        samples[frame * 2] = value;
        samples[frame * 2 + 1] = value;
    }
    take.appendInterleavedInt16(samples.data(), static_cast<int>(samples.size() / 2));
    std::string error;
    return take.saveWav(path.string(), error);
}

bool fillBuffer(CVPixelBufferRef buffer, int frameIndex) {
    CVPixelBufferLockBaseAddress(buffer, 0);
    auto* base = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
    const size_t bytesPerRow = CVPixelBufferGetBytesPerRow(buffer);
    const size_t width = CVPixelBufferGetWidth(buffer);
    const size_t height = CVPixelBufferGetHeight(buffer);
    for (size_t y = 0; y < height; ++y) {
        auto* row = base + y * bytesPerRow;
        for (size_t x = 0; x < width; ++x) {
            const uint8_t r = static_cast<uint8_t>((x + frameIndex * 8) % 256);
            const uint8_t g = static_cast<uint8_t>((y * 2 + frameIndex * 5) % 256);
            const uint8_t b = static_cast<uint8_t>((80 + frameIndex * 3) % 256);
            row[x * 4 + 0] = b;
            row[x * 4 + 1] = g;
            row[x * 4 + 2] = r;
            row[x * 4 + 3] = 255;
        }
    }
    CVPixelBufferUnlockBaseAddress(buffer, 0);
    return true;
}

bool createVideo(const std::filesystem::path& path, double frameRate, double durationSeconds) {
    std::filesystem::create_directories(path.parent_path());
    std::error_code fsError;
    std::filesystem::remove(path, fsError);

    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.string().c_str()]];
    NSError* error = nil;
    AVAssetWriter* writer = [[AVAssetWriter alloc] initWithURL:url fileType:AVFileTypeQuickTimeMovie error:&error];
    if (writer == nil) {
        std::cerr << "Could not create video writer: " << (error ? error.localizedDescription.UTF8String : "unknown") << "\n";
        return false;
    }

    NSDictionary* settings = @{
        AVVideoCodecKey: AVVideoCodecTypeH264,
        AVVideoWidthKey: @640,
        AVVideoHeightKey: @360
    };
    AVAssetWriterInput* input = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:settings];
    input.expectsMediaDataInRealTime = NO;
    NSDictionary* attributes = @{
        (NSString*)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
        (NSString*)kCVPixelBufferWidthKey: @640,
        (NSString*)kCVPixelBufferHeightKey: @360
    };
    AVAssetWriterInputPixelBufferAdaptor* adaptor =
        [AVAssetWriterInputPixelBufferAdaptor assetWriterInputPixelBufferAdaptorWithAssetWriterInput:input
                                                                         sourcePixelBufferAttributes:attributes];
    if (![writer canAddInput:input]) {
        std::cerr << "Could not add video writer input.\n";
        return false;
    }
    [writer addInput:input];
    if (![writer startWriting]) {
        std::cerr << "Could not start video writer.\n";
        return false;
    }
    [writer startSessionAtSourceTime:kCMTimeZero];

    const int frameCount = std::max(1, static_cast<int>(std::round(frameRate * durationSeconds)));
    const int timeScale = std::max(1, static_cast<int>(std::round(frameRate * 1000.0)));
    for (int frame = 0; frame < frameCount; ++frame) {
        while (!input.readyForMoreMediaData) {
            [NSThread sleepForTimeInterval:0.005];
        }
        CVPixelBufferRef buffer = nullptr;
        if (CVPixelBufferPoolCreatePixelBuffer(nullptr, adaptor.pixelBufferPool, &buffer) != kCVReturnSuccess || buffer == nullptr) {
            std::cerr << "Could not allocate video frame.\n";
            return false;
        }
        fillBuffer(buffer, frame);
        const CMTime time = CMTimeMake(frame * 1000, timeScale);
        if (![adaptor appendPixelBuffer:buffer withPresentationTime:time]) {
            CVPixelBufferRelease(buffer);
            std::cerr << "Could not append video frame.\n";
            return false;
        }
        CVPixelBufferRelease(buffer);
    }
    [input markAsFinished];
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [writer finishWritingWithCompletionHandler:^{
        dispatch_semaphore_signal(done);
    }];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    if (writer.status != AVAssetWriterStatusCompleted) {
        std::cerr << "Video writer failed: " << (writer.error ? writer.error.localizedDescription.UTF8String : "unknown") << "\n";
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        std::filesystem::path outDir = std::filesystem::temp_directory_path() / "neuracoust-video-render-fixture";
        double frameRate = 24.0;
        bool multiCut = false;
        double durationSeconds = 2.0;
        for (int index = 1; index < argc; ++index) {
            const std::string arg = argv[index] ? argv[index] : "";
            if (arg == "--frame-rate" && index + 1 < argc) {
                frameRate = std::atof(argv[++index]);
            } else if (arg == "--duration" && index + 1 < argc) {
                durationSeconds = std::max(0.1, std::atof(argv[++index]));
            } else if (arg == "--multi-cut") {
                multiCut = true;
            } else {
                outDir = arg;
            }
        }
        std::filesystem::create_directories(outDir);

        const auto audioPath = outDir / "Audio Files" / "tone.wav";
        const auto videoPath = outDir / "Video Files" / "picture.mov";
        const auto projectPath = outDir / "Video Render Fixture.ndaw";
        if (!createToneWav(audioPath, durationSeconds) || !createVideo(videoPath, frameRate, durationSeconds)) {
            return 2;
        }

        auto project = neuracoust::daw::defaultProject();
        project.name = "Video Render Fixture";
        project.sampleRate = 48000.0;
        project.bitDepth = 24;
        project.videoFrameRate = frameRate;
        const auto audioClipId = neuracoust::daw::appendAudioClipAt(project, "Audio 1", audioPath.string(), 0.0, durationSeconds);
        neuracoust::daw::setClipRegionName(project, audioClipId, "Tone");
        const auto videoClipId = neuracoust::daw::appendVideoReferenceClip(project, videoPath.string(), 0.0, multiCut ? durationSeconds * 0.5 : durationSeconds, frameRate, 640, 360, false);
        if (videoClipId.empty()) {
            std::cerr << "Could not append video clip.\n";
            return 3;
        }
        if (multiCut) {
            project.videoClips.push_back({"video-clip-2", project.videoClips.front().sourceId, "Picture Cut 2", durationSeconds * 0.5, durationSeconds * 0.5, durationSeconds * 0.5, 0.0, false, false});
        }
        const auto text = neuracoust::daw::serializeProjectForPath(project, projectPath);
        if (!writeTextFile(projectPath, text)) {
            std::cerr << "Could not write project.\n";
            return 4;
        }

        std::cout << "fixtureDir=" << outDir << "\n"
                  << "project=" << projectPath << "\n"
                  << "audio=" << audioPath << "\n"
                  << "video=" << videoPath << "\n";
        return 0;
    }
}
