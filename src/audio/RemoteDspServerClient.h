#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace neuracoust::daw {

struct RemoteDspServerNode {
    bool enabled = true;
    std::string id;
    std::string displayName;
    std::string host;
    uint16_t rtEnginePort = 20000;
    bool dedicatedLink = false;
    double linkCapacityMbps = 1000.0;
    double estimatedDspLoad = 0.0;
    uint32_t assignedTrackGroups = 0;
    uint32_t maxTrackGroups = 32;
    uint16_t reportedCoreCount = 4;
    uint16_t dawCoreReserve = 1;
    uint16_t monitorCoreReserve = 1;
    uint16_t pluginCoreReserve = 2;
};

struct RemoteDspServerSettings {
    // Two distinct machines answer the same NART protocol, and they are not interchangeable:
    // `host` is the EXTERNAL node, a general-purpose computer borrowed for spare cores;
    // `ndsHost` is the dedicated NDS appliance, which owns its own OS and timing. Each has its
    // own address and its own master switch so one can be off while the other carries work.
    bool enabled = true;
    bool autoAssignTrackGroups = true;
    bool dawDspEnabled = false;
    bool pluginDspEnabled = false;
    bool autoAssignPluginCores = true;
    std::string host = "studio.local";
    std::string ndsHost = "192.168.0.198";
    bool ndsEnabled = false;
    // Which machine carries which job ("internal" | "nds" | "external"). The user assigns these
    // explicitly; autoOverflow turns them into starting points that spill to the next machine
    // when one runs short. Carried here because this struct is what already reaches every place
    // that has to decide where a block is processed.
    std::string roleMonitor = "internal";
    std::string roleChannelStrip = "internal";
    std::string roleMaster = "internal";
    std::string roleInserts = "internal";
    bool autoOverflow = false;
    uint16_t rtEnginePort = 20000;
    uint16_t statusPort = 20001;
    uint16_t channelCount = 2;
    uint16_t frameCount = 128;
    uint16_t networkBufferFrames = 256;
    double sampleRate = 48000.0;
    int timeoutMs = 250;
    uint16_t totalCoreHint = 4;
    uint16_t dawCoreReserve = 1;
    uint16_t monitorCoreReserve = 1;
    uint16_t pluginCoreReserve = 2;
    std::string loadedPluginIdHint;
    std::vector<RemoteDspServerNode> nodes;
};

struct RemoteDspCorePlan {
    uint16_t totalCores = 4;
    uint16_t monitorCores = 1;
    uint16_t dawCores = 1;
    uint16_t pluginCores = 2;
    uint16_t idleCores = 0;
    bool monitorExternal = false;
    bool dawExternal = true;
    bool pluginExternal = true;
    bool pluginAutoAssigned = true;
    bool saturated = false;
    std::string summary;
};

struct RemoteDspServerProbeResult {
    bool reachable = false;
    bool protocolMatched = false;
    uint32_t sequence = 0;
    uint16_t channelCount = 0;
    uint16_t frameCount = 0;
    double roundTripMs = 0.0;
    float returnedPeak = 0.0f;
    std::string message;
};

struct RemoteDspProcessResult {
    bool processed = false;
    bool reachable = false;
    bool protocolMatched = false;
    uint32_t sequence = 0;
    uint16_t channelCount = 0;
    uint16_t frameCount = 0;
    double roundTripMs = 0.0;
    float returnedPeak = 0.0f;
    std::string message;
};

struct RemoteDspParameterValue {
    uint32_t index = 0;
    float normalizedValue = 0.0f;
};

struct RemoteDspPluginInfo {
    std::string pluginId;
    std::string pluginName;
    std::string pluginFormat;
    std::string pluginPath;
    std::string pluginClassId;
    std::string pluginClassName;
};

struct RemoteDspServerInfo {
    bool reachable = false;
    double roundTripMs = 0.0;
    std::string vendor;
    std::string model;
    std::string version;
    std::string hostname;
    std::string macAddress;
    std::string cpuModel;
    double cpuMhz = 0.0;
    uint32_t memoryMb = 0;
    double temperatureC = 0.0;
    double temperatureF = 0.0;
    std::vector<double> cpuCoreLoads;
    std::string nic;
    uint16_t audioPort = 20000;
    uint16_t monitorPort = 20001;
    uint32_t channels = 0;
    uint32_t coreCount = 0;
    std::string bufferProfiles;
    std::string performanceModes;
    std::string lpfc;
    std::string lpee;
    std::string pluginId;
    std::string pluginName;
    std::vector<RemoteDspPluginInfo> pluginCatalog;
    uint64_t packetsIn = 0;
    uint64_t packetsOut = 0;
    uint64_t badPackets = 0;
    std::string message;
};

struct RemoteDspNodeStatus {
    RemoteDspServerNode node;
    RemoteDspServerProbeResult probe;
};

struct RemoteDspNodeSelection {
    bool selected = false;
    size_t nodeIndex = 0;
    RemoteDspServerNode node;
    double score = 0.0;
    std::string message;
};

struct RemoteDspDiscoveryResult {
    RemoteDspServerNode node;
    RemoteDspServerInfo info;
};

RemoteDspServerSettings defaultRemoteDspServerSettings();

/// Which machine a routing mode talks to, and whether it is switched on at all.
/// "nds" is the appliance; "external"/"remote_external" the borrowed computer; "auto" prefers
/// the appliance when it is on, because it is the one with predictable timing. The returned
/// settings carry that machine's address in `host`, so every caller downstream stays
/// single-address and none of them has to know which machine it is speaking to.
RemoteDspServerSettings remoteDspSettingsForMode(const RemoteDspServerSettings& settings,
                                                 const std::string& mode);
/// True when `mode` names a remote machine that is switched on.
bool remoteDspModeAvailable(const RemoteDspServerSettings& settings, const std::string& mode);

/// What one trip to a remote machine costs, in samples, for delay compensation.
///
/// DECLARED, NOT MEASURED — the way Pro Tools HDX charges a host crossing. Adding the measured
/// round trip would make the compensation move whenever the network did, realigning every other
/// path in the mix against a drifting number: an audible phase shift. A fixed budget the trip
/// comfortably fits inside is the point; a block that misses it is a dropout to report, never a
/// delay to renegotiate. Everything that compensates for a crossing must use this one number.
unsigned int remoteDspCrossingLatencySamples(const RemoteDspServerSettings& settings, int maxBlockSize);
/// The routing mode a job's assignment resolves to. Explicit assignment is the mode itself;
/// with auto-overflow on every job becomes "auto" — use whatever machine has room.
std::string remoteDspModeForRole(const RemoteDspServerSettings& settings, const std::string& role);

std::vector<RemoteDspServerNode> defaultRemoteDspServerNodes();
std::vector<RemoteDspServerNode> effectiveRemoteDspServerNodes(const RemoteDspServerSettings& settings);
RemoteDspServerSettings settingsForRemoteDspServerNode(const RemoteDspServerSettings& settings,
                                                       const RemoteDspServerNode& node);
RemoteDspCorePlan makeRemoteDspCorePlan(const RemoteDspServerSettings& settings,
                                        uint32_t reportedCoreCount,
                                        bool monitorExternal);
std::vector<float> makeRemoteDspProbeBlock(uint16_t channelCount, uint16_t frameCount);
RemoteDspServerProbeResult probeRemoteDspServer(const RemoteDspServerSettings& settings);
RemoteDspServerProbeResult probeRemoteDspServerNode(const RemoteDspServerSettings& settings,
                                                    const RemoteDspServerNode& node);
RemoteDspProcessResult processRemoteDspInterleavedStereo(const RemoteDspServerSettings& settings,
                                                         const std::vector<float>& interleavedStereo,
                                                         std::vector<float>& processedInterleavedStereo);
RemoteDspProcessResult processRemoteDspInterleavedStereo(const RemoteDspServerSettings& settings,
                                                         const std::vector<float>& interleavedStereo,
                                                         const std::vector<RemoteDspParameterValue>& parameters,
                                                         std::vector<float>& processedInterleavedStereo);

class RemoteDspProcessSession {
public:
    RemoteDspProcessSession();
    ~RemoteDspProcessSession();

    RemoteDspProcessSession(const RemoteDspProcessSession&) = delete;
    RemoteDspProcessSession& operator=(const RemoteDspProcessSession&) = delete;

    void reset();
    RemoteDspProcessResult process(const RemoteDspServerSettings& settings,
                                   const std::vector<float>& interleavedStereo,
                                   std::vector<float>& processedInterleavedStereo);
    RemoteDspProcessResult process(const RemoteDspServerSettings& settings,
                                   const std::vector<float>& interleavedStereo,
                                   const std::vector<RemoteDspParameterValue>& parameters,
                                   std::vector<float>& processedInterleavedStereo);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

RemoteDspServerInfo queryRemoteDspServerInfo(const RemoteDspServerSettings& settings);
std::vector<RemoteDspDiscoveryResult> discoverRemoteDspServers(const RemoteDspServerSettings& settings,
                                                               const std::vector<std::string>& broadcastHosts = {},
                                                               int timeoutMs = 350);
RemoteDspNodeSelection selectRemoteDspNodeForTrackGroup(const std::vector<RemoteDspNodeStatus>& nodes);

} // namespace neuracoust::daw
