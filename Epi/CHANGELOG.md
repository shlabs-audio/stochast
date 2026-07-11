# Epi changelog

## 2.0.0 — 2026-07-11

First public release for VCV Rack 2. An epidemic-dynamics module that emits CV,
under the SHLabs brand:

- **Outbreak** — network SIR: infection spreads through an internally generated
  graph (Watts-Strogatz, Erdős–Rényi or Barabási–Albert) at rate β per infected
  neighbour with recovery rate γ. Topology reshapes the outbreak — small-world
  shortcuts spread fast, preferential-attachment hubs become superspreaders. Shows
  the network coloured by S / I / R state plus an S/I/R trajectory.

Release polish:

- Tempo-sync and readout corrections so clocked displays agree with the engine.
- Panel grammar and label alignment.
- Corrected the plugin metadata (author contact, homepage, source and donate URLs)
  to the SHLabs brand.

> Rack ABI note: the major version is `2` to match VCV Rack 2. There is no `1.x`
> line — this is the first Epi release series.
