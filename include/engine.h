#ifndef COURSE_DBMS_ENGINE_H
#define COURSE_DBMS_ENGINE_H

#include "db_ast.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace course_dbms {

class Table;

class Engine {
public:
    explicit Engine(std::filesystem::path root, std::string client_id = "local");

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    Engine(Engine&&) noexcept = default;
    Engine& operator=(Engine&&) noexcept = default;

    ~Engine() = default;

    void execute(const std::string& sql);

private:
    void create_database(const std::string& name);
    void drop_database(const std::string& name);
    void use_database(const std::string& name);

    void create_table(const Command& command);
    void drop_table(const std::string& db, const std::string& table);

    void insert(const Command& command);
    void select(const Command& command);
    void erase(const Command& command);
    void update(const Command& command);
    void revert(const Command& command);

    Table open_table(const std::optional<std::string>& explicit_db,const std::string& table);

    std::string resolve_db(const std::optional<std::string>& explicit_db) const;

    std::string require_current_db() const;

    void check_database(const std::string& db) const;

    std::filesystem::path table_dir(const std::string& db, const std::string& table) const;

    static void check_name(const std::string& name);

    static void save_schema(const std::filesystem::path& dir, const std::vector<Column>& columns);

    static std::vector<Column> load_schema(const std::filesystem::path& dir);

    void log_access(
        const std::string& sql,
        const std::string& started_text,
        const std::chrono::system_clock::time_point& started,
        const std::string& status
    ) const;

    std::filesystem::path root_;
    std::optional<std::string> current_db_;
    std::string client_id_;
};

}

#endif
