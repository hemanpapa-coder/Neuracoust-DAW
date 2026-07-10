#include "plugins/Vst3ModuleRuntime.h"
#include "audio/WavFile.h"
#include <chrono>
#include <climits>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace neuracoust::daw {

Vst3ModuleProbeResult probeVst3ModuleFactory(const Vst3PluginDescriptor& descriptor) {
    Vst3ModuleProbeResult result;
    if (descriptor.executablePath.empty() || !std::filesystem::exists(descriptor.executablePath)) {
        result.message = "VST3 executable path is missing.";
        return result;
    }

#if defined(_WIN32)
    HMODULE module = LoadLibraryA(descriptor.executablePath.c_str());
    if (module == nullptr) {
        result.message = "Could not load VST3 module.";
        return result;
    }
    result.opened = true;
    result.hasFactory = GetProcAddress(module, result.factorySymbol.c_str()) != nullptr;
    FreeLibrary(module);
#else
    void* module = dlopen(descriptor.executablePath.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (module == nullptr) {
        const char* error = dlerror();
        result.message = error != nullptr ? error : "Could not load VST3 module.";
        return result;
    }
    result.opened = true;
    dlerror();
    result.hasFactory = dlsym(module, result.factorySymbol.c_str()) != nullptr;
    dlclose(module);
#endif

    result.message = result.hasFactory
        ? "VST3 module opened and factory symbol was found."
        : "VST3 module opened, but factory symbol was not found.";
    return result;
}

std::string defaultVst3ProcessWorkerPath() {
    if (const char* env = std::getenv("NEURACOUST_VST3_PROCESS_WORKER")) {
        if (env[0] != '\0') {
            return env;
        }
    }
#if defined(_WIN32)
    char buffer[MAX_PATH] {};
    const DWORD length = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    const auto dir = std::filesystem::path(buffer).parent_path();
    const auto worker = dir / "neuracoust_vst3_process_worker.exe";
    return std::filesystem::exists(worker) ? worker.string() : std::string();
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size + 1, 0);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return {};
    }
    std::error_code error;
    const auto executable = std::filesystem::weakly_canonical(std::filesystem::path(buffer.data()), error);
    const auto dir = (error ? std::filesystem::path(buffer.data()) : executable).parent_path();
    for (const auto& candidate : {
             dir / "neuracoust_vst3_process_worker",
             dir / "Neuracoust VST3 Process Worker",
             dir.parent_path().parent_path().parent_path() / "neuracoust_vst3_process_worker"}) {
        if (std::filesystem::exists(candidate)) {
            return candidate.string();
        }
    }
    return {};
#else
    char buffer[PATH_MAX] {};
    const ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length <= 0) {
        return {};
    }
    buffer[length] = '\0';
    const auto worker = std::filesystem::path(buffer).parent_path() / "neuracoust_vst3_process_worker";
    return std::filesystem::exists(worker) ? worker.string() : std::string();
#endif
}

Vst3IsolatedProcessResult processStereoBufferWithIsolatedVst3(const std::string& workerExecutablePath,
                                                              const Vst3PluginDescriptor& descriptor,
                                                              WavAudioData& audio,
                                                              int maxBlockSize,
                                                              int timeoutSeconds) {
    Vst3IsolatedProcessResult result;
    if (workerExecutablePath.empty() || !std::filesystem::exists(workerExecutablePath)) {
        result.message = "Isolated VST3 worker executable is missing.";
        return result;
    }
    if (descriptor.bundlePath.empty() && descriptor.executablePath.empty()) {
        result.message = "VST3 plugin path is missing.";
        return result;
    }
    if (audio.channels != 2 || audio.sampleRate <= 0 || audio.interleavedSamples.empty()) {
        result.message = "Isolated VST3 processing requires a non-empty stereo buffer.";
        return result;
    }

    const auto tempRoot = std::filesystem::temp_directory_path();
    const auto unique = std::to_string(static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto inputPath = tempRoot / ("neuracoust-vst3-worker-in-" + unique + ".wav");
    const auto outputPath = tempRoot / ("neuracoust-vst3-worker-out-" + unique + ".wav");

    std::string wavError;
    if (!writeFloat32WavFile(inputPath, audio, wavError)) {
        result.message = "Could not write isolated VST3 worker input WAV: " + wavError;
        return result;
    }

    auto cleanup = [&]() {
        std::error_code ignored;
        std::filesystem::remove(inputPath, ignored);
        std::filesystem::remove(outputPath, ignored);
    };

#if defined(_WIN32)
    const std::string command = "\"" + workerExecutablePath + "\" --plugin \"" +
        (descriptor.bundlePath.empty() ? descriptor.executablePath : descriptor.bundlePath) +
        "\" --name \"" + descriptor.name + "\" --input \"" + inputPath.string() +
        "\" --output \"" + outputPath.string() + "\" --max-block " +
        std::to_string(maxBlockSize > 0 ? maxBlockSize : 256);
    const int status = std::system(command.c_str());
    result.exitStatus = status;
    if (status != 0) {
        result.message = "Isolated VST3 worker failed with status " + std::to_string(status) + ".";
        cleanup();
        return result;
    }
#else
    std::vector<std::string> argStorage {
        workerExecutablePath,
        "--plugin",
        descriptor.bundlePath.empty() ? descriptor.executablePath : descriptor.bundlePath,
        "--name",
        descriptor.name,
        "--input",
        inputPath.string(),
        "--output",
        outputPath.string(),
        "--max-block",
        std::to_string(maxBlockSize > 0 ? maxBlockSize : 256)
    };
    std::vector<char*> args;
    args.reserve(argStorage.size() + 1);
    for (auto& arg : argStorage) {
        args.push_back(arg.data());
    }
    args.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        result.message = "Could not fork isolated VST3 worker.";
        cleanup();
        return result;
    }
    if (pid == 0) {
        setenv("NEURACOUST_ALLOW_UNSAFE_INPROCESS_VST3", "1", 1);
        const int devNull = open("/dev/null", O_WRONLY);
        if (devNull >= 0) {
            dup2(devNull, STDERR_FILENO);
            close(devNull);
        }
        execv(workerExecutablePath.c_str(), args.data());
        _exit(127);
    }

    int status = 0;
    const int safeTimeoutSeconds = timeoutSeconds > 0 ? timeoutSeconds : 20;
    for (int waited = 0; waited <= safeTimeoutSeconds * 10; ++waited) {
        const pid_t waitResult = waitpid(pid, &status, WNOHANG);
        if (waitResult == pid) {
            break;
        }
        if (waitResult < 0) {
            result.message = "Isolated VST3 worker wait failed.";
            cleanup();
            return result;
        }
        if (waited == safeTimeoutSeconds * 10) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            result.message = "Isolated VST3 worker timed out after " + std::to_string(safeTimeoutSeconds) + " seconds.";
            cleanup();
            return result;
        }
        usleep(100000);
    }
    result.exitStatus = status;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (WIFSIGNALED(status)) {
            result.message = "Isolated VST3 worker terminated by signal " + std::to_string(WTERMSIG(status)) + ".";
        } else {
            result.message = "Isolated VST3 worker failed with status " + std::to_string(status) + ".";
        }
        cleanup();
        return result;
    }
#endif

    WavAudioData processed;
    if (!readPcmWavFile(outputPath, processed, wavError)) {
        result.message = "Could not read isolated VST3 worker output WAV: " + wavError;
        cleanup();
        return result;
    }
    if (processed.channels != 2 || processed.sampleRate != audio.sampleRate || processed.interleavedSamples.empty()) {
        result.message = "Isolated VST3 worker returned an incompatible output WAV.";
        cleanup();
        return result;
    }
    audio = std::move(processed);
    result.processed = true;
    result.message = "Isolated VST3 worker processed stereo buffer.";
    cleanup();
    return result;
}

} // namespace neuracoust::daw
