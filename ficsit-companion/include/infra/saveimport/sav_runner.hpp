#pragma once

#include <string>

namespace SavRunner
{
    /// @brief Spawn `node wrapper.js <sav_path>` and synchronously collect its
    /// JSON stdout (desktop only). wrapper.js is expected next to the executable
    /// under tools/sav_import/ (the post-build copy step ships it there).
    ///
    /// Shared by ProductionApp (graph import) and VehicleMapApp (logistics map);
    /// both parse different top-level blocks of the same single-pass output.
    ///
    /// @param sav_path Path to the .sav file to parse.
    /// @param node_exe Node executable to invoke (empty = use "node" from PATH).
    /// @param err Set to a human-readable message on failure.
    /// @return The captured JSON, or an empty string on failure (see @p err).
    ///         Always returns empty with an explanatory @p err on Emscripten.
    std::string RunSavWrapper(const std::string& sav_path,
                              const std::string& node_exe,
                              std::string& err);
}
