# Order Flow Imbalance: a microstructure study on top of the matching engine

The engine in this repository reconstructs a limit order book from a raw exchange message
stream and verifies it against the venue's own published book. That makes it infrastructure.
This directory uses it to ask a research question, which is a different kind of work: **does
the shape of order flow predict where the price goes next, and is the answer worth trading?**

The short version: **yes, and no** — with the "no" resting on a correction that only shows up
if you look for it.

---

## The question

At the close of each fixed time interval, measure *order flow imbalance* (OFI) — the net
pressure applied to the best bid and offer over that interval, in the sense of
[Cont, Kukanov & Stoikov (2014)](https://doi.org/10.1093/jjfinec/nbs014):

$$e_n = \mathbb{1}_{\{P^b_n \ge P^b_{n-1}\}} q^b_n - \mathbb{1}_{\{P^b_n \le P^b_{n-1}\}} q^b_{n-1} - \mathbb{1}_{\{P^a_n \le P^a_{n-1}\}} q^a_n + \mathbb{1}_{\{P^a_n \ge P^a_{n-1}\}} q^a_{n-1}$$

Then regress the mid-price change over the *following* interval on $\sum_n e_n$. The predictor
is measured strictly inside interval $t$; the target is composed entirely of movement after
interval $t$ closes.

## Findings

Measured on 60,000 messages (20 minutes of simulated session, 599 two-second intervals,
chronological 70/30 train/test split).

| | out-of-sample R² |
|---|---|
| Contemporaneous (same interval) — *price impact, not a forecast* | 0.251 |
| **Predictive (next interval)** | **0.090** |
| Permutation null, 500 trials (mean / 95th pct) | −0.003 / 0.008 |

The predictive result is real: empirical p-value 0.0000 against the permutation null, and the
horizon profile is smooth and single-peaked rather than spiking at one convenient lag.

It is also about a third the strength of the contemporaneous relationship. Consuming the offer
both creates positive OFI and raises the mid, so the contemporaneous number is close to
mechanical. Reporting it as though it were a forecast is the easiest way to oversell this
analysis, and it is done constantly.

### The signal does not pay for the spread

A round trip crosses the spread twice, so it pays one full spread before the prediction has to
be right about anything.

| threshold (ticks) | trades | hit rate | gross/trade | cost/trade | **net/trade** |
|---|---|---|---|---|---|
| 0.00 | 180 | 0.678 | +0.733 | 2.136 | **−1.403** |
| 0.10 | 172 | 0.686 | +0.765 | 2.055 | **−1.290** |
| 0.50 | 137 | 0.715 | +0.964 | 1.709 | **−0.746** |

The hit rate is 68% and the strategy still loses on every threshold, which is the whole lesson
about direction accuracy as a metric. **Breakeven spread: 0.733 ticks against 2.136 quoted** —
the spread would have to fall by two thirds.

### The correction that changes the conclusion

The cost is paid once per round trip, but the gross edge keeps growing with the holding period.
Hold longer and the net turns positive:

| horizon | trades | gross/trade | net/trade |
|---|---|---|---|
| 2 s | 180 | +0.73 | −1.40 |
| 10 s | 179 | +3.24 | **+1.10** |
| 20 s | 177 | +5.61 | **+3.46** |

That table is where a careless writeup declares a profitable strategy. It is double counting.
Entering every interval while holding for $h$ intervals means each trade overlaps the next
$h-1$; one favourable move is counted up to $h$ times. Thinning to disjoint holding windows and
attaching a $t$-statistic:

| horizon | **independent** trades | net/trade | t-stat |
|---|---|---|---|
| 2 s | 180 | −1.40 | **−7.55** |
| 4 s | 90 | −0.74 | **−2.10** |
| 10 s | 36 | +1.48 | 1.45 |
| 20 s | 18 | +3.91 | 1.70 |
| 40 s | 9 | +5.10 | 0.71 |

At the horizons with enough independent trades to support a claim, the strategy loses
decisively. Where it appears to win, 9–36 trades at $t \approx 1.5$ cannot distinguish an edge
from luck.

Note the asymmetry: **the losing result is the robust one and the winning result is the fragile
one.** Short horizons give many independent observations and a clean verdict; long horizons give
a flattering average and almost no statistical power. A backtest reporting only the second
misstates nothing in particular and is still worthless.

### A nonzero R² is not evidence of information

The `uninformed` control has no informed participants at all — flow is noise. It still produces
an out-of-sample R² of 0.068, with the **opposite sign** (β = −3.4e−4 vs +1.2e−4), purely from
mechanical liquidity replenishment: the book gets pushed off and springs back. Any study that
reports a coefficient without checking that its sign matches the economic story it claims could
be picking up exactly this.

---

## What the data is, and what it is not

**The data here is synthetic.** No claim in this directory is evidence about any real security.

That is partly circumstance — the free LOBSTER sample is not redistributable and the environment
this was built in had no network access to it — but starting synthetic would be right regardless.
Every serious failure mode in a study like this (a forward return overlapping its own predictor,
a leaking split, a flipped sign) produces a confident, plausible, entirely false result, and
real data offers no way to detect any of them because nobody knows the right answer. Synthetic
data with known ground truth does:

- **`--mode impact`** — a latent price drifts with momentum; informed participants trade toward
  it faster than the book adjusts. OFI is genuinely predictive. A correct pipeline must find it.
- **`--mode uninformed`** — nobody knows anything. Used as the control above.

The generator writes LOBSTER-format files from an order book implementation written
independently of the C++ engine, so replaying them is also a **differential test**: 60,000
messages reconcile with zero divergences, which means two separately written books agree at
every step. A divergence would mean one of them is wrong — and during development, one was
(the generator was posting limit orders that crossed the book, which a real venue would have
matched; the engine caught it immediately).

One caveat specific to the horizon result: the R² *rises* before it falls, peaking near 20
seconds. On real equity data OFI's predictive power typically decays monotonically within a few
seconds. The rise here is a property of the injected drift, not a discovery about markets.

## Reproducing

```bash
# from the repo root, with the project built
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
pip install numpy pandas matplotlib jupyter

python3 research/test_ofi_lib.py                       # test the analysis code first
jupyter notebook research/ofi_study.ipynb              # generates data and runs end to end
```

The notebook regenerates its own data (deterministic seeds), so the committed outputs are
reproducible from a clean checkout. Generated CSVs are gitignored.

## Running it on real market data

The pipeline is data-agnostic. Download a free sample day from
[LOBSTER](https://lobsterdata.com/info/DataSamples.php) into `data/`, then:

```bash
./build/run_lobster \
    data/AAPL_2012-06-21_34200000_57600000_message_10.csv \
    data/AAPL_2012-06-21_34200000_57600000_orderbook_10.csv \
    5 --recover --emit-features data/features_aapl.csv
```

Point `load_features` at the result and re-run the notebook. Use `--recover`: a top-10 feed is a
windowed view of the book, so a strict run stops at the first level the message stream cannot
explain (see the main README's reconciliation section), which for that session is early.

Two caveats to carry into any real-data result. A single ticker-day is one draw — enough to
demonstrate method, nowhere near enough to claim generalisation. And the deepest levels of a
top-N feed are structurally unreliable, so features built from them are noisier than they look.

## Files

| | |
|---|---|
| `ofi_study.ipynb` | The study. Executed, with outputs. |
| `ofi_lib.py` | Panel construction, forward returns, out-of-sample fitting, permutation null, backtest with costs. |
| `test_ofi_lib.py` | Tests for the above — forward-return alignment, no backward leakage, chronological-split enforcement, cost arithmetic. |
| `make_synthetic_lobster.py` | LOBSTER-format generator with known ground truth, on an independent book implementation. |

The C++ side contributes `--emit-features` on `run_lobster` (see `include/FeatureEmit.hpp`),
which emits per-update book state and the OFI increment. The division is deliberate: C++ computes
only what needs the previous update to define, and everything else — aggregation, returns,
regression, costs — lives here where it can be read and tested.

### Why the analysis code has its own test suite

The two errors that most often manufacture a fake result are a forward return that overlaps its
predictor and an evaluation that lets the model see the test period. Both are three-line
mistakes, both survive review easily, and both make the output *better* — which is exactly why
nobody catches them. `test_ofi_lib.py` checks them against series whose right answer is known by
construction, including a case where the relationship flips sign in the holdout: a chronological
split must score strongly negative there, while a shuffled split would score well. That test is
the one that would catch the single most damaging bug in this kind of work.
