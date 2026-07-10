#pragma once

#include <string>
#include <vector>

namespace neuracoust::daw {

struct LicenseStatus {
    bool agentReachable = false;
    bool authorized = false;
    std::string account;
    std::string tier;
    std::string message;
};

class LicenseAgentClient {
public:
    static LicenseAgentClient defaultClient();
    LicenseStatus check(const std::string& productAppId) const;

private:
    explicit LicenseAgentClient(std::string baseUrl);
    std::string baseUrl_;
};

} // namespace neuracoust::daw
