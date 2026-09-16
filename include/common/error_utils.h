#pragma once
#include <string>

using namespace std;

[[noreturn]] void throw_system_error(const string& context);