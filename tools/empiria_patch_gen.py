#!/usr/bin/env python3
"""Programmatic patch generator for the Empiria suite.

A .vcv file is a tar archive containing a single patch.json (plus optional
per-module state subdirectories under modules/). This script builds patch.json
directly from a high-level description (list of modules, list of cables, optional
per-module param overrides) and writes the .vcv file to disk.

The intended use is to seed students with a starting patch — same modules, same
positions, deterministic seed — so they can immediately compare results.

Run with no arguments to generate the three bundled starter patches under
../patches/. Use as a library by importing build_patch() / write_patch().
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

# 1 HP = 15 px in VCV Rack; pos is given in (column, row) grid coordinates,
# where column 0 is the leftmost slot of the row and each unit is 1 HP wide.
HP = 1


def _module(
    mid: int,
    plugin: str,
    model: str,
    col: int,
    row: int = 0,
    params: dict[int, float] | None = None,
    data: dict[str, Any] | None = None,
) -> dict[str, Any]:
    m: dict[str, Any] = {
        "id": mid,
        "plugin": plugin,
        "model": model,
        "version": "2.0.0",
        "params": [{"value": v, "id": k} for k, v in sorted((params or {}).items())],
        "pos": [col * HP, row],
    }
    if data is not None:
        m["data"] = data
    return m


def _cable(cid: int, src_id: int, src_out: int, dst_id: int, dst_in: int,
           color: str = "#3695ef") -> dict[str, Any]:
    return {
        "id": cid,
        "outputModuleId": src_id,
        "outputId": src_out,
        "inputModuleId": dst_id,
        "inputId": dst_in,
        "color": color,
    }


def build_patch(modules: list[dict], cables: list[dict],
                version: str = "2.6.0") -> dict[str, Any]:
    # Wire leftModuleId / rightModuleId for any modules placed contiguously in
    # the same row. VCV Rack uses these to render the expander connection and
    # to let consumer modules find their producer.
    by_pos: dict[tuple[int, int], dict] = {tuple(m["pos"]): m for m in modules}
    for m in modules:
        col, row = m["pos"]
        left  = by_pos.get((col - 20, row))  # 20HP module width
        right = by_pos.get((col + 20, row))
        if left is not None:
            m["leftModuleId"] = left["id"]
        if right is not None:
            m["rightModuleId"] = right["id"]
    return {
        "version": version,
        "zoom": 1.0,
        "gridOffset": [0.0, 0.0],
        "modules": modules,
        "cables": cables,
    }


def write_patch(patch: dict, out_path: Path) -> None:
    """Write a patch dict as a VCV Rack .vcv file.

    VCV Rack 2 stores patches as plain JSON in the .vcv file (any per-module
    state is serialised inline into each module's "data" field, not into a
    separate file). Earlier versions of this script wrote a tar archive
    containing patch.json, which Rack rejects with a JSON parse error since
    it tries to parse the raw bytes directly as JSON.
    """
    raw = json.dumps(patch, indent=2).encode("utf-8")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(raw)


# ---------------------------------------------------------------------------
# Three starter patches: one per academic workflow described in paper.md.
# ---------------------------------------------------------------------------
def starter_seeded_t_test() -> dict:
    """Workflow B: Seed → Sample → Frame → Test, all reproducible.

    A two-sample Welch's t-test on two independent Normal streams. The seed is
    fixed so every student arrives at the same t and p the first time they run
    the patch, then varies as soon as the user nudges the seed knob.
    """
    P_METHODS = "SHLabs-Methods"
    # IDs are arbitrary but must be unique within the patch.
    SEED, S1, S2, FRAME, TEST = 1, 2, 3, 4, 5
    modules = [
        _module(SEED,  P_METHODS, "Seed",   col=0,   params={0: 42.0 / 999.0}),
        _module(S1,    P_METHODS, "Sample", col=20),
        _module(S2,    P_METHODS, "Sample", col=40),
        _module(FRAME, P_METHODS, "Frame",  col=60),
        _module(TEST,  P_METHODS, "Test",   col=80),
    ]
    # Wire Seed → Sample.RESET, Sample → Frame & Test.
    # Output / input indices follow each module's enum order in src/<Module>.cpp.
    # Conservative: use port 0 from Seed (SEED_CV_OUTPUT) into RESET inputs.
    cables = [
        _cable(101, SEED, 1, S1,    1, "#ffd13a"),   # Seed TRIG → S1 RESET
        _cable(102, SEED, 1, S2,    1, "#ffd13a"),   # Seed TRIG → S2 RESET
        _cable(103, S1,   0, FRAME, 2, "#3695ef"),   # S1 → Frame SIG
        _cable(104, S1,   0, TEST,  2, "#3695ef"),   # S1 → Test SIG
        _cable(105, S2,   0, TEST,  3, "#f5784a"),   # S2 → Test SIG2
    ]
    return build_patch(modules, cables)


def starter_schelling_segregation() -> dict:
    """Workflow A (visceral): Schelling + Seed + Frame.

    The classic schoolroom demo. Students watch sharp segregation emerge from
    a mild tolerance preference. Frame captures the running segregation index.
    """
    P_METHODS = "SHLabs-Methods"
    P_SPACE   = "SHLabs-Space"
    SEED, SCH, FRAME = 1, 2, 3
    modules = [
        _module(SEED,  P_METHODS, "Seed",      col=0,  params={0: 7.0 / 999.0}),
        _module(SCH,   P_SPACE,   "Schelling", col=20),
        _module(FRAME, P_METHODS, "Frame",     col=40),
    ]
    cables = [
        _cable(101, SEED, 1, SCH,   1, "#ffd13a"),   # Seed TRIG → Schelling RESET
        _cable(102, SCH,  0, FRAME, 2, "#3695ef"),   # SEG → Frame SIG
    ]
    return build_patch(modules, cables)


def starter_coin_flip_lln() -> dict:
    """Workflow C (closed-form): Sample (near-Bernoulli Beta) → Frame (GROWING).

    The empirical mean must converge to the theoretical p = 0.5 as n grows.
    The Law of Large Numbers, made visible. Boot supplies a confidence band so
    students can see the convergence is *probabilistic*, not monotone.
    """
    P_METHODS = "SHLabs-Methods"
    SEED, S, FRAME, BOOT, GAUGE = 1, 2, 3, 4, 5
    modules = [
        _module(SEED,  P_METHODS, "Seed",   col=0,   params={0: 1.0 / 999.0}),
        # Sample params: distribution = Beta (3), P1 = 0.10, P2 = 0.10
        # (near-Bernoulli — Beta α≈0.05, β≈0.05 spikes at 0 and 1).
        # Indices follow Sample::ParamIds enum in src/Sample.cpp.
        _module(S,     P_METHODS, "Sample", col=20, params={3: 3.0, 4: 0.10, 5: 0.10}),
        # Frame in GROWING mode (mode index 2).
        _module(FRAME, P_METHODS, "Frame",  col=40, params={0: 2.0}),
        _module(BOOT,  P_METHODS, "Boot",   col=60),
        # Gauge in Probability preset. The preset index lives in the module's
        # `data` JSON; the A / B / DECIMALS knobs (params 0, 1, 2) hold the
        # actual mapping coefficients — set them to match the preset so the
        # patch opens with the readout already configured.
        # PRESET_PROBABILITY = 4 in src/Gauge.cpp (A = 0.1, B = 0).
        _module(GAUGE, P_METHODS, "Gauge",  col=80,
                params={0: 0.1, 1: 0.0, 2: 3.0},
                data={"preset": 4}),
    ]
    cables = [
        _cable(101, SEED, 1, S,     1, "#ffd13a"),   # Seed TRIG → Sample RESET
        _cable(102, S,    0, FRAME, 2, "#3695ef"),   # Sample → Frame SIG
        _cable(103, S,    0, BOOT,  2, "#3695ef"),   # Sample → Boot SIG
        _cable(104, FRAME,0, GAUGE, 0, "#80bf2e"),   # Frame MEAN → Gauge CV
    ]
    return build_patch(modules, cables)


# ---------------------------------------------------------------------------
# Screenshot patches — minimal, self-contained patches designed to populate
# one anchor module's visualisation cleanly. Open in VCV Rack, let it run for
# ~30 seconds so the buffers fill, then capture the module panel as a figure
# for the manuscript.
#
# Each patch uses Fundamental's LFO as the master clock so it stays compatible
# with a bare-bones Rack install (only the SHLabs-Methods plugin + the
# Fundamental plugin bundled with VCV Rack 2 are required).
#
# LFO param indices (Fundamental 2.x): 0=freq, 1=pw, 2=FM amount, 3=PW amount,
# 4=offset 0/1, 5=invert 0/1, 6=slow 0/1.
# LFO output indices: 0=sin, 1=tri, 2=saw, 3=sqr.
# ---------------------------------------------------------------------------
P_METHODS = "SHLabs-Methods"
P_FUND    = "Fundamental"


def screenshot_sample() -> dict:
    """Sample — histogram with theoretical PDF overlay.

    LFO drives Sample's CLOCK at ~16 Hz. Sample is configured as Normal
    distribution with μ ≈ 0 and σ ≈ 1 (P1 = 0.5, P2 = 0.5). After ~20 s
    the empirical histogram visibly converges to the bell-curve overlay.
    """
    LFO, SEED, S = 1, 2, 3
    modules = [
        # LFO: freq knob = 4.0 → fast clock (~16 Hz)
        _module(LFO,  P_FUND,    "LFO",    col=0,  params={0: 4.0}),
        _module(SEED, P_METHODS, "Seed",   col=8,  params={0: 42.0 / 999.0}),
        # Sample: DIST = NORMAL (0), P1 = 0.5 (μ at midpoint), P2 = 0.5 (σ moderate)
        _module(S,    P_METHODS, "Sample", col=14, params={0: 0.0, 1: 0.5, 2: 0.5}),
    ]
    cables = [
        _cable(101, LFO,  3, S,    0, "#ffd13a"),  # LFO SQR → Sample CLOCK
        _cable(102, SEED, 1, S,    1, "#ff8d00"),  # Seed TRIG → Sample RESET
    ]
    return build_patch(modules, cables)


def screenshot_frame() -> dict:
    """Frame — sampling distribution with shrinking CI band (LLN made visible).

    LFO at ~16 Hz drives both Sample and Frame. Sample is Normal(μ ≈ 0,
    σ ≈ 1). Frame is in GROWING mode (mode index 2) so the buffer
    accumulates without bound; the standard-error band visibly contracts
    as 1/√n while the histogram fills out.
    """
    LFO, SEED, S, FRAME = 1, 2, 3, 4
    modules = [
        _module(LFO,   P_FUND,    "LFO",    col=0,  params={0: 4.0}),
        _module(SEED,  P_METHODS, "Seed",   col=8,  params={0: 42.0 / 999.0}),
        _module(S,     P_METHODS, "Sample", col=14, params={0: 0.0, 1: 0.5, 2: 0.5}),
        # Frame: MODE = GROWING (2), default N, default CI level
        _module(FRAME, P_METHODS, "Frame",  col=34, params={0: 2.0}),
    ]
    cables = [
        _cable(101, LFO,  3, S,     0, "#ffd13a"),  # LFO SQR → Sample CLOCK
        _cable(102, LFO,  3, FRAME, 0, "#ffd13a"),  # LFO SQR → Frame CLOCK
        _cable(103, SEED, 1, S,     1, "#ff8d00"),  # Seed TRIG → Sample RESET
        _cable(104, SEED, 1, FRAME, 1, "#ff8d00"),  # Seed TRIG → Frame RESET
        _cable(105, S,    0, FRAME, 2, "#3695ef"),  # Sample → Frame SIG
    ]
    return build_patch(modules, cables)


def screenshot_regress() -> dict:
    """Regress — scatter with fitted line and trumpet CI band.

    Two parallel Sample modules feed Regress's X and Y inputs. With
    independent draws the scatter is amorphous and β ≈ 0 — illustrating
    a null relationship plus the trumpet-shaped CI band (the prediction
    interval widens at the extremes). For a positive-β demo, the user
    can right-click → "U-shaped opinion" on one Sample and re-seed.
    """
    LFO, SEED, SX, SY, REG = 1, 2, 3, 4, 5
    modules = [
        _module(LFO,  P_FUND,    "LFO",     col=0,  params={0: 4.0}),
        _module(SEED, P_METHODS, "Seed",    col=8,  params={0: 42.0 / 999.0}),
        # SX: Uniform (DIST=1), broad range
        _module(SX,   P_METHODS, "Sample",  col=14, params={0: 1.0, 1: 0.5, 2: 0.5}),
        # SY: Normal small variance
        _module(SY,   P_METHODS, "Sample",  col=34, params={0: 0.0, 1: 0.5, 2: 0.3}),
        # Regress: RUNNING mode (1), default N
        _module(REG,  P_METHODS, "Regress", col=54, params={0: 1.0}),
    ]
    cables = [
        _cable(101, LFO,  3, SX,  0, "#ffd13a"),
        _cable(102, LFO,  3, SY,  0, "#ffd13a"),
        _cable(103, LFO,  3, REG, 0, "#ffd13a"),
        _cable(104, SEED, 1, SX,  1, "#ff8d00"),
        _cable(105, SEED, 1, SY,  1, "#ff8d00"),
        _cable(106, SEED, 1, REG, 1, "#ff8d00"),
        _cable(107, SX,   0, REG, 2, "#3695ef"),  # SX → Regress X
        _cable(108, SY,   0, REG, 3, "#f5784a"),  # SY → Regress Y
    ]
    return build_patch(modules, cables)


def screenshot_test() -> dict:
    """Test — null t-distribution with shaded rejection regions and observed t.

    Two-sample setup: SX is Normal centered at P1 ≈ 0.5 (μ ≈ 0), SY is
    Normal centered at P1 ≈ 0.65 (μ slightly positive). With enough
    samples this becomes a "marginally significant" t-test — the observed
    t lands near the boundary of the 0.05 rejection region, useful for
    discussing the meaning of *p* and the role of effect size.
    """
    LFO, SEED, SX, SY, FRAME, TEST = 1, 2, 3, 4, 5, 6
    modules = [
        _module(LFO,   P_FUND,    "LFO",    col=0,  params={0: 4.0}),
        _module(SEED,  P_METHODS, "Seed",   col=8,  params={0: 42.0 / 999.0}),
        _module(SX,    P_METHODS, "Sample", col=14, params={0: 0.0, 1: 0.50, 2: 0.5}),
        _module(SY,    P_METHODS, "Sample", col=34, params={0: 0.0, 1: 0.65, 2: 0.5}),
        _module(FRAME, P_METHODS, "Frame",  col=54, params={0: 1.0}),
        # Test: MODE = TWO_SAMPLE (1), default N, ALPHA = 0.05 (default)
        _module(TEST,  P_METHODS, "Test",   col=74, params={0: 1.0}),
    ]
    cables = [
        _cable(101, LFO,  3, SX,    0, "#ffd13a"),
        _cable(102, LFO,  3, SY,    0, "#ffd13a"),
        _cable(103, LFO,  3, FRAME, 0, "#ffd13a"),
        _cable(104, LFO,  3, TEST,  0, "#ffd13a"),
        _cable(105, SEED, 1, SX,    1, "#ff8d00"),
        _cable(106, SEED, 1, SY,    1, "#ff8d00"),
        _cable(107, SX,   0, FRAME, 2, "#3695ef"),
        _cable(108, SX,   0, TEST,  2, "#3695ef"),  # SX → Test SIG
        _cable(109, SY,   0, TEST,  3, "#f5784a"),  # SY → Test SIG2
    ]
    return build_patch(modules, cables)


def screenshot_boot() -> dict:
    """Boot — bootstrap distribution with visible BCa diagnostics.

    Sample is a moderately skewed Beta to give the bootstrap distribution
    a visibly asymmetric shape, so the BCa z₀ and â corrections shift
    the interval away from the percentile baseline. After the Sample
    buffer fills, the bootstrap distribution histogram + point estimate
    + percentile CI bracket are visible on the panel.
    """
    LFO, SEED, S, BOOT = 1, 2, 3, 4
    modules = [
        _module(LFO,  P_FUND,    "LFO",    col=0,  params={0: 4.0}),
        _module(SEED, P_METHODS, "Seed",   col=8,  params={0: 42.0 / 999.0}),
        # Skewed Beta α≈1, β≈4 → right-skewed
        _module(S,    P_METHODS, "Sample", col=14, params={0: 3.0, 1: 0.42, 2: 0.74}),
        # Boot: MODE = SNAPSHOT (0), STAT = MEAN (0)
        _module(BOOT, P_METHODS, "Boot",   col=34, params={0: 0.0, 3: 0.0}),
    ]
    cables = [
        _cable(101, LFO,  3, S,    0, "#ffd13a"),
        _cable(102, LFO,  3, BOOT, 0, "#ffd13a"),
        _cable(103, SEED, 1, S,    1, "#ff8d00"),
        _cable(104, SEED, 1, BOOT, 1, "#ff8d00"),
        _cable(105, S,    0, BOOT, 2, "#3695ef"),  # Sample → Boot SIG
    ]
    return build_patch(modules, cables)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
STARTERS = {
    "seeded_t_test":         starter_seeded_t_test,
    "schelling_segregation": starter_schelling_segregation,
    "coin_flip_lln":         starter_coin_flip_lln,
    "screenshot_sample":     screenshot_sample,
    "screenshot_frame":      screenshot_frame,
    "screenshot_regress":    screenshot_regress,
    "screenshot_test":       screenshot_test,
    "screenshot_boot":       screenshot_boot,
}


def main(argv: list[str]) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--out", type=Path, default=None,
                   help="Output directory (default: ../patches/ relative to this script)")
    p.add_argument("--seed", type=int, default=None,
                   help="Per-student integer seed (0-999). Replaces the Seed knob value "
                        "in every patch. Use for handing out distinct-but-reproducible "
                        "copies to a class.")
    p.add_argument("--only", type=str, default=None,
                   help="Generate only this patch (default: all)")
    args = p.parse_args(argv)

    out_dir = args.out or Path(__file__).resolve().parent.parent / "patches"
    out_dir.mkdir(parents=True, exist_ok=True)

    names = [args.only] if args.only else list(STARTERS)
    for name in names:
        if name not in STARTERS:
            print(f"unknown patch: {name}", file=sys.stderr)
            return 2
        patch = STARTERS[name]()
        if args.seed is not None:
            for m in patch["modules"]:
                if m["model"] == "Seed" and m["params"]:
                    m["params"][0]["value"] = max(0, min(999, args.seed)) / 999.0
        path = out_dir / f"empiria_{name}.vcv"
        write_patch(patch, path)
        print(f"wrote {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
