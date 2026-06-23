#pragma once

#include <functional>
#include <string>

#include "infra/saveimport/sav_import.hpp"

struct FactorySnapshotModel;

namespace FactorySnapshot
{
    /// @brief Build a read-only snapshot from an already-parsed ParseResult.
    /// Reuses SavImport::BuildGraph for topology/rates, then computes the
    /// resource-flow report over the built nodes. On failure returns false and
    /// fills model.error (model is left cleared). On success model.ok = true and
    /// model owns the nodes/links/warnings/flow. Existing model state is cleared
    /// first either way.
    bool BuildSnapshot(const SavImport::ParseResult& parsed,
        const std::function<unsigned long long int()>& id_generator,
        const SavImport::BuildOptions& options,
        FactorySnapshotModel& model,
        std::string& err);

    /// @brief Convenience: parse wrapper JSON then BuildSnapshot. A parse error
    /// is surfaced through model.error / the bool result.
    bool BuildSnapshotFromJson(const std::string& wrapper_json,
        const std::function<unsigned long long int()>& id_generator,
        const SavImport::BuildOptions& options,
        FactorySnapshotModel& model,
        std::string& err);
}
