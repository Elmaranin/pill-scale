#!/usr/bin/env python3
"""Figures for the docs and the presentation: docs/data/*.csv → docs/img/*.png.

    python tools/figures.py

Re-run after every new recording in docs/data/. What the figures show and the
conclusions are in docs/experiments.md. Figure text is Russian on purpose:
the figures go into the Russian docs.
"""
import pathlib

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

ROOT = pathlib.Path(__file__).resolve().parent.parent
DATA = ROOT / "docs" / "data"
IMG = ROOT / "docs" / "img"

# Palette checked for colour-blind legibility (2 colours, light background)
SURFACE = "#fcfcfb"
TEXT = "#0b0b0b"
TEXT2 = "#52514e"
GRID = "#e4e3df"
BLUE = "#2a78d6"
ORANGE = "#eb6834"

plt.rcParams.update({
    "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
    "axes.edgecolor": GRID, "axes.labelcolor": TEXT2, "axes.titlecolor": TEXT,
    "axes.titlesize": 13, "axes.titleweight": "bold", "axes.titlelocation": "left",
    "xtick.color": TEXT2, "ytick.color": TEXT2, "axes.grid": True,
    "grid.color": GRID, "grid.linewidth": 0.8, "axes.spines.top": False,
    "axes.spines.right": False, "lines.linewidth": 2, "font.size": 11,
    "legend.frameon": False, "legend.labelcolor": TEXT,
})


def load(name):
    df = pd.read_csv(DATA / name, comment="#")
    df["min"] = (df.t_ms - df.t_ms.iloc[0]) / 60000.0
    return df


def scale_of(name):
    # scale (counts per gram) from the "# info: ..." line at the top of the CSV
    for line in open(DATA / name, encoding="utf-8"):
        if line.startswith("# info:"):
            return float(line.split("scale=")[1].split()[0])
    raise ValueError(f"{name}: no scale in the header")


def drift():
    """Zero drift: window open (cooling) and closed (warming)."""
    runs = [("2026-10-20_drift_window_open.csv", "окно открыто (+7 °C)", BLUE),
            ("2026-10-20_drift_window_closed.csv", "окно закрыли", ORANGE)]
    fig, ax = plt.subplots(figsize=(10, 5.2))
    for name, label, color in runs:
        if not (DATA / name).exists():
            continue
        df = load(name)
        g = (df.raw - df.raw.iloc[:120].mean()) / scale_of(name)  # relative to the start
        # 10 s means show the trend, single samples are the faint background
        b = (df["min"] * 6).astype(int)
        full = range(b.min(), b.max() + 1)  # empty 10 s bins (recording gaps) → line break
        mt = df["min"].groupby(b).mean().reindex(full)
        mg = g.groupby(b).mean().reindex(full)
        gap = df.t_ms.diff() > 1000
        ax.plot(df["min"].mask(gap), g.mask(gap), lw=0.3, color=color, alpha=0.18)
        ax.plot(mt, mg, color=color, label=label)
        end = (mg.notna() & mg.shift(1).notna())[::-1].idxmax()  # end of the line, not a lone point
        ax.annotate(label, (mt[end], mg[end]), xytext=(6, 0),
                    textcoords="offset points", va="center", color=TEXT, fontsize=10)
        if "open" in name:
            # exponential: g = A + B·exp(−t/τ), τ by search
            t = df["min"].values
            best = min(((np.linalg.lstsq(np.c_[np.ones_like(t), np.exp(-t / tau)], g, rcond=None), tau)
                        for tau in np.arange(1, 30, 0.1)), key=lambda r: r[0][1][0])
            (A, B), tau = best[0][0], best[1]
            ax.plot(t, A + B * np.exp(-t / tau), ls="--", lw=1.2, color=TEXT2)
            ax.text(0.45 * t[-1], A + 0.12, f"экспонента: τ ≈ {tau:.1f} мин, всего {A:+.2f} г",
                    color=TEXT2, fontsize=10)
    ax.axhline(0, color=TEXT2, lw=0.6)
    ax.set_title("Пустая платформа: ноль уходит при смене температуры")
    ax.set_xlabel("минуты от начала записи")
    ax.set_ylabel("изменение показаний, г")
    if len(ax.get_legend_handles_labels()[0]) > 1:  # one line — no legend needed
        ax.legend(loc="lower left")
    ax.margins(x=0.12)
    fig.tight_layout()
    fig.savefig(IMG / "drift.png", dpi=130)


def noise():
    """Spread of readings versus the number of averaged samples: does √n hold.

    Both points are from one day and one weight (2026-10-17, ~108.6 g), otherwise
    the comparison is unfair: noise depends on conditions.
    """
    pts = [(n, pd.read_csv(DATA / f"2026-10-17_stream_n{n}.csv", comment="#").g.std() * 1000)
           for n in (10, 40)]
    n0, s0 = pts[0]

    fig, ax = plt.subplots(figsize=(8, 5))
    nn = np.logspace(0, np.log10(60), 50)
    ax.plot(nn, s0 * np.sqrt(n0 / nn), ls="--", lw=1.2, color=TEXT2,
            label="теория: ∝ 1/√n (через точку n = 10)")
    ax.plot([p[0] for p in pts], [p[1] for p in pts], "o", ms=8, color=BLUE, label="измерено",
            markeredgecolor=SURFACE, markeredgewidth=2)
    for n, s in pts:
        ax.annotate(f"{s:.1f} мг", (n, s), xytext=(8, 6), textcoords="offset points",
                    color=TEXT, fontsize=10)
    ax.annotate(f"один отсчёт ≈ {s0 * np.sqrt(n0):.0f} мг (оценка)", (1, s0 * np.sqrt(n0)),
                xytext=(8, -4), textcoords="offset points", color=TEXT2, fontsize=10)
    ax.set_xscale("log"); ax.set_yscale("log")
    ax.set_xticks([1, 10, 40]); ax.set_xticklabels(["1", "10", "40"])
    ax.set_yticks([2, 3, 5, 10, 20]); ax.set_yticklabels(["2", "3", "5", "10", "20"])
    ax.minorticks_off()
    ax.set_title("Усреднение работает: шум падает как 1/√n")
    ax.set_xlabel("отсчётов в среднем (n), ~11.7 отсчёта в секунду")
    ax.set_ylabel("разброс (sd), мг")
    ax.legend(loc="upper right")
    fig.tight_layout()
    fig.savefig(IMG / "noise_vs_n.png", dpi=130)


def drift_rate():
    """Drift rate per minute — the slope of drift.png. Shows when the drift ended."""
    runs = [("2026-10-20_drift_window_open.csv", "окно открыто (+7 °C)", BLUE, -12),
            ("2026-10-20_drift_window_closed.csv", "окно закрыли", ORANGE, 12)]
    fig, ax = plt.subplots(figsize=(10, 4.2))
    ax.axhspan(-0.005, 0.005, color=GRID, zorder=0)
    ax.text(22.5, -0.035, "серая полоса: ±5 мг/мин —\nдрейф не мешает", color=TEXT2, fontsize=10)
    for name, label, color, dy in runs:
        if not (DATA / name).exists():
            continue
        df = load(name)
        g = (df.raw - df.raw.iloc[:120].mean()) / scale_of(name)
        mi = df["min"].astype(int)
        m = g.groupby(mi).mean().reindex(range(mi.max() + 1))  # mean per minute; gap → NaN
        rate = m.diff()                                        # g per minute
        ax.plot(rate.index, rate.values, "o-", ms=5, color=color, label=label,
                markeredgecolor=SURFACE, markeredgewidth=1.5)
        last = rate.dropna()
        ax.annotate(label, (last.index[-1], last.values[-1]), xytext=(6, dy),
                    textcoords="offset points", va="center", color=TEXT, fontsize=10)
    ax.axhline(0, color=TEXT2, lw=0.6)
    ax.set_title("Скорость дрейфа: остывание затухает за ~20 мин, нагрев медленнее")
    ax.set_xlabel("минуты от начала записи")
    ax.set_ylabel("дрейф, г/мин")
    if len(ax.get_legend_handles_labels()[0]) > 1:
        ax.legend(loc="lower right")
    ax.margins(x=0.12)
    fig.tight_layout()
    fig.savefig(IMG / "drift_rate.png", dpi=130)


def detector():
    """Share of pills caught versus mass: the first detect version and the fixed one."""
    df = pd.read_csv(DATA / "detector_sim.csv", comment="#")
    fig, ax = plt.subplots(figsize=(9, 5))
    ax.axvline(0.07, color=GRID, lw=6, zorder=0)
    ax.text(0.072, 4, "самая лёгкая\nтаблетка 0.07 г", color=TEXT2, fontsize=10)
    for col, label, color in [("before_pct", "до исправления", ORANGE),
                              ("after_pct", "после", BLUE)]:
        ax.plot(df.pill_g, df[col], "o-", ms=6, color=color, label=label,
                markeredgecolor=SURFACE, markeredgewidth=1.5)
    ax.annotate("до исправления", (0.10, 75.1), xytext=(10, -4), textcoords="offset points",
                color=TEXT, fontsize=10)
    ax.annotate("после", (0.05, 96.5), xytext=(-48, 4), textcoords="offset points",
                color=TEXT, fontsize=10)
    ax.set_ylim(-3, 105)
    ax.set_title("Детектор: сколько таблеток поймано (синтетика, 1000 прогонов)")
    ax.set_xlabel("масса таблетки, г")
    ax.set_ylabel("поймано, %")
    ax.legend(loc="lower right")
    fig.tight_layout()
    fig.savefig(IMG / "detector.png", dpi=130)


def lifts():
    """Box lifts: one figure per recording docs/data/*_lifts_*.csv."""
    import lifts as lifts_mod  # tools/lifts.py
    for csv in sorted(DATA.glob("*_lifts_*.csv")):
        lifts_mod.run(csv, IMG / (csv.stem.split("_", 1)[1] + ".png"))


def lifts_compare():
    """The main milestone 2 figure: "nothing taken" versus "one pill taken"."""
    import lifts as lifts_mod
    a_csv, b_csv = DATA / "2026-10-21_lifts_a_no_pill.csv", DATA / "2026-10-21_lifts_b_one_pill.csv"
    if not (a_csv.exists() and b_csv.exists()):
        return
    def ds(csv):
        df = lifts_mod.load(csv)
        _, _, thr = lifts_mod.levels(df.g)
        return lifts_mod.analyse(df, lifts_mod.segments(df, thr)).d.dropna().values * 1000  # mg
    a, b = ds(a_csv), ds(b_csv)
    rng = np.random.default_rng(1)  # vertical jitter so the dots do not overlap
    fig, ax = plt.subplots(figsize=(10, 4.2))
    ax.axvspan(-35, 35, color=GRID, zorder=0)
    ax.text(0, 1.62, "порог ±35 мг", ha="center", color=TEXT2, fontsize=10)
    for y, v, color, label in [(1, a, BLUE, f"ничего не взяли (А), {len(a)} снятий"),
                               (0, b, ORANGE, f"взяли таблетку (Б), {len(b)} снятий")]:
        ax.plot(v, y + rng.uniform(-0.18, 0.18, len(v)), "o", ms=8, color=color, label=label,
                markeredgecolor=SURFACE, markeredgewidth=1.5)
    for x, txt in [(np.median(b[(b < -100) & (b > -200)]), "мелкая\n~0.14 г"),
                   (np.median(b[b < -400]), "крупная\n~0.54 г"),
                   (np.median(b[(b > -100) & (b < -35)]), "самая\nмелкая\n~0.08 г")]:
        ax.annotate(txt, (x, -0.3), ha="center", va="top", color=TEXT2, fontsize=10)
    ax.set_yticks([0, 1]); ax.set_yticklabels(["Б: одна\nтаблетка", "А: ничего"])
    ax.set_ylim(-0.95, 1.8)
    ax.grid(axis="y", visible=False)
    ax.set_xlabel("«после» − «до», мг (метод двух нулей)")
    ax.set_title("Снятия коробки: доза отделяется от «ничего» с большим запасом")
    ax.legend(loc="upper left", bbox_to_anchor=(0, -0.2), ncol=2)
    fig.tight_layout()
    fig.savefig(IMG / "lifts_a_vs_b.png", dpi=130)


def creep():
    """Experiment B1: weight steps. Creep would be proportional to the weight and mirrored."""
    name = "2026-11-07_creep_v1.csv"
    if not (DATA / name).exists():
        return
    df = load(name)
    t = df["min"].values
    g = df.g.values * 1000  # mg
    steps = [(0, 10, "пусто, 0–10 мин", TEXT2, "-"),
             (10, 30, "поставили 92.5 г", BLUE, "-"),
             (30, 50, "сняли 92.5 г", BLUE, "--"),
             (50, 70, "поставили 183.9 г", ORANGE, "-"),
             (70, 95, "сняли 183.9 г", ORANGE, "--")]
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(13, 5.2), gridspec_kw={"width_ratios": [1.25, 1]})
    # left — the whole recording with the weight subtracted: each segment is shifted
    # so its first 30 s continue the previous one (the exact mass is unknown to the
    # scale — only the shape matters)
    prev = None
    for a, b, label, color, ls in steps:
        s = (t >= a + 0.4) & (t < b)
        x, y = t[s], g[s]
        bins = ((x - a) * 6).astype(int)
        mx = pd.Series(x).groupby(bins).mean()
        my = pd.Series(y).groupby(bins).median()
        if prev is not None:
            my = my - my.iloc[0] + prev
        prev = my.iloc[-1]
        if b - a == 20 and a in (10, 50):
            a1.axvspan(a, b, color=color, alpha=0.10, lw=0)
        a1.plot(mx, my, color=TEXT, lw=1.4)
        # right — each segment from its own start, the first 15 minutes
        k = mx - a <= 15
        a2.plot(mx[k] - a, my[k] - my.iloc[0], color=color, ls=ls, label=label)
    a1.text(20, a1.get_ylim()[1], "92.5 г", ha="center", va="top", color=BLUE)
    a1.text(60, a1.get_ylim()[1], "183.9 г", ha="center", va="top", color=ORANGE)
    a1.set_title("Ноль весов за 95 минут (груз вычтен)")
    a1.set_xlabel("минуты от начала записи")
    a1.set_ylabel("мг")
    a2.axhline(0, color=TEXT2, lw=0.6)
    a2.set_title("После каждого шага")
    a2.set_xlabel("минуты после шага")
    a2.set_ylabel("изменение, мг")
    a2.legend(loc="lower left", fontsize=10)
    fig.tight_layout()
    fig.savefig(IMG / "creep_v1.png", dpi=130)


if __name__ == "__main__":
    IMG.mkdir(parents=True, exist_ok=True)
    drift()
    drift_rate()
    noise()
    detector()
    lifts()
    lifts_compare()
    creep()
    print("done:", ", ".join(p.name for p in sorted(IMG.glob("*.png"))))

