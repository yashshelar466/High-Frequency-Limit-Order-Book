#!/usr/bin/env python3
"""Tests for the analysis helpers.

The C++ engine has four test suites; the statistics deserve the same treatment,
because a bug here does not crash -- it produces a publishable-looking number
that is wrong. Each test below uses a series whose correct answer is known by
construction rather than recorded from a previous run.

Run:  python3 research/test_ofi_lib.py
"""

import sys

import numpy as np
import pandas as pd

from ofi_lib import (TICK, add_forward_returns, backtest, build_panel, fit_oos,
                     permutation_null)

FAILURES = []


def check(cond, label):
    if cond:
        print(f"  [PASS] {label}")
    else:
        print(f"  [FAIL] {label}")
        FAILURES.append(label)


def test_forward_return_alignment():
    """fwd_h must be built only from prices observed after the interval closes.

    Constructed so the answer is unambiguous: mid_close climbs by exactly one
    tick per interval, so the h-step forward return must be exactly h ticks
    everywhere it is defined, and undefined for the last h rows.
    """
    print("forward return alignment")
    n = 10
    panel = pd.DataFrame({
        "mid_close": np.arange(n, dtype=float) * TICK,
        "mid_open": np.arange(n, dtype=float) * TICK,
    })
    out = add_forward_returns(panel, horizons=(1, 3))

    check(np.allclose(out.fwd_1.dropna(), 1.0), "1-step forward return is 1 tick")
    check(np.allclose(out.fwd_3.dropna(), 3.0), "3-step forward return is 3 ticks")
    check(out.fwd_1.isna().sum() == 1, "1-step leaves exactly 1 undefined row")
    check(out.fwd_3.isna().sum() == 3, "3-step leaves exactly 3 undefined rows")
    # The decisive check: the last defined 1-step value uses row n-1's price,
    # so shifting the price of the FINAL row must change it -- and must not
    # change any earlier row. That is what "no lookahead beyond h" means.
    panel2 = panel.copy()
    panel2.loc[n - 1, "mid_close"] += 5 * TICK
    out2 = add_forward_returns(panel2, horizons=(1,))
    changed = ~np.isclose(out.fwd_1.fillna(-999).to_numpy(),
                          out2.fwd_1.fillna(-999).to_numpy())
    check(changed.sum() == 1 and bool(changed[n - 2]),
          "only the row immediately preceding a changed price is affected")


def test_no_lookahead_in_panel():
    """A price change inside interval t must not alter interval t-1's features."""
    print("panel construction has no backward leakage")
    df = pd.DataFrame({
        "time": np.arange(20, dtype=float) * 0.5 + 34200.0,
        "quote_valid": 1,
        "ofi": np.arange(20, dtype=float),
        "signed_trade_sz": 0.0,
        "mid": 5_850_000.0 + np.arange(20) * TICK,
        "spread_ticks": 1.0,
        "imbalance": 0.0,
    })
    base = build_panel(df, dt=2.0)
    bumped = df.copy()
    bumped.loc[15:, "mid"] += 10 * TICK          # perturb only the later part
    after = build_panel(bumped, dt=2.0)

    # Buckets covering rows 0..14 (dt=2s over 0.5s spacing => 4 rows/bucket,
    # so buckets 0..2 are rows 0..11) must be untouched.
    check(np.allclose(base.mid_close[:3], after.mid_close[:3]),
          "earlier intervals unchanged by a later price move")
    check(not np.allclose(base.mid_close.iloc[-1], after.mid_close.iloc[-1]),
          "later intervals do change (control: the test can detect a change)")


def test_oos_r2_recovers_known_relationship():
    """With y = 2x + small noise, the fit must recover beta and a high OOS R^2."""
    print("out-of-sample fit on a known linear relationship")
    rng = np.random.default_rng(0)
    x = rng.normal(size=4000)
    y = 2.0 * x + rng.normal(scale=0.1, size=4000)
    r = fit_oos(x, y, train_frac=0.7)
    check(abs(r.beta - 2.0) < 0.02, f"beta ~ 2.0 (got {r.beta:.4f})")
    check(r.r2_oos > 0.99, f"OOS R2 > 0.99 (got {r.r2_oos:.4f})")
    check(r.n_train == 2800 and r.n_test == 1200, "chronological 70/30 split")


def test_oos_r2_is_zero_on_noise():
    """Unrelated series must not produce out-of-sample explanatory power."""
    print("out-of-sample fit on unrelated series")
    rng = np.random.default_rng(1)
    x = rng.normal(size=4000)
    y = rng.normal(size=4000)
    r = fit_oos(x, y, train_frac=0.7)
    check(abs(r.r2_oos) < 0.02, f"OOS R2 ~ 0 (got {r.r2_oos:+.4f})")
    # In-sample R^2 is mechanically positive even on pure noise -- that is the
    # entire reason the out-of-sample number is the one reported.
    check(r.r2_in >= 0, "in-sample R2 is non-negative even with no relationship")


def test_split_is_chronological_not_random():
    """A trend present only in the second half must degrade OOS performance.

    If the split were random, the model would see that regime during training
    and score well. This is the test that would catch a shuffled split.
    """
    print("train/test split respects time order")
    rng = np.random.default_rng(2)
    n = 4000
    x = rng.normal(size=n)
    y = np.where(np.arange(n) < n * 0.7, 2.0 * x, -2.0 * x) + rng.normal(scale=0.1, size=n)
    r = fit_oos(x, y, train_frac=0.7)
    check(r.r2_oos < -1.0,
          f"sign flip in the holdout gives strongly negative OOS R2 (got {r.r2_oos:.2f})")


def test_permutation_null_centres_on_zero():
    print("permutation null")
    rng = np.random.default_rng(3)
    x = rng.normal(size=2000)
    y = 0.5 * x + rng.normal(size=2000)
    null = permutation_null(x, y, n_trials=200, seed=5)
    check(abs(null.mean()) < 0.02, f"null mean ~ 0 (got {null.mean():+.4f})")
    real = fit_oos(x, y).r2_oos
    check(real > np.percentile(null, 95),
          "a real relationship clears the null's 95th percentile")


def test_backtest_cost_arithmetic():
    """Hand-computable PnL: 4 trades, all correct, 1-tick moves, 2-tick spread."""
    print("backtest cost arithmetic")
    pred = np.array([1.0, -1.0, 1.0, -1.0])
    actual = np.array([1.0, -1.0, 1.0, -1.0])     # always right, 1 tick each
    spread = np.array([2.0, 2.0, 2.0, 2.0])
    bt = backtest(pred, actual, spread)

    check(bt.n_trades == 4, "all four signals traded")
    check(np.isclose(bt.gross_per_trade, 1.0), "gross = 1 tick per trade")
    check(np.isclose(bt.cost_per_trade, 2.0), "cost = one full spread per round trip")
    check(np.isclose(bt.net_per_trade, -1.0), "net = -1 tick: a perfect signal still loses")
    check(np.isclose(bt.hit_rate, 1.0), "hit rate 100%")
    check(np.isclose(bt.breakeven_spread, 1.0), "breakeven spread = gross edge")
    check(np.isclose(bt.dollars(lot=100), -1.0 * 4 * 0.01 * 100),
          "dollar conversion at $0.01/tick/share")

    # The threshold must actually filter.
    quiet = backtest(np.array([0.05, -0.05, 2.0, -2.0]), actual, spread, threshold=1.0)
    check(quiet.n_trades == 2, "threshold suppresses low-confidence signals")

    # Halving the cost multiplier must halve the cost, not the gross.
    half = backtest(pred, actual, spread, cost_multiplier=0.5)
    check(np.isclose(half.cost_per_trade, 1.0) and np.isclose(half.gross_per_trade, 1.0),
          "cost_multiplier scales cost only")


def main():
    for t in (test_forward_return_alignment,
              test_no_lookahead_in_panel,
              test_oos_r2_recovers_known_relationship,
              test_oos_r2_is_zero_on_noise,
              test_split_is_chronological_not_random,
              test_permutation_null_centres_on_zero,
              test_backtest_cost_arithmetic):
        t()
        print()

    if FAILURES:
        print(f"--- {len(FAILURES)} CHECK(S) FAILED ---")
        for f in FAILURES:
            print(f"    {f}")
        return 1
    print("--- All analysis-library tests passed ---")
    return 0


if __name__ == "__main__":
    sys.exit(main())
