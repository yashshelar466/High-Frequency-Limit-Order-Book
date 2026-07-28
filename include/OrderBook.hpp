#pragma once

#include <cstdint>
#include <cstddef>
#include <unordered_map>
#include <map>
#include <vector>
#include <iostream>
#include <functional>
#include "MemoryPool.hpp"

// 1. The Core Order Data Structure
struct Order {
    uint64_t id;
    uint32_t price;
    uint32_t qty;
    bool is_buy;

    // Intrusive pointers for O(1) doubly-linked list insertion and removal
    Order* next = nullptr;
    Order* prev = nullptr;
    Order(uint64_t i = 0, uint32_t p = 0, uint32_t q = 0, bool b = false)
        : id(i), price(p), qty(q), is_buy(b), next(nullptr), prev(nullptr) {}
};

// 2. The Price Level Data Structure
struct PriceLevel {
    uint32_t price;
    uint64_t total_volume = 0;
    uint32_t order_count = 0;

    Order* head = nullptr;
    Order* tail = nullptr;

    // Helper method to add an order to the back of the queue (Time Priority)
    void append_order(Order* order) {
        order->next = nullptr;
        order->prev = tail;
        if (tail) {
            tail->next = order;
        } else {
            head = order;
        }
        tail = order;
        total_volume += order->qty;
        order_count++;
    }

    // Helper method to unlink an order from anywhere in the list in O(1)
    void remove_order(Order* order) {
        if (order->prev) order->prev->next = order->next;
        if (order->next) order->next->prev = order->prev;
        if (order == head) head = order->next;
        if (order == tail) tail = order->prev;

        total_volume -= order->qty;
        order_count--;
        order->next = nullptr;
        order->prev = nullptr;
    }
};

// 3. A single execution (fill) produced by the matching engine.
//
// Every fill pairs an incoming aggressor with one resting maker. Trades
// execute at the resting order's price (price-time priority), so `price` is
// the maker's limit price, not the aggressor's. A single crossing insert can
// produce several Trades — one per resting order it consumes.
struct Trade {
    uint64_t taker_id;    // aggressing (incoming) order
    uint64_t maker_id;    // resting (passive) order that was hit
    uint32_t price;       // execution price (the resting maker's price)
    uint32_t qty;         // quantity filled
    bool taker_is_buy;    // side of the aggressor
};

// 4. Order time-in-force / type.
//   LIMIT  – match what crosses, rest any remainder on the book (default).
//   IOC    – Immediate-Or-Cancel: match what crosses now, discard the rest.
//   FOK    – Fill-Or-Kill: fill the entire quantity immediately or do nothing.
//   MARKET – ignore the price limit and take liquidity until filled or the
//            opposite side is exhausted; never rests (remainder discarded).
enum class OrderType { LIMIT, IOC, FOK, MARKET };

// 5. One level of an L2 (market-by-price) depth snapshot.
struct DepthLevel {
    uint32_t price;
    uint64_t volume;
    uint32_t order_count;
};

// 6. The OrderBook Class Interface
class OrderBook {
private:
    // Ordered price -> level maps. Bids are keyed descending so begin() is the
    // best (highest) bid; asks ascending so begin() is the best (lowest) ask.
    // Levels are created lazily and erased the moment they empty, so a present
    // key always maps to a non-empty level. Prices span the full uint32 range
    // except the two reserved sentinels (see best-bid/ask getters).
    std::map<uint32_t, PriceLevel*, std::greater<uint32_t>> bids;  // highest first
    std::map<uint32_t, PriceLevel*> asks;                          // lowest first

    // Quick lookup from Order ID directly to the Order struct for O(1) cancels
    std::unordered_map<uint64_t, Order*> order_map;

    // Reserved sentinel prices: 0 means "no bid", 0xFFFFFFFF means "no ask".
    // Orders at these prices are rejected so the getters stay unambiguous.
    static constexpr uint32_t NO_BID = 0;
    static constexpr uint32_t NO_ASK = 0xFFFFFFFF;

public:
    OrderBook() = default;
    ~OrderBook();

    // Prevent copying to ensure deterministic memory behavior
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    // Core Matching Engine Methods. The 4-argument form is a plain LIMIT order;
    // the 5-argument form selects the time-in-force / type (see OrderType).
    void insert_order(uint64_t id, uint32_t price, uint32_t qty, bool is_buy) {
        insert_order(id, price, qty, is_buy, OrderType::LIMIT);
    }
    void insert_order(uint64_t id, uint32_t price, uint32_t qty, bool is_buy, OrderType type);
    void cancel_order(uint64_t id);

    // Amend (cancel-replace) a resting order. Semantics follow exchange
    // convention on time priority:
    //   - Reducing quantity at the same price is done in place and KEEPS the
    //     order's position in the FIFO queue.
    //   - Changing price, or increasing quantity, LOSES priority: the order is
    //     cancelled and re-submitted as a fresh aggressor, so it may cross the
    //     book and execute immediately (emitting trades) before any remainder
    //     rests at the back of the new level.
    // A no-op (unknown id, new_qty == 0, or out-of-range price) leaves the book
    // unchanged. The order keeps its original side.
    void amend_order(uint64_t id, uint32_t new_price, uint32_t new_qty);

    // Trade/execution reporting. Register a handler to observe every fill the
    // matching engine produces — the foundation for backtesting (trade blotter,
    // PnL, VWAP) and for reconciling against an exchange's own execution feed.
    // The handler is invoked synchronously from within insert_order, once per
    // fill, in execution order. Leaving it unset keeps the hot path free of any
    // reporting overhead (a single null check per fill).
    using TradeHandler = std::function<void(const Trade&)>;
    void set_trade_handler(TradeHandler handler) { trade_handler = std::move(handler); }

    // Getters for current top-of-book. With the ordered maps the best price is
    // simply the first key, so these are O(1). Empty sides report the reserved
    // sentinels (0 for bids, 0xFFFFFFFF for asks).
    uint32_t get_best_bid() const { return bids.empty() ? NO_BID : bids.begin()->first; }
    uint32_t get_best_ask() const { return asks.empty() ? NO_ASK : asks.begin()->first; }

    // Read-only access to a price level, for tests that need to verify
    // FIFO ordering and volume/count bookkeeping rather than just the
    // top-of-book price. Returns nullptr when no order rests at that price.
    const PriceLevel* get_bid_level(uint32_t price) const {
        auto it = bids.find(price);
        return it == bids.end() ? nullptr : it->second;
    }
    const PriceLevel* get_ask_level(uint32_t price) const {
        auto it = asks.find(price);
        return it == asks.end() ? nullptr : it->second;
    }

    // L2 (market-by-price) depth snapshots: up to `levels` price levels from the
    // top of each side, best first (bids high→low, asks low→high). Ordered map
    // iteration makes these O(levels), independent of how sparse the book is.
    std::vector<DepthLevel> get_bid_depth(size_t levels) const {
        return snapshot(bids, levels);
    }
    std::vector<DepthLevel> get_ask_depth(size_t levels) const {
        return snapshot(asks, levels);
    }

    // Spread and mid-price convenience helpers. Both return 0 when either side
    // of the book is empty (no two-sided market to quote).
    uint32_t spread() const {
        if (bids.empty() || asks.empty()) return 0;
        return get_best_ask() - get_best_bid();
    }
    double mid_price() const {
        if (bids.empty() || asks.empty()) return 0.0;
        return (static_cast<double>(get_best_bid()) + get_best_ask()) / 2.0;
    }

    private:
    // Build a depth snapshot from either side's ordered map (iteration order is
    // already best-first for both, thanks to the maps' comparators).
    template <typename Side>
    static std::vector<DepthLevel> snapshot(const Side& side, size_t levels) {
        std::vector<DepthLevel> out;
        out.reserve(levels < side.size() ? levels : side.size());
        for (const auto& kv : side) {
            if (out.size() >= levels) break;
            out.push_back(DepthLevel{kv.first, kv.second->total_volume, kv.second->order_count});
        }
        return out;
    }

    // Match an incoming aggressor against the opposite side up to `limit_price`
    // (inclusive), emitting a Trade per fill. Returns the unfilled remainder.
    uint32_t match_incoming(uint64_t taker_id, uint32_t limit_price, uint32_t qty, bool is_buy);

    // Read-only check used by FOK: is there enough resting liquidity at prices
    // that satisfy `limit_price` to completely fill `qty` right now?
    bool can_fully_fill(uint32_t limit_price, uint32_t qty, bool is_buy) const;

    MemoryPool<Order> order_pool;
    TradeHandler trade_handler;
};