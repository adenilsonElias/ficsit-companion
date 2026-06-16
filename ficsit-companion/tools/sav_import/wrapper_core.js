"use strict";

function normalizeObjectPath(pathName) {
    if (typeof pathName !== "string") return "";
    let p = pathName.trim();
    if ((p.startsWith("\"") && p.endsWith("\"")) || (p.startsWith("'") && p.endsWith("'"))) {
        p = p.substring(1, p.length - 1);
    }
    return p;
}

function objectPathAliases(pathName) {
    const p = normalizeObjectPath(pathName);
    if (!p) return [];
    const out = [p];
    const colon = p.lastIndexOf(":");
    if (colon >= 0 && colon + 1 < p.length) out.push(p.substring(colon + 1));
    const dot = p.lastIndexOf(".");
    if (dot >= 0 && dot + 1 < p.length) out.push(p.substring(dot + 1));
    return Array.from(new Set(out));
}

function pathSetHas(set, pathName) {
    return objectPathAliases(pathName).some(alias => set.has(alias));
}

function collectObjectRefPaths(value, out = [], depth = 0) {
    if (!value || depth > 6) return out;
    if (typeof value === "string") {
        const p = normalizeObjectPath(value);
        if (p) out.push(p);
        return out;
    }
    if (typeof value !== "object") return out;

    for (const key of ["pathName", "PathName", "objectPath", "ObjectPath"]) {
        if (typeof value[key] === "string") {
            const p = normalizeObjectPath(value[key]);
            if (p) out.push(p);
        }
    }
    if (value.value !== undefined) collectObjectRefPaths(value.value, out, depth + 1);
    if (value.Value !== undefined) collectObjectRefPaths(value.Value, out, depth + 1);
    return out;
}

function findProp(props, name) {
    if (!props) return undefined;
    if (Array.isArray(props)) {
        const m = props.find(p => p && (p.name === name || p.Name === name));
        return m ? (m.value !== undefined ? m.value : m.Value) : undefined;
    }
    if (typeof props === "object") {
        return props[name];
    }
    return undefined;
}

function findPropDeep(props, name, depth = 0) {
    if (!props || depth > 8) return undefined;
    const direct = findProp(props, name);
    if (direct !== undefined) return direct;

    if (Array.isArray(props)) {
        for (const p of props) {
            if (!p || typeof p !== "object") continue;
            const next = p.value !== undefined ? p.value : (p.Value !== undefined ? p.Value : p);
            const found = findPropDeep(next, name, depth + 1);
            if (found !== undefined) return found;
        }
        return undefined;
    }

    if (typeof props === "object") {
        for (const v of Object.values(props)) {
            if (!v || typeof v !== "object") continue;
            const found = findPropDeep(v, name, depth + 1);
            if (found !== undefined) return found;
        }
    }
    return undefined;
}

function readObjectRefPropPaths(props, name) {
    return Array.from(new Set(collectObjectRefPaths(findPropDeep(props, name))));
}

function findObjectByRefPaths(objectsByPath, paths) {
    const aliases = Array.from(new Set(paths.flatMap(objectPathAliases)));
    for (const alias of aliases) {
        const found = objectsByPath.get(alias);
        if (found) return found;
    }

    for (const alias of aliases) {
        if (alias.length < 16) continue;
        for (const [key, obj] of objectsByPath) {
            if (key.length < 16) continue;
            if (key.endsWith(alias) || alias.endsWith(key)) return obj;
        }
    }
    return undefined;
}

function objectRefPath(ref) {
    const paths = collectObjectRefPaths(ref);
    return paths.length > 0 ? paths[0] : "";
}

function getComponentPort(comp) {
    if (!comp) return null;
    const name = comp.instanceName || comp.InstanceName || "";
    const dot = name.lastIndexOf(".");
    const suffix = dot >= 0 ? name.substring(dot + 1) : name;
    const role = /^Fuel|^InputFuel|^OutputFuel/.test(suffix) ? "fuel" : null;
    if (suffix.startsWith("Input")) {
        const m = suffix.match(/(\d+)(?!.*\d)/);
        if (m) return { dir: "in", index: parseInt(m[1], 10) - 1, role };
    }
    if (suffix.startsWith("Output")) {
        const m = suffix.match(/(\d+)(?!.*\d)/);
        if (m) return { dir: "out", index: parseInt(m[1], 10) - 1, role };
    }
    const any = suffix.match(/^ConveyorAny(\d+)/);
    if (any) return { dir: "any", index: parseInt(any[1], 10), role };
    return null;
}

function readConnectedComponentPath(comp) {
    if (!comp) return "";
    const props = comp.properties || comp.Properties;
    const paths = readObjectRefPropPaths(props, "mConnectedComponent");
    return paths.length > 0 ? paths[0] : "";
}

function buildFloorHolePeerMap(floorHoleActors) {
    const peers = new Map();
    for (const actor of floorHoleActors || []) {
        const props = actor.properties || actor.Properties;
        const top = readObjectRefPropPaths(props, "mTopSnappedConnection");
        const bottom = readObjectRefPropPaths(props, "mBottomSnappedConnection");
        if (top.length !== 1 || bottom.length !== 1) continue;
        peers.set(top[0], bottom[0]);
        peers.set(bottom[0], top[0]);
    }
    return peers;
}

function floorHolePeerFor(floorHolePeerByConnection, compPath) {
    if (!floorHolePeerByConnection) return "";
    for (const alias of objectPathAliases(compPath)) {
        const peer = floorHolePeerByConnection.get(alias);
        if (peer) return peer;
    }
    return "";
}

function followBeltChain(startCompPath, context) {
    const objectsByPath = context.objectsByPath;
    const beltPaths = context.beltPaths;
    const floorHolePeerByConnection = context.floorHolePeerByConnection || new Map();

    const visited = new Set();
    let currentCompPath = startCompPath;
    let skipFloorHoleOnce = false;
    let hops = 0;
    while (hops++ < 10000) {
        const visitedKey = currentCompPath + "|" + (skipFloorHoleOnce ? "skip" : "normal");
        if (visited.has(visitedKey)) return null;
        visited.add(visitedKey);

        const comp = findObjectByRefPaths(objectsByPath, [currentCompPath]);
        if (!comp) return null;
        const parentPath = comp.parentEntityName || comp.ParentEntityName || "";
        if (!parentPath) return null;

        if (!pathSetHas(beltPaths, parentPath)) {
            return { compPath: currentCompPath, parentEntityPath: parentPath };
        }

        if (!skipFloorHoleOnce) {
            const peerAcrossFloor = floorHolePeerFor(floorHolePeerByConnection, currentCompPath);
            if (peerAcrossFloor) {
                currentCompPath = peerAcrossFloor;
                skipFloorHoleOnce = true;
                continue;
            }
        }
        skipFloorHoleOnce = false;

        const belt = findObjectByRefPaths(objectsByPath, [parentPath]);
        if (!belt) return null;
        const refs = belt.components || belt.Components || [];
        let otherCompPath = "";
        for (const r of refs) {
            const p = objectRefPath(r);
            if (!p || p === currentCompPath) continue;
            const c = findObjectByRefPaths(objectsByPath, [p]);
            if (!c) continue;
            if (getComponentPort(c) == null) continue;
            otherCompPath = p;
            break;
        }
        if (!otherCompPath) return null;

        const otherComp = findObjectByRefPaths(objectsByPath, [otherCompPath]);
        if (!otherComp) return null;
        const next = readConnectedComponentPath(otherComp);
        if (!next) return null;
        skipFloorHoleOnce = floorHolePeerFor(floorHolePeerByConnection, otherCompPath) === next;
        currentCompPath = next;
    }
    return null;
}

function stripClassWrap(cls) {
    const dotIdx = cls.lastIndexOf(".");
    let s = dotIdx >= 0 ? cls.substring(dotIdx + 1) : cls;
    if (s.endsWith("_C")) s = s.substring(0, s.length - 2);
    return s;
}

function classifyGeneratorClass(buildingClass) {
    const s = stripClassWrap(buildingClass);
    // Geothermal generators burn no item fuel, so there is no Power (...) recipe
    // to import them as. Treat them as unhandled rather than as a generator
    // whose fuel "fails" to resolve (which would drop them with a misleading
    // warning and is indistinguishable from a real resolution failure).
    if (/GeoThermal/i.test(s)) return "";
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
    // Descriptor-derived aliases: some generator fuel descriptors strip/split
    // into names that differ from the recipe item name (e.g. Desc_Biofuel ->
    // "Biofuel" rather than "Solid Biofuel"). Map those raw forms too so fuel
    // resolved straight from the descriptor still finds its Power recipe.
    ["Biofuel", "Power (Solid Biofuel)"],
    ["Liquid Fuel", "Power (Fuel)"],
    ["Liquid Turbo Fuel", "Power (Turbofuel)"],
    ["Packaged Biofuel", "Power (Packaged Liquid Biofuel)"],
    ["Nuclear Fuel Rod", "Power (Uranium Fuel Rod)"],
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
        if (value.Value && typeof value.Value === "object") {
            if (typeof value.Value.pathName === "string") return value.Value.pathName;
            if (typeof value.Value.PathName === "string") return value.Value.PathName;
            if (typeof value.Value.objectPath === "string") return value.Value.objectPath;
            if (typeof value.Value.ObjectPath === "string") return value.Value.ObjectPath;
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
        const props = c && (c.properties || c.Properties);
        const stacks = findProp(props, "mInventoryStacks");
        const vals = stacks && (stacks.values || stacks.value || stacks.Value);
        // Skip to the next matching component rather than abandoning the search:
        // a later component with the same suffix may still hold the fuel stack.
        if (!Array.isArray(vals)) continue;
        for (const stack of vals) {
            const stackProps = stack && (stack.properties || stack.Properties);
            const item = findProp(stackProps, "Item");
            const itemValue = item && (item.value !== undefined ? item.value : item.Value);
            const ir = itemValue && (itemValue.itemReference || itemValue.ItemReference);
            const path = readStringValue(ir);
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
    // A descriptor that resolves to a name backed by a real Power (...) recipe
    // wins immediately. A non-empty-but-unmapped descriptor name is NOT trusted
    // outright: fall through to the fuel inventory first (which often holds a
    // mappable fuel), and only use the unmapped descriptor name as a last resort
    // so genuinely unresolvable generators still surface a warning.
    let unmappedDescriptor = "";
    for (const propName of descriptorProps) {
        const name = itemDisplayName(readStringValue(findPropDeep(props, propName)));
        if (!name) continue;
        if (generatorRecipeForFuel(name)) return name;
        if (!unmappedDescriptor) unmappedDescriptor = name;
    }
    const fromInventory =
        firstInventoryItem(entity, "FuelInventory", objectsByPath, itemDisplayName)
        || firstInventoryItem(entity, "fuelInventory", objectsByPath, itemDisplayName)
        || firstInventoryItem(entity, "inventory", objectsByPath, itemDisplayName);
    return fromInventory || unmappedDescriptor;
}

function readIntPropValue(props, name) {
    const raw = findProp(props, name);
    if (typeof raw === "number") return raw;
    if (raw && typeof raw === "object" && typeof raw.value === "number") return raw.value;
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
    const rawFd = findProp(props, "mFluidDescriptor");
    const fd = (rawFd && typeof rawFd === "object" && rawFd.value !== undefined)
        ? rawFd.value : rawFd;
    if (fd && typeof fd === "object") {
        fluidClass = normalizeObjectPath(fd.pathName || fd.PathName || "");
    }
    return { id, fluidClass };
}

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
