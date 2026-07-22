#pragma once

#include <string>
#include <vector>

namespace neuracoust::daw {

// Audio-interface D/A output-stage catalog, baked from the research package by
// tools/gen_audio_interface_catalog.py. Catalog + measurement STATUS only — there are no bundled
// DSP coefficients, so selecting a model defines/labels the monitoring chain but does not (yet)
// colour the audio. Real D/A modelling waits for measured profiles (see the research handoff).

// Every catalog name ("None" first), for the picker.
const std::vector<std::string>& audioInterfaceModelCatalog();

// True when an independent measurement exists for the model (lights the "측정" badge). This does
// NOT mean a usable profile is bundled — it only marks that real data was located.
bool audioInterfaceModelMeasured(const std::string& name);

} // namespace neuracoust::daw
