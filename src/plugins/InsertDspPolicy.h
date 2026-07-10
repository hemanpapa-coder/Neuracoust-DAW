#pragma once

// Decides which execution path each plug-in insert runs on. Ported out of the old
// UI (DawWindowController.mm), where it was the only copy — the mode strings live
// in the project model, but the rules that produce them lived nowhere else.
//
// Modes:
//   native          in-process, on the audio thread
//   internal        out-of-process on the isolated performance core (sandbox bridge)
//   remote_internal a Neuracoust DSP module on a Remote Core
//   external        an external DSP server

#include "core/DawState.h"
#include "project/ProjectDocument.h"

#include <string>

namespace neuracoust::daw {

/// Anything not one of the three non-native modes is "native".
std::string normalizedInsertDspExecutionMode(const TrackInsertSlot& insert);

/// A slot holding a real, loadable VST3 (as opposed to an empty placeholder).
bool isLoadedTrackVst3Insert(const TrackInsertSlot& insert);

/// True when a Neuracoust DSP module can serve this insert on a Remote Core.
bool trackInsertHasNeuracoustDspModule(const TrackInsertSlot& insert);

/// The mode a newly added insert should start in.
///
/// With core isolation and monitor DSP both on, every loaded plug-in defaults to
/// `internal`: third-party plug-ins run out-of-process on the isolated core via the
/// sandbox bridge. `native` stays an explicit per-channel opt-in.
std::string defaultPluginInsertDspExecutionMode(const ProjectDocument& project,
                                                bool globalDspEnabled,
                                                const TrackInsertSlot& insert);

/// Server-backed modes need a matching Neuracoust module on a Remote Core. When a
/// third-party insert carries one anyway — from an older project, or a core that
/// went away — reset it to native. `internal` is legal for third-party plug-ins and
/// is left alone. Returns true when the insert was changed.
bool normalizeThirdPartyTrackInsertToNative(TrackInsertSlot& insert);

/// Short badge for the UI: "NAT", "INT", "RINT", "EXT".
const char* insertDspModeBadge(const TrackInsertSlot& insert);

/// Same, but reports "NAT" for every insert when core isolation is off — nothing
/// runs off the audio thread in that case, whatever the slot claims.
const char* effectiveInsertDspModeBadge(const TrackInsertSlot& insert, const ProjectDocument& project);

} // namespace neuracoust::daw
