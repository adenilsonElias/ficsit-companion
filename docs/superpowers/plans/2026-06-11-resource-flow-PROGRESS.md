# Resource Flow — Execution Progress

**Plan:** `docs/superpowers/plans/2026-06-11-resource-flow.md`
**Spec:** `docs/superpowers/specs/2026-06-11-resource-flow-design.md`
**Execution mode:** subagent-driven-development, **NO GIT** (no branches/commits; files edited in place on `main`).

This file is updated after each task so work can resume if the session ends.
To resume: find the first task below not marked ✅ and continue from there.

| Task | Title | Status |
| --- | --- | --- |
| 1 | Domain module + craft accumulation | ✅ done |
| 2 | Extractor production | ✅ done |
| 3 | Sink consumption + null-item skip | ✅ done |
| 4 | Group recursion | ✅ done |
| 5 | Robustness (organizers/logistics, fractional) | ✅ done |
| 6 | Row filter predicate | ✅ done |
| 7 | Persist show_resource_flow setting | ✅ done |
| 8 | Resource Flow window UI | ✅ done |
| 9 | Final verification | ✅ done |

## Notes / decisions
- Per-task `git commit` steps in the plan are **skipped** (user chose "no git").
- Code quality review uses the working-tree diff instead of commit SHAs.

## Log
- **Task 1 ✅** — Created `include/domain/resource_flow.hpp`, `src/domain/resource_flow.cpp` (craft branch only), `tests/test_resource_flow.cpp` (2 tests), wired CMake. Spec review passed. Code-quality review fixed: removed domain→app `app/utils.hpp` include (now a local `ByItemName` comparator), added balanced-chain test, minor cosmetics. Result: `[resource_flow]` = 29 assertions / 2 cases pass.
- **Tasks 2–5 ✅** — Added the extractor/sink/group `Walk` branches and 6 tests (extractor offset, sink consume, null-item skip, group recursion, organizers/logistics excluded, fractional exactness). TDD red→green confirmed. Fixed a stale `ItemPtrCompare`→`ByItemName` comment. Result: `[resource_flow]` = 8 cases / 51 assertions pass; full suite 286/64, no regressions.
- **Tasks 6–7 ✅** — Implemented real `RowPassesFilter` (status gate + case-insensitive `ContainsCI` name search) + test; added persisted `Settings::show_resource_flow` with `SettingsStore` load/save + round-trip test. Result: `[resource_flow]` = 9 cases / 60 assertions; `[settings]` green; full suite 65 cases / 296 assertions, all pass.
- **Task 8 ✅** — Added `RenderResourceFlowWindow()` (floating window: filter buttons, search, 5-col table with icons + colored net/status, footer counts, skipped-null note), a left-panel "Show/Hide Resource Flow" toggle button, the `resource_flow_filter`/`resource_flow_search` state, and the render-loop call. App target `ficsit-companion` builds cleanly; `fc-tests` still 65/296. No GUI smoke test run (no display in this environment).
- **Task 9 ✅** — Full Release build of all targets clean; `ctest` = **100% (65/65) passed**. Feature complete.

## ⚠️ Remaining manual step (not runnable here)
Launch the app and click **"Show Resource Flow"** to visually confirm the window/table/filters/footer render as intended (no display available in this environment to do it automatically).
