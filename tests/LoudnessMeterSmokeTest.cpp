// Verifies BS.1770 loudness: a -20 dBFS 1 kHz sine reads in a sane LUFS window and a
// quieter signal reads lower.
#include "audio/LoudnessMeter.h"

int main() {
    return neuracoust::daw::LoudnessMeter::runSelfTest();
}
