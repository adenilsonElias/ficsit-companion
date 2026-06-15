- Task 1 done: generator helper tests and pure mapping helpers added; JS tests pass.
- Task 2 done: wrapper classifies generator actors and emits resolved ones as Power recipe manufacturers; JS tests pass.
- Task 3 done: C++ BuildGraph test confirms resolved Power recipes import as CraftNodes; sav_import tests pass.
- Task 4 done: JS wrapper tests, focused importer checks, and full CTest suite pass.

## Paused 2026-06-14

Implementation is mostly complete, but final review found two remaining issues that must be fixed before calling the feature done:

1. Real generator fuel descriptor names may not match the exact `Power (...)` recipe map.
   - Example: production `itemDisplayName("Desc_Biofuel")` currently returns `Biofuel`, but the generator recipe map expects `Solid Biofuel`.
   - Add explicit production overrides in `ficsit-companion/tools/sav_import/wrapper.js` for known generator fuel descriptors, at minimum:
     - `Desc_Biofuel` -> `Solid Biofuel`
     - `Desc_LiquidFuel` -> `Fuel`
     - `Desc_LiquidTurboFuel` -> `Turbofuel`
     - `Desc_PackagedBiofuel` -> `Packaged Liquid Biofuel`
     - `Desc_NuclearFuelRod` -> `Uranium Fuel Rod`
   - Also add alias mappings in `ficsit-companion/tools/sav_import/wrapper_core.js` for descriptor-derived names:
     - `Biofuel` -> `Power (Solid Biofuel)`
     - `Liquid Fuel` -> `Power (Fuel)`
     - `Liquid Turbo Fuel` -> `Power (Turbofuel)`
     - `Packaged Biofuel` -> `Power (Packaged Liquid Biofuel)`
     - `Nuclear Fuel Rod` -> `Power (Uranium Fuel Rod)`

2. Uppercase descriptor object shape is still not handled by `readStringValue()`.
   - It handles `{ value: { pathName } }` and top-level `{ PathName }`.
   - It still needs to handle `{ Value: { PathName } }`.
   - Add a JS test for `resolveGeneratorFuelItem()` using `Properties: { mFuelClass: { Value: { PathName: "/Game/.../Desc_Coal.Desc_Coal_C" } } }` and assert `Coal`.

Notes:
- `firstInventoryItem()` has already been made more robust for uppercase/lowercase inventory component shapes.
- Do not use git; the user explicitly said no git.
- Re-run after the remaining fixes:
  - `npm test --prefix ficsit-companion/tools/sav_import`
  - `cmake --build build --config Release --target fc-tests`
  - `.\\build\\ficsit-companion\\Release\\fc-tests.exe "[sav_import]"`
  - `ctest --test-dir build -C Release --output-on-failure`

Last verified before final-review findings:
- JS wrapper tests: 8 pass, 0 fail.
- `fc-tests` build: passed.
- Catch2 `[sav_import]`: 109 assertions in 17 test cases passed.
- Full CTest: 111/111 passed.

## Resolved 2026-06-14 (final-review findings closed)

Both paused issues are now fixed and verified:

1. Generator fuel descriptor name mismatch fixed:
   - Added production `ITEM_DISPLAY_OVERRIDES` in `wrapper.js`: `Desc_Biofuel`->`Solid Biofuel`, `Desc_LiquidFuel`->`Fuel`, `Desc_LiquidTurboFuel`->`Turbofuel`, `Desc_PackagedBiofuel`->`Packaged Liquid Biofuel`, `Desc_NuclearFuelRod`->`Uranium Fuel Rod`.
   - Added descriptor-derived alias entries to `GENERATOR_RECIPE_BY_FUEL` in `wrapper_core.js`: `Biofuel`, `Liquid Fuel`, `Liquid Turbo Fuel`, `Packaged Biofuel`, `Nuclear Fuel Rod`.
2. Uppercase descriptor object shape `{ Value: { PathName } }` now handled in `readStringValue()`; the pre-existing failing JS test now passes.

Verified after fixes:
- JS wrapper tests: 10 pass, 0 fail.
- `fc-tests` build: passed.
- Catch2 `[sav_import]`: 109 assertions in 17 test cases passed.
- Full CTest: 111/111 passed.

Feature complete.
