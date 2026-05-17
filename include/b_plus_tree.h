#ifndef SYS_PROG_B_PLUS_TREE_H
#define SYS_PROG_B_PLUS_TREE_H

#include <algorithm>
#include <boost/container/static_vector.hpp>
#include <concepts>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <associative_container.h>
#include <pp_allocator.h>

template <typename tkey, typename tvalue, comparator<tkey> compare = std::less<tkey>, std::size_t t = 5>
class BP_tree final : private compare
{
    static_assert(t >= 2, "BP_tree minimal degree must be at least 2");

public:
    using tree_data_type = std::pair<tkey, tvalue>;
    using tree_data_type_const = std::pair<const tkey, tvalue>;
    using value_type = tree_data_type_const;

private:
    static constexpr const size_t minimum_keys_in_node = t - 1;
    static constexpr const size_t maximum_keys_in_node = 2 * t - 1;

    struct bptree_node_base
    {
        bool _is_terminate;

        explicit bptree_node_base(bool is_terminate) noexcept : _is_terminate(is_terminate) {}
        virtual ~bptree_node_base() = default;
    };

    struct bptree_node_term final : public bptree_node_base
    {
        bptree_node_term* _next;
        boost::container::static_vector<tree_data_type, maximum_keys_in_node + 1> _data;

        bptree_node_term() noexcept : bptree_node_base(true), _next(nullptr) {}
    };

    struct bptree_node_middle final : public bptree_node_base
    {
        boost::container::static_vector<tkey, maximum_keys_in_node + 1> _keys;
        boost::container::static_vector<bptree_node_base*, maximum_keys_in_node + 2> _pointers;

        bptree_node_middle() noexcept : bptree_node_base(false) {}
    };

    pp_allocator<value_type> _allocator;
    bptree_node_base* _root;
    size_t _size;

public:
    class key_not_found : public std::out_of_range
    {
    public:
        explicit key_not_found(const std::string& message) : std::out_of_range("BP_tree: " + message) {}
    };

    class invalid_iterator : public std::logic_error
    {
    public:
        explicit invalid_iterator(const std::string& message) : std::logic_error("BP_tree: " + message) {}
    };

private:
    using split_result = std::optional<std::pair<tkey, bptree_node_base*>>;

    inline bool compare_keys(const tkey& lhs, const tkey& rhs) const
    {
        return compare::operator()(lhs, rhs);
    }

    inline bool equal_keys(const tkey& lhs, const tkey& rhs) const
    {
        return !compare_keys(lhs, rhs) && !compare_keys(rhs, lhs);
    }

    inline bool compare_pairs(const tree_data_type& lhs, const tree_data_type& rhs) const
    {
        return compare_keys(lhs.first, rhs.first);
    }

    pp_allocator<value_type> get_allocator() const noexcept
    {
        return _allocator;
    }

    pp_allocator<bptree_node_term> term_allocator() const noexcept
    {
        return pp_allocator<bptree_node_term>(_allocator);
    }

    pp_allocator<bptree_node_middle> middle_allocator() const noexcept
    {
        return pp_allocator<bptree_node_middle>(_allocator);
    }

    bptree_node_term* allocate_term_node()
    {
        return term_allocator().template new_object<bptree_node_term>();
    }

    bptree_node_middle* allocate_middle_node()
    {
        return middle_allocator().template new_object<bptree_node_middle>();
    }

    void deallocate_node(bptree_node_base* node) noexcept
    {
        if (!node) return;
        if (node->_is_terminate) {
            term_allocator().template delete_object<bptree_node_term>(static_cast<bptree_node_term*>(node));
        } else {
            middle_allocator().template delete_object<bptree_node_middle>(static_cast<bptree_node_middle*>(node));
        }
    }

    void destroy_subtree(bptree_node_base* node) noexcept
    {
        if (!node) return;
        if (!node->_is_terminate) {
            auto* middle = static_cast<bptree_node_middle*>(node);
            for (auto* child : middle->_pointers) {
                destroy_subtree(child);
            }
        }
        deallocate_node(node);
    }

    static value_type& as_value(tree_data_type& data) noexcept
    {
        return *std::launder(reinterpret_cast<value_type*>(&data));
    }

    static const value_type& as_value(const tree_data_type& data) noexcept
    {
        return *std::launder(reinterpret_cast<const value_type*>(&data));
    }

    size_t lower_index(const boost::container::static_vector<tree_data_type, maximum_keys_in_node + 1>& data,
                       const tkey& key) const
    {
        size_t left = 0;
        size_t right = data.size();
        while (left < right) {
            const size_t middle = left + (right - left) / 2;
            if (compare_keys(data[middle].first, key)) {
                left = middle + 1;
            } else {
                right = middle;
            }
        }
        return left;
    }

    size_t upper_key_index(const boost::container::static_vector<tkey, maximum_keys_in_node + 1>& keys,
                           const tkey& key) const
    {
        size_t left = 0;
        size_t right = keys.size();
        while (left < right) {
            const size_t middle = left + (right - left) / 2;
            if (!compare_keys(key, keys[middle])) {
                left = middle + 1;
            } else {
                right = middle;
            }
        }
        return left;
    }

    bptree_node_term* first_leaf() noexcept
    {
        auto* current = _root;
        while (current && !current->_is_terminate) {
            current = static_cast<bptree_node_middle*>(current)->_pointers.front();
        }
        return static_cast<bptree_node_term*>(current);
    }

    const bptree_node_term* first_leaf() const noexcept
    {
        auto* current = _root;
        while (current && !current->_is_terminate) {
            current = static_cast<const bptree_node_middle*>(current)->_pointers.front();
        }
        return static_cast<const bptree_node_term*>(current);
    }

    bptree_node_term* find_leaf(const tkey& key) const
    {
        auto* current = _root;
        while (current && !current->_is_terminate) {
            auto* middle = static_cast<bptree_node_middle*>(current);
            const size_t index = upper_key_index(middle->_keys, key);
            current = middle->_pointers[index];
        }
        return static_cast<bptree_node_term*>(current);
    }

    bptree_node_base* clone_subtree(const bptree_node_base* source, bptree_node_term*& previous_leaf)
    {
        if (!source) return nullptr;
        if (source->_is_terminate) {
            const auto* source_leaf = static_cast<const bptree_node_term*>(source);
            auto* leaf = allocate_term_node();
            try {
                leaf->_data = source_leaf->_data;
                if (previous_leaf) previous_leaf->_next = leaf;
                previous_leaf = leaf;
            } catch (...) {
                deallocate_node(leaf);
                throw;
            }
            return leaf;
        }

        const auto* source_middle = static_cast<const bptree_node_middle*>(source);
        auto* middle = allocate_middle_node();
        try {
            middle->_keys = source_middle->_keys;
            for (const auto* child : source_middle->_pointers) {
                middle->_pointers.push_back(clone_subtree(child, previous_leaf));
            }
        } catch (...) {
            destroy_subtree(middle);
            throw;
        }
        return middle;
    }

    tkey subtree_min_key(const bptree_node_base* node) const
    {
        while (node && !node->_is_terminate) {
            node = static_cast<const bptree_node_middle*>(node)->_pointers.front();
        }

        return static_cast<const bptree_node_term*>(node)->_data.front().first;
    }

    void refresh_middle_keys(bptree_node_middle* node)
    {
        node->_keys.clear();
        for (size_t i = 1; i < node->_pointers.size(); ++i) {
            node->_keys.push_back(subtree_min_key(node->_pointers[i]));
        }
    }

    split_result split_leaf_node(bptree_node_term* leaf)
    {
        auto* new_right = allocate_term_node();
        try {
            for (size_t i = t; i < leaf->_data.size(); ++i) {
                new_right->_data.push_back(std::move(leaf->_data[i]));
            }
            leaf->_data.resize(t);
            new_right->_next = leaf->_next;
            leaf->_next = new_right;

            return std::make_pair(new_right->_data.front().first, static_cast<bptree_node_base*>(new_right));
        } catch (...) {
            deallocate_node(new_right);
            throw;
        }
    }

    split_result split_middle_node(bptree_node_middle* node)
    {
        auto* new_right = allocate_middle_node();
        try {
            constexpr size_t promoted_index = t;
            tkey promoted_key = std::move(node->_keys[promoted_index]);

            for (size_t i = promoted_index + 1; i < node->_keys.size(); ++i) {
                new_right->_keys.push_back(std::move(node->_keys[i]));
            }
            for (size_t i = promoted_index + 1; i < node->_pointers.size(); ++i) {
                new_right->_pointers.push_back(node->_pointers[i]);
            }

            node->_keys.resize(promoted_index);
            node->_pointers.resize(promoted_index + 1);

            return std::make_pair(std::move(promoted_key), static_cast<bptree_node_base*>(new_right));
        } catch (...) {
            deallocate_node(new_right);
            throw;
        }
    }

    split_result insert_into_subtree(bptree_node_base* node, tree_data_type data, bool& inserted)
    {
        if (node->_is_terminate) {
            auto* leaf = static_cast<bptree_node_term*>(node);
            const size_t index = lower_index(leaf->_data, data.first);
            if (index < leaf->_data.size() && equal_keys(leaf->_data[index].first, data.first)) {
                inserted = false;
                return std::nullopt;
            }

            leaf->_data.insert(leaf->_data.begin() + static_cast<ptrdiff_t>(index), std::move(data));
            inserted = true;
            return leaf->_data.size() > maximum_keys_in_node ? split_leaf_node(leaf) : std::nullopt;
        }

        auto* middle = static_cast<bptree_node_middle*>(node);
        const size_t child_index = upper_key_index(middle->_keys, data.first);
        split_result child_split = insert_into_subtree(middle->_pointers[child_index], std::move(data), inserted);
        if (!inserted) return std::nullopt;

        if (child_split) {
            middle->_keys.insert(middle->_keys.begin() + static_cast<ptrdiff_t>(child_index),
                                 std::move(child_split->first));
            middle->_pointers.insert(middle->_pointers.begin() + static_cast<ptrdiff_t>(child_index + 1),
                                     child_split->second);
        }

        if (middle->_keys.size() > maximum_keys_in_node) {
            return split_middle_node(middle);
        }

        return std::nullopt;
    }

    void create_new_root(bptree_node_base* left, split_result split)
    {
        auto* new_root = allocate_middle_node();
        try {
            new_root->_keys.push_back(std::move(split->first));
            new_root->_pointers.push_back(left);
            new_root->_pointers.push_back(split->second);
            _root = new_root;
        } catch (...) {
            deallocate_node(new_root);
            throw;
        }
    }

    void collapse_empty_root() noexcept
    {
        if (!_root || _root->_is_terminate) return;

        auto* root = static_cast<bptree_node_middle*>(_root);
        if (!root->_keys.empty()) return;

        if (root->_pointers.empty()) {
            deallocate_node(root);
            _root = nullptr;
            return;
        }

        _root = root->_pointers.front();
        root->_pointers.clear();
        deallocate_node(root);
    }

    bool is_underflow(const bptree_node_base* node) const noexcept
    {
        if (node->_is_terminate) {
            return static_cast<const bptree_node_term*>(node)->_data.size() < minimum_keys_in_node;
        }

        return static_cast<const bptree_node_middle*>(node)->_keys.size() < minimum_keys_in_node;
    }

    void rebalance_leaf_child(bptree_node_middle* parent, size_t child_index)
    {
        auto* leaf = static_cast<bptree_node_term*>(parent->_pointers[child_index]);
        auto* left = child_index > 0
                ? static_cast<bptree_node_term*>(parent->_pointers[child_index - 1])
                : nullptr;
        auto* right = child_index + 1 < parent->_pointers.size()
                ? static_cast<bptree_node_term*>(parent->_pointers[child_index + 1])
                : nullptr;

        if (left && left->_data.size() > minimum_keys_in_node) {
            leaf->_data.insert(leaf->_data.begin(), std::move(left->_data.back()));
            left->_data.pop_back();
            refresh_middle_keys(parent);
            return;
        }

        if (right && right->_data.size() > minimum_keys_in_node) {
            leaf->_data.push_back(std::move(right->_data.front()));
            right->_data.erase(right->_data.begin());
            refresh_middle_keys(parent);
            return;
        }

        if (right) {
            for (auto& item : right->_data) {
                leaf->_data.push_back(std::move(item));
            }
            leaf->_next = right->_next;

            parent->_keys.erase(parent->_keys.begin() + static_cast<ptrdiff_t>(child_index));
            parent->_pointers.erase(parent->_pointers.begin() + static_cast<ptrdiff_t>(child_index + 1));
            deallocate_node(right);
            refresh_middle_keys(parent);
            return;
        }

        if (left) {
            for (auto& item : leaf->_data) {
                left->_data.push_back(std::move(item));
            }
            left->_next = leaf->_next;

            parent->_keys.erase(parent->_keys.begin() + static_cast<ptrdiff_t>(child_index - 1));
            parent->_pointers.erase(parent->_pointers.begin() + static_cast<ptrdiff_t>(child_index));
            deallocate_node(leaf);
            refresh_middle_keys(parent);
        }
    }

    void rebalance_middle_child(bptree_node_middle* parent, size_t child_index)
    {
        auto* child = static_cast<bptree_node_middle*>(parent->_pointers[child_index]);
        auto* left = child_index > 0
                ? static_cast<bptree_node_middle*>(parent->_pointers[child_index - 1])
                : nullptr;
        auto* right = child_index + 1 < parent->_pointers.size()
                ? static_cast<bptree_node_middle*>(parent->_pointers[child_index + 1])
                : nullptr;

        if (left && left->_keys.size() > minimum_keys_in_node) {
            child->_pointers.insert(child->_pointers.begin(), left->_pointers.back());
            left->_pointers.pop_back();
            refresh_middle_keys(left);
            refresh_middle_keys(child);
            refresh_middle_keys(parent);
            return;
        }

        if (right && right->_keys.size() > minimum_keys_in_node) {
            child->_pointers.push_back(right->_pointers.front());
            right->_pointers.erase(right->_pointers.begin());
            refresh_middle_keys(right);
            refresh_middle_keys(child);
            refresh_middle_keys(parent);
            return;
        }

        if (right) {
            for (auto* pointer : right->_pointers) {
                child->_pointers.push_back(pointer);
            }
            parent->_pointers.erase(parent->_pointers.begin() + static_cast<ptrdiff_t>(child_index + 1));
            deallocate_node(right);
            refresh_middle_keys(child);
            refresh_middle_keys(parent);
            return;
        }

        if (left) {
            for (auto* pointer : child->_pointers) {
                left->_pointers.push_back(pointer);
            }
            parent->_pointers.erase(parent->_pointers.begin() + static_cast<ptrdiff_t>(child_index));
            deallocate_node(child);
            refresh_middle_keys(left);
            refresh_middle_keys(parent);
        }
    }

    void rebalance_child_after_erase(bptree_node_middle* parent, size_t child_index)
    {
        if (parent->_pointers[child_index]->_is_terminate) {
            rebalance_leaf_child(parent, child_index);
        } else {
            rebalance_middle_child(parent, child_index);
        }
    }

    bool erase_from_subtree(bptree_node_base* node, const tkey& key)
    {
        if (node->_is_terminate) {
            auto* leaf = static_cast<bptree_node_term*>(node);
            const size_t index = lower_index(leaf->_data, key);
            if (index >= leaf->_data.size() || !equal_keys(leaf->_data[index].first, key)) {
                return false;
            }

            leaf->_data.erase(leaf->_data.begin() + static_cast<ptrdiff_t>(index));
            return true;
        }

        auto* middle = static_cast<bptree_node_middle*>(node);
        const size_t child_index = upper_key_index(middle->_keys, key);
        if (!erase_from_subtree(middle->_pointers[child_index], key)) {
            return false;
        }

        if (is_underflow(middle->_pointers[child_index])) {
            rebalance_child_after_erase(middle, child_index);
        } else {
            refresh_middle_keys(middle);
        }

        return true;
    }

    friend void swap(BP_tree& lhs, BP_tree& rhs) noexcept
    {
        using std::swap;
        swap(static_cast<compare&>(lhs), static_cast<compare&>(rhs));
        swap(lhs._allocator, rhs._allocator);
        swap(lhs._root, rhs._root);
        swap(lhs._size, rhs._size);
    }

public:
    explicit BP_tree(const compare& cmp = compare(), pp_allocator<value_type> alloc = pp_allocator<value_type>())
        : compare(cmp), _allocator(std::move(alloc)), _root(nullptr), _size(0)
    {
    }

    explicit BP_tree(pp_allocator<value_type> alloc, const compare& comp = compare()) : BP_tree(comp, std::move(alloc)) {}

    template<input_iterator_for_pair<tkey, tvalue> iterator>
    explicit BP_tree(iterator begin, iterator end, const compare& cmp = compare(),
                     pp_allocator<value_type> alloc = pp_allocator<value_type>())
        : BP_tree(cmp, std::move(alloc))
    {
        try {
            for (; begin != end; ++begin) {
                insert(*begin);
            }
        } catch (...) {
            clear();
            throw;
        }
    }

    BP_tree(std::initializer_list<std::pair<tkey, tvalue>> data, const compare& cmp = compare(),
            pp_allocator<value_type> alloc = pp_allocator<value_type>())
        : BP_tree(data.begin(), data.end(), cmp, std::move(alloc))
    {
    }

    BP_tree(const BP_tree& other)
        : compare(static_cast<const compare&>(other)), _allocator(other._allocator), _root(nullptr), _size(0)
    {
        bptree_node_term* previous_leaf = nullptr;
        _root = clone_subtree(other._root, previous_leaf);
        _size = other._size;
    }

    BP_tree(BP_tree&& other) noexcept
        : compare(static_cast<const compare&>(other)),
          _allocator(std::move(other._allocator)),
          _root(other._root),
          _size(other._size)
    {
        other._root = nullptr;
        other._size = 0;
    }

    BP_tree& operator=(const BP_tree& other)
    {
        if (this != &other) {
            BP_tree tmp(other);
            swap(*this, tmp);
        }
        return *this;
    }

    BP_tree& operator=(BP_tree&& other) noexcept
    {
        if (this != &other) {
            clear();
            static_cast<compare&>(*this) = std::move(static_cast<compare&>(other));
            _allocator = std::move(other._allocator);
            _root = other._root;
            _size = other._size;
            other._root = nullptr;
            other._size = 0;
        }
        return *this;
    }

    ~BP_tree() noexcept
    {
        clear();
    }

    class bptree_iterator final
    {
        bptree_node_term* _node;
        size_t _index;

    public:
        using value_type = tree_data_type_const;
        using reference = value_type&;
        using pointer = value_type*;
        using iterator_category = std::forward_iterator_tag;
        using difference_type = ptrdiff_t;
        using self = bptree_iterator;

        friend class BP_tree;
        friend class bptree_const_iterator;

        reference operator*() const noexcept
        {
            return BP_tree::as_value(_node->_data[_index]);
        }

        pointer operator->() const noexcept
        {
            return std::addressof(operator*());
        }

        self& operator++()
        {
            if (!_node) return *this;
            ++_index;
            if (_index >= _node->_data.size()) {
                _node = _node->_next;
                _index = 0;
            }
            return *this;
        }

        self operator++(int)
        {
            self tmp = *this;
            ++(*this);
            return tmp;
        }

        bool operator==(const self& other) const noexcept
        {
            return _node == other._node && _index == other._index;
        }

        bool operator!=(const self& other) const noexcept
        {
            return !(*this == other);
        }

        size_t current_node_keys_count() const noexcept
        {
            return _node ? _node->_data.size() : 0;
        }

        size_t index() const noexcept
        {
            return _index;
        }

        explicit bptree_iterator(bptree_node_term* node = nullptr, size_t index = 0) : _node(node), _index(index) {}
    };

    class bptree_const_iterator final
    {
        const bptree_node_term* _node;
        size_t _index;

    public:
        using value_type = tree_data_type_const;
        using reference = const value_type&;
        using pointer = const value_type*;
        using iterator_category = std::forward_iterator_tag;
        using difference_type = ptrdiff_t;
        using self = bptree_const_iterator;

        friend class BP_tree;
        friend class bptree_iterator;

        bptree_const_iterator(const bptree_iterator& it) noexcept : _node(it._node), _index(it._index) {}

        reference operator*() const noexcept
        {
            return BP_tree::as_value(_node->_data[_index]);
        }

        pointer operator->() const noexcept
        {
            return std::addressof(operator*());
        }

        self& operator++()
        {
            if (!_node) return *this;
            ++_index;
            if (_index >= _node->_data.size()) {
                _node = _node->_next;
                _index = 0;
            }
            return *this;
        }

        self operator++(int)
        {
            self tmp = *this;
            ++(*this);
            return tmp;
        }

        bool operator==(const self& other) const noexcept
        {
            return _node == other._node && _index == other._index;
        }

        bool operator!=(const self& other) const noexcept
        {
            return !(*this == other);
        }

        size_t current_node_keys_count() const noexcept
        {
            return _node ? _node->_data.size() : 0;
        }

        size_t index() const noexcept
        {
            return _index;
        }

        explicit bptree_const_iterator(const bptree_node_term* node = nullptr, size_t index = 0)
            : _node(node), _index(index)
        {
        }
    };

    tvalue& at(const tkey& key)
    {
        auto it = find(key);
        if (it == end()) throw key_not_found("at: key not found");
        return it->second;
    }

    const tvalue& at(const tkey& key) const
    {
        auto it = find(key);
        if (it == cend()) throw key_not_found("at: key not found");
        return it->second;
    }

    tvalue& operator[](const tkey& key)
    {
        return emplace(key, tvalue{}).first->second;
    }

    tvalue& operator[](tkey&& key)
    {
        return emplace(std::move(key), tvalue{}).first->second;
    }

    bptree_iterator begin()
    {
        return bptree_iterator(first_leaf(), 0);
    }

    bptree_iterator end()
    {
        return bptree_iterator();
    }

    bptree_const_iterator begin() const
    {
        return cbegin();
    }

    bptree_const_iterator end() const
    {
        return cend();
    }

    bptree_const_iterator cbegin() const
    {
        return bptree_const_iterator(first_leaf(), 0);
    }

    bptree_const_iterator cend() const
    {
        return bptree_const_iterator();
    }

    size_t size() const noexcept
    {
        return _size;
    }

    bool empty() const noexcept
    {
        return _size == 0;
    }

    bptree_iterator find(const tkey& key)
    {
        auto* leaf = find_leaf(key);
        if (!leaf) return end();
        const size_t index = lower_index(leaf->_data, key);
        if (index < leaf->_data.size() && equal_keys(leaf->_data[index].first, key)) {
            return bptree_iterator(leaf, index);
        }
        return end();
    }

    bptree_const_iterator find(const tkey& key) const
    {
        const auto* leaf = find_leaf(key);
        if (!leaf) return cend();
        const size_t index = lower_index(leaf->_data, key);
        if (index < leaf->_data.size() && equal_keys(leaf->_data[index].first, key)) {
            return bptree_const_iterator(leaf, index);
        }
        return cend();
    }

    bptree_iterator lower_bound(const tkey& key)
    {
        auto* leaf = find_leaf(key);
        if (!leaf) return end();
        size_t index = lower_index(leaf->_data, key);
        while (leaf && index >= leaf->_data.size()) {
            leaf = leaf->_next;
            index = 0;
        }
        return leaf ? bptree_iterator(leaf, index) : end();
    }

    bptree_const_iterator lower_bound(const tkey& key) const
    {
        const auto* leaf = find_leaf(key);
        if (!leaf) return cend();
        size_t index = lower_index(leaf->_data, key);
        while (leaf && index >= leaf->_data.size()) {
            leaf = leaf->_next;
            index = 0;
        }
        return leaf ? bptree_const_iterator(leaf, index) : cend();
    }

    bptree_iterator upper_bound(const tkey& key)
    {
        auto it = lower_bound(key);
        while (it != end() && equal_keys(it->first, key)) {
            ++it;
        }
        return it;
    }

    bptree_const_iterator upper_bound(const tkey& key) const
    {
        auto it = lower_bound(key);
        while (it != cend() && equal_keys(it->first, key)) {
            ++it;
        }
        return it;
    }

    bool contains(const tkey& key) const
    {
        return find(key) != cend();
    }

    void clear() noexcept
    {
        destroy_subtree(_root);
        _root = nullptr;
        _size = 0;
    }

    std::pair<bptree_iterator, bool> insert(const tree_data_type& data)
    {
        tree_data_type copy(data);
        return insert(std::move(copy));
    }

    std::pair<bptree_iterator, bool> insert(tree_data_type&& data)
    {
        const tkey inserted_key = data.first;

        if (!_root) {
            auto* leaf = allocate_term_node();
            try {
                leaf->_data.push_back(std::move(data));
                _root = leaf;
                ++_size;
                return {bptree_iterator(leaf, 0), true};
            } catch (...) {
                deallocate_node(leaf);
                throw;
            }
        }

        bool inserted = false;
        split_result root_split = insert_into_subtree(_root, std::move(data), inserted);
        if (!inserted) {
            return {find(inserted_key), false};
        }

        ++_size;
        if (root_split) {
            create_new_root(_root, std::move(root_split));
        }
        return {find(inserted_key), true};
    }

    template <typename... Args>
    std::pair<bptree_iterator, bool> emplace(Args&&... args)
    {
        return insert(tree_data_type(std::forward<Args>(args)...));
    }

    bptree_iterator insert_or_assign(const tree_data_type& data)
    {
        auto it = find(data.first);
        if (it != end()) {
            it->second = data.second;
            return it;
        }
        return insert(data).first;
    }

    bptree_iterator insert_or_assign(tree_data_type&& data)
    {
        auto it = find(data.first);
        if (it != end()) {
            it->second = std::move(data.second);
            return it;
        }
        return insert(std::move(data)).first;
    }

    template <typename... Args>
    bptree_iterator emplace_or_assign(Args&&... args)
    {
        tree_data_type data(std::forward<Args>(args)...);
        return insert_or_assign(std::move(data));
    }

    bptree_iterator erase(bptree_iterator pos)
    {
        if (pos == end()) return end();
        const tkey key = pos->first;
        return erase(key);
    }

    bptree_iterator erase(bptree_const_iterator pos)
    {
        if (pos == cend()) return end();
        const tkey key = pos->first;
        return erase(key);
    }

    bptree_iterator erase(bptree_iterator beg, bptree_iterator en)
    {
        if (beg == en) return en;
        std::optional<tkey> end_key;
        if (en != end()) end_key = en->first;
        while (beg != end()) {
            if (end_key && equal_keys(beg->first, *end_key)) break;
            beg = erase(beg);
        }
        return beg;
    }

    bptree_iterator erase(bptree_const_iterator beg, bptree_const_iterator en)
    {
        if (beg == en) return (en == cend()) ? end() : find(en->first);
        std::optional<tkey> end_key;
        if (en != cend()) end_key = en->first;
        auto current = (beg == cend()) ? end() : find(beg->first);
        while (current != end()) {
            if (end_key && equal_keys(current->first, *end_key)) break;
            current = erase(current);
        }
        return current;
    }

    bptree_iterator erase(const tkey& key)
    {
        if (!_root) return end();
        if (!erase_from_subtree(_root, key)) return end();
        --_size;

        if (_size == 0) {
            clear();
            return end();
        }

        collapse_empty_root();
        return lower_bound(key);
    }
};

template<std::input_iterator iterator,
         comparator<typename std::iterator_traits<iterator>::value_type::first_type> compare =
                 std::less<typename std::iterator_traits<iterator>::value_type::first_type>,
         std::size_t t = 5, typename U>
BP_tree(iterator begin, iterator end, const compare& cmp = compare(), pp_allocator<U> = pp_allocator<U>())
        -> BP_tree<typename std::iterator_traits<iterator>::value_type::first_type,
                   typename std::iterator_traits<iterator>::value_type::second_type, compare, t>;

template<typename tkey, typename tvalue, comparator<tkey> compare = std::less<tkey>, std::size_t t = 5, typename U>
BP_tree(std::initializer_list<std::pair<tkey, tvalue>> data, const compare& cmp = compare(),
        pp_allocator<U> = pp_allocator<U>()) -> BP_tree<tkey, tvalue, compare, t>;

#endif
