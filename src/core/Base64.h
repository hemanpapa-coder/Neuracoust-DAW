#pragma once

// Base64 for plug-in state blobs. A VST3 component's state is opaque binary — the
// only way a workstation instrument's patch (KORG TRITON's program, a sampler's
// loaded instrument) survives a project save, or crosses the editor-host process
// boundary, is verbatim. The project document is JSON, so the blob is text here.

#include <cstdint>
#include <string>
#include <vector>

namespace neuracoust::daw {

std::string encodeBase64(const std::vector<uint8_t>& bytes);
std::string encodeBase64(const void* data, size_t length);

/// Decodes standard base64. Whitespace is skipped; any other invalid character
/// makes the whole decode fail (returns false, `out` cleared) rather than
/// silently handing a plug-in a truncated state.
bool decodeBase64(const std::string& text, std::vector<uint8_t>& out);

} // namespace neuracoust::daw
