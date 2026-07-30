#include "../include/OrderBook.hpp"
#include <algorithm>

OrderBook::~OrderBook() {
    // Return every live order to the pool, then free the price levels. Orders
    // are reachable via order_map; levels via the ordered side maps.
    for (auto& kv : order_map) {
        order_pool.deallocate(kv.second);
    }
    for (auto& kv : bids) delete kv.second;
    for (auto& kv : asks) delete kv.second;
}

uint32_t OrderBook::match_incoming(uint64_t taker_id, uint32_t limit_price,
                                   uint32_t qty, bool is_buy) {
    if (is_buy) {
        // Cross against the lowest asks priced at or below the limit.
        while (qty > 0 && !asks.empty() && asks.begin()->first <= limit_price) {
            auto ait = asks.begin();
            PriceLevel* level = ait->second;
            Order* resting = level->head;

            uint32_t fill = std::min(qty, resting->qty);
            qty -= fill;
            resting->qty -= fill;
            level->total_volume -= fill;

            // Report the fill at the resting maker's price before it may be freed.
            if (trade_handler) {
                trade_handler(Trade{taker_id, resting->id, ait->first, fill, true});
            }

            if (resting->qty == 0) {
                level->remove_order(resting);
                order_map.erase(resting->id);
                order_pool.deallocate(resting);
            }
            if (level->order_count == 0) {
                delete level;
                asks.erase(ait);
            }
        }
    } else {
        // Cross against the highest bids priced at or above the limit.
        while (qty > 0 && !bids.empty() && bids.begin()->first >= limit_price) {
            auto bit = bids.begin();
            PriceLevel* level = bit->second;
            Order* resting = level->head;

            uint32_t fill = std::min(qty, resting->qty);
            qty -= fill;
            resting->qty -= fill;
            level->total_volume -= fill;

            if (trade_handler) {
                trade_handler(Trade{taker_id, resting->id, bit->first, fill, false});
            }

            if (resting->qty == 0) {
                level->remove_order(resting);
                order_map.erase(resting->id);
                order_pool.deallocate(resting);
            }
            if (level->order_count == 0) {
                delete level;
                bids.erase(bit);
            }
        }
    }
    return qty;
}

bool OrderBook::can_fully_fill(uint32_t limit_price, uint32_t qty, bool is_buy) const {
    uint64_t available = 0;
    if (is_buy) {
        for (const auto& kv : asks) {
            if (kv.first > limit_price) break;   // asks ascending: no cheaper levels left
            available += kv.second->total_volume;
            if (available >= qty) return true;
        }
    } else {
        for (const auto& kv : bids) {
            if (kv.first < limit_price) break;   // bids descending: no higher levels left
            available += kv.second->total_volume;
            if (available >= qty) return true;
        }
    }
    return available >= qty;
}

void OrderBook::insert_order(uint64_t id, uint32_t price, uint32_t qty, bool is_buy,
                             OrderType type) {
    if (qty == 0) return;
    // 0 and 0xFFFFFFFF are reserved as empty-side sentinels; a priced order may
    // not sit on them. MARKET orders ignore the price field entirely.
    if (type != OrderType::MARKET && (price == NO_BID || price == NO_ASK)) return;

    // Reject a duplicate live order ID. Overwriting order_map[id] would orphan
    // the existing order: it stays linked in its price level (still counting
    // toward book volume) but becomes unreachable for cancellation and leaks at
    // shutdown, since the destructor only frees orders reachable via order_map.
    if (order_map.find(id) != order_map.end()) return;

    // A MARKET order sweeps the opposite side with no price limit.
    uint32_t limit_price = price;
    if (type == OrderType::MARKET) {
        limit_price = is_buy ? NO_ASK : NO_BID;
    }

    // Fill-Or-Kill: only proceed if the whole quantity can be filled right now.
    if (type == OrderType::FOK && !can_fully_fill(limit_price, qty, is_buy)) {
        return;
    }

    uint32_t remaining = match_incoming(id, limit_price, qty, is_buy);

    // Only LIMIT orders rest; IOC / FOK / MARKET discard any unfilled remainder.
    if (remaining > 0 && type == OrderType::LIMIT) {
        Order* new_order = order_pool.allocate(id, price, remaining, is_buy);
        order_map[id] = new_order;

        if (is_buy) {
            auto it = bids.find(price);
            PriceLevel* level = (it != bids.end()) ? it->second
                                                   : (bids[price] = new PriceLevel{price});
            level->append_order(new_order);
        } else {
            auto it = asks.find(price);
            PriceLevel* level = (it != asks.end()) ? it->second
                                                   : (asks[price] = new PriceLevel{price});
            level->append_order(new_order);
        }
    }
}

void OrderBook::cancel_order(uint64_t id) {
    auto it = order_map.find(id);
    if (it == order_map.end()) return;

    Order* order = it->second;
    uint32_t price = order->price;

    if (order->is_buy) {
        auto lit = bids.find(price);
        if (lit != bids.end()) {
            PriceLevel* level = lit->second;
            level->remove_order(order);
            if (level->order_count == 0) {
                delete level;
                bids.erase(lit);
            }
        }
    } else {
        auto lit = asks.find(price);
        if (lit != asks.end()) {
            PriceLevel* level = lit->second;
            level->remove_order(order);
            if (level->order_count == 0) {
                delete level;
                asks.erase(lit);
            }
        }
    }

    order_map.erase(it);
    order_pool.deallocate(order);
}

void OrderBook::amend_order(uint64_t id, uint32_t new_price, uint32_t new_qty) {
    auto it = order_map.find(id);
    if (it == order_map.end()) return;                 // unknown order
    if (new_qty == 0 || new_price == NO_BID || new_price == NO_ASK) return;  // invalid: leave as-is

    Order* order = it->second;
    bool is_buy = order->is_buy;

    // Reducing (or holding) quantity at the same price is done in place, which
    // preserves the order's spot in the FIFO queue (time priority).
    if (new_price == order->price && new_qty <= order->qty) {
        uint32_t delta = order->qty - new_qty;
        PriceLevel* level = nullptr;
        if (is_buy) {
            auto lit = bids.find(order->price);
            if (lit != bids.end()) level = lit->second;
        } else {
            auto lit = asks.find(order->price);
            if (lit != asks.end()) level = lit->second;
        }
        if (level) level->total_volume -= delta;       // keep bookkeeping in sync
        order->qty = new_qty;
        return;
    }

    // Any price change or size increase loses priority: cancel and re-submit as
    // a fresh (LIMIT) aggressor. The re-inserted order may cross the book and
    // execute immediately (emitting trades) before any remainder rests.
    cancel_order(id);
    insert_order(id, new_price, new_qty, is_buy);
}

void OrderBook::reduce_order(uint64_t id, uint32_t qty_delta) {
    auto it = order_map.find(id);
    if (it == order_map.end()) return;   // unknown or already gone
    if (qty_delta == 0) return;

    Order* order = it->second;

    // A reduction that meets or exceeds the resting size removes the order.
    if (qty_delta >= order->qty) {
        cancel_order(id);
        return;
    }

    // Partial reduction: shrink in place so the order keeps its queue position.
    PriceLevel* level = nullptr;
    if (order->is_buy) {
        auto lit = bids.find(order->price);
        if (lit != bids.end()) level = lit->second;
    } else {
        auto lit = asks.find(order->price);
        if (lit != asks.end()) level = lit->second;
    }
    if (level) level->total_volume -= qty_delta;   // keep bookkeeping in sync
    order->qty -= qty_delta;
}
