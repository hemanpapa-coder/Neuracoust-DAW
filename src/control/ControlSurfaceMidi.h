#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace neuracoust::daw {

struct ControlSurfaceMidiEndpoint {
    std::string id;
    std::string name;
};

class ControlSurfaceMidi {
public:
    ControlSurfaceMidi();
    ~ControlSurfaceMidi();
    ControlSurfaceMidi(const ControlSurfaceMidi&) = delete;
    ControlSurfaceMidi& operator=(const ControlSurfaceMidi&) = delete;

    std::vector<ControlSurfaceMidiEndpoint> inputs() const;
    std::vector<ControlSurfaceMidiEndpoint> outputs() const;
    bool connect(const std::string& inputId, const std::string& outputId);
    void disconnect();
    bool connected() const;
    std::string statusMessage() const;
    std::vector<std::vector<std::uint8_t>> consumeMessages();
    bool send(const std::vector<std::uint8_t>& bytes);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace neuracoust::daw
