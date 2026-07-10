#pragma once

#include "core/DawState.h"
#include <string>

namespace neuracoust::daw {

enum class RemoteDspInsertMode {
    LocalOnly,
    RemoteCapable,
    RemoteActive,
    RemoteUnavailable
};

struct RemoteDspPluginCapability {
    RemoteDspInsertMode mode = RemoteDspInsertMode::LocalOnly;
    std::string moduleId;
    std::string label;
};

RemoteDspPluginCapability remoteDspCapabilityForInsert(const TrackInsertSlot& insert,
                                                       bool remoteServerEnabled,
                                                       bool remoteServerReachable);

} // namespace neuracoust::daw
