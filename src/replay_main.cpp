#include "../include/OrderBook.hpp"
#include "../include/MarketDataFeed.hpp"
#include <cstdint>
#include <iostream>

int main() {
    OrderBook book;
    MarketDataFeed feed(book);

    // Collect executions as the feed replays. In a real backtest this is where
    // fills feed a trade blotter / PnL / VWAP calculation; here we just tally.
    uint64_t trade_count = 0;
    uint64_t traded_volume = 0;
    uint64_t notional = 0;  // sum of price * qty across all fills
    book.set_trade_handler([&](const Trade& t) {
        ++trade_count;
        traded_volume += t.qty;
        notional += static_cast<uint64_t>(t.price) * t.qty;
        std::cout << "  TRADE: taker=" << t.taker_id << " maker=" << t.maker_id
                  << " " << (t.taker_is_buy ? "BUY " : "SELL")
                  << " qty=" << t.qty << " @ " << t.price << std::endl;
    });

    std::cout << "--- Starting Market Data Replay ---" << std::endl;

    if (feed.replay_from_csv("data/ticks.csv", false)) {
        std::cout << "\n--- Execution Summary ---" << std::endl;
        std::cout << "Trades Executed: " << trade_count << std::endl;
        std::cout << "Volume Traded:   " << traded_volume << std::endl;
        if (traded_volume > 0) {
            std::cout << "VWAP:            "
                      << static_cast<double>(notional) / traded_volume << std::endl;
        }

        std::cout << "\n--- Final Order Book State ---" << std::endl;
        std::cout << "Best Bid Price: " << book.get_best_bid() << std::endl;
        std::cout << "Best Ask Price: " << book.get_best_ask() << std::endl;
        if (book.get_best_bid() != 0 && book.get_best_ask() != 0xFFFFFFFF) {
            std::cout << "Spread:         " << book.spread()
                      << "   Mid: " << book.mid_price() << std::endl;
        }

        std::cout << "\n--- L2 Depth (top 5) ---" << std::endl;
        std::cout << "Asks (low -> high):" << std::endl;
        auto asks = book.get_ask_depth(5);
        for (auto it = asks.rbegin(); it != asks.rend(); ++it) {
            std::cout << "  " << it->price << "  x " << it->volume
                      << "  (" << it->order_count << " orders)" << std::endl;
        }
        std::cout << "Bids (high -> low):" << std::endl;
        for (const auto& lvl : book.get_bid_depth(5)) {
            std::cout << "  " << lvl.price << "  x " << lvl.volume
                      << "  (" << lvl.order_count << " orders)" << std::endl;
        }
    }

    return 0;
}