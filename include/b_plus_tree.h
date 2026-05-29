#ifndef SYS_PROG_B_PLUS_TREE_H
#define SYS_PROG_B_PLUS_TREE_H

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <list>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <associative_container.h>

template <typename tkey, typename tvalue, comparator<tkey> compare = std::less<tkey>, std::size_t t = 5>
class BP_tree final : private compare
{
    static_assert(t >= 2, "BP_tree minimal degree must be at least 2");

public:
    using tree_data_type = std::pair<tkey, tvalue>;
    using tree_data_type_const = std::pair<tkey, tvalue>;
    using value_type = tree_data_type_const;

private:
    static constexpr const size_t minimum_keys_in_node = t - 1;
    static constexpr const size_t maximum_keys_in_node = 2 * t - 1;
    static constexpr const std::uint64_t null_node = 0;

    struct node
    {
        std::uint64_t id{null_node};
        bool leaf{true};
        std::uint64_t next{null_node};
        std::vector<tree_data_type> data;
        std::vector<tkey> keys;
        std::vector<std::uint64_t> children;
    };

    using split_result = std::optional<std::pair<tkey, std::uint64_t>>;

    struct cache_record
    {
        node page;
        typename std::list<std::uint64_t>::iterator lru_position;
    };

    static constexpr const size_t cache_capacity = 64;

    std::filesystem::path _storage_dir;
    std::uint64_t _root{null_node};
    std::uint64_t _next_id{1};
    size_t _size{0};
    mutable std::list<std::uint64_t> _cache_lru;
    mutable std::unordered_map<std::uint64_t, cache_record> _cache;

public:
    class key_not_found : public std::out_of_range
    {
    public:
        explicit key_not_found(const std::string& message) : std::out_of_range("BP_tree: " + message) {}
    };

    class storage_error : public std::runtime_error
    {
    public:
        explicit storage_error(const std::string& message) : std::runtime_error("BP_tree storage: " + message) {}
    };

private:
    static std::filesystem::path default_storage_dir()
    {
        const auto seed = std::to_string(reinterpret_cast<std::uintptr_t>(&minimum_keys_in_node));
        return std::filesystem::temp_directory_path() / ("course_dbms_bptree_" + seed);
    }

    std::filesystem::path meta_path() const
    {
        return _storage_dir / "meta.bin";
    }

    std::filesystem::path node_path(std::uint64_t id) const
    {
        return _storage_dir / ("node_" + std::to_string(id) + ".bin");
    }

    std::filesystem::path wal_path() const
    {
        return _storage_dir / "index.wal";
    }

    static bool is_node_file(const std::filesystem::path& path)
    {
        const std::string name = path.filename().string();
        return name.rfind("node_", 0) == 0 && path.extension() == ".bin";
    }

    static void atomic_replace(const std::filesystem::path& path, const std::string& data)
    {
        std::filesystem::create_directories(path.parent_path());
        const auto tmp = path.string() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) throw storage_error("cannot write temporary file " + tmp);
            out.write(data.data(), static_cast<std::streamsize>(data.size()));
            out.flush();
            if (!out) throw storage_error("cannot flush temporary file " + tmp);
        }
        std::filesystem::rename(tmp, path);
    }

    template <typename T>
    static void write_plain(std::ostream& out, const T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>, "Binary BP_tree field must be trivially copyable");
        out.write(reinterpret_cast<const char*>(&value), sizeof(T));
    }

    template <typename T>
    static T read_plain(std::istream& in)
    {
        static_assert(std::is_trivially_copyable_v<T>, "Binary BP_tree field must be trivially copyable");
        T value{};
        in.read(reinterpret_cast<char*>(&value), sizeof(T));
        if (!in) throw storage_error("corrupted binary file");
        return value;
    }

    static void write_key(std::ostream& out, const int& value)
    {
        write_plain(out, value);
    }

    static int read_int_key(std::istream& in)
    {
        return read_plain<int>(in);
    }

    static void write_key(std::ostream& out, const std::string& value)
    {
        const std::uint64_t size = static_cast<std::uint64_t>(value.size());
        write_plain(out, size);
        out.write(value.data(), static_cast<std::streamsize>(value.size()));
    }

    static std::string read_string_key(std::istream& in)
    {
        const auto size = read_plain<std::uint64_t>(in);
        std::string value(size, '\0');
        if (size != 0) in.read(value.data(), static_cast<std::streamsize>(size));
        if (!in) throw storage_error("corrupted string key");
        return value;
    }

    static tkey read_key(std::istream& in)
    {
        if constexpr (std::is_same_v<tkey, int>) {
            return read_int_key(in);
        } else if constexpr (std::is_same_v<tkey, std::string>) {
            return read_string_key(in);
        } else {
            static_assert(std::is_same_v<tkey, int> || std::is_same_v<tkey, std::string>,
                          "Disk BP_tree supports int and string keys");
        }
    }

    static void write_value(std::ostream& out, const tvalue& value)
    {
        if constexpr (std::is_arithmetic_v<tvalue> || std::is_enum_v<tvalue>) {
            write_plain(out, value);
        } else {
            static_assert(std::is_arithmetic_v<tvalue> || std::is_enum_v<tvalue>,
                          "Disk BP_tree value must be a compact row reference");
        }
    }

    static tvalue read_value(std::istream& in)
    {
        if constexpr (std::is_arithmetic_v<tvalue> || std::is_enum_v<tvalue>) {
            return read_plain<tvalue>(in);
        } else {
            static_assert(std::is_arithmetic_v<tvalue> || std::is_enum_v<tvalue>,
                          "Disk BP_tree value must be a compact row reference");
        }
    }

    static std::string escape_wal_text(const std::string& value)
    {
        std::string out;
        for (const char ch : value) {
            if (ch == '\\' || ch == '\t' || ch == '\n') out.push_back('\\');
            if (ch == '\t') out.push_back('t');
            else if (ch == '\n') out.push_back('n');
            else out.push_back(ch);
        }
        return out;
    }

    static std::string key_to_wal(const int& value)
    {
        return "I:" + std::to_string(value);
    }

    static std::string key_to_wal(const std::string& value)
    {
        return "S:" + escape_wal_text(value);
    }

    static std::string value_to_wal(const tvalue& value)
    {
        if constexpr (std::is_arithmetic_v<tvalue> || std::is_enum_v<tvalue>) {
            return std::to_string(value);
        } else {
            static_assert(std::is_arithmetic_v<tvalue> || std::is_enum_v<tvalue>,
                          "Disk BP_tree value must be a compact row reference");
        }
    }

    void append_wal(const std::string& phase, const std::string& operation,
                    const tkey& key, std::optional<tvalue> value = std::nullopt) const
    {
        std::filesystem::create_directories(_storage_dir);
        std::ofstream out(wal_path(), std::ios::app);
        if (!out) throw storage_error("cannot write " + wal_path().string());
        out << phase << '\t' << operation << '\t' << key_to_wal(key);
        if (value) out << '\t' << value_to_wal(*value);
        out << '\n';
        out.flush();
        if (!out) throw storage_error("cannot flush " + wal_path().string());
    }

    bool wal_has_unfinished_record() const
    {
        std::ifstream in(wal_path());
        if (!in) return false;

        int opened = 0;
        std::string line;
        while (std::getline(in, line)) {
            std::stringstream ss(line);
            std::string phase;
            if (!std::getline(ss, phase, '\t')) continue;
            if (phase == "BEGIN") ++opened;
            else if (phase == "COMMIT" && opened > 0) --opened;
        }
        return opened != 0;
    }

    void touch_cache(std::uint64_t id) const
    {
        const auto it = _cache.find(id);
        if (it == _cache.end()) return;
        _cache_lru.erase(it->second.lru_position);
        _cache_lru.push_front(id);
        it->second.lru_position = _cache_lru.begin();
    }

    void put_cache(const node& current) const
    {
        if (current.id == null_node) return;
        const auto existing = _cache.find(current.id);
        if (existing != _cache.end()) {
            existing->second.page = current;
            touch_cache(current.id);
            return;
        }

        _cache_lru.push_front(current.id);
        _cache.emplace(current.id, cache_record{current, _cache_lru.begin()});
        while (_cache.size() > cache_capacity) {
            const auto evicted = _cache_lru.back();
            _cache_lru.pop_back();
            _cache.erase(evicted);
        }
    }

    void remove_from_cache(std::uint64_t id) const
    {
        const auto it = _cache.find(id);
        if (it == _cache.end()) return;
        _cache_lru.erase(it->second.lru_position);
        _cache.erase(it);
    }

    void clear_cache() const
    {
        _cache_lru.clear();
        _cache.clear();
    }

    void ensure_storage()
    {
        if (_storage_dir.empty()) _storage_dir = default_storage_dir();
        std::filesystem::create_directories(_storage_dir);
        if (std::filesystem::exists(meta_path())) {
            load_meta();
        } else {
            save_meta();
        }
    }

    void load_meta()
    {
        std::ifstream in(meta_path(), std::ios::binary);
        if (!in) throw storage_error("cannot read " + meta_path().string());
        char magic[8]{};
        in.read(magic, sizeof(magic));
        if (std::string(magic, sizeof(magic)) != std::string("BPTREE01", 8)) {
            throw storage_error("bad meta header in " + meta_path().string());
        }
        _root = read_plain<std::uint64_t>(in);
        _next_id = read_plain<std::uint64_t>(in);
        _size = static_cast<size_t>(read_plain<std::uint64_t>(in));
    }

    void save_meta() const
    {
        std::ostringstream out;
        out.write("BPTREE01", 8);
        write_plain(out, _root);
        write_plain(out, _next_id);
        write_plain(out, static_cast<std::uint64_t>(_size));
        atomic_replace(meta_path(), out.str());
    }

    node read_node(std::uint64_t id) const
    {
        if (id == null_node) throw storage_error("attempt to read null node");
        const auto cached = _cache.find(id);
        if (cached != _cache.end()) {
            touch_cache(id);
            return cached->second.page;
        }

        std::ifstream in(node_path(id), std::ios::binary);
        if (!in) throw storage_error("cannot read " + node_path(id).string());

        node current;
        current.id = id;
        const char kind = read_plain<char>(in);
        current.leaf = kind == 'L';
        if (kind != 'L' && kind != 'I') throw storage_error("bad node kind");
        current.next = read_plain<std::uint64_t>(in);

        const auto key_count = read_plain<std::uint64_t>(in);
        if (key_count > maximum_keys_in_node + 1) throw storage_error("too many keys in node");

        if (current.leaf) {
            current.data.reserve(static_cast<size_t>(key_count));
            for (std::uint64_t i = 0; i < key_count; ++i) {
                tkey key = read_key(in);
                tvalue value = read_value(in);
                current.data.emplace_back(std::move(key), value);
            }
        } else {
            current.keys.reserve(static_cast<size_t>(key_count));
            for (std::uint64_t i = 0; i < key_count; ++i) current.keys.push_back(read_key(in));
            const auto child_count = read_plain<std::uint64_t>(in);
            if (child_count != key_count + 1) throw storage_error("bad child count in internal node");
            current.children.reserve(static_cast<size_t>(child_count));
            for (std::uint64_t i = 0; i < child_count; ++i) {
                current.children.push_back(read_plain<std::uint64_t>(in));
            }
        }

        put_cache(current);
        return current;
    }

    void write_node(const node& current) const
    {
        std::ostringstream out;
        write_plain(out, current.leaf ? 'L' : 'I');
        write_plain(out, current.next);
        const auto key_count = static_cast<std::uint64_t>(current.leaf ? current.data.size() : current.keys.size());
        write_plain(out, key_count);

        if (current.leaf) {
            for (const auto& item : current.data) {
                write_key(out, item.first);
                write_value(out, item.second);
            }
        } else {
            for (const auto& key : current.keys) write_key(out, key);
            write_plain(out, static_cast<std::uint64_t>(current.children.size()));
            for (const auto child : current.children) write_plain(out, child);
        }
        atomic_replace(node_path(current.id), out.str());
        put_cache(current);
    }

    std::uint64_t allocate_node(bool leaf)
    {
        node created;
        created.id = _next_id++;
        created.leaf = leaf;
        write_node(created);
        save_meta();
        return created.id;
    }

    inline bool compare_keys(const tkey& lhs, const tkey& rhs) const
    {
        return compare::operator()(lhs, rhs);
    }

    inline bool equal_keys(const tkey& lhs, const tkey& rhs) const
    {
        return !compare_keys(lhs, rhs) && !compare_keys(rhs, lhs);
    }

    size_t lower_data_index(const std::vector<tree_data_type>& data, const tkey& key) const
    {
        size_t left = 0;
        size_t right = data.size();
        while (left < right) {
            const size_t middle = left + (right - left) / 2;
            if (compare_keys(data[middle].first, key)) left = middle + 1;
            else right = middle;
        }
        return left;
    }

    size_t upper_key_index(const std::vector<tkey>& keys, const tkey& key) const
    {
        size_t left = 0;
        size_t right = keys.size();
        while (left < right) {
            const size_t middle = left + (right - left) / 2;
            if (!compare_keys(key, keys[middle])) left = middle + 1;
            else right = middle;
        }
        return left;
    }

    std::uint64_t first_leaf_id() const
    {
        auto current_id = _root;
        while (current_id != null_node) {
            const node current = read_node(current_id);
            if (current.leaf) return current.id;
            current_id = current.children.front();
        }
        return null_node;
    }

    std::uint64_t find_leaf_id(const tkey& key) const
    {
        auto current_id = _root;
        while (current_id != null_node) {
            const node current = read_node(current_id);
            if (current.leaf) return current.id;
            current_id = current.children[upper_key_index(current.keys, key)];
        }
        return null_node;
    }

    split_result split_leaf_node(node& leaf)
    {
        node right;
        right.id = allocate_node(true);
        right.leaf = true;
        right.data.insert(right.data.end(),
                          std::make_move_iterator(leaf.data.begin() + static_cast<std::ptrdiff_t>(t)),
                          std::make_move_iterator(leaf.data.end()));
        leaf.data.resize(t);
        right.next = leaf.next;
        leaf.next = right.id;

        write_node(leaf);
        write_node(right);
        return std::make_pair(right.data.front().first, right.id);
    }

    split_result split_internal_node(node& current)
    {
        node right;
        right.id = allocate_node(false);
        right.leaf = false;

        constexpr size_t promoted_index = t;
        tkey promoted_key = std::move(current.keys[promoted_index]);

        right.keys.insert(right.keys.end(),
                          std::make_move_iterator(current.keys.begin() + static_cast<std::ptrdiff_t>(promoted_index + 1)),
                          std::make_move_iterator(current.keys.end()));
        right.children.insert(right.children.end(),
                              current.children.begin() + static_cast<std::ptrdiff_t>(promoted_index + 1),
                              current.children.end());

        current.keys.resize(promoted_index);
        current.children.resize(promoted_index + 1);

        write_node(current);
        write_node(right);
        return std::make_pair(std::move(promoted_key), right.id);
    }

    split_result insert_into_subtree(std::uint64_t node_id, tree_data_type data, bool& inserted)
    {
        node current = read_node(node_id);
        if (current.leaf) {
            const size_t index = lower_data_index(current.data, data.first);
            if (index < current.data.size() && equal_keys(current.data[index].first, data.first)) {
                inserted = false;
                return std::nullopt;
            }

            current.data.insert(current.data.begin() + static_cast<std::ptrdiff_t>(index), std::move(data));
            inserted = true;
            if (current.data.size() > maximum_keys_in_node) return split_leaf_node(current);
            write_node(current);
            return std::nullopt;
        }

        const size_t child_index = upper_key_index(current.keys, data.first);
        split_result child_split = insert_into_subtree(current.children[child_index], std::move(data), inserted);
        if (!inserted) return std::nullopt;

        if (child_split) {
            current.keys.insert(current.keys.begin() + static_cast<std::ptrdiff_t>(child_index),
                                std::move(child_split->first));
            current.children.insert(current.children.begin() + static_cast<std::ptrdiff_t>(child_index + 1),
                                    child_split->second);
        }

        if (current.keys.size() > maximum_keys_in_node) return split_internal_node(current);
        write_node(current);
        return std::nullopt;
    }

    void create_new_root(std::uint64_t left_id, split_result split)
    {
        node root;
        root.id = allocate_node(false);
        root.leaf = false;
        root.keys.push_back(std::move(split->first));
        root.children.push_back(left_id);
        root.children.push_back(split->second);
        write_node(root);
        _root = root.id;
        save_meta();
    }

    struct path_step
    {
        std::uint64_t parent_id{null_node};
        size_t child_index{0};
    };

    std::uint64_t find_leaf_path(const tkey& key, std::vector<path_step>& path) const
    {
        std::uint64_t current_id = _root;
        while (current_id != null_node) {
            const node current = read_node(current_id);
            if (current.leaf) return current.id;
            const size_t child_index = upper_key_index(current.keys, key);
            path.push_back({current.id, child_index});
            current_id = current.children[child_index];
        }
        return null_node;
    }

    size_t key_count(const node& current) const
    {
        return current.leaf ? current.data.size() : current.keys.size();
    }

    void remove_node_file(std::uint64_t id) const
    {
        remove_from_cache(id);
        std::error_code ec;
        std::filesystem::remove(node_path(id), ec);
        if (ec) throw storage_error("cannot remove " + node_path(id).string() + ": " + ec.message());
    }

    tkey subtree_first_key(std::uint64_t node_id) const
    {
        node current = read_node(node_id);
        while (!current.leaf) {
            if (current.children.empty()) throw storage_error("internal node has no children");
            current = read_node(current.children.front());
        }
        if (current.data.empty()) throw storage_error("empty leaf remains in non-empty tree");
        return current.data.front().first;
    }

    void refresh_separator_keys(std::uint64_t node_id)
    {
        if (node_id == null_node) return;
        node current = read_node(node_id);
        if (current.leaf) return;
        for (const auto child_id : current.children) refresh_separator_keys(child_id);
        if (current.children.size() != current.keys.size() + 1) {
            throw storage_error("invalid internal node after rebalancing");
        }
        for (size_t i = 0; i < current.keys.size(); ++i) {
            current.keys[i] = subtree_first_key(current.children[i + 1]);
        }
        write_node(current);
    }

    void collapse_root_if_needed()
    {
        if (_root == null_node) return;
        node root = read_node(_root);
        if (root.leaf) {
            if (root.data.empty()) {
                const auto old_root = _root;
                _root = null_node;
                remove_node_file(old_root);
            }
            return;
        }
        if (root.keys.empty()) {
            if (root.children.size() != 1) throw storage_error("empty internal root has invalid children");
            const auto old_root = _root;
            _root = root.children.front();
            remove_node_file(old_root);
        }
    }

    bool erase_balanced(const tkey& key)
    {
        std::vector<path_step> path;
        const auto leaf_id = find_leaf_path(key, path);
        if (leaf_id == null_node) return false;

        node leaf = read_node(leaf_id);
        const size_t index = lower_data_index(leaf.data, key);
        if (index >= leaf.data.size() || !equal_keys(leaf.data[index].first, key)) return false;
        leaf.data.erase(leaf.data.begin() + static_cast<std::ptrdiff_t>(index));
        write_node(leaf);

        std::uint64_t current_id = leaf.id;
        while (!path.empty()) {
            node current = read_node(current_id);
            if (key_count(current) >= minimum_keys_in_node) break;

            const path_step step = path.back();
            path.pop_back();
            node parent = read_node(step.parent_id);
            const size_t child_index = step.child_index;
            if (child_index >= parent.children.size() || parent.children[child_index] != current_id) {
                throw storage_error("invalid deletion path");
            }

            bool fixed_by_borrow = false;
            if (child_index > 0) {
                node left = read_node(parent.children[child_index - 1]);
                if (key_count(left) > minimum_keys_in_node) {
                    if (current.leaf) {
                        current.data.insert(current.data.begin(), std::move(left.data.back()));
                        left.data.pop_back();
                    } else {
                        current.keys.insert(current.keys.begin(), parent.keys[child_index - 1]);
                        current.children.insert(current.children.begin(), left.children.back());
                        parent.keys[child_index - 1] = left.keys.back();
                        left.keys.pop_back();
                        left.children.pop_back();
                    }
                    write_node(left);
                    write_node(current);
                    write_node(parent);
                    fixed_by_borrow = true;
                }
            }
            if (!fixed_by_borrow && child_index + 1 < parent.children.size()) {
                node right = read_node(parent.children[child_index + 1]);
                if (key_count(right) > minimum_keys_in_node) {
                    if (current.leaf) {
                        current.data.push_back(std::move(right.data.front()));
                        right.data.erase(right.data.begin());
                    } else {
                        current.keys.push_back(parent.keys[child_index]);
                        current.children.push_back(right.children.front());
                        parent.keys[child_index] = right.keys.front();
                        right.keys.erase(right.keys.begin());
                        right.children.erase(right.children.begin());
                    }
                    write_node(current);
                    write_node(right);
                    write_node(parent);
                    fixed_by_borrow = true;
                }
            }
            if (fixed_by_borrow) break;

            if (child_index > 0) {
                node left = read_node(parent.children[child_index - 1]);
                if (current.leaf) {
                    left.data.insert(left.data.end(),
                                     std::make_move_iterator(current.data.begin()),
                                     std::make_move_iterator(current.data.end()));
                    left.next = current.next;
                } else {
                    left.keys.push_back(parent.keys[child_index - 1]);
                    left.keys.insert(left.keys.end(),
                                     std::make_move_iterator(current.keys.begin()),
                                     std::make_move_iterator(current.keys.end()));
                    left.children.insert(left.children.end(), current.children.begin(), current.children.end());
                }
                parent.keys.erase(parent.keys.begin() + static_cast<std::ptrdiff_t>(child_index - 1));
                parent.children.erase(parent.children.begin() + static_cast<std::ptrdiff_t>(child_index));
                write_node(left);
                remove_node_file(current.id);
            } else {
                if (parent.children.size() < 2) throw storage_error("cannot merge an only child");
                node right = read_node(parent.children[1]);
                if (current.leaf) {
                    current.data.insert(current.data.end(),
                                        std::make_move_iterator(right.data.begin()),
                                        std::make_move_iterator(right.data.end()));
                    current.next = right.next;
                } else {
                    current.keys.push_back(parent.keys.front());
                    current.keys.insert(current.keys.end(),
                                        std::make_move_iterator(right.keys.begin()),
                                        std::make_move_iterator(right.keys.end()));
                    current.children.insert(current.children.end(), right.children.begin(), right.children.end());
                }
                parent.keys.erase(parent.keys.begin());
                parent.children.erase(parent.children.begin() + 1);
                write_node(current);
                remove_node_file(right.id);
            }
            write_node(parent);
            current_id = parent.id;
        }

        collapse_root_if_needed();
        if (_root != null_node) refresh_separator_keys(_root);
        return true;
    }

    struct validation_state
    {
        size_t entries{0};
        std::optional<tkey> first;
        std::optional<tkey> last;
    };

    validation_state validate_subtree(std::uint64_t node_id, bool root_node,
                                      std::vector<std::uint64_t>& leaves) const
    {
        const node current = read_node(node_id);
        const size_t count = key_count(current);
        if (count > maximum_keys_in_node) throw storage_error("node overflow");
        if (!root_node && count < minimum_keys_in_node) throw storage_error("node underflow");

        if (current.leaf) {
            if (root_node && current.data.empty()) throw storage_error("empty root leaf must be removed");
            for (size_t i = 1; i < current.data.size(); ++i) {
                if (!compare_keys(current.data[i - 1].first, current.data[i].first)) {
                    throw storage_error("unsorted or duplicate leaf keys");
                }
            }
            leaves.push_back(current.id);
            validation_state state;
            state.entries = current.data.size();
            if (!current.data.empty()) {
                state.first = current.data.front().first;
                state.last = current.data.back().first;
            }
            return state;
        }

        if (current.children.size() != current.keys.size() + 1) {
            throw storage_error("invalid child count");
        }
        if (root_node && current.keys.empty()) throw storage_error("empty internal root must be collapsed");

        validation_state result;
        std::vector<validation_state> children;
        children.reserve(current.children.size());
        for (const auto child_id : current.children) {
            children.push_back(validate_subtree(child_id, false, leaves));
        }
        for (size_t i = 0; i < current.keys.size(); ++i) {
            if (!children[i + 1].first || !equal_keys(current.keys[i], *children[i + 1].first)) {
                throw storage_error("invalid separator key");
            }
            if (children[i].last && children[i + 1].first &&
                !compare_keys(*children[i].last, *children[i + 1].first)) {
                throw storage_error("overlapping child key ranges");
            }
        }
        for (const auto& child : children) result.entries += child.entries;
        result.first = children.front().first;
        result.last = children.back().last;
        return result;
    }

    bool validate_leaf_links(const std::vector<std::uint64_t>& leaves) const
    {
        for (size_t i = 0; i < leaves.size(); ++i) {
            const node leaf = read_node(leaves[i]);
            const std::uint64_t expected = i + 1 < leaves.size() ? leaves[i + 1] : null_node;
            if (!leaf.leaf || leaf.next != expected) return false;
        }
        return true;
    }

    bool insert_impl(tree_data_type&& data)
    {
        if (_root == null_node) {
            node leaf;
            leaf.id = allocate_node(true);
            leaf.leaf = true;
            leaf.data.push_back(std::move(data));
            write_node(leaf);
            _root = leaf.id;
            ++_size;
            save_meta();
            return true;
        }

        bool inserted = false;
        split_result root_split = insert_into_subtree(_root, std::move(data), inserted);
        if (!inserted) return false;

        ++_size;
        if (root_split) create_new_root(_root, std::move(root_split));
        save_meta();
        return true;
    }

    std::uint64_t skip_empty_leaves(std::uint64_t leaf_id) const
    {
        while (leaf_id != null_node) {
            const node leaf = read_node(leaf_id);
            if (!leaf.data.empty()) return leaf_id;
            leaf_id = leaf.next;
        }
        return null_node;
    }

public:
    explicit BP_tree(const compare& cmp = compare())
        : compare(cmp), _storage_dir(default_storage_dir())
    {
        ensure_storage();
    }

    explicit BP_tree(std::filesystem::path storage_dir, const compare& cmp = compare())
            : compare(cmp), _storage_dir(std::move(storage_dir))
    {
        ensure_storage();
    }

    BP_tree(const BP_tree&) = delete;
    BP_tree& operator=(const BP_tree&) = delete;

    BP_tree(BP_tree&& other) noexcept
        : compare(static_cast<const compare&>(other)),
          _storage_dir(std::move(other._storage_dir)),
          _root(other._root),
          _next_id(other._next_id),
          _size(other._size)
    {
        other._root = null_node;
        other._next_id = 1;
        other._size = 0;
    }

    BP_tree& operator=(BP_tree&& other) noexcept
    {
        if (this != &other) {
            static_cast<compare&>(*this) = std::move(static_cast<compare&>(other));
            _storage_dir = std::move(other._storage_dir);
            _root = other._root;
            _next_id = other._next_id;
            _size = other._size;
            other._root = null_node;
            other._next_id = 1;
            other._size = 0;
        }
        return *this;
    }

    void set_storage(std::filesystem::path storage_dir)
    {
        _storage_dir = std::move(storage_dir);
        _root = null_node;
        _next_id = 1;
        _size = 0;
        ensure_storage();
    }

    class bptree_const_iterator final
    {
        const BP_tree* _tree{nullptr};
        std::uint64_t _node{null_node};
        size_t _index{0};
        mutable std::optional<tree_data_type> _cache;

        void load_cache() const
        {
            if (!_tree || _node == null_node) return;
            const node leaf = _tree->read_node(_node);
            if (_index >= leaf.data.size()) throw storage_error("iterator points outside leaf");
            _cache = leaf.data[_index];
        }

    public:
        using value_type = tree_data_type_const;
        using reference = const value_type&;
        using pointer = const value_type*;
        using iterator_category = std::forward_iterator_tag;
        using difference_type = ptrdiff_t;
        using self = bptree_const_iterator;

        friend class BP_tree;

        reference operator*() const
        {
            load_cache();
            return *_cache;
        }

        pointer operator->() const
        {
            return std::addressof(operator*());
        }

        self& operator++()
        {
            if (!_tree || _node == null_node) return *this;
            node leaf = _tree->read_node(_node);
            ++_index;
            while (_index >= leaf.data.size()) {
                _node = leaf.next;
                _index = 0;
                if (_node == null_node) {
                    _cache.reset();
                    return *this;
                }
                leaf = _tree->read_node(_node);
            }
            _cache.reset();
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
            return _tree == other._tree && _node == other._node && _index == other._index;
        }

        bool operator!=(const self& other) const noexcept
        {
            return !(*this == other);
        }

        explicit bptree_const_iterator(const BP_tree* tree = nullptr, std::uint64_t node_id = null_node, size_t index = 0)
            : _tree(tree), _node(node_id), _index(index)
        {
        }
    };

    using bptree_iterator = bptree_const_iterator;

    bptree_iterator begin()
    {
        const auto leaf_id = skip_empty_leaves(first_leaf_id());
        return leaf_id == null_node ? end() : bptree_iterator(this, leaf_id, 0);
    }

    bptree_iterator end()
    {
        return bptree_iterator(this, null_node, 0);
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
        const auto leaf_id = skip_empty_leaves(first_leaf_id());
        return leaf_id == null_node ? cend() : bptree_const_iterator(this, leaf_id, 0);
    }

    bptree_const_iterator cend() const
    {
        return bptree_const_iterator(this, null_node, 0);
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
        const auto leaf_id = find_leaf_id(key);
        if (leaf_id == null_node) return end();
        const node leaf = read_node(leaf_id);
        const size_t index = lower_data_index(leaf.data, key);
        if (index < leaf.data.size() && equal_keys(leaf.data[index].first, key)) {
            return bptree_iterator(this, leaf_id, index);
        }
        return end();
    }

    bptree_const_iterator find(const tkey& key) const
    {
        const auto leaf_id = find_leaf_id(key);
        if (leaf_id == null_node) return cend();
        const node leaf = read_node(leaf_id);
        const size_t index = lower_data_index(leaf.data, key);
        if (index < leaf.data.size() && equal_keys(leaf.data[index].first, key)) {
            return bptree_const_iterator(this, leaf_id, index);
        }
        return cend();
    }

    bptree_iterator lower_bound(const tkey& key)
    {
        const auto leaf_id = find_leaf_id(key);
        if (leaf_id == null_node) return end();
        node leaf = read_node(leaf_id);
        size_t index = lower_data_index(leaf.data, key);
        std::uint64_t current_id = leaf_id;
        while (index >= leaf.data.size()) {
            current_id = leaf.next;
            index = 0;
            if (current_id == null_node) return end();
            leaf = read_node(current_id);
        }
        return bptree_iterator(this, current_id, index);
    }

    bptree_const_iterator lower_bound(const tkey& key) const
    {
        const auto leaf_id = find_leaf_id(key);
        if (leaf_id == null_node) return cend();
        node leaf = read_node(leaf_id);
        size_t index = lower_data_index(leaf.data, key);
        std::uint64_t current_id = leaf_id;
        while (index >= leaf.data.size()) {
            current_id = leaf.next;
            index = 0;
            if (current_id == null_node) return cend();
            leaf = read_node(current_id);
        }
        return bptree_const_iterator(this, current_id, index);
    }

    bptree_iterator upper_bound(const tkey& key)
    {
        auto it = lower_bound(key);
        while (it != end() && equal_keys(it->first, key)) ++it;
        return it;
    }

    bptree_const_iterator upper_bound(const tkey& key) const
    {
        auto it = lower_bound(key);
        while (it != cend() && equal_keys(it->first, key)) ++it;
        return it;
    }

    bool contains(const tkey& key) const
    {
        return find(key) != cend();
    }

    bool consistent(size_t expected_size) const noexcept
    {
        try {
            if (_size != expected_size || wal_has_unfinished_record()) return false;
            if (_root == null_node) return expected_size == 0;
            std::vector<std::uint64_t> leaves;
            const validation_state state = validate_subtree(_root, true, leaves);
            if (state.entries != expected_size || !validate_leaf_links(leaves)) return false;
            size_t counted = 0;
            for (auto it = cbegin(); it != cend(); ++it) ++counted;
            return counted == expected_size;
        } catch (...) {
            return false;
        }
    }

    void clear()
    {
        if (_storage_dir.empty()) _storage_dir = default_storage_dir();
        std::filesystem::remove_all(_storage_dir);
        std::filesystem::create_directories(_storage_dir);
        clear_cache();
        _root = null_node;
        _next_id = 1;
        _size = 0;
        save_meta();
    }

    std::pair<bptree_iterator, bool> insert(const tree_data_type& data)
    {
        tree_data_type copy(data);
        return insert(std::move(copy));
    }

    std::pair<bptree_iterator, bool> insert(tree_data_type&& data)
    {
        const tkey inserted_key = data.first;
        const tvalue inserted_value = data.second;
        if (contains(inserted_key)) return {find(inserted_key), false};
        append_wal("BEGIN", "INSERT", inserted_key, inserted_value);
        const bool inserted = insert_impl(std::move(data));
        append_wal("COMMIT", "INSERT", inserted_key, inserted_value);
        return {find(inserted_key), inserted};
    }

    template <typename... Args>
    std::pair<bptree_iterator, bool> emplace(Args&&... args)
    {
        return insert(tree_data_type(std::forward<Args>(args)...));
    }

    bptree_iterator erase(const tkey& key)
    {
        if (_root == null_node || !contains(key)) return end();
        append_wal("BEGIN", "DELETE", key);
        if (!erase_balanced(key)) return end();
        --_size;
        save_meta();
        append_wal("COMMIT", "DELETE", key);
        return lower_bound(key);
    }

    bptree_iterator erase(bptree_iterator pos)
    {
        if (pos == end()) return end();
        return erase(pos->first);
    }

    tvalue at(const tkey& key) const
    {
        auto it = find(key);
        if (it == cend()) throw key_not_found("at: key not found");
        return it->second;
    }
};


#endif
