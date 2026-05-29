#ifndef COURSE_DBMS_ROW_STORAGE_H
#define COURSE_DBMS_ROW_STORAGE_H

#include "db_ast.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <istream>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace course_dbms {

struct Row {
    std::size_t id{};
    bool alive{true};
    std::vector<Value> values;
};

struct RowRef {
    Row row;
    std::streamoff offset{};
};

class RowStorage {
public:
    struct DataHeader {
        std::uint64_t next_id{1};
        std::uint64_t physical_records{0};
        std::uint64_t alive_records{0};
    };

    RowStorage(std::filesystem::path dir, const std::vector<Column>& columns);

    void ensure_data_file();
    DataHeader read_header() const;

    std::optional<RowRef> row_at_offset(std::uint64_t offset) const;
    void scan_alive(const std::function<void(const RowRef&)>& visitor) const;

    std::vector<RowRef> append_rows(const std::vector<Row>& rows, std::uint64_t next_id);

    void mark_deleted(const std::vector<std::uint64_t>& offsets);

    std::vector<Row> alive_rows() const;
    void write_rows_atomically(const std::vector<Row>& rows, std::uint64_t next_id) const;

private:
    static constexpr std::uint32_t rows_magic = 0x43524442u;
    static constexpr std::streamoff header_size =
        static_cast<std::streamoff>(sizeof(std::uint32_t) + sizeof(std::uint64_t) * 3);

    std::filesystem::path data_path() const;

    void initialize_data_file() const;

    void write_header(std::fstream& io, const DataHeader& header) const;
    std::streamoff committed_end_offset(const DataHeader& header) const;
    void trim_uncommitted_tail();

    std::optional<RowRef> read_next_record(std::ifstream& in, std::streamoff offset) const;

    std::string serialize_payload(const Row& row) const;

    template <typename T>
    static void write_plain(std::ostream& out, const T& value);

    template <typename T>
    static T read_plain(std::istream& in);

    static void write_storage_value(
        std::ostream& out,
        const Value& value,
        ColumnType type
    );

    static Value read_storage_value(
        std::istream& in,
        ColumnType type
    );

    std::filesystem::path dir_;
    const std::vector<Column>& columns_;
};

}

#endif
