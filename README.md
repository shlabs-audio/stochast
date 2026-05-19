# Empiria

**A modular-synthesizer platform for teaching statistics, data science,
and computational social science**, built as a suite of plugins for
[VCV Rack 2](https://vcvrack.com/Rack).

[![License: GPL v3](https://img.shields.io/badge/License-GPL_v3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![VCV Rack 2](https://img.shields.io/badge/VCV%20Rack-2.x-brightgreen.svg)](https://vcvrack.com)
[![ORCID](https://img.shields.io/badge/ORCID-0000--0001--9892--5869-A6CE39.svg)](https://orcid.org/0000-0001-9892-5869)

Empiria treats the modular synthesizer's grammar — knobs, patch cables,
control-voltage signals, clocked sampling, polyphony, live
visualizations — as a teaching surface for inferential reasoning. The
patch *is* the curriculum: the wires drawn between modules externalize
the data-generating process, the estimator, and the inferential
apparatus in a way that no static diagram or single-purpose applet can
match. The same patch is also a deterministic, citeable artifact: every
random draw is Mersenne-Twister-seeded, every closed-form quantity is
computed by the standard numerical recipe (Lentz CF, BCa, Sanger,
STL), and every patch is portable JSON that runs byte-identically on
macOS, Windows, and Linux.

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
under GPL-3.0. The **Methods** plugin is the subject of the companion
paper (`paper.md`); a separate companion paper for the agent-based
plugins is forthcoming.

---

## Quick start

1. Install [VCV Rack 2](https://vcvrack.com/Rack) (Free edition is
   sufficient). Launch it once so it creates its plugin folder.
2. Download a release `.vcvplugin` archive from the
   [Releases page](https://github.com/kevinschoenholzer/empiria/releases),
   drop it into the plugin folder, and restart Rack.
3. Open one of the starter patches in [`patches/`](patches/), press
   play, and explore.

For a guided tour, see the [Methods plugin manual](docs/methods_manual.md).

---

## Documentation

- **Companion paper.** [paper.md](paper.md) — *Empiria: A
  Modular-Synthesizer Platform for Simulation-Based Statistics
  Education*. Submission-ready LaTeX build in
  [`submission/jsdse_latex/`](submission/jsdse_latex/).
- **User manual.** [docs/methods_manual.md](docs/methods_manual.md) —
  per-module reference for every Methods module: parameters, inputs,
  outputs, lights, visualizations, right-click options, implementation
  notes. PDF: [docs/methods_manual.pdf](docs/methods_manual.pdf).
- **Compiled paper.** [submission/jsdse_latex/empiria.pdf](submission/jsdse_latex/empiria.pdf)
  — the JSDSE-ready PDF build of [paper.md](paper.md), produced from
  the LaTeX sources in [`submission/jsdse_latex/`](submission/jsdse_latex/)
  ([empiria.tex](submission/jsdse_latex/empiria.tex),
  [_body.tex](submission/jsdse_latex/_body.tex), and
  [paper.bib](paper.bib)).

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

## Citing Empiria

```
Schoenholzer, K. (2026). Empiria: A modular-synthesizer platform for
  simulation-based statistics education. *Journal of Statistics and
  Data Science Education* [under review].
```

BibTeX:

```bibtex
@article{empiria2026,
  author  = {Schoenholzer, Kevin},
  title   = {Empiria: A Modular-Synthesizer Platform for
             Simulation-Based Statistics Education},
  journal = {Journal of Statistics and Data Science Education},
  year    = {2026},
  note    = {under review}
}
```

The repository tag corresponding to the version described in the
paper is `v2.0.0`.

---

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).

The plugin panels (SVG files under each plugin's `res/` directory)
are also GPL-3.0-or-later. The fonts ship with VCV Rack itself and
follow VCV Rack's licensing.

---

## Author

**Kevin Schoenholzer**
Postdoctoral Researcher
Università della Svizzera italiana (USI), Lugano, Switzerland
ORCID: [0000-0001-9892-5869](https://orcid.org/0000-0001-9892-5869)
Web: <https://kevinschoenholzer.com>

Empiria is released through the author's personal open-source imprint
**SHLabs**.

---

## Acknowledgements

The author thanks the VCV Rack developer community for the modular
plugin SDK and example modules, and acknowledges the long open-source
tradition of computational social science on which Empiria draws.
