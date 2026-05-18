#include <app.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <b_plus_tree.h>
#include <db_ast.h>
#include <nlohmann/json.hpp>
#include <sql_parser_api.h>

namespace fs = std::filesystem;

namespace {

struct DbError : std::runtime_error {
    explicit DbError(const std::string& message) : std::runtime_error(message) {}
};

std::string upper(std::string value) {
    for (size_t i = 0; i < value.size(); i++) {
        value[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(value[i])));
    }
    return value;
}

bool valid_identifier(const std::string& value) {
    if (value.empty()) return false;
    if (value[0] >= '0' && value[0] <= '9') return false;

    for (size_t i = 0; i < value.size(); i++) {
        char ch = value[i];
        bool is_letter = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        bool is_digit = (ch >= '0' && ch <= '9');
        bool is_underscore = (ch == '_');
        
        if (!is_letter && !is_digit && !is_underscore) {
            return false;
        }
    }
    return true;
}

std::string current_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count() % 1000000;
    std::tm tm{};
    localtime_r(&time, &tm);

    std::ostringstream out;
    out << std::put_time(&tm, "%Y.%m.%d-%H:%M:%S") << "." << std::setw(6) << std::setfill('0') << micros;
    return out.str();
}

std::string value_to_storage(const Value& value) {
    if (std::holds_alternative<std::monostate>(value)) return "N:";
    if (std::holds_alternative<int>(value)) return "I:" + std::to_string(std::get<int>(value));
    std::string out = "S:";
    for (char ch : *std::get<InternedString>(value)) {
        if (ch == '\\' || ch == '\t' || ch == '\n') out.push_back('\\');
        if (ch == '\t') out.push_back('t');
        else if (ch == '\n') out.push_back('n');
        else out.push_back(ch);
    }
    return out;
}

Value value_from_storage(const std::string& text) {
    if (text == "N:") return std::monostate{};
    if (text.find("I:") == 0) return std::stoi(text.substr(2));
    if (text.find("S:") == 0) {
        std::string out;
        const std::string raw = text.substr(2);
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\\' && i + 1 < raw.size()) {
                const char next = raw[++i];
                out.push_back(next == 't' ? '\t' : next == 'n' ? '\n' : next);
            } else {
                out.push_back(raw[i]);
            }
        }
        return intern_string(out);
    }
    throw DbError("Corrupted value in storage");
}

nlohmann::ordered_json value_to_json(const Value& value) {
    if (std::holds_alternative<std::monostate>(value)) return nullptr;
    if (std::holds_alternative<int>(value)) return std::get<int>(value);
    return *std::get<InternedString>(value);
}

Value token_to_value(const Token& token, ColumnType type) {
    if (token.kind == TokenKind::Identifier && upper(token.text) == "NULL") return std::monostate{};
    if (type == ColumnType::Int) {
        if (token.kind != TokenKind::Number) throw DbError("Expected int literal");
        return std::stoi(token.text);
    }
    if (token.kind != TokenKind::String) throw DbError("Expected string literal");
    return intern_string(token.text);
}

struct Row {
    size_t id{};
    bool alive{true};
    std::vector<Value> values;
};

struct Index {
    ColumnType type{};
    BP_tree<int, size_t> ints;
    BP_tree<std::string, size_t> strings;

    Index() = default;
    explicit Index(ColumnType column_type) : type(column_type) {}
};

class Table {
public:
    Table(fs::path dir, std::vector<Column> columns) : dir_(std::move(dir)), columns_(std::move(columns)) {
        for (size_t i = 0; i < columns_.size(); ++i) {
            column_pos_[columns_[i].name] = i;
            if (columns_[i].indexed) indexes_.emplace(columns_[i].name, Index(columns_[i].type));
        }
        load_rows();
        rebuild_indexes();
    }

    const std::vector<Column>& columns() const { return columns_; }

    void insert(const std::vector<std::string>& names, const std::vector<Token>& raw_values) {
        if (names.size() != raw_values.size()) throw DbError("Column count does not match value count");
        Row row;
        row.id = next_id_++;
        row.values.assign(columns_.size(), std::monostate{});
        for (size_t i = 0; i < columns_.size(); ++i) {
            if (columns_[i].has_default) row.values[i] = columns_[i].default_value;
        }

        for (size_t i = 0; i < names.size(); ++i) {
            const size_t pos = column_index(names[i]);
            row.values[pos] = token_to_value(raw_values[i], columns_[pos].type);
        }
        validate_row(row, std::nullopt);
        append_history("INSERT", row);
        rows_.push_back(row);
        rebuild_indexes();
        save_rows();
    }

    std::vector<Row*> matching(const Condition& condition) {
        if (auto indexed = indexed_lookup(condition)) return *indexed;
        std::vector<Row*> result;
        for (auto& row : rows_) {
            if (row.alive && evaluate(condition, row)) result.push_back(&row);
        }
        return result;
    }

    void select(const std::vector<Projection>& projections, const Condition& condition) {
        const auto rows = matching(condition);
        std::vector<Projection> actual = projections;
        if (actual.empty()) {
            for (const auto& column : columns_) actual.push_back({column.name, column.name});
        }

        bool has_aggregate = false;
        for (const auto& projection : actual) {
            if (projection.aggregate != Projection::Aggregate::None) {
                has_aggregate = true;
                break;
            }
        }

        if (has_aggregate) {
            bool has_regular = false;
            for (const auto& projection : actual) {
                if (projection.aggregate == Projection::Aggregate::None) {
                    has_regular = true;
                    break;
                }
            }
            if (has_regular) {
                throw DbError("Cannot mix aggregate and regular SELECT projections");
            }

            nlohmann::ordered_json object = nlohmann::ordered_json::object();
            for (const auto& projection : actual) {
                std::string field = projection.alias;
                if (field.empty()) {
                    if (projection.aggregate == Projection::Aggregate::Count) field = projection.count_all ? "COUNT(*)" : "COUNT(" + projection.column + ")";
                    if (projection.aggregate == Projection::Aggregate::Sum) field = "SUM(" + projection.column + ")";
                    if (projection.aggregate == Projection::Aggregate::Avg) field = "AVG(" + projection.column + ")";
                }

                if (projection.aggregate == Projection::Aggregate::Count) {
                    if (projection.count_all) {
                        object[field] = rows.size();
                    } else {
                        const size_t pos = column_index(projection.column);
                        size_t count = 0;
                        for (const auto* row : rows) {
                            if (!std::holds_alternative<std::monostate>(row->values[pos])) ++count;
                        }
                        object[field] = count;
                    }
                    continue;
                }

                const size_t pos = column_index(projection.column);
                if (columns_[pos].type != ColumnType::Int) throw DbError("SUM and AVG require int column: " + projection.column);
                long long sum = 0;
                size_t count = 0;
                for (const auto* row : rows) {
                    if (std::holds_alternative<std::monostate>(row->values[pos])) continue;
                    sum += std::get<int>(row->values[pos]);
                    ++count;
                }
                if (projection.aggregate == Projection::Aggregate::Sum) {
                    object[field] = sum;
                } else {
                    object[field] = count == 0 ? nlohmann::ordered_json(nullptr) : nlohmann::ordered_json(static_cast<double>(sum) / count);
                }
            }
            nlohmann::ordered_json result = nlohmann::ordered_json::array();
            result.push_back(std::move(object));
            std::cout << result.dump() << "\n";
            return;
        }

        nlohmann::ordered_json result = nlohmann::ordered_json::array();
        for (const auto* row : rows) {
            nlohmann::ordered_json object = nlohmann::ordered_json::object();
            for (const auto& projection : actual) {
                const size_t pos = column_index(projection.column);
                const std::string field = projection.alias.empty() ? projection.column : projection.alias;
                object[field] = value_to_json(row->values[pos]);
            }
            result.push_back(std::move(object));
        }
        std::cout << result.dump() << "\n";
    }

    size_t erase_where(const Condition& condition) {
        auto targets = matching(condition);
        for (auto* row : targets) {
            append_history("DELETE", *row);
            row->alive = false;
        }
        rebuild_indexes();
        save_rows();
        return targets.size();
    }

    size_t update_where(const std::vector<std::pair<std::string, Token>>& assignments, const Condition& condition) {
        auto targets = matching(condition);
        for (auto* row : targets) {
            append_history("UPDATE", *row);
            Row candidate = *row;
            for (const auto& [name, token] : assignments) {
                const size_t pos = column_index(name);
                candidate.values[pos] = token_to_value(token, columns_[pos].type);
            }
            validate_row(candidate, row->id);
            *row = std::move(candidate);
        }
        rebuild_indexes();
        save_rows();
        return targets.size();
    }

    void revert_to(const std::string& timestamp) {
        std::ifstream in(dir_ / "history.log");
        if (!in) {
            save_rows();
            return;
        }

        struct HistoryEntry {
            std::string timestamp;
            std::string operation;
            Row row;
        };

        std::vector<HistoryEntry> entries;
        std::string line;
        while (std::getline(in, line)) {
            std::stringstream ss(line);
            std::string ts;
            std::string op;
            std::string row_text;
            if (!std::getline(ss, ts, '\t') || !std::getline(ss, op, '\t') || !std::getline(ss, row_text)) continue;
            entries.push_back({ts, op, parse_row(row_text)});
        }

        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            if (it->timestamp <= timestamp) continue;
            if (it->operation == "INSERT") {
                for (auto& row : rows_) {
                    if (row.id == it->row.id) row.alive = false;
                }
            } else {
                bool replaced = false;
                for (auto& row : rows_) {
                    if (row.id == it->row.id) {
                        row = it->row;
                        replaced = true;
                        break;
                    }
                }
                if (!replaced) rows_.push_back(it->row);
            }
        }

        rebuild_indexes();
        save_rows();
    }

    size_t column_index(const std::string& name) const {
        const auto it = column_pos_.find(name);
        if (it == column_pos_.end()) throw DbError("Unknown column: " + name);
        return it->second;
    }

    Value operand_value(const Operand& operand, const Row& row) const {
        if (operand.column) return row.values[column_index(operand.name)];
        if (operand.literal.kind == TokenKind::Number) return std::stoi(operand.literal.text);
        if (operand.literal.kind == TokenKind::String) return intern_string(operand.literal.text);
        if (upper(operand.literal.text) == "NULL") return std::monostate{};
        throw DbError("Unknown literal: " + operand.literal.text);
    }

private:
    std::string serialize_row(const Row& row) const {
        std::ostringstream out;
        out << row.id << '\t' << (row.alive ? "1" : "0");
        for (const auto& value : row.values) out << '\t' << value_to_storage(value);
        return out.str();
    }

    Row parse_row(const std::string& line) {
        std::vector<std::string> parts;
        std::stringstream ss(line);
        std::string part;
        while (std::getline(ss, part, '\t')) parts.push_back(part);
        if (parts.size() != columns_.size() + 2) throw DbError("Corrupted row in history.log");

        Row row;
        row.id = static_cast<size_t>(std::stoull(parts[0]));
        row.alive = parts[1] == "1";
        for (size_t i = 2; i < parts.size(); ++i) row.values.push_back(value_from_storage(parts[i]));
        return row;
    }

    void append_history(const std::string& operation, const Row& row) const {
        std::ofstream out(dir_ / "history.log", std::ios::app);
        if (!out) throw DbError("Cannot write history.log");
        out << current_timestamp() << '\t' << operation << '\t' << serialize_row(row) << '\n';
    }

    void load_rows() {
        rows_.clear();
        next_id_ = 1;
        std::ifstream in(dir_ / "rows.dat");
        if (!in) return;
        std::string line;
        while (std::getline(in, line)) {
            std::vector<std::string> parts;
            std::stringstream ss(line);
            std::string part;
            while (std::getline(ss, part, '\t')) parts.push_back(part);
            if (parts.size() != columns_.size() + 2) throw DbError("Corrupted rows.dat");
            Row row = parse_row(line);
            next_id_ = std::max(next_id_, row.id + 1);
            rows_.push_back(std::move(row));
        }
    }

    void save_rows() const {
        fs::create_directories(dir_);
        std::ofstream out(dir_ / "rows.dat", std::ios::trunc);
        if (!out) throw DbError("Cannot write rows.dat");
        for (const auto& row : rows_) {
            out << serialize_row(row) << '\n';
        }
    }

    void rebuild_indexes() {
        for (auto& [_, index] : indexes_) {
            index.ints.clear();
            index.strings.clear();
        }
        for (const auto& row : rows_) {
            if (!row.alive) continue;
            for (const auto& column : columns_) {
                if (!column.indexed) continue;
                const auto& value = row.values[column_index(column.name)];
                if (std::holds_alternative<std::monostate>(value)) throw DbError("Indexed column contains NULL");
                auto& index = indexes_.at(column.name);
                bool inserted = false;
                if (column.type == ColumnType::Int) inserted = index.ints.insert({std::get<int>(value), row.id}).second;
                else inserted = index.strings.insert({*std::get<InternedString>(value), row.id}).second;
                if (!inserted) throw DbError("Duplicate value for INDEXED column: " + column.name);
            }
        }
    }

    void validate_row(const Row& row, std::optional<size_t> replacing_id) const {
        for (size_t i = 0; i < columns_.size(); ++i) {
            if ((columns_[i].not_null || columns_[i].indexed) && std::holds_alternative<std::monostate>(row.values[i])) {
                throw DbError("Column cannot be NULL: " + columns_[i].name);
            }
        }
        for (const auto& other : rows_) {
            if (!other.alive || (replacing_id && other.id == *replacing_id)) continue;
            for (size_t i = 0; i < columns_.size(); ++i) {
                if (!columns_[i].indexed) continue;
                if (row.values[i] == other.values[i]) throw DbError("Duplicate value for INDEXED column: " + columns_[i].name);
            }
        }
    }

    static int compare_values(const Value& lhs, const Value& rhs) {
        if (std::holds_alternative<std::monostate>(lhs) || std::holds_alternative<std::monostate>(rhs)) {
            if (std::holds_alternative<std::monostate>(lhs) && std::holds_alternative<std::monostate>(rhs)) return 0;
            return std::holds_alternative<std::monostate>(lhs) ? -1 : 1;
        }
        if (lhs.index() != rhs.index()) throw DbError("Cannot compare values of different types");
        if (std::holds_alternative<int>(lhs)) {
            const int a = std::get<int>(lhs), b = std::get<int>(rhs);
            return (a > b) - (a < b);
        }
        const auto& a = *std::get<InternedString>(lhs);
        const auto& b = *std::get<InternedString>(rhs);
        return (a > b) - (a < b);
    }

    bool evaluate(const Condition& condition, const Row& row) const {
        if (condition.kind == Condition::Kind::None) return true;
        if (condition.kind == Condition::Kind::And) return evaluate(*condition.lhs, row) && evaluate(*condition.rhs, row);
        if (condition.kind == Condition::Kind::Or) return evaluate(*condition.lhs, row) || evaluate(*condition.rhs, row);
        const Value left = operand_value(condition.left, row);
        if (condition.kind == Condition::Kind::Like) {
            if (!std::holds_alternative<InternedString>(left)) throw DbError("LIKE requires string value");
            const Value pattern = operand_value(condition.right, row);
            if (!std::holds_alternative<InternedString>(pattern)) throw DbError("LIKE pattern must be string");
            return std::regex_match(*std::get<InternedString>(left), std::regex(*std::get<InternedString>(pattern)));
        }
        if (condition.kind == Condition::Kind::Between) {
            const Value low = operand_value(condition.right, row);
            const Value high = operand_value(condition.high, row);
            return compare_values(left, low) >= 0 && compare_values(left, high) < 0;
        }
        const Value right = operand_value(condition.right, row);
        const int cmp = compare_values(left, right);
        if (condition.op == "==") return cmp == 0;
        if (condition.op == "!=") return cmp != 0;
        if (condition.op == "<") return cmp < 0;
        if (condition.op == ">") return cmp > 0;
        if (condition.op == "<=") return cmp <= 0;
        if (condition.op == ">=") return cmp >= 0;
        throw DbError("Unknown comparison operator");
    }

    Row* row_by_id(size_t row_id) {
        for (auto& row : rows_) {
            if (row.alive && row.id == row_id) return &row;
        }
        return nullptr;
    }

    template <typename Tree, typename Key>
    std::vector<Row*> collect_index_range(Tree& tree, const Key& low, bool has_low, bool low_strict,
                                          const Key& high, bool has_high, bool high_inclusive) {
        std::vector<Row*> result;
        auto it = has_low ? (low_strict ? tree.upper_bound(low) : tree.lower_bound(low)) : tree.begin();
        for (; it != tree.end(); ++it) {
            if (has_high) {
                const bool past = high_inclusive ? high < it->first : !(it->first < high);
                if (past) break;
            }
            if (auto* row = row_by_id(it->second)) result.push_back(row);
        }
        return result;
    }

    std::optional<std::vector<Row*>> indexed_lookup(const Condition& condition) {
        if (condition.kind == Condition::Kind::And || condition.kind == Condition::Kind::Or) return std::nullopt;
        if (!condition.left.column) return std::nullopt;
        const auto idx = indexes_.find(condition.left.name);
        if (idx == indexes_.end()) return std::nullopt;
        const size_t col = column_index(condition.left.name);

        if (condition.kind == Condition::Kind::Compare) {
            if (condition.right.column || condition.op == "!=") return std::nullopt;
            Value value = token_to_value(condition.right.literal, columns_[col].type);
            if (std::holds_alternative<std::monostate>(value)) return std::vector<Row*>{};

            if (condition.op == "==") {
                std::optional<size_t> row_id;
                if (columns_[col].type == ColumnType::Int) {
                    auto it = idx->second.ints.find(std::get<int>(value));
                    if (it != idx->second.ints.end()) row_id = it->second;
                } else {
                    auto it = idx->second.strings.find(*std::get<InternedString>(value));
                    if (it != idx->second.strings.end()) row_id = it->second;
                }
                if (!row_id) return std::vector<Row*>{};
                if (auto* row = row_by_id(*row_id)) return std::vector<Row*>{row};
                return std::vector<Row*>{};
            }

            if (columns_[col].type == ColumnType::Int) {
                const int key = std::get<int>(value);
                if (condition.op == ">") return collect_index_range(idx->second.ints, key, true, true, key, false, false);
                if (condition.op == ">=") return collect_index_range(idx->second.ints, key, true, false, key, false, false);
                if (condition.op == "<") return collect_index_range(idx->second.ints, key, false, false, key, true, false);
                if (condition.op == "<=") return collect_index_range(idx->second.ints, key, false, false, key, true, true);
            } else {
                const auto& key = *std::get<InternedString>(value);
                if (condition.op == ">") return collect_index_range(idx->second.strings, key, true, true, key, false, false);
                if (condition.op == ">=") return collect_index_range(idx->second.strings, key, true, false, key, false, false);
                if (condition.op == "<") return collect_index_range(idx->second.strings, key, false, false, key, true, false);
                if (condition.op == "<=") return collect_index_range(idx->second.strings, key, false, false, key, true, true);
            }
        }

        if (condition.kind == Condition::Kind::Between) {
            if (condition.right.column || condition.high.column) return std::nullopt;
            Value low = token_to_value(condition.right.literal, columns_[col].type);
            Value high = token_to_value(condition.high.literal, columns_[col].type);
            if (std::holds_alternative<std::monostate>(low) || std::holds_alternative<std::monostate>(high)) {
                return std::vector<Row*>{};
            }
            if (columns_[col].type == ColumnType::Int) {
                return collect_index_range(idx->second.ints, std::get<int>(low), true, false, std::get<int>(high), true, false);
            }
            return collect_index_range(idx->second.strings, *std::get<InternedString>(low), true, false,
                                       *std::get<InternedString>(high), true, false);
        }

        return std::nullopt;
    }

    fs::path dir_;
    std::vector<Column> columns_;
    std::unordered_map<std::string, size_t> column_pos_;
    std::map<std::string, Index> indexes_;
    std::vector<Row> rows_;
    size_t next_id_{1};
};

class Engine {
public:
    explicit Engine(fs::path root, std::string client_id = "local") : root_(std::move(root)), client_id_(std::move(client_id)) {
        fs::create_directories(root_);
    }

    void execute(const std::string& sql) {
        const auto started = std::chrono::system_clock::now();
        const std::string started_text = current_timestamp();
        std::string status = "OK";
        try {
        if (sql.find_first_not_of(" \t\r\n") == std::string::npos) return;
        const Command command = parse_sql_command(sql);

        switch (command.kind) {
            case Command::Kind::CreateDatabase:
                create_database(command.database_name);
                break;
            case Command::Kind::DropDatabase:
                drop_database(command.database_name);
                break;
            case Command::Kind::UseDatabase:
                use_database(command.database_name);
                break;
            case Command::Kind::CreateTable:
                create_table(command);
                break;
            case Command::Kind::DropTable:
                drop_table(resolve_db(command.table.database), command.table.table);
                break;
            case Command::Kind::Insert:
                insert(command);
                break;
            case Command::Kind::Select:
                select(command);
                break;
            case Command::Kind::Delete:
                erase(command);
                break;
            case Command::Kind::Update:
                update(command);
                break;
            case Command::Kind::Revert:
                revert(command);
                break;
        }
        } catch (const std::exception& ex) {
            status = std::string("ERROR: ") + ex.what();
            log_access(sql, started_text, started, status);
            throw;
        }
        log_access(sql, started_text, started, status);
    }

private:
    void create_database(const std::string& name) {
        check_name(name);
        const auto dir = root_ / name;
        if (fs::exists(dir)) throw DbError("Database already exists: " + name);
        fs::create_directories(dir);
        std::cout << "OK\n";
    }

    void drop_database(const std::string& name) {
        check_name(name);
        const auto dir = root_ / name;
        if (!fs::exists(dir)) throw DbError("Database does not exist: " + name);
        fs::remove_all(dir);
        if (current_db_ == name) current_db_.reset();
        std::cout << "OK\n";
    }

    void use_database(const std::string& name) {
        check_name(name);
        if (!fs::is_directory(root_ / name)) throw DbError("Database does not exist: " + name);
        current_db_ = name;
        std::cout << "OK\n";
    }

    void create_table(const Command& command) {
        const std::string table = command.table.table;
        const std::string db = resolve_db(command.table.database);
        check_database(db);
        check_name(table);
        const auto dir = table_dir(db, table);
        if (fs::exists(dir)) throw DbError("Table already exists: " + table);

        const auto& columns = command.columns;
        std::vector<std::string> seen_columns;
        for (const auto& column : columns) {
            if (column.has_default && column.not_null && std::holds_alternative<std::monostate>(column.default_value)) {
                throw DbError("NOT_NULL column cannot have DEFAULT NULL: " + column.name);
            }
            if (column.has_default && !std::holds_alternative<std::monostate>(column.default_value)) {
                if (column.type == ColumnType::Int && !std::holds_alternative<int>(column.default_value)) {
                    throw DbError("DEFAULT type mismatch for column: " + column.name);
                }
                if (column.type == ColumnType::String && !std::holds_alternative<InternedString>(column.default_value)) {
                    throw DbError("DEFAULT type mismatch for column: " + column.name);
                }
            }
            if (std::find(seen_columns.begin(), seen_columns.end(), column.name) != seen_columns.end()) {
                throw DbError("Duplicate column: " + column.name);
            }
            seen_columns.push_back(column.name);
        }
        if (columns.empty()) throw DbError("Table must contain at least one column");

        fs::create_directories(dir);
        save_schema(dir, columns);
        std::ofstream(dir / "rows.dat", std::ios::trunc);
        std::cout << "OK\n";
    }

    void drop_table(const std::string& db, const std::string& table) {
        const auto dir = table_dir(db, table);
        if (!fs::exists(dir)) throw DbError("Table does not exist: " + table);
        fs::remove_all(dir);
        std::cout << "OK\n";
    }

    void insert(const Command& command) {
        Table table = open_table(command.table.database, command.table.table);
        size_t inserted = 0;
        for (const auto& values : command.insert_rows) {
            table.insert(command.insert_columns, values);
            ++inserted;
        }
        std::cout << "OK " << inserted << "\n";
    }

    void select(const Command& command) {
        Table table = open_table(command.table.database, command.table.table);
        table.select(command.projections, command.has_condition ? command.condition : Condition{});
    }

    void erase(const Command& command) {
        Table table = open_table(command.table.database, command.table.table);
        const size_t count = table.erase_where(command.condition);
        std::cout << "OK " << count << "\n";
    }

    void update(const Command& command) {
        Table table = open_table(command.table.database, command.table.table);
        const size_t count = table.update_where(command.assignments, command.condition);
        std::cout << "OK " << count << "\n";
    }

    void revert(const Command& command) {
        Table table = open_table(command.table.database, command.table.table);
        table.revert_to(command.timestamp);
        std::cout << "OK\n";
    }

    Table open_table(const std::optional<std::string>& explicit_db, const std::string& table) {
        const std::string db = explicit_db ? *explicit_db : require_current_db();
        const auto dir = table_dir(db, table);
        if (!fs::is_directory(dir)) throw DbError("Table does not exist: " + table);
        return Table(dir, load_schema(dir));
    }

    std::string resolve_db(const std::optional<std::string>& explicit_db) const {
        return explicit_db ? *explicit_db : require_current_db();
    }

    std::string require_current_db() const {
        if (!current_db_) throw DbError("No active database. Use USE database_name or database.table");
        return *current_db_;
    }

    void check_database(const std::string& db) const {
        if (!fs::is_directory(root_ / db)) throw DbError("Database does not exist: " + db);
    }

    fs::path table_dir(const std::string& db, const std::string& table) const {
        return root_ / db / table;
    }

    static void check_name(const std::string& name) {
        if (!valid_identifier(name)) throw DbError("Invalid identifier: " + name);
    }

    static void save_schema(const fs::path& dir, const std::vector<Column>& columns) {
        std::ofstream out(dir / "schema.dat", std::ios::trunc);
        if (!out) throw DbError("Cannot write schema.dat");
        for (const auto& column : columns) {
            out << column.name << '\t' << (column.type == ColumnType::Int ? "int" : "string") << '\t'
                << column.not_null << '\t' << column.indexed << '\t' << column.has_default << '\t'
                << (column.has_default ? value_to_storage(column.default_value) : "N:") << '\n';
        }
    }

    static std::vector<Column> load_schema(const fs::path& dir) {
        std::ifstream in(dir / "schema.dat");
        if (!in) throw DbError("Cannot read schema.dat");
        std::vector<Column> columns;
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            std::vector<std::string> parts;
            std::stringstream ss(line);
            std::string part;
            while (std::getline(ss, part, '\t')) parts.push_back(part);
            if (parts.size() < 4) throw DbError("Corrupted schema.dat");

            Column column;
            column.name = parts[0];
            column.type = parts[1] == "int" ? ColumnType::Int : ColumnType::String;
            column.not_null = parts[2] == "1";
            column.indexed = parts[3] == "1";
            if (parts.size() >= 6) {
                column.has_default = parts[4] == "1";
                if (column.has_default) column.default_value = value_from_storage(parts[5]);
            }
            columns.push_back(std::move(column));
        }
        return columns;
    }

    void log_access(const std::string& sql, const std::string& started_text,
                    const std::chrono::system_clock::time_point& started, const std::string& status) const {
        const auto finished = std::chrono::system_clock::now();
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(finished - started).count();
        std::ofstream out(root_ / "access.log", std::ios::app);
        if (!out) return;

        std::string body = sql;
        std::replace(body.begin(), body.end(), '\n', ' ');
        std::replace(body.begin(), body.end(), '\t', ' ');

        out << "start=" << started_text
            << "\tfinish=" << current_timestamp()
            << "\telapsed_ms=" << elapsed_ms
            << "\tclient=" << client_id_
            << "\thandler=" << std::hash<std::thread::id>{}(std::this_thread::get_id())
            << "\tstatus=" << status
            << "\tbody=" << body << '\n';
    }

    fs::path root_;
    std::optional<std::string> current_db_;
    std::string client_id_;
};

std::vector<std::string> split_commands(std::istream& in) {
    std::vector<std::string> commands;
    std::string current;
    bool in_string = false;
    char ch;
    while (in.get(ch)) {
        if (ch == '"' && (current.empty() || current.back() != '\\')) in_string = !in_string;
        if (ch == ';' && !in_string) {
            commands.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty() && current.find_first_not_of(" \t\r\n") != std::string::npos) {
        commands.push_back(current);
    }
    return commands;
}

} // namespace

int course_dbms_main(int argc, char** argv) {
    try {
        Engine engine("db_data");
        if (argc == 1) {
            std::cout << "course_dbms> ";
            std::string line;
            std::string command;
            while (std::getline(std::cin, line)) {
                command += line + "\n";
                std::stringstream ss(command);
                auto commands = split_commands(ss);
                if (!commands.empty() && command.find(';') != std::string::npos) {
                    for (const auto& sql : commands) {
                        try {
                            engine.execute(sql);
                        } catch (const std::exception& ex) {
                            std::cout << "ERROR: " << ex.what() << "\n";
                        }
                    }
                    command.clear();
                }
                std::cout << "course_dbms> ";
            }
        } else if (argc == 2) {
            std::ifstream script(argv[1]);
            if (!script) {
                std::cerr << "ERROR: Cannot open script file\n";
                return 1;
            }
            for (const auto& sql : split_commands(script)) {
                try {
                    engine.execute(sql);
                } catch (const std::exception& ex) {
                    std::cout << "ERROR: " << ex.what() << "\n";
                }
            }
        } else {
            std::cerr << "Usage: " << argv[0] << " [script.sql]\n";
            return 1;
        }
    } catch (const std::exception& ex) {
        std::cerr << "FATAL: " << ex.what() << "\n";
        return 1;
    }
    return 0;
}
