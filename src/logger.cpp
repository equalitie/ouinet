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

#include "logger.h"

Logger logger(DEBUG);

const std::string log_level_announce[] =       {"SILLY"        , "DEBUG"     , "VERBOSE"   , "INFO"      , "WARN"        , "ERROR"      , "ABORT"};
const std::string log_level_color_prefix[] =   {"\033[1;35;47m", "\033[1;32m", "\033[1;37m", "\033[1;34m", "\033[90;103m", "\033[91;40m", "\033[91;40m"};
const bool log_level_colored_msg[] =           {true           , false       , false       , false       , true        , true          , true};

void Logger::initiate_textual_conversions()
{
}

// Standard constructor
// Threshold adopts a default level of DEBUG if an invalid threshold is provided.
Logger::Logger(log_level_t threshold)
{
    initiate_textual_conversions();

    if (threshold < SILLY || threshold > ERROR) {
        this->threshold = default_log_level;
    } else {
        this->threshold = threshold;
    }
    log_to_stderr = true;
    log_to_file = false;
    log_filename = "";

    log_ts_base = {0, 0};
    //gettimeofday(&log_ts_base, 0);

}

// Standard destructor
Logger::~Logger()
{
    if (log_file.is_open()) {
        log_file.close();
    }
}

// Configure the logger to log to stderr and/or to a file.
void Logger::config(bool log_stderr, bool log_to_file, std::string fname)
{
    log_to_stderr = log_stderr;
    this->log_to_file = log_to_file;
    if (log_to_file) {
        log_filename = fname;
        log_file.open(log_filename, std::ios::out | std::ios::app);
    } else {
        if (log_file.is_open()) {
            log_file.close();
        }
    }
}

// Update the logger's threshold.
// If an invalid level is provided, do not update.
void Logger::set_threshold(log_level_t level)
{
    if (level >= SILLY && level <= ABORT) {
        threshold = level;
    }
}

// Standard log function. Prints nice colors for each level.
void Logger::log(log_level_t level, std::string msg, std::string function_name)
{
    if (level < SILLY || level > ABORT || level < threshold) {
        return;
    }

    msg = (function_name.empty()) ? msg : function_name + ": " + msg;

    std::string message_prefix = "[" + log_level_announce[level];
#ifndef __ANDROID__
    message_prefix = log_level_color_prefix[level] + message_prefix + "]";
    if (log_level_colored_msg[level])
      message_prefix += " ";
    else
      message_prefix += "\033[0m ";
#else
    message_prefix = message_prefix + "] ";
#endif // ifndef __ANDROID__

    msg =  message_prefix + msg;
#ifndef __ANDROID__
    if (log_level_colored_msg[level])
      msg += "\033[0m";
#endif

    //time stamp
    if (_stamp_with_time) {
      msg = std::to_string(log_get_timestamp()) + ": " + msg;
    }

    if (log_to_stderr) {
        std::cerr << msg << std::endl;
        std::cerr.flush();
    }
    if (log_to_file && log_file.is_open()) {
        log_file << msg << std::endl;
    }
}

// Convenience methods

void Logger::silly(std::string msg, std::string function_name)
{
    log(SILLY, msg, function_name);
}

void Logger::debug(std::string msg, std::string function_name)
{
    log(DEBUG, msg, function_name);
}

void Logger::verbose(std::string msg, std::string function_name)
{
    log(VERBOSE, msg, function_name);
}

void Logger::info(std::string msg, std::string function_name)
{
    log(INFO, msg, function_name);
}

void Logger::warn(std::string msg, std::string function_name)
{
    log(WARN, msg, function_name);
}

void Logger::error(std::string msg, std::string function_name)
{
    log(ERROR, msg, function_name);
}

void Logger::abort(std::string msg, std::string function_name)
{
    log(ABORT, msg, function_name);
    exit(1);
}

void Logger::assert_or_die(bool expr, std::string failure_message, std::string function_name)
{
    if (!expr)
        abort(failure_message, function_name);
}

/** Get a timestamp, as a floating-point number of seconds. */
double
Logger::log_get_timestamp()
{
  struct timeval now, delta;
  gettimeofday(&now, 0);
  timeval_subtract(&now, &log_ts_base, &delta);
  return delta.tv_sec + double(delta.tv_usec) / 1e6;
}

