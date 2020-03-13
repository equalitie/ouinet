/**
 * Multiparty Off-the-Record Messaging library
 * Copyright (C) 2014, eQualit.ie
 *
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

#include <sys/time.h>

#include <string>
#include <iostream>
#include <fstream>
#include <iomanip> // std::setprecision

#include <boost/system/error_code.hpp>
#include <boost/filesystem.hpp>
#include "namespaces.h"
#include "logger.h"

static const long LOG_FILE_MAX_SIZE = 15 * 1024 * 1024;

const std::string log_level_announce[] =       {"SILLY"        , "DEBUG"     , "VERBOSE"   , "INFO"      , "WARN"        , "ERROR"      , "ABORT"};
const std::string log_level_color_prefix[] =   {"\033[1;35;47m", "\033[1;32m", "\033[1;37m", "\033[1;34m", "\033[90;103m", "\033[31;40m", "\033[1;31;40m"};
const bool log_level_colored_msg[] =           {true           , false       , false       , false       , true        , true          , true};

log_level_t default_log_level() {
    return INFO;
}

Logger logger(default_log_level());

/************************* Time Functions **************************/

static
int timeval_subtract(struct timeval *x, struct timeval *y, struct timeval *result) {
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
static double log_get_timestamp()
{
    struct timeval now, delta;
    gettimeofday(&now, 0);
    timeval_subtract(&now, &log_ts_base, &delta);
    return delta.tv_sec + double(delta.tv_usec) / 1e6;
}

/*********************** Time Functions END ************************/

// Standard constructor
// Threshold adopts a default level of DEBUG if an invalid threshold is provided.
Logger::Logger(log_level_t threshold)
{
    if (threshold < SILLY || threshold > ERROR) {
        this->threshold = default_log_level();
    } else {
        this->threshold = threshold;
    }
    log_to_stderr = true;
    log_filename = "";
    log_ts_base = {0, 0};
}

void Logger::log_to_file(std::string fname)
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
        } else {
            static const char* start_s = "\nOUINET START\n";
            *log_file << start_s;
        }
    }
}

std::fstream* Logger::get_log_file() {
    if (!log_file) return nullptr;
    return &*log_file;
}

// Update the logger's threshold.
// If an invalid level is provided, do not update.
void Logger::set_threshold(log_level_t level)
{
    if (level >= SILLY && level <= ABORT) {
        threshold = level;
    }
}

bool Logger::would_log(log_level_t level) const
{
    return get_threshold() <= level;
}

namespace {
    struct Printer {
        using string_view = boost::string_view;

        log_level_t level;
        bool with_color;
        boost::optional<double> ts;
        string_view msg;
        string_view fun;

        Printer(log_level_t level, bool with_color, boost::optional<double> ts, string_view msg, string_view fun)
            : level(level)
            , with_color(with_color)
            , ts(ts)
            , msg(msg)
            , fun(fun)
        {}

        friend std::ostream& operator<<(std::ostream& os, const Printer& p) {
            static const char* color_end = "\033[0m";

            if (p.ts) {
                // Prevent scientific notation
                os << std::fixed << std::showpoint << std::setprecision(4);
                os << *p.ts << ": ";
            }

            if (p.with_color) {
                os << log_level_color_prefix[p.level];
            }

            os << "[" << log_level_announce[p.level];

            if (log_level_colored_msg[p.level] || !p.with_color) {
                os << "] ";
            } else {
                os << "]" << color_end << " ";
            }

            if (!p.fun.empty()) {
                os << p.fun << ": ";
            }

            os << p.msg;

            if (p.with_color && log_level_colored_msg[p.level]) {
                os << color_end;
            }

            return os;
        }
    };
}

// Standard log function. Prints nice colors for each level.
void Logger::log(log_level_t level, const std::string& msg, boost::string_view function_name)
{
    if (level < SILLY || level > ABORT || level < threshold) {
        return;
    }

#ifdef __ANDROID__
    const bool android = true;
#else
    const bool android = false;
#endif

    bool with_color = !android;

    boost::optional<double> ts;

    if (_stamp_with_time || log_file) ts = log_get_timestamp();

    if (log_to_stderr) {
        if (_stamp_with_time)
            std::cerr << Printer(level, with_color, ts, msg, function_name) << "\n";
        else
            std::cerr << Printer(level, with_color, boost::none, msg, function_name) << "\n";
    }

    if (log_file && log_file->is_open()) {
        *log_file << Printer(level, false, ts, msg, function_name) << "\n";

        if (log_file->tellp() > LOG_FILE_MAX_SIZE) {
            log_file->seekp(0);
        }
    }
}

// Convenience methods

void Logger::silly(const std::string& msg, boost::string_view function_name)
{
    log(SILLY, msg, function_name);
}

void Logger::debug(const std::string& msg, boost::string_view function_name)
{
    log(DEBUG, msg, function_name);
}

void Logger::verbose(const std::string& msg, boost::string_view function_name)
{
    log(VERBOSE, msg, function_name);
}

void Logger::info(const std::string& msg, boost::string_view function_name)
{
    log(INFO, msg, function_name);
}

void Logger::warn(const std::string& msg, boost::string_view function_name)
{
    log(WARN, msg, function_name);
}

void Logger::error(const std::string& msg, boost::string_view function_name)
{
    log(ERROR, msg, function_name);
}

void Logger::abort(const std::string& msg, boost::string_view function_name)
{
    log(ABORT, msg, function_name);
    exit(1);
}

void Logger::assert_or_die(bool expr, std::string failure_message, std::string function_name)
{
    if (!expr)
        abort(failure_message, function_name);
}

