#pragma once

#include "project/ProjectDocument.h"
#include <string>
#include <vector>

namespace neuracoust::daw {

enum class MixerRouteKind {
    Audio,
    Aux,
    Master,
    Monitor,
    BasicFolder,
    RoutingFolder,
    Vca,
    Midi,
    Instrument,
    Unknown
};

enum class MixerProcessorKind {
    ClipSource,
    MidiSource,
    Instrument,
    Trim,
    Insert,
    Send,
    Fader,
    Pan,
    Meter,
    Output,
    MonitorDsp
};

struct MixerProcessorNode {
    MixerProcessorKind kind = MixerProcessorKind::Meter;
    std::string label;
    bool audioMutating = false;
    bool tap = false;
};

struct MixerRouteNode {
    std::string name;
    MixerRouteKind kind = MixerRouteKind::Unknown;
    std::string inputBus;
    std::string outputBus;
    int channelCount = 2;
    unsigned int selfLatencySamples = 0;
    unsigned int pathLatencySamples = 0;
    bool strictIo = true;
    bool audioCarrying = false;
    bool controlOnly = false;
    std::vector<MixerProcessorNode> processors;
};

struct MixerGraphEdge {
    std::string sourceRoute;
    std::string destinationRoute;
    std::string busName;
    bool send = false;
    bool preFader = false;
    float gainDb = 0.0f;
    float pan = 0.0f;
    bool physicalOutput = false;
};

struct MixerControlEdge {
    std::string controlRoute;
    std::string targetRoute;
};

struct MixerGraph {
    std::vector<MixerRouteNode> routes;
    std::vector<MixerGraphEdge> edges;
    std::vector<MixerControlEdge> controlEdges;
    std::vector<std::string> renderOrder;
    unsigned int maxPathLatencySamples = 0;
    std::vector<std::string> warnings;
};

MixerRouteKind mixerRouteKindForTrack(const TrackState& track);
bool mixerRouteCarriesAudio(MixerRouteKind kind);
bool mixerRouteIsControlOnly(MixerRouteKind kind);
std::string mixerRouteKindName(MixerRouteKind kind);
MixerGraph buildMixerGraph(const ProjectDocument& project);

} // namespace neuracoust::daw
