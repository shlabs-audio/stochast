# Design note: on-panel annotation overlay (deferred)

Carried forward from the 2026-05-15 UX review. Not yet implemented.

## Goal

When demoing Empiria in a classroom, instructors often want to point
at a module's element ("this knob controls the trend cutoff", "the
shaded band is the 95 % CI") without having to switch slides or
narrate over screen reading. An *annotation overlay* is a per-module
toggle that draws short labelled arrows on top of the panel,
pointing at the elements that matter for the current pedagogical
beat. The overlay is intentionally noisy — it's a teaching aid, not
a permanent label — so it must be one click away both to enable
and to dismiss.

## What it should look like

For each annotated module, the overlay is a small `Widget` parented
to the `ModuleWidget`, rendered on top of the panel through the
existing `drawLayer` pass. It contains a list of annotation entries,
each of which is roughly:

```
struct Annotation {
    Vec target;       // the point on the panel being labelled
    Vec callout;      // where the label box anchors
    std::string text; // ~30–50 chars; longer breaks the layout
};
```

The widget draws:
- A circle (or small filled disc) at `target`.
- A line from `target` to `callout`.
- A rounded rectangle behind the text at `callout`, with the same
  colour palette as `PanelLabels` (light grey 75 % opacity) so it
  feels of-a-piece with the existing typography.

## Toggle UX

A new entry in the module's right-click context menu:

```
[ ] Show annotations
```

Selecting it toggles a `bool showAnnotations` saved in the module's
JSON (via `dataToJson` / `dataFromJson`), so a patch shared with
annotations on opens with them visible. Implementation lives next
to `appendAboutMenu` in each plugin's `plugin.hpp`:

```cpp
inline void appendAnnotationsToggle(Menu* menu, bool* flag) {
    menu->addChild(createBoolPtrMenuItem("Show annotations", "", flag));
}
```

## Where the annotation list lives

Two reasonable patterns:

1. **Per-module hard-coded.** Each `XxxWidget` constructor builds
   the annotation list once, hands it to a single `AnnotationOverlay`
   widget, and adds the widget as a child. Pros: zero runtime cost
   when toggled off (visibility flag short-circuits the draw call);
   compile-time guarantees that every coordinate matches the panel.
   Cons: editing a coordinate requires recompiling the plugin.

2. **Loaded from a JSON sidecar.** Each plugin ships a
   `res/annotations/<Module>.json` with the list. The constructor
   reads it at module-creation time. Pros: annotations editable
   without a build. Cons: more moving parts; risk of the JSON and
   the panel drifting out of sync.

For the first cut, go with (1) — the annotations are a deliberate
pedagogical artifact, so they belong in code review alongside the
panel layout. If we end up with many community-contributed
annotation packs, switch to (2).

## Coordinate sourcing

Annotation targets should reuse the same constants already used for
panel layout — the `kCols = {45, 120, 195, 270}` column array in
`PanelLabels` and the y-coordinates from `docs/design_spec.md`.
Don't hard-code free coordinates that float independently of those
anchors, or the overlays will silently drift when the design spec
changes.

## What's NOT in scope

- **Live editing** of annotations in the patch ("annotate-on-the-fly"
  for ad-hoc teaching). Worthwhile, but a separate, larger feature
  with its own UI affordances (drag handles, text edit cursor, save
  format). Not blocking on this design.
- **Voice-over recording.** VCV Rack has audio rendering but
  syncing it to the overlay is its own project.
- **Per-student annotation sets.** Same shape as the patch
  generator — separate `tools/empiria_annotation_gen.py`. Out of
  scope for the first release.

## Pilot list — which modules first

Annotations have the highest pedagogical leverage on the modules
where the visualisation does the teaching. Start with these six,
in order:

1. **Frame** — point at the histogram, the vertical mean line, and
   the shaded CI band; one annotation each.
2. **Test** — point at the null distribution, the shaded rejection
   regions, the observed t marker, and the *p* readout.
3. **Boot** — point at the bootstrap histogram, the point estimate
   line, the CI bounds, and the BCa diagnostics.
4. **Regress** — point at the data scatter, the fitted line, the
   trumpet-shaped CI band, and the β / R² readouts.
5. **Lag** — point at one bar inside the Bartlett band and one
   outside, with the labels *"insignificant"* and *"significant"*.
6. **Schelling** — point at a mixed neighbourhood (early) and a
   segregated neighbourhood (late) to make the dynamic visible.

Beyond these six, the existing on-panel labels carry enough load
that the overlay would be redundant. Re-evaluate per-module once
the first six are live and we have classroom feedback on which
ones get the most use.
