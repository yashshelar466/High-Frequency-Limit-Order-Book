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

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: " << argv[0]
                  << " <message.csv> <orderbook.csv> [levels]\n";
        return 2;
    }
    const std::string msg_path = argv[1];
    const std::string book_path = argv[2];
    const size_t levels = (argc > 3) ? static_cast<size_t>(std::stoul(argv[3])) : 10;

    lobster::Stats st;
    std::string err;
    if (!lobster::replay_and_reconcile(msg_path, book_path, levels, st, err)) {
        std::cerr << "FAILED after " << st.messages << " messages: " << err << "\n";
        return 1;
    }

    std::cout << "Reconciled " << st.messages
              << " messages against LOBSTER's published top-" << levels
              << " book with zero divergences.\n"
              << "  skipped: " << st.skipped_hidden << " hidden executions, "
              << st.skipped_cross << " cross, " << st.skipped_halt << " halt\n"
              << "  references to orders not present in the file: "
              << st.unknown_refs << "\n";
    return 0;
}
