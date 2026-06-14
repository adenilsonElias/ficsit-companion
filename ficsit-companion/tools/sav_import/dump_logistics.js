#!/usr/bin/env node
/*
 * THROWAWAY discovery script (plan task 2).
 *
 * Runs @etothepii/satisfactory-file-parser on a .sav and prints the raw shape
 * of the vehicle / path / station actors so we can design the `logistics`
 * extraction in wrapper.js. Not shipped; not wired into the build.
 *
 * Usage: node dump_logistics.js <save.sav>
 */
const fs = require("fs");
const path = require("path");

function die(m) { process.stderr.write("[dump] " + m + "\n"); process.exit(1); }
if (process.argv.length < 3) die("usage: node dump_logistics.js <save.sav>");
const savPath = process.argv[2];
if (!fs.existsSync(savPath)) die("not found: " + savPath);

let Parser;
try { Parser = require("@etothepii/satisfactory-file-parser"); }
catch (e) { die("parser not installed: " + e); }

const nodeBuf = fs.readFileSync(savPath);
const bytes = nodeBuf.buffer.slice(nodeBuf.byteOffset, nodeBuf.byteOffset + nodeBuf.byteLength);
let save;
if (typeof Parser.Parser === "function") save = Parser.Parser.ParseSave(path.basename(savPath, ".sav"), bytes);
else if (typeof Parser.parseSave === "function") save = Parser.parseSave(path.basename(savPath, ".sav"), bytes);
else die("no ParseSave entry point");

function allActors(save) {
    const out = [];
    const levels = save.levels || save.Levels || {};
    const lvlList = Array.isArray(levels) ? levels : Object.values(levels);
    for (const lvl of lvlList) {
        const objs = (lvl && (lvl.objects || lvl.Objects)) || [];
        for (const o of objs) out.push(o);
    }
    if (Array.isArray(save.objects)) out.push(...save.objects);
    return out;
}

const actors = allActors(save);
process.stderr.write("[dump] total actors: " + actors.length + "\n");

// Count by class-name stem so we can see what's actually present.
function stem(cls) {
    const d = cls.lastIndexOf(".");
    let s = d >= 0 ? cls.substring(d + 1) : cls;
    if (s.endsWith("_C")) s = s.substring(0, s.length - 2);
    return s;
}
const counts = new Map();
for (const a of actors) {
    const cls = a.className || a.ClassName || a.typePath || "";
    const s = stem(cls);
    counts.set(s, (counts.get(s) || 0) + 1);
}
// Print the classes we care about + anything vehicle/path/station/railroad-ish.
const RE = /(Truck|Vehicle|Path|Train|Railroad|Rail|Station|Dock|Drone|Locomotive|FreightWagon|Wheeled)/i;
const interesting = [...counts.entries()].filter(([k]) => RE.test(k)).sort((a, b) => b[1] - a[1]);
process.stderr.write("[dump] interesting classes:\n");
for (const [k, v] of interesting) process.stderr.write("  " + v + "  " + k + "\n");

// Dump one full sample of each interesting class so we can read property names.
function sampleOf(predicate) {
    return actors.find(a => predicate(stem(a.className || a.ClassName || a.typePath || "")));
}
const wanted = [
    ["TruckStation", s => s.startsWith("Build_TruckStation")],
    ["TrainStation", s => s.startsWith("Build_TrainStation") || s.startsWith("Build_RailroadStation")],
    ["TrainDock", s => s.startsWith("Build_TrainDockingStation")],
    ["VehiclePath", s => s.startsWith("Build_VehiclePath") && !s.includes("Node")],
    ["VehiclePathNode", s => s.startsWith("Build_VehiclePathNode")],
    ["Truck", s => s === "BP_Truck" || s.startsWith("BP_Truck") || s.startsWith("Vehicle_Truck")],
    ["WheeledVehicle", s => s.startsWith("BP_") && /Truck|Tractor|Explorer|CyberWagon|Wagon/i.test(s)],
    ["Locomotive", s => /Locomotive/i.test(s)],
    ["FreightWagon", s => /FreightWagon|Wagon/i.test(s)],
    ["RailroadTrack", s => /RailroadTrack/i.test(s)],
];

const samples = {};
for (const [label, pred] of wanted) {
    const a = sampleOf(pred);
    if (a) samples[label] = a;
}

// Also dump the raw VehicleSpecialProperties container if present at top level.
process.stderr.write("[dump] save top-level keys: " + Object.keys(save).join(", ") + "\n");

// Trim a deep object for printing (cut huge arrays / binary).
function trim(v, depth = 0) {
    if (depth > 6) return "…";
    if (Array.isArray(v)) return v.slice(0, 6).map(x => trim(x, depth + 1)).concat(v.length > 6 ? ["…+" + (v.length - 6)] : []);
    if (v && typeof v === "object") {
        const o = {};
        for (const k of Object.keys(v)) {
            if (k === "data" && v[k] && v[k].byteLength) { o[k] = "<bytes " + v[k].byteLength + ">"; continue; }
            o[k] = trim(v[k], depth + 1);
        }
        return o;
    }
    return v;
}

process.stdout.write(JSON.stringify(trim(samples), null, 2));
