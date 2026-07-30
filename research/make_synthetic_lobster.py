#!/usr/bin/env python3
"""Generate synthetic market data in LOBSTER's exact CSV format.

Why this exists
---------------
The real LOBSTER sample is not redistributed with this repository, and the
analysis in ofi_study.ipynb should not be the first time the pipeline is run
end to end. More importantly, a study that only ever sees real data has no way
to answer the question that matters most about any predictive model:

    "Would this pipeline report a signal even if there were none?"

Almost every way a microstructure study goes wrong -- lookahead in the feature
construction, a train/test split that leaks, a forward return misaligned by one
row -- produces a *confident, plausible, entirely false* result on real data,
and there is nothing in real data to check it against. So this generator
produces two datasets with known ground truth:

    --mode impact : order flow is partly informed. A latent efficient price
                    random-walks; informed participants trade toward it, which
                    makes order flow imbalance genuinely predictive of where
                    the mid price goes next. A correct pipeline MUST find a
                    positive relationship here.

    --mode uninformed
                  : no participant knows anything. Order flow is noise and
                    price moves are independent coin flips. Note carefully
                    what this mode does NOT give you: a zero R^2. Randomly
                    consuming and replenishing liquidity produces mechanical
                    reversion -- the book is pushed off and springs back -- so
                    order flow imbalance still "predicts" the next move, with
                    the opposite sign. That is the lesson: a nonzero R^2 is not
                    evidence of information. The rigorous check for a leaking
                    pipeline is the permutation test in the notebook, which
                    destroys any real relationship while preserving both
                    marginal distributions.

The output also serves as a correctness check on the C++ side: the message
stream and the published book rows are generated from the same simulated state,
so `run_lobster` must reconcile them *strictly*, with zero divergences. Two
independent implementations of order book semantics agreeing is a real check.

Usage
-----
    python3 make_synthetic_lobster.py --mode impact --messages 60000 \
        --out-prefix data/SYNTH_impact

writes  data/SYNTH_impact_message_5.csv  and  data/SYNTH_impact_orderbook_5.csv
"""

import argparse
import random
from collections import deque

# LOBSTER conventions: prices are integer dollars x 10,000.
TICK = 100                 # $0.01
START_MID = 5_850_000      # $585.00, roughly AAPL in 2012
NO_ASK = 9_999_999_999
NO_BID = -9_999_999_999

LEVELS_OUT = 5             # levels written to the orderbook file
TARGET_LEVELS = 7          # levels the simulated book tries to maintain per side

# Latent-price dynamics for --mode impact. DRIFT_PHI is the per-message
# autocorrelation of the drift (0.995 => a trend persists over a few hundred
# messages, several seconds at this message rate); ADJUST_RATE controls how
# fast the visible book converges on the latent. Slow adjustment relative to
# drift persistence is what leaves predictable structure for the study to find.
DRIFT_PHI = 0.995
DRIFT_SIGMA = 0.004
ADJUST_RATE = 0.05


class Book:
    """A minimal price-time order book, independent of the C++ engine.

    Deliberately a separate implementation: the C++ replayer reconciling
    against rows produced by this class is a differential test across two
    independently written books, in the same spirit as tests/test_differential.
    """

    def __init__(self):
        self.bids = {}          # price -> deque[[order_id, size]]
        self.asks = {}
        self.order_px = {}      # order_id -> (price, is_buy)

    def side(self, is_buy):
        return self.bids if is_buy else self.asks

    def best(self, is_buy):
        s = self.side(is_buy)
        if not s:
            return None
        return max(s) if is_buy else min(s)

    def level_size(self, price, is_buy):
        return sum(o[1] for o in self.side(is_buy).get(price, ()))

    def add(self, oid, price, size, is_buy):
        self.side(is_buy).setdefault(price, deque()).append([oid, size])
        self.order_px[oid] = (price, is_buy)

    def reduce(self, oid, qty):
        """Shrink an order, removing it (and its level) when it empties."""
        price, is_buy = self.order_px[oid]
        q = self.side(is_buy)[price]
        for entry in q:
            if entry[0] == oid:
                entry[1] -= qty
                if entry[1] <= 0:
                    q.remove(entry)
                    del self.order_px[oid]
                break
        if not q:
            del self.side(is_buy)[price]

    def oldest_at(self, price, is_buy):
        q = self.side(is_buy).get(price)
        return (q[0][0], q[0][1]) if q else None

    def depth(self, is_buy, n):
        prices = sorted(self.side(is_buy), reverse=is_buy)[:n]
        return [(p, self.level_size(p, is_buy)) for p in prices]

    def row(self):
        """One LOBSTER orderbook row: Ask1,AskSz1,Bid1,BidSz1,Ask2,..."""
        asks = self.depth(False, LEVELS_OUT)
        bids = self.depth(True, LEVELS_OUT)
        cells = []
        for i in range(LEVELS_OUT):
            if i < len(asks):
                cells += [asks[i][0], asks[i][1]]
            else:
                cells += [NO_ASK, 0]
            if i < len(bids):
                cells += [bids[i][0], bids[i][1]]
            else:
                cells += [NO_BID, 0]
        return ",".join(str(c) for c in cells)


class Simulator:
    def __init__(self, mode, seed, rng=None):
        self.mode = mode
        self.rng = rng or random.Random(seed)
        self.book = Book()
        self.messages = []
        self.rows = []
        self.next_id = 1
        self.t = 34200.0                 # 09:30:00, as LOBSTER counts seconds
        # Latent efficient price, in ticks relative to START_MID. In impact
        # mode this drifts and informed flow chases it; in null mode it never
        # moves and all flow is noise.
        self.fair_ticks = 0.0
        # Momentum in the latent price. A pure random walk would be adjusted to
        # almost entirely within the interval in which it moves, leaving order
        # flow imbalance informative about the *contemporaneous* price change
        # and nearly useless about the next one -- so a pipeline tested only
        # against it could not tell a working forward-return alignment from a
        # broken one. An autocorrelated drift, adjusted to slowly, is what makes
        # the injected relationship genuinely PREDICTIVE and therefore a real
        # test of the thing the study claims to measure.
        self.drift = 0.0

    def emit(self, event, oid, size, price, direction):
        self.t += self.rng.expovariate(50.0)      # ~50 messages/second
        self.messages.append(
            f"{self.t:.9f},{event},{oid},{size},{price},{direction}")
        self.rows.append(self.book.row())

    # --- primitive actions, each of which is exactly one LOBSTER message ---

    def act_add(self, price, size, is_buy):
        """Post a resting limit order. Returns False if it would cross.

        This guard is load-bearing. LOBSTER's message stream is post-match: a
        marketable order arrives as executions (type 4) against resting
        liquidity, never as a type-1 order resting inside the spread. Emitting a
        crossing type-1 would make the generator and any real matching engine
        disagree immediately -- the engine matches it, the generator leaves both
        sides resting -- which is exactly the divergence the C++ replayer
        reports. Aggression is expressed by act_execute(), not by adding here.
        """
        opposite = self.book.best(not is_buy)
        if opposite is not None:
            if is_buy and price >= opposite:
                return False
            if not is_buy and price <= opposite:
                return False
        oid = self.next_id
        self.next_id += 1
        self.book.add(oid, price, size, is_buy)
        self.emit(1, oid, size, price, 1 if is_buy else -1)
        return True

    def act_delete(self, price, is_buy):
        hit = self.book.oldest_at(price, is_buy)
        if not hit:
            return False
        oid, size = hit
        self.book.reduce(oid, size)
        self.emit(3, oid, size, price, 1 if is_buy else -1)
        return True

    def act_partial_cancel(self, price, is_buy):
        hit = self.book.oldest_at(price, is_buy)
        if not hit or hit[1] <= 1:
            return False
        oid, size = hit
        qty = self.rng.randint(1, size - 1)
        self.book.reduce(oid, qty)
        self.emit(2, oid, qty, price, 1 if is_buy else -1)
        return True

    def act_execute(self, is_buy):
        """Execute against the best level of `is_buy` side (a resting order).

        LOBSTER's Direction on an execution names the side of the RESTING
        order, so executing against a resting bid (is_buy=True) is a
        seller-initiated trade.
        """
        px = self.book.best(is_buy)
        if px is None:
            return False
        hit = self.book.oldest_at(px, is_buy)
        if not hit:
            return False
        oid, size = hit
        qty = size if self.rng.random() < 0.4 else max(1, self.rng.randint(1, size))
        self.book.reduce(oid, qty)
        self.emit(4, oid, qty, px, 1 if is_buy else -1)
        return True

    def act_hidden(self):
        """A hidden execution: a real trade that leaves the visible book alone."""
        bid, ask = self.book.best(True), self.book.best(False)
        if bid is None or ask is None or ask - bid < 2 * TICK:
            return False
        px = bid + TICK
        self.emit(5, 0, self.rng.randint(1, 100), px,
                  1 if self.rng.random() < 0.5 else -1)
        return True

    # --- book maintenance -------------------------------------------------

    def replenish(self):
        """Keep a plausible ladder on both sides; each add is its own message."""
        for is_buy in (True, False):
            depth = self.book.depth(is_buy, TARGET_LEVELS)
            if len(depth) >= TARGET_LEVELS:
                continue
            anchor = self.book.best(is_buy)
            if anchor is None:
                other = self.book.best(not is_buy)
                anchor = ((other - TICK) if (other is not None and is_buy)
                          else (other + TICK) if other is not None
                          else START_MID + (-TICK if is_buy else TICK))
            existing = {p for p, _ in depth}
            step = -TICK if is_buy else TICK
            px = anchor
            for _ in range(TARGET_LEVELS * 2):
                if px not in existing and px > 0:
                    self.act_add(px, self.rng.randrange(1, 11) * 100, is_buy)
                    break
                px += step

    def mid_ticks(self):
        bid, ask = self.book.best(True), self.book.best(False)
        if bid is None or ask is None:
            return None
        return ((bid + ask) / 2 - START_MID) / TICK

    # --- the main loop ----------------------------------------------------

    def step(self):
        """Emit one or more messages advancing the simulation by one decision."""
        r = self.rng.random()

        # In impact mode the latent price drifts, and informed participants
        # push the book toward it: they lift offers when the book is cheap and
        # hit bids when it is rich. That is the mechanism which makes order
        # flow imbalance predictive -- flow and future price move together
        # because both are driven by the latent.
        if self.mode == "impact":
            self.drift = DRIFT_PHI * self.drift + self.rng.gauss(0, DRIFT_SIGMA)
            self.fair_ticks += self.drift

        mid = self.mid_ticks()
        gap = 0.0 if mid is None else (self.fair_ticks - mid)

        # Probability of informed (directional) action grows with the gap, but
        # slowly: the book converges on the latent over many messages rather
        # than within one, so a gap open at the end of one interval is still
        # being closed during the next.
        informed_p = (min(0.40, abs(gap) * ADJUST_RATE)
                      if self.mode == "impact" else 0.0)

        if r < informed_p:
            buy_pressure = gap > 0
            # Informed buying shows up two ways: lifting the offer (an
            # execution), or joining the queue more aggressively (a better bid
            # that still rests inside the spread). If the spread is one tick
            # there is no room for the second, so it becomes an execution too.
            if self.rng.random() < 0.6:
                self.act_execute(is_buy=not buy_pressure)   # consume opposite side
            else:
                best = self.book.best(buy_pressure)
                if best is not None:
                    px = best + (TICK if buy_pressure else -TICK)
                    if not self.act_add(px, self.rng.randrange(1, 8) * 100,
                                        buy_pressure):
                        self.act_execute(is_buy=not buy_pressure)
        elif r < informed_p + 0.42:
            self.act_add(self._noise_price(), self.rng.randrange(1, 11) * 100,
                         self.rng.random() < 0.5)
        elif r < informed_p + 0.66:
            is_buy = self.rng.random() < 0.5
            px = self.book.best(is_buy)
            if px is not None:
                if self.rng.random() < 0.5:
                    self.act_partial_cancel(px, is_buy)
                else:
                    self.act_delete(px, is_buy)
        elif r < informed_p + 0.90:
            self.act_execute(is_buy=self.rng.random() < 0.5)
        else:
            self.act_hidden()

        # In null mode the price still moves -- it just moves for reasons
        # unrelated to order flow, so no amount of flow analysis can predict it.
        if self.mode == "uninformed" and self.rng.random() < 0.05:
            self.act_execute(is_buy=self.rng.random() < 0.5)

        self.replenish()

    def _noise_price(self):
        bid, ask = self.book.best(True), self.book.best(False)
        base = START_MID if bid is None or ask is None else (bid + ask) // 2
        offset = self.rng.randint(-TARGET_LEVELS, TARGET_LEVELS) * TICK
        return max(TICK, (base + offset) // TICK * TICK)

    def run(self, n_messages):
        # Seed an opening ladder so the book is two-sided from the start.
        for i in range(TARGET_LEVELS):
            self.act_add(START_MID - (i + 1) * TICK,
                         self.rng.randrange(1, 11) * 100, True)
            self.act_add(START_MID + (i + 1) * TICK,
                         self.rng.randrange(1, 11) * 100, False)
        while len(self.messages) < n_messages:
            self.step()
        # Trim to exactly n_messages so message/book files stay aligned.
        self.messages = self.messages[:n_messages]
        self.rows = self.rows[:n_messages]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mode", choices=["impact", "uninformed"], default="impact")
    ap.add_argument("--messages", type=int, default=60000)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--out-prefix", default="data/SYNTH")
    args = ap.parse_args()

    sim = Simulator(args.mode, args.seed)
    sim.run(args.messages)

    msg_path = f"{args.out_prefix}_message_{LEVELS_OUT}.csv"
    book_path = f"{args.out_prefix}_orderbook_{LEVELS_OUT}.csv"
    with open(msg_path, "w") as f:
        f.write("\n".join(sim.messages) + "\n")
    with open(book_path, "w") as f:
        f.write("\n".join(sim.rows) + "\n")

    print(f"mode={args.mode} seed={args.seed}")
    print(f"  {len(sim.messages)} messages -> {msg_path}")
    print(f"  {len(sim.rows)} book rows -> {book_path}")
    print(f"  final latent (ticks): {sim.fair_ticks:.2f}   "
          f"final mid (ticks): {sim.mid_ticks()}")
    print("\nVerify with the C++ engine (must reconcile strictly, 0 divergences):")
    print(f"  ./build/run_lobster {msg_path} {book_path} {LEVELS_OUT}")


if __name__ == "__main__":
    main()
