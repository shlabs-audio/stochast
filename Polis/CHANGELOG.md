# Polis changelog

## 2.0.0 — 2026-07-11

First public release for VCV Rack 2. A suite of agent-based social-dynamics models
that emit CV, under the SHLabs brand:

- **Cascade** — Granovetter threshold model: heterogeneous agents tip past their
  activation points under a Pressure signal; outputs active fraction, delta, an
  ignition gate and per-agent gates.
- **Discourse** — Deffuant-Weisbuch bounded-confidence convergence; paired agents
  interact within a confidence threshold ε and pull together, fragmenting into
  stable clusters; outputs polyphonic values, mean, variance and cluster count.
- **Pareto** — stochastic pairwise exchange (Chakraborti / Boghosian); value
  condenses even under fair flips, tunable via BIAS and POOL; outputs value shares,
  Gini, top share and a concentration gate.
- **Dilemma** — iterated Prisoner's Dilemma round-robin over four strategies
  (ALL_C / ALL_D / TFT / GRIM); outputs cooperation gates, mean cooperation,
  average score and a sucker gate.
- **Diffusion** — Bass innovation-adoption dynamics producing the canonical
  S-curve; outputs per-agent adoption gates, fraction adopted, adoption rate and a
  saturation gate.
- **Network** — relational-structure generator (Watts-Strogatz, Erdős–Rényi or
  Barabási–Albert); outputs per-agent degree, mean degree, clustering coefficient
  and a connected-graph gate over a circular layout.

Release polish:

- Tempo-sync and readout corrections so clocked displays agree with the engine.
- Panel grammar and label alignment across the suite.
- Corrected the plugin metadata (author contact, homepage, source and donate URLs)
  to the SHLabs brand.

> Rack ABI note: the major version is `2` to match VCV Rack 2. There is no `1.x`
> line — this is the first Polis release series.
