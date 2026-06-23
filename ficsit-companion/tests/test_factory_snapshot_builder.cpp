#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "domain/snapshot/factory_snapshot_model.hpp"
#include "domain/gamedata/game_data.hpp"
#include "domain/graph/graph_model.hpp"
#include "domain/nodes/node.hpp"
#include "domain/snapshot/resource_flow.hpp"
#include "infra/saveimport/factory_snapshot_builder.hpp"
#include "infra/saveimport/sav_import.hpp"

#include "graph_test_helpers.hpp" // IdGen, FakeEditorBackend

namespace
{
    std::string AssetBase()
    {
        const std::filesystem::path source_path(__FILE__);
        const std::filesystem::path repo_root =
            source_path.parent_path().parent_path().parent_path();
        return (repo_root / "assets" / "satisfactory").string();
    }

    void EnsureGameDataLoaded()
    {
        static bool loaded = false;
        if (loaded) return;
        Data::LoadData(AssetBase());
        loaded = true;
    }

    SavImport::Building WaterExtractor(const std::string& id, const float x)
    {
        SavImport::Building b;
        b.id = id;
        b.kind = SavImport::BuildingKind::Miner;
        b.item_name = "Water";
        b.extractor_kind = 4;
        b.clock = 1.0;
        b.x = x;
        return b;
    }

    SavImport::Building Manufacturer(const std::string& id, const std::string& recipe, const float x)
    {
        SavImport::Building b;
        b.id = id;
        b.kind = SavImport::BuildingKind::Manufacturer;
        b.recipe_name = recipe;
        b.clock = 1.0;
        b.x = x;
        return b;
    }

    SavImport::Building Merger(const std::string& id, const float x)
    {
        SavImport::Building b;
        b.id = id;
        b.kind = SavImport::BuildingKind::Merger;
        b.x = x;
        return b;
    }

    SavImport::Belt Belt(
        const std::string& id,
        const std::string& src,
        const int src_port,
        const std::string& dst,
        const int dst_port)
    {
        SavImport::Belt belt;
        belt.id = id;
        belt.src_building = src;
        belt.src_port = src_port;
        belt.src_dir = "out";
        belt.dst_building = dst;
        belt.dst_port = dst_port;
        belt.dst_dir = "in";
        return belt;
    }
}

/// @test A non-empty ParseResult builds an ok snapshot whose flow report has rows.
/// @covers FactorySnapshot::BuildSnapshot success path + resource-flow wiring.
TEST_CASE("BuildSnapshot reports ok with a flow report for a craft graph", "[snapshot_builder]")
{
    EnsureGameDataLoaded();

    SavImport::ParseResult parsed;
    parsed.ok = true;
    parsed.buildings.push_back(Manufacturer("c1", "Iron Plate", 0.0f));

    IdGen ids;
    FactorySnapshotModel model;
    std::string err;
    REQUIRE(FactorySnapshot::BuildSnapshot(parsed, std::ref(ids), {}, model, err));
    REQUIRE(model.ok);
    REQUIRE(model.error.empty());
    REQUIRE_FALSE(model.nodes.empty());
    REQUIRE_FALSE(model.flow.rows.empty());
}

/// @test Regression: reloading game data must not leave BuildGraph's recipe/item
/// lookup cache holding dangling pointers. Build once (warms the cache), reload
/// Data (frees the old Item/Recipe objects, refilling the same containers), then
/// build again. Before the fix the cache keyed invalidation on the never-changing
/// container address, so the second build dereferenced freed recipes (SIGSEGV in
/// Debug). Now invalidation keys on Data::Generation().
/// @covers SavImport lookup-cache invalidation across Data reloads.
TEST_CASE("BuildSnapshot survives a game-data reload (stale lookup cache)", "[snapshot_builder]")
{
    EnsureGameDataLoaded();

    auto build_iron_plate = []() {
        SavImport::ParseResult parsed;
        parsed.ok = true;
        parsed.buildings.push_back(Manufacturer("c1", "Iron Plate", 0.0f));
        IdGen ids;
        FactorySnapshotModel model;
        std::string err;
        const bool ok = FactorySnapshot::BuildSnapshot(parsed, std::ref(ids), {}, model, err);
        return std::make_pair(ok, model.nodes.size());
    };

    const auto before = build_iron_plate();
    REQUIRE(before.first);
    REQUIRE(before.second > 0);

    const unsigned long long gen_before = Data::Generation();

    // Churn the heap so the reload's new Item/Recipe objects do NOT reuse the
    // exact addresses just freed. Without this, the Debug allocator hands the
    // freed blocks straight back and a stale cache's dangling pointers happen to
    // land on the rebuilt objects, hiding the bug. (This mirrors what the rest of
    // the suite does between the first load and a later reload.)
    {
        std::vector<std::unique_ptr<std::string>> churn;
        churn.reserve(20000);
        for (int i = 0; i < 20000; ++i)
            churn.push_back(std::make_unique<std::string>(64, 'x'));
    }

    Data::LoadData(AssetBase()); // reload: frees the objects the cache pointed at
    REQUIRE(Data::Generation() != gen_before);

    const auto after = build_iron_plate(); // must rebuild the cache, not crash
    REQUIRE(after.first);
    REQUIRE(after.second == before.second);
}

/// @test Regression (spec §10): a Water Extractor producing 120/min into a Pipe
/// Junction shows 120 on the producer-side junction input in the snapshot graph.
/// @covers BuildSnapshot preserves producer-side pipe-junction rates (via BuildGraph).
TEST_CASE("BuildSnapshot keeps 120/min on producer-side pipe-junction inputs", "[snapshot_builder]")
{
    EnsureGameDataLoaded();

    SavImport::ParseResult parsed;
    parsed.ok = true;
    parsed.buildings.push_back(WaterExtractor("water_a", 0.0f));
    parsed.buildings.push_back(WaterExtractor("water_b", 100.0f));
    parsed.buildings.push_back(Manufacturer("coal_generator", "Power (Coal)", 200.0f));

    SavImport::PipeNetwork net;
    net.id = 1;
    net.fluid = "Water";
    net.endpoints.push_back({ "water_a", "out" });
    net.endpoints.push_back({ "water_b", "out" });
    net.endpoints.push_back({ "coal_generator", "in" });
    parsed.pipe_networks.push_back(net);

    IdGen ids;
    FactorySnapshotModel model;
    std::string err;
    REQUIRE(FactorySnapshot::BuildSnapshot(parsed, std::ref(ids), {}, model, err));

    const LogisticsNode* junction = nullptr;
    for (const auto& n : model.nodes)
    {
        if (n->IsLogistics()
            && static_cast<const LogisticsNode*>(n.get())->logistics_kind == LogisticsNode::Kind::PipeJunction)
        {
            junction = static_cast<const LogisticsNode*>(n.get());
            break;
        }
    }
    REQUIRE(junction != nullptr);
    REQUIRE(junction->ins.size() == 2);
    REQUIRE(junction->ins[0]->current_rate == FractionalNumber(120, 1));
    REQUIRE(junction->ins[1]->current_rate == FractionalNumber(120, 1));
}

/// @test A failed parse (ok=false) yields ok=false and a non-empty error, no nodes.
/// @covers BuildSnapshot guard against an un-parsed ParseResult.
TEST_CASE("BuildSnapshot fails cleanly on a not-ok ParseResult", "[snapshot_builder]")
{
    SavImport::ParseResult parsed; // parsed.ok defaults to false
    IdGen ids;
    FactorySnapshotModel model;
    std::string err;
    REQUIRE_FALSE(FactorySnapshot::BuildSnapshot(parsed, std::ref(ids), {}, model, err));
    REQUIRE_FALSE(model.ok);
    REQUIRE_FALSE(model.error.empty());
    REQUIRE(model.nodes.empty());
}

/// @test Importer warnings are carried onto the snapshot model verbatim.
/// @covers BuildSnapshot copies BuildOutput::warnings into the model.
TEST_CASE("BuildSnapshot retains importer warnings", "[snapshot_builder]")
{
    EnsureGameDataLoaded();

    // A merger fed two different item streams emits a warning in BuildGraph.
    SavImport::ParseResult parsed;
    parsed.ok = true;
    parsed.buildings.push_back(Manufacturer("plastic_refinery", "Plastic", 0.0f));
    parsed.buildings.push_back(Manufacturer("sheet_constructor", "Copper Sheet", 100.0f));
    parsed.buildings.push_back(Merger("mixed_merger", 200.0f));
    parsed.belts.push_back(Belt("plastic_to_merger", "plastic_refinery", 0, "mixed_merger", 0));
    parsed.belts.push_back(Belt("sheet_to_merger", "sheet_constructor", 0, "mixed_merger", 1));

    IdGen ids;
    FactorySnapshotModel model;
    std::string err;
    REQUIRE(FactorySnapshot::BuildSnapshot(parsed, std::ref(ids), {}, model, err));
    REQUIRE_FALSE(model.warnings.empty());
}

/// @test Building a snapshot leaves a separately-constructed modeler GraphModel
/// completely empty — snapshot ownership never leaks into the planner.
/// @covers spec §10 "Snapshot load does not mutate a modeler GraphModel".
TEST_CASE("BuildSnapshot does not mutate a separate GraphModel", "[snapshot_builder]")
{
    EnsureGameDataLoaded();

    FakeEditorBackend fake;
    GraphModel planner(fake); // independent modeler graph
    const std::size_t before = planner.nodes.size();

    SavImport::ParseResult parsed;
    parsed.ok = true;
    parsed.buildings.push_back(Manufacturer("c1", "Iron Plate", 0.0f));

    IdGen ids;
    FactorySnapshotModel model;
    std::string err;
    REQUIRE(FactorySnapshot::BuildSnapshot(parsed, std::ref(ids), {}, model, err));

    REQUIRE(planner.nodes.size() == before);
}
