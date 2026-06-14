#pragma once

#include <string>

struct Recipe;
struct Item;

/// @brief Resolves recipe / item names to game-data pointers during node
/// deserialization. Injecting this (instead of calling the global Data::
/// singleton inside node constructors) lets node (de)serialization run in unit
/// tests against a fabricated data set.
struct INodeDataResolver
{
    virtual ~INodeDataResolver() = default;

    /// @brief Recipe with this name, or nullptr if unknown.
    virtual const Recipe* FindRecipe(const std::string& name) const = 0;
    /// @brief Item with this display name, or nullptr if unknown.
    virtual const Item* FindItem(const std::string& name) const = 0;
};

/// @brief Production resolver backed by the global game data (Data::Recipes /
/// Data::Items). Behaviorally identical to the inline lookups it replaces.
class GameDataResolver : public INodeDataResolver
{
public:
    const Recipe* FindRecipe(const std::string& name) const override;
    const Item* FindItem(const std::string& name) const override;
};
