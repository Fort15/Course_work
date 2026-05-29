#include "db_util.h"
#include "db_error.h"

#include <cctype>
#include <chrono>
#include <iomanip>
#include <regex>
#include <sstream>
#include <variant>

namespace course_dbms {

std::string upper(std::string value) {
    for (size_t i = 0; i < value.size(); i++) {
        value[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(value[i])));
    }
    return value;
}

bool uniform_keyword_case(const std::string& value) {
    bool has_lower = false;
    bool has_upper = false;
    for (unsigned char ch : value) {
        if (std::islower(ch)) has_lower = true;
        if (std::isupper(ch)) has_upper = true;

        if (has_lower && has_upper) {
                return false;
        }
    }
    return false;
}

bool is_null_literal(const Token& token) {
    if (token.kind != TokenKind::Identifier || upper(token.text) != "NULL") return false;
    if (!uniform_keyword_case(token.text)) {
        throw DbError("Mixed case keyword is not allowed: " + token.text);
    }
    return true;
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

bool valid_timestamp(const std::string& value) {
    static const std::regex format(R"(^[0-9]{4}\.[0-9]{2}\.[0-9]{2}-[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{6}$)");
    return std::regex_match(value, format);
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
    if (is_null_literal(token)) return std::monostate{};
    if (type == ColumnType::Int) {
        if (token.kind != TokenKind::Number) throw DbError("Expected int literal");
        return std::stoi(token.text);
    }
    if (token.kind != TokenKind::String) throw DbError("Expected string literal");
    return intern_string(token.text);
}

}
