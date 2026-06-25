// correctness.cpp
//
// Cross-validates both tree implementations against std::map under a random mix
// of insert / search / delete operations. This is the safety net that lets the
// benchmark numbers be trusted: if a tree disagrees with the reference map, the
// test aborts.

#include <cassert>
#include <cstdint>
#include <iostream>
#include <map>
#include <random>

#include "bplus_tree.hpp"
#include "fp_tree.hpp"

using namespace hmb;

template <typename Tree>
void check(const char* name, int degree) {
    Tree t(degree);
    std::map<int64_t, int64_t> ref;
    std::mt19937_64 rng(7);

    for (int i = 0; i < 50000; ++i) {
        int op = static_cast<int>(rng() % 3);
        int64_t k = static_cast<int64_t>(rng() % 3000);
        if (op == 0) {
            t.insert(k, k * 10);
            ref[k] = k * 10;
        } else if (op == 1) {
            auto v = t.search(k);
            auto it = ref.find(k);
            assert((it == ref.end()) == (!v.has_value()));
            if (v) assert(*v == it->second);
        } else {
            bool r = t.remove(k);
            bool e = ref.erase(k) > 0;
            assert(r == e);
        }
    }

    // Full final scan.
    for (int64_t k = 0; k < 3000; ++k) {
        auto v = t.search(k);
        auto it = ref.find(k);
        assert((it == ref.end()) == (!v.has_value()));
        if (v) assert(*v == it->second);
    }

    std::cout << name << ": OK (" << ref.size() << " live keys)\n";
}

int main() {
    check<BPlusTree<int64_t, int64_t>>("B+-tree", 16);
    check<BPlusTree<int64_t, int64_t>>("B+-tree (degree=4)", 4);
    check<FPTree<int64_t, int64_t, 16>>("FP-Tree", 16);
    check<FPTree<int64_t, int64_t, 8>>("FP-Tree (leaf=8)", 8);
    std::cout << "All correctness checks passed.\n";
    return 0;
}
