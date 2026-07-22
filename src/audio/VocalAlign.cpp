#include "audio/VocalAlign.h"

#include "audio/PitchEditor.h"   // detectOnsets — reused for transient snapping

#include <algorithm>
#include <cmath>
#include <complex>
#include <utility>
#include <vector>

namespace neuracoust::daw {
namespace {

// In-place radix-2 FFT (sign -1 = forward), same shape as the other files' local copy so this stands alone.
void fftRadix2(std::vector<std::complex<float>>& a, int sign) {
    const size_t n = a.size();
    if (n < 2) return;
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = sign * 2.0 * M_PI / static_cast<double>(len);
        const std::complex<float> wlen(static_cast<float>(std::cos(ang)), static_cast<float>(std::sin(ang)));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (size_t k = 0; k < len / 2; ++k) {
                const std::complex<float> u = a[i + k];
                const std::complex<float> v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

std::vector<float> downmix(const std::vector<float>& in, int channels) {
    if (channels <= 1) return in;
    const size_t frames = in.size() / static_cast<size_t>(channels);
    std::vector<float> mono(frames, 0.0f);
    for (size_t i = 0; i < frames; ++i) {
        float s = 0.0f;
        for (int c = 0; c < channels; ++c) s += in[i * channels + c];
        mono[i] = s / channels;
    }
    return mono;
}

double hzToMel(double f) { return 2595.0 * std::log10(1.0 + f / 700.0); }
double melToHz(double m) { return 700.0 * (std::pow(10.0, m / 2595.0) - 1.0); }

constexpr int kFFT = 1024;
constexpr int kMelFilters = 26;
constexpr int kCoeffs = 12;   // MFCC 1..12 (c0 dropped → level-independent).

// MFCC frames of one mono signal. hop is chosen by the caller (adaptive to bound frame count). Returns
// a [numFrames][kCoeffs] matrix with per-coefficient mean removed (cepstral mean normalization).
std::vector<std::vector<float>> mfcc(const std::vector<float>& mono, double sr, int hop) {
    const int n = static_cast<int>(mono.size());
    if (n < kFFT) return {};

    // Pre-emphasis (0.97) sharpens the higher formants speech alignment leans on.
    std::vector<float> emph(mono.size());
    emph[0] = mono[0];
    for (size_t i = 1; i < mono.size(); ++i) emph[i] = mono[i] - 0.97f * mono[i - 1];

    // Triangular mel filterbank over the 0..sr/2 band, kMelFilters filters.
    const int bins = kFFT / 2 + 1;
    std::vector<std::vector<float>> filt(kMelFilters, std::vector<float>(bins, 0.0f));
    {
        const double melMax = hzToMel(sr / 2.0);
        std::vector<double> centers(kMelFilters + 2);
        for (int m = 0; m < kMelFilters + 2; ++m) centers[m] = melToHz(melMax * m / (kMelFilters + 1));
        auto hzToBin = [&](double hz) { return hz * (kFFT) / sr; };
        for (int m = 1; m <= kMelFilters; ++m) {
            const double lo = hzToBin(centers[m - 1]), mid = hzToBin(centers[m]), hi = hzToBin(centers[m + 1]);
            for (int b = 0; b < bins; ++b) {
                double w = 0.0;
                if (b >= lo && b <= mid && mid > lo) w = (b - lo) / (mid - lo);
                else if (b > mid && b <= hi && hi > mid) w = (hi - b) / (hi - mid);
                filt[static_cast<size_t>(m - 1)][static_cast<size_t>(b)] = static_cast<float>(w);
            }
        }
    }

    // DCT-II basis (kCoeffs x kMelFilters), coefficients 1..kCoeffs.
    std::vector<std::vector<float>> dct(kCoeffs, std::vector<float>(kMelFilters));
    for (int k = 0; k < kCoeffs; ++k)
        for (int m = 0; m < kMelFilters; ++m)
            dct[static_cast<size_t>(k)][static_cast<size_t>(m)] =
                static_cast<float>(std::cos(M_PI * (k + 1) * (m + 0.5) / kMelFilters));

    std::vector<float> window(kFFT);
    for (int i = 0; i < kFFT; ++i) window[i] = 0.5f * (1.0f - std::cos(2.0f * static_cast<float>(M_PI) * i / (kFFT - 1)));

    std::vector<std::vector<float>> out;
    std::vector<std::complex<float>> spec(kFFT);
    std::vector<float> melE(kMelFilters);
    for (int start = 0; start + kFFT <= n; start += hop) {
        for (int i = 0; i < kFFT; ++i) spec[static_cast<size_t>(i)] = { emph[static_cast<size_t>(start + i)] * window[i], 0.0f };
        fftRadix2(spec, -1);
        for (int m = 0; m < kMelFilters; ++m) {
            double e = 0.0;
            for (int b = 0; b < bins; ++b) {
                const float w = filt[static_cast<size_t>(m)][static_cast<size_t>(b)];
                if (w != 0.0f) { const std::complex<float>& c = spec[static_cast<size_t>(b)]; e += w * (c.real() * c.real() + c.imag() * c.imag()); }
            }
            melE[static_cast<size_t>(m)] = static_cast<float>(std::log(e + 1e-10));
        }
        std::vector<float> coeffs(kCoeffs);
        for (int k = 0; k < kCoeffs; ++k) {
            float s = 0.0f;
            for (int m = 0; m < kMelFilters; ++m) s += dct[static_cast<size_t>(k)][static_cast<size_t>(m)] * melE[static_cast<size_t>(m)];
            coeffs[static_cast<size_t>(k)] = s;
        }
        out.push_back(std::move(coeffs));
    }

    // Cepstral mean normalization — subtract each coefficient's mean over the utterance (robust to
    // channel/mic/level differences between the two takes).
    if (!out.empty()) {
        std::vector<double> mean(kCoeffs, 0.0);
        for (const auto& f : out) for (int k = 0; k < kCoeffs; ++k) mean[static_cast<size_t>(k)] += f[static_cast<size_t>(k)];
        for (int k = 0; k < kCoeffs; ++k) mean[static_cast<size_t>(k)] /= out.size();
        for (auto& f : out) for (int k = 0; k < kCoeffs; ++k) f[static_cast<size_t>(k)] -= static_cast<float>(mean[static_cast<size_t>(k)]);
    }
    return out;
}

float dist2(const std::vector<float>& a, const std::vector<float>& b) {
    float s = 0.0f;
    for (int k = 0; k < kCoeffs; ++k) { const float d = a[static_cast<size_t>(k)] - b[static_cast<size_t>(k)]; s += d * d; }
    return s;
}

// Pick a hop so a signal frames to at most `maxFrames` (bounds the DTW matrix); never below 256.
int chooseHop(int samples, int maxFrames) {
    int hop = 256;
    if (samples / hop > maxFrames) hop = std::max(256, (samples + maxFrames - 1) / maxFrames);
    return hop;
}

}  // namespace

AlignmentAnchors alignVocals(const std::vector<float>& reference, int refChannels, double refRate,
                             const std::vector<float>& dub, int dubChannels, double dubRate,
                             int maxAnchors, bool snapTransients, bool gateSilence) {
    AlignmentAnchors result;
    if (reference.empty() || dub.empty() || refRate <= 0.0 || dubRate <= 0.0) return result;

    const std::vector<float> refMono = downmix(reference, refChannels);
    const std::vector<float> dubMono = downmix(dub, dubChannels);
    constexpr int kMaxFrames = 2500;   // caps DTW at ~2500x2500; longer clips get a coarser hop.
    const auto R = mfcc(refMono, refRate, chooseHop(static_cast<int>(refMono.size()), kMaxFrames));
    const auto D = mfcc(dubMono, dubRate, chooseHop(static_cast<int>(dubMono.size()), kMaxFrames));
    const int N = static_cast<int>(R.size());   // reference frames
    const int M = static_cast<int>(D.size());   // dub frames
    if (N < 2 || M < 2) return result;

    // Sakoe-Chiba band: the warp path stays within `radius` of the diagonal, so DTW cost is O(N*radius)
    // and pathological stretches are forbidden. Radius scales with length + the two clips' length gap.
    const int radius = std::max(32, std::max(std::abs(N - M) + N / 5, M / 5));
    const float kInf = 1e18f;
    std::vector<float> acc(static_cast<size_t>(N) * M, kInf);
    auto at = [&](int i, int j) -> float& { return acc[static_cast<size_t>(i) * M + j]; };

    at(0, 0) = dist2(R[0], D[0]);
    for (int i = 0; i < N; ++i) {
        const int jlo = std::max(0, static_cast<int>(std::llround(static_cast<double>(i) * M / N) - radius));
        const int jhi = std::min(M - 1, static_cast<int>(std::llround(static_cast<double>(i) * M / N) + radius));
        for (int j = jlo; j <= jhi; ++j) {
            if (i == 0 && j == 0) continue;
            const float local = dist2(R[static_cast<size_t>(i)], D[static_cast<size_t>(j)]);
            float best = kInf;
            if (i > 0) best = std::min(best, at(i - 1, j));
            if (j > 0) best = std::min(best, at(i, j - 1));
            if (i > 0 && j > 0) best = std::min(best, at(i - 1, j - 1));
            at(i, j) = (best >= kInf) ? kInf : local + best;
        }
    }
    if (at(N - 1, M - 1) >= kInf) return result;   // no path within the band

    // Backtrack the optimal path (i=ref, j=dub) from the corner to the origin.
    std::vector<std::pair<int, int>> path;
    for (int i = N - 1, j = M - 1; i > 0 || j > 0;) {
        path.emplace_back(i, j);
        float d = (i > 0 && j > 0) ? at(i - 1, j - 1) : kInf;
        float u = (i > 0) ? at(i - 1, j) : kInf;
        float l = (j > 0) ? at(i, j - 1) : kInf;
        if (d <= u && d <= l) { --i; --j; }
        else if (u <= l) { --i; }
        else { --j; }
    }
    path.emplace_back(0, 0);
    std::reverse(path.begin(), path.end());

    // Anchors must keep a minimum gap in BOTH axes so no time-map segment is shorter than the phase
    // vocoder's window (short segments degrade). The gap widens when maxAnchors is small.
    const double minGap = std::max(0.035, 0.9 / (maxAnchors + 1));

    // VAD: a voiced mask over the dub (short-window RMS above a floor relative to the peak), dilated so
    // attack edges count as voiced. Warp anchors are kept out of silence, whose features are unreliable,
    // so breaths/gaps stretch smoothly under their neighbours instead of being warped erratically.
    std::vector<char> dubVoiced;
    if (gateSilence && !dubMono.empty()) {
        const int vhop = 512;
        std::vector<double> env;
        for (size_t i = 0; i + vhop <= dubMono.size(); i += vhop) {
            double s = 0.0; for (int k = 0; k < vhop; ++k) { const float x = dubMono[i + k]; s += x * x; }
            env.push_back(std::sqrt(s / vhop));
        }
        double peak = 0.0; for (double e : env) peak = std::max(peak, e);
        const double thr = std::max(1e-4, peak * 0.06);   // ~ -24 dB below peak, or an absolute floor
        std::vector<char> raw(env.size(), 0);
        for (size_t i = 0; i < env.size(); ++i) raw[i] = env[i] >= thr ? 1 : 0;
        dubVoiced.assign(env.size(), 0);
        for (size_t i = 0; i < raw.size(); ++i)
            for (int d = -2; d <= 2; ++d) { const long j = static_cast<long>(i) + d; if (j >= 0 && j < static_cast<long>(raw.size()) && raw[static_cast<size_t>(j)]) { dubVoiced[i] = 1; break; } }
    }
    auto voicedAtDub = [&](double dn) {
        if (!gateSilence || dubVoiced.empty()) return true;
        const int i = std::clamp(static_cast<int>(dn * dubVoiced.size()), 0, static_cast<int>(dubVoiced.size()) - 1);
        return dubVoiced[static_cast<size_t>(i)] != 0;
    };

    // Insert an anchor into the sorted-by-dub `accepted` list only if it keeps the min gap and strict
    // monotonicity against both neighbors. Earlier inserts win, so onset anchors (added first) take
    // priority over path-fill anchors that fall too close.
    std::vector<std::pair<double, double>> accepted;   // (dubNorm, refNorm), sorted by dubNorm
    auto tryInsert = [&](double dn, double rn) {
        if (dn <= minGap || dn >= 1.0 - minGap || rn <= minGap || rn >= 1.0 - minGap) return;
        if (!voicedAtDub(dn)) return;   // never place a warp control point inside silence
        size_t idx = 0;
        while (idx < accepted.size() && accepted[idx].first < dn) ++idx;
        if (idx > 0) { const auto& L = accepted[idx - 1]; if (dn - L.first < minGap || rn - L.second < minGap) return; }
        if (idx < accepted.size()) { const auto& R = accepted[idx]; if (R.first - dn < minGap || R.second - rn < minGap) return; }
        accepted.insert(accepted.begin() + static_cast<long>(idx), { dn, rn });
    };

    // Phase 2 — transient snap: build the coarse dub→ref map from the DTW path, then for each dub onset
    // find the reference onset nearest its predicted position and pin them together. These perceptually
    // critical points (consonants/attacks) go in first, so they win placement over the path fill.
    if (snapTransients) {
        std::vector<double> refForDub(static_cast<size_t>(M), -1.0);
        for (const auto& pr : path) refForDub[static_cast<size_t>(pr.second)] = static_cast<double>(pr.first) / (N - 1);
        double carry = 0.0;
        for (int j = 0; j < M; ++j) { if (refForDub[static_cast<size_t>(j)] < 0.0) refForDub[static_cast<size_t>(j)] = carry; else carry = refForDub[static_cast<size_t>(j)]; }

        const double refDur = static_cast<double>(refMono.size()) / refRate;
        const double dubDur = static_cast<double>(dubMono.size()) / dubRate;
        if (refDur > 0.2 && dubDur > 0.2) {
            const auto refOn = detectOnsets(refMono, 1, refRate);
            const auto dubOn = detectOnsets(dubMono, 1, dubRate);
            const double tolNorm = 0.06 / refDur;   // match within ~60 ms of the predicted position
            for (double dt : dubOn) {
                const double dn = dt / dubDur;
                const int j = std::clamp(static_cast<int>(std::llround(dn * (M - 1))), 0, M - 1);
                const double pred = refForDub[static_cast<size_t>(j)];
                double best = 1e9, br = -1.0;
                for (double rt : refOn) { const double rn = rt / refDur; const double e = std::abs(rn - pred); if (e < best) { best = e; br = rn; } }
                if (br >= 0.0 && best <= tolNorm) tryInsert(dn, br);
            }
        }
    }

    // Fill the remaining gaps with the coarse DTW path so long stretches between onsets still warp.
    for (size_t k = 1; k + 1 < path.size() && static_cast<int>(accepted.size()) < maxAnchors; ++k)
        tryInsert(static_cast<double>(path[k].second) / (M - 1), static_cast<double>(path[k].first) / (N - 1));

    // Cap by even decimation if onsets alone overflowed (removing anchors only widens gaps).
    if (static_cast<int>(accepted.size()) > maxAnchors) {
        std::vector<std::pair<double, double>> keep;
        for (int a = 0; a < maxAnchors; ++a)
            keep.push_back(accepted[static_cast<size_t>(static_cast<double>(a) * (accepted.size() - 1) / (maxAnchors - 1))]);
        accepted.swap(keep);
    }

    for (const auto& pr : accepted) { result.dub.push_back(pr.first); result.ref.push_back(pr.second); }
    result.ok = true;   // an empty anchor set (pure uniform scale) is still a valid alignment.
    return result;
}

} // namespace neuracoust::daw
