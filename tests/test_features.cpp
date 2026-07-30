// Tests for the research feature emitter (include/FeatureEmit.hpp).
//
// Every number in the OFI study downstream is derived from these rows, so an
// error here doesn't produce a crash — it produces a *plausible wrong answer*,
// which is far worse. Two things in particular are easy to get backwards and
// impossible to notice by eye once aggregated:
//
//   1. The sign of the OFI increment. A sign flip turns "buying pressure
//      predicts price rises" into an equally confident claim of the opposite.
//   2. The sign of signed trade volume. LOBSTER's Direction field on an
//      execution names the side of the RESTING order, not the aggressor.
//
// So the fixture below is small enough that every expected OFI value is worked
// out by hand in the comments, from the definition, rather than recorded from
// whatever the code happened to print.

#include "../include/LobsterReplay.hpp"
#include "check.hpp"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr size_t LEVELS = 2;

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

// The sign convention, asserted directly rather than inferred from a fixture.
// LOBSTER Direction = 1 means a resting BUY limit order was executed, i.e.
// somebody sold into the bid — a seller-initiated trade, negative signed volume.
void test_trade_sign_convention() {
    CHECK(lobster::signed_execution_size(1, 100) == -100);   // resting bid hit -> sell
    CHECK(lobster::signed_execution_size(-1, 100) == 100);   // resting ask lifted -> buy
    std::cout << "[PASS] Signed trade convention (resting side vs. aggressor)" << std::endl;
}

// A scripted session whose OFI increments are all hand-computable. The
// definition, for reference:
//
//   e_n = 1{Pb_n >= Pb_n-1} qb_n  -  1{Pb_n <= Pb_n-1} qb_n-1
//       -  1{Pa_n <= Pa_n-1} qa_n  +  1{Pa_n >= Pa_n-1} qa_n-1
void test_ofi_values() {
    // time,event,id,size,price,direction   (direction: 1=bid, -1=ask)
    std::vector<std::string> messages = {
        "34200.0,1,1,100,5850000,-1",   // 1: new ask 100 @ 5850000   (no bid yet)
        "34200.1,1,2,200,5849000,1",    // 2: new bid 200 @ 5849000
        "34200.2,1,3,300,5849000,1",    // 3: bid level grows to 500
        "34200.3,1,4,50,5849500,1",     // 4: better bid, new level 50 @ 5849500
        "34200.4,3,4,50,5849500,1",     // 5: delete it; best bid falls back
        "34200.5,1,5,40,5849800,-1",    // 6: better ask, new level 40 @ 5849800
        "34200.6,4,5,40,5849800,-1",    // 7: execute it; best ask falls back
    };
    std::vector<std::string> rows = {
        book_row({{5850000, 100}}, {}),
        book_row({{5850000, 100}}, {{5849000, 200}}),
        book_row({{5850000, 100}}, {{5849000, 500}}),
        book_row({{5850000, 100}}, {{5849500, 50}, {5849000, 500}}),
        book_row({{5850000, 100}}, {{5849000, 500}}),
        book_row({{5849800, 40}, {5850000, 100}}, {{5849000, 500}}),
        book_row({{5850000, 100}}, {{5849000, 500}}),
    };
    write_file("feat_message.csv", messages);
    write_file("feat_orderbook.csv", rows);

    std::vector<lobster::FeatureRow> got;
    lobster::Stats st;
    std::string err;
    bool ok = lobster::replay_and_reconcile(
        "feat_message.csv", "feat_orderbook.csv", LEVELS, st, err, /*recover=*/false,
        [&](const lobster::FeatureRow& r) { got.push_back(r); });

    if (!ok) std::cerr << "unexpected divergence: " << err << std::endl;
    CHECK(ok);                              // fixture must reconcile, or the
    CHECK(got.size() == messages.size());   // features describe a wrong book

    // Row 1: the book is one-sided (no bid), so OFI is undefined.
    CHECK(got[0].quote_valid == 0);
    CHECK(got[0].ofi == 0);
    CHECK(got[0].ask_px == 5850000 && got[0].ask_sz == 100);
    CHECK(got[0].bid_sz == 0);

    // Row 2: both sides now live, but the PREVIOUS update was one-sided, so the
    // increment still has no defined predecessor. Emitted, flagged invalid.
    CHECK(got[1].quote_valid == 0);
    CHECK(got[1].ofi == 0);

    // Row 3: bid 200 -> 500 at an unchanged price; ask unchanged.
    //   bid: +500 (price held) - 200 (price held) = +300
    //   ask: -100 (price held) + 100 (price held) = 0
    CHECK(got[2].quote_valid == 1);
    CHECK(got[2].ofi == 300);

    // Row 4: best bid steps UP to 5849500 x 50. A better bid adds its own size
    // and does not retire the old one.
    //   bid: +50 (price improved) - 0 = +50
    //   ask: unchanged = 0
    CHECK(got[3].ofi == 50);
    CHECK(got[3].bid_px == 5849500 && got[3].bid_sz == 50);
    CHECK(got[3].bid_depth_vol == 550);   // 50 @ 5849500 + 500 @ 5849000

    // Row 5: that bid is deleted; best bid steps DOWN to 5849000 x 500.
    //   bid: 0 (price worsened, no add) - 50 (old level retired) = -50
    CHECK(got[4].ofi == -50);

    // Row 6: a better ask appears — sell-side pressure, so OFI goes negative.
    //   ask: -40 (price improved) + 0 = -40
    CHECK(got[5].ofi == -40);
    CHECK(got[5].ask_px == 5849800 && got[5].ask_sz == 40);

    // Row 7: that ask is executed. Ask liquidity leaves, which is upward
    // pressure, and the trade was buyer-initiated (a resting ask was lifted).
    //   ask: 0 (price worsened) + 40 (old level retired) = +40
    CHECK(got[6].ofi == 40);
    CHECK(got[6].signed_trade_sz == 40);
    CHECK(got[6].signed_hidden_sz == 0);

    // OFI must sum to the net change in queue sizes over a stretch where the
    // best prices start and end at the same place — a coarse conservation check
    // independent of the per-row arithmetic above. Rows 3..7 begin and end at
    // bid 5849000 x 500 / ask 5850000 x 100, so the increments must cancel.
    int64_t total = 0;
    for (size_t i = 2; i < got.size(); ++i) total += got[i].ofi;
    CHECK(total == 300);   // the one unreturned change: bid 200 -> 500 in row 3

    std::cout << "[PASS] OFI increments match hand-computed values" << std::endl;
}

// A one-sided book must break the chain rather than carrying a stale quote
// across the gap: an OFI computed against a quote from before the gap would be
// silently meaningless.
void test_one_sided_book_breaks_chain() {
    std::vector<std::string> messages = {
        "34200.0,1,1,100,5850000,-1",   // ask only
        "34200.1,1,2,200,5849000,1",    // both sides
        "34200.2,3,2,200,5849000,1",    // bid deleted -> one-sided again
        "34200.3,1,3,200,5849000,1",    // bid returns
        "34200.4,1,4,100,5849000,1",    // bid grows: first valid increment after
    };
    std::vector<std::string> rows = {
        book_row({{5850000, 100}}, {}),
        book_row({{5850000, 100}}, {{5849000, 200}}),
        book_row({{5850000, 100}}, {}),
        book_row({{5850000, 100}}, {{5849000, 200}}),
        book_row({{5850000, 100}}, {{5849000, 300}}),
    };
    write_file("feat_gap_message.csv", messages);
    write_file("feat_gap_orderbook.csv", rows);

    std::vector<lobster::FeatureRow> got;
    lobster::Stats st;
    std::string err;
    bool ok = lobster::replay_and_reconcile(
        "feat_gap_message.csv", "feat_gap_orderbook.csv", LEVELS, st, err, false,
        [&](const lobster::FeatureRow& r) { got.push_back(r); });

    if (!ok) std::cerr << "unexpected divergence: " << err << std::endl;
    CHECK(ok);
    CHECK(got.size() == 5);
    CHECK(got[0].quote_valid == 0);   // no bid
    CHECK(got[1].quote_valid == 0);   // previous update had no bid
    CHECK(got[2].quote_valid == 0);   // bid gone again
    CHECK(got[3].quote_valid == 0);   // previous update was one-sided
    CHECK(got[4].quote_valid == 1);   // chain re-established
    CHECK(got[4].ofi == 100);         // bid 200 -> 300, ask unchanged
    for (const auto& r : got)
        if (r.quote_valid == 0) CHECK(r.ofi == 0);

    std::cout << "[PASS] One-sided book breaks the OFI chain" << std::endl;
}

// The emitter must not perturb what it observes: a run with a sink attached has
// to reconcile identically to one without, or the features describe a book the
// reconciliation never validated.
void test_emission_does_not_change_replay() {
    lobster::Stats bare, sunk;
    std::string e1, e2;
    bool ok1 = lobster::replay_and_reconcile(
        "feat_message.csv", "feat_orderbook.csv", LEVELS, bare, e1);
    bool ok2 = lobster::replay_and_reconcile(
        "feat_message.csv", "feat_orderbook.csv", LEVELS, sunk, e2,
        false, [](const lobster::FeatureRow&) {});

    CHECK(ok1 == ok2);
    CHECK(bare.messages == sunk.messages);
    CHECK(bare.unknown_refs == sunk.unknown_refs);
    CHECK(bare.seed_attributed == sunk.seed_attributed);
    CHECK(bare.unexpected_trades == sunk.unexpected_trades);

    std::cout << "[PASS] Feature emission leaves the replay unchanged" << std::endl;
}

}  // namespace

int main() {
    test_trade_sign_convention();
    test_ofi_values();
    test_one_sided_book_breaks_chain();
    test_emission_does_not_change_replay();
    std::cout << "\n--- All Feature Emitter Tests Passed ---" << std::endl;
    return 0;
}
