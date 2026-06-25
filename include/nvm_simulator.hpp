// nvm_simulator.hpp
//
// A lightweight model of Non-Volatile Memory (NVM / Storage-Class Memory).
//
// In the real project, NVM sits on the memory bus and is byte-addressable like
// DRAM, but it has two important properties that drive every design decision:
//   1. Writes (persists) are noticeably more expensive than DRAM writes.
//   2. Durability requires explicit cache-line flushes / persist barriers.
//
// We cannot ship real Optane / pmem hardware in a portable prototype, so this
// header models the *cost* of persisting data. Both trees route their
// persistent writes through this layer, which lets us reproduce the paper's
// central observation: the structure that issues fewer NVM writes (the FP-Tree,
// thanks to selective write-back) wins on latency.

#ifndef HMB_NVM_SIMULATOR_HPP
#define HMB_NVM_SIMULATOR_HPP

#include <cstddef>
#include <cstdint>

namespace hmb {

// Cost (in nanoseconds) charged per NVM persist. This is a deliberately simple
// proxy: real SCM write latency is several hundred ns vs. tens of ns for DRAM.
// The absolute value does not matter for a showcase; the *ratio* of NVM writes
// between the two trees is what reproduces the paper's trade-off.
constexpr double NVM_WRITE_LATENCY_NS = 300.0;

// Tracks how much each data structure leans on persistent memory.
struct NvmStats {
    uint64_t persists = 0;        // number of persist() calls (≈ cache-line flushes)
    uint64_t bytes_written = 0;   // total bytes routed through NVM
    uint64_t leaf_persists = 0;   // persists attributed to leaf nodes
    uint64_t inner_persists = 0;  // persists attributed to inner nodes

    void reset() {
        persists = 0;
        bytes_written = 0;
        leaf_persists = 0;
        inner_persists = 0;
    }

    // Simulated time spent stalling on NVM writes for the recorded persists.
    double simulated_ns() const {
        return static_cast<double>(persists) * NVM_WRITE_LATENCY_NS;
    }
};

enum class NodeKind { Leaf, Inner };

// One global NVM device shared by the running process. Single-threaded by
// design — the paper's micro-benchmarks are single-threaded too.
class NvmSimulator {
public:
    static NvmSimulator& instance() {
        static NvmSimulator sim;
        return sim;
    }

    // Record a persist of `bytes` belonging to a node of kind `kind`.
    void persist(std::size_t bytes, NodeKind kind) {
        stats_.persists++;
        stats_.bytes_written += bytes;
        if (kind == NodeKind::Leaf) {
            stats_.leaf_persists++;
        } else {
            stats_.inner_persists++;
        }
    }

    const NvmStats& stats() const { return stats_; }
    void reset() { stats_.reset(); }

private:
    NvmSimulator() = default;
    NvmStats stats_;
};

// Convenience helper used throughout the tree implementations.
inline void nvm_persist(std::size_t bytes, NodeKind kind) {
    NvmSimulator::instance().persist(bytes, kind);
}

}  // namespace hmb

#endif  // HMB_NVM_SIMULATOR_HPP
