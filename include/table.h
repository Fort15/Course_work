#ifndef COURSE_DBMS_TABLE_H
#define COURSE_DBMS_TABLE_H

#include "b_plus_tree.h"
#include "db_ast.h"
#include "row_storage.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <ios>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace course_dbms {

class Table {
public:
    Table(std::filesystem::path dir, std::vector<Column> columns);
    ~Table();

    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;
    Table(Table&&) = delete;
    Table& operator=(Table&&) = delete;

    const std::vector<Column>& columns() const;

    void insert(const std::vector<std::string>& names,
                const std::vector<std::vector<Token>>& raw_rows);

    void select(const std::vector<Projection>& projections,
                const Condition& condition);

    std::size_t erase_where(const Condition& condition);

    std::size_t update_where(const std::vector<std::pair<std::string, Token>>& assignments,
                            const Condition& condition);

    void revert_to(const std::string& timestamp);

private:
    using IntegerIndexTree = BP_tree<int, std::uint64_t>;
    using StringIndexTree = BP_tree<std::string, std::uint64_t>;

    struct Index {
        ColumnType type{};
        std::unique_ptr<IntegerIndexTree> ints;
        std::unique_ptr<StringIndexTree> strings;

        Index(ColumnType column_type, const std::filesystem::path& dir);

        Index(Index&&) noexcept;
        Index& operator=(Index&&) noexcept;

        Index(const Index&) = delete;
        Index& operator=(const Index&) = delete;
    };

    enum class ExpressionType { Null, Int, String };

    std::vector<RowRef> matching(const Condition& condition);

    std::size_t column_index(const std::string& name) const;

    Value operand_value(const Operand& operand, const Row& row) const;

    ExpressionType operand_type(const Operand& operand) const;

    static bool comparable(ExpressionType lhs, ExpressionType rhs);

    static int compare_values(const Value& lhs, const Value& rhs);

    void validate_condition(const Condition& condition) const;

    void validate_projections(const std::vector<Projection>& projections) const;

    void validate_row(const Row& row) const;

    void validate_complete_state(const std::vector<Row>& rows) const;

    void validate_new_index_values(const std::vector<Row>& rows, const std::set<std::size_t>& replaced_row_ids) const;

    void rewrite_all_rows_and_indexes(const std::vector<Row>& rows, std::uint64_t next_id);

    std::string serialize_row(const Row& row) const;
    Row parse_row(const std::string& line);

    void append_history(const std::string& operation, const Row& row) const;

    bool has_indexes() const noexcept;
    std::filesystem::path index_dirty_path() const;
    void mark_indexes_dirty() const;
    void clear_indexes_dirty() const;
    void rebuild_indexes();

    void insert_row_indexes(const Row& row, std::streamoff offset);

    void erase_row_indexes(const Row& row);

    std::optional<std::uint64_t> indexed_row_offset(const Column& column, const Value& value) const;

    bool evaluate(const Condition& condition, const Row& row) const;

    template <typename Tree, typename Key>
    std::vector<RowRef> collect_index_range(
        Tree& tree,
        const Key& low,
        bool has_low,
        bool low_strict,
        const Key& high,
        bool has_high,
        bool high_inclusive
    );

    std::optional<std::vector<RowRef>> indexed_lookup(const Condition& condition);

    std::filesystem::path dir_;
    std::vector<Column> columns_;
    RowStorage storage_;
    std::unordered_map<std::string, std::size_t> column_pos_;
    std::map<std::string, Index> indexes_;
};

}

#endif
