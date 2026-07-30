#pragma once

// Feature emission for microstructure research.
//
// The reconciliation in LobsterReplay.hpp answers "is the reconstructed book
// correct?". Once the answer is yes, the same replay can answer a different
// kind of question: does the shape of the book predict where the price goes
// next? This header turns the replay into a feature generator — one row per
// message, carrying the state of the book that message produced.
//
// The division of labour is deliberate. C++ emits only what it is uniquely
// positioned to compute: the reconstructed book state at every update, and the
// quantities that need the *previous* update to define (order flow imbalance).
// Everything downstream — aggregation, forward returns, regression, costs —
// belongs in the analysis layer where it can be inspected, not buried in a
// binary. See research/ for that half.
//
// Reference for the OFI construction:
//   Cont, Kukanov & Stoikov (2014), "The Price Impact of Order Book Events",
//   Journal of Financial Econometrics 12(1).

#include "OrderBook.hpp"

#include <cstdint>
#include <functional>
#include <ostream>
#include <string>

namespace lobster {

// One observation: the book immediately after message `msg_index` was applied.
struct FeatureRow {
    uint64_t msg_index = 0;     // 1-based position in the message file
    double   time = 0.0;        // seconds after midnight (LOBSTER column 1)
    int      event = 0;         // LOBSTER event type that produced this state
    int      direction = 0;     // that message's direction field
    uint32_t msg_size = 0;
    uint32_t msg_price = 0;

    // Top of book, from our reconstruction. Prices are LOBSTER integer units
    // (dollars x 10,000). Zero size means that side is empty.
    uint32_t bid_px = 0, ask_px = 0;
    uint64_t bid_sz = 0, ask_sz = 0;

    // Summed visible volume over the top DEPTH_LEVELS levels of each side.
    uint64_t bid_depth_vol = 0, ask_depth_vol = 0;

    // Cont-Kukanov-Stoikov order flow imbalance increment for this update.
    // Meaningful only when quote_valid is 1 (see below).
    int64_t ofi = 0;

    // Signed executed volume: positive = buyer-initiated, negative =
    // seller-initiated. Zero when this message is not an execution.
    int64_t signed_trade_sz = 0;     // event 4, visible executions
    int64_t signed_hidden_sz = 0;    // event 5, hidden executions

    // 0 when either side of the book was empty on this update or the previous
    // one, which makes both the OFI increment and the mid-price undefined.
    // Rows are still emitted so the message stream stays complete; the analysis
    // layer filters on this rather than silently inheriting a stale quote.
    int quote_valid = 0;
};

constexpr size_t DEPTH_LEVELS = 5;

// Accumulates the previous top-of-book so each update's OFI can be formed.
//
// OFI measures net pressure at the touch, counting a quote's full size when it
// improves or holds its price and removing the prior size when it worsens or
// holds — so a bid that steps up adds its whole size, and a bid that steps down
// subtracts the size it replaced. Summing e_n over an interval gives the
// interval's imbalance, which is the regressor in the CKS study.
class OfiTracker {
public:
    // Feed the current top of book; returns the OFI increment for this update.
    // `valid` reports whether both this update and the previous one had two
    // live sides, which is what the definition requires.
    int64_t update(uint32_t bid_px, uint64_t bid_sz,
                   uint32_t ask_px, uint64_t ask_sz, bool& valid) {
        bool both_sides = bid_sz > 0 && ask_sz > 0;
        valid = both_sides && have_prev_;

        int64_t e = 0;
        if (valid) {
            // Bid side: gaining size at a price at least as good as before adds
            // pressure; losing the old level removes it.
            if (bid_px >= prev_bid_px_) e += static_cast<int64_t>(bid_sz);
            if (bid_px <= prev_bid_px_) e -= static_cast<int64_t>(prev_bid_sz_);
            // Ask side enters with the opposite sign: ask liquidity arriving is
            // downward pressure.
            if (ask_px <= prev_ask_px_) e -= static_cast<int64_t>(ask_sz);
            if (ask_px >= prev_ask_px_) e += static_cast<int64_t>(prev_ask_sz_);
        }

        if (both_sides) {
            prev_bid_px_ = bid_px; prev_bid_sz_ = bid_sz;
            prev_ask_px_ = ask_px; prev_ask_sz_ = ask_sz;
            have_prev_ = true;
        } else {
            have_prev_ = false;   // a one-sided book breaks the chain
        }
        return e;
    }

private:
    uint32_t prev_bid_px_ = 0, prev_ask_px_ = 0;
    uint64_t prev_bid_sz_ = 0, prev_ask_sz_ = 0;
    bool have_prev_ = false;
};

// Where emitted rows go. A callback rather than a file handle so tests can
// capture rows in memory; the CLI wires it to a CSV writer below.
using FeatureSink = std::function<void(const FeatureRow&)>;

inline const char* feature_csv_header() {
    return "msg_index,time,event,direction,msg_size,msg_price,"
           "bid_px,bid_sz,ask_px,ask_sz,bid_depth_vol,ask_depth_vol,"
           "ofi,signed_trade_sz,signed_hidden_sz,quote_valid";
}

inline void write_feature_csv(std::ostream& os, const FeatureRow& r) {
    // LOBSTER timestamps are seconds after midnight with nanosecond precision
    // (e.g. 34200.017460092). A default-formatted ostream gives six significant
    // digits, which for a number that size leaves a single decimal place and
    // silently destroys any sub-second time bucketing downstream.
    const std::ios_base::fmtflags flags = os.flags();
    const std::streamsize prec = os.precision();
    os << r.msg_index << ',';
    os << std::fixed;
    os.precision(9);
    os << r.time;
    os.flags(flags);
    os.precision(prec);
    os << ',' << r.event << ',' << r.direction
       << ',' << r.msg_size << ',' << r.msg_price << ',' << r.bid_px << ','
       << r.bid_sz << ',' << r.ask_px << ',' << r.ask_sz << ','
       << r.bid_depth_vol << ',' << r.ask_depth_vol << ',' << r.ofi << ','
       << r.signed_trade_sz << ',' << r.signed_hidden_sz << ','
       << r.quote_valid << '\n';
}

// Signed executed volume from a LOBSTER execution message.
//
// The sign convention is the one place this is easy to get backwards, and
// getting it backwards flips the sign of every result downstream. LOBSTER's
// Direction field on an execution describes the side of the *resting limit
// order that was executed*, not the side of the aggressor. So Direction = 1
// (a resting BUY order was hit) means somebody sold into it — a seller-
// initiated trade, which is negative signed volume. Hence the negation.
inline int64_t signed_execution_size(int direction, uint32_t size) {
    return -static_cast<int64_t>(direction) * static_cast<int64_t>(size);
}

}  // namespace lobster
