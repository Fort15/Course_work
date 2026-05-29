#ifndef COURSE_DBMS_DB_ERROR_H
#define COURSE_DBMS_DB_ERROR_H

#include <stdexcept>
#include <string>

namespace course_dbms {

struct DbError : std::runtime_error {
    explicit DbError(const std::string& message) : std::runtime_error(message) {}
};

}

#endif
