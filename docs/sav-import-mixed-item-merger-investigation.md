# Investigation: `.sav` importer produces mixed-item mergers (wrong connections)

**Started:** 2026-06-11 · **Status:** Phase 1 (root-cause investigation) — IN PROGRESS, not yet root-caused.
**Method:** superpowers:systematic-debugging. **Constraints:** NO GIT (edit in place; no commits/branches — see `[[git-preference]]`).
**User decision:** Fix the **importer** (`tools/sav_import`), not the app UI or a one-off save cleanup.

## How to resume
Read this file, then continue at **"Next steps"** below. The Iron Law still applies: **do not change importer code until we've confirmed whether the bad edge is a faithful read of the save or a walker artifact.**

---

## Symptom (what the user reported)
- In the running app's node graph, almost all links are green but some are **red** (the editor's error state for a connection the rate solver can't satisfy). Screenshot: `wrong_connection.png` (repo root).
- The user pointed at a **`Merger` labeled "Copper Sheet"** whose **two inputs are actually Plastic**, yet the merger's output is treated as Copper Sheet. A merger must carry exactly one item; mixing Plastic into a Copper-Sheet stream is wrong.
- This came from importing the user's live save **world `teste-1.2-beta`** (a 673-node / 791-link graph).

## Evidence gathered (from `build/ficsit-companion/Release/last_session.fcs`)
`.fcs` format facts (confirmed by reading `src/infra/session_serializer.cpp`):
- Link endpoints `{"node":k,"pin":i}` use **0-based array index** `k` into the `nodes` array (`get_node_index` returns position). Links always run **output(start) → input(end)**.
- `Craft`/`Extractor`/`Group` nodes do **not** serialize their pins (rebuilt from recipe/kind on load), so their `outs`/`ins` rates are absent in the `.fcs`. Merger/Splitter/Sink DO store pin rates (num/den).
- Node `kind` enum: 0 Craft, 1 CustomSplitter, 2 Merger, 3 Group, 4 GameSplitter, 5 Sink, 6 Extractor, 7 Logistics.

Findings:
- **25 rate-mismatched links** total (out of 791) — output pin rate ≠ connected input pin rate. Many are `Merger → GameSplitter` with float-derived fraction rates (denominators that are powers of two, e.g. `…/1099511627776 = 2^40`), suggesting **double→fraction rounding** in the importer's rate computation — a *separate, likely cosmetic* issue from the topology bug.
- **The Copper Sheet case (the one the user circled):**
  - `node577 Merger(Copper Sheet)`: ins = **[80, 20, 20]** (sum 120), out = **120**. Internally balanced as a merger.
    - inpin0 ← `node576 Merger(Copper Sheet)` out 80  ✅ copper sheet
    - inpin1 ← `node569 Craft(Plastic)`  ❌ plastic feeding a copper-sheet merger
    - inpin2 ← `node570 Craft(Plastic)`  ❌ plastic feeding a copper-sheet merger
  - `node577` out 120 → `node578 GameSplitter(Copper Sheet)` in **105** ⇒ **120≠105 ⇒ red link** (the visible symptom).
  - `node578` outs = [45, 60, 0] (105 total) → `node573 GameSplitter(Copper Sheet)` (45) and `node587 Merger(Copper Sheet)` (60).
  - NOTE: did **not** yet verify the Plastic crafts' recipe truly outputs Plastic (user interrupted that check, but the user confirmed the inputs are Plastic visually). Worth a 1-line confirmation.

## Continuation findings (2026-06-11)
- Reproduced with newest matching save:
  `%LOCALAPPDATA%\FactoryGame\Saved\SaveGames\<steam-id>\teste-1.2-beta_autosave_1.sav`.
- Raw wrapper output aligns with the saved node indices:
  - building 569 = `Build_OilRefinery_C_2146772098`, recipe `Plastic`
  - building 570 = `Build_OilRefinery_C_2146771387`, recipe `Plastic`
  - building 577 = `Build_ConveyorAttachmentMerger_C_2146381744`
- The bad topology is already present in raw wrapper JSON:
  - `belt-676`: Plastic refinery 569 output -> merger 577 input 1
  - `belt-677`: Plastic refinery 570 output -> merger 577 input 2
  - `belt-684`: merger 576 -> merger 577 input 0
  - `belt-685`: merger 577 -> splitter 578
- Temporary trace instrumentation showed both bad Plastic edges follow valid
  `mConnectedComponent` chains. Every belt/lift actor in those paths had exactly
  one alternate conveyor component, ending at merger inputs `Input2` and
  `Input3`. Conclusion: **not a walker mis-hop**; the importer faithfully reads
  a physically mixed save topology.
- Implemented importer-side handling in `SavImport::BuildGraph`: after wiring,
  detect plain organizer/logistics nodes whose connected cargo pins imply more
  than one item, emit a `mixed item pass-through` warning, and keep those nodes
  untyped instead of stamping the whole chain as one item.

## Code understanding (verified)
### App-side item model — `src/domain/graph_item_resolve.cpp`
- `ResolveOrganizerItem(origin)` floods the organizer/logistics chain and returns the **first** craft/extractor item it reaches. It does **not** check that all producers feeding a merger agree.
- `RecalculateOrganizerItemChain(origin)` then forces that single item onto **every** pin in the connected organizer chain (`OrganizerNode::ChangeItem`), flooding both up- and downstream (stops at CustomSplitter "walls", skips fuel inlets).
- Consequence: a merger physically fed by Plastic + Copper-Sheet gets labeled with ONE item (Copper Sheet here), the other inputs are silently mislabeled, and the rate flow can't reconcile ⇒ red link downstream. **The app neither validates nor flags mixed-item merges.** (This is why the symptom appears, regardless of importer correctness.)

### Importer — `ficsit-companion/tools/sav_import/wrapper.js` (1142 lines)
Canonical source: `ficsit-companion/tools/sav_import/wrapper.js` (build copies exist under `build/.../Release/tools/sav_import/`). Invoke: `node wrapper.js <save.sav>` → JSON to stdout. **Node v18.20.8 is installed.**
- Class dispatch (`classify`, ~L246-267): `Build_ConveyorAttachmentMerger_C` → "merger", `…Splitter` variants → splitter kinds, `Build_ConveyorBelt/Pole/Lift/Ceiling` → "belt_pole" (treated as belt).
- **Belt-chain reduction** (L594-733): for each emitted building's **output** connection component, follow `mConnectedComponent`; `followBeltChain` hops belt→belt (picks the belt's OTHER port component) until it reaches a **non-belt** building component (the terminus). Emits one edge `{src building/port, dst building/port}` (L816-821) with `item_name:""` (items resolved later, app-side).
- Guards: `beltsWrongSide` (L799) drops edges whose terminus is also an "out" port (deemed walker artifacts); `followBeltChain` has cycle detection.
- Port index from component name suffix `Input<N>/Output<N>` → `N-1`; `ConveyorAny<N>` → dir "any" (L615-645). **Merger inputs may surface as `ConveyorAny` ports**, so `dstPort.dir` could be "any" (not dropped by the wrong-side guard).

## Hypotheses (ranked, UNTESTED)
1. **Walker mis-hop / cross-wire (importer bug).** `followBeltChain` or the OTHER-component selection (L714-723) picks the wrong continuation at some belt/junction, fabricating a Plastic-constructor→Copper-Sheet-merger edge that doesn't exist in-game. If true, fix the traversal. *This is the hypothesis most consistent with "fix the importer".*
2. **Faithful read of a messy save.** The player really merged a Plastic belt into that merger in-game (Satisfactory mergers merge any belts). Then the "fix" is to **detect/flag** mixed-item mergers during import (emit a warning / split them) rather than silently mislabel.
3. **Item-flood relabel (app-side, not importer).** The merger is genuinely Plastic but got relabeled "Copper Sheet" by `RecalculateOrganizerItemChain` flooding from a downstream Copper-Sheet chain it's (wrongly) connected to. Still implies a bad edge from #1/#2.
4. **Rate rounding (separate issue).** The many float-derived fraction mismatches are a distinct rate-computation rounding problem in the importer; not the topology bug the user circled.

## Next steps (resume here)
1. **Reproduce.** Pick the newest `teste-1.2-beta` save and run the importer, capturing JSON:
   - Save dir: `%LOCALAPPDATA%\FactoryGame\Saved\SaveGames\<steam-id>`
   - Newest candidates seen: `teste-1.2-beta_030426-165208.sav`, `…_010526-163739.sav`, `…_010426-230916.sav` (pick newest by mtime; confirm with `ls -t`).
   - `node "ficsit-companion/tools/sav_import/wrapper.js" "<save.sav>" > /tmp/import.json` (mind the user's machine paths; user may prefer to run it themselves via `! node …`).
2. **Locate the offending merger in the RAW importer output** (`belts[]` + `buildings[]`): find merger buildings whose feeding belts' `src.building` are constructors with a **Plastic** recipe. Confirms whether the bad edge exists in importer output (vs. introduced app-side).
3. **Decide faithful vs. fabricated.** Add targeted diagnostics in `wrapper.js` around that merger: log the full belt-chain hop sequence (component paths) from each Plastic constructor output to the merger. Compare against the save's raw `mConnectedComponent` links to see if the hop is legitimate or a mis-pick. (This is the Phase-1 "instrument component boundaries" step.)
4. Only after the above: form ONE hypothesis, write a minimal repro/test, fix root cause, verify.

## Key facts / environment
- App build: `build/ficsit-companion/Release/ficsit-companion.exe`; current imported graph: `build/ficsit-companion/Release/last_session.fcs` (673 nodes, 791 links).
- `settings.json`: `sav_watch_world = teste-1.2-beta`, `sav_watch_dir = %LOCALAPPDATA%\FactoryGame\Saved\SaveGames\<steam-id>`, `last_sav_path = null`.
- Node v18.20.8. Repo also has a small `example_save.sav` at root (NOT the teste world).
- C++ import entry points: `ProductionApp::RunSavParserDesktop` / `ImportSavFromJson` (`src/app/production_app.cpp`); JSON→graph build also touches `infra/sav_import*`.
- Python is available for analyzing `.fcs`/JSON (used the `fractions` module for exact rate comparison).

## Analysis snippet that worked (for `.fcs`)
Load `last_session.fcs` JSON; `nodes`/`links`; link `start.node`/`end.node` are 0-based indices; compare `nodes[s].outs[s.pin]` rate vs `nodes[e].ins[e.pin]` rate (num/den → `fractions.Fraction`) to list mismatched (red) links. Craft/Extractor have no `outs`/`ins` arrays in the file (guard for missing keys).
