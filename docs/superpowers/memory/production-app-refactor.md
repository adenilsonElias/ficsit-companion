---
name: production-app-refactor
description: In-progress refactor splitting production_app.cpp into DI modules + Catch2 tests; how to resume
metadata:
  type: project
---

Splitting the ~5082-line `ficsit-companion/src/production_app.cpp` into
dependency-injected, unit-tested modules (Catch2). Subagent-driven execution.

**Full resume state lives in** `docs/superpowers/plans/2026-06-08-production-app-decomposition-PROGRESS.md`
(read it first). Plan: `docs/superpowers/plans/2026-06-08-production-app-decomposition.md`.
Spec: `docs/superpowers/specs/2026-06-08-production-app-decomposition-design.md`.

Status as of 2026-06-08: **T1–T5 DONE** (fc-core lib + Catch2 fc-tests, IFileStore,
SettingsStore, IEditorBackend, RateSolver). **T6 (GraphModel) is NEXT, not started.**
T7 SessionSerializer, T8 sav world-name, T9 verify remain. 11 tests pass; app builds.

Critical constraints (per user):
- Work on `main`, **NO git commits** — user manages git. Skip all "Commit" steps.
- Build/configure from repo ROOT: `cmake -S . -B build` (NOT `-S ficsit-companion`).
- Working tree has uncommitted WIP (Extractor/Logistics nodes etc.) → verbatim
  moves baseline against the **working tree, not HEAD** (HEAD diffs = false positives).
- Tests: fabricate `Item` with empty icon path (real path → glGenTextures crash).
- `RateSolver::Solve` signature carries `float& error_time, float error_flow_duration`
  (deviates from plan). See PROGRESS doc for the GraphModel/T6 step-by-step.
