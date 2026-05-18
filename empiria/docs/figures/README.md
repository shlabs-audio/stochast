# Figures for the SHLabs paper

This directory should contain screenshots referenced by `paper.md`.
Capture each module inside VCV Rack at native resolution, then crop to
the panel plus a small surrounding margin. Each module should be shown in
mid-action so that its visualization is interesting (a uniformly empty
histogram or a flat scrolling chart does not communicate the design).

## Required figures

### Suite overviews

- **`panel-grammar.png`** — A single representative SHLabs module
  (Cascade recommended) showing the shared panel layout: header,
  visualization, knob row, secondary row, inputs, outputs. Used in the
  Software Description section.
- **`pipeline-example.png`** — A representative compound patch with
  cables clearly visible: `Discourse (poly) → Frame (SIG) → Test (SIG)`,
  with all three modules in mid-action. Used in the Example Pedagogy
  section.
- **`polis-suite.png`** — All six Polis modules side by side (Cascade,
  Discourse, Pareto, Dilemma, Diffusion, Network), each in
  mid-simulation.
- **`methods-suite.png`** — The eight new Methods modules side by side
  (Sample, Frame, Regress, Test, Boot, Lag, Code, Tab), each with
  active data and visible visualizations.
- **`epi-suite.png`** — Contagion module alone, captured mid-outbreak
  with the I curve cresting and R₀ visible.

### Per-module figures

Each of these is a single-module screenshot in mid-action, used in the
Module Gallery section of `paper.md`.

#### Polis

- **`cascade.png`** — Cascade with PRESSURE around 0.3, threshold bars
  visible, a few agents activated, the water-level lines drawn.
- **`discourse.png`** — Discourse mid-convergence, with three or four
  visible bundles of trajectory lines.
- **`pareto.png`** — Pareto after enough runtime that the bars are
  meaningfully unequal (Gini ≳ 0.3); ideally with the CONCENTRATION
  light on for drama.
- **`dilemma.png`** — Dilemma with all four strategy lines visible and
  diverging (mix slider around 0.5–0.7, run for ~10 s); footer legend
  visible.
- **`diffusion.png`** — Diffusion mid-S-curve (around 50 % adopted), so
  both the rising trajectory and the agent dot row are interesting.
- **`network.png`** — Network in BARA mode (most visually distinctive)
  with N ≈ 12, showing a hub-and-spoke degree distribution — pick a
  seed that produces a clearly heterogeneous graph.

#### Methods

- **`sample.png`** — Sample with DIST = Normal, window grown enough for
  the histogram to clearly match the PDF curve.
- **`frame.png`** — Frame with a healthy histogram and a visible CI band
  and bracket.
- **`regress.png`** — Regress with a strong linear relationship (R² ≳
  0.6) so the trumpet-shaped CI band is clearly visible.
- **`test.png`** — Test with the observed t falling inside the rejection
  region (REJECT footer visible) — sets up the pedagogical point.
- **`boot.png`** — Boot in SNAPSHOT mode with a clean bootstrap
  histogram, visible red estimate line, visible CI bracket.
- **`lag.png`** — Lag with a clearly autocorrelated signal (e.g.
  `Sample → Slew limiter → Lag`) so several lags poke outside the
  amber Bartlett band; the "autocorr" footer should be visible.
- **`code.png`** — Code with K = 5 receiving a Normal-distributed input
  (e.g. `Sample (Normal, μ ≈ 0.3, σ ≈ 0.4)`); histogram visibly
  peaked at middle category, with cutpoint labels readable below.
- **`tab.png`** — Tab with K1 = K2 = 5 showing a clearly diagonal
  association heatmap (set up by feeding the same Discourse output
  into both X and Y, or use two correlated sources); χ² and V values
  visible top-right.

#### Epi

- **`contagion.png`** — Contagion with R₀ ≈ 3 (β ≈ 0.3, γ ≈ 0.1) caught
  mid-outbreak so all three curves (S falling, I cresting, R rising)
  are clearly visible.

## Format notes

- PNG at ~150–200 DPI is sufficient for the journal; SVG is also
  accepted by JOSE if exported directly from the rack.
- Use VCV Rack's "Save screenshot" feature (View → Screenshot) for
  consistent native-resolution captures.
- Crop with a small (~20 px) margin around the panel so the screenshots
  read well at small sizes.
- Consistent zoom level across captures looks more professional in the
  composed paper.
