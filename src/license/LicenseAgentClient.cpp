#include "license/LicenseAgentClient.h"
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace neuracoust::daw {

#if defined(_WIN32)
#define NEURACOUST_POPEN _popen
#define NEURACOUST_PCLOSE _pclose
#else
#define NEURACOUST_POPEN popen
#define NEURACOUST_PCLOSE pclose
#endif

LicenseAgentClient::LicenseAgentClient(std::string baseUrl)
    : baseUrl_(std::move(baseUrl)) {}

LicenseAgentClient LicenseAgentClient::defaultClient() {
    return LicenseAgentClient("http://127.0.0.1:48173");
}

static std::string readCommand(const std::string& command) {
    std::string output;
    FILE* pipe = NEURACOUST_POPEN(command.c_str(), "r");
    if (pipe == nullptr) {
        return output;
    }
    char buffer[512] = {};
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
    NEURACOUST_PCLOSE(pipe);
    return output;
}

LicenseStatus LicenseAgentClient::check(const std::string& productAppId) const {
    LicenseStatus status;
    std::ostringstream command;
    command << "curl -fsS --max-time 1 \""
            << baseUrl_
            << "/check?appId="
            << productAppId
            << "\""
#if defined(_WIN32)
            << " 2>NUL";
#else
            << " 2>/dev/null";
#endif

    const auto response = readCommand(command.str());
    if (response.empty()) {
        status.message = "Neuracoust License Agent is not reachable. Launch License Agent to unlock the DAW.";
        return status;
    }

    status.agentReachable = true;
    status.authorized =
        response.find("\"authorized\":true") != std::string::npos ||
        response.find("\"valid\":true") != std::string::npos ||
        response.find("\"ok\":true") != std::string::npos;
    status.tier = response.find("Studio") != std::string::npos ? "Studio" :
                  response.find("Pro") != std::string::npos ? "Pro" :
                  response.find("Signal") != std::string::npos ? "Signal" :
                  response.find("Free") != std::string::npos ? "Free" : "Unknown";
    status.message = status.authorized
        ? "License Agent authorized Neuracoust DAW."
        : "License Agent responded, but Neuracoust DAW is not authorized for this account.";
    return status;
}

} // namespace neuracoust::daw

#undef NEURACOUST_POPEN
#undef NEURACOUST_PCLOSE
