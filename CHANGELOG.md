# Empiria changelog

## 2.0.2 — 2026-05-19

Pre-Library-submission audit release. No new modules; many
correctness, save/load, and audio-thread safety fixes across the
suite. Five plugin slugs and all module slugs unchanged from 2.0.1;
patches saved with 2.0.1 load cleanly into 2.0.2 (the new fields
default to empty arrays so older patches without the saved state
behave as before).

**Memory-safety**

- Polis/Diffusion and Polis/Network now check the right-neighbour
  module identity before writing into its left-expander producer
  buffer (previously could clobber an unrelated third-party module's
  ~300-byte expander buffer if placed adjacent).
- EmpiriaNetworkMessage gained an explicit `uint16_t version` field
  alongside its magic header so future protocol layout changes can
  be detected safely.

**Save/load (reproducibility)**

- Methods/Cohort: `prevActiveK` now syncs to the restored `K_PARAM`
  in dataFromJson; previously the first process() tick after load
  silently overwrote the just-loaded cluster centres.
- Polis/{Cascade, Diffusion, Discourse, Pareto, Dilemma}: full
  per-agent state (thresholds, adoption flags, opinions, wealth
  values, strategies, scores) now persists to JSON.
- Polis/Network: seed now persists so the downstream graph-aware
  module sees the same adjacency on patch reload.
- Space/{Life, Schelling, Turing}: 24x24 / 48x48 grid state now
  persists (packed bitmaps for Life/Schelling, flat float arrays
  for Turing's u and v).
- Epi/Outbreak: graphSeed, per-agent state[], and peak-detection
  state now persist.
- Decisions/Bandit: per-arm `mu`, `sumR`, `count`, `totalPulls`,
  `bestArm`, `bestPulls`, `cumRegret` all persist (previously only
  `seedVal` and `policy`).
- Decisions/DDM: `x`, `trialSteps`, `trials`, `correct`, `lastRT`,
  `seedVal` now persist (previously none).

**Audio-thread safety**

- Methods/Strata: pre-allocated `seasonalTable` and `seasonalScratch`
  buffers at construction (kMaxPeriodSec * 192 kHz worth); process()
  resize + swap touches no heap.
- Methods/Lag: replaced per-tick `std::vector<float> data(n)` with
  a member `acfScratch` array.
- Methods/Boot: replaced per-bootstrap `std::vector<float>` for orig,
  resample, jack, loo, sortScratch with five member std::array
  buffers; statOn() now non-static, takes a pointer + length, and
  uses the shared sortScratch for the median path.
- Methods/Gauge and Space/Turing: function-local `static int`
  throttle counters promoted to instance members (previously shared
  across all instances in a patch).
- Space/Turing: capped substeps absorbed per process() call so an
  audio-rate clock cannot push the 2304-cell Gray-Scott update onto
  the audio thread at 48 kHz.
- Space/Schelling: pre-allocated `unhappyBuf` and `emptyBuf` member
  arrays; previously stepCA() allocated two per-tick vectors.

**Numerical correctness**

- Methods/Test: two-sample df now uses Welch-Satterthwaite (matches
  the Welch SE the t-statistic already uses); previously the pooled
  equal-variance df gave a p-value for the wrong distribution.
- Decisions/Bandit: UCB1 bonus is now the classical sqrt(2 ln t /
  n_i) (the textbook factor of 2 was missing). Thompson sampler
  documented as a Normal-Normal heuristic, not Beta conjugate.
- Methods/Sample: Beta PDF visualization clamps endpoint evaluation
  so the kernel does not diverge at extreme alpha,beta.
- Methods/Regress: confidence band now uses a Student-t multiplier
  with df = n - 2 (Cornish-Fisher approximation) instead of the
  normal-z; materially wider at small n.

**Description-vs-behaviour mismatches**

- Epi/Outbreak: about-menu rewrote ("Compartmental SIR/SEIR with
  sigma" -> the actual network SIR-on-graph that the module
  implements). Tag list: dropped incorrect "Logic" tag.
- Methods/Strata and Methods/Factor: dropped "Polyphonic" tag (the
  modules read only channel 0 of their inputs).
- Space/Turing: dropped "Polyphonic" tag (no polyphonic outputs).
- Polis/Cascade: clock-input tooltip said "free-runs at 8 Hz" but
  the constant is 30 Hz; corrected. About-menu's "Network (supply
  structure)" companion claim removed (Cascade never reads
  leftExpander).

**New cross-plugin functionality**

- Epi/Outbreak now consumes Polis/Network's adjacency-on-expander
  protocol when placed to Network's right. Falls back to internal
  graph generation when no upstream Network is present.
- Epi/Outbreak visualization scales node radius by degree, so
  Barabasi-Albert hubs are visually identifiable as the description
  promises.

**Compliance**

- Polis, Space, and Decisions plugin folders gained a LICENSE file
  (previously the Makefile's `LICENSE*` glob shipped nothing,
  technically a GPL section 4 violation).

**Polish**

- Space/Life: per-row polyphonic output now spans the grid evenly
  (channel 15 used to skip row 22).
- Methods/Seed: VALUE_PARAM marked `randomizeEnabled = false` so
  right-click Randomize does not scramble the reproducibility
  primitive.
- Methods/Sample: Beta-PDF visualization clamps endpoint evaluation.
- Decisions/Prospect: value-function visualization range corrected
  to match the +/-10 V input scaling promised in configInput.
- Polis/Pareto: clamp values >= 0 in tick() so float-accumulation
  drift cannot poison the Gini / topShare divisors.

## 2.0.1 — 2026-05-19

Metadata-only release in preparation for the VCV Rack Library
submission. No module behavior changes.

- Fixed `sourceUrl`, `manualUrl`, and `changelogUrl` in all five
  plugin.json files (previously pointed to a non-existent
  `kevinschoenholzer/empiria` path; now point to the canonical
  `kevisc/empiria` repository).
- Updated `pluginUrl` to <https://shlabs.kevinschoenholzer.com>.

## 2.0.0 — 2026-05-16

First public release of the Empiria suite for VCV Rack 2. Bundles
five plugins under the SHLabs brand:

- **Polis** (6 modules): Cascade, Diffusion, Dilemma, Discourse,
  Network, Pareto — social and economic dynamics.
- **Methods** (15 modules): Boot, Code, Cohort, Factor, Frame,
  Gauge, Lag, Quantity, Regress, Sample, Seed, Strata, Tab, Tape,
  Test — the statistical analysis layer.
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
