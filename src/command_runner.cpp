#include "app.h"

#include "engine.h"

#include <exception>
#include <fstream>
#include <iostream>
#include <istream>
#include <string>
#include <vector>

namespace course_dbms {

bool has_non_whitespace(const std::string& value) {
    return value.find_first_not_of(" \t\r\n") != std::string::npos;
}

std::vector<std::string> take_complete_commands(std::string& buffer) {
    std::vector<std::string> commands;

    bool in_string = false;
    bool escaped = false;
    std::size_t command_start = 0;

    for (std::size_t i = 0; i < buffer.size(); ++i) {
        const char ch = buffer[i];

        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }

            continue;
        }

        if (ch == '"') {
            in_string = true;
            continue;
        }

        if (ch == ';') {
            std::string command = buffer.substr(command_start, i - command_start);

            if (has_non_whitespace(command)) {
                commands.push_back(std::move(command));
            }

            command_start = i + 1;
        }
    }

    if (command_start != 0) {
        buffer.erase(0, command_start);
    }

    return commands;
}

void execute_commands(Engine& engine, const std::vector<std::string>& commands) {
    for (const auto& sql : commands) {
        try {
            engine.execute(sql);
        } catch (const std::exception& ex) {
            std::cout << "ERROR: " << ex.what() << '\n';
        }
    }
}

void process_input(std::istream& input, Engine& engine, bool interactive) {
    std::string line;
    std::string buffer;

    if (interactive) {
        std::cout << "course_dbms> ";
    }

    while (std::getline(input, line)) {
        buffer += line;
        buffer += '\n';

        const auto commands = take_complete_commands(buffer);
        execute_commands(engine, commands);

        if (interactive) {
            std::cout << "course_dbms> ";
        }
    }

    if (has_non_whitespace(buffer)) {
        std::cout << "ERROR: Command must end with ';'\n";
    }
}

}

int course_dbms_main(int argc, char** argv) {
    try {
        course_dbms::Engine engine("db_data");

        if (argc == 1) {
            course_dbms::process_input(std::cin, engine, true);
            return 0;
        }

        if (argc == 2) {
            std::ifstream script(argv[1]);

            if (!script) {
                std::cerr << "ERROR: Cannot open script file\n";
                return 1;
            }

            course_dbms::process_input(script, engine, false);
            return 0;
        }

        std::cerr << "Usage: " << argv[0] << " [script.sql]\n";
        return 1;

    } catch (const std::exception& ex) {
        std::cerr << "FATAL: " << ex.what() << '\n';
        return 1;
    }
}
