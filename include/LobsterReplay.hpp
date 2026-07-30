#pragma once

// LOBSTER market-data replay and book reconciliation.
//
// Replays a real trading day's message stream through the matching engine and
// checks the reconstructed book against the venue's own published order book,
// message by message. This extends the differential-testing idea from
// tests/test_differential.cpp — where the oracle is a simple reference model —
// to a far stronger oracle: actual exchange data.
//
// LOBSTER (lobsterdata.com) publishes two aligned CSVs per ticker-day:
//
//   message file:   Time, EventType, OrderID, Size, Price, Direction
//   orderbook file: AskPrice1, AskSize1, BidPrice1, BidSize1, AskPrice2, ...
//
// Row k of the orderbook file is the state of the book *after* message k, so
// the two files have the same row count and are walked in lockstep.
//
// The logic lives in this header (rather than in the executable's main) so it
// can be driven directly by tests against synthetic fixtures — see
// tests/test_lobster_replay.cpp.

#include "OrderBook.hpp"

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace lobster {

// LOBSTER event types (column 2 of the message file).
enum class Event {
    NEW_LIMIT = 1,       // new limit order
    PARTIAL_CANCEL = 2,  // partial cancellation (size reduction)
    FULL_DELETE = 3,     // total deletion
    EXEC_VISIBLE = 4,    // execution of a visible limit order
    EXEC_HIDDEN = 5,     // execution of a hidden order (not on the visible book)
    CROSS = 6,           // cross/auction trade
    HALT = 7,            // trading halt indicator
};

struct Message {
    double time = 0.0;
    int event = 0;
    uint64_t order_id = 0;
    uint32_t size = 0;
    uint32_t price = 0;   // LOBSTER prices are integer dollars x 10,000
    int direction = 0;    // 1 = buy/bid, -1 = sell/ask
};

// LOBSTER pads absent levels with these sentinels and a size of 0.
constexpr int64_t NO_ASK = 9999999999LL;
constexpr int64_t NO_BID = -9999999999LL;

// One side of the published book at a single level.
struct PubLevel {
    int64_t price = 0;
    int64_t size = 0;
    bool present = false;   // false when LOBSTER padded this level
};

struct Stats {
    uint64_t messages = 0;
    uint64_t skipped_hidden = 0;
    uint64_t skipped_cross = 0;
    uint64_t skipped_halt = 0;
    uint64_t unknown_refs = 0;      // delete/execute naming an order we never saw
    uint64_t seed_attributed = 0;   // unknown id resolved against seeded liquidity
    uint64_t unexpected_trades = 0; // engine matched — should never happen
};

// Seeded pre-existing liquidity, keyed by (is_buy, price) -> synthetic order id.
// Orders resting before the capture window have real exchange IDs we cannot
// know, so we stand in for them with synthetic IDs; this index lets a later
// message that references one of those real IDs still be attributed to the
// right aggregate. See attribute_to_seed().
using SeedIndex = std::map<std::pair<bool, uint32_t>, uint64_t>;

inline std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> out;
    std::stringstream ss(line);
    std::string cell;
    while (std::getline(ss, cell, ',')) out.push_back(cell);
    return out;
}

inline bool parse_message(const std::string& line, Message& m) {
    auto f = split_csv(line);
    if (f.size() < 6) return false;
    m.time = std::stod(f[0]);
    m.event = std::stoi(f[1]);
    m.order_id = std::stoull(f[2]);
    m.size = static_cast<uint32_t>(std::stoul(f[3]));
    m.price = static_cast<uint32_t>(std::stoll(f[4]));
    m.direction = std::stoi(f[5]);
    return true;
}

// Parse an orderbook row into `levels` ask levels and `levels` bid levels.
// Columns interleave as Ask1,AskSize1,Bid1,BidSize1,Ask2,... best-first.
inline bool parse_book_row(const std::string& line, size_t levels,
                           std::vector<PubLevel>& asks, std::vector<PubLevel>& bids) {
    auto f = split_csv(line);
    if (f.size() < levels * 4) return false;
    asks.clear();
    bids.clear();
    for (size_t i = 0; i < levels; ++i) {
        int64_t ap = std::stoll(f[i * 4 + 0]);
        int64_t as = std::stoll(f[i * 4 + 1]);
        int64_t bp = std::stoll(f[i * 4 + 2]);
        int64_t bs = std::stoll(f[i * 4 + 3]);
        asks.push_back(PubLevel{ap, as, ap != NO_ASK && as > 0});
        bids.push_back(PubLevel{bp, bs, bp != NO_BID && bs > 0});
    }
    return true;
}

// Compare our reconstructed depth against the venue's published levels.
// `ours` is already best-first; `pub` is best-first with padding marked absent.
inline bool compare_side(const std::vector<DepthLevel>& ours,
                         const std::vector<PubLevel>& pub,
                         const char* side, std::string& err) {
    for (size_t i = 0; i < pub.size(); ++i) {
        bool we_have = i < ours.size();
        if (!pub[i].present) {
            // The venue shows no level here, so neither should we.
            if (we_have) {
                err = std::string(side) + " level " + std::to_string(i + 1) +
                      ": venue has no level, we have " +
                      std::to_string(ours[i].price) + " x " +
                      std::to_string(ours[i].volume);
                return false;
            }
            continue;
        }
        if (!we_have) {
            err = std::string(side) + " level " + std::to_string(i + 1) +
                  ": venue has " + std::to_string(pub[i].price) + " x " +
                  std::to_string(pub[i].size) + ", we have nothing";
            return false;
        }
        if (static_cast<int64_t>(ours[i].price) != pub[i].price ||
            static_cast<int64_t>(ours[i].volume) != pub[i].size) {
            err = std::string(side) + " level " + std::to_string(i + 1) +
                  ": venue " + std::to_string(pub[i].price) + " x " +
                  std::to_string(pub[i].size) + ", ours " +
                  std::to_string(ours[i].price) + " x " +
                  std::to_string(ours[i].volume);
            return false;
        }
    }
    return true;
}

// Apply one LOBSTER message to the book.
// Handle a delete/execution whose order id we don't hold.
//
// Orders resting before the capture window were seeded under synthetic ids, so
// a later message referencing one by its real exchange id finds nothing. But
// the venue is still telling us something true and specific: "`size` shares
// left this price on this side." When we hold seeded liquidity at exactly that
// (side, price), attributing the reduction to it keeps the aggregate book — the
// thing an L2 reconciliation actually compares — correct, even though the
// order-level identity is unrecoverable.
//
// Returns true if the event was attributed to seeded liquidity.
inline bool attribute_to_seed(OrderBook& book, const Message& m,
                              SeedIndex& seeds, Stats& st) {
    auto key = std::make_pair(m.direction == 1, m.price);
    auto it = seeds.find(key);
    if (it == seeds.end()) return false;

    uint32_t held = book.get_order_qty(it->second);
    if (held == 0) { seeds.erase(it); return false; }   // seeded order already gone

    book.reduce_order(it->second, m.size);
    ++st.seed_attributed;
    if (book.get_order_qty(it->second) == 0) seeds.erase(it);
    return true;
}

inline void apply_message(OrderBook& book, const Message& m, Stats& st,
                          SeedIndex& seeds) {
    switch (static_cast<Event>(m.event)) {
        case Event::NEW_LIMIT:
            // LOBSTER's stream is already matched, so a new limit order never
            // crosses; it simply rests.
            book.insert_order(m.order_id, m.price, m.size, m.direction == 1);
            break;

        case Event::PARTIAL_CANCEL:
        case Event::EXEC_VISIBLE:
            // Both shrink a specific resting order by `size`. Executions are
            // pre-matched by the venue, so we apply them as reductions rather
            // than letting our engine match.
            if (book.get_order_qty(m.order_id) == 0) {
                if (!attribute_to_seed(book, m, seeds, st)) ++st.unknown_refs;
            } else {
                book.reduce_order(m.order_id, m.size);
            }
            break;

        case Event::FULL_DELETE:
            // LOBSTER reports the order's full size on a delete, so an
            // unattributable delete reduces the seeded aggregate by that much.
            if (book.get_order_qty(m.order_id) == 0) {
                if (!attribute_to_seed(book, m, seeds, st)) ++st.unknown_refs;
            } else {
                book.cancel_order(m.order_id);
            }
            break;

        case Event::EXEC_HIDDEN:
            ++st.skipped_hidden;   // hidden liquidity is not on the visible book
            break;
        case Event::CROSS:
            ++st.skipped_cross;
            break;
        case Event::HALT:
            ++st.skipped_halt;
            break;
    }
}

// Replay `msg_path` through a fresh book, reconciling against `book_path` after
// every message. Returns true if the whole file reconciled; on failure `err`
// describes the first divergence.
inline bool replay_and_reconcile(const std::string& msg_path,
                                 const std::string& book_path,
                                 size_t levels, Stats& st, std::string& err) {
    std::ifstream msg_file(msg_path);
    if (!msg_file) { err = "cannot open " + msg_path; return false; }
    std::ifstream book_file(book_path);
    if (!book_file) { err = "cannot open " + book_path; return false; }

    OrderBook book;

    // We apply LOBSTER's events literally: every execution is already encoded
    // as a size reduction on a specific resting order, so the engine's own
    // matching must never fire. If it does, our book has drifted into a crossed
    // state — surface that rather than letting it silently corrupt the book.
    book.set_trade_handler([&](const Trade&) { ++st.unexpected_trades; });

    std::string msg_line, book_line;
    std::vector<PubLevel> pub_asks, pub_bids;
    bool seeded = false;
    SeedIndex seeds;

    while (std::getline(msg_file, msg_line)) {
        if (msg_line.empty()) continue;
        if (!std::getline(book_file, book_line)) {
            err = "orderbook file ended early at message " + std::to_string(st.messages + 1);
            return false;
        }
        ++st.messages;

        Message m;
        if (!parse_message(msg_line, m)) {
            err = "malformed message at line " + std::to_string(st.messages);
            return false;
        }
        if (m.event < 1 || m.event > 7) {
            err = "unknown event type " + std::to_string(m.event) +
                  " at message " + std::to_string(st.messages);
            return false;
        }

        if (!parse_book_row(book_line, levels, pub_asks, pub_bids)) {
            err = "malformed orderbook row at line " + std::to_string(st.messages);
            return false;
        }

        // Warm-start seeding: LOBSTER's message stream begins at market open,
        // but NASDAQ's actual book at that instant is not empty — resting
        // liquidity established by the opening cross (and any pre-window
        // order flow) has no corresponding "new order" message in this file.
        // The venue's published first row can therefore show real depth that
        // a from-empty replay could never produce on its own.
        //
        // We bootstrap around this by inferring the implied state strictly
        // BEFORE message 1: take row 1 as published, and if message 1 is a
        // plain new-limit insert (the overwhelmingly common case), subtract
        // its own contribution back out of the one level it created. What
        // remains is pre-existing liquidity with no message provenance in
        // this file, which we seed as resting orders under synthetic IDs
        // (clearly out of range of any real LOBSTER order ID) so depth and
        // volume reconcile correctly. Message 1 is then applied for real,
        // through the normal path below, restoring row 1 exactly.
        //
        // Residual caveat: if a LATER message references one of these seeded
        // synthetic orders by its real (unrecoverable) LOBSTER id — e.g. the
        // venue eventually cancels or executes against pre-existing liquidity
        // we never had a true id for — that reference resolves as unknown
        // (tallied in Stats::unknown_refs) and the seeded quantity goes
        // stale. This is an inherent limit of reconstructing from a
        // message-only stream, not a defect in the reconciliation logic.
        if (!seeded) {
            auto seed_asks = pub_asks;
            auto seed_bids = pub_bids;
            if (static_cast<Event>(m.event) == Event::NEW_LIMIT) {
                auto& side = (m.direction == 1) ? seed_bids : seed_asks;
                for (auto& lvl : side) {
                    if (lvl.present && lvl.price == static_cast<int64_t>(m.price)) {
                        lvl.size -= m.size;
                        if (lvl.size <= 0) lvl.present = false;
                        break;
                    }
                }
            }
            uint64_t synthetic_id = 900000000000ULL;
            for (const auto& lvl : seed_bids) {
                if (!lvl.present) continue;
                uint32_t px = static_cast<uint32_t>(lvl.price);
                book.insert_order(synthetic_id, px, static_cast<uint32_t>(lvl.size), true);
                seeds[{true, px}] = synthetic_id++;
            }
            for (const auto& lvl : seed_asks) {
                if (!lvl.present) continue;
                uint32_t px = static_cast<uint32_t>(lvl.price);
                book.insert_order(synthetic_id, px, static_cast<uint32_t>(lvl.size), false);
                seeds[{false, px}] = synthetic_id++;
            }
            seeded = true;
        }

        apply_message(book, m, st, seeds);

        std::string side_err;
        if (!compare_side(book.get_ask_depth(levels), pub_asks, "ask", side_err) ||
            !compare_side(book.get_bid_depth(levels), pub_bids, "bid", side_err)) {
            err = "divergence at message " + std::to_string(st.messages) +
                  " (event=" + std::to_string(m.event) +
                  " id=" + std::to_string(m.order_id) +
                  " size=" + std::to_string(m.size) +
                  " price=" + std::to_string(m.price) + "): " + side_err;
            return false;
        }
    }

    if (st.unexpected_trades > 0) {
        err = "engine matched " + std::to_string(st.unexpected_trades) +
              " trade(s); LOBSTER events are pre-matched and should never trigger"
              " matching — the book was in a crossed state";
        return false;
    }
    return true;
}

}  // namespace lobster
