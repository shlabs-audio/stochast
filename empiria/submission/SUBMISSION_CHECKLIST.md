# JSDSE submission checklist

The *Journal of Statistics and Data Science Education* (JSDSE) is
published by Taylor & Francis. Submissions go through Taylor &
Francis's **Submission Portal**:

> <https://rp.tandfonline.com/submission/create?journalCode=UJSE>

Author instructions (verify current at time of submission):

> <https://www.tandfonline.com/journals/ujse20>

This checklist walks through every step in order. Items already
done in this repository are checked.

---

## ✅ Already done

### Manuscript

- [x] **Abstract ≤ 200 words** (194 words; unstructured)
- [x] **Keywords (3–6), none sharing any word with the title**
      (six: bootstrap resampling, agent-based modeling,
      computational social science, reproducible research,
      analog computing, probabilistic reasoning)
- [x] **No references inside the abstract**
- [x] **American spelling throughout** (sweep applied to all
      submission files — verified zero `-ise` / `-our` / `-re`
      stragglers)
- [x] **Section order:** Introduction → body → Conclusions →
      Acknowledgements → Disclosure Statement → Data Availability
      Statement → References
- [x] **Disclosure Statement section** (states no competing
      interests, no external funding)
- [x] **Data Availability Statement section** (points to the
      public GitHub repository; identifies which artefacts
      support the paper)
- [x] **Double-anonymous variant** ([empiria_anonymous.pdf](jsdse_latex/empiria_anonymous.pdf))
      with author name, affiliation, and identifying URL stripped
      from the body; a non-anonymous variant
      ([empiria.pdf](jsdse_latex/empiria.pdf)) carries the same
      content with author info on the title block

### Format

- [x] **LaTeX `article` class, 12 pt, double-spaced, 1" margins, ~6"
      text width** (uses the JSDSE-provided template; the master
      `empiria.tex` toggles anonymity via `\newcommand{\anon}{0|1}`)
- [x] **natbib + apalike.bst** for references (ASA/JSDSE style)
- [x] **Editable equations** (no equations rendered as images)
- [x] **Figures inline near first reference** (three live-Rack
      screenshots at the anchor-module paragraphs; three wiring
      diagrams at the worked-example sections)
- [x] **Tables inline as editable tables** (longtable; no images)
- [x] **Page width**, **margins**, and **spacing** verified
      against the JSDSE template

### Supplementary

- [x] **Methods plugin manual** ([docs/methods_manual.pdf](../docs/methods_manual.pdf))
      — 21 pages, JSDSE supplementary upload
- [x] **Three starter `.vcv` patches** ([patches/](../patches/))
- [x] **Patch generator script** ([tools/empiria_patch_gen.py](../tools/empiria_patch_gen.py))
- [x] **Plugin source code, panel SVGs, README**

### Reproducibility

- [x] Every random module is `std::mt19937`-seeded
- [x] Patches are portable JSON
- [x] CSV export from Tape produces deterministic output
- [x] Three worked-example patches reproduce the paper figures
      byte-identically on any operating system

---

## ⚠️ Outstanding — do before clicking submit

### Author identification

- [x] **ORCID** `0000-0001-9892-5869` recorded in
      [title_page.md](title_page.md), [paper.md](../paper.md), and
      the `\hypersetup{}` + author footnote of
      [jsdse_latex/empiria.tex](jsdse_latex/empiria.tex).
- [ ] Confirm the institutional affiliation in the `.tex` title
      block matches your current employer; update if anything has
      changed since 2026-05.
- [ ] (Optional) Add any social-media handles (Twitter / Mastodon
      / LinkedIn) you'd like printed in the published article on
      the title page.

### Repository

- [ ] **Publish the source on GitHub** at the URL recorded in the
      title page and the Data Availability Statement (currently
      `https://github.com/kevinschoenholzer/empiria`). If you
      publish under a different path, update **both** files
      before submission. The anonymized body uses the placeholder
      "URL WITHHELD FOR DOUBLE-ANONYMOUS REVIEW" — no change
      needed there.
- [ ] **Tag the release `v2.0.0`** on the source repository so
      the Data Availability Statement's "tag v2.0.0 corresponds
      to the version described in this paper" resolves.

### Submission Portal

- [ ] Suggested reviewers in
      [suggested_reviewers.md](suggested_reviewers.md) — verify
      each candidate's current email and confirm no recent
      collaborations with you (the file lists five candidates
      with rationale, and is flagged as advisory).
- [ ] CRediT roles: enter "Conceptualization, Software, Writing
      – original draft, Writing – review & editing,
      Visualization, Methodology, Resources" for yourself
      (sole author).

---

## Step-by-step Submission Portal workflow

### Step 1 — Manuscript type, title, abstract

| Field             | Value                                                                                          |
|-------------------|------------------------------------------------------------------------------------------------|
| Manuscript type   | **Article**                                                                                    |
| Title             | Empiria: A Modular-Synthesizer Platform for Simulation-Based Statistics Education              |
| Short title       | (T&F portal usually does not collect this)                                                     |
| Abstract          | Copy from [jsdse_latex/empiria.tex](jsdse_latex/empiria.tex) — between `\begin{abstract}` and `\end{abstract}` |

### Step 2 — Keywords

Enter six (see [highlights_keywords.md](highlights_keywords.md)):

1. bootstrap resampling
2. agent-based modeling
3. computational social science
4. reproducible research
5. analog computing
6. probabilistic reasoning

### Step 3 — Author details

Sole author. Enter as in [title_page.md](title_page.md):

- Kevin Schoenholzer, Postdoctoral Researcher, Università della
  Svizzera italiana (USI), Lugano, Switzerland.
- ORCID: *replace placeholder*
- Email: `kevinschonholzer@gmail.com`
- Corresponding author: yes

### Step 4 — CRediT taxonomy

Sole-author submission. Select all applicable roles:
**Conceptualization, Software, Writing — original draft, Writing
— review & editing, Visualization, Methodology, Resources, Data
curation**.

### Step 5 — Funding

"This research received no external funding."

### Step 6 — Disclosure / Conflict of interest

"The author reports there are no competing interests to declare."

### Step 7 — Data availability

"Data Availability Statement provided in the manuscript." Confirm
the repository URL and tag are reachable when reviewers click.

### Step 8 — File uploads

The Submission Portal asks for both anonymized and non-anonymous
PDFs, plus the LaTeX source as a zip. Upload in this order:

| Label                                      | File                                                                |
|--------------------------------------------|---------------------------------------------------------------------|
| Manuscript — with author details (PDF)     | [jsdse_latex/empiria.pdf](jsdse_latex/empiria.pdf)                  |
| Manuscript — anonymous (PDF)               | [jsdse_latex/empiria_anonymous.pdf](jsdse_latex/empiria_anonymous.pdf) |
| LaTeX source files (single ZIP)            | `submission/jsdse_latex_source.zip` (built below)                   |
| Cover letter (PDF)                         | [cover_letter.pdf](cover_letter.pdf)                                |
| Suggested reviewers (TXT)                  | [suggested_reviewers.md](suggested_reviewers.md)                    |
| Supplementary material — Methods manual    | [docs/methods_manual.pdf](../docs/methods_manual.pdf)               |
| Supplementary material — starter patches   | [patches/](../patches/) (zip the three `.vcv` files before upload)  |
| Supplementary material — patch generator   | [tools/empiria_patch_gen.py](../tools/empiria_patch_gen.py)         |

### Step 9 — Review & submit

The Submission Portal generates a combined preview PDF. Confirm:

- [ ] Anonymous PDF actually has no author identifiers anywhere
      in the body (the title page is NOT bundled into this file).
- [ ] Non-anonymous PDF has the title block with name +
      affiliation visible.
- [ ] Cover letter is addressed to the editor.
- [ ] All figures render correctly at 100 % zoom.
- [ ] References are in APA-like format with parenthetical
      citations (no `?` or unresolved keys).
- [ ] Page count is reasonable (~43 pages in the current build,
      double-spaced as required).

---

## After submission

- [ ] Note the manuscript ID assigned by the Submission Portal
      (e.g. `UJSE-2026-XXXX`).
- [ ] Expect first decision in 8–12 weeks.
- [ ] If revisions are requested, prepare a numbered
      point-by-point response file alongside the revised
      manuscript. Re-render both PDFs from the same `.tex` source
      to keep the change history clean.
- [ ] On acceptance, return signed publishing agreement within
      48 h of receiving proofs (per the JSDSE author guidelines).

---

## Files in this directory

```
submission/
├── SUBMISSION_CHECKLIST.md          # this file
├── cover_letter.md / .pdf           # to the JSDSE editor
├── title_page.md / .pdf             # author info, separate from manuscript body
├── highlights_keywords.md           # six keywords (none in title)
├── suggested_reviewers.md           # five candidate reviewers (advisory)
├── manuscript_anonymized.md         # legacy markdown source (kept for diffing)
├── manuscript_anonymized.pdf        # legacy pandoc/typst PDF
└── jsdse_latex/                     # ← JSDSE template build (canonical)
    ├── empiria.tex                  # master tex (anon = 1)
    ├── empiria_anonymous.tex        # anonymous variant (anon = 0)
    ├── empiria.pdf                  # non-anonymous PDF
    ├── empiria_anonymous.pdf        # anonymous PDF
    ├── _body.tex                    # body (with author info)
    ├── _body_anonymous.tex          # body (anonymized)
    ├── paper.bib                    # bibliography
    ├── apalike.bst                  # JSDSE-supplied bibliography style
    └── figures -> ../../figures     # symlink to figures/
```

`paper.md` at the repo root remains the canonical source. The
`.tex` files in `jsdse_latex/` are derived from it via pandoc +
post-processing; see the build commands in [BUILD.md](BUILD.md)
or rerun the conversion pipeline if `paper.md` changes.
