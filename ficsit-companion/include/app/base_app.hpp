#pragma once

#include <chrono>
#include <string>

#include "infra/saveimport/sav_import.hpp"

class BaseApp
{
public:
    BaseApp();
    virtual ~BaseApp();
    void Render();

    virtual void SaveSession() = 0;

    /// @brief Feed this tool the JSON produced by one shared `.sav` wrapper run,
    /// together with the shared import options (layout / spacing / route wiring)
    /// so every tool builds the graph identically — the single global load path.
    /// Default is a no-op so a tool can opt out. Overrides must be read-only with
    /// respect to other tools' state (they own only their own model).
    virtual void LoadFromWrapperJson(const std::string& wrapper_json,
                                     const SavImport::BuildOptions& options)
    {
        (void)wrapper_json;
        (void)options;
    }

    bool HasRecentInteraction() const;

protected:
    virtual void RenderImpl() = 0;

private:
    std::chrono::steady_clock::time_point last_time_interacted;
};
