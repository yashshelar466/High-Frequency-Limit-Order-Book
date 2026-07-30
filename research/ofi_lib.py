"""Analysis helpers for the order-flow-imbalance study.

Why this is a module and not notebook cells
-------------------------------------------
The two mistakes that most often produce a fake result in this kind of study
are (a) a forward return that accidentally overlaps the window its predictor is
measured over, and (b) an evaluation that lets the model see the test period.
Both are three-line errors, both survive code review easily, and both make the
output *better*, which is precisely why nobody catches them.

Keeping them in a module means they can be tested against series whose right
answer is known by construction -- see test_ofi_lib.py. A notebook cell can't
be tested.

All price quantities are in ticks (1 tick = $0.01 = 100 LOBSTER price units).
"""

import numpy as np
import pandas as pd

TICK = 100.0            # LOBSTER integer price units per $0.01
LOT = 100               # shares per traded lot, for PnL in dollars


def load_features(path):
    """Load a features CSV emitted by `run_lobster --emit-features`.

    Rows where the book was one-sided carry no usable quote and are dropped
    here rather than forward-filled: a stale mid inherited across a gap would
    show up as a fake zero return, and zeros are not neutral in a regression.
    """
    df = pd.read_csv(path)
    df = df[df.quote_valid == 1].copy()
    df["mid"] = (df.bid_px + df.ask_px) / 2.0
    df["spread_ticks"] = (df.ask_px - df.bid_px) / TICK
    # Queue imbalance at the touch, the other classic microstructure feature.
    denom = (df.bid_sz + df.ask_sz).replace(0, np.nan)
    df["imbalance"] = (df.bid_sz - df.ask_sz) / denom
    return df.reset_index(drop=True)


def build_panel(df, dt=2.0):
    """Aggregate per-message rows into fixed clock intervals of `dt` seconds.

    Returns one row per interval with the predictor measured *inside* the
    interval and the mid price observed at its close. Forward returns are added
    by add_forward_returns(), which is where the alignment lives.
    """
    t0 = df.time.iloc[0]
    bucket = np.floor((df.time - t0) / dt).astype(np.int64)
    g = df.groupby(bucket)
    panel = pd.DataFrame({
        "t_start": g.time.first(),
        "t_end": g.time.last(),
        "ofi": g.ofi.sum(),
        "trade_flow": g.signed_trade_sz.sum(),
        "imbalance": g.imbalance.mean(),
        "mid_open": g.mid.first(),
        "mid_close": g.mid.last(),
        "spread_ticks": g.spread_ticks.mean(),
        "n_updates": g.size(),
    })
    panel.index.name = "bucket"
    # Contemporaneous move: within this interval. NOT a prediction target --
    # it overlaps the window the predictor was measured over.
    panel["dmid_now"] = (panel.mid_close - panel.mid_open) / TICK
    return panel.reset_index()


def add_forward_returns(panel, horizons=(1, 2, 5, 10)):
    """Add mid-price changes over the NEXT h intervals.

    The alignment, stated explicitly because everything downstream depends on
    it: row t's predictor is measured over interval t, and `fwd_h` is

        mid_close[t + h] - mid_close[t]

    which is composed entirely of price movement occurring after interval t has
    closed. There is no overlap between the window the predictor sees and the
    window the target measures. Rows near the end of the sample have no
    t + h and are left as NaN for the caller to drop.
    """
    out = panel.copy()
    for h in horizons:
        out[f"fwd_{h}"] = (out.mid_close.shift(-h) - out.mid_close) / TICK
    return out


class FitResult:
    def __init__(self, beta, intercept, r2_in, r2_oos, n_train, n_test,
                 y_test, pred_test, test_index):
        self.beta = beta
        self.intercept = intercept
        self.r2_in = r2_in
        self.r2_oos = r2_oos
        self.n_train = n_train
        self.n_test = n_test
        self.y_test = y_test
        self.pred_test = pred_test
        self.test_index = test_index

    def __repr__(self):
        return (f"FitResult(beta={self.beta:+.4e}, R2_in={self.r2_in:+.4f}, "
                f"R2_oos={self.r2_oos:+.4f}, n_train={self.n_train}, "
                f"n_test={self.n_test})")


def fit_oos(x, y, train_frac=0.7):
    """Fit y ~ a + b*x on the first `train_frac` of the sample, score on the rest.

    The split is strictly chronological. A random split would be catastrophic
    here: order book series are strongly autocorrelated, so a shuffled holdout
    contains rows minutes away from their training neighbours and effectively
    leaks the answer. The resulting R^2 looks excellent and means nothing.

    Out-of-sample R^2 is computed against the TRAINING mean, not the test
    mean. Using the test mean would credit the model with knowing the test
    period's average return -- information it would not have had in real time.
    A negative value is meaningful and common: it says the fitted relationship
    predicts the holdout worse than a constant would.
    """
    x = np.asarray(x, dtype=float)
    y = np.asarray(y, dtype=float)
    ok = np.isfinite(x) & np.isfinite(y)
    x, y = x[ok], y[ok]

    n = len(x)
    k = int(n * train_frac)
    if k < 2 or n - k < 2:
        raise ValueError(f"not enough data to split: n={n}, train={k}")

    xtr, ytr, xte, yte = x[:k], y[:k], x[k:], y[k:]
    A = np.column_stack([np.ones(k), xtr])
    coef, *_ = np.linalg.lstsq(A, ytr, rcond=None)
    intercept, beta = coef[0], coef[1]

    pred_tr = intercept + beta * xtr
    pred_te = intercept + beta * xte

    ss_res_in = np.sum((ytr - pred_tr) ** 2)
    ss_tot_in = np.sum((ytr - ytr.mean()) ** 2)
    ss_res_oos = np.sum((yte - pred_te) ** 2)
    ss_tot_oos = np.sum((yte - ytr.mean()) ** 2)

    return FitResult(
        beta=beta, intercept=intercept,
        r2_in=1 - ss_res_in / ss_tot_in if ss_tot_in > 0 else np.nan,
        r2_oos=1 - ss_res_oos / ss_tot_oos if ss_tot_oos > 0 else np.nan,
        n_train=k, n_test=n - k,
        y_test=yte, pred_test=pred_te, test_index=np.arange(k, n),
    )


def permutation_null(x, y, n_trials=500, train_frac=0.7, seed=0):
    """Distribution of out-of-sample R^2 when the predictor cannot possibly work.

    Shuffling x against y destroys any real relationship while leaving both
    marginal distributions untouched, so whatever R^2 survives is what this
    pipeline manufactures from noise alone. If the measured R^2 does not stand
    clear of this distribution, there is no result -- and if the distribution
    is not centred near zero, the pipeline itself is leaking.
    """
    rng = np.random.default_rng(seed)
    x = np.asarray(x, dtype=float)
    y = np.asarray(y, dtype=float)
    ok = np.isfinite(x) & np.isfinite(y)
    x, y = x[ok], y[ok]

    out = np.empty(n_trials)
    for i in range(n_trials):
        out[i] = fit_oos(rng.permutation(x), y, train_frac).r2_oos
    return out


class Backtest:
    def __init__(self, n_trades, gross_ticks, cost_ticks, net_ticks,
                 gross_per_trade, cost_per_trade, net_per_trade,
                 hit_rate, breakeven_spread, equity_gross, equity_net):
        self.n_trades = n_trades
        self.gross_ticks = gross_ticks
        self.cost_ticks = cost_ticks
        self.net_ticks = net_ticks
        self.gross_per_trade = gross_per_trade
        self.cost_per_trade = cost_per_trade
        self.net_per_trade = net_per_trade
        self.hit_rate = hit_rate
        self.breakeven_spread = breakeven_spread
        self.equity_gross = equity_gross
        self.equity_net = equity_net

    def dollars(self, lot=LOT):
        """Net PnL in dollars for a `lot`-share position, at $0.01 per tick."""
        return self.net_ticks * 0.01 * lot

    def __repr__(self):
        return (f"Backtest(n={self.n_trades}, gross={self.gross_per_trade:+.4f} "
                f"ticks/trade, cost={self.cost_per_trade:.4f}, "
                f"net={self.net_per_trade:+.4f}, hit={self.hit_rate:.3f})")


def backtest(pred, actual, spread_ticks, threshold=0.0, cost_multiplier=1.0):
    """Directional strategy with an explicit, deliberately unflattering cost model.

    Take a position at the close of each interval whose predicted move exceeds
    `threshold` ticks, hold it for the prediction horizon, then exit. Gross PnL
    is the realised move in the predicted direction.

    Costs: entering by crossing the spread costs half of it, exiting the same,
    so a round trip pays one full spread. That is the honest floor for a
    liquidity-*taking* strategy and it is what a signal must clear to be worth
    trading. `cost_multiplier` scales it -- 0.5 approximates always earning one
    side by resting passively, which is a different (and much harder) strategy
    than the one this signal describes, since a passive order only fills when
    the market comes to you, and it comes to you exactly when you are wrong.

    Not modelled, all of which make the real number worse: market impact, queue
    position, fees/rebates, latency between signal and fill, and partial fills.
    """
    pred = np.asarray(pred, dtype=float)
    actual = np.asarray(actual, dtype=float)
    spread_ticks = np.asarray(spread_ticks, dtype=float)

    side = np.sign(pred) * (np.abs(pred) > threshold)
    traded = side != 0
    n = int(traded.sum())
    if n == 0:
        return Backtest(0, 0.0, 0.0, 0.0, np.nan, np.nan, np.nan,
                        np.nan, np.nan, np.array([]), np.array([]))

    gross = side[traded] * actual[traded]
    cost = cost_multiplier * spread_ticks[traded]
    net = gross - cost

    return Backtest(
        n_trades=n,
        gross_ticks=float(gross.sum()),
        cost_ticks=float(cost.sum()),
        net_ticks=float(net.sum()),
        gross_per_trade=float(gross.mean()),
        cost_per_trade=float(cost.mean()),
        net_per_trade=float(net.mean()),
        hit_rate=float((gross > 0).mean()),
        # The spread at which this strategy would exactly break even: the
        # average gross edge per trade. Comparing it to the spread actually
        # quoted is the whole question, in one number.
        breakeven_spread=float(gross.mean() / cost_multiplier),
        equity_gross=np.cumsum(gross),
        equity_net=np.cumsum(net),
    )
