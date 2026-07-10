// The listener-facing share URL must point at an address a listener can reach, not
// the loopback the relay ingests on. This runs on whatever network the machine is
// on, so it asserts reachability properties rather than a fixed address.

#include "audio/ListenRoom.h"

#include <cstdio>
#include <string>

namespace {

bool isLoopback(const std::string& host) {
    return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

bool startsWith(const std::string& s, const std::string& prefix) {
    return s.rfind(prefix, 0) == 0;
}

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

} // namespace

int main() {
    using namespace neuracoust::daw;

    // A loopback (default) relayHost means "pick a reachable address for the listener".
    ListenRoomSettings defaulted;
    defaulted.relayHost = "127.0.0.1";
    const std::string host = listenRoomShareHost(defaulted);
    std::printf("share host on this machine: %s\n", host.c_str());

    // If the machine has any network at all this is a routable or link-local IP; if it
    // is truly offline it falls back to loopback. Either way it is never "localhost"
    // or empty, and a private-range hit must not carry a port or path.
    check(!host.empty(), "the share host is never empty");
    check(host != "localhost", "loopback is spelled as an address, not a name");
    check(host.find(':') == std::string::npos && host.find('/') == std::string::npos,
          "the host is a bare address");

    const bool routable = startsWith(host, "192.168.") || startsWith(host, "10.") ||
                          startsWith(host, "172.") || startsWith(host, "169.254.");
    std::printf("routable off this machine: %s\n", routable ? "yes" : "loopback only");

    // An explicitly configured host — a tunnel hostname — is used verbatim.
    ListenRoomSettings tunneled;
    tunneled.relayHost = "listen.example.com";
    check(listenRoomShareHost(tunneled) == "listen.example.com",
          "an explicit host is used as-is");
    check(!isLoopback(listenRoomShareHost(tunneled)), "and is not overridden");

    // The full URL carries that host, plus the session and token.
    ListenRoomSettings full;
    full.relayHost = "127.0.0.1";
    full.sessionName = "mix";
    full.accessToken = "abc123";
    const std::string url = listenRoomPublicShareUrl(full);
    std::printf("share url: %s\n", url.c_str());
    check(url.find("127.0.0.1") == std::string::npos || host == "127.0.0.1",
          "the URL does not hand out loopback when a reachable address exists");
    check(url.find("session=mix") != std::string::npos, "the session rides along");
    check(url.find("token=abc123") != std::string::npos, "and the token");

    std::printf(failures == 0 ? "PASS\n" : "%d checks failed\n", failures);
    return failures == 0 ? 0 : 1;
}
