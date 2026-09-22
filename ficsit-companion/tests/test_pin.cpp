#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>

#include "domain/graph/link.hpp"
#include "domain/nodes/node.hpp"
#include "domain/graph/pin.hpp"
#include "domain/gamedata/recipe.hpp"

#include <array>
#include <memory>

namespace
{
    struct PinTestNode final : Node
    {
        explicit PinTestNode(const Kind kind, const uintptr_t id = 1) :
            Node(ax::NodeEditor::NodeId(id)), kind(kind)
        {
        }

        Kind GetKind() const override
        {
            return kind;
        }

        Pin* AddPin(const ax::NodeEditor::PinKind direction, const bool locked = false)
        {
            auto pin = std::make_unique<Pin>(
                ax::NodeEditor::PinId(next_pin_id++), direction, this, nullptr, locked);
            Pin* result = pin.get();
            (direction == ax::NodeEditor::PinKind::Input ? ins : outs).push_back(
                std::move(pin));
            return result;
        }

        Kind kind;
        uintptr_t next_pin_id = 10;
    };

    struct OrganizerFixture
    {
        OrganizerFixture(const Node::Kind kind, const bool single_locked,
            const std::array<bool, 3>& multi_locked) :
            node(kind)
        {
            const bool splitter = kind == Node::Kind::CustomSplitter;
            single = node.AddPin(
                splitter ? ax::NodeEditor::PinKind::Input
                         : ax::NodeEditor::PinKind::Output,
                single_locked);
            for (size_t i = 0; i < multi.size(); ++i)
            {
                multi[i] = node.AddPin(
                    splitter ? ax::NodeEditor::PinKind::Output
                             : ax::NodeEditor::PinKind::Input,
                    multi_locked[i]);
            }
        }

        PinTestNode node;
        Pin* single = nullptr;
        std::array<Pin*, 3> multi{};
    };

    void CheckOrganizerConstraints(const Node::Kind kind)
    {
        SECTION("locking the single pin locks the final unlocked multi pin")
        {
            OrganizerFixture fixture(kind, false, { true, true, false });

            fixture.single->SetLocked(true);

            REQUIRE(fixture.single->GetLocked());
            REQUIRE(fixture.multi[0]->GetLocked());
            REQUIRE(fixture.multi[1]->GetLocked());
            REQUIRE(fixture.multi[2]->GetLocked());
        }

        SECTION("unlocking the single pin repairs an all-locked multi side")
        {
            OrganizerFixture fixture(kind, true, { true, true, true });

            fixture.single->SetLocked(false);

            REQUIRE_FALSE(fixture.single->GetLocked());
            REQUIRE_FALSE(fixture.multi[0]->GetLocked());
            REQUIRE_FALSE(fixture.multi[1]->GetLocked());
            REQUIRE_FALSE(fixture.multi[2]->GetLocked());
        }

        SECTION("locking the last multi pin locks the single pin")
        {
            OrganizerFixture fixture(kind, false, { true, true, false });

            fixture.multi[2]->SetLocked(true);

            REQUIRE(fixture.single->GetLocked());
            REQUIRE(fixture.multi[2]->GetLocked());
        }

        SECTION("a locked single pin forces the final multi pin to lock")
        {
            OrganizerFixture fixture(kind, true, { true, false, false });

            fixture.multi[1]->SetLocked(true);

            REQUIRE(fixture.single->GetLocked());
            REQUIRE(fixture.multi[0]->GetLocked());
            REQUIRE(fixture.multi[1]->GetLocked());
            REQUIRE(fixture.multi[2]->GetLocked());
        }

        SECTION("unlocking the first multi pin also unlocks a locked single pin")
        {
            OrganizerFixture fixture(kind, true, { true, true, true });

            fixture.multi[0]->SetLocked(false);

            REQUIRE_FALSE(fixture.single->GetLocked());
            REQUIRE_FALSE(fixture.multi[0]->GetLocked());
            REQUIRE(fixture.multi[1]->GetLocked());
            REQUIRE(fixture.multi[2]->GetLocked());
        }
    }
}

TEST_CASE("pin construction initializes all observable state", "[pin]")
{
    PinTestNode node(Node::Kind::Sink);
    const Item item("Synthetic Pin Item", "", 9);
    Pin default_pin(
        ax::NodeEditor::PinId(100), ax::NodeEditor::PinKind::Input, &node, &item);
    Pin configured_pin(
        ax::NodeEditor::PinId(101), ax::NodeEditor::PinKind::Output, &node, &item,
        true, FractionalNumber(45, 2));

    REQUIRE(default_pin.id == ax::NodeEditor::PinId(100));
    REQUIRE(default_pin.direction == ax::NodeEditor::PinKind::Input);
    REQUIRE(default_pin.node == &node);
    REQUIRE(default_pin.item == &item);
    REQUIRE(default_pin.base_rate == FractionalNumber(0, 1));
    REQUIRE(default_pin.links.empty());
    REQUIRE(default_pin.current_rate == FractionalNumber(0, 1));
    REQUIRE_FALSE(default_pin.error);
    REQUIRE_FALSE(default_pin.GetLocked());

    REQUIRE(configured_pin.direction == ax::NodeEditor::PinKind::Output);
    REQUIRE(configured_pin.base_rate == FractionalNumber(45, 2));
    REQUIRE(configured_pin.GetLocked());
}

TEST_CASE("setting the existing lock state is a no-op", "[pin]")
{
    PinTestNode node(Node::Kind::Craft);
    Pin* unchanged = node.AddPin(ax::NodeEditor::PinKind::Input, false);
    Pin* inconsistent_sibling = node.AddPin(ax::NodeEditor::PinKind::Output, true);

    unchanged->SetLocked(false);

    REQUIRE_FALSE(unchanged->GetLocked());
    REQUIRE(inconsistent_sibling->GetLocked());
}

TEST_CASE("linked pins propagate lock changes in both directions", "[pin]")
{
    PinTestNode source(Node::Kind::Logistics, 1);
    PinTestNode destination(Node::Kind::Sink, 2);
    Pin* output = source.AddPin(ax::NodeEditor::PinKind::Output);
    Pin* input = destination.AddPin(ax::NodeEditor::PinKind::Input);
    Link link(ax::NodeEditor::LinkId(200), output, input);
    output->links.push_back(&link);
    input->links.push_back(&link);
    output->current_rate = FractionalNumber(17, 3);
    input->current_rate = FractionalNumber(17, 3);

    output->SetLocked(true);
    REQUIRE(output->GetLocked());
    REQUIRE(input->GetLocked());

    input->SetLocked(false);
    REQUIRE_FALSE(input->GetLocked());
    REQUIRE_FALSE(output->GetLocked());
    REQUIRE(output->SoleLink() == &link);
    REQUIRE(input->SoleLink() == &link);
    REQUIRE(output->current_rate == FractionalNumber(17, 3));
    REQUIRE(input->current_rate == FractionalNumber(17, 3));
}

TEST_CASE("craft group and game splitter synchronize every pin lock", "[pin]")
{
    const std::array<Node::Kind, 3> kinds{
        Node::Kind::Craft,
        Node::Kind::Group,
        Node::Kind::GameSplitter,
    };

    for (const Node::Kind kind : kinds)
    {
        CAPTURE(static_cast<int>(kind));
        PinTestNode node(kind);
        Pin* input_0 = node.AddPin(ax::NodeEditor::PinKind::Input);
        Pin* input_1 = node.AddPin(ax::NodeEditor::PinKind::Input);
        Pin* output_0 = node.AddPin(ax::NodeEditor::PinKind::Output);
        Pin* output_1 = node.AddPin(ax::NodeEditor::PinKind::Output);

        output_1->SetLocked(true);
        REQUIRE(input_0->GetLocked());
        REQUIRE(input_1->GetLocked());
        REQUIRE(output_0->GetLocked());
        REQUIRE(output_1->GetLocked());

        input_0->SetLocked(false);
        REQUIRE_FALSE(input_0->GetLocked());
        REQUIRE_FALSE(input_1->GetLocked());
        REQUIRE_FALSE(output_0->GetLocked());
        REQUIRE_FALSE(output_1->GetLocked());
    }
}

TEST_CASE("terminal node kinds only change the selected unlinked pin", "[pin]")
{
    const std::array<Node::Kind, 3> kinds{
        Node::Kind::Sink,
        Node::Kind::Extractor,
        Node::Kind::Logistics,
    };

    for (const Node::Kind kind : kinds)
    {
        CAPTURE(static_cast<int>(kind));
        PinTestNode node(kind);
        Pin* selected = node.AddPin(ax::NodeEditor::PinKind::Input);
        Pin* sibling = node.AddPin(ax::NodeEditor::PinKind::Output);

        selected->SetLocked(true);

        REQUIRE(selected->GetLocked());
        REQUIRE_FALSE(sibling->GetLocked());
    }
}

/// @test   A Pin starts with no link, accumulates them, and SoleLink() only yields a Link when
///         there is exactly one - it reports nullptr for a pin that fans out.
/// @covers Pin::links, Pin::SoleLink. Guards the invariant the snapshot and save-import code
///         relies on: there a pin never carries more than one link, and SoleLink() is the honest
///         accessor for that. A caller that must handle fan-out has to iterate `links` instead,
///         and SoleLink() returning nullptr is what forces it to.
TEST_CASE("pin links start empty and SoleLink reflects arity", "[pin]")
{
    PinTestNode producer(Node::Kind::Craft, 1);
    PinTestNode consumer_a(Node::Kind::Craft, 2);
    PinTestNode consumer_b(Node::Kind::Craft, 3);

    Pin* out = producer.AddPin(ax::NodeEditor::PinKind::Output);
    Pin* in_a = consumer_a.AddPin(ax::NodeEditor::PinKind::Input);
    Pin* in_b = consumer_b.AddPin(ax::NodeEditor::PinKind::Input);

    REQUIRE(out->links.empty());
    REQUIRE(out->SoleLink() == nullptr);

    Link link_a(ax::NodeEditor::LinkId(200), out, in_a);
    out->links.push_back(&link_a);
    in_a->links.push_back(&link_a);

    REQUIRE(out->links.size() == 1);
    REQUIRE(out->SoleLink() == &link_a);

    // Second consumer on the same output pin: the fan-out.
    Link link_b(ax::NodeEditor::LinkId(201), out, in_b);
    out->links.push_back(&link_b);
    in_b->links.push_back(&link_b);

    REQUIRE(out->links.size() == 2);
    REQUIRE(out->SoleLink() == nullptr); // no single "the" link: callers must iterate
    // Each consumer still sees exactly one link on its own side.
    REQUIRE(in_a->SoleLink() == &link_a);
    REQUIRE(in_b->SoleLink() == &link_b);
}

/// @test   Locking a pin that fans out does not lock its consumers, while a plain single-link
///         edge still propagates the lock in both directions.
/// @covers Pin::SetLocked on a multi-link pin. The lock means "this rate is fixed". Across a
///         sole edge that carries to the far end, because both ends must match. Across a fan-out
///         it does not: the lock fixes the pin's TOTAL, and says nothing about how that total
///         divides between branches - so the branches must stay independently lockable.
TEST_CASE("the lock does not cross a fan-out", "[pin][multilink]")
{
    PinTestNode producer(Node::Kind::Logistics, 1);
    PinTestNode consumer_a(Node::Kind::Sink, 2);
    PinTestNode consumer_b(Node::Kind::Sink, 3);

    Pin* out = producer.AddPin(ax::NodeEditor::PinKind::Output);
    Pin* in_a = consumer_a.AddPin(ax::NodeEditor::PinKind::Input);
    Pin* in_b = consumer_b.AddPin(ax::NodeEditor::PinKind::Input);

    Link link_a(ax::NodeEditor::LinkId(200), out, in_a);
    out->links.push_back(&link_a);
    in_a->links.push_back(&link_a);
    Link link_b(ax::NodeEditor::LinkId(201), out, in_b);
    out->links.push_back(&link_b);
    in_b->links.push_back(&link_b);

    out->SetLocked(true);

    REQUIRE(out->GetLocked());
    REQUIRE_FALSE(in_a->GetLocked()); // the lock stopped at the fanning pin
    REQUIRE_FALSE(in_b->GetLocked());

    // Locking one branch does not drag the other one along either.
    in_a->SetLocked(true);
    REQUIRE(in_a->GetLocked());
    REQUIRE_FALSE(in_b->GetLocked());

    // A sole edge still propagates, exactly as before this feature.
    PinTestNode lone_producer(Node::Kind::Logistics, 4);
    PinTestNode lone_consumer(Node::Kind::Sink, 5);
    Pin* lone_out = lone_producer.AddPin(ax::NodeEditor::PinKind::Output);
    Pin* lone_in = lone_consumer.AddPin(ax::NodeEditor::PinKind::Input);
    Link lone(ax::NodeEditor::LinkId(202), lone_out, lone_in);
    lone_out->links.push_back(&lone);
    lone_in->links.push_back(&lone);

    lone_out->SetLocked(true);
    REQUIRE(lone_in->GetLocked());
}

TEST_CASE("custom splitter lock constraints preserve a solvable state", "[pin]")
{
    CheckOrganizerConstraints(Node::Kind::CustomSplitter);
}

TEST_CASE("merger lock constraints mirror custom splitter constraints", "[pin]")
{
    CheckOrganizerConstraints(Node::Kind::Merger);
}
