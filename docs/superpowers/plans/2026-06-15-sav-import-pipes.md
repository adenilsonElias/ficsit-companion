# Pipe & Fluid-Container Import Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

> **Progress / git note (project preference):** This repo's owner wants NO git
> operations (commit/branch) unless explicitly asked. Instead of committing,
> each task's final step marks completion in
> `docs/superpowers/plans/2026-06-15-sav-import-pipes-PROGRESS.md`. Create that
> file at the start (a simple checklist of the task names below).

**Goal:** Reconstruct fluid (pipe) connections between machines from a `.sav`
import and place fluid containers as nodes, without simulating pressure/head.

**Architecture:** Pipes are grouped by `mPipeNetworkID` (read from each pipe
connector) rather than chain-walked; `FGPipeNetwork` actors give each network's
fluid. The wrapper emits a `pipe_networks` array; the C++ importer turns each
network into either a direct producer→consumer link (1:1) or a synthetic
`PipeJunction` manifold node (N:M). Fluid containers become new `FluidBuffer` /
`IndustrialFluidBuffer` logistics nodes. Pumps/junctions/segments are never
nodes — they are absorbed by network-id grouping.

**Tech Stack:** Node.js (`tools/sav_import`, `node:test`), C++17
(`ficsit-companion`, Catch2), CMake.

**Spec:** `docs/superpowers/specs/2026-06-15-sav-import-pipes-design.md`

---

## File Structure

- `ficsit-companion/tools/sav_import/wrapper_core.js` — add pure helpers
  `pipeConnectorInfo(comp)` and `readPipeNetwork(actor)` (unit-tested).
- `ficsit-companion/tools/sav_import/test/wrapper_core.test.js` — tests for the
  two new helpers.
- `ficsit-companion/tools/sav_import/wrapper.js` — reclassify pipe plumbing,
  add fluid-container kinds, build the network→fluid map, scan machine pipe
  ports, emit `pipe_networks`.
- `ficsit-companion/include/infra/sav_import.hpp` — `PipeEndpoint`/`PipeNetwork`
  structs, `pipe_networks` field, new `BuildingKind` values.
- `ficsit-companion/include/domain/node.hpp` — new `LogisticsNode::Kind` values.
- `ficsit-companion/src/domain/logistics_node.cpp` — `GetDisplayName` cases.
- `ficsit-companion/src/infra/sav_import.cpp` — parse `pipe_networks`, build
  the new building kinds, and the manifold-wiring pass.
- `ficsit-companion/tests/test_sav_import.cpp` — C++ tests for parsing, fluid
  buffer placement, direct 1:1 wiring, N:M manifold, unresolved-fluid skip.
- `docs/save_game.md` — short pipe-topology section.

---

## Build & Test Commands (Windows)

- JS helper tests:
  `cd ficsit-companion/tools/sav_import && node --test test/wrapper_core.test.js`
- Build C++ tests:
  `cmake --build build --config Release --target fc-tests`
- Run a single Catch2 test by name:
  `./build/ficsit-companion/Release/fc-tests.exe "<TEST_CASE name>"`
- Run all sav-import tests:
  `./build/ficsit-companion/Release/fc-tests.exe "[sav_import]"`

(If `build/` does not exist yet: `cmake -DCMAKE_BUILD_TYPE=Release -S . -B build`.)

---

### Task 1: Wrapper pure helpers — `pipeConnectorInfo` and `readPipeNetwork`

**Files:**
- Modify: `ficsit-companion/tools/sav_import/wrapper_core.js`
- Test: `ficsit-companion/tools/sav_import/test/wrapper_core.test.js`

- [ ] **Step 1: Write the failing tests**

Append to `test/wrapper_core.test.js`:

```javascript
const {
    pipeConnectorInfo,
    readPipeNetwork,
} = require("../wrapper_core");

test("pipeConnectorInfo reads network id and infers direction from the name", () => {
    const inPort = {
        instanceName: "Refinery.PipeInputFactory",
        properties: { mPipeNetworkID: { value: 15 } },
    };
    const outPort = {
        instanceName: "Refinery.PipeOutputFactory",
        properties: { mPipeNetworkID: { value: 7 } },
    };
    const anyPort = {
        instanceName: "Tank.ConnectionAny0",
        properties: { mPipeNetworkID: { value: 13 } },
    };
    const bare = {
        instanceName: "WaterPump.FGPipeConnectionFactory",
        properties: { mPipeNetworkID: { value: 2 } },
    };
    assert.deepEqual(pipeConnectorInfo(inPort), { networkId: 15, dir: "in" });
    assert.deepEqual(pipeConnectorInfo(outPort), { networkId: 7, dir: "out" });
    assert.deepEqual(pipeConnectorInfo(anyPort), { networkId: 13, dir: "any" });
    assert.deepEqual(pipeConnectorInfo(bare), { networkId: 2, dir: "any" });
});

test("pipeConnectorInfo returns null without a network id (belt port / unconnected)", () => {
    const beltPort = { instanceName: "Refinery.Output1", properties: {} };
    assert.equal(pipeConnectorInfo(beltPort), null);
    assert.equal(pipeConnectorInfo(null), null);
});

test("readPipeNetwork extracts id and raw fluid class", () => {
    const net = {
        instanceName: "FGPipeNetwork_1",
        properties: {
            mPipeNetworkID: { value: 7 },
            mFluidDescriptor: {
                value: { pathName: "/Game/.../Desc_HeavyOilResidue.Desc_HeavyOilResidue_C" },
            },
        },
    };
    assert.deepEqual(readPipeNetwork(net), {
        id: 7,
        fluidClass: "/Game/.../Desc_HeavyOilResidue.Desc_HeavyOilResidue_C",
    });
});

test("readPipeNetwork returns null when id is missing", () => {
    assert.equal(readPipeNetwork({ properties: {} }), null);
});
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cd ficsit-companion/tools/sav_import && node --test test/wrapper_core.test.js`
Expected: FAIL — `pipeConnectorInfo is not a function` / `readPipeNetwork is not a function`.

- [ ] **Step 3: Implement the helpers**

In `wrapper_core.js`, add these functions (above `module.exports`). They reuse
the file's existing `normalizeObjectPath` and add a tiny int reader:

```javascript
function readIntPropValue(props, name) {
    if (!props) return null;
    let v;
    if (Array.isArray(props)) {
        const m = props.find(p => p && (p.name === name || p.Name === name));
        v = m ? (m.value !== undefined ? m.value : m.Value) : undefined;
    } else {
        v = props[name];
    }
    if (typeof v === "number") return v;
    if (v && typeof v === "object" && typeof v.value === "number") return v.value;
    return null;
}

// A pipe connector on a machine (FGPipeConnectionFactory) or on plumbing
// (FGPipeConnectionComponent). Connected pipe connectors carry mPipeNetworkID;
// belt connectors and unconnected pipe ports do not, so those return null.
// Direction is inferred from the component name suffix; ambiguous ports
// (ConnectionAny*, bare FGPipeConnectionFactory) return "any" and are resolved
// C++-side against the machine's recipe fluid pins.
function pipeConnectorInfo(comp) {
    if (!comp) return null;
    const props = comp.properties || comp.Properties;
    const networkId = readIntPropValue(props, "mPipeNetworkID");
    if (networkId === null) return null;
    const name = comp.instanceName || comp.InstanceName || "";
    const dot = name.lastIndexOf(".");
    const suffix = dot >= 0 ? name.substring(dot + 1) : name;
    let dir = "any";
    if (/PipeInput/i.test(suffix)) dir = "in";
    else if (/PipeOutput/i.test(suffix)) dir = "out";
    return { networkId, dir };
}

// An FGPipeNetwork actor: { id, fluidClass } or null. fluidClass is the raw
// Desc_* path; the caller maps it to a display name via itemDisplayName.
function readPipeNetwork(actor) {
    const props = actor && (actor.properties || actor.Properties);
    const id = readIntPropValue(props, "mPipeNetworkID");
    if (id === null) return null;
    let fluidClass = "";
    let fd;
    if (Array.isArray(props)) {
        const m = props.find(p => p && (p.name === "mFluidDescriptor" || p.Name === "mFluidDescriptor"));
        fd = m ? (m.value !== undefined ? m.value : m.Value) : undefined;
    } else {
        fd = props.mFluidDescriptor && (props.mFluidDescriptor.value !== undefined
            ? props.mFluidDescriptor.value : props.mFluidDescriptor.Value);
    }
    if (fd && typeof fd === "object") {
        fluidClass = normalizeObjectPath(fd.pathName || fd.PathName || "");
    }
    return { id, fluidClass };
}
```

Add both to `module.exports`:

```javascript
module.exports = {
    buildFloorHolePeerMap,
    followBeltChain,
    getComponentPort,
    stripClassWrap,
    firstInventoryItem,
    classifyGeneratorClass,
    generatorRecipeForFuel,
    resolveGeneratorFuelItem,
    pipeConnectorInfo,
    readPipeNetwork,
};
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cd ficsit-companion/tools/sav_import && node --test test/wrapper_core.test.js`
Expected: PASS (all tests, including the pre-existing belt ones).

- [ ] **Step 5: Mark Task 1 done in PROGRESS.md**

Tick "Task 1" in `docs/superpowers/plans/2026-06-15-sav-import-pipes-PROGRESS.md`.

---

### Task 2: Wrapper — reclassify plumbing, add containers, emit `pipe_networks`

**Files:**
- Modify: `ficsit-companion/tools/sav_import/wrapper.js`

- [ ] **Step 1: Reclassify pipe plumbing and add fluid-container kinds**

In `classifyBuilding` (around `wrapper.js:277`), replace the pipe/junction
lines. Find:

```javascript
    if (s.startsWith("Build_Pipeline")) return "pipe";
```

and the later:

```javascript
    if (s.startsWith("Build_PipelineJunction")) return "merger";
```

Replace the first with a block that covers all plumbing, and DELETE the
junction→merger line entirely:

```javascript
    // Pipe plumbing carries no production node; networks are reconstructed from
    // mPipeNetworkID instead (see pipe_networks below). Pumps/junctions/segments
    // are absorbed by network-id grouping, so none of them are emitted.
    if (s.startsWith("Build_Pipeline")
        || s.startsWith("Build_FoundationPassthrough_Pipe")) return "pipe";
    if (s.startsWith("Build_PipeStorageTank")) return "fluid_buffer";
    if (s.startsWith("Build_IndustrialTank")) return "industrial_fluid_buffer";
```

(The existing `if (kind === "pipe") continue;` in the actor loop already skips
plumbing. `Build_PipelineJunction*`, `Build_PipelinePump*`,
`Build_PipelineSupport*`, `Build_PipelineFlowIndicator*` all start with
`Build_Pipeline`, so the single prefix covers them.)

- [ ] **Step 2: Require the new helpers**

At the top of `wrapper.js`, extend the destructure of `BeltCore` (around
`wrapper.js:24`):

```javascript
const {
    stripClassWrap,
    firstInventoryItem,
    classifyGeneratorClass,
    generatorRecipeForFuel,
    resolveGeneratorFuelItem,
    pipeConnectorInfo,
    readPipeNetwork,
} = BeltCore;
```

- [ ] **Step 3: Build the pipe_networks block**

Add this block AFTER the belt diagnostics block and BEFORE the
`// Logistics extraction` section (i.e. before `wrapper.js:962`):

```javascript
// ---------------------------------------------------------------------------
// Pipe networks.
//
// Unlike belts, pipes are not chain-walked: every pipe connector carries an
// mPipeNetworkID and FGPipeNetwork actors map that id -> the fluid carried.
// We group each emitted building's pipe connectors by network id; pumps,
// junctions and segments are plumbing that shares the network id and are not
// emitted. The C++ side resolves producer/consumer direction per endpoint and
// wires a manifold. Topology documented in docs/save_game.md.
// ---------------------------------------------------------------------------
const pipeFluidById = new Map(); // network id -> fluid display name
for (const a of actors) {
    if (stripClassWrap(a.className || a.ClassName || a.typePath || "") !== "FGPipeNetwork") continue;
    const net = readPipeNetwork(a);
    if (!net) continue;
    pipeFluidById.set(net.id, itemDisplayName(net.fluidClass));
}

const pipeEndpointsByNet = new Map(); // network id -> [{ building, dir }]
for (const b of buildings) {
    const entity = findObjectByRefPaths(objectsByPath, [b.id]);
    if (!entity) continue;
    const compRefs = entity.components || entity.Components || [];
    for (const ref of compRefs) {
        const compPath = objectRefPath(ref);
        if (!compPath) continue;
        const comp = findObjectByRefPaths(objectsByPath, [compPath]);
        const info = pipeConnectorInfo(comp);
        if (!info) continue;
        if (!pipeEndpointsByNet.has(info.networkId)) pipeEndpointsByNet.set(info.networkId, []);
        pipeEndpointsByNet.get(info.networkId).push({ building: b.id, dir: info.dir });
    }
}

const pipe_networks = [];
for (const [id, endpoints] of pipeEndpointsByNet) {
    pipe_networks.push({
        id,
        fluid: pipeFluidById.get(id) || "",
        endpoints,
    });
}
warnings.push("[pipes] " + pipe_networks.length + " network(s) with machine endpoints");
```

- [ ] **Step 4: Add pipe_networks to the output object**

Find the final `const out = {` (around `wrapper.js:1211`) and add the field:

```javascript
const out = {
    version: 1,
    buildings,
    belts,
    warnings,
    logistics,
    pipe_networks,
};
```

- [ ] **Step 5: Smoke-run the wrapper on the example save**

Run: `cd ficsit-companion/tools/sav_import && node wrapper.js ../../../example_save.sav > /tmp/out.json && node -e "const o=require('/tmp/out.json'); console.log('networks', o.pipe_networks.length); for(const n of o.pipe_networks.slice(0,6)) console.log(n.id, n.fluid, 'eps='+n.endpoints.length);"`
Expected: prints several networks, e.g. `2 Water eps=17`, `7 Heavy Oil Residue eps=5`, with non-empty `fluid` names for Water / Crude Oil / Heavy Oil Residue / Fuel.

- [ ] **Step 6: Mark Task 2 done in PROGRESS.md**

---

### Task 3: C++ data model + `pipe_networks` parsing + new building kinds

**Files:**
- Modify: `ficsit-companion/include/infra/sav_import.hpp`
- Modify: `ficsit-companion/src/infra/sav_import.cpp`
- Test: `ficsit-companion/tests/test_sav_import.cpp`

- [ ] **Step 1: Write the failing parse test**

Append to `tests/test_sav_import.cpp` (before the final closing, in the global
test area):

```cpp
/// @covers SavImport::ParseWrapperJson pipe_networks parsing.
TEST_CASE("ParseWrapperJson reads pipe_networks endpoints and fluid", "[sav_import]")
{
    const std::string json = R"({
        "buildings": [],
        "belts": [],
        "pipe_networks": [
            { "id": 7, "fluid": "Heavy Oil Residue",
              "endpoints": [
                { "building": "ref_a", "dir": "out" },
                { "building": "ref_b", "dir": "in" }
              ] }
        ]
    })";

    SavImport::ParseResult parsed = SavImport::ParseWrapperJson(json);
    REQUIRE(parsed.ok);
    REQUIRE(parsed.pipe_networks.size() == 1);
    REQUIRE(parsed.pipe_networks[0].id == 7);
    REQUIRE(parsed.pipe_networks[0].fluid == "Heavy Oil Residue");
    REQUIRE(parsed.pipe_networks[0].endpoints.size() == 2);
    REQUIRE(parsed.pipe_networks[0].endpoints[0].building == "ref_a");
    REQUIRE(parsed.pipe_networks[0].endpoints[0].dir == "out");
    REQUIRE(parsed.pipe_networks[0].endpoints[1].dir == "in");
}
```

- [ ] **Step 2: Build the tests to verify failure**

Run: `cmake --build build --config Release --target fc-tests`
Expected: COMPILE ERROR — `pipe_networks` is not a member of `ParseResult`.

- [ ] **Step 3: Add the data model to the header**

In `sav_import.hpp`, add the new `BuildingKind` values (append to the enum,
before the closing brace, after `DimensionalDepot`):

```cpp
        DimensionalDepot,
        FluidBuffer,
        IndustrialFluidBuffer,
    };
```

Add the new structs above `ParseResult` (after the `LogisticsStation` struct):

```cpp
    /// @brief One machine pipe connector on a network. `dir` is the wrapper's
    /// name-based hint ("in"/"out"/"any"); the importer prefers matching the
    /// network fluid against the node's recipe pins and uses `dir` as fallback.
    struct PipeEndpoint
    {
        std::string building;
        std::string dir;
    };

    /// @brief A fluid network: its carried fluid (display name, empty if
    /// unresolved) and the machine endpoints attached to it.
    struct PipeNetwork
    {
        int id = -1;
        std::string fluid;
        std::vector<PipeEndpoint> endpoints;
    };
```

In `ParseResult`, add the field (next to `vehicle_routes`):

```cpp
        std::vector<PipeNetwork> pipe_networks;
```

- [ ] **Step 4: Parse `pipe_networks` and map the new kinds**

In `sav_import.cpp`, extend `ParseKind` (after the `dimensional_depot` line):

```cpp
            if (s == "dimensional_depot") return BuildingKind::DimensionalDepot;
            if (s == "fluid_buffer")      return BuildingKind::FluidBuffer;
            if (s == "industrial_fluid_buffer") return BuildingKind::IndustrialFluidBuffer;
```

In `ParseWrapperJson`, after the `logistics` block and before
`result.ok = true;`, add:

```cpp
        if (root.contains("pipe_networks") && root["pipe_networks"].is_array())
        {
            for (const auto& n : root["pipe_networks"].get_array())
            {
                PipeNetwork net;
                if (n.contains("id") && n["id"].is_number()) net.id = n["id"].get<int>();
                if (n.contains("fluid") && n["fluid"].is_string()) net.fluid = n["fluid"].get_string();
                if (n.contains("endpoints") && n["endpoints"].is_array())
                {
                    for (const auto& e : n["endpoints"].get_array())
                    {
                        PipeEndpoint ep;
                        if (e.contains("building") && e["building"].is_string()) ep.building = e["building"].get_string();
                        if (e.contains("dir") && e["dir"].is_string()) ep.dir = e["dir"].get_string();
                        if (!ep.building.empty()) net.endpoints.push_back(std::move(ep));
                    }
                }
                result.pipe_networks.push_back(std::move(net));
            }
        }
```

- [ ] **Step 5: Build and run the parse test**

Run: `cmake --build build --config Release --target fc-tests && ./build/ficsit-companion/Release/fc-tests.exe "ParseWrapperJson reads pipe_networks endpoints and fluid"`
Expected: PASS.

- [ ] **Step 6: Mark Task 3 done in PROGRESS.md**

---

### Task 4: C++ — new logistics kinds + fluid-buffer node construction

**Files:**
- Modify: `ficsit-companion/include/domain/node.hpp`
- Modify: `ficsit-companion/src/domain/logistics_node.cpp`
- Modify: `ficsit-companion/src/infra/sav_import.cpp`
- Test: `ficsit-companion/tests/test_sav_import.cpp`

- [ ] **Step 1: Write the failing fluid-buffer placement test**

Append to `tests/test_sav_import.cpp`. Add a `FluidBuffer` helper next to the
other building helpers in the anonymous namespace:

```cpp
    SavImport::Building FluidBuffer(const std::string& id, const float x)
    {
        SavImport::Building b;
        b.id = id;
        b.kind = SavImport::BuildingKind::FluidBuffer;
        b.x = x;
        return b;
    }
```

And the test:

```cpp
/// @covers SavImport::BuildGraph places a fluid buffer as a logistics node.
TEST_CASE("BuildGraph places a Fluid Buffer node", "[sav_import]")
{
    EnsureGameDataLoaded();

    SavImport::ParseResult parsed;
    parsed.ok = true;
    parsed.buildings.push_back(FluidBuffer("tank", 0.0f));

    IdGen ids;
    SavImport::BuildOutput out;
    std::string err;

    REQUIRE(SavImport::BuildGraph(parsed, std::ref(ids), out, err));
    REQUIRE(out.nodes.size() == 1);
    REQUIRE(out.nodes[0]->IsLogistics());
    const LogisticsNode* l = static_cast<const LogisticsNode*>(out.nodes[0].get());
    REQUIRE(l->logistics_kind == LogisticsNode::Kind::FluidBuffer);
    REQUIRE(out.nodes[0]->ins.size() == 1);
    REQUIRE(out.nodes[0]->outs.size() == 1);
    REQUIRE(std::string(l->GetDisplayName()) == "Fluid Buffer");
}
```

- [ ] **Step 2: Build to verify failure**

Run: `cmake --build build --config Release --target fc-tests`
Expected: COMPILE ERROR — `FluidBuffer` is not a member of `LogisticsNode::Kind`.

- [ ] **Step 3: Add the new logistics kinds**

In `node.hpp`, extend `LogisticsNode::Kind` (append after `DimensionalDepot = 4`):

```cpp
        DimensionalDepot = 4,
        PipeJunction = 5,
        FluidBuffer = 6,
        IndustrialFluidBuffer = 7,
    };
```

- [ ] **Step 4: Add display names**

In `logistics_node.cpp`, add cases to `GetDisplayName` (before `default:`):

```cpp
    case LogisticsNode::Kind::PipeJunction:
        return "Pipe Junction";
    case LogisticsNode::Kind::FluidBuffer:
        return "Fluid Buffer";
    case LogisticsNode::Kind::IndustrialFluidBuffer:
        return "Industrial Fluid Buffer";
```

- [ ] **Step 5: Construct fluid-buffer nodes in BuildGraph**

In `sav_import.cpp`, in the `switch (b.kind)` that builds nodes, add cases for
the two new kinds. Place them right after the existing
`case BuildingKind::DimensionalDepot:` block closes — i.e. add two standalone
cases before `case BuildingKind::Unknown:`:

```cpp
            case BuildingKind::FluidBuffer:
            {
                node = std::make_unique<LogisticsNode>(id_generator(),
                    LogisticsNode::Kind::FluidBuffer, 1, 1, id_generator);
                break;
            }
            case BuildingKind::IndustrialFluidBuffer:
            {
                node = std::make_unique<LogisticsNode>(id_generator(),
                    LogisticsNode::Kind::IndustrialFluidBuffer, 2, 2, id_generator);
                break;
            }
```

Then set position + register (the code after the switch already does
`node->pos = ...; id_to_index[b.id] = ...; out.nodes.push_back(...)`, so no
extra work — just ensure the new cases assign `node` and `break`).

- [ ] **Step 6: Build and run the test**

Run: `cmake --build build --config Release --target fc-tests && ./build/ficsit-companion/Release/fc-tests.exe "BuildGraph places a Fluid Buffer node"`
Expected: PASS.

- [ ] **Step 7: Mark Task 4 done in PROGRESS.md**

---

### Task 5: C++ — manifold wiring pass

**Files:**
- Modify: `ficsit-companion/src/infra/sav_import.cpp`
- Test: `ficsit-companion/tests/test_sav_import.cpp`

- [ ] **Step 1: Write the failing wiring tests**

Append to `tests/test_sav_import.cpp`. These use real recipes:
`Residual Fuel` (Heavy Oil Residue → Fuel) is a single-in/single-out refinery
recipe; `Power (Fuel)` consumes Fuel. Add helpers + tests:

```cpp
/// @covers SavImport::BuildGraph direct 1:1 pipe link (no manifold node).
TEST_CASE("BuildGraph wires a 1:1 pipe network directly", "[sav_import]")
{
    EnsureGameDataLoaded();

    SavImport::ParseResult parsed;
    parsed.ok = true;
    // Residual Fuel: Heavy Oil Residue -> Fuel (single in, single out).
    parsed.buildings.push_back(Manufacturer("ref", "Residual Fuel", 0.0f));
    // Power (Fuel): consumes Fuel.
    parsed.buildings.push_back(Manufacturer("gen", "Power (Fuel)", 100.0f));

    SavImport::PipeNetwork net;
    net.id = 1;
    net.fluid = "Fuel";
    net.endpoints.push_back({ "ref", "out" });
    net.endpoints.push_back({ "gen", "in" });
    parsed.pipe_networks.push_back(net);

    IdGen ids;
    SavImport::BuildOutput out;
    std::string err;
    REQUIRE(SavImport::BuildGraph(parsed, std::ref(ids), out, err));

    // No manifold node was created (only the two machines).
    REQUIRE(out.nodes.size() == 2);
    // Exactly one pipe link, refinery Fuel output -> generator Fuel input.
    REQUIRE(out.links.size() == 1);
    const Pin* start = out.links[0]->start;
    const Pin* end = out.links[0]->end;
    REQUIRE(start->item != nullptr);
    REQUIRE(start->item->name == "Fuel");
    REQUIRE(start->node->IsCraft());
    REQUIRE(end->node->IsCraft());
}

/// @covers SavImport::BuildGraph N:M pipe network -> PipeJunction manifold.
TEST_CASE("BuildGraph builds a PipeJunction manifold for an N:M pipe network", "[sav_import]")
{
    EnsureGameDataLoaded();

    SavImport::ParseResult parsed;
    parsed.ok = true;
    parsed.buildings.push_back(Manufacturer("ref", "Residual Fuel", 0.0f));    // produces Fuel
    parsed.buildings.push_back(Manufacturer("gen_a", "Power (Fuel)", 100.0f)); // consumes Fuel
    parsed.buildings.push_back(Manufacturer("gen_b", "Power (Fuel)", 200.0f)); // consumes Fuel

    SavImport::PipeNetwork net;
    net.id = 1;
    net.fluid = "Fuel";
    net.endpoints.push_back({ "ref", "out" });
    net.endpoints.push_back({ "gen_a", "in" });
    net.endpoints.push_back({ "gen_b", "in" });
    parsed.pipe_networks.push_back(net);

    IdGen ids;
    SavImport::BuildOutput out;
    std::string err;
    REQUIRE(SavImport::BuildGraph(parsed, std::ref(ids), out, err));

    // 3 machines + 1 manifold node.
    REQUIRE(out.nodes.size() == 4);
    const LogisticsNode* junction = nullptr;
    for (const auto& n : out.nodes)
    {
        if (n->IsLogistics()
            && static_cast<const LogisticsNode*>(n.get())->logistics_kind == LogisticsNode::Kind::PipeJunction)
        {
            junction = static_cast<const LogisticsNode*>(n.get());
        }
    }
    REQUIRE(junction != nullptr);
    REQUIRE(junction->ins.size() == 1);   // one producer
    REQUIRE(junction->outs.size() == 2);  // two consumers
    REQUIRE(junction->ins[0]->item != nullptr);
    REQUIRE(junction->ins[0]->item->name == "Fuel");
    // 3 links: ref->junction, junction->gen_a, junction->gen_b.
    REQUIRE(out.links.size() == 3);
}

/// @covers SavImport::BuildGraph skips a pipe network whose fluid is unknown.
TEST_CASE("BuildGraph skips a pipe network with unresolved fluid", "[sav_import]")
{
    EnsureGameDataLoaded();

    SavImport::ParseResult parsed;
    parsed.ok = true;
    parsed.buildings.push_back(Manufacturer("ref", "Residual Fuel", 0.0f));
    parsed.buildings.push_back(Manufacturer("gen", "Power (Fuel)", 100.0f));

    SavImport::PipeNetwork net;
    net.id = 1;
    net.fluid = "Notafluid";  // does not resolve to an Item
    net.endpoints.push_back({ "ref", "out" });
    net.endpoints.push_back({ "gen", "in" });
    parsed.pipe_networks.push_back(net);

    IdGen ids;
    SavImport::BuildOutput out;
    std::string err;
    REQUIRE(SavImport::BuildGraph(parsed, std::ref(ids), out, err));

    REQUIRE(out.links.empty());
    REQUIRE(HasWarningContaining(out.warnings, "unresolved fluid"));
}
```

> **Verify recipe names before relying on them.** Confirm "Residual Fuel" and
> "Power (Fuel)" exist with the expected ins/outs:
> `cd ficsit-companion/tools/sav_import` is not needed — from repo root run:
> `python -c "import json;d=json.load(open('assets/satisfactory.json',encoding='utf-8'));rs=d.get('recipes') or d.get('Recipes');import sys;\n[print(r.get('name'), [i.get('name') for i in (r.get('inputs') or r.get('ins') or [])], '->', [o.get('name') for o in (r.get('outputs') or r.get('outs') or [])]) for r in rs if (r.get('name') or '').startswith('Residual Fuel') or (r.get('name') or '').startswith('Power (Fuel)')]"`
> Expected: `Residual Fuel ['Heavy Oil Residue'] -> ['Fuel']` and
> `Power (Fuel) ['Fuel'] -> []`. If the names differ, substitute the actual
> single-in/single-out fluid recipe names into the tests above.

- [ ] **Step 2: Build to verify failure**

Run: `cmake --build build --config Release --target fc-tests && ./build/ficsit-companion/Release/fc-tests.exe "[sav_import]"`
Expected: the three new tests FAIL (no manifold node, no links created).

- [ ] **Step 3: Implement the manifold wiring pass**

In `sav_import.cpp`, insert this pass after the upstream craft-input item
stamping loop (the `for (const auto& node : out.nodes)` block that ends near
the `// ---- Step: vehicle route links` comment) and BEFORE that vehicle-route
step. Add a small file-local helper just above `BuildGraph` (in the anonymous
namespace) for matching a free fluid pin:

```cpp
        // First output (is_output=true) / input pin carrying `item` that has no
        // link yet, or nullptr. Used to attach a pipe edge to the correct fluid
        // pin of a multi-port machine without colliding with an existing link.
        Pin* FreeFluidPin(Node* node, const Item* item, bool is_output)
        {
            auto& pins = is_output ? node->outs : node->ins;
            for (auto& p : pins)
            {
                if (p->item == item && p->link == nullptr) return p.get();
            }
            return nullptr;
        }
```

Then the pass inside `BuildGraph`:

```cpp
        // ---- Step: pipe networks (fluid manifolds) ----
        // Each network is a shared fluid bus. We classify endpoints into
        // producers (a node output carrying the fluid) and consumers (a node
        // input carrying the fluid), then either wire a 1:1 network directly
        // or build one PipeJunction logistics node (one input per producer,
        // one output per consumer). Pumps/junctions/segments never appear here
        // — they were absorbed by network-id grouping in the wrapper.
        size_t pipe_direct_links = 0;
        size_t pipe_manifolds = 0;
        size_t pipe_nets_unresolved = 0;
        size_t pipe_nets_no_flow = 0;
        std::unordered_map<Node*, bool> buffer_consumed;
        std::unordered_map<Node*, bool> buffer_produced;
        for (const PipeNetwork& net : parsed.pipe_networks)
        {
            const Item* fluid = LookupItem(net.fluid);
            if (fluid == nullptr)
            {
                pipe_nets_unresolved += 1;
                continue;
            }

            std::vector<Pin*> producers;
            std::vector<Pin*> consumers;
            std::vector<Node*> endpoint_nodes;
            std::unordered_set<Node*> seen_in_net;
            for (const PipeEndpoint& ep : net.endpoints)
            {
                auto idx = id_to_index.find(ep.building);
                if (idx == id_to_index.end()) continue;
                Node* node = out.nodes[idx->second].get();
                // One role per node per network (dedupe repeated connectors).
                if (!seen_in_net.insert(node).second) continue;
                endpoint_nodes.push_back(node);

                const bool is_fluid_buffer = node->IsLogistics()
                    && (static_cast<LogisticsNode*>(node)->logistics_kind == LogisticsNode::Kind::FluidBuffer
                        || static_cast<LogisticsNode*>(node)->logistics_kind == LogisticsNode::Kind::IndustrialFluidBuffer);

                if (is_fluid_buffer)
                {
                    // Consumer on the first network it touches; producer on the
                    // second (bridging). A same-network tap stays consumer-only,
                    // avoiding a self-cycle.
                    if (!buffer_consumed[node])
                    {
                        for (auto& p : node->ins) { if (p->link == nullptr) { p->item = fluid; consumers.push_back(p.get()); break; } }
                        buffer_consumed[node] = true;
                    }
                    else if (!buffer_produced[node])
                    {
                        for (auto& p : node->outs) { if (p->link == nullptr) { p->item = fluid; producers.push_back(p.get()); break; } }
                        buffer_produced[node] = true;
                    }
                    continue;
                }

                if (node->IsExtractor() && !node->outs.empty() && node->outs[0]->item == fluid && node->outs[0]->link == nullptr)
                {
                    producers.push_back(node->outs[0].get());
                    continue;
                }
                if (Pin* op = FreeFluidPin(node, fluid, true)) { producers.push_back(op); continue; }
                if (Pin* ip = FreeFluidPin(node, fluid, false)) { consumers.push_back(ip); continue; }
                // Fall back to the wrapper direction hint when neither pin
                // matches by item (e.g. a recipe whose pin item differs).
                if (ep.dir == "out" && !node->outs.empty() && node->outs[0]->link == nullptr) producers.push_back(node->outs[0].get());
                else if (ep.dir == "in" && !node->ins.empty() && node->ins[0]->link == nullptr) consumers.push_back(node->ins[0].get());
            }

            if (producers.empty() || consumers.empty())
            {
                pipe_nets_no_flow += 1;
                continue;
            }

            auto make_link = [&](Pin* a, Pin* b) {
                out.links.emplace_back(std::make_unique<Link>(id_generator(), a, b));
                a->link = out.links.back().get();
                b->link = out.links.back().get();
            };

            if (producers.size() == 1 && consumers.size() == 1)
            {
                make_link(producers[0], consumers[0]);
                pipe_direct_links += 1;
                continue;
            }

            // Manifold: one PipeJunction node, producers -> ins, outs -> consumers.
            auto junction = std::make_unique<LogisticsNode>(id_generator(),
                LogisticsNode::Kind::PipeJunction, producers.size(), consumers.size(), id_generator);
            float cx = 0.0f, cy = 0.0f;
            for (Node* n : endpoint_nodes) { cx += n->pos.x; cy += n->pos.y; }
            if (!endpoint_nodes.empty()) { cx /= endpoint_nodes.size(); cy /= endpoint_nodes.size(); }
            junction->pos = ImVec2(cx, cy);
            for (auto& p : junction->ins) p->item = fluid;
            for (auto& p : junction->outs) p->item = fluid;
            Node* jraw = junction.get();
            out.nodes.push_back(std::move(junction));
            for (size_t i = 0; i < producers.size(); ++i) make_link(producers[i], jraw->ins[i].get());
            for (size_t j = 0; j < consumers.size(); ++j) make_link(jraw->outs[j].get(), consumers[j]);
            pipe_manifolds += 1;
        }

        if (pipe_direct_links > 0 || pipe_manifolds > 0)
        {
            out.warnings.push_back("[pipes] " + std::to_string(pipe_direct_links)
                + " direct link(s), " + std::to_string(pipe_manifolds) + " manifold(s)");
        }
        if (pipe_nets_unresolved > 0)
        {
            out.warnings.push_back("[pipes] " + std::to_string(pipe_nets_unresolved)
                + " network(s) skipped: unresolved fluid");
        }
        if (pipe_nets_no_flow > 0)
        {
            out.warnings.push_back("[pipes] " + std::to_string(pipe_nets_no_flow)
                + " network(s) skipped: no producer/consumer pair");
        }
```

> **Note on pin items for producers:** craft/extractor output pins already carry
> their recipe item, so `producers` pins are correctly typed. The rate
> propagation passes that follow this code handle the new `PipeJunction`
> (logistics) and fluid-buffer nodes generically — no extra rate code needed.

- [ ] **Step 4: Build and run all sav-import tests**

Run: `cmake --build build --config Release --target fc-tests && ./build/ficsit-companion/Release/fc-tests.exe "[sav_import]"`
Expected: PASS — all sav-import tests including the three new ones.

- [ ] **Step 5: Mark Task 5 done in PROGRESS.md**

---

### Task 6: End-to-end validation + docs

**Files:**
- Modify: `docs/save_game.md`

- [ ] **Step 1: Run the full test suites**

Run (JS): `cd ficsit-companion/tools/sav_import && node --test test/wrapper_core.test.js`
Run (C++): `ctest --test-dir build -C Release --output-on-failure`
Expected: all green.

- [ ] **Step 2: End-to-end check on the example save**

Run: `cd ficsit-companion/tools/sav_import && node wrapper.js ../../../example_save.sav > /tmp/pipes.json && node -e "const o=require('/tmp/pipes.json'); const f={}; for(const n of o.pipe_networks){f[n.fluid]=(f[n.fluid]||0)+1;} console.log('fluids:',f); console.log('total nets:', o.pipe_networks.length);"`
Expected: non-empty fluids map containing Water, Crude Oil, Heavy Oil Residue,
Fuel. Confirm belt counts are unchanged from before (the buildings/belts arrays
should be identical to a pre-change run — spot check `o.belts.length`).

- [ ] **Step 3: Document pipe topology**

Add a short subsection to `docs/save_game.md` after the belt topology section
(§10 area), describing: pipe connectors carry `mPipeNetworkID`;
`FGPipeNetwork.mFluidDescriptor` gives the fluid; pumps/junctions/segments share
one network id and are not emitted; the importer builds a `PipeJunction`
manifold or a direct link per network; fluid containers become `FluidBuffer` /
`IndustrialFluidBuffer` nodes. Keep it to ~12 lines, matching the doc's style.

```markdown
## 10b. Pipe (fluid) topology

Pipes are reconstructed by **network id**, not chain-walking. Every pipe
connector (`FGPipeConnectionFactory` on machines, `FGPipeConnectionComponent` on
plumbing) carries `mPipeNetworkID`. `FGPipeNetwork` actors map that id to the
fluid (`mFluidDescriptor`) and list the network's members. Pumps
(`Build_PipelinePump`), junctions (`Build_PipelineJunction_*`) and segments
(`Build_Pipeline`) all share one network id and are **not** emitted as nodes —
grouping machine ports by network id absorbs them (a pump is never a dead end).
The importer classifies each machine port as producer/consumer by matching the
network fluid against the machine's recipe pins, then wires either a direct
producer→consumer link (1:1) or one synthetic `PipeJunction` manifold node
(N:M). `Build_PipeStorageTank` / `Build_IndustrialTank` become `FluidBuffer` /
`IndustrialFluidBuffer` logistics nodes. No pressure/head is modeled.
```

- [ ] **Step 4: Mark Task 6 done in PROGRESS.md and summarize**

Confirm every PROGRESS.md item is ticked; note final test results.

---

## Self-Review Notes (author)

- **Spec coverage:** wrapper reclassification + containers (Task 2), network
  extraction (Tasks 1–2), C++ data model/parse (Task 3), new logistics kinds +
  buffer nodes (Task 4), manifold/direct wiring + buffer bridging + warnings
  (Task 5), validation + docs (Task 6). All §4/§5/§7 spec items mapped.
- **Type consistency:** `PipeEndpoint{building,dir}`, `PipeNetwork{id,fluid,
  endpoints}`, `BuildingKind::FluidBuffer/IndustrialFluidBuffer`,
  `LogisticsNode::Kind::PipeJunction(5)/FluidBuffer(6)/IndustrialFluidBuffer(7)`,
  helpers `pipeConnectorInfo`/`readPipeNetwork`/`FreeFluidPin` are used
  consistently across tasks.
- **Known simplification:** buffer bridging picks in/out network by first-seen
  order (spec §8).
