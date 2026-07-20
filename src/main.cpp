#include "OrderBook.hpp"
#include <iostream>

int main() {
    OrderBook book;

    std::cout << "--- Submitting Orders ---" << std::endl;
    // Add Sell limit order: Sell 100 shares at $105
    book.insert_order(1, 105, 100, false);
    std::cout << "Best Ask Price: " << book.get_best_ask() << std::endl;

    // Add Buy limit order: Buy 50 shares at $105 (Crosses spread!)
    book.insert_order(2, 105, 50, true);
    std::cout << "Trade Executed! Remaining Ask Price: " << book.get_best_ask() << std::endl;

    return 0;
}