#include "project/AudioImportAnalysis.h"

#include "project/EditOperations.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace neuracoust::daw {
namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;

float monoSampleAtFrame(const WavAudioData& data, int64_t frame) {
    if (data.channels <= 0 || frame < 0 || frame >= data.frameCount()) {
        return 0.0f;
    }
    double sum = 0.0;
    const int64_t base = frame * data.channels;
    for (int channel = 0; channel < data.channels; ++channel) {
        sum += data.interleavedSamples[static_cast<size_t>(base + channel)];
    }
    return static_cast<float>(sum / static_cast<double>(data.channels));
}

double estimateTempoBpmFromAudio(const WavAudioData& data) {
    if (data.sampleRate <= 0 || data.frameCount() < data.sampleRate * 2) {
        return 0.0;
    }
    const int hop = std::max(256, data.sampleRate / 100);
    const int window = hop * 2;
    const int64_t maxFrames = std::min<int64_t>(data.frameCount(), static_cast<int64_t>(data.sampleRate) * 180);
    std::vector<double> energy;
    for (int64_t frame = 0; frame + window < maxFrames; frame += hop) {
        double sum = 0.0;
        for (int offset = 0; offset < window; offset += 4) {
            const double sample = monoSampleAtFrame(data, frame + offset);
            sum += sample * sample;
        }
        energy.push_back(std::sqrt(sum / std::max(1, window / 4)));
    }
    if (energy.size() < 64) {
        return 0.0;
    }
    std::vector<double> onset(energy.size(), 0.0);
    double onsetMean = 0.0;
    for (size_t index = 1; index < energy.size(); ++index) {
        onset[index] = std::max(0.0, energy[index] - energy[index - 1]);
        onsetMean += onset[index];
    }
    onsetMean /= static_cast<double>(std::max<size_t>(1, onset.size() - 1));
    for (double& value : onset) {
        value = std::max(0.0, value - onsetMean * 0.35);
    }
    const double hopSeconds = static_cast<double>(hop) / static_cast<double>(data.sampleRate);
    const int minLag = std::max(1, static_cast<int>(std::floor((60.0 / 190.0) / hopSeconds)));
    const int maxLag = std::min<int>(static_cast<int>(onset.size() / 2), static_cast<int>(std::ceil((60.0 / 55.0) / hopSeconds)));
    double bestScore = 0.0;
    int bestLag = 0;
    for (int lag = minLag; lag <= maxLag; ++lag) {
        double score = 0.0;
        for (size_t index = static_cast<size_t>(lag); index < onset.size(); ++index) {
            score += onset[index] * onset[index - static_cast<size_t>(lag)];
        }
        if (score > bestScore) {
            bestScore = score;
            bestLag = lag;
        }
    }
    if (bestLag <= 0 || bestScore <= 0.0) {
        return 0.0;
    }
    double bpm = 60.0 / (static_cast<double>(bestLag) * hopSeconds);
    while (bpm < 70.0) {
        bpm *= 2.0;
    }
    while (bpm > 180.0) {
        bpm *= 0.5;
    }
    return std::isfinite(bpm) && bpm >= 55.0 && bpm <= 190.0 ? bpm : 0.0;
}

double transientEnergyNearSecond(const WavAudioData& data, double seconds, double radiusSeconds) {
    if (data.sampleRate <= 0 || data.frameCount() <= 0 || !std::isfinite(seconds)) {
        return 0.0;
    }
    const int64_t center = static_cast<int64_t>(std::round(seconds * data.sampleRate));
    const int64_t radius = std::max<int64_t>(16, static_cast<int64_t>(std::round(radiusSeconds * data.sampleRate)));
    const int64_t start = std::max<int64_t>(1, center - radius);
    const int64_t end = std::min<int64_t>(data.frameCount(), center + radius);
    double score = 0.0;
    for (int64_t frame = start; frame < end; frame += 8) {
        const double current = std::abs(monoSampleAtFrame(data, frame));
        const double previous = std::abs(monoSampleAtFrame(data, frame - 1));
        score += std::max(0.0, current - previous);
    }
    return score;
}

struct MeterGrooveEstimate {
    int numerator = 4;
    int denominator = 4;
    std::string groove = "straight";
    double swing = 0.0;
    double compoundConfidence = 0.0;
};

struct CompoundMeterCandidate {
    int numerator = 6;
    double score = 0.0;
    double downbeatHierarchy = 1.0;
};

CompoundMeterCandidate compoundMeterCandidateForEighths(const WavAudioData& data,
                                                        double eighthSeconds,
                                                        double durationSeconds,
                                                        int numerator) {
    CompoundMeterCandidate candidate;
    candidate.numerator = numerator;
    if (data.sampleRate <= 0 || numerator < 6 || numerator > 12 || numerator % 3 != 0) {
        return candidate;
    }
    const int maxEighths = static_cast<int>(std::min(768.0, std::floor(durationSeconds / std::max(0.05, eighthSeconds))));
    if (maxEighths < numerator * 2) {
        return candidate;
    }
    std::vector<double> accents(static_cast<size_t>(numerator), 0.0);
    double total = 0.0;
    for (int eighth = 0; eighth < maxEighths; ++eighth) {
        const double energy = transientEnergyNearSecond(data, static_cast<double>(eighth) * eighthSeconds, eighthSeconds * 0.12);
        accents[static_cast<size_t>(eighth % numerator)] += energy;
        total += energy;
    }
    if (total <= 1.0e-6) {
        return candidate;
    }
    double pulseEnergy = 0.0;
    double secondaryPulseEnergy = 0.0;
    int pulseCount = 0;
    for (int position = 0; position < numerator; position += 3) {
        pulseEnergy += accents[static_cast<size_t>(position)];
        if (position > 0) {
            secondaryPulseEnergy += accents[static_cast<size_t>(position)];
        }
        ++pulseCount;
    }
    const double nonPulseAverage = (total - pulseEnergy) / static_cast<double>(std::max(1, numerator - pulseCount));
    const double downbeat = accents.front();
    const double secondaryPulseAverage = pulseCount > 1 ? secondaryPulseEnergy / static_cast<double>(pulseCount - 1) : 0.0;
    const double pulseShare = pulseEnergy / total;
    candidate.downbeatHierarchy = downbeat / std::max(1.0e-6, secondaryPulseAverage);
    const bool compoundPulse =
        downbeat > nonPulseAverage * 1.30 &&
        secondaryPulseAverage > nonPulseAverage * 1.08 &&
        pulseShare > 0.32;
    if (!compoundPulse) {
        return candidate;
    }

    double repeatedHalfPenalty = 1.0;
    if (numerator == 12) {
        double firstHalf = 0.0;
        double secondHalf = 0.0;
        for (int index = 0; index < 6; ++index) {
            firstHalf += accents[static_cast<size_t>(index)];
            secondHalf += accents[static_cast<size_t>(index + 6)];
        }
        const double halfSimilarity = std::min(firstHalf, secondHalf) / std::max(1.0e-6, std::max(firstHalf, secondHalf));
        const double midBarAccentRatio = accents[6] / std::max(1.0e-6, downbeat);
        if (halfSimilarity > 0.82 && midBarAccentRatio > 0.90) {
            repeatedHalfPenalty = 0.40;
        }
    }

    candidate.score = repeatedHalfPenalty * (
        (downbeat / std::max(1.0e-6, nonPulseAverage)) * 0.35 +
        (secondaryPulseAverage / std::max(1.0e-6, nonPulseAverage)) * 0.35 +
        pulseShare * 1.30 +
        static_cast<double>(pulseCount) * 0.08);
    return candidate;
}

MeterGrooveEstimate estimateMeterAndGroove(const WavAudioData& data, double bpm, double durationSeconds) {
    MeterGrooveEstimate estimate;
    if (!(std::isfinite(bpm) && bpm > 0.0) || data.sampleRate <= 0 || durationSeconds < 3.0) {
        return estimate;
    }
    const double beatSeconds = 60.0 / bpm;
    const int maxBeats = static_cast<int>(std::min(256.0, std::floor(durationSeconds / std::max(0.1, beatSeconds))));
    if (maxBeats < 8) {
        return estimate;
    }
    std::array<double, 6> beatAccent {};
    double totalBeatEnergy = 0.0;
    for (int beat = 0; beat < maxBeats; ++beat) {
        const double energy = transientEnergyNearSecond(data, static_cast<double>(beat) * beatSeconds, beatSeconds * 0.10);
        beatAccent[static_cast<size_t>(beat % 6)] += energy;
        totalBeatEnergy += energy;
    }
    if (totalBeatEnergy > 1.0e-6) {
        const double tripleAccent = beatAccent[0] + beatAccent[3] * 0.75;
        const double fourAccent = beatAccent[0] + beatAccent[4] * 0.35;
        const double sixEightLift = beatAccent[0] + beatAccent[3];
        if (sixEightLift > fourAccent * 1.30 && maxBeats >= 12) {
            estimate.numerator = 6;
            estimate.denominator = 8;
            estimate.compoundConfidence = std::max(estimate.compoundConfidence, sixEightLift / std::max(1.0e-6, fourAccent));
        } else if (tripleAccent > fourAccent * 1.16 && maxBeats >= 9) {
            estimate.numerator = 3;
            estimate.denominator = 4;
        }
    }

    const double eighthSeconds = beatSeconds * 0.5;
    std::array<CompoundMeterCandidate, 3> compoundCandidates {
        compoundMeterCandidateForEighths(data, eighthSeconds, durationSeconds, 6),
        compoundMeterCandidateForEighths(data, eighthSeconds, durationSeconds, 9),
        compoundMeterCandidateForEighths(data, eighthSeconds, durationSeconds, 12)
    };
    const auto bestCompound = std::max_element(compoundCandidates.begin(), compoundCandidates.end(), [](const auto& left, const auto& right) {
        return left.score < right.score;
    });
    if (bestCompound != compoundCandidates.end() && bestCompound->score > 1.65) {
        auto selectedCompound = *bestCompound;
        for (const auto& candidate : compoundCandidates) {
            if (candidate.score <= 1.65) {
                continue;
            }
            const bool longerCompoundNeedsClearDownbeat = candidate.numerator > 6 && candidate.downbeatHierarchy < 1.24;
            if (longerCompoundNeedsClearDownbeat) {
                continue;
            }
            const bool clearLongerCompound = candidate.numerator > selectedCompound.numerator &&
                candidate.downbeatHierarchy >= 1.24 &&
                candidate.score > selectedCompound.score * 0.82;
            if (candidate.score > selectedCompound.score * 1.08 ||
                clearLongerCompound ||
                (candidate.numerator < selectedCompound.numerator && candidate.score > selectedCompound.score * 0.92)) {
                selectedCompound = candidate;
            }
        }
        const bool currentIsTripleSimple = estimate.numerator == 3 && estimate.denominator == 4;
        if (!currentIsTripleSimple || selectedCompound.score > 1.95) {
            estimate.numerator = selectedCompound.numerator;
            estimate.denominator = 8;
            estimate.compoundConfidence = std::max(estimate.compoundConfidence, selectedCompound.score);
        }
    }

    double straightEnergy = 0.0;
    double tripletEnergy = 0.0;
    double lateTripletEnergy = 0.0;
    int grooveSamples = 0;
    for (int beat = 0; beat + 1 < maxBeats; ++beat) {
        const double beatStart = static_cast<double>(beat) * beatSeconds;
        straightEnergy += transientEnergyNearSecond(data, beatStart + beatSeconds * 0.50, beatSeconds * 0.08);
        tripletEnergy += transientEnergyNearSecond(data, beatStart + beatSeconds / 3.0, beatSeconds * 0.07);
        lateTripletEnergy += transientEnergyNearSecond(data, beatStart + beatSeconds * 2.0 / 3.0, beatSeconds * 0.07);
        ++grooveSamples;
    }
    if (grooveSamples > 0) {
        const double straightAverage = straightEnergy / static_cast<double>(grooveSamples);
        const double tripletAverage = tripletEnergy / static_cast<double>(grooveSamples);
        const double lateTripletAverage = lateTripletEnergy / static_cast<double>(grooveSamples);
        if (lateTripletAverage > straightAverage * 1.22 && lateTripletAverage > tripletAverage * 1.08) {
            estimate.groove = "shuffle";
            estimate.swing = std::max(0.50, std::min(0.72, 0.50 + (lateTripletAverage / std::max(1.0e-6, straightAverage + lateTripletAverage)) * 0.28));
        } else if ((tripletAverage + lateTripletAverage) > straightAverage * 1.35) {
            estimate.groove = "triplet";
            estimate.swing = 1.0 / 3.0;
        }
    }
    return estimate;
}

std::array<double, 12> chromaForWindow(const WavAudioData& data, double startSeconds, double durationSeconds) {
    std::array<double, 12> chroma {};
    if (data.sampleRate <= 0 || durationSeconds <= 0.05 || data.frameCount() <= 0) {
        return chroma;
    }
    const int64_t startFrame = std::max<int64_t>(0, static_cast<int64_t>(std::floor(startSeconds * data.sampleRate)));
    const int64_t endFrame = std::min<int64_t>(data.frameCount(), static_cast<int64_t>(std::ceil((startSeconds + durationSeconds) * data.sampleRate)));
    if (endFrame <= startFrame + 128) {
        return chroma;
    }
    const int64_t frameSpan = endFrame - startFrame;
    const int step = static_cast<int>(std::max<int64_t>(1, frameSpan / 6144));
    for (int midi = 36; midi <= 83; ++midi) {
        const double frequency = 440.0 * std::pow(2.0, (static_cast<double>(midi) - 69.0) / 12.0);
        const double phaseStep = 2.0 * kPi * frequency * static_cast<double>(step) / static_cast<double>(data.sampleRate);
        double phase = 0.0;
        double real = 0.0;
        double imag = 0.0;
        for (int64_t frame = startFrame; frame < endFrame; frame += step) {
            const double sample = monoSampleAtFrame(data, frame);
            real += sample * std::cos(phase);
            imag -= sample * std::sin(phase);
            phase += phaseStep;
            if (phase > kPi * 2.0) {
                phase = std::fmod(phase, kPi * 2.0);
            }
        }
        // Log-compress each pitch's magnitude before folding it into its pitch class. Raw magnitude
        // lets one loud bass fundamental outweigh a whole chord voicing, which skewed the key toward
        // whatever the bass sat on; compression makes presence matter more than level.
        const double magnitude = std::sqrt(real * real + imag * imag);
        chroma[static_cast<size_t>(midi % 12)] += std::log1p(magnitude);
    }
    return chroma;
}

/// Chroma for key finding: many short windows, each normalised, then averaged.
///
/// A single window over the whole song (what this used to do) lets the loudest section decide the
/// key — a big chorus outvotes everything else, and a modulation smears into an average that
/// matches nothing. Normalising each window first gives every moment of the song one equal vote.
std::array<double, 12> averagedChromaForKey(const WavAudioData& data, double durationSeconds) {
    std::array<double, 12> total {};
    if (durationSeconds <= 0.0) {
        return total;
    }
    constexpr double kWindowSeconds = 2.0;
    int windows = 0;
    for (double start = 0.0; start + 0.5 < durationSeconds; start += kWindowSeconds) {
        const double span = std::min(kWindowSeconds, durationSeconds - start);
        if (span < 0.5) {
            break;
        }
        auto window = chromaForWindow(data, start, span);
        double sum = 0.0;
        for (const double value : window) {
            sum += value;
        }
        if (sum <= 1.0e-9) {
            continue;   // silence contributes nothing rather than a flat vote
        }
        for (size_t pitchClass = 0; pitchClass < window.size(); ++pitchClass) {
            total[pitchClass] += window[pitchClass] / sum;
        }
        ++windows;
    }
    if (windows > 0) {
        for (double& value : total) {
            value /= static_cast<double>(windows);
        }
    }
    return total;
}

struct KeyEstimate {
    int root = 0;
    bool minor = false;
    double confidence = 0.0;
    double tonalShare = 0.0;
};

KeyEstimate estimateKeyFromChroma(const std::array<double, 12>& chroma) {
    // Krumhansl–Schmuckler key finding: correlate the 12-bin chroma against all 24 major/minor key
    // profiles (Krumhansl–Kessler tonal-hierarchy weights) and take the strongest. Pearson correlation
    // makes the match robust to the chroma's overall level and DC offset — this is the standard method
    // and is markedly more reliable than a flat diatonic-scale sum, which over-rewards busy chromas.
    // Temperley's Kostka–Payne profiles rather than the original Krumhansl–Kessler weights. K–K came
    // from probe-tone listening tests; these are derived from what actually occurs in scored music,
    // and are the better-performing pair in published key-finding comparisons — notably less prone
    // to confusing a key with its relative minor/major.
    static constexpr std::array<double, 12> kMajorProfile {
        5.00, 2.00, 3.50, 2.00, 4.50, 4.00, 2.00, 4.50, 2.00, 3.50, 1.50, 4.00};
    static constexpr std::array<double, 12> kMinorProfile {
        5.00, 2.00, 3.50, 4.50, 2.00, 4.00, 2.00, 4.50, 3.50, 2.00, 1.50, 4.00};
    double total = 0.0;
    for (double value : chroma) {
        total += value;
    }
    KeyEstimate estimate;
    if (total <= 1.0e-6) {
        return estimate;
    }
    auto sortedChroma = chroma;
    std::sort(sortedChroma.begin(), sortedChroma.end(), std::greater<double>());
    estimate.tonalShare = (sortedChroma[0] + sortedChroma[1] + sortedChroma[2] + sortedChroma[3]) / total;

    const double chromaMean = total / 12.0;
    const auto profileMean = [](const std::array<double, 12>& p) {
        double s = 0.0; for (double v : p) s += v; return s / 12.0;
    };
    const double majMean = profileMean(kMajorProfile);
    const double minMean = profileMean(kMinorProfile);
    // Pearson correlation of the chroma against `profile` rotated so its tonic (index 0) sits at `root`.
    const auto correlate = [&](const std::array<double, 12>& profile, double pMean, int root) {
        double num = 0.0, denC = 0.0, denP = 0.0;
        for (int i = 0; i < 12; ++i) {
            const double c = chroma[static_cast<size_t>(i)] - chromaMean;
            const double p = profile[static_cast<size_t>((i - root + 12) % 12)] - pMean;
            num += c * p; denC += c * c; denP += p * p;
        }
        const double den = std::sqrt(denC * denP);
        return den > 1.0e-12 ? num / den : 0.0;
    };

    double bestScore = -2.0;
    double secondScore = -2.0;
    for (int root = 0; root < 12; ++root) {
        const auto consider = [&](double score, bool isMinor) {
            if (score > bestScore) {
                secondScore = bestScore;
                bestScore = score;
                estimate.root = root;
                estimate.minor = isMinor;
            } else if (score > secondScore) {
                secondScore = score;
            }
        };
        consider(correlate(kMajorProfile, majMean, root), false);
        consider(correlate(kMinorProfile, minMean, root), true);
    }
    // Confidence = how far the winning key beats the runner-up (correlation margin), clamped ≥ 0.
    estimate.confidence = std::max(0.0, bestScore - std::max(0.0, secondScore));
    return estimate;
}

bool isDiatonicTriad(int chordRoot, bool chordMinor, int keyRoot, bool keyMinor) {
    const int degree = (chordRoot - keyRoot + 12) % 12;
    if (keyMinor) {
        return (degree == 0 && chordMinor) ||
            (degree == 3 && !chordMinor) ||
            (degree == 5 && chordMinor) ||
            (degree == 7 && chordMinor) ||
            (degree == 8 && !chordMinor) ||
            (degree == 10 && !chordMinor);
    }
    return (degree == 0 && !chordMinor) ||
        (degree == 2 && chordMinor) ||
        (degree == 4 && chordMinor) ||
        (degree == 5 && !chordMinor) ||
        (degree == 7 && !chordMinor) ||
        (degree == 9 && chordMinor);
}

std::string estimateChordNameFromChroma(const std::array<double, 12>& chroma, int keyRoot, bool keyMinor, bool keyKnown) {
    static constexpr std::array<const char*, 12> kNames {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
    double total = 0.0;
    for (double value : chroma) {
        total += value;
    }
    if (total <= 1.0e-6) {
        return "N.C.";
    }
    auto sortedChroma = chroma;
    std::sort(sortedChroma.begin(), sortedChroma.end(), std::greater<double>());
    const double strongestPitchShare = sortedChroma[0] / total;
    const double triadPitchShare = (sortedChroma[0] + sortedChroma[1] + sortedChroma[2]) / total;
    if (strongestPitchShare < 0.13 || triadPitchShare < 0.36) {
        return "N.C.";
    }
    double bestScore = 0.0;
    double secondScore = 0.0;
    int bestRoot = 0;
    bool bestMinor = false;
    const auto considerChord = [&](double score, int root, bool minor) {
        if (score > bestScore) {
            secondScore = bestScore;
            bestScore = score;
            bestRoot = root;
            bestMinor = minor;
        } else if (score > secondScore) {
            secondScore = score;
        }
    };
    for (int root = 0; root < 12; ++root) {
        const bool majorIsDiatonic = keyKnown && isDiatonicTriad(root, false, keyRoot, keyMinor);
        const bool minorIsDiatonic = keyKnown && isDiatonicTriad(root, true, keyRoot, keyMinor);
        const double major = chroma[static_cast<size_t>(root)] * 1.15 +
            chroma[static_cast<size_t>((root + 4) % 12)] +
            chroma[static_cast<size_t>((root + 7) % 12)] * 0.95;
        const double minor = chroma[static_cast<size_t>(root)] * 1.15 +
            chroma[static_cast<size_t>((root + 3) % 12)] +
            chroma[static_cast<size_t>((root + 7) % 12)] * 0.95;
        const double majorScore = major * (majorIsDiatonic ? 1.24 : (keyKnown ? 0.72 : 1.0));
        const double minorScore = minor * (minorIsDiatonic ? 1.24 : (keyKnown ? 0.72 : 1.0));
        considerChord(majorScore, root, false);
        considerChord(minorScore, root, true);
    }
    const double scoreShare = bestScore / total;
    const double margin = bestScore > 0.0 ? (bestScore - secondScore) / bestScore : 0.0;
    if (scoreShare < 0.20 || margin < 0.055) {
        return "N.C.";
    }
    return std::string(kNames[static_cast<size_t>(bestRoot)]) + (bestMinor ? "m" : "");
}

double tonalContinuityForWindow(const WavAudioData& data, double startSeconds, double durationSeconds) {
    if (data.sampleRate <= 0 || durationSeconds <= 0.05 || data.frameCount() <= 0) {
        return 0.0;
    }
    const int64_t startFrame = std::max<int64_t>(0, static_cast<int64_t>(std::floor(startSeconds * data.sampleRate)));
    const int64_t endFrame = std::min<int64_t>(data.frameCount(), static_cast<int64_t>(std::ceil((startSeconds + durationSeconds) * data.sampleRate)));
    if (endFrame <= startFrame + 128) {
        return 0.0;
    }
    const int step = std::max(1, data.sampleRate / 1000);
    int activeFrames = 0;
    int totalFrames = 0;
    for (int64_t frame = startFrame; frame < endFrame; frame += step) {
        if (std::abs(monoSampleAtFrame(data, frame)) > 0.012f) {
            ++activeFrames;
        }
        ++totalFrames;
    }
    return totalFrames > 0 ? static_cast<double>(activeFrames) / static_cast<double>(totalFrames) : 0.0;
}

std::string estimateChordNameForWindow(const WavAudioData& data, double startSeconds, double durationSeconds, int keyRoot, bool keyMinor, bool keyKnown) {
    if (tonalContinuityForWindow(data, startSeconds, durationSeconds) < 0.08) {
        return "N.C.";
    }
    return estimateChordNameFromChroma(chromaForWindow(data, startSeconds, durationSeconds), keyRoot, keyMinor, keyKnown);
}

bool markerAlreadyNear(const std::vector<MarkerState>& markers, double seconds, double tolerance) {
    return std::any_of(markers.begin(), markers.end(), [&](const MarkerState& marker) {
        return std::abs(marker.timeSeconds - seconds) <= tolerance;
    });
}

bool chordAlreadyNear(const std::vector<ChordEventState>& chords, double seconds, double tolerance) {
    return std::any_of(chords.begin(), chords.end(), [&](const ChordEventState& chord) {
        return std::abs(chord.timeSeconds - seconds) <= tolerance;
    });
}

bool isAutoSectionMarkerName(const std::string& name) {
    return name == "Intro" ||
        name == "Verse" ||
        name == "Pre" ||
        name == "Chorus" ||
        name == "Bridge" ||
        name == "Outro";
}

bool secondsInRange(double seconds, double startSeconds, double endSeconds, double tolerance = 0.02) {
    return std::isfinite(seconds) && seconds >= startSeconds - tolerance && seconds <= endSeconds + tolerance;
}

WavAudioData slicedAudioForClip(const WavAudioData& sourceData, const ClipState& clip) {
    WavAudioData slice;
    if (sourceData.channels <= 0 || sourceData.sampleRate <= 0 || sourceData.frameCount() <= 0 ||
        !std::isfinite(clip.sourceOffsetSeconds) || !std::isfinite(clip.durationSeconds) ||
        clip.durationSeconds <= 0.0) {
        return slice;
    }
    const int64_t sourceStartFrame = std::max<int64_t>(0, static_cast<int64_t>(std::round(clip.sourceOffsetSeconds * sourceData.sampleRate)));
    if (sourceStartFrame >= sourceData.frameCount()) {
        return slice;
    }
    const int64_t requestedFrames = std::max<int64_t>(1, static_cast<int64_t>(std::round(clip.durationSeconds * sourceData.sampleRate)));
    const int64_t availableFrames = sourceData.frameCount() - sourceStartFrame;
    const int64_t framesToCopy = std::min(requestedFrames, availableFrames);
    slice.channels = sourceData.channels;
    slice.sampleRate = sourceData.sampleRate;
    slice.bitsPerSample = sourceData.bitsPerSample;
    slice.floatingPoint = sourceData.floatingPoint;
    slice.embeddedTempoBpm = sourceData.embeddedTempoBpm;
    slice.hasBroadcastTimeReference = sourceData.hasBroadcastTimeReference;
    slice.broadcastTimeReferenceSamples = sourceData.broadcastTimeReferenceSamples + static_cast<uint64_t>(std::max<int64_t>(0, sourceStartFrame));
    slice.broadcastTimeReferenceSeconds = sourceData.broadcastTimeReferenceSeconds +
        static_cast<double>(std::max<int64_t>(0, sourceStartFrame)) / static_cast<double>(sourceData.sampleRate);
    const size_t sourceBegin = static_cast<size_t>(sourceStartFrame) * static_cast<size_t>(sourceData.channels);
    const size_t sampleCount = static_cast<size_t>(framesToCopy) * static_cast<size_t>(sourceData.channels);
    if (sourceBegin >= sourceData.interleavedSamples.size()) {
        return {};
    }
    const size_t sourceEnd = std::min(sourceData.interleavedSamples.size(), sourceBegin + sampleCount);
    slice.interleavedSamples.assign(sourceData.interleavedSamples.begin() + static_cast<std::ptrdiff_t>(sourceBegin),
                                    sourceData.interleavedSamples.begin() + static_cast<std::ptrdiff_t>(sourceEnd));
    return slice;
}

std::string lastChordNameBeforeOrAt(const std::vector<ChordEventState>& chords, double seconds) {
    std::string name;
    for (const auto& chord : chords) {
        if (chord.timeSeconds <= seconds + 0.0005) {
            name = chord.name;
        }
    }
    return name;
}

} // namespace

std::string analyzeImportedAudioIntoProject(ProjectDocument& project,
                                            const WavAudioData& data,
                                            double clipStartSeconds,
                                            double clipDurationSeconds,
                                            bool hasEmbeddedTempo,
                                            bool applyToTimeline) {
    static constexpr std::array<const char*, 12> kKeyNames {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
    double bpm = hasEmbeddedTempo ? data.embeddedTempoBpm : estimateTempoBpmFromAudio(data);
    if (!(std::isfinite(bpm) && bpm >= 55.0 && bpm <= 190.0)) {
        bpm = static_cast<double>(std::max(55, std::min(190, project.tempoBpm)));
    }
    auto meterGroove = estimateMeterAndGroove(data, bpm, clipDurationSeconds);
    bool normalizedCompoundTempo = false;
    if (hasEmbeddedTempo && bpm >= 55.0 && bpm <= 127.0) {
        const double eighthTempoCandidate = bpm * 1.5;
        if (eighthTempoCandidate <= 190.0) {
            const auto compoundCandidate = estimateMeterAndGroove(data, eighthTempoCandidate, clipDurationSeconds);
            if (compoundCandidate.denominator == 8 &&
                compoundCandidate.numerator == meterGroove.numerator &&
                compoundCandidate.compoundConfidence > 1.72 &&
                (meterGroove.denominator != 8 ||
                 compoundCandidate.compoundConfidence >= meterGroove.compoundConfidence * 0.88)) {
                bpm = eighthTempoCandidate;
                meterGroove = compoundCandidate;
                normalizedCompoundTempo = true;
            }
        }
    }
    // Detection results are held in locals so the summary reads the same whether or not it is
    // committed to the timeline. Only the project writes below are gated on applyToTimeline.
    const int numerator = meterGroove.numerator;
    const int denominator = meterGroove.denominator;
    const auto songChroma = averagedChromaForKey(data, std::min(clipDurationSeconds, 180.0));
    const KeyEstimate key = estimateKeyFromChroma(songChroma);
    const bool keyKnown = key.confidence >= 0.018 && key.tonalShare >= 0.36 &&
        tonalContinuityForWindow(data, 0.0, std::min(clipDurationSeconds, 180.0)) >= 0.08;
    const std::string detectedKeyName = keyKnown
        ? std::string(kKeyNames[static_cast<size_t>(std::max(0, std::min(11, key.root)))]) : "Unknown";
    const std::string detectedKeyMode = keyKnown ? (key.minor ? "minor" : "major") : "unknown";
    const bool chordKeyMinor = project.chordKeyModePreference == "minor"
        ? true
        : (project.chordKeyModePreference == "major" ? false : key.minor);
    const double beatSeconds = 60.0 / bpm;
    const double barSeconds = beatSeconds *
        static_cast<double>(std::max(1, numerator)) *
        (4.0 / static_cast<double>(std::max(1, denominator)));
    const double quarterNoteBeats = clipDurationSeconds / std::max(0.01, beatSeconds);
    const double notatedBeats = quarterNoteBeats *
        (static_cast<double>(std::max(1, denominator)) / 4.0);
    const double barCount = clipDurationSeconds / std::max(0.25, barSeconds);
    const int maxBars = static_cast<int>(std::max(1.0, std::ceil(clipDurationSeconds / std::max(0.25, barSeconds))));
    const int chordBars = std::min(maxBars, 96);
    size_t chordChangesAdded = 0;

    if (applyToTimeline) {
        project.tempoBpm = static_cast<int>(std::round(bpm));
        project.timeSignatureNumerator = numerator;
        project.timeSignatureDenominator = denominator;
        project.timeSignatureMap.clear();
        project.timeSignatureMap.push_back({0.0, numerator, denominator});
        project.grooveFeel = meterGroove.groove;
        project.grooveSwingAmount = meterGroove.swing;
        project.detectedKey = detectedKeyName;
        project.detectedKeyMode = detectedKeyMode;
        if (!hasEmbeddedTempo || project.tempoMap.empty()) {
            project.tempoMap.clear();
        }
        for (int bar = 0; bar <= maxBars; ++bar) {
            const double markerSeconds = clipStartSeconds + static_cast<double>(bar) * barSeconds;
            if (markerSeconds > clipStartSeconds + clipDurationSeconds + 0.01) {
                break;
            }
            const bool existing = std::any_of(project.tempoMap.begin(), project.tempoMap.end(), [&](const TempoMarkerState& marker) {
                return std::abs(marker.timeSeconds - markerSeconds) < 0.02;
            });
            if (!existing) {
                project.tempoMap.push_back({markerSeconds, bpm});
            }
        }
        std::sort(project.tempoMap.begin(), project.tempoMap.end(), [](const TempoMarkerState& left, const TempoMarkerState& right) {
            return left.timeSeconds < right.timeSeconds;
        });

        std::string lastChordName = lastChordNameBeforeOrAt(project.chordEvents, clipStartSeconds);
        for (int bar = 0; bar < chordBars; ++bar) {
            const double eventSeconds = clipStartSeconds + static_cast<double>(bar) * barSeconds;
            if (eventSeconds > clipStartSeconds + clipDurationSeconds - 0.05 || chordAlreadyNear(project.chordEvents, eventSeconds, 0.2)) {
                continue;
            }
            const double localStart = static_cast<double>(bar) * barSeconds;
            std::string chord = estimateChordNameForWindow(data,
                                                           localStart,
                                                           std::min(barSeconds, clipDurationSeconds - localStart),
                                                           key.root,
                                                           chordKeyMinor,
                                                           keyKnown);
            if (chord != "N.C." && !chord.empty() && chord != lastChordName) {
                addChordEventAt(project, eventSeconds, chord);
                lastChordName = chord;
                ++chordChangesAdded;
            }
        }

        const std::array<const char*, 6> sectionNames {"Intro", "Verse", "Pre", "Chorus", "Bridge", "Outro"};
        const int sectionStepBars = 8;
        for (int section = 0; section * sectionStepBars < maxBars && section < 16; ++section) {
            const double sectionSeconds = clipStartSeconds + static_cast<double>(section * sectionStepBars) * barSeconds;
            if (markerAlreadyNear(project.markers, sectionSeconds, 0.5)) {
                continue;
            }
            addMarkerAt(project, sectionSeconds);
            renameNearestMarker(project, sectionSeconds, 0.1, sectionNames[static_cast<size_t>(std::min<int>(section, static_cast<int>(sectionNames.size() - 1)))]);
        }
    }

    return std::string(applyToTimeline ? "" : "analysis only (timeline unchanged) · ") +
        (hasEmbeddedTempo ? (normalizedCompoundTempo ? "embedded dotted-pulse tempo normalized" : "embedded tempo") : "estimated tempo") +
        " " + std::to_string(static_cast<int>(std::round(bpm))) +
        " BPM · " + std::to_string(numerator) + "/" + std::to_string(denominator) +
        " · " + std::to_string(static_cast<int>(std::round(barCount))) + " bar(s) / " +
        std::to_string(static_cast<int>(std::round(notatedBeats))) + " beat(s)" +
        " · " + detectedKeyName + (detectedKeyMode == "unknown" ? "" : (detectedKeyMode == "minor" ? " minor" : " major")) +
        " · chord key mode " + (project.chordKeyModePreference.empty() ? "auto" : project.chordKeyModePreference) +
        " · " + meterGroove.groove + (meterGroove.swing > 0.0 ? " " + std::to_string(static_cast<int>(std::round(meterGroove.swing * 100.0))) + "%" : "") +
        " · " + std::to_string(chordChangesAdded) + " chord change(s) from " + std::to_string(chordBars) + " bar scan";
}

std::string reanalyzeClipMusicalMetadata(ProjectDocument& project,
                                         const std::string& clipId,
                                         const WavAudioData& sourceData,
                                         std::string& error) {
    error.clear();
    auto clipIt = std::find_if(project.clips.begin(), project.clips.end(), [&](const ClipState& clip) {
        return clip.id == clipId;
    });
    if (clipIt == project.clips.end()) {
        error = "Clip not found.";
        return {};
    }
    const WavAudioData clipAudio = slicedAudioForClip(sourceData, *clipIt);
    if (clipAudio.channels <= 0 || clipAudio.sampleRate <= 0 || clipAudio.frameCount() <= 0) {
        error = "Clip source audio could not be sliced for analysis.";
        return {};
    }
    const double clipStartSeconds = std::max(0.0, clipIt->startSeconds);
    const double clipDurationSeconds = static_cast<double>(clipAudio.frameCount()) / static_cast<double>(clipAudio.sampleRate);
    const double clipEndSeconds = clipStartSeconds + clipDurationSeconds;
    project.tempoMap.erase(std::remove_if(project.tempoMap.begin(), project.tempoMap.end(), [&](const TempoMarkerState& tempo) {
        return secondsInRange(tempo.timeSeconds, clipStartSeconds, clipEndSeconds);
    }), project.tempoMap.end());
    project.chordEvents.erase(std::remove_if(project.chordEvents.begin(), project.chordEvents.end(), [&](const ChordEventState& chord) {
        return secondsInRange(chord.timeSeconds, clipStartSeconds, clipEndSeconds, 0.20);
    }), project.chordEvents.end());
    project.markers.erase(std::remove_if(project.markers.begin(), project.markers.end(), [&](const MarkerState& marker) {
        return secondsInRange(marker.timeSeconds, clipStartSeconds, clipEndSeconds, 0.50) &&
            isAutoSectionMarkerName(marker.name);
    }), project.markers.end());
    const bool hasEmbeddedTempo = clipAudio.embeddedTempoBpm >= 20.0 && clipAudio.embeddedTempoBpm <= 400.0;
    const std::string summary = analyzeImportedAudioIntoProject(project, clipAudio, clipStartSeconds, clipDurationSeconds, hasEmbeddedTempo);
    auto refreshedClipIt = std::find_if(project.clips.begin(), project.clips.end(), [&](const ClipState& clip) {
        return clip.id == clipId;
    });
    if (refreshedClipIt != project.clips.end()) {
        refreshedClipIt->sourceTempoBpm = static_cast<double>(project.tempoBpm);
        refreshedClipIt->sourceTimeSignatureNumerator = project.timeSignatureNumerator;
        refreshedClipIt->sourceTimeSignatureDenominator = project.timeSignatureDenominator;
        refreshedClipIt->sourceGrooveFeel = project.grooveFeel;
        refreshedClipIt->sourceGrooveSwingAmount = project.grooveSwingAmount;
    }
    return summary;
}

ProjectMusicReanalysisReport reanalyzeProjectMusicalMetadata(ProjectDocument& project,
                                                             const std::function<bool(const std::string& path,
                                                                                      WavAudioData& data,
                                                                                      std::string& error)>& loadSourceAudio) {
    ProjectMusicReanalysisReport report;
    if (!loadSourceAudio) {
        report.summary = "Project music reanalysis failed: no source loader.";
        return report;
    }

    std::vector<std::string> clipIds;
    clipIds.reserve(project.clips.size());
    for (const auto& clip : project.clips) {
        if (!clip.id.empty()) {
            clipIds.push_back(clip.id);
        }
    }

    std::map<std::string, WavAudioData> audioCache;
    std::set<std::string> loadedPaths;
    for (const auto& clipId : clipIds) {
        const auto clipIt = std::find_if(project.clips.begin(), project.clips.end(), [&](const ClipState& clip) {
            return clip.id == clipId;
        });
        if (clipIt == project.clips.end() || clipIt->sourcePath.empty()) {
            ++report.skippedClips;
            report.messages.push_back("Skipped clip without source audio: " + clipId);
            continue;
        }

        WavAudioData* sourceAudio = nullptr;
        auto cacheIt = audioCache.find(clipIt->sourcePath);
        if (cacheIt == audioCache.end()) {
            WavAudioData loadedAudio;
            std::string loadError;
            if (!loadSourceAudio(clipIt->sourcePath, loadedAudio, loadError) || loadedAudio.frameCount() <= 0) {
                ++report.skippedClips;
                report.messages.push_back("Skipped " + clipId + ": " + (loadError.empty() ? "source WAV could not be read" : loadError));
                continue;
            }
            loadedPaths.insert(clipIt->sourcePath);
            cacheIt = audioCache.emplace(clipIt->sourcePath, std::move(loadedAudio)).first;
        } else {
            ++report.reusedSourceFiles;
        }
        sourceAudio = &cacheIt->second;

        std::string analysisError;
        const std::string clipSummary = reanalyzeClipMusicalMetadata(project, clipId, *sourceAudio, analysisError);
        if (!analysisError.empty()) {
            ++report.skippedClips;
            report.messages.push_back("Skipped " + clipId + ": " + analysisError);
            continue;
        }
        ++report.analyzedClips;
        report.messages.push_back(clipId + ": " + clipSummary);
    }

    report.summary = "reanalyzed " + std::to_string(report.analyzedClips) + " clip(s), skipped " +
        std::to_string(report.skippedClips) + ", loaded " + std::to_string(loadedPaths.size()) +
        " source file(s)";
    return report;
}

} // namespace neuracoust::daw
