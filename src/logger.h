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
#include "util.h"

//Logger micros which should be used for efficiency
#define LOG_SILLY(...) if (logger.get_threshold() <= SILLY) { logger.silly(util::str(__VA_ARGS__)); }
#define LOG_DEBUG(...) if (logger.get_threshold() <= DEBUG) { logger.debug(util::str(__VA_ARGS__)); }
#define LOG_VERBOSE(...) if (logger.get_threshold() <= VERBOSE) { logger.verbose(util::str(__VA_ARGS__)); }
#define LOG_INFO(...) if (logger.get_threshold() <= INFO) { logger.info(util::str(__VA_ARGS__)); }
#define LOG_WARN(...) if (logger.get_threshold() <= WARN) { logger.warn(util::str(__VA_ARGS__)); }
#define LOG_ERROR(...) if (logger.get_threshold() <= ERROR) { logger.error(util::str(__VA_ARGS__)); }
#define LOG_ABORT(...) logger.abort(util::str(__VA_ARGS__)) 

// Standard log levels, ascending order of specificity.
enum log_level_t { SILLY, DEBUG, VERBOSE, INFO, WARN, ERROR, ABORT };

log_level_t default_log_level();


class Logger
{
  protected:
    bool _stamp_with_time = false;
    log_level_t threshold;
    bool log_to_stderr;
    bool log_to_file;
    std::string log_filename;
    std::ofstream log_file;

    /************************* Time Functions **************************/

    int timeval_subtract(struct timeval *x, struct timeval *y,
		     struct timeval *result) {
      /* Perform the carry for the later subtraction by updating y. */
      if (x->tv_usec < y->tv_usec) {
        int nsec = (y->tv_usec - x->tv_usec) / 1000000 + 1;
        y->tv_usec -= 1000000 * nsec;
        y->tv_sec += nsec;
      }
      if (x->tv_usec - y->tv_usec > 1000000) {
        int nsec = (x->tv_usec - y->tv_usec) / 1000000;
        y->tv_usec += 1000000 * nsec;
        y->tv_sec -= nsec;
      }

      /* Compute the time remaining to wait.
         tv_usec is certainly positive. */
      result->tv_sec = x->tv_sec - y->tv_sec;
      result->tv_usec = x->tv_usec - y->tv_usec;

      /* Return 1 if result is negative. */
      return x->tv_sec < y->tv_sec;
    }

    struct timeval log_ts_base;
    /** Get a timestamp, as a floating-point number of seconds. */
    double log_get_timestamp();

  public:
    std::string state_to_text[0xFF]; // TOTAL_NO_OF_STATES
    std::string message_type_to_text[0xFF]; // TOTAL_NO_OF_MESSAGE_TYPE];

    // put name on states and message types
    void initiate_textual_conversions();

    // Constructor sets an initial threshold
    Logger(log_level_t threshold);
    // Destructor closes an open log file
    ~Logger();

    // Get the current log file name
    std::string current_log_file() { return log_filename; }

    // Get the current threshold
    log_level_t get_threshold() { return threshold;}
    void enable_timestamp() { _stamp_with_time = true;}
    void disable_timestamp() { _stamp_with_time = false;}
    
    void config(bool log_stderr, bool log_file, std::string fname);
    void set_threshold(log_level_t level);
    void log(log_level_t level, std::string msg, std::string function_name = "");
    void silly(std::string msg, std::string function_name = "");
    void debug(std::string msg, std::string function_name = "");
    void verbose(std::string msg, std::string function_name = "");
    void info(std::string msg, std::string function_name = "");
    void warn(std::string msg, std::string function_name = "");
    void error(std::string msg, std::string function_name = "");
    void abort(std::string msg, std::string function_name = "");

    void assert_or_die(bool expr, std::string failure_message, std::string function_name = "");
};

extern Logger logger;

#endif // SRC_LOGGER_H_
