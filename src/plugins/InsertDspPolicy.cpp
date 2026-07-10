#include "plugins/InsertDspPolicy.h"

#include "audio/RemoteDspPluginCatalog.h"

namespace neuracoust::daw {

std::string normalizedInsertDspExecutionMode(const TrackInsertSlot& insert) {
    if (insert.dspExecutionMode == "internal" ||
        insert.dspExecutionMode == "remote_internal" ||
        insert.dspExecutionMode == "external") {
        return insert.dspExecutionMode;
    }
    return "native";
}

bool isLoadedTrackVst3Insert(const TrackInsertSlot& insert) {
    return insert.enabled &&
        (insert.pluginFormat == "VST3" || insert.pluginFormat == "VST3/AU") &&
        insert.pluginName != "No Insert" &&
        !insert.pluginPath.empty();
}

bool trackInsertHasNeuracoustDspModule(const TrackInsertSlot& insert) {
    return !remoteDspCapabilityForInsert(insert, false, true).moduleId.empty();
}

std::string defaultPluginInsertDspExecutionMode(const ProjectDocument& project,
                                                bool globalDspEnabled,
                                                const TrackInsertSlot& insert) {
    if (!globalDspEnabled || !project.appleSiliconCoreIsolationEnabled ||
        !isLoadedTrackVst3Insert(insert)) {
        return "native";
    }
    return "internal";
}

bool normalizeThirdPartyTrackInsertToNative(TrackInsertSlot& insert) {
    if (!isLoadedTrackVst3Insert(insert) || trackInsertHasNeuracoustDspModule(insert)) {
        return false;
    }
    const auto mode = normalizedInsertDspExecutionMode(insert);
    if (mode == "native" || mode == "internal") {
        return false;
    }
    insert.dspExecutionMode = "native";
    insert.assignedDspServerId.clear();
    insert.serverModuleId.clear();
    insert.reportedLatencySamples = 0;
    insert.dspAvailable = true;
    insert.dspLastError.clear();
    return true;
}

const char* insertDspModeBadge(const TrackInsertSlot& insert) {
    const auto mode = normalizedInsertDspExecutionMode(insert);
    if (mode == "remote_internal") {
        return "RINT";
    }
    if (mode == "external") {
        return "EXT";
    }
    if (mode == "internal") {
        return "INT";
    }
    return "NAT";
}

const char* effectiveInsertDspModeBadge(const TrackInsertSlot& insert, const ProjectDocument& project) {
    if (!project.appleSiliconCoreIsolationEnabled) {
        return "NAT";
    }
    return insertDspModeBadge(insert);
}

} // namespace neuracoust::daw
