#include "audio/ConsoleChannelProcessor.h"
#include <algorithm>
#include <cmath>

namespace neuracoust::daw {
namespace {
constexpr float pi = 3.14159265358979323846f;
float clamp(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }
float dbToGain(float db) { return std::pow(10.0f, db / 20.0f); }
float gainToDb(float g) { return 20.0f * std::log10(std::max(g, 0.0000316228f)); }
float coeff(double sr, float ms) {
    return std::exp(-1.0f / static_cast<float>(sr * std::max(0.001f, ms) * 0.001f));
}
// `harmonic` 0..1 biases the coloration: lower = more 3rd-harmonic edge (symmetric),
// higher = more 2nd-harmonic warmth (asymmetric). 0.5 is the neutral console tone.
float circuitStage(float x, float amount = 1.0f, float harmonic = 0.5f) {
    const float drive = 1.0f + 0.16f * amount;
    const float even = 0.036f * amount * harmonic;              // 2nd-harmonic (warm) asymmetry
    const float asymmetric = x * drive + even * x * std::abs(x);
    return std::tanh(asymmetric) / std::tanh(drive);
}
// `harmonic` biases the tone: higher = more 2nd-harmonic (warm, asymmetric), lower = cleaner
// odd-harmonic tanh. Each console model passes a different value so the plate is audible, not cosmetic.
float saturate(float x, float drive, bool circuit, float harmonic = 0.5f) {
    if (circuit) return circuitStage(x * drive, 1.8f, harmonic);
    const float xd = x * drive;
    const float even = 0.06f * harmonic * xd * std::abs(xd);   // 2nd-harmonic warmth
    return std::tanh(xd + even) / std::max(0.0001f, std::tanh(drive));
}

// A console model's DSP "character" — bounded multipliers that voice the shared comp/gate algorithm
// as a named classic. These are created here (there was no per-model DSP before); each is a small,
// realtime-safe modulation, not a separate algorithm.
struct ConsoleModelChar {
    float compAtkMul = 1.0f, compRelMul = 1.0f;
    float compKneeDb = 1.5f;      // soft-knee width (0 = hard)
    float compDrive = 0.8f;       // circuit colour amount for the compressor stage
    float gateAtkMul = 1.0f, gateRelMul = 1.0f;
    float harmonic = 0.5f;        // 0 = edgy/3rd, 1 = warm/2nd
};
bool has(const std::string& s, const char* k) { return s.find(k) != std::string::npos; }
ConsoleModelChar modelChar(const std::string& m) {
    ConsoleModelChar c;
    if (has(m, "4000G") || has(m, "4000 G") || has(m, "G Series") || has(m, "9000")) {
        // SSL G/9K: smoother than the E — a touch slower, softer knee, gentler drive.
        c = {1.15f, 1.35f, 3.0f, 0.62f, 1.2f, 1.3f, 0.55f};
    } else if (has(m, "Neve") || has(m, "33609") || has(m, "88") || has(m, "8078") || has(m, "1073")) {
        // Neve diode-bridge: slow musical attack, long program-dependent release, soft knee, warm.
        c = {1.7f, 1.9f, 5.0f, 0.95f, 1.5f, 1.7f, 0.85f};
    } else if (has(m, "API") || has(m, "2500") || has(m, "Vision")) {
        // API VCA "thrust": fast, punchy, hard-ish knee, edgy 3rd-harmonic.
        c = {0.7f, 0.85f, 0.8f, 0.85f, 0.8f, 0.9f, 0.30f};
    } else if (has(m, "Neuracoust") || has(m, "NC") || has(m, "Clean") || has(m, "Transparent")) {
        // Neuracoust NC: clean/transparent — minimal colour, medium knee.
        c = {1.0f, 1.0f, 2.0f, 0.20f, 1.0f, 1.0f, 0.5f};
    }
    // default (SSL 4000E): punchy VCA — the previous baseline.
    return c;
}

float softKnee(float overDb, float kneeDb) {
    if (kneeDb <= 0.01f) return std::max(0.0f, overDb);
    if (overDb <= -kneeDb * 0.5f) return 0.0f;
    if (overDb >= kneeDb * 0.5f) return overDb;
    const float x = overDb + kneeDb * 0.5f;
    return x * x / (2.0f * kneeDb);
}
// Deterministic per-channel "component tolerance": a bounded value in [-1,1] from the channel seed
// and a per-parameter salt, so every strip differs but the same strip is stable across renders.
float biasVal(int seed, int salt) {
    unsigned h = static_cast<unsigned>(seed) * 2654435761u ^ static_cast<unsigned>(salt) * 40503u;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return static_cast<float>(h & 0xffffu) / 32767.5f - 1.0f;
}
// Nudge a model's character by the channel bias (no-op at depth 0). The salt base keeps comp / gate
// / saturator from all drifting the same way.
void biasModel(ConsoleModelChar& c, int seed, float depth, int salt) {
    if (depth <= 0.0f) return;
    c.harmonic   = clamp(c.harmonic + biasVal(seed, salt + 0) * depth * 0.06f, 0.0f, 1.0f);
    c.compAtkMul *= 1.0f + biasVal(seed, salt + 1) * depth * 0.04f;
    c.compRelMul *= 1.0f + biasVal(seed, salt + 2) * depth * 0.04f;
    c.compDrive  *= 1.0f + biasVal(seed, salt + 3) * depth * 0.05f;
    c.gateAtkMul *= 1.0f + biasVal(seed, salt + 4) * depth * 0.04f;
    c.gateRelMul *= 1.0f + biasVal(seed, salt + 5) * depth * 0.04f;
}
}

// The harmonic spectrum the saturator currently adds. A test sine is pushed through the exact
// saturate() math at the live drive/mix/model/circuit settings and a small DFT reads the level of
// each harmonic relative to the fundamental — so the picture is what is actually heard, not a guess.
void consoleSaturatorHarmonics(const ConsoleChannelState& p, float* out, int count) {
    for (int i = 0; i < count; ++i) out[i] = 0.0f;
    if (out == nullptr || count <= 0 || !p.saturatorEnabled) return;
    ConsoleModelChar mc = modelChar(p.model);
    biasModel(mc, p.channelBiasSeed, p.channelBiasDepth, 20);
    const float drive = dbToGain(p.saturatorDriveDb) * (0.85f + 0.35f * mc.harmonic);
    const float mix = clamp(p.saturatorMix, 0.0f, 1.0f);
    // One full period is exact for a memoryless waveshaper driven by a sine, so a short window
    // is enough — this runs per UI frame, per visible strip.
    constexpr int N = 512;
    const float amp = 0.5f;                       // moderate test level so drive actually bites
    const int H = count + 1;                      // harmonics 2..count+1 (index 1 is fundamental)
    std::vector<float> re(H + 1, 0.0f), im(H + 1, 0.0f);
    for (int s = 0; s < N; ++s) {
        const float ph = 2.0f * pi * static_cast<float>(s) / N;
        const float x = amp * std::sin(ph);
        const float wet = saturate(x, drive, p.saturatorCircuitMode, mc.harmonic);
        const float y = x * (1.0f - mix) + wet * mix;
        for (int h = 1; h <= H; ++h) { re[h] += y * std::cos(h * ph); im[h] += y * std::sin(h * ph); }
    }
    auto mag = [&](int h) { return std::sqrt(re[h] * re[h] + im[h] * im[h]); };
    const float fund = std::max(1e-9f, mag(1));
    for (int h = 2; h <= H; ++h) {
        const float rel = mag(h) / fund;
        const float db = 20.0f * std::log10(std::max(1e-6f, rel));
        out[h - 2] = clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);   // -60..0 dB → 0..1 bar height
    }
}

float ConsoleChannelProcessor::Biquad::process(float x) {
    // Slide the live coefficients toward the target each sample. The time constant is longer
    // than the UI update interval (~16 ms) on purpose, so consecutive knob updates blend into
    // one continuous glide instead of a series of short ramps that read as steps.
    constexpr float r = 0.00045f;   // ~50 ms settle at 48 kHz
    b0 += r * (tb0 - b0); b1 += r * (tb1 - b1); b2 += r * (tb2 - b2);
    a1 += r * (ta1 - a1); a2 += r * (ta2 - a2);
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
}

void ConsoleChannelProcessor::Biquad::set(float nb0, float nb1, float nb2, float na1, float na2) {
    tb0 = nb0; tb1 = nb1; tb2 = nb2; ta1 = na1; ta2 = na2;
    if (!primed) { b0 = nb0; b1 = nb1; b2 = nb2; a1 = na1; a2 = na2; primed = true; }
}

void ConsoleChannelProcessor::Biquad::peak(double sr, float hz, float q, float gainDb) {
    hz = clamp(hz, 20.0f, static_cast<float>(sr * 0.45));
    const float w = 2.0f * pi * hz / static_cast<float>(sr);
    const float a = std::pow(10.0f, gainDb / 40.0f);
    const float alpha = std::sin(w) / (2.0f * std::max(0.05f, q));
    const float a0 = 1.0f + alpha / a;
    const float nb1 = (-2.0f * std::cos(w)) / a0;
    set((1.0f + alpha * a) / a0, nb1, (1.0f - alpha * a) / a0, nb1, (1.0f - alpha / a) / a0);
}

void ConsoleChannelProcessor::Biquad::shelf(double sr, float hz, float gainDb, bool high) {
    hz = clamp(hz, 20.0f, static_cast<float>(sr * 0.45));
    const float w = 2.0f * pi * hz / static_cast<float>(sr);
    const float a = std::pow(10.0f, gainDb / 40.0f), c = std::cos(w), s = std::sin(w);
    const float beta = s * std::sqrt(a) / 0.70710678f;
    float nb0, nb1, nb2, na0, na1, na2;
    if (high) {
        nb0=a*((a+1)+(a-1)*c+beta); nb1=-2*a*((a-1)+(a+1)*c); nb2=a*((a+1)+(a-1)*c-beta);
        na0=(a+1)-(a-1)*c+beta; na1=2*((a-1)-(a+1)*c); na2=(a+1)-(a-1)*c-beta;
    } else {
        nb0=a*((a+1)-(a-1)*c+beta); nb1=2*a*((a-1)-(a+1)*c); nb2=a*((a+1)-(a-1)*c-beta);
        na0=(a+1)+(a-1)*c+beta; na1=-2*((a-1)+(a+1)*c); na2=(a+1)+(a-1)*c-beta;
    }
    const float inv = 1.0f / std::max(0.000001f, na0);
    set(nb0*inv, nb1*inv, nb2*inv, na1*inv, na2*inv);
}

void ConsoleChannelProcessor::Biquad::highPass(double sr, float hz) {
    hz = clamp(hz, 20.0f, static_cast<float>(sr * 0.45));
    const float w=2*pi*hz/static_cast<float>(sr), s=std::sin(w), c=std::cos(w);
    const float a=s/(2*0.70710678f), a0=1+a;
    set((1+c)*0.5f/a0, -(1+c)/a0, (1+c)*0.5f/a0, -2*c/a0, (1-a)/a0);
}

void ConsoleChannelProcessor::Biquad::lowPass(double sr, float hz) {
    hz = clamp(hz, 20.0f, static_cast<float>(sr * 0.45));
    const float w=2*pi*hz/static_cast<float>(sr), s=std::sin(w), c=std::cos(w);
    const float a=s/(2*0.70710678f), a0=1+a;
    set((1-c)*0.5f/a0, (1-c)/a0, (1-c)*0.5f/a0, -2*c/a0, (1-a)/a0);
}

void ConsoleChannelProcessor::reset(double sr) {
    sampleRate_ = sr;
    for (auto& channel : eq_) for (auto& band : channel) band.clear();
    compDetector_.fill(0); gateDetector_.fill(0);
    compGainDb_.fill(0); gateGainDb_.fill(0); gateHold_.fill(0);
}

void ConsoleChannelProcessor::processInterleavedStereo(std::vector<float>& audio,
                                                        const ConsoleChannelState& p,
                                                        double sr) {
    // "4001e" is the legacy project identifier used before the built-in
    // channel model was renamed. Keep it render-compatible on first load.
    if ((p.model != "4000e" && p.model != "4001e") ||
        (!p.filterEnabled && !p.eqEnabled && !p.compEnabled && !p.gateEnabled &&
         !p.saturatorEnabled && !p.phaseInvertL && !p.phaseInvertR)) return;
    if (sampleRate_ != sr) reset(sr);
    // Analog-console channel variation: a tiny per-strip EQ-frequency drift and an output trim,
    // both deterministic from the channel seed (no-op at depth 0). modelChar gets nudged too below.
    const float bd = clamp(p.channelBiasDepth, 0.0f, 1.0f);
    const float freqBias = 1.0f + biasVal(p.channelBiasSeed, 30) * bd * 0.01f;   // ±1% EQ centres
    const float gainTrim = dbToGain(biasVal(p.channelBiasSeed, 31) * bd * 0.2f); // ±0.2 dB trim
    // Coefficients are set from the live params here; the Biquad ramps to them per sample.
    for (auto& ch : eq_) {
        ch[0].highPass(sr, p.highPassHz * freqBias);
        ch[1].lowPass(sr, p.lowPassHz * freqBias);
        if (p.eqHfBell) ch[2].peak(sr, p.eqHfHz * freqBias, 0.7f, p.eqHfGainDb);
        else ch[2].shelf(sr, p.eqHfHz * freqBias, p.eqHfGainDb, true);
        const float hmfQ = p.eqEMode ? p.eqHmfQ : std::max(0.2f, p.eqHmfQ * 0.7f);
        const float lmfQ = p.eqEMode ? p.eqLmfQ : std::max(0.2f, p.eqLmfQ * 0.7f);
        ch[3].peak(sr, p.eqHmfHz * freqBias, hmfQ, p.eqHmfGainDb);
        ch[4].peak(sr, p.eqLmfHz * freqBias, lmfQ, p.eqLmfGainDb);
        if (p.eqLfBell) ch[5].peak(sr, p.eqLfHz * freqBias, 0.7f, p.eqLfGainDb);
        else ch[5].shelf(sr, p.eqLfHz * freqBias, p.eqLfGainDb, false);
    }
    // Per-model DSP character voices the shared comp/gate as a named classic (SSL E/G, Neve, API, …),
    // then the channel bias nudges each so no two strips are bit-identical.
    ConsoleModelChar cc = modelChar(p.compType); biasModel(cc, p.channelBiasSeed, bd, 0);
    ConsoleModelChar gc = modelChar(p.gateType); biasModel(gc, p.channelBiasSeed, bd, 10);
    // The strip's overall console model voices the saturator and the filter/EQ circuit colour
    // (the modules that share the console-model plate, as opposed to comp/gate's own models).
    ConsoleModelChar mc = modelChar(p.model); biasModel(mc, p.channelBiasSeed, bd, 20);
    const float cDet = coeff(sr, 8), gDet = coeff(sr, 5);
    const float cAtk = coeff(sr, (p.compFastAttack ? 3.0f : p.compAttackMs) * cc.compAtkMul);
    const float cRel = coeff(sr, p.compReleaseMs * cc.compRelMul);
    const float gAtk = coeff(sr, (p.gateFastAttack ? 0.1f : p.gateAttackMs) * gc.gateAtkMul);
    const float gRel = coeff(sr, p.gateReleaseMs * gc.gateRelMul);
    const int holdSamples = static_cast<int>(sr * std::max(0.0f, p.gateHoldMs) * 0.001);
    std::vector<std::string> order;
    size_t start=0;
    while(start<=p.moduleOrder.size()) {
        const auto end=p.moduleOrder.find(',',start);
        order.push_back(p.moduleOrder.substr(start,end==std::string::npos?std::string::npos:end-start));
        if(end==std::string::npos) break; start=end+1;
    }
    for (size_t i = 0; i + 1 < audio.size(); i += 2) {
        float l = audio[i], r = audio[i + 1];
        for (const auto& module : order) {
            if (module=="filter" && p.filterEnabled) {
                if (p.highPassEnabled) {
                    l=eq_[0][0].process(l); r=eq_[1][0].process(r);
                }
                if (p.lowPassEnabled) {
                    l=eq_[0][1].process(l); r=eq_[1][1].process(r);
                }
                if (p.filterCircuitMode) { l=circuitStage(l,0.35f,mc.harmonic); r=circuitStage(r,0.35f,mc.harmonic); }
            } else if (module=="eq" && p.eqEnabled) {
                for(size_t b=2;b<6;++b)l=eq_[0][b].process(l);
                for(size_t b=2;b<6;++b)r=eq_[1][b].process(r);
                if (p.eqCircuitMode) { l=circuitStage(l,0.55f,mc.harmonic); r=circuitStage(r,0.55f,mc.harmonic); }
            } else if (module=="comp" && p.compEnabled) {
                const float dryL = l, dryR = r;
                const float mix=clamp(p.compMix,0.0f,1.0f);
                const float linked = std::max(std::abs(l), std::abs(r));
                const std::array<float, 2> detectorInput {
                    p.dualMono ? std::abs(l) : linked,
                    p.dualMono ? std::abs(r) : linked
                };
                std::array<float, 2> gain {};
                for (size_t ch = 0; ch < 2; ++ch) {
                    const float d = detectorInput[ch];
                    if (p.compPeakMode) compDetector_[ch] = d;
                    else compDetector_[ch] = d*d + cDet*(compDetector_[ch] - d*d);
                    const float detectorLevel = p.compPeakMode
                        ? compDetector_[ch] : std::sqrt(std::max(0.0f, compDetector_[ch]));
                    const float over = softKnee(gainToDb(detectorLevel) - p.compThresholdDb, cc.compKneeDb);
                    const float target = -over*(1.0f - 1.0f/clamp(p.compRatio, 1.0f, 20.0f));
                    const float c = target < compGainDb_[ch] ? cAtk : cRel;
                    compGainDb_[ch] = target + c*(compGainDb_[ch] - target);
                    gain[ch] = dbToGain(compGainDb_[ch]);
                }
                l=dryL*(1.0f-mix)+(dryL*gain[0])*mix;
                r=dryR*(1.0f-mix)+(dryR*gain[1])*mix;
                if (p.compCircuitMode) { l=circuitStage(l, cc.compDrive, cc.harmonic); r=circuitStage(r, cc.compDrive, cc.harmonic); }
            } else if (module=="gate" && p.gateEnabled) {
                const float linked = std::max(std::abs(l), std::abs(r));
                const std::array<float, 2> detectorInput {
                    p.dualMono ? std::abs(l) : linked,
                    p.dualMono ? std::abs(r) : linked
                };
                std::array<float, 2> gain {};
                for (size_t ch = 0; ch < 2; ++ch) {
                    const float d = detectorInput[ch];
                    gateDetector_[ch] = d*d + gDet*(gateDetector_[ch] - d*d);
                    const float inDb = gainToDb(std::sqrt(std::max(0.0f, gateDetector_[ch])));
                    if (inDb >= p.gateThresholdDb) gateHold_[ch] = holdSamples;
                    else if (gateHold_[ch] > 0) --gateHold_[ch];
                    const float below = std::max(0.0f, p.gateThresholdDb - inDb);
                    float shape = clamp(below/24.0f, 0.0f, 1.0f);
                    if (!p.expanderMode) shape = below > 4.0f ? std::pow(shape, 0.35f) : 0.0f;
                    const float target = gateHold_[ch] > 0
                        ? 0.0f : -clamp(p.gateRangeDb, 0.0f, 40.0f)*shape;
                    const float c = target > gateGainDb_[ch] ? gAtk : gRel;
                    gateGainDb_[ch] = target + c*(gateGainDb_[ch] - target);
                    gain[ch] = dbToGain(gateGainDb_[ch]);
                }
                l*=gain[0]; r*=gain[1];
                if (p.gateCircuitMode) { l=circuitStage(l, 0.4f, gc.harmonic); r=circuitStage(r, 0.4f, gc.harmonic); }
            } else if (module=="saturator" && p.saturatorEnabled) {
                const float dryL=l, dryR=r;
                // Warm models (high harmonic) reach saturation a touch sooner; clean models later —
                // a bounded drive trim so the model choice is clearly audible, not just a nameplate.
                const float drive=dbToGain(p.saturatorDriveDb) * (0.85f + 0.35f*mc.harmonic);
                const float mix=clamp(p.saturatorMix,0.0f,1.0f);
                l=dryL*(1.0f-mix)+saturate(dryL,drive,p.saturatorCircuitMode,mc.harmonic)*mix;
                r=dryR*(1.0f-mix)+saturate(dryR,drive,p.saturatorCircuitMode,mc.harmonic)*mix;
            }
        }
        if (p.phaseInvertL) l = -l;               // channel polarity (Ø), per side, end of chain
        if (p.phaseInvertR) r = -r;
        l *= gainTrim; r *= gainTrim;             // per-channel output trim (analog bias, ±0.2 dB)
        audio[i]=l; audio[i+1]=r;
    }
}

} // namespace neuracoust::daw
