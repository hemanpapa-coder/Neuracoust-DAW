#include "audio/WavFile.h"
#include "plugins/Vst3HostFoundation.h"
#include "plugins/Vst3SdkAdapter.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

struct Options {
    bool refresh = false;
    bool process = true;
    bool noHeader = false;
    bool listOnly = false;
    int limit = 0;
    int timeoutSeconds = 20;
    std::string filter;
    std::string path;
    std::string name;
    std::string brand;
    std::string componentClassCid;
    std::string componentClassName;
};

void printUsage(const char* argv0) {
    std::cout
        << "Usage: " << argv0 << " [--refresh] [--no-process] [--list-only] [--limit N] [--timeout SECONDS] [--filter TEXT] [--path /path/plugin.vst3]\n"
        << "\n"
        << "Audits VST3 host stages for Neuracoust DAW.\n";
}

bool parseInt(const std::string& text, int& out) {
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || value < 0 || value > 100000) {
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

Options parseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            std::exit(0);
        }
        if (arg == "--refresh") {
            options.refresh = true;
        } else if (arg == "--no-process") {
            options.process = false;
        } else if (arg == "--list-only") {
            options.listOnly = true;
        } else if (arg == "--no-header") {
            options.noHeader = true;
        } else if (arg == "--limit" && i + 1 < argc) {
            if (!parseInt(argv[++i], options.limit)) {
                std::cerr << "Invalid --limit value.\n";
                std::exit(2);
            }
        } else if (arg == "--timeout" && i + 1 < argc) {
            if (!parseInt(argv[++i], options.timeoutSeconds) || options.timeoutSeconds <= 0) {
                std::cerr << "Invalid --timeout value.\n";
                std::exit(2);
            }
        } else if (arg == "--filter" && i + 1 < argc) {
            options.filter = argv[++i];
        } else if (arg == "--path" && i + 1 < argc) {
            options.path = argv[++i];
        } else if (arg == "--name" && i + 1 < argc) {
            options.name = argv[++i];
        } else if (arg == "--brand" && i + 1 < argc) {
            options.brand = argv[++i];
        } else if (arg == "--class-cid" && i + 1 < argc) {
            options.componentClassCid = argv[++i];
        } else if (arg == "--class-name" && i + 1 < argc) {
            options.componentClassName = argv[++i];
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            printUsage(argv[0]);
            std::exit(2);
        }
    }
    return options;
}

void reexecWithAbsoluteArgvIfNeeded(int argc, char** argv) {
#if !defined(_WIN32)
    if (argc <= 0 || argv == nullptr || argv[0] == nullptr || std::getenv("NEURACOUST_VST3_AUDIT_ABS_EXEC") != nullptr) {
        return;
    }
    const std::filesystem::path executable(argv[0]);
    if (executable.is_absolute()) {
        return;
    }
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(executable, ec);
    if (ec || absolute.empty()) {
        return;
    }
    setenv("NEURACOUST_VST3_AUDIT_ABS_EXEC", "1", 1);
    std::vector<char*> args;
    args.reserve(static_cast<size_t>(argc) + 1);
    std::string absoluteText = absolute.string();
    args.push_back(absoluteText.data());
    for (int index = 1; index < argc; ++index) {
        args.push_back(argv[index]);
    }
    args.push_back(nullptr);
    execv(absoluteText.c_str(), args.data());
#else
    (void)argc;
    (void)argv;
#endif
}

std::string shellQuote(const std::string& value) {
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out += "'";
    return out;
}

std::string yesNo(bool value) {
    return value ? "yes" : "no";
}

std::string sanitizeCell(std::string value) {
    std::replace(value.begin(), value.end(), '\t', ' ');
    std::replace(value.begin(), value.end(), '\n', ' ');
    std::replace(value.begin(), value.end(), '\r', ' ');
    return value;
}

neuracoust::daw::WavAudioData makeProbeAudio() {
    neuracoust::daw::WavAudioData audio;
    audio.channels = 2;
    audio.sampleRate = 48000;
    audio.interleavedSamples.resize(512 * 2, 0.0f);
    for (int frame = 0; frame < 512; ++frame) {
        const float value = (frame % 64) < 32 ? 0.05f : -0.05f;
        audio.interleavedSamples[static_cast<size_t>(frame) * 2] = value;
        audio.interleavedSamples[static_cast<size_t>(frame) * 2 + 1] = value;
    }
    return audio;
}

std::vector<neuracoust::daw::Vst3PluginDescriptor> collectPlugins(const Options& options) {
    if (!options.path.empty()) {
        auto descriptor = neuracoust::daw::describeVst3PluginBundle(options.path);
        if (!options.name.empty()) {
            descriptor.name = options.name;
        }
        if (!options.brand.empty()) {
            descriptor.brand = options.brand;
            descriptor.vendor = options.brand;
        }
        if (!options.componentClassCid.empty()) {
            descriptor.componentClassCid = options.componentClassCid;
        }
        if (!options.componentClassName.empty()) {
            descriptor.componentClassName = options.componentClassName;
        }
        return {descriptor};
    }

    auto plugins = neuracoust::daw::scanVst3PluginBundles(
        options.refresh ? neuracoust::daw::Vst3ScanMode::Refresh : neuracoust::daw::Vst3ScanMode::UseCache);
    if (!options.filter.empty()) {
        std::vector<neuracoust::daw::Vst3PluginDescriptor> filtered;
        for (const auto& plugin : plugins) {
            if (neuracoust::daw::vst3PluginDescriptorMatchesFilter(plugin, options.filter)) {
                filtered.push_back(plugin);
            }
        }
        plugins = std::move(filtered);
    }
    if (options.limit > 0 && static_cast<int>(plugins.size()) > options.limit) {
        plugins.resize(static_cast<size_t>(options.limit));
    }
    return plugins;
}

void printHeader() {
    std::cout
        << "name\tbrand\tbundle\tloadable\tmoduleInfoClasses\tfactoryOpen\tfactorySymbol\tfactoryClasses"
        << "\tcomponentCreated\tcontrollerCreated\tinitialized\tprocessor\tfloat32\tbusOk\tsetupOk\tprocessOk"
        << "\tlatency\ttail\tclass\tmessage\n";
}

void printListHeader() {
    std::cout << "name\tbrand\tbundle\tcid\tclass\tcategory\tloadable\n";
}

void printListRow(const neuracoust::daw::Vst3PluginDescriptor& plugin) {
    std::cout
        << sanitizeCell(plugin.name) << '\t'
        << sanitizeCell(plugin.brand) << '\t'
        << sanitizeCell(plugin.bundlePath) << '\t'
        << sanitizeCell(plugin.componentClassCid) << '\t'
        << sanitizeCell(plugin.componentClassName) << '\t'
        << sanitizeCell(plugin.category) << '\t'
        << yesNo(plugin.loadableBundle) << '\n';
}

void auditPlugin(const neuracoust::daw::Vst3PluginDescriptor& plugin, bool runProcess) {
    neuracoust::daw::Vst3FactoryInspection factory;
    neuracoust::daw::Vst3ProcessorProbe probe;
    neuracoust::daw::Vst3ParameterInspection parameters;
    std::vector<std::string> stageMessages;

    try {
        factory = neuracoust::daw::inspectVst3FactoryWithSdk(plugin);
    } catch (const std::exception& ex) {
        factory.message = std::string("factory exception: ") + ex.what();
        stageMessages.push_back(factory.message);
    } catch (...) {
        factory.message = "factory exception: unknown non-standard exception";
        stageMessages.push_back(factory.message);
    }

    try {
        probe = neuracoust::daw::probeVst3ProcessorWithSdk(plugin, 48000.0, 512);
    } catch (const std::exception& ex) {
        probe.message = std::string("probe exception: ") + ex.what();
        stageMessages.push_back(probe.message);
    } catch (...) {
        probe.message = "probe exception: unknown non-standard exception";
        stageMessages.push_back(probe.message);
    }

    try {
        parameters = neuracoust::daw::inspectVst3ParametersWithSdk(plugin, 1);
    } catch (const std::exception& ex) {
        parameters.message = std::string("parameters exception: ") + ex.what();
        stageMessages.push_back(parameters.message);
    } catch (...) {
        parameters.message = "parameters exception: unknown non-standard exception";
        stageMessages.push_back(parameters.message);
    }

    neuracoust::daw::Vst3ProcessResult process;
    if (runProcess) {
        auto audio = makeProbeAudio();
        try {
            process = neuracoust::daw::processStereoBufferWithVst3(plugin, audio, 512);
        } catch (const std::exception& ex) {
            process.message = std::string("process exception: ") + ex.what();
            stageMessages.push_back(process.message);
        } catch (...) {
            process.message = "process exception: unknown non-standard exception";
            stageMessages.push_back(process.message);
        }
    }

    std::string message = probe.message.empty() ? factory.message : probe.message;
    if (runProcess && !process.processed) {
        message = process.message;
    }
    if (!stageMessages.empty()) {
        message.clear();
        for (const auto& stageMessage : stageMessages) {
            if (!message.empty()) {
                message += "; ";
            }
            message += stageMessage;
        }
    }

    std::cout
        << sanitizeCell(plugin.name) << '\t'
        << sanitizeCell(plugin.brand) << '\t'
        << sanitizeCell(plugin.bundlePath) << '\t'
        << yesNo(plugin.loadableBundle) << '\t'
        << plugin.classCount << '\t'
        << yesNo(factory.opened) << '\t'
        << yesNo(factory.hasFactory) << '\t'
        << factory.classCount << '\t'
        << yesNo(probe.componentCreated) << '\t'
        << yesNo(parameters.controllerCreated) << '\t'
        << yesNo(probe.initialized) << '\t'
        << yesNo(probe.audioProcessorAvailable) << '\t'
        << yesNo(probe.sample32Supported) << '\t'
        << yesNo(probe.busArrangementAccepted) << '\t'
        << yesNo(probe.setupProcessingOk) << '\t'
        << (runProcess ? yesNo(process.processed) : "skip") << '\t'
        << probe.latencySamples << '\t'
        << probe.tailSamples << '\t'
        << sanitizeCell(probe.className.empty() ? process.className : probe.className) << '\t'
        << sanitizeCell(message)
        << '\n';
    std::cout.flush();
}

void printFailedAudit(const neuracoust::daw::Vst3PluginDescriptor& plugin, const std::string& message) {
    std::cout
        << sanitizeCell(plugin.name) << '\t'
        << sanitizeCell(plugin.brand) << '\t'
        << sanitizeCell(plugin.bundlePath) << '\t'
        << yesNo(plugin.loadableBundle) << '\t'
        << plugin.classCount << '\t'
        << "no\tno\t0\tno\tno\tno\tno\tno\tno\tno\tno\t0\t0\t\t"
        << sanitizeCell(message)
        << '\n';
}

int runChildAudit(const char* argv0,
                  const neuracoust::daw::Vst3PluginDescriptor& plugin,
                  const Options& options) {
    std::string auditExecutable = argv0 != nullptr ? argv0 : "";
    if (!auditExecutable.empty()) {
        std::error_code ec;
        const auto absolute = std::filesystem::absolute(auditExecutable, ec);
        if (!ec) {
            auditExecutable = absolute.string();
        }
    }
#if !defined(_WIN32)
    std::vector<std::string> argStorage;
    argStorage.push_back(auditExecutable);
    argStorage.push_back("--path");
    argStorage.push_back(plugin.bundlePath);
    if (!plugin.name.empty()) {
        argStorage.push_back("--name");
        argStorage.push_back(plugin.name);
    }
    if (!plugin.brand.empty()) {
        argStorage.push_back("--brand");
        argStorage.push_back(plugin.brand);
    }
    if (!plugin.componentClassCid.empty()) {
        argStorage.push_back("--class-cid");
        argStorage.push_back(plugin.componentClassCid);
    }
    if (!plugin.componentClassName.empty()) {
        argStorage.push_back("--class-name");
        argStorage.push_back(plugin.componentClassName);
    }
    argStorage.push_back("--no-header");
    if (!options.process) {
        argStorage.push_back("--no-process");
    }
    argStorage.push_back("--timeout");
    argStorage.push_back(std::to_string(options.timeoutSeconds));

    std::vector<char*> args;
    args.reserve(argStorage.size() + 1);
    for (auto& arg : argStorage) {
        args.push_back(arg.data());
    }
    args.push_back(nullptr);

    auto runAttempt = [&](std::string& failureMessage, bool& retryable) {
        retryable = false;
        const pid_t pid = fork();
        if (pid < 0) {
            failureMessage = "isolated host audit fork failed";
            return 1;
        }
        if (pid == 0) {
            const int devNull = open("/dev/null", O_WRONLY);
            if (devNull >= 0) {
                dup2(devNull, STDERR_FILENO);
                close(devNull);
            }
            execv(auditExecutable.c_str(), args.data());
            _exit(127);
        }

        int status = 0;
        for (int waited = 0; waited <= options.timeoutSeconds * 10; ++waited) {
            const pid_t waitResult = waitpid(pid, &status, WNOHANG);
            if (waitResult == pid) {
                break;
            }
            if (waitResult < 0) {
                failureMessage = "isolated host audit wait failed";
                return 1;
            }
            if (waited == options.timeoutSeconds * 10) {
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                failureMessage = "isolated host audit timed out after " + std::to_string(options.timeoutSeconds) + " seconds";
                retryable = true;
                return 1;
            }
            usleep(100000);
        }
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            return 0;
        }
        if (WIFSIGNALED(status)) {
            failureMessage = "isolated host audit terminated by signal " + std::to_string(WTERMSIG(status));
            retryable = true;
            return 1;
        }
        failureMessage = "isolated host audit exited with status " + std::to_string(status);
        return 1;
    };

    std::string failureMessage;
    bool retryable = false;
    if (runAttempt(failureMessage, retryable) == 0) {
        return 0;
    }
    if (retryable) {
        std::string retryFailureMessage;
        bool retryRetryable = false;
        if (runAttempt(retryFailureMessage, retryRetryable) == 0) {
            return 0;
        }
        failureMessage = retryFailureMessage.empty() ? failureMessage : retryFailureMessage;
    }
    printFailedAudit(plugin, failureMessage);
    return 1;
#else
    std::string command = shellQuote(auditExecutable) + " --path " + shellQuote(plugin.bundlePath) + " --no-header";
    if (!plugin.name.empty()) {
        command += " --name " + shellQuote(plugin.name);
    }
    if (!plugin.brand.empty()) {
        command += " --brand " + shellQuote(plugin.brand);
    }
    if (!plugin.componentClassCid.empty()) {
        command += " --class-cid " + shellQuote(plugin.componentClassCid);
    }
    if (!plugin.componentClassName.empty()) {
        command += " --class-name " + shellQuote(plugin.componentClassName);
    }
    if (!options.process) {
        command += " --no-process";
    }
    command += " 2>NUL";
    const int status = std::system(command.c_str());
    if (status != 0) {
        printFailedAudit(plugin, "isolated host audit process failed with status " + std::to_string(status));
        return 1;
    }
    return 0;
#endif
}

} // namespace

int main(int argc, char** argv) {
    reexecWithAbsoluteArgvIfNeeded(argc, argv);
    const Options options = parseOptions(argc, argv);
    const auto capabilities = neuracoust::daw::vst3HostCapabilities();
    if (!options.noHeader) {
        std::cerr << capabilities.message << "\n";
    }

    auto plugins = collectPlugins(options);
    neuracoust::daw::sortVst3PluginDescriptorsForDisplay(plugins);
    if (options.listOnly) {
        if (!options.noHeader) {
            printListHeader();
        }
        for (const auto& plugin : plugins) {
            printListRow(plugin);
        }
        return 0;
    }
    if (!options.noHeader) {
        printHeader();
        std::cout.flush();
    }
    for (const auto& plugin : plugins) {
        if (options.path.empty() || !options.noHeader) {
            runChildAudit(argv[0], plugin, options);
            continue;
        }
        try {
            auditPlugin(plugin, options.process);
        } catch (const std::exception& ex) {
            printFailedAudit(plugin, std::string("host audit exception: ") + ex.what());
        } catch (...) {
            printFailedAudit(plugin, "host audit exception: unknown non-standard exception");
        }
    }
    return 0;
}
