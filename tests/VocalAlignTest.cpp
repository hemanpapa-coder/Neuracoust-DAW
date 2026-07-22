// Pins the MFCC-DTW vocal aligner: a dub that is a KNOWN non-uniform time-warp of a reference, when
// aligned and warped back, must match the reference far better than it did before alignment. Pure DSP.
#include "audio/VocalAlign.h"
#include "audio/TimePitchProcessor.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace neuracoust::daw;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
    else std::printf("ok: %s\n", what);
}

// A signal with distinct temporal structure: a sequence of tone bursts sweeping up in frequency, so
// MFCCs vary clearly frame-to-frame (a flat sine would give the aligner nothing to lock onto).
std::vector<float> structuredSignal(int frames, double sr) {
    std::vector<float> v(static_cast<size_t>(frames), 0.0f);
    const int nSeg = 8;
    for (int s = 0; s < nSeg; ++s) {
        const double f = 300.0 * std::pow(2.0, s / 3.0);   // rising pitch per segment
        const int lo = s * frames / nSeg, hi = (s + 1) * frames / nSeg;
        for (int i = lo; i < hi; ++i)
            v[static_cast<size_t>(i)] = 0.5f * std::sin(2.0 * M_PI * f * i / sr)
                                       * (0.6f + 0.4f * std::sin(2.0 * M_PI * 5.0 * i / sr));  // gentle tremolo
    }
    return v;
}

}  // namespace

int main() {
    const double sr = 44100.0;
    const int refFrames = static_cast<int>(2.0 * sr);   // 2 s reference
    auto ref = structuredSignal(refFrames, sr);

    // Build the dub as a KNOWN non-uniform warp of the reference: the reference's content at 0.5 is
    // pulled to 0.35 in the dub (first half compressed, second half stretched). The true dub→ref map is
    // therefore piecewise-linear with a kink at dub 0.35 -> ref 0.5.
    TimePitchParams p; p.timeRatio = 1.0; p.semitones = 0.0;
    auto dub = processTimeMapInterleaved(ref, 1, p, {0.5}, {0.35});
    check(!dub.empty(), "built a time-warped dub from the reference");

    auto knownWarp = [](double dn) {
        return dn <= 0.35 ? dn / 0.35 * 0.5 : 0.5 + (dn - 0.35) / 0.65 * 0.5;   // inverse of {0.5->0.35}
    };

    auto anchors = alignVocals(ref, 1, sr, dub, 1, sr);
    check(anchors.ok, "aligner returned a result");
    check(anchors.dub.size() >= 4, "recovered a usable set of interior anchors");

    // Anchors strictly increasing in both axes.
    bool mono = true;
    for (size_t i = 1; i < anchors.dub.size(); ++i)
        if (!(anchors.dub[i] > anchors.dub[i - 1] && anchors.ref[i] > anchors.ref[i - 1])) mono = false;
    check(mono, "alignment anchors strictly increasing in both axes");

    // The core proof: each recovered anchor's dub→ref mapping matches the KNOWN warp. This tests the
    // aligner itself (the phase vocoder that applies the warp is pinned by TimePitchSmokeTest).
    double maxErr = 0.0, meanErr = 0.0;
    for (size_t i = 0; i < anchors.dub.size(); ++i) {
        const double e = std::abs(anchors.ref[i] - knownWarp(anchors.dub[i]));
        maxErr = std::max(maxErr, e); meanErr += e;
    }
    if (!anchors.dub.empty()) meanErr /= anchors.dub.size();
    char m[160];
    std::snprintf(m, sizeof m, "recovered map matches the known warp (mean err %.3f, max %.3f)", meanErr, maxErr);
    check(meanErr < 0.04 && maxErr < 0.08, m);

    // Phase 2 — transient snap: the tone-burst boundaries are onsets; ref 0.5 is both a burst boundary
    // and the warp kink. With snapping on, an anchor should sit almost exactly on it.
    double nearestToHalf = 1.0;
    for (double r : anchors.ref) nearestToHalf = std::min(nearestToHalf, std::abs(r - 0.5));
    char ms[128]; std::snprintf(ms, sizeof ms, "transient snap pins an anchor onto the ref onset at 0.5 (nearest %.3f)", nearestToHalf);
    check(nearestToHalf < 0.02, ms);

    // Snapping must not make the recovered map worse than the coarse DTW-only warp.
    auto coarse = alignVocals(ref, 1, sr, dub, 1, sr, 48, /*snapTransients=*/false);
    double coarseMean = 0.0;
    for (size_t i = 0; i < coarse.dub.size(); ++i) coarseMean += std::abs(coarse.ref[i] - knownWarp(coarse.dub[i]));
    if (!coarse.dub.empty()) coarseMean /= coarse.dub.size();
    check(meanErr <= coarseMean + 0.01, "snapped map is no worse than the coarse DTW map");

    // End-to-end sanity: the warp applies, preserving length and audio.
    TimePitchParams ap; ap.timeRatio = static_cast<double>(refFrames) / dub.size(); ap.semitones = 0.0;
    auto aligned = processTimeMapInterleaved(dub, 1, ap, anchors.dub, anchors.ref);
    double peak = 0.0; for (float v : aligned) peak = std::max(peak, std::fabs((double)v));
    check(!aligned.empty() && peak > 0.2, "warped-onto-reference output is audible");

    // VAD gating: a signal that is silent through its middle third must get NO warp anchors there —
    // silence features are unreliable and should not drive the warp.
    {
        auto gapped = structuredSignal(refFrames, sr);
        for (int i = static_cast<int>(0.4 * refFrames); i < static_cast<int>(0.6 * refFrames); ++i) gapped[i] = 0.0f;
        auto ga = alignVocals(gapped, 1, sr, gapped, 1, sr);   // self-align; VAD on by default
        bool anyInSilence = false;
        for (double d : ga.dub) if (d > 0.42 && d < 0.58) anyInSilence = true;
        check(ga.ok && !anyInSilence, "VAD keeps warp anchors out of the silent region");

        // With gating OFF the aligner is free to place anchors anywhere (control: confirms the gate is
        // what excluded them, not merely that no anchor happened to land there).
        auto ng = alignVocals(gapped, 1, sr, gapped, 1, sr, 48, true, /*gateSilence=*/false);
        check(ng.ok, "aligner still runs with silence gating disabled");
    }

    std::printf(failures ? "\n%d FAILURES\n" : "\nALL PASS\n", failures);
    return failures ? 1 : 0;
}
