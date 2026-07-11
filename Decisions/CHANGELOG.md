# Decisions changelog

## 2.0.0 — 2026-07-11

First public release for VCV Rack 2. Decision- and choice-model modules that emit
CV, under the SHLabs brand:

- **Prospect** — prospect theory (Kahneman & Tversky): transforms objective outcome
  and probability CV into subjective value U(x)·w(p) with loss aversion, diminishing
  sensitivity and probability weighting, drawing both curves.
- **Bandit** — multi-armed bandit (up to 8 arms) with ε-greedy, UCB1 or Thompson
  policies; visualizes per-arm estimate, pull count and cumulative regret.
- **DDM** — Ratcliff drift-diffusion choice model: a noisy accumulator races to ±a
  boundaries; outputs the evidence trace, a choice gate, reaction-time CV and running
  mean accuracy.

Release polish:

- Tempo-sync and readout corrections so clocked displays agree with the engine.
- Panel grammar and label alignment across the suite.
- Corrected the plugin metadata (author contact, homepage, source and donate URLs)
  to the SHLabs brand.

> Rack ABI note: the major version is `2` to match VCV Rack 2. There is no `1.x`
> line — this is the first Decisions release series.
