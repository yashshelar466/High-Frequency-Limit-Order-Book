// CLI entry point for LOBSTER market-data replay + book reconciliation.
// All of the logic lives in include/LobsterReplay.hpp so it can be tested
// directly against synthetic fixtures (tests/test_lobster_replay.cpp).
//
// Usage:
//   run_lobster <message.csv> <orderbook.csv> [levels]
//
// Download a free sample ticker-day from https://lobsterdata.com/info/DataSamples.php
// (the data is not redistributed in this repository). Exits non-zero on the
// first divergence between the reconstructed book and the venue's published one.

#include "../include/LobsterReplay.hpp"

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::vector<std::string> positional;
    bool recover = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--recover") recover = true;
        else positional.push_back(a);
    }

    if (positional.size() < 2) {
        std::cerr << "usage: " << argv[0]
                  << " <message.csv> <orderbook.csv> [levels] [--recover]\n"
                     "\n"
                     "  Strict (default): stop at the first published level the\n"
                     "  message stream cannot explain, reporting how far the book\n"
                     "  reconstructed exactly.\n"
                     "\n"
                     "  --recover: adopt unexplained levels and continue, reporting\n"
                     "  how many adoptions the session needed. A top-N feed omits\n"
                     "  events outside its price window, so such levels can appear\n"
                     "  unannounced; size mismatches and phantom levels still fail.\n";
        return 2;
    }
    const std::string msg_path = positional[0];
    const std::string book_path = positional[1];
    const size_t levels = (positional.size() > 2)
                              ? static_cast<size_t>(std::stoul(positional[2])) : 10;

    lobster::Stats st;
    std::string err;
    if (!lobster::replay_and_reconcile(msg_path, book_path, levels, st, err, recover)) {
        std::cerr << "FAILED after " << st.messages << " messages: " << err << "\n";
        if (!recover) {
            std::cerr << "\nStrict reconstruction horizon: " << st.messages
                      << " messages at depth " << levels
                      << ". Re-run with --recover to continue past unexplained"
                         " levels and count them.\n";
        }
        return 1;
    }

    std::cout << "Reconciled " << st.messages
              << " messages against LOBSTER's published top-" << levels
              << " book with zero divergences"
              << (recover ? " (recover mode).\n" : " (strict mode).\n")
              << "  skipped: " << st.skipped_hidden << " hidden executions, "
              << st.skipped_cross << " cross, " << st.skipped_halt << " halt\n"
              << "  events attributed to seeded pre-window liquidity: "
              << st.seed_attributed << "\n"
              << "  references to orders not present in the file: "
              << st.unknown_refs << "\n";
    if (recover) {
        std::cout << "  unexplained levels adopted from the published book: "
                  << st.recovered_levels << "\n"
                  << "  phantom levels pruned (died outside the window): "
                  << st.pruned_levels << "\n";
    }
    return 0;
}
