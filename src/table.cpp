#include "table.h"

#include "db_error.h"
#include "db_util.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace course_dbms {

namespace fs = std::filesystem;

Table::Index::Index(
    ColumnType column_type,
    const fs::path& dir
) : type(column_type) {
    if (type == ColumnType::Int) {
        ints = std::make_unique<IntegerIndexTree>(dir / "int");
    } else {
        strings = std::make_unique<StringIndexTree>(dir / "string");
    }
}

Table::Index::Index(Index&&) noexcept = default;
Table::Index& Table::Index::operator=(Index&&) noexcept = default;

Table::Table(fs::path dir, std::vector<Column> columns)
    : dir_(std::move(dir)),
      columns_(std::move(columns)),
      storage_(dir_, columns_) {
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        column_pos_[columns_[i].name] = i;
    }

    storage_.ensure_data_file();

    const bool table_has_rows =
        storage_.read_header().alive_records != 0;

    bool rebuild_required = fs::exists(index_dirty_path());

    for (const auto& column : columns_) {
        if (!column.indexed) {
            continue;
        }

        const fs::path base =
            dir_ / "indexes" / column.name;

        const fs::path meta = column.type == ColumnType::Int
            ? base / "int" / "meta.bin"
            : base / "string" / "meta.bin";

        if (table_has_rows && !fs::exists(meta)) {
            rebuild_required = true;
        }

        indexes_.emplace(
            column.name,
            Index(column.type, base)
        );
    }

    if (rebuild_required) {
        mark_indexes_dirty();
        rebuild_indexes();
        clear_indexes_dirty();
    }
}

Table::~Table() = default;

const std::vector<Column>& Table::columns() const {
    return columns_;
}

void Table::insert(
    const std::vector<std::string>& names,
    const std::vector<std::vector<Token>>& raw_rows
) {
    std::set<std::string> supplied_columns;

    for (const auto& name : names) {
        if (!supplied_columns.insert(name).second) {
            throw DbError("Duplicate INSERT column: " + name);
        }
        (void)column_index(name);
    }

    RowStorage::DataHeader header = storage_.read_header();
    std::uint64_t next_id = header.next_id;

    std::vector<Row> inserted_rows;
    inserted_rows.reserve(raw_rows.size());

    for (const auto& raw_values : raw_rows) {
        if (names.size() != raw_values.size()) {
            throw DbError(
                "Column count does not match value count"
            );
        }

        Row row;
        row.id = static_cast<std::size_t>(next_id++);
        row.alive = true;
        row.values.assign(columns_.size(), std::monostate{});

        for (std::size_t i = 0; i < columns_.size(); ++i) {
            if (columns_[i].has_default) {
                row.values[i] = columns_[i].default_value;
            }
        }

        for (std::size_t i = 0; i < names.size(); ++i) {
            const std::size_t pos = column_index(names[i]);

            row.values[pos] = token_to_value(
                raw_values[i],
                columns_[pos].type
            );
        }

        validate_row(row);
        inserted_rows.push_back(std::move(row));
    }

    validate_new_index_values(inserted_rows, {});

    if (inserted_rows.empty()) {
        return;
    }

    mark_indexes_dirty();

    const auto inserted_refs =
        storage_.append_rows(inserted_rows, next_id);

    for (const auto& ref : inserted_refs) {
        insert_row_indexes(ref.row, ref.offset);
    }

    clear_indexes_dirty();

    for (const auto& row : inserted_rows) {
        append_history("INSERT", row);
    }
}

std::vector<RowRef> Table::matching(const Condition& condition) {
    validate_condition(condition);

    if (auto indexed = indexed_lookup(condition)) {
        return *indexed;
    }

    std::vector<RowRef> result;

    storage_.scan_alive([&](const RowRef& ref) {
        if (evaluate(condition, ref.row)) {
            result.push_back(ref);
        }
    });

    return result;
}

void Table::select(
    const std::vector<Projection>& projections,
    const Condition& condition
) {
    validate_projections(projections);

    const auto rows = matching(condition);
    std::vector<Projection> actual = projections;

    if (actual.empty()) {
        for (const auto& column : columns_) {
            actual.push_back({column.name, column.name});
        }
    }

    bool has_aggregate = false;

    for (const auto& projection : actual) {
        if (projection.aggregate != Projection::Aggregate::None) {
            has_aggregate = true;
            break;
        }
    }

    if (has_aggregate) {
        nlohmann::ordered_json object =
            nlohmann::ordered_json::object();

        for (const auto& projection : actual) {
            std::string field = projection.alias;

            if (field.empty()) {
                if (projection.aggregate ==
                    Projection::Aggregate::Count) {
                    field = projection.count_all
                        ? "COUNT(*)"
                        : "COUNT(" + projection.column + ")";
                } else if (projection.aggregate ==
                           Projection::Aggregate::Sum) {
                    field = "SUM(" + projection.column + ")";
                } else {
                    field = "AVG(" + projection.column + ")";
                }
            }

            if (projection.aggregate ==
                Projection::Aggregate::Count) {
                if (projection.count_all) {
                    object[field] = rows.size();
                } else {
                    const std::size_t pos =
                        column_index(projection.column);

                    std::size_t count = 0;

                    for (const auto& ref : rows) {
                        if (!std::holds_alternative<std::monostate>(ref.row.values[pos])) {
                            ++count;
                        }
                    }

                    object[field] = count;
                }

                continue;
            }

            const std::size_t pos = column_index(projection.column);

            long long sum = 0;
            std::size_t count = 0;

            for (const auto& ref : rows) {
                if (std::holds_alternative<std::monostate>(ref.row.values[pos])) {
                    continue;
                }

                sum += std::get<int>(ref.row.values[pos]);
                ++count;
            }

            if (projection.aggregate == Projection::Aggregate::Sum) {
                object[field] = sum;
            } else {
                object[field] = count == 0
                    ? nlohmann::ordered_json(nullptr)
                    : nlohmann::ordered_json(
                        static_cast<double>(sum) /
                        static_cast<double>(count)
                    );
            }
        }

        nlohmann::ordered_json result =
            nlohmann::ordered_json::array();

        result.push_back(std::move(object));
        std::cout << result.dump() << '\n';
        return;
    }

    nlohmann::ordered_json result = nlohmann::ordered_json::array();

    for (const auto& ref : rows) {
        nlohmann::ordered_json object = nlohmann::ordered_json::object();

        for (const auto& projection : actual) {
            const std::size_t pos = column_index(projection.column);

            const std::string field = projection.alias.empty()
                ? projection.column
                : projection.alias;

            object[field] = value_to_json(ref.row.values[pos]);
        }

        result.push_back(std::move(object));
    }

    std::cout << result.dump() << '\n';
}

std::size_t Table::erase_where(const Condition& condition) {
    const auto targets = matching(condition);

    if (targets.empty()) {
        return 0;
    }

    std::vector<std::uint64_t> offsets;
    offsets.reserve(targets.size());

    for (const auto& ref : targets) {
        offsets.push_back(static_cast<std::uint64_t>(ref.offset));
    }

    mark_indexes_dirty();
    storage_.mark_deleted(offsets);

    for (const auto& ref : targets) {
        erase_row_indexes(ref.row);
    }

    clear_indexes_dirty();

    for (const auto& ref : targets) {
        append_history("DELETE", ref.row);
    }

    return targets.size();
}

std::size_t Table::update_where(const std::vector<std::pair<std::string, Token>>& assignments,
                                const Condition& condition) {
    std::set<std::string> assigned_columns;

    for (const auto& [name, token] : assignments) {
        if (!assigned_columns.insert(name).second) {
            throw DbError("Duplicate UPDATE assignment: " + name);
        }

        const std::size_t pos = column_index(name);

        const Value value = token_to_value(token, columns_[pos].type);

        if (std::holds_alternative<std::monostate>(value) &&
            (columns_[pos].not_null || columns_[pos].indexed)) {
            throw DbError("Column cannot be NULL: " + name);
        }
    }

    const auto targets = matching(condition);

    if (targets.empty()) {
        return 0;
    }

    std::set<std::size_t> target_ids;
    std::vector<Row> updated_rows;
    std::vector<std::uint64_t> old_offsets;

    updated_rows.reserve(targets.size());
    old_offsets.reserve(targets.size());

    for (const auto& ref : targets) {
        target_ids.insert(ref.row.id);
        old_offsets.push_back(static_cast<std::uint64_t>(ref.offset));

        Row updated = ref.row;
        updated.alive = true;

        for (const auto& [name, token] : assignments) {
            const std::size_t pos = column_index(name);

            updated.values[pos] = token_to_value(token, columns_[pos].type);
        }

        validate_row(updated);
        updated_rows.push_back(std::move(updated));
    }

    validate_new_index_values(updated_rows, target_ids);

    const std::uint64_t next_id = storage_.read_header().next_id;

    mark_indexes_dirty();
    storage_.mark_deleted(old_offsets);

    for (const auto& ref : targets) {
        erase_row_indexes(ref.row);
    }

    const auto new_refs = storage_.append_rows(updated_rows, next_id);

    for (const auto& ref : new_refs) {
        insert_row_indexes(ref.row, ref.offset);
    }

    clear_indexes_dirty();

    for (const auto& ref : targets) {
        append_history("UPDATE", ref.row);
    }

    return targets.size();
}

void Table::revert_to(const std::string& timestamp) {
    if (!valid_timestamp(timestamp)) {
        throw DbError("Invalid REVERT timestamp; expected yyyy.mm.dd-hh:mm:ss.msmsms");
    }

    std::ifstream in(dir_ / "history.log");

    if (!in) {
        return;
    }

    struct HistoryEntry {
        std::string timestamp;
        std::string operation;
        Row row;
        std::string raw_line;
    };

    std::vector<HistoryEntry> entries;
    std::string line;

    while (std::getline(in, line)) {
        std::stringstream stream(line);
        std::string entry_timestamp;
        std::string operation;
        std::string row_text;

        if (!std::getline(stream, entry_timestamp, '\t') ||
            !std::getline(stream, operation, '\t') ||
            !std::getline(stream, row_text)) {
            continue;
        }

        entries.push_back({
            entry_timestamp,
            operation,
            parse_row(row_text),
            line
        });
    }

    std::map<std::size_t, Row> state;

    for (const auto& row : storage_.alive_rows()) {
        state[row.id] = row;
    }

    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        if (it->timestamp <= timestamp) {
            continue;
        }

        if (it->operation == "INSERT") {
            state.erase(it->row.id);
        } else if (it->operation == "UPDATE" ||
                   it->operation == "DELETE") {
            Row restored = it->row;
            restored.alive = true;
            state[restored.id] = std::move(restored);
        }
    }

    std::vector<Row> restored_rows;
    restored_rows.reserve(state.size());

    for (auto& [_, row] : state) {
        restored_rows.push_back(std::move(row));
    }

    validate_complete_state(restored_rows);
    rewrite_all_rows_and_indexes(
        restored_rows,
        storage_.read_header().next_id
    );

    const fs::path temp = dir_ / "history.log.tmp";
    std::ofstream out(temp, std::ios::trunc);

    if (!out) {
        throw DbError("Cannot write history.log.tmp");
    }

    for (const auto& entry : entries) {
        if (entry.timestamp <= timestamp) {
            out << entry.raw_line << '\n';
        }
    }

    out.flush();

    if (!out) {
        throw DbError("Cannot flush history.log.tmp");
    }

    out.close();

    std::error_code error;
    fs::rename(temp, dir_ / "history.log", error);

    if (error) {
        fs::remove(temp);
        throw DbError(
            "Cannot replace history.log: " + error.message()
        );
    }
}

std::size_t Table::column_index(const std::string& name) const {
    const auto it = column_pos_.find(name);

    if (it == column_pos_.end()) {
        throw DbError("Unknown column: " + name);
    }

    return it->second;
}

Value Table::operand_value(const Operand& operand, const Row& row) const {
    if (operand.column) {
        return row.values[column_index(operand.name)];
    }

    if (operand.literal.kind == TokenKind::Number) {
        return std::stoi(operand.literal.text);
    }

    if (operand.literal.kind == TokenKind::String) {
        return intern_string(operand.literal.text);
    }

    if (upper(operand.literal.text) == "NULL") {
        if (!uniform_keyword_case(operand.literal.text)) {
            throw DbError(
                "Mixed case keyword is not allowed: " +
                operand.literal.text
            );
        }

        return std::monostate{};
    }

    throw DbError("Unknown literal: " + operand.literal.text);
}

Table::ExpressionType Table::operand_type(const Operand& operand) const {
    if (operand.column) {
        return columns_[column_index(operand.name)].type ==
                       ColumnType::Int
            ? ExpressionType::Int
            : ExpressionType::String;
    }

    if (operand.literal.kind == TokenKind::Number) {
        return ExpressionType::Int;
    }

    if (operand.literal.kind == TokenKind::String) {
        return ExpressionType::String;
    }

    if (is_null_literal(operand.literal)) {
        return ExpressionType::Null;
    }

    throw DbError("Unknown literal: " + operand.literal.text);
}

bool Table::comparable(ExpressionType lhs, ExpressionType rhs) {
    return lhs == ExpressionType::Null ||
           rhs == ExpressionType::Null ||
           lhs == rhs;
}

void Table::validate_condition(const Condition& condition) const {
    if (condition.kind == Condition::Kind::None) {
        return;
    }

    if (condition.kind == Condition::Kind::And ||
        condition.kind == Condition::Kind::Or) {
        if (!condition.lhs || !condition.rhs) {
            throw DbError("Invalid logical condition");
        }

        validate_condition(*condition.lhs);
        validate_condition(*condition.rhs);
        return;
    }

    const ExpressionType left = operand_type(condition.left);

    if (condition.kind == Condition::Kind::Like) {
        const ExpressionType pattern = operand_type(condition.right);

        if (left != ExpressionType::String) {
            throw DbError("LIKE requires string value");
        }

        if (pattern != ExpressionType::String) {
            throw DbError("LIKE pattern must be string");
        }

        if (!condition.right.column && condition.right.literal.kind == TokenKind::String) {
            try {
                (void)std::regex(condition.right.literal.text);
            } catch (const std::regex_error& ex) {
                throw DbError(std::string("Invalid LIKE regular expression: ") + ex.what());
            }
        }

        return;
    }

    const ExpressionType right = operand_type(condition.right);

    if (!comparable(left, right)) {
        throw DbError("Cannot compare values of different types");
    }

    if (condition.kind == Condition::Kind::Between) {
        const ExpressionType high = operand_type(condition.high);

        if (!comparable(left, high) || !comparable(right, high)) {
            throw DbError("Cannot compare values of different types");
        }
    }
}

void Table::validate_projections(const std::vector<Projection>& projections) const {
    bool has_aggregate = false;
    bool has_regular = false;

    for (const auto& projection : projections) {
        if (projection.aggregate == Projection::Aggregate::None) {
            has_regular = true;
            (void)column_index(projection.column);
            continue;
        }

        has_aggregate = true;

        if (projection.aggregate == Projection::Aggregate::Count && projection.count_all) {
            continue;
        }

        const std::size_t pos = column_index(projection.column);

        if ((projection.aggregate == Projection::Aggregate::Sum ||
             projection.aggregate == Projection::Aggregate::Avg) &&
            columns_[pos].type != ColumnType::Int) {
            throw DbError("SUM and AVG require int column: " + projection.column);
        }
    }

    if (has_aggregate && has_regular) {
        throw DbError("Cannot mix aggregate and regular SELECT projections");
    }
}

void Table::validate_row(const Row& row) const {
    if (row.values.size() != columns_.size()) {
        throw DbError("Row does not match table schema");
    }

    for (std::size_t i = 0; i < columns_.size(); ++i) {
        const Column& column = columns_[i];
        const Value& value = row.values[i];

        if (std::holds_alternative<std::monostate>(value)) {
            if (column.not_null || column.indexed) {
                throw DbError("Column cannot be NULL: " + column.name);
            }

            continue;
        }

        if (column.type == ColumnType::Int && !std::holds_alternative<int>(value)) {
            throw DbError("Expected int value for column: " + column.name);
        }

        if (column.type == ColumnType::String && !std::holds_alternative<InternedString>(value)) {
            throw DbError("Expected string value for column: " + column.name);
        }
    }
}

void Table::validate_complete_state(const std::vector<Row>& rows) const {
    std::set<std::size_t> row_ids;
    std::map<std::string, std::set<int>> integer_keys;
    std::map<std::string, std::set<std::string>> string_keys;

    for (const auto& row : rows) {
        if (!row_ids.insert(row.id).second) {
            throw DbError("Duplicate internal row id");
        }

        validate_row(row);

        for (std::size_t i = 0; i < columns_.size(); ++i) {
            const Column& column = columns_[i];

            if (!column.indexed) {
                continue;
            }

            const Value& value = row.values[i];
            bool inserted = false;

            if (column.type == ColumnType::Int) {
                inserted = integer_keys[column.name].insert(std::get<int>(value)).second;
            } else {
                inserted = string_keys[column.name].insert(*std::get<InternedString>(value)).second;
            }

            if (!inserted) {
                throw DbError("Duplicate value for INDEXED column: " + column.name);
            }
        }
    }
}

void Table::validate_new_index_values(const std::vector<Row>& rows,
                                const std::set<std::size_t>& replaced_row_ids) const {
    std::map<std::string, std::set<int>> integer_keys;
    std::map<std::string, std::set<std::string>> string_keys;

    for (const auto& row : rows) {
        for (std::size_t i = 0; i < columns_.size(); ++i) {
            const Column& column = columns_[i];

            if (!column.indexed) {
                continue;
            }

            const Value& value = row.values[i];
            bool inserted = false;

            if (column.type == ColumnType::Int) {
                inserted = integer_keys[column.name].insert(std::get<int>(value)).second;
            } else {
                inserted = string_keys[column.name].insert(*std::get<InternedString>(value)).second;
            }

            if (!inserted) {
                throw DbError("Duplicate value for INDEXED column: " +column.name);
            }

            const auto offset = indexed_row_offset(column, value);

            if (!offset) {
                continue;
            }

            const auto existing = storage_.row_at_offset(*offset);

            if (!existing || !replaced_row_ids.contains(existing->row.id)) {
                throw DbError("Duplicate value for INDEXED column: " + column.name);
            }
        }
    }
}

void Table::rewrite_all_rows_and_indexes(const std::vector<Row>& rows, std::uint64_t next_id) {
    mark_indexes_dirty();
    storage_.write_rows_atomically(rows, next_id);
    rebuild_indexes();
    clear_indexes_dirty();
}

std::string Table::serialize_row(const Row& row) const {
    std::ostringstream out;

    out << row.id << '\t' << (row.alive ? "1" : "0");

    for (const auto& value : row.values) {
        out << '\t' << value_to_storage(value);
    }

    return out.str();
}

Row Table::parse_row(const std::string& line) {
    std::vector<std::string> parts;
    std::stringstream stream(line);
    std::string part;

    while (std::getline(stream, part, '\t')) {
        parts.push_back(part);
    }

    if (parts.size() != columns_.size() + 2) {
        throw DbError("Corrupted row in history.log");
    }

    Row row;
    row.id = static_cast<std::size_t>(std::stoull(parts[0]));
    row.alive = parts[1] == "1";

    for (std::size_t i = 2; i < parts.size(); ++i) {
        row.values.push_back(value_from_storage(parts[i]));
    }

    return row;
}

void Table::append_history(const std::string& operation, const Row& row
) const {
    std::ofstream out(dir_ / "history.log", std::ios::app);

    if (!out) {
        throw DbError("Cannot write history.log");
    }

    out << current_timestamp()
        << '\t'
        << operation
        << '\t'
        << serialize_row(row)
        << '\n';

    out.flush();

    if (!out) {
        throw DbError("Cannot flush history.log");
    }
}

bool Table::has_indexes() const noexcept {
    return !indexes_.empty();
}

fs::path Table::index_dirty_path() const {
    return dir_ / "indexes" / ".dirty";
}

void Table::mark_indexes_dirty() const {
    if (!has_indexes()) {
        return;
    }

    fs::create_directories(index_dirty_path().parent_path());

    std::ofstream out(index_dirty_path(), std::ios::trunc);

    if (!out) {
        throw DbError("Cannot create index recovery marker");
    }

    out << "Index rebuild is required after interrupted DML\n";
    out.flush();

    if (!out) {
        throw DbError("Cannot flush index recovery marker");
    }
}

void Table::clear_indexes_dirty() const {
    if (!has_indexes()) {
        return;
    }

    std::error_code error;
    fs::remove(index_dirty_path(), error);

    if (error) {
        throw DbError("Cannot remove index recovery marker: " + error.message());
    }
}

void Table::rebuild_indexes() {
    for (auto& [_, index] : indexes_) {
        if (index.type == ColumnType::Int) {
            index.ints->clear();
        } else {
            index.strings->clear();
        }
    }

    storage_.scan_alive([&](const RowRef& ref) {
        insert_row_indexes(ref.row, ref.offset);
    });
}

void Table::insert_row_indexes(const Row& row, std::streamoff offset) {
    if (!row.alive) {
        return;
    }

    for (const auto& column : columns_) {
        if (!column.indexed) {
            continue;
        }

        const Value& value = row.values[column_index(column.name)];

        if (std::holds_alternative<std::monostate>(value)) {
            throw DbError("Indexed column contains NULL");
        }

        Index& index = indexes_.at(column.name);

        const auto stored_offset = static_cast<std::uint64_t>(offset);

        bool inserted = false;

        if (column.type == ColumnType::Int) {
            inserted = index.ints->insert({std::get<int>(value), stored_offset}).second;
        } else {
            inserted = index.strings->insert({*std::get<InternedString>(value), stored_offset}).second;
        }

        if (!inserted) {
            throw DbError("Duplicate value for INDEXED column: " + column.name);
        }
    }
}

void Table::erase_row_indexes(const Row& row) {
    if (!row.alive) {
        return;
    }

    for (const auto& column : columns_) {
        if (!column.indexed) {
            continue;
        }

        const Value& value = row.values[column_index(column.name)];

        if (std::holds_alternative<std::monostate>(value)) {
            continue;
        }

        Index& index = indexes_.at(column.name);

        if (column.type == ColumnType::Int) {
            index.ints->erase(std::get<int>(value));
        } else {
            index.strings->erase(*std::get<InternedString>(value));
        }
    }
}

std::optional<std::uint64_t> Table::indexed_row_offset(const Column& column, const Value& value) const {
    if (!column.indexed ||
        std::holds_alternative<std::monostate>(value)) {
        return std::nullopt;
    }

    const auto index_it = indexes_.find(column.name);

    if (index_it == indexes_.end()) {
        return std::nullopt;
    }

    if (column.type == ColumnType::Int) {
        auto it = index_it->second.ints->find(std::get<int>(value));

        if (it != index_it->second.ints->end()) {
            return it->second;
        }
    } else {
        auto it = index_it->second.strings->find(*std::get<InternedString>(value));

        if (it != index_it->second.strings->end()) {
            return it->second;
        }
    }

    return std::nullopt;
}

int Table::compare_values(const Value& lhs, const Value& rhs) {
    if (std::holds_alternative<std::monostate>(lhs) ||
        std::holds_alternative<std::monostate>(rhs)) {
        if (std::holds_alternative<std::monostate>(lhs) &&
            std::holds_alternative<std::monostate>(rhs)) {
            return 0;
        }

        return std::holds_alternative<std::monostate>(lhs)
            ? -1
            : 1;
    }

    if (lhs.index() != rhs.index()) {
        throw DbError("Cannot compare values of different types");
    }

    if (std::holds_alternative<int>(lhs)) {
        const int left = std::get<int>(lhs);
        const int right = std::get<int>(rhs);

        return (left > right) - (left < right);
    }

    const std::string& left = *std::get<InternedString>(lhs);

    const std::string& right = *std::get<InternedString>(rhs);

    return (left > right) - (left < right);
}

bool Table::evaluate(const Condition& condition, const Row& row) const {
    if (condition.kind == Condition::Kind::None) {
        return true;
    }

    if (condition.kind == Condition::Kind::And) {
        return evaluate(*condition.lhs, row) &&
               evaluate(*condition.rhs, row);
    }

    if (condition.kind == Condition::Kind::Or) {
        return evaluate(*condition.lhs, row) ||
               evaluate(*condition.rhs, row);
    }

    const Value left = operand_value(condition.left, row);

    if (condition.kind == Condition::Kind::Like) {
        if (std::holds_alternative<std::monostate>(left)) {
            return false;
        }

        const Value pattern = operand_value(condition.right, row);

        if (std::holds_alternative<std::monostate>(pattern)) {
            return false;
        }

        return std::regex_match(
            *std::get<InternedString>(left),
            std::regex(*std::get<InternedString>(pattern))
        );
    }

    if (condition.kind == Condition::Kind::Between) {
        const Value low = operand_value(condition.right, row);
        const Value high = operand_value(condition.high, row);

        if (std::holds_alternative<std::monostate>(left) ||
            std::holds_alternative<std::monostate>(low) ||
            std::holds_alternative<std::monostate>(high)) {
            return false;
        }

        return compare_values(left, low) >= 0 &&
               compare_values(left, high) < 0;
    }

    const Value right = operand_value(condition.right, row);
    const int comparison = compare_values(left, right);

    if (condition.op == "==") {
        return comparison == 0;
    }

    if (condition.op == "!=") {
        return comparison != 0;
    }

    if (condition.op == "<") {
        return comparison < 0;
    }

    if (condition.op == ">") {
        return comparison > 0;
    }

    if (condition.op == "<=") {
        return comparison <= 0;
    }

    if (condition.op == ">=") {
        return comparison >= 0;
    }

    throw DbError("Unknown comparison operator");
}

template <typename Tree, typename Key>
std::vector<RowRef> Table::collect_index_range(
    Tree& tree,
    const Key& low,
    bool has_low,
    bool low_strict,
    const Key& high,
    bool has_high,
    bool high_inclusive
) {
    std::vector<RowRef> result;

    auto it = has_low
        ? (low_strict ? tree.upper_bound(low) : tree.lower_bound(low))
        : tree.begin();

    for (; it != tree.end(); ++it) {
        if (has_high) {
            const bool past = high_inclusive
                ? high < it->first
                : !(it->first < high);

            if (past) {
                break;
            }
        }

        if (auto row = storage_.row_at_offset(it->second)) {
            result.push_back(*row);
        }
    }

    return result;
}

std::optional<std::vector<RowRef>> Table::indexed_lookup(const Condition& condition) {
    if (condition.kind == Condition::Kind::And) {
        auto candidates = indexed_lookup(*condition.lhs);

        if (!candidates) {
            candidates = indexed_lookup(*condition.rhs);
        }

        if (!candidates) {
            return std::nullopt;
        }

        std::vector<RowRef> filtered_candidates;
        filtered_candidates.reserve(candidates->size());

        for (const RowRef& ref : *candidates) {
            if (evaluate(condition, ref.row)) {
                filtered_candidates.push_back(ref);
            }
        }

        return filtered_candidates;
    }

    if (condition.kind == Condition::Kind::Or) {
        return std::nullopt;
    }

    if (condition.kind == Condition::Kind::Compare &&
        !condition.left.column &&
        condition.right.column) {
        Condition reversed = condition;

        reversed.left = condition.right;
        reversed.right = condition.left;

        if (condition.op == "<") {
            reversed.op = ">";
        } else if (condition.op == ">") {
            reversed.op = "<";
        } else if (condition.op == "<=") {
            reversed.op = ">=";
        } else if (condition.op == ">=") {
            reversed.op = "<=";
        }

        return indexed_lookup(reversed);
    }

    if (!condition.left.column) {
        return std::nullopt;
    }

    const auto index = indexes_.find(condition.left.name);

    if (index == indexes_.end()) {
        return std::nullopt;
    }

    const std::size_t column = column_index(condition.left.name);

    if (condition.kind == Condition::Kind::Compare) {
        if (condition.right.column || condition.op == "!=") {
            return std::nullopt;
        }

        const Value value = token_to_value(condition.right.literal, columns_[column].type);

        if (std::holds_alternative<std::monostate>(value)) {
            return std::vector<RowRef>{};
        }

        if (condition.op == "==") {
            const auto row_offset = indexed_row_offset(columns_[column], value);

            if (!row_offset) {
                return std::vector<RowRef>{};
            }

            if (auto row = storage_.row_at_offset(*row_offset)) {
                return std::vector<RowRef>{*row};
            }

            return std::vector<RowRef>{};
        }

        if (columns_[column].type == ColumnType::Int) {
            const int key = std::get<int>(value);

            if (condition.op == ">") {
                return collect_index_range(
                    *index->second.ints,
                    key, true, true,
                    key, false, false
                );
            }

            if (condition.op == ">=") {
                return collect_index_range(
                    *index->second.ints,
                    key, true, false,
                    key, false, false
                );
            }

            if (condition.op == "<") {
                return collect_index_range(
                    *index->second.ints,
                    key, false, false,
                    key, true, false
                );
            }

            if (condition.op == "<=") {
                return collect_index_range(
                    *index->second.ints,
                    key, false, false,
                    key, true, true
                );
            }
        } else {
            const std::string& key = *std::get<InternedString>(value);

            if (condition.op == ">") {
                return collect_index_range(
                    *index->second.strings,
                    key, true, true,
                    key, false, false
                );
            }

            if (condition.op == ">=") {
                return collect_index_range(
                    *index->second.strings,
                    key, true, false,
                    key, false, false
                );
            }

            if (condition.op == "<") {
                return collect_index_range(
                    *index->second.strings,
                    key, false, false,
                    key, true, false
                );
            }

            if (condition.op == "<=") {
                return collect_index_range(
                    *index->second.strings,
                    key, false, false,
                    key, true, true
                );
            }
        }
    }

    if (condition.kind == Condition::Kind::Between) {
        if (condition.right.column || condition.high.column) {
            return std::nullopt;
        }

        const Value low = token_to_value(condition.right.literal, columns_[column].type);

        const Value high = token_to_value(condition.high.literal, columns_[column].type);

        if (std::holds_alternative<std::monostate>(low) ||
            std::holds_alternative<std::monostate>(high)) {
            return std::vector<RowRef>{};
        }

        if (columns_[column].type == ColumnType::Int) {
            return collect_index_range(
                *index->second.ints,
                std::get<int>(low), true, false,
                std::get<int>(high), true, false
            );
        }

        return collect_index_range(
            *index->second.strings,
            *std::get<InternedString>(low), true, false,
            *std::get<InternedString>(high), true, false
        );
    }

    return std::nullopt;
}

}
