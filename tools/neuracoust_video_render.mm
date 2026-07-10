#include "audio/OfflineBounce.h"
#include "project/ProjectDocument.h"
#include "project/TimelineExport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>

#include <unistd.h>

namespace {

struct RenderArgs {
    std::filesystem::path projectPath;
    std::filesystem::path outputPath;
    neuracoust::daw::VideoDeliveryPreset preset = neuracoust::daw::VideoDeliveryPreset::YouTube1080p;
    bool keepTemp = false;
    double stallTimeoutSeconds = 180.0;
    double segmentSeconds = 180.0;
};

struct OutputValidation {
    bool ok = false;
    double durationSeconds = 0.0;
    int width = 0;
    int height = 0;
    size_t videoTrackCount = 0;
    size_t audioTrackCount = 0;
    std::string message;
};

void printUsage() {
    std::cerr
        << "Usage: neuracoust_video_render --project INPUT.ndaw --output OUTPUT.mp4 "
        << "[--preset youtube-1080p|youtube-4k|share-preview-720p] "
        << "[--segment-seconds SECONDS] [--stall-timeout-seconds SECONDS] [--keep-temp]\n";
}

bool parsePreset(const std::string& value, neuracoust::daw::VideoDeliveryPreset& preset) {
    if (value == "youtube-1080p") {
        preset = neuracoust::daw::VideoDeliveryPreset::YouTube1080p;
        return true;
    }
    if (value == "youtube-4k") {
        preset = neuracoust::daw::VideoDeliveryPreset::YouTube4k;
        return true;
    }
    if (value == "share-preview-720p") {
        preset = neuracoust::daw::VideoDeliveryPreset::SharePreview720p;
        return true;
    }
    return false;
}

bool parseArgs(int argc, char** argv, RenderArgs& args) {
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i] ? argv[i] : "";
        auto takeValue = [&](std::string& out) -> bool {
            if (i + 1 >= argc) {
                return false;
            }
            out = argv[++i] ? argv[i] : "";
            return true;
        };
        std::string value;
        if (key == "--project") {
            if (!takeValue(value)) {
                return false;
            }
            args.projectPath = value;
        } else if (key == "--output") {
            if (!takeValue(value)) {
                return false;
            }
            args.outputPath = value;
        } else if (key == "--preset") {
            if (!takeValue(value) || !parsePreset(value, args.preset)) {
                return false;
            }
        } else if (key == "--stall-timeout-seconds") {
            if (!takeValue(value)) {
                return false;
            }
            args.stallTimeoutSeconds = std::max(5.0, std::atof(value.c_str()));
        } else if (key == "--segment-seconds") {
            if (!takeValue(value)) {
                return false;
            }
            args.segmentSeconds = std::max(10.0, std::atof(value.c_str()));
        } else if (key == "--keep-temp") {
            args.keepTemp = true;
        } else if (key == "--help" || key == "-h") {
            printUsage();
            std::exit(0);
        } else {
            std::cerr << "Unknown argument: " << key << "\n";
            return false;
        }
    }
    return !args.projectPath.empty() && !args.outputPath.empty();
}

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::string jsonEscape(const std::string& value) {
    std::ostringstream out;
    for (unsigned char ch : value) {
        switch (ch) {
            case '\\': out << "\\\\"; break;
            case '"': out << "\\\""; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (ch < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch) << std::dec;
                } else {
                    out << static_cast<char>(ch);
                }
                break;
        }
    }
    return out.str();
}

NSString* presetNameForDelivery(neuracoust::daw::VideoDeliveryPreset preset) {
    switch (preset) {
        case neuracoust::daw::VideoDeliveryPreset::YouTube4k:
            return AVAssetExportPreset3840x2160;
        case neuracoust::daw::VideoDeliveryPreset::SharePreview720p:
            return AVAssetExportPreset1280x720;
        case neuracoust::daw::VideoDeliveryPreset::YouTube1080p:
        default:
            return AVAssetExportPreset1920x1080;
    }
}

NSURL* fileUrl(const std::filesystem::path& path) {
    return [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.string().c_str()]];
}

bool addAudioMixToComposition(AVMutableComposition* composition,
                              const std::filesystem::path& mixPath,
                              double durationSeconds,
                              std::string& error) {
    AVURLAsset* asset = [AVURLAsset URLAssetWithURL:fileUrl(mixPath) options:nil];
    NSArray<AVAssetTrack*>* tracks = [asset tracksWithMediaType:AVMediaTypeAudio];
    if (tracks.count == 0) {
        error = "Rendered audio mix did not contain an audio track.";
        return false;
    }
    AVMutableCompositionTrack* audioTrack = [composition addMutableTrackWithMediaType:AVMediaTypeAudio
                                                                    preferredTrackID:kCMPersistentTrackID_Invalid];
    CMTime duration = CMTimeMakeWithSeconds(std::max(0.001, durationSeconds), 600);
    NSError* nsError = nil;
    if (![audioTrack insertTimeRange:CMTimeRangeMake(kCMTimeZero, duration)
                             ofTrack:tracks.firstObject
                              atTime:kCMTimeZero
                               error:&nsError]) {
        error = nsError ? nsError.localizedDescription.UTF8String : "Could not insert rendered audio mix.";
        return false;
    }
    return true;
}

bool addVideoCutsToComposition(AVMutableComposition* composition,
                               const neuracoust::daw::VideoDeliveryPlan& plan,
                               std::string& error) {
    AVMutableCompositionTrack* videoTrack = [composition addMutableTrackWithMediaType:AVMediaTypeVideo
                                                                    preferredTrackID:kCMPersistentTrackID_Invalid];
    bool insertedAny = false;
    for (const auto& clip : plan.clips) {
        AVURLAsset* asset = [AVURLAsset URLAssetWithURL:fileUrl(clip.sourcePath) options:nil];
        NSArray<AVAssetTrack*>* tracks = [asset tracksWithMediaType:AVMediaTypeVideo];
        if (tracks.count == 0) {
            error = "Video source has no video track: " + clip.sourcePath;
            return false;
        }
        AVAssetTrack* sourceTrack = tracks.firstObject;
        if (!insertedAny) {
            videoTrack.preferredTransform = sourceTrack.preferredTransform;
        }
        CMTime sourceStart = CMTimeMakeWithSeconds(std::max(0.0, clip.sourceOffsetSeconds), 600);
        CMTime duration = CMTimeMakeWithSeconds(std::max(0.001, clip.durationSeconds), 600);
        CMTime timelineStart = CMTimeMakeWithSeconds(std::max(0.0, clip.timelineStartSeconds), 600);
        NSError* nsError = nil;
        if (![videoTrack insertTimeRange:CMTimeRangeMake(sourceStart, duration)
                                 ofTrack:sourceTrack
                                  atTime:timelineStart
                                   error:&nsError]) {
            error = nsError ? nsError.localizedDescription.UTF8String : "Could not insert video clip.";
            return false;
        }
        insertedAny = true;
    }
    if (!insertedAny) {
        error = "No active video clips to render.";
        return false;
    }
    return true;
}

NSString* compatiblePresetForComposition(AVAsset* composition, NSString* requestedPreset) {
    NSArray<NSString*>* presets = [AVAssetExportSession exportPresetsCompatibleWithAsset:composition];
    if ([presets containsObject:requestedPreset]) {
        return requestedPreset;
    }
    if ([presets containsObject:AVAssetExportPresetHighestQuality]) {
        return AVAssetExportPresetHighestQuality;
    }
    return presets.count > 0 ? presets.firstObject : requestedPreset;
}

AVMutableVideoComposition* makeVideoCompositionForDelivery(AVMutableComposition* composition,
                                                           const neuracoust::daw::VideoDeliveryPlan& plan) {
    NSArray<AVAssetTrack*>* tracks = [composition tracksWithMediaType:AVMediaTypeVideo];
    if (tracks.count == 0 || plan.width <= 0 || plan.height <= 0) {
        return nil;
    }
    AVAssetTrack* track = tracks.firstObject;
    CGSize transformedSize = CGSizeApplyAffineTransform(track.naturalSize, track.preferredTransform);
    const double sourceWidth = std::max(1.0, static_cast<double>(std::abs(transformedSize.width)));
    const double sourceHeight = std::max(1.0, static_cast<double>(std::abs(transformedSize.height)));
    const double scale = std::min(static_cast<double>(plan.width) / sourceWidth,
                                  static_cast<double>(plan.height) / sourceHeight);
    const double translateX = (static_cast<double>(plan.width) - sourceWidth * scale) * 0.5;
    const double translateY = (static_cast<double>(plan.height) - sourceHeight * scale) * 0.5;

    AVMutableVideoCompositionLayerInstruction* layer =
        [AVMutableVideoCompositionLayerInstruction videoCompositionLayerInstructionWithAssetTrack:track];
    CGAffineTransform transform = track.preferredTransform;
    transform = CGAffineTransformScale(transform, scale, scale);
    transform = CGAffineTransformTranslate(transform, translateX / std::max(0.0001, scale), translateY / std::max(0.0001, scale));
    [layer setTransform:transform atTime:kCMTimeZero];

    AVMutableVideoCompositionInstruction* instruction = [AVMutableVideoCompositionInstruction videoCompositionInstruction];
    instruction.timeRange = CMTimeRangeMake(kCMTimeZero, composition.duration);
    instruction.layerInstructions = @[layer];

    AVMutableVideoComposition* videoComposition = [AVMutableVideoComposition videoComposition];
    videoComposition.instructions = @[instruction];
    videoComposition.renderSize = CGSizeMake(plan.width, plan.height);
    const auto frameTimescale = std::max<int32_t>(1, static_cast<int32_t>(std::llround(plan.frameRate * 1000.0)));
    videoComposition.frameDuration = CMTimeMake(1000, frameTimescale);
    return videoComposition;
}

bool exportAssetToMp4(AVAsset* asset,
                      const std::filesystem::path& outputPath,
                      NSString* requestedPreset,
                      AVVideoComposition* videoComposition,
                      CMTimeRange timeRange,
                      double stallTimeoutSeconds,
                      const std::string& progressLabel,
                      std::string& error) {
    NSString* preset = compatiblePresetForComposition(asset, requestedPreset);
    AVAssetExportSession* exporter = [[AVAssetExportSession alloc] initWithAsset:asset presetName:preset];
    if (exporter == nil) {
        error = "Could not create AVAssetExportSession.";
        return false;
    }
    exporter.outputURL = fileUrl(outputPath);
    exporter.outputFileType = AVFileTypeMPEG4;
    exporter.shouldOptimizeForNetworkUse = YES;
    exporter.timeRange = timeRange;
    if (videoComposition != nil) {
        exporter.videoComposition = videoComposition;
    }

    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [exporter exportAsynchronouslyWithCompletionHandler:^{
        dispatch_semaphore_signal(done);
    }];

    auto lastProgressTime = std::chrono::steady_clock::now();
    auto lastPrintTime = lastProgressTime - std::chrono::seconds(1);
    float lastProgress = -1.0f;
    bool completed = false;
    while (!completed) {
        if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 250 * NSEC_PER_MSEC)) == 0) {
            completed = true;
            break;
        }

        const float progress = exporter.progress;
        const auto now = std::chrono::steady_clock::now();
        if (progress > lastProgress + 0.0005f) {
            lastProgress = progress;
            lastProgressTime = now;
        }
        if (now - lastPrintTime >= std::chrono::seconds(1)) {
            if (!progressLabel.empty()) {
                std::cout << "stage=" << progressLabel << "\n";
            }
            std::cout << "progress=" << std::fixed << std::setprecision(3) << progress << "\n";
            std::cout.flush();
            lastPrintTime = now;
        }
        if (stallTimeoutSeconds > 0.0 &&
            std::chrono::duration<double>(now - lastProgressTime).count() > stallTimeoutSeconds &&
            progress < 0.999f) {
            [exporter cancelExport];
            error = "MP4 export stalled with no progress for " + std::to_string(stallTimeoutSeconds) +
                " seconds at progress=" + std::to_string(progress) + ".";
            dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
            return false;
        }
    }

    if (exporter.status != AVAssetExportSessionStatusCompleted) {
        error = exporter.error ? exporter.error.localizedDescription.UTF8String : "MP4 export failed.";
        return false;
    }
    std::cout << "progress=1.000\n";
    std::cout.flush();
    return true;
}

bool concatenateMp4Chunks(const std::vector<std::filesystem::path>& chunkPaths,
                          const std::filesystem::path& outputPath,
                          double stallTimeoutSeconds,
                          std::string& error) {
    AVMutableComposition* joined = [AVMutableComposition composition];
    AVMutableCompositionTrack* joinedVideo = [joined addMutableTrackWithMediaType:AVMediaTypeVideo
                                                                 preferredTrackID:kCMPersistentTrackID_Invalid];
    AVMutableCompositionTrack* joinedAudio = [joined addMutableTrackWithMediaType:AVMediaTypeAudio
                                                                 preferredTrackID:kCMPersistentTrackID_Invalid];
    CMTime cursor = kCMTimeZero;
    bool hasVideo = false;
    bool hasAudio = false;
    for (const auto& chunkPath : chunkPaths) {
        AVURLAsset* asset = [AVURLAsset URLAssetWithURL:fileUrl(chunkPath) options:nil];
        NSArray<AVAssetTrack*>* videoTracks = [asset tracksWithMediaType:AVMediaTypeVideo];
        NSArray<AVAssetTrack*>* audioTracks = [asset tracksWithMediaType:AVMediaTypeAudio];
        if (videoTracks.count == 0) {
            error = "Rendered MP4 chunk has no video track: " + chunkPath.string();
            return false;
        }
        const CMTimeRange range = CMTimeRangeMake(kCMTimeZero, asset.duration);
        NSError* nsError = nil;
        if (![joinedVideo insertTimeRange:range ofTrack:videoTracks.firstObject atTime:cursor error:&nsError]) {
            error = nsError ? nsError.localizedDescription.UTF8String : "Could not append rendered video chunk.";
            return false;
        }
        if (!hasVideo) {
            joinedVideo.preferredTransform = videoTracks.firstObject.preferredTransform;
        }
        hasVideo = true;
        if (audioTracks.count > 0) {
            nsError = nil;
            if (![joinedAudio insertTimeRange:range ofTrack:audioTracks.firstObject atTime:cursor error:&nsError]) {
                error = nsError ? nsError.localizedDescription.UTF8String : "Could not append rendered audio chunk.";
                return false;
            }
            hasAudio = true;
        }
        cursor = CMTimeAdd(cursor, asset.duration);
    }
    if (!hasVideo || !hasAudio) {
        error = "Rendered MP4 chunks did not contain both video and audio.";
        return false;
    }
    return exportAssetToMp4(joined,
                            outputPath,
                            AVAssetExportPresetPassthrough,
                            nil,
                            CMTimeRangeMake(kCMTimeZero, joined.duration),
                            stallTimeoutSeconds,
                            "mp4_concat",
                            error);
}

bool exportCompositionToMp4(AVMutableComposition* composition,
                            const std::filesystem::path& outputPath,
                            const std::filesystem::path& tempRoot,
                            NSString* requestedPreset,
                            AVVideoComposition* videoComposition,
                            double segmentSeconds,
                            double stallTimeoutSeconds,
                            std::string& error) {
    const double durationSeconds = CMTimeGetSeconds(composition.duration);
    if (!std::isfinite(durationSeconds) || durationSeconds <= segmentSeconds + 0.001) {
        return exportAssetToMp4(composition,
                                outputPath,
                                requestedPreset,
                                videoComposition,
                                CMTimeRangeMake(kCMTimeZero, composition.duration),
                                stallTimeoutSeconds,
                                "mp4_export",
                                error);
    }

    std::vector<std::filesystem::path> chunkPaths;
    const int segmentCount = static_cast<int>(std::ceil(durationSeconds / segmentSeconds));
    for (int index = 0; index < segmentCount; ++index) {
        const double startSeconds = static_cast<double>(index) * segmentSeconds;
        const double chunkDurationSeconds = std::min(segmentSeconds, durationSeconds - startSeconds);
        if (chunkDurationSeconds <= 0.001) {
            continue;
        }
        const auto chunkPath = tempRoot / ("segment-" + std::to_string(index + 1) + ".mp4");
        std::cout << "stage=mp4_segment_export\n"
                  << "segment=" << (index + 1) << "/" << segmentCount << "\n";
        if (!exportAssetToMp4(composition,
                              chunkPath,
                              requestedPreset,
                              videoComposition,
                              CMTimeRangeMake(CMTimeMakeWithSeconds(startSeconds, 600),
                                              CMTimeMakeWithSeconds(chunkDurationSeconds, 600)),
                              stallTimeoutSeconds,
                              "mp4_segment_export",
                              error)) {
            return false;
        }
        chunkPaths.push_back(chunkPath);
    }
    if (chunkPaths.empty()) {
        error = "Segmented MP4 export produced no chunks.";
        return false;
    }
    std::cout << "stage=mp4_segments_concat\n";
    return concatenateMp4Chunks(chunkPaths, outputPath, stallTimeoutSeconds, error);
}

OutputValidation validateOutputMovie(const std::filesystem::path& outputPath,
                                     double expectedDurationSeconds,
                                     int expectedWidth,
                                     int expectedHeight) {
    OutputValidation result;
    AVURLAsset* asset = [AVURLAsset URLAssetWithURL:fileUrl(outputPath) options:nil];
    NSArray<AVAssetTrack*>* videoTracks = [asset tracksWithMediaType:AVMediaTypeVideo];
    NSArray<AVAssetTrack*>* audioTracks = [asset tracksWithMediaType:AVMediaTypeAudio];
    result.videoTrackCount = static_cast<size_t>(videoTracks.count);
    result.audioTrackCount = static_cast<size_t>(audioTracks.count);
    result.durationSeconds = CMTimeGetSeconds(asset.duration);
    if (videoTracks.count > 0) {
        AVAssetTrack* videoTrack = videoTracks.firstObject;
        CGSize size = CGSizeApplyAffineTransform(videoTrack.naturalSize, videoTrack.preferredTransform);
        result.width = static_cast<int>(std::llround(std::abs(size.width)));
        result.height = static_cast<int>(std::llround(std::abs(size.height)));
    }
    if (result.videoTrackCount == 0) {
        result.message = "Output MP4 has no video track.";
        return result;
    }
    if (result.audioTrackCount == 0) {
        result.message = "Output MP4 has no audio track.";
        return result;
    }
    if (!std::isfinite(result.durationSeconds) || result.durationSeconds <= 0.0) {
        result.message = "Output MP4 has invalid duration.";
        return result;
    }
    if (expectedDurationSeconds > 0.0 && std::abs(result.durationSeconds - expectedDurationSeconds) > 0.75) {
        result.message = "Output MP4 duration differs from timeline.";
        return result;
    }
    if (expectedWidth > 0 && expectedHeight > 0 &&
        (std::abs(result.width - expectedWidth) > 2 || std::abs(result.height - expectedHeight) > 2)) {
        result.message = "Output MP4 dimensions differ from delivery preset.";
        return result;
    }
    result.ok = true;
    result.message = "Output MP4 validation passed.";
    return result;
}

bool writeRenderManifest(const std::filesystem::path& manifestPath,
                         const RenderArgs& args,
                         const neuracoust::daw::VideoDeliveryPlan& plan,
                         const neuracoust::daw::BounceResult& bounce,
                         const OutputValidation& validation,
                         double elapsedSeconds,
                         std::string& error) {
    std::ofstream out(manifestPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "Could not open render manifest.";
        return false;
    }
    out << "{\n";
    out << "  \"ok\": " << (validation.ok ? "true" : "false") << ",\n";
    out << "  \"projectPath\": \"" << jsonEscape(args.projectPath.string()) << "\",\n";
    out << "  \"outputPath\": \"" << jsonEscape(args.outputPath.string()) << "\",\n";
    out << "  \"presetId\": \"" << jsonEscape(plan.presetId) << "\",\n";
    out << "  \"presetName\": \"" << jsonEscape(plan.presetName) << "\",\n";
    out << "  \"requestedWidth\": " << plan.width << ",\n";
    out << "  \"requestedHeight\": " << plan.height << ",\n";
    out << "  \"videoBitrateKbps\": " << plan.videoBitrateKbps << ",\n";
    out << "  \"audioBitrateKbps\": " << plan.audioBitrateKbps << ",\n";
    out << "  \"timelineDurationSeconds\": " << std::fixed << std::setprecision(6) << plan.durationSeconds << ",\n";
    out << "  \"bounceDurationSeconds\": " << bounce.durationSeconds << ",\n";
    out << "  \"outputDurationSeconds\": " << validation.durationSeconds << ",\n";
    out << "  \"outputWidth\": " << validation.width << ",\n";
    out << "  \"outputHeight\": " << validation.height << ",\n";
    out << "  \"videoTracks\": " << validation.videoTrackCount << ",\n";
    out << "  \"audioTracks\": " << validation.audioTrackCount << ",\n";
    out << "  \"elapsedSeconds\": " << elapsedSeconds << ",\n";
    out << "  \"message\": \"" << jsonEscape(validation.message) << "\",\n";
    out << "  \"clips\": [\n";
    for (size_t index = 0; index < plan.clips.size(); ++index) {
        const auto& clip = plan.clips[index];
        out << "    {\"clipId\": \"" << jsonEscape(clip.clipId)
            << "\", \"sourcePath\": \"" << jsonEscape(clip.sourcePath)
            << "\", \"timelineStartSeconds\": " << clip.timelineStartSeconds
            << ", \"sourceOffsetSeconds\": " << clip.sourceOffsetSeconds
            << ", \"durationSeconds\": " << clip.durationSeconds << "}";
        if (index + 1 < plan.clips.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
    if (!out) {
        error = "Could not write render manifest.";
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        const auto start = std::chrono::steady_clock::now();
        RenderArgs args;
        if (!parseArgs(argc, argv, args)) {
            printUsage();
            return 2;
        }

        std::string error;
        neuracoust::daw::ProjectDocument project;
        const auto projectText = readTextFile(args.projectPath);
        if (projectText.empty() ||
            !neuracoust::daw::deserializeProjectForPath(projectText, args.projectPath, project, error)) {
            std::cerr << "Could not read project: " << error << "\n";
            return 1;
        }
        std::cout << "stage=project_loaded\nprogress=0.050\n";

        std::error_code fsError;
        if (!args.outputPath.parent_path().empty()) {
            std::filesystem::create_directories(args.outputPath.parent_path(), fsError);
            if (fsError) {
                std::cerr << "Could not create output directory: " << fsError.message() << "\n";
                return 1;
            }
        }
        std::filesystem::remove(args.outputPath, fsError);

        const auto tempParent = args.outputPath.has_parent_path()
            ? args.outputPath.parent_path()
            : std::filesystem::temp_directory_path();
        const auto tempRoot = tempParent /
            (".neuracoust-video-render-" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                start.time_since_epoch()).count()) + "-" + std::to_string(static_cast<long long>(getpid())));
        std::filesystem::create_directories(tempRoot, fsError);
        if (fsError) {
            std::cerr << "Could not create temporary directory: " << fsError.message() << "\n";
            return 1;
        }

        const auto mixPath = tempRoot / "mix.wav";
        auto bounceProject = project;
        bounceProject.bitDepth = 24;
        neuracoust::daw::BounceOptions bounceOptions;
        bounceOptions.renderMode = neuracoust::daw::BounceRenderMode::Offline;
        bounceOptions.rangeMode = neuracoust::daw::BounceRangeMode::FullProject;
        const auto bounce = neuracoust::daw::bounceProjectToWav(bounceProject, mixPath.string(), bounceOptions);
        if (!bounce.ok) {
            std::cerr << "Audio bounce failed: " << bounce.message << "\n";
            if (!args.keepTemp) {
                std::filesystem::remove_all(tempRoot, fsError);
            }
            return 1;
        }
        std::cout << "stage=audio_bounced\nprogress=0.250\n";

        const auto plan = neuracoust::daw::makeVideoDeliveryPlan(project, args.preset, args.outputPath, mixPath);
        if (!plan.ok) {
            std::cerr << "Video delivery plan failed: " << plan.message << "\n";
            if (!args.keepTemp) {
                std::filesystem::remove_all(tempRoot, fsError);
            }
            return 1;
        }
        std::cout << "stage=delivery_plan_ready\nprogress=0.300\n";

        AVMutableComposition* composition = [AVMutableComposition composition];
        if (!addVideoCutsToComposition(composition, plan, error) ||
            !addAudioMixToComposition(composition, mixPath, std::max(plan.durationSeconds, bounce.durationSeconds), error)) {
            std::cerr << "Composition failed: " << error << "\n";
            if (!args.keepTemp) {
                std::filesystem::remove_all(tempRoot, fsError);
            }
            return 1;
        }
        std::cout << "stage=composition_ready\nprogress=0.350\n";

        AVMutableVideoComposition* videoComposition = makeVideoCompositionForDelivery(composition, plan);
        if (!exportCompositionToMp4(composition,
                                    args.outputPath,
                                    tempRoot,
                                    presetNameForDelivery(args.preset),
                                    videoComposition,
                                    args.segmentSeconds,
                                    args.stallTimeoutSeconds,
                                    error)) {
            std::cerr << "MP4 render failed: " << error << "\n";
            if (!args.keepTemp) {
                std::filesystem::remove_all(tempRoot, fsError);
            }
            return 1;
        }
        std::cout << "stage=mp4_exported\n";

        const double elapsedSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const auto validation = validateOutputMovie(args.outputPath, std::max(plan.durationSeconds, bounce.durationSeconds), plan.width, plan.height);
        std::string manifestError;
        auto renderManifestPath = args.outputPath;
        renderManifestPath.replace_extension(".render.json");
        if (!writeRenderManifest(renderManifestPath, args, plan, bounce, validation, elapsedSeconds, manifestError)) {
            std::cerr << "Render manifest failed: " << manifestError << "\n";
            if (!args.keepTemp) {
                std::filesystem::remove_all(tempRoot, fsError);
            }
            return 1;
        }
        if (!validation.ok) {
            std::cerr << "Output validation failed: " << validation.message << "\n";
            if (!args.keepTemp) {
                std::filesystem::remove_all(tempRoot, fsError);
            }
            return 1;
        }
        std::cout << "output=" << args.outputPath << "\n"
                  << "preset=" << plan.presetId << "\n"
                  << "clips=" << plan.clips.size() << "\n"
                  << "duration_seconds=" << plan.durationSeconds << "\n"
                  << "output_duration_seconds=" << validation.durationSeconds << "\n"
                  << "output_size=" << validation.width << "x" << validation.height << "\n"
                  << "render_manifest=" << renderManifestPath << "\n"
                  << "audio_mix=" << mixPath << "\n"
                  << "elapsed_seconds=" << elapsedSeconds << "\n";
        if (!args.keepTemp) {
            std::filesystem::remove_all(tempRoot, fsError);
        }
        return 0;
    }
}
