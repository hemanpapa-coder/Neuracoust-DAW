#include "audio/RemoteDspPluginCatalog.h"

#include <algorithm>
#include <cctype>

namespace neuracoust::daw {

namespace {

std::string lowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool isNeuracoust4001ChannelStrip(const TrackInsertSlot& insert) {
    const auto name = lowerCopy(insert.pluginName);
    const auto path = lowerCopy(insert.pluginPath);
    return name.find("4001e") != std::string::npos ||
        name.find("4001-2") != std::string::npos ||
        name.find("40012") != std::string::npos ||
        name.find("newacoust4001e") != std::string::npos ||
        path.find("4001e") != std::string::npos ||
        path.find("4001-2") != std::string::npos ||
        path.find("40012") != std::string::npos ||
        path.find("newacoust4001e") != std::string::npos;
}

bool isNeuracoustCompressorDm2c(const TrackInsertSlot& insert) {
    const auto name = lowerCopy(insert.pluginName);
    const auto path = lowerCopy(insert.pluginPath);
    const auto searchable = name + " " + path;
    return searchable.find("neuracoust-compressor-dm2c") != std::string::npos ||
        searchable.find("neuracoust compressor dm2c") != std::string::npos ||
        searchable.find("compressor dm2c") != std::string::npos ||
        searchable.find("dm2c") != std::string::npos;
}

bool isNeuracoustCoAir2026(const TrackInsertSlot& insert) {
    const auto name = lowerCopy(insert.pluginName);
    const auto path = lowerCopy(insert.pluginPath);
    const auto searchable = name + " " + path;
    return searchable.find("neuracoust-coair2026") != std::string::npos ||
        searchable.find("neuracoust coair2026") != std::string::npos ||
        searchable.find("neuracoust coair 2026") != std::string::npos ||
        searchable.find("coair2026") != std::string::npos ||
        searchable.find("coair 2026") != std::string::npos ||
        searchable.find("coair") != std::string::npos;
}

bool isNeuracoustQf2dEq(const TrackInsertSlot& insert) {
    const auto name = lowerCopy(insert.pluginName);
    const auto path = lowerCopy(insert.pluginPath);
    const auto searchable = name + " " + path;
    return searchable.find("neuracoust-eq-qf2d") != std::string::npos ||
        searchable.find("neuracoust eq qf2d") != std::string::npos ||
        searchable.find("eq qf2d") != std::string::npos ||
        searchable.find("qf2d") != std::string::npos;
}

// The 525A plugin family: the VST3/AU halves carry this name; the node half is
// na.neuracoust.api525a (tools/node/na_api525a.cpp).
bool isNeuracoust525A(const TrackInsertSlot& insert) {
    const auto name = lowerCopy(insert.pluginName);
    const auto path = lowerCopy(insert.pluginPath);
    const auto searchable = name + " " + path;
    return searchable.find("neuracoust 525a") != std::string::npos ||
        searchable.find("neuracoust-525a") != std::string::npos ||
        searchable.find("api 525a") != std::string::npos ||
        searchable.find("525a") != std::string::npos;
}

bool isNeuracoustMirage991(const TrackInsertSlot& insert) {
    const auto name = lowerCopy(insert.pluginName);
    const auto path = lowerCopy(insert.pluginPath);
    const auto searchable = name + " " + path;
    return searchable.find("neurcoust-mirage-991") != std::string::npos ||
        searchable.find("neuracoust mirage 991") != std::string::npos ||
        searchable.find("mirage991") != std::string::npos ||
        searchable.find("mirage 991") != std::string::npos ||
        searchable.find("neuracoust mirage 991.vst3") != std::string::npos;
}

bool isNeuracoustMirage8(const TrackInsertSlot& insert) {
    const auto name = lowerCopy(insert.pluginName);
    const auto path = lowerCopy(insert.pluginPath);
    const auto searchable = name + " " + path;
    return searchable.find("neuracoust-mirage-8") != std::string::npos ||
        searchable.find("neuracoust mirage 8") != std::string::npos ||
        searchable.find("neuracoust mirage 8.vst3") != std::string::npos ||
        searchable.find("mirage8") != std::string::npos ||
        searchable.find("mirage 8") != std::string::npos ||
        searchable.find("mirage_8") != std::string::npos;
}

bool isNeuracoustMirage901(const TrackInsertSlot& insert) {
    const auto name = lowerCopy(insert.pluginName);
    const auto path = lowerCopy(insert.pluginPath);
    const auto searchable = name + " " + path;
    return searchable.find("neuracoust-mirage-901") != std::string::npos ||
        searchable.find("neuracoust mirage 901") != std::string::npos ||
        searchable.find("neuracoust mirage 901.vst3") != std::string::npos ||
        searchable.find("mirage901") != std::string::npos ||
        searchable.find("mirage 901") != std::string::npos ||
        searchable.find("mirage_901") != std::string::npos;
}

bool isNeuracoustCompressor99(const TrackInsertSlot& insert) {
    const auto name = lowerCopy(insert.pluginName);
    const auto path = lowerCopy(insert.pluginPath);
    const auto searchable = name + " " + path;
    return searchable.find("neuracoust-compressor-99") != std::string::npos ||
        searchable.find("neuracoust compressor 99") != std::string::npos ||
        searchable.find("compressor 99") != std::string::npos;
}

bool isNeuracoustCompressor201(const TrackInsertSlot& insert) {
    const auto name = lowerCopy(insert.pluginName);
    const auto path = lowerCopy(insert.pluginPath);
    const auto searchable = name + " " + path;
    return searchable.find("neuracoust-compressor-201") != std::string::npos ||
        searchable.find("neuracoust compressor 201") != std::string::npos ||
        searchable.find("compressor 201") != std::string::npos ||
        searchable.find("edm201") != std::string::npos ||
        searchable.find("ed21") != std::string::npos;
}

bool isNeuracoustCompLimiter340(const TrackInsertSlot& insert) {
    const auto name = lowerCopy(insert.pluginName);
    const auto path = lowerCopy(insert.pluginPath);
    const auto searchable = name + " " + path;
    return searchable.find("neuracoust-comp-limiter-340") != std::string::npos ||
        searchable.find("neuracoust comp limiter 340") != std::string::npos ||
        searchable.find("comp limiter 340") != std::string::npos ||
        searchable.find("compressor 340") != std::string::npos ||
        searchable.find("limiter 340") != std::string::npos ||
        searchable.find("neuracoustcomplimiter340") != std::string::npos;
}

bool isNeuracoustSpaceSculptor(const TrackInsertSlot& insert) {
    const auto name = lowerCopy(insert.pluginName);
    const auto path = lowerCopy(insert.pluginPath);
    const auto searchable = name + " " + path;
    return searchable.find("neuracoust-space-sculptor") != std::string::npos ||
        searchable.find("neuracoust space sculptor") != std::string::npos ||
        searchable.find("neuracoust space sculptor.vst3") != std::string::npos ||
        searchable.find("space sculptor") != std::string::npos ||
        searchable.find("spacesculptor") != std::string::npos;
}

} // namespace

RemoteDspPluginCapability remoteDspCapabilityForInsert(const TrackInsertSlot& insert,
                                                       bool remoteServerEnabled,
                                                       bool remoteServerReachable) {
    RemoteDspPluginCapability capability;
    capability.label = "Local Only";

    if (!insert.enabled || insert.bypassed || insert.pluginName.empty() || insert.pluginName == "No Insert") {
        return capability;
    }

    if (isNeuracoust4001ChannelStrip(insert)) {
        capability.moduleId = "na.neuracoust.4001e";
    } else if (isNeuracoustCompressorDm2c(insert)) {
        capability.moduleId = "na.neuracoust.compressor.dm2c";
    } else if (isNeuracoustCoAir2026(insert)) {
        capability.moduleId = "na.neuracoust.coair2026";
    } else if (isNeuracoustQf2dEq(insert)) {
        capability.moduleId = "na.neuracoust.eq.qf2d";
    } else if (isNeuracoustCompressor99(insert)) {
        capability.moduleId = "na.neuracoust.compressor99";
    } else if (isNeuracoustCompressor201(insert)) {
        capability.moduleId = "na.neuracoust.compressor201";
    } else if (isNeuracoustCompLimiter340(insert)) {
        capability.moduleId = "na.neuracoust.comp-limiter-340";
    } else if (isNeuracoust525A(insert)) {
        capability.moduleId = "na.neuracoust.api525a";
    } else if (isNeuracoustMirage991(insert)) {
        capability.moduleId = "na.neuracoust.mirage991";
    } else if (isNeuracoustMirage8(insert)) {
        capability.moduleId = "na.neuracoust.mirage8";
    } else if (isNeuracoustMirage901(insert)) {
        capability.moduleId = "na.neuracoust.mirage901";
    } else if (isNeuracoustSpaceSculptor(insert)) {
        capability.moduleId = "na.neuracoust.space-sculptor";
    } else {
        return capability;
    }

    if (!remoteServerEnabled) {
        capability.mode = RemoteDspInsertMode::RemoteCapable;
        capability.label = "Remote DSP Ready";
        return capability;
    }
    if (!remoteServerReachable) {
        capability.mode = RemoteDspInsertMode::RemoteUnavailable;
        capability.label = "Remote DSP Unavailable";
        return capability;
    }

    capability.mode = RemoteDspInsertMode::RemoteActive;
    capability.label = "Remote DSP Active";
    return capability;
}

RemoteDspPluginCapability remoteDspCapabilityForMasterInsert(const InsertState& insert,
                                                             bool remoteServerEnabled,
                                                             bool remoteServerReachable) {
    TrackInsertSlot slot;
    slot.pluginName = insert.pluginName;
    slot.pluginFormat = insert.pluginFormat;
    slot.pluginPath = insert.pluginPath;
    slot.bypassed = insert.bypassed;
    slot.enabled = insert.available;
    slot.dspExecutionMode = insert.dspExecutionMode;
    slot.assignedDspServerId = insert.assignedDspServerId;
    slot.serverModuleId = insert.serverModuleId;
    slot.pluginClassId = insert.pluginClassId;
    slot.pluginClassName = insert.pluginClassName;
    slot.parameters = insert.parameters;
    return remoteDspCapabilityForInsert(slot, remoteServerEnabled, remoteServerReachable);
}

} // namespace neuracoust::daw
