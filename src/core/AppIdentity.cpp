#include "core/AppIdentity.h"

namespace neuracoust::daw {

AppIdentity currentAppIdentity() {
    return {
        "neuracoust-daw",
        "Neuracoust DAW",
        NEURACOUST_DAW_VERSION,
        "(C) 2026 Neuracoust",
        {"Free", "Demo", "Trial", "Pro", "Studio", "Signal"}
    };
}

} // namespace neuracoust::daw
