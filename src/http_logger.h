#ifndef HTTP_LOGGER_H
#define HTTP_LOGGER_H

#include <boost/beast/http.hpp>
#include <boost/filesystem.hpp>

#include "generic_stream.h"
#include "namespaces.h"
#include "session.h"

namespace http = ouinet::http;
using Request = http::request<http::string_body>;
using GenericStream = ouinet::GenericStream;
using Session = ouinet::Session;

class HTTPLogger {

public:
    HTTPLogger() = default;
    void log_to_file(const std::string&);
    void log(const std::string&, const Request&, const Session&, size_t);

private:
    static std::string get_datetime();
    static std::string get_header_value(const Request&, const http::field&);
    static std::string get_request_size(const Session&, size_t);
    static std::string get_request_line(const Request&);

    std::string log_filename;
    boost::optional<std::fstream> log_file;
};

extern HTTPLogger http_logger;

#endif //HTTP_LOGGER_H
