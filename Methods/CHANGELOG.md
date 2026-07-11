# Methods changelog

## 2.0.0 — 2026-07-11

First public release for VCV Rack 2. A statistics-teaching suite that turns
data-generating processes, estimators and tests into patchable modules under the
SHLabs brand:

- **Strata** — real-time STL-style decomposition: splits a CV/audio stream into
  Trend, Seasonal and Residual components.
- **Cohort** — self-organising quantiser using online k-means; learns K cluster
  centres and snaps to the nearest, with optional V/oct semitone snap.
- **Factor** — real-time PCA on six CV inputs (Sanger's rule); outputs the top
  three latent components as a modulation mixer.
- **Sample** — draws clocked random samples from a selectable distribution
  (Normal / Uniform / Exponential / Beta); outputs sample, running mean and SD,
  and a sync trigger, with the theoretical PDF overlaid on the histogram.
- **Frame** — sampling frame with SNAPSHOT / RUNNING / GROWING modes; reports
  sample mean, SD and standard error, and shades a confidence band.
- **Regress** — online OLS on clocked (X, Y) pairs; outputs slope, intercept, R²
  and residual with a live scatterplot and trumpet-shaped confidence band.
- **Test** — one- and two-sample (Welch) t-tests; outputs t, p, a reject gate and
  Cohen's d, with the null distribution and rejection regions drawn.
- **Boot** — bootstrap resampling of a buffer; shows the empirical sampling
  distribution and outputs the estimate, bootstrap SE and percentile-CI bounds.
- **Lag** — autocorrelation function ρ(k) with AR(1) coefficient, detrending and
  Bartlett significance bands; a WHITE gate flags white noise.
- **Code** — continuous-to-categorical encoder into K ordinal categories; outputs
  per-category CV/gates, running mean category and Shannon entropy.
- **Tab** — contingency-table cross-tabulation of two categorical streams; outputs
  χ², Cramér's V and an independence gate over a live heatmap.
- **Seed** — reproducibility primitive: a clock-incrementable seed saved in the
  patch, output as CV plus a change trigger for locking other modules.
- **Tape** — record-and-replay CV buffer (REC / PLAY / LOOP) for feeding one
  dataset into many parallel analyses unchanged.
- **Gauge** — maps CV to a real-world quantity (percent, z, IQ, Likert, …) via a
  linear preset menu, passing the original voltage through unchanged.
- **Quantity** — the input-side mirror of Gauge: dial a real-world value and emit
  the back-converted CV.

Release polish:

- Tempo-sync and readout corrections so clocked displays agree with the engine.
- Panel grammar and label alignment across the suite.
- Corrected the plugin metadata (author contact, homepage, source and donate URLs)
  to the SHLabs brand.

> Rack ABI note: the major version is `2` to match VCV Rack 2. There is no `1.x`
> line — this is the first Methods release series.
