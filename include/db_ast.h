#ifndef COURSE_DBMS_DB_AST_H
#define COURSE_DBMS_DB_AST_H

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

enum class TokenKind { Identifier, Number, String, Symbol, End };

struct Token {
    TokenKind kind{};
    std::string text;
};

enum class ColumnType { Int, String };

using InternedString = std::shared_ptr<const std::string>;

inline InternedString intern_string(const std::string& value) {
    static std::mutex mutex;
    static std::unordered_map<std::string, std::weak_ptr<const std::string>> pool;

    std::lock_guard<std::mutex> lock(mutex);
    auto it = pool.find(value);
    if (it != pool.end()) {
        if (auto existing = it->second.lock()) return existing;
    }

    auto stored = std::make_shared<const std::string>(value);
    pool[*stored] = stored;
    return stored;
}

using Value = std::variant<std::monostate, int, InternedString>;

struct Column {
    std::string name;
    ColumnType type{};
    bool not_null{false};
    bool indexed{false};
    bool has_default{false};
    Value default_value;
};

struct Operand {
    bool column{false};
    std::string name;
    Token literal;
};

struct Condition {
    enum class Kind { None, Compare, Between, Like, And, Or } kind{Kind::None};
    Operand left;
    std::string op;
    Operand right;
    Operand high;
    std::shared_ptr<Condition> lhs;
    std::shared_ptr<Condition> rhs;
};

struct Projection {
    enum class Aggregate { None, Count, Sum, Avg };

    std::string column;
    std::string alias;
    Aggregate aggregate{Aggregate::None};
    bool count_all{false};
};

struct TableRef {
    std::optional<std::string> database;
    std::string table;
};

struct Command {
    enum class Kind {
        CreateDatabase,
        DropDatabase,
        UseDatabase,
        CreateTable,
        DropTable,
        Insert,
        Select,
        Delete,
        Update,
        Revert
    };

    Kind kind{};
    std::string database_name;
    TableRef table;
    std::vector<Column> columns;
    std::vector<std::string> insert_columns;
    std::vector<std::vector<Token>> insert_rows;
    std::vector<Projection> projections;
    Condition condition;
    bool has_condition{false};
    std::vector<std::pair<std::string, Token>> assignments;
    std::string timestamp;
};

struct ParseContext {
    std::vector<Token> tokens;
    size_t position{0};
    Command result;
    std::string error;
};

#endif
