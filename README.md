# Stochast

**A suite of VCV Rack 2 plugins that turn real statistical, social, and
stochastic processes into control voltage** — quirky, generative patch
material with genuinely correct math under the hood.

[![License: GPL v3](https://img.shields.io/badge/License-GPL_v3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![VCV Rack 2](https://img.shields.io/badge/VCV%20Rack-2.x-brightgreen.svg)](https://vcvrack.com)

Each Stochast module is a genuine statistical, agent-based, or dynamical
process — a bootstrap resampler, a Granovetter cascade, an SIR epidemic, a
reaction-diffusion field — exposed as knobs and control voltage. Patch a
sampling distribution as a modulation source, clock a segregation grid into
a gate sequencer, let an epidemic curve sweep a filter. The output is
generative and alive precisely because the process underneath behaves the
way the theory says it should — and the math is not faked for effect: every
random draw is Mersenne-Twister-seeded and every closed-form quantity uses
the standard numerical recipe (Lentz CF, BCa, Sanger, STL), so a patch is
reproducible and the numbers are correct on macOS, Windows, and Linux. It
just happens to be fun.

> **Looking to teach or learn statistics?** That's the job of the
> browser-based **[Empiria web app](https://shlabs.ch/empiria/)** —
> a separate, classroom-focused tool. Stochast is the music-side sibling:
> the same real math, repurposed as modular-synth instruments.

---

## Suite

| Plugin       | Focus                                        | Modules                                                                            |
|--------------|----------------------------------------------|------------------------------------------------------------------------------------|
| **Methods**  | Statistical workflow & analog computation    | Sample, Frame, Regress, Test, Boot, Lag, Code, Tab, Strata, Cohort, Factor, Seed, Tape, Gauge, Quantity |
| **Polis**    | Agent-based social models                    | Cascade, Discourse, Pareto, Dilemma, Diffusion, Network                            |
| **Epi**      | Network epidemiology                         | Outbreak                                                                           |
| **Space**    | Spatial dynamics & emergence                 | Life, Schelling, Turing                                                            |
| **Decisions**| Behavioural economics & cognition            | Prospect, Bandit, DDM                                                              |

The full suite is 28 modules across five plugins, released together
under GPL-3.0. Technical notes on the methods and the numerics live in
[`paper.md`](paper.md).

---

## Quick start

1. Install [VCV Rack 2](https://vcvrack.com/Rack) (Free edition is
   sufficient). Launch it once so it creates its plugin folder.
2. Download a release `.vcvplugin` archive from the
   [Releases page](https://github.com/shlabs-audio/stochast/releases),
   drop it into the plugin folder, and restart Rack. (An official
   VCV Library listing is coming — once it lands you'll be able to
   subscribe and auto-update from inside Rack.)
3. Open one of the starter patches in [`patches/`](patches/), press
   play, and explore.

For a guided tour, see the [Methods plugin manual](docs/methods_manual.md).

---

## Documentation

- **Module manual.** [docs/methods_manual.md](docs/methods_manual.md) —
  per-module reference: parameters, inputs, outputs, lights,
  visualizations, right-click options, implementation notes.
- **Technical notes.** [paper.md](paper.md) — the design and the
  mathematics behind the modules: the named numerical recipes, the
  seeding/reproducibility model, and the analog-computer lineage.
- **Use cases & patches.** [docs/use_cases.md](docs/use_cases.md) and the
  starter patches under [`patches/`](patches/).

---

## Building from source

```bash
# Set RACK_DIR to point at your unpacked Rack SDK
export RACK_DIR=$HOME/Rack-SDK

# Build any of the five plugins
cd Methods   && make -j4 && make install   # or Polis, Epi, Space, Decisions
```

The build produces a `.vcvplugin` archive in `dist/` and installs it
into Rack's user-plugins folder. There are no external dependencies
beyond the Rack SDK; all numerical algorithms are implemented inline
in the C++17 standard library.

---

## Reproducibility & data sharing

- Every random module wraps `std::mt19937` (Mersenne Twister, period
  2¹⁹⁹³⁷ − 1) with an explicit, user-visible integer seed via the
  **Seed** module. The same seed produces byte-identical results on
  any operating system.
- Patches save as plain JSON (the `.vcv` file format). Three starter
  patches under [`patches/`](patches/) reproduce the worked examples
  in the paper.
- [`tools/empiria_patch_gen.py`](tools/empiria_patch_gen.py) is a
  small Python library + CLI that regenerates any starter patch with
  an arbitrary student-specific seed — useful for handing out
  reproducibly-distinct copies to a class.
- [`Tape`](docs/methods_manual.md#tape)'s right-click *Export buffer
  to CSV…* writes a buffered CV stream as plain CSV for downstream
  analysis in R or Python.

---

## Citing Stochast

Stochast is free for music, research, or teaching. If you'd like to
credit it, cite the software:

```bibtex
@software{stochast,
  author = {SHLabs},
  title  = {Stochast: Statistical and Social-Science Modules for VCV Rack},
  year   = {2026},
  url    = {https://github.com/shlabs-audio/stochast}
}
```

For teaching statistics specifically, see the
[Empiria web app](https://shlabs.ch/empiria/).

---

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).

The plugin panels (SVG files under each plugin's `res/` directory)
are also GPL-3.0-or-later. The fonts ship with VCV Rack itself and
follow VCV Rack's licensing.

---

## Maintainer

**SHLabs** — an open-source audio imprint.
Web: <https://shlabs.ch>
Contact: <hello@shlabs.ch>

---

## Acknowledgements

Thanks to the VCV Rack developer community for the modular plugin SDK
and example modules, and to the long open-source tradition of
computational social science on which Stochast draws.
