#include "audio/MixMath.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace neuracoust::daw {

namespace {

struct FaderPoint {
    double position;
    float db;
};

constexpr std::array<FaderPoint, 9> kDawFaderCurve{{
    {0.000, -120.0f},
    {0.080, -60.0f},
    {0.250, -30.0f},
    {0.390, -20.0f},
    {0.560, -10.0f},
    {0.680, -5.0f},
    {0.820, 0.0f},
    {0.940, 6.0f},
    {1.000, 12.0f},
}};

double interpolate(double x, double x0, double x1, double y0, double y1) {
    if (x1 <= x0) {
        return y0;
    }
    const double t = std::max(0.0, std::min(1.0, (x - x0) / (x1 - x0)));
    return y0 + (y1 - y0) * t;
}

} // namespace

float dbToLinearGain(float db) {
    if (!std::isfinite(db) || db <= -119.5f) {
        return 0.0f;
    }
    return std::pow(10.0f, db / 20.0f);
}

double dawFaderPositionForDb(float gainDb) {
    if (!std::isfinite(gainDb) || gainDb <= -119.5f) {
        return 0.0;
    }
    const double db = std::max(-120.0, std::min(12.0, static_cast<double>(gainDb)));
    for (size_t index = 1; index < kDawFaderCurve.size(); ++index) {
        const auto& right = kDawFaderCurve[index];
        if (db <= right.db) {
            const auto& left = kDawFaderCurve[index - 1];
            return interpolate(db, left.db, right.db, left.position, right.position);
        }
    }
    return 1.0;
}

float dbForDawFaderPosition(double position) {
    const double pos = std::max(0.0, std::min(1.0, position));
    if (pos <= 0.0001) {
        return -120.0f;
    }
    for (size_t index = 1; index < kDawFaderCurve.size(); ++index) {
        const auto& right = kDawFaderCurve[index];
        if (pos <= right.position) {
            const auto& left = kDawFaderCurve[index - 1];
            return static_cast<float>(interpolate(pos, left.position, right.position, left.db, right.db));
        }
    }
    return 12.0f;
}

} // namespace neuracoust::daw
