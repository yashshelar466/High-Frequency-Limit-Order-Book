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

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::vector<std::string> positional;
    bool recover = false;
    bool resync = false;
    bool published_features = false;
    std::string features_path;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--recover") {
            recover = true;
        } else if (a == "--resync") {
            resync = true;
        } else if (a == "--published-features") {
            published_features = true;
        } else if (a == "--emit-features") {
            if (i + 1 >= argc) {
                std::cerr << "--emit-features needs an output path\n";
                return 2;
            }
            features_path = argv[++i];
        } else {
            positional.push_back(a);
        }
    }

    if (positional.size() < 2) {
        std::cerr << "usage: " << argv[0]
                  << " <message.csv> <orderbook.csv> [levels]"
                     " [--recover|--resync] [--emit-features <out.csv>]\n"
                     "\n"
                     "  Strict (default): stop at the first published level the\n"
                     "  message stream cannot explain, reporting how far the book\n"
                     "  reconstructed exactly.\n"
                     "\n"
                     "  --recover: adopt unexplained levels and continue, reporting\n"
                     "  how many adoptions the session needed. A top-N feed omits\n"
                     "  events outside its price window, so such levels can appear\n"
                     "  unannounced; size mismatches and phantom levels still fail.\n"
                     "\n"
                     "  --resync: recover, and additionally correct the SIZE of\n"
                     "  levels we already track to match the venue, counting every\n"
                     "  correction and its depth. Reconciliation can then never\n"
                     "  fail, so this is for feature extraction over a full session,\n"
                     "  never a correctness claim -- read the counts it reports.\n"
                     "\n"
                     "  --published-features: take the emitted book state from the\n"
                     "  venue's published rows instead of our reconstruction, and\n"
                     "  continue past divergences, reporting the horizon rather than\n"
                     "  stopping at it. A top-N feed omits events outside its price\n"
                     "  window, so no reconstruction can track a full session exactly;\n"
                     "  for a study the venue's own book is the better source, and a\n"
                     "  drifted reconstruction would feed measurement error into every\n"
                     "  coefficient. Use this for feature extraction; use the default\n"
                     "  when you want to measure reconstruction accuracy.\n"
                     "\n"
                     "  --emit-features: write one row per message describing the\n"
                     "  reconstructed book (top of book, depth, OFI, signed trade\n"
                     "  flow) for downstream analysis — see research/. Pair it with\n"
                     "  --recover to cover a full session; in strict mode the file\n"
                     "  stops where the reconciliation does.\n";
        return 2;
    }
    const std::string msg_path = positional[0];
    const std::string book_path = positional[1];
    const size_t levels = (positional.size() > 2)
                              ? static_cast<size_t>(std::stoul(positional[2])) : 10;

    lobster::Stats st;
    std::string err;

    std::ofstream features;
    lobster::FeatureSink sink;
    uint64_t rows_written = 0;
    if (!features_path.empty()) {
        features.open(features_path);
        if (!features) {
            std::cerr << "cannot open " << features_path << " for writing\n";
            return 2;
        }
        features << lobster::feature_csv_header() << '\n';
        sink = [&](const lobster::FeatureRow& r) {
            lobster::write_feature_csv(features, r);
            ++rows_written;
        };
    }

    const bool ok = lobster::replay_and_reconcile(msg_path, book_path, levels,
                                                  st, err, recover, resync, sink,
                                                  published_features,
                                                  /*continue_past_divergence=*/
                                                  published_features);
    if (sink) {
        features.flush();
        std::cout << "Wrote " << rows_written << " feature rows to "
                  << features_path << "\n";
    }

    if (ok && published_features) {
        std::cout << "Features taken from the venue's published book.\n";
        if (st.first_divergence)
            std::cout << "  independent reconstruction matched exactly for the first "
                      << st.first_divergence << " messages, then diverged;\n"
                         "  run without --published-features to stop there and see why.\n";
        else
            std::cout << "  our independent reconstruction also matched throughout.\n";
    }

    if (!ok) {
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
              << (resync ? " (resync mode).\n"
                         : recover ? " (recover mode).\n" : " (strict mode).\n")
              << "  skipped: " << st.skipped_hidden << " hidden executions, "
              << st.skipped_cross << " cross, " << st.skipped_halt << " halt\n"
              << "  events attributed to seeded pre-window liquidity: "
              << st.seed_attributed << "\n"
              << "  references to orders not present in the file: "
              << st.unknown_refs << "\n";
    if (resync) {
        std::cout << "  tracked levels whose size was corrected to the venue's: "
                  << st.resynced_levels << " (" << st.resynced_shares
                  << " shares in total)\n";
        if (!st.resync_by_depth.empty()) {
            std::cout << "  corrections by depth (level 1 = touch):";
            for (size_t i = 0; i < st.resync_by_depth.size(); ++i)
                std::cout << " L" << (i + 1) << "=" << st.resync_by_depth[i];
            std::cout << "\n"
                      << "    Corrections concentrated at the deep end are the\n"
                         "    feed's price window; corrections at the touch would\n"
                         "    indicate a defect in the engine instead.\n";
        }
    }
    if (recover || resync) {
        std::cout << "  unexplained levels adopted from the published book: "
                  << st.recovered_levels << "\n"
                  << "  phantom levels pruned (died outside the window): "
                  << st.pruned_levels << "\n";
    }
    return 0;
}
