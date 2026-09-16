#include "error_utils.h"
#include <system_error>
#include <cstring>

[[noreturn]] void throw_system_error(const string& context){
    int err = errno;
    throw system_error(err, generic_category(), context + ": " + std::strerror(err) + " (errno=" + std::to_string(err) + ")");
}