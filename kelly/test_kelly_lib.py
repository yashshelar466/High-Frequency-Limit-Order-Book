#!/usr/bin/env python3
"""Tests for the Kelly study.

Every closed form in kelly_lib is checked two ways: against a brute-force
search (does c* really maximise E[g]?) and against the Monte Carlo engine
(does the simulated growth match the formula?). Agreement between an algebraic
derivation and an independent simulation is the evidence that both are right;
either alone could share a sign error with the write-up.

Simulation checks compare against a tolerance of a few standard errors
computed from the same run, with fixed seeds, so they are deterministic.

Run:  python3 kelly/test_kelly_lib.py
"""

import sys

import numpy as np

from kelly_lib import (BGK_BETA, bayes_kelly, binary_growth, binary_kelly,
                       expected_growth_estimated, fixed_fraction,
                       fractional_kelly, full_kelly, growth_rate,
                       kelly_fraction, optimal_multiple, oracle_shrinkage,
                       plugin_shrinkage, prob_ever_below, simulate,
                       simulate_binary, t_stat)

FAILURES = []


def check(cond, label):
    if cond:
        print(f"  [PASS] {label}")
    else:
        print(f"  [FAIL] {label}")
        FAILURES.append(label)


def within_se(estimate, se, target, k=4.0):
    return abs(estimate - target) <= k * se


# Sharpe 1 at 20% vol: f* = 5 (5x leverage), g* = 0.5 per year.
MU, SIGMA = 0.2, 0.2


def test_kelly_maximises_growth():
    print("Kelly fraction maximises eq. (1)")
    f = np.linspace(-5, 15, 20001)
    g = growth_rate(f, MU, SIGMA)
    check(np.isclose(f[np.argmax(g)], kelly_fraction(MU, SIGMA), atol=1e-3),
          "argmax of g(f) is mu/sigma^2")
    check(np.isclose(g.max(), MU ** 2 / (2 * SIGMA ** 2)), "max growth is mu^2 / (2 sigma^2)")
    check(np.isclose(growth_rate(2 * kelly_fraction(MU, SIGMA), MU, SIGMA), 0.0),
          "twice Kelly has zero growth")


def test_expected_growth_formula_by_integration():
    """Eq. (2) against a direct average of eq. (1) over draws of mu_hat.

    No path simulation here -- this isolates the algebra of taking the
    expectation from everything else.
    """
    print("eq. (2) matches averaging eq. (1) over estimation noise")
    rng = np.random.default_rng(1)
    s = 0.15
    mu_hat = MU + s * rng.standard_normal(2_000_000)
    for c in (0.25, 0.5, 1.0, 1.5):
        g = growth_rate(c * mu_hat / SIGMA ** 2, MU, SIGMA)
        se = g.std() / np.sqrt(len(g))
        check(within_se(g.mean(), se, expected_growth_estimated(c, MU, SIGMA, s)),
              f"c={c}: MC {g.mean():+.4f} vs formula {expected_growth_estimated(c, MU, SIGMA, s):+.4f}")


def test_optimal_multiple_by_grid_search():
    print("c* = t^2/(1+t^2) maximises eq. (2)")
    c = np.linspace(0, 2, 200001)
    for t in (0.5, 1.0, 2.0, 5.0):
        s = MU / t
        best = c[np.argmax(expected_growth_estimated(c, MU, SIGMA, s))]
        check(np.isclose(best, optimal_multiple(t), atol=1e-4),
              f"t={t}: grid argmax {best:.4f} vs c* {optimal_multiple(t):.4f}")
    check(np.isclose(optimal_multiple(1.0), 0.5), "t = 1 gives half Kelly")


def test_full_kelly_breakeven_at_t_equals_one():
    print("full Kelly has zero expected growth at t = 1")
    check(np.isclose(expected_growth_estimated(1.0, MU, SIGMA, MU), 0.0), "t = 1 -> E[g] = 0")
    check(expected_growth_estimated(1.0, MU, SIGMA, 1.2 * MU) < 0, "t < 1 -> E[g] < 0")
    check(expected_growth_estimated(0.5, MU, SIGMA, 1.2 * MU) > 0,
          "...while half Kelly still grows at t < 1")
    # The headline number: Sharpe 1, one year of data.
    check(np.isclose(t_stat(MU, SIGMA, 1.0), 1.0), "Sharpe 1 with 1 year of data is t = 1")


def test_bayes_with_matched_prior_is_oracle():
    """With prior N(0, mu^2), the Bayes rule and the oracle shrinkage coincide exactly."""
    print("Bayes rule with prior sd = |mu| equals oracle c*")
    s = 0.13
    mu_hat = np.linspace(-0.5, 0.8, 101)
    a = bayes_kelly(0.0, abs(MU))(mu_hat, SIGMA, s)
    b = oracle_shrinkage(MU)(mu_hat, SIGMA, s)
    check(np.allclose(a, b), "identical fractions for every mu_hat")
    check(np.allclose(bayes_kelly(0.0, 1e9)(mu_hat, SIGMA, s), mu_hat / SIGMA ** 2),
          "a flat prior recovers plain full Kelly")


def test_simulated_growth_matches_formula():
    """The full engine: estimation, sizing, and trading, against eq. (2)."""
    print("simulated growth matches eq. (2)")
    t_est = 1.0                                  # t = 1
    s = SIGMA / np.sqrt(t_est)
    rules = {"full": full_kelly(), "half": fractional_kelly(0.5),
             "fixed_1": fixed_fraction(1.0), "oracle": oracle_shrinkage(MU)}
    res = simulate(rules, MU, SIGMA, t_est=t_est, t_trade=2.0, n_paths=200_000,
                   steps_per_year=12, seed=2)
    for name, c in (("full", 1.0), ("half", 0.5), ("oracle", optimal_multiple(1.0))):
        m = res[name].summary()
        target = expected_growth_estimated(c, MU, SIGMA, s)
        check(within_se(m["mean_growth"], m["se_growth"], target),
              f"{name}: sim {m['mean_growth']:+.4f}±{m['se_growth']:.4f} vs {target:+.4f}")
    m = res["fixed_1"].summary()
    check(within_se(m["mean_growth"], m["se_growth"], growth_rate(1.0, MU, SIGMA)),
          f"fixed f=1: sim {m['mean_growth']:+.4f} vs {growth_rate(1.0, MU, SIGMA):+.4f}")
    # Paired comparison: common random numbers make the difference precise.
    diff = res["half"].growth - res["full"].growth
    check(diff.mean() - 4 * diff.std() / np.sqrt(len(diff)) > 0,
          f"half Kelly beats full Kelly at t = 1 (paired diff {diff.mean():+.4f})")


def test_common_random_numbers():
    print("every rule sees the same randomness")
    rules = {"a": fractional_kelly(0.7), "b": fractional_kelly(0.7), "c": full_kelly()}
    r1 = simulate(rules, MU, SIGMA, 1.0, 1.0, n_paths=500, steps_per_year=50, seed=3)
    r2 = simulate(rules, MU, SIGMA, 1.0, 1.0, n_paths=500, steps_per_year=50, seed=3)
    check(np.array_equal(r1["a"].growth, r1["b"].growth), "identical rules -> identical paths")
    check(np.array_equal(r1["c"].growth, r2["c"].growth), "same seed -> same result")
    # Same Brownian path: a rule's log wealth is f * (shared path) + deterministic
    # drift, so the noise parts of two rules must be perfectly correlated.
    ra, rc = r1["a"], r1["c"]
    na = (ra.growth - growth_rate(ra.f, MU, SIGMA)) / (ra.f * SIGMA)
    nc = (rc.growth - growth_rate(rc.f, MU, SIGMA)) / (rc.f * SIGMA)
    check(np.allclose(na, nc), "rules share one Brownian path")


def test_leverage_cap():
    print("leverage cap is applied")
    res = simulate({"full": full_kelly()}, MU, SIGMA, 0.25, 1.0, n_paths=2000,
                   steps_per_year=12, f_cap=2.0, seed=4)
    check(np.max(np.abs(res["full"].f)) <= 2.0 + 1e-12, "|f| <= cap")


def test_barrier_probability():
    """P(ever halving) against x^(2/c - 1), with mu known and fine monitoring.

    The simulation monitors at discrete steps and runs for a finite horizon,
    both of which under-count crossings. The first is corrected with the BGK
    shift; the horizon is long enough that the second is negligible (the log
    wealth drift is 0.5/yr against 1/sqrt(yr) volatility).
    """
    print("probability of ever halving matches x^(2/c - 1)")
    f_star = kelly_fraction(MU, SIGMA)
    steps = 100
    rules = {1.0: fixed_fraction(f_star), 0.5: fixed_fraction(0.5 * f_star)}
    res = simulate(rules, MU, SIGMA, t_est=np.inf, t_trade=30.0, n_paths=20_000,
                   steps_per_year=steps, seed=5)
    for c in (1.0, 0.5):
        v = c * MU / SIGMA                      # vol of log wealth
        shifted = np.log(0.5) - BGK_BETA * v * np.sqrt(1.0 / steps)
        target = prob_ever_below(np.exp(shifted), c)
        p = res[c].prob_ever_below(0.5)
        se = np.sqrt(target * (1 - target) / 20_000)
        check(within_se(p, se, target),
              f"c={c}: sim {p:.4f} vs {target:.4f} (continuous: {prob_ever_below(0.5, c):.4f})")
    check(np.isclose(prob_ever_below(0.5, 1.0), 0.5), "full Kelly: P(halving) = 1/2")
    check(np.isclose(prob_ever_below(0.5, 0.5), 0.125), "half Kelly: P(halving) = 1/8")


def test_binary_closed_forms():
    print("binary bet closed forms")
    p, b = 0.55, 1.0
    f = np.linspace(0, 0.999, 99901)
    g = binary_growth(f, p, b)
    check(np.isclose(f[np.argmax(g)], binary_kelly(p, b), atol=1e-4), "argmax is p - q/b")
    check(np.isclose(binary_kelly(0.5, 1.0), 0.0), "fair coin at even odds: bet nothing")
    check(np.isclose(binary_kelly(0.25, 3.0), 0.0), "fair odds in general: bet nothing")
    fs = binary_kelly(p, b)
    check(binary_growth(1.5 * fs, p, b) < binary_growth(0.5 * fs, p, b),
          "even odds: over-betting by 50% costs more than under-betting by 50%")
    check(binary_growth(2.2 * fs, p, b) < 0, "even odds: 2.2x Kelly loses money")
    check(np.isneginf(binary_growth(1.0, p, b)), "staking everything: growth is -inf")


def test_binary_simulation():
    print("binary simulation")
    p, b = 0.55, 1.0
    # With a huge estimation sample p_hat = p, so growth per bet must match the closed form.
    g, stakes = simulate_binary([1.0, 0.5], p, b, n_est=10 ** 9, n_bets=500,
                                n_paths=50_000, seed=6)
    for c in (1.0, 0.5):
        target = binary_growth(c * binary_kelly(p, b), p, b)
        se = g[c].std() / np.sqrt(len(g[c]))
        check(within_se(g[c].mean(), se, target),
              f"c={c}, p known: sim {g[c].mean():+.5f} vs {target:+.5f}")
    # A thin estimation sample must produce some zero stakes (p_hat <= 1/2).
    _, stakes = simulate_binary([1.0], p, b, n_est=20, n_bets=10, n_paths=5000, seed=7)
    check((stakes[1.0] == 0).mean() > 0.1, "noisy p_hat sometimes says 'do not bet'")
    check(stakes[1.0].min() >= 0 and stakes[1.0].max() <= 1, "stakes clipped to [0, 1]")


def main():
    tests = [
        test_kelly_maximises_growth,
        test_expected_growth_formula_by_integration,
        test_optimal_multiple_by_grid_search,
        test_full_kelly_breakeven_at_t_equals_one,
        test_bayes_with_matched_prior_is_oracle,
        test_simulated_growth_matches_formula,
        test_common_random_numbers,
        test_leverage_cap,
        test_barrier_probability,
        test_binary_closed_forms,
        test_binary_simulation,
    ]
    for t in tests:
        t()
    print()
    if FAILURES:
        print(f"{len(FAILURES)} FAILED:")
        for f in FAILURES:
            print(f"  - {f}")
        sys.exit(1)
    print("all checks passed")


if __name__ == "__main__":
    main()
