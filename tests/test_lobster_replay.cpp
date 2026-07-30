// Tests for the LOBSTER replay/reconciliation logic.
//
// The real LOBSTER data is not redistributed with this repository (see the
// README), so these tests build synthetic fixtures in LOBSTER's exact CSV
// format and drive the same code path `run_lobster` uses. That checks two
// things the real data alone could not:
//
//   1. A correct message stream reconciles with zero divergences — the parser,
//      the event mapping, and the depth comparison all agree.
//   2. A *deliberately corrupted* published book is DETECTED. Without this, a
//      reconciler that silently passed everything would look identical to one
//      that works — the same trap as tests whose assertions compile out.

#include "../include/LobsterReplay.hpp"
#include "check.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr size_t LEVELS = 2;   // small book keeps the fixtures readable

// Build one orderbook row in LOBSTER's interleaved format:
// Ask1,AskSize1,Bid1,BidSize1,Ask2,AskSize2,Bid2,BidSize2
// Absent levels use LOBSTER's padding sentinels with size 0.
std::string book_row(const std::vector<std::pair<int64_t, int64_t>>& asks,
                     const std::vector<std::pair<int64_t, int64_t>>& bids) {
    std::string row;
    for (size_t i = 0; i < LEVELS; ++i) {
        if (i) row += ",";
        if (i < asks.size()) row += std::to_string(asks[i].first) + "," + std::to_string(asks[i].second);
        else                 row += std::to_string(lobster::NO_ASK) + ",0";
        row += ",";
        if (i < bids.size()) row += std::to_string(bids[i].first) + "," + std::to_string(bids[i].second);
        else                 row += std::to_string(lobster::NO_BID) + ",0";
    }
    return row;
}

void write_file(const std::string& path, const std::vector<std::string>& lines) {
    std::ofstream f(path);
    for (const auto& l : lines) f << l << "\n";
}

// A scripted session exercising every event type the replayer handles:
// new limit orders on both sides, a partial cancel, a visible execution, a full
// delete, and a hidden execution (which must NOT move the visible book).
struct Fixture {
    std::vector<std::string> messages;
    std::vector<std::string> book;
};

Fixture build_fixture() {
    Fixture fx;
    auto step = [&](const std::string& msg,
                    const std::vector<std::pair<int64_t, int64_t>>& asks,
                    const std::vector<std::pair<int64_t, int64_t>>& bids) {
        fx.messages.push_back(msg);
        fx.book.push_back(book_row(asks, bids));
    };

    // time,event,id,size,price,direction   (direction: 1=bid, -1=ask)
    step("34200.0,1,1,100,5850000,-1", {{5850000, 100}}, {});
    step("34200.1,1,2,200,5849000,1",  {{5850000, 100}}, {{5849000, 200}});
    step("34200.2,1,3,50,5851000,-1",  {{5850000, 100}, {5851000, 50}}, {{5849000, 200}});
    step("34200.3,1,4,300,5849000,1",  {{5850000, 100}, {5851000, 50}}, {{5849000, 500}});

    // Partial cancel: order 2 shrinks by 150 (bid level 5849000: 500 -> 350).
    step("34200.4,2,2,150,5849000,1",  {{5850000, 100}, {5851000, 50}}, {{5849000, 350}});

    // Visible execution against order 1 (ask 5850000: 100 -> 40).
    step("34200.5,4,1,60,5850000,-1",  {{5850000, 40}, {5851000, 50}}, {{5849000, 350}});

    // Hidden execution: must not change the visible book at all.
    step("34200.6,5,0,25,5850500,-1",  {{5850000, 40}, {5851000, 50}}, {{5849000, 350}});

    // Full delete of order 1 clears the 5850000 ask level entirely.
    step("34200.7,3,1,40,5850000,-1",  {{5851000, 50}}, {{5849000, 350}});

    // Remaining bid orders deleted one at a time.
    step("34200.8,3,2,50,5849000,1",   {{5851000, 50}}, {{5849000, 300}});
    step("34200.9,3,4,300,5849000,1",  {{5851000, 50}}, {});
    return fx;
}

void test_reconciles_clean_stream() {
    Fixture fx = build_fixture();
    write_file("lobster_fixture_message.csv", fx.messages);
    write_file("lobster_fixture_orderbook.csv", fx.book);

    lobster::Stats st;
    std::string err;
    bool ok = lobster::replay_and_reconcile(
        "lobster_fixture_message.csv", "lobster_fixture_orderbook.csv", LEVELS, st, err);

    if (!ok) std::cerr << "unexpected divergence: " << err << std::endl;
    CHECK(ok);
    CHECK(st.messages == fx.messages.size());
    CHECK(st.skipped_hidden == 1);        // the hidden execution was skipped
    CHECK(st.unexpected_trades == 0);     // engine matching never fired
    CHECK(st.unknown_refs == 0);          // every referenced order existed

    std::cout << "[PASS] LOBSTER Replay Reconciles Clean Stream Test" << std::endl;
}

void test_reconciles_preexisting_opening_liquidity() {
    // LOBSTER's message stream starts at market open, but the venue's actual
    // book at that instant already holds resting liquidity established
    // before the capture window (the opening cross, carryover orders), with
    // no corresponding "new order" message in the file. The very first
    // published row can therefore show real depth beyond what message 1
    // alone explains. This reproduces that shape: message 1 accounts for
    // only the best bid; the second bid level and both ask levels are
    // phantom, pre-existing liquidity the replayer must bootstrap around.
    std::vector<std::string> messages = {
        "34200.0,1,500,10,5850000,1",   // new buy limit order, id 500, size 10 @ 5850000
    };
    std::vector<std::string> rows = {
        book_row({{5851000, 100}, {5852000, 50}},   // ask levels: entirely phantom
                {{5850000, 10}, {5849000, 200}}),   // bid 1: message 1's own order; bid 2: phantom
    };
    write_file("lobster_warm_message.csv", messages);
    write_file("lobster_warm_orderbook.csv", rows);

    lobster::Stats st;
    std::string err;
    bool ok = lobster::replay_and_reconcile(
        "lobster_warm_message.csv", "lobster_warm_orderbook.csv", LEVELS, st, err);

    if (!ok) std::cerr << "unexpected divergence: " << err << std::endl;
    CHECK(ok);
    CHECK(st.messages == 1);
    CHECK(st.unexpected_trades == 0);

    std::cout << "[PASS] LOBSTER Replay Reconciles Pre-Existing Opening Liquidity Test" << std::endl;
}

void test_attributes_unknown_id_to_seeded_liquidity() {
    // Reproduces a real failure from AAPL 2012-06-21: a delete referencing an
    // order id far older than anything in the file (i.e. resting before the
    // capture window, seeded here under a synthetic id). We can't match the id,
    // but the venue is telling us `size` shares left that exact (side, price) —
    // so the reduction is attributed to the seeded aggregate at that level.
    std::vector<std::string> messages = {
        "34200.0,1,500,10,5850000,1",       // msg 1: our own order, best bid
        "34200.1,3,15836282,100,5851000,-1" // msg 2: delete a PRE-WINDOW ask
    };
    std::vector<std::string> rows = {
        // Row 1: ask 5851000 x 300 is pre-existing liquidity (no message created it).
        book_row({{5851000, 300}, {5852000, 50}}, {{5850000, 10}, {5849000, 200}}),
        // Row 2: venue drops it to 200 after the delete of 100 shares.
        book_row({{5851000, 200}, {5852000, 50}}, {{5850000, 10}, {5849000, 200}}),
    };
    write_file("lobster_seedref_message.csv", messages);
    write_file("lobster_seedref_orderbook.csv", rows);

    lobster::Stats st;
    std::string err;
    bool ok = lobster::replay_and_reconcile(
        "lobster_seedref_message.csv", "lobster_seedref_orderbook.csv", LEVELS, st, err);

    if (!ok) std::cerr << "unexpected divergence: " << err << std::endl;
    CHECK(ok);
    CHECK(st.seed_attributed == 1);   // resolved against seeded liquidity...
    CHECK(st.unknown_refs == 0);      // ...rather than being dropped on the floor

    std::cout << "[PASS] LOBSTER Replay Attributes Unknown ID To Seeded Liquidity Test" << std::endl;
}

void test_seeds_full_depth_when_comparing_shallower() {
    // Reproduces a real failure from AAPL 2012-06-21 at reconcile-depth 5:
    // seeding only the compared depth throws away published liquidity below
    // it, so when the top of book is consumed, a level that should promote
    // into view is simply missing from our book. Seeding must use the file's
    // FULL published depth even when the comparison window is shallower.
    //
    // Here the file publishes 2 levels but we reconcile only 1. Deleting the
    // best bid must promote the second level — which only exists in our book
    // if it was seeded despite being outside the comparison window.
    std::vector<std::string> messages = {
        "34200.0,1,500,10,5850000,1",        // msg 1: our order, becomes best bid
        "34200.1,3,500,10,5850000,1",        // msg 2: delete it; level 2 promotes
    };
    std::vector<std::string> rows = {
        book_row({{5851000, 100}, {5852000, 50}}, {{5850000, 10}, {5849000, 200}}),
        book_row({{5851000, 100}, {5852000, 50}}, {{5849000, 200}}),
    };
    write_file("lobster_depth_message.csv", messages);
    write_file("lobster_depth_orderbook.csv", rows);

    lobster::Stats st;
    std::string err;
    bool ok = lobster::replay_and_reconcile(
        "lobster_depth_message.csv", "lobster_depth_orderbook.csv",
        /*levels=*/1, st, err);   // compare 1 level, file publishes 2

    if (!ok) std::cerr << "unexpected divergence: " << err << std::endl;
    CHECK(ok);
    CHECK(st.messages == 2);

    std::cout << "[PASS] LOBSTER Replay Seeds Full Depth When Comparing Shallower Test" << std::endl;
}

void test_detects_corrupted_book() {
    Fixture fx = build_fixture();
    // Corrupt one published size mid-stream. A reconciler that actually
    // compares must catch this; one that silently passes would not.
    fx.book[5] = book_row({{5850000, 41}, {5851000, 50}}, {{5849000, 350}});

    write_file("lobster_bad_message.csv", fx.messages);
    write_file("lobster_bad_orderbook.csv", fx.book);

    lobster::Stats st;
    std::string err;
    bool ok = lobster::replay_and_reconcile(
        "lobster_bad_message.csv", "lobster_bad_orderbook.csv", LEVELS, st, err);

    CHECK(!ok);                       // divergence must be reported
    CHECK(st.messages == 6);          // caught on the corrupted row, not later
    CHECK(err.find("divergence") != std::string::npos);

    std::cout << "[PASS] LOBSTER Replay Detects Corrupted Book Test" << std::endl;
}

void test_detects_dropped_message() {
    // Dropping a message desynchronizes the stream from the published book;
    // the reconciler must notice rather than drift silently.
    Fixture fx = build_fixture();
    fx.messages.erase(fx.messages.begin() + 4);   // drop the partial cancel
    fx.book.erase(fx.book.begin() + 4);

    write_file("lobster_drop_message.csv", fx.messages);
    write_file("lobster_drop_orderbook.csv", fx.book);

    lobster::Stats st;
    std::string err;
    bool ok = lobster::replay_and_reconcile(
        "lobster_drop_message.csv", "lobster_drop_orderbook.csv", LEVELS, st, err);

    CHECK(!ok);
    std::cout << "[PASS] LOBSTER Replay Detects Dropped Message Test" << std::endl;
}

void cleanup() {
    for (const char* f : {"lobster_fixture_message.csv", "lobster_fixture_orderbook.csv",
                          "lobster_warm_message.csv", "lobster_warm_orderbook.csv",
                          "lobster_seedref_message.csv", "lobster_seedref_orderbook.csv",
                          "lobster_depth_message.csv", "lobster_depth_orderbook.csv",
                          "lobster_bad_message.csv", "lobster_bad_orderbook.csv",
                          "lobster_drop_message.csv", "lobster_drop_orderbook.csv"}) {
        std::remove(f);
    }
}

}  // namespace

int main() {
    std::cout << "--- Running LOBSTER Replay Tests ---" << std::endl;
    test_reconciles_clean_stream();
    test_reconciles_preexisting_opening_liquidity();
    test_attributes_unknown_id_to_seeded_liquidity();
    test_seeds_full_depth_when_comparing_shallower();
    test_detects_corrupted_book();
    test_detects_dropped_message();
    cleanup();
    std::cout << "--- All LOBSTER Replay Tests Passed! ---" << std::endl;
    return 0;
}
