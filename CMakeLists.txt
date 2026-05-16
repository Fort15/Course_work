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

add_executable(course_dbms
    src/main.cpp
)

target_include_directories(course_dbms PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "/home/maks-alex/FIIT_SP/associative_container/indexing_tree/b_plus_tree/include"
    "/home/maks-alex/FIIT_SP/associative_container/include"
    "/home/maks-alex/FIIT_SP/allocator/allocator/include"
)

target_link_libraries(course_dbms PRIVATE nlohmann_json::nlohmann_json)
