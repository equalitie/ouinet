/**
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of version 3 of the GNU Lesser General
 * Public License as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */
#ifndef SRC_LOGGER_H_
#define SRC_LOGGER_H_

#include <iostream>
#include <fstream>

#include "namespaces.h"
#include "util/str.h"
#include <boost/optional.hpp>
#include <boost/utility/string_view.hpp>

// Logger macros which should be used for efficiency
// (also see <https://pzemtsov.github.io/2014/05/05/do-macro.html> for statement protection)
#define LOG_SILLY(...) do { if (logger.get_threshold() <= SILLY) logger.silly(util::str(__VA_ARGS__)); } while (false)
#define LOG_DEBUG(...) do { if (logger.get_threshold() <= DEBUG) logger.debug(util::str(__VA_ARGS__)); } while (false)
#define LOG_VERBOSE(...) do { if (logger.get_threshold() <= VERBOSE) logger.verbose(util::str(__VA_ARGS__)); } while (false)
#define LOG_INFO(...) do { if (logger.get_threshold() <= INFO) logger.info(util::str(__VA_ARGS__)); } while (false)
#define LOG_WARN(...) do { if (logger.get_threshold() <= WARN) logger.warn(util::str(__VA_ARGS__)); } while (false)
#define LOG_ERROR(...) do { if (logger.get_threshold() <= ERROR) logger.error(util::str(__VA_ARGS__)); } while (false)
#define LOG_ABORT(...) logger.abort(util::str(__VA_ARGS__)) 

// Standard log levels, ascending order of specificity.
enum log_level_t { SILLY, DEBUG, VERBOSE, INFO, WARN, ERROR, ABORT };

log_level_t default_log_level();

inline std::ostream& operator<<(std::ostream& os, log_level_t ll) {
    switch (ll) {
        case SILLY:   return os << "SILLY";
        case DEBUG:   return os << "DEBUG";
        case VERBOSE: return os << "VERBOSE";
        case INFO:    return os << "INFO";
        case WARN:    return os << "WARN";
        case ERROR:   return os << "ERROR";
        case ABORT:   return os << "ABORT";
    }
    return os << "???";
}

class Logger
{
  protected:
    bool _stamp_with_time = false;
    log_level_t threshold;
    bool log_to_stderr;
    std::string log_filename;
    boost::optional<std::fstream> log_file;

  public:
    std::string state_to_text[0xFF]; // TOTAL_NO_OF_STATES
    std::string message_type_to_text[0xFF]; // TOTAL_NO_OF_MESSAGE_TYPE];

    bool would_log(log_level_t) const;

    // Constructor sets an initial threshold
    Logger(log_level_t threshold);

    // Get the current log file name
    std::string current_log_file() { return log_filename; }
    std::fstream* get_log_file();

    // Get the current threshold
    log_level_t get_threshold() const { return threshold;}
    void enable_timestamp() { _stamp_with_time = true;}
    void disable_timestamp() { _stamp_with_time = false;}
    
    void log_to_file(std::string fname);

    void set_threshold(log_level_t level);

    void log(log_level_t level, const std::string& msg, boost::string_view function_name = "");

    void silly  (const std::string& msg, boost::string_view function_name = "");
    void debug  (const std::string& msg, boost::string_view function_name = "");
    void verbose(const std::string& msg, boost::string_view function_name = "");
    void info   (const std::string& msg, boost::string_view function_name = "");
    void warn   (const std::string& msg, boost::string_view function_name = "");
    void error  (const std::string& msg, boost::string_view function_name = "");
    void abort  (const std::string& msg, boost::string_view function_name = "");

    void assert_or_die(bool expr, std::string failure_message, std::string function_name = "");
};

extern Logger logger;

#endif // SRC_LOGGER_H_
