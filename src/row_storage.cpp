#include "row_storage.h"

#include "db_error.h"

#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace course_dbms {

namespace fs = std::filesystem;

RowStorage::RowStorage(fs::path dir, const std::vector<Column>& columns)
    : dir_(std::move(dir)), columns_(columns) {}

fs::path RowStorage::data_path() const {
    return dir_ / "rows.dat";
}

template <typename T>
void RowStorage::write_plain(std::ostream& out, const T& value) {
    static_assert(
        std::is_trivially_copyable_v<T>,
        "Binary storage field must be trivially copyable"
    );

    out.write(reinterpret_cast<const char*>(&value), static_cast<std::streamsize>(sizeof(T)));

    if (!out) {
        throw DbError("Cannot write rows.dat");
    }
}

template <typename T>
T RowStorage::read_plain(std::istream& in) {
    static_assert(
        std::is_trivially_copyable_v<T>,
        "Binary storage field must be trivially copyable"
    );

    T value{};

    in.read(reinterpret_cast<char*>(&value), static_cast<std::streamsize>(sizeof(T)));

    if (!in) {
        throw DbError("Corrupted rows.dat");
    }

    return value;
}

void RowStorage::write_storage_value(std::ostream& out, const Value& value, ColumnType type) {
    const auto is_null = static_cast<std::uint8_t>(std::holds_alternative<std::monostate>(value) ? 1 : 0);

    write_plain(out, is_null);

    if (is_null != 0) {
        return;
    }

    if (type == ColumnType::Int) {
        if (!std::holds_alternative<int>(value)) {
            throw DbError("Value type mismatch while writing row");
        }

        write_plain(out, static_cast<std::int32_t>(std::get<int>(value)));

        return;
    }

    if (!std::holds_alternative<InternedString>(value)) {
        throw DbError("Value type mismatch while writing row");
    }

    const std::string& text = *std::get<InternedString>(value);

    write_plain(out, static_cast<std::uint64_t>(text.size()));

    out.write(text.data(), static_cast<std::streamsize>(text.size()));

    if (!out) {
        throw DbError("Cannot write string value to rows.dat");
    }
}

Value RowStorage::read_storage_value(std::istream& in, ColumnType type) {
    const auto is_null = read_plain<std::uint8_t>(in);

    if (is_null != 0) {
        return std::monostate{};
    }

    if (type == ColumnType::Int) {
        return static_cast<int>(read_plain<std::int32_t>(in));
    }

    const auto size = read_plain<std::uint64_t>(in);

    std::string text(size, '\0');

    if (size != 0) {
        in.read(text.data(), static_cast<std::streamsize>(size));
    }

    if (!in) {
        throw DbError("Corrupted string value in rows.dat");
    }

    return intern_string(text);
}

std::string RowStorage::serialize_payload(const Row& row) const {
    if (row.values.size() != columns_.size()) {
        throw DbError("Row does not match table schema");
    }

    std::ostringstream payload(std::ios::out | std::ios::binary);

    for (std::size_t i = 0; i < columns_.size(); ++i) {
        write_storage_value(payload, row.values[i], columns_[i].type);
    }

    return payload.str();
}

RowStorage::DataHeader RowStorage::read_header() const {
    std::ifstream in(data_path(), std::ios::binary);

    if (!in) {
        throw DbError("Cannot read rows.dat");
    }

    const auto magic = read_plain<std::uint32_t>(in);

    if (magic != rows_magic) {
        throw DbError("Corrupted rows.dat header");
    }

    DataHeader header;

    header.next_id = read_plain<std::uint64_t>(in);

    header.physical_records = read_plain<std::uint64_t>(in);

    header.alive_records = read_plain<std::uint64_t>(in);

    if (header.alive_records > header.physical_records) {
        throw DbError("Corrupted rows.dat counters");
    }

    return header;
}

void RowStorage::write_header(std::fstream& io, const DataHeader& header) const {
    io.seekp(0);

    if (!io) {
        throw DbError("Cannot seek rows.dat header");
    }

    write_plain(io, rows_magic);
    write_plain(io, header.next_id);
    write_plain(io, header.physical_records);
    write_plain(io, header.alive_records);
}

void RowStorage::initialize_data_file() const {
    fs::create_directories(dir_);

    std::ofstream out(data_path(), std::ios::binary | std::ios::trunc);

    if (!out) {
        throw DbError("Cannot create rows.dat");
    }

    write_plain(out, rows_magic);

    write_plain(out, static_cast<std::uint64_t>(1));

    write_plain(out, static_cast<std::uint64_t>(0));

    write_plain(out, static_cast<std::uint64_t>(0));

    out.flush();

    if (!out) {
        throw DbError("Cannot initialize rows.dat");
    }
}

std::streamoff RowStorage::committed_end_offset(const DataHeader& header) const {
    std::ifstream in(data_path(), std::ios::binary);

    if (!in) {
        throw DbError("Cannot read rows.dat");
    }

    in.seekg(header_size);

    if (!in) {
        throw DbError("Cannot seek rows.dat records");
    }

    for (std::uint64_t i = 0; i < header.physical_records; ++i) {
        (void)read_plain<std::uint8_t>(in);
        (void)read_plain<std::uint64_t>(in);

        const auto payload_size = read_plain<std::uint64_t>(in);

        in.seekg(static_cast<std::streamoff>(payload_size), std::ios::cur);

        if (!in) {
            throw DbError("Corrupted row payload in rows.dat");
        }
    }

    return in.tellg();
}

void RowStorage::trim_uncommitted_tail() {
    const DataHeader header = read_header();

    const std::streamoff committed_end = committed_end_offset(header);

    if (committed_end < 0) {
        throw DbError("Cannot determine rows.dat committed size");
    }

    const auto real_size = static_cast<std::uintmax_t>(fs::file_size(data_path()));

    const auto committed_size = static_cast<std::uintmax_t>(committed_end);

    if (real_size > committed_size) {
        fs::resize_file(data_path(), committed_size);
    }
}

void RowStorage::ensure_data_file() {
    const fs::path temporary = data_path().string() + ".tmp";

    const fs::path backup = data_path().string() + ".bak";

    if (!fs::exists(data_path()) &&
        fs::exists(backup)) {
        std::error_code error;

        fs::rename(backup, data_path(), error);

        if (error) {
            throw DbError("Cannot restore rows.dat backup: " + error.message());
        }
    } else if (fs::exists(data_path()) &&
               fs::exists(backup)) {
        fs::remove(backup);
    }

    fs::remove(temporary);

    if (!fs::exists(data_path()) || fs::file_size(data_path()) == 0) {
        initialize_data_file();
        return;
    }

    (void)read_header();

    trim_uncommitted_tail();
}

std::optional<RowRef> RowStorage::read_next_record(std::ifstream& in, std::streamoff offset) const {
    const auto alive_flag = read_plain<std::uint8_t>(in);

    const auto row_id = read_plain<std::uint64_t>(in);

    const auto payload_size = read_plain<std::uint64_t>(in);

    std::string payload(payload_size, '\0');

    if (payload_size != 0) {
        in.read(payload.data(), static_cast<std::streamsize>(payload_size));
    }

    if (!in) {
        throw DbError("Corrupted row payload in rows.dat");
    }

    if (alive_flag == 0) {
        return std::nullopt;
    }

    std::istringstream payload_in(payload, std::ios::in | std::ios::binary);

    Row row;

    row.id = static_cast<std::size_t>(row_id);
    row.alive = true;
    row.values.reserve(columns_.size());

    for (const auto& column : columns_) {
        row.values.push_back(read_storage_value(payload_in, column.type));
    }

    return RowRef{std::move(row), offset};
}

std::optional<RowRef> RowStorage::row_at_offset(std::uint64_t offset) const {
    std::ifstream in(data_path(), std::ios::binary);

    if (!in) {
        throw DbError("Cannot read rows.dat");
    }

    if (offset < static_cast<std::uint64_t>(header_size)) {
        return std::nullopt;
    }

    in.seekg(static_cast<std::streamoff>(offset));

    if (!in) {
        return std::nullopt;
    }

    return read_next_record(in, static_cast<std::streamoff>(offset));
}

void RowStorage::scan_alive(const std::function<void(const RowRef&)>& visitor) const {
    std::ifstream in(data_path(), std::ios::binary);

    if (!in) {
        throw DbError("Cannot read rows.dat");
    }

    const DataHeader header = read_header();

    in.seekg(header_size);

    if (!in) {
        throw DbError("Cannot seek rows.dat records");
    }

    for (std::uint64_t i = 0; i < header.physical_records; ++i) {
        const std::streamoff offset = in.tellg();

        auto row = read_next_record(in, offset);

        if (row) {
            visitor(*row);
        }
    }
}

std::vector<RowRef> RowStorage::append_rows(const std::vector<Row>& rows, std::uint64_t next_id) {
    if (rows.empty()) {
        return {};
    }

    for (const auto& row : rows) {
        if (!row.alive) {
            throw DbError("Cannot append an inactive row version");
        }
    }

    const DataHeader old_header = read_header();

    const std::streamoff old_end = committed_end_offset(old_header);

    if (old_end < 0) {
        throw DbError("Cannot determine append position in rows.dat");
    }

    std::vector<RowRef> appended;

    appended.reserve(rows.size());

    try {
        fs::resize_file(data_path(), static_cast<std::uintmax_t>(old_end));

        std::fstream io(
            data_path(),
            std::ios::binary |
            std::ios::in |
            std::ios::out
        );

        if (!io) {
            throw DbError("Cannot append rows.dat");
        }

        io.seekp(old_end);

        if (!io) {
            throw DbError("Cannot seek append position in rows.dat");
        }

        for (const auto& row : rows) {
            const std::streamoff offset = io.tellp();

            const std::string payload =serialize_payload(row);

            write_plain(io, static_cast<std::uint8_t>(1));

            write_plain(io, static_cast<std::uint64_t>(row.id));

            write_plain(io, static_cast<std::uint64_t>(payload.size()));

            io.write(payload.data(), static_cast<std::streamsize>(payload.size()));

            if (!io) {
                throw DbError("Cannot append row payload to rows.dat");
            }

            appended.push_back(RowRef{row, offset});
        }

        io.flush();

        if (!io) {
            throw DbError("Cannot flush appended rows.dat");
        }

        DataHeader new_header = old_header;

        new_header.next_id = next_id;

        new_header.physical_records += static_cast<std::uint64_t>(rows.size());

        new_header.alive_records += static_cast<std::uint64_t>(rows.size());

        write_header(io, new_header);

        io.flush();

        if (!io) {
            throw DbError("Cannot flush rows.dat header");
        }
    } catch (...) {
        std::error_code ignored;

        fs::resize_file(
            data_path(),
            static_cast<std::uintmax_t>(old_end),
            ignored
        );

        try {
            std::fstream rollback(
                data_path(),
                std::ios::binary |
                std::ios::in |
                std::ios::out
            );

            if (rollback) {
                write_header(rollback, old_header);

                rollback.flush();
            }
        } catch (...) {
        }

        throw;
    }

    return appended;
}

void RowStorage::mark_deleted(const std::vector<std::uint64_t>& offsets) {
    if (offsets.empty()) {
        return;
    }

    std::set<std::uint64_t> unique_offsets(offsets.begin(), offsets.end());

    if (unique_offsets.size() != offsets.size()) {
        throw DbError("Duplicate row offset in delete operation");
    }

    for (const auto offset : offsets) {
        if (!row_at_offset(offset)) {
            throw DbError("Cannot delete missing or inactive row");
        }
    }

    const DataHeader old_header = read_header();

    if (old_header.alive_records < static_cast<std::uint64_t>(offsets.size())) {
        throw DbError("Corrupted rows.dat alive record count");
    }

    std::fstream io(
        data_path(),
        std::ios::binary |
        std::ios::in |
        std::ios::out
    );

    if (!io) {
        throw DbError("Cannot update rows.dat");
    }

    std::vector<std::uint64_t> marked;

    marked.reserve(offsets.size());

    try {
        for (const auto offset : offsets) {
            io.seekp(static_cast<std::streamoff>(offset));

            if (!io) {
                throw DbError("Cannot seek row for deletion");
            }

            write_plain(io, static_cast<std::uint8_t>(0));

            marked.push_back(offset);
        }

        io.flush();

        if (!io) {
            throw DbError("Cannot flush deleted row markers");
        }

        DataHeader new_header = old_header;

        new_header.alive_records -= static_cast<std::uint64_t>( offsets.size());

        write_header(io, new_header);

        io.flush();

        if (!io) {
            throw DbError("Cannot flush rows.dat header");
        }
    } catch (...) {
        try {
            for (const auto offset : marked) {
                io.clear();

                io.seekp(static_cast<std::streamoff>(offset));

                write_plain(io,static_cast<std::uint8_t>(1));
            }

            write_header(io, old_header);

            io.flush();
        } catch (...) {
        }

        throw;
    }
}

std::vector<Row> RowStorage::alive_rows() const {
    std::ifstream in(data_path(), std::ios::binary);

    if (!in) {
        throw DbError("Cannot read rows.dat");
    }

    const DataHeader header = read_header();

    std::vector<Row> rows;

    rows.reserve(static_cast<std::size_t>(header.alive_records));

    in.seekg(header_size);

    if (!in) {
        throw DbError("Cannot seek rows.dat records");
    }

    for (std::uint64_t i = 0; i < header.physical_records; ++i) {
        const std::streamoff offset = in.tellg();

        auto row = read_next_record(in, offset);

        if (row) {
            rows.push_back(std::move(row->row));
        }
    }

    return rows;
}

void RowStorage::write_rows_atomically(const std::vector<Row>& rows, std::uint64_t next_id
) const {
    fs::create_directories(dir_);

    const fs::path temporary = data_path().string() + ".tmp";

    const fs::path backup = data_path().string() + ".bak";

    fs::remove(temporary);

    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);

    if (!out) {
        throw DbError("Cannot create temporary rows.dat");
    }

    std::uint64_t alive_records = 0;

    for (const auto& row : rows) {
        if (row.alive) {
            ++alive_records;
        }
    }

    write_plain(out, rows_magic);
    write_plain(out, next_id);

    write_plain(out, static_cast<std::uint64_t>( rows.size()));

    write_plain(out, alive_records);

    for (const auto& row : rows) {
        const std::string payload = serialize_payload(row);

        write_plain(out, static_cast<std::uint8_t>(row.alive ? 1 : 0));

        write_plain(out, static_cast<std::uint64_t>(row.id));

        write_plain(out, static_cast<std::uint64_t>(payload.size()));

        out.write(payload.data(), static_cast<std::streamsize>(payload.size()));

        if (!out) {
            out.close();
            fs::remove(temporary);

            throw DbError("Cannot write temporary rows.dat");
        }
    }

    out.flush();

    if (!out) {
        out.close();
        fs::remove(temporary);

        throw DbError("Cannot flush temporary rows.dat");
    }

    out.close();

    std::error_code error;

    fs::rename(temporary, data_path(), error);

    if (!error) {
        return;
    }

    fs::remove(backup);

    std::error_code move_old_error;

    fs::rename(data_path(), backup, move_old_error);

    if (move_old_error) {
        fs::remove(temporary);

        throw DbError("Cannot replace rows.dat: " + move_old_error.message());
    }

    std::error_code publish_error;

    fs::rename(temporary, data_path(), publish_error);

    if (publish_error) {
        std::error_code rollback_error;

        fs::rename(backup,  data_path(), rollback_error);

        fs::remove(temporary);

        throw DbError("Cannot publish replacement rows.dat: " + publish_error.message()
        );
    }

    std::error_code ignored;

    fs::remove(backup, ignored);
}

}