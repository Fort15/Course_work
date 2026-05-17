cmake_minimum_required(VERSION 3.16)

project(course_dbms LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

find_package(FLEX REQUIRED)
find_package(BISON REQUIRED)

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

add_executable(course_dbms
    src/main.cpp
    src/dbms.cpp
    ${BISON_SqlParser_OUTPUTS}
    ${FLEX_SqlLexer_OUTPUTS}
)

target_include_directories(course_dbms PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

target_link_libraries(course_dbms PRIVATE nlohmann_json::nlohmann_json)
