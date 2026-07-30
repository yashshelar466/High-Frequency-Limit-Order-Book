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
#include "FeatureEmit.hpp"

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
    uint64_t recovered_levels = 0;  // published levels adopted in recover mode
    uint64_t pruned_levels = 0;     // phantom levels dropped in recover mode
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

// Adopt published levels we hold no liquidity at ("recover" mode).
//
// A top-N feed is a windowed view, not a complete event stream: LOBSTER emits
// messages only for events in the requested price range, so an order resting
// outside that window generates no message and then simply *appears* in the
// published book once the levels above it are consumed. No amount of care with
// the message stream can conjure liquidity it was never told about.
//
// Recover mode treats such a level the way a production feed handler treats a
// detected gap — adopt what the venue reports and carry on — while counting
// every adoption so the result stays honest. It only forgives levels that are
// *missing* on our side: a size mismatch on a level we do track, a price we
// invented, or a phantom of our own still fails the reconciliation.
//
// Levels that would cross our book are deliberately left alone: that indicates
// a phantom on the opposite side, which is a genuine defect worth surfacing
// rather than papering over.
inline size_t recover_missing_levels(OrderBook& book,
                                     const std::vector<PubLevel>& pub_asks,
                                     const std::vector<PubLevel>& pub_bids,
                                     SeedIndex& seeds, uint64_t& next_synthetic) {
    size_t recovered = 0;
    for (const auto& lvl : pub_bids) {
        if (!lvl.present) continue;
        uint32_t px = static_cast<uint32_t>(lvl.price);
        if (book.get_bid_level(px) != nullptr) continue;      // already tracked
        if (px >= book.get_best_ask()) continue;              // would cross — surface it
        book.insert_order(next_synthetic, px, static_cast<uint32_t>(lvl.size), true);
        seeds[{true, px}] = next_synthetic++;
        ++recovered;
    }
    for (const auto& lvl : pub_asks) {
        if (!lvl.present) continue;
        uint32_t px = static_cast<uint32_t>(lvl.price);
        if (book.get_ask_level(px) != nullptr) continue;
        if (px <= book.get_best_bid()) continue;              // would cross — surface it
        book.insert_order(next_synthetic, px, static_cast<uint32_t>(lvl.size), false);
        seeds[{false, px}] = next_synthetic++;
        ++recovered;
    }
    return recovered;
}

// Drop levels we hold that the venue does not ("recover" mode, second half).
//
// The mirror image of recover_missing_levels: liquidity that drifts *below* the
// published window can be cancelled or executed there, generating no message,
// so a level we are tracking can die without us ever being told. It then
// resurfaces as a phantom once the levels above it clear.
//
// A level is only judged when the published book is authoritative about its
// price. For asks that means prices at or inside the deepest published ask;
// beyond that the venue simply isn't reporting, and absence proves nothing. If
// the venue padded the side (fewer live levels than requested) it has shown its
// entire book, so every level we hold in the window is judgeable.
inline size_t prune_phantom_levels(OrderBook& book,
                                   const std::vector<PubLevel>& pub_asks,
                                   const std::vector<PubLevel>& pub_bids,
                                   SeedIndex& seeds, size_t levels) {
    size_t pruned = 0;

    auto drop_level = [&](uint32_t price, bool is_buy) {
        const PriceLevel* lvl = is_buy ? book.get_bid_level(price)
                                       : book.get_ask_level(price);
        if (!lvl) return;
        std::vector<uint64_t> ids;                 // collect first: cancelling relinks
        for (const Order* o = lvl->head; o; o = o->next) ids.push_back(o->id);
        for (uint64_t id : ids) book.cancel_order(id);
        seeds.erase({is_buy, price});
        ++pruned;
    };

    // --- asks ---------------------------------------------------------------
    {
        std::vector<uint32_t> published;
        for (const auto& l : pub_asks)
            if (l.present) published.push_back(static_cast<uint32_t>(l.price));
        bool venue_complete = published.size() < levels;   // padding => full book shown
        uint32_t deepest = published.empty() ? 0 : published.back();

        std::vector<uint32_t> doomed;
        for (const auto& mine : book.get_ask_depth(levels)) {
            if (!venue_complete && published.empty()) break;
            if (!venue_complete && mine.price > deepest) continue;   // outside window
            bool listed = false;
            for (uint32_t p : published) if (p == mine.price) { listed = true; break; }
            if (!listed) doomed.push_back(mine.price);
        }
        for (uint32_t p : doomed) drop_level(p, /*is_buy=*/false);
    }

    // --- bids ---------------------------------------------------------------
    {
        std::vector<uint32_t> published;
        for (const auto& l : pub_bids)
            if (l.present) published.push_back(static_cast<uint32_t>(l.price));
        bool venue_complete = published.size() < levels;
        uint32_t deepest = published.empty() ? 0 : published.back();

        std::vector<uint32_t> doomed;
        for (const auto& mine : book.get_bid_depth(levels)) {
            if (!venue_complete && published.empty()) break;
            if (!venue_complete && mine.price < deepest) continue;   // outside window
            bool listed = false;
            for (uint32_t p : published) if (p == mine.price) { listed = true; break; }
            if (!listed) doomed.push_back(mine.price);
        }
        for (uint32_t p : doomed) drop_level(p, /*is_buy=*/true);
    }

    return pruned;
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
// `recover` selects the two useful ways to read this data:
//   false (default) — strict. The first level we cannot explain fails the run,
//                     which measures the honest reconstruction horizon.
//   true            — adopt unexplained published levels and keep going,
//                     reporting how many adoptions the session required.
// `sink`, when set, receives one FeatureRow per message — the reconstructed
// book state that message produced. See FeatureEmit.hpp. Emission happens after
// the message is applied and after any recover-mode resynchronisation, so the
// features describe the same book the reconciliation just checked.
inline bool replay_and_reconcile(const std::string& msg_path,
                                 const std::string& book_path,
                                 size_t levels, Stats& st, std::string& err,
                                 bool recover = false,
                                 const FeatureSink& sink = FeatureSink{}) {
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
    OfiTracker ofi;
    // Synthetic ids stand in for orders we were never given real ids for. Kept
    // across the whole run so opening seeds and later recoveries never collide.
    uint64_t synthetic_id = 900000000000ULL;

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

        // Parse the row at the file's FULL published depth, not just the depth
        // we intend to compare. Seeding must use everything the venue tells us
        // about the opening book — liquidity at levels below the comparison
        // window still promotes into view as the top is consumed, and throwing
        // it away guarantees a divergence later. The comparison itself is
        // narrowed to `levels` further down.
        const size_t file_levels = split_csv(book_line).size() / 4;
        if (file_levels < levels) {
            err = "orderbook row at line " + std::to_string(st.messages) +
                  " has only " + std::to_string(file_levels) +
                  " levels, fewer than the " + std::to_string(levels) + " requested";
            return false;
        }
        if (!parse_book_row(book_line, file_levels, pub_asks, pub_bids)) {
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

        // In recover mode, adopt any published level we hold nothing at before
        // comparing — a windowed feed can surface liquidity it never announced.
        if (recover) {
            st.pruned_levels +=
                prune_phantom_levels(book, pub_asks, pub_bids, seeds, levels);
            st.recovered_levels +=
                recover_missing_levels(book, pub_asks, pub_bids, seeds, synthetic_id);
        }

        // Emit the feature row for this update, before the comparison — so a
        // strict-mode run that stops at a divergence still leaves a usable file
        // covering everything it did reconcile.
        if (sink) {
            FeatureRow row;
            row.msg_index = st.messages;
            row.time = m.time;
            row.event = m.event;
            row.direction = m.direction;
            row.msg_size = m.size;
            row.msg_price = m.price;

            auto bid_top = book.get_bid_depth(DEPTH_LEVELS);
            auto ask_top = book.get_ask_depth(DEPTH_LEVELS);
            if (!bid_top.empty()) { row.bid_px = bid_top[0].price; row.bid_sz = bid_top[0].volume; }
            if (!ask_top.empty()) { row.ask_px = ask_top[0].price; row.ask_sz = ask_top[0].volume; }
            for (const auto& l : bid_top) row.bid_depth_vol += l.volume;
            for (const auto& l : ask_top) row.ask_depth_vol += l.volume;

            bool valid = false;
            row.ofi = ofi.update(row.bid_px, row.bid_sz, row.ask_px, row.ask_sz, valid);
            row.quote_valid = valid ? 1 : 0;

            if (static_cast<Event>(m.event) == Event::EXEC_VISIBLE)
                row.signed_trade_sz = signed_execution_size(m.direction, m.size);
            else if (static_cast<Event>(m.event) == Event::EXEC_HIDDEN)
                row.signed_hidden_sz = signed_execution_size(m.direction, m.size);

            sink(row);
        }

        // Compare only the requested window. The deepest published levels are
        // structurally unreliable (see the depth-window note in the README), so
        // reconciling shallower than you ingest keeps the check inside the
        // region the message stream can actually account for.
        std::vector<PubLevel> cmp_asks(pub_asks.begin(), pub_asks.begin() + levels);
        std::vector<PubLevel> cmp_bids(pub_bids.begin(), pub_bids.begin() + levels);

        std::string side_err;
        if (!compare_side(book.get_ask_depth(levels), cmp_asks, "ask", side_err) ||
            !compare_side(book.get_bid_depth(levels), cmp_bids, "bid", side_err)) {
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
