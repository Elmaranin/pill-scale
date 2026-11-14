#!/usr/bin/env python3
"""Box lifts in a recording: "before", zero, "after" and the difference per lift.

    python tools/lifts.py data/20261021-191116_hx711.csv
    python tools/lifts.py data/....csv --plot docs/img/lifts_a.png

For every lift (box standing → platform empty → box standing):

    d = (R2 − Z2) − (R1 − Z1)

R1 — box before the lift, Z1 — zero right after removal, Z2 — zero right before
the return, R2 — box after the return. Zero drift between Z1 and Z2 cancels out
(docs/experiments.md, "Справка · Дрейф не ограничивает время без коробки").
The plain difference R2 − R1, without the zero correction, is printed for comparison.

Windows are taken only from calm stretches (small 1 s spread) so the hand and
bumps never get into "before" and "after".

Figure text is Russian: the figures go into the Russian docs.
"""
import argparse
import sys

import numpy as np
import pandas as pd

SPS_WINDOW = 12      # ~1 s at 11.7 SPS: window for "is it calm"
CALM_SD = 0.05       # g: 1 s spread below this — the stretch is calm
MIN_SEGMENT_S = 1.5  # shorter — a bump or a spike, not a state change
WIN_S = 3.0          # length of the R1, Z1, Z2, R2 windows
GAP_S = 1.0          # distance from the transition: the hand is still near
SETTLE_S = 3.0       # after the box returns, wait for it to settle
OVERLOAD_G = 500.0   # sensor limit


def load(path):
    df = pd.read_csv(path, comment="#")
    df["t"] = (df.t_ms - df.t_ms.iloc[0]) / 1000.0
    df["calm"] = df.g.rolling(SPS_WINDOW, center=True, min_periods=3).std() < CALM_SD
    return df


def levels(g):
    """Zero and box weight from the distribution of readings; threshold halfway."""
    zero = g[g < g.quantile(0.2) + 1.0].median()
    box = g[g > zero + 2.0].median()
    if not np.isfinite(box) or box - zero < 2.0:
        sys.exit("box not found: no stretches noticeably heavier than zero in the recording")
    return zero, box, zero + 0.5 * (box - zero)


def segments(df, thr):
    """Segments "box standing" / "empty". Short ones (bumps) merge into neighbours."""
    smooth = df.g.rolling(7, center=True, min_periods=1).median()  # drop single spikes
    state = (smooth > thr).astype(int)
    seg = []  # [loaded, i0, i1) by index
    start = 0
    for i in range(1, len(state) + 1):
        if i == len(state) or state.iat[i] != state.iat[start]:
            seg.append([bool(state.iat[start]), start, i])
            start = i
    # merge short segments with their neighbours
    t = df.t.values
    merged = []
    for s in seg:
        if merged and (t[s[2] - 1] - t[s[1]] < MIN_SEGMENT_S):
            merged[-1][2] = s[2]
        elif merged and merged[-1][0] == s[0]:
            merged[-1][2] = s[2]
        else:
            merged.append(s)
    return merged


def window(df, t0, t1):
    """Mean of the calm samples in [t0, t1). None — if too few are calm."""
    w = df[(df.t >= t0) & (df.t < t1)]
    c = w[w.calm]
    if len(w) == 0 or len(c) < 0.5 * len(w):
        return None
    return c.g.mean()


def analyse(df, segs):
    rows = []
    for a, e, b in zip(segs, segs[1:], segs[2:]):
        if not (a[0] and not e[0] and b[0]):
            continue
        t = df.t.values
        a_end, e_start, e_end, b_start, b_end = t[a[2] - 1], t[e[1]], t[e[2] - 1], t[b[1]], t[b[2] - 1]
        r1 = window(df, a_end - GAP_S - WIN_S, a_end - GAP_S)
        z1 = window(df, e_start + GAP_S, min(e_start + GAP_S + WIN_S, e_end - GAP_S))
        z2 = window(df, max(e_end - GAP_S - WIN_S, e_start + GAP_S), e_end - GAP_S)
        r2 = window(df, b_start + SETTLE_S, min(b_start + SETTLE_S + WIN_S, b_end))
        ok = None not in (r1, z1, z2, r2)
        rows.append({
            "t_min": round(a_end / 60, 2),
            "empty_s": round(e_end - e_start, 1),
            "R1": r1, "Z1": z1, "Z2": z2, "R2": r2,
            "naive": (r2 - r1) if ok else np.nan,
            "d": ((r2 - z2) - (r1 - z1)) if ok else np.nan,
            "zero_drift": (z2 - z1) if ok else np.nan,
        })
    return pd.DataFrame(rows)


def plot(df, res, thr, path):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    blue, orange, text2, grid, surface = "#2a78d6", "#eb6834", "#52514e", "#e4e3df", "#fcfcfb"
    plt.rcParams.update({"figure.facecolor": surface, "axes.facecolor": surface,
                         "axes.edgecolor": grid, "axes.grid": True, "grid.color": grid,
                         "axes.spines.top": False, "axes.spines.right": False,
                         "axes.titlelocation": "left", "axes.titleweight": "bold",
                         "xtick.color": text2, "ytick.color": text2, "axes.labelcolor": text2,
                         "legend.frameon": False})
    ok = res.dropna(subset=["d"]).reset_index(drop=True)
    n = np.arange(1, len(ok) + 1)
    fig, ax = plt.subplots(figsize=(9, 5))
    ax.axhspan(-0.035, 0.035, color=grid, zorder=0)
    ax.text(len(ok) + 0.6, 0.0, "порог\n±0.035 г", va="center", color=text2, fontsize=9)
    ax.plot(n, ok.naive, "o", ms=6, color=orange, label="просто «после» − «до» (R2 − R1)",
            markeredgecolor=surface, markeredgewidth=1.5)
    ax.plot(n, ok.d, "o", ms=7, color=blue, label="с двумя нулями: (R2 − Z2) − (R1 − Z1)",
            markeredgecolor=surface, markeredgewidth=1.5)
    ax.axhline(0, color=text2, lw=0.6)
    ax.set_xlabel("номер снятия")
    ax.set_ylabel("«после» − «до», г")
    ax.set_title(f"Снятия коробки: {len(ok)} шт., sd = {ok.d.std() * 1000:.0f} мг")
    ax.set_xticks(n)
    ax.margins(y=0.12)
    ax.legend(loc="upper center", bbox_to_anchor=(0.5, -0.16), ncol=2)
    ax.set_xlim(0.3, len(ok) + 1.6)
    fig.tight_layout()
    fig.savefig(path, dpi=130)


def run(csv, png):
    """For tools/figures.py: compute the lifts and save the figure."""
    df = load(csv)
    _, _, thr = levels(df.g)
    res = analyse(df, segments(df, thr))
    plot(df, res, thr, png)
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv")
    ap.add_argument("--plot", help="save the differences plot as PNG")
    args = ap.parse_args()

    df = load(args.csv)
    zero, box, thr = levels(df.g)
    segs = segments(df, thr)
    res = analyse(df, segs)

    print(f"{args.csv}: {df.t.iloc[-1] / 60:.1f} min, zero ≈ {zero:.2f} g, box ≈ {box - zero:.2f} g")
    over = df[df.g > OVERLOAD_G]
    if len(over):
        print(f"!! overload: {len(over)} samples above {OVERLOAD_G:.0f} g, max {df.g.max():.0f} g "
              f"at {over.t.iloc[0] / 60:.1f} min — the sensor is rated for 500 g")
    if res.empty:
        sys.exit("no lifts found")

    pd.set_option("display.width", 120)
    show = res.copy()
    for c in ("R1", "Z1", "Z2", "R2", "naive", "d", "zero_drift"):
        show[c] = show[c].map(lambda v: "—" if pd.isna(v) else f"{v:+.3f}" if c in ("naive", "d", "zero_drift") else f"{v:.3f}")
    show.index = range(1, len(show) + 1)
    print(show.to_string())

    ok = res.dropna(subset=["d"])
    print(f"\nlifts: {len(res)}, usable: {len(ok)}")
    for col, name in (("d", "two zeros"), ("naive", "plain R2 − R1")):
        v = ok[col]
        print(f"  {name:15s}: mean {v.mean() * 1000:+6.1f} mg, sd {v.std() * 1000:5.1f} mg, "
              f"from {v.min() * 1000:+6.1f} to {v.max() * 1000:+6.1f} mg")
    if args.plot:
        plot(df, res, thr, args.plot)
        print(f"figure: {args.plot}")



if __name__ == "__main__":
    main()
