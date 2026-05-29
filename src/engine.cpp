#include "engine.h"

#include "db_error.h"
#include "db_util.h"
#include "sql_parser_api.h"
#include "table.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace course_dbms {

namespace fs = std::filesystem;

Engine::Engine(fs::path root, std::string client_id)
        : root_(std::move(root)), client_id_(std::move(client_id)) {
    fs::create_directories(root_);
}

void Engine::execute(const std::string& sql) {
    if (sql.find_first_not_of(" \t\r\n") == std::string::npos) {
        return;
    }

    const auto started = std::chrono::system_clock::now();
    const std::string started_text = current_timestamp();
    std::string status = "OK";

    try {
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

void Engine::create_database(const std::string& name) {
    check_name(name);

    const fs::path dir = root_ / name;

    if (fs::exists(dir)) {
        throw DbError("Database already exists: " + name);
    }

    fs::create_directories(dir);
    std::cout << "OK\n";
}

void Engine::drop_database(const std::string& name) {
    check_name(name);

    const fs::path dir = root_ / name;

    if (!fs::exists(dir)) {
        throw DbError("Database does not exist: " + name);
    }

    fs::remove_all(dir);

    if (current_db_ == name) {
        current_db_.reset();
    }

    std::cout << "OK\n";
}

void Engine::use_database(const std::string& name) {
    check_name(name);

    if (!fs::is_directory(root_ / name)) {
        throw DbError("Database does not exist: " + name);
    }

    current_db_ = name;
    std::cout << "OK\n";
}

void Engine::create_table(const Command& command) {
    const std::string table = command.table.table;
    const std::string db = resolve_db(command.table.database);

    check_database(db);
    check_name(table);

    const fs::path dir = table_dir(db, table);

    if (fs::exists(dir)) {
        throw DbError("Table already exists: " + table);
    }

    const auto& columns = command.columns;
    std::vector<std::string> seen_columns;

    for (const auto& column : columns) {
        check_name(column.name);

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

    if (columns.empty()) {
        throw DbError("Table must contain at least one column");
    }

    const fs::path temp_dir = dir.string() + ".creating";
    fs::remove_all(temp_dir);

    try {
        fs::create_directories(temp_dir);
        save_schema(temp_dir, columns);

        Table initialized(temp_dir, columns);

        std::error_code error;
        fs::rename(temp_dir, dir, error);

        if (error) {
            throw DbError("Cannot atomically create table: " + error.message());
        }
    } catch (...) {
        fs::remove_all(temp_dir);
        throw;
    }

    std::cout << "OK\n";
}

void Engine::drop_table(const std::string& db, const std::string& table) {
    const fs::path dir = table_dir(db, table);

    if (!fs::exists(dir)) {
        throw DbError("Table does not exist: " + table);
    }

    fs::remove_all(dir);
    std::cout << "OK\n";
}

void Engine::insert(const Command& command) {
    Table table = open_table(command.table.database, command.table.table);

    table.insert(command.insert_columns, command.insert_rows);

    std::cout << "OK " << command.insert_rows.size() << "\n";
}

void Engine::select(const Command& command) {
    Table table = open_table(command.table.database,command.table.table);

    const Condition condition = command.has_condition ? command.condition : Condition{};

    table.select(command.projections, condition);
}

void Engine::erase(const Command& command) {
    Table table = open_table(command.table.database, command.table.table);

    const std::size_t count = table.erase_where(command.condition);

    std::cout << "OK " << count << "\n";
}

void Engine::update(const Command& command) {
    Table table = open_table(command.table.database, command.table.table);

    const std::size_t count =table.update_where(command.assignments, command.condition);

    std::cout << "OK " << count << "\n";
}

void Engine::revert(const Command& command) {
    Table table = open_table(command.table.database, command.table.table);

    table.revert_to(command.timestamp);
    std::cout << "OK\n";
}

Table Engine::open_table(const std::optional<std::string>& explicit_db, const std::string& table) {
    const std::string db = explicit_db ? *explicit_db : require_current_db();

    const fs::path dir = table_dir(db, table);

    if (!fs::is_directory(dir)) {
        throw DbError("Table does not exist: " + table);
    }

    return Table(dir, load_schema(dir));
}

std::string Engine::resolve_db(const std::optional<std::string>& explicit_db) const {
    return explicit_db ? *explicit_db : require_current_db();
}

std::string Engine::require_current_db() const {
    if (!current_db_) {
        throw DbError("No active database. Use USE database_name or database.table");
    }

    return *current_db_;
}

void Engine::check_database(const std::string& db) const {
    if (!fs::is_directory(root_ / db)) {
        throw DbError("Database does not exist: " + db);
    }
}

fs::path Engine::table_dir(const std::string& db,
                           const std::string& table) const {
    return root_ / db / table;
}

void Engine::check_name(const std::string& name) {
    if (!valid_identifier(name)) {
        throw DbError("Invalid identifier: " + name);
    }
}

void Engine::save_schema(const fs::path& dir, const std::vector<Column>& columns) {
    std::ofstream out(dir / "schema.dat", std::ios::trunc);

    if (!out) {
        throw DbError("Cannot write schema.dat");
    }

    for (const auto& column : columns) {
        out << column.name << '\t'
            << (column.type == ColumnType::Int ? "int" : "string")
            << '\t'
            << column.not_null
            << '\t'
            << column.indexed
            << '\t'
            << column.has_default
            << '\t'
            << (column.has_default ? value_to_storage(column.default_value) : "N:")
            << '\n';
    }
}

std::vector<Column> Engine::load_schema(const fs::path& dir) {
    std::ifstream in(dir / "schema.dat");

    if (!in) {
        throw DbError("Cannot read schema.dat");
    }

    std::vector<Column> columns;
    std::string line;

    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }

        std::vector<std::string> parts;
        std::stringstream stream(line);
        std::string part;

        while (std::getline(stream, part, '\t')) {
            parts.push_back(part);
        }

        if (parts.size() < 4) {
            throw DbError("Corrupted schema.dat");
        }

        Column column;
        column.name = parts[0];

        if (parts[1] == "int") {
            column.type = ColumnType::Int;
        } else if (parts[1] == "string") {
            column.type = ColumnType::String;
        } else {
            throw DbError("Corrupted schema.dat: unknown column type");
        }

        column.not_null = parts[2] == "1";
        column.indexed = parts[3] == "1";

        if (parts.size() >= 6) {
            column.has_default = parts[4] == "1";

            if (column.has_default) {
                column.default_value = value_from_storage(parts[5]);
            }
        }

        columns.push_back(std::move(column));
    }

    return columns;
}

void Engine::log_access(
    const std::string& sql,
    const std::string& started_text,
    const std::chrono::system_clock::time_point& started,
    const std::string& status) const {

    const auto finished = std::chrono::system_clock::now();

    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(finished - started).count();

    std::ofstream out(root_ / "access.log", std::ios::app);

    if (!out) {
        return;
    }

    std::string body = sql;
    std::replace(body.begin(), body.end(), '\n', ' ');
    std::replace(body.begin(), body.end(), '\t', ' ');

    out << "start=" << started_text
        << "\tfinish=" << current_timestamp()
        << "\telapsed_ms=" << elapsed_ms
        << "\tclient=" << client_id_
        << "\thandler="
        << std::hash<std::thread::id>{}(std::this_thread::get_id())
        << "\tstatus=" << status
        << "\tbody=" << body
        << '\n';
}

}
