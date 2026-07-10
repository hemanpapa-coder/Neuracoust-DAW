#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace neuracoust::daw {

struct AsioDriverRegistration {
    std::string deviceId;
    std::string registryRoot;
    std::string driverName;
    std::string clsid;
    std::string description;
    std::string comServerPath;
    bool registered = false;
    bool comServerFound = false;
};

struct AsioAdapterStatus {
    bool supportedPlatform = false;
    bool runtimeAdapterLinked = false;
    bool driverRegistered = false;
    bool comServerFound = false;
    size_t registeredDriverCount = 0;
    std::string deviceId;
    std::string registryRoot;
    std::string driverName;
    std::string clsid;
    std::string comServerPath;
    std::string diagnosticSummary;
    std::string message;
};

std::vector<AsioDriverRegistration> enumerateAsioDriverRegistrations();
AsioAdapterStatus asioAdapterStatusForDeviceId(const std::string& deviceId);
std::string normalizeAsioComServerPathForDiagnostics(const std::string& registryValue);
bool asioComServerPathExistsForDiagnostics(const std::string& registryValue);

} // namespace neuracoust::daw
