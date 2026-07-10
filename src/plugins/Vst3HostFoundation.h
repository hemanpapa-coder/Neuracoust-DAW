#pragma once

#include <string>
#include <vector>

namespace neuracoust::daw {

struct Vst3PluginDescriptor {
    std::string name;
    std::string brand;
    std::string category;
    std::string bundlePath;
    std::string executablePath;
    std::string moduleInfoPath;
    std::string vendor;
    bool loadableBundle = false;
    int classCount = 0;
    int audioClassCount = 0;
    std::string componentClassCid;
    std::string componentClassName;
};

struct Vst3ClassDescriptor {
    std::string cid;
    std::string category;
    std::string name;
    std::string vendor;
    std::string version;
    std::vector<std::string> subCategories;
};

struct Vst3HostCapabilities {
    bool bundleDiscovery = true;
    bool descriptorValidation = true;
    bool sdkRuntimeLoading = false;
    bool sdkProcessorProbe = false;
    bool sdkAudioProcessing = false;
    std::string message;
};

struct Vst3PluginFilterCriteria {
    std::string text;
    std::string brand;
    std::string category;
    bool requireLoadable = true;
};

enum class Vst3ScanMode {
    UseCache,
    Refresh
};

std::vector<Vst3PluginDescriptor> scanVst3PluginBundles(Vst3ScanMode mode = Vst3ScanMode::UseCache);
void clearVst3PluginScanCache();
void sortVst3PluginDescriptorsForDisplay(std::vector<Vst3PluginDescriptor>& descriptors);
bool vst3PluginDescriptorMatchesFilter(const Vst3PluginDescriptor& descriptor, const std::string& filter);
bool vst3PluginDescriptorMatchesCriteria(const Vst3PluginDescriptor& descriptor,
                                         const Vst3PluginFilterCriteria& criteria);
Vst3PluginDescriptor describeVst3PluginBundle(const std::string& bundlePath);
Vst3PluginDescriptor resolveVst3PluginDescriptorForInsert(const std::string& pluginName,
                                                          const std::string& pluginPath);
Vst3PluginDescriptor resolveVst3PluginDescriptorForInsert(const std::string& pluginName,
                                                          const std::string& pluginPath,
                                                          const std::string& pluginClassId,
                                                          const std::string& pluginClassName);
std::vector<Vst3ClassDescriptor> readVst3ModuleClasses(const Vst3PluginDescriptor& descriptor);
Vst3HostCapabilities vst3HostCapabilities();

} // namespace neuracoust::daw
