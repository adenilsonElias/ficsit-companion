#include "domain/solver/rate_solver_internal.hpp"
#include "domain/gamedata/building.hpp"
#include "domain/nodes/node.hpp"
#include "domain/graph/pin.hpp"
#include "domain/gamedata/recipe.hpp"

#include <cstddef>

namespace rate_solver_detail
{
    VariableMapping AssignVariables(const SeedResult& seed)
    {
        VariableMapping vars;

        // For each updated pin, the associated variable with a ratio
        // The ratio allows us to use only one variable/equation per craft node

        // Lambda function to create a variable for a pin
        // For nodes sharing one variable for all pins, will link the variable to all pins
        auto create_variable = [&](const Pin* pin) {
            // Pin already associated to a variable
            if (vars.associated_variable_index.find(pin) != vars.associated_variable_index.end())
            {
                return;
            }
            // No variable yet for this pin, create a new one
            switch (pin->node->GetKind())
            {
            // Craft/Group/GameSplitter, only one variable for all the pins
            case Node::Kind::Craft:
            case Node::Kind::Group:
            case Node::Kind::GameSplitter:
                for (auto& p : pin->node->ins)
                {
                    // For craft/group, the ratio is the base rate of the pin
                    if (pin->node->IsPowered())
                    {
                        vars.associated_variable_index.insert({ p.get(), { vars.num_variables, p->base_rate } });
                    }
                    // For GameSplitter, the variable is the rate of the input
                    else if (pin->node->IsGameSplitter())
                    {
                        vars.associated_variable_index.insert({ p.get(), { vars.num_variables, 1 } });
                    }
                }
                for (auto& p : pin->node->outs)
                {
                    // For group, the ratio is the base rate of the pin
                    if (pin->node->IsGroup())
                    {
                        vars.associated_variable_index.insert({ p.get(), { vars.num_variables, p->base_rate } });
                    }
                    // For craft, we need to deal with somersloop
                    else if (pin->node->IsCraft())
                    {
                        const CraftNode* craft_node = static_cast<CraftNode*>(p->node);
                        vars.associated_variable_index.insert({ p.get(), { vars.num_variables, p->base_rate * (1 + (craft_node->num_somersloop * craft_node->recipe->building->somersloop_mult)) } });
                    }
                    // For GameSplitter, the variable is the rate of the input divided by the number of outs
                    else if (pin->node->IsGameSplitter())
                    {
                        vars.associated_variable_index.insert({ p.get(), { vars.num_variables, FractionalNumber(1, pin->node->outs.size()) } });
                    }
                }
                break;
            // Extractor: only outputs, one variable for the node. Rate ratio is
            // the live base x purity (mirrors how ExtractorNode::UpdateRate works).
            case Node::Kind::Extractor:
                for (auto& p : pin->node->outs)
                {
                    const ExtractorNode* ex = static_cast<const ExtractorNode*>(p->node);
                    vars.associated_variable_index.insert({ p.get(), { vars.num_variables, ex->GetBaseRate() * ex->GetPurityMultiplier() } });
                }
                break;
            // For Merger, CustomSplitter and Sink, we need one variable per pin
            case Node::Kind::CustomSplitter:
            case Node::Kind::Merger:
            case Node::Kind::Sink:
            case Node::Kind::Logistics:
                vars.associated_variable_index.insert({ pin, { vars.num_variables, 1 } });
                break;
            }
            // Increase the variable index
            vars.num_variables += 1;
        };

        // Create first the variables related to strongly constrained pins (all but the overflow/secondary pins of merger/customsplitter)
        for (const Pin* pin : seed.relevant_pins)
        {
            switch (pin->node->GetKind())
            {
            case Node::Kind::CustomSplitter:
                if (pin->direction == ax::NodeEditor::PinKind::Input || seed.multi_pin_constrained.find(pin) != seed.multi_pin_constrained.end())
                {
                    create_variable(pin);
                }
                break;
            case Node::Kind::Merger:
                if (pin->direction == ax::NodeEditor::PinKind::Output || seed.multi_pin_constrained.find(pin) != seed.multi_pin_constrained.end())
                {
                    create_variable(pin);
                }
                break;
            case Node::Kind::Craft:
            case Node::Kind::Group:
            case Node::Kind::GameSplitter:
            case Node::Kind::Sink:
            case Node::Kind::Extractor:
            case Node::Kind::Logistics:
                create_variable(pin);
                break;
            }
        }

        // Then create variables for the remaining pins (this way they will be the last variables and used later as pivot when solving the equations)
        for (const Pin* pin : seed.relevant_pins)
        {
            create_variable(pin);
        }

        // One variable per relevant link, after every pin variable so the existing pivot
        // ordering is untouched. These map to no pin: reversed_variable_map stays null at their
        // index, exactly like the route totals allocated below.
        for (const Link* l : seed.relevant_links)
        {
            vars.link_variable_index[l] = vars.num_variables;
            vars.variable_of_link[vars.num_variables] = l;
            vars.num_variables += 1;
        }

        // Allocate one auxiliary "total" variable T per active route group, after
        // every pin variable (so existing pivot ordering is untouched). T maps to no
        // pin: it is skipped by the negative-rate check and rate-assignment loops,
        // and reversed_variable_map[T] stays null.
        vars.group_total_index.resize(seed.active_groups.size());
        // For the rare case T itself is a free variable (a totally underdetermined
        // group), fall back to the group's current supply total to avoid touching a
        // null pin in the free-variable resolver.
        for (size_t gi = 0; gi < seed.active_groups.size(); ++gi)
        {
            vars.group_total_index[gi] = vars.num_variables;
            FractionalNumber supply_total(0, 1);
            for (const Pin* p : seed.active_groups[gi].supply) supply_total += p->current_rate;
            vars.group_total_default[vars.num_variables] = supply_total;
            vars.num_variables += 1;
        }
        // Reverse lookup: which group/side a pin belongs to (for ratio resolution).
        for (size_t gi = 0; gi < seed.active_groups.size(); ++gi)
        {
            for (const Pin* p : seed.active_groups[gi].supply) vars.group_membership[p] = { gi, true };
            for (const Pin* p : seed.active_groups[gi].demand) vars.group_membership[p] = { gi, false };
        }

        // Map a variable index to a pin pointer (for variables linked to multiple pins, just select one of them)
        vars.reversed_variable_map.resize(vars.num_variables, nullptr);
        for (const auto& [p, pp] : vars.associated_variable_index)
        {
            vars.reversed_variable_map[pp.first] = p;
        }

        return vars;
    }
}
