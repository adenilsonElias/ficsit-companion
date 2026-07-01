#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <unordered_set>
#include <vector>

#include "domain/graph/link.hpp"
#include "domain/graph/pin.hpp"
#include "domain/nodes/node.hpp"
#include "domain/snapshot/snapshot_bypass.hpp"

#include "graph_test_helpers.hpp" // IdGen, MakeLink

namespace
{
    // Builds organizer nodes (splitter = 1-in/3-out, merger = 3-in/1-out) and
    // wires them with MakeLink. "Visibility" is chosen per-node by pointer so a
    // test controls topology independent of node kind (the production predicate
    // is kind-based, but ComputeVisibleEdges takes an arbitrary predicate).
    struct BypassFixture
    {
        IdGen idgen;
        std::vector<std::unique_ptr<Node>> nodes;
        std::vector<std::unique_ptr<Link>> links;
        std::unordered_set<const Node*> hidden;

        CustomSplitterNode* AddSplitter()
        {
            auto n = std::make_unique<CustomSplitterNode>(
                ax::NodeEditor::NodeId(idgen()), [this] { return idgen(); }, nullptr);
            CustomSplitterNode* raw = n.get();
            nodes.push_back(std::move(n));
            return raw;
        }
        MergerNode* AddMerger()
        {
            auto n = std::make_unique<MergerNode>(
                ax::NodeEditor::NodeId(idgen()), [this] { return idgen(); }, nullptr);
            MergerNode* raw = n.get();
            nodes.push_back(std::move(n));
            return raw;
        }
        void Connect(Pin* out_pin, Pin* in_pin)
        {
            links.push_back(MakeLink(idgen(), out_pin, in_pin));
        }
        void Hide(const Node* n) { hidden.insert(n); }

        std::unordered_set<const Node*> dropped; // hidden AND not bypassed
        void Drop(const Node* n) { hidden.insert(n); dropped.insert(n); }

        std::vector<SnapshotEdge> Run()
        {
            return ComputeVisibleEdges(nodes, links,
                [this](const Node& n) { return hidden.count(&n) != 0; });
        }

        std::vector<SnapshotEdge> RunWithDrop()
        {
            return ComputeVisibleEdges(nodes, links,
                [this](const Node& n) { return hidden.count(&n) != 0; },
                [this](const Node& n) { return dropped.count(&n) == 0; }); // bypass unless dropped
        }
        static bool Has(const std::vector<SnapshotEdge>& es,
                        const Pin* start, const Pin* end)
        {
            for (const auto& e : es)
                if (e.start_id.Get() == start->id.Get() &&
                    e.end_id.Get()   == end->id.Get()) return true;
            return false;
        }
    };
}

/// @test A visible->visible link passes through unchanged.
TEST_CASE("visible link is emitted unchanged", "[snapshot_bypass]")
{
    BypassFixture fx;
    CustomSplitterNode* p = fx.AddSplitter();
    MergerNode* c = fx.AddMerger();
    fx.Connect(p->outs[0].get(), c->ins[0].get());

    auto edges = fx.Run();
    REQUIRE(edges.size() == 1);
    REQUIRE(BypassFixture::Has(edges, p->outs[0].get(), c->ins[0].get()));
}

/// @test A hidden splitter is bypassed; the producer connects to each consumer.
TEST_CASE("hidden splitter fans the producer to all consumers", "[snapshot_bypass]")
{
    BypassFixture fx;
    CustomSplitterNode* p  = fx.AddSplitter();
    CustomSplitterNode* h  = fx.AddSplitter(); fx.Hide(h);
    MergerNode* c1 = fx.AddMerger();
    MergerNode* c2 = fx.AddMerger();
    fx.Connect(p->outs[0].get(), h->ins[0].get());
    fx.Connect(h->outs[0].get(), c1->ins[0].get());
    fx.Connect(h->outs[1].get(), c2->ins[0].get());

    auto edges = fx.Run();
    REQUIRE(edges.size() == 2);
    REQUIRE(BypassFixture::Has(edges, p->outs[0].get(), c1->ins[0].get()));
    REQUIRE(BypassFixture::Has(edges, p->outs[0].get(), c2->ins[0].get()));
}

/// @test A hidden merger is bypassed; each producer connects to the consumer.
TEST_CASE("hidden merger joins all producers to the consumer", "[snapshot_bypass]")
{
    BypassFixture fx;
    CustomSplitterNode* p1 = fx.AddSplitter();
    CustomSplitterNode* p2 = fx.AddSplitter();
    MergerNode* h = fx.AddMerger(); fx.Hide(h);
    MergerNode* c = fx.AddMerger();
    fx.Connect(p1->outs[0].get(), h->ins[0].get());
    fx.Connect(p2->outs[0].get(), h->ins[1].get());
    fx.Connect(h->outs[0].get(),  c->ins[0].get());

    auto edges = fx.Run();
    REQUIRE(edges.size() == 2);
    REQUIRE(BypassFixture::Has(edges, p1->outs[0].get(), c->ins[0].get()));
    REQUIRE(BypassFixture::Has(edges, p2->outs[0].get(), c->ins[0].get()));
}

/// @test A chain of consecutive hidden nodes collapses to one direct edge.
TEST_CASE("hidden chain collapses to a direct edge", "[snapshot_bypass]")
{
    BypassFixture fx;
    CustomSplitterNode* p  = fx.AddSplitter();
    CustomSplitterNode* h1 = fx.AddSplitter(); fx.Hide(h1);
    MergerNode* h2 = fx.AddMerger(); fx.Hide(h2);
    MergerNode* c  = fx.AddMerger();
    fx.Connect(p->outs[0].get(),  h1->ins[0].get());
    fx.Connect(h1->outs[0].get(), h2->ins[0].get());
    fx.Connect(h2->outs[0].get(), c->ins[0].get());

    auto edges = fx.Run();
    REQUIRE(edges.size() == 1);
    REQUIRE(BypassFixture::Has(edges, p->outs[0].get(), c->ins[0].get()));
}

/// @test A hidden node with no visible downstream produces no edge.
TEST_CASE("hidden endpoint disappears", "[snapshot_bypass]")
{
    BypassFixture fx;
    CustomSplitterNode* p = fx.AddSplitter();
    MergerNode* h = fx.AddMerger(); fx.Hide(h);
    fx.Connect(p->outs[0].get(), h->ins[0].get());

    auto edges = fx.Run();
    REQUIRE(edges.empty());
}

/// @test A cycle among hidden nodes terminates (visited guard) and yields none.
TEST_CASE("cyclic hidden nodes terminate", "[snapshot_bypass]")
{
    BypassFixture fx;
    CustomSplitterNode* p  = fx.AddSplitter();
    MergerNode* h1 = fx.AddMerger();        fx.Hide(h1);
    CustomSplitterNode* h2 = fx.AddSplitter(); fx.Hide(h2);
    fx.Connect(p->outs[0].get(),  h1->ins[0].get());
    fx.Connect(h1->outs[0].get(), h2->ins[0].get());
    fx.Connect(h2->outs[0].get(), h1->ins[1].get()); // back-edge

    auto edges = fx.Run();
    REQUIRE(edges.empty());
}

/// @test With nothing hidden, every link is emitted unchanged.
TEST_CASE("no hidden nodes leaves links untouched", "[snapshot_bypass]")
{
    BypassFixture fx;
    CustomSplitterNode* p = fx.AddSplitter();
    MergerNode* c = fx.AddMerger();
    fx.Connect(p->outs[0].get(), c->ins[0].get());

    auto edges = fx.Run(); // hidden set empty
    REQUIRE(edges.size() == 1);
    REQUIRE(BypassFixture::Has(edges, p->outs[0].get(), c->ins[0].get()));
}

/// @test A dropped node emits no edges: both its inbound and outbound links vanish.
TEST_CASE("dropped node removes its links entirely", "[snapshot_bypass]")
{
    BypassFixture fx;
    CustomSplitterNode* p = fx.AddSplitter();
    MergerNode* d = fx.AddMerger(); fx.Drop(d);     // dropped, not bypassed
    CustomSplitterNode* c = fx.AddSplitter();
    fx.Connect(p->outs[0].get(), d->ins[0].get());  // p -> d
    fx.Connect(d->outs[0].get(), c->ins[0].get());  // d -> c

    auto edges = fx.RunWithDrop();
    REQUIRE(edges.empty()); // no p->c edge; the dropped node's links disappear
}

/// @test A bypassed node still reroutes when another node is merely dropped.
TEST_CASE("drop predicate leaves bypass nodes rerouting", "[snapshot_bypass]")
{
    BypassFixture fx;
    CustomSplitterNode* p = fx.AddSplitter();
    CustomSplitterNode* b = fx.AddSplitter(); fx.Hide(b);   // bypassed (hidden, not dropped)
    MergerNode* c = fx.AddMerger();
    fx.Connect(p->outs[0].get(), b->ins[0].get());
    fx.Connect(b->outs[0].get(), c->ins[0].get());

    auto edges = fx.RunWithDrop();
    REQUIRE(edges.size() == 1);
    REQUIRE(BypassFixture::Has(edges, p->outs[0].get(), c->ins[0].get()));
}

/// @test A chain visible -> bypass -> drop -> consumer terminates at the drop.
TEST_CASE("bypass chain terminates at a dropped node", "[snapshot_bypass]")
{
    BypassFixture fx;
    CustomSplitterNode* p = fx.AddSplitter();
    CustomSplitterNode* b = fx.AddSplitter(); fx.Hide(b);   // bypassed
    MergerNode* d = fx.AddMerger(); fx.Drop(d);             // dropped
    CustomSplitterNode* c = fx.AddSplitter();
    fx.Connect(p->outs[0].get(), b->ins[0].get());
    fx.Connect(b->outs[0].get(), d->ins[0].get());
    fx.Connect(d->outs[0].get(), c->ins[0].get());

    auto edges = fx.RunWithDrop();
    REQUIRE(edges.empty()); // walk reaches d, which is dropped => no edge to c
}
