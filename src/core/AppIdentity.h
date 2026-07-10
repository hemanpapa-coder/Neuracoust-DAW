#pragma once

#include <string>
#include <vector>

namespace neuracoust::daw {

struct AppIdentity {
    std::string appId;
    std::string productName;
    std::string version;
    std::string copyright;
    std::vector<std::string> allowedTiers;
};

AppIdentity currentAppIdentity();

} // namespace neuracoust::daw
