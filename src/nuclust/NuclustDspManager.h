#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace neuracoust::nuclust {

enum class DspExecutionMode {
    Native,
    InternalDsp,
    ExternalDsp
};

enum class PluginAssignmentStatus {
    Ready,
    WaitingForServer,
    MissingServerModule,
    IncompatibleServer,
    HighJitter,
    NoCapacity,
    LicenseDenied,
    NativeFallback
};

enum class LatencyPolicy {
    OptimizeLatency,
    OptimizeStability
};

struct NetworkInterfaceInfo {
    std::string name;
    std::string address;
    std::string subnet;
    uint32_t linkSpeedMbps = 0;
    bool active = false;
};

struct NuclustServerModule {
    std::string moduleId;
    std::string displayName;
    std::string version;
    bool installed = false;
    bool updateAvailable = false;
};

struct NuclustServerStatus {
    std::string serverId;
    std::string name;
    std::string ipAddress;
    std::string macAddress;
    std::string interfaceName;
    std::string subnet;
    uint32_t nicLinkSpeedMbps = 0;
    std::string cpuModel;
    uint32_t ramMb = 0;
    std::string serverVersion;
    std::string firmwareVersion;
    bool connected = false;
    double temperatureC = 0.0;
    double cpuUsage = 0.0;
    std::vector<double> coreUsage;
    double inputLevel = 0.0;
    double outputLevel = 0.0;
    double peakHoldLevel = 0.0;
    uint64_t packetsIn = 0;
    uint64_t packetsOut = 0;
    uint64_t badPackets = 0;
    uint32_t networkBufferFrames = 256;
    uint32_t reportedLatencySamples = 0;
    double roundTripMs = 0.0;
    double jitterCurrentMs = 0.0;
    double jitterPeakMs = 0.0;
    bool jitterPeakLatched = false;
    uint32_t usedChannels = 0;
    uint32_t maxChannels = 64;
    std::vector<NuclustServerModule> modules;
    std::string message;
};

struct PluginRegistrationRequest {
    std::string instanceId;
    std::string pluginId;
    std::string pluginVersion;
    std::string requestedServerId;
    std::string serverModuleId;
    std::string trackGroupId;
    DspExecutionMode requestedMode = DspExecutionMode::Native;
    uint32_t channelCount = 2;
    double sampleRate = 48000.0;
    uint32_t blockSize = 128;
    uint32_t latencyRequirementSamples = 512;
    bool nativeFallbackAllowed = true;
};

struct PluginAssignmentResponse {
    std::string instanceId;
    DspExecutionMode assignedMode = DspExecutionMode::Native;
    std::string assignedServerId;
    std::string serverModuleId;
    uint32_t reportedLatencySamples = 0;
    PluginAssignmentStatus status = PluginAssignmentStatus::Ready;
    std::string errorMessage;
};

struct PluginInstanceState {
    PluginRegistrationRequest request;
    PluginAssignmentResponse assignment;
    std::chrono::system_clock::time_point lastSeen;
};

struct NuclustManagerSettings {
    bool launchAtLogin = false;
    std::vector<std::string> manualServerAddresses;
    std::vector<std::string> preferredInterfaces;
    uint32_t defaultBufferFrames = 256;
    LatencyPolicy latencyPolicy = LatencyPolicy::OptimizeLatency;
    double jitterLatchThresholdMs = 1.5;
    std::vector<std::pair<std::string, std::string>> serverAliases;
};

struct NuclustManagerSnapshot {
    std::vector<NetworkInterfaceInfo> interfaces;
    std::vector<NuclustServerStatus> servers;
    std::vector<PluginInstanceState> pluginInstances;
    NuclustManagerSettings settings;
    bool dspEngineRunning = true;
};

class NuclustDspManagerCore {
public:
    NuclustDspManagerCore();

    void loadSettings();
    void saveSettings() const;
    std::filesystem::path settingsPath() const;
    std::filesystem::path logPath() const;

    NuclustManagerSnapshot snapshot() const;
    std::vector<std::string> recentLogLines(size_t maxLines) const;
    std::string diagnosticText() const;

    void rescanServers();
    void refreshServerTelemetry();
    void addManualServer(std::string address);
    void removeServer(const std::string& serverId);
    void disconnectServer(const std::string& serverId);
    void identifyServer(const std::string& serverId);
    void clearJitterLatch(const std::string& serverId);
    void setDspEngineRunning(bool running);
    void setDefaultBufferFrames(uint32_t frames);
    void setLatencyPolicy(LatencyPolicy policy);

    PluginAssignmentResponse registerPlugin(const PluginRegistrationRequest& request);
    void unregisterPlugin(const std::string& instanceId);

    std::string handleIpcMessage(const std::string& line);

private:
    void log(std::string message);
    void updateSyntheticTelemetryLocked();
    PluginAssignmentResponse assignLocked(const PluginRegistrationRequest& request);
    std::optional<size_t> findServerIndexLocked(const std::string& serverId) const;

    mutable std::mutex mutex_;
    NuclustManagerSettings settings_;
    std::vector<NetworkInterfaceInfo> interfaces_;
    std::vector<NuclustServerStatus> servers_;
    std::vector<PluginInstanceState> plugins_;
    std::vector<std::string> logLines_;
    bool dspEngineRunning_ = true;
};

class NuclustDspManagerIpcServer {
public:
    explicit NuclustDspManagerIpcServer(NuclustDspManagerCore& core);
    ~NuclustDspManagerIpcServer();

    NuclustDspManagerIpcServer(const NuclustDspManagerIpcServer&) = delete;
    NuclustDspManagerIpcServer& operator=(const NuclustDspManagerIpcServer&) = delete;

    bool start(uint16_t port = 48780);
    void stop();
    bool running() const;
    uint16_t port() const;

private:
    void run();

    NuclustDspManagerCore& core_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    uint16_t port_ = 48780;
};

std::string toString(DspExecutionMode mode);
DspExecutionMode dspExecutionModeFromString(const std::string& text);
std::string toString(PluginAssignmentStatus status);
std::string toString(LatencyPolicy policy);
LatencyPolicy latencyPolicyFromString(const std::string& text);
uint32_t estimatedLatencySamples(uint32_t bufferFrames, uint32_t blockSize, double sampleRate, DspExecutionMode mode);

} // namespace neuracoust::nuclust
