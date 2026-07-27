// Spec-sheet-derived (approximate) frequency-response curves for speaker and audio-interface
// catalog models that have NO measured profile. These are NOT measurements — they are a
// physically-motivated approximation from the model's published class (field size → LF extension,
// brand/series voicing → HF balance). They fill the monitor response graph and voice the sim so a
// model without a real measurement still shows and sounds like *something* honest, clearly a level
// below a true measured profile. A measured profile (SpeakerProfiles.generated) always wins.
#include "audio/SpeakerProfiles.h"
#include "audio/AudioInterfaceProfiles.h"
#include "audio/MonitorCorrection.h"   // ResponseCurve, normalizeCurveMidband

#include <cmath>
#include <cstdint>
#include <string>

namespace neuracoust::daw {
namespace {

bool contains(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

// Brightness estimate in roughly [-0.28, +0.55]: >0 = forward/bright (small trans-radio monitors),
// <0 = neutral/dark (large mains). A compact echo of MonitorDspProcessor::speakerToneScore — it
// only needs to differentiate families, not be exhaustive.
float specBrightness(const std::string& m) {
    if (contains(m, "Flat") || contains(m, "Off")) return 0.0f;
    float s = 0.0f;
    if (contains(m, "NS-10") || contains(m, "NS10")) s += 0.24f;
    if (contains(m, "Auratone")) s += 0.42f;
    if (contains(m, "Avantone") || contains(m, "MixCube")) s += 0.37f;
    if (contains(m, "Laptop") || contains(m, "Phone")) s += 0.47f;
    if (contains(m, "TV") || contains(m, "Bluetooth")) s += 0.31f;
    if (contains(m, "Broadcast")) s += 0.16f;
    if (contains(m, "HS5")) s += 0.10f;
    if (contains(m, "HS7")) s += 0.07f;
    if (contains(m, "HS8")) s += 0.04f;
    if (contains(m, "KRK") || contains(m, "Rokit")) s += 0.10f;
    if (contains(m, "JBL")) s += 0.06f;
    if (contains(m, "Mackie")) s += 0.07f;
    if (contains(m, "PreSonus")) s += 0.08f;
    if (contains(m, "Neumann KH")) s -= 0.04f;
    if (contains(m, "Focal")) s -= 0.05f;
    if (contains(m, "Adam") || contains(m, "ADAM")) s -= 0.03f;
    if (contains(m, "Dynaudio")) s -= 0.035f;
    if (contains(m, "Genelec 83") || contains(m, "Genelec 10")) s -= 0.025f;
    if (contains(m, "Barefoot") || contains(m, "Augspurger")) s -= 0.075f;
    if (contains(m, "ATC") || contains(m, "PMC") || contains(m, "Quested")) s -= 0.055f;
    if (contains(m, "Ocean Way") || contains(m, "Meyer") || contains(m, "Tannoy")) s -= 0.06f;
    return std::clamp(s, -0.28f, 0.55f);
}

// Low-frequency −3 dB corner (Hz) from the field class printed in the name.
double specLfCorner(const std::string& m) {
    if (contains(m, "(LF)")) return 30.0;   // large / far-field mains reach deepest
    if (contains(m, "(MF)")) return 40.0;   // mid-field
    if (contains(m, "(NF)")) return 55.0;   // near-field, smaller cabinet
    return 46.0;                            // unlabeled → assume a typical near/mid monitor
}

uint32_t fnv1a(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    return h;
}

} // namespace

// Curated, review-informed curves for specific models whose published measurements we've folded in
// by hand (a tier above the generic spec parametric, below a true in-house measurement). First entry:
// Avantone CLA-10 / CLA-10A (Yamaha NS-10M clone), from the AudioScienceReview Klippel review —
// sealed-box bass rolloff, a strongly forward 1.5–5 kHz, a narrow ~3.5 kHz resonance, and a bright,
// over-emphasised low treble. The ASR source is graph-only, so this is a faithful shape, not a
// point-for-point lab import. Midband-normalized like the measured profiles.
ResponseCurve speakerCuratedCurve(const std::string& catalogName) {
    const std::string m = catalogName;
    const bool cla10 = contains(m, "CLA-10") || contains(m, "CLA10") || contains(m, "CLA 10");
    if (!cla10) return {};
    ResponseCurve c;
    const int points = 96;
    c.reserve(points);
    for (int i = 0; i < points; ++i) {
        const double f = 20.0 * std::pow(1000.0, static_cast<double>(i) / (points - 1));
        double db = 0.0;
        db += -10.0 * std::log10(1.0 + std::pow(85.0 / f, 4.0));                       // sealed 2nd-order bass rolloff (~85 Hz)
        const double lgLo = std::log(f / 250.0);
        db += -1.6 * std::exp(-(lgLo * lgLo) / (2 * 0.5 * 0.5));                        // mild low-mid recession ~250 Hz
        const double lgFwd = std::log(f / 3100.0);
        db += 5.2 * std::exp(-(lgFwd * lgFwd) / (2 * 0.55 * 0.55));                     // broad forward upper-mid (NS-10 character)
        const double lgRes = std::log(f / 3500.0);
        db += 2.6 * std::exp(-(lgRes * lgRes) / (2 * 0.10 * 0.10));                     // narrow ~3.5 kHz resonance
        db += 2.0 / (1.0 + std::pow(5000.0 / f, 3.0));                                  // bright low-treble lift
        db += -10.0 * std::log10(1.0 + std::pow(f / 13000.0, 4.0));                     // top-octave rolloff
        c.push_back({f, db});
    }
    return normalizeCurveMidband(c);
}

ResponseCurve speakerSpecCurve(const std::string& catalogName) {
    if (catalogName.empty() || contains(catalogName, "Flat") || contains(catalogName, "Off")) return {};
    if (auto curated = speakerCuratedCurve(catalogName); !curated.empty()) return curated;
    const double fLf = specLfCorner(catalogName);
    const double b = specBrightness(catalogName);
    ResponseCurve c;
    const int points = 96;
    c.reserve(points);
    for (int i = 0; i < points; ++i) {
        const double f = 20.0 * std::pow(1000.0, static_cast<double>(i) / (points - 1));
        double db = 0.0;
        db += -10.0 * std::log10(1.0 + std::pow(fLf / f, 8.0));        // LF rolloff (~4th order)
        db += -10.0 * std::log10(1.0 + std::pow(f / 19000.0, 6.0));    // HF air rolloff
        const double hfWeight = 1.0 / (1.0 + std::pow(2800.0 / f, 2.2)); // 0 below ~2.8k → 1 above
        db += (b * 4.2) * hfWeight;                                    // brightness → HF balance
        if (b > 0.12) {                                               // forward-mid grade monitors
            const double lg = std::log(f / 2000.0);
            db += (b - 0.12) * 3.0 * std::exp(-(lg * lg) / 0.5);       // gentle presence bump
        }
        c.push_back({f, db});
    }
    return normalizeCurveMidband(c);
}

ResponseCurve audioInterfaceSpecCurve(const std::string& catalogName) {
    if (catalogName.empty()) return {};
    // Modern converters are nearly flat; the honest spec spread between models is only tenths of a dB.
    // Derive a tiny deterministic voicing from the name so real vs modeling interfaces differ subtly
    // (a gentle tilt + HF shelf + a DC-block corner) instead of both reading dead flat.
    const uint32_t h = fnv1a(catalogName);
    const double tiltPerDecade = (static_cast<int>(h % 9u) - 4) / 40.0;   // ±0.1 dB/decade
    const double hfShelf = (static_cast<int>((h >> 4) % 7u) - 3) / 20.0;  // ±0.15 dB above ~3k
    const double lfCorner = 5.0 + static_cast<double>((h >> 8) % 16u);    // 5..20 Hz DC block
    ResponseCurve c;
    const int points = 96;
    c.reserve(points);
    for (int i = 0; i < points; ++i) {
        const double f = 20.0 * std::pow(1000.0, static_cast<double>(i) / (points - 1));
        double db = 0.0;
        db += -10.0 * std::log10(1.0 + std::pow(lfCorner / f, 4.0));   // sub-bass DC-block rolloff
        db += -10.0 * std::log10(1.0 + std::pow(f / 21000.0, 8.0));    // top-octave rolloff toward Nyquist
        db += tiltPerDecade * std::log10(f / 1000.0);
        db += hfShelf / (1.0 + std::pow(3000.0 / f, 2.0));
        c.push_back({f, db});
    }
    return normalizeCurveMidband(c);
}

} // namespace neuracoust::daw
