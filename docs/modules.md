# Empiria — User Manual

A complete reference for **Empiria**, an open-source suite of VCV
Rack 2 modules for teaching statistics, data science, computational
social science, behavioural economics, and spatial-emergence
modelling. Empiria is written and maintained by **Kevin Schoenholzer**
(<https://kevinschoenholzer.com>), a postdoctoral researcher in the
social sciences at Università della Svizzera italiana (USI), and
distributed under the GPL-3.0 licence through his personal
open-source imprint *SHLabs*.

| Plugin | Domain | Modules |
|---|---|---|
| **Polis** | Macro-level social phenomena | Cascade, Discourse, Pareto, Dilemma, Diffusion, Network |
| **Methods** | Data-science workflow | Sample, Frame, Regress, Test, Boot, Lag, Code, Tab, Strata, Cohort, Factor, Seed, Tape, Gauge |
| **Epi** | Network epidemiology | Outbreak |
| **Space** | Spatial dynamics & emergence | Life, Schelling, Turing |
| **Decisions** | Behavioural economics & cognition | Prospect, Bandit, DDM |

---

## What is VCV Rack?

[VCV Rack](https://vcvrack.com/) is a free, open-source software emulator
of a Eurorack modular synthesizer. Modules are placed on a virtual rack
and connected with virtual patch cables; signals flowing through the
cables are *control voltages* (CVs) at audio sample rate. It runs on
macOS, Windows, and Linux, and is distributed as both a free download
(VCV Rack Free) and a paid Pro edition (VCV Rack Pro — not required
for Empiria).

Although VCV Rack was built for electronic music, its signal-flow
paradigm is also a remarkably general substrate for *quantitative*
modelling: every CV is just a floating-point number streaming through
a wire, and a "module" is just a transformation of those numbers. This
is the same intuition behind the analog computers of the mid-twentieth
century — voltages-as-state-variables, wires-as-operators — and it is
what Empiria exploits to make sampling distributions, regression,
hypothesis testing, and agent-based models all manipulable through a
single, uniform interface.

### Installing VCV Rack

1. **Download VCV Rack Free** from
   [https://vcvrack.com/Rack](https://vcvrack.com/Rack). Pick the
   build for your platform (macOS Intel / macOS Apple Silicon /
   Windows / Linux). Pro is *not* required.
2. Run the installer and launch VCV Rack. On first launch you'll see
   an empty rack containing only the master audio I/O.
3. Empiria does *not* require any audio interface. The modules
   compute and visualise at sample rate; audio output is optional.

### Installing Empiria

Empiria ships as five `.vcvplugin` files (one per plugin):
`SHLabs-Polis-*.vcvplugin`, `SHLabs-Methods-*.vcvplugin`,
`SHLabs-Epi-*.vcvplugin`, `SHLabs-Space-*.vcvplugin`,
`SHLabs-Decisions-*.vcvplugin`. To install:

1. Locate your VCV Rack user folder:
   - macOS: `~/Library/Application Support/Rack2/plugins-mac-arm64/`
     (Apple Silicon) or `…/plugins-mac-x64/` (Intel)
   - Windows: `%LOCALAPPDATA%\Rack2\plugins-win-x64\`
   - Linux: `~/.local/share/Rack2/plugins-lin-x64/`
2. Drop the five `.vcvplugin` files into that folder.
3. Restart VCV Rack. The modules will appear in the module browser
   under the "SHLabs" brand (right-click on any empty rack space →
   filter by *Brand → SHLabs*).

### A 60-second tour of the VCV Rack interface

- **Module browser**: right-click any empty space in the rack, or
  press `Enter`. Browse by tag, brand, or search by name. Drag a
  module onto the rack to place it.
- **Patching**: click on an output port (the brass-coloured jacks at
  the bottom of most modules) and drag to an input port. Cables in
  Empiria carry both single-channel and multi-channel ("polyphonic",
  up to 16 channels) signals.
- **Knob control**: click-and-drag vertically to adjust. Hold `Cmd`/`Ctrl`
  for fine adjustment. Right-click a knob to type an exact value or
  reset to default.
- **Right-click any module** for module-specific options: distribution
  presets on Sample, action-policy selection on Bandit, CA rule
  presets on Life, scale-quantisation on Network, etc.
- **Right-click any port** for polyphony channel-count overrides and
  the "show signal" oscilloscope quick-view.
- **Saving a patch**: `File → Save As` writes a `.vcv` file (a small
  JSON document). Empiria patches are typically a few KB; they are
  fully self-describing and reproducible across machines.

### Convention shared across all Empiria modules

- **Panel grammar**: every Empiria module is 20 HP wide (300 px),
  with a title strip at the top, a live visualisation area below it,
  one or two rows of knobs / switches in the middle, an input jack
  row, and an output jack row at the bottom.
- **Polyphonic outputs** carry per-agent or per-observation state at
  up to 16 voices. Monophonic outputs carry population-level
  summaries (mean, SD, count, fraction, …).
- **CLOCK** inputs drive sampling and simulation ticks. *All* modules
  free-run at a built-in rate (10–240 Hz depending on the module) if
  CLOCK is unpatched, so a module dropped into an empty rack starts
  doing something immediately. Patching a clock overrides the
  internal rate and synchronises the module to the rest of the patch.
- **RESET** inputs clear all per-module state and re-randomise where
  applicable. The on-panel SHUFFLE / CLEAR / RESET / SEED button is
  the manual equivalent.
- **All voltages are clamped** to ±12 V at output (VCV Rack's
  physical-Eurorack-emulating limit). Most outputs natively live in
  0..10 V or ±10 V; statistic outputs are scaled to fit (e.g.
  Cramér's V at 0..1 is emitted as 0..10 V).
- **Reproducibility**: every random module derives its sequence from
  a per-module seed saved in the patch JSON. Two collaborators
  loading the same patch see the same simulation. The standalone
  `Seed` module (in Methods) lets you set the seed value explicitly
  and broadcast change-triggers across the whole patch.

### Helpful links

| | |
|---|---|
| **VCV Rack home** | <https://vcvrack.com/> |
| **VCV Rack manual** | <https://vcvrack.com/manual/> |
| **VCV community forum** | <https://community.vcvrack.com/> |
| **VCV Library (module index)** | <https://library.vcvrack.com/> |
| **Empiria source code** | (project repository URL) |
| **Empiria companion paper** | `paper.pdf` in the project root |
| **VCV Rack developer SDK** | <https://vcvrack.com/manual/PluginDevelopmentTutorial> |
| **Empiria design spec (panel grammar)** | [`design_spec.md`](design_spec.md) |
| **Empiria example patches** | [`use_cases.md`](use_cases.md) |

---

## Documenting and exporting results

For students writing up assignments, lab reports, or methods
chapters — or for instructors preparing course materials — Empiria
supports several routes for capturing what happens on the rack:

### Screenshots

VCV Rack has a built-in screenshot facility:

- **View → Screenshot** (or `Cmd/Ctrl-Shift-S`) captures the full rack
  to a PNG file. Resolution matches the current zoom level — zoom in
  before capturing if you want a high-resolution figure.
- Right-clicking inside any individual module also exposes a
  *Screenshot* option that captures just that module's panel.

Screenshots are the natural way to document a patch in a write-up:
each figure can show both the patch wiring and the on-panel
visualisation at a moment of interest.

### Saving the patch

A patch is just a JSON file (typically 5–50 KB). Use **File → Save
As** to write a `.vcv` file. The file is human-readable, fully
self-describing, and reproducible across machines — a collaborator
who opens it sees exactly the same modules in the same positions
with the same parameter values and the same seeded RNG state.
Shared `.vcv` files are the recommended way to distribute reference
patches to students and to attach to assignments.

### CSV export of synthetic data (Tape)

Empiria's **Tape** module records any polyphonic CV stream into an
in-memory buffer. Right-click on Tape and choose
**"Export buffer to CSV…"** to write the recorded samples to a CSV
file at a location of your choosing. The file format is:

```
sample,ch1,ch2,...,chN
0, <ch1 value>, <ch2 value>, ...
1, <ch1 value>, <ch2 value>, ...
...
```

— immediately loadable in R (`read.csv("tape.csv")`) or Python
(`pandas.read_csv("tape.csv")`). The typical workflow is:

1. Build a data-generating pipeline (e.g. `Sample → Code` for
   categorical responses, or `Discourse → Frame` for polyphonic
   agent state).
2. Patch its output into Tape's `SIG` input.
3. Set Tape's MODE to `REC` and let it fill its buffer (LENGTH
   knob sets the buffer size, 4..4096 samples).
4. Stop recording, right-click Tape, choose *Export buffer to CSV*.
5. Open the CSV in R / Python / Excel / your favourite tool, and
   carry the analysis further with the platform's analytical
   layer or any other ecosystem.

Because Empiria is fully deterministic given a Seed value, the
exported CSV is reproducible: another student running the same
patch with the same Seed will export a byte-identical CSV.

### Recording the audio output

VCV Rack itself ships with a *Recorder* module (Brand: VCV,
Category: Recorder) that captures the audio bus to a WAV file. If
you have wired an Empiria module's CV into an audio module
(e.g. a VCO whose pitch is being driven by Cohort's quantised
output), the Recorder captures that audio for later playback or
inclusion in a video.

### Composability with R, Python, Julia

Empiria is positioned as a *companion* to standard statistical
tooling, not a replacement. The CSV export from Tape lets students
take a synthetic dataset they generated *inside* Empiria and
analyse it *outside* in any environment of their choosing — useful
both for assignments that require a specific software package and
for graduate work that mixes Empiria's visualisation with an
established analytic stack.

---

## Starter patches

Three ready-made patches ship under `patches/` in the repository.
Each one wires the modules required for one of the worked
examples in the companion paper, with a fixed seed so the same
patch produces identical figures on every machine:

| File                                  | Workflow                                          |
|---------------------------------------|---------------------------------------------------|
| `empiria_schelling_segregation.vcv`   | Schelling segregation (visceral demo)             |
| `empiria_seeded_t_test.vcv`           | Two-sample Welch's t-test (seeded, reproducible)  |
| `empiria_coin_flip_lln.vcv`           | Coin-flip Law-of-Large-Numbers (closed-form check)|

To open one, in VCV Rack choose **File → Open** and pick the
`.vcv` file from the `patches/` directory.

### Generating per-student copies

For a class handout where every student should see slightly
different data — but each student's run must still be
reproducible — use the bundled generator to stamp a fresh seed
into the patch:

```bash
python3 tools/empiria_patch_gen.py --seed 137 \
                                   --out /tmp/handouts_student_137/
```

The generator writes the same three patches, with the Seed
module preset to the supplied integer (0..999). Pair it with a
shell loop to mint one patch per student:

```bash
for s in 101 102 103 104 105; do
  python3 tools/empiria_patch_gen.py --seed "$s" \
                                     --out "handouts/student_$s/"
done
```

The generator is also a small Python library — import
`build_patch` / `write_patch` from `tools/empiria_patch_gen.py`
to scaffold custom patches for your own course.

---

## Module reference

The remainder of this manual catalogues every module's parameters,
inputs, outputs, lights, and right-click context-menu options.

## Polis

### Cascade

Granovetter threshold model of collective behaviour with adjustable peer
influence.

| Param        | Range          | Default | Effect |
|--------------|----------------|---------|--------|
| POP          | 1..16          | 16      | Population size |
| MEAN         | 0..1           | 0.40    | Mean of the threshold distribution |
| SPREAD       | 0..1 → σ 0..0.4| 0.35    | Threshold dispersion |
| PRESSURE     | 0..1           | 0.12    | External activation pressure |
| INFLUENCE α  | 0..1.5         | 0.55    | Peer-feedback weight |
| SHUFFLE      | button         | —       | Redraw thresholds + reset activations |

| Input       | Type   | Notes |
|-------------|--------|-------|
| CLOCK       | trig   | 30 Hz internal |
| RESET       | trig   | Clear activations |
| P·CV        | CV     | 0..10 V adds to PRESSURE |

| Output  | Range  | Notes |
|---------|--------|-------|
| ACTIVE  | 0..10 V| Fraction of agents active |
| DELTA   | ±5 V   | Δ active per tick (rate-of-change) |
| IGNITE  | gate   | Pulse when ≥ N/8 agents flip in one tick |
| GATES   | poly   | Per-agent activation (10 V while active) |

Context-menu: **Reversible (default)** or **Hysteretic (Granovetter)**.

### Discourse

Deffuant–Weisbuch bounded-confidence value-convergence model.

| Param   | Range    | Default | Effect |
|---------|----------|---------|--------|
| POP     | 2..16    | 16      | Population size |
| TALK ε  | 0.01..0.5| 0.22    | Confidence threshold |
| PULL μ  | 0..0.5   | 0.30    | Convergence rate |
| RATE    | 1..32    | 6       | Pair interactions per tick |
| SHUFFLE | button   | —       | Re-randomize values |

| Input    | Type  | Notes |
|----------|-------|-------|
| CLOCK    | trig  | 30 Hz internal |
| RESET    | trig  | Re-randomize values |
| T·CV     | CV    | ±5 V → ±0.5 added to TALK |

| Output   | Range   | Notes |
|----------|---------|-------|
| VAL      | poly    | Per-agent value (0..10 V scaled) |
| MEAN     | 0..10 V | Population mean |
| VAR      | 0..10 V | Variance (scaled by 120) |
| CLUST    | 0..10 V | Cluster count / N |

### Pareto

Yard-Sale stochastic exchange model.

| Param   | Range     | Default | Effect |
|---------|-----------|---------|--------|
| POP     | 2..16     | 16      | Population size |
| TRADE β | 0.01..0.5 | 0.10    | Fraction of smaller value at stake |
| BIAS    | 0..0.5    | 0.0     | Higher-side advantage in coin flip |
| POOL τ  | 0..1      | 0.0     | Mean-field redistribution per tick |
| RATE    | 1..32     | 6       | Exchanges per tick |
| SHUFFLE | button    | —       | Randomize starting values |

| Input    | Type  | Notes |
|----------|-------|-------|
| CLOCK    | trig  | 30 Hz internal |
| RESET    | trig  | Reset all values to 1 (equality) |
| P·CV     | CV    | 0..10 V adds to POOL |

| Output  | Range   | Notes |
|---------|---------|-------|
| VAL     | poly    | Value share per agent |
| GINI    | 0..10 V | Gini coefficient |
| TOP     | 0..10 V | Top agent's value share |
| CONC    | gate    | High while top share > 50 % |

### Dilemma

Iterated Prisoner's Dilemma round-robin tournament across four
strategies (ALL_C, ALL_D, TFT, GRIM).

| Param   | Range  | Default | Effect |
|---------|--------|---------|--------|
| POP     | 2..16  | 12      | Population size |
| MIX     | 0..1   | 0.55    | Cooperative-bias of strategy distribution |
| PAYOFF  | 0..1   | 1.00    | T temptation payoff (3..5) |
| NOISE   | 0..1   | 0.00    | Action error rate (0..20 %) |
| RATE    | 1..8   | 1       | Rounds per tick (trimpot) |
| SHUFFLE | button | —       | Redraw strategies + reset scores |

| Input    | Type  | Notes |
|----------|-------|-------|
| CLOCK    | trig  | 15 Hz internal |
| RESET    | trig  | Clear scores, keep strategies |
| N·CV     | CV    | 0..10 V adds to NOISE |

| Output  | Range   | Notes |
|---------|---------|-------|
| COOP    | poly    | Gate per agent if cooperated in majority of pairings this round |
| MEAN    | 0..10 V | Mean cooperation rate this round |
| SCORE   | 0..10 V | Mean score per agent per round (scaled by 5) |
| SUCKER  | gate    | Pulse when a (C, D) outcome occurs |

### Diffusion

Bass innovation-adoption dynamics: λ = p + q · f per non-adopter.
Adoption is irreversible.

| Param   | Range    | Default | Effect |
|---------|----------|---------|--------|
| POP     | 1..16    | 16      | Population size |
| P       | 0..0.05  | 0.015   | Innovation rate (spontaneous adoption) |
| Q       | 0..0.5   | 0.25    | Imitation rate (peer-driven) |
| SPEED   | 1..8     | 1       | Substeps per clock tick |
| RESET   | button   | —       | Clear all adoptions |

| Input    | Type  | Notes |
|----------|-------|-------|
| CLOCK    | trig  | 20 Hz internal |
| RESET    | trig  | Clear adoptions |
| Q·CV     | CV    | 0..10 V adds 0..0.5 to Q |

| Output    | Range   | Notes |
|-----------|---------|-------|
| GATES     | poly    | 10 V while agent is adopted |
| ADOPT     | 0..10 V | Fraction adopted |
| Δ         | 0..10 V | Smoothed adoption rate |
| DONE      | gate    | Pulse on crossing 95 % adopted |

### Network

Relational structure generator + topology-driven sequencer. One of three
canonical graph models, plus a random walker that traverses the graph and
emits per-agent gate pulses on each clock tick.

| Param   | Range     | Default | Effect |
|---------|-----------|---------|--------|
| POP     | 2..16     | 12      | Population size |
| K       | 1..7      | 3       | Nearest neighbours per side (RING); seed connections (BARA) |
| β       | 0..1      | 0.10    | Rewire probability (RING); edge probability (ERDOS) |
| TYPE    | snap 0..2 | RING    | RING / ERDOS / BARA |
| SHUFFLE | button    | —       | Re-seed RNG (new random realization) |

| Input    | Type  | Notes |
|----------|-------|-------|
| CLOCK    | trig  | Drives the walker. Free-runs at 4 Hz if unpatched |
| RESET    | trig  | Reset seed + send walker back to node 0 |
| β·CV     | CV    | 0..10 V adds to β |

| Output    | Range   | Notes |
|-----------|---------|-------|
| GATES     | poly    | Walker gate per agent (20 ms pulse on visit) |
| ⟨k⟩       | V       | Mean degree (raw) |
| CC        | 0..10 V | Global clustering coefficient |
| JUMP      | 0..10 V or V/Oct | Index distance of the last hop (circular). With scale-quantize *Off*, scaled to 0..10 V; with a scale selected (right-click), output is V/Oct on that scale |

The walker performs a uniform random walk: from the current node, it
picks a neighbour uniformly at random as the next destination. From an
isolated node (no neighbours), it teleports to a random node. The graph
topology shapes the rhythmic feel — sequential on regular ring lattices,
chaotic on Erdős–Rényi graphs, hub-anchored on Barabási–Albert.

**Right-click options:** "Quantize JUMP to scale" lets the JUMP output
double as a V/Oct pitch signal. The scale options are:

| Scale | Semitone offsets per octave |
|---|---|
| Off (linear 0..10 V) | — (raw distance scaled by N/2) |
| Major | 0, 2, 4, 5, 7, 9, 11 |
| Natural minor | 0, 2, 3, 5, 7, 8, 10 |
| Major pentatonic | 0, 2, 4, 7, 9 |
| Chromatic | 0..11 |

Jump distance 1 → root (0 V); distances beyond the scale's size wrap to
the next octave. Connectivity status (single-component / disconnected)
remains visible in the visualization header (no longer a dedicated gate
output).

---

## Methods

### Sample

DGP from a selectable parametric distribution.

| Param   | Range         | Default | Effect |
|---------|---------------|---------|--------|
| DIST    | snap 0..3     | Normal  | Normal / Uniform / Exponential / Beta |
| P1      | 0..1 normalized| 0.50   | First parameter (μ / centre / λ / α) |
| P2      | 0..1 normalized| 0.30   | Second parameter (σ / width / — / β) |
| WIN     | 4..1024       | 128     | Window size for running stats |
| SHUFFLE | button        | —       | Re-seed RNG + clear window |

| Input    | Type  | Notes |
|----------|-------|-------|
| CLOCK    | trig  | 30 Hz internal |
| RESET    | trig  | Clear window + re-seed |
| P1·CV    | CV    | ±5 V adds to P1 |
| P2·CV    | CV    | ±5 V adds to P2 |

| Output  | Range   | Notes |
|---------|---------|-------|
| SAMPLE  | ±12 V   | Most recent draw |
| MEAN    | ±12 V   | Empirical mean over WIN |
| SD      | 0..12 V | Empirical SD over WIN |
| TRIG    | gate    | 2 ms pulse per new sample |

### Frame

Sampling-frame measurement window.

| Param  | Range      | Default | Effect |
|--------|------------|---------|--------|
| MODE   | snap 0..2  | Running | Snapshot / Running / Growing |
| N      | 4..4096    | 128     | Buffer size |
| CI     | snap 0..3  | 95 %    | Confidence level (80/90/95/99) |
| SUB    | 1..16      | 1       | Take every Kth clock tick |
| CLEAR  | button     | —       | Clear buffer |

| Input    | Type  | Notes |
|----------|-------|-------|
| CLOCK    | trig  | 30 Hz internal |
| RESET    | trig  | Clear buffer |
| SIG      | CV    | Signal to measure (polyphonic OK) |
| TRIG     | trig  | Manual snapshot (SNAP mode) |

| Output  | Range   | Notes |
|---------|---------|-------|
| MEAN    | ±12 V   | Sample mean |
| SD      | 0..12 V | Unbiased sample SD |
| SE      | 0..12 V | Standard error of mean |
| READY   | gate    | SNAP: high after completion. RUN/GROW: pulses per sample |

### Regress

Online OLS regression of Y on X.

| Param  | Range      | Default | Effect |
|--------|------------|---------|--------|
| MODE   | snap 0..2  | Running | Buffer behaviour |
| N      | 4..2048    | 128     | Buffer size |
| CI     | snap 0..3  | 95 %    | Confidence level |
| SUB    | 1..16      | 1       | Sub-sample factor |
| CLEAR  | button     | —       | Clear buffer |

| Input    | Type  | Notes |
|----------|-------|-------|
| CLOCK    | trig  | 30 Hz internal |
| RESET    | trig  | Clear buffer |
| X        | CV    | Predictor (polyphonic OK) |
| Y        | CV    | Response (polyphonic OK) |

| Output | Range   | Notes |
|--------|---------|-------|
| β      | ±12 V   | OLS slope |
| α      | ±12 V   | OLS intercept |
| R²     | 0..10 V | Coefficient of determination |
| RESID  | ±12 V   | Y − (α + β·X) on first channel |

### Test

Two-tailed t-test (one-sample or Welch's two-sample).

| Param   | Range      | Default     | Effect |
|---------|------------|-------------|--------|
| MODE    | snap 0..2  | Running     | Buffer behaviour |
| N       | 4..2048    | 128         | Buffer size per group |
| α       | snap 0..2  | 0.05        | Significance level |
| H₀      | -5..+5     | 0.0         | μ₀ (one-sample) or δ₀ (two-sample) |
| TEST    | snap 0..1  | One-sample  | One- or two-sample (trimpot) |
| CLEAR   | button     | —           | Clear buffers |

| Input    | Type  | Notes |
|----------|-------|-------|
| CLOCK    | trig  | 30 Hz internal |
| RESET    | trig  | Clear buffers |
| SIG      | CV    | Group 1 (polyphonic OK) |
| SIG₂     | CV    | Group 2 (two-sample only) |

| Output | Range   | Notes |
|--------|---------|-------|
| t      | ±12 V   | t-statistic |
| p      | 0..10 V | p-value (Gaussian approximation) |
| REJ    | gate    | High when |t| > critical |
| d      | ±12 V   | Cohen's d effect size |

### Boot

Bootstrap resampling of a statistic.

| Param   | Range      | Default     | Effect |
|---------|------------|-------------|--------|
| MODE    | snap 0..2  | Snapshot    | Buffer behaviour |
| N       | 4..2048    | 128         | Sample size |
| B       | 50..2000   | 500         | Bootstrap iterations |
| STAT    | snap 0..3  | Mean        | Mean / Median / SD / Variance |
| CI      | snap 0..3  | 95 %        | CI level (trimpot) |
| CLEAR   | button     | —           | Clear buffer + re-seed |

| Input     | Type  | Notes |
|-----------|-------|-------|
| CLOCK     | trig  | 30 Hz internal |
| RESET     | trig  | Clear buffer |
| SIG       | CV    | Signal to sample (polyphonic OK) |
| RSAMP     | trig  | Force a fresh bootstrap on the same data |

| Output  | Range   | Notes |
|---------|---------|-------|
| EST     | ±12 V   | Point estimate (statistic on original sample) |
| SE      | 0..12 V | Bootstrap standard error |
| CI_LO   | ±12 V   | Lower percentile bound |
| CI_HI   | ±12 V   | Upper percentile bound |

### Lag

Time-series autocorrelation analysis. Estimates ρ(k) at lags 1..K and the
AR(1) coefficient φ.

| Param   | Range     | Default | Effect |
|---------|-----------|---------|--------|
| MODE    | snap 0..2 | Running | Buffer behaviour |
| N       | 8..2048   | 128     | Buffer size |
| LAGS    | 1..16     | 8       | Number of lags shown in ACF |
| DETREND | snap 0..2 | Mean    | Off / Mean / Linear detrending |
| CLEAR   | button    | —       | Clear buffer |

| Input    | Type  | Notes |
|----------|-------|-------|
| CLOCK    | trig  | 30 Hz internal |
| RESET    | trig  | Clear buffer |
| SIG      | CV    | Signal (polyphonic OK) |

| Output  | Range   | Notes |
|---------|---------|-------|
| φ       | ±10 V   | AR(1) coefficient = ρ(1) |
| ACF     | poly    | ρ(1)..ρ(K) as polyphonic channels |
| σ²ε     | 0..12 V | AR(1) residual variance |
| WHITE   | gate    | High when all displayed lags fall within ±1.96/√n |

### Code

Continuous-to-categorical encoder. Maps a CV stream into K ordinal categories
with uniform cutpoints over [LOW, HIGH].

| Param   | Range      | Default | Effect |
|---------|------------|---------|--------|
| K       | 2..7       | 5       | Number of categories |
| LOW     | -12..12 V  | -5      | Lower bound for category 1 |
| HIGH    | -12..12 V  | +5      | Upper bound for category K |
| N       | 8..4096    | 128     | Window size for stats |
| CLEAR   | button     | —       | Clear histogram + stats |

| Input    | Type  | Notes |
|----------|-------|-------|
| CLOCK    | trig  | 60 Hz internal |
| RESET    | trig  | Clear histogram |
| SIG      | CV    | Signal to encode (polyphonic OK) |

| Output  | Range    | Notes |
|---------|----------|-------|
| CAT     | poly 1..K V | Per-channel category (1 V = cat 1, K V = cat K) |
| GATES   | poly K   | Per-category presence gates |
| MEAN    | 1..K V   | Running mean category over the window |
| ENT     | 0..10 V  | Shannon entropy of empirical distribution; 10 V = log₂(K) bits |

### Tab

Cross-tabulation of two categorical CV streams.

| Param   | Range      | Default | Effect |
|---------|------------|---------|--------|
| K1      | 2..7       | 5       | Rows / X categories |
| K2      | 2..7       | 5       | Columns / Y categories |
| LOW     | -12..12 V  | 0.5     | Lower bound (auto-detects 1V-per-category inputs) |
| HIGH    | -12..12 V  | 7.5     | Upper bound |
| CLEAR   | button     | —       | Clear table |

| Input    | Type  | Notes |
|----------|-------|-------|
| CLOCK    | trig  | 60 Hz internal |
| RESET    | trig  | Clear table |
| X        | CV    | Categorical CV (polyphonic OK; typically Code's CAT output) |
| Y        | CV    | Categorical CV (polyphonic OK) |

| Output  | Range   | Notes |
|---------|---------|-------|
| χ²      | 0..12 V | Pearson chi-squared statistic |
| V       | 0..10 V | Cramér's V effect size (0..1 × 10) |
| INDEP   | gate    | High when V < 0.1 |
| N       | 0..12 V | Cumulative N (scaled by 100) |

### Seed

Reproducibility primitive — a single integer seed value displayed
prominently with knob, RANDOMIZE button, and CLOCK-driven increment.
Outputs a CV and a change-trigger so a patch can be re-initialised in
lockstep.

| Param     | Range   | Default | Effect |
|-----------|---------|---------|--------|
| VALUE     | 0..999  | 1       | Seed value (snap, integer) |
| RANDOMIZE | button  | —       | Pick a uniform random seed |

| Input    | Type  | Notes |
|----------|-------|-------|
| CLOCK    | trig  | Increment seed on each rising edge |
| RESET    | trig  | Reset seed to 0 |

| Output   | Range   | Notes |
|----------|---------|-------|
| SEED·CV  | 0..10 V | Voltage proportional to seed value |
| TRIG     | gate    | 2 ms pulse on seed change — patch into RESET of other modules |

The current seed value is saved in the patch JSON, so a patch shared
between machines reproduces identically when reloaded.

### Tape

Record-and-replay buffer for CV streams. Capture once, replay many
times — the cornerstone of reproducible computational experiments.

| Param   | Range     | Default | Effect |
|---------|-----------|---------|--------|
| MODE    | snap 0..2 | REC     | REC (record into ring buffer) / PLAY (one-shot) / LOOP |
| LENGTH  | 4..4096   | 256     | Buffer size in samples |
| SPEED   | 0.25..4   | 1.0     | Playback rate multiplier |
| CLEAR   | button    | —       | Clear buffer + reset cursors |

| Input    | Type  | Notes |
|----------|-------|-------|
| CLOCK    | trig  | Drives REC writes and PLAY/LOOP reads |
| RESET    | trig  | Clear buffer + reset cursors |
| SIG      | CV    | Signal to record (polyphonic OK, up to 16 channels) |
| TRIG     | trig  | Restart current operation (rewind play, or reset rec cursor) |

| Output   | Range   | Notes |
|----------|---------|-------|
| SIG      | poly    | Replayed signal; pass-through during REC |
| POS      | 0..10 V | Playback cursor position (proportional to buffer length) |
| WRAP     | gate    | 10 ms pulse when LOOP wraps |
| ACT      | gate    | High while recording or playing |

In REC mode, the buffer acts as a continuously-updated ring of the
most recent N samples. Switching MODE from REC to PLAY snapshots the
buffer and replays it from the start. LOOP behaves like PLAY but
wraps at the end, emitting a WRAP pulse on each wrap-around.
Polyphonic channel count is preserved across REC → PLAY transitions.

**Right-click → Export buffer to CSV…** opens a file dialog and
writes the currently-recorded buffer to a CSV file (`sample, ch1,
ch2, …, chN` columns, one row per recorded sample, chronological
order). The menu item shows the current buffer size and channel
count, and is disabled when the buffer is empty. The exported CSV
is immediately loadable in R, Python, Excel, etc. — see the
*Documenting and exporting results* section near the top of this
manual for the full export workflow.

### Gauge

Translates a control voltage into a real-world quantity using a
user-selectable linear mapping `x = A·V + B`. Built primarily to
remove the interpretive barrier between *raw volts* (which mean
nothing to a non-synth-user) and *substantive units* (percent,
probability, IQ, height, °C, etc.) that the learner can think with.

| Param    | Range       | Default | Effect |
|----------|-------------|---------|--------|
| A        | −200..200   | 1.0     | Slope multiplier — drives the math at all times. Selecting a preset stamps a value here; tweak by hand to refine. |
| B        | −2000..2000 | 0.0     | Offset — drives the math at all times. Selecting a preset stamps a value here; tweak by hand to refine. |
| DECIM    | 0..4        | 2       | Display decimal places |
| RANGE    | 0..1        | 1.0     | Display bar range hint (0 = narrow, 1 = wide) |

| Input | Notes |
|-------|-------|
| IN    | CV to convert (polyphonic; the on-panel readout shows channel 1, but VAL and THRU outputs preserve all channels) |

| Output | Notes |
|--------|-------|
| VAL    | Converted value as CV (clamped to ±12 V) |
| THRU   | Pass-through of the input, unchanged — chain Gauge between any two modules without disturbing their CV |

The A and B knobs always drive the mapping. A preset is just a
shortcut that stamps a known (A, B) pair into the knobs — selecting
it from the right-click menu overwrites the knob values, but
afterwards the user can dial them by hand to tune the mapping. If
the knobs no longer match the preset's defaults, the preset name
on the panel turns amber and reads `(modified)`. Right-click
**→ Snap knobs to current preset** to revert.

**Right-click → Unit preset** lists the built-in mappings:

| Preset | Mapping (A, B) | Typical input range | Reads as |
|---|---|---|---|
| Voltage (passthrough) | (1, 0)     | any V            | raw V |
| Percent — unipolar   | (10, 0)    | 0..10 V          | 0..100 % |
| Percent — bipolar    | (10, 50)   | −5..+5 V         | 0..100 %    *(use this for LFOs / audio-rate inputs)* |
| Percent — signed     | (10, 0)    | −10..+10 V       | ±100 %       *(preserves sign)* |
| Probability          | (0.1, 0)   | 0..10 V          | 0..1 |
| Z-score              | (1, 0)     | ±3 V             | z-score |
| IQ (μ = 100, σ = 15) | (15, 100)  | ±3 V             | IQ score |
| Adult height (cm)    | (10, 170)  | ±4 V             | 130..210 cm |
| Likert 5-point       | (0.4, 3)   | ±5 V             | 1..5 |
| Likert 7-point       | (0.6, 4)   | ±5 V             | 1..7 |
| Test score (SAT)     | (100, 500) | ±3 V             | 200..800 |
| Temperature (°C)     | (5, 20)    | ±10 V            | −30..70 °C |
| Temperature (°F)     | (9, 68)    | ±10 V            | −22..158 °F |
| Reaction time (ms)   | (500, 0)   | 0..6 V           | 0..3000 ms |
| Count                | (10, 0)    | 0..10 V          | 0..100    |
| Custom               | (1, 0)     | —                | identity reset — start tuning the knobs from here |

The three **Percent** variants handle the three common voltage
conventions a learner might run into:

- **Unipolar** assumes a 0..10 V source — typical of gates,
  envelopes, and Empiria observables (e.g. Frame's MEAN of a
  normalised quantity).
- **Bipolar** assumes a centred −5..+5 V source (audio, LFOs,
  most modulators) and maps it onto 0..100 %, so a sine LFO reads
  as a percent sweep, not a ±50 % swing.
- **Signed** preserves the sign — use this if you want
  to see a CV that genuinely swings positive and negative.

The big numeric readout in the centre of the panel shows the
current value at the chosen precision; the unit label sits beside
it; the recent-range bar below shows where the value sits within
the preset's typical span. A small footer line shows the raw V
value for users who want to see both representations
simultaneously, plus the active `A·V + B` formula with the current
knob values so the mapping is never hidden.

Typical placement: anywhere between a Sample / Polis simulator and
a downstream observer, where the instructor or learner wants the
displayed value to be interpretable directly.

#### Pairing Gauge with the rest of Empiria

Gauge is built around the voltage conventions the other Empiria
modules already follow. To use it productively you do not need to
"calibrate" anything — but you do need to know which preset matches
which upstream output. The table below documents every
Empiria-to-Gauge pairing that comes up in normal teaching use.

**Fraction / probability / proportion outputs (0..10 V → 0..1).**
The dominant Empiria convention: any output that semantically
represents a fraction in [0, 1] is emitted as a CV in [0, 10] V.
Gauge's **Probability** preset (x = V / 10) renders it as a
decimal; the **Percent — unipolar** preset (x = 10·V) renders it
as 0..100 %. (Use **Percent — bipolar** instead when the upstream
source is a bipolar LFO or audio-rate signal, and **Percent —
signed** when you want negative values preserved.)

| Upstream output | Semantic | Recommended Gauge preset |
|---|---|---|
| Cascade.ACTIVE | Fraction of agents currently active | Percent or Probability |
| Discourse.CLUST | Cluster count / N | Probability |
| Discourse.MEAN | Mean opinion value (already in 0..10 V scale) | Probability |
| Pareto.GINI | Gini coefficient ∈ [0, 1] | Probability |
| Pareto.TOP | Top agent's share ∈ [0, 1] | Percent or Probability |
| Diffusion.ADOPT | Fraction adopted | Percent |
| Outbreak.I | Fraction infected | Percent |
| Outbreak.R | Fraction recovered | Percent |
| Schelling.SEG | Segregation index | Probability |
| Schelling.UNH | Unhappy fraction | Percent |
| Life.ALIVE | Live-cell fraction | Percent |
| DDM.ACC | Running classification accuracy | Percent |
| Test.p | p-value × 10 | Probability |
| Tab.V | Cramér's V × 10 | Probability |

**Standardised-score outputs (z-scored CV).** Sample with DIST =
Normal, P1 (μ) = 0, P2 (σ) = 1 emits an iid stream of standard
normal draws, with values typically in the ±3 V range. Gauge's
substantive presets re-express each z-score in the relevant
real-world unit. This is the canonical way to demonstrate
familiar statistical scales without having to rescale Sample's
output by hand.

| Sample setting | Gauge preset | Interpretation |
|---|---|---|
| Normal μ=0 σ=1 | IQ (μ=100, σ=15) | One draw = one person's IQ score |
| Normal μ=0 σ=1 | Adult height, cm (μ=170, σ=10) | One draw = one adult's height in cm |
| Normal μ=0 σ=1 | Test score (μ=500, σ=100) | One draw = one student's SAT-style subscore |
| Normal μ=0 σ=1 | Z-score | One draw = its own z-score (identity) |
| Normal μ=0 σ=1 | Custom (set A and B) | Any other Normal(μ=B, σ=A) interpretation |

Pedagogically, this is the high-value pattern: a *single*
underlying Sample(Normal 0, 1) drives the rack, while the learner
sweeps Gauge's preset to see the same stream of z-scores
*reinterpreted* across substantive scales. The graphical
operation — same data, different units — is exactly the move that
introductory statistics asks students to internalise.

**Test statistics, reaction times, and other "as-is" outputs.**
Some Empiria outputs are already in semantically meaningful units
on the voltage axis itself — they are not normalised. For these,
the Gauge preset is either Voltage (passthrough, just to make the
value displayable in large numerals) or a dedicated unit preset.

| Upstream output | Semantic | Recommended Gauge preset |
|---|---|---|
| Test.t | t-statistic | Z-score (the units coincide near df → ∞) or Voltage |
| Tab.χ² | χ² statistic | Voltage |
| Regress.β | Regression slope | Voltage (or Custom with A, B if the X-Y units differ) |
| Regress.R² | Coefficient of determination, ∈ [0, 1] | Probability |
| Frame.MEAN | Running mean | Voltage, or whatever Sample's distribution is in |
| DDM.RT | Reaction time (V × 0.5 = seconds) | RT, ms (the preset converts V → ms) |
| Bandit.REGR | Cumulative regret, scaled by 0.05 | Custom (A = 20, B = 0) |

**A pragmatic rule.** If you do not know what units a CV is in,
drop a Gauge between the upstream module and your visualiser, set
Gauge to the **Voltage** preset, and observe the raw V values for
a minute. Once you know the voltage range, pick the matching
preset (or use Custom with the right A and B).

#### Worked example: z-score reinterpretation

The pattern that best showcases Gauge's role is:

```
Seed → Sample(Normal μ=0 σ=1) → Gauge → Frame
                                        ↳ exports running mean / SD
```

Set Seed = 42, Sample to Normal (μ = 0, σ = 1). Patch
`Sample.SAMPLE → Gauge.IN → Frame.SIG`.

Now sweep Gauge's right-click preset through the substantive
options. Frame's running mean tracks the voltage mean (≈ 0), but
Gauge's on-panel numeric readout reinterprets each individual draw:

* **Z-score**: each draw reads ≈ 0 (very rarely > 2 or < −2)
* **IQ**: each draw reads ≈ 100 (rarely > 130 or < 70)
* **Height (cm)**: each draw reads ≈ 170 cm (rarely > 190 or < 150)
* **SAT score**: each draw reads ≈ 500 (rarely > 700 or < 300)

The lesson: a *single* z-scored data-generating process underlies
all of these "substantively different" pictures. What changes is
only the unit of measurement, not the statistical structure.
Crucially, Frame's running mean and SE are computed on the
voltages (z-scores), so the inferential apparatus is invariant
under the unit choice — exactly the conceptual point about scale
invariance that introductory statistics aims to convey.

### Sample — real-world presets

Right-click any Sample module → "Real-world distribution presets" → one
of: adult height, IQ scores, reaction times (Exponential), survey
responses (Uniform on [-1, 1]), U-shaped opinion (Beta α=β=0.2),
right-skewed income (Beta α=1, β=4), bell-shaped narrow Normal,
near-Bernoulli coin flip (Beta α=β=0.05). Each preset sets DIST, P1,
and P2 to canonical values for that shape.

---

## Epi

### Outbreak

Network SIR — infection spreads along edges of a generated graph
rather than through well-mixed mass action. Each agent only infects its
network neighbours.

| Param   | Range     | Default | Effect |
|---------|-----------|---------|--------|
| POP     | 2..16     | 16      | Population N (graph nodes) |
| β       | 0..1      | 0.30    | Per-edge transmission probability per tick |
| γ       | 0..1      | 0.10    | Recovery rate |
| TYPE    | 0..2      | 0       | 0 = Watts-Strogatz, 1 = Erdős–Rényi, 2 = Barabási–Albert |
| K       | trimpot   | 4       | Mean degree (WS, ER) or edges-per-new-node (BA) |
| β·NET   | trimpot   | 0.10    | Rewiring probability (WS) / edge probability (ER) — additional topology knob |
| SEED    | button    | —       | Regenerate graph and infect one random node |

| Input    | Type  | Notes |
|----------|-------|-------|
| CLOCK    | trig  | 20 Hz internal |
| RESET    | trig  | All agents to S, regenerate graph |
| β·CV     | CV    | ±10 V adds ±1 to β |

| Output  | Range   | Notes |
|---------|---------|-------|
| STATE   | poly    | Per-agent: S = 0 V, I = 5 V, R = 10 V |
| I       | 0..10 V | Fraction infected |
| R       | 0..10 V | Fraction recovered |
| PEAK    | gate    | Pulse when infected fraction crests |

The visualization shows the graph itself: nodes are coloured by S / I / R
state, and edges that just carried a transmission event flash. A mini
S/I/R trajectory plots along the bottom of the panel for the macro
view. Watts-Strogatz at low rewiring shows slow neighbour-to-neighbour
spread; Barabási–Albert shows rapid takeoff once a hub becomes infected
— the classic "structure matters" lesson of network epidemiology.

---

## Adjacency-on-expander

`Network` (in Polis) publishes its current adjacency matrix and walker state
to its right-hand neighbour via VCV Rack's expander mechanism. The receiving
module — currently `Diffusion` (Polis) — automatically switches from
well-mixed mass-action to graph-aware transmission whenever a Network sits
to its left. The mode is indicated on the receiver's panel:
**MASS** (grey) when no upstream Network is detected, **NET·N** (green) when
adjacency is being received, with N the active node count.

The receiving module also forwards the adjacency to its own right neighbour,
so additional graph-aware modules can be chained on the same graph. The
same agent indices are preserved end-to-end, so per-agent observers (Frame,
Tape, Boot) see a consistent set of nodes regardless of which mid-chain
module they sample from. The protocol is open — any future Empiria module
can opt in by allocating a matching message buffer on its left expander.

Pedagogically this turns "mixing structure" into a single rack-placement
decision rather than a software setting: students see the same parameters
produce qualitatively different dynamics simply by moving a Network next
to a simulator.

---

## Space

### Life

Conway's Game of Life on a 24×24 toroidal (or fixed-boundary) grid.

| Param        | Range  | Default | Effect |
|--------------|--------|---------|--------|
| DENSITY      | 0..1   | 0.30    | Random-seed live-cell density |
| SPEED        | 1..32  | 1       | Substeps per clock tick |
| WRAP         | switch | On      | Boundary: dead vs. toroidal |
| SHUFFLE      | button | —       | Re-seed at current density |

| Input        | Notes |
|--------------|-------|
| CLOCK        | Internal 8 Hz if unpatched |
| RESET        | Re-seed |
| DENSITY·CV   | 0..10 V offsets the density knob |

| Output  | Range   | Notes |
|---------|---------|-------|
| ALIVE   | 0..10 V | Fraction live |
| Δ       | ±5 V    | Births − deaths per tick, scaled |
| OSC     | gate    | Fires when an oscillation cycle is detected |
| ROW     | poly    | Per-row live count (16 of 24 rows, 0..10 V) |

Right-click presets switch the CA rule: Conway's Life (B3/S23), HighLife
(B36/S23), Day & Night (B3678/S34678), Majority (B5678/S45678), Voter
(B45678/S45678). The current rule is shown on the panel header.

### Schelling

Schelling segregation on a 24×24 grid. Two populations (red, blue) plus
empty cells. Each tick, every agent checks the fraction f of its 8 Moore
neighbours that share its type; if f < θ the agent is unhappy and
relocates to a random empty cell. The famous result: even at θ = 0.30
the system reaches highly segregated steady states.

| Param      | Range     | Default | Effect |
|------------|-----------|---------|--------|
| θ          | 0..1      | 0.30    | Tolerance — min same-type fraction agents tolerate |
| OCC        | 0.1..0.95 | 0.85    | Initial occupancy |
| BAL        | 0..1      | 0.5     | Fraction of occupied cells that are type 0 |
| SHUFFLE    | button    | —       | Re-seed |

| Input    | Notes |
|----------|-------|
| CLOCK    | Internal 8 Hz if unpatched |
| RESET    | Re-seed |
| θ·CV     | 0..10 V adds to θ |

| Output  | Range   | Notes |
|---------|---------|-------|
| SEG     | 0..10 V | Mean f_same across occupied cells (segregation index) |
| UNH     | 0..10 V | Fraction unhappy |
| LOCAL   | poly    | Per-quadrant (4×4 = 16) mean f_same |
| QUIET   | gate    | High when no agents moved last tick (equilibrium) |

### Turing

Gray-Scott reaction-diffusion on a 48×48 toroidal grid. Two chemicals
u and v diffuse and react: u + 2v → 3v (autocatalysis), v → P (decay).
Different (F, k) regions produce spots, stripes, labyrinth, or solitons
— Turing's original mechanism for biological pattern formation.

| Param  | Range        | Default | Effect |
|--------|--------------|---------|--------|
| F      | 0.005..0.12  | 0.040   | Feed rate |
| k      | 0.030..0.080 | 0.060   | Kill rate |
| Du     | 0.05..0.30   | 0.16    | u diffusion |
| Dv     | 0.02..0.20   | 0.08    | v diffusion |

| Input  | Notes |
|--------|-------|
| CLOCK  | Diffusion sub-step rate (internal 240 Hz) |
| RESET  | Re-seed v patch |
| F·CV   | ±5 V → ±0.05 to F |
| k·CV   | ±5 V → ±0.02 to k |

| Output | Range   | Notes |
|--------|---------|-------|
| ū      | 0..10 V | Mean(u) |
| v̄      | 0..10 V | Mean(v), scaled ×5 |
| VAR    | 0..10 V | Spatial variance of v (complexity readout) |
| OSC    | gate    | Fires when var(v) drops sharply (pattern formed) |

---

## Decisions

### Prospect

Kahneman-Tversky (1992) prospect theory transformation. Takes a
two-dimensional input — an objective outcome x and an objective
probability p — and outputs the subjective valuation:

  Û(x) = x^α for x ≥ 0;  −λ·(−x)^β for x < 0
  ŵ(p) = p^γ / (p^γ + (1−p)^γ)^(1/γ)

| Param  | Range    | Default | Effect |
|--------|----------|---------|--------|
| α      | 0.1..1.5 | 0.88    | Gain curvature (1 = linear, < 1 = concave) |
| λ      | 1..4     | 2.25    | Loss aversion (kink at x = 0) |
| γ      | 0.3..1.5 | 0.61    | Probability weighting (< 1 = overweight rare events) |
| β      | 0.1..1.5 | 0.88    | Loss curvature |

| Input | Notes |
|-------|-------|
| X     | Outcome x (±10 V = ±10 units) |
| P     | Probability p (0..10 V = 0..1) |

| Output | Notes |
|--------|-------|
| U      | Û(x) — subjective value |
| W      | ŵ(p) — subjective decision weight (0..10 V) |
| SU     | Û(x)·ŵ(p) — subjective expected utility |
| EV     | x·p — objective expected value, for comparison |

The on-panel visualization draws Û(x) and ŵ(p) simultaneously with the
current operating point (yellow dot) marked on both. A 45° linear
reference line in each shows the deviation from the rational-agent
expected-utility baseline.

### Bandit

Multi-armed bandit with up to 8 arms. Each tick the agent selects one
arm, observes a reward, updates its running mean estimate, and accrues
regret = μ* − r against the true optimum.

| Param  | Range  | Default | Effect |
|--------|--------|---------|--------|
| K      | 2..8   | 4       | Number of arms |
| ε / c  | 0..1   | 0.10    | ε (greedy explore prob) / c (UCB bonus weight) |
| σ_μ    | 0..5   | 1.5     | SD of true arm means at randomize |
| σ_r    | 0..3   | 0.8     | Per-pull noise SD |
| SHUFFLE| button | —       | Re-draw arm means |

| Input  | Notes |
|--------|-------|
| CLOCK  | Internal 4 Hz; one arm pulled per tick |
| RESET  | Re-draw and clear learning |

| Output  | Notes |
|---------|-------|
| R       | Last pull's reward (±12 V) |
| REGR    | Cumulative regret (scaled) |
| ARMS    | Per-arm pull gate (polyphonic; 30 ms pulse on pull) |
| BEST    | Fraction of pulls on the optimal arm (0..10 V) |

Right-click selects the action policy: **ε-greedy** (with probability
1−ε pull the current arg-max estimate; with probability ε explore
uniformly), **UCB1** (pull arg-max of Q̂ + c·√(ln t / N_i) — the
classical Hoeffding upper-bound bonus), or **Thompson sampling**
(Bayesian: sample one value from each arm's Normal(Q̂, 1/√N) posterior,
pull the arm with the largest sample).

The visualization shows per-arm: the true mean μ_i as a horizontal
tick, the running estimate Q̂_i as a dot, the ±2σ confidence band as a
shaded rectangle, with the optimal arm coloured green and the most
recent pull coloured yellow.

### DDM

Drift-diffusion (Ratcliff 1978) two-alternative forced-choice model.
An evidence accumulator x_t integrates noisy samples until it crosses
one of two absorbing boundaries at ±a; the trial ends and a new one
begins after a brief hold-and-flash. The trace paints left-to-right,
one trial at a time.

| Param  | Range    | Default | Effect |
|--------|----------|---------|--------|
| DRIFT v| −2..2    | 0.6     | Signal strength (0 = random walk; |v| → straight ramp) |
| a      | 0.2..5   | 1.5     | Threshold — speed/accuracy trade-off |
| σ      | 0..3     | 1.0     | Within-trial noise |
| BIAS z | −1..1    | 0       | Starting point z·a (response prejudice) |

| Input    | Notes |
|----------|-------|
| CLOCK    | Internal 120 Hz |
| RESET    | Restart trial, clear accuracy |
| DRIFT·CV | ±10 V → ±1 added to v |

| Output  | Notes |
|---------|-------|
| EVID    | Live evidence x_t scaled to ±a → ±10 V |
| CHO     | Choice gate (pulse when upper boundary hit) |
| RT      | Last completed trial's reaction time |
| ACC     | Running accuracy = correct-choice fraction (0..10 V) |

A boundary hit flashes the chosen side (green = upper, red = lower)
and the trace freezes for 750 ms so the completed path stays visible
before the next trial begins.
