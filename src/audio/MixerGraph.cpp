#include "audio/MixerGraph.h"
#include <algorithm>
#include <map>
#include <set>

namespace neuracoust::daw {

namespace {

bool isAudioTrackTypeAlias(const std::string& type) {
    return type == "audio" ||
        type == "mono" ||
        type == "stereo" ||
        type == "audio_mono" ||
        type == "audio_stereo";
}

std::string cleanTrackType(const TrackState& track) {
    if (track.name == "Master") {
        return "master";
    }
    if (track.name == "Monitor") {
        return "monitor";
    }
    if (track.trackType == "basic_folder") {
        return "folder";
    }
    if (track.trackType == "routing_folder") {
        return "bus_folder";
    }
    if (isAudioTrackTypeAlias(track.trackType)) {
        return "audio";
    }
    if (!track.trackType.empty()) {
        return track.trackType;
    }
    if (track.name.rfind("Bus Folder ", 0) == 0) {
        return "bus_folder";
    }
    if (track.name.rfind("Folder ", 0) == 0) {
        return "folder";
    }
    if (track.name.rfind("VCA ", 0) == 0) {
        return "vca";
    }
    if (track.inputBus.rfind("Bus ", 0) == 0) {
        return "aux";
    }
    return "audio";
}

bool routeKindMayOwnTimelineAudio(MixerRouteKind kind) {
    return kind == MixerRouteKind::Audio || kind == MixerRouteKind::Instrument;
}

bool isPhysicalOutputName(const std::string& busName) {
    return busName.rfind("Main", 0) == 0 ||
        busName.find(" Out ") != std::string::npos ||
        busName.find(" Output") != std::string::npos ||
        busName.find("Default Output") != std::string::npos;
}

void appendCommonProcessors(const TrackState& track, MixerRouteNode& route) {
    if (route.kind == MixerRouteKind::Instrument) {
        route.processors.push_back({MixerProcessorKind::MidiSource, "MIDI Regions", false, false});
        route.processors.push_back({
            MixerProcessorKind::Instrument,
            track.instrument.pluginName.empty() ? std::string("No Instrument") : track.instrument.pluginName,
            track.instrument.enabled && !track.instrument.bypassed,
            false
        });
    } else if (routeKindMayOwnTimelineAudio(route.kind)) {
        route.processors.push_back({MixerProcessorKind::ClipSource, "Timeline Clips", true, false});
    }
    route.processors.push_back({MixerProcessorKind::Trim, "Trim", true, false});
    for (const auto& insert : track.inserts) {
        if (!insert.enabled) {
            continue;
        }
        route.processors.push_back({
            MixerProcessorKind::Insert,
            insert.pluginName.empty() ? std::string("Insert") : insert.pluginName,
            !insert.bypassed,
            false
        });
    }
    for (const auto& send : track.sends) {
        if (!send.enabled || send.busName.empty()) {
            continue;
        }
        route.processors.push_back({
            MixerProcessorKind::Send,
            send.busName,
            false,
            true
        });
    }
    route.processors.push_back({MixerProcessorKind::Fader, "Fader", true, false});
    route.processors.push_back({MixerProcessorKind::Pan, "Pan", true, false});
    route.processors.push_back({MixerProcessorKind::Meter, "Meter", false, true});
    if (route.kind == MixerRouteKind::Monitor) {
        route.processors.push_back({MixerProcessorKind::MonitorDsp, "Monitor DSP", true, false});
    }
    route.processors.push_back({MixerProcessorKind::Output, route.outputBus, false, false});
}

unsigned int routeReportedLatencySamples(const TrackState& track) {
    unsigned int latency = 0;
    for (const auto& insert : track.inserts) {
        if (!insert.enabled || insert.bypassed) {
            continue;
        }
        latency += insert.reportedLatencySamples;
    }
    return latency;
}

const MixerRouteNode* findRouteByName(const MixerGraph& graph, const std::string& name) {
    const auto it = std::find_if(graph.routes.begin(), graph.routes.end(), [&](const MixerRouteNode& route) {
        return route.name == name;
    });
    return it == graph.routes.end() ? nullptr : &*it;
}

const MixerRouteNode* findRouteReceivingBus(const MixerGraph& graph, const std::string& busName) {
    const auto inputIt = std::find_if(graph.routes.begin(), graph.routes.end(), [&](const MixerRouteNode& route) {
        return route.inputBus == busName;
    });
    if (inputIt != graph.routes.end()) {
        return &*inputIt;
    }
    return findRouteByName(graph, busName);
}

void addEdgeForBus(MixerGraph& graph,
                   const MixerRouteNode& source,
                   const std::string& busName,
                   bool isSend,
                   bool preFader,
                   float gainDb,
                   float pan) {
    if (busName.empty()) {
        return;
    }
    MixerGraphEdge edge;
    edge.sourceRoute = source.name;
    edge.busName = busName;
    edge.send = isSend;
    edge.preFader = preFader;
    edge.gainDb = gainDb;
    edge.pan = pan;
    if (const auto* destination = findRouteReceivingBus(graph, busName)) {
        edge.destinationRoute = destination->name;
    } else if (isPhysicalOutputName(busName)) {
        edge.physicalOutput = true;
        edge.destinationRoute = busName;
    } else {
        graph.warnings.push_back(source.name + " routes to unresolved bus: " + busName);
    }
    graph.edges.push_back(std::move(edge));
}

std::vector<std::string> computeRenderOrder(MixerGraph& graph) {
    std::map<std::string, size_t> routeIndex;
    std::map<std::string, std::set<std::string>> outgoing;
    std::map<std::string, int> indegree;
    for (size_t index = 0; index < graph.routes.size(); ++index) {
        routeIndex[graph.routes[index].name] = index;
        indegree[graph.routes[index].name] = 0;
    }
    for (const auto& edge : graph.edges) {
        if (edge.physicalOutput || routeIndex.find(edge.destinationRoute) == routeIndex.end() ||
            routeIndex.find(edge.sourceRoute) == routeIndex.end()) {
            continue;
        }
        if (edge.sourceRoute == edge.destinationRoute) {
            graph.warnings.push_back("Mixer route feeds itself: " + edge.sourceRoute);
            continue;
        }
        if (outgoing[edge.sourceRoute].insert(edge.destinationRoute).second) {
            ++indegree[edge.destinationRoute];
        }
    }

    std::vector<std::string> ready;
    for (const auto& route : graph.routes) {
        if (indegree[route.name] == 0) {
            ready.push_back(route.name);
        }
    }
    std::vector<std::string> order;
    while (!ready.empty()) {
        const auto current = ready.front();
        ready.erase(ready.begin());
        order.push_back(current);
        for (const auto& next : outgoing[current]) {
            --indegree[next];
            if (indegree[next] == 0) {
                ready.push_back(next);
            }
        }
    }
    if (order.size() != graph.routes.size()) {
        graph.warnings.push_back("Mixer routing contains a cycle; falling back to project track order for cyclic routes");
        for (const auto& route : graph.routes) {
            if (std::find(order.begin(), order.end(), route.name) == order.end()) {
                order.push_back(route.name);
            }
        }
    }
    return order;
}

void computeRouteLatencies(MixerGraph& graph) {
    std::map<std::string, unsigned int> pathLatencyByRoute;
    std::map<std::string, size_t> routeIndexByName;
    for (size_t index = 0; index < graph.routes.size(); ++index) {
        routeIndexByName[graph.routes[index].name] = index;
        pathLatencyByRoute[graph.routes[index].name] = graph.routes[index].selfLatencySamples;
    }

    for (const auto& routeName : graph.renderOrder) {
        const auto routeIndexIt = routeIndexByName.find(routeName);
        if (routeIndexIt == routeIndexByName.end()) {
            continue;
        }
        auto& route = graph.routes[routeIndexIt->second];
        route.pathLatencySamples = std::max(route.pathLatencySamples, pathLatencyByRoute[route.name]);
        graph.maxPathLatencySamples = std::max(graph.maxPathLatencySamples, route.pathLatencySamples);
        for (const auto& edge : graph.edges) {
            if (edge.sourceRoute != route.name || edge.physicalOutput || edge.destinationRoute.empty()) {
                continue;
            }
            const auto destinationIt = routeIndexByName.find(edge.destinationRoute);
            if (destinationIt == routeIndexByName.end()) {
                continue;
            }
            const auto& destination = graph.routes[destinationIt->second];
            const unsigned int destinationPathLatency = route.pathLatencySamples + destination.selfLatencySamples;
            pathLatencyByRoute[destination.name] = std::max(pathLatencyByRoute[destination.name], destinationPathLatency);
        }
    }
}

} // namespace

MixerRouteKind mixerRouteKindForTrack(const TrackState& track) {
    const auto type = cleanTrackType(track);
    if (type == "audio") {
        return MixerRouteKind::Audio;
    }
    if (type == "aux") {
        return MixerRouteKind::Aux;
    }
    if (type == "master") {
        return MixerRouteKind::Master;
    }
    if (type == "monitor") {
        return MixerRouteKind::Monitor;
    }
    if (type == "folder") {
        return MixerRouteKind::BasicFolder;
    }
    if (type == "bus_folder") {
        return MixerRouteKind::RoutingFolder;
    }
    if (type == "vca") {
        return MixerRouteKind::Vca;
    }
    if (type == "midi") {
        return MixerRouteKind::Midi;
    }
    if (type == "instrument") {
        return MixerRouteKind::Instrument;
    }
    return MixerRouteKind::Unknown;
}

bool mixerRouteCarriesAudio(MixerRouteKind kind) {
    return kind == MixerRouteKind::Audio ||
        kind == MixerRouteKind::Aux ||
        kind == MixerRouteKind::Master ||
        kind == MixerRouteKind::Monitor ||
        kind == MixerRouteKind::RoutingFolder ||
        kind == MixerRouteKind::Instrument;
}

bool mixerRouteIsControlOnly(MixerRouteKind kind) {
    return kind == MixerRouteKind::BasicFolder ||
        kind == MixerRouteKind::Vca ||
        kind == MixerRouteKind::Midi;
}

std::string mixerRouteKindName(MixerRouteKind kind) {
    switch (kind) {
        case MixerRouteKind::Audio: return "audio";
        case MixerRouteKind::Aux: return "aux";
        case MixerRouteKind::Master: return "master";
        case MixerRouteKind::Monitor: return "monitor";
        case MixerRouteKind::BasicFolder: return "basic_folder";
        case MixerRouteKind::RoutingFolder: return "routing_folder";
        case MixerRouteKind::Vca: return "vca";
        case MixerRouteKind::Midi: return "midi";
        case MixerRouteKind::Instrument: return "instrument";
        case MixerRouteKind::Unknown: return "unknown";
    }
    return "unknown";
}

MixerGraph buildMixerGraph(const ProjectDocument& project) {
    MixerGraph graph;
    graph.routes.reserve(project.tracks.size());
    for (const auto& track : project.tracks) {
        if (track.name.empty()) {
            continue;
        }
        MixerRouteNode route;
        route.name = track.name;
        route.kind = mixerRouteKindForTrack(track);
        route.inputBus = track.inputBus;
        route.outputBus = track.outputBus;
        route.selfLatencySamples = routeReportedLatencySamples(track);
        route.audioCarrying = mixerRouteCarriesAudio(route.kind);
        route.controlOnly = mixerRouteIsControlOnly(route.kind);
        appendCommonProcessors(track, route);
        graph.routes.push_back(std::move(route));
    }

    for (const auto& track : project.tracks) {
        const auto* source = findRouteByName(graph, track.name);
        if (source == nullptr || !source->audioCarrying) {
            continue;
        }
        addEdgeForBus(graph, *source, track.outputBus, false, false, 0.0f, 0.0f);
        for (const auto& send : track.sends) {
            if (!send.enabled || send.busName.empty()) {
                continue;
            }
            addEdgeForBus(graph, *source, send.busName, true, send.preFader, send.gainDb, send.pan);
        }
    }
    for (const auto& track : project.tracks) {
        if (track.controlMasterTrackName.empty() || track.controlMasterTrackName == track.name) {
            continue;
        }
        const auto* control = findRouteByName(graph, track.controlMasterTrackName);
        const auto* target = findRouteByName(graph, track.name);
        if (control == nullptr || target == nullptr || !control->controlOnly) {
            graph.warnings.push_back(track.name + " references unresolved control master: " + track.controlMasterTrackName);
            continue;
        }
        graph.controlEdges.push_back({control->name, target->name});
    }

    graph.renderOrder = computeRenderOrder(graph);
    computeRouteLatencies(graph);
    return graph;
}

} // namespace neuracoust::daw
