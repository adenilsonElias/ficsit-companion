#!/usr/bin/env node
/*
 * ficsit-companion .sav -> wrapper-JSON adapter
 *
 * Reads a Satisfactory 1.2 .sav file, parses it with
 * @etothepii/satisfactory-file-parser, classifies each placed building as
 * one of {manufacturer, miner, splitter, smart_splitter, prog_splitter,
 * merger, sink, storage, truck_station, train_station, fluid_buffer, industrial_fluid_buffer}. Generator actors are
 * resolved to manufacturer entries with Power (...) recipes when their active
 * fuel is known. The adapter then collapses chained conveyor poles into single
 * belts and writes the result as JSON to stdout in the schema documented in
 * the project plan.
 *
 * Usage:
 *   node wrapper.js <path/to/Save.sav>     -> writes JSON to stdout
 *
 * Status: scaffolding. Class-name dispatch and belt-chain reduction are
 * deliberately partial; refine as we encounter real saves.
 */

const fs = require("fs");
const path = require("path");
const BeltCore = require("./wrapper_core");
const {
    stripClassWrap,
    splitCamel,
    recipeDisplayName,
    firstInventoryItem,
    classifyGeneratorClass,
    generatorRecipeForFuel,
    resolveGeneratorFuelItem,
    pipeConnectorInfo,
    readPipeNetwork,
} = BeltCore;

function die(msg) {
    process.stderr.write("[sav-import] " + msg + "\n");
    process.exit(1);
}

if (process.argv.length < 3) {
    die("usage: node wrapper.js <save.sav>");
}
const savPath = process.argv[2];
if (!fs.existsSync(savPath)) {
    die("save file not found: " + savPath);
}

let Parser;
try {
    Parser = require("@etothepii/satisfactory-file-parser");
} catch (e) {
    die("@etothepii/satisfactory-file-parser is not installed. Run `npm install` in this folder.");
}

// ---------------------------------------------------------------------------
// Class-name -> internal kind / display-name helpers.
//
// Satisfactory class names look like:
//   Build_ConstructorMk1_C   -> manufacturer (Constructor)
//   Build_AssemblerMk1_C     -> manufacturer (Assembler)
//   Build_MinerMk1_C         -> miner
//   Build_ConveyorBeltMk1_C  -> belt segment
//   Build_ConveyorAttachmentSplitter_C        -> plain splitter
//   Build_ConveyorAttachmentSplitterSmart_C   -> smart splitter
//   Build_ConveyorAttachmentSplitterProgrammable_C -> programmable splitter
//   Build_ConveyorAttachmentMerger_C          -> merger
//   Build_ResourceSinkPoint_C                 -> AWESOME Sink
//
// Recipes:
//   Recipe_IronPlate_C -> "Iron Plate"
//
// Items:
//   Desc_IronPlate_C -> "Iron Plate"
//
// The Satisfactory game data ships display names ("Iron Plate", "Constructor")
// in the C++ side (assets/satisfactory.json). For v1 we use a heuristic
// (strip prefix/suffix, split CamelCase) to derive a display name from the
// class name. Known exceptions are listed in DISPLAY_OVERRIDES.
// ---------------------------------------------------------------------------

// splitCamel and recipeDisplayName are shared from wrapper_core (single source
// of truth for the class-path/_C suffix rule and recipe-name heuristic).

function itemDisplayName(itemClass) {
    if (!itemClass) return "";
    let s = stripClassWrap(itemClass);
    if (ITEM_DISPLAY_OVERRIDES[s]) return ITEM_DISPLAY_OVERRIDES[s];
    if (s.startsWith("Desc_")) s = s.substring("Desc_".length);
    if (s.startsWith("BP_EquipmentDescriptor")) s = s.substring("BP_EquipmentDescriptor".length);
    return splitCamel(s);
}

// Display name of the first item stack held in a named inventory component of an
// entity (e.g. a station's "FuelInventory" or "inventory"). Returns "" when the
// component is missing or empty. Used to learn a station's fuel/cargo item, which
// the save records as inventory contents rather than as a connector flag.
function stationInventoryItem(entity, componentSuffix, objectsByPath) {
    // Shared with generator fuel resolution: firstInventoryItem handles the same
    // lookup but tolerates more parser casings (.Value / .Properties / uppercase
    // PathName), so a parser shape change is fixed in one place for both callers.
    return firstInventoryItem(entity, componentSuffix, objectsByPath, itemDisplayName);
}

// Raw resource class -> assets/satisfactory.json display name. The Desc_ class
// stem rarely matches the Coffee Stain display name (e.g., Desc_OreIron is
// "Iron Ore", not "Ore Iron"). Extend as new resources surface.
const ITEM_DISPLAY_OVERRIDES = {
    "Desc_OreIron":      "Iron Ore",
    "Desc_OreCopper":    "Copper Ore",
    "Desc_Stone":        "Limestone",
    "Desc_Coal":         "Coal",
    "Desc_OreGold":      "Caterium Ore",
    "Desc_RawQuartz":    "Raw Quartz",
    "Desc_Sulfur":       "Sulfur",
    "Desc_OreUranium":   "Uranium",
    "Desc_OreBauxite":   "Bauxite",
    "Desc_SAM":          "SAM",
    "Desc_LiquidOil":    "Crude Oil",
    "Desc_Water":        "Water",
    "Desc_NitrogenGas":  "Nitrogen Gas",
    // Generator fuel descriptors whose stripped/split name differs from the
    // recipe item name used by the Power (...) recipe map.
    "Desc_Biofuel":         "Solid Biofuel",
    "Desc_LiquidFuel":      "Fuel",
    "Desc_LiquidTurboFuel": "Turbofuel",
    "Desc_PackagedBiofuel": "Packaged Liquid Biofuel",
    "Desc_NuclearFuelRod":  "Uranium Fuel Rod",
};

// Build_MinerMkN_C / pumps -> { extractor_kind, default_resource_or_empty }.
// extractor_kind matches sav_import.cpp's numeric mapping (1..5).
function classifyExtractor(strippedClassName) {
    if (strippedClassName.startsWith("Build_MinerMk")) {
        const m = strippedClassName.match(/^Build_MinerMk(\d+)/);
        const mk = m ? Math.min(3, Math.max(1, parseInt(m[1], 10))) : 1;
        return { extractor_kind: mk, default_resource: "" };
    }
    if (strippedClassName.startsWith("Build_WaterPump")) {
        return { extractor_kind: 4, default_resource: "Water" };
    }
    if (strippedClassName.startsWith("Build_OilPump")) {
        return { extractor_kind: 5, default_resource: "Crude Oil" };
    }
    // FrackingExtractor and Resource Wells out of scope for this round.
    return { extractor_kind: 0, default_resource: "" };
}

// EResourcePurity in the game is RP_Inpure (sic) / RP_Normal / RP_Pure.
// The parser surfaces enum values as strings like "EResourcePurity::RP_Normal".
function normalizePurity(rawValue) {
    if (typeof rawValue !== "string") return "";
    const v = rawValue.toLowerCase();
    if (v.includes("inpure") || v.includes("impure")) return "impure";
    if (v.includes("normal")) return "normal";
    if (v.includes("pure")) return "pure";
    return "";
}

function readEnumValue(props, name) {
    const v = findProp(props, name);
    return readEnumValueFromValue(v);
}

function readEnumValueFromValue(v) {
    if (typeof v === "string") return v;
    if (v && typeof v === "object") {
        // EnumProperty in v4 is roughly { type: 'EnumProperty', value: { name, value } }
        if (v.value && typeof v.value === "object" && typeof v.value.value === "string") {
            return v.value.value;
        }
        if (v.value && typeof v.value === "object" && typeof v.value.name === "string") {
            return v.value.name;
        }
        if (typeof v.value === "string") return v.value;
    }
    return "";
}

function readEnumValueDeep(props, name) {
    return readEnumValueFromValue(findPropDeep(props, name));
}

/// Resolve a miner's resource + purity by following mExtractableResource to the
/// placed resource node actor and reading mResourceClass + mPurity.
function readExtractorResourceAndPurity(actor, objectsByPath) {
    const props = actor.properties || actor.Properties;
    const nodeRefs = [
        ...readObjectRefPropPaths(props, "mExtractableResource"),
        ...readObjectRefPropPaths(props, "mExtractResourceNode"), // older save fields
    ];
    if (nodeRefs.length === 0) return { resource: "", purity: "" };

    const node = findObjectByRefPaths(objectsByPath, nodeRefs);
    if (!node) return { resource: "", purity: "" };

    const nodeProps = node.properties || node.Properties;
    let resourceClass = readStringPropDeep(nodeProps, "mResourceClass");
    // Some save shapes nest the class one level deeper, e.g.
    // mResourceNodeRepresentation.mResourceNodeData.mResourceClass.
    if (!resourceClass) {
        resourceClass = readStringPropDeep(nodeProps, "mItemType");
    }
    // Resource nodes with an overridden resource (seen on many 1.x-save nodes)
    // carry the item in mResourceClassOverride — an ObjectProperty pointing at
    // the Desc_* class (e.g. .../Desc_Coal_C) — and the purity in mPurityOverride,
    // rather than the plain mResourceClass / mPurity fields. Without this, those
    // miners come back with no resource and get a wrong downstream-inferred item.
    if (!resourceClass) {
        const overridePaths = readObjectRefPropPaths(nodeProps, "mResourceClassOverride");
        if (overridePaths.length > 0) resourceClass = overridePaths[0];
    }

    const purityRaw = readEnumValueDeep(nodeProps, "mPurity")
        || readEnumValueDeep(nodeProps, "mResourcePurity")
        || readEnumValueDeep(nodeProps, "mPurityOverride");

    return {
        resource: itemDisplayName(resourceClass),
        purity: normalizePurity(purityRaw),
    };
}

// Known manufacturer class prefixes (after stripClassWrap removes the trailing "_C").
// Anything not on this list is treated as "unknown" and skipped silently, which
// keeps power lines / storage / vehicle paths / decorations from polluting the
// warning list as fake "manufacturers with no recipe".
const MANUFACTURER_PREFIXES = [
    "Build_SmelterMk",
    "Build_FoundryMk",
    "Build_ConstructorMk",
    "Build_AssemblerMk",
    "Build_ManufacturerMk",
    "Build_OilRefinery",
    "Build_Packager",
    "Build_Blender",
    "Build_HadronCollider",
    "Build_QuantumEncoder",
    "Build_Converter",
    "Build_ParticleAccelerator",
];

function classifyBuilding(buildingClass) {
    const s = stripClassWrap(buildingClass);
    if (s.startsWith("Build_ConveyorBelt")
        || s.startsWith("Build_ConveyorPole")
        || s.startsWith("Build_ConveyorLift")
        || s.startsWith("Build_ConveyorCeiling")) return "belt_pole";
    // Pipe plumbing carries no production node; networks are reconstructed from
    // mPipeNetworkID instead (see pipe_networks below). Pumps/junctions/segments
    // are absorbed by network-id grouping, so none of them are emitted.
    if (s.startsWith("Build_Pipeline")
        || s.startsWith("Build_FoundationPassthrough_Pipe")) return "pipe";
    if (s.startsWith("Build_PipeStorageTank")) return "fluid_buffer";
    if (s.startsWith("Build_IndustrialTank")) return "industrial_fluid_buffer";
    if (s.startsWith("Build_StorageContainerMk2")) return "industrial_storage";
    if (s.startsWith("Build_StorageContainer")) return "storage";
    // Dimensional Depot uploader (Satisfactory 1.0). The U6/1.0 class name is
    // Build_DimensionalDepotUploader_C; the storage-variant prefix is included
    // for forward-compat in case the class is renamed.
    if (s.startsWith("Build_DimensionalDepot") ||
        s.startsWith("Build_StorageDimensional")) return "dimensional_depot";
    if (s.startsWith("Build_TruckStation")) return "truck_station";
    if (s.startsWith("Build_TrainStation") ||
        s.startsWith("Build_RailroadStation") ||
        s.startsWith("Build_TrainDockingStation") ||
        s.startsWith("Build_TrainPlatform")) return "train_station";
    const generatorKind = classifyGeneratorClass(s);
    if (generatorKind) return generatorKind;
    if (s.startsWith("Build_ConveyorAttachmentSplitterSmart")) return "smart_splitter";
    if (s.startsWith("Build_ConveyorAttachmentSplitterProgrammable")) return "prog_splitter";
    if (s.startsWith("Build_ConveyorAttachmentSplitter")) return "splitter";
    if (s.startsWith("Build_ConveyorAttachmentMerger")) return "merger";
    if (s.includes("ResourceSink")) return "sink";
    if (s.startsWith("Build_Miner")) return "miner";
    if (s.startsWith("Build_OilPump") || s.startsWith("Build_WaterPump") || s.startsWith("Build_FrackingExtractor")) return "miner";
    for (const pre of MANUFACTURER_PREFIXES) {
        if (s.startsWith(pre)) return "manufacturer";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Parser invocation
// ---------------------------------------------------------------------------

let save;
try {
    const nodeBuf = fs.readFileSync(savPath);
    // The parser wants an ArrayBufferLike, not a Node Buffer. Slice out the
    // exact region this Buffer views — Node may share the underlying pool.
    const bytes = nodeBuf.buffer.slice(nodeBuf.byteOffset, nodeBuf.byteOffset + nodeBuf.byteLength);
    // @etothepii/satisfactory-file-parser exposes a couple of entry points
    // depending on version; try the most common ones.
    if (typeof Parser.Parser === "function") {
        save = Parser.Parser.ParseSave(path.basename(savPath, ".sav"), bytes);
    } else if (typeof Parser.parseSave === "function") {
        save = Parser.parseSave(path.basename(savPath, ".sav"), bytes);
    } else if (typeof Parser.ParseSave === "function") {
        save = Parser.ParseSave(path.basename(savPath, ".sav"), bytes);
    } else {
        die("Could not locate ParseSave entry point in @etothepii/satisfactory-file-parser; library may have changed APIs.");
    }
} catch (e) {
    die("Parser threw: " + (e && e.stack ? e.stack : String(e)));
}

// ---------------------------------------------------------------------------
// Extract placed buildings and belts.
//
// The parser model exposes levels -> objects (actors). Each actor has a
// className (UE class path), transform, and serialized property bag. We walk
// the actor list, classify by className, and extract the per-kind info we
// need.
//
// NOTE: actual property names depend on the parser library's choice of
// shape. The accessors below use defensive optional chaining; if the shape
// differs from what we expect on a given save, the field is omitted (the
// C++ side already treats missing fields as "no data").
// ---------------------------------------------------------------------------

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

function readNumberProp(props, name, fallback) {
    const v = findProp(props, name);
    if (typeof v === "number") return v;
    if (v && typeof v === "object" && typeof v.value === "number") return v.value;
    return fallback;
}

function readStringValue(v) {
    if (typeof v === "string") return v;
    if (v && typeof v === "object") {
        // v4 ObjectProperty: { type: 'ObjectProperty', value: { levelName, pathName } }
        if (v.value && typeof v.value === "object") {
            if (typeof v.value.pathName === "string") return v.value.pathName;
            if (typeof v.value.PathName === "string") return v.value.PathName;
            if (typeof v.value.objectPath === "string") return v.value.objectPath;
            if (typeof v.value.ObjectPath === "string") return v.value.ObjectPath;
        }
        // Older shapes: pathName at the top, or .value is a raw string.
        if (typeof v.pathName === "string") return v.pathName;
        if (typeof v.PathName === "string") return v.PathName;
        if (typeof v.objectPath === "string") return v.objectPath;
        if (typeof v.ObjectPath === "string") return v.ObjectPath;
        if (typeof v.value === "string") return v.value;
        if (typeof v.Value === "string") return v.Value;
    }
    return "";
}

function readStringProp(props, name) {
    return readStringValue(findProp(props, name));
}

function readStringPropDeep(props, name) {
    return readStringValue(findPropDeep(props, name));
}

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

function addObjectPathAlias(objectsByPath, pathName, obj) {
    for (const alias of objectPathAliases(pathName)) {
        if (!objectsByPath.has(alias)) objectsByPath.set(alias, obj);
    }
}

function addPathSetAliases(set, pathName) {
    for (const alias of objectPathAliases(pathName)) {
        set.add(alias);
    }
}

function pathSetHas(set, pathName) {
    return objectPathAliases(pathName).some(alias => set.has(alias));
}

function addPathMapAliases(map, pathName, value) {
    for (const alias of objectPathAliases(pathName)) {
        if (!map.has(alias)) map.set(alias, value);
    }
}

function pathMapGet(map, pathName) {
    for (const alias of objectPathAliases(pathName)) {
        if (map.has(alias)) return map.get(alias);
    }
    return undefined;
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

function readObjectRefPropPaths(props, name) {
    return Array.from(new Set(collectObjectRefPaths(findPropDeep(props, name))));
}

function objectRefPath(ref) {
    const paths = collectObjectRefPaths(ref);
    return paths.length > 0 ? paths[0] : "";
}

function findObjectByRefPaths(objectsByPath, paths) {
    const aliases = Array.from(new Set(paths.flatMap(objectPathAliases)));
    for (const alias of aliases) {
        const found = objectsByPath.get(alias);
        if (found) return found;
    }

    // Some parser builds disagree about whether the object path includes the
    // level/package prefix. Fall back to suffix matching for full-looking refs.
    for (const alias of aliases) {
        if (alias.length < 16) continue;
        for (const [key, obj] of objectsByPath) {
            if (key.length < 16) continue;
            if (key.endsWith(alias) || alias.endsWith(key)) return obj;
        }
    }
    return undefined;
}

function readPos(actor) {
    const t = actor.transform || actor.Transform || {};
    const p = t.translation || t.Translation || t.position || t.Position || {};
    return [Number(p.x || p.X || 0), Number(p.y || p.Y || 0), Number(p.z || p.Z || 0)];
}

function allActors(save) {
    const out = [];
    const levels = save.levels || save.Levels || {};
    // v4 of @etothepii/satisfactory-file-parser exposes `levels` as a map
    // keyed by level name. Older versions used an array. Handle both.
    const lvlList = Array.isArray(levels) ? levels : Object.values(levels);
    for (const lvl of lvlList) {
        const objs = (lvl && (lvl.objects || lvl.Objects)) || [];
        for (const o of objs) out.push(o);
    }
    if (Array.isArray(save.objects)) out.push(...save.objects);
    return out;
}

const actors = allActors(save);

// Phase 1: index everything by pathName so the belt walker can resolve
// ObjectReferences in O(1). Also pre-classify so we know which entities are
// belts vs emitted buildings.
const objectsByPath = new Map();   // pathName -> save object (entity or component)
const beltPaths = new Set();       // pathNames of belt entities
const emittedByPath = new Map();   // building pathName -> emitted-building id (same value, but explicit)

for (const a of actors) {
    addObjectPathAlias(objectsByPath, a.instanceName || a.InstanceName, a);
    addObjectPathAlias(objectsByPath, a.pathName || a.PathName, a);
    addObjectPathAlias(objectsByPath, a.objectPath || a.ObjectPath, a);
}

const floorHoleActors = actors.filter(a => {
    const cls = a.className || a.ClassName || a.typePath || "";
    return stripClassWrap(cls).startsWith("Build_FoundationPassthrough_Lift");
});
const floorHolePeerByConnection = BeltCore.buildFloorHolePeerMap(floorHoleActors);

const buildings = [];      // emitted
const warnings = [];
const fuelConnByStation = new Map(); // station id -> fuel connector suffix (e.g. "Input2")
let beltSegmentCount = 0;
let stationsMissingLoadMode = 0; // stations with no mIsInLoadMode flag (assumed Load)

for (const a of actors) {
    const cls = a.className || a.ClassName || a.typePath || "";
    if (!cls) continue;
    const kind = classifyBuilding(cls);

    if (kind === "belt_pole") {
        beltSegmentCount += 1;
        const path = a.instanceName || a.InstanceName;
        addPathSetAliases(beltPaths, path);
        continue;
    }
    if (kind === "pipe") {
        // Pipes are intentionally not wired yet (belts-only scope).
        continue;
    }
    if (kind === "unknown") continue;

    const props = a.properties || a.Properties;
    const recipeRef = readStringProp(props, "mCurrentRecipe");
    const itemRef = readStringProp(props, "mCurrentItem");
    const clock = readNumberProp(props, "mCurrentPotential", 1.0);
    const somersloops = readNumberProp(props, "mProductionBoostPowerConsumptionExponent", 0)
        || readNumberProp(props, "mNumProductionBoost", 0)
        || readNumberProp(props, "mNumProductionShards", 0);

    const stripped = stripClassWrap(cls);
    const id = a.instanceName || a.InstanceName || cls + "@" + buildings.length;
    const entry = {
        id,
        kind,
        class_name: stripped,
        recipe_name: recipeDisplayName(recipeRef),
        item_name: itemDisplayName(itemRef),
        clock: Number(clock) || 1.0,
        somersloops: Number(somersloops) || 0,
        pos: readPos(a),
        inputs: [],
        outputs: [],
    };

    // Extractor specifics: derive Mk level / pump type, follow the resource
    // node reference to figure out the produced item and its purity.
    if (kind === "miner") {
        const ext = classifyExtractor(stripped);
        entry.extractor_kind = ext.extractor_kind;
        const probed = readExtractorResourceAndPurity(a, objectsByPath);
        // Pumps default their resource (Water/Crude Oil); only override if the
        // probed value is non-empty and we trust it (avoids leaving Oil with
        // an empty resource on a partially-parsed save).
        entry.item_name = probed.resource || ext.default_resource || "";
        // Water Extractor has no purity in-game; force Normal so the C++ side
        // doesn't try to apply a 0.5x multiplier to a flat pump.
        if (ext.extractor_kind === 4) {
            entry.extractor_purity = "normal";
        } else {
            entry.extractor_purity = probed.purity || "normal";
        }
    }

    if (kind === "truck_station" || kind === "train_station") {
        const fuelSuffix = stationFuelConnectorSuffix(a);
        if (fuelSuffix) fuelConnByStation.set(id, fuelSuffix);
        // The save doesn't flag which conveyor is the fuel inlet, but it records
        // what each inventory currently holds: `FuelInventory` = the vehicle fuel,
        // `inventory` = the shipped cargo. The C++ importer uses these to tell a
        // station's fuel belt from its cargo belt. mIsInLoadMode === false means
        // the station unloads (cargo arrives by vehicle, leaves via the output),
        // so any input belt is fuel — a fallback when the inventory is empty.
        entry.fuel_item = stationInventoryItem(a, "FuelInventory", objectsByPath);
        entry.cargo_item = stationInventoryItem(a, "inventory", objectsByPath);
        const loadModeProp = a.properties && a.properties.mIsInLoadMode;
        // mIsInLoadMode is delta-serialized; when absent we assume Load mode.
        // Count these so a misclassified station direction isn't fully silent.
        if (!loadModeProp) stationsMissingLoadMode += 1;
        entry.is_unloader = loadModeProp ? loadModeProp.value === false : false;
    }

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

    buildings.push(entry);
    addPathMapAliases(emittedByPath, id, id);
}

// ---------------------------------------------------------------------------
// Belt-chain reduction.
//
// Satisfactory wires connections as a graph of FactoryConnectionComponent save
// components attached to each Build_* actor. Each component carries an
// `mConnectedComponent` ObjectProperty pointing to the paired component on
// the other end of the wire (which may be on another building OR on a belt
// segment). To collapse a belt chain into one logical edge we:
//
//   1) Start at an emitted building's *output* connection component.
//   2) Follow mConnectedComponent. If the target's parent is a belt, hop to
//      the belt's OTHER connection component and follow its mConnectedComponent.
//      Repeat until we reach a non-belt component whose parent is also an
//      emitted building.
//   3) Emit one belt entry { src_building/src_port, dst_building/dst_port }.
//
// Port indices are parsed from the component path suffix:
//   ".Input<N>"   -> input, idx = N - 1   (1-based in saves, 0-based in C++)
//   ".Output<N>"  -> output, idx = N - 1
//   ".ConveyorAny<N>" -> belt-side pseudo-port (direction handled by walker)
// ---------------------------------------------------------------------------

function getComponentPort(comp) {
    return BeltCore.getComponentPort(comp);
    if (!comp) return null;
    const name = comp.instanceName || comp.InstanceName || "";
    const dot = name.lastIndexOf(".");
    const suffix = dot >= 0 ? name.substring(dot + 1) : name;
    // Cover variants like "Output1", "OutputConveyor1", "OutputInventory1",
    // "OutputAny0". The leading "Input"/"Output" wins direction; the LAST
    // digit run in the suffix wins the index. Falls back to ConveyorAnyN.
    //
    // A few buildings expose a dedicated fuel inlet whose component name
    // typically starts with "Fuel" (e.g. TruckStation's FuelInputConveyor0)
    // or has "Fuel" tacked onto an Input/Output prefix. We flag those with
    // role:"fuel" so the C++ side can route the belt to a fuel-only pin
    // instead of using rank order with the cargo conveyors. Match only
    // well-anchored variants to avoid catching incidental "Fuel" substrings
    // (e.g. a station whose internal component is named "FuelDelta…" but
    // isn't actually the belt fuel inlet). The C++ side double-checks by
    // requiring the belt's resolved item to be a known fuel.
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

function ensurePortSlot(ports, index) {
    if (index < 0) return;
    while (ports.length <= index) {
        ports.push({ belt_id: "" });
    }
}

// Truck/Train stations expose their fuel conveyor inlet as a plain "Input<N>"
// component — there is no "Fuel" in the name — so getComponentPort's name match
// can't flag it. The fuel inlet is the one Input connector with no matching
// Output connector (cargo lanes come as Input<k>/Output<k> pairs, e.g.
// Input0/Output0, Input1/Output1, plus the unpaired fuel Input2). Returns that
// connector's component suffix (e.g. "Input2"), or null if none stands out.
function stationFuelConnectorSuffix(entity) {
    const comps = entity.components || entity.Components || [];
    const inputs = new Set();
    const outputs = new Set();
    for (const ref of comps) {
        const p = objectRefPath(ref);
        if (!p) continue;
        const dot = p.lastIndexOf(".");
        const suffix = dot >= 0 ? p.substring(dot + 1) : p;
        let m;
        if ((m = suffix.match(/^Input(\d+)$/))) inputs.add(parseInt(m[1], 10));
        else if ((m = suffix.match(/^Output(\d+)$/))) outputs.add(parseInt(m[1], 10));
    }
    let fuel = -1;
    for (const i of inputs) {
        if (!outputs.has(i) && i > fuel) fuel = i;
    }
    return fuel >= 0 ? ("Input" + fuel) : null;
}

function readConnectedComponentPath(comp) {
    if (!comp) return "";
    const props = comp.properties || comp.Properties;
    // mConnectedComponent is an ObjectProperty whose value is an ObjectReference.
    const paths = readObjectRefPropPaths(props, "mConnectedComponent");
    return paths.length > 0 ? paths[0] : "";
}

function followBeltChain(startCompPath) {
    return BeltCore.followBeltChain(startCompPath, {
        objectsByPath,
        beltPaths,
        floorHolePeerByConnection,
    });
    // Walk from a belt-side component to the next non-belt component. Returns
    // { compPath, parentEntityPath } or null on dead-end / cycle.
    const visited = new Set();
    let currentCompPath = startCompPath;
    let hops = 0;
    while (hops++ < 10000) {
        if (visited.has(currentCompPath)) return null;
        visited.add(currentCompPath);

        const comp = findObjectByRefPaths(objectsByPath, [currentCompPath]);
        if (!comp) return null;
        const parentPath = comp.parentEntityName || comp.ParentEntityName || "";
        if (!parentPath) return null;

        if (!pathSetHas(beltPaths, parentPath)) {
            // Reached a non-belt: this is the chain terminus.
            return { compPath: currentCompPath, parentEntityPath: parentPath };
        }

        // We're on a belt. Find the OTHER connection-style component on this
        // belt — belts can carry inventory/listener components too, and those
        // would derail the chain if naively picked.
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
        currentCompPath = next;
    }
    return null;
}

const belts = [];
let beltsEmitted = 0;
let beltsDropped = 0;
let beltsOutOfScope = 0;
let beltsWrongSide = 0;
let beltIdCounter = 0;

// Per-kind diagnostics so we can tell at a glance how miners are landing:
// how many were emitted, how many had resolved resources, and how many
// belt edges originated from them.
let minersEmittedTotal = 0;
let minersWithResource = 0;
let minersWithComps = 0;
let minerEdgesEmitted = 0;
for (const b of buildings) {
    if (b.kind === "miner") {
        minersEmittedTotal += 1;
        if (b.item_name) minersWithResource += 1;
    }
}

for (const b of buildings) {
    const entity = findObjectByRefPaths(objectsByPath, [b.id]);
    if (!entity) continue;
    const compRefs = entity.components || entity.Components || [];
    if (b.kind === "miner" && compRefs.length > 0) minersWithComps += 1;
    for (const ref of compRefs) {
        const compPath = objectRefPath(ref);
        if (!compPath) continue;
        const comp = findObjectByRefPaths(objectsByPath, [compPath]);
        const port = getComponentPort(comp);
        if (!port || port.dir !== "out") continue;

        const peerPath = readConnectedComponentPath(comp);
        if (!peerPath) continue;

        // Follow through any belt chain.
        const peer = findObjectByRefPaths(objectsByPath, [peerPath]);
        const peerParent = peer && (peer.parentEntityName || peer.ParentEntityName || "");
        let dstCompPath = peerPath;
        let dstParent = peerParent;
        if (peerParent && pathSetHas(beltPaths, peerParent)) {
            const reached = followBeltChain(peerPath);
            if (!reached) { beltsDropped += 1; continue; }
            dstCompPath = reached.compPath;
            dstParent = reached.parentEntityPath;
        }

        const emittedDst = pathMapGet(emittedByPath, dstParent);
        if (!emittedDst) { beltsOutOfScope += 1; continue; }

        const dstComp = findObjectByRefPaths(objectsByPath, [dstCompPath]);
        const dstPort = getComponentPort(dstComp);
        if (!dstPort) { beltsDropped += 1; continue; }

        // Conveyor belts are one-way: they always leave a producer's *output*
        // and arrive at a consumer's *input* (or a splitter/merger's
        // direction-agnostic "any" port, whose orientation the chain walker
        // already resolved). We start every walk from an output port, so the
        // source is guaranteed to be an output. If the terminus is *also* an
        // output, this edge is physically impossible — typically a walker
        // artifact — and wiring it would land on the wrong side of the
        // destination and collide with its real feeder belt. Drop it.
        if (dstPort.dir === "out") { beltsWrongSide += 1; continue; }

        // Tag the station fuel inlet. Its connector is a plain "Input<N>", so the
        // name-based role in getComponentPort misses it; instead match the belt's
        // connector against the fuel connector we identified for that station.
        const compSuffix = p => { const d = p.lastIndexOf("."); return d >= 0 ? p.substring(d + 1) : p; };
        let srcRole = port.role || null;
        if (srcRole === null) {
            const fc = fuelConnByStation.get(b.id);
            if (fc && compSuffix(compPath) === fc) srcRole = "fuel";
        }
        let dstRole = dstPort.role || null;
        if (dstRole === null) {
            const fc = fuelConnByStation.get(emittedDst);
            if (fc && compSuffix(dstCompPath) === fc) dstRole = "fuel";
        }

        belts.push({
            id: "belt-" + (beltIdCounter++),
            item_name: "",
            src: { building: b.id, port: port.index, role: srcRole, dir: port.dir },
            dst: { building: emittedDst, port: dstPort.index, role: dstRole, dir: dstPort.dir },
        });
        beltsEmitted += 1;
        if (b.kind === "miner") minerEdgesEmitted += 1;
    }
}

for (const b of buildings) {
    const entity = findObjectByRefPaths(objectsByPath, [b.id]);
    if (!entity) continue;
    const compRefs = entity.components || entity.Components || [];
    for (const ref of compRefs) {
        const compPath = objectRefPath(ref);
        if (!compPath) continue;
        const comp = findObjectByRefPaths(objectsByPath, [compPath]);
        const port = getComponentPort(comp);
        if (!port) continue;
        if (port.dir === "in") {
            ensurePortSlot(b.inputs, port.index);
        } else if (port.dir === "out") {
            ensurePortSlot(b.outputs, port.index);
        }
    }
}

if (beltSegmentCount > 0 && beltsEmitted === 0) {
    warnings.push("belt-chain walker produced no Links from "
        + beltSegmentCount + " belt segments (mConnectedComponent shape unexpected?)");
}
if (beltsDropped > 0) {
    warnings.push(beltsDropped + " belt chain(s) dropped (dead-end / cycle / missing component)");
}
if (beltsOutOfScope > 0) {
    warnings.push(beltsOutOfScope + " belt chain(s) terminate on a building that was not imported (storage / power / etc.)");
}
if (beltsWrongSide > 0) {
    warnings.push(beltsWrongSide + " belt(s) dropped: terminus is an output port (one-way belt cannot connect output to output)");
}
warnings.push("[debug] miners: "
    + minersEmittedTotal + " placed | "
    + minersWithResource + " with resolved resource | "
    + minersWithComps + " with components | "
    + minerEdgesEmitted + " belt edges out");
if (stationsMissingLoadMode > 0) {
    warnings.push(stationsMissingLoadMode + " station(s) had no mIsInLoadMode flag; "
        + "assumed Load mode — verify their load/unload direction after import");
}

// Diagnostic: count emitted edges by (src_class, src_port) and (dst_class, dst_port).
// Helps identify whether high port indices are coming from a specific building
// class (e.g. refinery pipe-side ports being miscounted as belt outputs).
if (belts.length > 0) {
    const srcCounts = new Map();
    const dstCounts = new Map();
    const buildingClassByPath = new Map();
    for (const b of buildings) buildingClassByPath.set(b.id, b.class_name);
    for (const e of belts) {
        const sk = (buildingClassByPath.get(e.src.building) || "?") + " out#" + e.src.port;
        const dk = (buildingClassByPath.get(e.dst.building) || "?") + " in#" + e.dst.port;
        srcCounts.set(sk, (srcCounts.get(sk) || 0) + 1);
        dstCounts.set(dk, (dstCounts.get(dk) || 0) + 1);
    }
    const topN = (m, n) => Array.from(m.entries())
        .sort((a, b) => b[1] - a[1]).slice(0, n)
        .map(([k, v]) => v + "x " + k).join(" | ");
    warnings.push("[debug] src ports (top 12): " + topN(srcCounts, 12));
    warnings.push("[debug] dst ports (top 12): " + topN(dstCounts, 12));
}

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
const pipe_networks = [];
try {
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

    for (const [id, endpoints] of pipeEndpointsByNet) {
        pipe_networks.push({
            id,
            fluid: pipeFluidById.get(id) || "",
            endpoints,
        });
    }
} catch (e) {
    warnings.push("[pipes] extraction failed: " + (e && e.message ? e.message : String(e)));
}
warnings.push("[pipes] " + pipe_networks.length + " network(s) with machine endpoints");

// ---------------------------------------------------------------------------
// Logistics extraction (Vehicle Map tool).
//
// Additive, single-pass over the already-built `actors` / `objectsByPath`.
// Existing buildings/belts consumers ignore this block; the Vehicle Map tool
// reads only `logistics`. Topology is documented in docs/save_game.md
// ("Vehicle / path / station topology").
//
//   stations: truck/train stations  -> name, world pos, GUID, cargo items
//   vehicles: wheeled vehicles       -> type, pos, ordered route GUIDs, fuel
//   paths:    Build_VehiclePath segs -> world polyline (the road map)
//
// Station<->vehicle linkage is left to the C++ side, which resolves a vehicle's
// ordered `route_guids` against each station's `guid`.
// ---------------------------------------------------------------------------

function readTextProp(props, name) {
    // TextProperty surfaces the display string nested under .value.value.
    let cur = findProp(props, name);
    for (let i = 0; i < 5 && cur && typeof cur === "object"; i++) {
        if (typeof cur.value === "string") return cur.value;
        cur = cur.value;
    }
    return typeof cur === "string" ? cur : "";
}

function readBoolProp(props, name) {
    const v = findProp(props, name);
    if (typeof v === "boolean") return v;
    if (v && typeof v === "object") {
        if (typeof v.value === "boolean") return v.value;
        if (typeof v.value === "number") return v.value !== 0;
    }
    return false;
}

// mPathNodeGUID is a StructProperty whose value is [a,b,c,d] (4x uint32).
// Render it as a stable "a_b_c_d" key so vehicle routes and station identifiers
// can be matched.
function readGuidKey(props, name) {
    const v = findProp(props, name);
    let arr = v;
    if (v && typeof v === "object" && !Array.isArray(v)) arr = v.value;
    if (Array.isArray(arr) && arr.length >= 4) return arr.slice(0, 4).join("_");
    return "";
}

// mVehicleRoute is an ArrayProperty of Guid -> ordered list of "a_b_c_d" keys.
function readGuidArray(props, name) {
    const v = findProp(props, name);
    const list = (v && (v.values || v.value)) || [];
    const out = [];
    if (!Array.isArray(list)) return out;
    for (const g of list) {
        const arr = Array.isArray(g) ? g : (g && g.value);
        if (Array.isArray(arr) && arr.length >= 4) out.push(arr.slice(0, 4).join("_"));
    }
    return out;
}

function isWheeledVehicleClass(stripped) {
    return /^BP_(Truck|Tractor|Explorer|CyberWagon)/.test(stripped);
}

function classifyVehicle(stripped) {
    if (/Tractor/i.test(stripped)) return "tractor";
    if (/Explorer/i.test(stripped)) return "explorer";
    if (/CyberWagon/i.test(stripped)) return "cyberwagon";
    if (/Truck/i.test(stripped)) return "truck";
    if (/Locomotive/i.test(stripped)) return "train";
    return "truck";
}

// Rotate a local vector by a quaternion (UE order), so segment-local spline
// points land at their true world position.
function quatRotate(q, v) {
    const qx = q.x || 0, qy = q.y || 0, qz = q.z || 0, qw = (q.w === undefined ? 1 : q.w);
    const vx = v.x || 0, vy = v.y || 0, vz = v.z || 0;
    const tx = 2 * (qy * vz - qz * vy);
    const ty = 2 * (qz * vx - qx * vz);
    const tz = 2 * (qx * vy - qy * vx);
    return {
        x: vx + qw * tx + (qy * tz - qz * ty),
        y: vy + qw * ty + (qz * tx - qx * tz),
        z: vz + qw * tz + (qx * ty - qy * tx),
    };
}

// Collect distinct Desc_* item class stems appearing anywhere under an object
// (used to read a station's cargo items from its .inventory component).
function collectItemClasses(obj, set, depth = 0) {
    if (!obj || depth > 14) return;
    if (typeof obj === "string") {
        const m = obj.match(/Desc_[A-Za-z0-9_]+/);
        if (m) set.add(m[0]);
        return;
    }
    if (typeof obj !== "object") return;
    if (Array.isArray(obj)) { for (const e of obj) collectItemClasses(e, set, depth + 1); return; }
    for (const k of Object.keys(obj)) collectItemClasses(obj[k], set, depth + 1);
}

function buildLogistics() {
    const stations = [];
    const vehicles = [];
    const paths = [];

    // node path alias -> mPathNetworkID
    const nodeNetwork = new Map();
    for (const a of actors) {
        const s = stripClassWrap(a.className || a.ClassName || a.typePath || "");
        if (!s.startsWith("Build_VehiclePathNode")) continue;
        const net = readNumberProp(a.properties || a.Properties, "mPathNetworkID", -1);
        addPathMapAliases(nodeNetwork, a.instanceName || a.InstanceName, net);
    }
    const segmentNetwork = (segActor) => {
        const props = segActor.properties || segActor.Properties;
        for (const field of ["mStartNode", "mEndNode"]) {
            for (const r of readObjectRefPropPaths(props, field)) {
                const n = pathMapGet(nodeNetwork, r);
                if (n !== undefined) return n;
            }
        }
        return -1;
    };

    for (const a of actors) {
        const cls = a.className || a.ClassName || a.typePath || "";
        const kindStr = classifyBuilding(cls);
        const stripped = stripClassWrap(cls);

        // --- Stations ---
        if (kindStr === "truck_station" || kindStr === "train_station") {
            const props = a.properties || a.Properties;
            const id = a.instanceName || a.InstanceName;
            let name = "", guid = "";
            const ident = findObjectByRefPaths(objectsByPath, readObjectRefPropPaths(props, "mStationIdentifier"));
            if (ident) {
                const ip = ident.properties || ident.Properties;
                name = readTextProp(ip, "mStationName");
                guid = readGuidKey(ip, "mPathNodeGUID");
            }
            let net = -1;
            for (const r of readObjectRefPropPaths(props, "mDockingPathNode")) {
                const n = pathMapGet(nodeNetwork, r);
                if (n !== undefined) { net = n; break; }
            }
            const items = new Set();
            const inv = findObjectByRefPaths(objectsByPath, [id + ".inventory"]);
            if (inv) {
                const descs = new Set();
                collectItemClasses(inv.properties || inv.Properties, descs);
                for (const d of descs) { const dn = itemDisplayName(d); if (dn) items.add(dn); }
            }
            const docked = [];
            const vtWrap = findProp(props, "mVehicleTracking");
            const vtEntries = (vtWrap && (vtWrap.values || vtWrap.value)) || [];
            if (Array.isArray(vtEntries)) {
                for (const e of vtEntries) {
                    const ep = e && (e.properties || e.Properties);
                    const refs = readObjectRefPropPaths(ep, "OwnerVehicle");
                    if (refs.length) docked.push(normalizeObjectPath(refs[0]));
                }
            }
            stations.push({
                id,
                kind: kindStr === "train_station" ? "train" : "truck",
                name: name || stripped,
                guid,
                network_id: net,
                pos: readPos(a),
                items: Array.from(items),
                docked_vehicle_ids: docked,
            });
            continue;
        }

        // --- Vehicles ---
        if (isWheeledVehicleClass(stripped) || /Locomotive/i.test(stripped)) {
            const props = a.properties || a.Properties;
            const id = a.instanceName || a.InstanceName;
            let name = "", route = [], fuel = "", autopilot = false;
            const ident = findObjectByRefPaths(objectsByPath, readObjectRefPropPaths(props, "mVehicleIdentifier"));
            if (ident) {
                const ip = ident.properties || ident.Properties;
                name = readTextProp(ip, "mVehicleName");
                route = readGuidArray(ip, "mVehicleRoute");
                fuel = itemDisplayName(readStringProp(ip, "mFuelTypeDescriptor"));
                autopilot = readBoolProp(ip, "mIsAutopilotEnabled");
            }
            let net = -1;
            const seg = findObjectByRefPaths(objectsByPath, readObjectRefPropPaths(props, "mCurrentVehiclePathSegment"));
            if (seg) net = segmentNetwork(seg);
            vehicles.push({
                id,
                type: classifyVehicle(stripped),
                name: name || stripped,
                pos: readPos(a),
                network_id: net,
                route_guids: route,
                fuel,
                autopilot,
            });
            continue;
        }

        // --- Path segments ---
        if (stripped.startsWith("Build_VehiclePath_Universal")) {
            const props = a.properties || a.Properties;
            const t = a.transform || a.Transform || {};
            const rot = t.rotation || t.Rotation || { x: 0, y: 0, z: 0, w: 1 };
            const tr = t.translation || t.Translation || { x: 0, y: 0, z: 0 };
            const spWrap = findProp(props, "mSplinePoints");
            const pts = (spWrap && (spWrap.values || spWrap.value)) || [];
            const wp = [];
            if (Array.isArray(pts)) {
                for (const sp of pts) {
                    const spp = sp && (sp.properties || sp.Properties);
                    const loc = spp && findProp(spp, "Location");
                    const lv = loc && (loc.value !== undefined ? loc.value : loc);
                    if (!lv || typeof lv.x !== "number") continue;
                    const w = quatRotate(rot, lv);
                    wp.push([w.x + (tr.x || 0), w.y + (tr.y || 0)]);
                }
            }
            if (wp.length >= 2) {
                paths.push({
                    id: a.instanceName || a.InstanceName,
                    kind: "road",
                    network_id: segmentNetwork(a),
                    waypoints: wp,
                });
            }
            continue;
        }
    }

    return { stations, vehicles, paths };
}

let logistics = { stations: [], vehicles: [], paths: [] };
try {
    logistics = buildLogistics();
} catch (e) {
    warnings.push("[logistics] extraction failed: " + (e && e.message ? e.message : String(e)));
}
warnings.push("[logistics] " + logistics.stations.length + " station(s), "
    + logistics.vehicles.length + " vehicle(s), " + logistics.paths.length + " path segment(s)");

const out = {
    version: 1,
    buildings,
    belts,
    warnings,
    logistics,
    pipe_networks,
};

process.stdout.write(JSON.stringify(out));
