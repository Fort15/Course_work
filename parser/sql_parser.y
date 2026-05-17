%skeleton "lalr1.cc"
%require "3.8"
%defines
%define api.namespace {sql}
%define api.parser.class {BisonParser}
%define api.value.type variant
%define api.token.constructor
%define parse.error verbose

%parse-param { ParseContext& ctx }
%lex-param { ParseContext& ctx }

%code requires {
    #include <stdexcept>
    #include <string>
    #include <utility>
    #include <vector>
    #include "db_ast.h"
}

%code {
    #include "sql_parser_api.h"

    static std::string upper_copy(std::string value) {
        for (char& ch : value) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        return value;
    }

    static bool keyword(const Token& token, const std::string& expected) {
        return token.kind == TokenKind::Identifier && upper_copy(token.text) == expected;
    }

    static Column pending_column;

    static Value token_to_pending_value(const Token& token, ColumnType) {
        if (token.kind == TokenKind::Identifier && upper_copy(token.text) == "NULL") return std::monostate{};
        if (token.kind == TokenKind::Number) return std::stoi(token.text);
        if (token.kind == TokenKind::String) return intern_string(token.text);
        throw std::runtime_error("Expected DEFAULT literal");
    }

    static sql::BisonParser::symbol_type yylex(ParseContext& ctx) {
        const Token& token = ctx.tokens.at(ctx.position++);
        if (token.kind == TokenKind::End) return sql::BisonParser::make_YYEOF();
        if (token.kind == TokenKind::Number) return sql::BisonParser::make_NUMBER(token.text);
        if (token.kind == TokenKind::String) return sql::BisonParser::make_STRING(token.text);

        if (token.kind == TokenKind::Symbol) {
            if (token.text == "==") return sql::BisonParser::make_EQ();
            if (token.text == "!=") return sql::BisonParser::make_NE();
            if (token.text == "<=") return sql::BisonParser::make_LE();
            if (token.text == ">=") return sql::BisonParser::make_GE();
            if (token.text == "<") return sql::BisonParser::make_LT();
            if (token.text == ">") return sql::BisonParser::make_GT();
            if (token.text == "(") return sql::BisonParser::make_LPAREN();
            if (token.text == ")") return sql::BisonParser::make_RPAREN();
            if (token.text == ",") return sql::BisonParser::make_COMMA();
            if (token.text == ".") return sql::BisonParser::make_DOT();
            if (token.text == "*") return sql::BisonParser::make_STAR();
            if (token.text == "=") return sql::BisonParser::make_ASSIGN();
        }

        if (keyword(token, "CREATE")) return sql::BisonParser::make_CREATE();
        if (keyword(token, "DATABASE")) return sql::BisonParser::make_DATABASE();
        if (keyword(token, "DROP")) return sql::BisonParser::make_DROP();
        if (keyword(token, "USE")) return sql::BisonParser::make_USE();
        if (keyword(token, "TABLE")) return sql::BisonParser::make_TABLE();
        if (keyword(token, "INT")) return sql::BisonParser::make_INT_TYPE();
        if (keyword(token, "STRING")) return sql::BisonParser::make_STRING_TYPE();
        if (keyword(token, "NOT_NULL")) return sql::BisonParser::make_NOT_NULL();
        if (keyword(token, "INDEXED")) return sql::BisonParser::make_INDEXED();
        if (keyword(token, "DEFAULT")) return sql::BisonParser::make_DEFAULT();
        if (keyword(token, "INSERT")) return sql::BisonParser::make_INSERT();
        if (keyword(token, "INTO")) return sql::BisonParser::make_INTO();
        if (keyword(token, "VALUE")) return sql::BisonParser::make_VALUE();
        if (keyword(token, "SELECT")) return sql::BisonParser::make_SELECT();
        if (keyword(token, "FROM")) return sql::BisonParser::make_FROM();
        if (keyword(token, "WHERE")) return sql::BisonParser::make_WHERE();
        if (keyword(token, "UPDATE")) return sql::BisonParser::make_UPDATE();
        if (keyword(token, "SET")) return sql::BisonParser::make_SET();
        if (keyword(token, "DELETE")) return sql::BisonParser::make_DELETE();
        if (keyword(token, "REVERT")) return sql::BisonParser::make_REVERT();
        if (keyword(token, "BETWEEN")) return sql::BisonParser::make_BETWEEN();
        if (keyword(token, "AND")) return sql::BisonParser::make_AND();
        if (keyword(token, "OR")) return sql::BisonParser::make_OR();
        if (keyword(token, "LIKE")) return sql::BisonParser::make_LIKE();
        if (keyword(token, "AS")) return sql::BisonParser::make_AS();
        if (keyword(token, "COUNT")) return sql::BisonParser::make_COUNT();
        if (keyword(token, "SUM")) return sql::BisonParser::make_SUM();
        if (keyword(token, "AVG")) return sql::BisonParser::make_AVG();
        return sql::BisonParser::make_IDENT(token.text);
    }

    Command parse_sql_command(const std::string& sql) {
        ParseContext ctx;
        ctx.tokens = lex_sql(sql);
        sql::BisonParser parser(ctx);
        if (parser.parse() != 0) {
            throw std::runtime_error(ctx.error.empty() ? "Syntax error" : ctx.error);
        }
        return ctx.result;
    }
}

%token CREATE DATABASE DROP USE TABLE INSERT INTO VALUE SELECT FROM WHERE UPDATE SET DELETE REVERT
%token INT_TYPE STRING_TYPE NOT_NULL INDEXED DEFAULT BETWEEN AND OR LIKE AS COUNT SUM AVG
%token EQ NE LE GE LT GT LPAREN RPAREN COMMA DOT STAR ASSIGN
%token <std::string> IDENT NUMBER STRING

%type <Command> command
%type <TableRef> table_ref
%type <std::vector<Column>> column_defs
%type <Column> column_def
%type <ColumnType> type
%type <Token> literal
%type <std::vector<std::string>> ident_list
%type <std::vector<Token>> value_list
%type <std::vector<std::vector<Token>>> row_list
%type <std::vector<Projection>> select_items projection_list
%type <Projection> projection
%type <Condition> opt_where condition or_condition and_condition primary_condition
%type <Operand> operand
%type <std::string> compare_op opt_alias
%type <std::vector<std::pair<std::string, Token>>> assignments
%type <std::pair<std::string, Token>> assignment

%%

input:
    command { ctx.result = std::move($1); }
;

command:
    CREATE DATABASE IDENT {
        $$ = Command{};
        $$.kind = Command::Kind::CreateDatabase;
        $$.database_name = std::move($3);
    }
  | DROP DATABASE IDENT {
        $$ = Command{};
        $$.kind = Command::Kind::DropDatabase;
        $$.database_name = std::move($3);
    }
  | USE IDENT {
        $$ = Command{};
        $$.kind = Command::Kind::UseDatabase;
        $$.database_name = std::move($2);
    }
  | CREATE TABLE table_ref LPAREN column_defs RPAREN {
        $$ = Command{};
        $$.kind = Command::Kind::CreateTable;
        $$.table = std::move($3);
        $$.columns = std::move($5);
    }
  | DROP TABLE table_ref {
        $$ = Command{};
        $$.kind = Command::Kind::DropTable;
        $$.table = std::move($3);
    }
  | INSERT INTO table_ref LPAREN ident_list RPAREN VALUE row_list {
        $$ = Command{};
        $$.kind = Command::Kind::Insert;
        $$.table = std::move($3);
        $$.insert_columns = std::move($5);
        $$.insert_rows = std::move($8);
    }
  | SELECT select_items FROM table_ref opt_where {
        $$ = Command{};
        $$.kind = Command::Kind::Select;
        $$.projections = std::move($2);
        $$.table = std::move($4);
        $$.condition = std::move($5);
        $$.has_condition = $$.condition.kind != Condition::Kind::None;
    }
  | DELETE FROM table_ref WHERE condition {
        $$ = Command{};
        $$.kind = Command::Kind::Delete;
        $$.table = std::move($3);
        $$.condition = std::move($5);
        $$.has_condition = true;
    }
  | UPDATE table_ref SET assignments WHERE condition {
        $$ = Command{};
        $$.kind = Command::Kind::Update;
        $$.table = std::move($2);
        $$.assignments = std::move($4);
        $$.condition = std::move($6);
        $$.has_condition = true;
    }
  | REVERT table_ref STRING {
        $$ = Command{};
        $$.kind = Command::Kind::Revert;
        $$.table = std::move($2);
        $$.timestamp = std::move($3);
    }
;

table_ref:
    IDENT {
        $$ = TableRef{};
        $$.table = std::move($1);
    }
  | IDENT DOT IDENT {
        $$ = TableRef{};
        $$.database = std::move($1);
        $$.table = std::move($3);
    }
;

column_defs:
    column_def { $$.push_back(std::move($1)); }
  | column_defs COMMA column_def { $$ = std::move($1); $$.push_back(std::move($3)); }
;

column_def:
    IDENT type { pending_column = Column{}; pending_column.type = $2; } column_mods {
        $$ = std::move(pending_column);
        $$.name = std::move($1);
        $$.type = $2;
    }
;

column_mods:
    %empty
  | column_mods NOT_NULL { pending_column.not_null = true; }
  | column_mods INDEXED { pending_column.indexed = true; pending_column.not_null = true; }
  | column_mods DEFAULT literal { pending_column.has_default = true; pending_column.default_value = token_to_pending_value($3, pending_column.type); }
;

type:
    INT_TYPE { $$ = ColumnType::Int; }
  | STRING_TYPE { $$ = ColumnType::String; }
;

ident_list:
    IDENT { $$.push_back(std::move($1)); }
  | ident_list COMMA IDENT { $$ = std::move($1); $$.push_back(std::move($3)); }
;

row_list:
    LPAREN value_list RPAREN { $$.push_back(std::move($2)); }
  | row_list COMMA LPAREN value_list RPAREN { $$ = std::move($1); $$.push_back(std::move($4)); }
;

value_list:
    literal { $$.push_back(std::move($1)); }
  | value_list COMMA literal { $$ = std::move($1); $$.push_back(std::move($3)); }
;

literal:
    NUMBER { $$ = Token{TokenKind::Number, std::move($1)}; }
  | STRING { $$ = Token{TokenKind::String, std::move($1)}; }
  | IDENT { $$ = Token{TokenKind::Identifier, std::move($1)}; }
;

select_items:
    STAR { $$ = {}; }
  | LPAREN projection_list RPAREN { $$ = std::move($2); }
;

projection_list:
    projection { $$.push_back(std::move($1)); }
  | projection_list COMMA projection { $$ = std::move($1); $$.push_back(std::move($3)); }
;

projection:
    IDENT opt_alias {
        $$ = Projection{};
        $$.column = std::move($1);
        $$.alias = $2.empty() ? $$.column : std::move($2);
    }
  | COUNT LPAREN STAR RPAREN opt_alias {
        $$ = Projection{};
        $$.aggregate = Projection::Aggregate::Count;
        $$.count_all = true;
        $$.alias = std::move($5);
    }
  | COUNT LPAREN IDENT RPAREN opt_alias {
        $$ = Projection{};
        $$.aggregate = Projection::Aggregate::Count;
        $$.column = std::move($3);
        $$.alias = std::move($5);
    }
  | SUM LPAREN IDENT RPAREN opt_alias {
        $$ = Projection{};
        $$.aggregate = Projection::Aggregate::Sum;
        $$.column = std::move($3);
        $$.alias = std::move($5);
    }
  | AVG LPAREN IDENT RPAREN opt_alias {
        $$ = Projection{};
        $$.aggregate = Projection::Aggregate::Avg;
        $$.column = std::move($3);
        $$.alias = std::move($5);
    }
;

opt_alias:
    %empty { $$ = {}; }
  | AS IDENT { $$ = std::move($2); }
;

opt_where:
    %empty { $$ = Condition{}; }
  | WHERE condition { $$ = std::move($2); }
;

condition:
    or_condition { $$ = std::move($1); }
;

or_condition:
    and_condition { $$ = std::move($1); }
  | or_condition OR and_condition {
        $$ = Condition{};
        $$.kind = Condition::Kind::Or;
        $$.lhs = std::make_shared<Condition>(std::move($1));
        $$.rhs = std::make_shared<Condition>(std::move($3));
    }
;

and_condition:
    primary_condition { $$ = std::move($1); }
  | and_condition AND primary_condition {
        $$ = Condition{};
        $$.kind = Condition::Kind::And;
        $$.lhs = std::make_shared<Condition>(std::move($1));
        $$.rhs = std::make_shared<Condition>(std::move($3));
    }
;

primary_condition:
    operand compare_op operand {
        $$ = Condition{};
        $$.kind = Condition::Kind::Compare;
        $$.left = std::move($1);
        $$.op = std::move($2);
        $$.right = std::move($3);
    }
  | operand BETWEEN operand AND operand {
        $$ = Condition{};
        $$.kind = Condition::Kind::Between;
        $$.left = std::move($1);
        $$.right = std::move($3);
        $$.high = std::move($5);
    }
  | operand LIKE operand {
        $$ = Condition{};
        $$.kind = Condition::Kind::Like;
        $$.left = std::move($1);
        $$.right = std::move($3);
    }
  | LPAREN condition RPAREN { $$ = std::move($2); }
;

operand:
    IDENT {
        if (upper_copy($1) == "NULL") {
            $$ = Operand{false, "", Token{TokenKind::Identifier, std::move($1)}};
        } else {
            $$ = Operand{true, std::move($1), {}};
        }
    }
  | NUMBER { $$ = Operand{false, "", Token{TokenKind::Number, std::move($1)}}; }
  | STRING { $$ = Operand{false, "", Token{TokenKind::String, std::move($1)}}; }
;

compare_op:
    EQ { $$ = "=="; }
  | NE { $$ = "!="; }
  | LT { $$ = "<"; }
  | GT { $$ = ">"; }
  | LE { $$ = "<="; }
  | GE { $$ = ">="; }
;

assignments:
    assignment { $$.push_back(std::move($1)); }
  | assignments COMMA assignment { $$ = std::move($1); $$.push_back(std::move($3)); }
;

assignment:
    IDENT ASSIGN literal { $$ = {std::move($1), std::move($3)}; }
;

%%

namespace sql {
void BisonParser::error(const std::string& message) {
    ctx.error = message;
}
}
