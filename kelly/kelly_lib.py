"""Kelly sizing under parameter uncertainty: closed forms and a Monte Carlo engine.

The question this module exists to answer
-----------------------------------------
Kelly tells you the growth-optimal bet *given the true edge*. Nobody is given
the true edge; they are given an estimate of it. This module makes the cost of
that gap exact, then checks the algebra against simulation.

The continuous-time model
-------------------------
One asset follows dS/S = mu dt + sigma dW. A strategy holds a constant fraction
f of wealth in it, rebalanced continuously. Log wealth then grows at

    g(f) = mu f - sigma^2 f^2 / 2                                    (1)

which is maximised by the Kelly fraction f* = mu / sigma^2, at g* = mu^2 / (2 sigma^2).

Now suppose mu is replaced by an estimate mu_hat ~ N(mu, s^2) and we bet a
multiple c of the Kelly fraction *computed from the estimate*: f = c mu_hat / sigma^2.
Taking the expectation of (1) over the estimation noise, using
E[mu_hat] = mu and E[mu_hat^2] = mu^2 + s^2:

    E[g] = ( c mu^2 - (c^2 / 2)(mu^2 + s^2) ) / sigma^2              (2)

Everything in the study follows from (2):

  * Full Kelly (c = 1): E[g] = (mu^2 - s^2) / (2 sigma^2). Zero when the
    t-statistic of the edge, t = mu / s, equals 1; negative below it.
  * The best multiple is c* = mu^2 / (mu^2 + s^2) = t^2 / (1 + t^2).
  * Estimating mu from T years of data gives s = sigma / sqrt(T), so
    t^2 = T * SR^2 with SR the annualised Sharpe ratio. A Sharpe-1 strategy
    with one year of history has t = 1: half Kelly is optimal and full Kelly
    has an expected growth rate of zero.

Units: time in years, mu and sigma annualised, growth in log-wealth per year.
"""

import numpy as np

# Broadie-Glasserman-Kou continuity correction, zeta(1/2)/sqrt(2*pi). A barrier
# monitored at discrete steps of dt is hit about as often as a continuously
# monitored one moved further away by BGK_BETA * vol * sqrt(dt).
BGK_BETA = 0.5826


# ---------------------------------------------------------------------------
# Closed forms: continuous model
# ---------------------------------------------------------------------------

def kelly_fraction(mu, sigma):
    """Growth-optimal fraction of wealth, f* = mu / sigma^2. Can exceed 1 (leverage)."""
    return mu / sigma ** 2


def growth_rate(f, mu, sigma):
    """Expected log-wealth growth per year for a constant fraction f, eq. (1)."""
    return mu * f - 0.5 * sigma ** 2 * f ** 2


def t_stat(mu, sigma, t_est):
    """t-statistic of the edge after observing the asset for `t_est` years."""
    return mu * np.sqrt(t_est) / sigma


def expected_growth_estimated(c, mu, sigma, s):
    """E[g] when betting c times the Kelly fraction of an estimate mu_hat ~ N(mu, s^2), eq. (2)."""
    return (c * mu ** 2 - 0.5 * c ** 2 * (mu ** 2 + s ** 2)) / sigma ** 2


def optimal_multiple(t):
    """Kelly multiple c* = t^2 / (1 + t^2) that maximises eq. (2).

    This is a shrinkage factor: with t -> infinity (edge known) it goes to 1,
    with t -> 0 (edge indistinguishable from noise) it goes to 0. It depends on
    the TRUE t, which is unknown -- see `plugin_shrinkage` for what happens
    when it is estimated instead.
    """
    t2 = np.asarray(t, dtype=float) ** 2
    return t2 / (1.0 + t2)


def prob_ever_below(x, c):
    """P(wealth ever falls to fraction x of its initial value), betting c * f* with mu known.

    Log wealth is a Brownian motion with drift m = (mu/sigma)^2 (c - c^2/2) and
    volatility v = c mu/sigma. For a positive drift, the probability of ever
    reaching level ln(x) < 0 is x^(2m/v^2) = x^(2/c - 1). Full Kelly halves your
    wealth at some point with probability 1/2; half Kelly with probability 1/8.
    At c >= 2 the drift is no longer positive and the level is reached surely.
    """
    c = float(c)
    if c >= 2.0:
        return 1.0
    return float(x) ** (2.0 / c - 1.0)


# ---------------------------------------------------------------------------
# Closed forms: discrete binary bet
# ---------------------------------------------------------------------------
#
# Win b per unit staked with probability p, lose the stake otherwise.

def binary_kelly(p, b):
    """Kelly stake p - (1 - p)/b. Negative means the bet has negative edge."""
    return p - (1.0 - p) / b


def binary_growth(f, p, b):
    """Expected log growth per bet, p ln(1 + b f) + (1 - p) ln(1 - f), for 0 <= f < 1.

    Unlike eq. (1) this is not a parabola. As f -> 1 the loss term goes to
    -infinity, so over-betting and under-betting are not symmetric: staking
    everything is ruin on the first loss, however large the edge.
    """
    f = np.asarray(f, dtype=float)
    with np.errstate(divide="ignore"):
        return p * np.log1p(b * f) + (1.0 - p) * np.log1p(-f)


# ---------------------------------------------------------------------------
# Sizing rules
# ---------------------------------------------------------------------------
#
# Every rule maps (mu_hat, sigma, s) to a fraction of wealth, vectorised over
# mu_hat. `s` is the standard error of mu_hat, which the strategy is allowed to
# know: it follows from sigma and the length of the estimation window.

def full_kelly():
    return lambda mu_hat, sigma, s: mu_hat / sigma ** 2


def fractional_kelly(c):
    return lambda mu_hat, sigma, s: c * mu_hat / sigma ** 2


def fixed_fraction(f0):
    """Always bet f0, whatever the estimate says. Immune to estimation noise --
    and so also blind to the case where the edge is not there at all."""
    return lambda mu_hat, sigma, s: np.full_like(np.asarray(mu_hat, dtype=float), f0)


def oracle_shrinkage(mu):
    """Bet c* times the estimated Kelly fraction, with c* computed from the TRUE t.

    Not implementable -- it needs mu -- but it is the ceiling that realisable
    rules are measured against.
    """
    def rule(mu_hat, sigma, s):
        return optimal_multiple(mu / s) * mu_hat / sigma ** 2
    return rule


def plugin_shrinkage():
    """Bet c*(t_hat) times the estimated Kelly fraction, with t_hat = mu_hat / s.

    The realisable version of oracle_shrinkage. It is not the same thing:
    t_hat is itself noisy, and it is large exactly when mu_hat has come out
    too high, so the rule shrinks least when it should shrink most.
    """
    def rule(mu_hat, sigma, s):
        return optimal_multiple(mu_hat / s) * mu_hat / sigma ** 2
    return rule


def bayes_kelly(prior_mean=0.0, prior_sd=1.0):
    """Maximise posterior expected log growth under a normal prior on mu.

    Eq. (1) is linear in mu, so its posterior expectation is (1) evaluated at
    the posterior mean: the Bayes-optimal fraction is simply E[mu | data] / sigma^2.
    Posterior *variance* never enters. Uncertainty alone does not make a
    log-utility investor bet smaller; what shrinks the bet is the prior pulling
    the mean toward prior_mean. With prior_mean = 0 and prior_sd = |mu| this
    reproduces the oracle c* exactly, which is one way to read c*: it is the
    empirical-Bayes shrinkage for a prior whose scale matches the true edge.
    """
    tau2 = prior_sd ** 2

    def rule(mu_hat, sigma, s):
        post_mean = (tau2 * mu_hat + s ** 2 * prior_mean) / (tau2 + s ** 2)
        return post_mean / sigma ** 2
    return rule


# ---------------------------------------------------------------------------
# Monte Carlo: continuous model
# ---------------------------------------------------------------------------

class SimResult:
    """Per-path outcomes for one sizing rule. All arrays have length n_paths."""

    def __init__(self, name, f, growth, min_log_w, max_drawdown, t_trade):
        self.name = name
        self.f = f                        # fraction chosen on each path
        self.growth = growth              # log(W_T / W_0) / T, per year
        self.min_log_w = min_log_w        # lowest log(W_t / W_0) seen (monitored at steps)
        self.max_drawdown = max_drawdown  # largest peak-to-trough loss, as a fraction
        self.t_trade = t_trade

    def prob_ever_below(self, x):
        return float(np.mean(self.min_log_w <= np.log(x)))

    def summary(self):
        g = self.growth
        return {
            "mean_f": float(np.mean(self.f)),
            "mean_growth": float(np.mean(g)),
            "se_growth": float(np.std(g, ddof=1) / np.sqrt(len(g))),
            "median_growth": float(np.median(g)),
            "p05_growth": float(np.quantile(g, 0.05)),
            "p95_growth": float(np.quantile(g, 0.95)),
            "p_loss": float(np.mean(g < 0)),
            "p_halved": self.prob_ever_below(0.5),
            "median_max_dd": float(np.median(self.max_drawdown)),
        }

    def __repr__(self):
        s = self.summary()
        return (f"SimResult({self.name}: mean_f={s['mean_f']:.2f}, "
                f"growth={s['mean_growth']:+.4f}±{s['se_growth']:.4f}, "
                f"median={s['median_growth']:+.4f}, p_halved={s['p_halved']:.3f})")


def simulate(rules, mu, sigma, t_est, t_trade, n_paths=10_000, steps_per_year=252,
             mu_trade=None, f_cap=None, seed=0):
    """Estimate mu, size the position, trade it, and record what happened.

    Each path runs in two phases:

      1. Estimation. The asset is observed for `t_est` years. With sigma known,
         the maximum-likelihood drift estimate is exactly mu_hat = mu + s Z with
         s = sigma / sqrt(t_est), so it is drawn directly. `t_est=np.inf` means
         the edge is known exactly.
      2. Trading. Each rule turns mu_hat into a fraction f, held for `t_trade`
         years with continuous rebalancing. The true drift during trading is
         `mu_trade` (default: mu). Setting it lower models an edge that decays
         after it was measured, the realistic way estimates go wrong.

    Common random numbers: every rule sees the same Z and the same Brownian
    path. Differences between rules are therefore differences in sizing, not
    in luck, and far fewer paths are needed to rank them.

    Log wealth is advanced with its exact GBM increment,
        (f mu - f^2 sigma^2 / 2) dt + f sigma dW,
    so the terminal growth is exact at any step size and `steps_per_year`
    only controls how finely the path is monitored for drawdowns and barrier
    crossings. Discrete monitoring slightly under-counts both; see BGK_BETA.

    `f_cap`, if given, clips |f| -- a leverage limit.
    """
    rng = np.random.default_rng(seed)
    mu_trade = mu if mu_trade is None else mu_trade
    s = 0.0 if np.isinf(t_est) else sigma / np.sqrt(t_est)

    mu_hat = mu + s * rng.standard_normal(n_paths)

    names = list(rules)
    F = np.vstack([np.asarray(rules[k](mu_hat, sigma, s), dtype=float)
                   * np.ones(n_paths) for k in names])
    if f_cap is not None:
        F = np.clip(F, -f_cap, f_cap)

    n_steps = max(1, int(round(t_trade * steps_per_year)))
    dt = t_trade / n_steps
    drift = (F * mu_trade - 0.5 * F ** 2 * sigma ** 2) * dt
    vol = F * sigma

    log_w = np.zeros_like(F)
    peak = np.zeros_like(F)
    min_log_w = np.zeros_like(F)
    max_dd_log = np.zeros_like(F)
    for _ in range(n_steps):
        dW = rng.standard_normal(n_paths) * np.sqrt(dt)   # shared by every rule
        log_w += drift + vol * dW
        np.maximum(peak, log_w, out=peak)
        np.minimum(min_log_w, log_w, out=min_log_w)
        np.maximum(max_dd_log, peak - log_w, out=max_dd_log)

    return {
        k: SimResult(k, F[i], log_w[i] / t_trade, min_log_w[i],
                     -np.expm1(-max_dd_log[i]), t_trade)
        for i, k in enumerate(names)
    }


# ---------------------------------------------------------------------------
# Monte Carlo: binary bets
# ---------------------------------------------------------------------------

def simulate_binary(multiples, p, b, n_est, n_bets, n_paths=10_000, seed=0):
    """Estimate p from `n_est` past trials, then place `n_bets` bets.

    For each Kelly multiple c the stake is c * binary_kelly(p_hat, b), clipped
    to [0, 1]: a negative estimate means no bet (the other side of a bet is
    usually not on offer), and a stake above 1 is not possible. A stake of
    exactly 1 that loses is ruin, recorded as log wealth -inf -- which makes
    the mean growth -inf, correctly. Common random numbers across multiples.

    Returns {c: per-path mean log growth per bet}, and the per-path stakes.
    """
    rng = np.random.default_rng(seed)
    p_hat = rng.binomial(n_est, p, size=n_paths) / n_est
    wins = rng.binomial(n_bets, p, size=n_paths)
    losses = n_bets - wins

    growth, stakes = {}, {}
    for c in multiples:
        f = np.clip(c * binary_kelly(p_hat, b), 0.0, 1.0)
        with np.errstate(divide="ignore", invalid="ignore"):
            log_w = wins * np.log1p(b * f) + np.where(losses > 0, losses * np.log1p(-f), 0.0)
        growth[c] = log_w / n_bets
        stakes[c] = f
    return growth, stakes
