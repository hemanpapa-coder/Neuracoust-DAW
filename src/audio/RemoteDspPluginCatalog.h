#pragma once

#include "core/DawState.h"
#include "audio/RemoteDspServerClient.h"
#include <string>
#include <vector>

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

/// The same judgement for a MASTER insert, which the project stores as an InsertState. The two
/// structs carry the same identity fields under slightly different names; mapping here keeps the
/// one catalog the single authority on what may leave the host.
RemoteDspPluginCapability remoteDspCapabilityForMasterInsert(const InsertState& insert,
                                                             bool remoteServerEnabled,
                                                             bool remoteServerReachable);

/// An insert's VST3 parameter snapshot as NART (index, 0..1) values — including the 4001E's
/// id-to-index table. Defined beside that table in NeuracoustDspEngine.cpp; declared here so the
/// offline bounce packs exactly what the realtime engine packs.
std::vector<RemoteDspParameterValue> remoteInsertParameterValues(const TrackInsertSlot& insert);

} // namespace neuracoust::daw
