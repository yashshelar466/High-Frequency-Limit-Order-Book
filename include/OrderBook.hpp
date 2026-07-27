#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>
#include <iostream>
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

// 3. The OrderBook Class Interface
class OrderBook {
private:
    // Fixed array for direct O(1) price-level lookup (e.g., tick index)
    static constexpr size_t MAX_PRICE_LEVELS = 100000;
    PriceLevel* bids[MAX_PRICE_LEVELS] = {nullptr};
    PriceLevel* asks[MAX_PRICE_LEVELS] = {nullptr};

    // Quick lookup from Order ID directly to the Order struct for O(1) cancels
    std::unordered_map<uint64_t, Order*> order_map;

    // Tracking current best bid and ask prices
    uint32_t best_bid_price = 0;
    uint32_t best_ask_price = 0xFFFFFFFF;

public:
    OrderBook() = default;
    ~OrderBook();

    // Prevent copying to ensure deterministic memory behavior
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    // Core Matching Engine Methods
    void insert_order(uint64_t id, uint32_t price, uint32_t qty, bool is_buy);
    void cancel_order(uint64_t id);

    // Getters for current top-of-book (Spread)
    uint32_t get_best_bid() const { return best_bid_price; }
    uint32_t get_best_ask() const { return best_ask_price; }

    // Read-only access to a price level, for tests that need to verify
    // FIFO ordering and volume/count bookkeeping rather than just the
    // top-of-book price.
    const PriceLevel* get_bid_level(uint32_t price) const {
        return (price < MAX_PRICE_LEVELS) ? bids[price] : nullptr;
    }
    const PriceLevel* get_ask_level(uint32_t price) const {
        return (price < MAX_PRICE_LEVELS) ? asks[price] : nullptr;
    }

    private:
    // Re-establish the top-of-book invariant after a matching sweep may have
    // depleted one or more levels: best_ask_price must point at the lowest
    // live ask (best_bid_price at the highest live bid), or the empty-side
    // sentinel when that side of the book holds no orders. The matching loop
    // only advances these as a lower/upper bound, so it can leave them parked
    // on an empty slot or a price gap — these normalize that.
    void normalize_best_ask() {
        while (best_ask_price < MAX_PRICE_LEVELS &&
               (!asks[best_ask_price] || !asks[best_ask_price]->head)) {
            best_ask_price++;
        }
        if (best_ask_price >= MAX_PRICE_LEVELS) best_ask_price = 0xFFFFFFFF;
    }
    void normalize_best_bid() {
        while (best_bid_price > 0 &&
               (!bids[best_bid_price] || !bids[best_bid_price]->head)) {
            best_bid_price--;
        }
    }

    MemoryPool<Order> order_pool;
};