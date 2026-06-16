# Satisfactory Save Game Format (`.sav`)

A practical reference for how a Satisfactory save file is structured, written while
decompiling `example_save.sav` (session `teste-1.2-beta`, build `491125`,
save version `60`, ~23,600 objects). It documents both the on‑disk layout and the
parsed object model that this project consumes via
[`@etothepii/satisfactory-file-parser`](https://www.npmjs.com/package/@etothepii/satisfactory-file-parser)
(see `ficsit-companion/tools/sav_import/wrapper.js`).

Field names below are the parser's JavaScript model. The raw binary uses Unreal
Engine's property serialization; the parser turns it into the JSON‑like objects
shown here. Where a name differs from the in‑game/UE name it is noted.

---

## 1. Big picture

A `.sav` is an Unreal Engine actor/component snapshot of one game session:

```
.sav file
├── Header               (uncompressed, plain UE-serialized struct)
└── Body                 (split into PZ/zlib-compressed chunks)
    ├── Save-body validation (world-partition grid hashes)
    └── Levels[]          (the persistent level + streamed sub-levels)
        └── Objects[]      (SaveEntity = actors, SaveComponent = components)
            └── Properties (the UE property bag) + SpecialProperties
```

Everything the player built, every creature, resource node, vehicle path and
power line is an **object** living inside a **level**. Machines reference their
recipe, their inventory and their neighbours through **object references**
(`levelName` + `pathName`). Belts and power lines store their topology in
**special properties**.

The parsed top‑level object exposes:

| Key | Meaning |
|-----|---------|
| `header` | Metadata block (see §3). Read without decompressing the body. |
| `compressionInfo` | How the body chunks are compressed (see §4). |
| `objectVersionData` | UE/engine and custom version numbers (see §5). |
| `saveBodyValidation` | World‑partition grid hashes (see §6). |
| `levels` | Map of level name → level data (see §7). The bulk of the file. |
| `unresolvedWorldSaveData` | Leftover world data the parser didn't model. |
| `name` | The save name passed to the parser. |

---

## 2. On‑disk binary layout

1. **Header** — serialized first and left **uncompressed** so tools (and the
   in‑game load menu) can show session name, play time and build version
   without inflating the whole file.
2. **Compressed body** — a sequence of chunks. Each chunk has a small header
   (`packageFileTag`, max uncompressed chunk size) followed by a compressed
   payload. In this save:
   - `packageFileTag`: `2653586369` (`0x9E2A83C1`, the UE save magic)
   - `chunkHeaderVersion`: `572662306`
   - `maxUncompressedChunkContentSize`: `131072` (128 KiB per chunk)
   - `compressionAlgorithm`: `3` (zlib)
3. Concatenating the inflated chunks yields the **body byte stream**, which is
   then parsed as: save‑body validation → per‑level TOC + object data.

The importer never touches this layer directly — the parser inflates the chunks
and hands back the structured model. It is documented here only so the chunked,
compressed nature of the format is understood (you cannot `grep` a `.sav`).

---

## 3. Header

Plain metadata. Real values from `example_save.sav`:

| Field | Example value | Meaning |
|-------|---------------|---------|
| `saveHeaderType` | `14` | Header schema version. |
| `saveVersion` | `60` | Save body schema version (gates how the body is read). |
| `buildVersion` | `491125` | Game build (changelist) that wrote the save. |
| `mapName` | `Persistent_Level` | Root map. |
| `mapOptions` | `?skiponboarding?...` | URL‑style session options (client identity, player snapshot). |
| `sessionName` | `teste-1.2-beta` | Player‑visible session name. |
| `playDurationSeconds` | `305554` | Total play time (~85 h). |
| `saveDateTime` | `1779329401958` | Save timestamp (UE ticks/epoch‑style number). |
| `sessionVisibility` | `0` | Private/friends/public flag. |
| `saveName` | `teste-1.2-beta_autosave_1` | File stem (note the `_autosave_1` suffix the importer strips when discovering worlds). |
| `fEditorObjectVersion` | `40` | UE editor object version. |
| `isModdedSave` / `rawModMetadataString` | `0` / `""` | Mod flag + SML metadata. |
| `saveIdentifier` | `E-pFWUjD0kMBYhScZ4VAhA` | Unique save id. |
| `partitionEnabledFlag` | `true` | World‑partition (streaming) enabled. |
| `consistencyHashBytes` | `{isValid, hash[16]}` | Integrity hash of the body. |
| `creativeModeEnabled` | `false` | Creative/advanced‑game‑settings flag. |

---

## 4. Compression info

`compressionInfo` (see values in §2) describes the chunk framing. Two practical
consequences:

- The body is **always chunked**; a single object can straddle a chunk boundary,
  so you must inflate and concatenate before parsing.
- `compressionAlgorithm: 3` = zlib in U1.0+/1.2 saves. Older formats used a
  different value; the parser handles the variants.

---

## 5. Object version data

`objectVersionData` carries the UE/engine versioning needed to interpret
property layouts:

| Field | Meaning |
|-------|---------|
| `saveObjectVersionDataVersion` | Version of this block itself. |
| `packageFileVersion` | UE package file version. |
| `licenceVersion` | UE licensee version. |
| `engineVersion` | Engine version string. |
| `customVersionContainer` | List of per‑subsystem custom version GUIDs+numbers (FactoryGame uses these to migrate old property formats). |

These matter when a property's binary layout changed between game versions;
the parser consults them to pick the right reader.

---

## 6. Save‑body validation (`saveBodyValidation.grids`)

Because the world uses **partitioning**, the body begins with a validation
structure: a set of named **grids**, each with child cells keyed by an id and a
hash. From this save:

```
grids:
  None      → cellSize 0,      gridHash 2291418232, children { None: ... }
  MainGrid  → children { "2RXE4Q564LS1...": <hash>, ... }   (one entry per streamed cell)
  ...
```

Each grid level (`MainGrid`, landscape grids, foliage grids, …) lists the cells
it covers and a hash per cell, used to validate/stream the matching level data.
For a production planner this block is **opaque** — it's read past to reach the
levels. It exists so the engine can stream and integrity‑check chunks of the
world independently.

---

## 7. Levels

`levels` is a **map of level name → level**. This save has **1,573 levels**: one
`Persistent_Level` plus streamed sub‑levels (landscape/foliage/partition cells).
Almost everything the importer cares about lives in `Persistent_Level`
(**16,671 objects** here).

Each level object:

| Field | Meaning |
|-------|---------|
| `name` | Level name, e.g. `Persistent_Level`. |
| `objects` | The actors + components in this level (see §8). |
| `collectables` | Save‑specific collectable references (often empty). |
| `writesDestroyedActorsInTOCBlob` | Flag for the destroyed‑actors encoding. |
| `destroyedActorsMap` | Actors removed from the level (so defaults aren't re‑spawned). |

To enumerate the whole world, iterate every level and concatenate its
`objects`. Across all levels this save holds **23,608 objects**.

---

## 8. Objects: entities and components

Every entry in a level's `objects` is one of two `type`s:

| `type` | Count here | What it is |
|--------|-----------:|------------|
| `SaveEntity` | 14,084 | An **actor** — a placed thing with a world transform (machines, belts, creatures, nodes, vehicles). |
| `SaveComponent` | 9,524 | A **component** owned by an actor (a conveyor connector, power connection, inventory, …). Has no transform of its own; positioned relative to its owner. |

### Common fields (both kinds)

| Field | Meaning |
|-------|---------|
| `typePath` | The UE class path, e.g. `…/Build_ConstructorMk1.Build_ConstructorMk1_C`. **This is how the importer classifies a building.** |
| `instanceName` | Fully qualified instance path, e.g. `Persistent_Level:PersistentLevel.Build_TruckStation_C_2146904052`. Used as the object's unique id and as the target of object references. |
| `rootObject` | Owning save/root id. |
| `flags` | Object flags. |
| `properties` | The property bag (see §9). |
| `specialProperties` | Type‑specific extra data (see §11); `EmptySpecialProperties` when none. |
| `trailingData` | Bytes after the properties the parser didn't model (kept for round‑trip). |
| `parentEntityName` | For components, the `instanceName` of the owning actor. |

### Actor‑only fields (`SaveEntity`)

| Field | Meaning |
|-------|---------|
| `transform` | `{ rotation{x,y,z,w}, translation{x,y,z}, scale3d{x,y,z} }` — world placement. `translation` (cm) is what the importer scales into editor coordinates. |
| `components` | List of object references to the actor's owned components (its conveyor connectors, power connection, inventories, …). |
| `parentObject` / `needTransform` / `wasPlacedInLevel` | Attachment + placement bookkeeping. |

**The actor ↔ component split is central to graph reconstruction:** a machine
actor lists its connector components in `components`; each connector component
records what it's wired to. See §10.

---

## 9. Properties (the UE property bag)

`properties` is a **map of property name → property**. (Some parser builds expose
it as an array of `{name,value}`; `wrapper.js` handles both.) Each property is a
**tagged value**:

```jsonc
"mCurrentRecipe": {
  "type": "ObjectProperty",            // how the value is encoded
  "name": "mCurrentRecipe",
  "propertyTagType": { "name": "ObjectProperty", "children": [] },
  "value": { "levelName": "", "pathName": ".../Recipe_IronRod.Recipe_IronRod_C" }
}
```

```jsonc
"mCurrentManufacturingProgress": {
  "type": "FloatProperty",
  "value": 0.0037
}
```

### Property types seen in this save (by frequency)

| Type | Count | Notes |
|------|------:|-------|
| `ObjectProperty` | 16,675 | Reference to another object (`{levelName, pathName}`). |
| `StructProperty` | 13,365 | Nested struct (transforms, colors, inventory stacks, …). |
| `ArrayProperty` | 11,913 | List of values/structs (inventory slots, connection lists). |
| `IntProperty` | 8,775 | 32‑bit int. |
| `FloatProperty` | 3,256 | 32‑bit float (clock, progress). |
| `BoolProperty` | 1,730 | Flags (e.g. station load mode). |
| `ByteProperty` | 705 | Byte / small enum. |
| `TextProperty` | 426 | Localized text (names/labels). |
| `EnumProperty` | 123 | Named enum value. |
| `Int8Property` | 101 | |
| `StrProperty` | 9 | Raw string. |
| `MapProperty` | 6 | Key→value map. |
| `SoftObjectProperty` | 4 | Lazy object reference. |
| `SetProperty` | 2 | |
| `UInt32Property` / `NameProperty` | 1 each | |

### Properties the importer actually reads

- `mCurrentRecipe` (ObjectProperty) → which recipe a manufacturer runs.
- `mCurrentItem` (ObjectProperty) → the item an organizer/extractor carries.
- `mCurrentPotential` (FloatProperty) → overclock multiplier.
- `mProductionBoost…` / `mNumProductionShards` → somersloop boost.
- `mInputInventory` / `mOutputInventory` / `FuelInventory` (StructProperty) →
  inventories; the presence of a separate `FuelInventory` is what marks a
  station's fuel side.
- `mConnectedComponent` (ObjectProperty, on connector components) → belt/wire
  topology (see §10).

> **Note on missing fields.** Unreal only serializes properties that differ from
> the class default. So an unconnected connector has an **empty** property bag,
> and a connector's direction (input/output) is *not* stored per‑object — it's
> implied by the component's **name** (`Input0`, `Output1`, `ConveyorAny0`). The
> importer relies on these name conventions.

---

## 10. Object references & graph topology

Cross‑object links use an **object reference**:

```jsonc
{ "levelName": "Persistent_Level", "pathName": "Persistent_Level:PersistentLevel.Build_ConveyorBeltMk1_C_2146898367.ConveyorAny1" }
```

`pathName` targets either an actor or a specific component on one. This is the
backbone of reconstructing the factory graph:

- **Recipes/items:** `mCurrentRecipe.value.pathName` →
  `…/Recipe_IronRod.Recipe_IronRod_C`.
- **Belts/pipes:** each machine actor owns `FGFactoryConnectionComponent`s named
  `Input<N>` / `Output<N>` (or `ConveyorAny<N>` on belts/splitters). A connector's
  `mConnectedComponent` points at the connector on the other end of the wire —
  often a belt segment, whose *other* `ConveyorAny` connector points onward. The
  importer **walks this chain**, hopping belt→belt, to collapse a run of conveyor
  segments into one logical edge between two buildings (see
  `followBeltChain` in `wrapper.js`).
- **Components:** an actor's `components` list references the connectors it owns.

### Worked example — Truck Station connectors

A `Build_TruckStation_C` actor owns these connector components:

```
Input0, Input1, Input2, Output0, Output1, FuelInventory, inventory, powerInfo, …
```

The two cargo lanes are the `Input<k>`/`Output<k>` **pairs** (0 and 1). The
**unpaired `Input2`** is the **fuel inlet** — despite being named like a normal
input, it has no matching `Output2`. The importer uses exactly this
"Input with no matching Output" rule to detect the fuel belt
(`stationFuelConnectorSuffix` in `wrapper.js`), because the name alone doesn't
say "fuel". Connector numbering here is **0‑based**.

## 10b. Pipe (fluid) topology

Pipes are reconstructed by **network id**, not chain-walking. Every pipe
connector (`FGPipeConnectionFactory` on machines, `FGPipeConnectionComponent` on
plumbing) carries `mPipeNetworkID`. `FGPipeNetwork` actors map that id to the
fluid (`mFluidDescriptor`) and list the network's members. Pumps
(`Build_PipelinePump`), junctions (`Build_PipelineJunction_*`) and segments
(`Build_Pipeline`) all share one network id and are **not** emitted as nodes:
grouping machine ports by network id absorbs them. The importer classifies each
machine port as producer/consumer by matching the network fluid against the
machine's recipe pins, then wires either a direct producer-to-consumer link
(1:1) or one synthetic `PipeJunction` manifold node (N:M).
`Build_PipeStorageTank` / `Build_IndustrialTank` become `FluidBuffer` /
`IndustrialFluidBuffer` logistics nodes. No pressure/head is modeled.

---

## 11. Special properties

`specialProperties` carries type‑specific data that doesn't fit the generic
property bag. Types present in this save:

| `type` | Count | Carries |
|--------|------:|---------|
| `EmptySpecialProperties` | 20,718 | Nothing (most objects). |
| `ConveyorSpecialProperties` | 1,443 | A conveyor belt segment's **items in transit** (the items physically riding the belt and their positions). |
| `ConveyorChainActorSpecialProperties` | 853 | A `FGConveyorChainActor` — the engine groups many belt segments into one chain actor for performance; this holds the chain's segment list. |
| `PowerLineSpecialProperties` | 570 | A power line's two endpoint connections. |
| `VehicleSpecialProperties` | 19 | Vehicle physics/path data. |
| `ObjectsListSpecialProperties` | 2 | A list of object references (subsystem bookkeeping). |
| `BuildableSubsystemSpecialProperties` | 1 | The buildable subsystem's data. |
| `CircuitSpecialProperties` | 1 | A power circuit's membership. |
| `PlayerSpecialProperties` | 1 | Player state. |

For the importer, the relevant ones are the **conveyor** types (belt topology)
and, indirectly, power lines/circuits (ignored — only conveyors are wired).

---

## 12. What the world is mostly made of

Top `SaveEntity` classes in `example_save.sav` (illustrates that most objects are
**not** factory machines):

```
1435  BP_BerryBush          (flora)
1048  BP_CreatureSpawner    (fauna)
1026  BP_Shroom_01          (flora)
 994  BP_ResourceDeposit    (minable deposits)
 848  FGConveyorChainActor  (belt chains)
 782  Build_ConveyorBeltMk1 (belts)
 627  Build_VehiclePath_Universal
 570  Build_PowerLine
 552  BP_NutBush
 489  Build_VehiclePathNode_Default
 459  BP_ResourceNode
 361  FGBlueprintProxy
 317  BP_SporeFlower
 308  Build_ConveyorLiftMk1
 253  FGItemPickup_Spawnable
```

The importer therefore **classifies by `typePath`** and keeps only buildings it
understands (manufacturers, miners, splitters/mergers, stations, storage, sinks),
ignoring flora, fauna, resource nodes, vehicle paths and power infrastructure.

---

## 13. From save to production graph (importer summary)

`ficsit-companion/tools/sav_import/wrapper.js` turns the parsed save into a small
JSON the C++ side (`sav_import.cpp`) builds a node graph from:

1. **Enumerate** every object in every level.
2. **Classify** actors by `typePath`; keep the supported building kinds.
3. **Read** per‑building info from `properties` (recipe, item, clock, somersloops,
   purity for miners).
4. **Walk conveyor chains** via `FGFactoryConnectionComponent.mConnectedComponent`
   to collapse belt runs into single edges, recording each endpoint's
   building + connector port + direction (`in`/`out`/`any`) + role (`fuel`).
5. **Emit** `{ buildings[], belts[] }` JSON.

The C++ side then resolves item types and rates, taking belt direction, the
station fuel inlet, and serial splitter/merger/storage chains into account.

---

## Vehicle / path / station topology (Vehicle Map tool)

Findings from running the discovery dump (`tools/sav_import/dump_logistics.js`) on
`example_save.sav`. This save contains **road vehicles only** (no trains), so the
train side below is documented from class names but unverified against this sample.

### Actor inventory (this save)
| Class (stem) | Count | Role |
|---|---|---|
| `Build_VehiclePath_Universal_C` | 627 | road **segment**: a spline + start/end node refs |
| `Build_VehiclePathNode_Default_C` | 489 | road graph node (junction / waypoint) |
| `Build_VehiclePathNode_DockingStation_C` | 46 | road graph node owned by a station |
| `Build_TruckStation_C` | 46 | truck station building |
| `FGDockingStationIdentifier` | 46 | station name + GUID (one per station) |
| `FGWheeledVehicleIdentifier` | 19 | vehicle name + route + fuel (one per vehicle) |
| `BP_Tractor_C` | 18 | the road vehicles (Truck/Tractor/Explorer/CyberWagon) |

### Station — `Build_TruckStation_C`
- `transform.translation` → world position `[x, y, z]`.
- `mStationIdentifier` → ObjectRef to its `FGDockingStationIdentifier`, which holds:
  - `mStationName` (TextProperty) → display name, e.g. `"Carvao-energia"`
    (read `.value.value`).
  - `mPathNodeGUID` (StructProperty) → **4×uint32 GUID** identifying this station
    in vehicle routes (see below).
- `mDockingPathNode` → ObjectRef to the station's `Build_VehiclePathNode_DockingStation_C`
  (which links the station into the road graph; carries `mPathNetworkID`).
- `mVehicleTracking[]` → array of `DockingStationVehicleTracking`, each with an
  `OwnerVehicle` ObjectRef → the vehicle currently associated with this dock.
- Cargo items: the station's `.inventory` save component (path = station instanceName
  + `".inventory"`) holds `mInventoryStacks[]` whose entries reference `Desc_*` item
  classes (e.g. `Desc_Coal`); `mAllowedItemDescriptors` lists configured item filters.
  Fuel lives separately in the `.FuelInventory` component. There is **no single
  load/unload-mode bool**; v1 treats the union of inventory items as the station's
  cargo `items` (load vs unload is not distinguished).

### Vehicle — `BP_Tractor_C` / `BP_Truck_C` / `BP_Explorer_C` / `BP_CyberWagon_C`
- `typePath` stem → vehicle type.
- `transform.translation` → current world position.
- `mVehicleIdentifier` → ObjectRef to `FGWheeledVehicleIdentifier`, which holds:
  - `mVehicleName` (TextProperty) → display name (e.g. `"Tractor"`).
  - `mVehicleRoute` (ArrayProperty of Guid) → **ordered list of station GUIDs** =
    the recorded route's stops. Each GUID matches a station identifier's
    `mPathNodeGUID`. This is the clean vehicle→station linkage (and stop order).
  - `mFuelTypeDescriptor` → `Desc_*` fuel item.
  - `mIsAutopilotEnabled` (bool).
- `mCurrentVehiclePathSegment` → ObjectRef to the `Build_VehiclePath_Universal` the
  vehicle is currently on. Following `segment.mStartNode → node.mPathNetworkID` gives
  the road **network id**.

### Path segments + graph
- `Build_VehiclePath_Universal_C`:
  - `mSplinePoints[]` (`SplinePointData`) → each has `Location` (Vector). **Locations
    are LOCAL to the segment** — first point is `(0,0,0)`; world point =
    `segment.transform` applied to `Location` (rotate by the transform quaternion,
    then add translation). For a 2D top-down map only x/y matter.
  - `mStartNode` / `mEndNode` → ObjectRefs to path nodes.
- `Build_VehiclePathNode_*`:
  - `mPathNetworkID` (int) → which connected road network the node belongs to.
  - `mArrivingConnections[]` / `mLeavingConnections[]` → segment ObjectRefs (graph edges).
  - DockingStation nodes have `parentObject` → the owning `Build_TruckStation_C`.

### Network id vs route — important
`mPathNetworkID` is the **physical connected road graph**, not a vehicle's route. In
this save one network (`2276`) contains ~490 nodes and almost every station, so it is
*too coarse* to distinguish individual vehicle routes. The per-vehicle **route** is
`mVehicleRoute` (the ordered station GUIDs). Therefore the tool:
- draws **all segments** as the faint background road map (grouped by `network_id`);
- treats a vehicle's **route** = its ordered `mVehicleRoute` stations, and draws a
  connecting polyline between consecutive route stations (the true driven sub-path
  through the segment graph is not reconstructed in v1).

### Linkage summary (resolved C++-side)
- station GUID (`FGDockingStationIdentifier.mPathNodeGUID`) → station.
- vehicle → ordered stations via `mVehicleRoute` GUIDs.
- station → serving vehicles = vehicles whose route contains the station's GUID
  (plus live `mVehicleTracking.OwnerVehicle`).
- orphans = stations served by no vehicle / vehicles with an empty route.

### Train side (unverified — no trains in sample)
Train stations (`Build_TrainStation_C` / `Build_RailroadStation_C`), locomotives
(`*Locomotive*`), freight wagons, and `RailroadTrack` actors would be classified with
`kind = "train"` / `"rail"`. Deferred until a save with trains is available.

---

### References
- Parser: `@etothepii/satisfactory-file-parser` (handles the binary/compression layer).
- Importer adapter: `ficsit-companion/tools/sav_import/wrapper.js`.
- Graph builder: `ficsit-companion/src/sav_import.cpp`.
- Sample analysed: `example_save.sav` (`teste-1.2-beta`, build 491125, save version 60).
