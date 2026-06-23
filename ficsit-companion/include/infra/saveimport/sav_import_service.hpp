#pragma once

#include <string>
#include <vector>

std::string DeriveWorldName(std::string stem);
std::vector<std::string> DiscoverWorldNames(const std::vector<std::string>& sav_stems);
