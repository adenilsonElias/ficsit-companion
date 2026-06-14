#include <catch2/catch_test_macros.hpp>

#include "domain/fractional_number.hpp"
#include "domain/link.hpp"
#include "domain/node.hpp"
#include "domain/pin.hpp"

// ---------------------------------------------------------------------------
// Constructs the real graph types (MergerNode, CustomSplitterNode, Pin, Link)
// to prove that fc-tests can link all symbols pulled in from fc-core.
// ---------------------------------------------------------------------------

/// @test   The real graph types (MergerNode, CustomSplitterNode, Pin, Link) can be constructed
///         and wired together from the test target, and their structural invariants hold:
///         a merger is 3-in/1-out, a custom splitter is 1-in/3-out, fresh pins default to rate 0,
///         node-kind predicates classify correctly, and a Link records its endpoints and pin ids.
/// @covers Construction + accessors of MergerNode, CustomSplitterNode, Pin, and Link. Primarily a
///         link/symbol-resolution test (every symbol pulled from fc-core must resolve) that also
///         pins down pin counts, IsMerger/IsOrganizer/IsCustomSplitter/IsCraft/IsPowered, and
///         Link::start/end/start_id/end_id.
TEST_CASE("graph types link in fc-tests", "[link]")
{
    // Minimal id generator used by node constructors
    unsigned long long int counter = 1;
    auto id_gen = [&counter]() { return counter++; };

    // --- MergerNode ---
    // A Merger has 3 input pins and 1 output pin (nullptr item = untyped).
    ax::NodeEditor::NodeId merger_node_id(1);
    MergerNode merger(merger_node_id, id_gen, nullptr);

    REQUIRE(merger.ins.size()  == 3);
    REQUIRE(merger.outs.size() == 1);
    REQUIRE(merger.IsMerger());
    REQUIRE(merger.IsOrganizer());
    REQUIRE(!merger.IsCustomSplitter());
    REQUIRE(!merger.IsCraft());
    REQUIRE(!merger.IsPowered());

    // All pins created without an explicit base_rate default to FractionalNumber(0,1)
    REQUIRE(merger.ins[0]->base_rate  == FractionalNumber(0, 1));
    REQUIRE(merger.outs[0]->base_rate == FractionalNumber(0, 1));

    // --- CustomSplitterNode ---
    // A CustomSplitter has 1 input pin and 3 output pins.
    ax::NodeEditor::NodeId splitter_node_id(100);
    CustomSplitterNode splitter(splitter_node_id, id_gen, nullptr);

    REQUIRE(splitter.ins.size()  == 1);
    REQUIRE(splitter.outs.size() == 3);
    REQUIRE(splitter.IsCustomSplitter());
    REQUIRE(splitter.IsOrganizer());
    REQUIRE(!splitter.IsMerger());
    REQUIRE(!splitter.IsCraft());
    REQUIRE(!splitter.IsPowered());

    // --- Link ---
    // Connect splitter output[0] → merger input[0] (forces link.o to be linked).
    Pin* out_pin = splitter.outs[0].get();
    Pin* in_pin  = merger.ins[0].get();
    REQUIRE(out_pin->direction == ax::NodeEditor::PinKind::Output);
    REQUIRE(in_pin->direction  == ax::NodeEditor::PinKind::Input);

    ax::NodeEditor::LinkId link_id(200);
    Link link(link_id, out_pin, in_pin);

    REQUIRE(link.start    == out_pin);
    REQUIRE(link.end      == in_pin);
    REQUIRE(link.start_id == out_pin->id);
    REQUIRE(link.end_id   == in_pin->id);
}
