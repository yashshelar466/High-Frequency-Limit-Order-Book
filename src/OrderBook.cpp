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

            // If resting order is fully filled, remove it
            if (resting_order->qty == 0) {
                level->remove_order(resting_order);
                order_map.erase(resting_order->id);
                order_pool.deallocate(resting_order);
            }

            // Move best ask forward if level fully depleted
            if (level->order_count == 0) {
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

            if (resting_order->qty == 0) {
                level->remove_order(resting_order);
                order_map.erase(resting_order->id);
                order_pool.deallocate(resting_order);
            }

            if (level->order_count == 0 && best_bid_price > 0) {
                best_bid_price--;
            }
        }
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
            if (best_bid_price == price) {
                while (best_bid_price > 0 && (!bids[best_bid_price] || !bids[best_bid_price]->head)) {
                    best_bid_price--;
                }
            }
        }
    } else if (!order->is_buy && asks[price]) {
        asks[price]->remove_order(order);
        if (!asks[price]->head) {
            delete asks[price];
            asks[price] = nullptr;
            if (best_ask_price == price) {
                while (best_ask_price < MAX_PRICE_LEVELS && (!asks[best_ask_price] || !asks[best_ask_price]->head)) {
                    best_ask_price++;
                }
            }
        }
    }

    order_map.erase(it);
    order_pool.deallocate(order);
}