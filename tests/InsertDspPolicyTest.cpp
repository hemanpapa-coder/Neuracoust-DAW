// Pins the insert DSP execution-mode rules that were ported out of the old UI.
// These strings are persisted in project files, so getting them wrong silently
// reroutes plug-ins away from the isolated core.

#include "plugins/InsertDspPolicy.h"
#include "project/ProjectDocument.h"

#include <cassert>
#include <cstdio>
#include <string>

using namespace neuracoust::daw;

namespace {

TrackInsertSlot loadedVst3(const std::string& mode = "native") {
    TrackInsertSlot insert;
    insert.pluginName = "FabFilter Pro-C 2";
    insert.pluginFormat = "VST3";
    insert.pluginPath = "/Library/Audio/Plug-Ins/VST3/FabFilter Pro-C 2.vst3";
    insert.enabled = true;
    insert.dspExecutionMode = mode;
    return insert;
}

TrackInsertSlot emptySlot() {
    return TrackInsertSlot{};   // "No Insert", format "None", empty path
}

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

} // namespace

int main() {
    // --- normalizedInsertDspExecutionMode -----------------------------------
    check(normalizedInsertDspExecutionMode(loadedVst3("internal")) == "internal", "internal survives");
    check(normalizedInsertDspExecutionMode(loadedVst3("remote_internal")) == "remote_internal", "remote_internal survives");
    check(normalizedInsertDspExecutionMode(loadedVst3("external")) == "external", "external survives");
    check(normalizedInsertDspExecutionMode(loadedVst3("native")) == "native", "native survives");
    check(normalizedInsertDspExecutionMode(loadedVst3("nonsense")) == "native", "unknown mode falls back to native");
    check(normalizedInsertDspExecutionMode(loadedVst3("")) == "native", "empty mode falls back to native");

    // --- isLoadedTrackVst3Insert -------------------------------------------
    check(isLoadedTrackVst3Insert(loadedVst3()), "a loaded VST3 is loaded");
    check(!isLoadedTrackVst3Insert(emptySlot()), "an empty slot is not loaded");
    {
        auto insert = loadedVst3();
        insert.enabled = false;
        check(!isLoadedTrackVst3Insert(insert), "a disabled slot is not loaded");
    }
    {
        auto insert = loadedVst3();
        insert.pluginPath.clear();
        check(!isLoadedTrackVst3Insert(insert), "a pathless slot is not loaded");
    }
    {
        auto insert = loadedVst3();
        insert.pluginFormat = "AU";
        check(!isLoadedTrackVst3Insert(insert), "AU is not a VST3 insert");
        insert.pluginFormat = "VST3/AU";
        check(isLoadedTrackVst3Insert(insert), "VST3/AU counts as VST3");
    }

    // --- defaultPluginInsertDspExecutionMode --------------------------------
    ProjectDocument project = defaultProject();
    project.appleSiliconCoreIsolationEnabled = true;

    check(defaultPluginInsertDspExecutionMode(project, true, loadedVst3()) == "internal",
          "with isolation + DSP on, a loaded plug-in defaults to internal");
    check(defaultPluginInsertDspExecutionMode(project, false, loadedVst3()) == "native",
          "with monitor DSP off, default is native");
    check(defaultPluginInsertDspExecutionMode(project, true, emptySlot()) == "native",
          "an empty slot defaults to native");

    project.appleSiliconCoreIsolationEnabled = false;
    check(defaultPluginInsertDspExecutionMode(project, true, loadedVst3()) == "native",
          "with core isolation off, default is native");

    // --- normalizeThirdPartyTrackInsertToNative -----------------------------
    {
        auto insert = loadedVst3("native");
        check(!normalizeThirdPartyTrackInsertToNative(insert), "native needs no normalization");
    }
    {
        // internal is legal for third-party plug-ins and must be left alone.
        auto insert = loadedVst3("internal");
        check(!normalizeThirdPartyTrackInsertToNative(insert), "internal is left alone");
        check(insert.dspExecutionMode == "internal", "internal is unchanged");
    }
    {
        // Server-backed modes need a Neuracoust module; a FabFilter plug-in has none.
        auto insert = loadedVst3("remote_internal");
        insert.assignedDspServerId = "studio-mac-2";
        insert.serverModuleId = "some-module";
        insert.reportedLatencySamples = 128;
        check(normalizeThirdPartyTrackInsertToNative(insert), "remote_internal on a third-party plug-in is reset");
        check(insert.dspExecutionMode == "native", "reset lands on native");
        check(insert.assignedDspServerId.empty(), "server id cleared");
        check(insert.serverModuleId.empty(), "module id cleared");
        check(insert.reportedLatencySamples == 0, "reported latency cleared");
        check(insert.dspAvailable, "dsp marked available again");
    }
    {
        auto insert = loadedVst3("external");
        check(normalizeThirdPartyTrackInsertToNative(insert), "external on a third-party plug-in is reset");
        check(insert.dspExecutionMode == "native", "external resets to native");
    }
    {
        auto insert = emptySlot();
        insert.dspExecutionMode = "external";
        check(!normalizeThirdPartyTrackInsertToNative(insert), "an empty slot is never normalized");
    }

    // --- badges -------------------------------------------------------------
    check(std::string(insertDspModeBadge(loadedVst3("native"))) == "NAT", "native badge");
    check(std::string(insertDspModeBadge(loadedVst3("internal"))) == "INT", "internal badge");
    check(std::string(insertDspModeBadge(loadedVst3("remote_internal"))) == "RINT", "remote_internal badge");
    check(std::string(insertDspModeBadge(loadedVst3("external"))) == "EXT", "external badge");

    project.appleSiliconCoreIsolationEnabled = true;
    check(std::string(effectiveInsertDspModeBadge(loadedVst3("internal"), project)) == "INT",
          "with isolation on, the effective badge is the slot's mode");
    project.appleSiliconCoreIsolationEnabled = false;
    check(std::string(effectiveInsertDspModeBadge(loadedVst3("internal"), project)) == "NAT",
          "with isolation off, everything reads NAT");

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("insert dsp policy OK\n");
    return 0;
}
