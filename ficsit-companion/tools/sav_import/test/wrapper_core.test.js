const test = require("node:test");
const assert = require("node:assert/strict");

const {
    buildFloorHolePeerMap,
    followBeltChain,
    readProductivity,
} = require("../wrapper_core");

// Build a FloatProperty in the array-of-properties shape the parser emits.
function floatProp(name, value) {
    return { type: "FloatProperty", name, value };
}

function ref(pathName) {
    return { value: { pathName } };
}

function connection(pathName, parentEntityName, connectedPath) {
    return {
        instanceName: pathName,
        parentEntityName,
        properties: {
            mConnectedComponent: ref(connectedPath),
        },
    };
}

function actor(instanceName, components = [], properties = {}) {
    return {
        instanceName,
        components: components.map(pathName => ref(pathName)),
        properties,
    };
}

function addAliases(objectsByPath, obj) {
    objectsByPath.set(obj.instanceName, obj);
}

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

test("floor-hole snapped connection crosses to the opposite lift before continuing", () => {
    const objectsByPath = new Map();
    const beltPaths = new Set(["LiftA", "LiftB"]);

    const liftAFloor = connection("LiftA.ConveyorAny0", "LiftA", "LiftB.ConveyorAny1");
    const liftABelt = connection("LiftA.ConveyorAny1", "LiftA", "Producer.Output1");
    const liftBFloor = connection("LiftB.ConveyorAny1", "LiftB", "LiftA.ConveyorAny0");
    const liftBBelt = connection("LiftB.ConveyorAny0", "LiftB", "Consumer.Input1");
    const consumerInput = connection("Consumer.Input1", "Consumer", "");
    const floorHole = actor("FloorHole", [], {
        mTopSnappedConnection: ref("LiftA.ConveyorAny0"),
        mBottomSnappedConnection: ref("LiftB.ConveyorAny1"),
    });

    for (const obj of [
        actor("LiftA", ["LiftA.ConveyorAny0", "LiftA.ConveyorAny1"]),
        actor("LiftB", ["LiftB.ConveyorAny0", "LiftB.ConveyorAny1"]),
        liftAFloor,
        liftABelt,
        liftBFloor,
        liftBBelt,
        consumerInput,
        floorHole,
    ]) {
        addAliases(objectsByPath, obj);
    }

    const floorHolePeerByConnection = buildFloorHolePeerMap([floorHole]);

    const reached = followBeltChain("LiftA.ConveyorAny0", {
        objectsByPath,
        beltPaths,
        floorHolePeerByConnection,
    });

    assert.deepEqual(reached, {
        compPath: "Consumer.Input1",
        parentEntityPath: "Consumer",
    });
});

test("generator class names are recognized without treating power lines as generators", () => {
    const {
        classifyGeneratorClass,
    } = require("../wrapper_core");

    assert.equal(classifyGeneratorClass("Build_GeneratorCoal_C"), "generator");
    assert.equal(classifyGeneratorClass("/Game/FactoryGame/Buildable/Factory/GeneratorFuel/Build_GeneratorFuel.Build_GeneratorFuel_C"), "generator");
    assert.equal(classifyGeneratorClass("Build_GeneratorNuclear_C"), "generator");
    assert.equal(classifyGeneratorClass("Build_BiomassGenerator_C"), "generator");
    assert.equal(classifyGeneratorClass("Build_PowerLine_C"), "");
    // Geothermal generators burn no item fuel and have no Power (...) recipe, so
    // they are deliberately not classified as importable generators.
    assert.equal(classifyGeneratorClass("Build_GeneratorGeoThermal_C"), "");
});

test("generator with an unmapped fuel descriptor falls back to a mappable inventory fuel", () => {
    const {
        resolveGeneratorFuelItem,
        generatorRecipeForFuel,
    } = require("../wrapper_core");

    // Descriptor resolves to a non-empty but unmapped name; the fuel inventory
    // holds a real, mappable fuel. The inventory fuel must win so the generator
    // isn't dropped.
    const entity = {
        components: [{ value: { pathName: "Generator.FuelInventory" } }],
        properties: {
            mFuelClass: { value: { pathName: "/Game/FactoryGame/Misc/Desc_SomeUnmappedThing.Desc_SomeUnmappedThing_C" } },
        },
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
                                    pathName: "/Game/FactoryGame/Resource/Parts/Coal/Desc_Coal.Desc_Coal_C",
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

    const fuel = resolveGeneratorFuelItem(entity, objectsByPath, itemDisplayName);
    assert.equal(fuel, "Coal");
    assert.equal(generatorRecipeForFuel(fuel), "Power (Coal)");
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
    assert.equal(generatorRecipeForFuel("Leaves"), "Power (Leaves)");
    assert.equal(generatorRecipeForFuel("Wood"), "Power (Wood)");
    assert.equal(generatorRecipeForFuel("Mycelia"), "Power (Mycelia)");
    assert.equal(generatorRecipeForFuel("Biomass"), "Power (Biomass)");
    assert.equal(generatorRecipeForFuel("Solid Biofuel"), "Power (Solid Biofuel)");
    assert.equal(generatorRecipeForFuel("Packaged Liquid Biofuel"), "Power (Packaged Liquid Biofuel)");
    assert.equal(generatorRecipeForFuel("Uranium Fuel Rod"), "Power (Uranium Fuel Rod)");
    assert.equal(generatorRecipeForFuel("Plutonium Fuel Rod"), "Power (Plutonium Fuel Rod)");
    assert.equal(generatorRecipeForFuel("Ficsonium Fuel Rod"), "Power (Ficsonium Fuel Rod)");
    assert.equal(generatorRecipeForFuel("Iron Ore"), "");
    assert.equal(generatorRecipeForFuel(""), "");
});

test("generator fuel item maps descriptor-derived aliases to exact Power recipe names", () => {
    const {
        generatorRecipeForFuel,
    } = require("../wrapper_core");

    assert.equal(generatorRecipeForFuel("Biofuel"), "Power (Solid Biofuel)");
    assert.equal(generatorRecipeForFuel("Liquid Fuel"), "Power (Fuel)");
    assert.equal(generatorRecipeForFuel("Liquid Turbo Fuel"), "Power (Turbofuel)");
    assert.equal(generatorRecipeForFuel("Packaged Biofuel"), "Power (Packaged Liquid Biofuel)");
    assert.equal(generatorRecipeForFuel("Nuclear Fuel Rod"), "Power (Uranium Fuel Rod)");
});

test("unrecognized generator fuel has no recipe mapping", () => {
    const {
        generatorRecipeForFuel,
    } = require("../wrapper_core");

    assert.equal(generatorRecipeForFuel("Alien Power Matrix"), "");
    assert.equal(generatorRecipeForFuel("Water"), "");
});

test("generator active fuel resolves from descriptor property before inventory", () => {
    const {
        resolveGeneratorFuelItem,
    } = require("../wrapper_core");

    const entity = {
        components: [{ value: { pathName: "Generator.FuelInventory" } }],
        properties: {
            mFuelClass: { value: { pathName: "/Game/FactoryGame/Resource/Parts/Coal/Desc_Coal.Desc_Coal_C" } },
        },
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

    assert.equal(resolveGeneratorFuelItem(entity, objectsByPath, itemDisplayName), "Coal");
});

test("generator active fuel resolves from uppercase descriptor object shape", () => {
    const {
        resolveGeneratorFuelItem,
    } = require("../wrapper_core");

    const entity = {
        Components: [],
        Properties: {
            mFuelClass: {
                Value: {
                    PathName: "/Game/FactoryGame/Resource/Parts/Coal/Desc_Coal.Desc_Coal_C",
                },
            },
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

test("generator active fuel resolves from uppercase parser fuel inventory shapes", () => {
    const {
        resolveGeneratorFuelItem,
    } = require("../wrapper_core");

    const entity = {
        Components: [{ Value: { PathName: "Generator.FuelInventory" } }],
        Properties: {},
    };
    const inventory = {
        InstanceName: "Generator.FuelInventory",
        Properties: {
            mInventoryStacks: {
                Value: [{
                    Properties: {
                        Item: {
                            Value: {
                                itemReference: {
                                    PathName: "/Game/FactoryGame/Resource/Parts/Biofuel/Desc_Biofuel.Desc_Biofuel_C",
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

test("pipeConnectorInfo and readPipeNetwork handle array-style property bags", () => {
    const port = {
        instanceName: "Refinery.PipeOutputFactory",
        properties: [{ name: "mPipeNetworkID", value: 7 }],
    };
    assert.deepEqual(pipeConnectorInfo(port), { networkId: 7, dir: "out" });

    const net = {
        instanceName: "FGPipeNetwork_2",
        properties: [
            { name: "mPipeNetworkID", value: 2 },
            { name: "mFluidDescriptor", value: { pathName: "/Game/.../Desc_Water.Desc_Water_C" } },
        ],
    };
    assert.deepEqual(readPipeNetwork(net), {
        id: 2,
        fluidClass: "/Game/.../Desc_Water.Desc_Water_C",
    });
});

test("readPipeNetwork returns empty fluidClass when descriptor is absent", () => {
    const net = { instanceName: "FGPipeNetwork_3", properties: { mPipeNetworkID: { value: 9 } } };
    assert.deepEqual(readPipeNetwork(net), { id: 9, fluidClass: "" });
});

test("recipeDisplayName strips Alternate_ and applies renamed-recipe overrides", () => {
    const { recipeDisplayName } = require("../wrapper_core");

    // Standard recipe: only Recipe_ is stripped, camelCase split.
    assert.equal(
        recipeDisplayName("/Game/FactoryGame/Recipes/Recipe_IronPlate.Recipe_IronPlate_C"),
        "Iron Plate");

    // Existing standard-recipe override still wins (class stem differs from display).
    assert.equal(recipeDisplayName("Recipe_IngotIron_C"), "Iron Ingot");

    // Descriptive alternate: stripping Alternate_ makes the heuristic match the
    // game-data display name.
    assert.equal(
        recipeDisplayName("/Game/.../Recipe_Alternate_CoatedIronPlate.Recipe_Alternate_CoatedIronPlate_C"),
        "Coated Iron Plate");
    assert.equal(
        recipeDisplayName("/Game/.../Recipe_Alternate_PureIronIngot.Recipe_Alternate_PureIronIngot_C"),
        "Pure Iron Ingot");

    // Renamed alternate: internal class name (EnrichedCoal) differs from the
    // display name (Compacted Coal); resolved via an explicit override.
    assert.equal(
        recipeDisplayName("/Game/.../Recipe_Alternate_EnrichedCoal.Recipe_Alternate_EnrichedCoal_C"),
        "Compacted Coal");

    // Empty input stays empty.
    assert.equal(recipeDisplayName(""), "");
});

test("normal lift traversal does not bounce back after crossing a floor hole", () => {
    const objectsByPath = new Map();
    const beltPaths = new Set(["LiftA", "LiftB"]);

    const liftAFloor = connection("LiftA.ConveyorAny0", "LiftA", "LiftB.ConveyorAny1");
    const liftABelt = connection("LiftA.ConveyorAny1", "LiftA", "Producer.Output1");
    const liftBFloor = connection("LiftB.ConveyorAny1", "LiftB", "LiftA.ConveyorAny0");
    const liftBBelt = connection("LiftB.ConveyorAny0", "LiftB", "Consumer.Input1");
    const consumerInput = connection("Consumer.Input1", "Consumer", "");
    const floorHole = actor("FloorHole", [], {
        mTopSnappedConnection: ref("LiftA.ConveyorAny0"),
        mBottomSnappedConnection: ref("LiftB.ConveyorAny1"),
    });

    for (const obj of [
        actor("LiftA", ["LiftA.ConveyorAny0", "LiftA.ConveyorAny1"]),
        actor("LiftB", ["LiftB.ConveyorAny0", "LiftB.ConveyorAny1"]),
        liftAFloor,
        liftABelt,
        liftBFloor,
        liftBBelt,
        consumerInput,
        floorHole,
        connection("Producer.Output1", "Producer", ""),
    ]) {
        addAliases(objectsByPath, obj);
    }

    const floorHolePeerByConnection = buildFloorHolePeerMap([floorHole]);

    const reached = followBeltChain("LiftA.ConveyorAny1", {
        objectsByPath,
        beltPaths,
        floorHolePeerByConnection,
    });

    assert.deepEqual(reached, {
        compPath: "Consumer.Input1",
        parentEntityPath: "Consumer",
    });
});

test("readProductivity uses the last completed measurement window", () => {
    const props = [
        floatProp("mLastProductivityMeasurementProduceDuration", 150),
        floatProp("mLastProductivityMeasurementDuration", 300),
    ];
    assert.equal(readProductivity(props), 0.5);
});

test("readProductivity clamps produce>total to 1.0 and negatives to 0", () => {
    assert.equal(readProductivity([
        floatProp("mLastProductivityMeasurementProduceDuration", 400),
        floatProp("mLastProductivityMeasurementDuration", 300),
    ]), 1.0);
    assert.equal(readProductivity([
        floatProp("mLastProductivityMeasurementProduceDuration", -5),
        floatProp("mLastProductivityMeasurementDuration", 300),
    ]), 0);
});

test("readProductivity falls back to the current window when last is absent", () => {
    const props = [
        floatProp("mCurrentProductivityMeasurementProduceDuration", 75),
        floatProp("mCurrentProductivityMeasurementDuration", 300),
    ];
    assert.equal(readProductivity(props), 0.25);
});

test("readProductivity defaults to 1.0 when no measurement is present", () => {
    assert.equal(readProductivity([]), 1.0);
    assert.equal(readProductivity([
        floatProp("mLastProductivityMeasurementDuration", 0),
    ]), 1.0);
});

test("readProductivity also accepts an object-map property shape", () => {
    const props = {
        mLastProductivityMeasurementProduceDuration: 60,
        mLastProductivityMeasurementDuration: 300,
    };
    assert.equal(readProductivity(props), 0.2);
});
