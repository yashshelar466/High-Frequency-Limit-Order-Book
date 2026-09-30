#!/usr/bin/env python3
"""Run the Kelly-under-uncertainty study: comparison tables and figures.

    python3 kelly/run_study.py              # tables only
    python3 kelly/run_study.py --figures    # also write kelly/figures/*.png

The baseline is a Sharpe-1 strategy at 20% volatility: Kelly leverage 5x and
a known-edge growth rate of 0.5/yr. Each rule sees the same estimation draws
and the same price paths (common random numbers), so the rows differ only in
how they size.
"""

import argparse
import os

import numpy as np

from kelly_lib import (bayes_kelly, expected_growth_estimated, fixed_fraction,
                       fractional_kelly, full_kelly, optimal_multiple,
                       oracle_shrinkage, plugin_shrinkage, simulate, t_stat)

MU, SIGMA = 0.2, 0.2
G_STAR = MU ** 2 / (2 * SIGMA ** 2)
HERE = os.path.dirname(os.path.abspath(__file__))


def rules():
    return {
        "full Kelly": full_kelly(),
        "3/4 Kelly": fractional_kelly(0.75),
        "1/2 Kelly": fractional_kelly(0.5),
        "1/4 Kelly": fractional_kelly(0.25),
        "fixed f=1": fixed_fraction(1.0),
        "plug-in c*(t_hat)": plugin_shrinkage(),
        # A sceptical prior: Sharpe ~ N(0, 0.5^2), i.e. mu ~ N(0, 0.1^2).
        "Bayes, prior SR sd 0.5": bayes_kelly(0.0, 0.5 * SIGMA),
        "oracle c*(t)": oracle_shrinkage(MU),
    }


def print_table(title, res):
    print(f"\n{title}")
    hdr = (f"{'rule':<24}{'mean f':>8}{'E[g]':>9}{'±se':>7}{'median':>9}"
           f"{'p05':>9}{'P(loss)':>9}{'P(halved)':>11}{'med maxDD':>11}")
    print(hdr)
    print("-" * len(hdr))
    for name, r in res.items():
        s = r.summary()
        print(f"{name:<24}{s['mean_f']:>8.2f}{s['mean_growth']:>+9.3f}{s['se_growth']:>7.3f}"
              f"{s['median_growth']:>+9.3f}{s['p05_growth']:>+9.3f}{s['p_loss']:>9.3f}"
              f"{s['p_halved']:>11.3f}{s['median_max_dd']:>11.3f}")


def run_tables(n_paths, t_trade):
    print(f"mu={MU}, sigma={SIGMA} (Sharpe {MU / SIGMA:.1f}), f*={MU / SIGMA ** 2:.1f}, "
          f"g*={G_STAR:.3f}/yr; trading horizon {t_trade:g} years, {n_paths} paths")
    for t_est in (0.5, 1.0, 4.0):
        t = t_stat(MU, SIGMA, t_est)
        s = SIGMA / np.sqrt(t_est)
        print(f"\n  theory at t={t:.2f}: full Kelly E[g]="
              f"{expected_growth_estimated(1.0, MU, SIGMA, s):+.3f}, "
              f"c*={optimal_multiple(t):.2f} -> E[g]="
              f"{expected_growth_estimated(optimal_multiple(t), MU, SIGMA, s):+.3f}")
        res = simulate(rules(), MU, SIGMA, t_est=t_est, t_trade=t_trade,
                       n_paths=n_paths, steps_per_year=52, seed=11)
        print_table(f"estimated from {t_est:g} years of data (t = {t:.2f})", res)

    # The edge was measured at Sharpe 1 over 2 years but is gone by the time
    # it is traded. Every rule that sizes from the estimate is now pure risk.
    res = simulate(rules(), MU, SIGMA, t_est=2.0, t_trade=t_trade, n_paths=n_paths,
                   steps_per_year=52, mu_trade=0.0, seed=12)
    print_table("edge decay: estimated over 2 years, true edge 0 while trading", res)


def run_figures(out_dir):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.colors import LinearSegmentedColormap, TwoSlopeNorm

    os.makedirs(out_dir, exist_ok=True)
    ink, muted, grid = "#1f1f1e", "#6b6a64", "#e4e3de"
    series = ["#2a78d6", "#eb6834", "#1baf7a", "#e87ba4"]   # categorical, fixed order
    plt.rcParams.update({
        "font.size": 10, "axes.edgecolor": muted, "axes.labelcolor": ink,
        "xtick.color": muted, "ytick.color": muted, "axes.spines.top": False,
        "axes.spines.right": False, "axes.grid": True, "grid.color": grid,
        "grid.linewidth": 0.8, "figure.dpi": 150,
    })

    # 1. Expected growth against the Kelly multiple, one line per t.
    c = np.linspace(0, 2, 401)
    fig, ax = plt.subplots(figsize=(7, 4.2))
    for color, t in zip(series, (0.5, 1.0, 2.0, 4.0)):
        s = MU / t
        g = expected_growth_estimated(c, MU, SIGMA, s) / G_STAR
        ax.plot(c, g, color=color, lw=2, label=f"t = {t:g}")
        cs = optimal_multiple(t)
        gs = expected_growth_estimated(cs, MU, SIGMA, s) / G_STAR
        ax.plot(cs, gs, "o", ms=7, color=color, mec="white", mew=1.5, zorder=3)
        ax.annotate(f"t = {t:g}", (cs, gs), xytext=(0, 7), textcoords="offset points",
                    ha="center", va="bottom", color=ink, fontsize=9)
    ax.axhline(0, color=muted, lw=1)
    ax.axvline(1, color=muted, lw=1, ls="--")
    ax.text(1.02, 0.95, "full Kelly", color=muted, fontsize=9, transform=ax.get_xaxis_transform())
    ax.set_xlim(0, 2)
    ax.set_ylim(-1.5, 1.15)
    ax.set_xlabel("Kelly multiple c  (bet = c × estimated Kelly fraction)")
    ax.set_ylabel("expected growth / known-edge growth")
    ax.set_title("Estimation error moves the growth-optimal bet below full Kelly\n"
                 "dots: c* = t²/(1+t²)", color=ink, fontsize=11, loc="left")
    ax.legend(frameon=False, loc="lower left")
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, "growth_vs_multiple.png"))
    plt.close(fig)

    # 2. The whole surface: E[g]/g* over (t, c). Diverging: growth vs loss.
    t = np.linspace(0.2, 5, 241)
    c = np.linspace(0, 2, 201)
    T, C = np.meshgrid(t, c)
    Z = expected_growth_estimated(C, MU, SIGMA, MU / T) / G_STAR
    cmap = LinearSegmentedColormap.from_list(
        "div", ["#a32a2a", "#d03b3b", "#f0efec", "#3987e5", "#1c5cab"])
    fig, ax = plt.subplots(figsize=(7, 4.6))
    pc = ax.pcolormesh(T, C, Z, cmap=cmap, norm=TwoSlopeNorm(0, vmin=-2, vmax=1),
                       shading="auto", rasterized=True)
    ax.contour(T, C, Z, levels=[0], colors=ink, linewidths=1.2)
    ax.plot(t, optimal_multiple(t), color="white", lw=3)
    ax.plot(t, optimal_multiple(t), color=ink, lw=1.5, ls="--")
    ax.text(3.0, optimal_multiple(3.0) - 0.13, "c* = t²/(1+t²), best multiple",
            color=ink, fontsize=9, ha="center")
    ax.text(3.0, 2 * optimal_multiple(3.0) - 0.13, "E[g] = 0 at c = 2c*",
            color=ink, fontsize=9, ha="center")
    ax.text(4.95, 1.03, "full Kelly", color=ink, fontsize=9, ha="right")
    ax.text(0.45, 1.75, "negative expected growth", color="white", fontsize=9)
    ax.axhline(1, color=ink, lw=0.8, ls=":")
    ax.set_xlabel("t-statistic of the estimated edge  (Sharpe × √years of data)")
    ax.set_ylabel("Kelly multiple c")
    ax.grid(False)
    ax.set_title("Expected growth relative to knowing the edge", color=ink,
                 fontsize=11, loc="left")
    cb = fig.colorbar(pc, ax=ax, pad=0.02)
    cb.set_label("E[g] / g*")
    cb.outline.set_visible(False)
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, "growth_surface.png"))
    plt.close(fig)
    print(f"\nfigures written to {out_dir}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--paths", type=int, default=20_000)
    ap.add_argument("--years", type=float, default=10.0, help="trading horizon")
    ap.add_argument("--figures", action="store_true")
    args = ap.parse_args()
    run_tables(args.paths, args.years)
    if args.figures:
        run_figures(os.path.join(HERE, "figures"))


if __name__ == "__main__":
    main()
