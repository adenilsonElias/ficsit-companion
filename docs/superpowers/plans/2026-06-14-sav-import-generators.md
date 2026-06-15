# `.sav` Import Energy Generators Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Import `.sav` power generators into the modeler as existing recipe-backed `CraftNode`s when their active fuel recipe can be resolved.

**Architecture:** Keep generators on the current manufacturer import path. Move generator class/fuel mapping helpers into `wrapper_core.js` for unit testing, then have `wrapper.js` classify generator actors and emit `kind: "manufacturer"` with `recipe_name: "Power (<fuel>)"` only when fuel resolution succeeds. C++ needs no new node type; add tests to lock existing `BuildGraph` behavior for `Power (...)` recipes.

**Tech Stack:** Node.js `node:test` for wrapper helper tests, C++ Catch2 for importer graph tests, existing CMake target `fc-tests`.

---

## File Structure

- Modify: `ficsit-companion/tools/sav_import/wrapper_core.js`
  - Add pure generator helper functions: class detection, fuel-to-recipe mapping, inventory stack item extraction, active fuel resolution.
- Modify: `ficsit-companion/tools/sav_import/test/wrapper_core.test.js`
  - Add unit tests for generator recipe mapping and unresolved fuel behavior.
- Modify: `ficsit-companion/tools/sav_import/wrapper.js`
  - Classify supported generator actor classes as `generator`.
  - Resolve generator fuel and emit supported generators as recipe-backed manufacturers.
  - Skip unresolved generators with a warning.
- Modify: `ficsit-companion/tests/test_sav_import.cpp`
  - Add a focused C++ test proving `Power (Coal)` imports as a `CraftNode` with the expected fuel and water pins and imported clock.
- Optional progress note: append to `docs/superpowers/plans/2026-06-14-sav-import-generators-PROGRESS.md`
  - Use this instead of git commits; the user explicitly said no git.

## Task 1: Add Tested Generator Mapping Helpers

**Files:**
- Modify: `ficsit-companion/tools/sav_import/wrapper_core.js`
- Modify: `ficsit-companion/tools/sav_import/test/wrapper_core.test.js`

- [ ] **Step 1: Write failing JS tests for generator helpers**

Append this to `ficsit-companion/tools/sav_import/test/wrapper_core.test.js`:

```js
test("generator class names are recognized without treating power lines as generators", () => {
    const {
        classifyGeneratorClass,
    } = require("../wrapper_core");

    assert.equal(classifyGeneratorClass("Build_GeneratorCoal_C"), "generator");
    assert.equal(classifyGeneratorClass("/Game/FactoryGame/Buildable/Factory/GeneratorFuel/Build_GeneratorFuel.Build_GeneratorFuel_C"), "generator");
    assert.equal(classifyGeneratorClass("Build_GeneratorNuclear_C"), "generator");
    assert.equal(classifyGeneratorClass("Build_BiomassGenerator_C"), "generator");
    assert.equal(classifyGeneratorClass("Build_PowerLine_C"), "");
});

test("generator fuel item maps to exact Power recipe names", () => {
    const {
        generatorRecipeForFuel,
    } = require("../wrapper_core");

    assert.equal(generatorRecipeForFuel("Coal"), "Power (Coal)");
    assert.equal(generatorRecipeForFuel("Compacted Coal"), "Power (Compacted Coal)");
    assert.equal(generatorRecipeForFuel("Petroleum Coke"), "Power (Petroleum Coke)");
    assert.equal(generatorRecipeForFuel("Fuel"), "Power (Fuel)");
    assert.equal(generatorRecipeForFuel("Turbofuel"), "Power (Turbofuel)");
    assert.equal(generatorRecipeForFuel("Liquid Biofuel"), "Power (Liquid Biofuel)");
    assert.equal(generatorRecipeForFuel("Rocket Fuel"), "Power (Rocket Fuel)");
    assert.equal(generatorRecipeForFuel("Ionized Fuel"), "Power (Ionized Fuel)");
    assert.equal(generatorRecipeForFuel("Solid Biofuel"), "Power (Solid Biofuel)");
    assert.equal(generatorRecipeForFuel("Uranium Fuel Rod"), "Power (Uranium Fuel Rod)");
    assert.equal(generatorRecipeForFuel("Plutonium Fuel Rod"), "Power (Plutonium Fuel Rod)");
    assert.equal(generatorRecipeForFuel("Ficsonium Fuel Rod"), "Power (Ficsonium Fuel Rod)");
    assert.equal(generatorRecipeForFuel("Iron Ore"), "");
    assert.equal(generatorRecipeForFuel(""), "");
});

test("generator active fuel resolves from descriptor property before inventory", () => {
    const {
        resolveGeneratorFuelItem,
    } = require("../wrapper_core");

    const entity = {
        properties: {
            mFuelClass: { value: { pathName: "/Game/FactoryGame/Resource/Parts/Coal/Desc_Coal.Desc_Coal_C" } },
        },
    };

    assert.equal(resolveGeneratorFuelItem(entity, new Map(), itemDisplayName), "Coal");
});

test("generator active fuel resolves from fuel inventory when descriptor is absent", () => {
    const {
        resolveGeneratorFuelItem,
    } = require("../wrapper_core");

    const entity = {
        components: [{ value: { pathName: "Generator.FuelInventory" } }],
        properties: {},
    };
    const inventory = {
        instanceName: "Generator.FuelInventory",
        properties: {
            mInventoryStacks: {
                values: [{
                    properties: {
                        Item: {
                            value: {
                                itemReference: {
                                    pathName: "/Game/FactoryGame/Resource/Parts/Biofuel/Desc_Biofuel.Desc_Biofuel_C",
                                },
                            },
                        },
                    },
                }],
            },
        },
    };
    const objectsByPath = new Map([
        ["Generator.FuelInventory", inventory],
        ["FuelInventory", inventory],
    ]);

    assert.equal(resolveGeneratorFuelItem(entity, objectsByPath, itemDisplayName), "Solid Biofuel");
});
```

Also add this small local helper near the top of the same test file, after `addAliases`:

```js
function stripClassWrapForTest(cls) {
    const dotIdx = cls.lastIndexOf(".");
    let s = dotIdx >= 0 ? cls.substring(dotIdx + 1) : cls;
    if (s.endsWith("_C")) s = s.substring(0, s.length - 2);
    return s;
}

function splitCamelForTest(s) {
    return s
        .replace(/([a-z0-9])([A-Z])/g, "$1 $2")
        .replace(/([A-Z]+)([A-Z][a-z])/g, "$1 $2")
        .replace(/_+/g, " ")
        .trim();
}

function itemDisplayName(itemClass) {
    if (!itemClass) return "";
    const overrides = {
        Desc_Biofuel: "Solid Biofuel",
    };
    let s = stripClassWrapForTest(itemClass);
    if (overrides[s]) return overrides[s];
    if (s.startsWith("Desc_")) s = s.substring("Desc_".length);
    return splitCamelForTest(s);
}
```

- [ ] **Step 2: Run JS tests and verify they fail**

Run:

```powershell
npm test --prefix ficsit-companion/tools/sav_import
```

Expected: FAIL because `classifyGeneratorClass`, `generatorRecipeForFuel`, and `resolveGeneratorFuelItem` are not exported yet.

- [ ] **Step 3: Implement generator helpers**

Modify `ficsit-companion/tools/sav_import/wrapper_core.js` by adding these functions before `module.exports`:

```js
function stripClassWrap(cls) {
    const dotIdx = cls.lastIndexOf(".");
    let s = dotIdx >= 0 ? cls.substring(dotIdx + 1) : cls;
    if (s.endsWith("_C")) s = s.substring(0, s.length - 2);
    return s;
}

function classifyGeneratorClass(buildingClass) {
    const s = stripClassWrap(buildingClass);
    if (s.startsWith("Build_Generator") ||
        s.startsWith("Build_BiomassGenerator")) {
        return "generator";
    }
    return "";
}

const GENERATOR_RECIPE_BY_FUEL = new Map([
    ["Coal", "Power (Coal)"],
    ["Compacted Coal", "Power (Compacted Coal)"],
    ["Petroleum Coke", "Power (Petroleum Coke)"],
    ["Fuel", "Power (Fuel)"],
    ["Turbofuel", "Power (Turbofuel)"],
    ["Liquid Biofuel", "Power (Liquid Biofuel)"],
    ["Rocket Fuel", "Power (Rocket Fuel)"],
    ["Ionized Fuel", "Power (Ionized Fuel)"],
    ["Leaves", "Power (Leaves)"],
    ["Wood", "Power (Wood)"],
    ["Mycelia", "Power (Mycelia)"],
    ["Biomass", "Power (Biomass)"],
    ["Solid Biofuel", "Power (Solid Biofuel)"],
    ["Packaged Liquid Biofuel", "Power (Packaged Liquid Biofuel)"],
    ["Uranium Fuel Rod", "Power (Uranium Fuel Rod)"],
    ["Plutonium Fuel Rod", "Power (Plutonium Fuel Rod)"],
    ["Ficsonium Fuel Rod", "Power (Ficsonium Fuel Rod)"],
]);

function generatorRecipeForFuel(fuelItemName) {
    return GENERATOR_RECIPE_BY_FUEL.get(fuelItemName || "") || "";
}

function readStringValue(value) {
    if (typeof value === "string") return value;
    if (value && typeof value === "object") {
        if (value.value && typeof value.value === "object") {
            if (typeof value.value.pathName === "string") return value.value.pathName;
            if (typeof value.value.PathName === "string") return value.value.PathName;
            if (typeof value.value.objectPath === "string") return value.value.objectPath;
            if (typeof value.value.ObjectPath === "string") return value.value.ObjectPath;
        }
        if (typeof value.pathName === "string") return value.pathName;
        if (typeof value.PathName === "string") return value.PathName;
        if (typeof value.objectPath === "string") return value.objectPath;
        if (typeof value.ObjectPath === "string") return value.ObjectPath;
        if (typeof value.value === "string") return value.value;
        if (typeof value.Value === "string") return value.Value;
    }
    return "";
}

function firstInventoryItem(entity, componentSuffix, objectsByPath, itemDisplayName) {
    const comps = entity.components || entity.Components || [];
    for (const ref of comps) {
        const cp = objectRefPath(ref);
        if (!cp || cp.split(".").pop() !== componentSuffix) continue;
        const c = findObjectByRefPaths(objectsByPath, [cp]);
        const stacks = c && c.properties && c.properties.mInventoryStacks;
        const vals = stacks && (stacks.values || stacks.value);
        if (!Array.isArray(vals)) return "";
        for (const stack of vals) {
            const ir = stack && stack.properties && stack.properties.Item
                && stack.properties.Item.value && stack.properties.Item.value.itemReference;
            const path = ir && ir.pathName;
            const name = itemDisplayName(path || "");
            if (name) return name;
        }
    }
    return "";
}

function resolveGeneratorFuelItem(entity, objectsByPath, itemDisplayName) {
    const props = entity.properties || entity.Properties;
    const descriptorProps = [
        "mFuelClass",
        "mCurrentFuelClass",
        "mCurrentFuel",
        "mFuelType",
        "mFuelTypeDescriptor",
    ];
    for (const propName of descriptorProps) {
        const name = itemDisplayName(readStringValue(findPropDeep(props, propName)));
        if (name) return name;
    }
    return firstInventoryItem(entity, "FuelInventory", objectsByPath, itemDisplayName)
        || firstInventoryItem(entity, "fuelInventory", objectsByPath, itemDisplayName)
        || firstInventoryItem(entity, "inventory", objectsByPath, itemDisplayName);
}
```

Then extend `module.exports` at the bottom of the same file:

```js
module.exports = {
    buildFloorHolePeerMap,
    followBeltChain,
    getComponentPort,
    classifyGeneratorClass,
    generatorRecipeForFuel,
    resolveGeneratorFuelItem,
};
```

- [ ] **Step 4: Run JS tests and verify they pass**

Run:

```powershell
npm test --prefix ficsit-companion/tools/sav_import
```

Expected: PASS for all `wrapper_core` tests.

- [ ] **Step 5: Record progress without git**

Append this line to `docs/superpowers/plans/2026-06-14-sav-import-generators-PROGRESS.md`:

```markdown
- Task 1 done: generator helper tests and pure mapping helpers added; JS tests pass.
```

## Task 2: Wire Generator Classification Into The Wrapper

**Files:**
- Modify: `ficsit-companion/tools/sav_import/wrapper.js`
- Modify: `ficsit-companion/tools/sav_import/test/wrapper_core.test.js`

- [ ] **Step 1: Write failing test for exact unresolved mapping warning text**

Append this test to `ficsit-companion/tools/sav_import/test/wrapper_core.test.js` to lock the unresolved case at the helper level:

```js
test("unrecognized generator fuel has no recipe mapping", () => {
    const {
        generatorRecipeForFuel,
    } = require("../wrapper_core");

    assert.equal(generatorRecipeForFuel("Alien Power Matrix"), "");
    assert.equal(generatorRecipeForFuel("Water"), "");
});
```

- [ ] **Step 2: Run JS tests and verify the new test passes before wrapper wiring**

Run:

```powershell
npm test --prefix ficsit-companion/tools/sav_import
```

Expected: PASS. This confirms the helper policy before integrating it into `wrapper.js`.

- [ ] **Step 3: Import generator helpers in `wrapper.js`**

Near the existing `const BeltCore = require("./wrapper_core");`, add:

```js
const {
    classifyGeneratorClass,
    generatorRecipeForFuel,
    resolveGeneratorFuelItem,
} = BeltCore;
```

- [ ] **Step 4: Classify generators before generic unknowns**

In `classifyBuilding(buildingClass)`, add this after the train-station checks and before splitter checks:

```js
    const generatorKind = classifyGeneratorClass(s);
    if (generatorKind) return generatorKind;
```

- [ ] **Step 5: Resolve generator recipes during building emission**

In the actor loop, after creating `entry` and after the existing extractor/station special cases, add:

```js
    if (kind === "generator") {
        const fuelItem = resolveGeneratorFuelItem(a, objectsByPath, itemDisplayName);
        const generatorRecipe = generatorRecipeForFuel(fuelItem);
        if (!generatorRecipe) {
            warnings.push("Skipping generator " + id + ": unresolved fuel recipe");
            continue;
        }
        entry.kind = "manufacturer";
        entry.recipe_name = generatorRecipe;
        entry.item_name = "";
    }
```

This must run before `buildings.push(entry);` so unresolved generators are skipped and resolved generators are emitted as normal manufacturers.

- [ ] **Step 6: Update wrapper header comment**

In the top file comment in `wrapper.js`, update the kind list to include generators as an input-only wrapper concept:

```js
 * one of {manufacturer, miner, splitter, smart_splitter, prog_splitter,
 * merger, sink, storage, truck_station, train_station}. Generator actors are
 * resolved to manufacturer entries with Power (...) recipes when their active
 * fuel is known.
```

- [ ] **Step 7: Run JS tests**

Run:

```powershell
npm test --prefix ficsit-companion/tools/sav_import
```

Expected: PASS.

- [ ] **Step 8: Record progress without git**

Append:

```markdown
- Task 2 done: wrapper classifies generator actors and emits resolved ones as Power recipe manufacturers; JS tests pass.
```

## Task 3: Lock C++ BuildGraph Behavior For Power Recipes

**Files:**
- Modify: `ficsit-companion/tests/test_sav_import.cpp`

- [ ] **Step 1: Write failing/guard C++ test**

Append this test to `ficsit-companion/tests/test_sav_import.cpp`:

```cpp
/// @test   A generator emitted by the JS wrapper as a normal manufacturer with
///         a Power (...) recipe imports as an ordinary CraftNode. This locks the
///         chosen design: no GeneratorNode or BuildingKind::Generator is needed
///         for this phase.
/// @covers SavImport::BuildGraph generator-as-manufacturer import.
TEST_CASE("BuildGraph imports a resolved generator recipe as a craft node", "[sav_import]")
{
    EnsureGameDataLoaded();

    SavImport::ParseResult parsed;
    parsed.ok = true;
    parsed.buildings.push_back(Manufacturer("coal_generator", "Power (Coal)", 0.0f));
    parsed.buildings.back().clock = 0.5;

    IdGen ids;
    SavImport::BuildOutput out;
    std::string err;
    REQUIRE(SavImport::BuildGraph(parsed, std::ref(ids), out, err));

    REQUIRE(out.nodes.size() == 1);
    REQUIRE(out.nodes[0]->IsCraft());

    const auto* generator = static_cast<const CraftNode*>(out.nodes[0].get());
    REQUIRE(generator->recipe != nullptr);
    REQUIRE(generator->recipe->name == "Power (Coal)");
    REQUIRE(generator->current_rate == FractionalNumber(1, 2));

    auto coal_pin = std::find_if(generator->ins.begin(), generator->ins.end(), [](const std::unique_ptr<Pin>& pin) {
        return pin->item != nullptr && pin->item->name == "Coal";
    });
    auto water_pin = std::find_if(generator->ins.begin(), generator->ins.end(), [](const std::unique_ptr<Pin>& pin) {
        return pin->item != nullptr && pin->item->name == "Water";
    });
    REQUIRE(coal_pin != generator->ins.end());
    REQUIRE(water_pin != generator->ins.end());
    REQUIRE(generator->outs.empty());
}
```

- [ ] **Step 2: Run the focused C++ test**

Run:

```powershell
cmake --build build --config Release --target fc-tests
ctest --test-dir build -C Release --output-on-failure -R sav_import
```

Expected: PASS. This test should pass with existing C++ code because the design reuses manufacturer/CraftNode behavior.

- [ ] **Step 3: Record progress without git**

Append:

```markdown
- Task 3 done: C++ BuildGraph test confirms resolved Power recipes import as CraftNodes; sav_import tests pass.
```

## Task 4: Real-Wrapper Smoke Check And Full Verification

**Files:**
- Read/verify: `ficsit-companion/tools/sav_import/package.json`
- Read/verify: `ficsit-companion/tests/test_sav_import.cpp`

- [ ] **Step 1: Run JS wrapper tests**

Run:

```powershell
npm test --prefix ficsit-companion/tools/sav_import
```

Expected: PASS.

- [ ] **Step 2: Run focused C++ importer tests**

Run:

```powershell
cmake --build build --config Release --target fc-tests
ctest --test-dir build -C Release --output-on-failure -R sav_import
```

Expected: PASS.

- [ ] **Step 3: Run all C++ tests if the focused suite passes**

Run:

```powershell
ctest --test-dir build -C Release --output-on-failure
```

Expected: PASS.

- [ ] **Step 4: Optional real-save parser check**

If a local `.sav` path is available, run:

```powershell
node ficsit-companion/tools/sav_import/wrapper.js "C:\path\to\save.sav"
```

Expected:

- The JSON contains generator actors as buildings with `"kind":"manufacturer"` and `"recipe_name":"Power (...)"` when fuel is resolved.
- The JSON warnings contain `Skipping generator <id>: unresolved fuel recipe` for unresolved generator actors.
- No pipe connections are emitted for generator water/fuel pipes.

- [ ] **Step 5: Record final progress without git**

Append:

```markdown
- Task 4 done: JS tests, focused C++ importer tests, and available full verification completed.
```

## Self-Review

**Spec coverage:**  
Generator actors map to existing `Power (...)` recipes in Task 2. Unknown fuel skips with warning in Task 2. No new C++ node type is introduced. Existing belt-only behavior is preserved. Pipe topology remains out of scope and is verified by omission.

**Placeholder scan:**  
The plan contains no TBD/TODO/fill-in steps. The optional real-save command requires a user-provided `.sav` path; that is explicitly optional and not required for completion.

**Type consistency:**  
Helper names are consistent across tests and implementation: `classifyGeneratorClass`, `generatorRecipeForFuel`, `resolveGeneratorFuelItem`. Wrapper integration uses the existing `entry.kind`, `entry.recipe_name`, and `warnings` variables already present in `wrapper.js`. C++ uses existing `SavImport::BuildingKind::Manufacturer`, `CraftNode`, and `FractionalNumber`.
