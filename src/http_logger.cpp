#include "http_logger.h"

static const long LOG_FILE_MAX_SIZE = 15 * 1024 * 1024;

HTTPLogger http_logger{};

std::string HTTPLogger::get_datetime()
{
    std::time_t now_time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm now_tm = *std::localtime(&now_time);
    std::stringstream now_str;
    now_str << std::put_time(&now_tm, "%d/%b/%Y:%H:%M:%S %z");
    return "[" + now_str.str() + "]";
}

std::string HTTPLogger::get_header_value(const Request& rq, const http::field& field)
{
    auto it = rq.find(field);
    std::string hdr_value = "-";
    if (it != rq.end())
        hdr_value = "\"" + it->value().to_string() + "\"";
    return hdr_value;
}

std::string HTTPLogger::get_request_size(const Session& sess, size_t fwd_bytes)
{
    std::string content_length = "0";
    auto& res = sess.response_header();
    auto hdr = res.find(http::field::content_length);
    if (hdr != res.end()) {
        content_length = "\"" + hdr->value().to_string() + "\"";
    } else {
        content_length = std::to_string(fwd_bytes);
    }
    return content_length;
}

void HTTPLogger::log_to_file(std::string fname)
{
    using std::ios;

    if (fname.empty()) {
        if (!log_filename.empty()) {
            ouinet::sys::error_code ignored_ec;
            ouinet::fs::remove(log_filename, ignored_ec);
        }
        log_file = boost::none;
        return;
    }

    if (log_filename != fname || !log_file) {
        log_filename = fname;
        log_file = std::fstream();

        if (ouinet::fs::exists(log_filename)) {
            log_file->open(log_filename, ios::in | ios::out | ios::ate);
        } else {
            // `trunc` causes the file to be created
            log_file->open(log_filename, ios::in | ios::out | ios::trunc);
        }

        if (!log_file->is_open()) {
            std::cerr << "Failed to open log file " << fname  << "\n";
            log_filename = "";
            log_file = boost::none;
        }
    }
}

void HTTPLogger::log(const GenericStream& con, const Request& rq, const Session& sess, size_t fwd_bytes)
{
    if (!log_file || !log_file->is_open()) return;

    auto& inh = sess.response_header();

    auto ua = get_header_value(rq, http::field::user_agent);
    auto referer = get_header_value(rq, http::field::referer);

    std::stringstream request_line_ss;
    std::string http_version = std::to_string(rq.version()); // TODO: Adjust the format of the HTTP version
    request_line_ss << to_string(rq.method()) << " " << rq.target() << " " << http_version;

    if (log_file && log_file->is_open()) {
        *log_file << "\"" + con.remote_endpoint() + "\" " // TODO: Fetch the target IP
                << "- " // identd
                << "- " // userid
                << get_datetime() << " "
                << "\"" << request_line_ss.str() << "\" "
                << inh.result_int() << " "
                << get_request_size(sess, fwd_bytes) << " " // TODO: Calculate request size
                << referer << " "
                << ua << "\n";
    }

    if (log_file->tellp() > LOG_FILE_MAX_SIZE) {
        log_file->seekp(0);
    }
}