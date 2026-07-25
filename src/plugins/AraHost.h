#pragma once

// ARA (Audio Random Access) hosting — the way Melodyne and friends are meant to be used.
//
// An ARA plug-in is not a realtime effect. Instead of streaming audio through it, the host hands it
// the whole audio file up front (that is the "random access") and the plug-in analyses and edits it
// in place, in context. Hosted as a plain insert it falls back to its own transport and wedges the
// DAW — see requiresAraHost() in Vst3SdkAdapter.
//
// This layer is built bottom-up:
//   1. Reach the plug-in's ARAFactory through its VST3 factory  ← implemented
//   2. Create a document controller and describe the session to it
//   3. Bind a plug-in instance to the controller and show its editor
//
// Step 1 is what tells us the plug-in is genuinely usable and what it supports, so it is also the
// honest gate for the UI: no factory, no ARA.

#include "plugins/Vst3HostFoundation.h"

#include <memory>
#include <string>

namespace neuracoust::daw {

/// What a plug-in's ARAFactory reports about itself. Read once, at scan/inspect time.
struct AraFactoryInfo {
    bool available = false;
    /// Stable identifier for the plug-in's ARA implementation, e.g. "com.celemony.ara.melodyne".
    std::string factoryId;
    std::string plugInName;
    std::string manufacturerName;
    std::string versionString;
    /// The archive format this plug-in reads/writes, needed before any of its state can be stored.
    std::string documentArchiveId;
    /// The ARA API generations this plug-in supports. Hosting requires an overlap with ours.
    int lowestSupportedApiGeneration = 0;
    int highestSupportedApiGeneration = 0;
    /// True when the plug-in's supported range overlaps the generation this host implements.
    bool compatibleWithHost = false;
    std::string message;
};

/// Opens the plug-in and asks its ARA main factory to describe itself. Returns `available = false`
/// (with a message) when the plug-in is not ARA, cannot be opened, or the SDK is not compiled in.
/// Loads plug-in code, so this belongs at inspect time — never in a scan over every plug-in and
/// never on the audio thread.
AraFactoryInfo inspectAraFactory(const Vst3PluginDescriptor& descriptor);

/// True when this build has the ARA SDK compiled in at all.
bool araHostingCompiledIn();

/// A live ARA document controller for one plug-in — step 2 of the bring-up.
///
/// Owns the host-side callbacks the plug-in calls back into (audio access and archiving are
/// mandatory) and the plug-in's document controller. Everything here is main-thread only.
///
/// The audio the plug-in analyses comes from a WAV on disk, read on demand through the audio-access
/// callbacks — that is what "random access" means: the plug-in pulls whatever range it wants,
/// whenever it wants, rather than being fed a stream.
class AraDocumentController {
public:
    AraDocumentController();
    ~AraDocumentController();
    AraDocumentController(const AraDocumentController&) = delete;
    AraDocumentController& operator=(const AraDocumentController&) = delete;

    /// Creates the plug-in's document controller. `documentName` is what the plug-in shows for the
    /// session. False (with `message`) if the plug-in is not ARA or refuses.
    bool create(const Vst3PluginDescriptor& descriptor,
                const std::string& documentName,
                std::string& message);

    /// Registers one audio file with the plug-in as an audio source it may analyse, and wraps it in
    /// an audio modification and a playback region covering the whole file. `persistentId` must be
    /// stable across sessions (the clip id serves). False (with `message`) on failure.
    bool addAudioFile(const std::string& wavPath,
                      const std::string& persistentId,
                      const std::string& name,
                      std::string& message);

    /// Binds a fresh VST3 instance of the same plug-in to this document controller, putting it in
    /// ARA mode and claiming the playback-renderer, edit-renderer and editor-view roles.
    ///
    /// ARA is strict about when this may happen: once per instance, BEFORE setActive/setState or
    /// creating the GUI. So the instance is created here and left inactive; whoever renders or shows
    /// its editor takes it from `boundPlaybackRenderer()` afterwards.
    bool bindPlugInInstance(std::string& message);

    /// True once a plug-in instance is bound and reported the roles we asked for.
    bool hasBoundInstance() const;
    /// Which of the three ARA roles the bound instance actually provided — a plug-in may decline.
    bool boundPlaybackRenderer() const;
    bool boundEditorRenderer() const;
    bool boundEditorView() const;

    /// Stores the plug-in's edits into an opaque base64 blob — this is what makes a Melodyne edit
    /// survive a project save. The plug-in decides the contents; the host only carries them.
    /// Must not be called inside an editing session.
    bool storeArchive(std::string& base64Out, std::string& message);

    /// Injects a blob from storeArchive() back into the plug-in. The document graph must already be
    /// rebuilt — same persistent IDs — before this is called, because that is how the plug-in
    /// matches archived state to objects.
    bool restoreArchive(const std::string& base64In, std::string& message);

    /// Renders every playback region through the bound instance's PLAYBACK RENDERER role and writes
    /// the result to `outPath` — the edited audio, as the plug-in hears it.
    ///
    /// Offline, blocking, main-thread. This is deliberately not the realtime path: an ARA renderer
    /// pulls samples through the host's audio-access callbacks, which read a file, so it has no
    /// business on the audio thread. "Commit the edit back to a clip" is what this is for.
    bool renderToWavFile(const std::string& outPath, std::string& message);

    /// Creates the bound plug-in's editor view (its real GUI — Melodyne's blob editor).
    ///
    /// Must follow bindPlugInInstance(): ARA requires the bind before any GUI exists, otherwise the
    /// editor comes up in the plug-in's non-ARA mode. `parentNSView` is the NSView to attach into;
    /// pass null to create the view without attaching (which is enough to prove the plug-in offers
    /// one). Reports the view's preferred size so a host window can be sized to it.
    bool createEditorView(void* parentNSView, int& widthOut, int& heightOut, std::string& message);
    /// True once an editor view exists.
    bool hasEditorView() const;
    /// Detaches and releases the editor view. Safe to call when there is none.
    void destroyEditorView();

    bool isValid() const;
    /// How many audio sources are currently registered.
    size_t audioSourceCount() const;
    /// Tears the document down in the order ARA requires. Called by the destructor.
    void destroy();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace neuracoust::daw
