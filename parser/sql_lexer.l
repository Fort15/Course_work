%option noyywrap nounput noinput batch

%{
#include <stdexcept>
#include <string>
#include <vector>

#include "db_ast.h"

static std::vector<Token>* output_tokens = nullptr;

static std::string unquote(const char* text, int length) {
    std::string raw(text + 1, text + length - 1);
    std::string result;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size()) {
            result.push_back(raw[++i]);
        } else {
            result.push_back(raw[i]);
        }
    }
    return result;
}
%}

%%
[ \t\r\n]+                  ;
\"([^\"\\]|\\.)*\"          { output_tokens->push_back({TokenKind::String, unquote(yytext, yyleng)}); }
[0-9]{4}\.[0-9]{2}\.[0-9]{2}-[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]+ { output_tokens->push_back({TokenKind::String, yytext}); }
-?[0-9]+                    { output_tokens->push_back({TokenKind::Number, yytext}); }
[A-Za-z_][A-Za-z0-9_]*      { output_tokens->push_back({TokenKind::Identifier, yytext}); }
"=="|"!="|"<="|">="         { output_tokens->push_back({TokenKind::Symbol, yytext}); }
[(),.*;<>="]                { output_tokens->push_back({TokenKind::Symbol, yytext}); }
.                           { throw std::runtime_error(std::string("Unexpected character: ") + yytext); }
%%

std::vector<Token> lex_sql(const std::string& sql) {
    std::vector<Token> tokens;
    output_tokens = &tokens;
    YY_BUFFER_STATE buffer = yy_scan_bytes(sql.data(), static_cast<int>(sql.size()));
    yylex();
    yy_delete_buffer(buffer);
    output_tokens = nullptr;
    tokens.push_back({TokenKind::End, ""});
    return tokens;
}
