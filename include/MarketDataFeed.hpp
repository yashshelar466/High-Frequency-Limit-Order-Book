#pragma once
#include "OrderBook.hpp"
#include <string>
#include <fstream>
#include <sstream>
#include <iostream>
#include <chrono>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

enum class EventType {
    ADD,
    CANCEL,
    EXECUTE
};

struct MarketTick {
    uint64_t timestamp; // Epoch nanoseconds or milliseconds
    EventType event;
    uint64_t order_id;
    uint32_t price;
    uint32_t quantity;
    bool is_buy;
};

class MarketDataFeed {
public:
    MarketDataFeed(OrderBook& book) : order_book(book) {}

    // Process a single tick directly into the matching engine
    void process_tick(const MarketTick& tick) {
        switch (tick.event) {
            case EventType::ADD:
                order_book.insert_order(tick.order_id, tick.price, tick.quantity, tick.is_buy);
                break;
            case EventType::CANCEL:
                order_book.cancel_order(tick.order_id);
                break;
            case EventType::EXECUTE:
                // Execution handled automatically during insert matching in OrderBook
                break;
        }
    }

    // Read and stream a CSV file containing market tick events
    bool replay_from_csv(const std::string& filepath, bool simulate_realtime = false) {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            std::cerr << "Error: Could not open tick data file: " << filepath << std::endl;
            return false;
        }

        std::string line;
        // Skip header if present
        std::getline(file, line);

        uint64_t last_timestamp = 0;
        size_t processed_count = 0;

        while (std::getline(file, line)) {
            if (line.empty()) continue;

            std::stringstream ss(line);
            std::string token;

            MarketTick tick;
            std::string event_str, side_str;

            // CSV Format: timestamp,event_type,order_id,price,quantity,side
            std::getline(ss, token, ','); tick.timestamp = std::stoull(token);
            std::getline(ss, event_str, ',');
            std::getline(ss, token, ','); tick.order_id = std::stoull(token);
            std::getline(ss, token, ','); tick.price = std::stoul(token);
            std::getline(ss, token, ','); tick.quantity = std::stoul(token);
            std::getline(ss, side_str, ','); tick.is_buy = (side_str == "BUY" || side_str == "B");

            if (event_str == "ADD") tick.event = EventType::ADD;
            else if (event_str == "CANCEL") tick.event = EventType::CANCEL;
            else continue;

            // Optional: simulate real-time delay based on timestamps
            if (simulate_realtime && last_timestamp > 0 && tick.timestamp > last_timestamp) {
                auto delta = tick.timestamp - last_timestamp;
    #ifdef _WIN32
                Sleep(static_cast<DWORD>((delta + 999) / 1000));
    #else
                std::this_thread::sleep_for(std::chrono::microseconds(delta));
    #endif
            }
            last_timestamp = tick.timestamp;

            process_tick(tick);
            processed_count++;
        }

        std::cout << "Successfully replayed " << processed_count << " market events." << std::endl;
        return true;
    }

private:
    OrderBook& order_book;
};