# Space changelog

## 2.0.0 — 2026-07-11

First public release for VCV Rack 2. Grid-based spatial models that emit CV, under
the SHLabs brand:

- **Life** — Conway's Game of Life on a 24×24 grid (B3/S23), with right-click
  presets for HighLife, Day & Night, majority and voter rules; outputs live count,
  generation, an oscillation gate and per-row activity.
- **Schelling** — segregation model on a 24×24 grid: agents relocate when fewer
  than θ of their neighbours match; outputs segregation index, unhappy fraction,
  average local same-type fraction and a segregated gate.
- **Turing** — Gray-Scott reaction-diffusion on a 48×48 grid producing spots,
  stripes, labyrinths or solitons; outputs mean u, mean v, pattern variance and an
  oscillation gate.

Release polish:

- Tempo-sync and readout corrections so clocked displays agree with the engine.
- Panel grammar and label alignment across the suite.
- Corrected the plugin metadata (author contact, homepage, source and donate URLs)
  to the SHLabs brand.

> Rack ABI note: the major version is `2` to match VCV Rack 2. There is no `1.x`
> line — this is the first Space release series.
