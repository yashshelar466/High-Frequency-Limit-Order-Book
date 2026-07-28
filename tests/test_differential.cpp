// Randomized differential test.
//
// Fires a long stream of random ADD / CANCEL / duplicate-ID / crossing
// operations at both the production OrderBook and a deliberately simple,
// obviously-correct reference model built on std::map, then asserts the two
// agree on top-of-book AND full per-level FIFO/volume state.
//
// The reference trades all of the production engine's performance tricks
// (fixed price arrays, intrusive lists, memory pool, incremental best-price
// tracking) for clarity, so any divergence points at a bug in the fast path.
// This is the kind of property/differential test that catches entire classes
// of matching-engine bugs — including stale best-price tracking and duplicate
// order-ID handling — that hand-written example tests tend to miss.

#include "../include/OrderBook.hpp"

#include "check.hpp"
#include <cstdint>
#include <iostream>
#include <list>
#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr uint32_t MAX_PRICE_LEVELS = 100000;  // must match OrderBook
constexpr uint32_t EMPTY_ASK = 0xFFFFFFFF;     // OrderBook's empty-ask sentinel
constexpr uint32_t EMPTY_BID = 0;              // OrderBook's empty-bid sentinel

// A straightforward, allocation-happy order book used purely as an oracle.
// Bids are keyed descending so begin() is the best (highest) bid; asks ascending
// so begin() is the best (lowest) ask. Each price maps to a FIFO of resting
// orders, exactly mirroring price-time priority.
class ReferenceBook {
public:
    struct Entry {
        uint64_t id;
        uint32_t qty;
    };
    using Level = std::list<Entry>;

    std::map<uint32_t, Level, std::greater<uint32_t>> bids;  // highest first
    std::map<uint32_t, Level> asks;                          // lowest first
    std::unordered_map<uint64_t, std::pair<uint32_t, bool>> live;  // id -> (price, is_buy)
    std::vector<Trade> trades;  // fills produced by the most recent operation

    void insert_order(uint64_t id, uint32_t price, uint32_t qty, bool is_buy) {
        insert_order(id, price, qty, is_buy, OrderType::LIMIT);
    }

    void insert_order(uint64_t id, uint32_t price, uint32_t qty, bool is_buy, OrderType type) {
        if (qty == 0) return;
        if (type != OrderType::MARKET && (price == EMPTY_BID || price == EMPTY_ASK)) return;
        if (live.count(id)) return;  // duplicate live ID rejected

        uint32_t limit = price;
        if (type == OrderType::MARKET) limit = is_buy ? EMPTY_ASK : EMPTY_BID;
        if (type == OrderType::FOK && !can_fully_fill(limit, qty, is_buy)) return;

        uint32_t remaining = match(id, limit, qty, is_buy);

        if (remaining > 0 && type == OrderType::LIMIT) {
            if (is_buy) {
                bids[price].push_back({id, remaining});
                live[id] = {price, true};
            } else {
                asks[price].push_back({id, remaining});
                live[id] = {price, false};
            }
        }
    }

    // Match an aggressor against the opposite side up to `limit`; return leftover.
    uint32_t match(uint64_t taker_id, uint32_t limit, uint32_t qty, bool is_buy) {
        if (is_buy) {
            while (qty > 0 && !asks.empty() && asks.begin()->first <= limit) {
                uint32_t lvl_price = asks.begin()->first;
                Level& fifo = asks.begin()->second;
                match_into(fifo, qty, taker_id, lvl_price, true);
                if (fifo.empty()) asks.erase(asks.begin());
            }
        } else {
            while (qty > 0 && !bids.empty() && bids.begin()->first >= limit) {
                uint32_t lvl_price = bids.begin()->first;
                Level& fifo = bids.begin()->second;
                match_into(fifo, qty, taker_id, lvl_price, false);
                if (fifo.empty()) bids.erase(bids.begin());
            }
        }
        return qty;
    }

    bool can_fully_fill(uint32_t limit, uint32_t qty, bool is_buy) const {
        uint64_t available = 0;
        if (is_buy) {
            for (const auto& kv : asks) {
                if (kv.first > limit) break;
                for (const auto& e : kv.second) available += e.qty;
                if (available >= qty) return true;
            }
        } else {
            for (const auto& kv : bids) {
                if (kv.first < limit) break;
                for (const auto& e : kv.second) available += e.qty;
                if (available >= qty) return true;
            }
        }
        return available >= qty;
    }

    void cancel_order(uint64_t id) {
        auto it = live.find(id);
        if (it == live.end()) return;
        uint32_t price = it->second.first;
        bool is_buy = it->second.second;

        if (is_buy) {
            erase_from_level(bids, price, id);
        } else {
            erase_from_level(asks, price, id);
        }
        live.erase(it);
    }

    void amend_order(uint64_t id, uint32_t new_price, uint32_t new_qty) {
        auto it = live.find(id);
        if (it == live.end()) return;
        if (new_qty == 0 || new_price == EMPTY_BID || new_price == EMPTY_ASK) return;
        uint32_t price = it->second.first;
        bool is_buy = it->second.second;

        // In-place reduction at the same price keeps the FIFO position.
        Entry* e = find_entry(is_buy, price, id);
        if (new_price == price && e != nullptr && new_qty <= e->qty) {
            e->qty = new_qty;
            return;
        }

        // Otherwise cancel-replace: re-enter as a fresh aggressor.
        cancel_order(id);
        insert_order(id, new_price, new_qty, is_buy);
    }

    uint32_t best_bid() const { return bids.empty() ? EMPTY_BID : bids.begin()->first; }
    uint32_t best_ask() const { return asks.empty() ? EMPTY_ASK : asks.begin()->first; }

private:
    // Fill an incoming aggressor (remaining size `qty`) against the front of a
    // resting FIFO until either the aggressor is exhausted or the level empties.
    // A fully-filled resting order must leave `live` too, so its id becomes
    // reusable — exactly as the fast engine frees it from order_map.
    void match_into(Level& fifo, uint32_t& qty, uint64_t taker_id,
                    uint32_t price, bool taker_is_buy) {
        while (qty > 0 && !fifo.empty()) {
            Entry& front = fifo.front();
            uint32_t fill = std::min(qty, front.qty);
            qty -= fill;
            front.qty -= fill;
            trades.push_back(Trade{taker_id, front.id, price, fill, taker_is_buy});
            if (front.qty == 0) {
                live.erase(front.id);
                fifo.pop_front();
            }
        }
    }

    // Locate a resting order by id within its price level (linear scan — the
    // reference trades speed for clarity). Returns nullptr if absent.
    Entry* find_entry(bool is_buy, uint32_t price, uint64_t id) {
        if (is_buy) {
            auto lvl = bids.find(price);
            if (lvl == bids.end()) return nullptr;
            for (auto& e : lvl->second) if (e.id == id) return &e;
        } else {
            auto lvl = asks.find(price);
            if (lvl == asks.end()) return nullptr;
            for (auto& e : lvl->second) if (e.id == id) return &e;
        }
        return nullptr;
    }

    template <typename Map>
    static void erase_from_level(Map& side, uint32_t price, uint64_t id) {
        auto lvl = side.find(price);
        if (lvl == side.end()) return;
        Level& fifo = lvl->second;
        for (auto e = fifo.begin(); e != fifo.end(); ++e) {
            if (e->id == id) {
                fifo.erase(e);
                break;
            }
        }
        if (fifo.empty()) side.erase(lvl);
    }
};

// Compare a single side's price level between the fast engine and the oracle:
// existence, order count, aggregate volume, and the exact FIFO id/qty sequence.
bool level_matches(const PriceLevel* fast, const ReferenceBook::Level* ref,
                   uint32_t price, const char* side, std::string& err) {
    bool fast_live = (fast != nullptr && fast->head != nullptr);
    bool ref_live = (ref != nullptr && !ref->empty());

    if (fast_live != ref_live) {
        err = std::string(side) + " level " + std::to_string(price) +
              " existence mismatch (fast=" + std::to_string(fast_live) +
              " ref=" + std::to_string(ref_live) + ")";
        return false;
    }
    if (!fast_live) return true;

    if (fast->order_count != ref->size()) {
        err = std::string(side) + " level " + std::to_string(price) +
              " order_count mismatch (fast=" + std::to_string(fast->order_count) +
              " ref=" + std::to_string(ref->size()) + ")";
        return false;
    }

    uint64_t ref_vol = 0;
    for (const auto& e : *ref) ref_vol += e.qty;
    if (fast->total_volume != ref_vol) {
        err = std::string(side) + " level " + std::to_string(price) +
              " volume mismatch (fast=" + std::to_string(fast->total_volume) +
              " ref=" + std::to_string(ref_vol) + ")";
        return false;
    }

    const Order* o = fast->head;
    auto it = ref->begin();
    size_t pos = 0;
    while (o != nullptr && it != ref->end()) {
        if (o->id != it->id || o->qty != it->qty) {
            err = std::string(side) + " level " + std::to_string(price) +
                  " FIFO mismatch at pos " + std::to_string(pos) +
                  " (fast id=" + std::to_string(o->id) + " qty=" + std::to_string(o->qty) +
                  " vs ref id=" + std::to_string(it->id) + " qty=" + std::to_string(it->qty) + ")";
            return false;
        }
        o = o->next;
        ++it;
        ++pos;
    }
    return true;
}

// Compare the entire active price band between the two books.
bool full_compare(const OrderBook& fast, const ReferenceBook& ref,
                  uint32_t pmin, uint32_t pmax, std::string& err) {
    if (fast.get_best_bid() != ref.best_bid()) {
        err = "best_bid mismatch (fast=" + std::to_string(fast.get_best_bid()) +
              " ref=" + std::to_string(ref.best_bid()) + ")";
        return false;
    }
    if (fast.get_best_ask() != ref.best_ask()) {
        err = "best_ask mismatch (fast=" + std::to_string(fast.get_best_ask()) +
              " ref=" + std::to_string(ref.best_ask()) + ")";
        return false;
    }

    for (uint32_t p = pmin; p <= pmax; ++p) {
        auto rb = ref.bids.find(p);
        const ReferenceBook::Level* rbl = (rb == ref.bids.end()) ? nullptr : &rb->second;
        if (!level_matches(fast.get_bid_level(p), rbl, p, "bid", err)) return false;

        auto ra = ref.asks.find(p);
        const ReferenceBook::Level* ral = (ra == ref.asks.end()) ? nullptr : &ra->second;
        if (!level_matches(fast.get_ask_level(p), ral, p, "ask", err)) return false;
    }
    return true;
}

}  // namespace

int main() {
    // Deterministic seed so a failure is exactly reproducible.
    std::mt19937_64 rng(42);

    // Narrow price band => frequent crossings and real book depth, and a cheap
    // full-book comparison (only ~30 prices to scan).
    constexpr uint32_t PMIN = 100;
    constexpr uint32_t PMAX = 130;
    constexpr int NUM_OPS = 300000;
    constexpr int FULL_COMPARE_EVERY = 1000;

    std::uniform_int_distribution<uint32_t> price_dist(PMIN, PMAX);
    std::uniform_int_distribution<uint32_t> qty_dist(1, 50);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int> op_dist(0, 99);

    OrderBook fast;
    ReferenceBook ref;
    std::vector<uint64_t> used_ids;  // for cancel targeting + duplicate-ID attempts
    uint64_t next_id = 1;

    // Capture the fast engine's fills for the current op so they can be
    // compared against the reference model's fills op-for-op.
    std::vector<Trade> fast_trades;
    fast.set_trade_handler([&](const Trade& t) { fast_trades.push_back(t); });

    std::cout << "--- Running Differential Test (" << NUM_OPS
              << " ops vs reference model) ---" << std::endl;

    for (int i = 0; i < NUM_OPS; ++i) {
        int roll = op_dist(rng);

        fast_trades.clear();
        ref.trades.clear();

        if (roll < 20 && !used_ids.empty()) {
            // Cancel a previously-seen id (may already be filled/cancelled — the
            // no-op path is worth exercising and both books must agree on it).
            uint64_t id = used_ids[rng() % used_ids.size()];
            fast.cancel_order(id);
            ref.cancel_order(id);
        } else if (roll < 35 && !used_ids.empty()) {
            // Amend a previously-seen id to a random new price/qty, exercising
            // both the in-place-reduction and cancel-replace (possibly crossing)
            // paths.
            uint64_t id = used_ids[rng() % used_ids.size()];
            uint32_t new_price = price_dist(rng);
            uint32_t new_qty = qty_dist(rng);
            fast.amend_order(id, new_price, new_qty);
            ref.amend_order(id, new_price, new_qty);
        } else {
            // Insert. 10% of inserts deliberately reuse a live-ish id to probe
            // duplicate-ID handling; the rest use a fresh monotonic id.
            uint64_t id;
            if (roll >= 90 && !used_ids.empty()) {
                id = used_ids[rng() % used_ids.size()];
            } else {
                id = next_id++;
                used_ids.push_back(id);
            }
            uint32_t price = price_dist(rng);
            uint32_t qty = qty_dist(rng);
            bool is_buy = side_dist(rng) == 1;

            // Mix in the non-resting order types so their matching, kill, and
            // remainder-discard behavior is checked against the reference too.
            OrderType type = OrderType::LIMIT;
            switch (rng() % 10) {
                case 0: type = OrderType::IOC; break;
                case 1: type = OrderType::FOK; break;
                case 2: type = OrderType::MARKET; break;
                default: type = OrderType::LIMIT; break;
            }

            fast.insert_order(id, price, qty, is_buy, type);
            ref.insert_order(id, price, qty, is_buy, type);
        }

        // Cheap invariant checked on every single op.
        if (fast.get_best_bid() != ref.best_bid() || fast.get_best_ask() != ref.best_ask()) {
            std::cerr << "TOP-OF-BOOK mismatch at op " << i
                      << " (fast bid=" << fast.get_best_bid()
                      << " ask=" << fast.get_best_ask()
                      << " | ref bid=" << ref.best_bid()
                      << " ask=" << ref.best_ask() << ")" << std::endl;
            CHECK(false && "top-of-book divergence");
        }

        // The trade stream this op produced must match the reference exactly,
        // fill for fill, in order.
        if (fast_trades.size() != ref.trades.size()) {
            std::cerr << "TRADE COUNT mismatch at op " << i
                      << " (fast=" << fast_trades.size()
                      << " ref=" << ref.trades.size() << ")" << std::endl;
            CHECK(false && "trade count divergence");
        }
        for (size_t k = 0; k < fast_trades.size(); ++k) {
            const Trade& a = fast_trades[k];
            const Trade& b = ref.trades[k];
            if (a.taker_id != b.taker_id || a.maker_id != b.maker_id ||
                a.price != b.price || a.qty != b.qty || a.taker_is_buy != b.taker_is_buy) {
                std::cerr << "TRADE mismatch at op " << i << " fill " << k
                          << " (fast taker=" << a.taker_id << " maker=" << a.maker_id
                          << " px=" << a.price << " qty=" << a.qty
                          << " | ref taker=" << b.taker_id << " maker=" << b.maker_id
                          << " px=" << b.price << " qty=" << b.qty << ")" << std::endl;
                CHECK(false && "trade divergence");
            }
        }

        // Expensive full-state comparison periodically.
        if ((i + 1) % FULL_COMPARE_EVERY == 0) {
            std::string err;
            if (!full_compare(fast, ref, PMIN, PMAX, err)) {
                std::cerr << "FULL-BOOK mismatch at op " << i << ": " << err << std::endl;
                CHECK(false && "full-book divergence");
            }
        }
    }

    // Final exhaustive comparison.
    std::string err;
    if (!full_compare(fast, ref, PMIN, PMAX, err)) {
        std::cerr << "FINAL full-book mismatch: " << err << std::endl;
        CHECK(false && "final full-book divergence");
    }

    std::cout << "[PASS] Differential test: " << NUM_OPS
              << " ops matched the reference model exactly (book state + trade stream)."
              << std::endl;
    return 0;
}
