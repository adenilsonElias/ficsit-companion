#include <catch2/catch_test_macros.hpp>

#include "domain/gamedata/building.hpp"
#include "domain/gamedata/recipe.hpp"

#include <string>
#include <vector>

TEST_CASE("Item and CountedItem retain recipe-domain values", "[recipe]")
{
    const Item item("Synthetic Iron Ore", "", 17);
    CountedItem counted(&item, FractionalNumber(45, 2));

    REQUIRE(item.name == "Synthetic Iron Ore");
    REQUIRE(item.new_line_name == "Synthetic\nIron\nOre");
    REQUIRE(item.sink_value == 17);
    REQUIRE(counted.item == &item);
    REQUIRE(counted.quantity.GetNumerator() == 45);
    REQUIRE(counted.quantity.GetDenominator() == 2);
}

TEST_CASE("Recipe construction retains ingredients and metadata", "[recipe]")
{
    const Building building(
        "Synthetic Constructor", FractionalNumber(0, 1), 4.0, 1.6, 2.0, false);
    const Item input("Synthetic Input", "", 1);
    const Item output("Synthetic Output", "", 2);
    const std::vector<CountedItem> inputs{
        CountedItem(&input, FractionalNumber(30, 1)),
    };
    const std::vector<CountedItem> outputs{
        CountedItem(&output, FractionalNumber(20, 1)),
    };

    const Recipe standard(
        inputs, outputs, &building, false, 4.5, "Synthetic Recipe", true);
    const Recipe alternate(
        inputs, outputs, &building, true, 7.25, "Synthetic Alternate");

    REQUIRE(standard.ins.size() == 1);
    REQUIRE(standard.ins[0].item == &input);
    REQUIRE(standard.ins[0].quantity == FractionalNumber(30, 1));
    REQUIRE(standard.outs.size() == 1);
    REQUIRE(standard.outs[0].item == &output);
    REQUIRE(standard.outs[0].quantity == FractionalNumber(20, 1));
    REQUIRE(standard.building == &building);
    REQUIRE_FALSE(standard.alternate);
    REQUIRE(standard.power == 4.5);
    REQUIRE(standard.name == "Synthetic Recipe");
    REQUIRE(standard.display_name == "Synthetic Recipe");
    REQUIRE(standard.is_spoiler);

    REQUIRE(alternate.building == &building);
    REQUIRE(alternate.alternate);
    REQUIRE(alternate.power == 7.25);
    REQUIRE(alternate.name == "Synthetic Alternate");
    REQUIRE(alternate.display_name == "*Synthetic Alternate");
    REQUIRE_FALSE(alternate.is_spoiler);
}

TEST_CASE("Recipe FindInName is case insensitive and returns string positions", "[recipe]")
{
    const Recipe recipe(
        {}, {}, nullptr, false, 0.0, "Reinforced Iron Plate");

    REQUIRE(recipe.FindInName("Reinforced Iron Plate") == 0);
    REQUIRE(recipe.FindInName("FoRcEd IrOn") == 4);
    REQUIRE(recipe.FindInName("IRON") == 11);
    REQUIRE(recipe.FindInName(" Plate") == 15);
    REQUIRE(recipe.FindInName("") == 0);
    REQUIRE(recipe.FindInName("iron rod") == std::string::npos);
}

TEST_CASE("Recipe FindInName handles an empty recipe name", "[recipe]")
{
    const Recipe recipe({}, {}, nullptr, false, 0.0);

    REQUIRE(recipe.FindInName("") == 0);
    REQUIRE(recipe.FindInName("anything") == std::string::npos);
}

TEST_CASE("Recipe FindInIngredients searches inputs and outputs case insensitively",
          "[recipe]")
{
    const Item component("Alpha Component", "", 1);
    const Item later_iron("Later Iron", "", 2);
    const Item iron("Iron", "", 3);
    const Item byproduct("Synthetic Byproduct", "", 4);
    const Recipe recipe(
        {
            CountedItem(&component, FractionalNumber(1, 1)),
            CountedItem(&later_iron, FractionalNumber(2, 1)),
        },
        {
            CountedItem(&iron, FractionalNumber(3, 1)),
            CountedItem(&byproduct, FractionalNumber(4, 1)),
        },
        nullptr, false, 0.0, "Ingredient Search");

    REQUIRE(recipe.FindInIngredients("ALPHA") == 0);
    REQUIRE(recipe.FindInIngredients("component") == 6);
    REQUIRE(recipe.FindInIngredients("IRON") == 0);
    REQUIRE(recipe.FindInIngredients("product") == 12);
    REQUIRE(recipe.FindInIngredients("") == 0);
    REQUIRE(recipe.FindInIngredients("copper") == std::string::npos);
}

TEST_CASE("Recipe FindInIngredients returns no match when there are no ingredients",
          "[recipe]")
{
    const Recipe recipe({}, {}, nullptr, false, 0.0, "Empty Ingredients");

    REQUIRE(recipe.FindInIngredients("") == std::string::npos);
    REQUIRE(recipe.FindInIngredients("anything") == std::string::npos);
}
