#pragma once
#include <string>
#include <iostream>
#include "http_parser.h"

bool gzip_compress(const std::string& input, std::string& output);

bool should_compress(const HttpRequest& req, const HttpResponse& resp);