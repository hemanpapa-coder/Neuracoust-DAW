#include "audio/AsioRuntimeAdapter.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <set>
#endif

namespace neuracoust::daw {

namespace {

std::string trimCopy(const std::string& value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

std::string lowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string stripOuterQuotes(std::string value) {
    value = trimCopy(value);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        return trimCopy(value.substr(1, value.size() - 2));
    }
    return value;
}

std::string firstCommandTokenPreservingUnquotedPaths(std::string value) {
    value = trimCopy(value);
    if (value.empty()) {
        return {};
    }
    if (value.front() == '"') {
        const auto endQuote = value.find('"', 1);
        if (endQuote != std::string::npos) {
            return value.substr(1, endQuote - 1);
        }
    }

    const auto lower = lowerCopy(value);
    const auto dll = lower.find(".dll");
    if (dll != std::string::npos) {
        return value.substr(0, dll + 4);
    }
    const auto ocx = lower.find(".ocx");
    if (ocx != std::string::npos) {
        return value.substr(0, ocx + 4);
    }
    return stripOuterQuotes(value);
}

#if defined(_WIN32)
std::string expandWindowsEnvironmentStrings(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const DWORD required = ExpandEnvironmentStringsA(value.c_str(), nullptr, 0);
    if (required == 0) {
        return value;
    }
    std::string expanded(static_cast<size_t>(required), '\0');
    const DWORD written = ExpandEnvironmentStringsA(value.c_str(), expanded.data(), required);
    if (written == 0 || written > required) {
        return value;
    }
    while (!expanded.empty() && expanded.back() == '\0') {
        expanded.pop_back();
    }
    return expanded;
}

std::string registryStringValue(HKEY key, const char* valueName) {
    DWORD type = 0;
    DWORD byteCount = 0;
    const LONG sizeResult = RegGetValueA(key, nullptr, valueName, RRF_RT_REG_SZ, &type, nullptr, &byteCount);
    if (sizeResult != ERROR_SUCCESS || byteCount <= 1) {
        return {};
    }

    std::string value(static_cast<size_t>(byteCount), '\0');
    const LONG readResult = RegGetValueA(key, nullptr, valueName, RRF_RT_REG_SZ, &type, value.data(), &byteCount);
    if (readResult != ERROR_SUCCESS || byteCount == 0) {
        return {};
    }
    while (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return value;
}

std::string clsidRegistryPath(const std::string& clsid) {
    return clsid.empty() ? std::string{} : "CLSID\\" + clsid + "\\InprocServer32";
}

std::string comServerPathForClsid(const std::string& clsid) {
    const auto path = clsidRegistryPath(clsid);
    if (path.empty()) {
        return {};
    }
    HKEY key = nullptr;
    if (RegOpenKeyExA(HKEY_CLASSES_ROOT, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return {};
    }
    auto serverPath = normalizeAsioComServerPathForDiagnostics(registryStringValue(key, ""));
    RegCloseKey(key);
    return serverPath;
}

void appendAsioDriversFromRoot(HKEY root,
                               const char* path,
                               const char* rootLabel,
                               std::set<std::string>& seen,
                               std::vector<AsioDriverRegistration>& drivers) {
    HKEY asioRoot = nullptr;
    if (RegOpenKeyExA(root, path, 0, KEY_READ, &asioRoot) != ERROR_SUCCESS) {
        return;
    }

    for (DWORD index = 0;; ++index) {
        char driverName[256] {};
        DWORD driverNameSize = static_cast<DWORD>(sizeof(driverName));
        const LONG enumResult = RegEnumKeyExA(asioRoot, index, driverName, &driverNameSize, nullptr, nullptr, nullptr, nullptr);
        if (enumResult == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (enumResult != ERROR_SUCCESS || driverNameSize == 0) {
            continue;
        }

        HKEY driverKey = nullptr;
        if (RegOpenKeyExA(asioRoot, driverName, 0, KEY_READ, &driverKey) != ERROR_SUCCESS) {
            continue;
        }

        const std::string keyName(driverName, driverNameSize);
        AsioDriverRegistration driver;
        driver.registryRoot = rootLabel;
        driver.driverName = keyName;
        driver.clsid = registryStringValue(driverKey, "CLSID");
        driver.description = registryStringValue(driverKey, "Description");
        driver.comServerPath = comServerPathForClsid(driver.clsid);
        driver.comServerFound = asioComServerPathExistsForDiagnostics(driver.comServerPath);
        driver.registered = !driver.clsid.empty();
        driver.deviceId = std::string("asio:") + rootLabel + ":" + (!driver.clsid.empty() ? driver.clsid : keyName);
        if (seen.insert(driver.deviceId).second) {
            drivers.push_back(std::move(driver));
        }
        RegCloseKey(driverKey);
    }

    RegCloseKey(asioRoot);
}

std::string stripAsioPrefix(const std::string& deviceId) {
    constexpr const char* prefix = "asio:";
    return deviceId.rfind(prefix, 0) == 0 ? deviceId.substr(5) : deviceId;
}

bool asioDeviceMatches(const AsioDriverRegistration& driver, const std::string& deviceId) {
    if (deviceId.empty()) {
        return false;
    }
    const auto stripped = stripAsioPrefix(deviceId);
    return driver.deviceId == deviceId ||
           driver.deviceId == "asio:" + stripped ||
           (!driver.clsid.empty() && stripped.find(driver.clsid) != std::string::npos) ||
           (!driver.driverName.empty() && stripped.find(driver.driverName) != std::string::npos);
}

std::string diagnosticForStatus(const AsioAdapterStatus& status) {
    if (!status.supportedPlatform) {
        return "ASIO diagnostic: unsupported platform.";
    }
    std::string summary = "ASIO diagnostic: " + std::to_string(status.registeredDriverCount) + " registered driver(s)";
    if (!status.driverName.empty()) {
        summary += "; selected " + status.driverName;
    }
    if (!status.registryRoot.empty()) {
        summary += " from " + status.registryRoot;
    }
    if (!status.clsid.empty()) {
        summary += "; CLSID " + status.clsid;
    }
    if (!status.comServerPath.empty()) {
        summary += "; COM server " + status.comServerPath;
    }
    summary += status.comServerFound ? "; COM server found" : "; COM server not found";
    return summary + ".";
}

#endif

} // namespace

std::vector<AsioDriverRegistration> enumerateAsioDriverRegistrations() {
#if defined(_WIN32)
    std::set<std::string> seen;
    std::vector<AsioDriverRegistration> drivers;
    appendAsioDriversFromRoot(HKEY_LOCAL_MACHINE, "SOFTWARE\\ASIO", "hklm", seen, drivers);
    appendAsioDriversFromRoot(HKEY_CURRENT_USER, "SOFTWARE\\ASIO", "hkcu", seen, drivers);
    appendAsioDriversFromRoot(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\ASIO", "hklm-wow6432", seen, drivers);
    return drivers;
#else
    return {};
#endif
}

std::string normalizeAsioComServerPathForDiagnostics(const std::string& registryValue) {
    auto path = firstCommandTokenPreservingUnquotedPaths(registryValue);
#if defined(_WIN32)
    path = expandWindowsEnvironmentStrings(path);
#endif
    return stripOuterQuotes(path);
}

bool asioComServerPathExistsForDiagnostics(const std::string& registryValue) {
    const auto path = normalizeAsioComServerPathForDiagnostics(registryValue);
    if (path.empty()) {
        return false;
    }
    std::error_code error;
    return std::filesystem::exists(path, error) && std::filesystem::is_regular_file(path, error);
}

AsioAdapterStatus asioAdapterStatusForDeviceId(const std::string& deviceId) {
    AsioAdapterStatus status;
    status.deviceId = deviceId;
#if !defined(_WIN32)
    status.message = "ASIO is only available on Windows builds.";
    status.diagnosticSummary = "ASIO diagnostic: unsupported platform.";
    return status;
#else
    status.supportedPlatform = true;
    const auto drivers = enumerateAsioDriverRegistrations();
    status.registeredDriverCount = drivers.size();
    const auto found = std::find_if(drivers.begin(), drivers.end(), [&](const AsioDriverRegistration& driver) {
        return asioDeviceMatches(driver, deviceId);
    });
    if (found == drivers.end()) {
        status.message = "ASIO runtime adapter is not linked, and no matching ASIO driver registration was found.";
        status.diagnosticSummary = diagnosticForStatus(status);
        return status;
    }

    status.driverRegistered = found->registered;
    status.comServerFound = found->comServerFound;
    status.registryRoot = found->registryRoot;
    status.driverName = !found->description.empty() ? found->description : found->driverName;
    status.clsid = found->clsid;
    status.comServerPath = found->comServerPath;
    status.message = "ASIO runtime adapter is not linked.";
    if (status.driverRegistered && status.comServerFound) {
        status.message += " Driver registration and COM server were found: " + status.driverName + ".";
    } else if (status.driverRegistered) {
        status.message += " Driver registration was found, but the COM server path was not found: " + status.driverName + ".";
    } else {
        status.message += " Driver key was found, but no CLSID was registered: " + status.driverName + ".";
    }
    status.diagnosticSummary = diagnosticForStatus(status);
    return status;
#endif
}

} // namespace neuracoust::daw
