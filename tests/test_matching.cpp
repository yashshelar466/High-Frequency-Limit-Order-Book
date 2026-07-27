#include "../include/OrderBook.hpp"
#include <cassert>
#include <iostream>
#include <vector>

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

void test_duplicate_order_id_rejected() {
    OrderBook book;

    // Resting ask under id 1.
    book.insert_order(1, 105, 100, false);

    // Re-using id 1 for a different resting ask must be rejected outright, not
    // overwrite the map entry (which would orphan the original order).
    book.insert_order(1, 106, 50, false);

    // No level was created at 106, and the original @105 is untouched.
    assert(book.get_ask_level(106) == nullptr);
    const PriceLevel* lvl = book.get_ask_level(105);
    assert(lvl != nullptr && lvl->order_count == 1 && lvl->total_volume == 100);

    // The original order is still reachable for cancellation via id 1.
    book.cancel_order(1);
    assert(book.get_ask_level(105) == nullptr);
    assert(book.get_best_ask() == 0xFFFFFFFF);

    std::cout << "[PASS] Duplicate Order ID Rejected Test" << std::endl;
}

void test_trade_reporting_partial_fill() {
    OrderBook book;
    std::vector<Trade> trades;
    book.set_trade_handler([&](const Trade& t) { trades.push_back(t); });

    book.insert_order(1, 105, 100, false);   // resting ask, no fill yet
    assert(trades.empty());

    book.insert_order(2, 105, 40, true);     // buy 40 crosses -> one fill of 40

    assert(trades.size() == 1);
    const Trade& t = trades[0];
    assert(t.taker_id == 2 && t.maker_id == 1);
    assert(t.price == 105 && t.qty == 40);   // executes at the resting maker's price
    assert(t.taker_is_buy == true);

    std::cout << "[PASS] Trade Reporting Partial Fill Test" << std::endl;
}

void test_trade_reporting_multi_maker_sweep() {
    OrderBook book;
    std::vector<Trade> trades;
    book.set_trade_handler([&](const Trade& t) { trades.push_back(t); });

    // Two resting sells at the same price, plus one a tick higher.
    book.insert_order(1, 105, 50, false);
    book.insert_order(2, 105, 30, false);
    book.insert_order(3, 106, 40, false);

    // Aggressive buy for 100 @ 106: fills 50 (id1) + 30 (id2) at 105, then 20
    // (id3) at 106, in price-time order. 20 of id3 remains resting.
    book.insert_order(4, 106, 100, true);

    assert(trades.size() == 3);
    assert(trades[0].maker_id == 1 && trades[0].price == 105 && trades[0].qty == 50);
    assert(trades[1].maker_id == 2 && trades[1].price == 105 && trades[1].qty == 30);
    assert(trades[2].maker_id == 3 && trades[2].price == 106 && trades[2].qty == 20);
    for (const auto& t : trades) {
        assert(t.taker_id == 4 && t.taker_is_buy == true);
    }

    // Remainder of id3 rests; total filled equals the 100 the buyer wanted.
    const PriceLevel* lvl = book.get_ask_level(106);
    assert(lvl != nullptr && lvl->total_volume == 20);

    std::cout << "[PASS] Trade Reporting Multi-Maker Sweep Test" << std::endl;
}

void test_trade_reporting_sell_aggressor() {
    OrderBook book;
    std::vector<Trade> trades;
    book.set_trade_handler([&](const Trade& t) { trades.push_back(t); });

    book.insert_order(1, 100, 60, true);     // resting bid
    book.insert_order(2, 100, 25, false);    // sell 25 crosses -> fill of 25

    assert(trades.size() == 1);
    const Trade& t = trades[0];
    assert(t.taker_id == 2 && t.maker_id == 1);
    assert(t.price == 100 && t.qty == 25);
    assert(t.taker_is_buy == false);

    std::cout << "[PASS] Trade Reporting Sell Aggressor Test" << std::endl;
}

void test_no_trades_for_resting_order() {
    OrderBook book;
    int count = 0;
    book.set_trade_handler([&](const Trade&) { ++count; });

    book.insert_order(1, 105, 100, false);   // rests
    book.insert_order(2, 100, 50, true);     // rests, no cross
    book.cancel_order(1);

    assert(count == 0);
    std::cout << "[PASS] No Trades For Resting Orders Test" << std::endl;
}

void test_amend_qty_decrease_keeps_priority() {
    OrderBook book;

    // Three resting asks at the same price, in FIFO order.
    book.insert_order(1, 105, 50, false);
    book.insert_order(2, 105, 30, false);
    book.insert_order(3, 105, 20, false);

    // Reduce the head order's size. It must stay at the head (priority kept)
    // and the level volume must drop by the reduction.
    book.amend_order(1, 105, 10);

    const PriceLevel* lvl = book.get_ask_level(105);
    assert(lvl != nullptr && lvl->order_count == 3);
    assert(lvl->total_volume == 60);  // 10 + 30 + 20
    assert(lvl->head->id == 1 && lvl->head->qty == 10);
    assert(lvl->head->next->id == 2);
    assert(lvl->head->next->next->id == 3);

    std::cout << "[PASS] Amend Qty-Decrease Keeps Priority Test" << std::endl;
}

void test_amend_price_change_requeues() {
    OrderBook book;

    book.insert_order(1, 105, 50, false);
    book.insert_order(2, 106, 30, false);

    // Move order 1 up to 106; it loses priority and joins the back of 106.
    book.amend_order(1, 106, 50);

    assert(book.get_ask_level(105) == nullptr);  // vacated
    const PriceLevel* lvl = book.get_ask_level(106);
    assert(lvl != nullptr && lvl->order_count == 2 && lvl->total_volume == 80);
    // Order 2 was already resting, so it sits ahead of the re-queued order 1.
    assert(lvl->head->id == 2);
    assert(lvl->head->next->id == 1);

    std::cout << "[PASS] Amend Price Change Re-queues Test" << std::endl;
}

void test_amend_qty_increase_loses_priority() {
    OrderBook book;

    book.insert_order(1, 105, 20, false);
    book.insert_order(2, 105, 30, false);

    // Increasing size at the same price loses priority: order 1 goes to the back.
    book.amend_order(1, 105, 40);

    const PriceLevel* lvl = book.get_ask_level(105);
    assert(lvl != nullptr && lvl->order_count == 2 && lvl->total_volume == 70);
    assert(lvl->head->id == 2);          // order 2 now has priority
    assert(lvl->head->next->id == 1 && lvl->head->next->qty == 40);

    std::cout << "[PASS] Amend Qty-Increase Loses Priority Test" << std::endl;
}

void test_amend_into_cross_executes() {
    OrderBook book;
    std::vector<Trade> trades;
    book.set_trade_handler([&](const Trade& t) { trades.push_back(t); });

    book.insert_order(1, 105, 40, false);   // resting ask
    book.insert_order(2, 100, 40, true);    // resting bid, no cross

    // Re-price the bid up to 105: it now crosses the ask and executes.
    book.amend_order(2, 105, 40);

    assert(trades.size() == 1);
    assert(trades[0].taker_id == 2 && trades[0].maker_id == 1);
    assert(trades[0].price == 105 && trades[0].qty == 40);
    assert(book.get_ask_level(105) == nullptr);   // ask fully consumed
    assert(book.get_best_ask() == 0xFFFFFFFF);
    assert(book.get_bid_level(100) == nullptr);   // original bid vacated

    std::cout << "[PASS] Amend Into Cross Executes Test" << std::endl;
}

void test_amend_unknown_and_invalid_noop() {
    OrderBook book;
    book.insert_order(1, 105, 40, false);

    book.amend_order(999, 105, 10);  // unknown id
    book.amend_order(1, 105, 0);     // zero qty
    book.amend_order(1, 1000000, 10);// out-of-range price

    const PriceLevel* lvl = book.get_ask_level(105);
    assert(lvl != nullptr && lvl->order_count == 1 && lvl->total_volume == 40);
    assert(lvl->head->id == 1 && lvl->head->qty == 40);

    std::cout << "[PASS] Amend Unknown/Invalid No-op Test" << std::endl;
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
    test_duplicate_order_id_rejected();
    test_trade_reporting_partial_fill();
    test_trade_reporting_multi_maker_sweep();
    test_trade_reporting_sell_aggressor();
    test_no_trades_for_resting_order();
    test_amend_qty_decrease_keeps_priority();
    test_amend_price_change_requeues();
    test_amend_qty_increase_loses_priority();
    test_amend_into_cross_executes();
    test_amend_unknown_and_invalid_noop();
    std::cout << "--- All Tests Passed Successfully! ---" << std::endl;
    return 0;
}