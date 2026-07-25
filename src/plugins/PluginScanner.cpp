#include "plugins/PluginScanner.h"
#include "plugins/Vst3HostFoundation.h"
#include "plugins/Vst3SdkAdapter.h"
#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <vector>

namespace neuracoust::daw {

namespace {

std::string lowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool pluginCandidateDisplayLess(const PluginCandidate& lhs, const PluginCandidate& rhs) {
    const auto lhsKey = lowerCopy(lhs.brand + "\n" + lhs.name + "\n" + lhs.category + "\n" +
                                  lhs.format + "\n" + lhs.scope + "\n" + lhs.pluginName + "\n" +
                                  lhs.pluginClassId + "\n" + lhs.pluginClassName + "\n" + lhs.path);
    const auto rhsKey = lowerCopy(rhs.brand + "\n" + rhs.name + "\n" + rhs.category + "\n" +
                                  rhs.format + "\n" + rhs.scope + "\n" + rhs.pluginName + "\n" +
                                  rhs.pluginClassId + "\n" + rhs.pluginClassName + "\n" + rhs.path);
    return lhsKey < rhsKey;
}

std::vector<std::string> sortedUniqueNonEmptyValues(const std::vector<PluginCandidate>& candidates,
                                                    const std::string PluginCandidate::*member) {
    std::set<std::string> values;
    for (const auto& candidate : candidates) {
        const auto& value = candidate.*member;
        if (!value.empty()) {
            values.insert(value);
        }
    }
    std::vector<std::string> result(values.begin(), values.end());
    // Case-INSENSITIVE display order so e.g. "iZotope" sorts under I with the rest, not last after
    // Z (a byte sort puts lowercase after all uppercase). Brand / category lists read alphabetically.
    std::sort(result.begin(), result.end(),
              [](const std::string& a, const std::string& b) { return lowerCopy(a) < lowerCopy(b); });
    return result;
}

bool pathStartsWithHome(const std::string& path) {
    const char* home = std::getenv("HOME");
    return home != nullptr && *home != '\0' && path.rfind(home, 0) == 0;
}

#if defined(_WIN32)
bool pathStartsWithUserDataRoot(const std::string& path) {
    for (const char* variable : {"APPDATA", "LOCALAPPDATA"}) {
        const char* root = std::getenv(variable);
        if (root != nullptr && *root != '\0' && path.rfind(root, 0) == 0) {
            return true;
        }
    }
    return false;
}
#endif

bool containsAny(const std::string& haystack, std::initializer_list<const char*> needles) {
    for (const auto* needle : needles) {
        if (haystack.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

/// Reads the plug-in's moduleinfo.json (VST3 3.7+) and looks for an ARA Main Factory class. A file
/// read — no plug-in code runs, which is what makes this safe to do for every plug-in in a scan.
bool vst3ModuleInfoMentionsAra(const Vst3PluginDescriptor& descriptor) {
    if (descriptor.moduleInfoPath.empty()) {
        return false;
    }
    std::ifstream file(descriptor.moduleInfoPath, std::ios::binary);
    if (!file) {
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return text.find("ARA Main Factory") != std::string::npos;
}

/// The plug-ins we already know are ARA, for the ones that ship no moduleinfo.json (Melodyne is one).
bool pluginNameLooksLikeAra(const std::string& name) {
    const auto text = lowerCopy(name);
    return containsAny(text, {"melodyne", "celemony", "spectralayers", "revoice"});
}

std::string normalizedSearchText(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        if (ch == '_' || ch == '-' || ch == '/' || ch == '\\') {
            return ' ';
        }
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string compactSearchText(const std::string& value) {
    std::string compact;
    compact.reserve(value.size());
    for (const unsigned char ch : value) {
        if (std::isalnum(ch)) {
            compact.push_back(static_cast<char>(std::tolower(ch)));
        }
    }
    return compact;
}

std::string wordInitialsSearchText(const std::string& value) {
    std::string initials;
    bool atWordStart = true;
    for (const unsigned char ch : normalizedSearchText(value)) {
        if (std::isalnum(ch)) {
            if (atWordStart) {
                initials.push_back(static_cast<char>(std::tolower(ch)));
            }
            atWordStart = false;
        } else {
            atWordStart = true;
        }
    }
    return initials;
}

std::string categorySearchAliases(const std::string& category) {
    const auto lower = lowerCopy(category);
    if (lower.find("reverb") != std::string::npos) {
        return " verb room hall plate chamber ambience space 리버브 공간 홀 플레이트";
    }
    if (lower.find("delay") != std::string::npos) {
        return " echo slap repeat tape 딜레이 에코";
    }
    if (lower.find("dynamics") != std::string::npos) {
        return " compressor comp limiter limit gate expander deesser de-esser dynamic dynamics 컴프 컴프레서 리미터 게이트 다이나믹";
    }
    if (lower.find("eq") != std::string::npos || lower.find("filter") != std::string::npos) {
        return " equalizer filter eq dynamic-eq 이큐 이퀄라이저 필터";
    }
    if (lower.find("saturation") != std::string::npos) {
        return " distortion saturator drive clip tape tube amp exciter 새츄레이션 디스토션 드라이브 테이프";
    }
    if (lower.find("modulation") != std::string::npos) {
        return " chorus flanger phaser tremolo vibrato doubler modulation 모듈레이션 코러스 플랜저 페이저";
    }
    if (lower.find("pitch") != std::string::npos || lower.find("vocal") != std::string::npos) {
        return " pitch tune vocal voice doubler adt 피치 보컬 튠";
    }
    if (lower.find("mastering") != std::string::npos) {
        return " master mastering limiter loudness meter 마스터 마스터링 라우드니스";
    }
    if (lower.find("spatial") != std::string::npos) {
        return " stereo image imager surround room binaural spatial 스테레오 이미저 공간 서라운드";
    }
    if (lower.find("analyzer") != std::string::npos) {
        return " meter metering analyzer analysis loudness scope 미터 분석";
    }
    if (lower.find("instrument") != std::string::npos) {
        return " synth sampler piano drum bass instrument 악기 신스 샘플러";
    }
    if (lower.find("guitar") != std::string::npos || lower.find("amp") != std::string::npos) {
        return " guitar amp stomp pedal cabinet 기타 앰프 페달";
    }
    if (lower.find("restoration") != std::string::npos || lower.find("noise") != std::string::npos) {
        return " repair restore noise denoise declick declip cleanup 노이즈 복원 수리";
    }
    if (lower.find("generator") != std::string::npos) {
        return " generator tone synth sub bass signal 생성기 시그널 서브";
    }
    return {};
}

std::string pluginCategoryOverride(const std::string& name) {
    const auto text = lowerCopy(name);
    if (containsAny(text, {"neuracoust mirage", "mirage 4", "mirage 8", "mirage 70", "mirage 901", "mirage 991"})) {
        return "Reverb";
    }
    if (containsAny(text, {"fabfilter twin", "fabfilter one"})) {
        return "Instrument";
    }
    if (containsAny(text, {"fabfilter pro-c", "fabfilter pro-mb", "fabfilter pro-ds", "fabfilter pro-g", "fabfilter pro-l"})) {
        return "Dynamics";
    }
    if (containsAny(text, {"fabfilter pro-r"})) {
        return "Reverb";
    }
    if (containsAny(text, {"fabfilter timeless"})) {
        return "Delay";
    }
    if (containsAny(text, {"fabfilter saturn"})) {
        return "Saturation";
    }
    if (containsAny(text, {"fabfilter pro-q", "fabfilter volcano", "fabfilter simplon", "fabfilter micro"})) {
        return "EQ / Filter";
    }
    if (containsAny(text, {"ozone 12", "ozone "})) {
        if (containsAny(text, {"maximizer", "dynamics", "compressor", "limiter", "unlimiter", "impact"})) {
            return "Dynamics";
        }
        if (containsAny(text, {"equalizer", "dynamic eq", "match eq", "stem eq", "vintage eq", "stabilizer"})) {
            return "EQ / Filter";
        }
        if (containsAny(text, {"exciter", "vintage tape"})) {
            return "Saturation";
        }
        if (containsAny(text, {"imager"})) {
            return "Spatial";
        }
        if (containsAny(text, {"clarity", "spectral shaper"})) {
            return "Restoration";
        }
        return "Mastering";
    }
    if (containsAny(text, {"rx 12 de-click", "rx 12 de-clip", "rx 12 de-crackle", "rx 12 de-hum", "rx 12 de-plosive",
                           "rx 12 de-bleed", "rx 12 de-ess", "rx 12 spectral de-noise", "rx 12 voice de-noise",
                           "rx 12 guitar de-noise", "rx 12 dialogue isolate", "rx 12 mouth de-click",
                           "rx 12 repair assistant", "rx 12 breath control"})) {
        return "Restoration";
    }
    if (containsAny(text, {"bass rider", "vocal rider"})) {
        return "Dynamics";
    }
    if (containsAny(text, {"cla effects", "cla guitars", "cla unplugged", "ekramer ba", "ekramer dr"})) {
        return "Artist / Multi FX";
    }
    if (containsAny(text, {"cla epic", "echosphere"})) {
        return "Delay";
    }
    if (containsAny(text, {"h-reverb", "h reverb", "hverb", "oneknob wetter", "one knob wetter"})) {
        return "Reverb";
    }
    if (containsAny(text, {"loair", "lo air", "submarine", "sub align"})) {
        return "Generator";
    }
    if (containsAny(text, {"oneknob brighter", "one knob brighter"})) {
        return "EQ / Filter";
    }
    if (containsAny(text, {"oneknob louder", "one knob louder", "oneknob pressure", "one knob pressure"})) {
        return "Dynamics";
    }
    if (containsAny(text, {"oneknob phatter", "one knob phatter", "retro fi", "gtr stomp", "gtr tool rack", "gtr solo tool rack",
                           "prs archon", "prs dallas", "prs v9", "ekramer gt"})) {
        return "Guitar / Amp";
    }
    if (containsAny(text, {"ekramer fx", "jjp-guitars", "jjp guitars", "jjp-strings-keys", "jjp strings keys",
                           "maserati acg", "maserati gti", "maserati hmx", "studiovers audio effects", "studioverse audio effects",
                           "waves gemstones"})) {
        return "Artist / Multi FX";
    }
    if (containsAny(text, {"inphase", "in phase"})) {
        return "Phase / Alignment";
    }
    if (containsAny(text, {"key detector"})) {
        return "Analyzer";
    }
    if (containsAny(text, {"feedback hunter", "x-fdbk", "x fdbk"})) {
        return "Restoration";
    }
    if (containsAny(text, {"dorrough"})) {
        return text.find("360") != std::string::npos ? "Spatial" : "Analyzer";
    }
    return {};
}

std::string pluginSearchAliasText(const std::string& brand,
                                  const std::string& name,
                                  const std::string& category,
                                  const std::string& pluginName,
                                  const std::string& pluginClassName) {
    const std::string identity = brand + " " + name + " " + pluginName + " " + pluginClassName;
    const std::string categoryOverride = pluginCategoryOverride(identity);
    std::string aliases = " " + wordInitialsSearchText(identity) + " " +
        compactSearchText(identity) + " " +
        categorySearchAliases(category) + " " +
        categorySearchAliases(categoryOverride);
    const auto lower = lowerCopy(identity);
    if (lower.find("fabfilter") != std::string::npos) {
        aliases += " ff fab fabfilter 패브필터 페브필터";
    }
    if (lower.find("waves") != std::string::npos || lower.find("waveshell") != std::string::npos) {
        aliases += " waves wave 웨이브스";
    }
    if (lower.find("izotope") != std::string::npos || lower.find("ozone") != std::string::npos || lower.find("rx 12") != std::string::npos) {
        aliases += " izotope ozone rx 아이조톱 아이조토프 오존";
    }
    if (lower.find("neuracoust") != std::string::npos || lower.find("newacoust") != std::string::npos) {
        aliases += " neuracoust newacoust 누라쿠스트 뉴라쿠스트 뉴어쿠스트";
    }
    return aliases;
}

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string trimCopy(std::string value) {
    auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](unsigned char ch) {
        return !isSpace(ch);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](unsigned char ch) {
        return !isSpace(ch);
    }).base(), value.end());
    return value;
}

std::string plistValueAfterKey(const std::string& plist, const std::string& key) {
    const auto keyMarker = "<key>" + key + "</key>";
    const auto keyPos = plist.find(keyMarker);
    if (keyPos == std::string::npos) {
        return {};
    }
    const auto stringStart = plist.find("<string>", keyPos + keyMarker.size());
    const auto stringEnd = plist.find("</string>", stringStart);
    if (stringStart == std::string::npos || stringEnd == std::string::npos) {
        return {};
    }
    return plist.substr(stringStart + 8, stringEnd - (stringStart + 8));
}

std::string bundleMetadataText(const std::filesystem::path& path) {
    const auto plist = readTextFile(path / "Contents" / "Info.plist");
    if (plist.empty()) {
        return {};
    }
    std::string text;
    for (const auto* key : {"CFBundleIdentifier", "CFBundleName", "CFBundleDisplayName", "CFBundleExecutable", "VSTManufacturer"}) {
        text += " " + plistValueAfterKey(plist, key);
    }
    return text;
}

std::string candidateDisplayName(const std::filesystem::path& path) {
    const auto plist = readTextFile(path / "Contents" / "Info.plist");
    if (!plist.empty()) {
        const auto displayName = plistValueAfterKey(plist, "CFBundleDisplayName");
        if (!displayName.empty()) {
            return displayName;
        }
        const auto bundleName = plistValueAfterKey(plist, "CFBundleName");
        if (!bundleName.empty()) {
            return bundleName;
        }
    }
    return path.stem().string();
}

std::string manifestNameValue(const std::filesystem::path& manifestPath) {
    std::istringstream lines(readTextFile(manifestPath));
    std::string line;
    while (std::getline(lines, line)) {
        const auto trimmed = trimCopy(line);
        constexpr const char* marker = "name:";
        if (trimmed.rfind(marker, 0) != 0) {
            continue;
        }
        std::string value = trimCopy(trimmed.substr(std::char_traits<char>::length(marker)));
        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }
        return value;
    }
    return {};
}

std::string inferPluginCandidateBrand(const std::string& name,
                                      const std::string& path,
                                      const std::string& metadata) {
    const auto metadataText = lowerCopy(name + " " + metadata);
    const auto text = lowerCopy(name + " " + path + " " + metadata);
    if (containsAny(metadataText, {"neuracoust", "newacoust"})) {
        return "Neuracoust";
    }
    if (metadataText.find("fabfilter") != std::string::npos) {
        return "FabFilter";
    }
    if (containsAny(metadataText, {"izotope", "ozone", " rx", "nectar", "neoverb", "stutter"})) {
        return "iZotope";
    }
    if (containsAny(text, {"waveshell", "waves ", "/waves/"})) {
        return "Waves";
    }
    if (containsAny(text, {"apple", "com.apple."})) {
        return "Apple";
    }
    if (containsAny(text, {"u-he", "uhe"})) {
        return "u-he";
    }
    if (text.find("soundtoys") != std::string::npos) {
        return "Soundtoys";
    }
    if (containsAny(text, {"plugin alliance", "brainworx", "bx_"})) {
        return "Plugin Alliance";
    }
    if (text.find("softube") != std::string::npos) {
        return "Softube";
    }
    if (text.find("arturia") != std::string::npos) {
        return "Arturia";
    }
    if (containsAny(text, {"native instruments", "kontakt", "guitar rig"})) {
        return "Native Instruments";
    }
    if (containsAny(metadataText, {"celemony", "melodyne"})) {
        return "Celemony";
    }
    if (containsAny(metadataText, {"antares", "auto-tune", "autotune"})) {
        return "Antares";
    }
    if (containsAny(text, {"synchro arts", "revoice", "vocalign"})) {
        return "Synchro Arts";
    }
    if (text.find("valhalla") != std::string::npos) {
        return "Valhalla DSP";
    }
    if (text.find("eventide") != std::string::npos) {
        return "Eventide";
    }
    if (containsAny(text, {"sound radix", "soundradix"})) {
        return "Sound Radix";
    }
    if (text.find("oeksound") != std::string::npos) {
        return "oeksound";
    }
    if (text.find("sonible") != std::string::npos) {
        return "sonible";
    }
    if (text.find("acustica") != std::string::npos) {
        return "Acustica Audio";
    }
    if (text.find("plugin boutique") != std::string::npos) {
        return "Plugin Boutique";
    }
    if (containsAny(text, {"mastering the mix", "animate", "bassroom", "fuser", "levels", "reference", "mixroom", "expose"})) {
        return "Mastering The Mix";
    }
    const auto firstSpace = name.find(' ');
    const auto fallback = firstSpace == std::string::npos ? name : name.substr(0, firstSpace);
    return fallback.empty() ? "Unknown" : fallback;
}

std::string inferPluginCandidateCategory(const std::string& name) {
    const auto text = lowerCopy(name);
    if (const auto overrideCategory = pluginCategoryOverride(name); !overrideCategory.empty()) {
        return overrideCategory;
    }
    if (containsAny(text, {"fabfilter twin", "fabfilter one"})) {
        return "Instrument";
    }
    // Well-known synths/samplers whose names carry no generic keyword (Kontakt,
    // Serum, …) — shared with the VST3 scan's categorizer in Vst3HostFoundation.
    if (pluginNameLooksLikeKnownInstrument(text)) {
        return "Instrument";
    }
    if (containsAny(text, {"noise reduction", "restoration", "spectral", "denoise", "de-noise", "de noise", "de-noiser", "rx ", "clarity", "ns1", "x-noise", "x noise", "x hum", "x-hum", "x click", "x-click", "x crackle", "x-crackle", "debreath", "de-breath", "x-fdbk"})) {
        return "Noise Reduction";
    }
    if (containsAny(text, {"oneknob", "one knob", "maserati", "cla ", "cla-", "jjp", "ekramer", "eddie kramer", "greg wells", "butch vig", "scheps parallel", "silk vocal"})) {
        return "Artist / Multi FX";
    }
    if (containsAny(text, {"reverb", "verb", "room", "hall", "plate", "space", "chamber", "ambience"})) {
        return "Reverb";
    }
    if (containsAny(text, {"delay", "echo", "slap", "repeat", "timeless"})) {
        return "Delay";
    }
    if (containsAny(text, {"pitch", "tune", "vocal", "doubler", "adt", "shift", "ovox", "morphoder", "bender",
                           "melodyne", "celemony", "auto-tune", "autotune", "antares", "revoice", "vocalign",
                           "waves tune", "little alterboy", "metatune", "graillon", "nectar"})) {
        return "Pitch / Vocal";
    }
    if (containsAny(text, {"channel strip", "channelstrip", "ssl ev2", "sslchannel", "sslgchannel", "console"})) {
        return "Channel Strip";
    }
    if (containsAny(text, {"compress", "limiter", "limit", "gate", "expander", "de-esser", "dynamic", "pro-c", "pro-l", "pro-mb", "pro-g"})) {
        return "Dynamics";
    }
    if (containsAny(text, {"eq", "equal", "filter", "tilt", "shelf", "notch", "pro-q", "volcano", "simplon"})) {
        return "EQ / Filter";
    }
    if (containsAny(text, {"chorus", "flanger", "phaser", "mod", "tremolo", "vibrato", "rotary"})) {
        return "Modulation";
    }
    if (containsAny(text, {"saturat", "distort", "drive", "clip", "amp", "preamp", "exciter", "saturn", "tape", "redd", "j37", "kramer hls"})) {
        return "Saturation";
    }
    if (containsAny(text, {"synth", "piano", "organ", "drum", "instrument"})) {
        return "Instrument";
    }
    return "General";
}

std::string pluginScopeForPath(const std::string& path) {
#if defined(_WIN32)
    return pathStartsWithUserDataRoot(path) ? "User" : "System";
#else
    return pathStartsWithHome(path) ? "User" : "System";
#endif
}

bool isWavesInternalWrapperDescriptor(const Vst3PluginDescriptor& plugin) {
    const auto identity = lowerCopy(plugin.brand + " " + plugin.vendor + " " + plugin.name + " " +
                                    plugin.componentClassName);
    const auto path = lowerCopy(plugin.bundlePath + " " + plugin.executablePath);
    return (identity.find("waves") != std::string::npos ||
            path.find("waveshell") != std::string::npos) &&
        (identity.find("waveshell") != std::string::npos ||
         identity.find("immersive wrapper") != std::string::npos);
}

std::vector<std::string> wavesPluginBundleRoots() {
    std::vector<std::string> roots;
#if defined(__APPLE__)
    roots.push_back("/Applications/Waves/Plug-Ins V16");
    roots.push_back("/Applications/Waves/Plug-Ins V15");
    roots.push_back("/Applications/Waves/Plug-Ins V14");
#endif
    return roots;
}

std::string withoutWavesChannelSuffix(std::string value) {
    value = trimCopy(value);
    const std::vector<std::string> suffixes = {
        " Mono/Stereo",
        " Stereo",
        " Mono",
        " 7.1.4",
        " 7.1.2",
        " 7.1",
        " 5.1",
        " 5.0",
        " Quad",
        " LCR"
    };
    for (const auto& suffix : suffixes) {
        if (value.size() > suffix.size() &&
            value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0) {
            value.resize(value.size() - suffix.size());
            return trimCopy(value);
        }
    }
    return value;
}

std::set<std::string> wavesProductLookupBases(const std::string& productName) {
    std::set<std::string> bases;
    const auto add = [&](const std::string& value) {
        const auto normalized = lowerCopy(withoutWavesChannelSuffix(trimCopy(value)));
        if (!normalized.empty()) {
            bases.insert(normalized);
        }
    };
    add(productName);
    const auto normalized = lowerCopy(withoutWavesChannelSuffix(trimCopy(productName)));
    if (normalized.find("l2") != std::string::npos &&
        normalized.find("ultramaximizer") != std::string::npos) {
        bases.insert("l2");
    }
    return bases;
}

const Vst3PluginDescriptor* findWavesProductDescriptor(const std::vector<Vst3PluginDescriptor>& vst3Plugins,
                                                       const std::string& productName) {
    const auto normalizedProducts = wavesProductLookupBases(productName);
    if (normalizedProducts.empty()) {
        return nullptr;
    }

    const Vst3PluginDescriptor* fallback = nullptr;
    const Vst3PluginDescriptor* monoFallback = nullptr;
    for (const auto& plugin : vst3Plugins) {
        const auto pathText = lowerCopy(plugin.bundlePath + " " + plugin.executablePath);
        if (pathText.find("waveshell") == std::string::npos ||
            pathText.find("ara") != std::string::npos ||
            plugin.componentClassCid.empty()) {
            continue;
        }
        const auto className = trimCopy(plugin.componentClassName.empty() ? plugin.name : plugin.componentClassName);
        const auto classBase = lowerCopy(withoutWavesChannelSuffix(className));
        if (normalizedProducts.find(classBase) == normalizedProducts.end()) {
            continue;
        }
        const auto lowerClass = lowerCopy(className);
        if (std::any_of(normalizedProducts.begin(), normalizedProducts.end(), [&](const std::string& product) {
                return lowerClass == product + " stereo";
            })) {
            return &plugin;
        }
        if (fallback == nullptr &&
            lowerClass.find("stereo") != std::string::npos &&
            lowerClass.find("mono/stereo") == std::string::npos) {
            fallback = &plugin;
        }
        if (fallback == nullptr &&
            lowerClass.find("mono/stereo") != std::string::npos) {
            fallback = &plugin;
        }
        if (monoFallback == nullptr) {
            monoFallback = &plugin;
        }
    }
    return fallback != nullptr ? fallback : monoFallback;
}

void appendWavesProductAliases(std::vector<PluginCandidate>& candidates,
                               std::set<std::string>& seenPaths,
                               const std::vector<Vst3PluginDescriptor>& vst3Plugins) {
    std::set<std::string> seenProducts;
    for (const auto& rootString : wavesPluginBundleRoots()) {
        const std::filesystem::path root(rootString);
        std::error_code ec;
        if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
            continue;
        }
        for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
            if (ec) {
                break;
            }
            if (!entry.is_directory() || entry.path().extension() != ".bundle") {
                continue;
            }
            std::string productName = manifestNameValue(entry.path() / "Contents" / "manifest.yaml");
            if (productName.empty()) {
                productName = candidateDisplayName(entry.path());
            }
            const auto versionPos = productName.find(" 16.");
            if (versionPos != std::string::npos) {
                productName = productName.substr(0, versionPos);
            }
            productName = trimCopy(productName);
            const auto normalized = lowerCopy(productName);
            if (productName.empty() ||
                normalized.find("waveslib") != std::string::npos ||
                normalized.find("waveshell") != std::string::npos ||
                normalized.find("immersive wrapper") != std::string::npos ||
                !seenProducts.insert(normalized).second) {
                continue;
            }
            const std::string metadata = readTextFile(entry.path() / "Contents" / "Resources" / "MVS3" / "1000.xml");
            const auto* productDescriptor = findWavesProductDescriptor(vst3Plugins, productName);
            if (productDescriptor == nullptr) {
                const auto productPath = entry.path().string();
                const auto seenKey = productPath + "\n" + productName + "\nAU-only";
                if (!seenPaths.insert(seenKey).second) {
                    continue;
                }
                candidates.push_back({
                    productName,
                    productPath,
                    "AU only",
                    pluginScopeForPath(productPath),
                    "Waves",
                    inferPluginCandidateCategory(productName + " " + metadata),
                    std::filesystem::exists(productPath),
                    productName,
                    true,
                    {},
                    {}
                });
                continue;
            }
            const auto productPath = productDescriptor->bundlePath.empty() ? productDescriptor->executablePath : productDescriptor->bundlePath;
            if (productPath.empty()) {
                continue;
            }
            const auto seenKey = productPath + "\n" + productName + "\n" + productDescriptor->componentClassCid;
            if (!seenPaths.insert(seenKey).second) {
                continue;
            }
            candidates.push_back({
                productName,
                productPath,
                "VST3",
                pluginScopeForPath(productPath),
                "Waves",
                productDescriptor->category.empty()
                    ? inferPluginCandidateCategory(productName + " " + metadata)
                    : productDescriptor->category,
                std::filesystem::exists(productPath),
                productName,
                false,
                productDescriptor->componentClassCid,
                productDescriptor->componentClassName
            });
        }
    }
}

} // namespace

bool pluginCandidateMatchesFilter(const PluginCandidate& candidate, const std::string& filter) {
    if (filter.empty()) {
        return true;
    }
    const auto terms = normalizedSearchText(filter);
    const auto searchText = candidate.brand + " " + candidate.name + " " + candidate.category + " " +
        candidate.format + " " + candidate.scope + " " + candidate.pluginName + " " +
        candidate.pluginClassId + " " + candidate.pluginClassName + " " + candidate.path + " " +
        pluginSearchAliasText(candidate.brand, candidate.name, candidate.category, candidate.pluginName, candidate.pluginClassName);
    const auto haystack = normalizedSearchText(searchText);
    const auto compactHaystack = compactSearchText(searchText);
    size_t cursor = 0;
    while (cursor < terms.size()) {
        while (cursor < terms.size() && std::isspace(static_cast<unsigned char>(terms[cursor]))) {
            ++cursor;
        }
        const auto tokenStart = cursor;
        while (cursor < terms.size() && !std::isspace(static_cast<unsigned char>(terms[cursor]))) {
            ++cursor;
        }
        if (cursor <= tokenStart) {
            continue;
        }
        const auto token = terms.substr(tokenStart, cursor - tokenStart);
        const auto compactToken = compactSearchText(token);
        if (haystack.find(token) == std::string::npos &&
            (compactToken.empty() || compactHaystack.find(compactToken) == std::string::npos)) {
            return false;
        }
    }
    return true;
}

bool pluginCandidateMatchesCriteria(const PluginCandidate& candidate,
                                    const PluginCandidateFilterCriteria& criteria) {
    if (criteria.requireExisting && !candidate.exists) {
        return false;
    }
    if (!criteria.brand.empty() && candidate.brand != criteria.brand) {
        return false;
    }
    if (!criteria.category.empty() && candidate.category != criteria.category) {
        return false;
    }
    if (!criteria.excludeCategory.empty() && candidate.category == criteria.excludeCategory) {
        return false;
    }
    if (!criteria.format.empty() && candidate.format != criteria.format) {
        return false;
    }
    if (!criteria.scope.empty() && candidate.scope != criteria.scope) {
        return false;
    }
    return pluginCandidateMatchesFilter(candidate, criteria.text);
}

std::vector<PluginCandidate> filterPluginCandidates(const std::vector<PluginCandidate>& candidates,
                                                    const std::string& filter) {
    std::vector<PluginCandidate> filtered;
    for (const auto& candidate : candidates) {
        if (pluginCandidateMatchesFilter(candidate, filter)) {
            filtered.push_back(candidate);
        }
    }
    return filtered;
}

// Higher = more relevant to the search query. A NAME match beats a match found only in the brand,
// category, path or alias — so typing "emo" surfaces "EMO-Generator" at the top instead of burying
// it among the many plug-ins whose path/alias merely contains those letters.
static int pluginSearchRelevance(const PluginCandidate& candidate, const std::string& query) {
    const std::string q = lowerCopy(query);
    if (q.empty()) return 0;
    const std::string name = lowerCopy(candidate.name);
    if (name == q) return 100;
    if (name.rfind(q, 0) == 0) return 80;                          // name starts with the query
    if (name.find(q) != std::string::npos) return 60;             // name contains the query
    if (lowerCopy(candidate.brand).find(q) != std::string::npos) return 20;
    if (lowerCopy(candidate.category).find(q) != std::string::npos) return 10;
    return 0;                                                       // matched only via path / alias
}

std::vector<PluginCandidate> filterPluginCandidates(const std::vector<PluginCandidate>& candidates,
                                                    const PluginCandidateFilterCriteria& criteria) {
    std::vector<PluginCandidate> filtered;
    for (const auto& candidate : candidates) {
        if (pluginCandidateMatchesCriteria(candidate, criteria)) {
            filtered.push_back(candidate);
        }
    }
    // With a text query, rank by relevance (stable, so the display order breaks ties).
    if (!criteria.text.empty()) {
        std::stable_sort(filtered.begin(), filtered.end(),
                         [&](const PluginCandidate& a, const PluginCandidate& b) {
                             return pluginSearchRelevance(a, criteria.text) >
                                    pluginSearchRelevance(b, criteria.text);
                         });
    }
    return filtered;
}

void sortPluginCandidatesForDisplay(std::vector<PluginCandidate>& candidates) {
    std::stable_sort(candidates.begin(), candidates.end(), pluginCandidateDisplayLess);
}

PluginCandidateFilterOptions pluginCandidateFilterOptions(const std::vector<PluginCandidate>& candidates) {
    return {
        sortedUniqueNonEmptyValues(candidates, &PluginCandidate::brand),
        sortedUniqueNonEmptyValues(candidates, &PluginCandidate::category),
        sortedUniqueNonEmptyValues(candidates, &PluginCandidate::format),
        sortedUniqueNonEmptyValues(candidates, &PluginCandidate::scope)
    };
}

PluginCandidate describeInstalledPluginCandidate(const std::string& pathString,
                                                 const std::string& format) {
    const std::filesystem::path path(pathString);
    const auto name = candidateDisplayName(path);
    const auto metadata = bundleMetadataText(path);
    return {
        name,
        pathString,
        format,
        pluginScopeForPath(pathString),
        inferPluginCandidateBrand(name, pathString, metadata),
        inferPluginCandidateCategory(name + " " + metadata),
        std::filesystem::exists(path),
        {},
        false,
        {},
        {}
    };
}

std::vector<PluginCandidate> scanKnownPluginLocations(bool forceRescan) {
    std::vector<PluginCandidate> candidates;
    std::set<std::string> seenPaths;
    const auto vst3Plugins = scanVst3PluginBundles(
        forceRescan ? Vst3ScanMode::Refresh : Vst3ScanMode::UseCache);

    for (const auto& plugin : vst3Plugins) {
        if (isWavesInternalWrapperDescriptor(plugin)) {
            continue;
        }
        const auto path = plugin.bundlePath.empty() ? plugin.executablePath : plugin.bundlePath;
        const auto seenKey = path + "\n" + plugin.name;
        if (path.empty() || !seenPaths.insert(seenKey).second) {
            continue;
        }
        // A plug-in's own declared brand/category is used when it's meaningful, but many declare
        // nothing useful — Melodyne shows up as brand "Unknown", category "Utility". When the
        // declared value is that weak, infer from the name/vendor so the browser's Brand and
        // Category facets actually group it (Melodyne → Celemony, Pitch / Vocal).
        const auto isWeak = [](const std::string& value) {
            const auto v = lowerCopy(value);
            return v.empty() || v == "unknown" || v == "utility" || v == "general" ||
                   v == "other" || v == "fx" || v == "effect" || v == "-";
        };
        const std::string inferMetadata = plugin.name + " " + plugin.vendor + " " + plugin.componentClassName;
        const std::string brand = isWeak(plugin.brand)
            ? inferPluginCandidateBrand(plugin.name, path, inferMetadata) : plugin.brand;
        const std::string category = isWeak(plugin.category)
            ? inferPluginCandidateCategory(inferMetadata) : plugin.category;
        // ARA capability for the browser badge, WITHOUT executing plug-in code. Opening every one of
        // the ~900 installed VST3s during a scan is both slow and unsafe — several vendors crash when
        // loaded in-process, which is why isKnownUnsafeForInProcessVst3Host exists. So the scan reads
        // moduleinfo.json when the plug-in ships one, and otherwise falls back to the known-name list.
        // The authoritative factory probe (vst3AdvertisesAraFactory) runs later, for the single
        // plug-in the user actually tries to insert.
        const bool araCapable = vst3ModuleInfoMentionsAra(plugin) || pluginNameLooksLikeAra(plugin.name);
        candidates.push_back({
            plugin.name,
            path,
            "VST3",
            pluginScopeForPath(path),
            brand,
            category,
            plugin.loadableBundle || std::filesystem::exists(path),
            plugin.name,
            false,
            plugin.componentClassCid,
            plugin.componentClassName,
            araCapable
        });
    }
    appendWavesProductAliases(candidates, seenPaths, vst3Plugins);
    sortPluginCandidatesForDisplay(candidates);
    return candidates;
}

} // namespace neuracoust::daw
