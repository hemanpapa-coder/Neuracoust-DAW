#pragma once

#include <string>
#include <vector>

namespace neuracoust::daw {

enum class DawUiSurface {
    Transport,
    Timeline,
    TrackInspector,
    Mixer,
    Monitor,
    AiAssistant
};

struct DawUiColor {
    unsigned char red = 0;
    unsigned char green = 0;
    unsigned char blue = 0;
    unsigned char alpha = 255;
};

struct DawUiTheme {
    DawUiColor chrome {169, 170, 166, 255};
    DawUiColor panel {104, 106, 103, 255};
    DawUiColor timeline {35, 40, 39, 255};
    DawUiColor trackLane {47, 51, 50, 255};
    DawUiColor selectedTrack {93, 111, 122, 255};
    DawUiColor clipAudio {72, 183, 174, 255};
    DawUiColor accentBlue {51, 137, 219, 255};
    DawUiColor accentAmber {219, 161, 28, 255};
    DawUiColor textPrimary {244, 245, 243, 255};
    DawUiColor textSecondary {202, 206, 202, 255};
};

struct DawUiSurfaceSpec {
    DawUiSurface surface = DawUiSurface::Transport;
    std::string stableId;
    std::string displayName;
    int minimumWidth = 0;
    int minimumHeight = 0;
};

struct DawUiManifest {
    std::string productName;
    std::string version;
    std::string copyright;
    DawUiTheme theme;
    std::vector<DawUiSurfaceSpec> surfaces;
};

DawUiManifest makeDefaultDawUiManifest(const std::string& productName,
                                       const std::string& version,
                                       const std::string& copyright);

const DawUiSurfaceSpec* findDawUiSurface(const DawUiManifest& manifest, DawUiSurface surface);

} // namespace neuracoust::daw
