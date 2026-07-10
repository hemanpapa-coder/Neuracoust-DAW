#pragma once

#include "audio/WavFile.h"
#include "plugins/MonitorDspModules.h"
#include <vector>

namespace neuracoust::daw {

struct StereoFrame {
    float left = 0.0f;
    float right = 0.0f;
};

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
