// fp_tree.hpp
//
// The FP-Tree (Fingerprinting Persistent Tree) from Oukid et al., 2016 — the
// "state-of-the-art" structure the paper compares against.
//
// We implement the four design principles that matter for a single-threaded
// showcase (concurrency principle #3 is out of scope here):
//
//   1. Fingerprinting
//        Each leaf stores a 1-byte hash ("fingerprint") of every key. A point
//        lookup first scans the contiguous fingerprint array; only slots whose
//        fingerprint matches require a full key comparison. This turns most leaf
//        searches into a cheap byte scan.
//
//   2. Selective Persistence
//        Leaf nodes live in NVM (persistent). Inner nodes live in DRAM and are
//        considered rebuildable on recovery, so modifying them costs *nothing*
//        in NVM writes. This is the single biggest reason the FP-Tree issues far
//        fewer NVM writes than the fully-persistent B+-tree.
//
//   3. Selective Concurrency  -> out of scope (single-threaded benchmark).
//
//   4. (Effect of) unsorted leaves + selective write-back
//        Leaves are *unsorted*: an insert appends into the first free slot and
//        flips a validity bit, rather than shifting half the leaf. A delete just
//        clears a validity bit. Both are single, small persists — the
//        "selective write-back / lightweight logging" behaviour the paper credits
//        for the FP-Tree's low insert/delete latency.

#ifndef HMB_FP_TREE_HPP
#define HMB_FP_TREE_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "nvm_simulator.hpp"

namespace hmb {

template <typename Key, typename Value, std::size_t LeafSize = 64>
class FPTree {
public:
    explicit FPTree(int inner_degree) : inner_degree_(inner_degree < 3 ? 3 : inner_degree) {
        root_leaf_ = new Leaf();
        persist_leaf(root_leaf_);  // first leaf is persistent
        root_ = nullptr;           // no inner level yet
        first_leaf_ = root_leaf_;
    }

    ~FPTree() {
        destroy_inner(root_);
        // Free the persistent leaf chain.
        Leaf* l = first_leaf_;
        while (l) {
            Leaf* nxt = l->next;
            delete l;
            l = nxt;
        }
    }

    FPTree(const FPTree&) = delete;
    FPTree& operator=(const FPTree&) = delete;

    bool insert(const Key& key, const Value& value) {
        Leaf* leaf = find_leaf(key);
        uint8_t fp = fingerprint(key);

        // Update path: fingerprint scan, then key compare on matches.
        for (std::size_t i = 0; i < LeafSize; ++i) {
            if (leaf->valid[i] && leaf->fps[i] == fp && leaf->keys[i] == key) {
                leaf->values[i] = value;
                persist_leaf(leaf);  // selective write-back: one leaf persist
                return false;
            }
        }

        if (leaf->count == LeafSize) {
            leaf = split_leaf(leaf, key);
        }

        // Append into the first free slot (unsorted leaf, no shifting).
        for (std::size_t i = 0; i < LeafSize; ++i) {
            if (!leaf->valid[i]) {
                leaf->keys[i] = key;
                leaf->values[i] = value;
                leaf->fps[i] = fp;
                leaf->valid[i] = true;
                leaf->count++;
                persist_leaf(leaf);  // single small persist
                return true;
            }
        }
        return true;  // unreachable
    }

    std::optional<Value> search(const Key& key) const {
        Leaf* leaf = find_leaf(key);
        uint8_t fp = fingerprint(key);
        for (std::size_t i = 0; i < LeafSize; ++i) {
            if (leaf->valid[i] && leaf->fps[i] == fp && leaf->keys[i] == key) {
                return leaf->values[i];
            }
        }
        return std::nullopt;
    }

    bool remove(const Key& key) {
        Leaf* leaf = find_leaf(key);
        uint8_t fp = fingerprint(key);
        for (std::size_t i = 0; i < LeafSize; ++i) {
            if (leaf->valid[i] && leaf->fps[i] == fp && leaf->keys[i] == key) {
                leaf->valid[i] = false;  // just clear the validity bit
                leaf->count--;
                persist_leaf(leaf);  // single small persist (lightweight logging)
                return true;
            }
        }
        return false;
    }

private:
    // Persistent leaf (lives in NVM). Fingerprints are packed first so a search
    // touches them as one contiguous, cache-friendly scan.
    struct Leaf {
        std::array<uint8_t, LeafSize> fps{};   // 1-byte key hashes
        std::array<bool, LeafSize> valid{};    // validity bitmap
        std::array<Key, LeafSize> keys{};
        std::array<Value, LeafSize> values{};
        std::size_t count = 0;
        Leaf* next = nullptr;
        Key min_key{};  // smallest live key, used for inner routing after split
    };

    // Volatile inner node (lives in DRAM). Children may be Inner* or Leaf*.
    struct Inner {
        std::vector<Key> keys;
        std::vector<void*> children;  // Inner* or Leaf*
        std::vector<bool> child_is_leaf;
    };

    int inner_degree_;
    Inner* root_;
    Leaf* root_leaf_;   // valid only while the tree is a single leaf
    Leaf* first_leaf_;  // head of the persistent leaf chain

    int max_inner_keys() const { return inner_degree_ - 1; }

    static uint8_t fingerprint(const Key& key) {
        // Cheap 1-byte hash. Mixing keeps fingerprints well-distributed so the
        // fingerprint filter actually prunes work.
        uint64_t h = static_cast<uint64_t>(key);
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33;
        return static_cast<uint8_t>(h & 0xFF);
    }

    // Leaves are persistent: every leaf modification is an NVM write.
    void persist_leaf(Leaf* /*l*/) const {
        nvm_persist(sizeof(Leaf), NodeKind::Leaf);
    }

    Leaf* find_leaf(const Key& key) const {
        if (root_ == nullptr) return root_leaf_;
        Inner* cur = root_;
        while (true) {
            auto it = std::upper_bound(cur->keys.begin(), cur->keys.end(), key);
            std::size_t idx = static_cast<std::size_t>(it - cur->keys.begin());
            void* child = cur->children[idx];
            if (cur->child_is_leaf[idx]) {
                return static_cast<Leaf*>(child);
            }
            cur = static_cast<Inner*>(child);
        }
    }

    // Recompute the smallest live key of a leaf (used for inner routing keys).
    static Key compute_min(Leaf* l) {
        Key m{};
        bool seen = false;
        for (std::size_t i = 0; i < LeafSize; ++i) {
            if (l->valid[i] && (!seen || l->keys[i] < m)) {
                m = l->keys[i];
                seen = true;
            }
        }
        return m;
    }

    // Split a full leaf into two, distributing keys by median. Returns the leaf
    // into which `key` should now be inserted.
    Leaf* split_leaf(Leaf* leaf, const Key& key) {
        // Collect live entries and sort by key to find a clean split point.
        std::vector<std::pair<Key, Value>> entries;
        entries.reserve(leaf->count);
        for (std::size_t i = 0; i < LeafSize; ++i) {
            if (leaf->valid[i]) entries.emplace_back(leaf->keys[i], leaf->values[i]);
        }
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });

        std::size_t mid = entries.size() / 2;
        Key split_key = entries[mid].first;

        Leaf* right = new Leaf();

        // Rewrite the left leaf with the lower half, right leaf with upper half.
        leaf->fps.fill(0);
        leaf->valid.fill(false);
        leaf->count = 0;
        auto put = [](Leaf* dst, const Key& k, const Value& v) {
            std::size_t slot = dst->count;
            dst->keys[slot] = k;
            dst->values[slot] = v;
            dst->fps[slot] = fingerprint(k);
            dst->valid[slot] = true;
            dst->count++;
        };
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (i < mid) {
                put(leaf, entries[i].first, entries[i].second);
            } else {
                put(right, entries[i].first, entries[i].second);
            }
        }

        right->next = leaf->next;
        leaf->next = right;
        leaf->min_key = compute_min(leaf);
        right->min_key = compute_min(right);

        persist_leaf(leaf);   // both persistent leaves are written...
        persist_leaf(right);  // ...but inner-node routing below is DRAM-only.

        insert_into_inner(leaf, split_key, right);

        return (key < split_key) ? leaf : right;
    }

    // Insert a (split_key, right-leaf) routing entry into the DRAM inner level.
    void insert_into_inner(Leaf* left, const Key& split_key, Leaf* right) {
        if (root_ == nullptr) {
            // Grow the first inner level above the two leaves.
            root_ = new Inner();
            root_->keys.push_back(split_key);
            root_->children.push_back(left);
            root_->children.push_back(right);
            root_->child_is_leaf.push_back(true);
            root_->child_is_leaf.push_back(true);
            return;
        }

        std::vector<Inner*> path;
        Inner* cur = root_;
        while (true) {
            path.push_back(cur);
            auto it = std::upper_bound(cur->keys.begin(), cur->keys.end(), split_key);
            std::size_t idx = static_cast<std::size_t>(it - cur->keys.begin());
            if (cur->child_is_leaf[idx]) break;
            cur = static_cast<Inner*>(cur->children[idx]);
        }

        Inner* parent = path.back();
        insert_routing(parent, split_key, right, /*child_is_leaf=*/true);

        // Cascade inner splits up the path. (DRAM only — no NVM cost.)
        for (int i = static_cast<int>(path.size()) - 1; i >= 0; --i) {
            Inner* node = path[i];
            if (static_cast<int>(node->keys.size()) <= max_inner_keys()) break;
            split_inner(node, (i > 0) ? path[i - 1] : nullptr);
        }
    }

    void insert_routing(Inner* node, const Key& key, void* child, bool is_leaf) {
        auto it = std::upper_bound(node->keys.begin(), node->keys.end(), key);
        std::size_t idx = static_cast<std::size_t>(it - node->keys.begin());
        node->keys.insert(it, key);
        node->children.insert(node->children.begin() + idx + 1, child);
        node->child_is_leaf.insert(node->child_is_leaf.begin() + idx + 1, is_leaf);
    }

    void split_inner(Inner* node, Inner* parent) {
        std::size_t mid = node->keys.size() / 2;
        Key up = node->keys[mid];

        Inner* right = new Inner();
        right->keys.assign(node->keys.begin() + mid + 1, node->keys.end());
        right->children.assign(node->children.begin() + mid + 1, node->children.end());
        right->child_is_leaf.assign(node->child_is_leaf.begin() + mid + 1,
                                    node->child_is_leaf.end());

        node->keys.resize(mid);
        node->children.resize(mid + 1);
        node->child_is_leaf.resize(mid + 1);

        if (parent == nullptr) {
            Inner* new_root = new Inner();
            new_root->keys.push_back(up);
            new_root->children.push_back(node);
            new_root->children.push_back(right);
            new_root->child_is_leaf.push_back(false);
            new_root->child_is_leaf.push_back(false);
            root_ = new_root;
        } else {
            insert_routing(parent, up, right, /*is_leaf=*/false);
        }
    }

    void destroy_inner(Inner* node) {
        if (!node) return;
        for (std::size_t i = 0; i < node->children.size(); ++i) {
            if (!node->child_is_leaf[i]) {
                destroy_inner(static_cast<Inner*>(node->children[i]));
            }
        }
        delete node;
    }
};

}  // namespace hmb

#endif  // HMB_FP_TREE_HPP
