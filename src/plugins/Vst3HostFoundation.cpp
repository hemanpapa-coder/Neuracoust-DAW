#include "plugins/Vst3HostFoundation.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>

#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <iomanip>
#endif

#if defined(NEURACOUST_HAS_VST3_SDK)
#include "pluginterfaces/base/ipluginbase.h"
#endif

namespace neuracoust::daw {

namespace {

constexpr const char* kPersistentVst3InventoryFileName = "vst3_inventory_v10.tsv";
constexpr const char* kPersistentVst3InventoryHeader = "Neuracoust DAW VST3 Inventory v10";

void appendVst3Root(std::vector<std::filesystem::path>& roots,
                    std::set<std::string>& seen,
                    const std::filesystem::path& root) {
    if (root.empty()) {
        return;
    }
    const auto key = root.lexically_normal().string();
    if (seen.insert(key).second) {
        roots.push_back(root);
    }
}

void appendVst3RootsFromPathList(std::vector<std::filesystem::path>& roots,
                                 std::set<std::string>& seen,
                                 const char* pathList) {
    if (pathList == nullptr || *pathList == '\0') {
        return;
    }
#if defined(_WIN32)
    constexpr char separator = ';';
#else
    constexpr char separator = ':';
#endif
    std::string paths(pathList);
    size_t start = 0;
    while (start <= paths.size()) {
        const auto end = paths.find(separator, start);
        const auto token = paths.substr(start, end == std::string::npos ? std::string::npos : end - start);
        appendVst3Root(roots, seen, std::filesystem::path(token));
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
}

std::vector<std::filesystem::path> vst3Roots() {
    std::vector<std::filesystem::path> roots;
    std::set<std::string> seen;
    appendVst3RootsFromPathList(roots, seen, std::getenv("NEURACOUST_DAW_VST3_PATHS"));
#if defined(_WIN32)
    if (const char* programFiles = std::getenv("ProgramFiles")) {
        appendVst3Root(roots, seen, std::filesystem::path(programFiles) / "Common Files" / "VST3");
    }
    if (const char* programFilesX86 = std::getenv("ProgramFiles(x86)")) {
        appendVst3Root(roots, seen, std::filesystem::path(programFilesX86) / "Common Files" / "VST3");
    }
    if (const char* commonProgramFiles = std::getenv("CommonProgramFiles")) {
        appendVst3Root(roots, seen, std::filesystem::path(commonProgramFiles) / "VST3");
    }
    if (const char* appData = std::getenv("APPDATA")) {
        appendVst3Root(roots, seen, std::filesystem::path(appData) / "VST3");
    }
    if (const char* localAppData = std::getenv("LOCALAPPDATA")) {
        appendVst3Root(roots, seen, std::filesystem::path(localAppData) / "Programs" / "Common" / "VST3");
    }
    return roots;
#elif defined(__linux__)
    appendVst3Root(roots, seen, "/usr/lib/vst3");
    appendVst3Root(roots, seen, "/usr/local/lib/vst3");
    if (const char* home = std::getenv("HOME")) {
        appendVst3Root(roots, seen, std::filesystem::path(home) / ".vst3");
    }
    return roots;
#else
    appendVst3Root(roots, seen, "/Library/Audio/Plug-Ins/VST3");
    if (const char* home = std::getenv("HOME")) {
        appendVst3Root(roots, seen, std::filesystem::path(home) / "Library/Audio/Plug-Ins/VST3");
    }
    return roots;
#endif
}

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::filesystem::path persistentScanCachePath() {
    if (const char* overridePath = std::getenv("NEURACOUST_DAW_VST3_CACHE_PATH")) {
        if (*overridePath != '\0') {
            return overridePath;
        }
    }
#if defined(_WIN32)
    if (const char* localAppData = std::getenv("LOCALAPPDATA")) {
        return std::filesystem::path(localAppData) / "Neuracoust" / "Neuracoust DAW" / kPersistentVst3InventoryFileName;
    }
    if (const char* appData = std::getenv("APPDATA")) {
        return std::filesystem::path(appData) / "Neuracoust" / "Neuracoust DAW" / kPersistentVst3InventoryFileName;
    }
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path(home) / "Library" / "Caches" / "Neuracoust" / "Neuracoust DAW" / kPersistentVst3InventoryFileName;
    }
#else
    if (const char* xdgCacheHome = std::getenv("XDG_CACHE_HOME")) {
        return std::filesystem::path(xdgCacheHome) / "Neuracoust" / "Neuracoust DAW" / kPersistentVst3InventoryFileName;
    }
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path(home) / ".cache" / "Neuracoust" / "Neuracoust DAW" / kPersistentVst3InventoryFileName;
    }
#endif
    return {};
}

void removeLegacyPersistentScanCaches(const std::filesystem::path& activePath) {
    if (activePath.empty()) {
        return;
    }
    const auto directory = activePath.parent_path();
    if (directory.empty()) {
        return;
    }
    const std::filesystem::path legacyFiles[] = {
        directory / "vst3_inventory_v1.tsv",
        directory / "vst3_inventory_v2.tsv",
        directory / "vst3_inventory_v3.tsv",
        directory / "vst3_inventory_v4.tsv",
        directory / "vst3_inventory_v5.tsv",
        directory / "vst3_inventory_v6.tsv",
        directory / "vst3_inventory_v7.tsv",
        directory / "vst3_inventory_v8.tsv",
        directory / "vst3_inventory_v9.tsv"
    };
    std::error_code error;
    for (const auto& legacy : legacyFiles) {
        if (legacy != activePath) {
            std::filesystem::remove(legacy, error);
            error.clear();
        }
    }
}

std::string escapeCacheField(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char ch : value) {
        switch (ch) {
            case '\\':
                escaped += "\\\\";
                break;
            case '\t':
                escaped += "\\t";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            default:
                escaped += ch;
                break;
        }
    }
    return escaped;
}

std::string unescapeCacheField(const std::string& value) {
    std::string unescaped;
    unescaped.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '\\' || i + 1 >= value.size()) {
            unescaped += value[i];
            continue;
        }
        const char code = value[++i];
        switch (code) {
            case '\\':
                unescaped += '\\';
                break;
            case 't':
                unescaped += '\t';
                break;
            case 'n':
                unescaped += '\n';
                break;
            case 'r':
                unescaped += '\r';
                break;
            default:
                unescaped += code;
                break;
        }
    }
    return unescaped;
}

std::vector<std::string> splitTabLine(const std::string& line) {
    std::vector<std::string> fields;
    size_t start = 0;
    while (start <= line.size()) {
        const auto tab = line.find('\t', start);
        if (tab == std::string::npos) {
            fields.push_back(unescapeCacheField(line.substr(start)));
            break;
        }
        fields.push_back(unescapeCacheField(line.substr(start, tab - start)));
        start = tab + 1;
    }
    return fields;
}

bool descriptorStillExists(const Vst3PluginDescriptor& descriptor) {
    if (!descriptor.bundlePath.empty() && std::filesystem::exists(descriptor.bundlePath)) {
        return true;
    }
    if (!descriptor.executablePath.empty() && std::filesystem::exists(descriptor.executablePath)) {
        return true;
    }
    return false;
}

bool isGenericWaveShellDescriptor(const Vst3PluginDescriptor& descriptor) {
    auto lower = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    };
    const auto searchable = lower(descriptor.name + " " + descriptor.bundlePath + " " + descriptor.componentClassName);
    if (searchable.find("waveshell") == std::string::npos) {
        return false;
    }
    return descriptor.componentClassName.empty() || lower(descriptor.componentClassName).find("waveshell") != std::string::npos;
}

bool isWavesDescriptorNameOrPath(const std::string& pluginName, const std::string& pluginPath) {
    std::string searchable = pluginName + " " + pluginPath;
    std::transform(searchable.begin(), searchable.end(), searchable.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return searchable.find("waves") != std::string::npos ||
        searchable.find("waveshell") != std::string::npos;
}

std::string trimCopy(std::string value);
std::string lowerCopy(std::string value);
bool containsAny(const std::string& haystack, std::initializer_list<const char*> needles);

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

bool wavesDescriptorMatchesRequestedProduct(const Vst3PluginDescriptor& descriptor,
                                            const std::string& requestedProductName) {
    if (requestedProductName.empty()) {
        return true;
    }
    const auto bases = wavesProductLookupBases(requestedProductName);
    if (bases.empty()) {
        return true;
    }
    const auto className = trimCopy(descriptor.componentClassName.empty() ? descriptor.name : descriptor.componentClassName);
    const auto classBase = lowerCopy(withoutWavesChannelSuffix(className));
    return bases.find(classBase) != bases.end();
}

std::optional<std::vector<Vst3PluginDescriptor>> loadPersistentScanCache() {
    const auto path = persistentScanCachePath();
    if (path.empty()) {
        return std::nullopt;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }

    std::string line;
    if (!std::getline(in, line) || line != kPersistentVst3InventoryHeader) {
        return std::nullopt;
    }

    std::vector<Vst3PluginDescriptor> descriptors;
    while (std::getline(in, line)) {
        const auto fields = splitTabLine(line);
        if (fields.size() != 12) {
            continue;
        }
        Vst3PluginDescriptor descriptor;
        descriptor.name = fields[0];
        descriptor.brand = fields[1];
        descriptor.category = fields[2];
        descriptor.bundlePath = fields[3];
        descriptor.executablePath = fields[4];
        descriptor.moduleInfoPath = fields[5];
        descriptor.vendor = fields[6];
        descriptor.componentClassCid = fields[7];
        descriptor.componentClassName = fields[8];
        descriptor.loadableBundle = fields[9] == "1";
        descriptor.classCount = std::atoi(fields[10].c_str());
        descriptor.audioClassCount = std::atoi(fields[11].c_str());
        if (descriptorStillExists(descriptor)) {
            descriptors.push_back(std::move(descriptor));
        }
    }
    if (std::any_of(descriptors.begin(), descriptors.end(), isGenericWaveShellDescriptor)) {
        return std::nullopt;
    }
    sortVst3PluginDescriptorsForDisplay(descriptors);
    return descriptors;
}

void savePersistentScanCache(const std::vector<Vst3PluginDescriptor>& descriptors) {
    const auto path = persistentScanCachePath();
    if (path.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return;
    }
    removeLegacyPersistentScanCaches(path);

    const auto tempPath = path.string() + ".tmp";
    std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        return;
    }
    out << kPersistentVst3InventoryHeader << '\n';
    for (const auto& descriptor : descriptors) {
        out << escapeCacheField(descriptor.name) << '\t'
            << escapeCacheField(descriptor.brand) << '\t'
            << escapeCacheField(descriptor.category) << '\t'
            << escapeCacheField(descriptor.bundlePath) << '\t'
            << escapeCacheField(descriptor.executablePath) << '\t'
            << escapeCacheField(descriptor.moduleInfoPath) << '\t'
            << escapeCacheField(descriptor.vendor) << '\t'
            << escapeCacheField(descriptor.componentClassCid) << '\t'
            << escapeCacheField(descriptor.componentClassName) << '\t'
            << (descriptor.loadableBundle ? "1" : "0") << '\t'
            << descriptor.classCount << '\t'
            << descriptor.audioClassCount << '\n';
    }
    out.close();
    if (!out) {
        std::filesystem::remove(tempPath, error);
        return;
    }
    std::filesystem::rename(tempPath, path, error);
    if (error) {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(tempPath, path, error);
    }
    if (error) {
        std::filesystem::remove(tempPath, error);
    }
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

std::string valueAfterQuotedKey(const std::string& text, const std::string& key, size_t start = 0) {
    const auto keyMarker = "\"" + key + "\"";
    const auto keyPos = text.find(keyMarker, start);
    if (keyPos == std::string::npos) {
        return {};
    }
    const auto colon = text.find(':', keyPos + keyMarker.size());
    if (colon == std::string::npos) {
        return {};
    }
    const auto firstQuote = text.find('"', colon + 1);
    if (firstQuote == std::string::npos) {
        return {};
    }
    const auto secondQuote = text.find('"', firstQuote + 1);
    if (secondQuote == std::string::npos) {
        return {};
    }
    return text.substr(firstQuote + 1, secondQuote - firstQuote - 1);
}

std::vector<std::string> stringArrayAfterQuotedKey(const std::string& text, const std::string& key, size_t start = 0) {
    std::vector<std::string> values;
    const auto keyMarker = "\"" + key + "\"";
    const auto keyPos = text.find(keyMarker, start);
    if (keyPos == std::string::npos) {
        return values;
    }
    const auto open = text.find('[', keyPos + keyMarker.size());
    const auto close = text.find(']', open);
    if (open == std::string::npos || close == std::string::npos) {
        return values;
    }
    size_t cursor = open + 1;
    while (cursor < close) {
        const auto firstQuote = text.find('"', cursor);
        if (firstQuote == std::string::npos || firstQuote >= close) {
            break;
        }
        const auto secondQuote = text.find('"', firstQuote + 1);
        if (secondQuote == std::string::npos || secondQuote > close) {
            break;
        }
        values.push_back(text.substr(firstQuote + 1, secondQuote - firstQuote - 1));
        cursor = secondQuote + 1;
    }
    return values;
}

std::string factoryVendorFromModuleInfo(const std::string& text) {
    const auto factoryPos = text.find("\"Factory Info\"");
    return factoryPos == std::string::npos ? std::string{} : valueAfterQuotedKey(text, "Vendor", factoryPos);
}

std::string lowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string trimCopy(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

std::vector<std::string> splitCategoryPath(const std::string& text, char delimiter) {
    std::vector<std::string> parts;
    std::string current;
    std::istringstream stream(text);
    while (std::getline(stream, current, delimiter)) {
        current = trimCopy(current);
        if (!current.empty()) {
            parts.push_back(current);
        }
    }
    return parts;
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
    if (lower.find("reverb") != std::string::npos) return " verb room hall plate chamber ambience space 리버브 공간 홀 플레이트";
    if (lower.find("delay") != std::string::npos) return " echo slap repeat tape 딜레이 에코";
    if (lower.find("dynamics") != std::string::npos) return " compressor comp limiter limit gate expander deesser de-esser dynamic dynamics 컴프 컴프레서 리미터 게이트 다이나믹";
    if (lower.find("eq") != std::string::npos || lower.find("filter") != std::string::npos) return " equalizer filter eq dynamic-eq 이큐 이퀄라이저 필터";
    if (lower.find("saturation") != std::string::npos) return " distortion saturator drive clip tape tube amp exciter 새츄레이션 디스토션 드라이브 테이프";
    if (lower.find("modulation") != std::string::npos) return " chorus flanger phaser tremolo vibrato doubler modulation 모듈레이션 코러스 플랜저 페이저";
    if (lower.find("pitch") != std::string::npos || lower.find("vocal") != std::string::npos) return " pitch tune vocal voice doubler adt 피치 보컬 튠";
    if (lower.find("mastering") != std::string::npos) return " master mastering limiter loudness meter 마스터 마스터링 라우드니스";
    if (lower.find("spatial") != std::string::npos) return " stereo image imager surround room binaural spatial 스테레오 이미저 공간 서라운드";
    if (lower.find("analyzer") != std::string::npos) return " meter metering analyzer analysis loudness scope 미터 분석";
    if (lower.find("instrument") != std::string::npos) return " synth sampler piano drum bass instrument 악기 신스 샘플러";
    if (lower.find("guitar") != std::string::npos || lower.find("amp") != std::string::npos) return " guitar amp stomp pedal cabinet 기타 앰프 페달";
    if (lower.find("restoration") != std::string::npos || lower.find("noise") != std::string::npos) return " repair restore noise denoise declick declip cleanup 노이즈 복원 수리";
    if (lower.find("generator") != std::string::npos) return " generator tone synth sub bass signal 생성기 시그널 서브";
    return {};
}

std::string pluginCategoryOverride(const std::string& name) {
    const auto text = lowerCopy(name);
    if (containsAny(text, {"neuracoust mirage", "mirage 4", "mirage 8", "mirage 70", "mirage 901", "mirage 991"})) return "Reverb";
    if (containsAny(text, {"fabfilter twin", "fabfilter one"})) return "Instrument";
    if (containsAny(text, {"fabfilter pro-c", "fabfilter pro-mb", "fabfilter pro-ds", "fabfilter pro-g", "fabfilter pro-l"})) return "Dynamics";
    if (containsAny(text, {"fabfilter pro-r"})) return "Reverb";
    if (containsAny(text, {"fabfilter timeless"})) return "Delay";
    if (containsAny(text, {"fabfilter saturn"})) return "Saturation";
    if (containsAny(text, {"fabfilter pro-q", "fabfilter volcano", "fabfilter simplon", "fabfilter micro"})) return "EQ / Filter";
    if (containsAny(text, {"ozone 12", "ozone "})) {
        if (containsAny(text, {"maximizer", "dynamics", "compressor", "limiter", "unlimiter", "impact"})) return "Dynamics";
        if (containsAny(text, {"equalizer", "dynamic eq", "match eq", "stem eq", "vintage eq", "stabilizer"})) return "EQ / Filter";
        if (containsAny(text, {"exciter", "vintage tape"})) return "Saturation";
        if (containsAny(text, {"imager"})) return "Spatial";
        if (containsAny(text, {"clarity", "spectral shaper"})) return "Restoration";
        return "Mastering";
    }
    if (containsAny(text, {"rx 12 de-click", "rx 12 de-clip", "rx 12 de-crackle", "rx 12 de-hum", "rx 12 de-plosive",
                           "rx 12 de-bleed", "rx 12 de-ess", "rx 12 spectral de-noise", "rx 12 voice de-noise",
                           "rx 12 guitar de-noise", "rx 12 dialogue isolate", "rx 12 mouth de-click",
                           "rx 12 repair assistant", "rx 12 breath control"})) return "Restoration";
    if (containsAny(text, {"bass rider", "vocal rider"})) return "Dynamics";
    if (containsAny(text, {"cla effects", "cla guitars", "cla unplugged", "ekramer ba", "ekramer dr"})) return "Artist / Multi FX";
    if (containsAny(text, {"cla epic", "echosphere"})) return "Delay";
    if (containsAny(text, {"h-reverb", "h reverb", "hverb", "oneknob wetter", "one knob wetter"})) return "Reverb";
    if (containsAny(text, {"loair", "lo air", "submarine", "sub align"})) return "Generator";
    if (containsAny(text, {"oneknob brighter", "one knob brighter"})) return "EQ / Filter";
    if (containsAny(text, {"oneknob louder", "one knob louder", "oneknob pressure", "one knob pressure"})) return "Dynamics";
    if (containsAny(text, {"oneknob phatter", "one knob phatter", "retro fi", "gtr stomp", "gtr tool rack", "gtr solo tool rack",
                           "prs archon", "prs dallas", "prs v9", "ekramer gt"})) return "Guitar / Amp";
    if (containsAny(text, {"ekramer fx", "jjp-guitars", "jjp guitars", "jjp-strings-keys", "jjp strings keys",
                           "maserati acg", "maserati gti", "maserati hmx", "studiovers audio effects", "studioverse audio effects",
                           "waves gemstones"})) return "Artist / Multi FX";
    if (containsAny(text, {"inphase", "in phase"})) return "Phase / Alignment";
    if (containsAny(text, {"key detector"})) return "Analyzer";
    if (containsAny(text, {"feedback hunter", "x-fdbk", "x fdbk"})) return "Restoration";
    if (containsAny(text, {"dorrough"})) return text.find("360") != std::string::npos ? "Spatial" : "Analyzer";
    return {};
}

std::string pluginSearchAliasText(const std::string& brand,
                                  const std::string& name,
                                  const std::string& category,
                                  const std::string& vendor,
                                  const std::string& componentClassName) {
    const std::string identity = brand + " " + vendor + " " + name + " " + componentClassName;
    const std::string categoryOverride = pluginCategoryOverride(identity);
    std::string aliases = " " + wordInitialsSearchText(identity) + " " +
        compactSearchText(identity) + " " +
        categorySearchAliases(category) + " " +
        categorySearchAliases(categoryOverride);
    const auto lower = lowerCopy(identity);
    if (lower.find("fabfilter") != std::string::npos) aliases += " ff fab fabfilter 패브필터 페브필터";
    if (lower.find("waves") != std::string::npos || lower.find("waveshell") != std::string::npos) aliases += " waves wave 웨이브스";
    if (lower.find("izotope") != std::string::npos || lower.find("ozone") != std::string::npos || lower.find("rx 12") != std::string::npos) aliases += " izotope ozone rx 아이조톱 아이조토프 오존";
    if (lower.find("neuracoust") != std::string::npos || lower.find("newacoust") != std::string::npos) aliases += " neuracoust newacoust 누라쿠스트 뉴라쿠스트 뉴어쿠스트";
    return aliases;
}

std::string asciiFromHexClassId(const std::string& cid) {
    std::string hex;
    for (unsigned char ch : cid) {
        if (std::isxdigit(ch)) {
            hex.push_back(static_cast<char>(ch));
        }
    }
    if (hex.size() % 2 != 0) {
        return {};
    }
    std::string ascii;
    ascii.reserve(hex.size() / 2);
    for (size_t index = 0; index + 1 < hex.size(); index += 2) {
        const std::string byteText = hex.substr(index, 2);
        char* end = nullptr;
        const long value = std::strtol(byteText.c_str(), &end, 16);
        if (end == byteText.c_str() || *end != '\0' || value < 0 || value > 255) {
            return {};
        }
        if (value >= 32 && value <= 126) {
            ascii.push_back(static_cast<char>(value));
        }
    }
    return ascii;
}

std::string wavesShellDisplayNameFromClass(const Vst3ClassDescriptor& klass) {
    const auto lowerName = lowerCopy(klass.name);
    if (lowerName.find("waveshell") == std::string::npos) {
        return klass.name;
    }
    std::string ascii = asciiFromHexClassId(klass.cid);
    if (ascii.rfind("VST", 0) == 0) {
        ascii = ascii.substr(3);
    }
    const auto immersivePos = ascii.find("Immersive");
    if (immersivePos == std::string::npos) {
        return klass.name;
    }
    const auto code = ascii.substr(0, immersivePos);
    std::string suffix = code;
    if (code == "100") {
        suffix = "Mono";
    } else if (code == "200") {
        suffix = "Stereo";
    } else if (code == "QUAD") {
        suffix = "Quad";
    } else if (code == "LCR") {
        suffix = "LCR";
    } else if (code.size() == 3 && std::isdigit(static_cast<unsigned char>(code[0])) &&
               std::isdigit(static_cast<unsigned char>(code[1])) &&
               std::isdigit(static_cast<unsigned char>(code[2]))) {
        suffix = std::string(1, code[0]) + "." + code[1];
        if (code[2] != '0') {
            suffix += "." + std::string(1, code[2]);
        }
    }
    return suffix.empty() ? "Immersive Wrapper" : "Immersive Wrapper " + suffix;
}

std::vector<int> wavesShellVersionParts(const std::filesystem::path& path) {
    const std::string filename = path.filename().string();
    const std::string lower = lowerCopy(filename);
    const auto marker = lower.find("waveshell1-vst3 ");
    if (marker == std::string::npos) {
        return {};
    }
    const auto versionStart = marker + std::string("waveshell1-vst3 ").size();
    std::vector<int> parts;
    size_t cursor = versionStart;
    while (cursor < filename.size()) {
        while (cursor < filename.size() && !std::isdigit(static_cast<unsigned char>(filename[cursor]))) {
            if (filename[cursor] == '.') {
                ++cursor;
                continue;
            }
            return parts;
        }
        if (cursor >= filename.size()) {
            break;
        }
        size_t end = cursor;
        while (end < filename.size() && std::isdigit(static_cast<unsigned char>(filename[end]))) {
            ++end;
        }
        parts.push_back(std::atoi(filename.substr(cursor, end - cursor).c_str()));
        cursor = end;
        if (cursor >= filename.size() || filename[cursor] != '.') {
            break;
        }
        ++cursor;
    }
    return parts;
}

bool wavesShellVersionLess(const std::filesystem::path& lhs,
                           const std::filesystem::path& rhs) {
    const auto lhsVersion = wavesShellVersionParts(lhs);
    const auto rhsVersion = wavesShellVersionParts(rhs);
    const size_t count = std::max(lhsVersion.size(), rhsVersion.size());
    for (size_t index = 0; index < count; ++index) {
        const int left = index < lhsVersion.size() ? lhsVersion[index] : 0;
        const int right = index < rhsVersion.size() ? rhsVersion[index] : 0;
        if (left != right) {
            return left < right;
        }
    }
    return lowerCopy(lhs.filename().string()) < lowerCopy(rhs.filename().string());
}

bool searchTextContainsAllTokens(const std::string& haystack, const std::string& filter) {
    const auto normalizedHaystack = normalizedSearchText(haystack);
    const auto normalizedFilter = normalizedSearchText(filter);
    const auto compactHaystack = compactSearchText(haystack);
    size_t cursor = 0;
    while (cursor < normalizedFilter.size()) {
        while (cursor < normalizedFilter.size() && std::isspace(static_cast<unsigned char>(normalizedFilter[cursor]))) {
            ++cursor;
        }
        const auto tokenStart = cursor;
        while (cursor < normalizedFilter.size() && !std::isspace(static_cast<unsigned char>(normalizedFilter[cursor]))) {
            ++cursor;
        }
        if (cursor <= tokenStart) {
            continue;
        }
        const auto token = normalizedFilter.substr(tokenStart, cursor - tokenStart);
        const auto compactToken = compactSearchText(token);
        if (normalizedHaystack.find(token) == std::string::npos &&
            (compactToken.empty() || compactHaystack.find(compactToken) == std::string::npos)) {
            return false;
        }
    }
    return true;
}

bool vst3DisplayLess(const Vst3PluginDescriptor& lhs, const Vst3PluginDescriptor& rhs) {
    const auto lhsKey = lowerCopy(lhs.brand + "\n" + lhs.name + "\n" + lhs.category + "\n" +
                                  lhs.bundlePath + "\n" + lhs.executablePath + "\n" + lhs.componentClassName);
    const auto rhsKey = lowerCopy(rhs.brand + "\n" + rhs.name + "\n" + rhs.category + "\n" +
                                  rhs.bundlePath + "\n" + rhs.executablePath + "\n" + rhs.componentClassName);
    return lhsKey < rhsKey;
}

bool containsAny(const std::string& haystack, std::initializer_list<const char*> needles) {
    for (const auto* needle : needles) {
        if (haystack.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string inferPluginBrand(const std::string& name, const std::string& vendor) {
    const auto combined = lowerCopy(vendor + " " + name);
    if (containsAny(combined, {"neuracoust", "newacoust"})) {
        return "Neuracoust";
    }
    if (combined.find("fabfilter") != std::string::npos) {
        return "FabFilter";
    }
    if (containsAny(combined, {"izotope", "ozone", "rx "})) {
        return "iZotope";
    }
    if (containsAny(combined, {"waves", "waveshell"})) {
        return "Waves";
    }
    if (combined.find("apple") != std::string::npos) {
        return "Apple";
    }
    if (containsAny(combined, {"mastering the mix", "animate", "bassroom", "fuser", "mixroom"})) {
        return "Mastering The Mix";
    }
    return vendor.empty() ? "Unknown" : vendor;
}

std::string inferPluginCategory(const std::string& name, const std::vector<Vst3ClassDescriptor>& classes) {
    std::string combinedForOverride = name;
    for (const auto& klass : classes) {
        combinedForOverride += " " + klass.name;
        for (const auto& subCategory : klass.subCategories) {
            combinedForOverride += " " + subCategory;
        }
    }
    if (const auto overrideCategory = pluginCategoryOverride(combinedForOverride); !overrideCategory.empty()) {
        return overrideCategory;
    }
    for (const auto& klass : classes) {
        for (const auto& subCategory : klass.subCategories) {
            const auto sub = lowerCopy(subCategory);
            if (sub.find("mastering") != std::string::npos) {
                return "Mastering";
            }
            if (sub.find("dynamics") != std::string::npos) {
                return "Dynamics";
            }
            if (sub.find("channel strip") != std::string::npos) {
                return "Channel Strip";
            }
            if (sub.find("eq") != std::string::npos ||
                sub.find("filter") != std::string::npos) {
                return "EQ / Filter";
            }
            if (sub.find("reverb") != std::string::npos) {
                return "Reverb";
            }
            if (sub.find("delay") != std::string::npos) {
                return "Delay";
            }
            if (sub.find("modulation") != std::string::npos) {
                return "Modulation";
            }
            if (sub.find("pitch shift") != std::string::npos ||
                sub.find("vocals") != std::string::npos) {
                return "Pitch / Vocal";
            }
            if (sub.find("restoration") != std::string::npos) {
                return "Restoration";
            }
            if (sub.find("distortion") != std::string::npos) {
                return "Saturation";
            }
            if (sub.find("generator") != std::string::npos) {
                return "Generator";
            }
            if (sub.find("instrument") != std::string::npos ||
                sub.find("synth") != std::string::npos ||
                sub.find("sampler") != std::string::npos ||
                sub.find("piano") != std::string::npos ||
                sub.find("drum") != std::string::npos) {
                return "Instrument";
            }
            if (sub.find("spatial") != std::string::npos ||
                sub.find("surround") != std::string::npos ||
                sub.find("ambisonics") != std::string::npos) {
                return "Spatial";
            }
            if (sub.find("analyzer") != std::string::npos) {
                return "Analyzer";
            }
            if (sub.find("tools") != std::string::npos) {
                return "Utility";
            }
        }
    }
    std::string combined = lowerCopy(name);
    for (const auto& klass : classes) {
        combined += " " + lowerCopy(klass.category + " " + klass.name);
        for (const auto& subCategory : klass.subCategories) {
            combined += " " + lowerCopy(subCategory);
        }
    }
    if (containsAny(combined, {"fabfilter twin", "fabfilter one"})) {
        return "Instrument";
    }
    if (containsAny(combined, {"reverb", "room", "plate", "chamber", "space"})) {
        return "Reverb";
    }
    if (containsAny(combined, {"delay", "echo", "tape"})) {
        return "Delay";
    }
    if (containsAny(combined, {"compress", "limiter", "gate", "expander", "de-esser", "dynamics"})) {
        return "Dynamics";
    }
    if (containsAny(combined, {"eq", "equalizer", "filter", "geq", "channel strip"})) {
        return "EQ / Filter";
    }
    if (containsAny(combined, {"saturat", "distortion", "drive", "exciter", "amp", "preamp", "tube", "vinyl"})) {
        return "Saturation";
    }
    if (containsAny(combined, {"chorus", "flanger", "phaser", "modulation", "doubler", "pitch", "vocal bender"})) {
        return "Modulation";
    }
    if (containsAny(combined, {"instrument", "synth", "sampler", "drum", "piano", "bass", "clavinet"})) {
        return "Instrument";
    }
    if (containsAny(combined, {"noise", "denoise", "de-noise", "declick", "dereverb", "restoration", "clarity"})) {
        return "Restoration";
    }
    return "Utility";
}

std::vector<Vst3ClassDescriptor> parseModuleInfoClasses(const std::string& text) {
    std::vector<Vst3ClassDescriptor> classes;
    const auto classesPos = text.find("\"Classes\"");
    if (classesPos == std::string::npos) {
        return classes;
    }
    size_t cursor = text.find('{', classesPos);
    while (cursor != std::string::npos) {
        const auto nextClassPos = text.find("\"CID\"", cursor);
        if (nextClassPos == std::string::npos) {
            break;
        }
        const auto objectEnd = text.find("\n    }", nextClassPos);
        if (objectEnd == std::string::npos) {
            break;
        }
        Vst3ClassDescriptor descriptor;
        descriptor.cid = valueAfterQuotedKey(text, "CID", nextClassPos);
        descriptor.category = valueAfterQuotedKey(text, "Category", nextClassPos);
        descriptor.name = valueAfterQuotedKey(text, "Name", nextClassPos);
        descriptor.vendor = valueAfterQuotedKey(text, "Vendor", nextClassPos);
        descriptor.version = valueAfterQuotedKey(text, "Version", nextClassPos);
        descriptor.subCategories = stringArrayAfterQuotedKey(text, "Sub Categories", nextClassPos);
        if (!descriptor.cid.empty()) {
            classes.push_back(std::move(descriptor));
        }
        cursor = text.find('{', objectEnd + 1);
    }
    return classes;
}

#if defined(__APPLE__) && defined(NEURACOUST_HAS_VST3_SDK)
std::string tuidToHex(const Steinberg::TUID cid) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (int index = 0; index < 16; ++index) {
        out << std::setw(2) << (static_cast<int>(cid[index]) & 0xff);
    }
    return out.str();
}

class ScopedCurrentDirectory {
public:
    explicit ScopedCurrentDirectory(const std::filesystem::path& next) {
        std::error_code ec;
        previous_ = std::filesystem::current_path(ec);
        if (!ec && !next.empty() && std::filesystem::exists(next, ec)) {
            std::filesystem::current_path(next, ec);
            changed_ = !ec;
        }
    }

    ~ScopedCurrentDirectory() {
        if (changed_) {
            std::error_code ec;
            std::filesystem::current_path(previous_, ec);
        }
    }

private:
    std::filesystem::path previous_;
    bool changed_ = false;
};

std::vector<Vst3ClassDescriptor> readMacVst3FactoryClasses(const Vst3PluginDescriptor& descriptor) {
    std::vector<Vst3ClassDescriptor> classes;
    if (descriptor.bundlePath.empty()) {
        return classes;
    }
    ScopedCurrentDirectory cwd(std::filesystem::path(descriptor.bundlePath) / "Contents" / "Resources");
    CFStringRef bundlePath = CFStringCreateWithCString(kCFAllocatorDefault,
                                                       descriptor.bundlePath.c_str(),
                                                       kCFStringEncodingUTF8);
    if (bundlePath == nullptr) {
        return classes;
    }
    CFURLRef bundleUrl = CFURLCreateWithFileSystemPath(kCFAllocatorDefault,
                                                       bundlePath,
                                                       kCFURLPOSIXPathStyle,
                                                       true);
    CFRelease(bundlePath);
    if (bundleUrl == nullptr) {
        return classes;
    }
    CFBundleRef bundle = CFBundleCreate(kCFAllocatorDefault, bundleUrl);
    CFRelease(bundleUrl);
    if (bundle == nullptr) {
        return classes;
    }
    bool bundleEntryCalled = false;
    using BundleEntryFn = bool (*)(CFBundleRef);
    using BundleExitFn = bool (*)();
    using GetPluginFactoryFn = Steinberg::IPluginFactory* (*)();
    if (CFBundleLoadExecutable(bundle)) {
        auto* bundleEntry = reinterpret_cast<BundleEntryFn>(CFBundleGetFunctionPointerForName(bundle, CFSTR("bundleEntry")));
        auto* bundleExit = reinterpret_cast<BundleExitFn>(CFBundleGetFunctionPointerForName(bundle, CFSTR("bundleExit")));
        if (bundleEntry == nullptr || bundleEntry(bundle)) {
            bundleEntryCalled = bundleEntry != nullptr;
            auto* factoryFn = reinterpret_cast<GetPluginFactoryFn>(CFBundleGetFunctionPointerForName(bundle, CFSTR("GetPluginFactory")));
            Steinberg::IPluginFactory* factory = factoryFn != nullptr ? factoryFn() : nullptr;
            if (factory != nullptr) {
                Steinberg::IPluginFactory2* factory2 = nullptr;
                if (factory->queryInterface(Steinberg::IPluginFactory2::iid,
                                            reinterpret_cast<void**>(&factory2)) != Steinberg::kResultOk) {
                    factory2 = nullptr;
                }
                const int classCount = factory->countClasses();
                for (int index = 0; index < classCount; ++index) {
                    Steinberg::PClassInfo info;
                    if (factory->getClassInfo(index, &info) != Steinberg::kResultOk) {
                        continue;
                    }
                    Vst3ClassDescriptor klass;
                    klass.cid = tuidToHex(info.cid);
                    klass.category = info.category;
                    klass.name = info.name;
                    klass.vendor = descriptor.vendor.empty() ? "Waves" : descriptor.vendor;
                    if (factory2 != nullptr) {
                        Steinberg::PClassInfo2 info2;
                        if (factory2->getClassInfo2(index, &info2) == Steinberg::kResultOk) {
                            if (info2.vendor[0] != '\0') {
                                klass.vendor = info2.vendor;
                            }
                            if (info2.version[0] != '\0') {
                                klass.version = info2.version;
                            }
                            if (info2.subCategories[0] != '\0') {
                                klass.subCategories = splitCategoryPath(info2.subCategories, '|');
                                if (klass.subCategories.size() > 1 && lowerCopy(klass.subCategories.front()) == "fx") {
                                    klass.subCategories.erase(klass.subCategories.begin());
                                }
                            }
                        }
                    }
                    if (!klass.cid.empty() && !klass.name.empty()) {
                        classes.push_back(std::move(klass));
                    }
                }
                if (factory2 != nullptr) {
                    factory2->release();
                }
            }
        }
        if (bundleEntryCalled && bundleExit != nullptr) {
            bundleExit();
        }
        CFBundleUnloadExecutable(bundle);
    }
    CFRelease(bundle);
    return classes;
}
#endif

std::string executableInBundle(const std::filesystem::path& bundlePath, const std::string& executableName) {
#if defined(_WIN32)
    const auto platformPath = bundlePath / "Contents" / "x86_64-win";
    if (!std::filesystem::exists(platformPath)) {
        return {};
    }
    if (!executableName.empty()) {
        const auto vst3Name = platformPath / (executableName + ".vst3");
        if (std::filesystem::exists(vst3Name)) {
            return vst3Name.string();
        }
        const auto dllName = platformPath / (executableName + ".dll");
        if (std::filesystem::exists(dllName)) {
            return dllName.string();
        }
    }
    for (const auto& entry : std::filesystem::directory_iterator(platformPath)) {
        if (entry.is_regular_file() && (entry.path().extension() == ".vst3" || entry.path().extension() == ".dll")) {
            return entry.path().string();
        }
    }
    return {};
#else
    const auto macOSPath = bundlePath / "Contents" / "MacOS";
    if (!std::filesystem::exists(macOSPath)) {
        return {};
    }
    if (!executableName.empty()) {
        const auto exact = macOSPath / executableName;
        if (std::filesystem::exists(exact)) {
            return exact.string();
        }
    }
    for (const auto& entry : std::filesystem::directory_iterator(macOSPath)) {
        if (entry.is_regular_file()) {
            return entry.path().string();
        }
    }
    return {};
#endif
}

Vst3PluginDescriptor describeBundle(const std::filesystem::path& bundlePath) {
    Vst3PluginDescriptor descriptor;
    descriptor.bundlePath = bundlePath.string();
    descriptor.name = bundlePath.stem().string();
    if (!std::filesystem::is_directory(bundlePath) || bundlePath.extension() != ".vst3") {
        return descriptor;
    }

    const auto plistPath = bundlePath / "Contents" / "Info.plist";
    const auto moduleInfoPath = bundlePath / "Contents" / "Resources" / "moduleinfo.json";
    const auto plist = readTextFile(plistPath);
    const auto moduleInfo = readTextFile(moduleInfoPath);
    const auto bundleName = plistValueAfterKey(plist, "CFBundleName");
    const auto displayName = plistValueAfterKey(plist, "CFBundleDisplayName");
    const auto executableName = plistValueAfterKey(plist, "CFBundleExecutable");
    descriptor.moduleInfoPath = std::filesystem::exists(moduleInfoPath) ? moduleInfoPath.string() : "";
    descriptor.vendor = plistValueAfterKey(plist, "VSTManufacturer");
    if (descriptor.vendor.empty()) {
        descriptor.vendor = factoryVendorFromModuleInfo(moduleInfo);
    }
    if (!displayName.empty()) {
        descriptor.name = displayName;
    } else if (!bundleName.empty()) {
        descriptor.name = bundleName;
    }
    descriptor.executablePath = executableInBundle(bundlePath, executableName);
    descriptor.loadableBundle = !descriptor.executablePath.empty();
    const auto classes = parseModuleInfoClasses(moduleInfo);
    descriptor.classCount = static_cast<int>(classes.size());
    for (const auto& klass : classes) {
        if (klass.category == "Audio Module Class") {
            ++descriptor.audioClassCount;
        }
    }
    descriptor.brand = inferPluginBrand(descriptor.name, descriptor.vendor);
    descriptor.category = inferPluginCategory(descriptor.name, classes);
    return descriptor;
}

std::vector<Vst3PluginDescriptor> expandDescriptorByModuleClasses(const Vst3PluginDescriptor& descriptor) {
    if (descriptor.moduleInfoPath.empty()) {
        return {descriptor};
    }
    const auto classes = parseModuleInfoClasses(readTextFile(descriptor.moduleInfoPath));
    if (classes.empty() || descriptor.audioClassCount <= 1) {
        return {descriptor};
    }
    std::vector<Vst3PluginDescriptor> expanded;
    std::set<std::string> seen;
    for (const auto& klass : classes) {
        if (klass.category != "Audio Module Class" && klass.category != "Audio Effect Class") {
            continue;
        }
        Vst3PluginDescriptor copy = descriptor;
        copy.name = klass.name.empty() ? descriptor.name : klass.name;
        copy.vendor = klass.vendor.empty() ? descriptor.vendor : klass.vendor;
        copy.componentClassCid = klass.cid;
        copy.componentClassName = klass.name;
        copy.brand = inferPluginBrand(copy.name, copy.vendor);
        copy.category = inferPluginCategory(copy.name, {klass});
        const auto key = copy.bundlePath + "\n" + copy.name + "\n" + copy.componentClassCid;
        if (seen.insert(key).second) {
            expanded.push_back(std::move(copy));
        }
    }
    return expanded.empty() ? std::vector<Vst3PluginDescriptor>{descriptor} : expanded;
}

#if defined(__APPLE__)
std::string audioComponentName(AudioComponent component) {
    CFStringRef name = nullptr;
    if (AudioComponentCopyName(component, &name) != noErr || name == nullptr) {
        return {};
    }
    char buffer[1024] = {};
    const bool ok = CFStringGetCString(name, buffer, sizeof(buffer), kCFStringEncodingUTF8);
    CFRelease(name);
    return ok ? std::string(buffer) : std::string{};
}

std::vector<std::string> expandedWavesAudioUnitNames() {
    std::vector<std::string> names;
    std::set<std::string> seen;
    const AudioComponentDescription descriptions[] = {
        {kAudioUnitType_Effect, 0, 0, 0, 0},
        {kAudioUnitType_MusicEffect, 0, 0, 0, 0}
    };
    for (const auto& query : descriptions) {
        AudioComponentDescription desc = query;
        AudioComponent component = nullptr;
        while ((component = AudioComponentFindNext(component, &desc)) != nullptr) {
            const auto fullName = audioComponentName(component);
            if (fullName.empty()) {
                continue;
            }
            const auto colon = fullName.find(':');
            const auto manufacturer = colon == std::string::npos ? std::string{} : fullName.substr(0, colon);
            std::string pluginName = colon == std::string::npos ? fullName : fullName.substr(colon + 1);
            pluginName.erase(pluginName.begin(), std::find_if(pluginName.begin(), pluginName.end(), [](unsigned char ch) {
                return !std::isspace(ch);
            }));
            pluginName.erase(std::find_if(pluginName.rbegin(), pluginName.rend(), [](unsigned char ch) {
                return !std::isspace(ch);
            }).base(), pluginName.end());
            const auto searchable = lowerCopy(manufacturer + " " + pluginName);
            if (pluginName.empty() || searchable.find("waves") == std::string::npos) {
                continue;
            }
            if (seen.insert(pluginName).second) {
                names.push_back(pluginName);
            }
        }
    }
    std::sort(names.begin(), names.end(), [](const std::string& lhs, const std::string& rhs) {
        return lowerCopy(lhs) < lowerCopy(rhs);
    });
    return names;
}

std::string primaryRegularWaveShellVst3Path() {
    std::vector<std::filesystem::path> candidates;
    for (const auto& root : vst3Roots()) {
        if (!std::filesystem::exists(root)) {
            continue;
        }
        for (const auto& entry : std::filesystem::directory_iterator(root)) {
            const auto path = entry.path();
            const auto lowerPath = lowerCopy(path.filename().string());
            if (path.extension() == ".vst3" &&
                lowerPath.find("waveshell") != std::string::npos &&
                lowerPath.find("ara") == std::string::npos) {
                candidates.push_back(path);
            }
        }
    }
    if (candidates.empty()) {
        return {};
    }
    std::sort(candidates.begin(), candidates.end(), wavesShellVersionLess);
    return candidates.front().string();
}

std::vector<Vst3PluginDescriptor> expandWavesShellDescriptor(const Vst3PluginDescriptor& descriptor) {
    const auto lowerName = lowerCopy(descriptor.name + " " + descriptor.bundlePath);
    if (lowerName.find("waveshell") == std::string::npos) {
        return {descriptor};
    }
    if (lowerName.find("ara") != std::string::npos) {
        return {};
    }
    const auto primaryShell = primaryRegularWaveShellVst3Path();
    if (primaryShell.empty() || descriptor.bundlePath != primaryShell) {
        return {};
    }
    std::vector<Vst3PluginDescriptor> expanded;
#if defined(NEURACOUST_HAS_VST3_SDK)
    const auto factoryClasses = readMacVst3FactoryClasses(descriptor);
    std::set<std::string> seenFactoryClasses;
    for (const auto& klass : factoryClasses) {
        if (lowerCopy(klass.category).find("controller") != std::string::npos) {
            continue;
        }
        const auto lowerClassName = lowerCopy(klass.name);
        if (lowerClassName.empty() || lowerClassName.find("waveshell") != std::string::npos) {
            const auto displayName = wavesShellDisplayNameFromClass(klass);
            if (displayName.empty() || lowerCopy(displayName).find("waveshell") != std::string::npos) {
                continue;
            }
        }
        Vst3PluginDescriptor copy = descriptor;
        copy.name = wavesShellDisplayNameFromClass(klass);
        copy.brand = "Waves";
        copy.vendor = klass.vendor.empty() ? (descriptor.vendor.empty() ? "Waves" : descriptor.vendor) : klass.vendor;
        copy.category = inferPluginCategory(copy.name, {klass});
        copy.componentClassCid = klass.cid;
        copy.componentClassName = klass.name;
        copy.classCount = static_cast<int>(factoryClasses.size());
        copy.audioClassCount = 1;
        const auto key = copy.bundlePath + "\n" + copy.componentClassCid + "\n" + copy.name;
        if (seenFactoryClasses.insert(key).second) {
            expanded.push_back(std::move(copy));
        }
    }
    if (!expanded.empty()) {
        return expanded;
    }
#endif
    const auto names = expandedWavesAudioUnitNames();
    if (names.empty()) {
        return {descriptor};
    }
    for (const auto& name : names) {
        Vst3PluginDescriptor copy = descriptor;
        copy.name = name;
        copy.brand = "Waves";
        copy.vendor = descriptor.vendor.empty() ? "Waves" : descriptor.vendor;
        copy.category = inferPluginCategory(name, {});
        copy.componentClassName = name;
        expanded.push_back(std::move(copy));
    }
    return expanded;
}
#endif

std::vector<Vst3PluginDescriptor> scanVst3PluginBundlesUncached() {
    std::vector<Vst3PluginDescriptor> descriptors;
    std::set<std::string> seen;
    for (const auto& root : vst3Roots()) {
        if (!std::filesystem::exists(root)) {
            continue;
        }
        for (const auto& entry : std::filesystem::directory_iterator(root)) {
            if (!entry.is_directory() || entry.path().extension() != ".vst3") {
                continue;
            }
            const auto key = entry.path().string();
            if (!seen.insert(key).second) {
                continue;
            }
            const auto described = describeBundle(entry.path());
#if defined(__APPLE__)
            const auto wavesExpanded = expandWavesShellDescriptor(described);
            if (lowerCopy(described.name + " " + described.bundlePath).find("waveshell") != std::string::npos) {
                descriptors.insert(descriptors.end(), wavesExpanded.begin(), wavesExpanded.end());
                continue;
            }
#endif
            const auto expanded = expandDescriptorByModuleClasses(described);
            descriptors.insert(descriptors.end(), expanded.begin(), expanded.end());
        }
    }
    sortVst3PluginDescriptorsForDisplay(descriptors);
    return descriptors;
}

std::mutex& vst3ScanCacheMutex() {
    static std::mutex mutex;
    return mutex;
}

std::optional<std::vector<Vst3PluginDescriptor>>& vst3ScanCache() {
    static std::optional<std::vector<Vst3PluginDescriptor>> cache;
    return cache;
}

} // namespace

std::vector<Vst3PluginDescriptor> scanVst3PluginBundles(Vst3ScanMode mode) {
    std::lock_guard<std::mutex> lock(vst3ScanCacheMutex());
    auto& cache = vst3ScanCache();
    if (!cache.has_value() && mode == Vst3ScanMode::UseCache) {
        cache = loadPersistentScanCache();
    }
    if (mode == Vst3ScanMode::Refresh || !cache.has_value()) {
        cache = scanVst3PluginBundlesUncached();
        savePersistentScanCache(*cache);
    }
    return *cache;
}

void clearVst3PluginScanCache() {
    std::lock_guard<std::mutex> lock(vst3ScanCacheMutex());
    vst3ScanCache().reset();
}

std::string vst3BundleInventorySignature() {
    // Walk the VST3 roots and list .vst3 bundle paths — no probing, so this is fast enough to
    // run every time the browser opens. Include the bundle's modification time so a plug-in
    // updated in place is picked up too.
    std::vector<std::string> entries;
    for (const auto& root : vst3Roots()) {
        std::error_code ec;
        if (!std::filesystem::exists(root, ec)) continue;
        for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
            if (ec) break;
            if (entry.path().extension() != ".vst3") continue;
            std::error_code tec;
            const auto mtime = std::filesystem::last_write_time(entry.path(), tec);
            const auto ticks = tec ? 0LL
                : static_cast<long long>(mtime.time_since_epoch().count());
            entries.push_back(entry.path().string() + "|" + std::to_string(ticks));
        }
    }
    std::sort(entries.begin(), entries.end());
    std::string signature;
    for (const auto& e : entries) { signature += e; signature += '\n'; }
    return signature;
}

void sortVst3PluginDescriptorsForDisplay(std::vector<Vst3PluginDescriptor>& descriptors) {
    std::stable_sort(descriptors.begin(), descriptors.end(), vst3DisplayLess);
}

bool vst3PluginDescriptorMatchesFilter(const Vst3PluginDescriptor& descriptor, const std::string& filter) {
    if (filter.empty()) {
        return true;
    }
    return searchTextContainsAllTokens(descriptor.brand + " " + descriptor.name + " " + descriptor.category + " " +
                                       descriptor.vendor + " " + descriptor.componentClassName + " " +
                                       descriptor.bundlePath + " " + descriptor.executablePath + " " +
                                       pluginSearchAliasText(descriptor.brand,
                                                             descriptor.name,
                                                             descriptor.category,
                                                             descriptor.vendor,
                                                             descriptor.componentClassName),
                                       filter);
}

bool vst3PluginDescriptorMatchesCriteria(const Vst3PluginDescriptor& descriptor,
                                         const Vst3PluginFilterCriteria& criteria) {
    if (criteria.requireLoadable && !descriptor.loadableBundle) {
        return false;
    }
    if (!criteria.brand.empty() && descriptor.brand != criteria.brand) {
        return false;
    }
    if (!criteria.category.empty() && descriptor.category != criteria.category) {
        return false;
    }
    return vst3PluginDescriptorMatchesFilter(descriptor, criteria.text);
}

Vst3PluginDescriptor describeVst3PluginBundle(const std::string& bundlePath) {
    return describeBundle(std::filesystem::path(bundlePath));
}

Vst3PluginDescriptor resolveVst3PluginDescriptorForInsert(const std::string& pluginName,
                                                          const std::string& pluginPath) {
    return resolveVst3PluginDescriptorForInsert(pluginName, pluginPath, {}, {});
}

Vst3PluginDescriptor resolveVst3PluginDescriptorForInsert(const std::string& pluginName,
                                                          const std::string& pluginPath,
                                                          const std::string& pluginClassId,
                                                          const std::string& pluginClassName) {
    const auto plugins = scanVst3PluginBundles();
    const auto normalizedWantedClassId = normalizedSearchText(pluginClassId);
    const auto lowerRequestedClass = lowerCopy(pluginClassName + " " + pluginClassId);
    const bool requestedWavesWrapper = lowerRequestedClass.find("immersive wrapper") != std::string::npos ||
        lowerRequestedClass.find("696d6d657273697665") != std::string::npos;
    if (requestedWavesWrapper &&
        !pluginName.empty() &&
        lowerCopy(pluginName).find("immersive wrapper") == std::string::npos) {
        const auto normalizedProductNames = wavesProductLookupBases(pluginName);
        const Vst3PluginDescriptor* fallback = nullptr;
        const Vst3PluginDescriptor* monoFallback = nullptr;
        for (const auto& plugin : plugins) {
            const auto wavesText = lowerCopy(plugin.brand + " " + plugin.vendor + " " + plugin.bundlePath + " " + plugin.executablePath);
            if (wavesText.find("waves") == std::string::npos ||
                plugin.componentClassCid.empty()) {
                continue;
            }
            const auto className = trimCopy(plugin.componentClassName.empty() ? plugin.name : plugin.componentClassName);
            if (normalizedProductNames.find(lowerCopy(withoutWavesChannelSuffix(className))) == normalizedProductNames.end()) {
                continue;
            }
            const auto lowerClass = lowerCopy(className);
            if (std::any_of(normalizedProductNames.begin(), normalizedProductNames.end(), [&](const std::string& productName) {
                    return lowerClass == productName + " stereo";
                })) {
                Vst3PluginDescriptor resolved = plugin;
                resolved.name = pluginName;
                return resolved;
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
        const auto* resolvedPlugin = fallback != nullptr ? fallback : monoFallback;
        if (resolvedPlugin != nullptr) {
            Vst3PluginDescriptor resolved = *resolvedPlugin;
            resolved.name = pluginName;
            return resolved;
        }
    }
    if (!normalizedWantedClassId.empty() || !pluginClassName.empty()) {
        auto byClass = std::find_if(plugins.begin(), plugins.end(), [&](const Vst3PluginDescriptor& plugin) {
            const bool pathMatches = pluginPath.empty() ||
                plugin.bundlePath == pluginPath ||
                plugin.executablePath == pluginPath;
            if (!pathMatches) {
                return false;
            }
            if (!normalizedWantedClassId.empty() &&
                normalizedSearchText(plugin.componentClassCid) == normalizedWantedClassId) {
                return true;
            }
            return !pluginClassName.empty() && plugin.componentClassName == pluginClassName;
        });
        if (byClass != plugins.end()) {
            Vst3PluginDescriptor resolved = *byClass;
            if (!pluginName.empty() &&
                resolved.name != pluginName &&
                isWavesDescriptorNameOrPath(resolved.name, resolved.bundlePath.empty() ? pluginPath : resolved.bundlePath)) {
                if (!wavesDescriptorMatchesRequestedProduct(resolved, pluginName)) {
                    byClass = plugins.end();
                } else {
                resolved.name = pluginName;
                }
            }
            if (byClass != plugins.end()) {
                return resolved;
            }
        }
    }
    if (isWavesDescriptorNameOrPath(pluginName, pluginPath)) {
        const auto normalizedProductNames = wavesProductLookupBases(pluginName);
        const Vst3PluginDescriptor* fallback = nullptr;
        const Vst3PluginDescriptor* monoFallback = nullptr;
        for (const auto& plugin : plugins) {
            const auto wavesText = lowerCopy(plugin.brand + " " + plugin.vendor + " " + plugin.bundlePath + " " + plugin.executablePath);
            const bool pathMatches = pluginPath.empty() ||
                plugin.bundlePath == pluginPath ||
                plugin.executablePath == pluginPath;
            if (!pathMatches ||
                wavesText.find("waves") == std::string::npos ||
                plugin.componentClassCid.empty()) {
                continue;
            }
            const auto className = trimCopy(plugin.componentClassName.empty() ? plugin.name : plugin.componentClassName);
            if (normalizedProductNames.find(lowerCopy(withoutWavesChannelSuffix(className))) == normalizedProductNames.end()) {
                continue;
            }
            const auto lowerClass = lowerCopy(className);
            if (std::any_of(normalizedProductNames.begin(), normalizedProductNames.end(), [&](const std::string& productName) {
                    return lowerClass == productName + " stereo";
                })) {
                Vst3PluginDescriptor resolved = plugin;
                resolved.name = pluginName;
                return resolved;
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
        const auto* resolvedPlugin = fallback != nullptr ? fallback : monoFallback;
        if (resolvedPlugin != nullptr) {
            Vst3PluginDescriptor resolved = *resolvedPlugin;
            resolved.name = pluginName;
            return resolved;
        }
        auto wavesByName = std::find_if(plugins.begin(), plugins.end(), [&](const Vst3PluginDescriptor& plugin) {
            return !pluginName.empty() &&
                plugin.name == pluginName &&
                lowerCopy(plugin.brand + " " + plugin.vendor + " " + plugin.bundlePath).find("waves") != std::string::npos;
        });
        if (wavesByName != plugins.end()) {
            return *wavesByName;
        }
    }
    auto found = std::find_if(plugins.begin(), plugins.end(), [&](const Vst3PluginDescriptor& plugin) {
        return !pluginName.empty() && plugin.name == pluginName &&
               !pluginPath.empty() && (plugin.bundlePath == pluginPath || plugin.executablePath == pluginPath);
    });
    if (found != plugins.end()) {
        return *found;
    }
    found = std::find_if(plugins.begin(), plugins.end(), [&](const Vst3PluginDescriptor& plugin) {
        return !pluginName.empty() && plugin.name == pluginName;
    });
    if (found != plugins.end()) {
        return *found;
    }
    found = std::find_if(plugins.begin(), plugins.end(), [&](const Vst3PluginDescriptor& plugin) {
        return !pluginPath.empty() && (plugin.bundlePath == pluginPath || plugin.executablePath == pluginPath);
    });
    if (found != plugins.end()) {
        return *found;
    }

    if (!pluginPath.empty()) {
        const std::filesystem::path path(pluginPath);
        if (std::filesystem::is_directory(path) && path.extension() == ".vst3") {
            return describeBundle(path);
        }
        if (std::filesystem::is_regular_file(path)) {
            Vst3PluginDescriptor descriptor;
            descriptor.name = pluginName.empty() ? path.stem().string() : pluginName;
            descriptor.bundlePath = path.parent_path().string();
            descriptor.executablePath = path.string();
            descriptor.loadableBundle = true;
            descriptor.brand = inferPluginBrand(descriptor.name, {});
            descriptor.category = inferPluginCategory(descriptor.name, {});
            descriptor.componentClassCid = pluginClassId;
            descriptor.componentClassName = pluginClassName;
            return descriptor;
        }
    }

    Vst3PluginDescriptor descriptor;
    descriptor.name = pluginName;
    descriptor.bundlePath = pluginPath;
    descriptor.executablePath = pluginPath;
    descriptor.loadableBundle = !pluginPath.empty();
    descriptor.brand = inferPluginBrand(descriptor.name, {});
    descriptor.category = inferPluginCategory(descriptor.name, {});
    descriptor.componentClassCid = pluginClassId;
    descriptor.componentClassName = pluginClassName;
    return descriptor;
}

std::vector<Vst3ClassDescriptor> readVst3ModuleClasses(const Vst3PluginDescriptor& descriptor) {
    if (descriptor.moduleInfoPath.empty()) {
        return {};
    }
    return parseModuleInfoClasses(readTextFile(descriptor.moduleInfoPath));
}

Vst3HostCapabilities vst3HostCapabilities() {
    Vst3HostCapabilities capabilities;
    capabilities.sdkRuntimeLoading = true;
    capabilities.sdkProcessorProbe = true;
    capabilities.sdkAudioProcessing = true;
    capabilities.message = "VST3 bundle discovery, descriptor validation, module factory probing, SDK processor readiness probing, and offline stereo process calls are enabled.";
    return capabilities;
}

} // namespace neuracoust::daw
