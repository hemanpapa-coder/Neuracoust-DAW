#include "core/DawState.h"

namespace neuracoust::daw {

DawState makeInitialDawState() {
    DawState state;
    state.identity = currentAppIdentity();
    state.license = LicenseAgentClient::defaultClient().check(state.identity.appId);
    state.devices = enumerateAudioDevices();
    state.tracks.resize(4);
    state.tracks[0].name = "Audio 1";
    state.tracks[1].name = "Audio 2";
    state.tracks[2].name = "Master";
    state.tracks[3].name = "Monitor";
    state.tracks[0].trackType = "audio";
    state.tracks[0].outputBus = "Master";
    state.tracks[1].trackType = "audio";
    state.tracks[1].outputBus = "Master";
    state.tracks[2].trackType = "master";
    state.tracks[2].inputBus.clear();
    state.tracks[2].outputBus = "Monitor";
    state.tracks[3].trackType = "monitor";
    state.tracks[3].inputBus = "Monitor";
    state.tracks[3].outputBus = "Main 1-2";
    state.tracks[0].colorHex = "#35BFA8";
    state.tracks[1].colorHex = "#4B84E8";
    state.tracks[2].colorHex = "#9AA0A6";
    state.tracks[3].colorHex = "#6EC6FF";
    state.masterInserts.clear();
    state.vst3Plugins = scanVst3PluginBundles();
    state.remoteDspServer = defaultRemoteDspServerSettings();
    return state;
}

} // namespace neuracoust::daw
