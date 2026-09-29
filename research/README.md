# Order Flow Imbalance: a microstructure study on top of the matching engine

The engine in this repository reconstructs a limit order book from a raw exchange message stream
and verifies it against the venue's own published book. That makes it infrastructure. This
directory uses it to ask a research question, which is different work: **does the shape of order
flow predict where the price goes next, and is the answer worth trading?**

Measured on a real NASDAQ session — every message AAPL's order book received on 21 June 2012 —
the answer is **yes, and emphatically no.** Order flow imbalance carries real out-of-sample
predictive power. It is roughly thirty times too small to pay for the spread it would have to
cross.

Two notebooks:

- [`ofi_study_real.ipynb`](ofi_study_real.ipynb) — the study, on AAPL 2012-06-21.
- [`ofi_study.ipynb`](ofi_study.ipynb) — the same pipeline on synthetic data with **known ground
  truth**. This is the validation step, and it ran first: it establishes that the estimator finds
  an injected relationship, reports approximately zero when the predictor is permuted, and that
  the forward-return alignment is what it claims to be. Every failure mode in a study like this
  produces a confident, plausible, false result, and real data offers no way to detect any of
  them because nobody knows the right answer.

---

## The question

At the close of each fixed time interval, measure *order flow imbalance* (OFI) — the net pressure
applied to the best bid and offer over that interval, in the sense of
[Cont, Kukanov & Stoikov (2014)](https://doi.org/10.1093/jjfinec/nbs014):

$$e_n = \mathbb{1}_{\{P^b_n \ge P^b_{n-1}\}} q^b_n - \mathbb{1}_{\{P^b_n \le P^b_{n-1}\}} q^b_{n-1} - \mathbb{1}_{\{P^a_n \le P^a_{n-1}\}} q^a_n + \mathbb{1}_{\{P^a_n \ge P^a_{n-1}\}} q^a_{n-1}$$

Then regress the mid-price change over the *following* interval on $\sum_n e_n$. The predictor is
measured strictly inside interval $t$; the target is composed entirely of movement after interval
$t$ closes.

## Findings — AAPL, 2012-06-21

400,390 two-sided book updates over the full 6.5-hour session, aggregated into 11,129 two-second
intervals, split chronologically 70/30 (7,790 train / 3,339 test).

### The signal is real, and small

| | out-of-sample R² |
|---|---|
| Contemporaneous (same interval) — *price impact, not a forecast* | **0.359** |
| **Predictive (next interval)** | **0.0118** |
| Permutation null, 500 trials (mean) | −0.00015 |

β = +4.41e−4 ticks per unit of OFI — the sign the economics predicts — with an empirical p-value
of 0.0000 against a permutation null sitting on zero.

The contemporaneous relationship is **thirty times stronger than the predictive one**. That gap
is the whole distinction between explaining the present and forecasting the future. Consuming the
offer both creates positive OFI and raises the mid, so the contemporaneous number is close to
mechanical; reporting it as though it were a forecast is the easiest way to oversell this work.

### It cannot pay for the spread, and not marginally

| | |
|---|---|
| Gross edge per trade | **+0.427 ticks** |
| Cost per round trip (one full spread) | 12.06 ticks |
| **Net per trade** | **−11.63 ticks** (t = −104.3, 3,339 independent trades) |
| Hit rate | 0.392 |

NASDAQ's book quoted a median spread of 15 ticks that day, which is wide. The conclusion does not
depend on it: **the gross edge is 0.427 ticks and the smallest spread that can exist is 1 tick.**
Against a hypothetical permanently one-tick market the signal still recovers only 43% of the cost
of crossing. There is no market condition in which this trades profitably as a liquidity-taking
strategy.

The hit rate is worth a second look: the signal is directionally *right* less than 40% of the
time, yet gross PnL per trade is positive, because its wins are bigger than its losses. Direction
accuracy is not edge, in either direction.

### Every horizon loses, significantly

| horizon | independent trades | net/trade | t-stat |
|---|---|---|---|
| 2 s | 3,339 | −11.63 | −104.3 |
| 4 s | 1,670 | −11.43 | −57.7 |
| 10 s | 668 | −11.64 | −25.6 |
| 20 s | 334 | −10.81 | −13.3 |
| 40 s | 167 | −8.97 | −6.2 |
| 80 s | 84 | −6.63 | −2.1 |

Trades are thinned to **disjoint holding windows** before this table is computed. Entering every
interval while holding for *h* intervals makes each trade overlap the next *h*−1, counting one
favourable move up to *h* times and inflating the apparent sample without adding information. On
the synthetic data that correction was decisive — it turned an apparently profitable long-horizon
strategy into 18 independent trades at t ≈ 1.7, too few to claim anything. Here it changes
nothing: every horizon loses and every one is significant.

### The horizon profile decays — as predicted in advance

R² falls monotonically: 0.0118 → 0.0045 → 0.0031 → 0.0005 from 2 to 20 seconds.

On the synthetic data it *rose* to a peak near 20 seconds, because that generator injects an
autocorrelated drift the visible book adjusts to slowly. That difference was written down, and
the prediction that real AAPL would decay instead was recorded, **before this notebook was run
against real data**. Confirming it is modest evidence that the pipeline measures the
data-generating process rather than itself.

### What this does not establish

Nothing about equities generally. **One ticker-day is one draw**, and 21 June 2012 was a sharply
down day — AAPL fell from about $588 to $577 — so it is a volatile sample, not a representative
one.

LOBSTER is also a **single-venue** feed. This is NASDAQ's own book; the consolidated NBBO across
all venues would have been tighter than the 15 ticks quoted here, which is exactly why the
one-tick robustness argument above carries the conclusion rather than the measured spread.

## The synthetic study, and why it came first

[`ofi_study.ipynb`](ofi_study.ipynb) runs this identical pipeline on generated data. It makes no
claim about any real security, and it was never meant to: it is the validation step.
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

Download a free sample day from [LOBSTER](https://lobsterdata.com/info/DataSamples.php) into
`data/` and open **`ofi_study_real.ipynb`**. It runs the replayer itself and needs no edits if
you have the AAPL 2012-06-21 sample; otherwise change `MSG_PATH` / `BOOK_PATH` in the config
cell. It replays with `--resync`, because neither strict nor recover mode can cross a full
real session — recover stops at message 1,965 on a level whose size drifted while it sat
outside the feed's price window. Resync corrects such levels and counts every correction;
the `corrections by depth` histogram it prints is a data-quality measure to read *before*
the results, since corrections at the touch would undermine the OFI features themselves. Everything else — the trimming, the split, the null, the cost model, the overlap
correction — is identical code to the synthetic study.

It ships **unexecuted and without conclusions**, which is deliberate. Its markdown says what
each step does and what would distinguish a finding from an artefact; the final cell prints a
summary block, and the writeup gets built from that. Deciding what the data means before seeing
it is how studies like this go wrong.

Beyond the synthetic notebook it adds three things that only matter on real data:

- **Open/close trimming**, since the first and last minutes are a different process and sit
  exactly at the edges of a chronological split. The robustness section re-runs the headline
  number untrimmed, so the choice can be seen not to be carrying the result.
- **A multiple-testing note.** It also tests signed trade flow and queue imbalance; reporting the
  best of three predictors inflates its significance, and the permutation null was computed for
  OFI specifically.
- **Robustness checks** — interval length, first half vs. second half, trimmed vs. untrimmed.
  One number from one configuration on one day is not a result.

Two caveats belong in any writeup regardless of what it produces. A single ticker-day is one
draw — enough to demonstrate method, nowhere near enough to claim generalisation. And the
deepest levels of a top-N feed are structurally unreliable, which is why the reconciliation
compares five levels rather than the ten it ingests.

## Files

| | |
|---|---|
| `ofi_study.ipynb` | The synthetic study — validation against known ground truth. Executed, with outputs. |
| `ofi_study_real.ipynb` | The same pipeline pointed at a real LOBSTER session. Ships **unexecuted**: it needs data that is not in this repository, and its conclusions are deliberately left to be written from its own output rather than assumed in advance. |
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
