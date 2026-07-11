// Verifies the analyzer's FFT: a 1 kHz tone must peak in the expected bin.
#include "audio/NeuracoustDspEngine.h"

int main() {
    return neuracoust::daw::NeuracoustDspEngine::runSpectrumSelfTest();
}
