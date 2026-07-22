#pragma once

#include <vector>

namespace neuracoust::daw {

// Time-alignment of a "dub" vocal onto a "reference" (lead) vocal, VocAlign-style: MFCC features +
// Dynamic Time Warping find how the dub's timing maps onto the reference's, returned as matched anchor
// positions. Feed these (with a timeRatio) to processTimeMapFormantPreserving to warp the dub onto the
// lead's timing while keeping its timbre. Offline, deterministic, no external deps (pinned by a test).
struct AlignmentAnchors {
    // Matched INTERIOR anchors in [0,1] (endpoints 0 and 1 are implicit). dub[k] is a position in the
    // dub; ref[k] is where that content sits in the reference. Both strictly increasing, same length.
    std::vector<double> dub;
    std::vector<double> ref;
    bool ok = false;   // false = alignment could not be computed (too short / silent), use no anchors.
};

// Align `dub` onto `reference`. Both interleaved; each is downmixed to mono and, if the rates differ,
// compared on its own time base (anchors are normalized, so rate differences wash out). `maxAnchors`
// caps the returned interior anchor count (the DTW path is decimated to it). Returns ok=false when
// either input is too short to frame.
// `snapTransients` refines the DTW warp so consonant/attack onsets in the dub land exactly on the
// matching onsets in the reference (Phase 2) — the perceptually critical points. Set false for the
// coarse DTW-only warp. `gateSilence` keeps warp anchors out of the dub's silent/breath regions, whose
// features are unreliable, so gaps stretch smoothly instead of being warped erratically.
AlignmentAnchors alignVocals(const std::vector<float>& reference, int refChannels, double refRate,
                             const std::vector<float>& dub, int dubChannels, double dubRate,
                             int maxAnchors = 48, bool snapTransients = true, bool gateSilence = true);

} // namespace neuracoust::daw
