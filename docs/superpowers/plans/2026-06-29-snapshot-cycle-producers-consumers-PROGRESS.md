# Progress: Snapshot cycle producers/consumers

- [x] Task 1 — `NodesConsumingItem` helper (node_display.hpp/.cpp) + Catch2 test in test_node_display.cpp. Build + `[node_display]` tests pass (23 assertions, 6 cases).
- [x] Task 2 — UI wiring: `NavMode` state in factory_snapshot_app.hpp; per-cell Selectables (item name + Produced -> producers, Consumed -> consumers) in RenderResourceFlowTable; mode-aware cycling in RenderGraphCanvas. Full build clean; full suite passes (1222 assertions, 210 cases).
  - Manual click-through verification (Step 6): confirmed working by user in the running app.
