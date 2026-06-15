# `.sav` Import - Energy Generators

## Goal

Import fuel-consuming power generators from a Satisfactory `.sav` into the production modeler without adding a new generator node type. The modeler already represents generators through existing `Power (...)` recipes in `assets/satisfactory.json`, so imported generators should become normal `CraftNode`s using those recipes.

## Scope

In scope:

- Coal, Fuel, Biomass, and Nuclear generator actors that can be mapped to an existing `Power (...)` recipe.
- Solid/input belt and output belt connections already handled by the current belt importer.
- Clock rate import through the existing `mCurrentPotential` path.
- Warning and skipping generators whose active fuel recipe cannot be resolved.

Out of scope:

- Pipe topology. Water and liquid/gas fuel pipe connections are not wired in this phase.
- Power lines, power circuits, priority switches, and grid membership.
- Placeholder generator nodes with unknown fuel.
- Geothermal generators unless they can be represented cleanly by existing data without fuel inputs.

## Architecture

The implementation stays on the current `.sav` import path:

1. `tools/sav_import/wrapper.js` recognizes generator actor classes in `classifyBuilding`.
2. The wrapper resolves the generator's active fuel item from save data.
3. If the fuel maps to a known generator recipe name, the wrapper emits the building as `kind: "manufacturer"` with `recipe_name: "Power (<fuel>)"`.
4. `SavImport::ParseWrapperJson` and `SavImport::BuildGraph` continue to use the existing manufacturer path and validate the recipe against loaded game data.
5. `BuildGraph` creates a `CraftNode`, applies clock rate, and wires any belt connections the current belt reducer can see.

This avoids adding a `BuildingKind::Generator` or `GeneratorNode` because no separate behavior is needed yet. The recipe controls the pins, consumption rates, waste outputs, and power display using the same code used for any other recipe-backed building.

## Fuel Resolution

The wrapper should resolve only explicit, trustworthy fuel signals from the save. Candidate sources include the generator's current fuel descriptor/property and fuel inventory contents, depending on what the parser exposes for each actor type.

The mapping is exact:

- `Coal` -> `Power (Coal)`
- `Compacted Coal` -> `Power (Compacted Coal)`
- `Petroleum Coke` -> `Power (Petroleum Coke)`
- `Fuel` -> `Power (Fuel)`
- `Turbofuel` -> `Power (Turbofuel)`
- `Liquid Biofuel` -> `Power (Liquid Biofuel)`
- `Rocket Fuel` -> `Power (Rocket Fuel)`
- `Ionized Fuel` -> `Power (Ionized Fuel)`
- `Leaves`, `Wood`, `Mycelia`, `Biomass`, `Solid Biofuel`, `Packaged Liquid Biofuel` -> matching biomass `Power (...)` recipes
- `Uranium Fuel Rod`, `Plutonium Fuel Rod`, `Ficsonium Fuel Rod` -> matching nuclear `Power (...)` recipes

If no exact recipe name can be resolved, the wrapper skips the generator with a warning. If a recipe name is emitted but is not found in `Data::Recipes()`, C++ skips it through the existing manufacturer warning path. The importer must not guess from connected belts or downstream demand.

## Connections

Belts are handled by the existing belt-chain reducer:

- Solid fuel belts can connect to generator input pins when the generator recipe has that item input.
- Nuclear waste belts can connect from generator output pins when the selected nuclear recipe outputs waste.
- Coal and nuclear water inputs are recipe pins but pipe connections remain unwired until pipe import is implemented.
- Fuel generator liquid/gas fuel inputs are recipe pins but pipe connections remain unwired until pipe import is implemented.

This means imported generator nodes may have unconnected pipe-dependent inputs. That is expected for this phase and should not block importing the node when the fuel recipe is known.

## Warnings And Errors

Warnings should be concise and actionable:

- `Skipping generator <id>: unresolved fuel recipe`
- Existing C++ warning for a mapped generator whose `Power (...)` recipe is not found.

Existing C++ manufacturer warnings can cover missing recipe lookup once the wrapper emits a recipe name. Wrapper-side warnings are still useful when a generator is skipped before C++ sees it.

## Testing

Add focused tests at both boundaries:

- JS wrapper/core tests for generator class detection and fuel-to-recipe mapping.
- C++ `BuildGraph` test proving a parsed generator-like manufacturer with `recipe_name: "Power (Coal)"` becomes a `CraftNode` with the expected input item and imported clock.
- C++ or wrapper test proving an unresolved generator is skipped with a warning and does not create a placeholder building.

No pipe wiring tests are required in this phase.
