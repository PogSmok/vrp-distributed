# 0030. Live map: workers route each new best plan on the roads

- **Status:** Accepted
- **Date:** 2026-11-23
- **Related:** 0012, 0014, 0018, 0022, 0024, 0025, 0028

## Context

The client shows how the search improves its routes. During SOLVE a plan is
only a stop order: road geometry came from FINALIZE alone (0025). Straight
lines between stops are not routes, and only clutter the map.

## Decision

- **Routing.** During SOLVE, one low-priority thread on each worker routes
  that worker's latest best plan with FINALIZE's code. The search never waits
  for it, plans that arrive meanwhile replace the waiting one, and a failure
  here never affects the task.
- **Contract.** A worker sends a `RoadPlan`, thinned to points a few metres
  apart. The orchestrator keeps each job's cheapest in memory, beside the
  migrant pool and outside the Scheduler, and a poll returns it if it beats
  the plan the client holds.
- **Map.** Only these plans are drawn. The best is bright; the plans it
  replaced stay visible, dimmer the older they are.
- **Rendering.** A Streamlit component draws the map with deck.gl and
  MapLibre, from exact versions on a CDN with integrity hashes. Basemaps come
  from OpenFreeMap, dark or light with the viewer's theme.

## Alternatives considered

- **Straight lines between stops:** cheap, but they show nothing real.
- **Routing in the client:** it would need the graph and a router of its own.
- **Streaming every candidate the GPU evaluates:** far more data than a viewer
  can see, on the control plane.
- **`st.pydeck_chart`:** no way to animate between Streamlit reruns.

## Consequences

- Gain: the map shows real routes improving as the search runs.
- Cost: some CPU during SOLVE; the map can trail the search on large problems;
  the map needs internet access for its libraries and basemap.
- Revisit: reuse unchanged legs between plans, if routing a plan gets slow.

## Confirmation

A real job shows road routes during SOLVE, with older plans dimmed, in both
themes. Solver steps per second are compared with routing on and off.
