# Empiria 20HP design specification

This document is the canonical reference for the 20HP panel grammar used
across all Empiria modules. It exists so that every module — whether
already converted or still on the 14HP legacy layout — can be brought
into the same grid with a mechanical, low-risk procedure.

## Panel dimensions

```
Width:   300 px (20HP)
Height:  380 px (fixed by VCV Rack 3U panel height)
```

The choice of 20HP (versus the older 14HP / 210 px) is motivated by
the need for scientific-grade readability: 20HP gives the
visualization area ~65 % more pixels and lets us split the controls
into two well-spaced rows rather than cramming four knobs across a
single tight row. The width is a small enough increase that a typical
teaching rack can still hold the full Methods chain (Sample + Frame +
Regress + Test + Boot + Lag + Tab + Code = 8 × 20HP = 160HP, within
the standard VCV Rack window width).

## Vertical layout

```
y = 0      ┌──────────────────────────────┐
           │  TITLE STRIP  (rendered in   │   header
           │  C++ by ModuleTitle widget)  │   y = 0..36
y = 36     ├──────────────────────────────┤
y = 44     │  ┌────────────────────────┐  │
           │  │                        │  │
           │  │   VISUALIZATION        │  │   viz box
           │  │   280 × 190 px         │  │   (10, 44, 280, 190)
           │  │                        │  │
           │  └────────────────────────┘  │
y = 234    ├──────────────────────────────┤   divider y = 238
y = 246    │  K1 label   ... K4 label     │   row-1 labels
y = 258    │  ◯  K1   K2   K3   K4       │   knob row 1
y = 282    │  K5 label   ... K8 label     │   row-2 labels
y = 294    │  ◯  K5   K6   K7   K8       │   knob row 2 (optional)
y = 311    ├──────────────────────────────┤   divider
y = 319    │  IN label  →   I1 ... I4     │   input labels
y = 327    │  ▣  I1   I2   I3   I4        │   input jack row
y = 347    ├──────────────────────────────┤   divider
y = 351    │  OUT label →   O1 ... O4     │   output labels
y = 358    │  ▣  O1   O2   O3   O4        │   output jack row
y = 380    └──────────────────────────────┘
```

## Standard slot positions

Four columns of slots, equally spaced:

```
Column index    1     2     3     4
x (center)     45    120   195   270
```

Two rows of knobs (a single-row layout uses only row 1):

```
Row index    1     2
y (center)  258   294
```

Two rows of jacks:

```
Input  jacks: y = 327
Output jacks: y = 358
```

`IN` / `OUT` strip labels:

```
Left edge x = 10, baseline at the row's y (e.g. y = 327 for inputs).
```

## Screw positions

```
ScrewSilver at (15, 0), (270, 0), (15, 365), (270, 365)
```

## Numeric value readouts

The single biggest legibility improvement in the 20HP layout is the
addition of a **numeric readout box** under each knob, showing the
current parameter value in its native units. Implemented via NanoVG in
each module's `drawLayer`. Reads in a small monospace font from the
knob's `params[...]` at draw time. Format: 3–5 significant figures,
units suffix where appropriate. Position: centered under the knob,
roughly y = 276 for the row-1 readout strip and y = 312 for row-2.

This is what turns the suite from a "synth-shaped toy" into something
a researcher would actually drive — no more *guessing* whether α is
0.55 or 0.60.

## Visualization conventions

Inside the viz box (280 × 190 px after the 10 px outer padding):

- Top strip (y = 44..56, ~12 px) — module-specific running header
  (e.g. "boot of MEAN  n=128  B=500" on Boot). Left-aligned.
- Bottom strip (y = 218..234, ~16 px) — module-specific footer
  (e.g. CI bracket on Boot, "p = 0.012 **" on Tab). Left-aligned.
- Main plot area between the two strips. Use `nvgSave / nvgScissor /
  nvgRestore` to clip aggressively to the plot rectangle, so traces,
  histogram bars, and confidence bands never bleed outside.

## Per-knob tooltip text

Every `configParam`, `configInput`, `configOutput` must have a tooltip
string. Standard format: `"Display name (units or range hint)"`.
Example: `"Drift rate v (−2..2; 0 = random walk)"`.

## Color palette

```
panel-bg gradient:   #1c1f2a  →  #11131a   (top→bottom)
title-strip gradient: #23273a →  #161824
viz-bg:              #080a10
viz-border:          #2b2f42  (stroke 0.5)
section divider:     #2b2f42  (stroke 0.4)
primary cyan trace:  #6ec8e0   (rgb 110, 200, 224)
critical green:      #78dc8c   (rgb 120, 220, 140)
warning amber:       #f5c85a   (rgb 245, 200, 90)
error red:           #f57878   (rgb 245, 120, 120)
text muted:          #9aa0b4
text faint:          #6a7088
```

## Per-module conversion procedure

1. **SVG.** Rewrite the panel SVG to `width="300" height="380"
   viewBox="0 0 300 380"` and re-place every text label at the
   coordinates listed above. Keep label sizes the same (`font-size="8"`
   for knob labels, `"7.5"` for jack labels, `"7"` for IN/OUT strip
   labels).
2. **Widget code.** Replace every `Vec(x, y)` in the `ModuleWidget`
   subclass with the new (column, row) coordinates from the tables
   above. Replace the visualization box position/size with
   `Vec(10, 44)` and `Vec(280, 190)`.
3. **Title widget.** Update `addChild(new ModuleTitle("NAME", 210.f))`
   → `addChild(new ModuleTitle("NAME", 300.f))`.
4. **Screws.** Move from `(15,0), (180,0), (15,365), (180,365)` to
   `(15,0), (270,0), (15,365), (270,365)`.
5. **Numeric readouts.** Inside the visualization `drawLayer`, draw
   small numeric labels under each knob using the current
   `params[…].getValue()` and the same NanoVG font as the title.
6. **Scissor clipping.** Wrap the main plot drawing in
   `nvgSave / nvgScissor(vg, plotX, plotY, plotW, plotH) / nvgRestore`
   so traces and bands can never bleed outside the box.
7. **Build, install, and verify in Rack** that the panel renders, the
   knobs sit on their labels, and the visualization fills the box.

## Conversion status

| Plugin    | Module     | Converted | Notes |
|-----------|-----------|-----------|-------|
| Methods   | Frame     | ✓         |       |
| Methods   | Regress   | ✓         |       |
| Methods   | Test      | ✓         |       |
| Methods   | Sample    | ✓         |       |
| Methods   | Boot      | ✓         |       |
| Methods   | Lag       | ✓         |       |
| Methods   | Tab       | ✓         |       |
| Methods   | Code      | ✓         |       |
| Methods   | Cohort    | deferred  | Non-standard 12HP layout with custom scope; needs bespoke redesign |
| Methods   | Factor    | deferred  | Non-standard 18HP layout with two sub-widgets (XY + loadings); needs bespoke redesign |
| Methods   | Strata    | ✓         |       |
| Methods   | Seed      | ✓         |       |
| Methods   | Tape      | ✓         |       |
| Polis     | Network   | ✓         |       |
| Polis     | Cascade   | ✓         |       |
| Polis     | Discourse | ✓         |       |
| Polis     | Pareto    | ✓         |       |
| Polis     | Dilemma   | ✓         |       |
| Polis     | Diffusion | ✓         |       |
| Epi       | Outbreak  | ✓         |       |
| Space     | Life      | ✓         |       |
| Space     | Schelling | ✓         |       |
| Space     | Turing    | ✓         |       |
| Decisions | Prospect  | ✓         |       |
| Decisions | Bandit    | ✓         |       |
| Decisions | DDM       | ✓         |       |

**Coverage: 24 of 26 modules converted (92 %).** The two deferred modules
(Cohort, Factor) carry custom sub-widget layouts that don't fit the
standard 4-column grid cleanly; they require a one-off redesign that
respects their visualization structure rather than the mechanical
conversion used elsewhere. They retain their pre-existing 12HP and
18HP panels and remain fully functional.
