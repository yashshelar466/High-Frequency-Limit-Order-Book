#include "../include/OrderBook.hpp"
#include <algorithm>

OrderBook::~OrderBook() {
    // Clean up allocated PriceLevels and Orders
    for (auto& order_pair : order_map) {
        order_pool.deallocate(order_pair.second);
    }
    for (size_t i = 0; i < MAX_PRICE_LEVELS; ++i) {
        delete bids[i];
        delete asks[i];
    }
}

void OrderBook::insert_order(uint64_t id, uint32_t price, uint32_t qty, bool is_buy) {
    if (price >= MAX_PRICE_LEVELS || qty == 0) return;

    // Reject a duplicate live order ID. Overwriting order_map[id] would orphan
    // the existing order: it stays linked in its price level (still counting
    // toward book volume) but becomes unreachable for cancellation and leaks at
    // shutdown, since the destructor only frees orders reachable via order_map.
    // Exchanges reject duplicate client order IDs outright, so mirror that.
    if (order_map.find(id) != order_map.end()) return;

    // 1. MATCHING LOOP: Try to execute against resting opposite orders first
    if (is_buy) {
        while (qty > 0 && best_ask_price <= price) {
            PriceLevel* level = asks[best_ask_price];
            if (!level || !level->head) {
                // Update best ask if level is empty
                best_ask_price++;
                continue;
            }

            Order* resting_order = level->head;
            uint32_t fill_qty = std::min(qty, resting_order->qty);

            // Execute trade fill
            qty -= fill_qty;
            resting_order->qty -= fill_qty;
            level->total_volume -= fill_qty;

            // Report the fill (at the resting maker's price) before the resting
            // order may be freed below.
            if (trade_handler) {
                trade_handler(Trade{id, resting_order->id, level->price, fill_qty, true});
            }

            // If resting order is fully filled, remove it
            if (resting_order->qty == 0) {
                level->remove_order(resting_order);
                order_map.erase(resting_order->id);
                order_pool.deallocate(resting_order);
            }

            // Level fully depleted: free it (mirrors cancel_order's cleanup)
            // and move best ask forward.
            if (level->order_count == 0) {
                delete asks[best_ask_price];
                asks[best_ask_price] = nullptr;
                best_ask_price++;
            }
        }
    } else {
        // Sell order logic: cross with bids
        while (qty > 0 && best_bid_price >= price && best_bid_price > 0) {
            PriceLevel* level = bids[best_bid_price];
            if (!level || !level->head) {
                best_bid_price--;
                continue;
            }

            Order* resting_order = level->head;
            uint32_t fill_qty = std::min(qty, resting_order->qty);

            qty -= fill_qty;
            resting_order->qty -= fill_qty;
            level->total_volume -= fill_qty;

            // Report the fill (at the resting maker's price) before the resting
            // order may be freed below. Aggressor here is a sell.
            if (trade_handler) {
                trade_handler(Trade{id, resting_order->id, level->price, fill_qty, false});
            }

            if (resting_order->qty == 0) {
                level->remove_order(resting_order);
                order_map.erase(resting_order->id);
                order_pool.deallocate(resting_order);
            }

            if (level->order_count == 0) {
                delete bids[best_bid_price];
                bids[best_bid_price] = nullptr;
                if (best_bid_price > 0) best_bid_price--;
            }
        }
    }

    // The matching loop only moves best_bid/best_ask as a bound while sweeping,
    // so it can exit parked on an emptied level or a price gap. Restore the
    // exact top-of-book (or empty-side sentinel) before anyone reads it or the
    // resting-insert comparison below relies on it.
    if (is_buy) {
        normalize_best_ask();
    } else {
        normalize_best_bid();
    }

    // 2. RESTING ORDER: If order quantity remains, place rest on book
    if (qty > 0) {
        Order* new_order = order_pool.allocate(id, price, qty, is_buy);
        order_map[id] = new_order;

        if (is_buy) {
            if (!bids[price]) bids[price] = new PriceLevel{price};
            bids[price]->append_order(new_order);
            if (price > best_bid_price) best_bid_price = price;
        } else {
            if (!asks[price]) asks[price] = new PriceLevel{price};
            asks[price]->append_order(new_order);
            if (price < best_ask_price) best_ask_price = price;
        }
    }
}

void OrderBook::cancel_order(uint64_t id) {
    auto it = order_map.find(id);
    if (it == order_map.end()) return;

    Order* order = it->second;
    uint32_t price = order->price;

    if (order->is_buy && bids[price]) {
        bids[price]->remove_order(order);
        if (!bids[price]->head) {
            delete bids[price];
            bids[price] = nullptr;
            // If we just removed the top-of-book level, re-establish the
            // best-price invariant (also resets to the empty sentinel when the
            // side is now empty) via the shared normalizer.
            if (best_bid_price == price) normalize_best_bid();
        }
    } else if (!order->is_buy && asks[price]) {
        asks[price]->remove_order(order);
        if (!asks[price]->head) {
            delete asks[price];
            asks[price] = nullptr;
            if (best_ask_price == price) normalize_best_ask();
        }
    }

    order_map.erase(it);
    order_pool.deallocate(order);
}

void OrderBook::amend_order(uint64_t id, uint32_t new_price, uint32_t new_qty) {
    auto it = order_map.find(id);
    if (it == order_map.end()) return;                       // unknown order
    if (new_qty == 0 || new_price >= MAX_PRICE_LEVELS) return;  // invalid: leave as-is

    Order* order = it->second;
    bool is_buy = order->is_buy;

    // Reducing (or holding) quantity at the same price is done in place, which
    // preserves the order's spot in the FIFO queue (time priority).
    if (new_price == order->price && new_qty <= order->qty) {
        uint32_t delta = order->qty - new_qty;
        PriceLevel* level = is_buy ? bids[order->price] : asks[order->price];
        if (level) level->total_volume -= delta;             // keep bookkeeping in sync
        order->qty = new_qty;
        return;
    }

    // Any price change or size increase loses priority: cancel and re-submit as
    // a fresh aggressor. The re-inserted order may cross the book and execute
    // immediately (emitting trades) before any remainder rests at the back.
    cancel_order(id);
    insert_order(id, new_price, new_qty, is_buy);
}