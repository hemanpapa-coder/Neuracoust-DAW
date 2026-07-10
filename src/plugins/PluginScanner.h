#pragma once

#include <string>
#include <vector>

namespace neuracoust::daw {

struct PluginCandidate {
    std::string name;
    std::string path;
    std::string format;
    std::string scope;
    std::string brand;
    std::string category;
    bool exists = false;
    std::string pluginName;
    bool requiresHostRenderer = false;
    std::string pluginClassId;
    std::string pluginClassName;
};

struct PluginCandidateFilterOptions {
    std::vector<std::string> brands;
    std::vector<std::string> categories;
    std::vector<std::string> formats;
    std::vector<std::string> scopes;
};

struct PluginCandidateFilterCriteria {
    std::string text;
    std::string brand;
    std::string category;
    std::string format;
    std::string scope;
    bool requireExisting = true;
};

bool pluginCandidateMatchesFilter(const PluginCandidate& candidate, const std::string& filter);
bool pluginCandidateMatchesCriteria(const PluginCandidate& candidate,
                                    const PluginCandidateFilterCriteria& criteria);
std::vector<PluginCandidate> filterPluginCandidates(const std::vector<PluginCandidate>& candidates,
                                                    const std::string& filter);
std::vector<PluginCandidate> filterPluginCandidates(const std::vector<PluginCandidate>& candidates,
                                                    const PluginCandidateFilterCriteria& criteria);
void sortPluginCandidatesForDisplay(std::vector<PluginCandidate>& candidates);
PluginCandidateFilterOptions pluginCandidateFilterOptions(const std::vector<PluginCandidate>& candidates);
PluginCandidate describeInstalledPluginCandidate(const std::string& path,
                                                 const std::string& format);
std::vector<PluginCandidate> scanKnownPluginLocations();

} // namespace neuracoust::daw
