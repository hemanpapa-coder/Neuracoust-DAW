#pragma once

namespace neuracoust::daw {

float dbToLinearGain(float db);
double dawFaderPositionForDb(float gainDb);
float dbForDawFaderPosition(double position);

} // namespace neuracoust::daw
