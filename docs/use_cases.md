# Stochast — example patches

Each section below describes a complete teaching patch: the modules
required, the cables to draw, the parameter settings to try, the
behaviour to watch for, and the conceptual takeaway. Patches are
ordered from simplest to most compound.

---

## 1. Visible Law of Large Numbers

**Modules:** Sample, Frame
**Concept:** as `n` grows, the sample mean converges to the population
mean and its standard error shrinks as 1/√n.

**Cables**
- `Sample.SAMPLE` → `Frame.SIG`
- `Sample.TRIG`   → `Frame.CLOCK`

**Settings**
- Sample: DIST = **Normal**, P1 = 0.50 (μ), P2 = 0.30 (σ)
- Frame: MODE = **Growing**, CI = **95%**

**What to observe**
- Frame's running mean steadies toward Sample's μ.
- `SE` shrinks visibly as the buffer grows; the shaded CI band on the
  histogram narrows.
- The histogram itself tightens around μ but its *shape* stays roughly
  Normal — only the spread changes.

**Optional variations**
- Switch Sample DIST to **Exponential** — the underlying distribution
  is skewed, but the *mean* still settles smoothly. (Hint: it's the
  CLT, not just LLN.)
- Press Sample's SHUFFLE to re-seed and re-run from scratch.

---

## 2. Sampling distribution of an estimator (bootstrap)

**Modules:** Sample, Boot
**Concept:** the bootstrap distribution of an estimator IS the
sampling distribution students usually have to imagine.

**Cables**
- `Sample.SAMPLE` → `Boot.SIG`
- `Sample.TRIG`   → `Boot.CLOCK`

**Settings**
- Sample: DIST = **Normal**, P2 = 0.50
- Boot: MODE = **Snapshot**, N = 128, B = 500, STAT = **Mean**, CI = **95%**

**What to observe**
- Once Boot has gathered 128 samples, the bootstrap distribution
  appears as a cyan histogram. The red vertical line is the original
  sample mean; the amber band is the 95 % percentile CI.
- Toggle Boot's STAT knob to **Median**, **SD**, then **Variance**:
  same data, four different sampling distributions, four different
  CIs. The bootstrap *generalises* across statistics where parametric
  formulas would each need a separate derivation.

**Re-resampling**
- Patch a slow LFO (or just a clock at 1 Hz) into `Boot.RSAMP`. Each
  pulse triggers a fresh batch of B resamples on the **same data**.
  The histogram shimmers slightly — the bootstrap's own variability
  is visible.

---

## 3. Hypothesis test with visible power

**Modules:** Sample, Test
**Concept:** sample size, effect size, and α together determine the
power of the test to reject H₀.

**Cables**
- `Sample.SAMPLE` → `Test.SIG`
- `Sample.TRIG`   → `Test.CLOCK`

**Settings**
- Sample: DIST = **Normal**, P1 = 0.50 (μ), P2 = 0.50 (σ)
- Test: TEST = **One-sample**, H₀ = 0.0, α = **0.05**, MODE = **Running**, N = 16

**What to observe**
- With N = 16, the observed t-statistic dances around in the null
  distribution. The REJECT gate fires intermittently.
- Slowly turn Test's N knob up to 256. The REJECT gate lights up
  more often and more persistently; the observed t-statistic drifts
  further into the rejection region; Cohen's d on the EFFECT output
  stays roughly the same (effect size is sample-size-invariant).
- Now lower Sample's P1 toward 0 (smaller effect). At some point the
  test stops rejecting reliably — the effect is no longer detectable
  at the current n. This is **statistical power** in one knob.

---

## 4. OLS regression with a visible "trumpet"

**Modules:** Sample (×2 or one Sample plus an offset/sum), Regress
**Concept:** OLS estimates a line; the confidence band on the line is
narrowest where data is densest and widens at the extremes.

**Cables**
- `Sample (Normal).SAMPLE` → `Regress.X`
- `Sample.TRIG`            → `Regress.CLOCK`
- (Generate Y from X: pass X through a `+0.7×` operation and add a
  small noise source, or use a second Sample summed to X with a
  weighted mixer. Patch the resulting Y stream into `Regress.Y`.)

**Settings**
- Regress: MODE = **Running**, N = 256, CI = **95%**, SUB = 1

**What to observe**
- The scatter cloud fills out as data flows. The amber fitted line
  rotates and re-positions in real time; β and α converge toward the
  true generative values.
- The shaded amber band is the confidence interval on the prediction
  ŷ at each x. Note the **trumpet shape** — narrow near the mean of
  X, wider at the extremes. This visually conveys why extrapolation
  beyond observed data is risky.

**Diagnostic**
- Patch `Regress.RESID` → `Frame.SIG`. Frame's histogram of residuals
  should be roughly centred on zero with constant spread if the
  linear model is well-specified. A non-zero mean or asymmetric
  histogram is a model-fit problem the student can *see*.

---

## 5. Threshold cascade (Granovetter)

**Modules:** Cascade (alone, or with an LFO and a VCO)
**Concept:** a population of heterogeneous activation thresholds
exhibits sharp tipping behaviour as external pressure crosses a
critical value.

**Settings**
- Cascade: POP = 16, MEAN = 0.40, SPREAD = 0.35, INFLUENCE = 0.55,
  PRESSURE = 0.00

**What to do**
- Slowly turn the PRESSURE knob up. The "water level" in the
  visualization rises; agents whose threshold dot becomes submerged
  light up.
- At some pressure value the IGNITION gate fires and a cohort of
  agents activates en masse. That's the **tipping point**.
- Right-click → "Cascade mode" → **Hysteretic (Granovetter)**. Now
  lowering PRESSURE does *not* de-activate the ignited agents — the
  cascade is irreversible. This is the canonical Granovetter dynamic.

**Musical use**
- Patch `Cascade.GATES` (polyphonic) into a 16-voice oscillator bank.
  Cooperation-led activation becomes audible harmony.

---

## 6. Compound patch: macro phenomena → micro analysis

**Modules:** Discourse, Frame, Test
**Concept:** chain a generative social model into an inferential
pipeline. The output of the simulator becomes data; the methods
modules treat it as such.

**Cables**
- `Discourse.OPIN (poly)` → `Frame.SIG`
- `Frame.MEAN`            → `Test.SIG`
- A shared clock into all three CLOCK inputs (or use their internal
  clocks)

**Settings**
- Discourse: POP = 16, TALK = 0.20 (polarising regime), PULL = 0.30
- Frame: MODE = **Running**, N = 64
- Test: TEST = **One-sample**, H₀ = **0.50**, α = **0.05**, MODE = **Running**, N = 32

**What to observe**
- Discourse's 16 agents drift into two or three clusters of opinion;
  the population mean wanders away from 0.5.
- Frame measures that wandering mean, smoothing across a window.
- Test reports whether the population mean has *significantly*
  drifted from the neutral midpoint of 0.5.
- This is a fully closed loop: *generative social process* →
  *measurement* → *inferential decision*. Each step is a single
  inspectable module.

---

## 7. Inequality with visible policy

**Modules:** Pareto (alone) and / or → Cascade
**Concept:** redistribution policy alters steady-state inequality;
inequality, once entrenched, may itself trigger collective behaviour.

**Settings**
- Pareto: POP = 16, TRADE = 0.10, BIAS = 0.00, POOL = 0.00,
  RATE = 6

**What to observe**
- With POOL = 0, the value concentrates on a single agent; Gini
  climbs toward 1; the CONCENTRATION gate lights up.
- Now turn POOL up to ~0.5. Gini stabilises at a moderate value; the
  CONCENTRATION gate releases. Even small redistribution prevents
  total condensation.

**Cross-module composition**
- Patch `Pareto.CONC` (concentration gate) → `Cascade.PRESSURE_CV`.
  Inequality crosses the oligarchy threshold → pressure rises in
  Cascade → tipping point fires. The patch tells a small story.

---

## 8. Network as a topology-driven drum sequencer

**Modules:** Network, plus a clock source, plus a polyphonic envelope /
drum-trigger bank
**Concept:** the structure of a social-network graph becomes the
*rhythm* of a musical pattern. Different network types yield distinctly
different grooves.

**Cables**
- External clock → `Network.CLOCK` (e.g. 4–8 Hz for an eighth-note feel)
- `Network.GATES` (poly) → drum trigger / envelope generator with one
  voice per channel (kick / snare / hat / clap / ride / tom×… )

**Settings**
- Network: POP = 8 (matches an 8-voice drum bank), K = 2, β = 0.10,
  TYPE = RING (start here)

**What to observe**
- TYPE = **RING** with β = 0 — pure regular lattice — produces a smooth
  walk that mostly steps to neighbouring nodes; the gate pattern feels
  sequential and predictable, like a marching pattern.
- Slowly increase β. Around β ≈ 0.1 the walker starts taking
  small-world shortcuts; the rhythm gains "swing" — mostly local steps
  with the occasional unexpected jump.
- Set β = 1 (random graph) — the walker jumps freely; the rhythm is
  scattered, syncopated.
- Switch TYPE to **BARA** (preferential attachment): hubs accumulate
  most edges, so the walker dwells on them. The hub voices fire
  repeatedly; leaves rarely. Heavy-low-end, sparse-high-end feel.
- Press SHUFFLE for a new random graph with the same parameters — the
  pattern reshuffles deterministically from the new seed.

**Beyond drums**
- Patch `Network.GATES` into 8 sine oscillators tuned to a chord; the
  walker plays an arpeggio whose shape encodes the graph.
- Use `Network.⟨k⟩` → low-pass filter cutoff: denser graphs make
  brighter tone.
- Right-click Network → "Quantize JUMP to scale" → **Major
  pentatonic**. Patch a single oscillator with V/Oct from
  `Network.JUMP`, gate from any of the `Network.GATES` channels (or
  sum them via a logic-OR module). The oscillator plays a melodic
  line whose interval-shape mirrors the random walk: short steps
  yield neighbouring scale degrees, long shortcuts yield octave-leap
  surprises. With BARA topology, long jumps cluster around hub
  revisits → repeated motif.

---

## 9. Autocorrelation diagnostics

**Modules:** Sample, Lag
**Concept:** classical inference assumes independent observations.
Autocorrelation in time-series data breaks that assumption — Lag makes
it visible.

**Cables**
- `Sample.SAMPLE` → `Lag.SIG`
- `Sample.TRIG`   → `Lag.CLOCK`

**Settings**
- Sample: DIST = **Normal**, P1 = 0.0, P2 = 0.3
- Lag: MODE = **Running**, N = 256, LAGS = 8, DETREND = **Mean**

**What to observe**
- With i.i.d. samples from Sample, the bars in the ACF stay within the
  dashed amber Bartlett bands — the **white** footer indicator is on.
- Now insert a 1-pole low-pass filter (e.g. VCV Fundamental's Slew) in
  the signal path. The same Sample stream now has temporal correlation:
  ρ(1) jumps to ~0.7, ρ(2) ~0.5, ρ(3) ~0.35, decaying — bars pierce the
  band, the **autocorr** footer turns amber.
- Toggle DETREND between Off / Mean / Linear with a trending input
  (e.g. add a slow LFO to the signal): "Off" shows spurious high
  positive autocorrelation at all lags; "Linear" detrending recovers
  the underlying white-noise diagnostic.

---

## 10. Epidemic outbreak with public-health intervention

**Modules:** Contagion, plus an LFO source
**Concept:** β controls the takeoff threshold; modulating it
mid-outbreak simulates a lockdown / unlock cycle.

**Cables**
- Patch an LFO (e.g. VCV Fundamental's LFO at ~0.05 Hz, square wave) →
  `Contagion.β·CV`. Attenuate so it adds ±0.15 to β.
- Optionally `Contagion.PEAK` → drum trigger as a sonic punctuation of
  outbreak peaks.

**Settings**
- Contagion: POP = 16, β = 0.30, γ = 0.10 (R₀ = 3.0, super-critical)
- Press SEED to start an outbreak

**What to observe**
- With the LFO unpatched: classical S/I/R trajectory — S falls, I
  rises and peaks, R climbs to ~all-recovered, the **PEAK** gate
  fires when the I curve crests.
- With the LFO on β: β oscillates between (0.30 − 0.15) = 0.15 and
  (0.30 + 0.15) = 0.45. The R₀ readout in the header flips colour
  between green (R₀ < 1) and orange (R₀ ≥ 1). I rises during
  "unlock" half-periods and stalls during "lockdown" half-periods —
  the **flatten-the-curve** dynamic visualized.

---

## 11. Survey-response simulation: continuous opinion → Likert categories

**Modules:** Sample, Code, Frame (or Test)
**Concept:** real-world surveys collect categorical Likert responses to
underlying continuous attitudes. Code is the encoder; downstream we can
study how categorization affects inference.

**Cables**
- `Sample.SAMPLE` → `Code.SIG`
- `Sample.TRIG`   → `Code.CLOCK`
- `Code.MEAN`     → `Frame.SIG`  (or `Test.SIG`)
- `Sample.TRIG`   → `Frame.CLOCK`

**Settings**
- Sample: DIST = **Normal**, P1 = 0.55 (μ; slightly above neutral),
  P2 = 0.3 (σ)
- Code: K = 5 (5-point Likert), LOW = -1, HIGH = 1
- Frame: MODE = **Running**, N = 256

**What to observe**
- The underlying continuous attitude has μ = 0.55 (slightly positive).
- Code's histogram clusters around the middle category (3) and the
  upper-mid (4) — what surveyors would call "agree" / "neutral".
- Frame reports the *mean of the categorical responses* (~3.5–4.0).
- Compare this against a parallel pipeline (Sample → Frame directly):
  the continuous mean is ~0.55, the categorical mean recovers the
  *ordinal* position but loses absolute scale — exactly what happens
  in real surveys.
- Increase Sample's σ. As variance grows, Code's histogram spreads
  across categories. The **entropy ENT** output rises toward 10 V
  (uniform distribution = max entropy).

---

## 12. Cross-tabulation: are two opinions associated?

**Modules:** Discourse (×2), Code (×2), Tab
**Concept:** two opinion dimensions can be statistically independent or
associated. Tab's χ² and Cramér's V make this visible.

**Cables**
- `Discourse #1.OPIN` → `Code #1.SIG` → `Tab.X`
- `Discourse #2.OPIN` → `Code #2.SIG` → `Tab.Y`
- Shared clock into all four modules' CLOCK inputs

**Settings**
- Both Discourse modules: POP = 16, TALK = 0.20, PULL = 0.30
- Both Code modules: K = 5, LOW = 0, HIGH = 1
- Tab: K1 = 5, K2 = 5, LOW = 0.5, HIGH = 5.5 (matches Code's 1..K
  output range)

**What to observe**
- **Independent setup**: with the two Discourse modules running on
  separate seeds, their cluster patterns are uncorrelated. Tab's
  heatmap appears roughly uniform; V hovers near 0, the **indep**
  indicator (green) stays on.
- **Associated setup**: patch the same Discourse's OPIN into both X
  and Y (so identical opinions). Tab's heatmap shows a strong
  diagonal — perfect association; V → 1, **assoc** indicator (amber)
  on.
- **Intermediate setup**: cross-couple the two Discourses by patching
  Discourse #1's MEAN into Discourse #2's T·CV. The TALK threshold of
  #2 now varies with #1's mean opinion — they become statistically
  associated through TALK. V settles somewhere in the middle.

---

## 13. Reproducible simulation across machines

**Modules:** Seed plus any random simulator (e.g. Cascade, Sample,
Contagion)
**Concept:** make the random realisation explicit and shareable.

**Cables**
- `Seed.TRIG` → `Cascade.RESET` (and any other random module's RESET)

**What to do**
- Adjust Seed's VALUE knob (0..999). Each change emits a TRIG pulse;
  Cascade resets and restarts its dynamics from its constructor seed
  — effectively producing the "next" stochastic realisation in
  lockstep with the rest of the patch.
- Press **RANDOMIZE** for a uniform random seed value.
- Save the patch (Cmd/Ctrl+S in VCV Rack). Send the resulting `.vcv`
  file to a collaborator. They open it and see exactly the same
  simulation — the Seed value is stored in the patch JSON.

**Conceptual takeaway**
- Cross-machine reproducibility is straightforward when every random
  process is initialised from a deterministic seed, and when that seed
  is shareable.
- The "world seed" abstraction — one big number that fixes the entire
  random realisation — is a real concept in computational social
  science, and now visible on the panel.

---

## 14. Frozen dataset, many analyses (Tape)

**Modules:** Sample, Tape, Frame, Regress, Test, Boot
**Concept:** capture one synthetic dataset and run multiple analyses
on identical data — the cornerstone of computational reproducibility.

**Cables**
- `Sample.SAMPLE` → `Tape.SIG`
- A shared clock into both modules' CLOCK inputs
- `Tape.SIG` → fan-out to `Frame.SIG`, `Test.SIG`, `Boot.SIG`

**Workflow**
1. Configure Sample: DIST = Normal, P1 = 0.5, P2 = 0.3.
2. Set Tape: MODE = REC, LENGTH = 256.
3. Let it record for ~5 seconds (256 ticks at the shared clock rate).
4. Flip Tape's MODE knob to **LOOP**. The same 256-sample dataset now
   repeats forever.
5. Frame, Test, and Boot all receive the same frozen data; you can
   tweak Test's H₀ or Boot's STAT without the data underneath
   changing — only the analytical lens changes.

**Why this matters**
- Standard practice in computational reproducibility: freeze the data,
  vary the analysis.
- Without Tape, every parameter twist on Sample produces a new draw,
  conflating "analysis varies" with "data varies".
- With Tape, the student can answer "is this estimate sensitive to
  the analytical choice?" cleanly.

**Variation — different analyses, same data, side-by-side**
- Use one Tape module as the canonical "experimental dataset".
- Attach Frame for the descriptive summary, Test for the hypothesis
  test, Boot for the non-parametric CI, Lag for the autocorrelation
  diagnostic.
- Switch Tape between **PLAY** (one pass) and **LOOP** (continuous)
  depending on whether the analysis needs a static cross-section or a
  running window.

---

## Tips for teaching with Stochast

* **Slow the clocks.** All modules have internal clocks at 30 Hz so
  they run unattended, but a shared external clock at 2–4 Hz lets
  students follow the dynamics step by step.
* **Right-click everything.** Many modules have context-menu options
  (Cascade's hysteretic mode, Sample's distribution selection,
  Boot's CI level, Tape's CSV export) that are essential to the
  pedagogy.
* **Mute the audio.** Stochast modules are not audio sources — they
  produce CV. Patch outputs into an oscillator + VCA only when a
  musical interpretation is desired.
