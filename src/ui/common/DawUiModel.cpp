#include "ui/common/DawUiModel.h"

#include <algorithm>

namespace neuracoust::daw {

DawUiManifest makeDefaultDawUiManifest(const std::string& productName,
                                       const std::string& version,
                                       const std::string& copyright) {
    DawUiManifest manifest;
    manifest.productName = productName;
    manifest.version = version;
    manifest.copyright = copyright;
    manifest.surfaces = {
        {DawUiSurface::Transport, "transport", "Transport", 880, 96},
        {DawUiSurface::Timeline, "timeline", "Timeline", 1024, 520},
        {DawUiSurface::TrackInspector, "track-inspector", "Track Inspector", 240, 520},
        {DawUiSurface::Mixer, "mixer", "Mix", 1040, 744},
        {DawUiSurface::Monitor, "monitor", "Monitor", 352, 680},
        {DawUiSurface::AiAssistant, "ai-assistant", "AI Assistant", 560, 390},
    };
    return manifest;
}

const DawUiSurfaceSpec* findDawUiSurface(const DawUiManifest& manifest, DawUiSurface surface) {
    const auto found = std::find_if(manifest.surfaces.begin(), manifest.surfaces.end(), [&](const DawUiSurfaceSpec& spec) {
        return spec.surface == surface;
    });
    return found == manifest.surfaces.end() ? nullptr : &(*found);
}

} // namespace neuracoust::daw
