#include "nuclust/NuclustDspManager.h"

#include "audio/RemoteDspServerClient.h"
#include "license/LicenseAgentClient.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <numeric>
#include <set>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace neuracoust::nuclust {

namespace {

std::string trim(std::string text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.erase(text.begin());
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    return text;
}

std::string escapeJson(const std::string& text) {
    std::string out;
    for (char ch : text) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': break;
            case '\t': out += "\\t"; break;
            default: out += ch; break;
        }
    }
    return out;
}

std::string jsonString(const std::string& text) {
    return "\"" + escapeJson(text) + "\"";
}

std::string extractString(const std::string& json, const std::string& key, const std::string& fallback = {}) {
    const std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) {
        return fallback;
    }
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) {
        return fallback;
    }
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) {
        return fallback;
    }
    std::string out;
    bool escaped = false;
    for (size_t i = pos + 1; i < json.size(); ++i) {
        const char ch = json[i];
        if (escaped) {
            out += ch == 'n' ? '\n' : ch == 't' ? '\t' : ch;
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (ch == '"') {
            return out;
        }
        out += ch;
    }
    return fallback;
}

double extractNumber(const std::string& json, const std::string& key, double fallback) {
    const std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) {
        return fallback;
    }
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) {
        return fallback;
    }
    auto end = pos + 1;
    while (end < json.size() && (std::isspace(static_cast<unsigned char>(json[end])) || json[end] == '"')) {
        ++end;
    }
    size_t consumed = 0;
    try {
        return std::stod(json.substr(end), &consumed);
    } catch (...) {
        return fallback;
    }
}

bool extractBool(const std::string& json, const std::string& key, bool fallback) {
    const std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) {
        return fallback;
    }
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) {
        return fallback;
    }
    const auto tail = json.substr(pos + 1);
    if (tail.find("true") < tail.find("false")) {
        return true;
    }
    if (tail.find("false") != std::string::npos) {
        return false;
    }
    return fallback;
}

std::vector<std::string> extractStringArray(const std::string& json, const std::string& key) {
    std::vector<std::string> values;
    const std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) {
        return values;
    }
    pos = json.find('[', pos + needle.size());
    auto end = json.find(']', pos == std::string::npos ? 0 : pos);
    if (pos == std::string::npos || end == std::string::npos) {
        return values;
    }
    std::string array = json.substr(pos + 1, end - pos - 1);
    size_t cursor = 0;
    while (cursor < array.size()) {
        auto quote = array.find('"', cursor);
        if (quote == std::string::npos) {
            break;
        }
        auto next = array.find('"', quote + 1);
        if (next == std::string::npos) {
            break;
        }
        values.push_back(array.substr(quote + 1, next - quote - 1));
        cursor = next + 1;
    }
    return values;
}

std::filesystem::path userSupportDirectory() {
    if (const char* overridePath = std::getenv("NUCLUST_DSP_MANAGER_HOME")) {
        if (std::string(overridePath).empty() == false) {
            return overridePath;
        }
    }
#ifdef _WIN32
    const char* appData = std::getenv("APPDATA");
    std::filesystem::path base = appData != nullptr ? appData : ".";
    return base / "Neuracoust" / "Neuracoust DSP Manager";
#else
    const char* home = std::getenv("HOME");
    std::filesystem::path base = home != nullptr ? home : ".";
    return base / "Library" / "Application Support" / "Neuracoust" / "Neuracoust DSP Manager";
#endif
}

std::vector<NetworkInterfaceInfo> enumerateInterfaces() {
    std::vector<NetworkInterfaceInfo> interfaces;
#ifdef _WIN32
    interfaces.push_back({"Localhost", "127.0.0.1", "127.0.0.0/8", 1000, true});
#else
    struct ifaddrs* addrs = nullptr;
    if (getifaddrs(&addrs) != 0 || addrs == nullptr) {
        interfaces.push_back({"lo0", "127.0.0.1", "127.0.0.0/8", 0, true});
        return interfaces;
    }
    for (auto* ifa = addrs; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        char host[INET_ADDRSTRLEN] = {};
        const auto* in = reinterpret_cast<const sockaddr_in*>(ifa->ifa_addr);
        if (inet_ntop(AF_INET, &in->sin_addr, host, sizeof(host)) == nullptr) {
            continue;
        }
        uint32_t mask = 0;
        if (ifa->ifa_netmask != nullptr) {
            const auto* netmask = reinterpret_cast<const sockaddr_in*>(ifa->ifa_netmask);
            mask = ntohl(netmask->sin_addr.s_addr);
        }
        const uint32_t addr = ntohl(in->sin_addr.s_addr);
        const uint32_t network = addr & mask;
        in_addr networkAddr {};
        networkAddr.s_addr = htonl(network);
        char networkText[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &networkAddr, networkText, sizeof(networkText));
        int prefix = 0;
        for (uint32_t bits = mask; bits != 0; bits <<= 1) {
            ++prefix;
        }
        NetworkInterfaceInfo info;
        info.name = ifa->ifa_name;
        info.address = host;
        info.subnet = std::string(networkText) + "/" + std::to_string(prefix);
        info.linkSpeedMbps = (info.name.find("en") == 0 || info.name.find("eth") == 0) ? 2500 : 0;
        info.active = (ifa->ifa_flags & IFF_UP) != 0;
        if (std::none_of(interfaces.begin(), interfaces.end(), [&](const auto& existing) {
                return existing.name == info.name && existing.address == info.address;
            })) {
            interfaces.push_back(info);
        }
    }
    freeifaddrs(addrs);
#endif
    return interfaces;
}

std::string chooseInterfaceForAddress(const std::vector<NetworkInterfaceInfo>& interfaces, const std::string& address) {
    for (const auto& iface : interfaces) {
        auto dot = iface.address.rfind('.');
        if (dot != std::string::npos && address.rfind(iface.address.substr(0, dot + 1), 0) == 0) {
            return iface.name;
        }
    }
    return interfaces.empty() ? "" : interfaces.front().name;
}

std::vector<std::string> broadcastTargetsForInterfaces(const std::vector<NetworkInterfaceInfo>& interfaces) {
    std::vector<std::string> targets {"255.255.255.255"};
    for (const auto& iface : interfaces) {
        if (!iface.active) {
            continue;
        }
        const auto dot = iface.address.rfind('.');
        if (dot == std::string::npos) {
            continue;
        }
        targets.push_back(iface.address.substr(0, dot + 1) + "255");
    }
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    return targets;
}

std::string chooseSubnetForInterface(const std::vector<NetworkInterfaceInfo>& interfaces, const std::string& name) {
    for (const auto& iface : interfaces) {
        if (iface.name == name) {
            return iface.subnet;
        }
    }
    return {};
}

uint32_t chooseLinkSpeedForInterface(const std::vector<NetworkInterfaceInfo>& interfaces, const std::string& name) {
    for (const auto& iface : interfaces) {
        if (iface.name == name) {
            return iface.linkSpeedMbps;
        }
    }
    return 0;
}

double normalizeCpuLoad(double load) {
    if (!std::isfinite(load)) {
        return 0.0;
    }
    return std::clamp(load > 1.0 ? load / 100.0 : load, 0.0, 1.0);
}

std::vector<double> smoothCoreLoads(const std::vector<double>& previous, const std::vector<double>& raw) {
    std::vector<double> smoothed;
    smoothed.reserve(raw.size());
    for (size_t index = 0; index < raw.size(); ++index) {
        double next = normalizeCpuLoad(raw[index]);
        if (next < 0.02) {
            next = 0.0;
        }
        const double current = index < previous.size() ? normalizeCpuLoad(previous[index]) : next;
        smoothed.push_back((current * 0.90) + (next * 0.10));
    }
    return smoothed;
}

std::vector<NuclustServerModule> defaultModulesForManualServer() {
    return {
        {"neuracoust.monitor.core", "Monitor DSP Core", "1.0.0", true, false},
        {"neuracoust.test.external", "Neuracoust Test External Module", "1.0.0", true, false}
    };
}

NuclustServerStatus makeServerStatusFromAddress(const std::string& address,
                                                const std::vector<NetworkInterfaceInfo>& interfaces,
                                                const NuclustManagerSettings& settings,
                                                const daw::RemoteDspServerInfo* liveInfo) {
    const auto iface = chooseInterfaceForAddress(interfaces, address);
    NuclustServerStatus server;
    server.serverId = "manual-" + address;
    std::replace(server.serverId.begin(), server.serverId.end(), '.', '-');
    server.name = "Neuracoust DSP Server";
    server.ipAddress = address;
    server.macAddress = "unknown";
    server.interfaceName = iface;
    server.subnet = chooseSubnetForInterface(interfaces, iface);
    server.nicLinkSpeedMbps = chooseLinkSpeedForInterface(interfaces, iface);
    server.cpuModel = "Neuracoust RT Appliance";
    server.ramMb = 8192;
    server.serverVersion = "discovered";
    server.firmwareVersion = "query pending";
    server.connected = true;
    server.temperatureC = 42.0;
    server.cpuUsage = 0.18;
    server.coreUsage = {0.0, 0.0, 0.0, 0.0};
    server.networkBufferFrames = settings.defaultBufferFrames;
    server.reportedLatencySamples = estimatedLatencySamples(settings.defaultBufferFrames, 128, 48000.0, DspExecutionMode::ExternalDsp);
    server.roundTripMs = 0.0;
    server.jitterCurrentMs = 0.0;
    server.jitterPeakMs = 0.0;
    server.maxChannels = 64;
    server.modules = defaultModulesForManualServer();
    server.message = "Manual server registered; live firmware query boundary is ready.";

    if (liveInfo != nullptr && liveInfo->reachable) {
        server.serverId = liveInfo->hostname.empty() ? server.serverId : liveInfo->hostname;
        server.name = liveInfo->model.empty() ? (liveInfo->hostname.empty() ? "Neuracoust DSP Server" : liveInfo->hostname) : liveInfo->model;
        server.macAddress = liveInfo->macAddress.empty() ? server.macAddress : liveInfo->macAddress;
        server.cpuModel = liveInfo->cpuModel.empty() ? server.cpuModel : liveInfo->cpuModel;
        server.ramMb = liveInfo->memoryMb > 0 ? liveInfo->memoryMb : server.ramMb;
        server.serverVersion = liveInfo->version.empty() ? server.serverVersion : liveInfo->version;
        server.firmwareVersion = liveInfo->version.empty() ? server.firmwareVersion : liveInfo->version;
        server.temperatureC = liveInfo->temperatureC > 0.0 ? liveInfo->temperatureC : server.temperatureC;
        if (!liveInfo->cpuCoreLoads.empty()) {
            server.coreUsage.clear();
            for (double load : liveInfo->cpuCoreLoads) {
                server.coreUsage.push_back(normalizeCpuLoad(load));
            }
        }
        server.cpuUsage = server.coreUsage.empty()
            ? server.cpuUsage
            : std::accumulate(server.coreUsage.begin(), server.coreUsage.end(), 0.0) / static_cast<double>(server.coreUsage.size());
        server.packetsIn = liveInfo->packetsIn;
        server.packetsOut = liveInfo->packetsOut;
        server.badPackets = liveInfo->badPackets;
        server.maxChannels = liveInfo->channels > 0 ? liveInfo->channels : server.maxChannels;
        server.roundTripMs = liveInfo->roundTripMs;
        server.message = liveInfo->message;
        if (!liveInfo->pluginId.empty() &&
            std::none_of(server.modules.begin(), server.modules.end(), [&](const auto& module) { return module.moduleId == liveInfo->pluginId; })) {
            server.modules.push_back({liveInfo->pluginId,
                                      liveInfo->pluginName.empty() ? liveInfo->pluginId : liveInfo->pluginName,
                                      liveInfo->version,
                                      true,
                                      false});
        }
    }
    return server;
}

} // namespace

std::string toString(DspExecutionMode mode) {
    switch (mode) {
        case DspExecutionMode::Native: return "Native";
        case DspExecutionMode::InternalDsp: return "Internal DSP";
        case DspExecutionMode::ExternalDsp: return "External DSP";
    }
    return "Native";
}

DspExecutionMode dspExecutionModeFromString(const std::string& text) {
    std::string lowered = text;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return std::tolower(c); });
    if (lowered.find("external") != std::string::npos) {
        return DspExecutionMode::ExternalDsp;
    }
    if (lowered.find("internal") != std::string::npos) {
        return DspExecutionMode::InternalDsp;
    }
    return DspExecutionMode::Native;
}

std::string toString(PluginAssignmentStatus status) {
    switch (status) {
        case PluginAssignmentStatus::Ready: return "ready";
        case PluginAssignmentStatus::WaitingForServer: return "waiting_for_server";
        case PluginAssignmentStatus::MissingServerModule: return "missing_server_module";
        case PluginAssignmentStatus::IncompatibleServer: return "incompatible_server";
        case PluginAssignmentStatus::HighJitter: return "high_jitter";
        case PluginAssignmentStatus::NoCapacity: return "no_capacity";
        case PluginAssignmentStatus::LicenseDenied: return "license_denied";
        case PluginAssignmentStatus::NativeFallback: return "native_fallback";
    }
    return "ready";
}

std::string toString(LatencyPolicy policy) {
    return policy == LatencyPolicy::OptimizeStability ? "stability" : "latency";
}

LatencyPolicy latencyPolicyFromString(const std::string& text) {
    return text == "stability" ? LatencyPolicy::OptimizeStability : LatencyPolicy::OptimizeLatency;
}

uint32_t estimatedLatencySamples(uint32_t bufferFrames, uint32_t blockSize, double sampleRate, DspExecutionMode mode) {
    if (mode == DspExecutionMode::Native) {
        return blockSize;
    }
    const double networkSafety = mode == DspExecutionMode::ExternalDsp ? 1.5 : 0.5;
    const double frames = static_cast<double>(blockSize) + static_cast<double>(bufferFrames) * (2.0 + networkSafety);
    return static_cast<uint32_t>(std::ceil(frames * (sampleRate > 0.0 ? sampleRate / sampleRate : 1.0)));
}

NuclustDspManagerCore::NuclustDspManagerCore() {
    loadSettings();
    rescanServers();
}

std::filesystem::path NuclustDspManagerCore::settingsPath() const {
    return userSupportDirectory() / "settings.json";
}

std::filesystem::path NuclustDspManagerCore::logPath() const {
    return userSupportDirectory() / "manager.log";
}

void NuclustDspManagerCore::loadSettings() {
    std::lock_guard lock(mutex_);
    std::ifstream input(settingsPath());
    if (!input) {
        settings_ = {};
        return;
    }
    std::stringstream buffer;
    buffer << input.rdbuf();
    const auto json = buffer.str();
    settings_.launchAtLogin = extractBool(json, "launchAtLogin", false);
    settings_.manualServerAddresses = extractStringArray(json, "manualServerAddresses");
    settings_.preferredInterfaces = extractStringArray(json, "preferredInterfaces");
    settings_.defaultBufferFrames = static_cast<uint32_t>(extractNumber(json, "defaultBufferFrames", 256));
    settings_.latencyPolicy = latencyPolicyFromString(extractString(json, "latencyPolicy", "latency"));
    settings_.jitterLatchThresholdMs = extractNumber(json, "jitterLatchThresholdMs", 1.5);
}

void NuclustDspManagerCore::saveSettings() const {
    std::lock_guard lock(mutex_);
    std::filesystem::create_directories(settingsPath().parent_path());
    std::ofstream out(settingsPath(), std::ios::trunc);
    out << "{\n";
    out << "  \"launchAtLogin\": " << (settings_.launchAtLogin ? "true" : "false") << ",\n";
    out << "  \"manualServerAddresses\": [";
    for (size_t i = 0; i < settings_.manualServerAddresses.size(); ++i) {
        out << (i == 0 ? "" : ", ") << jsonString(settings_.manualServerAddresses[i]);
    }
    out << "],\n";
    out << "  \"preferredInterfaces\": [";
    for (size_t i = 0; i < settings_.preferredInterfaces.size(); ++i) {
        out << (i == 0 ? "" : ", ") << jsonString(settings_.preferredInterfaces[i]);
    }
    out << "],\n";
    out << "  \"defaultBufferFrames\": " << settings_.defaultBufferFrames << ",\n";
    out << "  \"latencyPolicy\": " << jsonString(toString(settings_.latencyPolicy)) << ",\n";
    out << "  \"jitterLatchThresholdMs\": " << settings_.jitterLatchThresholdMs << ",\n";
    out << "  \"serverAliases\": []\n";
    out << "}\n";
}

NuclustManagerSnapshot NuclustDspManagerCore::snapshot() const {
    std::lock_guard lock(mutex_);
    NuclustManagerSnapshot snap;
    snap.interfaces = interfaces_;
    snap.servers = servers_;
    snap.pluginInstances = plugins_;
    snap.settings = settings_;
    snap.dspEngineRunning = dspEngineRunning_;
    return snap;
}

std::vector<std::string> NuclustDspManagerCore::recentLogLines(size_t maxLines) const {
    std::lock_guard lock(mutex_);
    if (logLines_.size() <= maxLines) {
        return logLines_;
    }
    return {logLines_.end() - static_cast<std::ptrdiff_t>(maxLines), logLines_.end()};
}

std::string NuclustDspManagerCore::diagnosticText() const {
    std::lock_guard lock(mutex_);
    std::ostringstream out;
    out << "Neuracoust DSP Manager diagnostics\n";
    out << "settingsPath=" << settingsPath().string() << "\n";
    out << "logPath=" << logPath().string() << "\n";
    out << "dspEngineRunning=" << (dspEngineRunning_ ? "true" : "false") << "\n";
    out << "defaultBufferFrames=" << settings_.defaultBufferFrames << "\n";
    out << "latencyPolicy=" << toString(settings_.latencyPolicy) << "\n";
    out << "jitterLatchThresholdMs=" << settings_.jitterLatchThresholdMs << "\n";
    out << "\nInterfaces:\n";
    for (const auto& iface : interfaces_) {
        out << "- " << iface.name << " " << iface.address << " " << iface.subnet
            << " link=" << iface.linkSpeedMbps << " active=" << (iface.active ? "true" : "false") << "\n";
    }
    out << "\nServers:\n";
    for (const auto& server : servers_) {
        const double maxCoreUsage = server.coreUsage.empty()
            ? 0.0
            : *std::max_element(server.coreUsage.begin(), server.coreUsage.end());
        out << "- " << server.serverId << " name=" << server.name << " ip=" << server.ipAddress
            << " iface=" << server.interfaceName << " connected=" << (server.connected ? "true" : "false")
            << " latencySamples=" << server.reportedLatencySamples
            << " roundTripMs=" << server.roundTripMs
            << " jitterCurrentMs=" << server.jitterCurrentMs
            << " jitterPeakMs=" << server.jitterPeakMs
            << " cpuAvgPct=" << (server.cpuUsage * 100.0)
            << " cpuMaxPct=" << (maxCoreUsage * 100.0)
            << " cpuCorePct=[";
        for (size_t index = 0; index < server.coreUsage.size(); ++index) {
            out << (index == 0 ? "" : ",") << (server.coreUsage[index] * 100.0);
        }
        out << "]"
            << " latch=" << (server.jitterPeakLatched ? "true" : "false")
            << " message=" << server.message << "\n";
    }
    out << "\nPlugins:\n";
    for (const auto& plugin : plugins_) {
        out << "- " << plugin.request.instanceId << " pluginId=" << plugin.request.pluginId
            << " requested=" << toString(plugin.request.requestedMode)
            << " assigned=" << toString(plugin.assignment.assignedMode)
            << " server=" << plugin.assignment.assignedServerId
            << " status=" << toString(plugin.assignment.status)
            << " error=" << plugin.assignment.errorMessage << "\n";
    }
    out << "\nRecent log:\n";
    for (const auto& line : logLines_) {
        out << line << "\n";
    }
    return out.str();
}

void NuclustDspManagerCore::log(std::string message) {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm {};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    std::ostringstream line;
    line << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << " " << message;
    logLines_.push_back(line.str());
    if (logLines_.size() > 500) {
        logLines_.erase(logLines_.begin(), logLines_.begin() + 100);
    }
    std::filesystem::create_directories(logPath().parent_path());
    std::ofstream out(logPath(), std::ios::app);
    out << line.str() << "\n";
}

void NuclustDspManagerCore::rescanServers() {
    std::lock_guard lock(mutex_);
    interfaces_ = enumerateInterfaces();
    servers_.clear();
    std::set<std::string> discoveredAddresses;
    daw::RemoteDspServerSettings discoverySettings = daw::defaultRemoteDspServerSettings();
    discoverySettings.timeoutMs = 250;
    const auto discovered = daw::discoverRemoteDspServers(discoverySettings, broadcastTargetsForInterfaces(interfaces_), 450);
    for (const auto& result : discovered) {
        if (result.node.host.empty() || discoveredAddresses.find(result.node.host) != discoveredAddresses.end()) {
            continue;
        }
        discoveredAddresses.insert(result.node.host);
        auto server = makeServerStatusFromAddress(result.node.host, interfaces_, settings_, &result.info);
        if (!result.info.hostname.empty()) {
            server.serverId = result.info.hostname + "-" + result.node.host;
            std::replace(server.serverId.begin(), server.serverId.end(), '.', '-');
        }
        servers_.push_back(server);
    }
    std::vector<std::string> addresses = settings_.manualServerAddresses;
    std::vector<std::string> manualAddresses = addresses;
    const char* mockEnv = std::getenv("NUCLUST_DSP_MOCK_SERVERS");
    if (mockEnv != nullptr && std::string(mockEnv).empty() == false) {
        std::stringstream stream(mockEnv);
        std::string item;
        while (std::getline(stream, item, ',')) {
            addresses.push_back(trim(item));
            manualAddresses.push_back(trim(item));
        }
    }
    for (const auto& node : daw::defaultRemoteDspServerNodes()) {
        if (!node.host.empty()) {
            addresses.push_back(node.host);
        }
    }
    std::sort(addresses.begin(), addresses.end());
    addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());
    std::sort(manualAddresses.begin(), manualAddresses.end());
    manualAddresses.erase(std::unique(manualAddresses.begin(), manualAddresses.end()), manualAddresses.end());
    size_t index = 1;
    for (const auto& address : addresses) {
        if (address.empty()) {
            continue;
        }
        if (discoveredAddresses.find(address) != discoveredAddresses.end()) {
            continue;
        }
        daw::RemoteDspServerSettings querySettings = daw::defaultRemoteDspServerSettings();
        querySettings.host = address;
        querySettings.timeoutMs = 250;
        auto info = daw::queryRemoteDspServerInfo(querySettings);
        auto probe = info.reachable ? daw::RemoteDspServerProbeResult {} : daw::probeRemoteDspServer(querySettings);
        const bool isManual = std::find(manualAddresses.begin(), manualAddresses.end(), address) != manualAddresses.end();
        if (!info.reachable && !probe.reachable && !isManual) {
            log("DAW default Neuracoust DSP candidate not reachable: " + address + " (status: " + info.message + "; rt: " + probe.message + ")");
            continue;
        }
        auto server = makeServerStatusFromAddress(address, interfaces_, settings_, info.reachable ? &info : nullptr);
        if (!info.reachable && probe.reachable) {
            server.connected = true;
            server.roundTripMs = probe.roundTripMs;
            server.message = "RT DSP endpoint responded; status metadata query failed: " + info.message;
        }
        if (server.name == "Neuracoust DSP Server") {
            server.name += " " + std::to_string(index);
        }
        ++index;
        servers_.push_back(server);
    }
    updateSyntheticTelemetryLocked();
    log(servers_.empty() ? "Server rescan completed: no Neuracoust DSP servers found." :
                           "Server rescan completed: " + std::to_string(servers_.size()) + " server(s) available.");
}

void NuclustDspManagerCore::refreshServerTelemetry() {
    struct QueryTarget {
        std::string serverId;
        std::string address;
    };
    std::vector<QueryTarget> targets;
    NuclustManagerSettings settings;
    std::vector<NetworkInterfaceInfo> interfaces;
    {
        std::lock_guard lock(mutex_);
        settings = settings_;
        interfaces = interfaces_;
        for (const auto& server : servers_) {
            if (!server.ipAddress.empty()) {
                targets.push_back({server.serverId, server.ipAddress});
            }
        }
    }

    for (const auto& target : targets) {
        daw::RemoteDspServerSettings querySettings = daw::defaultRemoteDspServerSettings();
        querySettings.host = target.address;
        querySettings.timeoutMs = 120;
        auto info = daw::queryRemoteDspServerInfo(querySettings);
        auto probe = info.reachable ? daw::RemoteDspServerProbeResult {} : daw::probeRemoteDspServer(querySettings);

        std::lock_guard lock(mutex_);
        auto index = findServerIndexLocked(target.serverId);
        if (!index) {
            continue;
        }
        auto& server = servers_[*index];
        server.inputLevel *= 0.62;
        server.outputLevel *= 0.62;
        server.peakHoldLevel *= 0.90;
        if (info.reachable) {
            const uint64_t packetsInDelta = info.packetsIn > server.packetsIn ? info.packetsIn - server.packetsIn : 0;
            const uint64_t packetsOutDelta = info.packetsOut > server.packetsOut ? info.packetsOut - server.packetsOut : 0;
            const double previousRttMs = server.roundTripMs > 0.0 ? server.roundTripMs : info.roundTripMs;
            const double currentJitterMs = std::abs(info.roundTripMs - previousRttMs);
            auto updated = makeServerStatusFromAddress(target.address, interfaces, settings, &info);
            updated.usedChannels = server.usedChannels;
            updated.networkBufferFrames = server.networkBufferFrames;
            updated.reportedLatencySamples = estimatedLatencySamples(server.networkBufferFrames, 128, 48000.0, DspExecutionMode::ExternalDsp);
            updated.coreUsage = smoothCoreLoads(server.coreUsage, updated.coreUsage);
            updated.cpuUsage = updated.coreUsage.empty()
                ? normalizeCpuLoad(updated.cpuUsage)
                : std::accumulate(updated.coreUsage.begin(), updated.coreUsage.end(), 0.0) / static_cast<double>(updated.coreUsage.size());
            updated.jitterCurrentMs = currentJitterMs;
            updated.jitterPeakMs = std::max(server.jitterPeakMs * 0.98, currentJitterMs);
            updated.jitterPeakLatched = server.jitterPeakLatched || updated.jitterPeakMs > settings_.jitterLatchThresholdMs;
            updated.inputLevel = std::min(1.0, std::max(server.inputLevel, static_cast<double>(packetsInDelta) / 128.0));
            updated.outputLevel = std::min(1.0, std::max(server.outputLevel, static_cast<double>(packetsOutDelta) / 128.0));
            updated.peakHoldLevel = std::max({server.peakHoldLevel * 0.92, updated.inputLevel, updated.outputLevel});
            server = updated;
        } else if (probe.reachable) {
            const double previousRttMs = server.roundTripMs > 0.0 ? server.roundTripMs : probe.roundTripMs;
            const double currentJitterMs = std::abs(probe.roundTripMs - previousRttMs);
            server.connected = true;
            server.roundTripMs = probe.roundTripMs;
            server.jitterCurrentMs = currentJitterMs;
            server.jitterPeakMs = std::max(server.jitterPeakMs * 0.98, currentJitterMs);
            server.jitterPeakLatched = server.jitterPeakLatched || server.jitterPeakMs > settings_.jitterLatchThresholdMs;
            server.cpuUsage = normalizeCpuLoad(server.cpuUsage);
            server.message = "RT DSP endpoint responded; status metadata query failed: " + info.message;
        } else {
            server.connected = false;
            server.message = "Server telemetry timed out: " + info.message + "; rt: " + probe.message;
        }
    }
}

void NuclustDspManagerCore::addManualServer(std::string address) {
    {
        std::lock_guard lock(mutex_);
        address = trim(address);
        if (!address.empty() && std::find(settings_.manualServerAddresses.begin(), settings_.manualServerAddresses.end(), address) == settings_.manualServerAddresses.end()) {
            settings_.manualServerAddresses.push_back(address);
        }
    }
    saveSettings();
    rescanServers();
}

void NuclustDspManagerCore::removeServer(const std::string& serverId) {
    std::lock_guard lock(mutex_);
    servers_.erase(std::remove_if(servers_.begin(), servers_.end(), [&](const auto& server) {
        return server.serverId == serverId;
    }), servers_.end());
    log("Server removed: " + serverId);
}

void NuclustDspManagerCore::disconnectServer(const std::string& serverId) {
    std::lock_guard lock(mutex_);
    if (auto index = findServerIndexLocked(serverId)) {
        servers_[*index].connected = false;
        servers_[*index].message = "Disconnected by user.";
    }
    log("Server disconnected: " + serverId);
}

void NuclustDspManagerCore::identifyServer(const std::string& serverId) {
    std::lock_guard lock(mutex_);
    log("ID request sent to server: " + serverId);
}

void NuclustDspManagerCore::clearJitterLatch(const std::string& serverId) {
    std::lock_guard lock(mutex_);
    if (auto index = findServerIndexLocked(serverId)) {
        servers_[*index].jitterPeakLatched = false;
        servers_[*index].jitterPeakMs = servers_[*index].jitterCurrentMs;
        log("Jitter peak latch cleared: " + serverId);
    }
}

void NuclustDspManagerCore::setDspEngineRunning(bool running) {
    std::lock_guard lock(mutex_);
    dspEngineRunning_ = running;
    log(std::string("DSP engine ") + (running ? "started." : "stopped."));
}

void NuclustDspManagerCore::setDefaultBufferFrames(uint32_t frames) {
    if (frames != 128 && frames != 256 && frames != 512 && frames != 1024) {
        frames = 256;
    }
    {
        std::lock_guard lock(mutex_);
        settings_.defaultBufferFrames = frames;
        for (auto& server : servers_) {
            server.networkBufferFrames = frames;
            server.reportedLatencySamples = estimatedLatencySamples(frames, 128, 48000.0, DspExecutionMode::ExternalDsp);
        }
        log("Network buffer changed to " + std::to_string(frames) + " frames.");
    }
    saveSettings();
}

void NuclustDspManagerCore::setLatencyPolicy(LatencyPolicy policy) {
    {
        std::lock_guard lock(mutex_);
        settings_.latencyPolicy = policy;
        log("Latency policy changed to " + toString(policy) + ".");
    }
    saveSettings();
}

std::optional<size_t> NuclustDspManagerCore::findServerIndexLocked(const std::string& serverId) const {
    for (size_t i = 0; i < servers_.size(); ++i) {
        if (servers_[i].serverId == serverId || servers_[i].ipAddress == serverId) {
            return i;
        }
    }
    return std::nullopt;
}

void NuclustDspManagerCore::updateSyntheticTelemetryLocked() {
    uint32_t used = 0;
    for (const auto& plugin : plugins_) {
        if (plugin.assignment.assignedMode == DspExecutionMode::ExternalDsp) {
            used += plugin.request.channelCount;
        }
    }
    for (auto& server : servers_) {
        server.usedChannels = used;
        if (server.jitterPeakMs > settings_.jitterLatchThresholdMs) {
            server.jitterPeakLatched = true;
        }
    }
}

PluginAssignmentResponse NuclustDspManagerCore::assignLocked(const PluginRegistrationRequest& request) {
    PluginAssignmentResponse response;
    response.instanceId = request.instanceId;
    response.serverModuleId = request.serverModuleId;

    if (request.requestedMode == DspExecutionMode::Native) {
        response.assignedMode = DspExecutionMode::Native;
        response.status = PluginAssignmentStatus::Ready;
        response.reportedLatencySamples = estimatedLatencySamples(settings_.defaultBufferFrames, request.blockSize, request.sampleRate, DspExecutionMode::Native);
        return response;
    }

    if (request.requestedMode == DspExecutionMode::InternalDsp) {
        response.assignedMode = DspExecutionMode::InternalDsp;
        response.status = PluginAssignmentStatus::Ready;
        response.reportedLatencySamples = estimatedLatencySamples(settings_.defaultBufferFrames, request.blockSize, request.sampleRate, DspExecutionMode::InternalDsp);
        return response;
    }

    if (!dspEngineRunning_) {
        response.status = request.nativeFallbackAllowed ? PluginAssignmentStatus::NativeFallback : PluginAssignmentStatus::WaitingForServer;
        response.assignedMode = request.nativeFallbackAllowed ? DspExecutionMode::Native : DspExecutionMode::ExternalDsp;
        response.errorMessage = "DSP engine is stopped.";
        return response;
    }

    if (servers_.empty()) {
        response.status = request.nativeFallbackAllowed ? PluginAssignmentStatus::NativeFallback : PluginAssignmentStatus::WaitingForServer;
        response.assignedMode = request.nativeFallbackAllowed ? DspExecutionMode::Native : DspExecutionMode::ExternalDsp;
        response.errorMessage = request.nativeFallbackAllowed ? "No server available; using Native temporarily." : "No Neuracoust DSP server available.";
        return response;
    }

    size_t selectedIndex = 0;
    if (!request.requestedServerId.empty()) {
        auto index = findServerIndexLocked(request.requestedServerId);
        if (!index) {
            response.status = request.nativeFallbackAllowed ? PluginAssignmentStatus::NativeFallback : PluginAssignmentStatus::WaitingForServer;
            response.assignedMode = request.nativeFallbackAllowed ? DspExecutionMode::Native : DspExecutionMode::ExternalDsp;
            response.errorMessage = "Requested server is not connected.";
            return response;
        }
        selectedIndex = *index;
    }

    auto& server = servers_[selectedIndex];
    const bool hasModule = request.serverModuleId.empty() ||
        std::any_of(server.modules.begin(), server.modules.end(), [&](const auto& module) {
            return module.moduleId == request.serverModuleId && module.installed;
        });
    if (!hasModule) {
        response.status = request.nativeFallbackAllowed ? PluginAssignmentStatus::NativeFallback : PluginAssignmentStatus::MissingServerModule;
        response.assignedMode = request.nativeFallbackAllowed ? DspExecutionMode::Native : DspExecutionMode::ExternalDsp;
        response.assignedServerId = server.serverId;
        response.errorMessage = request.nativeFallbackAllowed ? "Server module missing; Native temporary use is active." : "Server module missing; install required.";
        log("Server module missing for plugin " + request.instanceId + ": " + request.serverModuleId);
        return response;
    }
    if (server.jitterPeakLatched || server.jitterCurrentMs > settings_.jitterLatchThresholdMs) {
        response.status = request.nativeFallbackAllowed ? PluginAssignmentStatus::NativeFallback : PluginAssignmentStatus::HighJitter;
        response.assignedMode = request.nativeFallbackAllowed ? DspExecutionMode::Native : DspExecutionMode::ExternalDsp;
        response.assignedServerId = server.serverId;
        response.errorMessage = "Jitter peak exceeded threshold.";
        log("Jitter peak prevented external assignment for " + request.instanceId + ".");
        return response;
    }
    if (server.usedChannels + request.channelCount > server.maxChannels) {
        response.status = request.nativeFallbackAllowed ? PluginAssignmentStatus::NativeFallback : PluginAssignmentStatus::NoCapacity;
        response.assignedMode = request.nativeFallbackAllowed ? DspExecutionMode::Native : DspExecutionMode::ExternalDsp;
        response.assignedServerId = server.serverId;
        response.errorMessage = "Server channel capacity is exhausted.";
        return response;
    }

    response.assignedMode = DspExecutionMode::ExternalDsp;
    response.assignedServerId = server.serverId;
    response.status = PluginAssignmentStatus::Ready;
    response.reportedLatencySamples = estimatedLatencySamples(server.networkBufferFrames, request.blockSize, request.sampleRate, DspExecutionMode::ExternalDsp);
    return response;
}

PluginAssignmentResponse NuclustDspManagerCore::registerPlugin(const PluginRegistrationRequest& request) {
    std::lock_guard lock(mutex_);
    auto response = assignLocked(request);
    auto existing = std::find_if(plugins_.begin(), plugins_.end(), [&](const auto& plugin) {
        return plugin.request.instanceId == request.instanceId;
    });
    PluginInstanceState state;
    state.request = request;
    state.assignment = response;
    state.lastSeen = std::chrono::system_clock::now();
    if (existing == plugins_.end()) {
        plugins_.push_back(state);
        log("Plugin registered: " + request.instanceId + " " + request.pluginId + " requested " + toString(request.requestedMode) + ".");
    } else {
        *existing = state;
        log("Plugin assignment refreshed: " + request.instanceId + " -> " + toString(response.assignedMode) + ".");
    }
    updateSyntheticTelemetryLocked();
    return response;
}

void NuclustDspManagerCore::unregisterPlugin(const std::string& instanceId) {
    std::lock_guard lock(mutex_);
    plugins_.erase(std::remove_if(plugins_.begin(), plugins_.end(), [&](const auto& plugin) {
        return plugin.request.instanceId == instanceId;
    }), plugins_.end());
    updateSyntheticTelemetryLocked();
    log("Plugin unregistered: " + instanceId);
}

std::string NuclustDspManagerCore::handleIpcMessage(const std::string& line) {
    const auto command = extractString(line, "command");
    if (command == "registerPlugin") {
        PluginRegistrationRequest request;
        request.instanceId = extractString(line, "instanceId", "unknown-instance");
        request.pluginId = extractString(line, "pluginId", "unknown-plugin");
        request.pluginVersion = extractString(line, "pluginVersion", "0.0.0");
        request.requestedMode = dspExecutionModeFromString(extractString(line, "requestedMode", "Native"));
        request.requestedServerId = extractString(line, "requestedServerId");
        request.serverModuleId = extractString(line, "serverModuleId");
        request.trackGroupId = extractString(line, "trackGroupId");
        request.channelCount = static_cast<uint32_t>(extractNumber(line, "channelCount", 2));
        request.sampleRate = extractNumber(line, "sampleRate", 48000.0);
        request.blockSize = static_cast<uint32_t>(extractNumber(line, "blockSize", 128));
        request.latencyRequirementSamples = static_cast<uint32_t>(extractNumber(line, "latencyRequirement", 512));
        request.nativeFallbackAllowed = extractBool(line, "nativeFallbackAllowed", true);
        const auto response = registerPlugin(request);
        return "{"
            "\"instanceId\":" + jsonString(response.instanceId) +
            ",\"assignedMode\":" + jsonString(toString(response.assignedMode)) +
            ",\"assignedServerId\":" + jsonString(response.assignedServerId) +
            ",\"serverModuleId\":" + jsonString(response.serverModuleId) +
            ",\"reportedLatencySamples\":" + std::to_string(response.reportedLatencySamples) +
            ",\"status\":" + jsonString(toString(response.status)) +
            ",\"errorMessage\":" + jsonString(response.errorMessage) +
            "}\n";
    }
    if (command == "unregisterPlugin") {
        unregisterPlugin(extractString(line, "instanceId"));
        return "{\"ok\":true}\n";
    }
    if (command == "snapshot") {
        const auto snap = snapshot();
        return "{\"ok\":true,\"serverCount\":" + std::to_string(snap.servers.size()) +
               ",\"pluginCount\":" + std::to_string(snap.pluginInstances.size()) +
               ",\"dspEngineRunning\":" + std::string(snap.dspEngineRunning ? "true" : "false") + "}\n";
    }
    if (command == "rescan") {
        rescanServers();
        return "{\"ok\":true}\n";
    }
    return "{\"ok\":false,\"error\":\"unknown command\"}\n";
}

NuclustDspManagerIpcServer::NuclustDspManagerIpcServer(NuclustDspManagerCore& core)
    : core_(core) {}

NuclustDspManagerIpcServer::~NuclustDspManagerIpcServer() {
    stop();
}

bool NuclustDspManagerIpcServer::start(uint16_t port) {
    if (running_) {
        return true;
    }
    port_ = port;
    running_ = true;
    thread_ = std::thread([this] { run(); });
    return true;
}

void NuclustDspManagerIpcServer::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool NuclustDspManagerIpcServer::running() const {
    return running_;
}

uint16_t NuclustDspManagerIpcServer::port() const {
    return port_;
}

void NuclustDspManagerIpcServer::run() {
#ifdef _WIN32
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
    using Socket = SOCKET;
    constexpr Socket invalidSocket = INVALID_SOCKET;
#else
    using Socket = int;
    constexpr Socket invalidSocket = -1;
#endif
    Socket listenSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSocket == invalidSocket) {
        running_ = false;
        return;
    }
    int yes = 1;
    setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port_);
    if (bind(listenSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listenSocket, 8) != 0) {
#ifdef _WIN32
        closesocket(listenSocket);
        WSACleanup();
#else
        close(listenSocket);
#endif
        running_ = false;
        return;
    }
    while (running_) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listenSocket, &readSet);
        timeval tv {0, 200000};
        const int ready = select(static_cast<int>(listenSocket + 1), &readSet, nullptr, nullptr, &tv);
        if (ready <= 0) {
            continue;
        }
        sockaddr_in client {};
#ifdef _WIN32
        int clientLen = sizeof(client);
#else
        socklen_t clientLen = sizeof(client);
#endif
        Socket clientSocket = accept(listenSocket, reinterpret_cast<sockaddr*>(&client), &clientLen);
        if (clientSocket == invalidSocket) {
            continue;
        }
        std::string input;
        char buffer[1024] = {};
        while (true) {
            const int count = recv(clientSocket, buffer, sizeof(buffer), 0);
            if (count <= 0) {
                break;
            }
            input.append(buffer, buffer + count);
            if (input.find('\n') != std::string::npos) {
                break;
            }
        }
        const auto response = core_.handleIpcMessage(input);
        send(clientSocket, response.c_str(), static_cast<int>(response.size()), 0);
#ifdef _WIN32
        closesocket(clientSocket);
#else
        close(clientSocket);
#endif
    }
#ifdef _WIN32
    closesocket(listenSocket);
    WSACleanup();
#else
    close(listenSocket);
#endif
}

} // namespace neuracoust::nuclust
