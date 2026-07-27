// Calls the app's own discovery so a failure can be pinned on the library or on the app's
// environment (macOS Local Network privacy applies to the .app, not to a CLI run from a terminal).
#include "audio/RemoteDspServerClient.h"
#include <cstdio>

int main() {
    auto settings = neuracoust::daw::defaultRemoteDspServerSettings();
    std::printf("probing status port %u ...\n", settings.statusPort);
    const auto found = neuracoust::daw::discoverRemoteDspServers(settings, {}, 1500);
    std::printf("found %zu node(s)\n", found.size());
    for (const auto& node : found) {
        std::printf("  host=%s (cores %d) %s\n",
                    node.node.host.c_str(), 0, "");
    }
    return 0;
}
