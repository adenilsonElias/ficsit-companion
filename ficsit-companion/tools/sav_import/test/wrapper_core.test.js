const test = require("node:test");
const assert = require("node:assert/strict");

const {
    buildFloorHolePeerMap,
    followBeltChain,
} = require("../wrapper_core");

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
