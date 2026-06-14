#include "domain/node_data_resolver.hpp"

#include "domain/game_data.hpp"
#include "domain/recipe.hpp"

#include <algorithm>

const Recipe* GameDataResolver::FindRecipe(const std::string& name) const
{
    const auto it = std::find_if(Data::Recipes().begin(), Data::Recipes().end(),
        [&name](const std::unique_ptr<Recipe>& recipe) { return recipe->name == name; });
    return it != Data::Recipes().end() ? it->get() : nullptr;
}

const Item* GameDataResolver::FindItem(const std::string& name) const
{
    const auto it = Data::Items().find(name);
    return it != Data::Items().end() ? it->second.get() : nullptr;
}
