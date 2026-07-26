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
float circuitStage(float x, float amount = 1.0f) {
    const float drive = 1.0f + 0.16f * amount;
    const float asymmetric = x * drive + 0.018f * amount * x * std::abs(x);
    return std::tanh(asymmetric) / std::tanh(drive);
}
float saturate(float x, float drive, bool circuit) {
    if (circuit) return circuitStage(x * drive, 1.8f);
    return std::tanh(x * drive) / std::max(0.0001f, std::tanh(drive));
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
         !p.saturatorEnabled)) return;
    if (sampleRate_ != sr) reset(sr);
    // Coefficients are set from the live params here; the Biquad ramps to them per sample.
    for (auto& ch : eq_) {
        ch[0].highPass(sr, p.highPassHz);
        ch[1].lowPass(sr, p.lowPassHz);
        if (p.eqHfBell) ch[2].peak(sr, p.eqHfHz, 0.7f, p.eqHfGainDb);
        else ch[2].shelf(sr, p.eqHfHz, p.eqHfGainDb, true);
        const float hmfQ = p.eqEMode ? p.eqHmfQ : std::max(0.2f, p.eqHmfQ * 0.7f);
        const float lmfQ = p.eqEMode ? p.eqLmfQ : std::max(0.2f, p.eqLmfQ * 0.7f);
        ch[3].peak(sr, p.eqHmfHz, hmfQ, p.eqHmfGainDb);
        ch[4].peak(sr, p.eqLmfHz, lmfQ, p.eqLmfGainDb);
        if (p.eqLfBell) ch[5].peak(sr, p.eqLfHz, 0.7f, p.eqLfGainDb);
        else ch[5].shelf(sr, p.eqLfHz, p.eqLfGainDb, false);
    }
    const float cDet = coeff(sr, 8), gDet = coeff(sr, 5);
    const float cAtk = coeff(sr, p.compFastAttack ? 3.0f : p.compAttackMs);
    const float cRel = coeff(sr, p.compReleaseMs);
    const float gAtk = coeff(sr, p.gateFastAttack ? 0.1f : p.gateAttackMs);
    const float gRel = coeff(sr, p.gateReleaseMs);
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
                if (p.filterCircuitMode) { l=circuitStage(l,0.35f); r=circuitStage(r,0.35f); }
            } else if (module=="eq" && p.eqEnabled) {
                for(size_t b=2;b<6;++b)l=eq_[0][b].process(l);
                for(size_t b=2;b<6;++b)r=eq_[1][b].process(r);
                if (p.eqCircuitMode) { l=circuitStage(l,0.55f); r=circuitStage(r,0.55f); }
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
                    const float over = std::max(0.0f, gainToDb(detectorLevel) - p.compThresholdDb);
                    const float target = -over*(1.0f - 1.0f/clamp(p.compRatio, 1.0f, 20.0f));
                    const float c = target < compGainDb_[ch] ? cAtk : cRel;
                    compGainDb_[ch] = target + c*(compGainDb_[ch] - target);
                    gain[ch] = dbToGain(compGainDb_[ch]);
                }
                l=dryL*(1.0f-mix)+(dryL*gain[0])*mix;
                r=dryR*(1.0f-mix)+(dryR*gain[1])*mix;
                if (p.compCircuitMode) { l=circuitStage(l,0.8f); r=circuitStage(r,0.8f); }
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
                if (p.gateCircuitMode) { l=circuitStage(l,0.4f); r=circuitStage(r,0.4f); }
            } else if (module=="saturator" && p.saturatorEnabled) {
                const float dryL=l, dryR=r;
                const float drive=dbToGain(p.saturatorDriveDb);
                const float mix=clamp(p.saturatorMix,0.0f,1.0f);
                l=dryL*(1.0f-mix)+saturate(dryL,drive,p.saturatorCircuitMode)*mix;
                r=dryR*(1.0f-mix)+saturate(dryR,drive,p.saturatorCircuitMode)*mix;
            }
        }
        audio[i]=l; audio[i+1]=r;
    }
}

} // namespace neuracoust::daw
