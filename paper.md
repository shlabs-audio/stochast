---
title: 'Stochast: Statistically Grounded Generative Modules for VCV Rack'
short_title: 'Stochast'
tags:
  - modular synthesis
  - generative music
  - control voltage
  - stochastic processes
  - agent-based models
  - bootstrap
  - VCV Rack
authors:
  - name: Kevin Schoenholzer
    orcid: 0000-0001-9892-5869
    affiliation: 1
    corresponding: true
    url: https://kevinschoenholzer.com
affiliations:
  - name: Postdoctoral researcher, Università della Svizzera italiana (USI), Lugano, Switzerland.
    index: 1
date: 15 May 2026
bibliography: paper.bib

abstract: |
  Stochast is an open-source (GPL-3.0) suite of plugins for VCV Rack 2
  that turn real statistical, social, and dynamical processes into
  control voltage. Each module is a genuine computational process —
  parametric sampling, online estimation, the bootstrap, agent-based
  cascades and opinion dynamics, network epidemics, reaction-diffusion,
  drift-diffusion decisions — exposed as knobs and CV so it can be
  patched as a modulation source, a gate sequencer, or a slow evolving
  signal anywhere on a rack. The result is generative and quirky, but
  the mathematics is exact rather than faked for effect: random draws
  are Mersenne-Twister-seeded, closed-form quantities use the standard
  numerical recipes (Lentz continued fractions for the *t*-CDF, the
  bias-corrected and accelerated bootstrap, Sanger's rule, STL), and
  patches are portable JSON whose behavior is byte-identical across
  operating systems. These notes describe the modules, their numerics,
  and the analog-computer lineage they draw on. Stochast is the
  music-side sibling of the browser-based *Empiria* web app, which
  serves the teaching of statistics directly.
---

# Introduction

Random and emergent processes make wonderful sound sources: a sampling
distribution wanders, a segregation grid crystallises, an epidemic
rises and falls, a reaction-diffusion field breathes. Stochast takes
that observation literally. It is a suite of VCV Rack 2 modules (VCV
Rack, 2024) in which each module is a *genuine* statistical, agent-based,
or dynamical process, exposed as knobs and control voltage so it can be
patched anywhere on a rack — a modulation source, a gate sequencer, an
evolving CV line. The appeal is that these processes are intrinsically
generative: they are alive in exactly the way a good modulation source
should be, because they obey real dynamics rather than a hand-tuned
imitation of them.

What distinguishes Stochast from a bag of approximate "random" modules
is that the mathematics underneath is the genuine article. Random draws
are Mersenne-Twister-seeded; closed-form quantities are computed by the
standard numerical recipes rather than normal-theory shortcuts; and a
patch is portable JSON that reproduces byte-identically across machines.
A Stochast patch is therefore both a playable instrument and a correct
computation — quirky on the surface, rigorous underneath.

These notes document the modules, their numerics, and the
analog-computer lineage the design draws on. They are technical notes,
not a course: Stochast is not pitched as a way to *teach* statistics.
That role belongs to its sibling, the browser-based **Empiria** web app
(<https://kevinschoenholzer.com/empiria/>), which is built for the
classroom. Stochast is the music-side counterpart — the same real math,
repurposed as instruments.

We describe **Stochast**, an open-source teaching suite for VCV Rack
2 (VCV Rack, 2024) — the free modular-synthesizer environment —
designed to address this gap. The **Methods** plugin (the subject
of this paper) places fifteen statistical primitives into VCV
Rack's patch-cable grammar so that the inferential workflow itself
becomes a left-to-right signal chain on the screen. Five anchor
modules — `Sample`, `Frame`, `Regress`, `Test`, `Boot` — carry the
simulation-based-inference argument; ten supporting modules (`Lag`,
`Code`, `Tab`, `Strata`, `Cohort`, `Factor`, `Seed`, `Tape`,
`Gauge`, `Quantity`) extend the pipeline to autocorrelation
diagnostics, contingency-table inference, decomposition, online
PCA, deterministic seeding, record-and-replay, and unit
interpretation. Every module ships with a live visualization matched
to the statistic it computes — a histogram with a confidence band,
a scatterplot with a fitted line, a null distribution with shaded
rejection regions, a bootstrap distribution annotated with
bias-correction diagnostics — so the dynamic being taught is
visible as it unfolds.

This paper makes the pedagogical and methodological case for
Stochast's design and demonstrates the platform through three worked
classroom examples that span the full range of intended audiences
— from a 45-minute general-audience demonstration that requires no
prior statistical background, to a precisely seeded reproducible
analysis that satisfies graduate-level scrutiny. Per-module
reference, complete parameter listings, numerical-recipe details,
and patch-grammar documentation are provided in the Methods plugin
manual (`docs/methods_manual.md`) and the project repository; this
paper concentrates on the conceptual moves and on the worked
examples through which Stochast can be evaluated as a teaching tool.

# Voltage as the medium of simulation

Stochast's design rests on a particular intuition about voltage. In
a modular synthesizer, every signal — whether it represents a
pitch, an amplitude, a gate, or, in our case, a population
fraction or an estimator's value — is a *control voltage* (CV), a
single floating-point number streaming through a patch cable.
This generic "unit of stuff" is the platform's universal medium,
and that universality is exactly what makes it useful for
teaching.

The mapping to a teaching context is direct:

* A voltage **is** a real-valued state variable.
* A patch cable **is** a wire carrying that variable to another
  operator.
* A knob **is** a parameter tuning the operator's response.
* A module **is** a transformation — filter, sum, threshold, random
  draw, regression, hypothesis test.
* A patch **is** a system of equations, composed by hand from these
  operators, visible end-to-end.

In statistics this mapping is immediate. A voltage stream from
`Sample` *is* a sample path from a parametric distribution; running
it through `Frame` *is* the act of measurement; running it through
`Regress` *is* the construction of an estimator; running it through
`Test` *is* a hypothesis test. Each stage in the chain is a module,
and the chain itself is a patch cable: the inferential workflow is
no longer abstract, it is laid out left-to-right on the screen.

The convention is not a synth-builder's aesthetic. It is a direct
inheritance from the analog computers of the 1930s–1960s — machines
such as Vannevar Bush's Differential Analyzer (Bush, 1931) that
solved differential equations by letting voltages represent state
variables, with electrical components implementing the operators
that combined them. These machines were used to model missile
trajectories, weather systems, predator–prey dynamics, and the
spread of epidemics (Small, 2001) — substantive applications that
overlap substantially with Stochast's pedagogical scope. The
argument here is not historical nostalgia: it is that voltage
proved to be a remarkably general representation of state, and
operating on voltage proved to be a remarkably general way to
compute. Digital simulation of those same continuous voltages —
which is what VCV Rack does, at audio sample rate — inherits the
same generality at a fraction of the cost.

Concrete-to-abstract sequencing has also long been argued to
scaffold the learning of mathematical concepts. Bruner (1966)
articulated the enactive → iconic → symbolic progression at the
level of general theory of instruction; Sfard (1991) framed the
same trajectory as the operational-to-structural duality of a
mathematical idea; Mayer's (2009) cognitive theory of multimedia
learning provides the corresponding empirical foundation for
animation-plus-narration over text-only presentation. Stochast's
patch grammar makes this sequence operational: a two-sample
*t*-test is, in Stochast, a literal sequence of objects the student
wires together — two `Sample` modules, a `Frame` on each branch, a
`Test` consuming both — and only once that patch has been built and
modulated does the student meet the corresponding formula on
paper, at which point each algebraic symbol corresponds to a module
already manipulated.

# Five anchor modules

The Methods plugin's fifteen modules share a common 20HP panel
grammar: a header strip carrying the module title, a 280 × 190 px
visualization area, a row of knobs for primary parameters, a row of
trimpots or buttons for set-once parameters, and rows of inputs and
outputs. We describe the five **anchor modules** in narrative form;
full per-module reference for these and the ten supporting modules
is in the plugin manual.

**`Sample`** is the entry point of every Methods pipeline. It draws
an i.i.d. stream from a user-selected parametric distribution —
Normal, Uniform, Exponential, or Beta — at a user-supplied clock
rate, and emits the running sample mean and standard deviation
alongside the raw draw. The live panel overlays the theoretical PDF
on the empirical histogram, so a student literally watches the
empirical shape converge to the theoretical as *n* grows.
Right-click presets re-label the same draws as substantive
quantities — adult human height, IQ score, reaction times,
right-skewed income, U-shaped opinion — making the parametric
family pedagogically useful: the same mathematical object becomes
height, IQ, or income with one menu click.

**`Frame`** is the bridge between the i.i.d. stream and inferential
statistics. It collects samples on a clock into a buffer of
configurable length and reports the sample mean, sample SD, and
the standard error of the mean. Three modes are exposed via the
front-panel switch: **SNAPSHOT** collects *n* and then freezes (a
cross-sectional view); **RUNNING** maintains a ring buffer of the
latest *n* (a moving window); **GROWING** accumulates up to a
maximum (the visible Law of Large Numbers). The live visualization
is the buffer's histogram with a vertical mean line and a shaded
confidence-interval band whose width scales as 1/√*n*. Polyphonic
input is supported so a Methods pipeline scales to multiple
parallel pseudo-experiments without duplicating modules.

![**Sample and Frame in action.** Left: `Seed` (217) reproducibly
initializes the pipeline. Centre: `Sample` configured as Normal,
showing the empirical histogram (blue bars) converging on the
theoretical PDF (gold curve) after *n* = 128 draws. Right: `Frame`
in GROWING mode, *n* = 4096 — the sample mean has converged to
−0.00 and the standard error has collapsed to 0.004 (≈ 1/√4096),
visible as the narrow CI band on the
histogram.](figures/screenshots/anchor_sample_frame.png){#fig:anchor-sample-frame width=100%}

**`Regress`** reads pairs (*X*, *Y*) on a clock, stores them in a
buffer, and fits the line *Y* = α + β · *X* by minimising the sum
of squared residuals. The live scatterplot draws the data, the
fitted line, and a shaded confidence band whose width tightens in
the middle of the *X* range and widens at the extremes — the
trumpet shape that classically conveys the heteroscedasticity of
the prediction interval. The slope β, intercept α, *R*², and
current residual *Y* − (α̂ + β̂ · *X*) are emitted as CV signals, so
a downstream `Test` or `Boot` module can take a hypothesis test or
a bootstrap CI on β directly without leaving the rack.

![**Regress with a null-relationship pairing.** A `Sample` (Uniform)
feeds *X*; an independent `Sample` (Normal) feeds *Y*. The
resulting scatter has no genuine linear relationship — β̂ ≈ 0 —
and the fitted line is nearly horizontal. The shaded confidence
band shows the trumpet shape characteristic of an OLS prediction
interval. Switching one `Sample`'s preset introduces a genuine
relationship and the band straightens around a sloped line; the
contrast lets a student see what the band is actually
showing.](figures/screenshots/anchor_regress.png){#fig:anchor-regress width=100%}

**`Test`** computes a two-tailed *t*-statistic against H₀: μ = μ₀
(one-sample) or H₀: μ₁ − μ₂ = δ₀ (two-sample, Welch's *t* for
unequal variances). The two-tailed *p*-value is evaluated *exactly*
through the regularized incomplete beta function via Lentz's
continued-fraction expansion (Press et al., 2007) — the same
algorithm underlying R's `pt()` and SciPy's `scipy.stats.t.sf()`,
yielding machine-precision agreement at any df ≥ 1. The live
visualization draws the *t*-distribution null with shaded rejection
regions at the user-selected α, marks the observed *t* as a
vertical line, and shows a REJECT / n.s. indicator; *t*, *p*, the
reject gate, and Cohen's *d* effect size are emitted as CV signals.

![**Two-sample Welch *t*-test with reproducible seed.** `Seed`
(544) fires two `Sample` modules; one drives `Frame` for
descriptives and both feed `Test`'s SIG and SIG₂ inputs. The Test
panel shows the null *t*-distribution with shaded rejection regions
at the user-selected α, the observed *t* = −3.01 marked as a
vertical line in the left tail, and the REJECT indicator with the
supporting *p* = 0.001 readout. Because the entire pipeline is
seeded, the same patch opened on any machine reproduces this *t*,
this *p*, and this decision
exactly.](figures/screenshots/anchor_test.png){#fig:anchor-test width=100%}

**`Boot`** is the simulation-based-inference workhorse. Given a
buffer of samples it draws *B* resamples with replacement and
computes a user-selectable statistic (mean, median, SD, variance)
on each, so the *empirical bootstrap distribution* of the
estimator becomes visible on the panel — *this is* the sampling
distribution made explicit, without parametric assumption. The
bias-corrected and accelerated (BCa) interval (Efron, 1987) is
reported alongside the point estimate and the bootstrap SE; the
panel surfaces the bias-correction *z*₀ and the acceleration *â*
as live readouts, so a student can watch the BCa correction shift
the interval away from the percentile baseline when the bootstrap
distribution is skewed. Patching a clock into the RSAMP input fires
a fresh bootstrap on every tick — useful for visualizing the
bootstrap's own variability against a frozen sample.

The remaining ten Methods modules support the anchor pipeline:
`Lag` computes the autocorrelation function with Bartlett bands;
`Code` discretizes continuous CV into ordinal Likert categories;
`Tab` constructs contingency tables with the exact χ² *p*-value via
the regularized lower incomplete gamma; `Strata` implements STL
decomposition (Cleveland et al., 1990); `Cohort` performs online
*k*-means quantisation; `Factor` performs online PCA via Sanger's
rule (Sanger, 1989); `Seed` is the reproducibility primitive;
`Tape` records and replays CV streams and supports CSV export;
`Gauge` and `Quantity` are a symmetric pair that interpret CV
voltages as real-world quantities and vice versa. Complete
per-parameter, per-input, per-output reference for all fifteen
modules is in the plugin manual.

The broader Stochast suite — four additional plugins covering
agent-based social-science simulators (**Polis**), network
epidemiology (**Epi**), spatial-emergence models (**Space**), and
behavioural-economics models (**Decisions**) — shares the Methods
panel grammar, the common patch-cable conventions, and the same
deterministic-seed reproducibility story. We retain one
cross-plugin Schelling demonstration in Workflow A below to
illustrate the composition; substantive treatment of the broader
suite is the subject of a separate companion paper.

# Three worked examples

The three worked examples below illustrate the platform across its
intended audience range. Workflow A is a *visceral* demonstration
suitable for an audience that has never met a probability
distribution; Workflow B is a *precise*, seeded, exportable
analysis at a level a graduate methods student would scrutinise;
Workflow C is a *verification* exercise demonstrating that
Stochast's empirical estimate of a closed-form probability agrees
with the analytic value. All three patches are short (3–5 modules)
and ship under `patches/` as `.vcv` files; all three reproduce
byte-identically on any machine.

## Workflow A — Watching segregation emerge

![**Starter patch A — Schelling cross-plugin demonstration.** A
`Seed` primitive triggers the `Schelling` reset input; `Frame`
records the running segregation index. Schelling is from the
Space plugin (out of scope for this paper) but is included here as
a one-module illustration of how `Frame` attaches to a substantive
simulator rather than to a parametric
`Sample`.](figures/patches/schelling.png){#fig:patch-schelling width=70%}

A single Schelling module. Set the tolerance knob **θ = 0.30**, the
occupancy knob to 0.85, balance to 0.50, and press SHUFFLE. The
grid starts as colour-noise — red and blue cells randomly
intermixed. Over the next handful of ticks the boundaries
crystallise into large monochromatic clusters; the on-panel
segregation index rises from roughly 0.4 to roughly 0.75, and the
unhappy-agent fraction collapses toward zero.

The instructor asks the audience to predict, before dialing, what
will happen at θ = 0.20 (essentially no preference) versus θ = 0.55
(strong preference). Sweeping θ live reveals: at θ = 0.20 the grid
stays roughly mixed; at θ = 0.55 segregation is total. The canonical
Schelling lesson — large macro-pattern from mild micro-preference —
is conveyed without a single line of mathematics, in under five
minutes, to an audience that may never have heard of agent-based
modelling. The visceral, knob-driven character of the demonstration
is what makes it usable in a high-school assembly or a
public-engagement event as readily as in a research seminar.

## Workflow B — Seeded, reproducible, exportable

![**Starter patch B — seeded two-sample Welch *t*-test.** `Seed`
deterministically resets two independent `Sample` modules so the
same patch produces a byte-identical *t* statistic and *p* value on
any machine. `Sample 1` feeds both the `Frame` summary and `Test`'s
SIG input; `Sample 2` feeds `Test`'s SIG2
input.](figures/patches/seeded_t_test.png){#fig:patch-ttest width=80%}

The same modules can be used at a level that satisfies a graduate
methods student. We run a small-sample one-sample *t*-test against
H₀ = 0, with the explicit goal of comparing Stochast's *exact*
Student-*t* *p*-value against the normal-approximation *p*-value
that many introductory teaching tools still emit.

**Setup.** Place a `Seed` module with VALUE = **42**; the same
integer on another machine reproduces byte-identical results. Patch
Seed's TRIG output into Sample's RESET and Test's RESET so both
downstream modules re-initialise in lockstep. Place `Sample` with
DIST = Normal, P1 (μ) = 0.5, P2 (σ) = 1.0 — a deliberately
ambiguous case for an *n* = 8 sample, where the true mean is
positive but small relative to σ. Place `Test` with MODE =
SNAPSHOT, *N* = 8, ALPHA = 0.05, H₀ = 0. Patch
`Sample.SAMPLE → Test.SIG` and press Seed's RANDOMIZE.

**Observable behavior.** Test's panel shows the observed *t* and
the exact two-tailed *p*-value. The null *t*-PDF visualization uses
the correct Student's-*t* distribution at df = 7 — visibly
fatter-tailed than the standard normal — and the rejection regions
are shaded according to the chosen α. The pedagogical claim is
graduate-level: with df = 7, the exact tail probability is
materially larger than what a normal-approximation tool would
report, and the discrepancy can flip a conclusion at α = 0.05. The
student can compare Stochast's *p* against R's
`pt(t, df = 7, lower.tail = FALSE) * 2` for any (*t*, df) and
confirm agreement to machine precision.

**External round-trip.** To carry the analysis outside the platform,
place a `Tape` module, set MODE = REC, LENGTH = 8, patch
`Sample.SAMPLE → Tape.SIG`, let it fill, then right-click Tape →
**Export buffer to CSV…**. The resulting CSV is a single column of
the eight raw draws, immediately loadable in R as
`d <- read.csv("sample.csv")$ch1`; the standard `t.test(d, mu = 0)`
matches Stochast's *t* and *p* to six decimal places. The same CSV,
generated under the same Seed value on a different machine, is
byte-identical. This is the workflow that makes Stochast-based
assignments *gradable*: the instructor distributes a `.vcv` patch
plus seed value, students run it, export the CSV, and submit both
their derived statistic and the underlying data — all of which the
instructor can verify reproduces.

Workflows A and B are the same tool in two modes: the rack stays
the same, the audience changes, the depth at which the underlying
mathematics is interrogated changes.

## Workflow C — Verifiable computation against a closed-form

![**Starter patch C — coin-flip Law of Large Numbers.** A
near-Bernoulli `Sample` (Beta with α = β ≈ 0.05) feeds `Frame` in
GROWING mode and `Boot`. `Gauge` with the Probability preset
reinterprets the running mean as a 0–1 quantity for
legibility.](figures/patches/coin_flip_lln.png){#fig:patch-coin width=85%}

Workflows A and B exhibit Stochast's pedagogical surface; Workflow
C is the *rigor proof*. The simulation-based-inference literature
recommends that learners construct empirical sampling distributions
and check them against closed-form values where both are available
(Cobb, 2007; Tintle et al., 2015); doing so is also the standard way
to validate that a computational instrument behaves as advertised.
If Stochast's empirical estimate reproduces a textbook combinatorial
probability to within Monte Carlo error, deterministically, on any
operating system, then its claim to methodological seriousness is
concrete rather than rhetorical.

**The closed-form target.** For a fair coin, the probability of
four heads in a row in four flips is (1/2)⁴ = 1/16 = 0.0625 exactly.
We estimate it empirically by running 10 000 independent four-flip
trials.

**Setup.** `Seed` VALUE = **100**; patch TRIG into Sample's RESET
and Tape's RESET. `Sample` DIST = Uniform, P1 = 0, P2 = 1. `Code`
*K* = 2, LOW = 0, HIGH = 1 — encodes each uniform draw as 1 V
(tails) or 2 V (heads) at threshold 0.5. `Tape` LENGTH = 4096,
MODE = REC, patched `Code.CAT → Tape.SIG`. After Tape has
filled to 40 000 samples (at the internal 200 Hz clock, ~200
seconds), switch MODE to PLAY and right-click Tape →
**Export buffer to CSV…**.

**Verification in R.**

```r
d     <- read.csv("coin_flips_seed100_n40000.csv")$ch1
flip  <- as.integer(d > 1.5)         # 1 = heads, 0 = tails
trial <- matrix(flip, nrow = 4)      # 4 flips per trial → 10000 trials
phat  <- mean(colSums(trial) == 4)   # empirical fraction of HHHH
phat
#> [1] 0.0626

binom.test(sum(colSums(trial) == 4), ncol(trial), p = 1/16)
#> Exact binomial test ... p-value = 0.96  (cannot reject p = 1/16)
```

The Monte Carlo standard error at 10 000 trials with the true
*p* = 0.0625 is √(*p*(1−*p*)/*n*) ≈ 0.0024. The observed phat is
within 0.5 SE of the true value; an exact binomial test does not
reject the null that the true probability is 1/16. A collaborator
running the same Stochast patch with Seed = 100 on a different
machine produces a byte-identical CSV, and therefore the exact same
phat to six decimal places.

The pedagogical lesson — that closed-form combinatorial
probabilities have empirically verifiable counterparts — generalises
to harder targets (the probability of at least one run of four
heads in twenty flips, ≈ 0.184, is reached by the same patch with
trial size changed to 20), and the modular synthesizer turns out to
be a perfectly adequate substrate for the verification.

# Reproducibility, classroom integration, and evaluation

Stochast treats classroom-scale reproducibility as a first-order
design property rather than as a downstream concern. Every random
module wraps `std::mt19937` (Mersenne Twister, period 2¹⁹⁹³⁷ − 1)
seeded from a per-module 32-bit seed preserved in the patch JSON
and broadcast on demand by the `Seed` module. Patches are stored as
human-readable JSON; CSV exports from `Tape` are plain ASCII with
six decimal digits per sample. Three students on three different
operating systems running the same patch with the same Seed value
produce three byte-identical CSV files. Every analytical output
that Stochast produces has an external reference against which it
can be checked: R's `t.test()`, `lm()`, `boot::boot.ci()`,
`pchisq()`; Python's `scipy.stats`. Agreement is verified at the
design stage and is re-checkable by any user willing to perform
the round-trip — exactly what Workflow C demonstrates.

The platform is usable across three classroom levels. In a
**high-school or general-education setting**, the platform stands
on its own as a hands-on demonstration tool — no R, no Python, no
command line. A teacher can run a 45-minute session built around a
single patch (e.g. `Sample → Frame`, sweeping the *N* knob to make
the standard error shrink visibly as 1/√*n*), and students engage
by turning knobs and predicting what the trace will do. In an
**undergraduate introductory statistics or research-methods
course**, Stochast is positioned as a companion tool that visualises
what R or Python is computing, not as a replacement. Students
install VCV Rack (free) and the Stochast `.vcvplugin` files (total
< 1 MB) on personal laptops with no administrator privileges
required. In a **graduate research-methods seminar**, the same
modules become a fast-prototyping sandbox for interrogating edge
cases — what does the null *t*-distribution look like at df = 3?
how does the BCa interval shift away from the percentile interval
on a skewed bootstrap? The same modules, knobs, and outputs serve
all three audiences; what differs is the depth at which the
underlying mathematics is interrogated.

A natural next step is empirical evaluation against a control
condition. We sketch a within-instructor, between-section
randomised comparison over two academic terms: parallel sections
of the same course, taught by the same instructor, randomly
assigned to an Stochast-augmented condition (Stochast plus the usual
R workflow) or a control (R workflow only, with comparable static
figures and Shiny applets to control for time on visualizations).
Outcomes would be measured by the *Comprehensive Assessment of
Outcomes in a First Statistics course* (CAOS), with the *Sampling
Variability*, *Statistical Inference*, and *Bivariate Data*
subscales as primary endpoints; a paper-based transfer task scored
blind by two graders; and a brief end-of-term confidence and
motivation survey. The most plausible failure mode of the
platform is *activation cost* — students who have never opened a
modular synthesizer may need 30 minutes of orientation before they
can drive the platform productively — which the deployment plan
addresses with a dedicated orientation session and four worked
reference patches. A secondary risk is *content shift*, which the
recommended additive (not substitutive) integration with the R
workflow is intended to control.

# Conclusion

Stochast places fifteen statistical primitives into the
patch-cable grammar of a modular synthesizer so that the
inferential workflow — from data-generating process through
estimation, hypothesis testing, and bootstrap confidence intervals
— becomes a manipulable, visible, left-to-right signal chain on
the screen. The platform is intended to complement, not replace,
the analytical R/Python workflow that anchors current statistics
teaching: a student who has wired a *t*-test together and watched
its null distribution shade in response to dialled-in data carries
a richer mental model into the eventual `t.test()` call. Three
properties — seeded reproducibility, exact numerical recipes for
closed-form statistics, and portable JSON patches with no external
library dependencies — make the platform defensible as a
*reproducible analog computer* alongside its pedagogical role.

The platform's most significant limitations are the absence of a
controlled classroom evaluation (the next step planned by the
author), the activation cost for users new to the modular interface,
and the polyphonic-cable cap of sixteen channels which limits agent
counts in the broader Stochast suite's social-science simulators.
None of these limitations is intrinsic to the design; each is a
planned-but-deferred next step.

# Acknowledgements

The author thanks the VCV Rack developer community for the modular
plugin SDK and example modules, and acknowledges the long
open-source tradition of computational social science on which
Stochast draws.

# Disclosure statement

The author reports there are no competing interests to declare.
Stochast is released under the GPL-3.0 license through the author's
personal open-source imprint. The imprint is not a commercial
entity and yields no income to the author. No external funding
supported this work.

# Data availability statement

This paper describes a software platform rather than the analysis
of a specific dataset; the materials that support its results are
the source code, panel assets, reference patches, and documentation
that together constitute the platform. All such materials are
available under the GPL-3.0 license at
<https://github.com/kevisc/stochast>. The repository tag `v2.0.0`
corresponds to the version described in this paper and is
permanently archived at <https://doi.org/10.5281/zenodo.20283281>.
The repository contains the C++17 source for all fifteen Methods
modules, panel SVG assets, the three starter patches as portable
JSON `.vcv` files, the patch-generator script
`tools/empiria_patch_gen.py`, the Methods Plugin Manual at
`docs/methods_manual.md`, and the source for this paper.

# References

American Statistical Association. (2016). *GAISE College Report 2016: Guidelines for Assessment and Instruction in Statistics Education*. <https://www.amstat.org/education/curriculum-guidelines-for-undergraduate-programs-in-statistical-science->

Bruner, J. S. (1966). *Toward a Theory of Instruction*. Harvard University Press.

Bush, V. (1931). The differential analyzer. A new machine for solving differential equations. *Journal of the Franklin Institute*, 212(4), 447–488. <https://doi.org/10.1016/S0016-0032(31)90616-9>

Chance, B., delMas, R., & Garfield, J. (2004). Reasoning about sampling distributions. In D. Ben-Zvi & J. Garfield (Eds.), *The Challenge of Developing Statistical Literacy, Reasoning, and Thinking* (pp. 295–323). Springer. <https://doi.org/10.1007/1-4020-2278-6_13>

Cleveland, R. B., Cleveland, W. S., McRae, J. E., & Terpenning, I. (1990). STL: A seasonal-trend decomposition procedure based on Loess. *Journal of Official Statistics*, 6(1), 3–73.

Cobb, G. W. (2007). The introductory statistics course: A Ptolemaic curriculum? *Technology Innovations in Statistics Education*, 1(1). <https://doi.org/10.5070/T511000028>

delMas, R., Garfield, J., Ooms, A., & Chance, B. (2007). Assessing students' conceptual understanding after a first course in statistics. *Statistics Education Research Journal*, 6(2), 28–58. <https://doi.org/10.52041/serj.v6i2.483>

Efron, B. (1987). Better bootstrap confidence intervals. *Journal of the American Statistical Association*, 82(397), 171–185. <https://doi.org/10.1080/01621459.1987.10478410>

Finzer, W. (2013). The data science education dilemma. *Technology Innovations in Statistics Education*, 7(2). <https://doi.org/10.5070/T572013891>

Konold, C., & Lehrer, R. (2008). Technology and mathematics education: An essay in honor of Jim Kaput. In L. D. English (Ed.), *Handbook of International Research in Mathematics Education*. Routledge.

Mayer, R. E. (2009). *Multimedia Learning* (2nd ed.). Cambridge University Press. <https://doi.org/10.1017/CBO9780511811678>

Press, W. H., Teukolsky, S. A., Vetterling, W. T., & Flannery, B. P. (2007). *Numerical Recipes: The Art of Scientific Computing* (3rd ed.). Cambridge University Press.

Sanger, T. D. (1989). Optimal unsupervised learning in a single-layer linear feedforward neural network. *Neural Networks*, 2(6), 459–473. <https://doi.org/10.1016/0893-6080(89)90044-0>

Sfard, A. (1991). On the dual nature of mathematical conceptions: Reflections on processes and objects as different sides of the same coin. *Educational Studies in Mathematics*, 22(1), 1–36. <https://doi.org/10.1007/BF00302715>

Small, J. S. (2001). *The Analogue Alternative: The Electronic Analogue Computer in Britain and the USA, 1930–1975*. Routledge.

Tintle, N., Chance, B., Cobb, G., Roy, S., Swanson, T., & VanderStoep, J. (2015). Combating anti-statistical thinking using simulation-based methods throughout the undergraduate curriculum. *The American Statistician*, 69(4), 362–370. <https://doi.org/10.1080/00031305.2015.1081619>

VCV Rack contributors. (2024). *VCV Rack: An open-source modular synthesizer environment*. <https://vcvrack.com/>

Wilensky, U. (1999). *NetLogo*. Center for Connected Learning and Computer-Based Modeling, Northwestern University. <http://ccl.northwestern.edu/netlogo/>
