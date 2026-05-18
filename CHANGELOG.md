# Empiria changelog

## 2.0.0 — 2026-05-16

First public release of the Empiria suite for VCV Rack 2. Bundles
five plugins under the SHLabs brand:

- **Polis** (6 modules): Cascade, Diffusion, Dilemma, Discourse,
  Network, Pareto — social and economic dynamics.
- **Methods** (14 modules): Boot, Code, Cohort, Factor, Frame,
  Gauge, Lag, Regress, Sample, Seed, Strata, Tab, Tape, Test —
  the statistical analysis layer.
- **Epi** (1 module): Outbreak — compartmental SIR / SEIR.
- **Space** (3 modules): Life, Schelling, Turing — spatial CA.
- **Decisions** (3 modules): Bandit, DDM, Prospect — behavioural
  economics and decision-making.

Highlights:

- Right-click *What does this do?* menu on every module showing a
  one-paragraph description and typical companion modules.
- Larger numeric readouts on Frame, Boot, Regress and Test so the
  key statistic is legible at lecture-projector distance.
- Family-coloured panel stripe so each plugin's modules are
  visually distinct on a busy rack.
- Three starter `.vcv` patches under `patches/`, plus a
  `tools/empiria_patch_gen.py` script for per-student seeded copies.
- CSV export from Tape's right-click menu, for bridging into
  R / Python / Julia.
- Adjacency-on-expander protocol: Network publishes a network
  topology that Diffusion and Outbreak consume directly.

Documentation:
- Full user manual under `docs/modules.md`.
- Companion paper (`paper.md`, ~8,500 words) targeted at the
  *Journal of Statistics and Data Science Education*.
