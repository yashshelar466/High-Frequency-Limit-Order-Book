#include "../include/OrderBook.hpp"
#include <cassert>
#include <iostream>

void test_partial_fill() {
    OrderBook book;
    
    // Add Sell order: 100 qty @ 105
    book.insert_order(1, 105, 100, false);
    assert(book.get_best_ask() == 105);

    // Add Buy order: 40 qty @ 105 (Partial execution)
    book.insert_order(2, 105, 40, true);
    
    // Best ask should remain 105 (60 shares left resting)
    assert(book.get_best_ask() == 105);
    
    std::cout << "[PASS] Partial Fill Test" << std::endl;
}

void test_full_sweep() {
    OrderBook book;

    // Add multiple Sell orders at level 105
    book.insert_order(1, 105, 50, false);
    book.insert_order(2, 105, 50, false);

    // Buy order clears the entire level (100 shares)
    book.insert_order(3, 105, 100, true);

    // Best ask should no longer be 105
    assert(book.get_best_ask() != 105);

    std::cout << "[PASS] Full Level Sweep Test" << std::endl;
}

void test_order_cancellation() {
    OrderBook book;

    // Insert Sell order
    book.insert_order(1, 110, 100, false);
    assert(book.get_best_ask() == 110);

    // Cancel order 1
    book.cancel_order(1);

    // Best ask should no longer be 110
    assert(book.get_best_ask() != 110);

    std::cout << "[PASS] Order Cancellation Test" << std::endl;
}

void test_invalid_inputs() {
    OrderBook book;

    // Zero quantity order should be ignored safely
    book.insert_order(1, 100, 0, true);
    assert(book.get_best_bid() != 100);

    // Out of bounds price should be ignored safely
    book.insert_order(2, 1000000, 50, true);

    std::cout << "[PASS] Invalid Inputs Test" << std::endl;
}

void test_fifo_order_preserved_after_partial_fill() {
    OrderBook book;

    // Three resting sell orders at the same price, in submission order.
    book.insert_order(1, 105, 50, false);
    book.insert_order(2, 105, 30, false);
    book.insert_order(3, 105, 20, false);

    // A buy for 20 should partially fill order 1 only (price-time priority:
    // order 1 is at the head, so it fills first even though it's not the
    // smallest quantity).
    book.insert_order(4, 105, 20, true);

    const PriceLevel* level = book.get_ask_level(105);
    assert(level != nullptr);
    assert(level->order_count == 3);        // all three orders still resting
    assert(level->total_volume == 80);       // 50-20 + 30 + 20 = 80

    // Walk the list directly: head should still be order 1, now with 30
    // qty left, followed by order 2 (untouched) and order 3 (untouched).
    Order* first = level->head;
    assert(first != nullptr && first->id == 1 && first->qty == 30);

    Order* second = first->next;
    assert(second != nullptr && second->id == 2 && second->qty == 30);

    Order* third = second->next;
    assert(third != nullptr && third->id == 3 && third->qty == 20);
    assert(third->next == nullptr);          // tail correctly terminated

    std::cout << "[PASS] FIFO Order Preserved After Partial Fill Test" << std::endl;
}

void test_best_ask_resets_to_sentinel_after_full_sweep() {
    OrderBook book;

    // Only ask on the book, then a buy sweeps it completely.
    book.insert_order(1, 105, 10, false);
    book.insert_order(2, 105, 10, true);

    // With no asks left, best ask must report the empty sentinel, not a
    // stale price the sweep incremented past.
    assert(book.get_best_ask() == 0xFFFFFFFF);
    assert(book.get_ask_level(105) == nullptr);

    std::cout << "[PASS] Best Ask Resets To Sentinel After Full Sweep Test" << std::endl;
}

void test_best_bid_resets_to_sentinel_after_full_sweep() {
    OrderBook book;

    // Only bid on the book, then a sell sweeps it completely.
    book.insert_order(1, 100, 10, true);
    book.insert_order(2, 100, 10, false);

    // With no bids left, best bid must report the empty sentinel (0).
    assert(book.get_best_bid() == 0);
    assert(book.get_bid_level(100) == nullptr);

    std::cout << "[PASS] Best Bid Resets To Sentinel After Full Sweep Test" << std::endl;
}

void test_best_ask_skips_gap_after_sweep() {
    OrderBook book;

    // Two asks with a price gap between them.
    book.insert_order(1, 105, 10, false);
    book.insert_order(2, 107, 10, false);
    assert(book.get_best_ask() == 105);

    // A buy at 106 can only fill the 105 level; the 107 level is out of reach.
    book.insert_order(3, 106, 10, true);

    // Best ask must now be the surviving 107 level, not the emptied 105 slot
    // nor the 106 the sweep parked on.
    assert(book.get_best_ask() == 107);
    assert(book.get_ask_level(105) == nullptr);
    assert(book.get_ask_level(107) != nullptr);

    std::cout << "[PASS] Best Ask Skips Price Gap After Sweep Test" << std::endl;
}

void test_higher_ask_after_sweep_is_reported() {
    OrderBook book;

    // Sweep the book empty, then rest a NEW ask at a higher price than the
    // one the sweep parked best_ask on. It must become the reported best ask.
    book.insert_order(1, 105, 10, false);
    book.insert_order(2, 105, 10, true);   // full sweep
    book.insert_order(3, 108, 10, false);  // new resting ask, higher price

    assert(book.get_best_ask() == 108);

    std::cout << "[PASS] Higher Ask After Sweep Is Reported Test" << std::endl;
}

int main() {
    std::cout << "--- Running Expanded Unit Tests ---" << std::endl;
    test_partial_fill();
    test_full_sweep();
    test_order_cancellation();
    test_invalid_inputs();
    test_fifo_order_preserved_after_partial_fill();
    test_best_ask_resets_to_sentinel_after_full_sweep();
    test_best_bid_resets_to_sentinel_after_full_sweep();
    test_best_ask_skips_gap_after_sweep();
    test_higher_ask_after_sweep_is_reported();
    std::cout << "--- All Tests Passed Successfully! ---" << std::endl;
    return 0;
}