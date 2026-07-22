#pragma once

#include "audio/WavFile.h"
#include "plugins/MonitorDspModules.h"
#include <cmath>
#include <vector>

namespace neuracoust::daw {

struct StereoFrame {
    float left = 0.0f;
    float right = 0.0f;
};

// Monitor-path safety soft-clip. The monitor EQ / speaker-sim is level-matched on the midband, so a
// presence/air boost stays a real boost and a hot mix can momentarily exceed full scale. Rather than
// pre-darkening the tone (the old peak-normalization) or hard-clamping (gritty), this catches only
// the overshoot: transparent below the knee (~-1 dBFS), then a tanh knee that asymptotes below 1.0.
// Memoryless and branch-cheap below the knee, so it is realtime-safe on every monitor sample.
inline float monitorSafetySoftClip(float x) {
    constexpr float knee = 0.891251f;     // -1 dBFS: transparent below here
    constexpr float ceiling = 0.9995f;    // asymptote a hair below full scale, so output is never ≥ 1.0
    const float a = std::fabs(x);
    if (a <= knee) return x;
    const float over = (a - knee) / (1.0f - knee);         // 0..∞ above the knee
    const float shaped = knee + (ceiling - knee) * std::tanh(over);
    return std::copysign(shaped, x);
}

class MonitorDspProcessor {
public:
    void configure(double sampleRate, std::vector<MonitorDspModule> modules);
    StereoFrame process(StereoFrame frame);
    unsigned int reportedLatencySamples() const;

private:
    struct Biquad {
        float b0 = 1.0f;
        float b1 = 0.0f;
        float b2 = 0.0f;
        float a1 = 0.0f;
        float a2 = 0.0f;
        float z1 = 0.0f;
        float z2 = 0.0f;

        float process(float input);
        void reset();
    };

    struct ChannelFilters {
        Biquad speakerHighPass;
        Biquad speakerLowShelf;
        Biquad speakerPresence;
        Biquad speakerAir;
        Biquad headphoneTilt;
        Biquad graphicLowShelf;
        Biquad graphicMid;
        Biquad graphicHighShelf;
        Biquad roomRumble;
        Biquad roomDeskNotch;
    };

    Biquad makeLowPass(float frequencyHz, float q) const;
    Biquad makeHighPass(float frequencyHz, float q) const;
    Biquad makeLowShelf(float frequencyHz, float gainDb, float slope) const;
    Biquad makeHighShelf(float frequencyHz, float gainDb, float slope) const;
    Biquad makePeaking(float frequencyHz, float gainDb, float q) const;
    void configureFilterState();
    StereoFrame applySpeakerSimulation(StereoFrame frame, const MonitorDspModule& module);
    StereoFrame applyHeadphoneSimulation(StereoFrame frame, const MonitorDspModule& module);
    StereoFrame applyGraphicEq(StereoFrame frame);
    StereoFrame applyRoomCorrection(StereoFrame frame);
    StereoFrame applyCrossfeed(StereoFrame frame);

    double sampleRate_ = 48000.0;
    std::vector<MonitorDspModule> modules_;
    ChannelFilters left_;
    ChannelFilters right_;
    Biquad headphoneCrossfeedLeft_;
    Biquad headphoneCrossfeedRight_;
    Biquad crossfeedLeft_;
    Biquad crossfeedRight_;
    bool hasAudibleProcessing_ = false;
};

void applyMonitorDspToInterleavedStereo(WavAudioData& audio, const std::vector<MonitorDspModule>& modules);

} // namespace neuracoust::daw
