#include "audio/OfflineBounce.h"
#include "project/ProjectDocument.h"
#include "project/TimelineExport.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace {

struct Args {
    std::filesystem::path projectPath;
    std::filesystem::path outputPath;
    std::string ffmpegPath = "ffmpeg";
    neuracoust::daw::VideoDeliveryPreset preset = neuracoust::daw::VideoDeliveryPreset::YouTube1080p;
    bool keepTemp = false;
};

void printUsage() {
    std::cerr
        << "Usage: neuracoust_video_render_ffmpeg --project INPUT.ndaw --output OUTPUT.mp4 "
        << "[--preset youtube-1080p|youtube-4k|share-preview-720p] [--ffmpeg PATH] [--keep-temp]\n";
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

bool parseArgs(int argc, char** argv, Args& args) {
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i] ? argv[i] : "";
        auto take = [&](std::string& out) -> bool {
            if (i + 1 >= argc) {
                return false;
            }
            out = argv[++i] ? argv[i] : "";
            return true;
        };
        std::string value;
        if (key == "--project") {
            if (!take(value)) return false;
            args.projectPath = value;
        } else if (key == "--output") {
            if (!take(value)) return false;
            args.outputPath = value;
        } else if (key == "--preset") {
            if (!take(value) || !parsePreset(value, args.preset)) return false;
        } else if (key == "--ffmpeg") {
            if (!take(value)) return false;
            args.ffmpegPath = value;
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

std::string readText(const std::filesystem::path& path) {
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

uintmax_t fileSizeOrZero(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    return error ? 0 : size;
}

std::string quoteCommandArg(const std::string& value) {
    std::string out = "\"";
    for (char ch : value) {
#if defined(_WIN32)
        if (ch == '^' || ch == '&' || ch == '|' || ch == '<' || ch == '>' || ch == '(' || ch == ')') {
            out.push_back('^');
            out.push_back(ch);
        } else
#endif
        if (ch == '"') {
#if defined(_WIN32)
            out += "\"\"";
#else
            out += "\\\"";
#endif
        } else {
            out.push_back(ch);
        }
    }
    out.push_back('"');
    return out;
}

std::string ffmpegCommand(const Args& args,
                          const neuracoust::daw::VideoDeliveryPlan& plan,
                          const std::filesystem::path& mixPath) {
    const auto& clip = plan.clips.front();
    std::ostringstream command;
#if defined(_WIN32)
    command << "call ";
#endif
    command << quoteCommandArg(args.ffmpegPath)
            << " -y"
            << " -ss " << clip.sourceOffsetSeconds
            << " -t " << clip.durationSeconds
            << " -i " << quoteCommandArg(clip.sourcePath)
            << " -i " << quoteCommandArg(mixPath.string())
            << " -map 0:v:0 -map 1:a:0"
            << " -c:v libx264 -preset slow"
            << " -b:v " << plan.videoBitrateKbps << "k"
            << " -maxrate " << plan.videoBitrateKbps << "k"
            << " -bufsize " << plan.videoBitrateKbps * 2 << "k"
            << " -vf " << quoteCommandArg("scale=" + std::to_string(plan.width) + ":" + std::to_string(plan.height) +
                                          ":force_original_aspect_ratio=decrease,pad=" +
                                          std::to_string(plan.width) + ":" + std::to_string(plan.height) +
                                          ":-1:-1")
            << " -pix_fmt yuv420p"
            << " -r " << plan.frameRate
            << " -c:a aac -b:a " << plan.audioBitrateKbps << "k"
            << " -movflags +faststart -shortest "
            << quoteCommandArg(args.outputPath.string());
    return command.str();
}

bool writeRenderManifest(const std::filesystem::path& manifestPath,
                         const Args& args,
                         const neuracoust::daw::VideoDeliveryPlan& plan,
                         const neuracoust::daw::BounceResult& bounce,
                         double elapsedSeconds,
                         std::string& error) {
    std::ofstream out(manifestPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "Could not open render manifest.";
        return false;
    }
    const auto outputBytes = fileSizeOrZero(args.outputPath);
    const bool ok = outputBytes > 0;
    out << "{\n";
    out << "  \"ok\": " << (ok ? "true" : "false") << ",\n";
    out << "  \"renderer\": \"ffmpeg\",\n";
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
    out << "  \"outputBytes\": " << outputBytes << ",\n";
    out << "  \"elapsedSeconds\": " << elapsedSeconds << ",\n";
    out << "  \"message\": \"" << (ok ? "FFmpeg MP4 render completed." : "FFmpeg MP4 output was missing or empty.") << "\",\n";
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
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    const auto start = std::chrono::steady_clock::now();
    Args args;
    if (!parseArgs(argc, argv, args)) {
        printUsage();
        return 2;
    }

    std::string error;
    neuracoust::daw::ProjectDocument project;
    if (!neuracoust::daw::deserializeProjectForPath(readText(args.projectPath), args.projectPath, project, error)) {
        std::cerr << "Could not read project: " << error << "\n";
        return 1;
    }
    std::cout << "stage=project_loaded\nprogress=0.050\n";

    std::error_code fsError;
    if (!args.outputPath.parent_path().empty()) {
        std::filesystem::create_directories(args.outputPath.parent_path(), fsError);
    }
    std::filesystem::remove(args.outputPath, fsError);
    const auto tempParent = args.outputPath.has_parent_path()
        ? args.outputPath.parent_path()
        : std::filesystem::temp_directory_path();
    const auto tempRoot = tempParent /
        ("neuracoust-video-render-ffmpeg-" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
            start.time_since_epoch()).count()));
    std::filesystem::create_directories(tempRoot, fsError);
    const auto mixPath = tempRoot / "mix.wav";

    auto bounceProject = project;
    bounceProject.bitDepth = 24;
    neuracoust::daw::BounceOptions bounceOptions;
    const auto bounce = neuracoust::daw::bounceProjectToWav(bounceProject, mixPath.string(), bounceOptions);
    if (!bounce.ok) {
        std::cerr << "Audio bounce failed: " << bounce.message << "\n";
        if (!args.keepTemp) std::filesystem::remove_all(tempRoot, fsError);
        return 1;
    }
    std::cout << "stage=audio_bounced\nprogress=0.250\n";

    const auto plan = neuracoust::daw::makeVideoDeliveryPlan(project, args.preset, args.outputPath, mixPath);
    if (!plan.ok || plan.clips.empty()) {
        std::cerr << "Video delivery plan failed: " << plan.message << "\n";
        if (!args.keepTemp) std::filesystem::remove_all(tempRoot, fsError);
        return 1;
    }
    if (plan.clips.size() > 1) {
        std::cerr << "FFmpeg renderer currently supports the first active video clip only.\n";
        if (!args.keepTemp) std::filesystem::remove_all(tempRoot, fsError);
        return 1;
    }
    std::cout << "stage=delivery_plan_ready\nprogress=0.350\n";

    const auto command = ffmpegCommand(args, plan, mixPath);
    std::cout << "ffmpeg_command=" << command << "\n";
    const int exitCode = std::system(command.c_str());
    if (exitCode != 0) {
        std::cerr << "FFmpeg render failed with exit code " << exitCode << "\n";
        if (!args.keepTemp) std::filesystem::remove_all(tempRoot, fsError);
        return 1;
    }
    const double elapsedSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::string manifestError;
    auto renderManifestPath = args.outputPath;
    renderManifestPath.replace_extension(".render.json");
    if (!writeRenderManifest(renderManifestPath, args, plan, bounce, elapsedSeconds, manifestError)) {
        std::cerr << "Render manifest failed: " << manifestError << "\n";
        if (!args.keepTemp) std::filesystem::remove_all(tempRoot, fsError);
        return 1;
    }
    std::cout << "progress=1.000\n"
              << "output=" << args.outputPath << "\n"
              << "preset=" << plan.presetId << "\n"
              << "clips=" << plan.clips.size() << "\n"
              << "duration_seconds=" << plan.durationSeconds << "\n"
              << "render_manifest=" << renderManifestPath << "\n"
              << "elapsed_seconds=" << elapsedSeconds << "\n";
    if (!args.keepTemp) std::filesystem::remove_all(tempRoot, fsError);
    return 0;
}
