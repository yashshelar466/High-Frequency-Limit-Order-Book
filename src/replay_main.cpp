#include "../include/OrderBook.hpp"
#include "../include/MarketDataFeed.hpp"
#include <iostream>

int main() {
    OrderBook book;
    MarketDataFeed feed(book);

    std::cout << "--- Starting Market Data Replay ---" << std::endl;
    
    if (feed.replay_from_csv("data/ticks.csv", false)) {
        std::cout << "\n--- Final Order Book State ---" << std::endl;
        std::cout << "Best Bid Price: " << book.get_best_bid() << std::endl;
        std::cout << "Best Ask Price: " << book.get_best_ask() << std::endl;
    }

    return 0;
}