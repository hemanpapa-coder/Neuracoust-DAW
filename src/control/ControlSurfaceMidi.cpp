#include "control/ControlSurfaceMidi.h"

#if !defined(__APPLE__)
namespace neuracoust::daw {
class ControlSurfaceMidi::Impl {};
ControlSurfaceMidi::ControlSurfaceMidi() : impl_(std::make_unique<Impl>()) {}
ControlSurfaceMidi::~ControlSurfaceMidi() = default;
std::vector<ControlSurfaceMidiEndpoint> ControlSurfaceMidi::inputs() const { return {}; }
std::vector<ControlSurfaceMidiEndpoint> ControlSurfaceMidi::outputs() const { return {}; }
bool ControlSurfaceMidi::connect(const std::string&, const std::string&) { return false; }
void ControlSurfaceMidi::disconnect() {}
bool ControlSurfaceMidi::connected() const { return false; }
std::string ControlSurfaceMidi::statusMessage() const { return "Control surfaces are unavailable."; }
std::vector<std::vector<std::uint8_t>> ControlSurfaceMidi::consumeMessages() { return {}; }
bool ControlSurfaceMidi::send(const std::vector<std::uint8_t>&) { return false; }
}
#endif
