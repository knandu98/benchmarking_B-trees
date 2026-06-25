# Hybrid-Memory B-Trees — Prototype

A small, self-contained C++17 prototype that recreates the core ideas of my 2025
research project **"Exploring Hybrid Memory B+-Trees: Profiling, Design
Decisions, and Trade-offs"** (Otto von Guericke University, Magdeburg).

The original benchmarking code is built on a private research codebase and
cannot be released, so this repository is a **clean-room reimplementation of the
key concepts** for portfolio/demonstration purposes. It reproduces the paper's
central result — *why a hybrid NVM-DRAM FP-Tree beats a fully-persistent
NVM-optimized B+-tree* — in a few hundred lines you can build and run in seconds.

> The technical paper is included in this repo
> ([PDF](DBSE_Project___Exploring_Hybrid_Memory_B__Trees__Profiling__Design_Decisions__and_Trade_offs.pdf)).

## The idea in one paragraph

Non-Volatile Memory (NVM / Storage-Class Memory) is byte-addressable and
persistent like DRAM, but **writes are far more expensive**. So the structure
that issues the *fewest* persistent writes usually wins. The **NVM-optimized
B+-tree** persists *every* node it touches, so a split or a merge causes write
amplification. The **FP-Tree** keeps inner nodes in DRAM (rebuildable on
recovery) and only persists *leaves*, using fingerprints and unsorted leaves to
keep each leaf update to a single small write. This prototype models that write
cost and measures the difference.

## What's implemented

| Component | File | Notes |
|---|---|---|
| NVM cost model | [include/nvm_simulator.hpp](include/nvm_simulator.hpp) | Counts persists / bytes and charges a per-write latency. |
| NVM-optimized B+-tree | [include/bplus_tree.hpp](include/bplus_tree.hpp) | Configurable degree; full insert/search/delete (split, borrow, merge); **every** node modification is persisted. |
| FP-Tree | [include/fp_tree.hpp](include/fp_tree.hpp) | Fingerprinting, selective persistence (DRAM inner / NVM leaves), unsorted leaves with a validity bitmap. |
| Benchmark + profiler | [src/main.cpp](src/main.cpp) | Warm-up + Search/Insert/Delete phases; reports latency *and* NVM write counts; emits CSV. |
| Correctness tests | [tests/correctness.cpp](tests/correctness.cpp) | Cross-validates both trees against `std::map`. |

### FP-Tree design principles modeled (Oukid et al., 2016)

1. **Fingerprinting** — a 1-byte hash per key; lookups scan fingerprints first
   and only compare full keys on a match.
2. **Selective persistence** — leaves in NVM, inner nodes in DRAM (zero NVM cost
   to modify).
3. *Selective concurrency* — out of scope (single-threaded, like the paper's
   micro-benchmarks).
4. **Selective write-back** — unsorted leaves: insert appends to a free slot,
   delete clears a validity bit — each a single small persist.

## Build & run

Requires CMake ≥ 3.16 and a C++17 compiler.

```bash
./scripts/run.sh                 # build, test, and benchmark with defaults
# or with custom key counts:
./scripts/run.sh 1000 100000 1000000
```

Manual build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build            # correctness
./build/benchmark                 # micro-benchmark -> results/benchmark_results.csv
```

## Sample output

Latency is reported in ns/op **including the simulated NVM write cost**
(300 ns/persist by default). The last two columns are the NVM writes issued
during each phase — the write-amplification metric from the paper.

```
=== n_keys = 500000 ===
Operation  |      B+-tree |      FP-Tree |  B+ writes |  FP writes
-----------+--------------+--------------+------------+------------
Search     |        387.9 |        175.7 |          0 |          0
Insert     |        748.6 |        941.5 |     500000 |     500000
Delete     |        822.0 |        665.6 |     925811 |     500000
```

**What this shows (matching the paper's conclusions):**

- **Search**: the FP-Tree's fingerprint filter makes leaf lookups cheaper.
- **Delete**: the B+-tree's merges persist many nodes (≈925k writes vs 500k) —
  classic **write amplification**. The FP-Tree just clears a validity bit.
- **Insert/Search scale logarithmically** for both, confirming scalability.

(Numbers vary by machine; the *ratios* and the write-count gap are the point.)

## Reading the CSV

`results/benchmark_results.csv` columns:

`tree, n_keys, operation, real_ns_per_op, nvm_ns_per_op, total_ns_per_op,
nvm_persists, leaf_persists, inner_persists`

- `real_ns_per_op` — measured CPU/wall time (fingerprint scan vs. binary search).
- `nvm_ns_per_op` — simulated stall on NVM writes (the persistence cost).
- `leaf_persists` / `inner_persists` — where the writes land (the FP-Tree's
  `inner_persists` is always 0 by design).

## Scope & honest limitations

This is a **prototype for demonstration**, not the research artifact:

- NVM is *modeled* (a write counter + latency), not real Optane/`pmem` hardware.
- Profiling is built-in counters, not Intel VTune (the paper used VTune/perf/Valgrind).
- Single-threaded; fixed-size 8-byte integer keys/values (as in the paper's
  fixed-key experiments). Variable-length strings and zero-copy deserialization
  (Cista) are listed as future work in the paper and are not implemented here.

## Reference

Oukid, Lasperas, Nica, Willhalm, Lehner. *FPTree: A Hybrid SCM-DRAM Persistent
and Concurrent B-Tree for Storage Class Memory.* SIGMOD 2016.