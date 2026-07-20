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

int main() {
    std::cout << "--- Running Expanded Unit Tests ---" << std::endl;
    test_partial_fill();
    test_full_sweep();
    test_order_cancellation();
    test_invalid_inputs();
    std::cout << "--- All Tests Passed Successfully! ---" << std::endl;
    return 0;
}