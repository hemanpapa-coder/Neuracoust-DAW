#pragma once

#include <cstdint>

namespace neuracoust::daw {

// A high-accuracy native test-signal source (our own — no third-party plug-in, so every control
// works). Fuses Waves EMO-Generator (Sine/White/Pink, quick freq/level buttons, L/L+R/R, Ø),
// Logic's Test Oscillator (sine + sweep) and Pro Tools' Signal Generator (Sine/Square/Saw/Triangle/
// White/Pink), plus our character (precise dBFS/Hz, measurement-linked sweep, click-free on/off).
enum class TestSignalWaveform { Sine, Square, Triangle, Saw, WhiteNoise, PinkNoise, Sweep };
enum class TestSignalChannel { Left, Stereo, Right };

struct TestSignalParams {
    TestSignalWaveform waveform = TestSignalWaveform::Sine;
    double frequencyHz = 1000.0;   // for the tonal waveforms
    double levelDb = -20.0;        // dBFS: peak for oscillators, applied as gain to noise
    TestSignalChannel channel = TestSignalChannel::Stereo;
    bool polarityInvert = false;
    bool enabled = true;
    // Sweep (Sweep waveform only) — start→end over sweepSeconds, then repeats.
    double sweepStartHz = 20.0;
    double sweepEndHz = 20000.0;
    double sweepSeconds = 5.0;
    bool sweepLogarithmic = true;  // log (musical) vs linear frequency travel
};

class TestSignalGenerator {
public:
    // Called from the UI thread; the render reads a stable copy each block. Cheap POD assign.
    void setParams(const TestSignalParams& params) { params_ = params; }
    const TestSignalParams& params() const { return params_; }

    void reset();

    // Overwrites `interleaved` (stereo, `frames` frames) with the generated signal. Band-limited
    // oscillators (PolyBLEP/BLAMP) so square/saw/triangle don't alias. Click-free: a short gain ramp
    // follows enable/disable so toggling never pops. Deterministic noise (seeded), so tests repeat.
    void generateInterleavedStereo(float* interleaved, int frames, double sampleRate);

private:
    float nextWhite();
    float nextPink();

    TestSignalParams params_;
    double phase_ = 0.0;        // 0..1 oscillator phase
    double sweepPos_ = 0.0;     // seconds into the current sweep
    double triInt_ = 0.0;       // running integral of the band-limited square → triangle
    double triDc_ = 0.0;        // DC-blocker state for the integrated triangle
    float pink_[7] = {0, 0, 0, 0, 0, 0, 0};
    uint32_t rng_ = 0x2545F491u;
    float ramp_ = 0.0f;         // click-free gain, chases enabled ? 1 : 0
};

}  // namespace neuracoust::daw
