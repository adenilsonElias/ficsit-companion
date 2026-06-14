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

module.exports = {
    buildFloorHolePeerMap,
    followBeltChain,
    getComponentPort,
};
