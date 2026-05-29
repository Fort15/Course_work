cmake_minimum_required(VERSION 3.16)

project(course_dbms LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    add_library(nlohmann_json INTERFACE)
    target_include_directories(nlohmann_json INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
    add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
endif()

find_package(FLEX QUIET)
find_package(BISON QUIET)

if(FLEX_FOUND AND BISON_FOUND)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
    BISON_TARGET(SqlParser
        "${CMAKE_CURRENT_SOURCE_DIR}/parser/sql_parser.y"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/sql_parser.cpp"
        DEFINES_FILE "${CMAKE_CURRENT_BINARY_DIR}/generated/sql_parser.hpp"
    )
    FLEX_TARGET(SqlLexer
        "${CMAKE_CURRENT_SOURCE_DIR}/parser/sql_lexer.l"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/sql_lexer.cpp"
    )
    ADD_FLEX_BISON_DEPENDENCY(SqlLexer SqlParser)
    set(SQL_PARSER_SOURCES ${BISON_SqlParser_OUTPUTS} ${FLEX_SqlLexer_OUTPUTS})
    set(SQL_GENERATED_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/generated")
else()
    message(STATUS "Flex/Bison not found; using checked-in generated parser sources")
    set(SQL_PARSER_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/generated/sql_parser.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/generated/sql_lexer.cpp"
    )
    set(SQL_GENERATED_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/generated")
endif()

add_executable(course_dbms
    src/main.cpp
    src/db_util.cpp
    src/row_storage.cpp
    src/table.cpp
    src/engine.cpp
    src/command_runner.cpp
    ${SQL_PARSER_SOURCES}
)

target_include_directories(course_dbms PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${SQL_GENERATED_INCLUDE}"
)

target_link_libraries(course_dbms PRIVATE nlohmann_json::nlohmann_json)
