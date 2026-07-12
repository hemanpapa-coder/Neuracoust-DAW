// Pan law (#60): mono tracks take a constant-power law, stereo keep balance; new projects
// default to constant-power, projects saved before the field load as legacy, and the
// setting round-trips through save/load.
#include "audio/MixerProcessorChain.h"
#include "project/ProjectDocument.h"
#include <cmath>
#include <cstdio>
#include <string>

using namespace neuracoust::daw;

static int failures = 0;
static void check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

// Left/right gain a centred (pan=0) unity frame gets under a law for a mono/stereo track.
static std::pair<float, float> centreGains(const std::string& law, bool mono) {
    const MixerStereoFrame in{1.0f, 1.0f};
    const auto out = applyMixerGainPan(in, 0.0f, 0.0f, law, mono);
    return {out.left, out.right};
}

int main() {
    // New projects default to constant-power.
    check(defaultProject().panLaw == "-4.5dB", "new project defaults to -4.5dB");

    // Mono track, centre: legacy = unity, constant-power laws pull the centre down.
    check(std::abs(centreGains("legacy", true).first - 1.0f) < 1e-4, "legacy mono centre = 1.0");
    check(std::abs(centreGains("-3dB", true).first - 0.70711f) < 1e-3, "-3dB mono centre = 0.707");
    check(std::abs(centreGains("-4.5dB", true).first - 0.59460f) < 1e-3, "-4.5dB mono centre = 0.595");
    check(std::abs(centreGains("-6dB", true).first - 0.5f) < 1e-3, "-6dB mono centre = 0.5");

    // Stereo track keeps the linear balance (centre unity) regardless of the law.
    check(std::abs(centreGains("-3dB", false).first - 1.0f) < 1e-4, "stereo centre stays 1.0 under -3dB");
    check(std::abs(centreGains("-4.5dB", false).first - 1.0f) < 1e-4, "stereo centre stays 1.0 under -4.5dB");

    // Constant-power keeps total power even: centre L^2+R^2 == hard-panned.
    {
        const auto c = centreGains("-3dB", true);
        const MixerStereoFrame hard = applyMixerGainPan({1.0f, 1.0f}, 0.0f, -1.0f, "-3dB", true);
        const float centrePow = c.first * c.first + c.second * c.second;
        const float hardPow = hard.left * hard.left + hard.right * hard.right;
        check(std::abs(centrePow - hardPow) < 1e-3, "-3dB is constant power across the sweep");
    }

    // Round-trip: an explicit law survives save/load.
    {
        auto p = defaultProject();
        p.panLaw = "-3dB";
        std::string err;
        ProjectDocument reloaded;
        check(deserializeProject(serializeProject(p), reloaded, err), "serialize/deserialize ok");
        check(reloaded.panLaw == "-3dB", "pan law round-trips");
    }

    // An old project (no panLaw field) loads as legacy — existing balance preserved.
    // Take a real serialized project and strip the panLaw line to simulate a pre-field save.
    {
        std::string text = serializeProject(defaultProject());
        const size_t pos = text.find("\"panLaw\"");
        check(pos != std::string::npos, "serialized project has panLaw");
        if (pos != std::string::npos) {
            const size_t lineStart = text.rfind('\n', pos) + 1;
            const size_t lineEnd = text.find('\n', pos);
            text.erase(lineStart, lineEnd - lineStart + 1);
        }
        std::string err;
        ProjectDocument old;
        check(deserializeProject(text, old, err), "parse project with panLaw removed ok");
        check(old.panLaw == "legacy", "project without panLaw loads as legacy");
    }

    printf(failures == 0 ? "ALL PASS\n" : "%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
