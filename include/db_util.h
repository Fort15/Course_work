#ifndef COURSE_DBMS_DB_UTIL_H
#define COURSE_DBMS_DB_UTIL_H

#include <string>
#include <nlohmann/json.hpp>
#include "db_ast.h"

namespace course_dbms {

std::string upper(std::string value);
bool uniform_keyword_case(const std::string& value);
bool is_null_literal(const Token& token);
bool valid_identifier(const std::string& value);
std::string current_timestamp();
bool valid_timestamp(const std::string& value);
std::string value_to_storage(const Value& value);
Value value_from_storage(const std::string& text);
nlohmann::ordered_json value_to_json(const Value& value);
Value token_to_value(const Token& token, ColumnType type);

}

#endif
