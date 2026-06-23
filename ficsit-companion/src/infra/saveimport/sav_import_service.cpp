#include "infra/saveimport/sav_import_service.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_set>

std::string DeriveWorldName(std::string stem)
{
    for (const char* suffix : { "_CMP", "_autosave_0", "_autosave_1", "_autosave_2" })
    {
        const size_t suffix_len = std::strlen(suffix);
        if (stem.size() > suffix_len && stem.compare(stem.size() - suffix_len, suffix_len, suffix) == 0)
        {
            stem.resize(stem.size() - suffix_len);
            break;
        }
    }

    size_t underscore_with_digit = std::string::npos;
    for (size_t i = 0; i + 1 < stem.size(); ++i)
    {
        if (stem[i] == '_' && std::isdigit(static_cast<unsigned char>(stem[i + 1])))
        {
            underscore_with_digit = i;
            break;
        }
    }
    if (underscore_with_digit != std::string::npos)
    {
        stem.resize(underscore_with_digit);
    }

    return stem;
}

std::vector<std::string> DiscoverWorldNames(const std::vector<std::string>& sav_stems)
{
    std::unordered_set<std::string> seen;
    std::vector<std::string> worlds;

    for (const std::string& sav_stem : sav_stems)
    {
        std::string world = DeriveWorldName(sav_stem);
        if (world.empty()) continue;
        if (seen.insert(world).second)
        {
            worlds.push_back(world);
        }
    }

    std::sort(worlds.begin(), worlds.end());
    return worlds;
}
