#include "infra/factory_snapshot_builder.hpp"

#include "domain/factory_snapshot_model.hpp"
#include "domain/link.hpp"
#include "domain/node.hpp"
#include "domain/resource_flow.hpp"

namespace FactorySnapshot
{
    bool BuildSnapshot(const SavImport::ParseResult& parsed,
        const std::function<unsigned long long int()>& id_generator,
        const SavImport::BuildOptions& options,
        FactorySnapshotModel& model,
        std::string& err)
    {
        model.Clear();
        err.clear();

        if (!parsed.ok)
        {
            model.error = parsed.error.empty() ? "Save data did not parse" : parsed.error;
            err = model.error;
            return false;
        }

        SavImport::BuildOutput built;
        std::string build_err;
        if (!SavImport::BuildGraph(parsed, id_generator, built, build_err, options))
        {
            model.error = build_err.empty() ? "Failed to build snapshot graph" : build_err;
            err = model.error;
            return false;
        }

        model.nodes = std::move(built.nodes);
        model.links = std::move(built.links);
        model.warnings = std::move(built.warnings);
        model.flow = BuildResourceFlowReport(model.nodes);
        model.ok = true;
        return true;
    }

    bool BuildSnapshotFromJson(const std::string& wrapper_json,
        const std::function<unsigned long long int()>& id_generator,
        const SavImport::BuildOptions& options,
        FactorySnapshotModel& model,
        std::string& err)
    {
        const SavImport::ParseResult parsed = SavImport::ParseWrapperJson(wrapper_json);
        return BuildSnapshot(parsed, id_generator, options, model, err);
    }
}
