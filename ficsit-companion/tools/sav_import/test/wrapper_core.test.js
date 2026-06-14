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
