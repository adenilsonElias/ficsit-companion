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
    REQUIRE(default_pin.link == nullptr);
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
    output->link = &link;
    input->link = &link;
    output->current_rate = FractionalNumber(17, 3);
    input->current_rate = FractionalNumber(17, 3);

    output->SetLocked(true);
    REQUIRE(output->GetLocked());
    REQUIRE(input->GetLocked());

    input->SetLocked(false);
    REQUIRE_FALSE(input->GetLocked());
    REQUIRE_FALSE(output->GetLocked());
    REQUIRE(output->link == &link);
    REQUIRE(input->link == &link);
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

TEST_CASE("custom splitter lock constraints preserve a solvable state", "[pin]")
{
    CheckOrganizerConstraints(Node::Kind::CustomSplitter);
}

TEST_CASE("merger lock constraints mirror custom splitter constraints", "[pin]")
{
    CheckOrganizerConstraints(Node::Kind::Merger);
}
