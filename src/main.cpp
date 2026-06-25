// main.cpp
//
// Micro-benchmark + profiling harness that reproduces, at small scale, the
// methodology from "Exploring Hybrid Memory B+-Trees: Profiling, Design
// Decisions, and Trade-offs".
//
// For each key count it:
//   1. Warms up the tree with N unique keys.
//   2. Runs N back-to-back Search, Insert(update), and Delete operations.
//   3. Reports per-operation latency AND the simulated NVM write cost — the two
//      quantities the paper measures via micro-benchmarking and VTune profiling.
//
// Output:
//   * A human-readable comparison table on stdout.
//   * A CSV (results/benchmark_results.csv) you can plot for a portfolio.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "bplus_tree.hpp"
#include "fp_tree.hpp"
#include "nvm_simulator.hpp"

using namespace hmb;
using Key = int64_t;    // 8-byte key, as in the paper
using Value = int64_t;  // 8-byte value

using Clock = std::chrono::steady_clock;

struct OpResult {
    double real_ns_per_op = 0.0;       // measured CPU/wall time per op
    double nvm_ns_per_op = 0.0;        // simulated NVM stall per op
    double total_ns_per_op = 0.0;      // real + nvm
    uint64_t nvm_persists = 0;         // NVM writes issued during the phase
    uint64_t leaf_persists = 0;
    uint64_t inner_persists = 0;
};

struct PhaseReport {
    OpResult search;
    OpResult insert;
    OpResult remove;
};

// Snapshot helper: run `fn` over all keys, attribute NVM stats + time to it.
template <typename Fn>
OpResult time_phase(std::size_t op_count, Fn&& fn) {
    auto& nvm = NvmSimulator::instance();
    NvmStats before = nvm.stats();

    auto t0 = Clock::now();
    fn();
    auto t1 = Clock::now();

    NvmStats after = nvm.stats();
    uint64_t persists = after.persists - before.persists;
    uint64_t leafp = after.leaf_persists - before.leaf_persists;
    uint64_t innerp = after.inner_persists - before.inner_persists;

    double real_ns =
        std::chrono::duration<double, std::nano>(t1 - t0).count();
    double nvm_ns = static_cast<double>(persists) * NVM_WRITE_LATENCY_NS;

    OpResult r;
    r.real_ns_per_op = real_ns / op_count;
    r.nvm_ns_per_op = nvm_ns / op_count;
    r.total_ns_per_op = (real_ns + nvm_ns) / op_count;
    r.nvm_persists = persists;
    r.leaf_persists = leafp;
    r.inner_persists = innerp;
    return r;
}

template <typename Tree>
PhaseReport benchmark_tree(Tree& tree, const std::vector<Key>& keys) {
    const std::size_t n = keys.size();
    PhaseReport rep;

    // ---- warm-up: bulk insert (not timed as the "insert" phase) -----------
    for (std::size_t i = 0; i < n; ++i) {
        tree.insert(keys[i], keys[i] + 1);
    }

    // ---- search phase -----------------------------------------------------
    volatile Value sink = 0;
    rep.search = time_phase(n, [&] {
        for (std::size_t i = 0; i < n; ++i) {
            auto v = tree.search(keys[i]);
            if (v) sink += *v;
        }
    });
    (void)sink;

    // ---- insert/update phase (re-insert existing keys -> updates) ---------
    rep.insert = time_phase(n, [&] {
        for (std::size_t i = 0; i < n; ++i) {
            tree.insert(keys[i], keys[i] + 2);
        }
    });

    // ---- delete phase -----------------------------------------------------
    rep.remove = time_phase(n, [&] {
        for (std::size_t i = 0; i < n; ++i) {
            tree.remove(keys[i]);
        }
    });

    return rep;
}

std::vector<Key> make_keys(std::size_t n, uint64_t seed) {
    std::vector<Key> keys(n);
    std::mt19937_64 rng(seed);
    for (std::size_t i = 0; i < n; ++i) keys[i] = static_cast<Key>(i);
    std::shuffle(keys.begin(), keys.end(), rng);  // uniform, random order
    return keys;
}

void print_op(const std::string& label, const OpResult& b, const OpResult& f) {
    std::cout << std::left << std::setw(10) << label << " | " << std::right
              << std::setw(12) << std::fixed << std::setprecision(1)
              << b.total_ns_per_op << " | " << std::setw(12)
              << f.total_ns_per_op << " | " << std::setw(10) << b.nvm_persists
              << " | " << std::setw(10) << f.nvm_persists << "\n";
}

int main(int argc, char** argv) {
    // Small-scale key counts (the paper uses 50k..50M; we scale down so the
    // prototype runs in seconds). Override via CLI: ./benchmark 1000 10000 ...
    std::vector<std::size_t> key_counts = {1000, 10000, 100000, 500000};
    if (argc > 1) {
        key_counts.clear();
        for (int i = 1; i < argc; ++i) {
            key_counts.push_back(std::stoul(argv[i]));
        }
    }

    const int bplus_degree = 128;  // a representative "knee point" from the sweep
    const int fp_inner_degree = 128;

    std::cout << "Hybrid-Memory B-Tree Prototype — micro-benchmark + NVM profiling\n";
    std::cout << "  B+-tree degree = " << bplus_degree
              << ", FP-Tree inner degree = " << fp_inner_degree
              << ", leaf size = 64\n";
    std::cout << "  Simulated NVM write latency = " << NVM_WRITE_LATENCY_NS
              << " ns/persist\n\n";

    std::ofstream csv("results/benchmark_results.csv");
    csv << "tree,n_keys,operation,real_ns_per_op,nvm_ns_per_op,"
           "total_ns_per_op,nvm_persists,leaf_persists,inner_persists\n";

    auto dump_csv = [&](const std::string& tree, std::size_t n,
                        const std::string& op, const OpResult& r) {
        csv << tree << "," << n << "," << op << "," << r.real_ns_per_op << ","
            << r.nvm_ns_per_op << "," << r.total_ns_per_op << ","
            << r.nvm_persists << "," << r.leaf_persists << ","
            << r.inner_persists << "\n";
    };

    for (std::size_t n : key_counts) {
        auto keys = make_keys(n, /*seed=*/42);

        NvmSimulator::instance().reset();
        BPlusTree<Key, Value> bplus(bplus_degree);
        PhaseReport bp = benchmark_tree(bplus, keys);

        NvmSimulator::instance().reset();
        FPTree<Key, Value, 64> fp(fp_inner_degree);
        PhaseReport fpr = benchmark_tree(fp, keys);

        std::cout << "=== n_keys = " << n << " ===\n";
        std::cout << "Latency in ns/op (lower is better), incl. simulated NVM cost\n";
        std::cout << std::left << std::setw(10) << "Operation" << " | "
                  << std::right << std::setw(12) << "B+-tree" << " | "
                  << std::setw(12) << "FP-Tree" << " | " << std::setw(10)
                  << "B+ writes" << " | " << std::setw(10) << "FP writes"
                  << "\n";
        std::cout << "-----------+--------------+--------------+------------+"
                     "------------\n";
        print_op("Search", bp.search, fpr.search);
        print_op("Insert", bp.insert, fpr.insert);
        print_op("Delete", bp.remove, fpr.remove);
        std::cout << "\n";

        dump_csv("bplus", n, "search", bp.search);
        dump_csv("bplus", n, "insert", bp.insert);
        dump_csv("bplus", n, "delete", bp.remove);
        dump_csv("fptree", n, "search", fpr.search);
        dump_csv("fptree", n, "insert", fpr.insert);
        dump_csv("fptree", n, "delete", fpr.remove);
    }

    csv.close();
    std::cout << "Wrote results/benchmark_results.csv\n";
    return 0;
}
