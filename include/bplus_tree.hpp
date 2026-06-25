// bplus_tree.hpp
//
// An NVM-optimized B+-tree (the "baseline" structure in the paper).
//
// Design notes that mirror the paper:
//   * Order / degree is configurable (the paper sweeps degree 32..1024 to find
//     a "knee point"). Higher degree => shorter tree => fewer levels touched.
//   * The whole tree lives in NVM: every node that is structurally modified
//     must be persisted. This is what causes the write amplification the paper
//     reports — an insert that splits nodes persists several nodes.
//   * Leaves are linked for fast range scans (classic B+-tree property).
//
// This is intentionally a clean, single-threaded reference implementation:
// correct insert / search / delete (with borrow + merge), readable over clever.

#ifndef HMB_BPLUS_TREE_HPP
#define HMB_BPLUS_TREE_HPP

#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

#include "nvm_simulator.hpp"

namespace hmb {

template <typename Key, typename Value>
class BPlusTree {
public:
    // `degree` is the maximum number of children an internal node may hold.
    // A node therefore holds at most `degree - 1` keys.
    explicit BPlusTree(int degree) : degree_(degree < 3 ? 3 : degree) {
        root_ = new Node(true);
        persist(root_);
    }

    ~BPlusTree() { destroy(root_); }

    BPlusTree(const BPlusTree&) = delete;
    BPlusTree& operator=(const BPlusTree&) = delete;

    // Insert or update. Returns true on a fresh insert, false on update.
    bool insert(const Key& key, const Value& value) {
        Node* leaf = find_leaf(key);
        auto it = std::lower_bound(leaf->keys.begin(), leaf->keys.end(), key);
        std::size_t idx = static_cast<std::size_t>(it - leaf->keys.begin());

        if (it != leaf->keys.end() && *it == key) {
            leaf->values[idx] = value;  // update in place
            persist(leaf);
            return false;
        }

        leaf->keys.insert(it, key);
        leaf->values.insert(leaf->values.begin() + idx, value);
        persist(leaf);

        if (static_cast<int>(leaf->keys.size()) > max_keys()) {
            split_leaf(leaf);
        }
        return true;
    }

    // Point lookup.
    std::optional<Value> search(const Key& key) const {
        Node* leaf = find_leaf(key);
        auto it = std::lower_bound(leaf->keys.begin(), leaf->keys.end(), key);
        if (it != leaf->keys.end() && *it == key) {
            std::size_t idx = static_cast<std::size_t>(it - leaf->keys.begin());
            return leaf->values[idx];
        }
        return std::nullopt;
    }

    // Remove a key. Returns true if the key existed.
    bool remove(const Key& key) {
        bool ok = remove_internal(root_, key);
        // The root may become empty after a merge: collapse it.
        if (!root_->leaf && root_->keys.empty()) {
            Node* old = root_;
            root_ = root_->children[0];
            old->children.clear();
            delete old;
        }
        return ok;
    }

private:
    struct Node {
        bool leaf;
        std::vector<Key> keys;
        std::vector<Value> values;    // leaf only
        std::vector<Node*> children;  // inner only
        Node* next = nullptr;         // leaf sibling link
        explicit Node(bool is_leaf) : leaf(is_leaf) {}
    };

    int degree_;
    Node* root_;

    int max_keys() const { return degree_ - 1; }
    int min_keys() const { return (degree_ - 1) / 2; }

    // Every persist of a B+-tree node hits NVM (the tree is fully persistent).
    void persist(Node* n) const {
        std::size_t bytes = sizeof(Node) + n->keys.size() * sizeof(Key) +
                            n->values.size() * sizeof(Value) +
                            n->children.size() * sizeof(Node*);
        nvm_persist(bytes, n->leaf ? NodeKind::Leaf : NodeKind::Inner);
    }

    Node* find_leaf(const Key& key) const {
        Node* cur = root_;
        while (!cur->leaf) {
            auto it = std::upper_bound(cur->keys.begin(), cur->keys.end(), key);
            std::size_t idx = static_cast<std::size_t>(it - cur->keys.begin());
            cur = cur->children[idx];
        }
        return cur;
    }

    void split_leaf(Node* leaf) {
        int mid = static_cast<int>(leaf->keys.size()) / 2;
        Node* right = new Node(true);
        right->keys.assign(leaf->keys.begin() + mid, leaf->keys.end());
        right->values.assign(leaf->values.begin() + mid, leaf->values.end());
        leaf->keys.resize(mid);
        leaf->values.resize(mid);

        right->next = leaf->next;
        leaf->next = right;

        persist(leaf);
        persist(right);

        Key up = right->keys.front();
        insert_into_parent(leaf, up, right);
    }

    void split_inner(Node* node) {
        int mid = static_cast<int>(node->keys.size()) / 2;
        Key up = node->keys[mid];

        Node* right = new Node(false);
        right->keys.assign(node->keys.begin() + mid + 1, node->keys.end());
        right->children.assign(node->children.begin() + mid + 1,
                               node->children.end());
        node->keys.resize(mid);
        node->children.resize(mid + 1);

        persist(node);
        persist(right);

        insert_into_parent(node, up, right);
    }

    void insert_into_parent(Node* left, const Key& key, Node* right) {
        Node* parent = find_parent(root_, left);
        if (parent == nullptr) {
            // `left` was the root: grow a new root.
            Node* new_root = new Node(false);
            new_root->keys.push_back(key);
            new_root->children.push_back(left);
            new_root->children.push_back(right);
            root_ = new_root;
            persist(new_root);
            return;
        }

        auto it = std::upper_bound(parent->keys.begin(), parent->keys.end(), key);
        std::size_t idx = static_cast<std::size_t>(it - parent->keys.begin());
        parent->keys.insert(it, key);
        parent->children.insert(parent->children.begin() + idx + 1, right);
        persist(parent);

        if (static_cast<int>(parent->keys.size()) > max_keys()) {
            split_inner(parent);
        }
    }

    Node* find_parent(Node* start, Node* child) const {
        if (start->leaf || start->children.empty()) return nullptr;
        for (Node* c : start->children) {
            if (c == child) return start;
        }
        for (Node* c : start->children) {
            if (!c->leaf) {
                Node* p = find_parent(c, child);
                if (p) return p;
            }
        }
        return nullptr;
    }

    // ---- deletion ---------------------------------------------------------

    bool remove_internal(Node* node, const Key& key) {
        if (node->leaf) {
            auto it = std::lower_bound(node->keys.begin(), node->keys.end(), key);
            if (it == node->keys.end() || *it != key) return false;
            std::size_t idx = static_cast<std::size_t>(it - node->keys.begin());
            node->keys.erase(it);
            node->values.erase(node->values.begin() + idx);
            persist(node);
            return true;
        }

        auto it = std::upper_bound(node->keys.begin(), node->keys.end(), key);
        std::size_t idx = static_cast<std::size_t>(it - node->keys.begin());
        Node* child = node->children[idx];
        bool ok = remove_internal(child, key);
        if (!ok) return false;

        if (static_cast<int>(child->keys.size()) < min_keys()) {
            rebalance(node, idx);
        }
        return true;
    }

    void rebalance(Node* parent, std::size_t idx) {
        Node* child = parent->children[idx];

        // Try to borrow from the left sibling.
        if (idx > 0) {
            Node* left = parent->children[idx - 1];
            if (static_cast<int>(left->keys.size()) > min_keys()) {
                borrow_from_left(parent, idx, left, child);
                return;
            }
        }
        // Try to borrow from the right sibling.
        if (idx + 1 < parent->children.size()) {
            Node* right = parent->children[idx + 1];
            if (static_cast<int>(right->keys.size()) > min_keys()) {
                borrow_from_right(parent, idx, child, right);
                return;
            }
        }
        // Otherwise merge with a sibling.
        if (idx > 0) {
            merge(parent, idx - 1);
        } else {
            merge(parent, idx);
        }
    }

    void borrow_from_left(Node* parent, std::size_t idx, Node* left,
                          Node* child) {
        if (child->leaf) {
            child->keys.insert(child->keys.begin(), left->keys.back());
            child->values.insert(child->values.begin(), left->values.back());
            left->keys.pop_back();
            left->values.pop_back();
            parent->keys[idx - 1] = child->keys.front();
        } else {
            child->keys.insert(child->keys.begin(), parent->keys[idx - 1]);
            parent->keys[idx - 1] = left->keys.back();
            left->keys.pop_back();
            child->children.insert(child->children.begin(),
                                   left->children.back());
            left->children.pop_back();
        }
        persist(left);
        persist(child);
        persist(parent);
    }

    void borrow_from_right(Node* parent, std::size_t idx, Node* child,
                           Node* right) {
        if (child->leaf) {
            child->keys.push_back(right->keys.front());
            child->values.push_back(right->values.front());
            right->keys.erase(right->keys.begin());
            right->values.erase(right->values.begin());
            parent->keys[idx] = right->keys.front();
        } else {
            child->keys.push_back(parent->keys[idx]);
            parent->keys[idx] = right->keys.front();
            right->keys.erase(right->keys.begin());
            child->children.push_back(right->children.front());
            right->children.erase(right->children.begin());
        }
        persist(right);
        persist(child);
        persist(parent);
    }

    // Merge children[idx] and children[idx+1] using parent->keys[idx].
    void merge(Node* parent, std::size_t idx) {
        Node* left = parent->children[idx];
        Node* right = parent->children[idx + 1];

        if (left->leaf) {
            left->keys.insert(left->keys.end(), right->keys.begin(),
                              right->keys.end());
            left->values.insert(left->values.end(), right->values.begin(),
                                right->values.end());
            left->next = right->next;
        } else {
            left->keys.push_back(parent->keys[idx]);
            left->keys.insert(left->keys.end(), right->keys.begin(),
                              right->keys.end());
            left->children.insert(left->children.end(), right->children.begin(),
                                  right->children.end());
        }

        parent->keys.erase(parent->keys.begin() + idx);
        parent->children.erase(parent->children.begin() + idx + 1);

        persist(left);
        persist(parent);
        delete right;
    }

    void destroy(Node* n) {
        if (!n) return;
        if (!n->leaf) {
            for (Node* c : n->children) destroy(c);
        }
        delete n;
    }
};

}  // namespace hmb

#endif  // HMB_BPLUS_TREE_HPP
