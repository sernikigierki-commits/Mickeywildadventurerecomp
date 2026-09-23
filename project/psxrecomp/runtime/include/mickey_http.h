#pragma once

#include <string>

struct MickeyHttpResponse {
    int status = 0;
    std::string body;
};

MickeyHttpResponse mickey_http_request(const char* url,
                                      const char* post = nullptr,
                                      const char* content_type = nullptr);
