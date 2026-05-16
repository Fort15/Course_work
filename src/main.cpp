#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <boost/spirit/include/qi.hpp>
#include <b_plus_tree.h>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace {

enum class TokenKind { Identifier, Number, String, Symbol, End };

struct Token {
    TokenKind kind{};
    std::string text;
};

struct DbError : std::runtime_error {
    explicit DbError(const std::string& message) : std::runtime_error(message) {}
};

std::string upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return value;
}

bool valid_identifier(const std::string& value) {
    if (value.empty() || std::isdigit(static_cast<unsigned char>(value.front()))) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '_';
    });
}

struct Parser {
    explicit Parser(std::string input) : input_(std::move(input)), it_(input_.begin()), end_(input_.end()) {}

    bool done() {
        skip();
        return it_ == end_;
    }

    bool match_symbol(const std::string& symbol) {
        auto tmp = it_;
        const bool ok = boost::spirit::qi::phrase_parse(tmp, end_, boost::spirit::qi::lit(symbol), boost::spirit::qi::space);
        if (!ok) return false;
        it_ = tmp;
        return true;
    }

    bool match_keyword(const std::string& keyword) {
        auto tmp = it_;
        std::string parsed;
        if (!parse_identifier(tmp, parsed) || upper(parsed) != keyword) return false;
        it_ = tmp;
        return true;
    }

    bool next_is_keyword(const std::string& keyword) const {
        auto tmp = it_;
        std::string parsed;
        return parse_identifier(tmp, parsed) && upper(parsed) == keyword;
    }

    bool next_is_symbol() const {
        auto tmp = it_;
        skip(tmp);
        if (tmp == end_) return false;
        const char ch = *tmp;
        return std::string("!<>=(),.*").find(ch) != std::string::npos;
    }

    std::string comparison_operator() {
        static const std::vector<std::string> ops = {"==", "!=", "<=", ">=", "<", ">"};
        for (const auto& op : ops) {
            if (match_symbol(op)) return op;
        }
        throw DbError("Expected comparison operator");
    }

    void expect_keyword(const std::string& keyword) {
        if (!match_keyword(keyword)) throw DbError("Expected keyword " + keyword);
    }

    void expect_symbol(const std::string& symbol) {
        if (!match_symbol(symbol)) throw DbError("Expected '" + symbol + "'");
    }

    std::string identifier() {
        std::string value;
        if (!parse_identifier(it_, value) || !valid_identifier(value)) throw DbError("Expected identifier");
        return value;
    }

    std::string table_name(std::optional<std::string>& db_name) {
        std::string first = identifier();
        if (match_symbol(".")) {
            db_name = first;
            return identifier();
        }
        db_name.reset();
        return first;
    }

    Token literal_or_identifier() {
        Token token;
        if (parse_string(it_, token.text)) {
            token.kind = TokenKind::String;
            return token;
        }
        if (parse_number(it_, token.text)) {
            token.kind = TokenKind::Number;
            return token;
        }
        if (parse_identifier(it_, token.text)) {
            token.kind = TokenKind::Identifier;
            return token;
        }
        throw DbError("Expected value or column name");
    }

    std::string remaining_preview() {
        skip();
        std::string rest(it_, end_);
        const auto pos = rest.find_first_of("\r\n");
        if (pos != std::string::npos) rest = rest.substr(0, pos);
        if (rest.size() > 32) rest = rest.substr(0, 32) + "...";
        return rest.empty() ? "<end>" : rest;
    }

private:
    using It = std::string::const_iterator;

    void skip() { skip(it_); }

    void skip(It& it) const {
        boost::spirit::qi::phrase_parse(it, end_, boost::spirit::qi::eps, boost::spirit::qi::space);
    }

    bool parse_identifier(It& it, std::string& value) const {
        namespace qi = boost::spirit::qi;
        value.clear();
        auto tmp = it;
        const bool ok = qi::phrase_parse(tmp, end_, qi::lexeme[(qi::alpha | qi::char_('_')) >> *(qi::alnum | qi::char_('_'))],
                                         qi::space, value);
        if (!ok) return false;
        it = tmp;
        return true;
    }

    bool parse_number(It& it, std::string& value) const {
        namespace qi = boost::spirit::qi;
        value.clear();
        auto tmp = it;
        const bool ok = qi::phrase_parse(tmp, end_, qi::raw[qi::lexeme[-qi::char_('-') >> +qi::digit]], qi::space, value);
        if (!ok) return false;
        it = tmp;
        return true;
    }

    bool parse_string(It& it, std::string& value) const {
        namespace qi = boost::spirit::qi;
        value.clear();
        auto tmp = it;
        const bool ok = qi::phrase_parse(
                tmp, end_,
                qi::raw[qi::lexeme['"' >> *((qi::char_('\\') >> qi::char_) | (qi::char_ - '"')) >> '"']],
                qi::space, value);
        if (!ok) return false;
        if (value.size() < 2) return false;
        value = value.substr(1, value.size() - 2);
        std::string unescaped;
        for (size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '\\' && i + 1 < value.size()) {
                unescaped.push_back(value[++i]);
            } else {
                unescaped.push_back(value[i]);
            }
        }
        value = std::move(unescaped);
        it = tmp;
        return true;
    }

    std::string input_;
    It it_;
    It end_;
};

enum class ColumnType { Int, String };

using Value = std::variant<std::monostate, int, std::string>;

struct Column {
    std::string name;
    ColumnType type{};
    bool not_null{false};
    bool indexed{false};
    bool has_default{false};
    Value default_value;
};

std::string value_to_storage(const Value& value) {
    if (std::holds_alternative<std::monostate>(value)) return "N:";
    if (std::holds_alternative<int>(value)) return "I:" + std::to_string(std::get<int>(value));
    std::string out = "S:";
    for (char ch : std::get<std::string>(value)) {
        if (ch == '\\' || ch == '\t' || ch == '\n') out.push_back('\\');
        if (ch == '\t') out.push_back('t');
        else if (ch == '\n') out.push_back('n');
        else out.push_back(ch);
    }
    return out;
}

Value value_from_storage(const std::string& text) {
    if (text == "N:") return std::monostate{};
    if (text.rfind("I:", 0) == 0) return std::stoi(text.substr(2));
    if (text.rfind("S:", 0) == 0) {
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
        return out;
    }
    throw DbError("Corrupted value in storage");
}

nlohmann::ordered_json value_to_json(const Value& value) {
    if (std::holds_alternative<std::monostate>(value)) return nullptr;
    if (std::holds_alternative<int>(value)) return std::get<int>(value);
    return std::get<std::string>(value);
}

Value token_to_value(const Token& token, ColumnType type) {
    if (token.kind == TokenKind::Identifier && upper(token.text) == "NULL") return std::monostate{};
    if (type == ColumnType::Int) {
        if (token.kind != TokenKind::Number) throw DbError("Expected int literal");
        return std::stoi(token.text);
    }
    if (token.kind != TokenKind::String) throw DbError("Expected string literal");
    return token.text;
}

struct Row {
    size_t id{};
    bool alive{true};
    std::vector<Value> values;
};

struct Operand {
    bool column{false};
    std::string name;
    Token literal;
};

struct Condition {
    enum class Kind { None, Compare, Between, Like } kind{Kind::None};
    Operand left;
    std::string op;
    Operand right;
    Operand high;
};

struct Projection {
    enum class Aggregate { None, Count, Sum, Avg };

    std::string column;
    std::string alias;
    Aggregate aggregate{Aggregate::None};
    bool count_all{false};
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

        const bool has_aggregate = std::any_of(actual.begin(), actual.end(), [](const Projection& projection) {
            return projection.aggregate != Projection::Aggregate::None;
        });
        if (has_aggregate) {
            if (std::any_of(actual.begin(), actual.end(), [](const Projection& projection) {
                    return projection.aggregate == Projection::Aggregate::None;
                })) {
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
        for (auto* row : targets) row->alive = false;
        rebuild_indexes();
        save_rows();
        return targets.size();
    }

    size_t update_where(const std::vector<std::pair<std::string, Token>>& assignments, const Condition& condition) {
        auto targets = matching(condition);
        for (auto* row : targets) {
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

    size_t column_index(const std::string& name) const {
        const auto it = column_pos_.find(name);
        if (it == column_pos_.end()) throw DbError("Unknown column: " + name);
        return it->second;
    }

    Value operand_value(const Operand& operand, const Row& row) const {
        if (operand.column) return row.values[column_index(operand.name)];
        if (operand.literal.kind == TokenKind::Number) return std::stoi(operand.literal.text);
        if (operand.literal.kind == TokenKind::String) return operand.literal.text;
        if (upper(operand.literal.text) == "NULL") return std::monostate{};
        throw DbError("Unknown literal: " + operand.literal.text);
    }

private:
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
            Row row;
            row.id = static_cast<size_t>(std::stoull(parts[0]));
            row.alive = parts[1] == "1";
            for (size_t i = 2; i < parts.size(); ++i) row.values.push_back(value_from_storage(parts[i]));
            next_id_ = std::max(next_id_, row.id + 1);
            rows_.push_back(std::move(row));
        }
    }

    void save_rows() const {
        fs::create_directories(dir_);
        std::ofstream out(dir_ / "rows.dat", std::ios::trunc);
        if (!out) throw DbError("Cannot write rows.dat");
        for (const auto& row : rows_) {
            out << row.id << '\t' << (row.alive ? "1" : "0");
            for (const auto& value : row.values) out << '\t' << value_to_storage(value);
            out << '\n';
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
                else inserted = index.strings.insert({std::get<std::string>(value), row.id}).second;
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
        const auto& a = std::get<std::string>(lhs);
        const auto& b = std::get<std::string>(rhs);
        return (a > b) - (a < b);
    }

    bool evaluate(const Condition& condition, const Row& row) const {
        if (condition.kind == Condition::Kind::None) return true;
        const Value left = operand_value(condition.left, row);
        if (condition.kind == Condition::Kind::Like) {
            if (!std::holds_alternative<std::string>(left)) throw DbError("LIKE requires string value");
            const Value pattern = operand_value(condition.right, row);
            if (!std::holds_alternative<std::string>(pattern)) throw DbError("LIKE pattern must be string");
            return std::regex_match(std::get<std::string>(left), std::regex(std::get<std::string>(pattern)));
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
                    auto it = idx->second.strings.find(std::get<std::string>(value));
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
                const auto& key = std::get<std::string>(value);
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
            return collect_index_range(idx->second.strings, std::get<std::string>(low), true, false,
                                       std::get<std::string>(high), true, false);
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
    explicit Engine(fs::path root) : root_(std::move(root)) {
        fs::create_directories(root_);
    }

    void execute(const std::string& sql) {
        Parser parser(sql);
        if (parser.done()) return;

        if (parser.match_keyword("CREATE")) {
            if (parser.match_keyword("DATABASE")) create_database(parser.identifier());
            else if (parser.match_keyword("TABLE")) create_table(parser);
            else throw DbError("Expected DATABASE or TABLE after CREATE");
        } else if (parser.match_keyword("DROP")) {
            if (parser.match_keyword("DATABASE")) drop_database(parser.identifier());
            else if (parser.match_keyword("TABLE")) {
                std::optional<std::string> db;
                const std::string table = parser.table_name(db);
                drop_table(resolve_db(parser, db), table);
            } else throw DbError("Expected DATABASE or TABLE after DROP");
        } else if (parser.match_keyword("USE")) {
            use_database(parser.identifier());
        } else if (parser.match_keyword("INSERT")) {
            insert(parser);
        } else if (parser.match_keyword("SELECT")) {
            select(parser);
        } else if (parser.match_keyword("DELETE")) {
            erase(parser);
        } else if (parser.match_keyword("UPDATE")) {
            update(parser);
        } else {
            throw DbError("Unknown command");
        }
        if (!parser.done()) throw DbError("Unexpected token after command: " + parser.remaining_preview());
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

    void create_table(Parser& parser) {
        std::optional<std::string> explicit_db;
        const std::string table = parser.table_name(explicit_db);
        const std::string db = explicit_db ? *explicit_db : require_current_db();
        check_database(db);
        check_name(table);
        const auto dir = table_dir(db, table);
        if (fs::exists(dir)) throw DbError("Table already exists: " + table);

        parser.expect_symbol("(");
        std::vector<Column> columns;
        do {
            Column column;
            column.name = parser.identifier();
            const std::string type = upper(parser.identifier());
            if (type == "INT") column.type = ColumnType::Int;
            else if (type == "STRING") column.type = ColumnType::String;
            else throw DbError("Unknown column type: " + type);
            while (true) {
                if (parser.match_keyword("NOT_NULL")) {
                    column.not_null = true;
                } else if (parser.match_keyword("INDEXED")) {
                    column.indexed = true;
                    column.not_null = true;
                } else if (parser.match_keyword("DEFAULT")) {
                    column.has_default = true;
                    column.default_value = token_to_value(parser.literal_or_identifier(), column.type);
                } else {
                    break;
                }
            }
            if (column.has_default && column.not_null && std::holds_alternative<std::monostate>(column.default_value)) {
                throw DbError("NOT_NULL column cannot have DEFAULT NULL: " + column.name);
            }
            if (std::any_of(columns.begin(), columns.end(), [&](const Column& other) { return other.name == column.name; })) {
                throw DbError("Duplicate column: " + column.name);
            }
            columns.push_back(column);
        } while (parser.match_symbol(","));
        parser.expect_symbol(")");
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

    void insert(Parser& parser) {
        parser.expect_keyword("INTO");
        std::optional<std::string> explicit_db;
        const std::string table_name = parser.table_name(explicit_db);
        Table table = open_table(explicit_db, table_name);

        parser.expect_symbol("(");
        std::vector<std::string> columns;
        do columns.push_back(parser.identifier()); while (parser.match_symbol(","));
        parser.expect_symbol(")");
        parser.expect_keyword("VALUE");
        size_t inserted = 0;
        do {
            parser.expect_symbol("(");
            std::vector<Token> values;
            do values.push_back(parser.literal_or_identifier()); while (parser.match_symbol(","));
            parser.expect_symbol(")");
            table.insert(columns, values);
            ++inserted;
        } while (parser.match_symbol(","));
        std::cout << "OK " << inserted << "\n";
    }

    void select(Parser& parser) {
        std::vector<Projection> projections;
        if (parser.match_symbol("*")) {
        } else {
            parser.expect_symbol("(");
            do {
                Projection projection;
                if (parser.next_is_keyword("COUNT") || parser.next_is_keyword("SUM") || parser.next_is_keyword("AVG")) {
                    const std::string function_name = upper(parser.identifier());
                    if (function_name == "COUNT") projection.aggregate = Projection::Aggregate::Count;
                    if (function_name == "SUM") projection.aggregate = Projection::Aggregate::Sum;
                    if (function_name == "AVG") projection.aggregate = Projection::Aggregate::Avg;
                    parser.expect_symbol("(");
                    if (projection.aggregate == Projection::Aggregate::Count && parser.match_symbol("*")) {
                        projection.count_all = true;
                    } else {
                        projection.column = parser.identifier();
                    }
                    parser.expect_symbol(")");
                } else {
                    projection.column = parser.identifier();
                    projection.alias = projection.column;
                }
                if (parser.match_keyword("AS")) projection.alias = parser.identifier();
                projections.push_back(projection);
            } while (parser.match_symbol(","));
            parser.expect_symbol(")");
        }
        parser.expect_keyword("FROM");
        std::optional<std::string> explicit_db;
        const std::string table_name = parser.table_name(explicit_db);
        Table table = open_table(explicit_db, table_name);
        Condition condition;
        if (parser.match_keyword("WHERE")) condition = parse_condition(parser);
        table.select(projections, condition);
    }

    void erase(Parser& parser) {
        parser.expect_keyword("FROM");
        std::optional<std::string> explicit_db;
        const std::string table_name = parser.table_name(explicit_db);
        Table table = open_table(explicit_db, table_name);
        parser.expect_keyword("WHERE");
        const size_t count = table.erase_where(parse_condition(parser));
        std::cout << "OK " << count << "\n";
    }

    void update(Parser& parser) {
        std::optional<std::string> explicit_db;
        const std::string table_name = parser.table_name(explicit_db);
        Table table = open_table(explicit_db, table_name);
        parser.expect_keyword("SET");
        std::vector<std::pair<std::string, Token>> assignments;
        do {
            std::string column = parser.identifier();
            parser.expect_symbol("=");
            assignments.push_back({column, parser.literal_or_identifier()});
        } while (parser.match_symbol(","));
        parser.expect_keyword("WHERE");
        const size_t count = table.update_where(assignments, parse_condition(parser));
        std::cout << "OK " << count << "\n";
    }

    static Operand parse_operand(Parser& parser) {
        Token token = parser.literal_or_identifier();
        if (token.kind == TokenKind::Identifier && upper(token.text) != "NULL") {
            return Operand{true, token.text, {}};
        }
        return Operand{false, "", token};
    }

    static Condition parse_condition(Parser& parser) {
        Condition condition;
        condition.left = parse_operand(parser);
        if (parser.match_keyword("BETWEEN")) {
            condition.kind = Condition::Kind::Between;
            condition.right = parse_operand(parser);
            parser.expect_keyword("AND");
            condition.high = parse_operand(parser);
            return condition;
        }
        if (parser.match_keyword("LIKE")) {
            condition.kind = Condition::Kind::Like;
            condition.right = parse_operand(parser);
            return condition;
        }
        condition.kind = Condition::Kind::Compare;
        condition.op = parser.comparison_operator();
        if (condition.op != "==" && condition.op != "!=" && condition.op != "<" && condition.op != ">" &&
            condition.op != "<=" && condition.op != ">=") {
            throw DbError("Invalid comparison operator: " + condition.op);
        }
        condition.right = parse_operand(parser);
        return condition;
    }

    Table open_table(const std::optional<std::string>& explicit_db, const std::string& table) {
        const std::string db = explicit_db ? *explicit_db : require_current_db();
        const auto dir = table_dir(db, table);
        if (!fs::is_directory(dir)) throw DbError("Table does not exist: " + table);
        return Table(dir, load_schema(dir));
    }

    std::string resolve_db(Parser&, const std::optional<std::string>& explicit_db) const {
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

    fs::path root_;
    std::optional<std::string> current_db_;
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

int main(int argc, char** argv) {
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
