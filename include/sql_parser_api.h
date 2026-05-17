#ifndef COURSE_DBMS_SQL_PARSER_API_H
#define COURSE_DBMS_SQL_PARSER_API_H

#include <string>
#include <vector>

#include "db_ast.h"

std::vector<Token> lex_sql(const std::string& sql);
Command parse_sql_command(const std::string& sql);

#endif
