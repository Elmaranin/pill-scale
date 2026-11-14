#!/usr/bin/env python3
"""Quick look at a recording: weight over time and a noise histogram.


    python tools/plot.py data/20261017-120000_hx711.csv

Axis labels are Russian, like the docs the figures are made for.
"""

import sys

import matplotlib.pyplot as plt
import pandas as pd


def main(path):
    df = pd.read_csv(path, comment="#")
    df["t_s"] = (df["t_ms"] - df["t_ms"].iloc[0]) / 1000

    fig, (a1, a2) = plt.subplots(2, 1, figsize=(10, 7))
    a1.plot(df["t_s"], df["g"], lw=0.8)
    a1.set_xlabel("время, с")
    a1.set_ylabel("вес, г")
    a1.set_title(path)
    a1.grid(alpha=0.3)

    a2.hist(df["g"] - df["g"].median(), bins=60)
    a2.set_xlabel("отклонение от медианы, г")
    a2.set_title(f"sd = {df['g'].std():.4f} г, n = {len(df)}")
    a2.grid(alpha=0.3)

    fig.tight_layout()
    plt.show()


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
